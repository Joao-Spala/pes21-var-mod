-- PES 2021 VAR Mod (Sider 7.x Controller - Standalone & Zero-Boot-Overhead)
-- Requer: PES2021.exe (Steam 1.7.2.0) + Sider 7.x (luajit.ext.enabled = 1) + modules\var_mod\var_core.dll
-- Escopo: exclusivamente partidas offline (Exibicao, Master Liga, BAL, Copas, Ligas).
-- Performance: o VAR carrega sob demanda (set_teams / overlay_on). Na abertura do jogo so a verificacao de
-- atualizacao comeca, em segundo plano (sem hooks e sem tocar na memoria do jogo).

local m = { version = "4.5.3" }

local ffi = ffi
local cdef_registered = false
local var_dll = nil
local dll_loaded = false
local dll_load_failed = false
local dll_error = nil      -- chave de TXT (traduzida na exibicao)
local dll_error_arg = ""  -- detalhe opcional (caminho do arquivo)
local var_enabled = true
local ini_language = "pt"
local saved_sider_dir = nil
local config_path = nil
local auto_update = true   -- auto_update (ou check_updates da v4.5.0) no var_mod.ini
local updater_lib = nil    -- var_core.dll carregada na abertura so para a verificacao de atualizacao

local STATUS_BUF_SIZE = 512
local status_buf = nil

local function trim(s)
    if not s then return "" end
    return (s:gsub("^%s+", ""):gsub("%s+$", ""))
end

local function normalize_dir(dir)
    if not dir or dir == "" then return "" end
    local last = dir:sub(-1)
    if last ~= "\\" and last ~= "/" then
        return dir .. "\\"
    end
    return dir
end

local function load_config()
    if not config_path then return end
    local f = io.open(config_path, "r")
    if not f then return end
    for line in f:lines() do
        local k, v = line:match("^%s*([%w_]+)%s*=%s*([^#]+)")
        if k and v then
            k = trim(k)
            v = trim(v)
            if k == "enabled" then
                var_enabled = (v == "1" or v == "true")
            elseif k == "language" then
                local lv = v:lower()
                ini_language = (lv == "en" or lv == "english" or lv == "1") and "en" or "pt"
            elseif k == "auto_update" or k == "check_updates" then
                auto_update = (v == "1" or v == "true")
            end
        end
    end
    f:close()
end

local function save_config()
    if not config_path then return end
    local lines = {}
    local found = false
    local fin = io.open(config_path, "r")
    if fin then
        for line in fin:lines() do
            if line:match("^%s*enabled%s*=") then
                lines[#lines + 1] = string.format("enabled = %d", var_enabled and 1 or 0)
                found = true
            else
                lines[#lines + 1] = line
            end
        end
        fin:close()
    end
    if not found then
        if #lines == 0 then
            lines[#lines + 1] = "# PES 2021 VAR Mod v" .. m.version .. " - Configuracao"
        end
        lines[#lines + 1] = string.format("enabled = %d", var_enabled and 1 or 0)
    end
    local f = io.open(config_path, "w")
    if not f then return end
    f:write(table.concat(lines, "\n"), "\n")
    f:close()
end

local function register_cdef()
    if cdef_registered then return end
    ffi.cdef[[
        int  var_init(const char* logDirUtf8);
        void var_shutdown(void);
        void var_set_enabled(int enabled);
        void var_set_offline_mode(int isOffline);
        void var_set_sider_overlay_open(int isOpen);
        void var_on_match_start(void);
        void var_on_match_end(void);
        void var_toggle_debug_overlay(void);
        int  var_get_language(void);
        int  var_get_status_text(char* outBuf, int bufSize);
        int  var_update_start(const char* baseDirUtf8);
        int  var_update_state(void);
    ]]
    cdef_registered = true
end

-- Na abertura do jogo: carrega a DLL so para verificar atualizacao em segundo plano. Se uma versao nova for
-- instalada antes da primeira partida, ensure_native_loaded ja usa a nova. Sem pcall no Sider: cada passo
-- confere antes de chamar.
local function start_update_check()
    if not auto_update or type(ffi) ~= "table" or not saved_sider_dir then return end
    local base_dir = normalize_dir(saved_sider_dir) .. "modules\\var_mod\\"
    local dll_path = base_dir .. "var_core.dll"
    local f = io.open(dll_path, "rb")
    if not f then return end
    f:close()
    register_cdef()
    local lib = ffi.load(dll_path)
    if not lib then return end
    updater_lib = lib
    lib.var_update_start(base_dir)
    log("[var_mod] Verificacao de atualizacao iniciada em segundo plano (detalhes em modules\\var_mod\\var_update.log)")
end

local function ensure_native_loaded()
    if dll_loaded then return true end
    if dll_load_failed then return false end

    if type(ffi) ~= "table" then
        dll_load_failed = true
        dll_error = "noffi"
        log("[var_mod] ERRO: FFI indisponivel, ative luajit.ext.enabled = 1 no sider.ini")
        return false
    end

    register_cdef()

    local base_dir = normalize_dir(saved_sider_dir) .. "modules\\var_mod\\"
    local dll_path = base_dir .. "var_core.dll"

    local fcheck = io.open(dll_path, "rb")
    if not fcheck then
        dll_load_failed = true
        dll_error = "nofile"
        dll_error_arg = dll_path
        log("[var_mod] ERRO: arquivo nao encontrado: " .. dll_path)
        return false
    end
    fcheck:close()

    -- O ambiente Lua do Sider nao expoe pcall. A falha e marcada ANTES da chamada: se ffi.load ou
    -- var_init lancarem erro, as proximas chamadas (overlay_on roda a cada frame) retornam false na hora
    -- em vez de repetir o erro.
    dll_load_failed = true
    dll_error = "loadfail"
    local lib = nil
    -- Atualizacao instalada enquanto o jogo estava nos menus (estado 3): a copia var_core.next.dll e a versao
    -- nova e ja vale nesta sessao. A DLL antiga continua carregada, parada (so fez a verificacao).
    if updater_lib and updater_lib.var_update_state() == 3 then
        local next_path = base_dir .. "var_core.next.dll"
        local fnext = io.open(next_path, "rb")
        if fnext then
            fnext:close()
            log("[var_mod] Atualizacao automatica instalada agora: carregando a versao nova (" .. next_path .. ")")
            lib = ffi.load(next_path)
        end
    end
    if not lib then
        lib = updater_lib or ffi.load(dll_path)
    end
    if not lib then
        log("[var_mod] ERRO: falha ao carregar var_core.dll")
        return false
    end

    var_dll = lib
    status_buf = ffi.new("char[?]", STATUS_BUF_SIZE)

    dll_error = "initfail"
    local init_ok = var_dll.var_init(base_dir)
    dll_load_failed = false
    if init_ok == 0 then
        dll_load_failed = true
        dll_error = "initfail"
        log("[var_mod] ERRO: var_init retornou falha (veja modules\\var_mod\\var_core.log)")
        return false
    end

    var_dll.var_set_enabled(var_enabled and 1 or 0)
    var_dll.var_set_offline_mode(1)
    dll_loaded = true
    dll_error = nil
    log("[var_mod] var_core.dll v" .. m.version .. " carregada sob demanda (" .. dll_path .. ")")
    return true
end

-- Idioma atual: a DLL e a fonte da verdade (o usuario troca no painel), o INI cobre o periodo antes do carregamento
local function is_english()
    if dll_loaded then
        return var_dll.var_get_language() == 1
    end
    return ini_language == "en"
end

local TXT = {
    pt = {
        err    = "PES 2021 VAR Mod v%s\nStatus: ERRO (%s)\nVerifique se modules\\var_mod\\var_core.dll esta instalado e luajit.ext.enabled = 1.",
        noload = "DLL nao carregada",
        na     = "Indisponivel",
        on     = "ATIVADO",
        off    = "DESATIVADO",
        keys   = "[7] Ativar/Desativar VAR (atual: %s)  |  [8] Telemetria ao vivo",
        noffi    = "FFI indisponivel: ative luajit.ext.enabled = 1 no sider.ini",
        nofile   = "Arquivo nao encontrado: %s",
        loadfail = "Falha ao carregar var_core.dll (veja sider.log)",
        initfail = "Falha ao iniciar var_core.dll (veja modules\\var_mod\\var_core.log)",
    },
    en = {
        err    = "PES 2021 VAR Mod v%s\nStatus: ERROR (%s)\nMake sure modules\\var_mod\\var_core.dll is installed and luajit.ext.enabled = 1.",
        noload = "DLL not loaded",
        na     = "Unavailable",
        on     = "ENABLED",
        off    = "DISABLED",
        keys   = "[7] Enable/Disable VAR (current: %s)  |  [8] Live telemetry",
        noffi    = "FFI unavailable: set luajit.ext.enabled = 1 in sider.ini",
        nofile   = "File not found: %s",
        loadfail = "Could not load var_core.dll (see sider.log)",
        initfail = "var_core.dll failed to start (see modules\\var_mod\\var_core.log)",
    },
}

local function txt()
    return is_english() and TXT.en or TXT.pt
end

-- Key repeat guard state (used by overlay_on, key_down and key_up below)
local keys_held = {}
local overlay_frame = 0

function m.set_teams(ctx, home, away)
    if not ensure_native_loaded() then return end
    var_dll.var_set_offline_mode(1)
    var_dll.var_on_match_start()
    log(string.format("[var_mod] Partida iniciada (home=%s, away=%s, tid=%s)",
        tostring(home), tostring(away), tostring(ctx.tournament_id)))
end

function m.after_set_conditions(ctx)
    ensure_native_loaded()
end

function m.context_reset(ctx)
    if not dll_loaded then return end
    var_dll.var_set_offline_mode(1)
    var_dll.var_on_match_end()
end

function m.overlay_on(ctx)
    overlay_frame = overlay_frame + 1
    if ensure_native_loaded() then
        var_dll.var_set_sider_overlay_open(1)
    end

    local T = txt()
    if not dll_loaded then
        local reason = T.noload
        if dll_error and T[dll_error] then
            reason = string.format(T[dll_error], dll_error_arg)
        end
        return string.format(T.err, m.version, reason)
    end

    -- Linha 1: status nativo. Linha 2: atalho real do painel (respeita o remap) e onde mudar idioma/atalhos.
    local n = var_dll.var_get_status_text(status_buf, STATUS_BUF_SIZE)
    local native_status = (n > 0) and ffi.string(status_buf, n) or T.na

    return string.format(
        "PES 2021 VAR Mod v%s\n%s\n" .. T.keys,
        m.version,
        native_status,
        var_enabled and T.on or T.off
    )
end

-- key_down repeats while the key is held (system auto-repeat): act once per press. A press is re-armed by
-- key_up, or after ~20 overlay frames without repeats (key_up is not delivered once the overlay closes).

function m.key_up(ctx, vkey)
    keys_held[vkey] = nil
end

function m.key_down(ctx, vkey)
    local last = keys_held[vkey]
    keys_held[vkey] = overlay_frame
    if last and (overlay_frame - last) < 20 then return end
    if not ensure_native_loaded() then return end
    if vkey == 0x37 then
        var_enabled = not var_enabled
        var_dll.var_set_enabled(var_enabled and 1 or 0)
        save_config()
        log("[var_mod] VAR " .. (var_enabled and "ATIVADO" or "DESATIVADO") .. " pelo usuario")
    elseif vkey == 0x38 then
        var_dll.var_toggle_debug_overlay()
    end
end

function m.init(ctx)
    saved_sider_dir = ctx.sider_dir
    config_path = normalize_dir(ctx.sider_dir) .. "modules\\var_mod\\var_mod.ini"
    load_config()
    ctx.register("set_teams", m.set_teams)
    ctx.register("after_set_conditions", m.after_set_conditions)
    ctx.register("context_reset", m.context_reset)
    ctx.register("overlay_on", m.overlay_on)
    ctx.register("key_down", m.key_down)
    ctx.register("key_up", m.key_up)
    log("[var_mod] v" .. m.version .. " registrado (VAR carregado sob demanda na primeira partida)")
    start_update_check()
end

return m
