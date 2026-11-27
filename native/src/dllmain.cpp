#include "dx11_hook.h"
#include "i18n.h"
#include "inline_hook.h"
#include "input_manager.h"
#include "logger.h"
#include "heap_guard.h"
#include "match_hooks.h"
#include "update_checker.h"
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
    // Automatic update (option "Atualizacao automatica", on by default): background check every 6 h, a newer
    // release is installed only with a valid signature and takes effect at the next game start
    var_mod::UpdateChecker::SetBaseDir(baseDir);
    var_mod::UpdateChecker::Start(var_mod::VarState::Instance().GetHudConfig().autoUpdate);

    // Engine heap fix, independent from the VAR itself: makes the dlmalloc segment release atomic so the
    // post-match heap trim cannot write into a segment it has just released (crash at PES2021.exe+0x3FE31D).
    var_mod::HeapGuard::Install();

    // Install XInput IAT hook immediately (< 1 ms); defer all AOB scans and DX11 hooking
    // to the async background worker so Sider / patch startup never stalls the main thread.
    var_mod::InputManager::InstallXInputHook();
    var_mod::Dx11Hook::StartAsync();
    VAR_LOG_INF("var_core.dll v" VAR_MOD_VERSION " initialization started (async)");
    return 1;
}

__declspec(dllexport) void __cdecl var_shutdown() {
    if (!g_initialized.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    // Unhooking while the game runs is never safe (a thread may be inside a detour or trampoline, the
    // HeapGuard runs inside the allocator). "Shutdown" therefore only turns the VAR off; the module is pinned.
    g_initialized.store(true, std::memory_order_release);
    var_mod::VarState::Instance().SetEnabled(false);
    VAR_LOG_INF("var_shutdown: VAR disabled (hooks stay installed until the process exits)");
    var_mod::Logger::Flush();
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

// One line per period with what the mod cost the game: heap release replacement and present hook.
// Lets a slow loading be attributed (or not) to the mod from the user's log alone.
static void LogPerfPeriod(const char* period) {
    static var_mod::HeapGuard::Stats s_prev{};
    const var_mod::HeapGuard::Stats now = var_mod::HeapGuard::Totals();
    const var_mod::HeapGuard::Stats hs = var_mod::HeapGuard::Delta(now, s_prev);
    s_prev = now;
    const var_mod::Dx11Hook::PresentStats ps = var_mod::Dx11Hook::TakePresentStats();
    VAR_LOG_INF("Perf %s: heap release %llu call(s) (%llu released, %llu refused, %llu refused from cache) in %llu us, "
                "present hook %llu frame(s) in %llu us (max %llu us)",
                period, static_cast<unsigned long long>(hs.calls), static_cast<unsigned long long>(hs.released),
                static_cast<unsigned long long>(hs.refused), static_cast<unsigned long long>(hs.cacheHits),
                static_cast<unsigned long long>(hs.microseconds), static_cast<unsigned long long>(ps.frames),
                static_cast<unsigned long long>(ps.microseconds), static_cast<unsigned long long>(ps.maxMicroseconds));
}

// The C++ runtime's demangler is only reached by the fatal "terminate" handler to pretty-print a type name.
// Providing this stub keeps its 128 KB object out of the DLL; the handler then prints the raw name.
extern "C" char* __cxa_demangle(const char*, char*, size_t*, int* status) {
    if (status) *status = -2;
    return nullptr;
}

__declspec(dllexport) void __cdecl var_on_match_start() {
    if (!g_initialized.load(std::memory_order_acquire)) return;
    if (var_mod::Dx11Hook::IsInitFinished() && !var_mod::MatchHooks::IsInstalled()) {
        var_mod::MatchHooks::Install();
    }
    var_mod::MatchHooks::OnMatchReset();
    var_mod::VarState::Instance().ResetMatch();
    var_mod::VarHudRenderer::RequestReset();
    LogPerfPeriod("since the previous match (menus and loading)");
    var_mod::HeapGuard::ClearRefusedCache();
    VAR_LOG_INF("Match session started (ring buffers reset)");
}

__declspec(dllexport) void __cdecl var_on_match_end() {
    if (!g_initialized.load(std::memory_order_acquire)) return;
    var_mod::MatchHooks::OnMatchReset();
    var_mod::VarState::Instance().CancelVerdict();
    var_mod::VarState::Instance().ClearReplayFocusAndGrace();
    var_mod::VarHudRenderer::RequestReset();
    LogPerfPeriod("during the match");
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
