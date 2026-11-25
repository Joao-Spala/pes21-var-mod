#include "input_manager.h"

#include "aob_scanner.h"
#include "game_structs.h"
#include "i18n.h"
#include "inline_hook.h"
#include "logger.h"
#include "match_hooks.h"
#include "var_state.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace var_mod {

namespace {

using FnXInputGetState   = DWORD(WINAPI*)(DWORD dwUserIndex, RawXInputState* pState);
using FnGetKeyboardState = BOOL(WINAPI*)(PBYTE lpKeyState);
using FnDInputPoll       = void(__fastcall*)(void* self);

IatHook    g_xinputIatHook;
IatHook    g_getKbStateIatHook;
InlineHook g_hookDInputKb;
InlineHook g_hookDInputJoy;
InlineHook g_hookDInputMouse;
InlineHook g_hookPesPadUpdate;
FnXInputGetState g_fallbackXInputGetState = nullptr;

bool ShouldBlockGameInput() {
    return VarState::Instance().ShouldBlockGameInputNow(GetTickCount64());
}

void __fastcall Detour_DInputKeyboardPoll(void* self) {
    auto orig = g_hookDInputKb.Original<FnDInputPoll>();
    if (orig) {
        orig(self); // Runs original (so Sider overlay still sees Space / hotkeys cleanly!)
    }
    if (ShouldBlockGameInput() && IsValidPtr(self)) {
        // Zero Fox Engine's 256-byte DirectInput keyboard buffer at [self + 0x268]
        std::memset(static_cast<uint8_t*>(self) + 0x268, 0, 256);
    }
}

void __fastcall Detour_DInputJoystickPoll(void* self) {
    // Gamepads (DInput or XInput) are never blocked: they keep controlling the Replay while the panel is open
    auto orig = g_hookDInputJoy.Original<FnDInputPoll>();
    if (orig) {
        orig(self);
    }
}

void __fastcall Detour_DInputMousePoll(void* self) {
    auto orig = g_hookDInputMouse.Original<FnDInputPoll>();
    if (orig) {
        orig(self);
    }
    if (ShouldBlockGameInput() && IsValidPtr(self)) {
        // DIMOUSESTATE (16 bytes at [self + 0x268]): zero deltas & mouse buttons
        std::memset(static_cast<uint8_t*>(self) + 0x268, 0, 16);
    }
}

void __fastcall Detour_PesPadUpdate(void* self) {
    // No longer clears the unified pad state: that also wiped the real gamepad. Keyboard and mouse are still
    // blocked at their source (DInput keyboard/mouse buffers + GetKeyboardState), which PesPad reads from
    // inside this very call, so the panel keys never reach the Replay while the gamepad keeps working.
    auto orig = g_hookPesPadUpdate.Original<FnDInputPoll>();
    if (orig) {
        orig(self); // Runs XInput/DInput polling (keeps our XInput hook fed & DInput acquired)
    }
}

BOOL WINAPI Detour_GetKeyboardState(PBYTE lpKeyState) {
    auto orig = g_getKbStateIatHook.Original<FnGetKeyboardState>();
    const BOOL ok = orig ? orig(lpKeyState) : FALSE;
    if (ok && lpKeyState && ShouldBlockGameInput()) {
        std::memset(lpKeyState, 0, 256);
    }
    return ok;
}

uint16_t ExtractPadButtonsWithStick(const RawXInputGamepad& gp) {
    uint16_t mask = gp.wButtons;
    constexpr int16_t kStickDeadzone = 16500;
    if (gp.sThumbLY >  kStickDeadzone) mask |= PadButtons::DpadUp;
    if (gp.sThumbLY < -kStickDeadzone) mask |= PadButtons::DpadDown;
    if (gp.sThumbLX < -kStickDeadzone) mask |= PadButtons::DpadLeft;
    if (gp.sThumbLX >  kStickDeadzone) mask |= PadButtons::DpadRight;
    return mask;
}

std::atomic<uint16_t> g_rawPadButtonsBySlot[4] = { 0, 0, 0, 0 };
std::atomic<uint64_t> g_lastHookPadTickBySlot[4] = { 0, 0, 0, 0 };
std::atomic<HWND> g_hwnd{ nullptr };

std::mutex g_actionMutex;
std::vector<NavAction> g_queuedKeyActions;
std::atomic<bool> g_hasQueuedActions{ false };

struct RepeatTracker {
    uint16_t lastMask = 0;
    uint64_t holdStartMs = 0;
    uint64_t lastRepeatMs = 0;
};
RepeatTracker g_padTracker{};
RepeatTracker g_keyDirTracker{};

// User bindings cached on the render thread, refreshed lock-free only when VarState::ConfigRevision changes
InputBindings g_bindings = InputBindings::Defaults();
uint32_t      g_bindingsRev = 0;
// Read by the game's XInput thread (collapsed-panel masking), so it is published atomically
std::atomic<uint16_t> g_toggleFocusPadMask{ 0x0080 };

// Previous key-down state per action for rising-edge detection via GetAsyncKeyState (zero WndProc subclassing)
bool g_prevActionKey[kBindActionCount] = {};

// Remap capture state (render thread only)
struct CaptureCtx {
    bool     active = false;
    bool     waitingRelease = true;
    int      column = 0;
    uint64_t startMs = 0;
    bool     prevEsc = true;
};
CaptureCtx g_capture{};

// Last device that produced input (render thread only): drives keyboard vs gamepad prompt glyphs
bool g_preferPadGlyphs = false;

bool IsKeyDown(int vk) {
    return vk != 0 && (GetAsyncKeyState(vk) & 0x8000) != 0;
}

bool IsActionKeyDown(BindAction a) {
    const auto& k = g_bindings.keys[static_cast<size_t>(a)];
    return IsKeyDown(k[0]) || IsKeyDown(k[1]);
}

void RefreshBindings();

bool IsGameForeground() {
    HWND h = g_hwnd.load(std::memory_order_acquire);
    if (!h) return false; // window unknown yet: never react to keys typed in another app
    HWND fg = GetForegroundWindow();
    return (fg == h);
}

DWORD WINAPI Detour_XInputGetState(DWORD dwUserIndex, RawXInputState* pState) {
    auto orig = g_xinputIatHook.Original<FnXInputGetState>();
    const DWORD res = orig ? orig(dwUserIndex, pState) : ERROR_DEVICE_NOT_CONNECTED;
    if (res == ERROR_SUCCESS && pState != nullptr) {
        if (dwUserIndex < 4) {
            g_rawPadButtonsBySlot[dwUserIndex].store(
                ExtractPadButtonsWithStick(pState->Gamepad), std::memory_order_relaxed);
            g_lastHookPadTickBySlot[dwUserIndex].store(GetTickCount64(), std::memory_order_relaxed);
        }

        const auto& st = VarState::Instance();
        if (st.IsEnabled() && st.IsOfflineAllowed() && !st.IsSiderOverlayOpen() &&
            (st.IsInManualReplay() || st.ShouldBlockGameInputNow(GetTickCount64()))) {
            // The pad keeps controlling the Replay (open or collapsed panel): only the open/collapse button
            // (R3 by default, or the user's remap) is masked so toggling the panel never leaks into the game.
            pState->Gamepad.wButtons &= static_cast<uint16_t>(~g_toggleFocusPadMask.load(std::memory_order_relaxed));
        }
    } else if (dwUserIndex < 4) {
        if (g_rawPadButtonsBySlot[dwUserIndex].load(std::memory_order_relaxed) != 0) {
            g_rawPadButtonsBySlot[dwUserIndex].store(0, std::memory_order_relaxed);
        }
    }
    return res;
}

FnXInputGetState ResolveFallbackXInput() {
    if (g_fallbackXInputGetState) return g_fallbackXInputGetState;
    const char* dlls[] = { "xinput1_3.dll", "xinput1_4.dll", "xinput9_1_0.dll" };
    for (const char* name : dlls) {
        HMODULE h = GetModuleHandleA(name);
        if (!h) h = LoadLibraryA(name);
        if (h) {
            auto fn = reinterpret_cast<FnXInputGetState>(GetProcAddress(h, "XInputGetState"));
            if (!fn) {
                fn = reinterpret_cast<FnXInputGetState>(GetProcAddress(h, MAKEINTRESOURCEA(2)));
            }
            if (fn) {
                g_fallbackXInputGetState = fn;
                return fn;
            }
        }
    }
    return nullptr;
}

uint16_t ReadCurrentPadButtons() {
    const uint64_t now = GetTickCount64();
    if (g_xinputIatHook.IsInstalled()) {
        bool anyFresh = false;
        uint16_t combined = 0;
        for (DWORD idx = 0; idx < 4; ++idx) {
            const uint64_t t = g_lastHookPadTickBySlot[idx].load(std::memory_order_relaxed);
            if (now >= t && (now - t) < 150) {
                anyFresh = true;
                combined |= g_rawPadButtonsBySlot[idx].load(std::memory_order_relaxed);
            }
        }
        if (anyFresh) {
            return combined;
        }
    }

    if (auto fn = ResolveFallbackXInput()) {
        uint16_t combined = 0;
        for (DWORD idx = 0; idx < 4; ++idx) {
            RawXInputState st{};
            if (fn(idx, &st) == ERROR_SUCCESS) {
                combined |= ExtractPadButtonsWithStick(st.Gamepad);
            }
        }
        return combined;
    }
    return 0;
}

void ResetKeyEdgeTrackers() {
    for (int a = 0; a < kBindActionCount; ++a) {
        g_prevActionKey[a] = IsActionKeyDown(static_cast<BindAction>(a));
    }
    g_keyDirTracker.lastMask = 0;
}

void RefreshBindings() {
    const uint32_t rev = VarState::Instance().ConfigRevision();
    if (rev == g_bindingsRev) return;
    g_bindingsRev = rev;
    g_bindings = VarState::Instance().GetHudConfig().bindings;
    g_toggleFocusPadMask.store(g_bindings.pad[static_cast<size_t>(BindAction::ToggleFocus)],
                               std::memory_order_relaxed);
    ResetKeyEdgeTrackers(); // a freshly bound key that is still held must not fire immediately
}

// Rising edge of an action's keyboard shortcut (updates the tracker)
bool KeyEdge(BindAction a) {
    const bool cur = IsActionKeyDown(a);
    bool& prev = g_prevActionKey[static_cast<size_t>(a)];
    const bool edge = cur && !prev;
    prev = cur;
    return edge;
}

constexpr NavAction kNavForBind[kBindActionCount] = {
    NavAction::ToggleFocus, NavAction::Confirm, NavAction::Back, NavAction::CancelVerdict,
    NavAction::PrevTab, NavAction::NextTab, NavAction::QuickSwap, NavAction::PrevIncident,
    NavAction::NextIncident, NavAction::TogglePlayPause,
    NavAction::Up, NavAction::Down, NavAction::Left, NavAction::Right, NavAction::ToggleDebug,
};

// Panel-only discrete actions (directions handled separately with auto-repeat)
constexpr BindAction kPanelActions[] = {
    BindAction::Confirm, BindAction::Back, BindAction::CancelVerdict, BindAction::PrevTab,
    BindAction::NextTab, BindAction::QuickSwap, BindAction::PrevIncident, BindAction::NextIncident,
    BindAction::PlayPause,
};

bool IsAnyKeyboardKeyDown() {
    for (int vk = 0x08; vk <= 0xFE; ++vk) {
        if (vk >= 0x10 && vk <= 0x12) continue; // generic modifiers mirror the L/R ones
        if (IsKeyDown(vk)) return true;
    }
    return false;
}

} // namespace

bool InputManager::InstallXInputHook() {
    if (g_xinputIatHook.IsInstalled()) return true;

    if (g_xinputIatHook.InstallByOrdinal(nullptr, "xinput1_3.dll", 2, reinterpret_cast<void*>(&Detour_XInputGetState))) {
        VAR_LOG_INF("XInput IAT hook installed on xinput1_3.dll ord_2");
        return true;
    }
    if (g_xinputIatHook.InstallByName(nullptr, "xinput1_3.dll", "XInputGetState", reinterpret_cast<void*>(&Detour_XInputGetState))) {
        VAR_LOG_INF("XInput IAT hook installed on xinput1_3.dll!XInputGetState");
        return true;
    }

    VAR_LOG_WRN("XInput IAT hook not found in main module; using fallback polling");
    return false;
}

void InputManager::UninstallXInputHook() {
    g_xinputIatHook.Uninstall();
}

bool InputManager::InstallNativeInputBlockHooks(bool quietOnMiss) {
    bool anyOk = false;

    if (!g_hookDInputKb.IsInstalled()) {
        const AobMatchResult kb = AobScanner::ScanModuleTextWithHint(
            nullptr, Aobs::DInputKeyboard_Poll, AobHints::DInputKeyboard_Poll);
        if (kb.unique()) {
            if (g_hookDInputKb.Install(kb.address, reinterpret_cast<void*>(&Detour_DInputKeyboardPoll), 5)) {
                VAR_LOG_INF("Native DInput8 Keyboard blocker installed at 0x%llX",
                            static_cast<unsigned long long>(kb.address));
                anyOk = true;
            }
        } else if (!quietOnMiss) {
            VAR_LOG_WRN("DInputKeyboard_Poll AOB not found (matchCount=%zu)", kb.matchCount);
        }
    } else {
        anyOk = true;
    }

    if (!g_hookDInputJoy.IsInstalled()) {
        const AobMatchResult joy = AobScanner::ScanModuleTextWithHint(
            nullptr, Aobs::DInputJoystick_Poll, AobHints::DInputJoystick_Poll);
        if (joy.unique()) {
            if (g_hookDInputJoy.Install(joy.address, reinterpret_cast<void*>(&Detour_DInputJoystickPoll), 5)) {
                VAR_LOG_INF("Native DInput8 Joystick blocker installed at 0x%llX",
                            static_cast<unsigned long long>(joy.address));
                anyOk = true;
            }
        }
    }

    if (!g_hookDInputMouse.IsInstalled()) {
        const AobMatchResult mouse = AobScanner::ScanModuleTextWithHint(
            nullptr, Aobs::DInputMouse_Poll, AobHints::DInputMouse_Poll);
        if (mouse.unique()) {
            if (g_hookDInputMouse.Install(mouse.address, reinterpret_cast<void*>(&Detour_DInputMousePoll), 5)) {
                VAR_LOG_INF("Native DInput8 Mouse blocker installed at 0x%llX",
                            static_cast<unsigned long long>(mouse.address));
                anyOk = true;
            }
        }
    }

    if (!g_hookPesPadUpdate.IsInstalled()) {
        const AobMatchResult pad = AobScanner::ScanModuleTextWithHint(
            nullptr, Aobs::PesPad_Update, AobHints::PesPad_Update);
        if (pad.unique()) {
            if (g_hookPesPadUpdate.Install(pad.address, reinterpret_cast<void*>(&Detour_PesPadUpdate), 8)) {
                VAR_LOG_INF("Native PES2021 PesPad::Update blocker installed at 0x%llX",
                            static_cast<unsigned long long>(pad.address));
                anyOk = true;
            }
        }
    }

    if (!g_getKbStateIatHook.IsInstalled()) {
        if (g_getKbStateIatHook.InstallByName(nullptr, "user32.dll", "GetKeyboardState",
                                              reinterpret_cast<void*>(&Detour_GetKeyboardState))) {
            VAR_LOG_INF("user32!GetKeyboardState IAT blocker installed");
            anyOk = true;
        }
    }

    // Done only when every blocker the panel relies on is in place; otherwise the worker retries
    // (bounded) instead of stopping after the first partial success and leaking keys to the replay.
    (void)anyOk;
    const bool essentialOk = g_hookDInputKb.IsInstalled() && g_hookDInputMouse.IsInstalled() &&
                             g_getKbStateIatHook.IsInstalled();
    if (!essentialOk && !quietOnMiss) {
        VAR_LOG_WRN("Input blockers incomplete: keyboard=%d mouse=%d GetKeyboardState=%d",
                    g_hookDInputKb.IsInstalled() ? 1 : 0, g_hookDInputMouse.IsInstalled() ? 1 : 0,
                    g_getKbStateIatHook.IsInstalled() ? 1 : 0);
    }
    return essentialOk;
}

void InputManager::UninstallNativeInputBlockHooks() {
    g_getKbStateIatHook.Uninstall();
    g_hookPesPadUpdate.Uninstall();
    g_hookDInputMouse.Uninstall();
    g_hookDInputJoy.Uninstall();
    g_hookDInputKb.Uninstall();
}

bool InputManager::AttachWindow(HWND hwnd) {
    if (!hwnd) return false;
    g_hwnd.store(hwnd, std::memory_order_release);
    ResetKeyEdgeTrackers();
    VAR_LOG_INF("InputManager tracking HWND=0x%p (polling mode, zero WndProc subclass)", static_cast<void*>(hwnd));
    return true;
}

void InputManager::DetachWindow() {
    g_hwnd.store(nullptr, std::memory_order_release);
}

HWND InputManager::AttachedWindow() {
    return g_hwnd.load(std::memory_order_acquire);
}

void InputManager::QueueKeyboardAction(NavAction action) {
    if (action == NavAction::None) return;
    std::lock_guard<std::mutex> lock(g_actionMutex);
    if (g_queuedKeyActions.size() < 32) {
        g_queuedKeyActions.push_back(action);
        g_hasQueuedActions.store(true, std::memory_order_release);
    }
}

bool InputManager::ConsumePendingDebugToggle() {
    bool toggled = false;
    if (g_hasQueuedActions.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(g_actionMutex);
        for (NavAction act : g_queuedKeyActions) {
            if (act == NavAction::ToggleDebug) {
                toggled = !toggled;
            }
        }
        g_queuedKeyActions.clear();
        g_hasQueuedActions.store(false, std::memory_order_release);
    }

    // Throttle idle debug-key polling to ~60 Hz: same sampling as v4.0 at 60 fps (no missed short taps),
    // but high refresh rates (120/144/240 Hz) no longer pay 2 to 4 Win32 syscalls per extra frame.
    static uint64_t s_lastIdlePollMs = 0;
    const uint64_t nowMs = GetTickCount64();
    if (nowMs - s_lastIdlePollMs >= 15ULL) { // GetTickCount64 ticks in ~15.6 ms steps
        s_lastIdlePollMs = nowMs;
        RefreshBindings();
        if (IsGameForeground() && !VarState::Instance().IsSiderOverlayOpen()) {
            if (KeyEdge(BindAction::ToggleDebug)) {
                toggled = !toggled;
            }
        } else {
            g_prevActionKey[static_cast<size_t>(BindAction::ToggleDebug)] = IsActionKeyDown(BindAction::ToggleDebug);
        }
    }
    return toggled;
}

std::vector<NavAction> InputManager::PollActions(bool inManualReplay, bool panelFocused, bool siderOverlayOpen) {
    std::vector<NavAction> out;
    if (g_hasQueuedActions.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(g_actionMutex);
        out.swap(g_queuedKeyActions);
        g_hasQueuedActions.store(false, std::memory_order_release);
    }

    RefreshBindings();

    if (siderOverlayOpen || g_capture.active) {
        // Sider overlay open, or the Controls page is capturing a new shortcut: no navigation at all
        if (siderOverlayOpen) out.clear();
        g_padTracker.lastMask = ReadCurrentPadButtons();
        ResetKeyEdgeTrackers();
        return out;
    }

    const uint64_t now = GetTickCount64();

    // 1. Keyboard polling (only when PES 2021 window is in foreground)
    if (IsGameForeground()) {
        if (KeyEdge(BindAction::ToggleDebug)) out.push_back(NavAction::ToggleDebug);

        const bool toggleEdge = KeyEdge(BindAction::ToggleFocus);
        if (inManualReplay && toggleEdge) {
            out.push_back(NavAction::ToggleFocus);
        }

        if (inManualReplay && panelFocused) {
            for (BindAction a : kPanelActions) {
                if (KeyEdge(a)) out.push_back(kNavForBind[static_cast<size_t>(a)]);
            }

            uint16_t kDir = 0;
            if (IsActionKeyDown(BindAction::Up))    kDir |= PadButtons::DpadUp;
            if (IsActionKeyDown(BindAction::Down))  kDir |= PadButtons::DpadDown;
            if (IsActionKeyDown(BindAction::Left))  kDir |= PadButtons::DpadLeft;
            if (IsActionKeyDown(BindAction::Right)) kDir |= PadButtons::DpadRight;

            const uint16_t kPressed = static_cast<uint16_t>(kDir & ~g_keyDirTracker.lastMask);
            bool emitKeyDir = false;
            if (kPressed != 0) {
                emitKeyDir = true;
                g_keyDirTracker.holdStartMs = now;
                g_keyDirTracker.lastRepeatMs = now;
            } else if (kDir != 0 && (now - g_keyDirTracker.holdStartMs) >= 340 && (now - g_keyDirTracker.lastRepeatMs) >= 110) {
                emitKeyDir = true;
                g_keyDirTracker.lastRepeatMs = now;
            }
            if (emitKeyDir) {
                if (kDir & PadButtons::DpadUp)    out.push_back(NavAction::Up);
                if (kDir & PadButtons::DpadDown)  out.push_back(NavAction::Down);
                if (kDir & PadButtons::DpadLeft)  out.push_back(NavAction::Left);
                if (kDir & PadButtons::DpadRight) out.push_back(NavAction::Right);
            }
            g_keyDirTracker.lastMask = kDir;
        } else {
            for (BindAction a : kPanelActions) {
                g_prevActionKey[static_cast<size_t>(a)] = IsActionKeyDown(a);
            }
            g_keyDirTracker.lastMask = 0;
        }
    } else {
        ResetKeyEdgeTrackers();
    }

    if (!out.empty()) g_preferPadGlyphs = false; // keyboard produced something this frame

    // 2. XInput gamepad polling
    const uint16_t cur = ReadCurrentPadButtons();
    const uint16_t pressed = static_cast<uint16_t>(cur & ~g_padTracker.lastMask);
    if (pressed & g_bindings.pad[static_cast<size_t>(BindAction::ToggleFocus)]) g_preferPadGlyphs = true;

    // Open/collapse button (R3 by default) toggles VAR panel focus when inside Manual Replay
    const uint16_t togglePad = g_bindings.pad[static_cast<size_t>(BindAction::ToggleFocus)];
    if (inManualReplay && togglePad != 0 && (pressed & togglePad)) {
        out.push_back(NavAction::ToggleFocus);
    }

    // The gamepad never navigates the panel (keyboard and mouse only): its D-Pad, sticks and face buttons
    // keep driving the Replay. Only the open/collapse button above is handled here.
    g_padTracker.lastMask = cur;
    return out;
}

bool InputManager::PreferPadGlyphs() {
    return g_preferPadGlyphs;
}

void InputManager::NotePointerActivity() {
    g_preferPadGlyphs = false;
}

// ============================================================================
// Remap capture
// ============================================================================
void InputManager::BeginCapture(int column) {
    g_capture = CaptureCtx{};
    g_capture.active = true;
    g_capture.waitingRelease = true;
    g_capture.column = (column < 0) ? 0 : (column > 2 ? 2 : column);
    g_capture.startMs = GetTickCount64();
    g_capture.prevEsc = true;
}

void InputManager::CancelCapture() {
    if (!g_capture.active) return;
    g_capture.active = false;
    g_padTracker.lastMask = ReadCurrentPadButtons();
    ResetKeyEdgeTrackers();
}

bool InputManager::IsCapturing() {
    return g_capture.active;
}

InputManager::CaptureState InputManager::PollCapture(uint16_t& outCode) {
    outCode = 0;
    if (!g_capture.active) return CaptureState::Idle;

    const uint64_t now = GetTickCount64();
    auto finish = [](CaptureState s) {
        g_capture.active = false;
        g_padTracker.lastMask = ReadCurrentPadButtons();
        ResetKeyEdgeTrackers();
        return s;
    };

    if (now - g_capture.startMs > 8000ULL || VarState::Instance().IsSiderOverlayOpen()) {
        return finish(CaptureState::Cancelled);
    }
    if (!IsGameForeground()) return CaptureState::Pending;

    const uint16_t pad = ReadCurrentPadButtons();
    constexpr uint16_t kPadCandidates = 0x0010 | 0x0020 | 0x0040 | 0x0080 | 0x0100 | 0x0200 |
                                        0x1000 | 0x2000 | 0x4000 | 0x8000;

    // Phase 1: wait until the key/button that opened the capture (and everything else) is released
    if (g_capture.waitingRelease) {
        if (!IsAnyKeyboardKeyDown() && (pad & kPadCandidates) == 0) {
            g_capture.waitingRelease = false;
            g_capture.prevEsc = false;
        }
        return CaptureState::Pending;
    }

    // Esc always cancels (never captured)
    const bool esc = IsKeyDown(VK_ESCAPE);
    if (esc && !g_capture.prevEsc) return finish(CaptureState::Cancelled);
    g_capture.prevEsc = esc;

    if (g_capture.column == 2) {
        const uint16_t hit = static_cast<uint16_t>(pad & kPadCandidates);
        if (hit != 0) {
            outCode = static_cast<uint16_t>(hit & (~hit + 1)); // lowest pressed button
            return finish(CaptureState::Captured);
        }
        return CaptureState::Pending;
    }

    // Prefer specific L/R modifier codes and skip mouse buttons / generic modifiers / Esc
    for (int vk = 0x08; vk <= 0xFE; ++vk) {
        if (vk == VK_ESCAPE || (vk >= 0x10 && vk <= 0x12)) continue;
        if (IsKeyDown(vk)) {
            outCode = static_cast<uint16_t>(vk);
            return finish(CaptureState::Captured);
        }
    }
    return CaptureState::Pending;
}

// ============================================================================
// Names
// ============================================================================
void InputManager::FormatKeyName(uint8_t vk, char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    out[0] = '\0';
    if (vk == 0) { std::snprintf(out, outSize, "-"); return; }

    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
        std::snprintf(out, outSize, "%c", static_cast<char>(vk));
        return;
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        std::snprintf(out, outSize, "F%d", vk - VK_F1 + 1);
        return;
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        std::snprintf(out, outSize, "Num %d", vk - VK_NUMPAD0);
        return;
    }

    const char* name = nullptr;
    switch (vk) {
        case VK_RETURN:   name = "Enter"; break;
        case VK_ESCAPE:   name = "Esc"; break;
        case VK_BACK:     name = "Backspace"; break;
        case VK_TAB:      name = "Tab"; break;
        case VK_SPACE:    name = Tr("Espaço"); break;
        case VK_PRIOR:    name = "Page Up"; break;
        case VK_NEXT:     name = "Page Down"; break;
        case VK_END:      name = "End"; break;
        case VK_HOME:     name = "Home"; break;
        case VK_INSERT:   name = "Insert"; break;
        case VK_DELETE:   name = "Delete"; break;
        case VK_UP:       name = Tr("Seta cima"); break;
        case VK_DOWN:     name = Tr("Seta baixo"); break;
        case VK_LEFT:     name = Tr("Seta esq."); break;
        case VK_RIGHT:    name = Tr("Seta dir."); break;
        case VK_PAUSE:    name = "Pause"; break;
        case VK_CAPITAL:  name = "Caps Lock"; break;
        case VK_NUMLOCK:  name = "Num Lock"; break;
        case VK_SCROLL:   name = "Scroll Lock"; break;
        case VK_SNAPSHOT: name = "Print Screen"; break;
        case VK_LSHIFT:   name = Tr("Shift esq."); break;
        case VK_RSHIFT:   name = Tr("Shift dir."); break;
        case VK_LCONTROL: name = Tr("Ctrl esq."); break;
        case VK_RCONTROL: name = Tr("Ctrl dir."); break;
        case VK_LMENU:    name = Tr("Alt esq."); break;
        case VK_RMENU:    name = Tr("Alt dir."); break;
        case VK_LWIN:     name = Tr("Win esq."); break;
        case VK_RWIN:     name = Tr("Win dir."); break;
        case VK_MULTIPLY: name = "Num *"; break;
        case VK_ADD:      name = "Num +"; break;
        case VK_SUBTRACT: name = "Num -"; break;
        case VK_DECIMAL:  name = "Num ."; break;
        case VK_DIVIDE:   name = "Num /"; break;
        default: break;
    }
    if (name) {
        std::snprintf(out, outSize, "%s", name);
        return;
    }

    // Punctuation / OEM keys: ask Windows for the character on the user's real layout (ABNT2, US, ...)
    const UINT ch = MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0x7FFFU;
    if (ch >= 0x21 && ch < 0x7F) {
        std::snprintf(out, outSize, "%c", static_cast<char>(ch));
        return;
    }
    if (ch >= 0xA0 && ch <= 0xFF) {
        // Latin-1 (e.g. the ABNT2 C-cedilla key): emit UTF-8, the HUD font covers Latin-1
        std::snprintf(out, outSize, "%c%c", static_cast<char>(0xC0 | (ch >> 6)), static_cast<char>(0x80 | (ch & 0x3F)));
        return;
    }
    std::snprintf(out, outSize, Tr("Tecla 0x%02X"), static_cast<unsigned>(vk));
}

void InputManager::FormatPadName(uint16_t mask, char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    const char* name = "-";
    switch (mask) {
        case PadButtons::A:             name = "A"; break;
        case PadButtons::B:             name = "B"; break;
        case PadButtons::X:             name = "X"; break;
        case PadButtons::Y:             name = "Y"; break;
        case PadButtons::LeftShoulder:  name = "LB"; break;
        case PadButtons::RightShoulder: name = "RB"; break;
        case PadButtons::LeftThumb:     name = "L3"; break;
        case PadButtons::RightThumb:    name = "R3"; break;
        case PadButtons::Start:         name = "Start"; break;
        case PadButtons::Back:          name = "Select"; break;
        case PadButtons::DpadUp:        name = "D-Pad Up"; break;
        case PadButtons::DpadDown:      name = "D-Pad Down"; break;
        case PadButtons::DpadLeft:      name = "D-Pad Left"; break;
        case PadButtons::DpadRight:     name = "D-Pad Right"; break;
        default: break;
    }
    std::snprintf(out, outSize, "%s", name);
}

void InputManager::FormatActionHint(BindAction action, char* out, size_t outSize, bool includePad) {
    RefreshBindings();
    FormatHintFrom(g_bindings, action, out, outSize, includePad);
}

void InputManager::FormatHintFrom(const InputBindings& bindings, BindAction action, char* out, size_t outSize,
                                  bool includePad) {
    if (!out || outSize == 0) return;
    out[0] = '\0';
    const auto i = static_cast<size_t>(action);
    if (i >= static_cast<size_t>(kBindActionCount)) return;

    size_t used = 0;
    auto append = [&](const char* s) {
        if (!s || !s[0] || used + 1 >= outSize) return;
        const int n = std::snprintf(out + used, outSize - used, "%s%s", used ? " / " : "", s);
        if (n > 0) used = std::min(outSize - 1, used + static_cast<size_t>(n));
    };
    char buf[32];
    for (uint8_t vk : bindings.keys[i]) {
        if (vk == 0) continue;
        FormatKeyName(vk, buf, sizeof(buf));
        append(buf);
    }
    if (includePad && InputBindings::IsPadRemappable(action) && bindings.pad[i] != 0) {
        FormatPadName(bindings.pad[i], buf, sizeof(buf));
        append(buf);
    }
    if (used == 0) std::snprintf(out, outSize, "-");
}

} // namespace var_mod