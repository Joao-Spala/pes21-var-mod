#pragma once

#include <cstdint>

namespace var_mod {

// AOB signatures (all verified unique in PES2021.exe Steam 1.7.2.0 .text section)
namespace Aobs {
    // Prologue up to and including `mov rax, [rip+disp32]` at +0x30 (the GMATCH global, disp differs per build)
    constexpr const char* MainTick =
        "48 89 5C 24 20 4C 89 44 24 18 48 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 60 "
        "33 DB 48 8B E9 33 FF 66 66 66 0F 1F 84 00 00 00 00 00 48 8B 05 ?? ?? ?? ??";

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
        "48 8B 05 ?? ?? ?? ?? C3 CC CC CC CC CC CC CC CC 83 79 30 04 0F 94 C0"; // disp differs per build

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

    // FoulSnapshotPool::Find(pool, id) at 0x140A82120: linear search of ids[3] at pool + 0xBAC,
    // returns pool + idx * 0x3E4 or NULL. Clean 6-byte prologue (mov r8d, edx / mov r9, rcx).
    constexpr const char* FoulSnapshotPool_Find =
        "44 8B C2 4C 8B C9 85 D2 74 1F 33 D2 49 8D 81 AC 0B 00 00 8B CA 44 39 00 74 12";

    // Native vanishing spray reset (0x140452EB0, reached from the MatchControl message handler 0x140423B80):
    //   SprayRef_GetWrite(fsm + 0x17CA8) -> VanishingSpray_Reset(spray) -> SprayRef_Commit(fsm + 0x17CA8).
    // The three call targets are resolved from this single site (+0x14, +0x21, +0x2D).
    constexpr const char* VanishingSpray_NativeReset =
        "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 48 81 C1 A8 7C 01 00 E8 ?? ?? ?? ?? 48 85 C0 74 08 48 8B C8 "
        "E8 ?? ?? ?? ?? 48 8D 8B A8 7C 01 00 E8 ?? ?? ?? ?? C6 83 04 09 00 00 01";

    // RecordHistory::AddEvent(history, event) at 0x140A3E260: every match event enters the record history here,
    // before the stats dispatcher reads it back. Clean 11-byte prologue (40 55 / 56 / 57 / 48 81 EC 80 06 00 00).
    // Complete spray clear the engine runs for message 0x105001A (0x140445FD0, called from the MatchControl
    // message handler 0x140423B80): the same data reset as VanishingSpray_NativeReset, then a tail jump to
    // VanishingSpray_ClearDrawn, which removes the spray already painted on the pitch under the renderer lock.
    // Whole bodies, address fields masked: the match itself proves both functions.
    constexpr const char* VanishingSpray_FullReset =
        "40 53 48 83 EC 20 48 8D 99 A8 7C 01 00 48 8B CB E8 ?? ?? ?? ?? 48 85 C0 74 08 48 8B C8 E8 ?? ?? ?? ?? "
        "48 8B CB E8 ?? ?? ?? ?? 48 83 C4 20 5B E9 ?? ?? ?? ??";
    constexpr size_t VanishingSpray_FullResetTailJump = 0x2F; // E9 rel32 -> VanishingSpray_ClearDrawn
    constexpr const char* VanishingSpray_ClearDrawn =
        "40 53 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 8D 1D ?? ?? ?? ?? 48 89 5C 24 40 48 8B CB E8 ?? ?? ?? ?? 90 "
        "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 1B 48 8B 90 68 01 00 00 48 85 D2 74 0F 48 8B 4A 08 48 85 C9 74 06 "
        "E8 ?? ?? ?? ?? 90 48 8B CB E8 ?? ?? ?? ?? 90 48 83 C4 30 5B C3";

    constexpr const char* RecordHistory_AddEvent =
        "40 55 56 57 48 81 EC 80 06 00 00 48 C7 44 24 28 FE FF FF FF 48 89 9C 24 B0 06 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4";

    // RecordHistory::ResetPending(history) at 0x140A3E760 (xor eax,eax / mov [rcx+0x271F0],rax / ret): called only
    // by the record manager update (0x140A0D842, 0x140A0DAF0, 0x140A0DC1D), so a write to the history made here
    // runs on the record update's own thread, between its steps. 9-byte stolen prologue.
    constexpr const char* RecordHistory_ResetPending =
        "33 C0 48 89 81 F0 71 02 00 C3";

    // SimMain@Screen@match2D vfunc 19 (0x142101130): compares the score bytes of its data object (*(this+0x38) +
    // 0x9E home / +0x9F away) with the score on screen and redraws when they differ. Hooked only to learn `this`.
    // Clean 9-byte prologue (40 53 / 48 83 EC 20 / 48 8B D9).
    // FixDemoMain clear of its cutscene map (0x141F3F3A0, this + 0xB08 head / + 0xB10 size): walks the tree with
    // the in-order successor. A corrupted node link crashed it at the end of a match (0x141F3F3FA). Clean 11-byte
    // prologue (48 89 7C 24 20 / 41 56 / 48 83 EC 40).
    constexpr const char* FixDemoMain_ClearMap =
        "48 89 7C 24 20 41 56 48 83 EC 40 48 89 5C 24 50 48 8B F9 48 89 74 24 60 0F B6 F2 84 D2 0F 84";

    constexpr const char* SimMain_UpdateScore =
        "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 4C 8B 4B 38 4C 8D 43 4C 33 C9";
} // namespace Aobs

// Call sites of FoulSnapshotPool::Find whose callers dereference the result WITHOUT a NULL check
// (foul event consumer 0x1404B4870 and its helpers 0x1404B4B40 / 0x1404B4D90). Verified in Steam 1.7.2.0:
// each holds `E8 rel32` targeting FoulSnapshotPool_Find. A missing snapshot id there is the
// PES2021.exe+0x4B4C25 crash. Every other caller (0x140510EE6, 0x140A1FCAA, 0x140A347F7, 0x140A3D71C,
// 0x14145FDCA, 0x141477EF7) already tests for NULL and keeps the native result.
// Found by signature (16 bytes of context, the call, 12 bytes after), never by address: the context holds the
// pool offset 0x17AC and the FoulInfo snapshot field 0x15AC, so a layout change stops the match. The call is
// at kCallOffset in each pattern. Hints = reference build addresses of the call.
namespace FoulSnapshotUncheckedCallSites {
    constexpr const char* Patterns[] = {
        "49 8B 0E 48 8B 09 48 81 C1 AC 17 00 00 8B 55 88 E8 ?? ?? ?? ?? 4C 8B E0 8B 4C 24 68 83 F9 01 75 33",
        "74 24 50 48 8D 8A AC 17 00 00 8B 92 AC 15 00 00 E8 ?? ?? ?? ?? 49 8B 0F 4C 8B E0 48 8B 54 24 38 4C",
        "48 8B 10 48 8D 8A AC 17 00 00 8B 92 AC 15 00 00 E8 ?? ?? ?? ?? 4C 8B D8 48 89 44 24 20 49 6B C6 2C",
    };
    constexpr uintptr_t HintRvas[] = { 0x04B49BD, 0x04B4BC7, 0x04B4EA0 };
    constexpr size_t kCallOffset = 16;
} // namespace FoulSnapshotUncheckedCallSites

// Vtables the mod compares objects against, found by their MSVC RTTI name at install (the RVA differs per
// build). Hints = reference build vtable RVAs.
namespace RttiVtables {
    constexpr const char* ReplayCameraLow   = ".?AVCameraLow@match@@";          // replay camera mode 0 controller
    constexpr const char* ReplayCameraUp    = ".?AVCameraUp@match@@";           // replay camera mode 1 controller
    constexpr const char* VanishingSprayRef = ".?AVVanishingSprayInfoRef@registry@match@@";
    constexpr const char* SimMain           = ".?AVSimMain@Screen@match2D@@";   // scoreboard model
    constexpr uintptr_t ReplayCameraLowHint   = 0x2BDFB40;
    constexpr uintptr_t ReplayCameraUpHint    = 0x2BDFB88;
    constexpr uintptr_t VanishingSprayRefHint = 0x25A1570;
    constexpr uintptr_t SimMainHint           = 0x2BF58F8;
} // namespace RttiVtables

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
    constexpr uintptr_t FoulSnapshotPool_Find      = 0x0A82120;
    constexpr uintptr_t VanishingSpray_NativeReset = 0x0452EB0;
    constexpr uintptr_t RecordHistory_AddEvent     = 0x0A3E260;
    constexpr uintptr_t VanishingSpray_FullReset   = 0x0445FD0;
    constexpr uintptr_t VanishingSpray_ClearDrawn  = 0x1EB3700;
    constexpr uintptr_t RecordHistory_ResetPending = 0x0A3E760;
    constexpr uintptr_t SimMain_UpdateScore        = 0x2101130;
    constexpr uintptr_t FixDemoMain_ClearMap       = 0x1F3F3A0;
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

    // Render thread, every frame: logs where the sim thread is when MainTick stops for 4 s outside replays
    static void CheckSimStall();
};

} // namespace var_mod
