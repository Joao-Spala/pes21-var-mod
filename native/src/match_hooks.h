#pragma once

#include <cstdint>

namespace var_mod {

// AOB signatures (all verified unique in PES2021.exe Steam 1.7.2.0 .text section)
namespace Aobs {
    constexpr const char* MainTick =
        "48 89 5C 24 20 4C 89 44 24 18 48 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 60";

    constexpr const char* JudgeFinalizeFrame =
        "33 C0 48 89 81 B8 03 00 00 48 89 81 C0 03 00 00 C3";

    constexpr const char* UpdateFoulCounters =
        "41 80 B8 60 22 00 00 00 74 13 8B 82 44 01 00 00";

    constexpr const char* MatchFSM_EnterOutOfPlay =
        "48 89 5C 24 08 57 48 83 EC 30 80 B9 11 4A 00 00";

    constexpr const char* MatchFSM_RebuildSetPlay =
        "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 33 D2 48 81 C1 90 27";

    constexpr const char* MatchInfo_GetTimeFloat =
        "40 53 48 83 EC 20 48 8B D9 E8 12 00 00 00 F3 0F";

    constexpr const char* MatchInfo_SetPlayerOnPitch =
        "83 FA 15 77 12 45 84 C0 48 63 C2 41 0F 95 C0 44 88 84 08 48";

    // Call site at 0x1404231DC; +6 is `call TeamInfo_GetPlayerBySlot` (0x140A3B5B0)
    constexpr const char* CommitCardsCallSite =
        "8B 9E 48 29 00 00 E8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 83 EB 03 74 07 83 FB 01";

    // Getter at 0x1414BC550: `mov rax, [rip+disp]; ret` -> resolves MatchRoster global
    constexpr const char* MatchRosterGetter =
        "48 8B 05 E9 7C 25 02 C3 CC CC CC CC CC CC CC CC 83 79 30 04 0F 94 C0";

    // ManualReplayController::SyncToReplayManager (0x1420C16A0):
    // Clean 20-byte non-RIP-relative prologue; runs every frame during Manual Replay (playing or paused).
    // +0x32 holds `mov rax, [rip + disp32]` resolving ReplaySingleton global (0x143703558).
    constexpr const char* ReplayCtrl_SyncToManager =
        "40 53 55 56 57 41 54 41 55 41 56 41 57 48 81 EC D8 00 00 00 48 8B 99 30 04 00 00 48 8B F1";

    // ReplayPlayer::SetFrame(void* replayPlayer, uint16_t targetFrame) at 0x14217E800:
    // Validates circular window, updates currentFrame (+0x238), floatFrame (+0x14), delta (+0x18=0),
    // and tail-calls ReplayPlayer::UpdatePlayback(0.0f).
    constexpr const char* ReplayPlayer_SetFrame =
        "0F B7 81 3C 02 00 00 4C 8B C9 44 0F B7 81 40 02 00 00 66 41 3B C0";

    // ReplayPlayer::GetFocusPos(void* replayPlayer, int32_t focusTarget) at 0x14217EC70:
    // Returns const float[3] world position of focus target (0=Ball, 1..22=Player slot+1) at current replay frame.
    constexpr const char* ReplayPlayer_GetFocusPos =
        "48 63 C2 48 8B 8C C1 20 01 00 00 48 85 C9 74 07 48 8B 01 48 FF 60 08";

    // Native Fox Engine GrSwapChain::Present0(GrSwapChain* this, UINT syncInterval) at 0x141FF46A0.
    constexpr const char* GrSwapChain_Present0 =
        "48 8B 49 20 45 33 C0 48 8B 01 48 FF 60 40";

    // Native Fox Engine DirectInput8 Keyboard::Poll (0x141B638B0) and Joystick::Poll (0x141B63900).
    constexpr const char* DInputKeyboard_Poll =
        "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 BA 00 01 00 00 48 8B 49 10 4C 8D 83 68 02 00 00";

    constexpr const char* DInputJoystick_Poll =
        "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 BA 50 00 00 00 48 8B 49 10 4C 8D 83 68 02 00 00";

    // Native Fox Engine DirectInput8 Mouse::Poll (0x141B63950, 16-byte DIMOUSESTATE at [this + 0x268])
    constexpr const char* DInputMouse_Poll =
        "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 BA 10 00 00 00 48 8B 49 10 4C 8D 83 68 02 00 00";

    // PES 2021 PadUpdate (0x1403F86E0): dispatches DirectInput Joystick (0x1403F8710) or XInput (0x1403F8CE0).
    constexpr const char* PesPad_Update =
        "48 83 79 18 00 48 8B D1 74 14 B8 5E 04 00 00 66 39 41 10";
} // namespace Aobs

// Fast O(1) Steam 1.7.2.0 RVA hints (validated against pattern; falls back to full .text scan on mismatch)
namespace AobHints {
    constexpr uintptr_t MainTick                   = 0x04468F0;
    constexpr uintptr_t JudgeFinalizeFrame         = 0x050CD70;
    constexpr uintptr_t UpdateFoulCounters         = 0x0424410;
    constexpr uintptr_t MatchFSM_EnterOutOfPlay    = 0x04541A0;
    constexpr uintptr_t MatchFSM_RebuildSetPlay    = 0x0454280;
    constexpr uintptr_t MatchInfo_GetTimeFloat     = 0x0A3C680;
    constexpr uintptr_t MatchInfo_SetPlayerOnPitch = 0x0A3D0E0;
    constexpr uintptr_t CommitCardsCallSite        = 0x04231DC;
    constexpr uintptr_t MatchRosterGetter          = 0x14BC550;
    constexpr uintptr_t ReplayCtrl_SyncToManager   = 0x20C16A0;
    constexpr uintptr_t ReplayPlayer_SetFrame      = 0x217E800;
    constexpr uintptr_t ReplayPlayer_GetFocusPos   = 0x217EC70;
    constexpr uintptr_t GrSwapChain_Present0       = 0x1FF46A0;
    constexpr uintptr_t DInputKeyboard_Poll        = 0x1B638B0;
    constexpr uintptr_t DInputJoystick_Poll        = 0x1B63900;
    constexpr uintptr_t DInputMouse_Poll           = 0x1B63950;
    constexpr uintptr_t PesPad_Update              = 0x03F86E0;
} // namespace AobHints

class MatchHooks {
public:
    static bool Install(bool quietOnMiss = false);
    static void Uninstall();
    static bool IsInstalled();
    static void OnMatchReset();

    // Safe read-only live poll callable from DX11 Present thread so Replay cursor & ManualFlag
    // stay 100% synced even when simulation (MatchFSM::MainTick) is paused during Replay.
    static void PollLiveReplayState();
};

} // namespace var_mod
