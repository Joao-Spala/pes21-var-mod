#include "match_hooks.h"

#include "aob_scanner.h"
#include "game_structs.h"
#include "inline_hook.h"
#include "logger.h"
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
FnSetPlayerOnPitch  g_fnSetPlayerOnPitch  = nullptr;
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
int32_t  g_suppressNextFoulCommitFrames = 0;
// The VAR writes the card into PlayerRecord itself before injecting the foul. The engine's CommitToRegistry
// adds the card of the committed foul again (0x1404231DC..0x140423202) right after UpdateFoulCounters, so
// the committed foul must be neutralised. The frame window above covers the usual same-tick commit. This
// token also covers a commit that comes later (long foul cutscene): it matches only OUR foul, by offender
// and by its unique snapshot id, for at most 15 s of sim ticks.
struct InjectedFoulToken {
    bool     armed     = false;
    int32_t  offender  = -1;
    uint32_t snapId    = 0;
    uint64_t untilTick = 0;
};
InjectedFoulToken g_injectedFoulToken{};
bool     g_forceRosterSyncNextTick = false;
int32_t  g_recentBallOwnerSlot = -1;
uint32_t g_recentBallOwnerTick = 0;
std::atomic<bool> g_simResetPending{ false };
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
std::atomic<const uint8_t*> g_verifiedReplayRingBufPtr{ nullptr };

struct PendingSetPlayInjection {
    bool           active        = false;
    int32_t        ttlFrames     = 0;
    SetPlayKind    spKind        = SetPlayKind::None;
    int32_t        benefitedTeam = 0;
    Vec3           spot{};
    StoppageReason stopReason    = StoppageReason::SilentRestart;
    bool           foulValid     = false;
    CardKind       card          = CardKind::None;
    int32_t        offenderSlot  = -1;
    int32_t        victimSlot    = -1;
    uint32_t       snapId        = 0;
    uint32_t       safeSnapId    = 0;
};
PendingSetPlayInjection g_pendingSetPlayInjection{};
bool                    g_clearFsmJudgeOnNextTick = false;
bool                    g_clearFsmJudgeExpectSilent = false;
SetPlayKind             g_clearFsmJudgeExpectKind = SetPlayKind::None;
uint32_t                g_clearFsmJudgeSafeSnapId = 0;
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

float ReadTeamAttackDir(uint8_t* gmatch, int32_t teamIdx) {
    if (!IsValidPtr(gmatch) || (teamIdx != 0 && teamIdx != 1)) {
        return (teamIdx == 0) ? -1.0f : 1.0f;
    }
    const uint8_t* tactics = *reinterpret_cast<const uint8_t* const*>(
        gmatch + Offsets::GMATCH_TeamTacticsBase + static_cast<size_t>(teamIdx) * Offsets::GMATCH_EntryStride);
    if (!IsValidPtr(tactics)) return (teamIdx == 0) ? -1.0f : 1.0f;
    const float dir = *reinterpret_cast<const float*>(tactics + Offsets::TeamTactics_AttackDir);
    return (dir >= 0.0f) ? 1.0f : -1.0f;
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

uint8_t* GetPlayerRecord(uint8_t* gmatch, int32_t slot) {
    if (!IsValidPtr(gmatch) || !IsValidSlot(slot)) return nullptr;
    uint8_t* teamInfo = GetTeamInfo(gmatch, SlotToTeam(slot));
    if (!teamInfo) return nullptr;
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
    return len > 0;
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
    // Flamengo x Coritiba (BMPES 16.3): Arrascaeta 10, Pedro 9, Rossi 1, Danilo 13, Bruno Henrique 27.
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
            // Edited / BMPES team kits at *(MatchRoster + 0x58) + (teamIdx * 2) * 0x84
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

    // 2. Quick 60Hz scan of PlayerAction flags (22 pointer derefs, < 0.3 us)
    uint8_t actionIds[22] = {};
    uint8_t actionHits[22] = {};
    bool anyPhysicalAction = false;
    for (int slot = 0; slot < 22; ++slot) {
        if (!*reinterpret_cast<const uint8_t*>(matchInfo + Offsets::MatchInfo_PlayerOnPitch + slot)) continue;
        const uint8_t* act = *reinterpret_cast<const uint8_t* const*>(
            gmatch + Offsets::GMATCH_PlayerActionBase + static_cast<size_t>(slot) * Offsets::GMATCH_EntryStride);
        if (!IsValidPtr(act)) continue;
        const uint8_t aid = act[Offsets::PlayerAction_ActionId];
        const uint32_t flags = *reinterpret_cast<const uint32_t*>(act + Offsets::PlayerAction_ActionFlags);
        const uint8_t hit = (flags & PlayerActionId::FlagPhysicalHit) ? 1 : 0;
        actionIds[slot] = aid;
        actionHits[slot] = hit;
        if (hit ||
            aid == PlayerActionId::SlideTackle ||
            aid == PlayerActionId::VictimFall ||
            aid == PlayerActionId::StandTackleA ||
            aid == PlayerActionId::StandTackleB ||
            aid == PlayerActionId::BodyChargeA ||
            aid == PlayerActionId::BodyChargeB) {
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
        snap.playerActionId[slot]  = actionIds[slot];
        snap.playerActionHit[slot] = actionHits[slot];
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

bool SetPlayerCardsAbsolute(uint8_t* gmatch, uint8_t* matchInfo, int32_t slot, uint8_t newYellows, uint8_t newReds) {
    if (!IsValidSlot(slot)) return false;
    uint8_t* rec = GetPlayerRecord(gmatch, slot);
    if (!rec) {
        VAR_LOG_WRN("SetPlayerCardsAbsolute: PlayerRecord null for slot %d", slot);
        return false;
    }

    const bool wasOnPitch = (*reinterpret_cast<const uint8_t*>(matchInfo + Offsets::MatchInfo_PlayerOnPitch + slot) != 0);
    rec[Offsets::PlayerRecord_Yellows] = newYellows;
    rec[Offsets::PlayerRecord_Reds] = newReds;

    const bool shouldBeOnPitch = (newReds == 0 && newYellows < 2);
    if (wasOnPitch == shouldBeOnPitch) {
        // Card count only: never touch the on-pitch flags or PlayerStatus when nothing changes
        VAR_LOG_INF("SetPlayerCardsAbsolute slot=%d -> Y=%u R=%u (on-pitch unchanged=%d)",
                    slot, static_cast<unsigned>(newYellows), static_cast<unsigned>(newReds), wasOnPitch ? 1 : 0);
        return false;
    }
    if (g_fnSetPlayerOnPitch) {
        g_fnSetPlayerOnPitch(matchInfo, static_cast<uint32_t>(slot), shouldBeOnPitch ? 1 : 0);
    } else {
        *reinterpret_cast<uint8_t*>(matchInfo + Offsets::MatchInfo_PlayerOnPitch + slot) = shouldBeOnPitch ? 1 : 0;
    }
    if (uint8_t* writeBuf = GetMatchInfoWriteBuffer(matchInfo)) {
        *reinterpret_cast<uint8_t*>(writeBuf + Offsets::MatchInfo_PlayerOnPitch + slot) = shouldBeOnPitch ? 1 : 0;
    }

    uint8_t* statusArr = *reinterpret_cast<uint8_t**>(gmatch + Offsets::GMATCH_PlayerStatus);
    if (IsValidPtr(statusArr)) {
        for (size_t i = 0; i < 24; ++i) {
            uint8_t* st = statusArr + i * Offsets::PlayerStatus_Stride;
            const int32_t stSlot = *reinterpret_cast<const int32_t*>(st + Offsets::PlayerStatus_Slot);
            if (stSlot == slot) {
                *reinterpret_cast<uint8_t*>(st + Offsets::PlayerStatus_OffPitch) = shouldBeOnPitch ? 0 : 1;
            }
        }
    }

    VAR_LOG_INF("SetPlayerCardsAbsolute slot=%d -> Y=%u R=%u onPitch=%d (was=%d)",
                slot, static_cast<unsigned>(newYellows), static_cast<unsigned>(newReds),
                shouldBeOnPitch ? 1 : 0, wasOnPitch ? 1 : 0);
    return wasOnPitch != shouldBeOnPitch;
}

bool ApplyCardDelta(uint8_t* gmatch, uint8_t* matchInfo, int32_t slot, CardKind origCard, CardKind targetCard) {
    if (!IsValidSlot(slot)) return false;
    uint8_t* rec = GetPlayerRecord(gmatch, slot);
    if (!rec) {
        VAR_LOG_WRN("ApplyCardDelta: PlayerRecord null for slot %d", slot);
        return false;
    }

    uint8_t curY = rec[Offsets::PlayerRecord_Yellows];
    uint8_t curR = rec[Offsets::PlayerRecord_Reds];

    if (origCard == CardKind::Yellow) {
        if (curY >= 2 && curR > 0) --curR;
        if (curY > 0) --curY;
    } else if (origCard == CardKind::Red) {
        if (curR > 0) --curR;
    }

    if (targetCard == CardKind::Yellow) {
        ++curY;
        if (curY >= 2 && curR == 0) curR = 1;
    } else if (targetCard == CardKind::Red) {
        if (curR == 0) curR = 1;
    }

    return SetPlayerCardsAbsolute(gmatch, matchInfo, slot, curY, curR);
}

bool ApplyVerdictCards(uint8_t* gmatch, uint8_t* matchInfo, const VarVerdict& v) {
    if (v.directCardEdit) {
        return SetPlayerCardsAbsolute(gmatch, matchInfo, v.offenderSlot, v.directYellows, v.directReds);
    }

    const bool hasOrigOffender = IsValidSlot(v.origOffenderSlot);
    const bool movedPlayer = hasOrigOffender && IsValidSlot(v.offenderSlot) && (v.origOffenderSlot != v.offenderSlot);
    if (movedPlayer) {
        bool expChanged = false;
        if (v.origCard == CardKind::Yellow || v.origCard == CardKind::Red) {
            expChanged |= ApplyCardDelta(gmatch, matchInfo, v.origOffenderSlot, v.origCard, CardKind::None);
        }
        if (v.targetCard == CardKind::Yellow || v.targetCard == CardKind::Red) {
            expChanged |= ApplyCardDelta(gmatch, matchInfo, v.offenderSlot, CardKind::None, v.targetCard);
        }
        return expChanged;
    }

    if (v.targetCard != v.origCard) {
        return ApplyCardDelta(gmatch, matchInfo, v.offenderSlot, v.origCard, v.targetCard);
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
void WriteSetPlayAndFoulToStruct(uint8_t* baseSetPlay, uint8_t* baseFoul,
                                 SetPlayKind spKind, int32_t benefitedTeam, const Vec3& pos,
                                 StoppageReason stopReason, bool foulValid,
                                 CardKind card, int32_t offenderSlot, int32_t victimSlot,
                                 uint32_t snapshotTickIndex, uint32_t safeFallbackSnapId) {
    auto* sp = reinterpret_cast<RawSetPlayInfo*>(baseSetPlay);
    std::memset(sp, 0, sizeof(RawSetPlayInfo));
    sp->setPlayKind     = static_cast<int32_t>(spKind);
    sp->benefitedTeam   = (benefitedTeam == 1) ? 1 : 0;
    sp->posA            = pos;
    sp->posB            = pos;
    sp->stoppageReason  = static_cast<int32_t>(stopReason);
    sp->eventPlayerSlot = IsValidSlot(offenderSlot) ? static_cast<uint32_t>(offenderSlot) : 0xFFU;

    auto* fi = reinterpret_cast<RawFoulInfo*>(baseFoul);
    std::memset(fi, 0, sizeof(RawFoulInfo));
    fi->isValid           = foulValid ? 1 : 0;
    fi->foulKind          = foulValid ? static_cast<int32_t>(FoulKind::StandardFoul) : 0;
    fi->foulJudgementKind = foulValid
        ? (card == CardKind::Yellow ? 3 : card == CardKind::Red ? 4 : 1)
        : 0;
    fi->offenderSlot      = IsValidSlot(offenderSlot) ? offenderSlot : 0xFF;
    fi->victimSlot        = IsValidSlot(victimSlot) ? victimSlot : 0xFF;
    // Every FoulInfo must keep a valid snapshot id in the pool so 0x1404B4B40 -> 0x140A82120 never returns NULL
    fi->snapshotTickIndex = foulValid ? snapshotTickIndex : safeFallbackSnapId;
}

PendingSetPlayInjection BuildSetPlayInjection(uint8_t* fsm, uint8_t* matchInfo, uint8_t* gmatch,
                                              SetPlayKind spKind, int32_t benefitedTeam, const Vec3& pos,
                                              StoppageReason stopReason, bool foulValid,
                                              CardKind card, int32_t offenderSlot, int32_t victimSlot) {
    PendingSetPlayInjection req{};
    req.active        = true;
    req.ttlFrames     = 3;
    req.spKind        = spKind;
    req.benefitedTeam = (benefitedTeam == 1) ? 1 : 0;
    req.spot          = pos;
    req.stopReason    = stopReason;
    req.foulValid     = foulValid;
    req.card          = card;
    req.offenderSlot  = offenderSlot;
    req.victimSlot    = victimSlot;

    req.safeSnapId = EnsureValidFoulSnapshotId(matchInfo, gmatch, fsm);
    req.snapId     = foulValid ? req.safeSnapId : 0U;
    return req;
}

void WriteSetPlayToMatchInfoBuffers(uint8_t* matchInfo, const PendingSetPlayInjection& req) {
    if (!IsValidPtr(matchInfo)) return;
    WriteSetPlayAndFoulToStruct(
        matchInfo + Offsets::MatchInfo_SetPlayInfo,
        matchInfo + Offsets::MatchInfo_FoulInfo,
        req.spKind, req.benefitedTeam, req.spot, req.stopReason,
        req.foulValid, req.card, req.offenderSlot, req.victimSlot,
        req.snapId, req.safeSnapId);
    if (uint8_t* writeBuf = GetMatchInfoWriteBuffer(matchInfo)) {
        WriteSetPlayAndFoulToStruct(
            writeBuf + Offsets::MatchInfo_SetPlayInfo,
            writeBuf + Offsets::MatchInfo_FoulInfo,
            req.spKind, req.benefitedTeam, req.spot, req.stopReason,
            req.foulValid, req.card, req.offenderSlot, req.victimSlot,
            req.snapId, req.safeSnapId);
    }
}

void InjectSetPlayIntoFsmJudgeResult(uint8_t* fsm, uint8_t* matchInfo, const PendingSetPlayInjection& req) {
    WriteSetPlayToMatchInfoBuffers(matchInfo, req);
    WriteSetPlayAndFoulToStruct(
        fsm + Offsets::MatchFSM_JudgeResult,
        fsm + Offsets::MatchFSM_JudgeFoulInfo,
        req.spKind, req.benefitedTeam, req.spot, req.stopReason,
        req.foulValid, req.card, req.offenderSlot, req.victimSlot,
        req.snapId, req.safeSnapId);
    // Prevent quick-restart scatter and CommitToRegistry out-of-bounds clamping (+0x27FC is 1-byte uint8_t)
    *reinterpret_cast<uint8_t*>(fsm + Offsets::MatchFSM_QuickRestartFlag) = 0;
    *reinterpret_cast<uint64_t*>(fsm + Offsets::MatchFSM_KickerCache) = 0x000000FF000000FFULL;
    g_suppressNextFoulCommitFrames = 6;
    if (req.foulValid && req.snapId != 0 && IsValidSlot(req.offenderSlot)) {
        g_injectedFoulToken.armed = true;
        g_injectedFoulToken.offender = req.offenderSlot;
        g_injectedFoulToken.snapId = req.snapId;
        g_injectedFoulToken.untilTick = g_mainTickCount + 900;
    }
    // Clear fsm + 0x49C0 on the NEXT tick (after CommitToRegistry commits Tick T) because SilentRestart (7)
    // skips State_23, so leaving JudgeResult non-zero would re-trigger CommitToRegistry on every OutOfPlay tick.
    g_clearFsmJudgeOnNextTick = true;
    g_clearFsmJudgeSafeSnapId = req.safeSnapId;
    g_clearFsmJudgeExpectSilent = (req.stopReason == StoppageReason::SilentRestart);
    g_clearFsmJudgeExpectKind = req.spKind;
}

void ExecuteDirectEnterOutOfPlay(uint8_t* fsm, uint8_t* matchInfo, const PendingSetPlayInjection& req) {
    InjectSetPlayIntoFsmJudgeResult(fsm, matchInfo, req);
    if (g_fnEnterOutOfPlay) {
        g_fnEnterOutOfPlay(fsm);
    }
    // EnterOutOfPlay sets QuickRestartFlag = 1 when spKind == 5: clear it back to 0 for formal set-piece placement
    *reinterpret_cast<uint8_t*>(fsm + Offsets::MatchFSM_QuickRestartFlag) = 0;
}

void TryExecuteVerdictPoolInMainTick(uint8_t* fsm, uint8_t* gmatch, uint8_t* matchInfo, uint8_t* replayMgr, int32_t fsmState) {
    if (g_clearFsmJudgeOnNextTick) {
        g_clearFsmJudgeOnNextTick = false;
        // Only SilentRestart skips State_23 (the state that consumes JudgeResult), so only that injection
        // needs the manual clear. And only if JudgeResult still holds exactly what was injected: never wipe
        // a result the engine itself wrote during the last tick.
        const auto* jr = reinterpret_cast<const RawSetPlayInfo*>(fsm + Offsets::MatchFSM_JudgeResult);
        const bool stillOurs = g_clearFsmJudgeExpectSilent &&
                               jr->setPlayKind == static_cast<int32_t>(g_clearFsmJudgeExpectKind) &&
                               jr->stoppageReason == static_cast<int32_t>(StoppageReason::SilentRestart);
        if (stillOurs) {
            const uint32_t safeId = EnsureValidFoulSnapshotId(matchInfo, gmatch, fsm);
            WriteSetPlayAndFoulToStruct(
                fsm + Offsets::MatchFSM_JudgeResult,
                fsm + Offsets::MatchFSM_JudgeFoulInfo,
                SetPlayKind::None, 0, Vec3{}, StoppageReason::InitOrKickOff,
                false, CardKind::None, -1, -1, 0U, safeId ? safeId : g_clearFsmJudgeSafeSnapId);
            *reinterpret_cast<uint8_t*>(fsm + Offsets::MatchFSM_QuickRestartFlag) = 0;
        }
    }

    // Failsafe if a queued Judge::FinalizeFrame injection did not fire within its TTL (e.g. early branch in 0x1A).
    // Only in the two states where the injection is valid: after a goal, half-time or a replay the request is
    // dropped instead of forcing EnterOutOfPlay into an unrelated state.
    if (g_pendingSetPlayInjection.active) {
        if (--g_pendingSetPlayInjection.ttlFrames <= 0) {
            g_pendingSetPlayInjection.active = false;
            if ((fsmState == 0x1A || fsmState == 0x1C) && IsMatchEligibleForExecution(gmatch, matchInfo, replayMgr)) {
                VAR_LOG_WRN("PendingSetPlayInjection TTL expired in fsmState=0x%X, executing direct EnterOutOfPlay fallback", fsmState);
                PendingSetPlayInjection req = g_pendingSetPlayInjection;
                const uint32_t id = EnsureValidFoulSnapshotId(matchInfo, gmatch, fsm);
                if (id) { req.safeSnapId = id; req.snapId = req.foulValid ? id : 0U; }
                ExecuteDirectEnterOutOfPlay(fsm, matchInfo, req);
            } else {
                VAR_LOG_WRN("PendingSetPlayInjection dropped: fsmState=0x%X is not InPlay/SetPlayReady anymore", fsmState);
                VarState::Instance().SetPostApplyBanner("DECISÃO NÃO APLICADA  ·  O lance já mudou", 4800, BannerStyle::Amber);
            }
        }
        return;
    }

    if (!VarState::Instance().HasArmedVerdict()) return;
    if (!IsMatchEligibleForExecution(gmatch, matchInfo, replayMgr)) return;

    const bool inPlay = (fsmState == 0x1C);
    const bool inSetPlayReady = (fsmState == 0x1A);
    const bool inCutscene = (fsmState >= 0x03 && fsmState <= 0x11);
    if (!inPlay && !inSetPlayReady && !inCutscene) {
        return;
    }

    VerdictPool pool{};
    if (!VarState::Instance().ConsumeVerdictPool(pool)) return;

    VAR_LOG_INF("Executing VAR VerdictPool seq=%u count=%zu hasPlay=%d cardCount=%zu fsmState=0x%X",
                pool.sequence, pool.Count(), pool.hasPlayDecision ? 1 : 0, pool.CardCount(), fsmState);
    Logger::Flush();

    struct PoolStep {
        bool     isPlay   = false;
        uint32_t order    = 0;
        int      cardSlot = -1;
    };
    std::array<PoolStep, kMaxCardPoolEntries + 1> steps{};
    size_t stepCount = 0;
    if (pool.hasPlayDecision) {
        steps[stepCount++] = PoolStep{ true, pool.playOrder, -1 };
    }
    for (int s = 0; s < static_cast<int>(kMaxCardPoolEntries); ++s) {
        if (pool.cardEntries[static_cast<size_t>(s)].active) {
            steps[stepCount++] = PoolStep{ false, pool.cardEntries[static_cast<size_t>(s)].order, s };
        }
    }
    std::sort(steps.begin(), steps.begin() + stepCount, [](const PoolStep& a, const PoolStep& b) {
        return a.order < b.order;
    });

    // Check if a later non-direct EditCard on the exact same official event supersedes playDecision's card delta
    bool playCardSuperseded = false;
    CardKind supersedingCard = CardKind::None;
    if (pool.hasPlayDecision && pool.playDecision.sourceEventId != 0) {
        for (const auto& ce : pool.cardEntries) {
            if (ce.active && !ce.verdict.directCardEdit &&
                ce.verdict.sourceEventId == pool.playDecision.sourceEventId &&
                ce.order > pool.playOrder) {
                playCardSuperseded = true;
                supersedingCard = ce.verdict.targetCard;
            }
        }
    }

    bool anyExpulsionChanged = false;
    bool anyRedApplied = false;

    // 1. Apply all card mutations in exact chronological order BEFORE Original(MainTick) copies PlayerOnPitch
    for (size_t i = 0; i < stepCount; ++i) {
        if (steps[i].isPlay) {
            const VarVerdict& pv = pool.playDecision;
            VAR_LOG_INF("  Pool[play] seq=%u action=%d origOff=%d off=%d vic=%d team=%d origCard=%d targetCard=%d",
                        pv.sequence, static_cast<int>(pv.action), pv.origOffenderSlot, pv.offenderSlot,
                        pv.victimSlot, pv.benefitedTeam, static_cast<int>(pv.origCard), static_cast<int>(pv.targetCard));
            if (playCardSuperseded) continue;

            if (pv.action == VarAction::OverturnCancel ||
                pv.action == VarAction::RestartCorner ||
                pv.action == VarAction::RestartGoalKick ||
                pv.action == VarAction::RestartThrowIn) {
                const int32_t cardSlot = IsValidSlot(pv.origOffenderSlot) ? pv.origOffenderSlot : pv.offenderSlot;
                if (pv.origCard == CardKind::Yellow || pv.origCard == CardKind::Red) {
                    anyExpulsionChanged |= ApplyCardDelta(gmatch, matchInfo, cardSlot, pv.origCard, CardKind::None);
                }
            } else if (pv.action == VarAction::ConvertToPenalty ||
                       pv.action == VarAction::RetroactivePenalty ||
                       pv.action == VarAction::ConvertToFreeKick ||
                       pv.action == VarAction::RetroactiveFreeKick) {
                anyExpulsionChanged |= ApplyVerdictCards(gmatch, matchInfo, pv);
                if (pv.targetCard == CardKind::Red) anyRedApplied = true;
            }
        } else {
            const VarVerdict& cv = pool.cardEntries[static_cast<size_t>(steps[i].cardSlot)].verdict;
            VAR_LOG_INF("  Pool[card entry=%d] seq=%u direct=%d Y=%u R=%u origCard=%d targetCard=%d",
                        steps[i].cardSlot, cv.sequence, cv.directCardEdit ? 1 : 0,
                        static_cast<unsigned>(cv.directYellows), static_cast<unsigned>(cv.directReds),
                        static_cast<int>(cv.origCard), static_cast<int>(cv.targetCard));
            anyExpulsionChanged |= ApplyVerdictCards(gmatch, matchInfo, cv);
            if (cv.targetCard == CardKind::Red || (cv.directCardEdit && cv.directReds > 0)) {
                anyRedApplied = true;
            }
        }
    }

    // 2. Execute SetPlay / Foul state transition (at most ONE play decision per Pool)
    if (pool.hasPlayDecision) {
        const VarVerdict& pv = pool.playDecision;
        const PitchDims dims = ReadPitchDims(gmatch);
        const int32_t offenderTeam = IsValidSlot(pv.offenderSlot) ? SlotToTeam(pv.offenderSlot) : (1 - pv.benefitedTeam);
        const float offenderAttackDir = ReadTeamAttackDir(gmatch, offenderTeam);

        // Reflect final card on the play offender if a later card entry in the pool also modified this slot
        CardKind effectivePlayCard = playCardSuperseded ? supersedingCard : pv.targetCard;
        if (IsValidSlot(pv.offenderSlot)) {
            const CardPoolEntry* latest = pool.FindCardForSlot(pv.offenderSlot);
            if (latest && latest->order > pool.playOrder) {
                const CardPoolEntry& ce = *latest;
                effectivePlayCard = ce.verdict.directCardEdit
                    ? (ce.verdict.directReds > 0 ? CardKind::Red : (ce.verdict.directYellows > 0 ? CardKind::Yellow : CardKind::Simple))
                    : ce.verdict.targetCard;
            }
        }

        bool wantTransition = false;
        SetPlayKind spKind = SetPlayKind::FreeKick;
        int32_t benefitedTeam = (pv.benefitedTeam == 1) ? 1 : 0;
        Vec3 spot{};
        StoppageReason stopReason = StoppageReason::SilentRestart;
        bool foulValid = false;
        int32_t effOff = pv.offenderSlot;
        int32_t effVic = pv.victimSlot;

        switch (pv.action) {
            case VarAction::ConvertToPenalty:
            case VarAction::RetroactivePenalty:
                spKind        = SetPlayKind::PenaltyKick;
                spot          = PitchGeometry::CalcPenaltySpot(dims, offenderAttackDir);
                stopReason    = inPlay ? StoppageReason::Foul : StoppageReason::SilentRestart;
                foulValid     = true;
                wantTransition = true;
                break;

            case VarAction::ConvertToFreeKick:
            case VarAction::RetroactiveFreeKick:
                spKind        = SetPlayKind::FreeKick;
                spot          = PitchGeometry::ProjectOutsideDefendingBox(dims, pv.spotPos, offenderAttackDir, 0.7f);
                stopReason    = inPlay ? StoppageReason::Foul : StoppageReason::SilentRestart;
                foulValid     = true;
                wantTransition = true;
                break;

            case VarAction::OverturnCancel:
                if (inSetPlayReady || inCutscene) {
                    const auto curSp = *reinterpret_cast<const RawSetPlayInfo*>(matchInfo + Offsets::MatchInfo_SetPlayInfo);
                    const bool wasPenalty = (curSp.setPlayKind == static_cast<int32_t>(SetPlayKind::PenaltyKick)) ||
                                            PitchGeometry::IsInsideDefendingBox(dims, pv.spotPos, offenderAttackDir);
                    benefitedTeam = (pv.benefitedTeam == 0 || pv.benefitedTeam == 1)
                        ? pv.benefitedTeam
                        : (1 - curSp.benefitedTeam);
                    spKind        = wasPenalty ? SetPlayKind::GoalKick : SetPlayKind::FreeKick;
                    spot          = wasPenalty
                        ? PitchGeometry::CalcGoalKickSpot(dims, ReadTeamAttackDir(gmatch, benefitedTeam), pv.spotPos.z)
                        : PitchGeometry::ClampToPitch(dims, pv.spotPos);
                    stopReason    = StoppageReason::SilentRestart;
                    foulValid     = false;
                    effOff        = -1;
                    effVic        = -1;
                    wantTransition = true;
                }
                break;

            case VarAction::RestartCorner:
            case VarAction::RestartGoalKick:
            case VarAction::RestartThrowIn:
                if (pv.action == VarAction::RestartCorner) {
                    spKind = SetPlayKind::CornerKick;
                    spot   = PitchGeometry::CalcCornerSpot(dims, ReadTeamAttackDir(gmatch, 1 - benefitedTeam), pv.spotPos.z);
                } else if (pv.action == VarAction::RestartGoalKick) {
                    spKind = SetPlayKind::GoalKick;
                    spot   = PitchGeometry::CalcGoalKickSpot(dims, ReadTeamAttackDir(gmatch, benefitedTeam), pv.spotPos.z);
                } else {
                    spKind = SetPlayKind::ThrowIn;
                    spot   = PitchGeometry::CalcThrowInSpot(dims, pv.spotPos);
                }
                stopReason     = StoppageReason::SilentRestart;
                foulValid      = false;
                effOff         = -1;
                effVic         = -1;
                wantTransition = true;
                break;

            default:
                break;
        }

        if (wantTransition) {
            const PendingSetPlayInjection req = BuildSetPlayInjection(
                fsm, matchInfo, gmatch, spKind, benefitedTeam, spot, stopReason,
                foulValid, effectivePlayCard, effOff, effVic);
            g_suppressNextFoulCommitFrames = 5;
            if (inPlay || inSetPlayReady) {
                // Both State_InPlay (0x140449F40) and State_SetPlayReady (0x14044DD50) run Judge::FinalizeFrame
                // (0x14050CD70) and immediately transition via MatchFSM_EnterOutOfPlay + CommitToRegistry RCU sync!
                g_pendingSetPlayInjection = req;
            } else {
                ExecuteDirectEnterOutOfPlay(fsm, matchInfo, req);
            }
        }

        // If this was a retroactive foul/penalty on an uncalled play, record it directly as reviewedByVar
        if (pv.sourceEventId == 0 && foulValid) {
            VarEvent revEv{};
            revEv.type          = (spKind == SetPlayKind::PenaltyKick) ? VarEventType::Penalty : VarEventType::Foul;
            revEv.matchPhase    = *reinterpret_cast<const int32_t*>(matchInfo + Offsets::MatchInfo_MatchPhase);
            revEv.time60Hz      = GetMatchTime60Hz(matchInfo);
            revEv.matchTick     = *reinterpret_cast<const uint32_t*>(matchInfo + Offsets::MatchInfo_MatchTick);
            revEv.setPlayKind   = spKind;
            revEv.benefitedTeam = benefitedTeam;
            revEv.offenderSlot  = effOff;
            revEv.victimSlot    = effVic;
            revEv.cardKind      = effectivePlayCard;
            revEv.pos           = spot;
            revEv.reviewedByVar = true;
            std::snprintf(revEv.varNote, sizeof(revEv.varNote), "%s",
                          (spKind == SetPlayKind::PenaltyKick) ? "Pênalti pelo VAR" : "Falta pelo VAR");
            if (IsValidSlot(effOff)) {
                if (uint8_t* rec = GetPlayerRecord(gmatch, effOff)) {
                    revEv.offenderYellowsAfter = rec[Offsets::PlayerRecord_Yellows];
                    revEv.offenderRedsAfter    = rec[Offsets::PlayerRecord_Reds];
                }
            }
            VarState::Instance().PushEvent(revEv);
        }
    } else {
        // Cards-only Pool: keep active MatchInfo->FoulInfo (both RCU buffers) consistent if an edited player is the active offender
        auto syncActiveFoulBuffer = [&](uint8_t* miBase) {
            if (!IsValidPtr(miBase)) return;
            auto* miFoul = reinterpret_cast<RawFoulInfo*>(miBase + Offsets::MatchInfo_FoulInfo);
            if (!miFoul->isValid) return;
            // Chronological order, and only entries about the player of the active foul: editing the card of
            // an older event must never rewrite the offender or card of the foul being taken now.
            for (size_t i = 0; i < stepCount; ++i) {
                if (steps[i].isPlay) continue;
                const VarVerdict& cv = pool.cardEntries[static_cast<size_t>(steps[i].cardSlot)].verdict;
                const int32_t aboutSlot = (!cv.directCardEdit && IsValidSlot(cv.origOffenderSlot))
                    ? cv.origOffenderSlot : cv.offenderSlot;
                const bool matchesFoul = IsValidSlot(aboutSlot) && miFoul->offenderSlot == aboutSlot;
                if (matchesFoul) {
                    const CardKind effCard = cv.directCardEdit
                        ? (cv.directReds > 0 ? CardKind::Red : (cv.directYellows > 0 ? CardKind::Yellow : CardKind::Simple))
                        : cv.targetCard;
                    miFoul->foulJudgementKind = (effCard == CardKind::Yellow) ? 3 : (effCard == CardKind::Red) ? 4 : 1;
                    if (!cv.directCardEdit && IsValidSlot(cv.offenderSlot)) {
                        miFoul->offenderSlot = cv.offenderSlot;
                    }
                }
            }
        };
        syncActiveFoulBuffer(matchInfo);
        syncActiveFoulBuffer(GetMatchInfoWriteBuffer(matchInfo));
        if (inCutscene) {
            g_suppressNextFoulCommitFrames = 5;
            auto* miFoul = reinterpret_cast<const RawFoulInfo*>(matchInfo + Offsets::MatchInfo_FoulInfo);
            auto* fsmFoul = reinterpret_cast<RawFoulInfo*>(fsm + Offsets::MatchFSM_JudgeFoulInfo);
            if (miFoul->isValid && fsmFoul->isValid) {
                fsmFoul->foulJudgementKind = miFoul->foulJudgementKind;
                fsmFoul->offenderSlot      = miFoul->offenderSlot;
            }
        }

        if (anyExpulsionChanged && (inSetPlayReady || inCutscene)) {
            const auto curSp = *reinterpret_cast<const RawSetPlayInfo*>(matchInfo + Offsets::MatchInfo_SetPlayInfo);
            const auto curFi = *reinterpret_cast<const RawFoulInfo*>(matchInfo + Offsets::MatchInfo_FoulInfo);
            const CardKind curCard = (curFi.foulJudgementKind == 4) ? CardKind::Red :
                                     (curFi.foulJudgementKind == 3) ? CardKind::Yellow : CardKind::Simple;
            const SetPlayKind spKind = curSp.setPlayKind
                ? static_cast<SetPlayKind>(curSp.setPlayKind)
                : SetPlayKind::FreeKick;
            const PendingSetPlayInjection req = BuildSetPlayInjection(
                fsm, matchInfo, gmatch, spKind, curSp.benefitedTeam, curSp.posA,
                StoppageReason::SilentRestart, curFi.isValid != 0, curCard,
                IsValidSlot(curFi.offenderSlot) ? curFi.offenderSlot : -1,
                IsValidSlot(curFi.victimSlot) ? curFi.victimSlot : -1);
            g_suppressNextFoulCommitFrames = 5;
            if (inSetPlayReady) {
                g_pendingSetPlayInjection = req;
            } else {
                ExecuteDirectEnterOutOfPlay(fsm, matchInfo, req);
            }
        }
    }

    // 3. Update event ledger, intervention counter, live roster cards & broadcast banner
    VarState::Instance().IncrementInterventions();
    if (pool.hasPlayDecision) {
        VarState::Instance().UpdateEventAfterVerdict(pool.playDecision);
    }
    for (size_t i = 0; i < stepCount; ++i) {
        if (!steps[i].isPlay) {
            VarState::Instance().UpdateEventAfterVerdict(pool.cardEntries[static_cast<size_t>(steps[i].cardSlot)].verdict);
        }
    }
    SyncPlayersAndCards(gmatch, matchInfo);

    const bool isOverturnOrRestart = pool.hasPlayDecision &&
        (pool.playDecision.action == VarAction::OverturnCancel ||
         pool.playDecision.action == VarAction::RestartCorner ||
         pool.playDecision.action == VarAction::RestartGoalKick ||
         pool.playDecision.action == VarAction::RestartThrowIn);
    const BannerStyle bStyle = anyRedApplied ? BannerStyle::RedCard
                             : isOverturnOrRestart ? BannerStyle::Amber
                             : BannerStyle::Applied;

    char bannerBuf[128] = {};
    pool.FormatCombinedSummary(bannerBuf, sizeof(bannerBuf));
    VarState::Instance().SetPostApplyBanner(
        bannerBuf[0] ? bannerBuf : "DECISÃO DO VAR APLICADA", 4800, bStyle);
    VAR_LOG_INF("VAR VerdictPool seq=%u applied cleanly", pool.sequence);
    Logger::Flush();
}

void ResetSimTrackersOnSimThread() {
    g_lastSnapshotTick = 0;
    g_lastSeenMatchTick = 0;
    g_lastAdvActive = false;
    g_lastFoulValidEdge = false;
    g_lastFoulOffenderEdge = -1;
    g_lastFoulCardEdge = -1;
    g_lastFoulPhaseEdge = -1;
    g_lastFoulTimeEdge = -1000.0f;
    g_suppressNextFoulCommitFrames = 0;
    g_injectedFoulToken = InjectedFoulToken{};
    g_forceRosterSyncNextTick = false;
    g_recentBallOwnerSlot = -1;
    g_recentBallOwnerTick = 0;
    g_verifiedRosterDataPtr = nullptr;
    g_verifiedEditedKitPtr = nullptr;
    g_verifiedBallStatePtr = nullptr;
    g_verifiedMatchInfoWriteBuf = nullptr;
    g_pendingSetPlayInjection = PendingSetPlayInjection{};
    g_clearFsmJudgeOnNextTick = false;
    g_clearFsmJudgeSafeSnapId = 0;
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
            VarState::Instance().ResetMatch();
            ResetSimTrackersOnSimThread();
        }
        g_lastSeenMatchTick = matchTick;

        const int32_t fsmState = GetFsmCurrentState(fsm);
        TryExecuteVerdictPoolInMainTick(fsm, gmatch, matchInfo, replayMgr, fsmState);

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
            uint8_t* ball = *reinterpret_cast<uint8_t**>(gmatch + Offsets::GMATCH_Ball);
            if (IsValidPtr(ball)) {
                std::memcpy(&ev.pos, ball + Offsets::Ball_PosVec3, sizeof(Vec3));
            }
            VarState::Instance().PushEvent(ev);
        }
        g_lastAdvActive = advNow;

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

    const uint32_t ret = g_hookMainTick.Original<FnMainTick>()(matchFsm, runCtx, arg3);

    if (g_suppressNextFoulCommitFrames > 0) {
        --g_suppressNextFoulCommitFrames;
    }

    return ret;
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

    if (!g_pendingSetPlayInjection.active || !IsValidPtr(judge)) {
        return;
    }

    auto* judgeBytes = static_cast<uint8_t*>(judge);
    auto* expectedFsm = static_cast<uint8_t*>(g_lastMatchFsm.load(std::memory_order_relaxed));
    uint8_t* fsm = judgeBytes - Offsets::MatchFSM_EmbeddedJudge;
    if (!IsValidPtr(expectedFsm) || fsm != expectedFsm) {
        return;
    }

    const int32_t fsmState = GetFsmCurrentState(fsm);
    if (fsmState != 0x1C && fsmState != 0x1A) {
        return;
    }

    uint8_t* gmatch = GetGmatch();
    uint8_t* matchInfo = GetMatchInfo(gmatch);
    if (!IsValidPtr(matchInfo)) {
        return;
    }

    PendingSetPlayInjection req = g_pendingSetPlayInjection;
    g_pendingSetPlayInjection.active = false;
    g_pendingSetPlayInjection.ttlFrames = 0;

    // Re-resolve the snapshot right before injecting: the native Judge pipeline of this tick may have
    // rotated its pool since the request was built.
    if (const uint32_t id = EnsureValidFoulSnapshotId(matchInfo, gmatch, fsm)) {
        req.safeSnapId = id;
        req.snapId = req.foulValid ? id : 0U;
    }
    InjectSetPlayIntoFsmJudgeResult(fsm, matchInfo, req);
    VAR_LOG_INF("Injected VAR SetPlay in Judge::FinalizeFrame (fsmState=0x%X): sp=%d reason=%d team=%d off=%d vic=%d card=%d snap=%u pos=(%.1f, %.1f)",
                fsmState, static_cast<int>(req.spKind), static_cast<int>(req.stopReason),
                req.benefitedTeam, req.offenderSlot, req.victimSlot,
                static_cast<int>(req.card), req.foulValid ? req.snapId : req.safeSnapId,
                req.spot.x, req.spot.z);
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

        if (g_injectedFoulToken.armed && g_mainTickCount > g_injectedFoulToken.untilTick) {
            g_injectedFoulToken.armed = false;
        }
        const bool isOurInjectedFoul = g_injectedFoulToken.armed && fi->isValid &&
                                       fi->offenderSlot == g_injectedFoulToken.offender &&
                                       fi->snapshotTickIndex == g_injectedFoulToken.snapId;
        if (g_suppressNextFoulCommitFrames > 0 || isOurInjectedFoul) {
            if (isOurInjectedFoul) {
                g_injectedFoulToken.armed = false;
                VAR_LOG_INF("Neutralised engine card commit of the VAR foul (offender=%d snap=%u)",
                            fi->offenderSlot, fi->snapshotTickIndex);
            }
            fi->isValid = 0;
            fi->foulKind = 0;
            fi->foulJudgementKind = 0;
            g_lastFoulValidEdge = false;
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
                if (IsValidSlot(ev.offenderSlot) && gmatch) {
                    if (uint8_t* rec = GetPlayerRecord(gmatch, ev.offenderSlot)) {
                        ev.offenderYellowsAfter = rec[Offsets::PlayerRecord_Yellows] + (ev.cardKind == CardKind::Yellow ? 1 : 0);
                        ev.offenderRedsAfter = rec[Offsets::PlayerRecord_Reds] +
                            ((ev.cardKind == CardKind::Red || (ev.cardKind == CardKind::Yellow && ev.offenderYellowsAfter >= 2)) ? 1 : 0);
                    }
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

    const uintptr_t addrGetPlayerBySlot = AobScanner::ResolveRipRelative(addrCommitCards + 6, 1, 5);
    VAR_LOG_INF("Resolved TeamInfo_GetPlayerBySlot -> 0x%llX", static_cast<unsigned long long>(addrGetPlayerBySlot));

    if (addrRosterGetter) {
        g_matchRosterPtrAddr = AobScanner::ResolveRipRelative(addrRosterGetter, 3, 7);
        VAR_LOG_INF("Resolved MatchRoster global ptr -> 0x%llX", static_cast<unsigned long long>(g_matchRosterPtrAddr));
    }

    g_fnEnterOutOfPlay    = reinterpret_cast<FnEnterOutOfPlay>(addrEnterOutOfPlay);
    g_fnGetTimeFloat      = reinterpret_cast<FnGetTimeFloat>(addrGetTimeFloat);
    g_fnSetPlayerOnPitch  = reinterpret_cast<FnSetPlayerOnPitch>(addrSetOnPitch);
    g_fnGetPlayerBySlot   = reinterpret_cast<FnGetPlayerBySlot>(addrGetPlayerBySlot);
    g_fnReplaySetFrame    = reinterpret_cast<FnReplaySetFrame>(addrReplaySetFrame);
    g_fnReplayGetFocusPos = reinterpret_cast<FnReplayGetFocusPos>(addrReplayFocusPos);

    // Guard first, so no verdict can ever run without it.
    const size_t guardSites = ArmFoulSnapshotGuardSites(addrFoulSnapFind);
    if (guardSites != kUncheckedFindSites) {
        VAR_LOG_ERR("FoulSnapshotPool guard: only %zu/%zu call sites validated, aborting hook install",
                    guardSites, kUncheckedFindSites);
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
    VAR_LOG_INF("All MatchHooks v4.0 installed cleanly (replaySeekReady=%d)", seekReady ? 1 : 0);
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
    g_simResetPending.store(true, std::memory_order_release);
    g_lastMatchFsm.store(nullptr, std::memory_order_relaxed);
    g_lastMatchControl.store(nullptr, std::memory_order_relaxed);
}

void MatchHooks::PollLiveReplayState() {
    if (!g_installed.load(std::memory_order_acquire) || !g_gmatchPtrAddr) return;

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
