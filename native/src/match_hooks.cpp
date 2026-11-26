#include "match_hooks.h"

#include "aob_scanner.h"
#include "game_structs.h"
#include "inline_hook.h"
#include "logger.h"
#include "var_hud.h"
#include "var_state.h"

#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace var_mod {

namespace {

using FnMainTick            = uint32_t(__fastcall*)(void* matchFsm, void* runCtx, void* arg3);
using FnJudgeFinalizeFrame  = void(__fastcall*)(void* judge);
using FnUpdateFoulCounters  = void(__fastcall*)(void* matchControl, void* matchInfo, void* workingState);
using FnReplayCtrlSync      = void(__fastcall*)(void* replayCtrl);
using FnEnterOutOfPlay      = void(__fastcall*)(void* matchFsm);
using FnGetTimeFloat        = float(__fastcall*)(void* matchInfoTimeBase);
using FnSetPlayerOnPitch    = void(__fastcall*)(void* matchInfo, uint32_t slot, uint8_t onPitch);
using FnGetPlayerBySlot     = uint8_t*(__fastcall*)(void* teamInfo, int32_t slot);
using FnReplaySetFrame      = uint64_t(__fastcall*)(void* replayPlayer, uint16_t targetFrame);
using FnReplayGetFocusPos   = const float*(__fastcall*)(void* replayPlayer, int32_t focusTarget);

InlineHook g_hookMainTick;
InlineHook g_hookJudgeFinalize;
InlineHook g_hookUpdateFoulCounters;
InlineHook g_hookReplayCtrlSync;
InlineHook g_hookFoulSnapshotFind;

// Native vanishing spray reset (see Aobs::VanishingSpray_NativeReset)
using FnSprayRefGetWrite = uint8_t*(__fastcall*)(void* ref);
using FnSprayReset       = void(__fastcall*)(void* spray);
using FnSprayRefCommit   = void(__fastcall*)(void* ref);
FnSprayRefGetWrite g_fnSprayRefGetWrite = nullptr;
FnSprayReset       g_fnSprayReset       = nullptr;
FnSprayRefCommit   g_fnSprayRefCommit   = nullptr;
using FnRebuildSetPlay = void(__fastcall*)(void* matchFsm);
FnRebuildSetPlay   g_fnRebuildSetPlay   = nullptr; // MatchFSM_RebuildSetPlay 0x140454280
uintptr_t          g_sprayRefVtable     = 0;

// Foul snapshot guard (see Detour_FoulSnapshotFind)
using FnFoulSnapshotFind = uint8_t*(__fastcall*)(uint8_t* pool, uint32_t id);
constexpr size_t kUncheckedFindSites = sizeof(FoulSnapshotUncheckedCallSites::Rvas) / sizeof(uintptr_t);
uintptr_t g_uncheckedFindReturnAddrs[kUncheckedFindSites] = {};
std::atomic<uint32_t> g_foulSnapshotGuardHits{ 0 };
// Zeroed stand-in snapshot: 0x3E4 bytes per snapshot, but the consumers index entries with
// (playerIdx * 0x2C) where playerIdx can be 0xFF (unresolved), so keep 0x4000 bytes of zeros.
alignas(16) uint8_t g_emptyFoulSnapshot[0x4000] = {};

FnEnterOutOfPlay    g_fnEnterOutOfPlay    = nullptr;
FnGetTimeFloat      g_fnGetTimeFloat      = nullptr;
FnGetPlayerBySlot   g_fnGetPlayerBySlot   = nullptr;
FnReplaySetFrame    g_fnReplaySetFrame    = nullptr;
FnReplayGetFocusPos g_fnReplayGetFocusPos = nullptr;

uintptr_t g_gmatchPtrAddr          = 0; // Address of global GMATCH* (0x143702208)
uintptr_t g_matchRosterPtrAddr     = 0; // Address of global MatchRoster* (0x143714240)
uintptr_t g_replaySingletonPtrAddr = 0; // Address of global ReplaySingleton* (0x143703558)

std::mutex g_hooksLifecycleMutex;
std::atomic<bool>  g_installed{ false };
std::atomic<void*> g_lastMatchFsm{ nullptr };
std::atomic<void*> g_lastMatchControl{ nullptr };

// Simulation-thread-only counters/state (accessed solely inside MatchFSM/MatchControl thread)
uint64_t g_mainTickCount = 0;
uint64_t g_finalizeFrameCount = 0;
uint64_t g_foulCommitCount = 0;
uint32_t g_lastSnapshotTick = 0;
uint32_t g_lastSeenMatchTick = 0;
bool     g_lastAdvActive = false;
bool     g_lastFoulValidEdge = false;
int32_t  g_lastFoulOffenderEdge = -1;
int32_t  g_lastFoulCardEdge = -1;
int32_t  g_lastFoulPhaseEdge = -1;
float    g_lastFoulTimeEdge = -1000.0f;
bool     g_forceRosterSyncNextTick = false;
int32_t  g_recentBallOwnerSlot = -1;
uint32_t g_recentBallOwnerTick = 0;
std::atomic<bool> g_simResetPending{ false };
uint32_t g_stoppageCount = 0;     // ball in play <-> out of play switches of this match (sim thread)
int32_t  g_prevStatusForStoppage = -1;
uint32_t g_requeueSeq = 0;        // pool sequence requeued last and how many times in a row
uint32_t g_requeueCount = 0;
bool     g_teamWbBlockedLogged = false;
// Set at match start/end (Lua), cleared by the first MainTick: the render thread stops reading match objects
// that the post-match scene change may release.
std::atomic<bool> g_pollSuspended{ false };
uint32_t g_syncedStateGeneration = 0xFFFFFFFFU;
int      g_rosterDiagCount = 0;
uint64_t g_rosterDiagTick = 0;

// Roster caches (refreshed automatically on substitutions or match reset)
uint32_t g_cachedPlayerIds[22] = {};
int32_t  g_cachedRosterIdx[22] = {};
uint8_t  g_cachedShirtNums[22] = {};
uint8_t  g_cachedYellows[22]   = {};
uint8_t  g_cachedReds[22]      = {};
uint8_t  g_cachedOnPitch[22]   = {};
bool     g_hasPushedPlayersOnce = false;
char     g_cachedPlayerNames[22][64] = {};
char     g_cachedShirtNames[22][32] = {};
const uint8_t* g_verifiedRosterDataPtr = nullptr;
const uint8_t* g_verifiedEditedKitPtr = nullptr;
const uint8_t* g_verifiedBallStatePtr = nullptr;
uint8_t*       g_verifiedMatchInfoWriteBuf = nullptr;
uint8_t*       g_verifiedTeamInfoWriteBuf[2] = { nullptr, nullptr };
bool           g_warnedTeamInfoWb[2] = { false, false };
std::atomic<const uint8_t*> g_verifiedReplayRingBufPtr{ nullptr };

const uint8_t* g_syncedTeamRosterTable = nullptr;
uint32_t g_syncedTeamKitIds[2] = { 0xFFFFFFFFU, 0xFFFFFFFFU };
bool     g_loggedRosterSyncOnce = false;
bool     g_loggedTeamSyncOnce = false;

// VirtualQuery pointer cache for DX11 render-thread live polling
uint8_t* g_pollLastGmatch = nullptr;
uint8_t* g_pollLastMatchInfo = nullptr;
uint8_t* g_pollLastReplayMgr = nullptr;
uint8_t* g_pollLastFsm = nullptr;
uint8_t  g_pollLoggedReplayMode = 0;
uint8_t  g_pollLoggedManualFlag = 0;
bool     g_pollWasMatchActive = false;
bool     g_pollWasInReplay = false;

bool IsMemoryRangeReadable(const void* ptr, size_t len) {
    if (!IsValidPtr(ptr) || len == 0) return false;
    uintptr_t cur = reinterpret_cast<uintptr_t>(ptr);
    const uintptr_t want = cur + len;
    if (want < cur) return false;
    // A range may span several VirtualQuery regions (same allocation, different attributes):
    // walk them all instead of requiring a single region.
    for (int guard = 0; cur < want && guard < 64; ++guard) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<const void*>(cur), &mbi, sizeof(mbi)) == 0) return false;
        if (mbi.State != MEM_COMMIT) return false;
        const DWORD p = mbi.Protect & 0xFF;
        const bool readable = (p == PAGE_READONLY || p == PAGE_READWRITE ||
                               p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READ ||
                               p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY);
        if (!readable || (mbi.Protect & PAGE_GUARD)) return false;
        const uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (end <= cur) return false;
        cur = end;
    }
    return cur >= want;
}

uint8_t* GetGmatch() {
    if (!g_gmatchPtrAddr) return nullptr;
    uint8_t* gm = *reinterpret_cast<uint8_t**>(g_gmatchPtrAddr);
    return IsValidPtr(gm) ? gm : nullptr;
}

uint8_t* GetMatchInfo(uint8_t* gmatch) {
    if (!IsValidPtr(gmatch)) return nullptr;
    uint8_t* mi = *reinterpret_cast<uint8_t**>(gmatch + Offsets::GMATCH_MatchInfo);
    return IsValidPtr(mi) ? mi : nullptr;
}

// MatchInfo is an RCU double-buffered allocation (0x140412870: 0x46F8 bytes = 8B header + 2 * 0x2378B).
// Read Buffer [0] is at matchInfo; Write Buffer [1] is adjacent at matchInfo + 0x2378 (0x1404237C0 / 0x14041BEC0).
uint8_t* GetMatchInfoWriteBuffer(uint8_t* matchInfo) {
    if (!IsValidPtr(matchInfo)) return nullptr;
    uint8_t* wb = matchInfo + Offsets::MatchInfo_StructSize;
    if (wb != g_verifiedMatchInfoWriteBuf) {
        if (!IsMemoryRangeReadable(matchInfo - 8, 8) || !IsMemoryRangeReadable(wb, Offsets::MatchInfo_StructSize)) {
            return nullptr;
        }
        // 0x140412870 allocates 0x46F8 bytes: refcount dword (= 1) at A, buffers at A + 8 and A + 0x2380.
        // Only trust matchInfo + 0x2378 when matchInfo really is A + 8, otherwise a write would land past
        // the allocation and corrupt the game heap.
        const int32_t refCount = *reinterpret_cast<const int32_t*>(matchInfo - 8);
        const int32_t phaseR = *reinterpret_cast<const int32_t*>(matchInfo + Offsets::MatchInfo_MatchPhase);
        const int32_t phaseW = *reinterpret_cast<const int32_t*>(wb + Offsets::MatchInfo_MatchPhase);
        if (refCount < 1 || refCount > 64 || phaseR != phaseW) {
            static bool s_warned = false;
            if (!s_warned) {
                s_warned = true;
                VAR_LOG_WRN("MatchInfo write buffer rejected (ref=%d phase %d/%d): RCU mirror writes disabled",
                            refCount, phaseR, phaseW);
            }
            return nullptr;
        }
        g_verifiedMatchInfoWriteBuf = wb;
    }
    return wb;
}

// TeamInfo is double buffered like MatchInfo: GMATCH + 0x1978 points at the read cache A + 8, the engine
// writes A + 0x2F08 (0x14041E820) and the publish callback 0x14041BE40 copies those 0x2F00 bytes over A + 8 on
// every MatchControl frame. A card written only to the read cache is undone one frame later, so every
// PlayerRecord write goes to both. The write buffer is trusted only when it mirrors the read cache.
uint8_t* GetTeamInfoWriteBuffer(uint8_t* teamInfo, int32_t team) {
    if (!IsValidPtr(teamInfo) || (team != 0 && team != 1)) return nullptr;
    uint8_t* wb = teamInfo + Offsets::TeamInfo_BufferSize;
    if (wb == g_verifiedTeamInfoWriteBuf[team]) return wb;
    if (!IsMemoryRangeReadable(teamInfo - 8, 8) || !IsMemoryRangeReadable(wb, Offsets::TeamInfo_BufferSize)) {
        return nullptr;
    }
    const int32_t refCount = *reinterpret_cast<const int32_t*>(teamInfo - 8);
    // Active roster indices and the player keys of the 40 records must match between the two buffers
    bool same = (refCount >= 1 && refCount <= 64) &&
                std::memcmp(teamInfo + Offsets::TeamInfo_ActiveRosterIdx, wb + Offsets::TeamInfo_ActiveRosterIdx,
                            11 * sizeof(int32_t)) == 0;
    for (size_t i = 0; same && i < Offsets::MatchRoster_MaxPlayers; ++i) {
        const size_t off = Offsets::TeamInfo_PlayerRecordsBase + i * Offsets::TeamInfo_PlayerRecordStride +
                           Offsets::PlayerRecord_PlayerKey;
        same = std::memcmp(teamInfo + off, wb + off, sizeof(uint64_t)) == 0;
    }
    if (!same) {
        if (!g_warnedTeamInfoWb[team]) {
            g_warnedTeamInfoWb[team] = true;
            VAR_LOG_WRN("TeamInfo write buffer rejected (team %d, ref=%d): VAR decisions wait until it validates",
                        team, refCount);
        }
        return nullptr;
    }
    g_verifiedTeamInfoWriteBuf[team] = wb;
    return wb;
}

uint8_t* GetReplayMgr(uint8_t* gmatch) {
    if (!IsValidPtr(gmatch)) return nullptr;
    uint8_t* rm = *reinterpret_cast<uint8_t**>(gmatch + Offsets::GMATCH_ReplayManager);
    return IsValidPtr(rm) ? rm : nullptr;
}

int32_t GetFsmCurrentState(uint8_t* fsm) {
    if (!IsValidPtr(fsm)) return -1;
    const uint8_t head = *reinterpret_cast<const uint8_t*>(fsm + Offsets::MatchFSM_QueueHead);
    if (head >= 30) return -1;
    return *reinterpret_cast<const int32_t*>(fsm + Offsets::MatchFSM_StateQueue + static_cast<size_t>(head) * 4);
}

float GetMatchTime60Hz(uint8_t* matchInfo) {
    if (!IsValidPtr(matchInfo)) return 0.0f;
    if (g_fnGetTimeFloat) {
        return g_fnGetTimeFloat(matchInfo + Offsets::MatchInfo_MatchPhase);
    }
    return *reinterpret_cast<const float*>(matchInfo + Offsets::MatchInfo_PhaseElapsed);
}

PitchDims ReadPitchDims(uint8_t* gmatch) {
    if (!IsValidPtr(gmatch)) return PitchDims{};
    const float* raw = *reinterpret_cast<const float* const*>(gmatch + Offsets::GMATCH_PitchDims);
    return PitchDims::FromRaw(raw);
}

// ---------------------------------------------------------------------------------------------------------------
// Own-goal side of each team (+1: defends the goal at +x). Feeds every spot computation (penalty spot, goal kick,
// corner, inside / outside the box) and the 2D pitch.
//  - The engine keeps a signed side byte at TeamTactics + 0x254 that flips at half time.
//  - Its sign convention is calibrated at every kick-off, when each team stands entirely in its own half:
//    convention = sign(team centroid x) * engine byte. ownSide = convention * engine byte from then on.
//  - Before the first calibration (mod loaded mid-match) the goalkeeper (first slot of the team) decides, when he is
//    clearly in one half.
// ---------------------------------------------------------------------------------------------------------------
int8_t g_sideConvention = 0; // 0 = not calibrated yet, else +1 / -1

int8_t ReadEngineSideByte(uint8_t* gmatch, int32_t teamIdx) {
    if (!IsValidPtr(gmatch) || (teamIdx != 0 && teamIdx != 1)) return 0;
    const uint8_t* tactics = *reinterpret_cast<const uint8_t* const*>(
        gmatch + Offsets::GMATCH_TeamTacticsBase + static_cast<size_t>(teamIdx) * Offsets::GMATCH_EntryStride);
    if (!IsValidPtr(tactics) || !IsMemoryRangeReadable(tactics + Offsets::TeamTactics_SideSign, 1)) return 0;
    return *reinterpret_cast<const int8_t*>(tactics + Offsets::TeamTactics_SideSign);
}

bool ReadPlayerPos(uint8_t* gmatch, int32_t slot, Vec3& out) {
    if (!IsValidPtr(gmatch) || !IsValidSlot(slot)) return false;
    const uint8_t* spatial = *reinterpret_cast<const uint8_t* const*>(
        gmatch + Offsets::GMATCH_PlayerSpatialBase + static_cast<size_t>(slot) * Offsets::GMATCH_EntryStride);
    if (!IsValidPtr(spatial) || !IsMemoryRangeReadable(spatial + Offsets::PlayerSpatial_PosVec3, sizeof(Vec3))) return false;
    std::memcpy(&out, spatial + Offsets::PlayerSpatial_PosVec3, sizeof(Vec3));
    return true;
}

float ReadTeamAttackDir(uint8_t* gmatch, int32_t teamIdx) {
    const float fallback = (teamIdx == 0) ? -1.0f : 1.0f;
    if (teamIdx != 0 && teamIdx != 1) return fallback;
    const int8_t b = ReadEngineSideByte(gmatch, teamIdx);
    if (g_sideConvention != 0 && b != 0) {
        return (g_sideConvention * b > 0) ? 1.0f : -1.0f;
    }
    Vec3 gk{};
    if (ReadPlayerPos(gmatch, teamIdx * 11, gk) && std::fabs(gk.x) > 20.0f) {
        return (gk.x > 0.0f) ? 1.0f : -1.0f;
    }
    return fallback;
}

// Called every MainTick: calibrates the engine byte convention at kick-off (SetPlayReady with a kick-off set play)
void CalibrateSidesAtKickoff(uint8_t* gmatch, uint8_t* matchInfo, int32_t matchStatus) {
    if (matchStatus != static_cast<int32_t>(MatchStatus::SetPlayReady)) return;
    const int32_t kind = *reinterpret_cast<const int32_t*>(matchInfo + Offsets::MatchInfo_SetPlayInfo);
    if (kind != 1) return; // set play kind 1 = kick-off (category 3 builders)
    float sumX[2] = { 0.0f, 0.0f };
    int n[2] = { 0, 0 };
    for (int s = 0; s < 22; ++s) {
        if (*reinterpret_cast<const uint8_t*>(matchInfo + Offsets::MatchInfo_PlayerOnPitch + s) == 0) continue;
        Vec3 p{};
        if (!ReadPlayerPos(gmatch, s, p)) continue;
        sumX[SlotToTeam(s)] += p.x;
        ++n[SlotToTeam(s)];
    }
    if (n[0] < 7 || n[1] < 7) return;
    const float c0 = sumX[0] / n[0], c1 = sumX[1] / n[1];
    if (std::fabs(c0) < 3.0f || std::fabs(c1) < 3.0f || (c0 > 0.0f) == (c1 > 0.0f)) return; // not lined up yet
    const int8_t b0 = ReadEngineSideByte(gmatch, 0), b1 = ReadEngineSideByte(gmatch, 1);
    if (b0 == 0 || b1 == 0) return;
    const int8_t conv0 = static_cast<int8_t>(((c0 > 0.0f) ? 1 : -1) * (b0 > 0 ? 1 : -1));
    const int8_t conv1 = static_cast<int8_t>(((c1 > 0.0f) ? 1 : -1) * (b1 > 0 ? 1 : -1));
    if (conv0 != conv1) return;
    if (g_sideConvention != conv0) {
        g_sideConvention = conv0;
        VAR_LOG_INF("Team sides calibrated at kick-off: convention=%d home own goal x%s away own goal x%s",
                    conv0, c0 > 0.0f ? "+" : "-", c1 > 0.0f ? "+" : "-");
    }
}

uint8_t* GetTeamInfo(uint8_t* gmatch, int32_t teamIdx) {
    if (!IsValidPtr(gmatch) || (teamIdx != 0 && teamIdx != 1)) return nullptr;
    uint8_t* ti = *reinterpret_cast<uint8_t**>(
        gmatch + Offsets::GMATCH_TeamInfoBase + static_cast<size_t>(teamIdx) * Offsets::GMATCH_EntryStride);
    return IsValidPtr(ti) ? ti : nullptr;
}

int32_t ReadActiveRosterIndex(uint8_t* gmatch, int32_t slot) {
    if (!IsValidSlot(slot)) return -1;
    uint8_t* teamInfo = GetTeamInfo(gmatch, SlotToTeam(slot));
    if (!teamInfo) return -1;
    const int32_t idx = *reinterpret_cast<const int32_t*>(
        teamInfo + Offsets::TeamInfo_ActiveRosterIdx + static_cast<size_t>(slot % 11) * 4);
    if (idx < 0 || idx >= static_cast<int32_t>(Offsets::MatchRoster_MaxPlayers)) return -1;
    return idx;
}

uint8_t* GetPlayerRecord(uint8_t* gmatch, int32_t slot);

// Record of a specific player: `rosterIdx` (0..39) is who held `slot` when the incident happened. The record
// of a substituted player stays in TeamInfo, so his cards are still written to him and never to the
// substitute now holding the slot. -1 = whoever holds the slot now.
uint8_t* GetPlayerRecordFor(uint8_t* gmatch, int32_t slot, int32_t rosterIdx) {
    if (rosterIdx < 0 || rosterIdx >= static_cast<int32_t>(Offsets::MatchRoster_MaxPlayers)) {
        return GetPlayerRecord(gmatch, slot);
    }
    if (!IsValidPtr(gmatch) || !IsValidSlot(slot)) return nullptr;
    uint8_t* teamInfo = GetTeamInfo(gmatch, SlotToTeam(slot));
    if (!teamInfo) return nullptr;
    uint8_t* rec = teamInfo + Offsets::TeamInfo_PlayerRecordsBase +
                   static_cast<size_t>(rosterIdx) * Offsets::TeamInfo_PlayerRecordStride;
    return IsMemoryRangeReadable(rec, 2) ? rec : nullptr;
}

// True when `slot` is still held by the player `rosterIdx` (or the identity is unknown)
bool SlotHeldBy(uint8_t* gmatch, int32_t slot, int32_t rosterIdx) {
    if (rosterIdx < 0) return true;
    return ReadActiveRosterIndex(gmatch, slot) == rosterIdx;
}

uint8_t* GetPlayerRecord(uint8_t* gmatch, int32_t slot) {
    if (!IsValidPtr(gmatch) || !IsValidSlot(slot)) return nullptr;
    uint8_t* teamInfo = GetTeamInfo(gmatch, SlotToTeam(slot));
    if (!teamInfo) return nullptr;
    // TeamInfo_GetPlayerBySlot (0x140A3B5B0) returns record 0 for an empty position (roster index 0xFF):
    // a card for "nobody" would land on another player
    if (ReadActiveRosterIndex(gmatch, slot) < 0) return nullptr;
    if (g_fnGetPlayerBySlot) {
        uint8_t* rec = g_fnGetPlayerBySlot(teamInfo, slot);
        if (IsValidPtr(rec)) return rec;
    }
    const int32_t rosterIdx = ReadActiveRosterIndex(gmatch, slot);
    if (rosterIdx >= 0) {
        return teamInfo + Offsets::TeamInfo_PlayerRecordsBase +
               static_cast<size_t>(rosterIdx) * Offsets::TeamInfo_PlayerRecordStride;
    }
    return nullptr;
}

int32_t ReadRealBallOwnerSlot(uint8_t* gmatch) {
    if (!IsValidPtr(gmatch)) return -1;
    uint8_t* ball = *reinterpret_cast<uint8_t**>(gmatch + Offsets::GMATCH_Ball);
    if (!IsValidPtr(ball)) return -1;
    const uint8_t* ballState = *reinterpret_cast<const uint8_t* const*>(ball + Offsets::Ball_StatePtr);
    if (!IsValidPtr(ballState)) return -1;
    if (ballState != g_verifiedBallStatePtr) {
        if (!IsMemoryRangeReadable(ballState + Offsets::BallState_OwnerSlot, 4)) {
            return -1;
        }
        g_verifiedBallStatePtr = ballState;
    }
    const int32_t owner = *reinterpret_cast<const int32_t*>(ballState + Offsets::BallState_OwnerSlot);
    return IsValidSlot(owner) ? owner : -1;
}

bool CopySanitizedUtf8(const char* src, size_t maxSrcLen, char* outBuf, size_t outSize) {
    if (!src || !outBuf || outSize == 0) return false;
    size_t len = 0;
    while (len + 1 < outSize && len < maxSrcLen && src[len] != '\0') {
        const unsigned char c = static_cast<unsigned char>(src[len]);
        if (c < 0x20) break;
        outBuf[len] = static_cast<char>(c);
        ++len;
    }
    while (len > 0 && outBuf[len - 1] == ' ') --len;
    outBuf[len] = '\0';
    // A byte limit can cut a multi-byte character: never leave a lone lead byte before the terminator
    Utf8TrimIncompleteTail(outBuf);
    for (size_t n = std::strlen(outBuf); n > 0 && outBuf[n - 1] == ' '; --n) outBuf[n - 1] = '\0';
    return outBuf[0] != '\0';
}

bool ReadBalAvatarName(const uint8_t* matchRoster, char* outBuf, size_t outSize) {
    if (!IsValidPtr(matchRoster) || !IsMemoryRangeReadable(matchRoster + Offsets::MatchRoster_BalNameString, 32)) {
        return false;
    }
    const uint8_t* strObj = matchRoster + Offsets::MatchRoster_BalNameString;
    const size_t len = *reinterpret_cast<const size_t*>(strObj + 0x10);
    const size_t cap = *reinterpret_cast<const size_t*>(strObj + 0x18);
    if (len == 0 || len > 63 || cap < len || cap > 256) return false;
    const char* ptr = (cap >= 16)
        ? *reinterpret_cast<const char* const*>(strObj)
        : reinterpret_cast<const char*>(strObj);
    if (!IsValidPtr(ptr) || !IsMemoryRangeReadable(ptr, len)) return false;
    return CopySanitizedUtf8(ptr, len, outBuf, outSize);
}

const uint8_t* ResolveRosterTable(const uint8_t*& outMatchRoster) {
    outMatchRoster = nullptr;
    if (!g_matchRosterPtrAddr) return nullptr;
    const uint8_t* matchRoster = *reinterpret_cast<const uint8_t* const*>(g_matchRosterPtrAddr);
    if (!IsValidPtr(matchRoster)) return nullptr;
    const uint8_t* dataPtr = *reinterpret_cast<const uint8_t* const*>(matchRoster + Offsets::MatchRoster_DataPtr);
    if (!IsValidPtr(dataPtr)) return nullptr;

    const uint8_t* rosterTable = dataPtr + Offsets::MatchRoster_TableOffset;
    const size_t totalSpan = Offsets::MatchRoster_PlayerBase + 2 * Offsets::MatchRoster_TeamStride;
    if (g_verifiedRosterDataPtr != dataPtr) {
        if (!IsMemoryRangeReadable(rosterTable, totalSpan)) {
            return nullptr;
        }
        g_verifiedRosterDataPtr = dataPtr;
    }
    outMatchRoster = matchRoster;
    return rosterTable;
}

bool ExtractRosterEntryIdentity(const uint8_t* matchRoster, const uint8_t* rosterTable, int32_t teamIdx,
                                size_t entryIdx, const uint8_t* entry,
                                char* outName, size_t outNameSz,
                                char* outShirt, size_t outShirtSz,
                                uint8_t& outShirtNum) {
    // Shirt numbers live in the team entry (uint16[40], same order as the roster), verified live on
    // Flamengo x Coritiba (patched game): Arrascaeta 10, Pedro 9, Rossi 1, Danilo 13, Bruno Henrique 27.
    const uint8_t* teamEntry = rosterTable + Offsets::MatchRoster_TeamEntryBase +
                               static_cast<size_t>(teamIdx) * Offsets::MatchRoster_TeamEntryStride;
    const uint16_t num = *reinterpret_cast<const uint16_t*>(
        teamEntry + Offsets::TeamEntry_ShirtNumbers + entryIdx * sizeof(uint16_t));
    outShirtNum = (num >= 1 && num <= 99) ? static_cast<uint8_t>(num) : 0;

    CopySanitizedUtf8(reinterpret_cast<const char*>(entry + Offsets::PlayerEntry_ShirtName),
                      24, outShirt, outShirtSz);

    const int32_t balFlag = *reinterpret_cast<const int32_t*>(entry + Offsets::PlayerEntry_BalAvatarFlag);
    if (balFlag < 0 && ReadBalAvatarName(matchRoster, outName, outNameSz)) {
        return true;
    }
    if (CopySanitizedUtf8(reinterpret_cast<const char*>(entry + Offsets::PlayerEntry_DisplayName),
                          56, outName, outNameSz)) {
        return true;
    }
    if (outShirt[0] != '\0') {
        std::snprintf(outName, outNameSz, "%s", outShirt);
        return true;
    }
    return false;
}

bool LookupPlayerFromRoster(int32_t teamIdx, int32_t rosterIdx, int32_t slotInTeam,
                            uint32_t playerId, uint32_t playerIdLow,
                            char* outName, size_t outNameSz,
                            char* outShirt, size_t outShirtSz,
                            uint8_t& outShirtNum) {
    if ((teamIdx != 0 && teamIdx != 1) || !outName || outNameSz == 0) return false;

    const uint8_t* matchRoster = nullptr;
    const uint8_t* rosterTable = ResolveRosterTable(matchRoster);
    if (!rosterTable) return false;

    const uint8_t* teamBase = rosterTable + Offsets::MatchRoster_PlayerBase +
                              static_cast<size_t>(teamIdx) * Offsets::MatchRoster_TeamStride;

    // Pass 0..1: exact and official 0xF00FFFFF masked match on high 32-bit playerId
    if (playerId != 0) {
        const uint32_t maskedId = playerId & 0xF00FFFFFU;
        for (int pass = 0; pass < 2; ++pass) {
            for (size_t i = 0; i < Offsets::MatchRoster_MaxPlayers; ++i) {
                const uint8_t* entry = teamBase + i * Offsets::MatchRoster_EntryStride;
                const uint64_t key = *reinterpret_cast<const uint64_t*>(entry + Offsets::PlayerEntry_PlayerKey);
                const uint32_t entryId = static_cast<uint32_t>(key >> 32);
                const bool match = (pass == 0)
                    ? (entryId == playerId)
                    : ((entryId & 0xF00FFFFFU) == maskedId);
                if (match && ExtractRosterEntryIdentity(matchRoster, rosterTable, teamIdx, i, entry, outName, outNameSz,
                                                        outShirt, outShirtSz, outShirtNum)) {
                    return true;
                }
            }
        }
    }

    // Pass 2: match on low 32-bit playerId (custom/edited DB entries)
    if (playerIdLow != 0 && playerIdLow != playerId) {
        const uint32_t maskedLow = playerIdLow & 0xF00FFFFFU;
        for (size_t i = 0; i < Offsets::MatchRoster_MaxPlayers; ++i) {
            const uint8_t* entry = teamBase + i * Offsets::MatchRoster_EntryStride;
            const uint64_t key = *reinterpret_cast<const uint64_t*>(entry + Offsets::PlayerEntry_PlayerKey);
            const uint32_t entryId = static_cast<uint32_t>(key >> 32);
            if ((entryId == playerIdLow || (entryId & 0xF00FFFFFU) == maskedLow) &&
                ExtractRosterEntryIdentity(matchRoster, rosterTable, teamIdx, i, entry, outName, outNameSz,
                                           outShirt, outShirtSz, outShirtNum)) {
                return true;
            }
        }
    }

    // Pass 3: direct live roster index from TeamInfo + 0x04 (handles substitutions & BAL cleanly)
    if (rosterIdx >= 0 && rosterIdx < static_cast<int32_t>(Offsets::MatchRoster_MaxPlayers)) {
        const uint8_t* entry = teamBase + static_cast<size_t>(rosterIdx) * Offsets::MatchRoster_EntryStride;
        if (ExtractRosterEntryIdentity(matchRoster, rosterTable, teamIdx, static_cast<size_t>(rosterIdx), entry, outName, outNameSz,
                                       outShirt, outShirtSz, outShirtNum)) {
            return true;
        }
    }

    // Pass 4: starting XI slot fallback
    if (slotInTeam >= 0 && slotInTeam < 11) {
        const uint8_t* entry = teamBase + static_cast<size_t>(slotInTeam) * Offsets::MatchRoster_EntryStride;
        if (ExtractRosterEntryIdentity(matchRoster, rosterTable, teamIdx, static_cast<size_t>(slotInTeam), entry, outName, outNameSz,
                                       outShirt, outShirtSz, outShirtNum)) {
            return true;
        }
    }
    return false;
}

void MakeFallbackShortName(const char* fullName, int teamIdx, char* outShort, size_t outShortSz) {
    size_t n = 0;
    if (fullName) {
        for (size_t i = 0; fullName[i] != '\0' && n < 3 && n + 1 < outShortSz; ++i) {
            const unsigned char c = static_cast<unsigned char>(fullName[i]);
            if (std::isalnum(c)) {
                outShort[n++] = static_cast<char>(std::toupper(c));
            }
        }
    }
    if (n > 0) {
        outShort[n] = '\0';
    } else {
        outShort[0] = '\0'; // unknown: the HUD shows its localized CAS/FOR fallback
    }
}

void SyncTeamsAndColors() {
    const uint8_t* matchRoster = nullptr;
    const uint8_t* rosterTable = ResolveRosterTable(matchRoster);
    if (!rosterTable || !matchRoster) return;

    const uint32_t curKit0 = *reinterpret_cast<const uint32_t*>(rosterTable + Offsets::MatchRoster_SelectedKitId);
    const uint32_t curKit1 = *reinterpret_cast<const uint32_t*>(rosterTable + Offsets::MatchRoster_SelectedKitId + 4);
    if (rosterTable == g_syncedTeamRosterTable &&
        curKit0 == g_syncedTeamKitIds[0] &&
        curKit1 == g_syncedTeamKitIds[1]) {
        return; // Already resolved for this match & uniform selection (O(1) fast return)
    }

    const uint8_t* editedKitPtr = *reinterpret_cast<const uint8_t* const*>(
        matchRoster + Offsets::MatchRoster_EditedKitPtr);
    bool editedKitReadable = false;
    if (IsValidPtr(editedKitPtr)) {
        if (editedKitPtr == g_verifiedEditedKitPtr) {
            editedKitReadable = true;
        } else if (IsMemoryRangeReadable(editedKitPtr, Offsets::EditedKit_TeamStride + Offsets::EditedKit_SecondaryRGB + 4)) {
            g_verifiedEditedKitPtr = editedKitPtr;
            editedKitReadable = true;
        }
    }

    std::array<TeamDisplayInfo, 2> teams{};
    teams[0].primaryColor   = RgbColor{ 48, 152, 245 };
    teams[0].secondaryColor = RgbColor{ 240, 246, 255 };
    teams[1].primaryColor   = RgbColor{ 238, 76, 86 };
    teams[1].secondaryColor = RgbColor{ 255, 244, 244 };

    bool anyReal = false;
    bool allReal = true;
    bool allNamed = true;
    for (int t = 0; t < 2; ++t) {
        const uint8_t* teamEntry = rosterTable + Offsets::MatchRoster_TeamEntryBase +
                                   static_cast<size_t>(t) * Offsets::MatchRoster_TeamEntryStride;

        const bool hasFull = CopySanitizedUtf8(
            reinterpret_cast<const char*>(teamEntry + Offsets::TeamEntry_FullName),
            63, teams[t].fullName, sizeof(teams[t].fullName));
        if (!hasFull) {
            teams[t].fullName[0] = '\0'; // unknown: the HUD shows its localized CASA/FORA fallback
        }

        const bool hasShort = CopySanitizedUtf8(
            reinterpret_cast<const char*>(teamEntry + Offsets::TeamEntry_ShortName),
            7, teams[t].shortName, sizeof(teams[t].shortName));
        if (!hasShort) {
            MakeFallbackShortName(teams[t].fullName, t, teams[t].shortName, sizeof(teams[t].shortName));
        }

        // Uniform primary & secondary RGB (exact logic of 0x1414C9580)
        const uint32_t kitId = *reinterpret_cast<const uint32_t*>(
            rosterTable + Offsets::MatchRoster_SelectedKitId + static_cast<size_t>(t) * 4);
        const uint32_t teamBaseId = *reinterpret_cast<const uint32_t*>(teamEntry + Offsets::TeamEntry_TeamId);

        bool gotColor = false;
        if (((kitId ^ teamBaseId) & 0xFFFFC000U) == 0) {
            // Licensed team kit table at teamEntry + 0x5D8 (10 entries * 8 bytes)
            const uint32_t wantHi  = (kitId >> 9) & 0x1FU;
            const uint32_t wantMid = (kitId >> 7) & 0x03U;
            const uint32_t wantLo  = kitId & 0x7FU;
            const uint8_t* kt = teamEntry + Offsets::TeamEntry_KitTable;
            for (size_t i = 0; i < Offsets::TeamEntry_KitTableCount; ++i) {
                const uint8_t* e = kt + i * Offsets::TeamEntry_KitEntryStride;
                if ((static_cast<uint32_t>(e[0] & 0x0FU) == wantHi) &&
                    (static_cast<uint32_t>(e[1] & 0x01U) == wantMid) &&
                    (static_cast<uint32_t>(e[0] >> 4) == wantLo)) {
                    teams[t].primaryColor   = RgbColor{ e[2], e[3], e[4] };
                    teams[t].secondaryColor = RgbColor{ e[5], e[6], e[7] };
                    gotColor = true;
                    break;
                }
            }
            // Fallback for licensed/patched kits whose slot index wasn't matched: try kit table slot 0
            if (!gotColor) {
                const uint8_t* e0 = kt;
                if ((e0[2] | e0[3] | e0[4] | e0[5] | e0[6] | e0[7]) != 0) {
                    teams[t].primaryColor   = RgbColor{ e0[2], e0[3], e0[4] };
                    teams[t].secondaryColor = RgbColor{ e0[5], e0[6], e0[7] };
                    gotColor = true;
                }
            }
        }
        if (!gotColor && editedKitReadable) {
            // Edited / patch team kits at *(MatchRoster + 0x58) + (teamIdx * 2) * 0x84
            const uint8_t* kb = editedKitPtr + static_cast<size_t>(t) * Offsets::EditedKit_TeamStride;
            const RgbColor pri{ kb[Offsets::EditedKit_PrimaryRGB + 0],
                                kb[Offsets::EditedKit_PrimaryRGB + 1],
                                kb[Offsets::EditedKit_PrimaryRGB + 2] };
            const RgbColor sec{ kb[Offsets::EditedKit_SecondaryRGB + 0],
                                kb[Offsets::EditedKit_SecondaryRGB + 1],
                                kb[Offsets::EditedKit_SecondaryRGB + 2] };
            if ((pri.r | pri.g | pri.b | sec.r | sec.g | sec.b) != 0) {
                teams[t].primaryColor = pri;
                teams[t].secondaryColor = sec;
                gotColor = true;
            }
        }

        teams[t].hasRealData = hasFull || hasShort || gotColor;
        anyReal = anyReal || teams[t].hasRealData;
        allReal = allReal && teams[t].hasRealData;
        allNamed = allNamed && hasFull;
    }

    ColorHelpers::EnsureDistinctTeamColors(
        teams[0].primaryColor, teams[0].secondaryColor,
        teams[1].primaryColor, teams[1].secondaryColor);

    if (anyReal) {
        // Latch the O(1) cache only when BOTH teams are fully resolved, so a roster that is still
        // loading on one side keeps being re-read every 25 ticks exactly like before.
        if (allReal && allNamed) {
            g_syncedTeamRosterTable = rosterTable;
            g_syncedTeamKitIds[0] = curKit0;
            g_syncedTeamKitIds[1] = curKit1;
        }
        VarState::Instance().UpdateTeams(teams);
        if (!g_loggedTeamSyncOnce && allNamed) {
            g_loggedTeamSyncOnce = true;
            VAR_LOG_INF("Resolved real teams: HOME='%s' (%s) RGB(%u,%u,%u) vs AWAY='%s' (%s) RGB(%u,%u,%u)",
                        teams[0].fullName, teams[0].shortName,
                        teams[0].primaryColor.r, teams[0].primaryColor.g, teams[0].primaryColor.b,
                        teams[1].fullName, teams[1].shortName,
                        teams[1].primaryColor.r, teams[1].primaryColor.g, teams[1].primaryColor.b);
        }
    }
}

// One-shot (max 3 per match) dump of every stage of the name resolution, so a failed mapping can be
// diagnosed from var_core.log alone.
void LogRosterDiagnostics(uint8_t* gmatch, int resolvedCount) {
    const uint8_t* mr = g_matchRosterPtrAddr
        ? *reinterpret_cast<const uint8_t* const*>(g_matchRosterPtrAddr) : nullptr;
    const uint8_t* dataPtr = (IsValidPtr(mr) && IsMemoryRangeReadable(mr + Offsets::MatchRoster_DataPtr, 8))
        ? *reinterpret_cast<const uint8_t* const*>(mr + Offsets::MatchRoster_DataPtr) : nullptr;
    const uint8_t* table = IsValidPtr(dataPtr) ? dataPtr + Offsets::MatchRoster_TableOffset : nullptr;
    const size_t span = Offsets::MatchRoster_PlayerBase + 2 * Offsets::MatchRoster_TeamStride;
    const bool tableOk = table && IsMemoryRangeReadable(table, span);
    VAR_LOG_INF("RosterDiag tick=%llu resolved=%d/22 rosterGlobal=0x%llX matchRoster=%p dataPtr=%p table=%p readable=%d",
                static_cast<unsigned long long>(g_mainTickCount), resolvedCount,
                static_cast<unsigned long long>(g_matchRosterPtrAddr),
                static_cast<const void*>(mr), static_cast<const void*>(dataPtr),
                static_cast<const void*>(table), tableOk ? 1 : 0);
    for (int t = 0; t < 2; ++t) {
        uint8_t* ti = GetTeamInfo(gmatch, t);
        char full[64] = "", shortN[8] = "", p0[48] = "";
        uint32_t teamId = 0;
        uint64_t key0 = 0;
        unsigned char raw[8] = {};
        if (tableOk) {
            const uint8_t* te = table + Offsets::MatchRoster_TeamEntryBase +
                                static_cast<size_t>(t) * Offsets::MatchRoster_TeamEntryStride;
            teamId = *reinterpret_cast<const uint32_t*>(te + Offsets::TeamEntry_TeamId);
            std::memcpy(raw, te + Offsets::TeamEntry_FullName, sizeof(raw));
            CopySanitizedUtf8(reinterpret_cast<const char*>(te + Offsets::TeamEntry_FullName), 63, full, sizeof(full));
            CopySanitizedUtf8(reinterpret_cast<const char*>(te + Offsets::TeamEntry_ShortName), 7, shortN, sizeof(shortN));
            const uint8_t* e0 = table + Offsets::MatchRoster_PlayerBase +
                                static_cast<size_t>(t) * Offsets::MatchRoster_TeamStride;
            key0 = *reinterpret_cast<const uint64_t*>(e0 + Offsets::PlayerEntry_PlayerKey);
            CopySanitizedUtf8(reinterpret_cast<const char*>(e0 + Offsets::PlayerEntry_DisplayName), 47, p0, sizeof(p0));
        }
        const int slot0 = t * 11;
        uint8_t* rec = GetPlayerRecord(gmatch, slot0);
        const uint64_t recKey = rec ? *reinterpret_cast<const uint64_t*>(rec + Offsets::PlayerRecord_PlayerKey) : 0;
        VAR_LOG_INF("RosterDiag team%d teamInfo=%p teamId=0x%X full='%s' short='%s' raw=%02X %02X %02X %02X %02X %02X %02X %02X "
                    "entry0key=0x%llX entry0name='%s' slot%d rec=%p recKey=0x%llX rosterIdx=%d",
                    t, static_cast<void*>(ti), teamId, full, shortN,
                    raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7],
                    static_cast<unsigned long long>(key0), p0, slot0, static_cast<void*>(rec),
                    static_cast<unsigned long long>(recKey), ReadActiveRosterIndex(gmatch, slot0));
    }
}

void SyncPlayersAndCards(uint8_t* gmatch, uint8_t* matchInfo) {
    // VarState::ResetMatch (Lua thread) wipes names. If it lands after this thread already latched
    // its caches, nothing would ever be pushed again: re-arm on every state generation change.
    const uint32_t gen = VarState::Instance().MatchGeneration();
    if (gen != g_syncedStateGeneration) {
        g_syncedStateGeneration = gen;
        g_syncedTeamRosterTable = nullptr;
        g_hasPushedPlayersOnce = false;
    }
    SyncTeamsAndColors();
    std::array<PlayerSlotInfo, 22> list{};
    int resolvedCount = 0;
    bool anyChanged = !g_hasPushedPlayersOnce;

    for (int slot = 0; slot < 22; ++slot) {
        auto& info = list[slot];
        info.slot = slot;
        info.team = SlotToTeam(slot);
        info.onPitch = *reinterpret_cast<const uint8_t*>(matchInfo + Offsets::MatchInfo_PlayerOnPitch + slot);

        const int32_t rosterIdx = ReadActiveRosterIndex(gmatch, slot);
        uint32_t playerIdLow = 0;
        uint8_t* rec = GetPlayerRecord(gmatch, slot);
        info.rosterIdx = static_cast<int8_t>(rosterIdx);
        if (rec) {
            info.yellows = rec[Offsets::PlayerRecord_Yellows];
            info.reds    = rec[Offsets::PlayerRecord_Reds];
            const uint64_t key = *reinterpret_cast<const uint64_t*>(rec + Offsets::PlayerRecord_PlayerKey);
            info.playerId = static_cast<uint32_t>(key >> 32);
            playerIdLow = static_cast<uint32_t>(key & 0xFFFFFFFFULL);
        }

        const bool cacheHit = (info.playerId != 0) &&
                              (info.playerId == g_cachedPlayerIds[slot]) &&
                              (rosterIdx == g_cachedRosterIdx[slot]) &&
                              (g_cachedPlayerNames[slot][0] != '\0');
        if (cacheHit) {
            std::memcpy(info.name, g_cachedPlayerNames[slot], sizeof(info.name));
            std::memcpy(info.shirtName, g_cachedShirtNames[slot], sizeof(info.shirtName));
            info.shirtNumber = g_cachedShirtNums[slot];
            ++resolvedCount;
        } else if (LookupPlayerFromRoster(info.team, rosterIdx, slot % 11, info.playerId, playerIdLow,
                                          info.name, sizeof(info.name),
                                          info.shirtName, sizeof(info.shirtName),
                                          info.shirtNumber)) {
            g_cachedPlayerIds[slot] = info.playerId ? info.playerId : static_cast<uint32_t>(slot + 1);
            g_cachedRosterIdx[slot] = rosterIdx;
            g_cachedShirtNums[slot] = info.shirtNumber;
            std::memcpy(g_cachedPlayerNames[slot], info.name, sizeof(info.name));
            std::memcpy(g_cachedShirtNames[slot], info.shirtName, sizeof(info.shirtName));
            anyChanged = true;
            ++resolvedCount;
        } else {
            if (g_cachedPlayerIds[slot] != 0 || g_cachedRosterIdx[slot] != -1) {
                anyChanged = true;
            }
            g_cachedPlayerIds[slot] = 0;
            g_cachedRosterIdx[slot] = -1;
            g_cachedShirtNums[slot] = 0;
            g_cachedPlayerNames[slot][0] = '\0';
            g_cachedShirtNames[slot][0] = '\0';
            info.shirtNumber = 0;
            info.name[0] = '\0'; // unknown: the HUD shows its localized "Casa N" / "Fora N" fallback
        }

        if (info.yellows != g_cachedYellows[slot] ||
            info.reds    != g_cachedReds[slot]    ||
            info.onPitch != g_cachedOnPitch[slot]) {
            g_cachedYellows[slot] = info.yellows;
            g_cachedReds[slot]    = info.reds;
            g_cachedOnPitch[slot] = info.onPitch;
            anyChanged = true;
        }
    }

    if (!g_loggedRosterSyncOnce && resolvedCount > 0) {
        g_loggedRosterSyncOnce = true;
        char b0[80], b11[80];
        list[0].FormatBroadcastName(b0, sizeof(b0));
        list[11].FormatBroadcastName(b11, sizeof(b11));
        VAR_LOG_INF("Resolved %d/22 real players (home[0]='%s', away[0]='%s')", resolvedCount, b0, b11);
    }

    if (anyChanged) {
        g_hasPushedPlayersOnce = true;
        VarState::Instance().UpdatePlayers(list);
    }

    const bool incomplete = (resolvedCount < 22) || !g_loggedTeamSyncOnce;
    if (g_rosterDiagCount < 3 &&
        (g_rosterDiagCount == 0 || (incomplete && g_mainTickCount - g_rosterDiagTick >= 900))) {
        ++g_rosterDiagCount;
        g_rosterDiagTick = g_mainTickCount;
        LogRosterDiagnostics(gmatch, resolvedCount);
    }
}

void SampleInPlayFrame(uint8_t* gmatch, uint8_t* matchInfo, int32_t matchPhase, float time60Hz,
                       uint32_t matchTick, bool storeSnapshot) {
    // 1. Track real ball owner at 60Hz so we always know who held possession before any tackle
    const int32_t realOwner = ReadRealBallOwnerSlot(gmatch);
    if (IsValidSlot(realOwner)) {
        g_recentBallOwnerSlot = realOwner;
        g_recentBallOwnerTick = matchTick;
    } else if (matchTick < g_recentBallOwnerTick || (matchTick - g_recentBallOwnerTick) > 48U) {
        g_recentBallOwnerSlot = -1;
    }

    // 2. Quick 60Hz scan of PlayerAction (22 pointer derefs, < 0.3 us): engine action id, flags and contact causer
    int8_t actionIds[22] = {};
    uint32_t actionFlags[22] = {};
    int8_t causers[22];
    std::memset(causers, -1, sizeof(causers));
    bool anyPhysicalAction = false;
    for (int slot = 0; slot < 22; ++slot) {
        if (!*reinterpret_cast<const uint8_t*>(matchInfo + Offsets::MatchInfo_PlayerOnPitch + slot)) continue;
        const uint8_t* act = *reinterpret_cast<const uint8_t* const*>(
            gmatch + Offsets::GMATCH_PlayerActionBase + static_cast<size_t>(slot) * Offsets::GMATCH_EntryStride);
        if (!IsValidPtr(act)) continue;
        const int8_t aid = *reinterpret_cast<const int8_t*>(act + Offsets::PlayerAction_ActionId);
        const uint32_t flags = *reinterpret_cast<const uint32_t*>(act + Offsets::PlayerAction_ActionFlags);
        actionIds[slot] = aid;
        actionFlags[slot] = flags;
        causers[slot] = *reinterpret_cast<const int8_t*>(act + Offsets::PlayerAction_Causer);
        if (PlayerActionId::IsSlide(aid) || aid == PlayerActionId::Tackle || aid == PlayerActionId::Jostle ||
            aid == PlayerActionId::Dive || PlayerActionId::IsContactReaction(aid, flags) ||
            (flags & PlayerActionId::FlagSimulation) != 0) {
            anyPhysicalAction = true;
        }
    }

    // Every frame with physical contact is evaluated: the incident clustering keeps the closest
    // contact distance across the whole tackle, so sub-sampling here would degrade detection.
    if (!storeSnapshot && !anyPhysicalAction) {
        return;
    }

    PlaySnapshot snap{};
    snap.matchPhase          = matchPhase;
    snap.time60Hz            = time60Hz;
    snap.matchTick           = matchTick;
    snap.ballOwnerSlot       = realOwner;
    snap.recentBallOwnerSlot = g_recentBallOwnerSlot;

    uint8_t* ball = *reinterpret_cast<uint8_t**>(gmatch + Offsets::GMATCH_Ball);
    if (IsValidPtr(ball)) {
        std::memcpy(&snap.ballPos, ball + Offsets::Ball_PosVec3, sizeof(Vec3));
    }

    for (int slot = 0; slot < 22; ++slot) {
        snap.playerOnPitch[slot]   = *reinterpret_cast<const uint8_t*>(matchInfo + Offsets::MatchInfo_PlayerOnPitch + slot);
        snap.playerActionId[slot]    = actionIds[slot];
        snap.playerActionFlags[slot] = actionFlags[slot];
        snap.playerCauser[slot]      = causers[slot];
        snap.playerRosterIdx[slot]   = static_cast<int8_t>(ReadActiveRosterIndex(gmatch, slot));
        uint8_t* spatial = *reinterpret_cast<uint8_t**>(
            gmatch + Offsets::GMATCH_PlayerSpatialBase + static_cast<size_t>(slot) * Offsets::GMATCH_EntryStride);
        if (IsValidPtr(spatial)) {
            std::memcpy(&snap.playerPos[slot], spatial + Offsets::PlayerSpatial_PosVec3, sizeof(Vec3));
        }
    }

    if (anyPhysicalAction) {
        const PitchDims dims = ReadPitchDims(gmatch);
        const float dirs[2] = { ReadTeamAttackDir(gmatch, 0), ReadTeamAttackDir(gmatch, 1) };
        DetectedIncident inc{};
        if (VarState::DetectPhysicalIncident(snap, dims, dirs, inc)) {
            VarState::Instance().PushDetectedIncident(inc);
        }
    }

    if (storeSnapshot) {
        VarState::ComputeClosestContact(snap);
        VarState::Instance().PushSnapshot(snap);
    }
}

bool IsMatchEligibleForExecution(uint8_t* gmatch, uint8_t* matchInfo, uint8_t* replayMgr) {
    if (!VarState::Instance().IsEnabled() || !VarState::Instance().IsOfflineAllowed()) {
        return false;
    }
    if (!IsValidPtr(gmatch) || !IsValidPtr(matchInfo) || !IsValidPtr(replayMgr)) {
        return false;
    }
    const uint8_t specialMode = *reinterpret_cast<const uint8_t*>(matchInfo + Offsets::MatchInfo_SpecialModeKind);
    if (specialMode != 0) return false;

    const int32_t phase = *reinterpret_cast<const int32_t*>(matchInfo + Offsets::MatchInfo_MatchPhase);
    if (phase < 0 || phase >= 8) return false;

    const uint8_t replayMode = *reinterpret_cast<const uint8_t*>(replayMgr + Offsets::ReplayMgr_Mode);
    const uint8_t manualFlag = *reinterpret_cast<const uint8_t*>(replayMgr + Offsets::ReplayMgr_ManualScreenFlag);
    if (replayMode != 0 || manualFlag != 0) return false;

    return true;
}

// ============================================================================
// Native-first VAR execution
// ============================================================================
// Engine facts this design relies on (PES2021.exe 1.7.2.0, see re_notes/05_native_decision_pipeline.md):
//  - Every frame the engine zeroes MatchControl + 0x6E0 (0x1404228D0 -> 0x140441810) and the FSM JudgeResult
//    (0x140444A20 -> 0x14050B2D0) BEFORE MainTick, then CommitToRegistry applies the committed foul's card once
//    (0x1404231DC..0x140423202: yellow++, second yellow -> red++, red++). So an injected JudgeResult gets its
//    card applied by the engine exactly once, on the injection tick.
//  - BuildOutOfPlayQueue (0x1404530C0) queues states 0x12 (0x140449050: on-pitch flags recomputed from the
//    PlayerRecord cards) and 0x13 (0x1404485B0: actor removal) for StoppageReason 7 (SilentRestart, builders
//    at 0x14045333C) and, through 0x140452F00, for reason 5 with a carded foul.
//  - MatchFSM_RebuildSetPlay (0x140454280) re-queues the SAME set play with 0x12 / 0x13 and no foul (the engine
//    uses it in SetPlayReady for kicker / tactical changes: 0x14044E286, 0x14044E328, 0x14044E3AF).
//  - There is no native card decrement: revoking a card is the only manual PlayerRecord write left.
// So the mod: writes manual cards only for revocations and card-only edits, lets the engine apply the card of
// every injected foul, never touches the on-pitch flags, and verifies each step by reading the FSM back.

enum class ExecState : uint8_t {
    Idle,             // nothing in flight; VarState may hold an armed pool
    AwaitNativeHook,  // pool consumed, waiting for Judge::FinalizeFrame (inside the 0x1A / 0x1C handler)
    AwaitTransition,  // request performed, waiting for the FSM to leave the state it was in
    Cooldown,         // settle ticks before the next pool
};

enum class ExecRequestKind : uint8_t { None, InjectSetPlay, RebuildSetPlay };

struct ExecRequest {
    ExecRequestKind kind          = ExecRequestKind::None;
    SetPlayKind     spKind        = SetPlayKind::None;
    int32_t         benefitedTeam = 0;
    Vec3            spot{};
    StoppageReason  stopReason    = StoppageReason::SilentRestart;
    bool            foulValid     = false;
    CardKind        card          = CardKind::None; // applied by the engine's CommitToRegistry
    int32_t         offenderSlot  = -1;
    int32_t         victimSlot    = -1;
    int32_t         offenderRoster = -1; // roster index of the meant player (-1 = current holder of the slot)
    int32_t         victimRoster   = -1;
    uint32_t        snapId        = 0;
};

struct ExecCtx {
    ExecState   state          = ExecState::Idle;
    uint32_t    matchGen       = 0;
    uint32_t    poolSeq        = 0;
    uint64_t    deadlineTick   = 0;
    uint8_t*    fsm            = nullptr;
    int32_t     startFsmState  = -1;
    ExecRequest req{};
    // Ledger, recorded only once the engine confirmed the transition
    bool        hasPlayLedger  = false;
    VarVerdict  playVerdict{};
    bool        pushRevEvent   = false;
    VarEvent    revEvent{};
    std::array<VarVerdict, kMaxCardPoolEntries> cardLedger{};
    size_t      cardLedgerCount = 0;
    bool        cardsApplied   = false;
    bool        redInvolved    = false;
    BannerStyle bannerStyle    = BannerStyle::Applied;
    char        bannerText[128] = {};
    // The engine's commit of OUR injected foul must not be recorded as a new official incident
    bool        expectOwnFoulCommit = false;
    int32_t     expectOffender = -1;
    uint32_t    expectSnapId   = 0;
    bool        injected       = false; // the request was written into the JudgeResult
    bool        playOrigRevoked = false; // the play's original card was revoked by hand
    // Undo data of a SetPlayReady injection, verified right after this tick's MainTick (before the commit)
    bool        verifyAfterTick = false;
    bool        undoActive      = false;
    struct CardUndo { uint8_t* rec; uint8_t* wbRec; uint8_t y; uint8_t r; };
    // Worst case per injection: 1 revocation + 2 writes per moved card edit (32) + 1 dropped-offender card
    std::array<CardUndo, 2 * kMaxCardPoolEntries + 8> cardUndo{};
    size_t      cardUndoCount   = 0;
    bool        cardUndoOverflow = false;
    uint8_t     savedJudge[sizeof(RawSetPlayInfo) + sizeof(RawFoulInfo)]{};
    uint8_t     savedMiSetPlay[0x30]{};
    uint8_t     savedMiFoul[0x3C]{};
    uint8_t     savedWbSetPlay[0x30]{};
    uint8_t     savedWbFoul[0x3C]{};
    bool        savedWbValid    = false;
    uint32_t    savedQuickRestart = 0;
    uint64_t    savedKickerCache  = 0;
    bool        baselineValid  = false; // offender's cards right before the injection (engine commit check)
    uint8_t     baselineYellows = 0;
    uint8_t     baselineReds    = 0;
    CardKind    playManualCard = CardKind::None; // card of the play given by hand (engine did not commit it)
    bool        ownFoulCommitted = false; // the engine committed (and carded) the injected foul
    // Plan (decided at consume time, applied in the tick the engine runs it)
    VerdictPool pool{};
    bool        cardsPlanApplied = false;
    std::array<bool, kMaxCardPoolEntries> absorbed{};
    bool        laterDirectOnOffender = false;
    CardKind    playCard       = CardKind::None;
    uint8_t     onPitchBefore[22] = {};
    int32_t     revokedSlot    = -1;
    bool        revokedWasSentOff = false;
    bool        sentOffChanged = false;
};
ExecCtx g_exec{};
bool    g_loggedDeferredOnPitch = false;

constexpr uint64_t kExecHookTimeoutTicks       = 3;   // FinalizeFrame runs every frame in 0x1A / 0x1C
constexpr uint64_t kExecTransitionTimeoutTicks = 3;   // EnterOutOfPlay / RebuildSetPlay are synchronous
constexpr uint64_t kExecCooldownTicks          = 20;

bool IsExecFsmState(int32_t s) { return s == 0x1A || s == 0x1C; }

void ExecEnter(ExecState s, uint64_t ticks) {
    g_exec.state = s;
    g_exec.deadlineTick = g_mainTickCount + ticks;
}

void ExecResetToIdle() {
    g_exec = ExecCtx{};
}

// Current cards of a player: write buffer first (where the engine commits), read cache as fallback
bool ReadCards(uint8_t* gmatch, int32_t slot, int32_t rosterIdx, uint8_t& yellows, uint8_t& reds);

// Same record in the TeamInfo write buffer (see GetTeamInfoWriteBuffer), or nullptr
uint8_t* WriteBufferRecordOf(uint8_t* gmatch, int32_t slot, uint8_t* rec) {
    uint8_t* teamInfo = GetTeamInfo(gmatch, SlotToTeam(slot));
    if (!teamInfo || !rec) return nullptr;
    const uintptr_t first = reinterpret_cast<uintptr_t>(teamInfo + Offsets::TeamInfo_PlayerRecordsBase);
    const uintptr_t r = reinterpret_cast<uintptr_t>(rec);
    if (r < first || r >= first + Offsets::MatchRoster_MaxPlayers * Offsets::TeamInfo_PlayerRecordStride) return nullptr;
    uint8_t* wb = GetTeamInfoWriteBuffer(teamInfo, SlotToTeam(slot));
    return wb ? rec + Offsets::TeamInfo_BufferSize : nullptr;
}

bool ReadCards(uint8_t* gmatch, int32_t slot, int32_t rosterIdx, uint8_t& yellows, uint8_t& reds) {
    yellows = 0;
    reds = 0;
    uint8_t* rec = IsValidSlot(slot) ? GetPlayerRecordFor(gmatch, slot, rosterIdx) : nullptr;
    if (!rec) return false;
    const uint8_t* src = WriteBufferRecordOf(gmatch, slot, rec);
    if (!src) src = rec;
    yellows = src[Offsets::PlayerRecord_Yellows];
    reds = src[Offsets::PlayerRecord_Reds];
    return true;
}

bool SetPlayerCardsAbsolute(uint8_t* gmatch, int32_t slot, uint8_t newYellows, uint8_t newReds, int32_t rosterIdx = -1) {
    // Manual PlayerRecord write (the engine has no card decrement). The on-pitch flags are NOT touched:
    // state 0x12 (0x140449050) recomputes them from these bytes when the set-play queue runs.
    // Returns true when the sent-off status (red or two yellows) changed.
    if (!IsValidSlot(slot)) return false;
    uint8_t* rec = GetPlayerRecordFor(gmatch, slot, rosterIdx);
    if (!rec || !IsMemoryRangeReadable(rec, 2)) {
        VAR_LOG_WRN("SetPlayerCardsAbsolute: PlayerRecord unavailable for slot %d", slot);
        return false;
    }
    uint8_t* wbRec = WriteBufferRecordOf(gmatch, slot, rec);
    if (!wbRec) {
        // Without the write buffer the next publish (0x14041BE40) would undo this write one frame later:
        // a revocation would come back and the engine would still add the new card on top
        VAR_LOG_ERR("SetPlayerCardsAbsolute: TeamInfo write buffer unavailable for slot %d, card not written", slot);
        return false;
    }
    const uint8_t* cur = wbRec;
    const uint8_t oldYellows = cur[Offsets::PlayerRecord_Yellows];
    const uint8_t oldReds = cur[Offsets::PlayerRecord_Reds];
    if (g_exec.undoActive) {
        if (g_exec.cardUndoCount < g_exec.cardUndo.size()) {
            g_exec.cardUndo[g_exec.cardUndoCount++] = ExecCtx::CardUndo{ rec, wbRec, oldYellows, oldReds };
        } else {
            g_exec.cardUndoOverflow = true;
        }
    }
    rec[Offsets::PlayerRecord_Yellows] = newYellows;
    rec[Offsets::PlayerRecord_Reds] = newReds;
    wbRec[Offsets::PlayerRecord_Yellows] = newYellows;
    wbRec[Offsets::PlayerRecord_Reds] = newReds;
    const bool wasSentOff = (oldReds > 0 || oldYellows >= 2);
    const bool nowSentOff = (newReds > 0 || newYellows >= 2);
    const bool holdsSlot = SlotHeldBy(gmatch, slot, rosterIdx);
    VAR_LOG_INF("SetPlayerCardsAbsolute slot=%d roster=%d%s Y %u->%u R %u->%u", slot, rosterIdx,
                holdsSlot ? "" : " (substituted)",
                static_cast<unsigned>(oldYellows), static_cast<unsigned>(newYellows),
                static_cast<unsigned>(oldReds), static_cast<unsigned>(newReds));
    // A substituted player is off the pitch either way: his cards never change who is on it
    return holdsSlot && (wasSentOff != nowSentOff);
}

bool ApplyCardDelta(uint8_t* gmatch, int32_t slot, CardKind origCard, CardKind targetCard, int32_t rosterIdx = -1) {
    if (!IsValidSlot(slot)) return false;
    uint8_t* rec = GetPlayerRecordFor(gmatch, slot, rosterIdx);
    if (!rec || !IsMemoryRangeReadable(rec, 2)) {
        VAR_LOG_WRN("ApplyCardDelta: PlayerRecord unavailable for slot %d", slot);
        return false;
    }
    const uint8_t* src = WriteBufferRecordOf(gmatch, slot, rec);
    if (!src) src = rec;
    uint8_t curY = src[Offsets::PlayerRecord_Yellows];
    uint8_t curR = src[Offsets::PlayerRecord_Reds];
    if (origCard == CardKind::Yellow) {
        if (curY >= 2 && curR > 0) --curR;
        if (curY > 0) --curY;
    } else if (origCard == CardKind::Red) {
        if (curR > 0) --curR;
    }
    // Same rule as CommitToRegistry (0x1404231FB..0x140423202): a second yellow also adds a red. Never a third
    // yellow or a second red (the engine never produces them): a sent-off player gets no further card.
    if (targetCard == CardKind::Yellow) {
        if (!(curY >= 2 || (curY >= 1 && curR > 0))) {
            ++curY;
            if (curY >= 2 && curR == 0) curR = 1;
        }
    } else if (targetCard == CardKind::Red) {
        if (curR == 0) curR = 1;
    }
    curY = std::min<uint8_t>(curY, 2);
    curR = std::min<uint8_t>(curR, 1);
    return SetPlayerCardsAbsolute(gmatch, slot, curY, curR, rosterIdx);
}

// Card-only edit (no set play): the whole delta / absolute value is a manual write
bool ApplyVerdictCards(uint8_t* gmatch, const VarVerdict& v) {
    if (v.directCardEdit) {
        return SetPlayerCardsAbsolute(gmatch, v.offenderSlot, v.directYellows, v.directReds, v.offenderRosterIdx);
    }
    const bool movedPlayer = IsValidSlot(v.origOffenderSlot) && IsValidSlot(v.offenderSlot) &&
                             (v.origOffenderSlot != v.offenderSlot ||
                              v.origOffenderRosterIdx != v.offenderRosterIdx);
    if (movedPlayer) {
        bool changed = false;
        if (v.origCard == CardKind::Yellow || v.origCard == CardKind::Red) {
            changed |= ApplyCardDelta(gmatch, v.origOffenderSlot, v.origCard, CardKind::None, v.origOffenderRosterIdx);
        }
        if (v.targetCard == CardKind::Yellow || v.targetCard == CardKind::Red) {
            changed |= ApplyCardDelta(gmatch, v.offenderSlot, CardKind::None, v.targetCard, v.offenderRosterIdx);
        }
        return changed;
    }
    if (v.targetCard != v.origCard) {
        return ApplyCardDelta(gmatch, v.offenderSlot, v.origCard, v.targetCard, v.offenderRosterIdx);
    }
    return false;
}

namespace {
bool PoolHasId(const uint8_t* pool, uint32_t id) {
    if (id == 0) return false;
    const auto* ids = reinterpret_cast<const uint32_t*>(pool + Offsets::SnapshotPool_Ids);
    return ids[0] == id || ids[1] == id || ids[2] == id;
}

int PoolSlotOfId(const uint8_t* pool, uint32_t id) {
    const auto* ids = reinterpret_cast<const uint32_t*>(pool + Offsets::SnapshotPool_Ids);
    for (int i = 0; i < 3; ++i) if (id != 0 && ids[i] == id) return i;
    return -1;
}

// Copies snapshot `id` (data + id) from src pool into the same slot of dst pool, if dst lacks it.
void MirrorSnapshotSlot(uint8_t* dst, const uint8_t* src, uint32_t id) {
    if (!dst || !src || dst == src || PoolHasId(dst, id)) return;
    const int slot = PoolSlotOfId(src, id);
    if (slot < 0) return;
    const size_t off = static_cast<size_t>(slot) * Offsets::MatchInfo_FoulSnapshotStride;
    std::memcpy(dst + off, src + off, Offsets::MatchInfo_FoulSnapshotStride);
    reinterpret_cast<uint32_t*>(dst + Offsets::SnapshotPool_Ids)[slot] = id;
}
} // namespace

// Returns a foul snapshot id that exists in the Judge pool (MatchFSM + 0x4B08), the pool the engine
// propagates to MatchControl and then over MatchInfo + 0x17AC at every CommitToRegistry. The same snapshot
// is mirrored into both MatchInfo buffers so a reader between our write and the next commit also finds it.
// When the Judge pool is empty a snapshot is added the way AddSnapshot (0x140A82070) does: next id from
// +0xBB8, slot from the ring index +0xBBC (wraps at 2), both advanced exactly like the engine.
uint32_t EnsureValidFoulSnapshotId(uint8_t* matchInfo, uint8_t* gmatch, uint8_t* fsm) {
    if (!IsValidPtr(matchInfo) || !IsValidPtr(fsm)) return 0;
    uint8_t* judgePool = fsm + Offsets::MatchFSM_JudgeSnapshotPool;
    uint8_t* miPool = matchInfo + Offsets::MatchInfo_FoulSnapshotPool;
    if (!IsMemoryRangeReadable(judgePool, Offsets::SnapshotPool_Span) ||
        !IsMemoryRangeReadable(miPool, Offsets::SnapshotPool_Span)) {
        return 0;
    }
    uint8_t* writeBuf = GetMatchInfoWriteBuffer(matchInfo);
    uint8_t* wbPool = writeBuf ? writeBuf + Offsets::MatchInfo_FoulSnapshotPool : nullptr;

    auto* jIds = reinterpret_cast<uint32_t*>(judgePool + Offsets::SnapshotPool_Ids);
    uint32_t id = 0;
    for (int i = 0; i < 3; ++i) {
        if (jIds[i] > id) id = jIds[i]; // most recent snapshot (ids only grow, wrap skips to 1)
    }

    if (id == 0) {
        auto* nextId = reinterpret_cast<uint32_t*>(judgePool + Offsets::SnapshotPool_NextId);
        auto* ring = reinterpret_cast<int32_t*>(judgePool + Offsets::SnapshotPool_RingIndex);
        const int32_t slot = (*ring >= 0 && *ring < 2) ? *ring : 0;
        id = (*nextId != 0 && *nextId != 0xFFFFFFFFU) ? *nextId : 1U;

        uint8_t* snap = judgePool + static_cast<size_t>(slot) * Offsets::MatchInfo_FoulSnapshotStride;
        std::memset(snap, 0, Offsets::MatchInfo_FoulSnapshotStride);
        if (IsValidPtr(gmatch)) {
            for (int s = 0; s < 22; ++s) {
                uint8_t* spatial = *reinterpret_cast<uint8_t**>(
                    gmatch + Offsets::GMATCH_PlayerSpatialBase + static_cast<size_t>(s) * Offsets::GMATCH_EntryStride);
                if (!IsValidPtr(spatial)) continue;
                Vec3 p{};
                std::memcpy(&p, spatial + Offsets::PlayerSpatial_PosVec3, sizeof(Vec3));
                uint8_t* entry = snap + static_cast<size_t>(s) * Offsets::MatchInfo_FoulSnapshotEntryStride;
                std::memcpy(entry + 0x18, &p.x, sizeof(float));
                std::memcpy(entry + 0x20, &p.z, sizeof(float));
            }
        }
        jIds[slot] = id;
        const uint32_t nid = id + 1U;
        *nextId = (nid == 0xFFFFFFFFU || nid == 0) ? 1U : nid;
        *ring = (slot + 1 >= 2) ? 0 : slot + 1;
        VAR_LOG_INF("Foul snapshot added to Judge pool: slot=%d id=%u", slot, id);
    }

    MirrorSnapshotSlot(miPool, judgePool, id);
    MirrorSnapshotSlot(wbPool, judgePool, id);
    return id;
}


// Fills SetPlayInfo + FoulInfo starting from the ENGINE defaults (0x140A3CB50 / 0x140A3CAE0), never from
// zeros: FoulInfo +0x14 / +0x18 are actor indices whose "none" sentinel is 0x18, +0x2C is 9, +0x30 is 4,
// SetPlayInfo +0x04 is 2 and +0x28 is 0xFF.
void WriteSetPlayAndFoulToStruct(uint8_t* baseSetPlay, uint8_t* baseFoul, const ExecRequest& req) {
    auto* sp = reinterpret_cast<RawSetPlayInfo*>(baseSetPlay);
    std::memset(sp, 0, sizeof(RawSetPlayInfo));
    sp->eventPlayerSlot = 0xFFU;
    sp->setPlayKind     = static_cast<int32_t>(req.spKind);
    sp->benefitedTeam   = (req.benefitedTeam == 1) ? 1 : 0;
    sp->posA            = req.spot;
    sp->posB            = req.spot;
    sp->stoppageReason  = static_cast<int32_t>(req.stopReason);
    if (req.foulValid && IsValidSlot(req.offenderSlot)) {
        sp->eventPlayerSlot = static_cast<uint32_t>(req.offenderSlot);
    }

    auto* fi = reinterpret_cast<RawFoulInfo*>(baseFoul);
    std::memset(fi, 0, sizeof(RawFoulInfo));
    fi->offenderSlot = 0xFF;
    fi->victimSlot   = 0xFF;
    uint8_t* raw = baseFoul;
    *reinterpret_cast<int32_t*>(raw + 0x14) = 0x18;
    *reinterpret_cast<int32_t*>(raw + 0x18) = 0x18;
    raw[0x2C] = 9;
    *reinterpret_cast<int32_t*>(raw + 0x30) = 4;
    if (req.foulValid) {
        fi->isValid           = 1;
        fi->foulKind          = static_cast<int32_t>(FoulKind::StandardFoul);
        fi->foulJudgementKind = (req.card == CardKind::Yellow) ? 3 : (req.card == CardKind::Red) ? 4 : 1;
        fi->offenderSlot      = IsValidSlot(req.offenderSlot) ? req.offenderSlot : 0xFF;
        fi->victimSlot        = IsValidSlot(req.victimSlot) ? req.victimSlot : 0xFF;
        fi->snapshotTickIndex = req.snapId;
    }
}

// Clears the referee's vanishing spray exactly like the engine does before every new set piece
// (0x140452EB0 without its +0x904 flag). Without it, re-placing a free kick while the old spray is still on
// the pitch draws a straight spray stroke from the old spot to the new one.
void ResetVanishingSpray(uint8_t* fsm) {
    if (!g_fnSprayRefGetWrite || !g_fnSprayReset || !g_fnSprayRefCommit || !IsValidPtr(fsm)) return;
    uint8_t* ref = fsm + Offsets::MatchFSM_VanishingSprayRef;
    if (!IsMemoryRangeReadable(ref, 0x28)) return;
    if (*reinterpret_cast<const uintptr_t*>(ref) != g_sprayRefVtable) return; // not the spray reference
    uint8_t* spray = g_fnSprayRefGetWrite(ref);
    if (spray) {
        g_fnSprayReset(spray);
    }
    g_fnSprayRefCommit(ref);
}

// Writes the request into the FSM JudgeResult (the engine consumes it on this tick) and mirrors it into both
// MatchInfo buffers for readers that run before the next commit.
void PerformInjection(uint8_t* fsm, uint8_t* gmatch, uint8_t* matchInfo) {
    ExecRequest& req = g_exec.req;
    if (req.foulValid) {
        // Re-resolve right before writing: the Judge pipeline of this tick may have rotated its pool
        if (const uint32_t id = EnsureValidFoulSnapshotId(matchInfo, gmatch, fsm)) req.snapId = id;
    }
    ResetVanishingSpray(fsm);
    WriteSetPlayAndFoulToStruct(matchInfo + Offsets::MatchInfo_SetPlayInfo, matchInfo + Offsets::MatchInfo_FoulInfo, req);
    if (uint8_t* writeBuf = GetMatchInfoWriteBuffer(matchInfo)) {
        WriteSetPlayAndFoulToStruct(writeBuf + Offsets::MatchInfo_SetPlayInfo, writeBuf + Offsets::MatchInfo_FoulInfo, req);
    }
    WriteSetPlayAndFoulToStruct(fsm + Offsets::MatchFSM_JudgeResult, fsm + Offsets::MatchFSM_JudgeFoulInfo, req);
    // Clear a stale quick restart and kicker cache (0x14045422E). EnterOutOfPlay then sets +0x27FC itself for
    // throw-ins and free kicks (0x1404541AA..0x1404541E9).
    *reinterpret_cast<uint32_t*>(fsm + Offsets::MatchFSM_QuickRestartFlag) = 0;
    *reinterpret_cast<uint64_t*>(fsm + Offsets::MatchFSM_KickerCache) = 0x000000FF000000FFULL;

    g_exec.expectOwnFoulCommit = req.foulValid;
    // FoulInfo holds 0xFF for "no offender" (WriteSetPlayAndFoulToStruct): compare with the engine's value
    g_exec.expectOffender = IsValidSlot(req.offenderSlot) ? req.offenderSlot : 0xFF;
    g_exec.expectSnapId = req.snapId;
    g_exec.injected = true;
    VAR_LOG_INF("VAR request injected (fsmState=0x%X): sp=%d reason=%d team=%d off=%d vic=%d card=%d snap=%u pos=(%.1f, %.1f)",
                g_exec.startFsmState, static_cast<int>(req.spKind), static_cast<int>(req.stopReason),
                req.benefitedTeam, req.offenderSlot, req.victimSlot, static_cast<int>(req.card),
                req.snapId, req.spot.x, req.spot.z);
}

// True when the injected foul's card is missing: the detour did not see our commit AND the offender's cards
// did not move since the injection (a commit the detour failed to recognise must never be doubled)
bool InjectedCardMissing(uint8_t* gmatch) {
    if (!g_exec.injected || !g_exec.req.foulValid || g_exec.ownFoulCommitted) return false;
    if (g_exec.req.card != CardKind::Yellow && g_exec.req.card != CardKind::Red) return false;
    if (!g_exec.baselineValid) return false;
    uint8_t y = 0, r = 0;
    if (!ReadCards(gmatch, g_exec.req.offenderSlot, g_exec.req.offenderRoster, y, r)) return false;
    return y == g_exec.baselineYellows && r == g_exec.baselineReds;
}

// Offender's real card counts after execution, so the timeline matches the game exactly
void StampLedgerCounts(VarVerdict& v) {
    // An edit of an older event keeps the estimate: today's totals are not "the counts after that play"
    if (!(v.fromLiveFoul || v.sourceEventId == 0)) return;
    // Cancels and restarts keep the event's offender: its counts are the ones to show
    const bool keepsOffender = v.action == VarAction::OverturnCancel || v.action == VarAction::RestartCorner ||
                               v.action == VarAction::RestartGoalKick || v.action == VarAction::RestartThrowIn;
    const bool useOff = IsValidSlot(v.offenderSlot) && !(keepsOffender && IsValidSlot(v.origOffenderSlot));
    const int32_t slot = useOff ? v.offenderSlot : v.origOffenderSlot;
    const int32_t roster = useOff ? v.offenderRosterIdx : v.origOffenderRosterIdx;
    if (!IsValidSlot(slot)) return;
    uint8_t y = 0, r = 0;
    if (ReadCards(GetGmatch(), slot, roster, y, r)) {
        v.ledgerYellows = static_cast<int8_t>(std::min<int>(y, 2));
        v.ledgerReds = static_cast<int8_t>(std::min<int>(r, 1));
    }
}

void ExecCommitLedger() {
    // Never record into a different match
    if (VarState::Instance().MatchGeneration() != g_exec.matchGen) return;
    if (g_exec.pushRevEvent) {
        // Card counts after the engine's own commit
        uint8_t ry = 0, rr = 0;
        if (ReadCards(GetGmatch(), g_exec.revEvent.offenderSlot, g_exec.revEvent.offenderRosterIdx, ry, rr)) {
            g_exec.revEvent.offenderYellowsAfter = ry;
            g_exec.revEvent.offenderRedsAfter    = rr;
        }
        VarState::Instance().PushEvent(g_exec.revEvent);
    }
    if (g_exec.hasPlayLedger) {
        StampLedgerCounts(g_exec.playVerdict);
        VarState::Instance().UpdateEventAfterVerdict(g_exec.playVerdict);
    }
    for (size_t i = 0; i < g_exec.cardLedgerCount; ++i) {
        StampLedgerCounts(g_exec.cardLedger[i]);
        VarState::Instance().UpdateEventAfterVerdict(g_exec.cardLedger[i]);
    }
    VarState::Instance().IncrementInterventions();
}

void ExecFinish(bool applied, const char* failBannerPt) {
    if (applied) {
        if (g_exec.redInvolved) g_exec.bannerStyle = BannerStyle::RedCard;
        ExecCommitLedger();
        VarState::Instance().SetPostApplyBanner(
            g_exec.bannerText[0] ? g_exec.bannerText : "DECISÃO DO VAR APLICADA", 4800, g_exec.bannerStyle);
        VAR_LOG_INF("VAR pool seq=%u applied and verified", g_exec.poolSeq);
    } else {
        // The injected foul was written but the engine never committed it (the kick was taken on that tick):
        // its card was not applied while the original card was already revoked. Apply it by hand so the
        // "cards were applied" outcome is complete and the offender is never left one card short.
        if (VarState::Instance().MatchGeneration() == g_exec.matchGen && InjectedCardMissing(GetGmatch())) {
            ApplyCardDelta(GetGmatch(), g_exec.req.offenderSlot, CardKind::None, g_exec.req.card, g_exec.req.offenderRoster);
            g_exec.cardsApplied = true;
            g_exec.playManualCard = g_exec.req.card;
            VAR_LOG_WRN("Injected foul not committed by the engine: card %d applied manually to slot %d",
                        static_cast<int>(g_exec.req.card), g_exec.req.offenderSlot);
        }
        // Cards already written stay written: record them so the ledger matches the match. The play itself
        // (and a "reviewed by VAR" incident) is not recorded because the engine never ran it.
        const bool cardKindGiven = (g_exec.req.card == CardKind::Yellow || g_exec.req.card == CardKind::Red);
        bool engineGave = g_exec.ownFoulCommitted && g_exec.req.foulValid && cardKindGiven;
        if (!engineGave && g_exec.injected && g_exec.req.foulValid && cardKindGiven && g_exec.baselineValid &&
            g_exec.playManualCard == CardKind::None) {
            // A commit the detour did not recognise still moves the offender's cards
            uint8_t y = 0, r = 0;
            if (ReadCards(GetGmatch(), g_exec.req.offenderSlot, g_exec.req.offenderRoster, y, r) &&
                (y != g_exec.baselineYellows || r != g_exec.baselineReds)) {
                engineGave = true;
            }
        }
        const CardKind given = engineGave ? g_exec.req.card
                             : (g_exec.playManualCard == CardKind::Yellow || g_exec.playManualCard == CardKind::Red)
                                   ? g_exec.playManualCard : CardKind::None;
        if ((g_exec.cardsApplied || given != CardKind::None) && VarState::Instance().MatchGeneration() == g_exec.matchGen) {
            // The play never ran, but its card part did: record it as a card change of its event
            if (g_exec.hasPlayLedger && g_exec.playVerdict.sourceEventId != 0 &&
                (g_exec.playOrigRevoked || given != CardKind::None)) {
                VarVerdict u = g_exec.playVerdict;
                u.action = VarAction::EditCard;
                u.targetCard = given;
                if (given == CardKind::None) {
                    // Only the revocation happened: the event keeps its original player
                    u.offenderSlot = IsValidSlot(u.origOffenderSlot) ? u.origOffenderSlot : u.offenderSlot;
                    u.offenderRosterIdx = IsValidSlot(u.origOffenderSlot) ? u.origOffenderRosterIdx : u.offenderRosterIdx;
                }
                StampLedgerCounts(u);
                VarState::Instance().UpdateEventAfterVerdict(u);
            }
            if (g_exec.pushRevEvent && given != CardKind::None) {
                g_exec.revEvent.cardKind = given;
                uint8_t ry = 0, rr = 0;
                if (ReadCards(GetGmatch(), g_exec.revEvent.offenderSlot, g_exec.revEvent.offenderRosterIdx, ry, rr)) {
                    g_exec.revEvent.offenderYellowsAfter = ry;
                    g_exec.revEvent.offenderRedsAfter = rr;
                }
                VarState::Instance().PushEvent(g_exec.revEvent);
            }
            for (size_t i = 0; i < g_exec.cardLedgerCount; ++i) {
                StampLedgerCounts(g_exec.cardLedger[i]);
                VarState::Instance().UpdateEventAfterVerdict(g_exec.cardLedger[i]);
            }
            VarState::Instance().IncrementInterventions();
        }
        VarState::Instance().SetPostApplyBanner(failBannerPt, 4800, BannerStyle::Amber);
        VAR_LOG_WRN("VAR pool seq=%u not applied: %s", g_exec.poolSeq, failBannerPt);
    }
    SyncPlayersAndCards(GetGmatch(), GetMatchInfo(GetGmatch()));
    g_exec.req = ExecRequest{};
    ExecEnter(ExecState::Cooldown, kExecCooldownTicks);
}

// Conservative check: could applying this card-only pool send a player off or bring one back?
// (red involved, second yellow, or a player who is currently off the pitch)
bool PoolMayChangeOnPitch(const VerdictPool& pool, uint8_t* gmatch, uint8_t* matchInfo) {
    auto offPitch = [&](int32_t slot) {
        return IsValidSlot(slot) && *reinterpret_cast<const uint8_t*>(matchInfo + Offsets::MatchInfo_PlayerOnPitch + slot) == 0;
    };
    auto yellowsOf = [&](int32_t slot, int32_t roster) -> uint8_t {
        uint8_t y = 0, r = 0;
        ReadCards(gmatch, slot, roster, y, r);
        return y;
    };
    for (const auto& e : pool.cardEntries) {
        if (!e.active) continue;
        const VarVerdict& v = e.verdict;
        if (offPitch(v.offenderSlot) || offPitch(v.origOffenderSlot)) return true;
        if (v.directCardEdit) {
            if (v.directReds > 0 || v.directYellows >= 2) return true;
        } else {
            if (v.targetCard == CardKind::Red || v.origCard == CardKind::Red) return true;
            if (v.targetCard == CardKind::Yellow && yellowsOf(v.offenderSlot, v.offenderRosterIdx) >= 1) return true;
            if (v.origCard == CardKind::Yellow &&
                (IsValidSlot(v.origOffenderSlot) ? yellowsOf(v.origOffenderSlot, v.origOffenderRosterIdx)
                                                 : yellowsOf(v.offenderSlot, v.offenderRosterIdx)) >= 2) return true;
        }
    }
    return false;
}

struct ConsumeGateCtx {
    bool     inPlay;
    uint8_t* gmatch;
    uint8_t* matchInfo;
};

// Would the play of this pool run the engine's roster states (0x12 / 0x13) when executed with the ball in play?
// Restarts use SilentRestart builders (0x14045333C, all queue 0x12 / 0x13). Penalties and carded fouls are never
// quick restarts (0x140445740) and go through 0x140452F00 (queues 0x12 / 0x13). A card-free free kick may become a
// quick free kick (state 0x18) that skips them.
bool PlayQueuesRosterStatesInPlay(const VerdictPool& pool) {
    if (!pool.hasPlayDecision) return false;
    const VarVerdict& pv = pool.playDecision;
    switch (pv.action) {
        case VarAction::RestartCorner:
        case VarAction::RestartGoalKick:
        case VarAction::RestartThrowIn:
        case VarAction::ConvertToPenalty:
        case VarAction::RetroactivePenalty:
            return true;
        case VarAction::ConvertToFreeKick:
        case VarAction::RetroactiveFreeKick:
            return pv.targetCard == CardKind::Yellow || pv.targetCard == CardKind::Red;
        default:
            return false;
    }
}

// Runs under the VarState lock, so no HUD edit can slip in between the check and the take
bool ConsumeGate(const VerdictPool& pool, void* user) {
    const auto* g = static_cast<const ConsumeGateCtx*>(user);
    if (!pool.HasExecutableItems()) return false;
    if (!g->inPlay) {
        // SetPlayReady of a kick-off (half start, after a goal): the pool waits for a real stoppage
        const auto* sp = reinterpret_cast<const RawSetPlayInfo*>(g->matchInfo + Offsets::MatchInfo_SetPlayInfo);
        return sp->setPlayKind != static_cast<int32_t>(SetPlayKind::KickOff);
    }
    // With the ball in play, a pool that changes who is on the pitch only runs when its own play makes the engine
    // run the roster states; otherwise it waits for the next stoppage (the engine never removes / restores an
    // actor with the ball in play).
    bool changesRoster = PoolMayChangeOnPitch(pool, g->gmatch, g->matchInfo);
    if (pool.hasPlayDecision) {
        const VarVerdict& pv = pool.playDecision;
        const int32_t origSlot = IsValidSlot(pv.origOffenderSlot) ? pv.origOffenderSlot : pv.offenderSlot;
        if ((pv.origCard == CardKind::Red || pv.origCard == CardKind::Yellow) && IsValidSlot(origSlot) &&
            *reinterpret_cast<const uint8_t*>(g->matchInfo + Offsets::MatchInfo_PlayerOnPitch + origSlot) == 0) {
            changesRoster = true; // revoking the card that sent the original offender off
        }
    }
    if (!changesRoster) return true;
    return PlayQueuesRosterStatesInPlay(pool);
}

// Manual card writes of the plan, in chronological order: the revocation of the card the engine already counted
// for the original call (no native decrement exists) and every card-only edit. Absorbed edits are skipped (the
// engine applies their card through the injected foul).
void ApplyPlanCards(uint8_t* gmatch) {
    if (g_exec.cardsPlanApplied) return;
    g_exec.cardsPlanApplied = true;
    const VerdictPool& pool = g_exec.pool;
    const VarVerdict* pv = pool.hasPlayDecision ? &pool.playDecision : nullptr;

    struct PoolStep { bool isPlay; uint32_t order; int entry; };
    std::array<PoolStep, kMaxCardPoolEntries + 1> steps{};
    size_t stepCount = 0;
    if (pv) steps[stepCount++] = PoolStep{ true, pool.playOrder, -1 };
    for (int s = 0; s < static_cast<int>(kMaxCardPoolEntries); ++s) {
        if (pool.cardEntries[static_cast<size_t>(s)].active) {
            steps[stepCount++] = PoolStep{ false, pool.cardEntries[static_cast<size_t>(s)].order, s };
        }
    }
    std::sort(steps.begin(), steps.begin() + stepCount,
              [](const PoolStep& a, const PoolStep& b) { return a.order < b.order; });

    for (size_t i = 0; i < stepCount; ++i) {
        if (steps[i].isPlay) {
            if (pv->origCard == CardKind::Yellow || pv->origCard == CardKind::Red) {
                const bool hasOrig = IsValidSlot(pv->origOffenderSlot);
                const int32_t origSlot = hasOrig ? pv->origOffenderSlot : pv->offenderSlot;
                const int32_t origRoster = hasOrig ? pv->origOffenderRosterIdx : pv->offenderRosterIdx;
                uint8_t oy = 0, orr = 0;
                if (ReadCards(gmatch, origSlot, origRoster, oy, orr)) {
                    // Only a player still holding his slot can come back on through this revocation
                    if (SlotHeldBy(gmatch, origSlot, origRoster)) {
                        g_exec.revokedSlot = origSlot;
                        g_exec.revokedWasSentOff = (orr > 0 || oy >= 2);
                    }
                }
                g_exec.sentOffChanged |= ApplyCardDelta(gmatch, origSlot, pv->origCard, CardKind::None, origRoster);
                g_exec.playOrigRevoked = true;
                g_exec.cardsApplied = true;
            }
            continue;
        }
        const size_t e = static_cast<size_t>(steps[i].entry);
        const VarVerdict& cv = pool.cardEntries[e].verdict;
        if (g_exec.cardLedgerCount < g_exec.cardLedger.size()) g_exec.cardLedger[g_exec.cardLedgerCount++] = cv;
        if (g_exec.absorbed[e]) continue;
        g_exec.sentOffChanged |= ApplyVerdictCards(gmatch, cv);
        g_exec.cardsApplied = true;
        if (cv.targetCard == CardKind::Red || (cv.directCardEdit && cv.directReds > 0)) g_exec.redInvolved = true;
    }
}

// History entry of a foul the VAR created on a play without an official event
void FillRevEvent(uint8_t* gmatch, uint8_t* matchInfo, int32_t offenderSlot, int32_t offenderRoster, CardKind card) {
    const ExecRequest& req = g_exec.req;
    VarEvent& ev = g_exec.revEvent;
    ev = VarEvent{};
    ev.type          = (req.spKind == SetPlayKind::PenaltyKick) ? VarEventType::Penalty : VarEventType::Foul;
    ev.matchPhase    = *reinterpret_cast<const int32_t*>(matchInfo + Offsets::MatchInfo_MatchPhase);
    ev.time60Hz      = GetMatchTime60Hz(matchInfo);
    ev.matchTick     = *reinterpret_cast<const uint32_t*>(matchInfo + Offsets::MatchInfo_MatchTick);
    ev.setPlayKind   = req.spKind;
    ev.benefitedTeam = req.benefitedTeam;
    ev.offenderSlot  = offenderSlot;
    ev.victimSlot    = req.victimSlot;
    ev.offenderRosterIdx = static_cast<int8_t>(offenderRoster >= 0 ? offenderRoster
                                                                   : ReadActiveRosterIndex(gmatch, offenderSlot));
    ev.victimRosterIdx = static_cast<int8_t>(req.victimRoster >= 0 ? req.victimRoster
                                                                  : ReadActiveRosterIndex(gmatch, req.victimSlot));
    ev.cardKind      = card;
    ev.pos           = req.spot;
    ev.reviewedByVar = true;
    {
        const PitchDims bd = ReadPitchDims(gmatch);
        const int32_t bTeam = IsValidSlot(offenderSlot) ? SlotToTeam(offenderSlot) : (1 - req.benefitedTeam);
        ev.boxDistM = PitchGeometry::SignedDistanceToDefendingBox(bd, ev.pos, ReadTeamAttackDir(gmatch, bTeam));
        ev.boxDistValid = true;
    }
    std::snprintf(ev.varNote, sizeof(ev.varNote), "%s",
                  (req.spKind == SetPlayKind::PenaltyKick) ? "Pênalti pelo VAR" : "Falta pelo VAR");
    g_exec.pushRevEvent = true;
}

// Card a foul of this pool may still give to `slot`: none after a later direct edit of the same player
// (absolute value wins) or when he is already sent off (no third yellow / second red)
CardKind EffectivePlayCard(uint8_t* gmatch, int32_t slot, int32_t roster) {
    CardKind card = g_exec.playCard;
    if (g_exec.laterDirectOnOffender) card = CardKind::Simple;
    uint8_t y = 0, r = 0;
    if (ReadCards(gmatch, slot, roster, y, r)) {
        // No third yellow / second red. A yellow for a player with only a straight red is legal (1 Y + 1 R).
        if (card == CardKind::Yellow && (y >= 2 || (y >= 1 && r > 0))) card = CardKind::Simple;
        if (card == CardKind::Red && r > 0) card = CardKind::Simple;
    }
    return card;
}

// Final shape of an injected foul, decided after the manual writes of the same tick
void FinalizeInjectedFoul(uint8_t* fsm, uint8_t* gmatch, uint8_t* matchInfo) {
    ExecRequest& req = g_exec.req;
    // The slot must still be held by the player of the incident (a substitute is never the offender or victim)
    auto wasOnPitch = [&](int32_t slot, int32_t roster) {
        return IsValidSlot(slot) && g_exec.onPitchBefore[slot] != 0 && SlotHeldBy(gmatch, slot, roster);
    };
    // A player off the pitch before this pool cannot be in the foul, unless he was off only because of the card
    // this pool just revoked (convert a red-card foul into a penalty against the same player).
    auto eligible = [&](int32_t slot, int32_t roster) {
        return wasOnPitch(slot, roster) ||
               (slot == g_exec.revokedSlot && g_exec.revokedWasSentOff && SlotHeldBy(gmatch, slot, roster));
    };
    if (IsValidSlot(req.victimSlot) && !wasOnPitch(req.victimSlot, req.victimRoster)) req.victimSlot = -1;
    if (req.foulValid && IsValidSlot(req.offenderSlot) && !eligible(req.offenderSlot, req.offenderRoster)) {
        VAR_LOG_WRN("Play offender slot %d (roster %d) is not on the pitch: restart kept, foul data dropped",
                    req.offenderSlot, req.offenderRoster);
        req.foulValid = false;
        // Absorbed edits are already folded into playCard and the offender: giving them separately too
        // would card the player twice
        for (auto& a : g_exec.absorbed) a = false;
        // Same card rules as an injected foul, given by hand because the engine will not commit a foul
        const CardKind dc = EffectivePlayCard(gmatch, req.offenderSlot, req.offenderRoster);
        if (g_exec.hasPlayLedger) g_exec.playVerdict.targetCard = dc;
        if (dc == CardKind::Yellow) {
            uint8_t py = 0, pr = 0;
            if (ReadCards(gmatch, req.offenderSlot, req.offenderRoster, py, pr) && py >= 1) g_exec.redInvolved = true;
        }
        if (dc == CardKind::Yellow || dc == CardKind::Red) {
            g_exec.sentOffChanged |= ApplyCardDelta(gmatch, req.offenderSlot, CardKind::None, dc, req.offenderRoster);
            g_exec.cardsApplied = true;
            g_exec.playManualCard = dc;
            if (dc == CardKind::Red) g_exec.redInvolved = true;
            if (g_exec.pool.playDecision.sourceEventId == 0) {
                FillRevEvent(gmatch, matchInfo, req.offenderSlot, req.offenderRoster, dc);
            }
        }
        req.offenderSlot = -1;
    }
    if (!req.foulValid) {
        // Open play with no foul left to record: reason 5 would queue the card cutscene of an older foul and an
        // automatic replay of nothing. Reason 7 restarts silently and always runs states 0x12 / 0x13.
        if (req.stopReason == StoppageReason::Foul) req.stopReason = StoppageReason::SilentRestart;
        return;
    }

    // The engine applies this card on the injection tick (CommitToRegistry). No card when a later direct edit
    // already set the absolute value, or when the offender is already sent off by an edit of this pool.
    const CardKind card = EffectivePlayCard(gmatch, req.offenderSlot, req.offenderRoster);
    req.card = card;
    if (g_exec.hasPlayLedger) g_exec.playVerdict.targetCard = card; // the history shows the card really given
    if (card == CardKind::Yellow) {
        uint8_t py = 0, pr = 0;
        if (ReadCards(gmatch, req.offenderSlot, req.offenderRoster, py, pr) && py >= 1) g_exec.redInvolved = true; // second yellow
    }
    // A foul without a card in open play may become a quick free kick (0x140445740), which skips states
    // 0x12 / 0x13: a player this pool sent off would stay on the pitch. Restart silently instead.
    if (req.stopReason == StoppageReason::Foul && card != CardKind::Yellow && card != CardKind::Red &&
        g_exec.sentOffChanged) {
        req.stopReason = StoppageReason::SilentRestart;
    }
    if (card == CardKind::Red) g_exec.redInvolved = true;
    if (const uint32_t id = EnsureValidFoulSnapshotId(matchInfo, gmatch, fsm)) req.snapId = id;

    if (g_exec.pool.playDecision.sourceEventId == 0) {
        FillRevEvent(gmatch, matchInfo, req.offenderSlot, req.offenderRoster, req.card);
    }
}

// Performs the pending request. The manual card writes happen HERE, in the same tick the engine runs the
// decision, so a request that never fires leaves no trace. Called from Judge::FinalizeFrame (inside the 0x1A /
// 0x1C handler, which then calls EnterOutOfPlay itself) or, as a fallback, from the MainTick prologue.
bool ExecPerform(uint8_t* fsm, uint8_t* gmatch, uint8_t* matchInfo, bool fromFallback) {
    if (g_exec.req.kind == ExecRequestKind::InjectSetPlay) {
        if (fromFallback && !g_fnEnterOutOfPlay) return false;
        // In SetPlayReady the handler can still leave without EnterOutOfPlay after FinalizeFrame (substitution /
        // menu exits 0x14044DEA5, 0x14044E167). Everything written this tick is then undone after MainTick.
        if (!fromFallback && g_exec.startFsmState == 0x1A) {
            g_exec.verifyAfterTick = true;
            g_exec.undoActive = true;
            g_exec.cardUndoCount = 0;
            g_exec.cardUndoOverflow = false;
            std::memcpy(g_exec.savedJudge, fsm + Offsets::MatchFSM_JudgeResult, sizeof(g_exec.savedJudge));
            std::memcpy(g_exec.savedMiSetPlay, matchInfo + Offsets::MatchInfo_SetPlayInfo, sizeof(g_exec.savedMiSetPlay));
            std::memcpy(g_exec.savedMiFoul, matchInfo + Offsets::MatchInfo_FoulInfo, sizeof(g_exec.savedMiFoul));
            uint8_t* wb = GetMatchInfoWriteBuffer(matchInfo);
            g_exec.savedWbValid = (wb != nullptr);
            if (wb) {
                std::memcpy(g_exec.savedWbSetPlay, wb + Offsets::MatchInfo_SetPlayInfo, sizeof(g_exec.savedWbSetPlay));
                std::memcpy(g_exec.savedWbFoul, wb + Offsets::MatchInfo_FoulInfo, sizeof(g_exec.savedWbFoul));
            }
            g_exec.savedQuickRestart = *reinterpret_cast<const uint32_t*>(fsm + Offsets::MatchFSM_QuickRestartFlag);
            g_exec.savedKickerCache = *reinterpret_cast<const uint64_t*>(fsm + Offsets::MatchFSM_KickerCache);
        }
        ApplyPlanCards(gmatch);
        FinalizeInjectedFoul(fsm, gmatch, matchInfo);
        g_exec.undoActive = false;
        if (g_exec.req.foulValid) {
            g_exec.baselineValid = ReadCards(gmatch, g_exec.req.offenderSlot, g_exec.req.offenderRoster,
                                             g_exec.baselineYellows, g_exec.baselineReds);
        }
        PerformInjection(fsm, gmatch, matchInfo);
        if (fromFallback) {
            // EnterOutOfPlay decides the quick restart itself (0x1404541AA..0x1404541E9), as in the
            // Judge::FinalizeFrame path: never override it afterwards
            g_fnEnterOutOfPlay(fsm);
        }
        return true;
    }
    if (g_exec.req.kind == ExecRequestKind::RebuildSetPlay) {
        if (!g_fnRebuildSetPlay) return false;
        ResetVanishingSpray(fsm);
        g_fnRebuildSetPlay(fsm);
        VAR_LOG_INF("VAR requested native RebuildSetPlay (fsmState=0x%X)", g_exec.startFsmState);
        return true;
    }
    return false;
}

// Idle -> AwaitNativeHook / AwaitTransition (or straight to Cooldown when the engine has nothing to run)
void ExecStartPool(uint8_t* fsm, uint8_t* gmatch, uint8_t* matchInfo, int32_t fsmState) {
    const bool inPlay = (fsmState == 0x1C);
    const bool inSetPlayReady = (fsmState == 0x1A);

    ConsumeGateCtx gate{ inPlay, gmatch, matchInfo };
    VerdictPool consumed{};
    const uint32_t genBefore = VarState::Instance().MatchGeneration();
    if (!VarState::Instance().ConsumeVerdictPoolIf(&ConsumeGate, &gate, consumed)) {
        if (inPlay && VarState::Instance().HasArmedVerdict() && !g_loggedDeferredOnPitch) {
            g_loggedDeferredOnPitch = true;
            VAR_LOG_INF("Pool changes who is on the pitch: waiting for the next stoppage");
        }
        return;
    }
    g_loggedDeferredOnPitch = false;

    if (!GetTeamInfoWriteBuffer(GetTeamInfo(gmatch, 0), 0) || !GetTeamInfoWriteBuffer(GetTeamInfo(gmatch, 1), 1)) {
        // Checked before the consume too; if it changed in between, the pool goes back untouched
        VarState::Instance().RequeueVerdictPool(consumed, genBefore);
        return;
    }
    if (VarState::Instance().MatchGeneration() != genBefore) {
        // A match reset raced with the consume: this pool belongs to the previous match
        VAR_LOG_WRN("VAR pool seq=%u dropped: match was reset while it was taken", consumed.sequence);
        return;
    }
    ExecResetToIdle();
    g_exec.pool = consumed;
    // "-1 = whoever holds the slot now" is resolved here, at execution: from now on every write and the
    // ledger (which updates the event history) refer to that exact player even after a substitution.
    {
        auto resolve = [&](VarVerdict& v) {
            if (v.offenderRosterIdx < 0 && IsValidSlot(v.offenderSlot))
                v.offenderRosterIdx = static_cast<int8_t>(ReadActiveRosterIndex(gmatch, v.offenderSlot));
            if (v.victimRosterIdx < 0 && IsValidSlot(v.victimSlot))
                v.victimRosterIdx = static_cast<int8_t>(ReadActiveRosterIndex(gmatch, v.victimSlot));
            if (v.origOffenderRosterIdx < 0 && IsValidSlot(v.origOffenderSlot))
                v.origOffenderRosterIdx = static_cast<int8_t>(ReadActiveRosterIndex(gmatch, v.origOffenderSlot));
        };
        if (g_exec.pool.hasPlayDecision) resolve(g_exec.pool.playDecision);
        for (auto& e : g_exec.pool.cardEntries) {
            if (e.active) resolve(e.verdict);
        }
    }
    g_exec.matchGen = genBefore;
    g_exec.poolSeq = consumed.sequence;
    g_exec.fsm = fsm;
    g_exec.startFsmState = fsmState;
    consumed.FormatCombinedSummary(g_exec.bannerText, sizeof(g_exec.bannerText));
    std::memcpy(g_exec.onPitchBefore, matchInfo + Offsets::MatchInfo_PlayerOnPitch, sizeof(g_exec.onPitchBefore));

    VAR_LOG_INF("Executing VAR pool seq=%u count=%zu hasPlay=%d cards=%zu fsmState=0x%X",
                consumed.sequence, consumed.Count(), consumed.hasPlayDecision ? 1 : 0, consumed.CardCount(), fsmState);

    const VarVerdict* pv = consumed.hasPlayDecision ? &g_exec.pool.playDecision : nullptr;
    ExecRequest& req = g_exec.req;
    if (pv) {
        const PitchDims dims = ReadPitchDims(gmatch);
        // For an overturn the HUD gives the restart to the original offender's side, so that IS his team
        const int32_t offenderTeam = IsValidSlot(pv->offenderSlot) ? SlotToTeam(pv->offenderSlot)
                                   : (pv->action == VarAction::OverturnCancel) ? pv->benefitedTeam
                                   : (1 - pv->benefitedTeam);
        const float offenderAttackDir = ReadTeamAttackDir(gmatch, offenderTeam);
        req.benefitedTeam = (pv->benefitedTeam == 1) ? 1 : 0;
        req.offenderSlot = pv->offenderSlot;
        req.victimSlot = pv->victimSlot;
        req.offenderRoster = pv->offenderRosterIdx;
        req.victimRoster = pv->victimRosterIdx;
        req.stopReason = StoppageReason::SilentRestart;

        switch (pv->action) {
            case VarAction::ConvertToPenalty:
            case VarAction::RetroactivePenalty:
                req.kind = ExecRequestKind::InjectSetPlay;
                req.spKind = SetPlayKind::PenaltyKick;
                req.spot = PitchGeometry::CalcPenaltySpot(dims, offenderAttackDir);
                req.stopReason = inPlay ? StoppageReason::Foul : StoppageReason::SilentRestart;
                req.foulValid = true;
                break;
            case VarAction::ConvertToFreeKick:
            case VarAction::RetroactiveFreeKick:
                req.kind = ExecRequestKind::InjectSetPlay;
                req.spKind = SetPlayKind::FreeKick;
                req.spot = PitchGeometry::ProjectOutsideDefendingBox(dims, pv->spotPos, offenderAttackDir, 0.7f);
                req.stopReason = inPlay ? StoppageReason::Foul : StoppageReason::SilentRestart;
                req.foulValid = true;
                break;
            case VarAction::OverturnCancel: {
                // A restart only for the foul the match is stopped for. Overturning an older (already taken) call
                // only revokes its card: replacing the pending set play with an old spot would be wrong.
                // The live set play must still be the one the cancel was made for (same offender and set play kind):
                // a cancel deferred to a later stoppage must never replace that stoppage's set play
                bool sameStoppage = inSetPlayReady && pv->targetsLiveSetPlay;
                if (sameStoppage) {
                    const auto curFi = *reinterpret_cast<const RawFoulInfo*>(matchInfo + Offsets::MatchInfo_FoulInfo);
                    const auto curSp0 = *reinterpret_cast<const RawSetPlayInfo*>(matchInfo + Offsets::MatchInfo_SetPlayInfo);
                    const int32_t wantOff = IsValidSlot(pv->origOffenderSlot) ? pv->origOffenderSlot : pv->offenderSlot;
                    if (!curFi.isValid || (IsValidSlot(wantOff) && curFi.offenderSlot != wantOff)) sameStoppage = false;
                    if (pv->liveSetPlayKind >= 0 && curSp0.setPlayKind != pv->liveSetPlayKind) sameStoppage = false;
                    if (!sameStoppage) VAR_LOG_WRN("Cancel of the stopped-for foul is stale: only its card is revoked");
                }
                if (sameStoppage) {
                    const auto curSp = *reinterpret_cast<const RawSetPlayInfo*>(matchInfo + Offsets::MatchInfo_SetPlayInfo);
                    const bool wasPenalty = (curSp.setPlayKind == static_cast<int32_t>(SetPlayKind::PenaltyKick)) ||
                                            PitchGeometry::IsInsideDefendingBox(dims, pv->spotPos, offenderAttackDir);
                    req.kind = ExecRequestKind::InjectSetPlay;
                    req.spKind = wasPenalty ? SetPlayKind::GoalKick : SetPlayKind::FreeKick;
                    req.spot = wasPenalty
                        ? PitchGeometry::CalcGoalKickSpot(dims, ReadTeamAttackDir(gmatch, req.benefitedTeam), pv->spotPos.z)
                        : PitchGeometry::ClampToPitch(dims, pv->spotPos);
                }
                break;
            }
            case VarAction::RestartCorner:
                req.kind = ExecRequestKind::InjectSetPlay;
                req.spKind = SetPlayKind::CornerKick;
                req.spot = PitchGeometry::CalcCornerSpot(dims, ReadTeamAttackDir(gmatch, 1 - req.benefitedTeam), pv->spotPos.z);
                break;
            case VarAction::RestartGoalKick:
                req.kind = ExecRequestKind::InjectSetPlay;
                req.spKind = SetPlayKind::GoalKick;
                req.spot = PitchGeometry::CalcGoalKickSpot(dims, ReadTeamAttackDir(gmatch, req.benefitedTeam), pv->spotPos.z);
                break;
            case VarAction::RestartThrowIn:
                req.kind = ExecRequestKind::InjectSetPlay;
                req.spKind = SetPlayKind::ThrowIn;
                req.spot = PitchGeometry::CalcThrowInSpot(dims, pv->spotPos);
                break;
            default:
                break;
        }
        if (!req.foulValid) {
            req.offenderSlot = -1;
            req.victimSlot = -1;
        }
        // SilentRestart builders exist only for kinds 1..6 (0x14045333C): any other kind would build no queue
        if (req.kind == ExecRequestKind::InjectSetPlay &&
            (static_cast<int32_t>(req.spKind) < 1 || static_cast<int32_t>(req.spKind) > 6)) {
            VAR_LOG_ERR("VAR request with invalid set play kind %d dropped", static_cast<int32_t>(req.spKind));
            req.kind = ExecRequestKind::None;
        }
        if (pv->action == VarAction::OverturnCancel && req.kind == ExecRequestKind::None) {
            std::snprintf(g_exec.bannerText, sizeof(g_exec.bannerText), "%s",
                          (pv->origCard == CardKind::Yellow || pv->origCard == CardKind::Red)
                              ? "ANULAÇÃO REGISTRADA  ·  A cobrança já tinha sido feita, o cartão foi retirado"
                              : "ANULAÇÃO REGISTRADA  ·  A cobrança já tinha sido feita");
        }
        g_exec.hasPlayLedger = true;
        g_exec.playVerdict = *pv;

        // Card of the injected foul: the play's own target, overridden by a LATER edit of the same event (absorbed:
        // the engine applies the final card once, to the player that edit chose) or by a LATER direct edit of the
        // offender (absolute value wins: the engine then applies no card). Only for a foul that is injected.
        g_exec.playCard = pv->targetCard;
        if (req.kind == ExecRequestKind::InjectSetPlay && req.foulValid) {
            for (size_t i = 0; i < kMaxCardPoolEntries; ++i) {
                const auto& ce = consumed.cardEntries[i];
                if (!ce.active || ce.order <= consumed.playOrder) continue;
                if (!ce.verdict.directCardEdit && pv->sourceEventId != 0 && ce.verdict.sourceEventId == pv->sourceEventId) {
                    g_exec.playCard = ce.verdict.targetCard;
                    g_exec.absorbed[i] = true;
                    if (IsValidSlot(ce.verdict.offenderSlot)) {
                        req.offenderSlot = ce.verdict.offenderSlot;
                        req.offenderRoster = ce.verdict.offenderRosterIdx;
                    }
                } else if (ce.verdict.directCardEdit && ce.verdict.offenderSlot == req.offenderSlot &&
                           (ce.verdict.offenderRosterIdx < 0 || req.offenderRoster < 0 ||
                            ce.verdict.offenderRosterIdx == req.offenderRoster)) {
                    g_exec.laterDirectOnOffender = true;
                }
            }
        }
    }

    const bool isOverturnOrRestart = pv &&
        (pv->action == VarAction::OverturnCancel || pv->action == VarAction::RestartCorner ||
         pv->action == VarAction::RestartGoalKick || pv->action == VarAction::RestartThrowIn);

    if (req.kind == ExecRequestKind::InjectSetPlay) {
        // Cards are written together with the injection (ExecPerform), never before the engine runs it
        g_exec.bannerStyle = isOverturnOrRestart ? BannerStyle::Amber : BannerStyle::Applied;
        ExecEnter(ExecState::AwaitNativeHook, kExecHookTimeoutTicks);
        return;
    }

    // No set play to inject: the manual writes are the decision itself
    ApplyPlanCards(gmatch);
    g_exec.bannerStyle = g_exec.redInvolved ? BannerStyle::RedCard
                       : isOverturnOrRestart ? BannerStyle::Amber
                       : BannerStyle::Applied;
    if (g_exec.sentOffChanged && inSetPlayReady) {
        // Card-only change of who is on the pitch: native rebuild of the same set play (states 0x12 / 0x13),
        // before the dispatcher runs so the rebuilt queue starts from its first state
        req.kind = ExecRequestKind::RebuildSetPlay;
        if (ExecPerform(fsm, gmatch, matchInfo, true)) {
            ExecEnter(ExecState::AwaitTransition, kExecTransitionTimeoutTicks);
        } else {
            ExecFinish(false, "REINÍCIO NÃO APLICADO  ·  Os cartões foram aplicados, o lance já mudou");
        }
        return;
    }
    ExecFinish(true, nullptr);
}

// Right after the original MainTick of the injection tick, before CommitToRegistry: the SetPlayReady handler must
// have called EnterOutOfPlay (queue reset reason 1). Otherwise every write of this tick is undone (cards, MatchInfo
// set play / foul in both buffers, FSM fields, JudgeResult) and the pool goes back to wait: no half-applied
// set play, no card without its decision. The JudgeResult only reaches the commit through EnterOutOfPlay.
void ExecVerifyAfterMainTick(uint8_t* fsm) {
    if (!g_exec.verifyAfterTick) return;
    g_exec.verifyAfterTick = false;
    if (fsm != g_exec.fsm || !IsValidPtr(fsm)) return;
    const int32_t reason = *reinterpret_cast<const int32_t*>(fsm + Offsets::MatchFSM_QueueResetReason);
    if (reason == 1) return; // EnterOutOfPlay ran: the decision goes on to the native commit
    uint8_t* gmatch = GetGmatch();
    uint8_t* matchInfo = GetMatchInfo(gmatch);
    // A partial card undo followed by a retry would card some players twice: if the log overflowed, the cards
    // stay as written and the pool is not retried (the set play data is still restored below)
    const bool undoCards = !g_exec.cardUndoOverflow;
    for (size_t i = undoCards ? g_exec.cardUndoCount : 0; i-- > 0;) {
        const auto& u = g_exec.cardUndo[i];
        if (u.rec) { u.rec[Offsets::PlayerRecord_Yellows] = u.y; u.rec[Offsets::PlayerRecord_Reds] = u.r; }
        if (u.wbRec) { u.wbRec[Offsets::PlayerRecord_Yellows] = u.y; u.wbRec[Offsets::PlayerRecord_Reds] = u.r; }
    }
    if (IsValidPtr(matchInfo)) {
        std::memcpy(matchInfo + Offsets::MatchInfo_SetPlayInfo, g_exec.savedMiSetPlay, sizeof(g_exec.savedMiSetPlay));
        std::memcpy(matchInfo + Offsets::MatchInfo_FoulInfo, g_exec.savedMiFoul, sizeof(g_exec.savedMiFoul));
        if (g_exec.savedWbValid) {
            if (uint8_t* wb = GetMatchInfoWriteBuffer(matchInfo)) {
                std::memcpy(wb + Offsets::MatchInfo_SetPlayInfo, g_exec.savedWbSetPlay, sizeof(g_exec.savedWbSetPlay));
                std::memcpy(wb + Offsets::MatchInfo_FoulInfo, g_exec.savedWbFoul, sizeof(g_exec.savedWbFoul));
            }
        }
    }
    *reinterpret_cast<uint32_t*>(fsm + Offsets::MatchFSM_QuickRestartFlag) = g_exec.savedQuickRestart;
    *reinterpret_cast<uint64_t*>(fsm + Offsets::MatchFSM_KickerCache) = g_exec.savedKickerCache;
    // Back to the engine's own resting JudgeResult (its FoulInfo half is not all zeros: 0x18 / 9 / 4 sentinels)
    std::memcpy(fsm + Offsets::MatchFSM_JudgeResult, g_exec.savedJudge, sizeof(g_exec.savedJudge));
    // The working block was already copied to MatchControl at the end of MainTick (0x140446D37), and the commit
    // reads that copy: restore the same three fields there (same offsets relative to the block)
    if (uint8_t* copy = *reinterpret_cast<uint8_t* const*>(fsm + Offsets::MatchFSM_WorkingCopyPtr)) {
        const uintptr_t judgeOff = Offsets::MatchFSM_JudgeResult - Offsets::MatchFSM_WorkingBlock;
        const uintptr_t quickOff = Offsets::MatchFSM_QuickRestartFlag - Offsets::MatchFSM_WorkingBlock;
        const uintptr_t kickOff  = Offsets::MatchFSM_KickerCache - Offsets::MatchFSM_WorkingBlock;
        auto* mc = static_cast<uint8_t*>(g_lastMatchControl.load(std::memory_order_relaxed));
        const bool expected = !IsValidPtr(mc) || copy == mc + Offsets::MatchControl_WorkingState;
        if (expected && IsMemoryRangeReadable(copy, judgeOff + sizeof(g_exec.savedJudge)) &&
            IsMemoryRangeReadable(copy + kickOff, sizeof(uint64_t))) {
            // Same fields the engine copies (0x140A3C310 skips SetPlayInfo + 0x24): +0x00..+0x23 and +0x28..+0x6B
            std::memcpy(copy + judgeOff, g_exec.savedJudge, 0x24);
            std::memcpy(copy + judgeOff + 0x28, g_exec.savedJudge + 0x28, sizeof(g_exec.savedJudge) - 0x28);
            *reinterpret_cast<uint32_t*>(copy + quickOff) = g_exec.savedQuickRestart;
            *reinterpret_cast<uint64_t*>(copy + kickOff) = g_exec.savedKickerCache;
        } else {
            VAR_LOG_ERR("Rollback: MatchControl working copy not where expected, its JudgeResult was not restored");
        }
    }
    VAR_LOG_WRN("VAR pool seq=%u: SetPlayReady left without EnterOutOfPlay (queue reason 0x%X), tick undone",
                g_exec.poolSeq, reason);
    const VerdictPool pool = g_exec.pool;
    const uint32_t gen = g_exec.matchGen;
    const uint32_t seq = g_exec.poolSeq;
    ExecResetToIdle();
    if (g_requeueSeq != seq) { g_requeueSeq = seq; g_requeueCount = 0; }
    if (undoCards && ++g_requeueCount <= 3 && VarState::Instance().RequeueVerdictPool(pool, gen)) {
        ExecEnter(ExecState::Cooldown, kExecCooldownTicks);
        return;
    }
    VarState::Instance().SetPostApplyBanner(undoCards ? "DECISÃO NÃO APLICADA  ·  O lance já mudou"
                                                      : "REINÍCIO NÃO APLICADO  ·  Os cartões foram aplicados, o lance já mudou",
                                            4800, BannerStyle::Amber);
    ExecEnter(ExecState::Cooldown, kExecCooldownTicks);
}

// Runs at the top of every MainTick (sim thread). The only place that changes g_exec.state, together with
// ExecOnJudgeFinalize.
void ExecStep(uint8_t* fsm, uint8_t* gmatch, uint8_t* matchInfo, uint8_t* replayMgr, int32_t fsmState) {
    if (g_exec.state != ExecState::Idle && VarState::Instance().MatchGeneration() != g_exec.matchGen) {
        VAR_LOG_WRN("VAR pool seq=%u aborted: match was reset", g_exec.poolSeq);
        ExecResetToIdle();
    }

    switch (g_exec.state) {
        case ExecState::Idle:
            if (!VarState::Instance().HasArmedVerdict()) return;
            if (!VarState::Instance().IsEnabled() || !VarState::Instance().IsOfflineAllowed()) return;
            if (!IsExecFsmState(fsmState)) return; // cutscenes, replays, kick-off: the pool waits
            if (!IsMatchEligibleForExecution(gmatch, matchInfo, replayMgr)) return;
            if (VarState::Instance().DropStalePlayDecision(g_stoppageCount)) {
                VAR_LOG_WRN("VAR play decision armed in an earlier stoppage dropped (the game moved on)");
                VarState::Instance().SetPostApplyBanner(
                    "LANCE NÃO APLICADO  ·  O jogo já tinha seguido, só os cartões continuam no pool", 4800, BannerStyle::Amber);
                if (!VarState::Instance().HasArmedVerdict()) return;
            }
            if (!GetTeamInfoWriteBuffer(GetTeamInfo(gmatch, 0), 0) || !GetTeamInfoWriteBuffer(GetTeamInfo(gmatch, 1), 1)) {
                // Cards could not be written durably: the pool stays armed (never half a decision)
                if (!g_teamWbBlockedLogged) {
                    g_teamWbBlockedLogged = true;
                    VAR_LOG_ERR("VAR pool waiting: TeamInfo write buffer could not be validated");
                    VarState::Instance().SetPostApplyBanner(
                        "DECISÃO AGUARDANDO  ·  Estado da partida indisponível", 4800, BannerStyle::Amber);
                }
                return;
            }
            ExecStartPool(fsm, gmatch, matchInfo, fsmState);
            return;

        case ExecState::AwaitNativeHook:
            if (g_mainTickCount < g_exec.deadlineTick) return;
            // FinalizeFrame did not fire: perform from here, but only in the same state and with a free JudgeResult
            if (fsm == g_exec.fsm && fsmState == g_exec.startFsmState &&
                *reinterpret_cast<const int32_t*>(fsm + Offsets::MatchFSM_JudgeResult) == 0 &&
                IsMatchEligibleForExecution(gmatch, matchInfo, replayMgr)) {
                VAR_LOG_WRN("VAR request fallback from MainTick (fsmState=0x%X)", fsmState);
                if (ExecPerform(fsm, gmatch, matchInfo, true)) {
                    ExecEnter(ExecState::AwaitTransition, kExecTransitionTimeoutTicks);
                    return;
                }
            }
            if (g_requeueSeq != g_exec.poolSeq) { g_requeueSeq = g_exec.poolSeq; g_requeueCount = 0; }
            if (!g_exec.cardsPlanApplied && !g_exec.injected && ++g_requeueCount <= 3 &&
                VarState::Instance().RequeueVerdictPool(g_exec.pool, g_exec.matchGen)) {
                // Nothing was written (the engine produced its own result this tick): the decision waits for the
                // next stoppage instead of being lost
                VAR_LOG_INF("VAR pool seq=%u not run this tick: re-armed (its play still needs this same stoppage)", g_exec.poolSeq);
                g_exec.req = ExecRequest{};
                ExecEnter(ExecState::Cooldown, kExecCooldownTicks);
                return;
            }
            ExecFinish(false, g_exec.cardsApplied
                ? "REINÍCIO NÃO APLICADO  ·  Os cartões foram aplicados, o lance já mudou"
                : "DECISÃO NÃO APLICADA  ·  O lance já mudou");
            return;

        case ExecState::AwaitTransition: {
            // Verified by reading the FSM back: EnterOutOfPlay / RebuildSetPlay are synchronous, so the FSM must have
            // left the state of the request and must not be back in open play
            const bool left = (fsmState != g_exec.startFsmState) && fsmState != 0x1C;
            if (left) {
                if (InjectedCardMissing(gmatch)) {
                    // The set play changed but the engine did not commit our foul: give its card by hand
                    ApplyCardDelta(gmatch, g_exec.req.offenderSlot, CardKind::None, g_exec.req.card, g_exec.req.offenderRoster);
                    g_exec.cardsApplied = true;
                    g_exec.playManualCard = g_exec.req.card;
                    VAR_LOG_WRN("Injected foul left the state without an engine commit: card %d applied manually",
                                static_cast<int>(g_exec.req.card));
                }
                ExecFinish(true, nullptr);
                return;
            }
            if (g_mainTickCount >= g_exec.deadlineTick) {
                ExecFinish(false, g_exec.cardsApplied
                    ? "REINÍCIO NÃO APLICADO  ·  Os cartões foram aplicados, o lance já mudou"
                    : "DECISÃO NÃO APLICADA  ·  O lance já mudou");
            }
            return;
        }

        case ExecState::Cooldown:
            if (g_mainTickCount >= g_exec.deadlineTick) ExecResetToIdle();
            return;
    }
}

// Judge::FinalizeFrame runs inside the 0x1A / 0x1C state handler: the native place for a new JudgeResult
void ExecOnJudgeFinalize(uint8_t* fsm) {
    if (g_exec.state != ExecState::AwaitNativeHook || fsm != g_exec.fsm) return;
    if (VarState::Instance().MatchGeneration() != g_exec.matchGen) return;
    if (GetFsmCurrentState(fsm) != g_exec.startFsmState) return;
    // The engine produced its own result this frame (goal, ball out, a real foul): never overwrite it
    if (*reinterpret_cast<const int32_t*>(fsm + Offsets::MatchFSM_JudgeResult) != 0) return;
    uint8_t* gmatch = GetGmatch();
    uint8_t* matchInfo = GetMatchInfo(gmatch);
    if (!IsValidPtr(matchInfo) || !IsMatchEligibleForExecution(gmatch, matchInfo, GetReplayMgr(gmatch))) return;
    if (ExecPerform(fsm, gmatch, matchInfo, false)) {
        ExecEnter(ExecState::AwaitTransition, kExecTransitionTimeoutTicks);
    }
}

// ============================================================================
// Replay ring diagnostics (read-only)
// ============================================================================
// Evidence for a future "keep the pre-foul play in the manual replay" feature. Nothing here writes game memory.
// Layout (re_notes/06_replay_ring_buffer.md): replay system *(0x143703558), active buffer pointer +0x6A8, active slot
// index +0x698; buffer +0x10 u16 write index, +0x12 u16 capacity, +0x14 u32 match tick of the last write, +0x40 frames
// (stride 0x1FB0, float time at +0, u16 at +0x24C == 0x7FFF marks an invalid frame).
namespace ReplayDiag {
constexpr uintptr_t Sys_ActiveSlot   = 0x698;
constexpr uintptr_t Sys_ActiveBuffer = 0x6A8;
constexpr uintptr_t Buf_WriteIndex   = 0x10;
constexpr uintptr_t Buf_Capacity     = 0x12;
constexpr uintptr_t Buf_LastTick     = 0x14;
constexpr uintptr_t Buf_Frames       = 0x40;
constexpr uintptr_t Frame_Valid      = 0x24C;

struct RingInfo {
    bool     ok = false;
    uint32_t slot = 0;
    uint16_t writeIdx = 0;
    uint16_t cap = 0;
    uint32_t lastTick = 0;
    uint32_t validFrames = 0;
    float    oldestTime = 0.0f;
    float    newestTime = 0.0f;
};

uint8_t* ActiveBuffer(uint32_t* outSlot) {
    if (!g_replaySingletonPtrAddr) return nullptr;
    const uint8_t* sys = *reinterpret_cast<const uint8_t* const*>(g_replaySingletonPtrAddr);
    if (!IsValidPtr(sys) || !IsMemoryRangeReadable(sys + Sys_ActiveSlot, 0x18)) return nullptr;
    if (outSlot) *outSlot = *reinterpret_cast<const uint32_t*>(sys + Sys_ActiveSlot);
    uint8_t* buf = *reinterpret_cast<uint8_t* const*>(sys + Sys_ActiveBuffer);
    if (!IsValidPtr(buf) || !IsMemoryRangeReadable(buf, 0x50)) return nullptr;
    return buf;
}

RingInfo Read(bool scanFrames) {
    RingInfo r{};
    uint8_t* buf = ActiveBuffer(&r.slot);
    if (!buf) return r;
    r.writeIdx = *reinterpret_cast<const uint16_t*>(buf + Buf_WriteIndex);
    r.cap = *reinterpret_cast<const uint16_t*>(buf + Buf_Capacity);
    r.lastTick = *reinterpret_cast<const uint32_t*>(buf + Buf_LastTick);
    r.ok = (r.cap > 0 && r.cap <= 8192);
    if (!r.ok || !scanFrames) return r;
    const uint8_t* frames = *reinterpret_cast<const uint8_t* const*>(buf + Buf_Frames);
    if (!IsValidPtr(frames) || !IsMemoryRangeReadable(frames, static_cast<size_t>(r.cap) * Offsets::ReplayRing_FrameStride)) {
        r.ok = false;
        return r;
    }
    bool first = true;
    for (uint32_t i = 0; i < r.cap; ++i) {
        const uint8_t* f = frames + static_cast<size_t>(i) * Offsets::ReplayRing_FrameStride;
        if (*reinterpret_cast<const uint16_t*>(f + Frame_Valid) == 0x7FFF) continue;
        const float t = *reinterpret_cast<const float*>(f);
        ++r.validFrames;
        if (first || t < r.oldestTime) r.oldestTime = t;
        if (first || t > r.newestTime) r.newestTime = t;
        first = false;
    }
    return r;
}

void Log(const char* when, const RingInfo& r) {
    if (!r.ok) {
        VAR_LOG_INF("ReplayDiag %s: ring unavailable", when);
        return;
    }
    VAR_LOG_INF("ReplayDiag %s: slot=%u writeIdx=%u cap=%u lastTick=%u valid=%u time=[%.1f .. %.1f] span=%.1f",
                when, r.slot, r.writeIdx, r.cap, r.lastTick, r.validFrames, r.oldestTime, r.newestTime,
                r.newestTime - r.oldestTime);
}

// Sim-thread tracking: write-index advance rate and the ring right before / after each stoppage
uint16_t g_prevWriteIdx = 0;
uint64_t g_advanceCount = 0;
uint64_t g_rateWindowStartTick = 0;
int      g_rateLogsLeft = 2;
int32_t  g_prevMatchStatus = -1;
uint8_t  g_prevClearFlag = 0;
int      g_stoppageLogsLeft = 6;
bool     g_loggedManualEntry = false;
int32_t  g_prevFsmState = -1;
int      g_autoReplayLogsLeft = 4;
uint32_t g_lastStoppageTick = 0;

void ResetForMatch() {
    g_prevWriteIdx = 0;
    g_advanceCount = 0;
    g_rateWindowStartTick = 0;
    g_rateLogsLeft = 2;
    g_prevMatchStatus = -1;
    g_prevClearFlag = 0;
    g_stoppageLogsLeft = 6;
    g_loggedManualEntry = false;
    g_prevFsmState = -1;
    g_autoReplayLogsLeft = 4;
    g_lastStoppageTick = 0;
}

// Native automatic foul replay (state 0x1D): which window it plays, relative to the stoppage tick
void OnFsmState(int32_t fsmState, uint8_t* replayMgr, uint32_t matchTick) {
    if (fsmState == g_prevFsmState) return;
    if (fsmState == 0x1D && g_autoReplayLogsLeft > 0 && IsValidPtr(replayMgr) &&
        IsMemoryRangeReadable(replayMgr, Offsets::ReplayMgr_CursorPhase + 4)) {
        --g_autoReplayLogsLeft;
        VAR_LOG_INF("ReplayDiag auto replay 0x1D: window=[%.1f p%d .. %.1f p%d] cursor=%.1f stoppageTick=%u nowTick=%u",
                    *reinterpret_cast<const float*>(replayMgr + Offsets::ReplayMgr_WindowStartTime),
                    *reinterpret_cast<const int32_t*>(replayMgr + Offsets::ReplayMgr_WindowStartPhase),
                    *reinterpret_cast<const float*>(replayMgr + Offsets::ReplayMgr_WindowEndTime),
                    *reinterpret_cast<const int32_t*>(replayMgr + Offsets::ReplayMgr_WindowEndPhase),
                    *reinterpret_cast<const float*>(replayMgr + Offsets::ReplayMgr_CursorTime),
                    g_lastStoppageTick, matchTick);
    }
    g_prevFsmState = fsmState;
}

void OnSimTick(uint8_t* matchInfo, int32_t matchStatus) {
    const RingInfo r = Read(false);
    if (r.ok) {
        if (g_rateWindowStartTick == 0) {
            g_rateWindowStartTick = g_mainTickCount;
            g_prevWriteIdx = r.writeIdx;
        } else if (r.writeIdx != g_prevWriteIdx) {
            g_advanceCount += static_cast<uint16_t>((r.writeIdx + r.cap - g_prevWriteIdx) % r.cap);
            g_prevWriteIdx = r.writeIdx;
        }
        if (g_rateLogsLeft > 0 && g_mainTickCount - g_rateWindowStartTick >= 600) {
            --g_rateLogsLeft;
            VAR_LOG_INF("ReplayDiag rate: %llu frames recorded in 600 sim ticks (cap=%u slot=%u)",
                        static_cast<unsigned long long>(g_advanceCount), r.cap, r.slot);
            g_advanceCount = 0;
            g_rateWindowStartTick = g_mainTickCount;
        }
    }
    // MatchInfo + 0x11B8: when set, the controller tick clears the active ring (0x14042E9F0 -> 0x140A8F310)
    const uint8_t clearFlag = *reinterpret_cast<const uint8_t*>(matchInfo + 0x11B8);
    if (clearFlag != g_prevClearFlag) {
        if (g_stoppageLogsLeft > 0) {
            VAR_LOG_INF("ReplayDiag MatchInfo+0x11B8 %u -> %u (status=%d)", g_prevClearFlag, clearFlag, matchStatus);
        }
        g_prevClearFlag = clearFlag;
    }
    if (g_prevMatchStatus == static_cast<int32_t>(MatchStatus::InPlay) && matchStatus != g_prevMatchStatus) {
        g_lastStoppageTick = *reinterpret_cast<const uint32_t*>(matchInfo + Offsets::MatchInfo_MatchTick);
    }
    if (g_prevMatchStatus == static_cast<int32_t>(MatchStatus::InPlay) && matchStatus != g_prevMatchStatus &&
        g_stoppageLogsLeft > 0) {
        --g_stoppageLogsLeft;
        Log("at stoppage", Read(true));
    }
    g_prevMatchStatus = matchStatus;
}

// Replay thread: what the manual replay actually plays
void OnManualReplayController(uint8_t* ctrl) {
    if (g_loggedManualEntry) return;
    g_loggedManualEntry = true;
    const uint8_t* player = ctrl + Offsets::ReplayCtrl_PlayerOffset;
    const uint32_t playerSlot = IsMemoryRangeReadable(player, 0x10) ? *reinterpret_cast<const uint32_t*>(player + 4) : 0xFFFFFFFFU;
    VAR_LOG_INF("ReplayDiag manual replay: player slot=%u cur=%u cap=%u win=[%u .. %u]",
                playerSlot,
                static_cast<unsigned>(*reinterpret_cast<const uint16_t*>(ctrl + Offsets::ReplayCtrl_CurFrame)),
                static_cast<unsigned>(*reinterpret_cast<const uint16_t*>(ctrl + Offsets::ReplayCtrl_RingCap)),
                static_cast<unsigned>(*reinterpret_cast<const uint16_t*>(ctrl + Offsets::ReplayCtrl_WinStartFrame)),
                static_cast<unsigned>(*reinterpret_cast<const uint16_t*>(ctrl + Offsets::ReplayCtrl_WinEndFrame)));
    Log("in manual replay", Read(true));
}
} // namespace ReplayDiag

void ResetSimTrackersOnSimThread() {
    g_lastSnapshotTick = 0;
    g_lastSeenMatchTick = 0;
    g_lastAdvActive = false;
    g_lastFoulValidEdge = false;
    g_lastFoulOffenderEdge = -1;
    g_lastFoulCardEdge = -1;
    g_lastFoulPhaseEdge = -1;
    g_lastFoulTimeEdge = -1000.0f;
    g_forceRosterSyncNextTick = false;
    g_recentBallOwnerSlot = -1;
    g_recentBallOwnerTick = 0;
    g_verifiedRosterDataPtr = nullptr;
    g_verifiedEditedKitPtr = nullptr;
    g_verifiedBallStatePtr = nullptr;
    g_verifiedMatchInfoWriteBuf = nullptr;
    g_verifiedTeamInfoWriteBuf[0] = g_verifiedTeamInfoWriteBuf[1] = nullptr;
    g_warnedTeamInfoWb[0] = g_warnedTeamInfoWb[1] = false;
    g_stoppageCount = 0;
    g_prevStatusForStoppage = -1;
    g_requeueSeq = 0;
    g_requeueCount = 0;
    g_teamWbBlockedLogged = false;
    ExecResetToIdle();
    g_loggedDeferredOnPitch = false;
    ReplayDiag::ResetForMatch();
    g_verifiedReplayRingBufPtr.store(nullptr, std::memory_order_release);
    g_syncedTeamRosterTable = nullptr;
    g_syncedTeamKitIds[0] = 0xFFFFFFFFU;
    g_syncedTeamKitIds[1] = 0xFFFFFFFFU;
    g_hasPushedPlayersOnce = false;
    g_rosterDiagCount = 0;
    g_rosterDiagTick = 0;
    g_loggedRosterSyncOnce = false;
    g_loggedTeamSyncOnce = false;
    std::memset(g_cachedPlayerIds, 0, sizeof(g_cachedPlayerIds));
    for (int i = 0; i < 22; ++i) g_cachedRosterIdx[i] = -1;
    std::memset(g_cachedShirtNums, 0, sizeof(g_cachedShirtNums));
    std::memset(g_cachedYellows, 0, sizeof(g_cachedYellows));
    std::memset(g_cachedReds, 0, sizeof(g_cachedReds));
    std::memset(g_cachedOnPitch, 0, sizeof(g_cachedOnPitch));
    std::memset(g_cachedPlayerNames, 0, sizeof(g_cachedPlayerNames));
    std::memset(g_cachedShirtNames, 0, sizeof(g_cachedShirtNames));
}

// ============================================================================
// Detour 1: MatchFSM::MainTick (0x1404468F0)
// ============================================================================
uint32_t __fastcall Detour_MainTick(void* matchFsm, void* runCtx, void* arg3) {
    ++g_mainTickCount;
    g_pollSuspended.store(false, std::memory_order_release);
    g_lastMatchFsm.store(matchFsm, std::memory_order_relaxed);

    if (g_simResetPending.exchange(false, std::memory_order_acq_rel)) {
        ResetSimTrackersOnSimThread();
    }

    auto* fsm = static_cast<uint8_t*>(matchFsm);
    uint8_t* gmatch = GetGmatch();
    uint8_t* matchInfo = GetMatchInfo(gmatch);
    uint8_t* replayMgr = GetReplayMgr(gmatch);

    if (IsValidPtr(fsm) && IsValidPtr(gmatch) && IsValidPtr(matchInfo)) {
        const uint32_t matchTick = *reinterpret_cast<const uint32_t*>(matchInfo + Offsets::MatchInfo_MatchTick);
        if (g_lastSeenMatchTick > 120 && matchTick + 60 < g_lastSeenMatchTick) {
            VAR_LOG_INF("Detected Restart Match (tick %u -> %u), resetting VAR ring buffers",
                        g_lastSeenMatchTick, matchTick);
            // Also the only reset for flows where Sider sends no set_teams between matches (rematch, some
            // league/cup chains): the previous match's pool, events and HUD state never leak into this one.
            VarState::Instance().ResetMatch();
            VarHudRenderer::RequestReset();
            ResetSimTrackersOnSimThread();
        }
        g_lastSeenMatchTick = matchTick;

        {
            const int32_t st = *reinterpret_cast<const int32_t*>(matchInfo + Offsets::MatchInfo_MatchStatus);
            // Every switch between ball in play and ball out of play starts a new phase: a decision belongs
            // to the phase it was armed in (a stoppage, or a pause during open play, which keeps the status)
            const bool wasInPlay = g_prevStatusForStoppage == static_cast<int32_t>(MatchStatus::InPlay);
            const bool isInPlay = st == static_cast<int32_t>(MatchStatus::InPlay);
            if (g_prevStatusForStoppage != -1 && wasInPlay != isInPlay) {
                ++g_stoppageCount;
            }
            g_prevStatusForStoppage = st;
        }
        const int32_t fsmState = GetFsmCurrentState(fsm);
        ExecStep(fsm, gmatch, matchInfo, replayMgr, fsmState);

        const int32_t matchPhase  = *reinterpret_cast<const int32_t*>(matchInfo + Offsets::MatchInfo_MatchPhase);
        const int32_t matchStatus = *reinterpret_cast<const int32_t*>(matchInfo + Offsets::MatchInfo_MatchStatus);
        const float time60Hz      = GetMatchTime60Hz(matchInfo);

        // 60Hz physical incident detection + 10Hz rolling spatial snapshot during InPlay (every 6 frames)
        if (matchStatus == static_cast<int32_t>(MatchStatus::InPlay)) {
            const bool storeSnap = (matchTick < g_lastSnapshotTick || matchTick - g_lastSnapshotTick >= 6);
            if (storeSnap) g_lastSnapshotTick = matchTick;
            SampleInPlayFrame(gmatch, matchInfo, matchPhase, time60Hz, matchTick, storeSnap);
        }

        // Sync roster names, shirt numbers, substitutions, team colors & cards periodically
        if (g_forceRosterSyncNextTick || (g_mainTickCount % 25 == 0) || g_mainTickCount <= 5) {
            g_forceRosterSyncNextTick = false;
            SyncPlayersAndCards(gmatch, matchInfo);
        }

        // Detect advantage play-on edge (MatchInfo + 0x1544)
        const uint32_t advA = *reinterpret_cast<const uint32_t*>(matchInfo + Offsets::MatchInfo_AdvantageInfo + 0x04);
        const uint32_t advB = *reinterpret_cast<const uint32_t*>(matchInfo + Offsets::MatchInfo_AdvantageInfo + 0x08);
        const bool advNow = (advA != 0 && advB != 0 && matchStatus == static_cast<int32_t>(MatchStatus::InPlay));
        if (advNow && !g_lastAdvActive) {
            VarEvent ev{};
            ev.type = VarEventType::Advantage;
            ev.matchPhase = matchPhase;
            ev.time60Hz = time60Hz;
            ev.matchTick = matchTick;
            ev.setPlayKind = SetPlayKind::FreeKick;
            ev.wasAdvantage = true;
            // Advantage goes to the team that suffered the foul: the opponents of the offender of the contact
            // that just happened. Only without a detected contact, the team in possession.
            const int32_t advOff = VarState::Instance().RecentIncidentOffender(matchPhase, time60Hz, 90.0f);
            if (IsValidSlot(advOff)) ev.benefitedTeam = 1 - SlotToTeam(advOff);
            else if (IsValidSlot(g_recentBallOwnerSlot)) ev.benefitedTeam = SlotToTeam(g_recentBallOwnerSlot);
            uint8_t* ball = *reinterpret_cast<uint8_t**>(gmatch + Offsets::GMATCH_Ball);
            if (IsValidPtr(ball)) {
                std::memcpy(&ev.pos, ball + Offsets::Ball_PosVec3, sizeof(Vec3));
            }
            {
                const PitchDims bd = ReadPitchDims(gmatch);
                ev.boxDistM = PitchGeometry::SignedDistanceToDefendingBox(bd, ev.pos, ReadTeamAttackDir(gmatch, 1 - ev.benefitedTeam));
                ev.boxDistValid = true;
            }
            VarState::Instance().PushEvent(ev);
        }
        g_lastAdvActive = advNow;

        CalibrateSidesAtKickoff(gmatch, matchInfo, matchStatus);
        ReplayDiag::OnSimTick(matchInfo, matchStatus);
        ReplayDiag::OnFsmState(fsmState, replayMgr, matchTick);

        const uint8_t curReplayMode = IsValidPtr(replayMgr) ? *reinterpret_cast<const uint8_t*>(replayMgr + Offsets::ReplayMgr_Mode) : 0;
        const uint8_t curManualFlag = IsValidPtr(replayMgr) ? *reinterpret_cast<const uint8_t*>(replayMgr + Offsets::ReplayMgr_ManualScreenFlag) : 0;
        // Telemetry is pushed every tick: the render thread does not contend on live frames
        // (PollLiveReplayState fast path), so this is one uncontended lock per 16.6 ms and keeps
        // time60Hz / liveBallPos exact at the moment the simulation pauses for a Replay.
        {
            LiveTelemetry tel{};
            tel.matchActive = true;
            tel.gmatchPtr = reinterpret_cast<uintptr_t>(gmatch);
            tel.matchFsmPtr = reinterpret_cast<uintptr_t>(fsm);
            tel.matchControlPtr = reinterpret_cast<uintptr_t>(g_lastMatchControl.load(std::memory_order_relaxed));
            tel.matchInfoPtr = reinterpret_cast<uintptr_t>(matchInfo);
            tel.replayMgrPtr = reinterpret_cast<uintptr_t>(replayMgr);

            tel.matchPhase = matchPhase;
            tel.matchStatus = matchStatus;
            tel.fsmState = fsmState;
            tel.matchTick = matchTick;
            tel.time60Hz = time60Hz;
            tel.specialModeKind = *reinterpret_cast<const uint8_t*>(matchInfo + Offsets::MatchInfo_SpecialModeKind);

            if (IsValidPtr(replayMgr)) {
                tel.replayMode             = curReplayMode;
                tel.replayManualFlag       = curManualFlag;
                tel.replayIsPlaying        = *reinterpret_cast<const uint8_t*>(replayMgr + Offsets::ReplayMgr_IsPlaying);
                tel.replayUiHidden         = *reinterpret_cast<const uint8_t*>(replayMgr + Offsets::ReplayMgr_UiHidden);
                tel.replayCursorTime       = *reinterpret_cast<const float*>(replayMgr + Offsets::ReplayMgr_CursorTime);
                tel.replayCursorPhase      = *reinterpret_cast<const int32_t*>(replayMgr + Offsets::ReplayMgr_CursorPhase);
                tel.replayWindowStartTime  = *reinterpret_cast<const float*>(replayMgr + Offsets::ReplayMgr_WindowStartTime);
                tel.replayWindowStartPhase = *reinterpret_cast<const int32_t*>(replayMgr + Offsets::ReplayMgr_WindowStartPhase);
                tel.replayWindowEndTime    = *reinterpret_cast<const float*>(replayMgr + Offsets::ReplayMgr_WindowEndTime);
                tel.replayWindowEndPhase   = *reinterpret_cast<const int32_t*>(replayMgr + Offsets::ReplayMgr_WindowEndPhase);
                tel.replayFocusSlot        = *reinterpret_cast<const int32_t*>(replayMgr + Offsets::ReplayMgr_CameraFocusSlot);
                const uint32_t tlPos       = *reinterpret_cast<const uint32_t*>(replayMgr + Offsets::ReplayMgr_TimelinePos);
                const uint32_t tlDur       = *reinterpret_cast<const uint32_t*>(replayMgr + Offsets::ReplayMgr_TimelineDur);
                tel.replayTimelineNorm     = ReplayTimelineMath::ComputeTimelineNorm(tlPos, tlDur);
            }

            const auto* sp = reinterpret_cast<const RawSetPlayInfo*>(matchInfo + Offsets::MatchInfo_SetPlayInfo);
            const auto* fi = reinterpret_cast<const RawFoulInfo*>(matchInfo + Offsets::MatchInfo_FoulInfo);
            tel.activeSetPlayKind   = sp->setPlayKind;
            tel.stoppageCount       = g_stoppageCount;
            tel.activeBenefitedTeam = sp->benefitedTeam;
            tel.activeFoulValid     = fi->isValid;
            tel.activeFoulKind      = fi->foulKind;
            tel.activeFoulCard      = fi->foulJudgementKind;
            tel.activeOffenderSlot  = IsValidSlot(fi->offenderSlot) ? fi->offenderSlot : -1;
            tel.activeVictimSlot    = IsValidSlot(fi->victimSlot) ? fi->victimSlot : -1;

            tel.pitchDims = ReadPitchDims(gmatch);
            tel.teamAttackDir[0] = ReadTeamAttackDir(gmatch, 0);
            tel.teamAttackDir[1] = ReadTeamAttackDir(gmatch, 1);
            uint8_t* ballLive = *reinterpret_cast<uint8_t**>(gmatch + Offsets::GMATCH_Ball);
            if (IsValidPtr(ballLive)) {
                std::memcpy(&tel.liveBallPos, ballLive + Offsets::Ball_PosVec3, sizeof(Vec3));
            }

            tel.mainTickCount = g_mainTickCount;
            tel.finalizeFrameCount = g_finalizeFrameCount;
            tel.foulCommitCount = g_foulCommitCount;

            VarState::Instance().UpdateTelemetry(tel);
        }
    }

    const uint32_t result = g_hookMainTick.Original<FnMainTick>()(matchFsm, runCtx, arg3);
    ExecVerifyAfterMainTick(static_cast<uint8_t*>(matchFsm));
    return result;
}

// ============================================================================
// Detour 2: Judge::FinalizeFrame (0x14050CD70)
// Called every tick inside BOTH State_InPlay (0x1C) and State_SetPlayReady (0x1A)
// immediately before `if (*(fsm + 0x49c0) != 0) MatchFSM_EnterOutOfPlay(fsm)`.
// ============================================================================
void __fastcall Detour_JudgeFinalizeFrame(void* judge) {
    ++g_finalizeFrameCount;

    // Run native Judge::FinalizeFrame first (unbinds judge + 0x3B8 / + 0x3C0)
    g_hookJudgeFinalize.Original<FnJudgeFinalizeFrame>()(judge);

    if (!IsValidPtr(judge)) return;
    auto* judgeBytes = static_cast<uint8_t*>(judge);
    auto* expectedFsm = static_cast<uint8_t*>(g_lastMatchFsm.load(std::memory_order_relaxed));
    uint8_t* fsm = judgeBytes - Offsets::MatchFSM_EmbeddedJudge;
    if (!IsValidPtr(expectedFsm) || fsm != expectedFsm) return;
    ExecOnJudgeFinalize(fsm);
}

// ============================================================================
// Detour 3: MatchControl::UpdateFoulCounters (0x140424410)
// ============================================================================
void __fastcall Detour_UpdateFoulCounters(void* matchControl, void* matchInfo, void* workingState) {
    ++g_foulCommitCount;
    g_lastMatchControl.store(matchControl, std::memory_order_relaxed);

    if (IsValidPtr(workingState) && IsValidPtr(matchInfo)) {
        auto* ws = static_cast<uint8_t*>(workingState);
        auto* mi = static_cast<uint8_t*>(matchInfo);
        auto* sp = reinterpret_cast<RawSetPlayInfo*>(ws + Offsets::Working_SetPlayInfo);
        auto* fi = reinterpret_cast<RawFoulInfo*>(ws + Offsets::Working_FoulInfo);

        // The engine commits (and cards) the VAR's own injected foul natively. It is recorded by the VAR ledger
        // as "reviewed by VAR", so it is not pushed again here as an official referee incident.
        const bool isOurInjectedFoul = g_exec.expectOwnFoulCommit && fi->isValid &&
                                       fi->offenderSlot == g_exec.expectOffender &&
                                       fi->snapshotTickIndex == g_exec.expectSnapId;
        if (isOurInjectedFoul) {
            g_exec.expectOwnFoulCommit = false;
            g_exec.ownFoulCommitted = true;
            g_lastFoulValidEdge = true;
            g_lastFoulOffenderEdge = fi->offenderSlot;
            g_lastFoulCardEdge = fi->foulJudgementKind;
            g_lastFoulPhaseEdge = *reinterpret_cast<const int32_t*>(mi + Offsets::MatchInfo_MatchPhase);
            g_lastFoulTimeEdge = GetMatchTime60Hz(mi);
            VAR_LOG_INF("Engine committed the VAR foul natively (offender=%d card=%d snap=%u)",
                        fi->offenderSlot, fi->foulJudgementKind, fi->snapshotTickIndex);
        } else if (fi->isValid == 0) {
            g_lastFoulValidEdge = false;
        } else if (fi->foulKind >= static_cast<int32_t>(FoulKind::StandardFoul)) {
            const int32_t curPhase = *reinterpret_cast<const int32_t*>(mi + Offsets::MatchInfo_MatchPhase);
            const float curTime = GetMatchTime60Hz(mi);
            const int32_t curOff = IsValidSlot(fi->offenderSlot) ? fi->offenderSlot : -1;
            const int32_t curCard = fi->foulJudgementKind;

            const bool isNewIncident = !g_lastFoulValidEdge ||
                                       (curOff != g_lastFoulOffenderEdge) ||
                                       (curCard != g_lastFoulCardEdge) ||
                                       (curPhase != g_lastFoulPhaseEdge) ||
                                       (std::fabs(curTime - g_lastFoulTimeEdge) > 90.0f);

            g_lastFoulValidEdge = true;
            if (isNewIncident) {
                g_lastFoulOffenderEdge = curOff;
                g_lastFoulCardEdge = curCard;
                g_lastFoulPhaseEdge = curPhase;
                g_lastFoulTimeEdge = curTime;

                VarEvent ev{};
                ev.matchPhase = curPhase;
                ev.time60Hz   = curTime;
                ev.matchTick  = *reinterpret_cast<const uint32_t*>(mi + Offsets::MatchInfo_MatchTick);

                ev.setPlayKind   = static_cast<SetPlayKind>(sp->setPlayKind);
                ev.benefitedTeam = (sp->benefitedTeam == 1) ? 1 : 0;
                ev.offenderSlot  = curOff;
                ev.victimSlot    = IsValidSlot(fi->victimSlot) ? fi->victimSlot : -1;
                ev.pos           = sp->posA;
                ev.wasAdvantage  = (fi->wasAdvantageFoul != 0);

                if (curCard == 3) ev.cardKind = CardKind::Yellow;
                else if (curCard == 4) ev.cardKind = CardKind::Red;
                else ev.cardKind = CardKind::Simple;

                if (ev.setPlayKind == SetPlayKind::PenaltyKick) ev.type = VarEventType::Penalty;
                else if (ev.setPlayKind == SetPlayKind::FreeKick) ev.type = VarEventType::Foul;
                else if (ev.cardKind == CardKind::Yellow || ev.cardKind == CardKind::Red) ev.type = VarEventType::CardOnly;
                else ev.type = VarEventType::Foul;

                uint8_t* gmatch = GetGmatch();
                ev.offenderRosterIdx = static_cast<int8_t>(ReadActiveRosterIndex(gmatch, ev.offenderSlot));
                ev.victimRosterIdx = static_cast<int8_t>(ReadActiveRosterIndex(gmatch, ev.victimSlot));
                if (IsValidSlot(ev.offenderSlot) && gmatch) {
                    uint8_t cy = 0, cr = 0;
                    if (ReadCards(gmatch, ev.offenderSlot, -1, cy, cr)) {
                        ev.offenderYellowsAfter = cy + (ev.cardKind == CardKind::Yellow ? 1 : 0);
                        ev.offenderRedsAfter = cr +
                            ((ev.cardKind == CardKind::Red || (ev.cardKind == CardKind::Yellow && ev.offenderYellowsAfter >= 2)) ? 1 : 0);
                    }
                }

                if (gmatch) {
                    const PitchDims bd = ReadPitchDims(gmatch);
                    const int32_t bTeam = IsValidSlot(ev.offenderSlot) ? SlotToTeam(ev.offenderSlot) : (1 - ev.benefitedTeam);
                    ev.boxDistM = PitchGeometry::SignedDistanceToDefendingBox(bd, ev.pos, ReadTeamAttackDir(gmatch, bTeam));
                    ev.boxDistValid = true;
                }
                const uint32_t id = VarState::Instance().PushEvent(ev);
                g_forceRosterSyncNextTick = true;

                if (ev.type == VarEventType::Penalty) {
                    VarState::Instance().SetBroadcastAlert(
                        "PÊNALTI MARCADO  ·  Revisão disponível no replay", 3400, BannerStyle::Amber);
                } else if (ev.cardKind == CardKind::Red || ev.offenderRedsAfter > 0) {
                    VarState::Instance().SetBroadcastAlert(
                        "CARTÃO VERMELHO  ·  Revisão disponível no replay", 3400, BannerStyle::RedCard);
                } else if (ev.type == VarEventType::Foul && gmatch) {
                    const PitchDims d = ReadPitchDims(gmatch);
                    const int32_t offTeam = IsValidSlot(ev.offenderSlot) ? SlotToTeam(ev.offenderSlot) : (1 - ev.benefitedTeam);
                    const float distBox = PitchGeometry::SignedDistanceToDefendingBox(d, ev.pos, ReadTeamAttackDir(gmatch, offTeam));
                    if (distBox <= 3.5f) {
                        VarState::Instance().SetBroadcastAlert(
                            "FALTA PERTO DA ÁREA  ·  Revisão disponível no replay", 3000, BannerStyle::Info);
                    }
                }

                VAR_LOG_INF("Recorded Event #%u type=%d sp=%d card=%d off=%d vic=%d team=%d phase=%d t=%.1f",
                            id, static_cast<int>(ev.type), static_cast<int>(ev.setPlayKind),
                            static_cast<int>(ev.cardKind), ev.offenderSlot, ev.victimSlot,
                            ev.benefitedTeam, ev.matchPhase, ev.time60Hz);
            }
        }
    }

    g_hookUpdateFoulCounters.Original<FnUpdateFoulCounters>()(matchControl, matchInfo, workingState);
}

// ============================================================================
// Detour 4: ManualReplayController::SyncToReplayManager (0x1420C16A0)
// Runs on the exact Replay thread every frame (playing or paused).
// Executes queued Native Seek (0x14217E800), Play/Pause (+0x5C) & Camera Focus
// (+0x64D, +0x80, 0x14217EC70, ReplayCamera +0x18 -> +0x00/+0x44) right before
// the engine mirrors all ReplayController state into ReplayManager.
// ============================================================================
void __fastcall Detour_ReplayCtrlSync(void* replayCtrl) {
    auto* ctrl = static_cast<uint8_t*>(replayCtrl);
    if (IsValidPtr(ctrl) &&
        VarState::Instance().IsEnabled() &&
        VarState::Instance().IsOfflineAllowed() &&
        ctrl[Offsets::ReplayCtrl_Mode] == 0x0A) { // 0x0A = Interactive Manual Replay

        ReplayDiag::OnManualReplayController(ctrl);
        ReplayControlCommand cmd{};
        if (VarState::Instance().ConsumeReplayCommand(cmd)) {
            uint8_t* player = ctrl + Offsets::ReplayCtrl_PlayerOffset;

            if (cmd.hasSeek && g_fnReplaySetFrame) {
                const uint16_t ringCap  = *reinterpret_cast<const uint16_t*>(ctrl + Offsets::ReplayCtrl_RingCap);
                const uint16_t winStart = *reinterpret_cast<const uint16_t*>(ctrl + Offsets::ReplayCtrl_WinStartFrame);
                const uint16_t winEnd   = *reinterpret_cast<const uint16_t*>(ctrl + Offsets::ReplayCtrl_WinEndFrame);
                if (ringCap > 0 && ringCap <= 4096) {
                    uint16_t targetFrame = ReplayTimelineMath::ComputeFrameFromNorm(winStart, winEnd, ringCap, cmd.targetNorm);
                    bool usedExactRingTime = false;

                    if (cmd.seekByTime && g_replaySingletonPtrAddr) {
                        const uint8_t* singleton = *reinterpret_cast<const uint8_t* const*>(g_replaySingletonPtrAddr);
                        if (IsValidPtr(singleton)) {
                            const uint8_t* holder = *reinterpret_cast<const uint8_t* const*>(
                                singleton + Offsets::ReplaySingleton_RingHolder);
                            if (IsValidPtr(holder)) {
                                const uint8_t* ringBuf = *reinterpret_cast<const uint8_t* const*>(
                                    holder + Offsets::ReplayRingHolder_BufferPtr);
                                const size_t ringBytes = static_cast<size_t>(ringCap) * Offsets::ReplayRing_FrameStride;
                                bool bufOk = (ringBuf == g_verifiedReplayRingBufPtr.load(std::memory_order_acquire) &&
                                              IsValidPtr(ringBuf));
                                if (!bufOk && IsValidPtr(ringBuf) && IsMemoryRangeReadable(ringBuf, ringBytes)) {
                                    g_verifiedReplayRingBufPtr.store(ringBuf, std::memory_order_release);
                                    bufOk = true;
                                }
                                if (bufOk) {
                                    targetFrame = ReplayTimelineMath::FindClosestFrameByTime(
                                        winStart, winEnd, ringCap, cmd.targetPhase, cmd.targetTime60Hz,
                                        [ringBuf](uint16_t f) -> float {
                                            return *reinterpret_cast<const float*>(
                                                ringBuf + static_cast<size_t>(f) * Offsets::ReplayRing_FrameStride +
                                                Offsets::ReplayRing_FrameTimeOff);
                                        },
                                        [ringBuf](uint16_t f) -> int32_t {
                                            return static_cast<int32_t>(
                                                ringBuf[static_cast<size_t>(f) * Offsets::ReplayRing_FrameStride +
                                                        Offsets::ReplayRing_FramePhaseOff] & 0x0FU);
                                        });
                                    usedExactRingTime = true;
                                }
                            }
                        }
                    }

                    if (cmd.pauseAfterSeek) {
                        ctrl[Offsets::ReplayCtrl_IsPlaying] = 0;
                        *reinterpret_cast<float*>(ctrl + Offsets::ReplayCtrl_PlaybackSpeed) = 0.0f;
                    }
                    g_fnReplaySetFrame(player, targetFrame);

                    // Immediately refresh 3D Replay camera LookAtPos at the sought frame (even while paused)
                    if (!cmd.hasFocusTarget && g_fnReplayGetFocusPos) {
                        uint8_t* cam = *reinterpret_cast<uint8_t**>(ctrl + Offsets::ReplayCtrl_CameraPtr);
                        if (IsValidPtr(cam)) {
                            uint8_t* camState = *reinterpret_cast<uint8_t**>(cam + Offsets::ReplayCam_StatePtr);
                            if (IsValidPtr(camState)) {
                                const int32_t curFocus = std::clamp(
                                    *reinterpret_cast<const int32_t*>(ctrl + Offsets::ReplayCtrl_FocusSlotDword), 0, 22);
                                const float* pos = g_fnReplayGetFocusPos(player, curFocus);
                                if (IsValidPtr(pos)) {
                                    std::memcpy(camState + Offsets::ReplayCamState_LookAtPos, pos, 12);
                                }
                            }
                        }
                    }

                    (void)usedExactRingTime;
                }
            }

            if (cmd.hasPlayToggle) {
                ctrl[Offsets::ReplayCtrl_IsPlaying] = cmd.targetPlaying ? 1 : 0;
                *reinterpret_cast<float*>(ctrl + Offsets::ReplayCtrl_PlaybackSpeed) = cmd.targetPlaying ? 1.0f : 0.0f;
            }

            if (cmd.hasFocusTarget) {
                const int32_t target = std::clamp(cmd.focusSlot, 0, 22);
                // If in Free Cam (3), switch back to Follow Cam (0) so camera tracks target cleanly
                if (ctrl[Offsets::ReplayCtrl_CameraMode] == 3) {
                    ctrl[Offsets::ReplayCtrl_CameraMode] = 0;
                }
                ctrl[Offsets::ReplayCtrl_FocusSlotByte] = static_cast<uint8_t>(target);
                *reinterpret_cast<int32_t*>(ctrl + Offsets::ReplayCtrl_FocusSlotDword) = target;

                uint8_t* cam = *reinterpret_cast<uint8_t**>(ctrl + Offsets::ReplayCtrl_CameraPtr);
                if (IsValidPtr(cam)) {
                    uint8_t* camState = *reinterpret_cast<uint8_t**>(cam + Offsets::ReplayCam_StatePtr);
                    if (IsValidPtr(camState)) {
                        if (g_fnReplayGetFocusPos) {
                            const float* pos = g_fnReplayGetFocusPos(player, target);
                            if (IsValidPtr(pos)) {
                                std::memcpy(camState + Offsets::ReplayCamState_LookAtPos, pos, 12);
                            }
                        }
                        *reinterpret_cast<int32_t*>(camState + Offsets::ReplayCamState_FocusTarget) = target;
                    }
                }
            }
        }
    }

    g_hookReplayCtrlSync.Original<FnReplayCtrlSync>()(replayCtrl);
}

// ============================================================================
// Detour: FoulSnapshotPool::Find (0x140A82120)
// ============================================================================
// The foul event consumer 0x1404B4870 (and helpers 0x1404B4B40 / 0x1404B4D90) looks the foul snapshot
// up by the id in FoulInfo.snapshotTickIndex and dereferences the result without a NULL check. Native
// fouls always have their snapshot (the Judge creates it with AddSnapshot 0x140A82070), but a foul
// injected by the VAR can carry an id that the pool the consumer reads no longer holds, which crashed
// at PES2021.exe+0x4B4C25. For those 3 call sites only, a missing snapshot resolves to a zeroed
// stand-in (no positions), exactly like an empty pool slot. Every other caller keeps the native NULL.
__attribute__((noinline)) uint8_t* __fastcall Detour_FoulSnapshotFind(uint8_t* pool, uint32_t id) {
    uint8_t* found = g_hookFoulSnapshotFind.Original<FnFoulSnapshotFind>()(pool, id);
    if (found) return found;
    const uintptr_t ret = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    for (size_t i = 0; i < kUncheckedFindSites; ++i) {
        if (g_uncheckedFindReturnAddrs[i] != 0 && ret == g_uncheckedFindReturnAddrs[i]) {
            std::memset(g_emptyFoulSnapshot, 0, sizeof(g_emptyFoulSnapshot));
            const uint32_t hits = g_foulSnapshotGuardHits.fetch_add(1, std::memory_order_relaxed) + 1;
            if (hits <= 8) {
                VAR_LOG_WRN("Foul snapshot guard: id=%u missing in pool %p, caller 0x%llX got an empty snapshot (hit %u)",
                            id, static_cast<void*>(pool), static_cast<unsigned long long>(ret), hits);
            }
            return g_emptyFoulSnapshot;
        }
    }
    return nullptr;
}

// Validates each unchecked call site (`E8 rel32` -> FoulSnapshotPool_Find) and records its return address.
size_t ArmFoulSnapshotGuardSites(uintptr_t findAddr) {
    const auto imageBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    size_t armed = 0;
    for (size_t i = 0; i < kUncheckedFindSites; ++i) {
        g_uncheckedFindReturnAddrs[i] = 0;
        const uintptr_t site = imageBase + FoulSnapshotUncheckedCallSites::Rvas[i];
        if (!IsMemoryRangeReadable(reinterpret_cast<const void*>(site), 5)) continue;
        const auto* p = reinterpret_cast<const uint8_t*>(site);
        if (p[0] != 0xE8) continue;
        int32_t rel = 0;
        std::memcpy(&rel, p + 1, sizeof(rel));
        if (static_cast<uintptr_t>(static_cast<intptr_t>(site + 5) + rel) != findAddr) continue;
        g_uncheckedFindReturnAddrs[i] = site + 5;
        ++armed;
    }
    return armed;
}

// Resolves `E8 rel32` at `site` and only accepts a target inside this image's executable code
uintptr_t ResolveCallChecked(uintptr_t site, const char* name) {
    if (!site || !IsMemoryRangeReadable(reinterpret_cast<const void*>(site), 5)) return 0;
    if (*reinterpret_cast<const uint8_t*>(site) != 0xE8) {
        VAR_LOG_ERR("%s: expected a call at 0x%llX", name, static_cast<unsigned long long>(site));
        return 0;
    }
    const uintptr_t target = AobScanner::ResolveRipRelative(site, 1, 5);
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const uintptr_t lo = base + sec->VirtualAddress;
        const uintptr_t hi = lo + sec->Misc.VirtualSize;
        if (target >= lo && target < hi) return target;
    }
    VAR_LOG_ERR("%s: call target 0x%llX is outside the executable sections", name, static_cast<unsigned long long>(target));
    return 0;
}

void UninstallLocked() {
    g_hookFoulSnapshotFind.Uninstall();
    g_hookReplayCtrlSync.Uninstall();
    g_hookUpdateFoulCounters.Uninstall();
    g_hookJudgeFinalize.Uninstall();
    g_hookMainTick.Uninstall();
    g_installed.store(false, std::memory_order_release);
    VarState::Instance().SetReplaySeekReady(false);
    VarState::Instance().SetHooksInstalled(false);
}

} // namespace

bool MatchHooks::Install(bool quietOnMiss) {
    std::lock_guard<std::mutex> lock(g_hooksLifecycleMutex);
    if (g_installed.load(std::memory_order_relaxed)) return true;

    auto resolveUnique = [quietOnMiss](const char* name, const char* pat, uintptr_t hintRva) -> uintptr_t {
        const AobMatchResult r = AobScanner::ScanModuleTextWithHint(nullptr, pat, hintRva);
        if (!r.unique()) {
            if (!quietOnMiss) {
                VAR_LOG_ERR("AOB '%s' failed: matchCount=%zu addr=0x%llX",
                            name, r.matchCount, static_cast<unsigned long long>(r.address));
            }
            return 0;
        }
        VAR_LOG_INF("AOB '%s' resolved -> 0x%llX", name, static_cast<unsigned long long>(r.address));
        return r.address;
    };

    const uintptr_t addrMainTick       = resolveUnique("MatchFSM::MainTick", Aobs::MainTick, AobHints::MainTick);
    const uintptr_t addrJudgeFinalize  = resolveUnique("Judge::FinalizeFrame", Aobs::JudgeFinalizeFrame, AobHints::JudgeFinalizeFrame);
    const uintptr_t addrUpdateFoul     = resolveUnique("MatchControl::UpdateFoulCounters", Aobs::UpdateFoulCounters, AobHints::UpdateFoulCounters);
    const uintptr_t addrEnterOutOfPlay = resolveUnique("MatchFSM_EnterOutOfPlay", Aobs::MatchFSM_EnterOutOfPlay, AobHints::MatchFSM_EnterOutOfPlay);
    const uintptr_t addrGetTimeFloat   = resolveUnique("MatchInfo_GetTimeFloat", Aobs::MatchInfo_GetTimeFloat, AobHints::MatchInfo_GetTimeFloat);
    const uintptr_t addrSetOnPitch     = resolveUnique("MatchInfo_SetPlayerOnPitch", Aobs::MatchInfo_SetPlayerOnPitch, AobHints::MatchInfo_SetPlayerOnPitch);
    const uintptr_t addrCommitCards    = resolveUnique("CommitCardsCallSite", Aobs::CommitCardsCallSite, AobHints::CommitCardsCallSite);
    const uintptr_t addrRosterGetter   = resolveUnique("MatchRosterGetter", Aobs::MatchRosterGetter, AobHints::MatchRosterGetter);

    // Native Replay Seek & Camera Focus AOBs
    const uintptr_t addrReplayCtrlSync = resolveUnique("ManualReplayController::SyncToReplayManager",
                                                       Aobs::ReplayCtrl_SyncToManager, AobHints::ReplayCtrl_SyncToManager);
    const uintptr_t addrReplaySetFrame = resolveUnique("ReplayPlayer::SetFrame",
                                                       Aobs::ReplayPlayer_SetFrame, AobHints::ReplayPlayer_SetFrame);
    const uintptr_t addrReplayFocusPos = resolveUnique("ReplayPlayer::GetFocusPos",
                                                       Aobs::ReplayPlayer_GetFocusPos, AobHints::ReplayPlayer_GetFocusPos);
    const uintptr_t addrFoulSnapFind   = resolveUnique("FoulSnapshotPool::Find",
                                                       Aobs::FoulSnapshotPool_Find, AobHints::FoulSnapshotPool_Find);
    const uintptr_t addrRebuildSetPlay = resolveUnique("MatchFSM_RebuildSetPlay",
                                                       Aobs::MatchFSM_RebuildSetPlay, AobHints::MatchFSM_RebuildSetPlay);
    g_fnRebuildSetPlay = reinterpret_cast<FnRebuildSetPlay>(addrRebuildSetPlay);
    const uintptr_t addrSprayReset     = resolveUnique("VanishingSpray native reset",
                                                       Aobs::VanishingSpray_NativeReset, AobHints::VanishingSpray_NativeReset);
    if (addrSprayReset) {
        const uintptr_t gw = ResolveCallChecked(addrSprayReset + 0x14, "SprayRef_GetWrite");
        const uintptr_t rs = ResolveCallChecked(addrSprayReset + 0x21, "VanishingSpray_Reset");
        const uintptr_t cm = ResolveCallChecked(addrSprayReset + 0x2D, "SprayRef_Commit");
        if (gw && rs && cm) { // all three or none: a partial set would publish a half-reset spray
            g_fnSprayRefGetWrite = reinterpret_cast<FnSprayRefGetWrite>(gw);
            g_fnSprayReset       = reinterpret_cast<FnSprayReset>(rs);
            g_fnSprayRefCommit   = reinterpret_cast<FnSprayRefCommit>(cm);
        }
        g_sprayRefVtable     = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + Offsets::VanishingSprayRefVtableRva;
        VAR_LOG_INF("Vanishing spray reset resolved: getWrite=0x%llX reset=0x%llX commit=0x%llX",
                    static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_fnSprayRefGetWrite)),
                    static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_fnSprayReset)),
                    static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_fnSprayRefCommit)));
    } else {
        VAR_LOG_WRN("Vanishing spray reset AOB not found: re-placed free kicks may show a spray stroke");
    }

    // FoulSnapshotPool::Find is critical: without its guard an applied foul can crash the game.
    if (!addrMainTick || !addrJudgeFinalize || !addrUpdateFoul ||
        !addrEnterOutOfPlay || !addrGetTimeFloat ||
        !addrSetOnPitch || !addrCommitCards || !addrFoulSnapFind) {
        if (!quietOnMiss) {
            VAR_LOG_ERR("MatchHooks::Install aborted: one or more critical AOBs did not resolve uniquely");
        }
        return false;
    }

    g_gmatchPtrAddr = AobScanner::ResolveRipRelative(addrMainTick + 0x30, 3, 7);
    VAR_LOG_INF("Resolved GMATCH global ptr -> 0x%llX", static_cast<unsigned long long>(g_gmatchPtrAddr));

    const uintptr_t addrGetPlayerBySlot = ResolveCallChecked(addrCommitCards + 6, "TeamInfo_GetPlayerBySlot");
    VAR_LOG_INF("Resolved TeamInfo_GetPlayerBySlot -> 0x%llX", static_cast<unsigned long long>(addrGetPlayerBySlot));

    if (addrRosterGetter) {
        g_matchRosterPtrAddr = AobScanner::ResolveRipRelative(addrRosterGetter, 3, 7);
        VAR_LOG_INF("Resolved MatchRoster global ptr -> 0x%llX", static_cast<unsigned long long>(g_matchRosterPtrAddr));
    }

    g_fnEnterOutOfPlay    = reinterpret_cast<FnEnterOutOfPlay>(addrEnterOutOfPlay);
    g_fnGetTimeFloat      = reinterpret_cast<FnGetTimeFloat>(addrGetTimeFloat);
    (void)addrSetOnPitch; // on-pitch flags are recomputed natively by state 0x12, never written by the mod
    g_fnGetPlayerBySlot   = reinterpret_cast<FnGetPlayerBySlot>(addrGetPlayerBySlot);
    g_fnReplaySetFrame    = reinterpret_cast<FnReplaySetFrame>(addrReplaySetFrame);
    g_fnReplayGetFocusPos = reinterpret_cast<FnReplayGetFocusPos>(addrReplayFocusPos);

    // Guard first, so no verdict can ever run without it.
    const size_t guardSites = ArmFoulSnapshotGuardSites(addrFoulSnapFind);
    if (guardSites != kUncheckedFindSites) {
        if (!quietOnMiss) {
            VAR_LOG_ERR("FoulSnapshotPool guard: only %zu/%zu call sites validated, aborting hook install",
                        guardSites, kUncheckedFindSites);
        }
        return false;
    }
    // Exact 6-byte prologue: 44 8B C2 (mov r8d, edx) 4C 8B C9 (mov r9, rcx)
    if (!g_hookFoulSnapshotFind.Install(addrFoulSnapFind, reinterpret_cast<void*>(&Detour_FoulSnapshotFind), 6)) {
        VAR_LOG_ERR("Failed to install FoulSnapshotPool::Find guard hook");
        UninstallLocked();
        return false;
    }
    VAR_LOG_INF("FoulSnapshotPool::Find guard installed at 0x%llX (%zu unchecked call sites covered)",
                static_cast<unsigned long long>(addrFoulSnapFind), guardSites);

    if (!g_hookMainTick.Install(addrMainTick, reinterpret_cast<void*>(&Detour_MainTick), 5)) {
        VAR_LOG_ERR("Failed to install MainTick hook");
        UninstallLocked();
        return false;
    }
    if (!g_hookJudgeFinalize.Install(addrJudgeFinalize, reinterpret_cast<void*>(&Detour_JudgeFinalizeFrame), 9)) {
        VAR_LOG_ERR("Failed to install Judge::FinalizeFrame hook");
        UninstallLocked();
        return false;
    }
    if (!g_hookUpdateFoulCounters.Install(addrUpdateFoul, reinterpret_cast<void*>(&Detour_UpdateFoulCounters), 8)) {
        VAR_LOG_ERR("Failed to install MatchControl::UpdateFoulCounters hook");
        UninstallLocked();
        return false;
    }

    bool seekReady = false;
    if (addrReplayCtrlSync && addrReplaySetFrame) {
        // Resolve ReplaySingleton global (0x143703558) from `48 8B 05 disp32` at SyncToReplayManager + 0x32
        const auto* ripInsn = reinterpret_cast<const uint8_t*>(addrReplayCtrlSync + 0x32);
        if (ripInsn[0] == 0x48 && ripInsn[1] == 0x8B && ripInsn[2] == 0x05) {
            g_replaySingletonPtrAddr = AobScanner::ResolveRipRelative(addrReplayCtrlSync + 0x32, 3, 7);
            VAR_LOG_INF("Resolved ReplaySingleton global ptr -> 0x%llX",
                        static_cast<unsigned long long>(g_replaySingletonPtrAddr));
        }

        // Exact 5-byte atomic prologue: 40 53 (push rbx) 55 (push rbp) 56 (push rsi) 57 (push rdi)
        if (g_hookReplayCtrlSync.Install(addrReplayCtrlSync, reinterpret_cast<void*>(&Detour_ReplayCtrlSync), 5)) {
            seekReady = true;
            VAR_LOG_INF("Native ManualReplayController hook installed at 0x%llX (Seek + Play/Pause + Camera Focus ready)",
                        static_cast<unsigned long long>(addrReplayCtrlSync));
        } else {
            VAR_LOG_WRN("Optional ReplayCtrlSync hook failed to install; falling back to passive timeline sync");
        }
    }

    VarState::Instance().SetReplaySeekReady(seekReady);
    g_installed.store(true, std::memory_order_release);
    VarState::Instance().SetHooksInstalled(true);
    VAR_LOG_INF("All MatchHooks installed cleanly (replaySeekReady=%d)", seekReady ? 1 : 0);
    return true;
}

void MatchHooks::Uninstall() {
    std::lock_guard<std::mutex> lock(g_hooksLifecycleMutex);
    UninstallLocked();
    VAR_LOG_INF("MatchHooks uninstalled");
}

bool MatchHooks::IsInstalled() {
    return g_installed.load(std::memory_order_acquire);
}

void MatchHooks::OnMatchReset() {
    g_pollSuspended.store(true, std::memory_order_release);
    g_simResetPending.store(true, std::memory_order_release);
    g_lastMatchFsm.store(nullptr, std::memory_order_relaxed);
    g_lastMatchControl.store(nullptr, std::memory_order_relaxed);
}

void MatchHooks::PollLiveReplayState() {
    if (!g_installed.load(std::memory_order_acquire) || !g_gmatchPtrAddr) return;
    if (g_pollSuspended.load(std::memory_order_acquire)) {
        if (g_pollWasMatchActive || g_pollWasInReplay) {
            g_pollWasMatchActive = false;
            g_pollWasInReplay = false;
            VarState::Instance().PatchLiveReplayTelemetry(
                false, 0, 0, 0, 0, 0, -1, 0,
                0, 0, 0, 0, 0.0f, 0, 0.0f, 0, 0.0f, 0, 0, 0.0f,
                0, 0, 0, 0, 0, -1, -1);
        }
        return;
    }

    uint8_t* gmatch = GetGmatch();
    if (!gmatch) {
        if (!g_pollWasMatchActive) return;
        g_pollWasMatchActive = false;
        g_pollWasInReplay = false;
        g_pollLastGmatch = nullptr;
        g_pollLastMatchInfo = nullptr;
        g_pollLastReplayMgr = nullptr;
        g_pollLastFsm = nullptr;
        VarState::Instance().PatchLiveReplayTelemetry(
            false, 0, 0, 0, 0, 0, -1, 0,
            0, 0, 0, 0, 0.0f, 0, 0.0f, 0, 0.0f, 0, 0, 0.0f,
            0, 0, 0, 0, 0, -1, -1);
        return;
    }

    // Re-checked every frame like the other match objects (one VirtualQuery): the render thread must never
    // read a released GMATCH block whose global was not cleared
    g_pollLastGmatch = nullptr;
    if (gmatch != g_pollLastGmatch) {
        if (!IsMemoryRangeReadable(gmatch, Offsets::GMATCH_ReplayManager + 8)) {
            g_pollLastGmatch = nullptr;
            if (g_pollWasMatchActive) {
                g_pollWasMatchActive = false;
                g_pollWasInReplay = false;
                VarState::Instance().PatchLiveReplayTelemetry(
                    false, 0, 0, 0, 0, 0, -1, 0,
                    0, 0, 0, 0, 0.0f, 0, 0.0f, 0, 0.0f, 0, 0, 0.0f,
                    0, 0, 0, 0, 0, -1, -1);
            }
            return;
        }
        g_pollLastGmatch = gmatch;
    }

    uint8_t* matchInfo = GetMatchInfo(gmatch);
    uint8_t* replayMgr = GetReplayMgr(gmatch);
    if (!matchInfo) return;
    // This runs on the render thread, outside the simulation: the match objects can be released between two
    // frames (scene change after the final whistle) while the pointers still hold the old values. Re-check
    // the pages every frame (3 VirtualQuery calls) instead of trusting a pointer validated once.
    g_pollLastMatchInfo = nullptr;
    g_pollLastReplayMgr = nullptr;
    g_pollLastFsm = nullptr;
    if (matchInfo != g_pollLastMatchInfo) {
        if (!IsMemoryRangeReadable(matchInfo, Offsets::MatchInfo_SpecialModeKind + 1)) {
            g_pollLastMatchInfo = nullptr;
            return;
        }
        g_pollLastMatchInfo = matchInfo;
    }

    if (replayMgr && replayMgr != g_pollLastReplayMgr) {
        if (IsMemoryRangeReadable(replayMgr, Offsets::ReplayMgr_CursorPhase + 4)) {
            g_pollLastReplayMgr = replayMgr;
        } else {
            g_pollLastReplayMgr = nullptr;
        }
    }

    const uint8_t rMode   = (replayMgr && g_pollLastReplayMgr == replayMgr)
        ? *reinterpret_cast<const uint8_t*>(replayMgr + Offsets::ReplayMgr_Mode) : 0;
    const uint8_t rManual = (replayMgr && g_pollLastReplayMgr == replayMgr)
        ? *reinterpret_cast<const uint8_t*>(replayMgr + Offsets::ReplayMgr_ManualScreenFlag) : 0;
    const bool inReplayNow = (rMode != 0 || rManual != 0);

    // Zero-contention fast path during live gameplay: MainTick already updates telemetry on Sim thread!
    if (!inReplayNow && !g_pollWasInReplay && g_pollWasMatchActive && !VarState::Instance().IsDebugOverlayOpen()) {
        return;
    }
    g_pollWasMatchActive = true;
    g_pollWasInReplay = inReplayNow;

    const int32_t matchPhase  = *reinterpret_cast<const int32_t*>(matchInfo + Offsets::MatchInfo_MatchPhase);
    const int32_t matchStatus = *reinterpret_cast<const int32_t*>(matchInfo + Offsets::MatchInfo_MatchStatus);
    const uint8_t specKind    = *reinterpret_cast<const uint8_t*>(matchInfo + Offsets::MatchInfo_SpecialModeKind);

    int32_t fsmState = -1;
    auto* fsm = static_cast<uint8_t*>(g_lastMatchFsm.load(std::memory_order_relaxed));
    if (fsm) {
        if (fsm != g_pollLastFsm) {
            if (IsMemoryRangeReadable(fsm, Offsets::MatchFSM_QueueHead + 1)) {
                g_pollLastFsm = fsm;
            } else {
                g_pollLastFsm = nullptr;
            }
        }
        if (g_pollLastFsm == fsm) {
            fsmState = GetFsmCurrentState(fsm);
        }
    }

    uint8_t rPlaying = 0, rHidden = 0;
    float rCursorTime = 0.0f, rWinStartTime = 0.0f, rWinEndTime = 0.0f, rNorm = 0.0f;
    int32_t rCursorPhase = 0, rWinStartPhase = 0, rWinEndPhase = 0, rFocus = 0;
    if (replayMgr && g_pollLastReplayMgr == replayMgr) {
        rPlaying       = *reinterpret_cast<const uint8_t*>(replayMgr + Offsets::ReplayMgr_IsPlaying);
        rHidden        = *reinterpret_cast<const uint8_t*>(replayMgr + Offsets::ReplayMgr_UiHidden);
        rCursorTime    = *reinterpret_cast<const float*>(replayMgr + Offsets::ReplayMgr_CursorTime);
        rCursorPhase   = *reinterpret_cast<const int32_t*>(replayMgr + Offsets::ReplayMgr_CursorPhase);
        rWinStartTime  = *reinterpret_cast<const float*>(replayMgr + Offsets::ReplayMgr_WindowStartTime);
        rWinStartPhase = *reinterpret_cast<const int32_t*>(replayMgr + Offsets::ReplayMgr_WindowStartPhase);
        rWinEndTime    = *reinterpret_cast<const float*>(replayMgr + Offsets::ReplayMgr_WindowEndTime);
        rWinEndPhase   = *reinterpret_cast<const int32_t*>(replayMgr + Offsets::ReplayMgr_WindowEndPhase);
        rFocus         = *reinterpret_cast<const int32_t*>(replayMgr + Offsets::ReplayMgr_CameraFocusSlot);
        const uint32_t tlPos = *reinterpret_cast<const uint32_t*>(replayMgr + Offsets::ReplayMgr_TimelinePos);
        const uint32_t tlDur = *reinterpret_cast<const uint32_t*>(replayMgr + Offsets::ReplayMgr_TimelineDur);
        rNorm = ReplayTimelineMath::ComputeTimelineNorm(tlPos, tlDur);

        if (rMode != g_pollLoggedReplayMode || rManual != g_pollLoggedManualFlag) {
            g_pollLoggedReplayMode = rMode;
            g_pollLoggedManualFlag = rManual;
            VAR_LOG_INF("Replay transition: mode=%u manual(+0x07)=%u playing=%u pos=%u/%u phase=%d status=%d fsm=0x%X focus=%d t=%.1f",
                        static_cast<unsigned>(rMode), static_cast<unsigned>(rManual),
                        static_cast<unsigned>(rPlaying), tlPos, tlDur,
                        matchPhase, matchStatus, fsmState, rFocus, rCursorTime);
        }
    }

    const auto* sp = reinterpret_cast<const RawSetPlayInfo*>(matchInfo + Offsets::MatchInfo_SetPlayInfo);
    const auto* fi = reinterpret_cast<const RawFoulInfo*>(matchInfo + Offsets::MatchInfo_FoulInfo);

    VarState::Instance().PatchLiveReplayTelemetry(
        true,
        reinterpret_cast<uintptr_t>(gmatch),
        reinterpret_cast<uintptr_t>(matchInfo),
        reinterpret_cast<uintptr_t>(replayMgr),
        matchPhase, matchStatus, fsmState, specKind,
        rMode, rManual, rPlaying, rHidden, rCursorTime, rCursorPhase,
        rWinStartTime, rWinStartPhase, rWinEndTime, rWinEndPhase,
        rFocus, rNorm,
        sp->setPlayKind, sp->benefitedTeam, fi->isValid, fi->foulKind, fi->foulJudgementKind,
        IsValidSlot(fi->offenderSlot) ? fi->offenderSlot : -1,
        IsValidSlot(fi->victimSlot) ? fi->victimSlot : -1);
}

} // namespace var_mod
