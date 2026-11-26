#include "dx11_hook.h"
#include "i18n.h"
#include "inline_hook.h"
#include "input_manager.h"
#include "logger.h"
#include "heap_guard.h"
#include "match_hooks.h"
#include "var_hud.h"
#include "var_state.h"
#include "version.h"

#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

HMODULE g_selfModule = nullptr;
std::atomic<bool> g_initialized{ false };

std::wstring PathToWide(const char* s) {
    if (!s || !s[0]) return L"";
    // Try strict UTF-8 first; fall back to Windows system ANSI codepage (LuaJIT / Sider ctx.sider_dir)
    UINT cp = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int len = MultiByteToWideChar(cp, flags, s, -1, nullptr, 0);
    if (len <= 1) {
        cp = CP_ACP;
        flags = 0;
        len = MultiByteToWideChar(cp, flags, s, -1, nullptr, 0);
    }
    if (len <= 1) return L"";
    std::wstring out(static_cast<size_t>(len - 1), L'\0');
    MultiByteToWideChar(cp, flags, s, -1, out.data(), len);
    return out;
}

std::wstring ResolveBaseDir(const char* logDirUtf8) {
    std::wstring dir = PathToWide(logDirUtf8);
    if (dir.empty() && g_selfModule) {
        wchar_t modPath[MAX_PATH] = {};
        if (GetModuleFileNameW(g_selfModule, modPath, MAX_PATH) > 0) {
            dir = modPath;
            const size_t pos = dir.find_last_of(L"\\/");
            if (pos != std::wstring::npos) {
                dir.resize(pos);
            }
        }
    }
    if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') {
        dir.push_back(L'\\');
    }
    return dir;
}

// Folds the UTF-8 Latin-1 letters used by the HUD (acentos, cedilha, ordinais) and the middle dot to ASCII.
void AsciiFoldInPlace(char* s) {
    static const char kLatin1[] =
        // 0xC0..0xFF
        "AAAAAAACEEEEIIII" "DNOOOOOxOUUUUYTs" "aaaaaaaceeeeiiii" "dnooooo/ouuuuyty";
    unsigned char* r = reinterpret_cast<unsigned char*>(s);
    char* w = s;
    while (*r) {
        if (r[0] == 0xC3 && r[1] >= 0x80 && r[1] <= 0xBF) {
            *w++ = kLatin1[(r[1] - 0x80) + 0x00];
            r += 2;
        } else if (r[0] == 0xC2 && r[1] != 0) {
            if (r[1] == 0xB7) *w++ = '-';        // middle dot
            else if (r[1] == 0xBA) *w++ = 'o';   // masculine ordinal
            else if (r[1] == 0xAA) *w++ = 'a';   // feminine ordinal
            r += 2;
        } else if (r[0] >= 0x80) {
            // other multi-byte sequences (ellipsis, dashes): skip the whole code point
            int n = (r[0] >> 5) == 0x6 ? 2 : (r[0] >> 4) == 0xE ? 3 : (r[0] >> 3) == 0x1E ? 4 : 1;
            while (n-- > 0 && *r) ++r;
        } else {
            *w++ = static_cast<char>(*r++);
        }
    }
    *w = '\0';
}

} // namespace

extern "C" {

__declspec(dllexport) int __cdecl var_init(const char* logDirUtf8) {
    if (g_initialized.exchange(true, std::memory_order_acq_rel)) {
        return 1;
    }

    // Pin the DLL: detours stay patched into the game for the whole process, so the module must never be
    // unmapped by a stray FreeLibrary (threads would return into freed code).
    {
        HMODULE pinned = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&var_init), &pinned);
    }

    const std::wstring baseDir = ResolveBaseDir(logDirUtf8);
    var_mod::Logger::Init(baseDir.empty() ? L"var_core.log" : (baseDir + L"var_core.log"));
    VAR_LOG_INF("var_core.dll initializing (PES 2021 VAR Mod v" VAR_MOD_VERSION ")");

    var_mod::VarState::Instance().SetIniPath(baseDir.empty() ? L"var_mod.ini" : (baseDir + L"var_mod.ini"));
    var_mod::VarState::Instance().LoadHudConfig();

    // Engine heap fix, independent from the VAR itself: makes the dlmalloc segment release atomic so the
    // post-match heap trim cannot write into a segment it has just released (crash at PES2021.exe+0x3FE31D).
    var_mod::HeapGuard::Install();

    // Install XInput IAT hook immediately (< 1 ms); defer all AOB scans and DX11 hooking
    // to the async background worker so Sider / BMPES startup never stalls the main thread.
    var_mod::InputManager::InstallXInputHook();
    var_mod::Dx11Hook::StartAsync();
    VAR_LOG_INF("var_core.dll v" VAR_MOD_VERSION " initialization started (async)");
    return 1;
}

__declspec(dllexport) void __cdecl var_shutdown() {
    if (!g_initialized.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    VAR_LOG_INF("var_core.dll shutting down");
    var_mod::Dx11Hook::Shutdown();
    var_mod::InputManager::UninstallNativeInputBlockHooks();
    var_mod::InputManager::UninstallXInputHook();
    var_mod::MatchHooks::Uninstall();
    var_mod::Logger::Shutdown();
}

__declspec(dllexport) void __cdecl var_set_enabled(int enabled) {
    var_mod::VarState::Instance().SetEnabled(enabled != 0);
}

__declspec(dllexport) void __cdecl var_set_offline_mode(int isOffline) {
    var_mod::VarState::Instance().SetOfflineAllowed(isOffline != 0);
}

__declspec(dllexport) void __cdecl var_set_sider_overlay_open(int isOpen) {
    var_mod::VarState::Instance().SetSiderOverlayOpen(isOpen != 0);
}

__declspec(dllexport) void __cdecl var_on_match_start() {
    if (!g_initialized.load(std::memory_order_acquire)) return;
    if (var_mod::Dx11Hook::IsInitFinished() && !var_mod::MatchHooks::IsInstalled()) {
        var_mod::MatchHooks::Install();
    }
    var_mod::MatchHooks::OnMatchReset();
    var_mod::VarState::Instance().ResetMatch();
    var_mod::VarHudRenderer::RequestReset();
    VAR_LOG_INF("Match session started (ring buffers reset)");
}

__declspec(dllexport) void __cdecl var_on_match_end() {
    if (!g_initialized.load(std::memory_order_acquire)) return;
    var_mod::MatchHooks::OnMatchReset();
    var_mod::VarState::Instance().CancelVerdict();
    var_mod::VarState::Instance().ClearReplayFocusAndGrace();
    var_mod::VarHudRenderer::RequestReset();
    VAR_LOG_INF("Match session ended");
    var_mod::Logger::Flush();
}

// 0 = Portugues (BR), 1 = English. Lets var_mod.lua localize its own Sider overlay lines.
__declspec(dllexport) int __cdecl var_get_language() {
    return (var_mod::i18n::GetLanguage() == var_mod::Language::English) ? 1 : 0;
}

__declspec(dllexport) void __cdecl var_toggle_debug_overlay() {
    auto& st = var_mod::VarState::Instance();
    st.SetDebugOverlayOpen(!st.IsDebugOverlayOpen());
}

__declspec(dllexport) int __cdecl var_get_status_text(char* outBuf, int bufSize) {
    if (!outBuf || bufSize <= 0) return 0;

    const auto tel = var_mod::VarState::Instance().GetTelemetry();
    const auto armed = var_mod::VarState::Instance().PeekVerdict();
    const auto events = var_mod::VarState::Instance().GetRecentEvents(64);
    const auto stats = var_mod::VarState::Instance().GetStats();

    using var_mod::Tr;
    const char* stateStr = (!tel.hooksInstalled && !var_mod::Dx11Hook::IsInitFinished()) ? Tr("INICIALIZANDO")
                         : !tel.hooksInstalled ? Tr("ERRO HOOKS")
                         : !tel.offlineAllowed ? Tr("BLOQUEADO (ONLINE)")
                         : !tel.enabled        ? Tr("DESATIVADO")
                         : tel.matchActive     ? Tr("ATIVO EM PARTIDA")
                         : Tr("PRONTO (FORA DE PARTIDA)");

    // Panel shortcut shown to the user reflects their remap (Sider thread: pure formatter, no HUD cache)
    char panelHint[96];
    var_mod::InputManager::FormatHintFrom(var_mod::VarState::Instance().GetHudConfig().bindings,
                                          var_mod::BindAction::ToggleFocus, panelHint, sizeof(panelHint));

    char line1[320];
    std::snprintf(line1, sizeof(line1),
        Tr("VAR: %s | DX11: %s | Seek: %s | Lances: %zu | Revisoes: %u | Veredito: %s"),
        stateStr,
        var_mod::Dx11Hook::IsHooked() ? "OK" : Tr("AGUARDANDO"),
        tel.replaySeekReady ? Tr("NATIVO") : Tr("PASSIVO"),
        events.size(),
        stats.totalVarInterventions,
        armed.armed ? armed.summary : Tr("Nenhum"));
    char line2[200];
    std::snprintf(line2, sizeof(line2),
        Tr("Painel no Replay Manual: %s  |  Idioma e atalhos: aba OPCOES do painel"), panelHint);
    const int total = std::snprintf(outBuf, static_cast<size_t>(bufSize), "%s\n%s", line1, line2);
    if (total <= 0) return 0;
    AsciiFoldInPlace(outBuf); // Sider renders the overlay with its own font: keep it plain ASCII
    return static_cast<int>(std::strlen(outBuf));
}

} // extern "C"

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpReserved) {
    (void)lpReserved;
    if (fdwReason == DLL_PROCESS_ATTACH) {
        g_selfModule = hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);
    } else if (fdwReason == DLL_PROCESS_DETACH) {
        // Never join threads, unpatch game code or take locks under the loader lock: another thread may have
        // been killed inside a detour or while holding the log mutex. var_init pins the module, so this only
        // runs at process exit. Info and above are already flushed line by line.
        var_mod::SetHooksProcessTerminating();
    }
    return TRUE;
}
