#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace var_mod {

// ============================================================================
// Verified PES 2021 (Steam 1.7.2.0) enums, struct offsets and pitch geometry
// Every offset below is binary-verified via disassembly & Ghidra decompilation
// ============================================================================

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct RgbColor {
    uint8_t r = 220;
    uint8_t g = 220;
    uint8_t b = 220;
};

enum class MatchStatus : int32_t {
    Unknown      = 0,
    InPlay       = 1,
    OutOfPlay    = 2,
    SetPlayReady = 3,
};

enum class SetPlayKind : int32_t {
    None        = 0,
    KickOff     = 1,
    ThrowIn     = 2,
    CornerKick  = 3,
    GoalKick    = 4,
    FreeKick    = 5,
    PenaltyKick = 6,
    TimeEnd     = 7,
};

enum class StoppageReason : int32_t {
    InitOrKickOff  = 0,
    HalfOrFullEnd  = 1,
    QuickOut       = 2,
    Goal           = 3,
    BallOut        = 4,
    Foul           = 5,
    RebuildSetPlay = 6,
    SilentRestart  = 7,
    Shootout       = 8,
};

enum class FoulKind : int32_t {
    None           = 0,
    Offside        = 1,
    StandardFoul   = 2,
    Simulation     = 3,
    HandOrIndirect = 4,
};

enum class CardKind : int32_t {
    None    = 0,
    Simple  = 1, // Foul without card
    Warning = 2, // Verbal warning
    Yellow  = 3,
    Red     = 4,
};

// PlayerAction + 0x01 is the engine ANIME_* enum, read as a SIGNED byte (movsbl). Names from the pointer table at
// 0x1434B3470 (index 11 -> "ANIME_SLIDING", 13 -> "ANIME_TACKLE"), categories from 0x142651590: bit7 only for 11/12
// (0x140A6EAE0), bit6 only for 13 (0x140A6EBB0), bit13 for 15/16/17, bit9 for 56..61 (goalkeeper).
namespace PlayerActionId {
    constexpr int8_t Sliding     = 11; // slide tackle
    constexpr int8_t SlidingKeep = 12; // slide tackle (sustained)
    constexpr int8_t Tackle      = 13; // standing tackle (includes shoulder-charge motions)
    constexpr int8_t Block       = 14; // shot / cross block (ObserverBlock 0x140A1ADA0)
    constexpr int8_t Dive        = 15; // dive: excluded by the Judge contact filter 0x14050E050 (simulation)
    constexpr int8_t Stagger     = 16; // contact reaction
    constexpr int8_t FallDown    = 17; // contact reaction (fall)
    constexpr int8_t Jostle      = 72; // body-to-body duel
    constexpr int8_t KeeperFirst = 55; // goalkeeper actions 55..71 never produce an outfield incident
    constexpr int8_t KeeperLast  = 71;

    // PlayerAction + 0x08
    constexpr uint32_t FlagFallOrStagger = (1U << 13); // ObserverFallDownOrStagger trigger (0x140A6E9F0)
    constexpr uint32_t FlagSimulation    = (1U << 29); // read by the Judge as simulation (0x1405B7E80)

    inline bool IsSlide(int8_t a) { return a == Sliding || a == SlidingKeep; }
    inline bool IsKeeper(int8_t a) { return a >= KeeperFirst && a <= KeeperLast; }
    inline bool IsContactReaction(int8_t a, uint32_t flags) {
        return a == Stagger || a == FallDown || (a != Dive && (flags & FlagFallOrStagger) != 0);
    }
} // namespace PlayerActionId

// Exact binary layout of SetPlayInfo (0x30 bytes)
// Located at MatchInfo + 0x1554, MatchFSM + 0x49C0, MatchControl + 0x2910
#pragma pack(push, 1)
struct RawSetPlayInfo {
    int32_t  setPlayKind;     // +0x00
    int32_t  benefitedTeam;   // +0x04 (0 = Home, 1 = Away)
    Vec3     posA;            // +0x08
    Vec3     posB;            // +0x14
    int32_t  stoppageReason;  // +0x20
    uint32_t reserved24;      // +0x24
    uint32_t eventPlayerSlot; // +0x28 (0..21 or 0xFF)
    uint32_t reserved2C;      // +0x2C
};
static_assert(sizeof(RawSetPlayInfo) == 0x30, "RawSetPlayInfo size mismatch");

// Exact binary layout of FoulInfo (0x3C bytes)
// Located at MatchInfo + 0x1584, MatchFSM + 0x49F0, MatchControl + 0x2940
struct RawFoulInfo {
    uint8_t  isValid;           // +0x00 (JudgeResult +0x30)
    uint8_t  pad01[3];          // +0x01
    int32_t  foulKind;          // +0x04 (1=Offside, 2=StandardFoul)
    int32_t  foulJudgementKind; // +0x08 (0=None, 1=Simple, 2=Warning, 3=Yellow, 4=Red)
    int32_t  offenderSlot;      // +0x0C (0..21 or 0xFF)
    int32_t  victimSlot;        // +0x10 (0..21 or 0xFF)
    uint8_t  unknown14[12];     // +0x14..+0x1F
    uint8_t  wasAdvantageFoul;  // +0x20 (JudgeResult +0x50)
    uint8_t  goalOnAdvantage;   // +0x21 (JudgeResult +0x51)
    uint8_t  pad22[6];          // +0x22..+0x27
    uint32_t snapshotTickIndex; // +0x28 (JudgeResult +0x58)
    uint32_t contactCategory;   // +0x2C (JudgeResult +0x5C)
    uint8_t  tail30[12];        // +0x30..+0x3B
};
static_assert(sizeof(RawFoulInfo) == 0x3C, "RawFoulInfo size mismatch");
#pragma pack(pop)

// Verified field offsets in PES2021.exe (Steam 1.7.2.0)
namespace Offsets {
    // GMATCH (verified via 0x140A48F00..0x140A49020)
    constexpr uintptr_t GMATCH_Ball              = 0x40;
    constexpr uintptr_t GMATCH_PlayerStatus      = 0x180;
    constexpr uintptr_t GMATCH_MatchInfo         = 0x270;
    constexpr uintptr_t GMATCH_ReplayManager     = 0x388;
    constexpr uintptr_t GMATCH_TeamTacticsBase   = 0x1928; // (idx + 0xA1) * 0x28 (0x140A49020)
    constexpr uintptr_t GMATCH_TeamInfoBase      = 0x1978; // (idx + 0xA3) * 0x28 (0x140A48FC0)
    constexpr uintptr_t GMATCH_PlayerActionBase  = 0x19C8; // (idx + 0xA5) * 0x28 (0x140A48F00)
    constexpr uintptr_t GMATCH_PlayerSpatialBase = 0x1DD8; // (idx + 0xBF) * 0x28 (0x140A49000)
    constexpr uintptr_t GMATCH_PlayerEntityBase  = 0x25F8; // (idx + 0xF3) * 0x28 (0x140A48FE0)
    constexpr uintptr_t GMATCH_PitchDims         = 0x2AA8;
    constexpr uintptr_t GMATCH_EntryStride       = 0x28;

    // Ball (0x140A62E10, 0x140A57ED0)
    constexpr uintptr_t Ball_StatePtr            = 0x18;    // void*
    constexpr uintptr_t BallState_OwnerSlot      = 0x2601C; // int32_t (0..21 or 0xFF)
    constexpr uintptr_t Ball_PosVec3             = 0x88;

    // TeamTactics
    // int8 side sign (+1 / -1), read with movsbl by the engine (0x140438475, 0x140439F82, 0x14043AB5D). It flips at
    // half time. NOT a float: reading 4 bytes as a float takes the sign from +0x257 and never flips.
    constexpr uintptr_t TeamTactics_SideSign     = 0x254;

    // TeamInfo (0x140A3B500, 0x140A3B550)
    constexpr uintptr_t TeamInfo_ActiveRosterIdx    = 0x04;  // int32_t[11] (0..39, live substitutions)
    constexpr uintptr_t TeamInfo_PlayerRecordsBase  = 0xA8;
    constexpr uintptr_t TeamInfo_PlayerRecordStride = 0x128;
    // Read cache (A + 8, what GMATCH + 0x1978 points at) and write buffer (A + 0x2F08) are 0x2F00 bytes each;
    // 0x14041BE40 copies write -> read on every MatchControl frame (0x1404225E3)
    constexpr uintptr_t TeamInfo_BufferSize         = 0x2F00;

    // PlayerAction (0x140A6EAE0, 0x140A6EBB0, 0x1405B9400, 0x140A1B120)
    constexpr uintptr_t PlayerAction_ActionId    = 0x01; // uint8_t
    constexpr uintptr_t PlayerAction_ActionFlags = 0x08; // uint32_t (bit 13 fall/stagger, bit 29 simulation)
    constexpr uintptr_t PlayerAction_Causer      = 0x3D; // int8 slot of the player who caused the contact (0x140A1AF50)

    // PlayerSpatial
    constexpr uintptr_t PlayerSpatial_PosVec3    = 0x55C;

    // PlayerStatus array (24 entries)
    constexpr uintptr_t PlayerStatus_Stride      = 0x74;
    constexpr uintptr_t PlayerStatus_OffPitch    = 0x0D;
    constexpr uintptr_t PlayerStatus_Slot        = 0x20;

    // PlayerRecord (0x141032A40, 0x1404231EF, 0x140A3B923)
    constexpr uintptr_t PlayerRecord_Yellows     = 0x00; // uint8_t
    constexpr uintptr_t PlayerRecord_Reds        = 0x01; // uint8_t
    constexpr uintptr_t PlayerRecord_PlayerKey   = 0x30; // uint64_t (high 32 = playerId)

    // MatchInfo
    constexpr uintptr_t MatchInfo_MatchPhase      = 0x1168; // int32_t
    constexpr uintptr_t MatchInfo_MatchStatus     = 0x116C; // int32_t (1=InPlay, 2=OutOfPlay, 3=SetPlayReady)
    constexpr uintptr_t MatchInfo_PhaseElapsed    = 0x1174; // float
    constexpr uintptr_t MatchInfo_MatchTick       = 0x1198; // uint32_t
    constexpr uintptr_t MatchInfo_AdvantageInfo   = 0x1544; // 16 bytes
    constexpr uintptr_t MatchInfo_SetPlayInfo     = 0x1554; // RawSetPlayInfo (0x30)
    constexpr uintptr_t MatchInfo_FoulInfo        = 0x1584; // RawFoulInfo (0x3C)
    constexpr uintptr_t MatchInfo_KickerSlots     = 0x173C; // int32_t[2]
    constexpr uintptr_t MatchInfo_PlayerOnPitch   = 0x1748; // uint8_t[22]
    constexpr uintptr_t MatchInfo_SpecialModeKind = 0x1775; // uint8_t (0=normal match)
    // Foul position snapshot pool (verified via 0x1404B4B40 / 0x140A82120)
    constexpr uintptr_t MatchInfo_FoulSnapshotPool        = 0x17AC;
    constexpr size_t    MatchInfo_FoulSnapshotStride      = 0x3E4;
    constexpr size_t    MatchInfo_FoulSnapshotEntryStride = 0x2C;
    constexpr uintptr_t MatchInfo_FoulSnapshotIds         = 0x2358; // 0x17AC + 3 * 0x3E4
    // Pool-relative (0x140A81E70 ctor, 0x140A82070 AddSnapshot): ids uint32[3] +0xBAC, next id +0xBB8, ring index +0xBBC
    constexpr uintptr_t SnapshotPool_Ids       = 0xBAC;
    constexpr uintptr_t SnapshotPool_NextId    = 0xBB8;
    constexpr uintptr_t SnapshotPool_RingIndex = 0xBBC;
    constexpr size_t    SnapshotPool_Span      = 0xBC0;
    // MatchInfo RCU double-buffer stride (verified via 0x140412870, 0x1404237C0, 0x14041BEC0):
    // Read Buffer [0] is at matchInfo (+0x0000); Write Buffer [1] is adjacent at matchInfo + 0x2378.
    constexpr size_t    MatchInfo_StructSize              = 0x2378;

    // ReplayManager (verified in 0x1420C16A0..0x1420C1840)
    constexpr uintptr_t ReplayMgr_Mode             = 0x00; // uint8_t (0=off, 1..4=active)
    constexpr uintptr_t ReplayMgr_ManualScreenFlag = 0x07; // uint8_t (!=0 during Manual Replay UI)
    constexpr uintptr_t ReplayMgr_IsPlaying        = 0x0A; // uint8_t (1=playing, 0=paused)
    constexpr uintptr_t ReplayMgr_UiHidden         = 0x0B; // uint8_t (1=FreeCam / UI hidden)
    constexpr uintptr_t ReplayMgr_PlaybackSpeed    = 0x10; // float
    constexpr uintptr_t ReplayMgr_TimelinePos      = 0x14; // uint32_t (relative frame count)
    constexpr uintptr_t ReplayMgr_TimelineDur      = 0x18; // uint32_t (total window frames)
    constexpr uintptr_t ReplayMgr_CameraFocusSlot  = 0x1C; // int32_t (0=ball, 1..22=players)
    constexpr uintptr_t ReplayMgr_FocusPlayerKey   = 0x20; // uint64_t
    constexpr uintptr_t ReplayMgr_WindowStartTime  = 0x48; // float (60Hz ticks)
    constexpr uintptr_t ReplayMgr_WindowStartPhase = 0x4C; // int32_t
    constexpr uintptr_t ReplayMgr_WindowEndTime    = 0x50; // float (60Hz ticks)
    constexpr uintptr_t ReplayMgr_WindowEndPhase   = 0x54; // int32_t
    constexpr uintptr_t ReplayMgr_CursorTime       = 0x58; // float (60Hz ticks)
    constexpr uintptr_t ReplayMgr_CursorPhase      = 0x5C; // int32_t

    // ManualReplayController (RCX at 0x1420C16A0 / 0x1420BFCE0)
    constexpr uintptr_t ReplayCtrl_Mode            = 0x0A;  // uint8_t (0x0A = Manual Replay interactive)
    constexpr uintptr_t ReplayCtrl_CameraPtr       = 0x20;  // ReplayCamera*
    constexpr uintptr_t ReplayCtrl_PlayerOffset    = 0x50;  // Embedded ReplayPlayer*
    constexpr uintptr_t ReplayCtrl_IsPlaying       = 0x5C;  // uint8_t (player + 0x0C: 1=play, 0=pause)
    constexpr uintptr_t ReplayCtrl_FloatFrame      = 0x64;  // float   (player + 0x14)
    constexpr uintptr_t ReplayCtrl_FocusSlotDword  = 0x80;  // int32_t (0=ball, 1..22=player)
    constexpr uintptr_t ReplayCtrl_CurFrame        = 0x288; // uint16_t (player + 0x238)
    constexpr uintptr_t ReplayCtrl_RingCap         = 0x28A; // uint16_t (player + 0x23A)
    constexpr uintptr_t ReplayCtrl_WinStartFrame   = 0x28C; // uint16_t (player + 0x23C)
    constexpr uintptr_t ReplayCtrl_WinEndFrame     = 0x290; // uint16_t (player + 0x240)
    constexpr uintptr_t ReplayCtrl_RingCap2        = 0x292; // uint16_t (player + 0x242)
    constexpr uintptr_t ReplayCtrl_PlaybackSpeed   = 0x310; // float
    constexpr uintptr_t ReplayCtrl_ReplayMgrPtr    = 0x430; // ReplayManager*
    constexpr uintptr_t ReplayCtrl_CameraMode      = 0x64C; // uint8_t (0..2=Follow, 3=FreeCam)
    constexpr uintptr_t ReplayCtrl_FocusSlotByte   = 0x64D; // uint8_t (0=ball, 1..22=player)

    // ReplayCamera (0x141EB8FF0 / 0x141EB9390)
    constexpr uintptr_t ReplayCam_StatePtr         = 0x18;
    constexpr uintptr_t ReplayCamState_LookAtPos   = 0x00; // Vec3 (12 bytes)
    constexpr uintptr_t ReplayCamState_FocusTarget = 0x44; // int32_t (0=ball, 1..22=player)

    // Replay ring buffer singleton (*(*(0x143703558) + 0x6A8) + 0x40, stride 0x1FB0)
    constexpr uintptr_t ReplaySingleton_RingHolder = 0x6A8;
    constexpr uintptr_t ReplayRingHolder_BufferPtr = 0x40;
    constexpr size_t    ReplayRing_FrameStride     = 0x1FB0;
    constexpr uintptr_t ReplayRing_FrameTimeOff    = 0x00; // float (60Hz ticks)
    constexpr uintptr_t ReplayRing_FramePhaseOff   = 0x07; // uint8_t (low nibble = phase)

    // MatchFSM
    constexpr uintptr_t MatchFSM_SequenceCategory  = 0x874;  // int32_t
    constexpr uintptr_t MatchFSM_StateQueue        = 0x878;  // int32_t[30]
    constexpr uintptr_t MatchFSM_QueueHead         = 0x8F0;  // uint8_t
    constexpr uintptr_t MatchFSM_QueueTail         = 0x8F1;  // uint8_t
    constexpr uintptr_t MatchFSM_SubState          = 0x8F2;  // uint8_t
    constexpr uintptr_t MatchFSM_EmbeddedJudge     = 0x908;  // Judge@ai@match
    constexpr uintptr_t MatchFSM_WorkingState      = 0x2790;
    constexpr uintptr_t MatchFSM_WorkingStatus     = 0x2794; // int32_t
    constexpr uintptr_t MatchFSM_QuickRestartFlag  = 0x27FC; // uint32_t, written as a dword by 0x1404541E9 (copied to MatchControl + 0x74C; 0 = formal set-play)
    constexpr uintptr_t MatchFSM_KickerCache       = 0x4954; // uint64_t (0x000000FF000000FF clears)
    constexpr uintptr_t MatchFSM_JudgeResult       = 0x49C0; // RawSetPlayInfo (+0x00) + RawFoulInfo (+0x30)
    constexpr uintptr_t MatchFSM_JudgeFoulInfo     = 0x49F0; // RawFoulInfo
    // Reason of the last queue reset (0x1404529F0 writes its edx here): 1 = EnterOutOfPlay / RebuildSetPlay,
    // 0xE / 0xF = the two SetPlayReady handler exits that skip EnterOutOfPlay (0x140453F10, 0x140453370)
    constexpr uintptr_t MatchFSM_QueueResetReason  = 0x874;
    // At the end of MainTick (0x140446D37) the engine copies the FSM working block (fsm + 0x2790) to the pointer
    // stored at fsm + 0x17858 (MatchControl + 0x6E0, set by 0x140444A30). CommitToRegistry then reads that copy.
    constexpr uintptr_t MatchFSM_WorkingBlock      = 0x2790;
    constexpr uintptr_t MatchFSM_WorkingCopyPtr    = 0x17858;
    // Judge snapshot pool = JudgeResult + 0x148 (AddSnapshot 0x140A82070 at 0x14050E887, ctor 0x1404437BE).
    // FSM working state (+0x2790) is copied to MatchControl + 0x6E0 (0x14044402D) and CommitToRegistry copies
    // MatchControl + 0x2A58 over MatchInfo + 0x17AC (0x140423196 -> 0x140A3D3B0). So a foul snapshot id is only
    // durable if it lives in THIS pool.
    constexpr uintptr_t MatchFSM_JudgeSnapshotPool = 0x4B08;
    // VanishingSprayInfoRef owned by the FSM: the ctor stores vtable 0x1425A1570 at +0x17CA8 (0x14044201E).
    // 0x1425A1550 is only the ReferenceCopy base the dtor restores, never seen on a live object.
    // VanishingSprayInfo (0x70 bytes, write buffer = holder + 0x78 in 0x140423830):
    //   +0x00 Vec3 circle centre (current ball position), +0x0C angle, +0x10 circle alpha,
    //   +0x14..+0x24 last published circle (pos, angle, alpha), +0x28 dirty flag,
    //   +0x2C / +0x38 Vec3 wall line ends, +0x44 line alpha, +0x48.. last published line, +0x60 line alpha published.
    // 0x140A5CB10 / 0x140A5CBC0 publish a change only when current != last published. Moving an active spray
    // without a reset is drawn as a stroke from the old spot to the new one, so the engine always resets it
    // (0x140A5CCE0: alphas = 0) before a new set piece.
    constexpr uintptr_t MatchFSM_VanishingSprayRef   = 0x17CA8;
    constexpr uintptr_t VanishingSprayRefVtableRva   = 0x25A1570;
    constexpr uintptr_t MatchFSM_DelayedCardFlag   = 0x57F8; // uint8_t

    // Judge
    constexpr uintptr_t Judge_ResultPtr            = 0x3C0;  // points to MatchFSM + 0x49C0 while active

    // MatchControl
    constexpr uintptr_t MatchControl_TeamInfo0     = 0x1D0;
    constexpr uintptr_t MatchControl_TeamInfo1     = 0x1F8;
    constexpr uintptr_t MatchControl_WorkingState  = 0x6E0;
    // Relative to WorkingState (MatchControl + 0x6E0):
    constexpr uintptr_t Working_SetPlayInfo        = 0x2230; // MatchControl + 0x2910
    constexpr uintptr_t Working_FoulInfo           = 0x2260; // MatchControl + 0x2940

    // MatchRoster (global returned by 0x1414BC550 -> *(uint64_t*)0x143714240)
    // Verified via 0x1414C6A30, 0x1414C6AA0, 0x1414C5D70, 0x1414C6C70, 0x1414C9580
    constexpr uintptr_t MatchRoster_DataPtr         = 0x50;
    constexpr uintptr_t MatchRoster_EditedKitPtr    = 0x58;
    constexpr uintptr_t MatchRoster_BalNameString   = 0x98; // MSVC std::string (SSO <= 15)
    constexpr uintptr_t MatchRoster_TableOffset     = 0x35960;
    constexpr uintptr_t MatchRoster_SelectedKitId   = 0x120; // uint32_t[2]
    constexpr uintptr_t MatchRoster_TeamEntryBase   = 0x138;
    constexpr uintptr_t MatchRoster_TeamEntryStride = 0x690;
    constexpr uintptr_t TeamEntry_TeamId            = 0x00;  // uint32_t
    constexpr uintptr_t TeamEntry_FullName          = 0x04;  // char[70] UTF-8
    constexpr uintptr_t TeamEntry_ShortName         = 0x4A;  // char[16] 3-letter scoreboard abbrev
    constexpr uintptr_t TeamEntry_ShirtNumbers      = 0x29A; // uint16_t[40] shirt numbers, roster order (verified live)
    constexpr uintptr_t TeamEntry_KitTable          = 0x5D8; // 10 entries * 8 bytes (+2..4 pri, +5..7 sec)
    constexpr size_t    TeamEntry_KitTableCount     = 10;
    constexpr size_t    TeamEntry_KitEntryStride    = 8;

    // Edited / BMPES team kits (*(matchData + 0x58) + (teamIdx * 2) * 0x84)
    constexpr uintptr_t EditedKit_TeamStride        = 0x108; // 2 * 0x84 (home outfield at 0, away outfield at +0x108)
    constexpr uintptr_t EditedKit_PrimaryRGB        = 0x1991;
    constexpr uintptr_t EditedKit_SecondaryRGB      = 0x1994;

    // Player entries inside rosterTable (0x1414C6A30)
    constexpr uintptr_t MatchRoster_PlayerBase      = 0x1308;
    constexpr uintptr_t MatchRoster_TeamStride      = 0x3D40; // 40 * 0x188
    constexpr uintptr_t MatchRoster_EntryStride     = 0x188;
    constexpr size_t    MatchRoster_MaxPlayers      = 40;
    constexpr uintptr_t PlayerEntry_PlayerKey       = 0x2C;  // uint64_t (>> 32 = playerId)
    constexpr uintptr_t PlayerEntry_DisplayName     = 0x38;  // char[] UTF-8
    constexpr uintptr_t PlayerEntry_ShirtName       = 0x75;  // char[]
    constexpr uintptr_t PlayerEntry_BalAvatarFlag   = 0x144; // int32_t (< 0 => BAL avatar)
    // 0x178 = age, 0x001 = overall rating (NOT the shirt number: see TeamEntry_ShirtNumbers)
} // namespace Offsets

// Safe user-mode canonical pointer range check on Windows x64
inline bool IsValidPtr(const void* p) {
    const uintptr_t addr = reinterpret_cast<uintptr_t>(p);
    return addr >= 0x10000ULL && addr <= 0x00007FFFFFFFFFFFULL;
}

inline bool IsValidSlot(int32_t slot) {
    return slot >= 0 && slot < 22;
}

inline int32_t SlotToTeam(int32_t slot) {
    if (slot >= 0 && slot < 11) return 0;
    if (slot >= 11 && slot < 22) return 1;
    return -1;
}

// ============================================================================
// Pure uniform color contrast & radar readability helpers (mirrors 0x141519B80)
// ============================================================================
namespace ColorHelpers {

inline float RelativeLuma(RgbColor c) {
    return 0.299f * static_cast<float>(c.r) +
           0.587f * static_cast<float>(c.g) +
           0.114f * static_cast<float>(c.b);
}

inline float ColorDistance(RgbColor a, RgbColor b) {
    const float dr = static_cast<float>(a.r) - static_cast<float>(b.r);
    const float dg = static_cast<float>(a.g) - static_cast<float>(b.g);
    const float db = static_cast<float>(a.b) - static_cast<float>(b.b);
    return std::sqrt(dr * dr + dg * dg + db * db);
}

// Mirrors Fox Engine Radar brightness lift (0x141519B80) so black/navy kits remain crisp on dark pitch
inline RgbColor LiftForDarkRadar(RgbColor c) {
    if (c.r < 0x48 && c.g < 0x48 && c.b < 0x48) {
        return RgbColor{
            static_cast<uint8_t>(std::min(255, static_cast<int>(c.r) + 0x38)),
            static_cast<uint8_t>(std::min(255, static_cast<int>(c.g) + 0x38)),
            static_cast<uint8_t>(std::min(255, static_cast<int>(c.b) + 0x38))
        };
    }
    return c;
}

inline void EnsureDistinctTeamColors(RgbColor& homePri, RgbColor& homeSec,
                                     RgbColor& awayPri, RgbColor& awaySec) {
    homePri = LiftForDarkRadar(homePri);
    awayPri = LiftForDarkRadar(awayPri);

    // Ensure secondary border contrasts against its own primary fill
    if (ColorDistance(homePri, homeSec) < 42.0f) {
        homeSec = (RelativeLuma(homePri) > 140.0f) ? RgbColor{ 24, 30, 42 } : RgbColor{ 240, 245, 252 };
    }
    if (ColorDistance(awayPri, awaySec) < 42.0f) {
        awaySec = (RelativeLuma(awayPri) > 140.0f) ? RgbColor{ 24, 30, 42 } : RgbColor{ 240, 245, 252 };
    }

    // If Home and Away primary kits are visually too close on the 2D radar, promote Away secondary or clear contrast
    if (ColorDistance(homePri, awayPri) < 62.0f) {
        const RgbColor liftedAwaySec = LiftForDarkRadar(awaySec);
        if (ColorDistance(homePri, liftedAwaySec) >= 62.0f) {
            awaySec = awayPri;
            awayPri = liftedAwaySec;
        } else {
            awayPri = (RelativeLuma(homePri) > 130.0f) ? RgbColor{ 52, 168, 255 } : RgbColor{ 248, 206, 62 };
            awaySec = RgbColor{ 18, 24, 36 };
        }
    }
}

} // namespace ColorHelpers

// ============================================================================
// Pure Replay circular ring-buffer math (verified against 0x1420C1734..0x1420C17AA
// and ReplayPlayer::SetFrame 0x14217E800)
// ============================================================================
namespace ReplayTimelineMath {

inline float ComputeTimelineNorm(uint32_t posFrames, uint32_t durFrames) {
    if (durFrames == 0) return 0.0f;
    return std::clamp(static_cast<float>(posFrames) / static_cast<float>(durFrames), 0.0f, 1.0f);
}

inline uint16_t ComputeWindowDuration(uint16_t winStart, uint16_t winEnd, uint16_t ringCap) {
    if (ringCap == 0) return 0;
    const int32_t diff = (static_cast<int32_t>(winEnd) - static_cast<int32_t>(winStart) + static_cast<int32_t>(ringCap)) %
                         static_cast<int32_t>(ringCap);
    return static_cast<uint16_t>(diff);
}

inline uint16_t ComputeFrameFromNorm(uint16_t winStart, uint16_t winEnd, uint16_t ringCap, float norm) {
    if (ringCap == 0) return winStart;
    const uint16_t dur = ComputeWindowDuration(winStart, winEnd, ringCap);
    const float clamped = std::clamp(norm, 0.0f, 1.0f);
    const uint32_t rel = static_cast<uint32_t>(std::lround(clamped * static_cast<float>(dur)));
    return static_cast<uint16_t>((static_cast<uint32_t>(winStart) + rel) % static_cast<uint32_t>(ringCap));
}

// Scans circular window [winStart..winEnd] for the frame whose time60Hz is closest to targetTime
// within a compatible match half (phase / 2). Immune to halftime resets, wrap-around, and sentinel frames.
template <typename ReadTimeFn, typename ReadPhaseFn>
inline uint16_t FindClosestFrameByTime(uint16_t winStart, uint16_t winEnd, uint16_t ringCap,
                                       int32_t targetPhase, float targetTime60Hz,
                                       ReadTimeFn&& readTimeAtFrame, ReadPhaseFn&& readPhaseAtFrame) {
    if (ringCap == 0) return winStart;
    const uint16_t dur = ComputeWindowDuration(winStart, winEnd, ringCap);
    if (dur == 0) return winStart;

    auto frameAt = [winStart, ringCap](uint32_t rel) -> uint16_t {
        return static_cast<uint16_t>((static_cast<uint32_t>(winStart) + rel) % static_cast<uint32_t>(ringCap));
    };

    const int32_t targetHalf = targetPhase / 2;
    uint16_t bestSameHalfFrame = frameAt(0);
    float    bestSameHalfDt    = 1e30f;
    bool     foundSameHalf     = false;

    uint16_t bestAnyFrame = frameAt(0);
    float    bestAnyDt    = 1e30f;

    for (uint32_t rel = 0; rel <= dur; ++rel) {
        const uint16_t f = frameAt(rel);
        const float t = readTimeAtFrame(f);
        if (!std::isfinite(t) || t < 0.0f) continue;
        const float dt = std::fabs(t - targetTime60Hz);

        if (dt < bestAnyDt) {
            bestAnyDt = dt;
            bestAnyFrame = f;
        }
        const int32_t ph = readPhaseAtFrame(f);
        if ((ph / 2) == targetHalf && dt < bestSameHalfDt) {
            bestSameHalfDt = dt;
            bestSameHalfFrame = f;
            foundSameHalf = true;
        }
    }

    return foundSameHalf ? bestSameHalfFrame : bestAnyFrame;
}

} // namespace ReplayTimelineMath

// ============================================================================
// Pure pitch geometry helpers (verified against Pitch_ClassifyZone 0x140A73470
// and Judge_CalcPenaltySpot 0x14050DC00)
// ============================================================================
struct PitchDims {
    float halfLength = 52.5f;  // dims[0]
    float halfWidth  = 34.0f;  // dims[1]
    float box6X      = 47.0f;  // dims[4]
    float box6Z      = 9.16f;  // dims[5]
    float box18X     = 36.0f;  // dims[6]
    float box18Z     = 20.16f; // dims[7]
    float penSpotX   = 41.5f;  // dims[8]

    static PitchDims FromRaw(const float* raw) {
        PitchDims d{};
        if (IsValidPtr(raw) && raw[0] > 20.0f && raw[0] < 120.0f && raw[1] > 15.0f && raw[1] < 90.0f) {
            d.halfLength = raw[0];
            d.halfWidth  = raw[1];
            d.box6X      = raw[4];
            d.box6Z      = raw[5];
            d.box18X     = raw[6];
            d.box18Z     = raw[7];
            d.penSpotX   = raw[8];
        }
        return d;
    }
};

namespace PitchGeometry {

// The "attackDir" arguments below are the side of the goal the team DEFENDS (own goal side): +1 when the team
// defends the goal at +x. The team's own penalty box is (x * side > box18X), its goal kicks and the corners it
// concedes are at that end. MatchHooks::ReadTeamAttackDir returns exactly this value (calibrated at each
// kick-off from where the players stand, see match_hooks.cpp).
inline bool IsInsideDefendingBox(const PitchDims& d, const Vec3& pos, float offenderAttackDir) {
    const float dirSign = (offenderAttackDir >= 0.0f) ? 1.0f : -1.0f;
    return (pos.x * dirSign > d.box18X) && (std::fabs(pos.z) < d.box18Z);
}

inline Vec3 CalcPenaltySpot(const PitchDims& d, float offenderAttackDir) {
    const float dirSign = (offenderAttackDir >= 0.0f) ? 1.0f : -1.0f;
    return Vec3{ dirSign * d.penSpotX, 0.0f, 0.0f };
}

inline Vec3 ClampToPitch(const PitchDims& d, const Vec3& pos, float margin = 0.5f) {
    Vec3 out = pos;
    const float maxX = (d.halfLength > margin) ? (d.halfLength - margin) : d.halfLength;
    const float maxZ = (d.halfWidth  > margin) ? (d.halfWidth  - margin) : d.halfWidth;
    if (out.x >  maxX) out.x =  maxX;
    if (out.x < -maxX) out.x = -maxX;
    if (out.z >  maxZ) out.z =  maxZ;
    if (out.z < -maxZ) out.z = -maxZ;
    out.y = 0.0f;
    return out;
}

// Projects a point outside the defending penalty box by `offsetMeters` along the closest edge
// (front edge of 18-yard box vs side edges), then clamps within playable pitch bounds.
inline Vec3 ProjectOutsideDefendingBox(const PitchDims& d, const Vec3& pos, float offenderAttackDir, float offsetMeters = 0.6f) {
    Vec3 clamped = ClampToPitch(d, pos, 0.75f);
    if (!IsInsideDefendingBox(d, clamped, offenderAttackDir)) {
        return clamped;
    }

    const float dirSign = (offenderAttackDir >= 0.0f) ? 1.0f : -1.0f;
    const float distToFront = std::fabs(clamped.x * dirSign - d.box18X);
    const float distToSide  = std::fabs(d.box18Z - std::fabs(clamped.z));

    if (distToFront <= distToSide) {
        clamped.x = dirSign * (d.box18X - offsetMeters);
    } else {
        const float zSign = (clamped.z >= 0.0f) ? 1.0f : -1.0f;
        clamped.z = zSign * (d.box18Z + offsetMeters);
    }
    return ClampToPitch(d, clamped, 0.75f);
}

// Signed distance (meters) from `pos` to the defending penalty box edge:
//   < 0.0f : INSIDE the defending penalty box (magnitude = distance to nearest line)
//   > 0.0f : OUTSIDE the defending penalty box (magnitude = Euclidean distance to box edge)
inline float SignedDistanceToDefendingBox(const PitchDims& d, const Vec3& pos, float offenderAttackDir) {
    const float dirSign = (offenderAttackDir >= 0.0f) ? 1.0f : -1.0f;
    const float xAlong = pos.x * dirSign;
    const float az = std::fabs(pos.z);

    if (xAlong > d.box18X && xAlong <= d.halfLength && az < d.box18Z) {
        const float dFront = xAlong - d.box18X;
        const float dSide  = d.box18Z - az;
        return -std::fmin(dFront, dSide);
    }

    float dx = 0.0f;
    if (xAlong < d.box18X) dx = d.box18X - xAlong;
    else if (xAlong > d.halfLength) dx = xAlong - d.halfLength;
    const float dz = (az > d.box18Z) ? (az - d.box18Z) : 0.0f;
    return std::sqrt(dx * dx + dz * dz);
}

inline Vec3 CalcCornerSpot(const PitchDims& d, float defenderAttackDir, float refZ) {
    const float dirSign = (defenderAttackDir >= 0.0f) ? 1.0f : -1.0f;
    const float zSign = (refZ >= 0.0f) ? 1.0f : -1.0f;
    return Vec3{ dirSign * (d.halfLength - 0.5f), 0.0f, zSign * (d.halfWidth - 0.5f) };
}

inline Vec3 CalcGoalKickSpot(const PitchDims& d, float defenderAttackDir, float refZ) {
    const float dirSign = (defenderAttackDir >= 0.0f) ? 1.0f : -1.0f;
    const float zSign = (refZ >= 0.0f) ? 1.0f : -1.0f;
    return Vec3{ dirSign * d.box6X, 0.0f, zSign * (d.box6Z * 0.5f) };
}

inline Vec3 CalcThrowInSpot(const PitchDims& d, const Vec3& refPos) {
    const float maxX = (d.halfLength > 1.5f) ? (d.halfLength - 1.5f) : d.halfLength;
    float x = refPos.x;
    if (x >  maxX) x =  maxX;
    if (x < -maxX) x = -maxX;
    const float zSign = (refPos.z >= 0.0f) ? 1.0f : -1.0f;
    const float zEdge = (d.halfWidth > 0.1f) ? (d.halfWidth - 0.1f) : d.halfWidth;
    return Vec3{ x, 0.0f, zSign * zEdge };
}

// Formats match phase + 60Hz half-time ticks into mm:ss (verified against 0x1420C6760 / 0x140A6C3B0).
inline void FormatMatchClock(int32_t matchPhase, float time60Hz, int& outMinutes, int& outSeconds) {
    int totalSec = static_cast<int>(time60Hz / 60.0f);
    if (totalSec < 0) totalSec = 0;
    int baseMin = 0;
    if (matchPhase == 2 || matchPhase == 3) {
        baseMin = 45;
    } else if (matchPhase == 4 || matchPhase == 5) {
        baseMin = 90;
    } else if (matchPhase == 6 || matchPhase == 7) {
        baseMin = 105;
    }
    outMinutes = baseMin + (totalSec / 60);
    outSeconds = totalSec % 60;
}

} // namespace PitchGeometry

} // namespace var_mod
