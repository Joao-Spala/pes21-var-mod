#pragma once

#include "var_state.h"

#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace var_mod {

// Standard Windows XInput ABI definitions (avoids MinGW header variance)
#pragma pack(push, 1)
struct RawXInputGamepad {
    uint16_t wButtons;
    uint8_t  bLeftTrigger;
    uint8_t  bRightTrigger;
    int16_t  sThumbLX;
    int16_t  sThumbLY;
    int16_t  sThumbRX;
    int16_t  sThumbRY;
};

struct RawXInputState {
    uint32_t         dwPacketNumber;
    RawXInputGamepad Gamepad;
};
#pragma pack(pop)

namespace PadButtons {
    constexpr uint16_t DpadUp        = 0x0001;
    constexpr uint16_t DpadDown      = 0x0002;
    constexpr uint16_t DpadLeft      = 0x0004;
    constexpr uint16_t DpadRight     = 0x0008;
    constexpr uint16_t Start         = 0x0010;
    constexpr uint16_t Back          = 0x0020;
    constexpr uint16_t LeftThumb     = 0x0040;
    constexpr uint16_t RightThumb    = 0x0080;
    constexpr uint16_t LeftShoulder  = 0x0100;
    constexpr uint16_t RightShoulder = 0x0200;
    constexpr uint16_t A             = 0x1000;
    constexpr uint16_t B             = 0x2000;
    constexpr uint16_t X             = 0x4000;
    constexpr uint16_t Y             = 0x8000;

    // Masked from game ONLY when VAR panel is actively focused
    // Keeps LS, RS, LT, RT (Replay camera/timeline) and Back/SELECT (Replay HUD toggle) 100% free!
    constexpr uint16_t VarFocusMask =
        DpadUp | DpadDown | DpadLeft | DpadRight |
        RightThumb | LeftShoulder | RightShoulder | A | B | X | Y;
} // namespace PadButtons

enum class NavAction : int32_t {
    None = 0,
    ToggleFocus,
    ToggleDebug,
    Up,
    Down,
    Left,
    Right,
    Confirm,
    Back,
    CancelVerdict,
    PrevTab,
    NextTab,
    QuickSwap,       // Y on gamepad / Tab on keyboard (swap offender/victim or jump to Squad)
    PrevIncident,    // '[' key: jump Replay natively to previous detected incident
    NextIncident,    // ']' key: jump Replay natively to next detected incident
    TogglePlayPause, // 'P' key: toggle native Replay Play/Pause
};

class InputManager {
public:
    static bool InstallXInputHook();
    static void UninstallXInputHook();

    // Hooks native Fox Engine DirectInput8 Keyboard/Joystick poll (0x141B638B0 / 0x141B63900)
    // and user32!GetKeyboardState IAT so replay never receives keys/buttons while VAR panel is open.
    static bool InstallNativeInputBlockHooks(bool quietOnMiss = false);
    static void UninstallNativeInputBlockHooks();

    // Stores target game HWND for foreground focus checks (never subclasses GWLP_WNDPROC)
    static bool AttachWindow(HWND hwnd);
    static void DetachWindow();
    static HWND AttachedWindow();

    // Polls keyboard/gamepad edges on the render thread and returns queued navigation actions.
    static std::vector<NavAction> PollActions(bool inManualReplay, bool panelFocused, bool siderOverlayOpen);

    // Lightweight check when HUD is idle (polls the debug key edge without polling gamepad).
    static bool ConsumePendingDebugToggle();

    // Direct push (used by unit tests)
    static void QueueKeyboardAction(NavAction action);

    // ---- Prompt glyphs follow the last device the user touched (gamepad vs keyboard/mouse)
    static bool PreferPadGlyphs();
    static void NotePointerActivity(); // mouse moved over the HUD

    // ---- Remap capture (Controls page). column 0/1 = keyboard slot, 2 = gamepad button.
    enum class CaptureState : int32_t { Idle = 0, Pending, Captured, Cancelled };
    static void BeginCapture(int column);
    static void CancelCapture();
    static bool IsCapturing();
    // Call once per frame while capturing. Waits for every key/button to be released first, so the
    // Enter / A press that opened the capture is never recorded. Esc or 8 s timeout cancels.
    static CaptureState PollCapture(uint16_t& outCode);

    // ---- Human readable names (follow the selected UI language and the user's keyboard layout)
    static void FormatKeyName(uint8_t vk, char* out, size_t outSize);
    static void FormatPadName(uint16_t mask, char* out, size_t outSize);
    // "V / F7 / R3" style hint with every shortcut currently bound to the action
    static void FormatActionHint(BindAction action, char* out, size_t outSize, bool includePad = true);
    // Thread-safe variant (no render-thread cache access): used by the Sider overlay status text
    static void FormatHintFrom(const InputBindings& b, BindAction action, char* out, size_t outSize, bool includePad = true);
};

} // namespace var_mod
