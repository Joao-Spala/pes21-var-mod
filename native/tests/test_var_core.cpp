#include "aob_scanner.h"
#include "game_structs.h"
#include "heap_guard.h"
#include "i18n.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "inline_hook.h"
#include "input_manager.h"
#include "match_hooks.h"
#include "update_checker.h"
#include "engine_guards.h"
#include "var_hud.h"
#include "var_state.h"
#include "version.h"

#include <windows.h>
#include <bcrypt.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace var_mod;

namespace {

int g_passed = 0;
int g_failed = 0;

#define EXPECT_TRUE(cond, msg)                                                      \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::printf("[FAIL] %s (line %d): %s\n", __func__, __LINE__, (msg));    \
            ++g_failed;                                                             \
            return;                                                                 \
        }                                                                           \
    } while (0)

#define EXPECT_EQ(a, b, msg)                                                        \
    do {                                                                            \
        if ((a) != (b)) {                                                           \
            std::printf("[FAIL] %s (line %d): %s\n", __func__, __LINE__, (msg));    \
            ++g_failed;                                                             \
            return;                                                                 \
        }                                                                           \
    } while (0)

void MarkPass(const char* name) {
    std::printf("[PASS] %s\n", name);
    ++g_passed;
}

// ---------------------------------------------------------------------------
// Test 1: AobScanner synthetic buffer & RIP-relative resolution
// ---------------------------------------------------------------------------
void TestAobScannerBasic() {
    const uint8_t buf[] = {
        0x90, 0x48, 0x89, 0x5C, 0x24, 0x20, 0xCC,
        0x48, 0x8B, 0x05, 0x10, 0x00, 0x00, 0x00, // mov rax, [rip + 0x10]
        0x90, 0x48, 0x89, 0x5C, 0x24, 0x99
    };

    const auto r1 = AobScanner::ScanBuffer(buf, sizeof(buf), "48 89 5C 24 20");
    EXPECT_TRUE(r1.unique(), "Unique exact match failed");
    EXPECT_EQ(r1.address, reinterpret_cast<uintptr_t>(buf + 1), "Wrong exact match offset");

    const auto r2 = AobScanner::ScanBuffer(buf, sizeof(buf), "48 89 5C 24 ??");
    EXPECT_EQ(r2.matchCount, static_cast<size_t>(2), "Wildcard duplicate detection failed");
    EXPECT_TRUE(!r2.unique(), "Duplicate pattern must not report unique");

    const uintptr_t ripTarget = AobScanner::ResolveRipRelative(reinterpret_cast<uintptr_t>(buf + 7), 3, 7);
    EXPECT_EQ(ripTarget, reinterpret_cast<uintptr_t>(buf + 7 + 7 + 0x10), "ResolveRipRelative wrong target");

    MarkPass("TestAobScannerBasic");
}

// ---------------------------------------------------------------------------
// Test 2: Real PES2021.exe .text AOB & RIP-relative verification on disk
// ---------------------------------------------------------------------------
// PES2021_EXE, else the default Steam folders on C: and F:
bool FindReferenceExe(char* out, size_t size) {
    if (GetEnvironmentVariableA("PES2021_EXE", out, static_cast<DWORD>(size)) > 0 &&
        GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) return true;
    const char* candidates[] = {
        "C:\\Program Files (x86)\\Steam\\steamapps\\common\\eFootball PES 2021\\PES2021.exe",
        "F:\\SteamLibrary\\steamapps\\common\\eFootball PES 2021\\PES2021.exe",
    };
    for (const char* c : candidates) {
        if (GetFileAttributesA(c) != INVALID_FILE_ATTRIBUTES) {
            std::snprintf(out, size, "%s", c);
            return true;
        }
    }
    return false;
}

void TestRealPes2021BinaryAobs() {
    // Set PES2021_EXE to your game executable to run this check; defaults to the standard Steam location
    char exePath[MAX_PATH] = {};
    if (!FindReferenceExe(exePath, sizeof(exePath))) {
        std::printf("[SKIP] PES2021.exe not found (set PES2021_EXE)\n");
        return;
    }
    std::ifstream f(exePath, std::ios::binary);
    if (!f.is_open()) {
        std::printf("[SKIP] PES2021.exe not found at %s\n", exePath);
        return;
    }

    IMAGE_DOS_HEADER dos{};
    f.read(reinterpret_cast<char*>(&dos), sizeof(dos));
    EXPECT_EQ(dos.e_magic, IMAGE_DOS_SIGNATURE, "Invalid DOS header");

    f.seekg(dos.e_lfanew, std::ios::beg);
    IMAGE_NT_HEADERS64 nt{};
    f.read(reinterpret_cast<char*>(&nt), sizeof(nt));
    EXPECT_EQ(nt.Signature, IMAGE_NT_SIGNATURE, "Invalid NT header");

    std::vector<IMAGE_SECTION_HEADER> secs(nt.FileHeader.NumberOfSections);
    f.read(reinterpret_cast<char*>(secs.data()), static_cast<std::streamsize>(secs.size() * sizeof(IMAGE_SECTION_HEADER)));

    const IMAGE_SECTION_HEADER* textSec = nullptr;
    for (const auto& s : secs) {
        if (std::memcmp(s.Name, ".text", 5) == 0) {
            textSec = &s;
            break;
        }
    }
    EXPECT_TRUE(textSec != nullptr, ".text section not found");

    std::vector<uint8_t> textBytes(textSec->SizeOfRawData);
    f.seekg(textSec->PointerToRawData, std::ios::beg);
    f.read(reinterpret_cast<char*>(textBytes.data()), static_cast<std::streamsize>(textBytes.size()));

    const uintptr_t bufBase = reinterpret_cast<uintptr_t>(textBytes.data());
    const uint64_t imageBase = 0x140000000ULL;
    const uint64_t textVaBase = imageBase + textSec->VirtualAddress;

    struct CheckEntry {
        const char* name;
        const char* pattern;
        uintptr_t hintRva;
        uint64_t expectedVa;
    };

    const CheckEntry checks[] = {
        { "MainTick",                   Aobs::MainTick,                   AobHints::MainTick,                   0x1404468F0ULL },
        { "JudgeFinalizeFrame",         Aobs::JudgeFinalizeFrame,         AobHints::JudgeFinalizeFrame,         0x14050CD70ULL },
        { "UpdateFoulCounters",         Aobs::UpdateFoulCounters,         AobHints::UpdateFoulCounters,         0x140424410ULL },
        { "MatchFSM_EnterOutOfPlay",    Aobs::MatchFSM_EnterOutOfPlay,    AobHints::MatchFSM_EnterOutOfPlay,    0x1404541A0ULL },
        { "MatchFSM_RebuildSetPlay",    Aobs::MatchFSM_RebuildSetPlay,    AobHints::MatchFSM_RebuildSetPlay,    0x140454280ULL },
        { "MatchInfo_GetTimeFloat",     Aobs::MatchInfo_GetTimeFloat,     AobHints::MatchInfo_GetTimeFloat,     0x140A3C680ULL },
        { "MatchInfo_SetPlayerOnPitch", Aobs::MatchInfo_SetPlayerOnPitch, AobHints::MatchInfo_SetPlayerOnPitch, 0x140A3D0E0ULL },
        { "CommitCardsCallSite",        Aobs::CommitCardsCallSite,        AobHints::CommitCardsCallSite,        0x1404231DCULL },
        { "MatchRosterGetter",          Aobs::MatchRosterGetter,          AobHints::MatchRosterGetter,          0x1414BC550ULL },
        { "GrSwapChain_Present0",       Aobs::GrSwapChain_Present0,       AobHints::GrSwapChain_Present0,       0x141FF46A0ULL },
        { "DInputKeyboard_Poll",        Aobs::DInputKeyboard_Poll,        AobHints::DInputKeyboard_Poll,        0x141B638B0ULL },
        { "DInputJoystick_Poll",        Aobs::DInputJoystick_Poll,        AobHints::DInputJoystick_Poll,        0x141B63900ULL },
        { "DInputMouse_Poll",           Aobs::DInputMouse_Poll,           AobHints::DInputMouse_Poll,           0x141B63950ULL },
        { "PesPad_Update",              Aobs::PesPad_Update,              AobHints::PesPad_Update,              0x1403F86E0ULL },
        { "FoulSnapshotPool_Find",      Aobs::FoulSnapshotPool_Find,      AobHints::FoulSnapshotPool_Find,      0x140A82120ULL },
        { "HeapGuard_Win32Munmap",      HeapGuardAob::Win32Munmap,        HeapGuardAob::Win32MunmapHintRva,     0x1403FC0D0ULL },
        { "VanishingSpray_NativeReset", Aobs::VanishingSpray_NativeReset, AobHints::VanishingSpray_NativeReset, 0x140452EB0ULL },
        { "ReplayCtrl_SyncToManager",   Aobs::ReplayCtrl_SyncToManager,   AobHints::ReplayCtrl_SyncToManager,   0x1420C16A0ULL },
        { "ReplayPlayer_SetFrame",      Aobs::ReplayPlayer_SetFrame,      AobHints::ReplayPlayer_SetFrame,      0x14217E800ULL },
        { "ReplayPlayer_GetFocusPos",   Aobs::ReplayPlayer_GetFocusPos,   AobHints::ReplayPlayer_GetFocusPos,   0x14217EC70ULL },
        { "RecordHistory_AddEvent",     Aobs::RecordHistory_AddEvent,     AobHints::RecordHistory_AddEvent,     0x140A3E260ULL },
        { "RecordHistory_ResetPending", Aobs::RecordHistory_ResetPending, AobHints::RecordHistory_ResetPending, 0x140A3E760ULL },
        { "SimMain_UpdateScore",        Aobs::SimMain_UpdateScore,        AobHints::SimMain_UpdateScore,        0x142101130ULL },
        { "FixDemoMain_ClearMap",       Aobs::FixDemoMain_ClearMap,       AobHints::FixDemoMain_ClearMap,       0x141F3F3A0ULL },
    };

    uintptr_t mainTickBufAddr = 0;
    uintptr_t commitCardsBufAddr = 0;
    uintptr_t rosterGetterBufAddr = 0;
    uintptr_t grPresentBufAddr = 0;
    uintptr_t replaySyncBufAddr = 0;
    uintptr_t sprayResetBufAddr = 0;

    for (const auto& c : checks) {
        const auto res = AobScanner::ScanBuffer(textBytes.data(), textBytes.size(), c.pattern, 2);
        if (!res.unique()) {
            std::printf("[FAIL] Real AOB '%s' not unique (count=%zu)\n", c.name, res.matchCount);
            ++g_failed;
            return;
        }
        const uint64_t foundVa = textVaBase + (res.address - bufBase);
        if (foundVa != c.expectedVa) {
            std::printf("[FAIL] Real AOB '%s' VA mismatch: got 0x%llX expected 0x%llX\n",
                        c.name, static_cast<unsigned long long>(foundVa),
                        static_cast<unsigned long long>(c.expectedVa));
            ++g_failed;
            return;
        }
        if (imageBase + c.hintRva != foundVa) {
            std::printf("[FAIL] Real AOB '%s' hintRva mismatch: hint=0x%llX found=0x%llX\n",
                        c.name, static_cast<unsigned long long>(imageBase + c.hintRva),
                        static_cast<unsigned long long>(foundVa));
            ++g_failed;
            return;
        }
        if (std::strcmp(c.name, "MainTick") == 0) mainTickBufAddr = res.address;
        if (std::strcmp(c.name, "CommitCardsCallSite") == 0) commitCardsBufAddr = res.address;
        if (std::strcmp(c.name, "MatchRosterGetter") == 0) rosterGetterBufAddr = res.address;
        if (std::strcmp(c.name, "GrSwapChain_Present0") == 0) grPresentBufAddr = res.address;
        if (std::strcmp(c.name, "ReplayCtrl_SyncToManager") == 0) replaySyncBufAddr = res.address;
        if (std::strcmp(c.name, "VanishingSpray_NativeReset") == 0) sprayResetBufAddr = res.address;
    }

    // Verify MeasureSafeStolenLen on real prologues
    uintptr_t probeGrPresent = grPresentBufAddr;
    EXPECT_EQ(InlineHook::MeasureSafeStolenLen(probeGrPresent), static_cast<size_t>(7),
              "Real GrSwapChain::Present0 stolen len must be 7");

    uintptr_t probeReplaySync = replaySyncBufAddr;
    EXPECT_EQ(InlineHook::MeasureSafeStolenLen(probeReplaySync), static_cast<size_t>(5),
              "Real ReplayCtrl_SyncToManager stolen len must be 5 (40 53 55 56 57)");

    // Verify RIP-relative resolutions against real bytes
    const int64_t gmatchDisp = *reinterpret_cast<const int32_t*>(mainTickBufAddr + 0x30 + 3);
    const uint64_t gmatchVa = (0x1404468F0ULL + 0x30 + 7) + gmatchDisp;
    EXPECT_EQ(gmatchVa, 0x143702208ULL, "GMATCH RIP-relative resolution mismatch");

    const int64_t getPlayerDisp = *reinterpret_cast<const int32_t*>(commitCardsBufAddr + 6 + 1);
    const uint64_t getPlayerVa = (0x1404231DCULL + 6 + 5) + getPlayerDisp;
    EXPECT_EQ(getPlayerVa, 0x140A3B5B0ULL, "TeamInfo_GetPlayerBySlot call rel32 resolution mismatch");

    const int64_t rosterDisp = *reinterpret_cast<const int32_t*>(rosterGetterBufAddr + 3);
    const uint64_t rosterVa = (0x1414BC550ULL + 7) + rosterDisp;
    EXPECT_EQ(rosterVa, 0x143714240ULL, "MatchRoster RIP-relative resolution mismatch");

    // FoulSnapshotPool::Find guard: 6-byte prologue and every unchecked call site must be `E8 rel32` to it
    for (uintptr_t rva : FoulSnapshotUncheckedCallSites::HintRvas) {
        const uint64_t siteVa = imageBase + rva;
        const uintptr_t siteBuf = bufBase + static_cast<uintptr_t>(siteVa - textVaBase);
        const auto* sp = reinterpret_cast<const uint8_t*>(siteBuf);
        const int64_t rel = *reinterpret_cast<const int32_t*>(siteBuf + 1);
        EXPECT_TRUE(sp[0] == 0xE8 && static_cast<uint64_t>(siteVa + 5 + rel) == 0x140A82120ULL,
                    "FoulSnapshotPool unchecked call site must call FoulSnapshotPool::Find");
    }

    // TeamTactics + 0x254 is a SIGNED BYTE side sign (movsbl), not a float: 0F BE 98 54 02 00 00 at 0x140438475
    {
        const auto* mp = reinterpret_cast<const uint8_t*>(bufBase + static_cast<uintptr_t>(0x140438475ULL - textVaBase));
        EXPECT_TRUE(mp[0] == 0x0F && mp[1] == 0xBE && *reinterpret_cast<const uint32_t*>(mp + 3) == Offsets::TeamTactics_SideSign,
                    "Engine must read TeamTactics side sign as a signed byte (movsbl +0x254)");
    }

    // TeamInfo double buffer: the publish callback copies 0x5E * 0x80 = TeamInfo_BufferSize bytes from
    // A + 0x2F08 (write buffer = read cache + TeamInfo_BufferSize) to A + 8 (read cache)
    {
        const auto* tp = reinterpret_cast<const uint8_t*>(bufBase + static_cast<uintptr_t>(0x14041BE40ULL - textVaBase));
        const uint8_t expect[] = { 0x48, 0x8D, 0x91, 0x08, 0x2F, 0x00, 0x00, 0x48, 0x8D, 0x41, 0x08, 0xB9, 0x5E, 0x00, 0x00, 0x00 };
        EXPECT_TRUE(std::memcmp(tp, expect, sizeof(expect)) == 0, "TeamInfo publish copy must be write (A+0x2F08) -> read (A+8)");
        EXPECT_EQ(static_cast<size_t>(0x5E * 0x80), static_cast<size_t>(Offsets::TeamInfo_BufferSize),
                  "TeamInfo buffer size must match the publish copy");
        EXPECT_EQ(static_cast<size_t>(0x2F08 - 0x8), static_cast<size_t>(Offsets::TeamInfo_BufferSize),
                  "Write buffer must be read cache + TeamInfo_BufferSize");
        EXPECT_TRUE(Offsets::TeamInfo_PlayerRecordsBase + Offsets::MatchRoster_MaxPlayers * Offsets::TeamInfo_PlayerRecordStride <=
                    Offsets::TeamInfo_BufferSize, "40 player records must fit in one TeamInfo buffer");
    }

    // Rollback contract (ExecVerifyAfterMainTick): the queue reset stores its reason at fsm + 0x874, EnterOutOfPlay
    // passes 1, and MainTick ends by copying the working block (fsm + 0x2790) to *(fsm + 0x17858)
    {
        const auto* rp = reinterpret_cast<const uint8_t*>(bufBase + static_cast<uintptr_t>(0x1404529F0ULL - textVaBase));
        EXPECT_TRUE(rp[13] == 0x89 && rp[14] == 0x91 &&
                    *reinterpret_cast<const uint32_t*>(rp + 15) == Offsets::MatchFSM_QueueResetReason,
                    "Queue reset must store its reason at fsm + MatchFSM_QueueResetReason");
        const auto* ep = reinterpret_cast<const uint8_t*>(bufBase + static_cast<uintptr_t>(0x140454229ULL - textVaBase));
        EXPECT_TRUE(ep[0] == 0xBA && *reinterpret_cast<const uint32_t*>(ep + 1) == 1U, "EnterOutOfPlay must reset with reason 1");
        const auto* cp = reinterpret_cast<const uint8_t*>(bufBase + static_cast<uintptr_t>(0x140446D29ULL - textVaBase));
        EXPECT_TRUE(cp[0] == 0x48 && cp[1] == 0x8B && cp[2] == 0x8D &&
                    *reinterpret_cast<const uint32_t*>(cp + 3) == Offsets::MatchFSM_WorkingCopyPtr &&
                    cp[7] == 0x48 && cp[8] == 0x8D && cp[9] == 0x95 &&
                    *reinterpret_cast<const uint32_t*>(cp + 10) == Offsets::MatchFSM_WorkingBlock && cp[14] == 0xE8 &&
                    0x140446D3CULL + static_cast<int64_t>(*reinterpret_cast<const int32_t*>(cp + 15)) == 0x140443DB0ULL,
                    "MainTick must copy the FSM working block to *(fsm + MatchFSM_WorkingCopyPtr)");
    }

    // Vanishing spray reset: the three calls must land on the accessor, the reset and the commit
    {
        auto callTarget = [&](uintptr_t bufSite, uint64_t siteVa) -> uint64_t {
            return siteVa + 5 + static_cast<int64_t>(*reinterpret_cast<const int32_t*>(bufSite + 1));
        };
        EXPECT_EQ(callTarget(sprayResetBufAddr + 0x14, 0x140452EB0ULL + 0x14), 0x140423830ULL, "Spray getWrite target mismatch");
        EXPECT_EQ(callTarget(sprayResetBufAddr + 0x21, 0x140452EB0ULL + 0x21), 0x140A5CCE0ULL, "Spray reset target mismatch");
        EXPECT_EQ(callTarget(sprayResetBufAddr + 0x2D, 0x140452EB0ULL + 0x2D), 0x140422220ULL, "Spray commit target mismatch");
        // The FSM ctor stores the live vtable at +0x17CA8: lea rcx, [rip + disp] at 0x14044201E
        const uintptr_t leaBuf = bufBase + static_cast<uintptr_t>(0x14044201EULL - textVaBase);
        const auto* lp = reinterpret_cast<const uint8_t*>(leaBuf);
        const uint64_t leaTarget = 0x14044201EULL + 7 + static_cast<int64_t>(*reinterpret_cast<const int32_t*>(leaBuf + 3));
        EXPECT_TRUE(lp[0] == 0x48 && lp[1] == 0x8D && lp[2] == 0x0D, "FSM ctor must load the spray ref vtable with lea rcx");
        EXPECT_EQ(leaTarget, imageBase + RttiVtables::VanishingSprayRefHint, "Live spray ref vtable RVA mismatch");
    }

    const int64_t replaySingDisp = *reinterpret_cast<const int32_t*>(replaySyncBufAddr + 0x32 + 3);
    const uint64_t replaySingVa = (0x1420C16A0ULL + 0x32 + 7) + replaySingDisp;
    EXPECT_EQ(replaySingVa, 0x143703558ULL, "ReplaySingleton RIP-relative resolution mismatch");

    MarkPass("TestRealPes2021BinaryAobs");
}

// ---------------------------------------------------------------------------
// Engine builds: the executable is mapped as an image (never run) and the mod's own lookup code runs on it
// exactly as in the game: every AOB unique in the executable sections, the derived globals, the FoulSnapshot
// call sites by signature and the four vtables by RTTI. Reference build (PES2021.exe) and, when VAR_OTHER_BUILD_EXE points
// to it, a build with other section names and other addresses.
// ---------------------------------------------------------------------------
struct BuildExpect {
    uint32_t gmatch, roster, replaySingleton, cameraLow, cameraUp, spray, simMain;
    uint32_t foulSites[3];
};

void CheckEngineBuild(const char* label, const char* path, const BuildExpect& e) {
    wchar_t wpath[MAX_PATH] = {};
    MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH);
    HMODULE h = LoadLibraryExW(wpath, nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE);
    if (!h) {
        std::printf("[FAIL] %s: could not map %s as an image (error %lu)\n", label, path, GetLastError());
        ++g_failed;
        return;
    }
    auto* base = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(h) & ~static_cast<uintptr_t>(3));
    auto rva = [&](uintptr_t a) { return static_cast<uint32_t>(a - reinterpret_cast<uintptr_t>(base)); };
    struct Item { const char* name; const char* pattern; uintptr_t hint; };
    const Item items[] = {
        { "MainTick", Aobs::MainTick, AobHints::MainTick },
        { "JudgeFinalizeFrame", Aobs::JudgeFinalizeFrame, AobHints::JudgeFinalizeFrame },
        { "UpdateFoulCounters", Aobs::UpdateFoulCounters, AobHints::UpdateFoulCounters },
        { "MatchFSM_EnterOutOfPlay", Aobs::MatchFSM_EnterOutOfPlay, AobHints::MatchFSM_EnterOutOfPlay },
        { "MatchFSM_RebuildSetPlay", Aobs::MatchFSM_RebuildSetPlay, AobHints::MatchFSM_RebuildSetPlay },
        { "MatchInfo_GetTimeFloat", Aobs::MatchInfo_GetTimeFloat, AobHints::MatchInfo_GetTimeFloat },
        { "MatchInfo_SetPlayerOnPitch", Aobs::MatchInfo_SetPlayerOnPitch, AobHints::MatchInfo_SetPlayerOnPitch },
        { "CommitCardsCallSite", Aobs::CommitCardsCallSite, AobHints::CommitCardsCallSite },
        { "MatchRosterGetter", Aobs::MatchRosterGetter, AobHints::MatchRosterGetter },
        { "ReplayCtrl_SyncToManager", Aobs::ReplayCtrl_SyncToManager, AobHints::ReplayCtrl_SyncToManager },
        { "ReplayPlayer_SetFrame", Aobs::ReplayPlayer_SetFrame, AobHints::ReplayPlayer_SetFrame },
        { "ReplayPlayer_GetFocusPos", Aobs::ReplayPlayer_GetFocusPos, AobHints::ReplayPlayer_GetFocusPos },
        { "GrSwapChain_Present0", Aobs::GrSwapChain_Present0, AobHints::GrSwapChain_Present0 },
        { "DInputKeyboard_Poll", Aobs::DInputKeyboard_Poll, AobHints::DInputKeyboard_Poll },
        { "DInputJoystick_Poll", Aobs::DInputJoystick_Poll, AobHints::DInputJoystick_Poll },
        { "DInputMouse_Poll", Aobs::DInputMouse_Poll, AobHints::DInputMouse_Poll },
        { "PesPad_Update", Aobs::PesPad_Update, AobHints::PesPad_Update },
        { "FoulSnapshotPool_Find", Aobs::FoulSnapshotPool_Find, AobHints::FoulSnapshotPool_Find },
        { "VanishingSpray_NativeReset", Aobs::VanishingSpray_NativeReset, AobHints::VanishingSpray_NativeReset },
        { "RecordHistory_AddEvent", Aobs::RecordHistory_AddEvent, AobHints::RecordHistory_AddEvent },
        { "RecordHistory_ResetPending", Aobs::RecordHistory_ResetPending, AobHints::RecordHistory_ResetPending },
        { "FixDemoMain_ClearMap", Aobs::FixDemoMain_ClearMap, AobHints::FixDemoMain_ClearMap },
        { "SimMain_UpdateScore", Aobs::SimMain_UpdateScore, AobHints::SimMain_UpdateScore },
        { "HeapGuard_Win32Munmap", HeapGuardAob::Win32Munmap, HeapGuardAob::Win32MunmapHintRva },
    };
    uintptr_t addr[sizeof(items) / sizeof(items[0])] = {};
    bool allUnique = true;
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); ++i) {
        // Full scan (no hint) proves uniqueness in this build, the hinted lookup must land on the same address
        const AobMatchResult full = AobScanner::ScanModuleText(base, items[i].pattern);
        const AobMatchResult hinted = AobScanner::ScanModuleTextWithHint(base, items[i].pattern, items[i].hint);
        if (!full.unique() || !hinted.unique() || full.address != hinted.address) {
            std::printf("[FAIL] %s: AOB %s count=%zu (hinted 0x%X)\n", label, items[i].name, full.matchCount, rva(hinted.address));
            ++g_failed;
            allUnique = false;
            continue;
        }
        addr[i] = full.address;
    }
    if (allUnique) {
        auto ripTarget = [&](uintptr_t insn) { return rva(AobScanner::ResolveRipRelative(insn, 3, 7)); };
        const auto* mt = reinterpret_cast<const uint8_t*>(addr[0] + 0x30);
        EXPECT_TRUE(mt[0] == 0x48 && mt[1] == 0x8B && mt[2] == 0x05, "MainTick+0x30 is the GMATCH load");
        EXPECT_EQ(ripTarget(addr[0] + 0x30), e.gmatch, "GMATCH global");
        EXPECT_EQ(ripTarget(addr[8]), e.roster, "MatchRoster global");
        EXPECT_EQ(ripTarget(addr[9] + 0x32), e.replaySingleton, "ReplaySingleton global");
        auto at = [&](uint32_t r) { return reinterpret_cast<uintptr_t>(base) + r; };
        EXPECT_TRUE(AobScanner::IsInWritableSection(base, at(e.gmatch), 8) && AobScanner::IsInWritableSection(base, at(e.roster), 8) &&
                    AobScanner::IsInWritableSection(base, at(e.replaySingleton), 8), "globals are writable data");
        EXPECT_TRUE(!AobScanner::IsInWritableSection(base, addr[0], 8), "code is never accepted as a global");
        for (size_t i = 0; i < 3; ++i) {
            const uintptr_t hint = FoulSnapshotUncheckedCallSites::HintRvas[i] - FoulSnapshotUncheckedCallSites::kCallOffset;
            const AobMatchResult full = AobScanner::ScanModuleText(base, FoulSnapshotUncheckedCallSites::Patterns[i]);
            const AobMatchResult hinted = AobScanner::ScanModuleTextWithHint(base, FoulSnapshotUncheckedCallSites::Patterns[i], hint);
            EXPECT_TRUE(full.unique() && hinted.address == full.address, "FoulSnapshot call site signature unique");
            if (!full.unique()) continue;
            const uintptr_t site = full.address + FoulSnapshotUncheckedCallSites::kCallOffset;
            EXPECT_EQ(rva(site), e.foulSites[i], "FoulSnapshot call site address");
            const int32_t rel = *reinterpret_cast<const int32_t*>(site + 1);
            EXPECT_TRUE(*reinterpret_cast<const uint8_t*>(site) == 0xE8 && site + 5 + rel == addr[17],
                        "FoulSnapshot call site calls FoulSnapshotPool::Find");
        }
    }
    struct Vt { const char* name; uintptr_t hint; uint32_t expect; };
    const Vt vts[] = {
        { RttiVtables::ReplayCameraLow, RttiVtables::ReplayCameraLowHint, e.cameraLow },
        { RttiVtables::ReplayCameraUp, RttiVtables::ReplayCameraUpHint, e.cameraUp },
        { RttiVtables::VanishingSprayRef, RttiVtables::VanishingSprayRefHint, e.spray },
        { RttiVtables::SimMain, RttiVtables::SimMainHint, e.simMain },
    };
    for (const Vt& v : vts) {
        const uintptr_t scanned = AobScanner::FindVtableByRtti(base, v.name, 0);
        const uintptr_t hinted = AobScanner::FindVtableByRtti(base, v.name, v.hint);
        if (!scanned || scanned != hinted || rva(scanned) != v.expect) {
            std::printf("[FAIL] %s: vtable %s scanned 0x%X hinted 0x%X expected 0x%X\n", label, v.name,
                        scanned ? rva(scanned) : 0, hinted ? rva(hinted) : 0, v.expect);
            ++g_failed;
        }
    }
    EXPECT_TRUE(AobScanner::FindVtableByRtti(base, ".?AVNoSuchClass@var@@", 0) == 0, "unknown class gives 0");
    FreeLibrary(h);
}

void TestEngineBuildsByImage() {
    char ref[MAX_PATH] = {};
    if (FindReferenceExe(ref, sizeof(ref))) {
        CheckEngineBuild("PES2021", ref, BuildExpect{ 0x3702208, 0x3714240, 0x3703558, 0x2BDFB40, 0x2BDFB88, 0x25A1570, 0x2BF58F8,
                                                      { 0x4B49BD, 0x4B4BC7, 0x4B4EA0 } });
    } else {
        std::printf("[SKIP] reference PES2021.exe not found\n");
    }
    char other[MAX_PATH] = {};
    if (GetEnvironmentVariableA("VAR_OTHER_BUILD_EXE", other, MAX_PATH) > 0) {
        CheckEngineBuild("other build", other, BuildExpect{ 0x36F3DC8, 0x3705E10, 0x36F5118, 0x2BD3C80, 0x2BD3CC8, 0x2598490, 0x2BE9A88,
                                                     { 0x4B3FFD, 0x4B4207, 0x4B44E0 } });
    } else {
        std::printf("[SKIP] VAR_OTHER_BUILD_EXE not set: second engine build not checked\n");
    }
    MarkPass("TestEngineBuildsByImage");
}

// Engine check (engine_guards.inc): both studied builds prove every guarded offset and hooked function body; an
// executable that is not the game (this test binary) is refused.
void TestEngineGuards() {
    auto run = [](const char* label, const char* path, bool expectReference) {
        wchar_t wpath[MAX_PATH] = {};
        MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH);
        HMODULE h = LoadLibraryExW(wpath, nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE);
        if (!h) { std::printf("[FAIL] %s not mapped\n", label); ++g_failed; return; }
        void* base = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(h) & ~static_cast<uintptr_t>(3));
        const EngineGuards::Report r = EngineGuards::VerifyImage(base);
        std::printf("[info] %s: %s\n", label, EngineGuards::Summary(r).c_str());
        if (!r.missingOffsets.empty()) std::printf("[info]   missing offsets: %s\n", r.missingOffsets.c_str());
        if (!r.missingBodies.empty()) std::printf("[info]   missing bodies: %s\n", r.missingBodies.c_str());
        EXPECT_TRUE(r.ok, "studied build passes the engine check");
        EXPECT_TRUE(r.goalRecordOk && r.goalRecordOffsets == 4, "goal record layout proven (goal annulment)");
        EXPECT_TRUE(r.referenceBuild == expectReference, "reference build recognized by its addresses");
        EXPECT_TRUE(r.offsets > 50 && r.bodies >= 12, "the check covers the structure offsets and the hooked functions");
        FreeLibrary(h);
    };
    char ref[MAX_PATH] = {};
    if (FindReferenceExe(ref, sizeof(ref))) run("reference", ref, true);
    char other[MAX_PATH] = {};
    if (GetEnvironmentVariableA("VAR_OTHER_BUILD_EXE", other, MAX_PATH) > 0) run("other build", other, false);
    const EngineGuards::Report self = EngineGuards::VerifyImage(GetModuleHandleW(nullptr));
    EXPECT_TRUE(!self.ok && self.offsetsConfirmed == 0, "an executable that is not the game is refused");
    MarkPass("TestEngineGuards");
}

// ---------------------------------------------------------------------------
// Test 3: InlineHook live execution + EFLAGS/ZF preservation + exact restore
// ---------------------------------------------------------------------------
using FnFlagProbe = int(__fastcall*)(const uint8_t* flagPtr);
InlineHook g_testInlineHook;
int g_testDetourHits = 0;

int __fastcall Detour_FlagProbe(const uint8_t* flagPtr) {
    ++g_testDetourHits;
    return g_testInlineHook.Original<FnFlagProbe>()(flagPtr);
}

void TestInlineHookEflagsAndRestore() {
    // Machine code mimicking MatchControl::UpdateFoulCounters (0x140424410):
    //   80 39 00       cmp byte ptr [rcx], 0        (3 bytes)
    //   90 90 90       nop; nop; nop                (3 bytes -> total 6 stolen bytes, ZF live!)
    //   74 06          je +6 (to zero_branch)       (2 bytes)
    //   B8 2A 00 00 00 mov eax, 42                  (5 bytes)
    //   C3             ret                          (1 byte)
    // zero_branch:
    //   B8 07 00 00 00 mov eax, 7                   (5 bytes)
    //   C3             ret                          (1 byte)
    const uint8_t code[] = {
        0x80, 0x39, 0x00,
        0x90, 0x90, 0x90,
        0x74, 0x06,
        0xB8, 0x2A, 0x00, 0x00, 0x00,
        0xC3,
        0xB8, 0x07, 0x00, 0x00, 0x00,
        0xC3
    };

    void* execMem = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    EXPECT_TRUE(execMem != nullptr, "VirtualAlloc failed");
    std::memcpy(execMem, code, sizeof(code));
    FlushInstructionCache(GetCurrentProcess(), execMem, sizeof(code));

    auto fn = reinterpret_cast<FnFlagProbe>(execMem);
    const uint8_t zero = 0;
    const uint8_t one = 1;

    EXPECT_EQ(fn(&zero), 7, "Baseline ZF=1 failed");
    EXPECT_EQ(fn(&one), 42, "Baseline ZF=0 failed");

    g_testDetourHits = 0;
    EXPECT_TRUE(g_testInlineHook.Install(reinterpret_cast<uintptr_t>(execMem),
                                         reinterpret_cast<void*>(&Detour_FlagProbe), 6),
                "InlineHook::Install failed");

    EXPECT_EQ(fn(&zero), 7, "Hooked ZF=1 branch corrupted EFLAGS");
    EXPECT_EQ(fn(&one), 42, "Hooked ZF=0 branch corrupted EFLAGS");
    EXPECT_EQ(g_testDetourHits, 2, "Detour call count mismatch");

    EXPECT_TRUE(g_testInlineHook.Uninstall(), "InlineHook::Uninstall failed");
    EXPECT_EQ(std::memcmp(execMem, code, sizeof(code)), 0, "Original bytes not restored byte-for-byte");
    EXPECT_EQ(fn(&zero), 7, "Post-uninstall ZF=1 failed");
    EXPECT_EQ(fn(&one), 42, "Post-uninstall ZF=0 failed");
    EXPECT_EQ(g_testDetourHits, 2, "Detour still called after uninstall");

    VirtualFree(execMem, 0, MEM_RELEASE);
    MarkPass("TestInlineHookEflagsAndRestore");
}

// ---------------------------------------------------------------------------
// Test 4: VtableHook install, chain, restore
// ---------------------------------------------------------------------------
using FnDummyV = int(*)(int);
VtableHook g_testVtableHook;
int DummyOrigV(int x) { return x + 10; }
int DummyDetourV(int x) { return g_testVtableHook.Original<FnDummyV>()(x) * 2; }

void TestVtableHook() {
    void* fakeVtable[4] = { nullptr, reinterpret_cast<void*>(&DummyOrigV), nullptr, nullptr };
    EXPECT_TRUE(g_testVtableHook.Install(fakeVtable, 1, reinterpret_cast<void*>(&DummyDetourV)),
                "VtableHook install failed");
    auto fn = reinterpret_cast<FnDummyV>(fakeVtable[1]);
    EXPECT_EQ(fn(5), 30, "VtableHook chained return mismatch");
    EXPECT_TRUE(g_testVtableHook.Uninstall(), "VtableHook uninstall failed");
    EXPECT_EQ(fakeVtable[1], reinterpret_cast<void*>(&DummyOrigV), "Vtable slot not restored");
    MarkPass("TestVtableHook");
}

// ---------------------------------------------------------------------------
// Test 5: PitchGeometry math & clock formatting
// ---------------------------------------------------------------------------
void TestPitchGeometry() {
    PitchDims d{}; // standard 105x68m pitch: box18X=36, box18Z=20.16, penSpotX=41.5

    // Penalty spots for both attack directions
    const Vec3 penPos = PitchGeometry::CalcPenaltySpot(d, +1.0f);
    const Vec3 penNeg = PitchGeometry::CalcPenaltySpot(d, -1.0f);
    EXPECT_TRUE(std::fabs(penPos.x - 41.5f) < 0.001f && std::fabs(penPos.z) < 0.001f, "Positive penalty spot wrong");
    EXPECT_TRUE(std::fabs(penNeg.x + 41.5f) < 0.001f && std::fabs(penNeg.z) < 0.001f, "Negative penalty spot wrong");

    // Box classification
    EXPECT_TRUE(PitchGeometry::IsInsideDefendingBox(d, Vec3{ 42.0f, 0.0f, 5.0f }, +1.0f), "Inside +X box not detected");
    EXPECT_TRUE(!PitchGeometry::IsInsideDefendingBox(d, Vec3{ -42.0f, 0.0f, 5.0f }, +1.0f), "Opposite box wrongly detected");
    EXPECT_TRUE(PitchGeometry::IsInsideDefendingBox(d, Vec3{ -42.0f, 0.0f, -15.0f }, -1.0f), "Inside -X box not detected");
    EXPECT_TRUE(!PitchGeometry::IsInsideDefendingBox(d, Vec3{ 35.0f, 0.0f, 0.0f }, +1.0f), "Outside front edge wrongly inside");
    EXPECT_TRUE(!PitchGeometry::IsInsideDefendingBox(d, Vec3{ 42.0f, 0.0f, 21.0f }, +1.0f), "Outside side edge wrongly inside");

    // Projection outside defending box (front edge closer vs side edge closer)
    const Vec3 projFront = PitchGeometry::ProjectOutsideDefendingBox(d, Vec3{ 37.0f, 0.0f, 2.0f }, +1.0f, 0.6f);
    EXPECT_TRUE(!PitchGeometry::IsInsideDefendingBox(d, projFront, +1.0f), "Projected front point still inside box");
    EXPECT_TRUE(projFront.x < d.box18X, "Projected front point should move outside front X");

    const Vec3 projSide = PitchGeometry::ProjectOutsideDefendingBox(d, Vec3{ 46.0f, 0.0f, -19.5f }, +1.0f, 0.6f);
    EXPECT_TRUE(!PitchGeometry::IsInsideDefendingBox(d, projSide, +1.0f), "Projected side point still inside box");
    EXPECT_TRUE(projSide.z < -d.box18Z, "Projected side point should move outside -Z edge");

    // Match clock formatting
    int mm = 0, ss = 0;
    PitchGeometry::FormatMatchClock(0, 60.0f * 125.0f, mm, ss); // 1T 02:05
    EXPECT_EQ(mm, 2, "1T minutes wrong");
    EXPECT_EQ(ss, 5, "1T seconds wrong");

    PitchGeometry::FormatMatchClock(2, 60.0f * 610.0f, mm, ss); // 2T 55:10
    EXPECT_EQ(mm, 55, "2T minutes wrong");
    EXPECT_EQ(ss, 10, "2T seconds wrong");

    PitchGeometry::FormatMatchClock(4, 60.0f * 180.0f, mm, ss); // ET1 93:00
    EXPECT_EQ(mm, 93, "ET1 minutes wrong");

    MarkPass("TestPitchGeometry");
}

// ---------------------------------------------------------------------------
// Test 6: VarState ring buffers, replay sync & atomic verdict queue
// ---------------------------------------------------------------------------
void TestVarStateAndSync() {
    auto& st = VarState::Instance();
    st.ResetMatch();

    // Fill past ring capacity (128 events)
    for (uint32_t i = 0; i < 140; ++i) {
        VarEvent ev{};
        ev.type = (i % 2 == 0) ? VarEventType::Foul : VarEventType::Penalty;
        ev.matchPhase = 0;
        ev.time60Hz = static_cast<float>(i * 600);
        ev.matchTick = i * 600;
        ev.offenderSlot = static_cast<int32_t>(i % 22);
        ev.cardKind = CardKind::Yellow;
        st.PushEvent(ev);
    }

    const auto recent = st.GetRecentEvents(200);
    EXPECT_EQ(recent.size(), VarState::kMaxEvents, "Ring buffer should cap at kMaxEvents");
    EXPECT_EQ(recent[0].matchTick, 139U * 600U, "Newest event should be first");

    // Add a 2nd-half event at same time60Hz = 12000 as a 1st-half event
    VarEvent ev2H{};
    ev2H.type = VarEventType::Penalty;
    ev2H.matchPhase = 2; // 2nd half
    ev2H.time60Hz = 12000.0f;
    ev2H.matchTick = 90000;
    ev2H.offenderSlot = 4;
    ev2H.cardKind = CardKind::Red;
    const uint32_t ev2HId = st.PushEvent(ev2H);

    // Sync in 2nd half within tolerance (+45 ticks)
    const auto sync2H = st.SyncToReplayCursor(2, 12045.0f);
    EXPECT_TRUE(sync2H.hasEvent, "Should match 2nd-half event");
    EXPECT_EQ(sync2H.event.id, ev2HId, "Matched wrong event across halves");

    // Sync in 2nd half far away -> should NOT match event (NoFoul play-on)
    const auto syncNoFoul = st.SyncToReplayCursor(2, 25000.0f);
    EXPECT_TRUE(!syncNoFoul.hasEvent, "Far cursor should not match stale event");

    // Snapshot closest contact pair test
    PlaySnapshot snap{};
    snap.matchPhase = 2;
    snap.time60Hz = 25000.0f;
    snap.ballPos = Vec3{ 10.0f, 0.0f, 5.0f };
    for (int i = 0; i < 22; ++i) {
        snap.playerOnPitch[i] = 1;
        snap.playerPos[i] = Vec3{ -40.0f + static_cast<float>(i), 0.0f, -20.0f };
    }
    snap.playerPos[9]  = Vec3{ 10.2f, 0.0f, 5.1f }; // Home striker on ball
    snap.playerPos[14] = Vec3{ 10.6f, 0.0f, 5.3f }; // Away CB right next to striker
    VarState::ComputeClosestContact(snap);
    EXPECT_EQ(snap.ballOwnerSlot, 9, "Wrong ball owner detected");
    EXPECT_EQ(snap.closestA, 9, "Wrong closest home contact");
    EXPECT_EQ(snap.closestB, 14, "Wrong closest away contact");

    // Verdict queue single-consume check
    VarVerdict v{};
    v.action = VarAction::RetroactivePenalty;
    v.offenderSlot = 14;
    v.benefitedTeam = 0;
    v.targetCard = CardKind::Yellow;
    st.ArmVerdict(v);

    VarVerdict out{};
    EXPECT_TRUE(st.ConsumeVerdict(out), "First ConsumeVerdict should succeed");
    EXPECT_EQ(out.action, VarAction::RetroactivePenalty, "Consumed action mismatch");
    EXPECT_TRUE(!st.ConsumeVerdict(out), "Second ConsumeVerdict must return false");

    MarkPass("TestVarStateAndSync");
}

// ---------------------------------------------------------------------------
// Test 7: VarHudController contextual menus, Back nav & zero arbitrary locks
// ---------------------------------------------------------------------------
void TestVarHudController() {
    auto& st = VarState::Instance();
    st.ResetMatch();

    VarHudController ctrl;
    LiveTelemetry tel{};
    tel.enabled = true;
    tel.offlineAllowed = true;
    tel.matchActive = true;
    tel.matchPhase = 0;
    tel.matchStatus = static_cast<int32_t>(MatchStatus::SetPlayReady);
    tel.fsmState = 0x1A; // Post-state 0x13
    tel.replayMode = 2;
    tel.replayManualFlag = 1;
    tel.replayCursorPhase = 0;
    tel.replayCursorTime = 5000.0f;

    VarEvent redFoul{};
    redFoul.id = 77;
    redFoul.type = VarEventType::Foul;
    redFoul.matchPhase = 0;
    redFoul.time60Hz = 5000.0f;
    redFoul.offenderSlot = 3;
    redFoul.victimSlot = 19;
    redFoul.benefitedTeam = 1;
    redFoul.cardKind = CardKind::Red;

    ReplaySyncResult sync{};
    sync.hasEvent = true;
    sync.event = redFoul;

    const auto players = st.GetPlayers();
    const auto teams   = st.GetTeams();
    std::vector<VarEvent> hist = { redFoul };

    ctrl.UpdateContext(tel, sync, players, teams, hist, MatchVarStats{}, HudConfig{});
    EXPECT_TRUE(ctrl.IsInManualReplay(), "Should detect manual replay");
    EXPECT_TRUE(ctrl.IsFocused(), "Should auto-open focused by default on entering Manual Replay");
    EXPECT_EQ(ctrl.Page(), HudPage::Main, "Should open on Main page");

    // ToggleFocus collapses to compact pill and re-opens cleanly
    ctrl.HandleAction(NavAction::ToggleFocus, st);
    EXPECT_TRUE(!ctrl.IsFocused(), "ToggleFocus should collapse panel");
    ctrl.HandleAction(NavAction::ToggleFocus, st);
    EXPECT_TRUE(ctrl.IsFocused(), "ToggleFocus should reopen panel");
    EXPECT_EQ(ctrl.Page(), HudPage::Main, "Should reopen on Main page");

    // Navigate to "Alterar Cartao" (index 2 in Foul menu: 0=Anular, 1=Converter Penalti, 2=Alterar Cartao)
    ctrl.HandleAction(NavAction::Down, st);
    ctrl.HandleAction(NavAction::Down, st);
    ctrl.HandleAction(NavAction::Confirm, st);
    EXPECT_EQ(ctrl.Page(), HudPage::Configure, "Should enter Configure page");
    EXPECT_EQ(ctrl.DraftAction(), VarAction::EditCard, "Draft action should be EditCard");
    EXPECT_EQ(ctrl.DraftCard(), CardKind::Red, "Draft card should default to original Red");
    EXPECT_EQ(ctrl.SelectedIndex(), 0, "Configure should start on row 0 (Cartao)");

    // Downgrade Red -> Yellow (tests zero locks + post-0x13 warning)
    ctrl.HandleAction(NavAction::Left, st); // Red -> Yellow
    EXPECT_EQ(ctrl.DraftCard(), CardKind::Yellow, "Should allow downgrading Red to Yellow");
    EXPECT_TRUE(ctrl.IsPostState13RedRescindWarning(), "Should show non-blocking warning for post-0x13 red rescind");

    // Test Back navigation returns cleanly to Main
    ctrl.HandleAction(NavAction::Back, st);
    EXPECT_EQ(ctrl.Page(), HudPage::Main, "Back from Configure must return to Main");

    // Test Tab navigation & Back
    ctrl.HandleAction(NavAction::NextTab, st);
    EXPECT_EQ(ctrl.Page(), HudPage::Squad, "NextTab must open Squad tab");
    ctrl.HandleAction(NavAction::Back, st);
    EXPECT_EQ(ctrl.Page(), HudPage::Main, "Back from Squad must return to Main");

    // Now test arming "Converter em Penalti" (index 1 in Foul menu)
    ctrl.HandleAction(NavAction::Down, st);
    ctrl.HandleAction(NavAction::Confirm, st);
    EXPECT_EQ(ctrl.Page(), HudPage::Configure, "Should open Configure for ConvertToPenalty");
    EXPECT_EQ(ctrl.DraftAction(), VarAction::ConvertToPenalty, "Expected ConvertToPenalty");
    // Dynamic fields for Penalty: [0=Card, 1=Team, 2=Offender, 3=Confirm, 4=Back]
    EXPECT_EQ(ctrl.ActiveConfigFields().size(), static_cast<size_t>(5), "Penalty Configure must have 5 concise rows");
    ctrl.HandleAction(NavAction::Down, st); // Team
    ctrl.HandleAction(NavAction::Down, st); // Offender
    ctrl.HandleAction(NavAction::Down, st); // Confirm
    ctrl.HandleAction(NavAction::Confirm, st);
    EXPECT_EQ(ctrl.Page(), HudPage::Main, "Confirming verdict returns to Main showing green armed bar");
    const auto armed = st.PeekVerdict();
    EXPECT_TRUE(armed.armed, "Verdict must be armed in VarState");
    EXPECT_EQ(armed.action, VarAction::ConvertToPenalty, "Armed verdict mismatch");

    MarkPass("TestVarHudController");
}

// ---------------------------------------------------------------------------
// Test 8: InlineHook 9-byte stolen prologue on 16-byte aligned address (cmpxchg16b)
// ---------------------------------------------------------------------------
using FnNineByte = uint64_t(__fastcall*)(uint64_t* outBuf);
InlineHook g_testHook9;
int g_testHook9Hits = 0;

uint64_t __fastcall Detour_NineByte(uint64_t* outBuf) {
    ++g_testHook9Hits;
    return g_testHook9.Original<FnNineByte>()(outBuf);
}

void TestInlineHook16ByteAtomic() {
    // Mimics Judge::FinalizeFrame (0x14050CD70): 9 stolen bytes on 16-byte aligned page start:
    //   33 C0                    xor eax, eax                 (2 bytes)
    //   48 89 81 00 00 00 00     mov [rcx + 0], rax           (7 bytes -> 9 stolen total)
    //   48 C7 C0 7B 00 00 00     mov rax, 123                 (7 bytes)
    //   C3                       ret                          (1 byte)
    const uint8_t code[] = {
        0x33, 0xC0,
        0x48, 0x89, 0x81, 0x00, 0x00, 0x00, 0x00,
        0x48, 0xC7, 0xC0, 0x7B, 0x00, 0x00, 0x00,
        0xC3
    };
    void* execMem = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    EXPECT_TRUE(execMem != nullptr && (reinterpret_cast<uintptr_t>(execMem) & 0xFULL) == 0,
                "VirtualAlloc 16-byte alignment failed");
    std::memcpy(execMem, code, sizeof(code));
    FlushInstructionCache(GetCurrentProcess(), execMem, sizeof(code));

    auto fn = reinterpret_cast<FnNineByte>(execMem);
    uint64_t slot = 0xDEADBEEF12345678ULL;
    g_testHook9Hits = 0;

    EXPECT_TRUE(g_testHook9.Install(reinterpret_cast<uintptr_t>(execMem),
                                    reinterpret_cast<void*>(&Detour_NineByte), 9),
                "9-byte cmpxchg16b InlineHook::Install failed");
    EXPECT_EQ(fn(&slot), 123ULL, "9-byte hooked return value wrong");
    EXPECT_EQ(slot, 0ULL, "9-byte stolen instruction did not zero [rcx]");
    EXPECT_EQ(g_testHook9Hits, 1, "9-byte detour hit count wrong");

    EXPECT_TRUE(g_testHook9.Uninstall(), "9-byte Uninstall failed");
    EXPECT_EQ(std::memcmp(execMem, code, sizeof(code)), 0, "9-byte original code not restored");
    slot = 999;
    EXPECT_EQ(fn(&slot), 123ULL, "Post-uninstall 9-byte call failed");
    EXPECT_EQ(slot, 0ULL, "Post-uninstall 9-byte store failed");

    VirtualFree(execMem, 0, MEM_RELEASE);
    MarkPass("TestInlineHook16ByteAtomic");
}

// ---------------------------------------------------------------------------
// Test 9: Context freeze while focused, Mistaken Identity, Direct Card Mode, Idempotent Event Update
// ---------------------------------------------------------------------------
void TestRound1Regressions() {
    auto& st = VarState::Instance();
    st.ResetMatch();

    // Seed players: slot 4 already has 1 red card, slot 5 has 1 yellow card
    auto players = st.GetPlayers();
    players[4].reds = 1;
    players[4].onPitch = 0;
    players[5].yellows = 1;
    st.UpdatePlayers(players);

    // Push an official Foul + Yellow on slot 3
    VarEvent foul{};
    foul.type = VarEventType::Foul;
    foul.matchPhase = 0;
    foul.time60Hz = 3000.0f;
    foul.matchTick = 3000;
    foul.setPlayKind = SetPlayKind::FreeKick;
    foul.benefitedTeam = 1;
    foul.offenderSlot = 3;
    foul.victimSlot = 15;
    foul.cardKind = CardKind::Yellow;
    foul.id = st.PushEvent(foul);

    LiveTelemetry tel{};
    tel.enabled = true;
    tel.offlineAllowed = true;
    tel.matchActive = true;
    tel.matchPhase = 0;
    tel.fsmState = 0x1A;
    tel.replayMode = 2;
    tel.replayManualFlag = 1;
    tel.replayCursorPhase = 0;
    tel.replayCursorTime = 3000.0f;

    ReplaySyncResult syncFoul = st.SyncToReplayCursor(0, 3000.0f);
    EXPECT_TRUE(syncFoul.hasEvent, "Should sync to foul");

    VarHudController ctrl;
    const auto teams = st.GetTeams();
    ctrl.UpdateContext(tel, syncFoul, players, teams, st.GetRecentEvents(), MatchVarStats{}, HudConfig{});
    EXPECT_TRUE(ctrl.IsFocused(), "Should be focused automatically on entering Replay");
    EXPECT_TRUE(ctrl.HasActiveEvent(), "Should have active foul event");

    // 1. Draft freeze test: once Configure page is open, scrubbing away MUST NOT corrupt active draft/event
    ctrl.HandleAction(NavAction::Down, st);
    ctrl.HandleAction(NavAction::Down, st);
    ctrl.HandleAction(NavAction::Confirm, st);
    EXPECT_EQ(ctrl.Page(), HudPage::Configure, "Expected Configure page");
    EXPECT_EQ(ctrl.DraftOrigOffenderSlot(), 3, "Orig offender must stay 3");

    tel.replayCursorTime = 9000.0f;
    ReplaySyncResult syncFar = st.SyncToReplayCursor(0, 9000.0f);
    EXPECT_TRUE(!syncFar.hasEvent, "Far cursor should have no event");
    ctrl.UpdateContext(tel, syncFar, players, teams, st.GetRecentEvents(), MatchVarStats{}, HudConfig{});
    EXPECT_TRUE(ctrl.HasActiveEvent(), "Active event MUST stay frozen while in Configure");
    EXPECT_EQ(ctrl.ActiveEvent().id, foul.id, "Frozen event ID mismatch");

    // 2. Mistaken identity test: fields [0=Card, 1=Offender, 2=Confirm, 3=Back]
    ctrl.HandleAction(NavAction::Down, st); // row 1 (Offender)
    EXPECT_EQ(ctrl.SelectedIndex(), 1, "Should be on Offender row");
    ctrl.HandleAction(NavAction::Right, st); // 3 -> 4
    ctrl.HandleAction(NavAction::Right, st); // 4 -> 5
    EXPECT_EQ(ctrl.DraftOffenderSlot(), 5, "New offender should be 5");
    EXPECT_EQ(ctrl.DraftOrigOffenderSlot(), 3, "Orig offender must remain 3 for mistaken identity");
    ctrl.HandleAction(NavAction::Down, st); // row 2 (Confirm)
    ctrl.HandleAction(NavAction::Confirm, st);

    VarVerdict v{};
    EXPECT_TRUE(st.ConsumeVerdict(v), "Should consume mistaken-identity verdict");
    EXPECT_EQ(v.origOffenderSlot, 3, "Verdict origOffenderSlot wrong");
    EXPECT_EQ(v.offenderSlot, 5, "Verdict new offenderSlot wrong");
    EXPECT_TRUE(!v.directCardEdit, "Event-based edit should not be directCardEdit");

    // 3. Idempotent event update test
    st.UpdateEventAfterVerdict(v);
    const auto updatedSync = st.SyncToReplayCursor(0, 3000.0f);
    EXPECT_TRUE(updatedSync.hasEvent, "Updated event should still sync");
    EXPECT_EQ(updatedSync.event.offenderSlot, 5, "Event offenderSlot must update after verdict");
    EXPECT_TRUE(updatedSync.event.reviewedByVar, "Event must be marked reviewedByVar");

    // 4. Direct Card Mode via CARTOES: live cursor at NoFoul updates Main automatically -> pick player #5 (slot 4)
    ctrl.UpdateContext(tel, syncFar, players, teams, st.GetRecentEvents(), MatchVarStats{}, HudConfig{});
    EXPECT_TRUE(!ctrl.HasActiveEvent(), "Back on Main at far cursor, context should track NoFoul live");
    // In NoFoul menu, index 2 is "Aplicar ou Tirar Cartao" -> opens Squad on row 1 (slot 0)
    ctrl.HandleAction(NavAction::Down, st);
    ctrl.HandleAction(NavAction::Down, st);
    ctrl.HandleAction(NavAction::Confirm, st);
    EXPECT_EQ(ctrl.Page(), HudPage::Squad, "NoFoul card action must open Squad list");
    // Move from row 1 (slot 0) to row 5 (slot 4, who has 1 Red card)
    for (int i = 0; i < 4; ++i) ctrl.HandleAction(NavAction::Down, st);
    ctrl.HandleAction(NavAction::Confirm, st);
    EXPECT_EQ(ctrl.Page(), HudPage::Configure, "Picking player opens direct card Configure");
    EXPECT_TRUE(ctrl.DraftDirectCardMode(), "Squad EditCard must use directCardMode");
    EXPECT_EQ(ctrl.DraftOffenderSlot(), 4, "Picked player must be slot 4");
    EXPECT_EQ(ctrl.DraftDirectCardPreset(), 3, "Slot 4 with Red card should auto-sync to preset 3");
    // Row 0 is Card -> cycle 3 -> 0 (Sem cartoes) -> must trigger post-0x13 red rescind warning!
    ctrl.HandleAction(NavAction::Right, st);
    EXPECT_EQ(ctrl.DraftDirectCardPreset(), 0, "Expected preset 0 (Sem cartoes)");
    EXPECT_TRUE(ctrl.IsPostState13RedRescindWarning(), "Rescinding Red in direct mode post-0x13 must show warning");

    // Move to Confirm (row 2) and arm
    ctrl.HandleAction(NavAction::Down, st);
    ctrl.HandleAction(NavAction::Down, st);
    ctrl.HandleAction(NavAction::Confirm, st);
    VarVerdict dv{};
    EXPECT_TRUE(st.ConsumeVerdict(dv), "Should consume direct card verdict");
    EXPECT_TRUE(dv.directCardEdit, "directCardEdit must be true");
    EXPECT_EQ(dv.offenderSlot, 4, "directCardEdit offender mismatch");
    EXPECT_EQ(dv.directYellows, static_cast<uint8_t>(0), "directYellows must be 0");
    EXPECT_EQ(dv.directReds, static_cast<uint8_t>(0), "directReds must be 0");

    MarkPass("TestRound1Regressions");
}

// ---------------------------------------------------------------------------
// Test 10: Round 3 regressions (Advantage->Retroactive update, Sider overlay queue clear)
// ---------------------------------------------------------------------------
void TestRound3Regressions() {
    auto& st = VarState::Instance();
    st.ResetMatch();

    VarEvent adv{};
    adv.type = VarEventType::Advantage;
    adv.matchPhase = 0;
    adv.time60Hz = 4200.0f;
    adv.matchTick = 4200;
    adv.setPlayKind = SetPlayKind::FreeKick;
    adv.benefitedTeam = 0;
    adv.offenderSlot = 12;
    adv.cardKind = CardKind::None;
    adv.wasAdvantage = true;
    adv.id = st.PushEvent(adv);

    VarVerdict v{};
    v.armed = true;
    v.action = VarAction::RetroactivePenalty;
    v.sourceEventId = adv.id;
    v.offenderSlot = 12;
    v.victimSlot = 9;
    v.benefitedTeam = 0;
    v.origCard = CardKind::None;
    v.targetCard = CardKind::Yellow;
    st.UpdateEventAfterVerdict(v);

    const auto synced = st.SyncToReplayCursor(0, 4200.0f);
    EXPECT_TRUE(synced.hasEvent, "Advantage event should still sync after retroactive penalty");
    EXPECT_EQ(synced.event.type, VarEventType::Penalty, "Advantage event must upgrade to Penalty");
    EXPECT_EQ(synced.event.setPlayKind, SetPlayKind::PenaltyKick, "SetPlayKind must upgrade to PenaltyKick");
    EXPECT_EQ(synced.event.cardKind, CardKind::Yellow, "CardKind must update to Yellow");
    EXPECT_TRUE(synced.event.reviewedByVar, "Event must be marked reviewedByVar");

    // Verify queued keyboard actions are dropped while Sider overlay is open
    InputManager::QueueKeyboardAction(NavAction::Confirm);
    InputManager::QueueKeyboardAction(NavAction::Down);
    const auto dropped = InputManager::PollActions(true, true, true);
    EXPECT_TRUE(dropped.empty(), "PollActions must drop queued keys when Sider overlay is open");
    const auto afterClose = InputManager::PollActions(true, true, false);
    EXPECT_TRUE(afterClose.empty(), "Dropped keys must not leak after Sider overlay closes");

    MarkPass("TestRound3Regressions");
}

// ---------------------------------------------------------------------------
// Test 11: v2.0 features (box distance, restart spots, squad editor, quick swap,
// manual restart verdict, HUD fast-path, INI persistence)
// ---------------------------------------------------------------------------
void TestV2Features() {
    PitchDims d{};
    // Signed box distance
    const float inside = PitchGeometry::SignedDistanceToDefendingBox(d, Vec3{ 40.0f, 0.0f, 0.0f }, +1.0f);
    EXPECT_TRUE(inside < 0.0f && std::fabs(inside + 4.0f) < 0.01f, "Inside box should be -4.0m");
    const float outside = PitchGeometry::SignedDistanceToDefendingBox(d, Vec3{ 33.0f, 0.0f, 0.0f }, +1.0f);
    EXPECT_TRUE(std::fabs(outside - 3.0f) < 0.01f, "Outside front edge should be +3.0m");
    const float oppHalf = PitchGeometry::SignedDistanceToDefendingBox(d, Vec3{ -40.0f, 0.0f, 0.0f }, +1.0f);
    EXPECT_TRUE(oppHalf > 70.0f, "Opposite half must be far from defending box");

    const Vec3 corner = PitchGeometry::CalcCornerSpot(d, -1.0f, 5.0f);
    EXPECT_TRUE(corner.x < -50.0f && corner.z > 32.0f, "Corner spot wrong quadrant");
    const Vec3 gk = PitchGeometry::CalcGoalKickSpot(d, +1.0f, -3.0f);
    EXPECT_TRUE(std::fabs(gk.x - d.box6X) < 0.01f && gk.z < 0.0f, "Goal kick spot wrong");
    const Vec3 ti = PitchGeometry::CalcThrowInSpot(d, Vec3{ 70.0f, 0.0f, -10.0f });
    EXPECT_TRUE(ti.x <= d.halfLength - 1.5f + 0.01f && ti.z < -33.0f, "Throw-in spot wrong");

    auto& st = VarState::Instance();
    st.ResetMatch();
    st.SetEnabled(true);
    st.SetOfflineAllowed(true);

    // HUD fast path: idle -> no render, manual replay telemetry -> render
    st.SetDebugOverlayOpen(false);
    EXPECT_TRUE(!st.ShouldRenderAnyHud(GetTickCount64()), "Idle HUD must not render");
    LiveTelemetry tel{};
    tel.matchActive = true;
    tel.matchPhase = 0;
    tel.replayMode = 2;
    tel.replayManualFlag = 1;
    tel.fsmState = 0x1A;
    st.UpdateTelemetry(tel);
    EXPECT_TRUE(st.ShouldRenderAnyHud(GetTickCount64()), "Manual replay must render HUD");
    tel.replayManualFlag = 0;
    tel.replayMode = 0;
    st.UpdateTelemetry(tel);
    EXPECT_TRUE(!st.ShouldRenderAnyHud(GetTickCount64()), "Exiting replay must stop HUD render");
    st.SetPostApplyBanner("TESTE", 5000, BannerStyle::Applied);
    EXPECT_TRUE(st.ShouldRenderAnyHud(GetTickCount64()), "Active banner must render");
    st.SetPostApplyBanner(nullptr, 0, BannerStyle::Applied);

    // Squad editor: open CARTOES tab via NextTab, switch to Away, pick player #3 (row 3 -> slot 13)
    auto players = st.GetPlayers();
    players[13].yellows = 1;
    st.UpdatePlayers(players);
    tel.replayMode = 2;
    tel.replayManualFlag = 1;
    tel.enabled = true;
    tel.offlineAllowed = true;
    VarHudController ctrl;
    ctrl.UpdateContext(tel, ReplaySyncResult{}, players, st.GetTeams(), {}, MatchVarStats{}, HudConfig{});
    EXPECT_TRUE(ctrl.IsFocused(), "Auto-open should focus panel immediately");
    ctrl.HandleAction(NavAction::NextTab, st);
    EXPECT_EQ(ctrl.Page(), HudPage::Squad, "NextTab on Main must open Squad");
    ctrl.HandleAction(NavAction::Right, st);
    EXPECT_EQ(ctrl.SquadTeamFilter(), 1, "Right must switch to Away team");
    // Row 0 = Team selector, Row 1 = player 0, Row 2 = player 1, Row 3 = player 2 (slot 13)
    ctrl.HandleAction(NavAction::Down, st);
    ctrl.HandleAction(NavAction::Down, st);
    ctrl.HandleAction(NavAction::Down, st);
    ctrl.HandleAction(NavAction::Confirm, st);
    EXPECT_EQ(ctrl.Page(), HudPage::Configure, "Selecting player must open Configure");
    EXPECT_TRUE(ctrl.DraftDirectCardMode(), "Squad edit must be direct card mode");
    EXPECT_EQ(ctrl.DraftOffenderSlot(), 13, "Squad selection slot mismatch");
    EXPECT_EQ(ctrl.DraftDirectCardPreset(), 1, "Preset must sync to existing yellow");
    ctrl.HandleAction(NavAction::Back, st);
    EXPECT_EQ(ctrl.Page(), HudPage::Squad, "Back from Squad edit must return to Squad");

    // Manual restart verdict from NoFoul (index 3 = Bola Parada) cycles Corner -> GoalKick
    ctrl.HandleAction(NavAction::Back, st);
    EXPECT_EQ(ctrl.Page(), HudPage::Main, "Back from Squad returns to Main");
    for (int i = 0; i < 3; ++i) ctrl.HandleAction(NavAction::Down, st);
    ctrl.HandleAction(NavAction::Confirm, st);
    EXPECT_EQ(ctrl.DraftAction(), VarAction::RestartCorner, "Restart default must be Corner");
    ctrl.HandleAction(NavAction::Right, st); // Row 0 is RestartType -> cycles Corner -> GoalKick
    EXPECT_EQ(ctrl.DraftAction(), VarAction::RestartGoalKick, "Right must cycle to GoalKick");
    // Dynamic fields: [0=RestartType, 1=Team, 2=Spot, 3=Confirm, 4=Back]
    for (int i = 0; i < 3; ++i) ctrl.HandleAction(NavAction::Down, st);
    ctrl.HandleAction(NavAction::Confirm, st);
    VarVerdict rv{};
    EXPECT_TRUE(st.ConsumeVerdict(rv), "Restart verdict must be armed");
    EXPECT_EQ(rv.action, VarAction::RestartGoalKick, "Armed restart action mismatch");

    // INI persistence round-trip
    wchar_t tmpDir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmpDir);
    const std::wstring ini = std::wstring(tmpDir) + L"var_mod_v2_test.ini";
    DeleteFileW(ini.c_str());
    st.SetIniPath(ini);
    HudConfig cfg{};
    cfg.dockLeft = true;
    cfg.scalePreset = 2;
    cfg.showRadar = false;
    cfg.broadcastAlerts = false;
    cfg.allowMouse = false;
    st.SetHudConfig(cfg, true);
    st.SetHudConfig(HudConfig{}, false);
    st.LoadHudConfig();
    const HudConfig loaded = st.GetHudConfig();
    DeleteFileW(ini.c_str());
    st.SetIniPath(L"");
    st.SetHudConfig(HudConfig{}, false);
    EXPECT_TRUE(loaded.dockLeft && loaded.scalePreset == 2 && !loaded.showRadar &&
                !loaded.broadcastAlerts && loaded.allowMouse, "INI round-trip mismatch (v3.0 keeps mouse enabled)");

    // Verify InlineHook::MeasureSafeStolenLen on exact Windows 11 dxgi!Present & dxgi!ResizeBuffers prologues
    const uint8_t dxgiPresentPrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18 };
    uintptr_t pAddr = reinterpret_cast<uintptr_t>(dxgiPresentPrologue);
    EXPECT_EQ(InlineHook::MeasureSafeStolenLen(pAddr), static_cast<size_t>(5), "dxgi!Present stolen len must be 5");

    const uint8_t dxgiResizePrologue[] = { 0x48, 0x8B, 0xC4, 0x44, 0x89, 0x48, 0x20, 0x44, 0x89, 0x40, 0x18 };
    uintptr_t rAddr = reinterpret_cast<uintptr_t>(dxgiResizePrologue);
    EXPECT_EQ(InlineHook::MeasureSafeStolenLen(rAddr), static_cast<size_t>(7), "dxgi!ResizeBuffers stolen len must be 7");

    // Verify InlineHook chaining over an existing 5-byte E9 rel32 jump (Steam Overlay scenario)
    uint8_t* page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    EXPECT_TRUE(page != nullptr, "VirtualAlloc for E9 chain test failed");
    // Entry at page+0: jmp page+64 (E9 3B 00 00 00)
    // Target at page+64: mov eax, 77; ret (B8 4D 00 00 00 C3)
    page[0] = 0xE9;
    const int32_t rel64 = 64 - 5;
    std::memcpy(page + 1, &rel64, sizeof(rel64));
    const uint8_t targetBody[] = { 0xB8, 0x4D, 0x00, 0x00, 0x00, 0xC3 };
    std::memcpy(page + 64, targetBody, sizeof(targetBody));
    FlushInstructionCache(GetCurrentProcess(), page, 128);

    uintptr_t e9Addr = reinterpret_cast<uintptr_t>(page);
    EXPECT_EQ(InlineHook::MeasureSafeStolenLen(e9Addr), static_cast<size_t>(5), "E9 entry stolen len must be 5");

    static InlineHook e9Hook;
    static int e9Hits = 0;
    e9Hits = 0;
    using FnRetInt = int(__fastcall*)();
    auto e9Detour = []() -> int {
        ++e9Hits;
        return e9Hook.Original<FnRetInt>()() + 3;
    };
    EXPECT_TRUE(e9Hook.Install(e9Addr, reinterpret_cast<void*>(+e9Detour), 5), "E9 chain InlineHook::Install failed");
    EXPECT_EQ(reinterpret_cast<FnRetInt>(page)(), 80, "E9 chained call must return 77 + 3 = 80");
    EXPECT_EQ(e9Hits, 1, "E9 detour hit count mismatch");
    EXPECT_TRUE(e9Hook.Uninstall(), "E9 chain Uninstall failed");
    EXPECT_EQ(reinterpret_cast<FnRetInt>(page)(), 77, "Post-uninstall E9 call must return 77");

    // Live execution test of exact PES2021.exe GrSwapChain::Present0 (0x141FF46A0) machine code:
    //   mov rcx, [rcx+0x20]; xor r8d, r8d; mov rax, [rcx]; jmp [rax+0x40]
    const uint8_t grPresentStub[] = {
        0x48, 0x8B, 0x49, 0x20,
        0x45, 0x33, 0xC0,
        0x48, 0x8B, 0x01,
        0x48, 0xFF, 0x60, 0x40
    };
    std::memcpy(page + 128, grPresentStub, sizeof(grPresentStub));
    FlushInstructionCache(GetCurrentProcess(), page + 128, 32);

    struct MockSwapChain { void** vtbl; int tag; };
    struct MockGrSwapChain { uint8_t pad[0x20]; MockSwapChain* sc; };
    static int mockPresentCalls = 0;
    static int mockDetourCalls = 0;
    static MockSwapChain* mockObservedSc = nullptr;
    mockPresentCalls = 0;
    mockDetourCalls = 0;
    mockObservedSc = nullptr;

    auto mockVtblPresent = [](MockSwapChain* sc, unsigned int syncInterval, unsigned int flags) -> long {
        ++mockPresentCalls;
        return static_cast<long>((sc ? sc->tag : 0) + static_cast<int>(syncInterval) * 10 + static_cast<int>(flags));
    };
    void* mockVtbl[16] = {};
    mockVtbl[8] = reinterpret_cast<void*>(+mockVtblPresent); // +0x40
    MockSwapChain mockSc{ mockVtbl, 500 };
    MockGrSwapChain mockGr{};
    mockGr.sc = &mockSc;

    using FnMockGrPresent = long(__fastcall*)(MockGrSwapChain*, unsigned int);
    uintptr_t grAddr = reinterpret_cast<uintptr_t>(page + 128);
    EXPECT_EQ(InlineHook::MeasureSafeStolenLen(grAddr), static_cast<size_t>(7), "GrSwapChain::Present0 stolen len must be 7");

    static InlineHook grHook;
    auto grDetour = [](MockGrSwapChain* gr, unsigned int syncInterval) -> long {
        ++mockDetourCalls;
        mockObservedSc = gr ? gr->sc : nullptr;
        return grHook.Original<FnMockGrPresent>()(gr, syncInterval);
    };
    EXPECT_TRUE(grHook.Install(grAddr, reinterpret_cast<void*>(+grDetour), 7), "GrSwapChain::Present0 InlineHook install failed");
    EXPECT_EQ(reinterpret_cast<FnMockGrPresent>(page + 128)(&mockGr, 1U), 510L, "Hooked GrSwapChain::Present0 return value mismatch");
    EXPECT_EQ(mockDetourCalls, 1, "GrSwapChain::Present0 detour hit count mismatch");
    EXPECT_EQ(mockPresentCalls, 1, "GrSwapChain::Present0 vtable[8] call count mismatch");
    EXPECT_EQ(mockObservedSc, &mockSc, "GrSwapChain::Present0 SwapChain* mismatch");
    EXPECT_TRUE(grHook.Uninstall(), "GrSwapChain::Present0 Uninstall failed");
    EXPECT_EQ(reinterpret_cast<FnMockGrPresent>(page + 128)(&mockGr, 1U), 510L, "Post-uninstall GrSwapChain::Present0 return mismatch");
    EXPECT_EQ(mockPresentCalls, 2, "Post-uninstall vtable[8] call count mismatch");

    VirtualFree(page, 0, MEM_RELEASE);

    MarkPass("TestV2Features");
}

// ---------------------------------------------------------------------------
// Test 12: v3.0 regressions (260ms input-block grace window, ClearReplayFocusAndGrace,
// QuickSwap navigation, SetSelectedIndex bounds)
// ---------------------------------------------------------------------------
void TestV3Regressions() {
    auto& st = VarState::Instance();
    st.ResetMatch();
    st.SetEnabled(true);
    st.SetOfflineAllowed(true);
    st.SetSiderOverlayOpen(false);

    const uint64_t now = GetTickCount64();

    // 1. Outside Manual Replay: must NEVER block game input even if panelFocused were toggled
    st.SetInManualReplay(false);
    st.SetPanelFocused(true);
    EXPECT_TRUE(!st.ShouldBlockGameInputNow(now), "Must never block game input outside Manual Replay");

    // 2. Inside Manual Replay: block while focused AND during 260ms grace window after closing
    st.SetInManualReplay(true);
    st.SetPanelFocused(true);
    EXPECT_TRUE(st.ShouldBlockGameInputNow(GetTickCount64()), "Must block input while focused in Replay");

    st.SetPanelFocused(false);
    const uint64_t closeMs = GetTickCount64();
    EXPECT_TRUE(!st.IsPanelFocused(), "Panel focus must turn false immediately");
    EXPECT_TRUE(st.ShouldBlockGameInputNow(closeMs + 50ULL), "260ms grace window must stay active right after close");
    EXPECT_TRUE(!st.ShouldBlockGameInputNow(closeMs + 400ULL), "260ms grace window must expire cleanly");

    // 3. Exiting Manual Replay or ClearReplayFocusAndGrace cancels grace immediately
    st.SetPanelFocused(true);
    st.SetPanelFocused(false);
    st.ClearReplayFocusAndGrace();
    EXPECT_TRUE(!st.ShouldBlockGameInputNow(GetTickCount64()), "ClearReplayFocusAndGrace must unblock immediately");

    // 4. QuickSwap (Y / Tab) and SetSelectedIndex bounds
    LiveTelemetry tel{};
    tel.enabled = true;
    tel.offlineAllowed = true;
    tel.matchActive = true;
    tel.matchPhase = 0;
    tel.fsmState = 0x1C;
    tel.replayMode = 2;
    tel.replayManualFlag = 1;

    VarHudController ctrl;
    ctrl.UpdateContext(tel, ReplaySyncResult{}, st.GetPlayers(), st.GetTeams(), {}, MatchVarStats{}, HudConfig{});
    EXPECT_EQ(ctrl.Page(), HudPage::Main, "Should open on Main");
    ctrl.HandleAction(NavAction::NextTab, st);
    EXPECT_EQ(ctrl.Page(), HudPage::Squad, "NextTab on Main must jump to CARTOES");
    EXPECT_EQ(ctrl.SquadTeamFilter(), 0, "Default team filter should be Home");
    ctrl.HandleAction(NavAction::QuickSwap, st);
    EXPECT_EQ(ctrl.SquadTeamFilter(), 1, "QuickSwap on Squad must toggle Home/Away");
    ctrl.SetSelectedIndex(999);
    EXPECT_TRUE(ctrl.SelectedIndex() < 13, "SetSelectedIndex must ignore out-of-range values");

    // Back to Main -> Marcar Falta (index 0) -> Configure -> QuickSwap toggles Team
    ctrl.HandleAction(NavAction::Back, st);
    ctrl.SelectMainOption(0, st);
    EXPECT_EQ(ctrl.Page(), HudPage::Configure, "Expected Configure for RetroactiveFreeKick");
    const int32_t teamBefore = ctrl.DraftBenefitedTeam();
    ctrl.HandleAction(NavAction::QuickSwap, st);
    EXPECT_EQ(ctrl.DraftBenefitedTeam(), 1 - teamBefore, "QuickSwap in Configure must toggle benefited team");

    MarkPass("TestV3Regressions");
}

// ---------------------------------------------------------------------------
// Test 13: v4.0 Engine Layer (verified GMATCH offsets, kit colors, circular
// Replay ring math, #N NOME broadcast formatting, 60Hz physical incident
// clustering, replay command queue)
// ---------------------------------------------------------------------------
void TestV4EngineAndDataExtraction() {
    // 1. Verify all 5 critical GMATCH offsets + new v4.0 struct offsets
    EXPECT_EQ(Offsets::GMATCH_TeamTacticsBase,  static_cast<uintptr_t>(0x1928), "GMATCH_TeamTacticsBase must be 0x1928");
    EXPECT_EQ(Offsets::GMATCH_TeamInfoBase,     static_cast<uintptr_t>(0x1978), "GMATCH_TeamInfoBase must be 0x1978");
    EXPECT_EQ(Offsets::GMATCH_PlayerActionBase, static_cast<uintptr_t>(0x19C8), "GMATCH_PlayerActionBase must be 0x19C8");
    EXPECT_EQ(Offsets::GMATCH_PlayerSpatialBase,static_cast<uintptr_t>(0x1DD8), "GMATCH_PlayerSpatialBase must be 0x1DD8");
    EXPECT_EQ(Offsets::GMATCH_PlayerEntityBase, static_cast<uintptr_t>(0x25F8), "GMATCH_PlayerEntityBase must be 0x25F8");
    EXPECT_EQ(Offsets::BallState_OwnerSlot,     static_cast<uintptr_t>(0x2601C),"BallState_OwnerSlot must be 0x2601C");
    EXPECT_EQ(Offsets::TeamInfo_ActiveRosterIdx,static_cast<uintptr_t>(0x04),   "TeamInfo_ActiveRosterIdx must be 0x04");
    EXPECT_EQ(Offsets::TeamEntry_ShirtNumbers,  static_cast<uintptr_t>(0x29A),  "TeamEntry_ShirtNumbers must be 0x29A (live-verified)");
    EXPECT_EQ(Offsets::TeamEntry_KitTable,      static_cast<uintptr_t>(0x5D8),  "TeamEntry_KitTable must be 0x5D8");
    EXPECT_EQ(Offsets::EditedKit_PrimaryRGB,    static_cast<uintptr_t>(0x1991), "EditedKit_PrimaryRGB must be 0x1991");
    EXPECT_EQ(Offsets::ReplayCtrl_CurFrame,     static_cast<uintptr_t>(0x288),  "ReplayCtrl_CurFrame must be 0x288");
    EXPECT_EQ(Offsets::ReplayRing_FrameStride,  static_cast<size_t>(0x1FB0),    "ReplayRing_FrameStride must be 0x1FB0");

    // 2. Kit color radar lift & team contrast separation (mirrors 0x141519B80)
    const RgbColor liftedBlack = ColorHelpers::LiftForDarkRadar(RgbColor{ 10, 12, 16 });
    EXPECT_TRUE(liftedBlack.r >= 0x40 && liftedBlack.g >= 0x40 && liftedBlack.b >= 0x40,
                "Pure black kit must be lifted for dark 2D radar visibility");
    RgbColor hPri{ 220, 20, 30 }, hSec{ 20, 20, 20 };
    RgbColor aPri{ 215, 25, 32 }, aSec{ 245, 245, 245 };
    ColorHelpers::EnsureDistinctTeamColors(hPri, hSec, aPri, aSec);
    EXPECT_TRUE(ColorHelpers::ColorDistance(hPri, aPri) >= 62.0f,
                "Clashing Home/Away primary colors must be separated cleanly");

    // 3. Replay circular ring-buffer math & binary search
    EXPECT_TRUE(std::fabs(ReplayTimelineMath::ComputeTimelineNorm(300U, 600U) - 0.5f) < 0.001f, "Timeline norm 50% wrong");
    EXPECT_EQ(ReplayTimelineMath::ComputeWindowDuration(900, 100, 1000), static_cast<uint16_t>(200),
              "Wrapped circular window duration wrong");
    EXPECT_EQ(ReplayTimelineMath::ComputeFrameFromNorm(900, 100, 1000, 0.75f), static_cast<uint16_t>(50),
              "Wrapped circular frame from norm 0.75 wrong");

    // Synthetic wrapped circular buffer spanning a halftime boundary:
    //   rel 0..99 (frames 900..999): phase 0 (1st half), time60Hz = 160000 + rel * 2
    //   rel 100..200 (frames 0..100): phase 2 (2nd half), time60Hz = 100 + (rel - 100) * 2
    auto readWrappedTime = [](uint16_t frame) -> float {
        if (frame >= 900) return 160000.0f + static_cast<float>(frame - 900U) * 2.0f;
        return 100.0f + static_cast<float>(frame) * 2.0f;
    };
    auto readWrappedPhase = [](uint16_t frame) -> int32_t {
        return (frame >= 900) ? 0 : 2;
    };
    const uint16_t foundHalf1 = ReplayTimelineMath::FindClosestFrameByTime(
        900, 100, 1000, 0, 160060.0f, readWrappedTime, readWrappedPhase);
    EXPECT_EQ(foundHalf1, static_cast<uint16_t>(930), "Circular ring search failed for 1st-half frame");
    const uint16_t foundHalf2 = ReplayTimelineMath::FindClosestFrameByTime(
        900, 100, 1000, 2, 160.0f, readWrappedTime, readWrappedPhase);
    EXPECT_EQ(foundHalf2, static_cast<uint16_t>(30), "Circular ring search across halftime boundary failed");

    // 4. Broadcast player formatting (#N NOME)
    PlayerSlotInfo p{};
    p.shirtNumber = 10;
    std::snprintf(p.name, sizeof(p.name), "GABIGOL");
    char bname[64] = {};
    p.FormatBroadcastName(bname, sizeof(bname));
    EXPECT_EQ(std::strcmp(bname, "#10 GABIGOL"), 0, "FormatBroadcastName with shirt number wrong");
    p.shirtNumber = 0;
    p.FormatBroadcastName(bname, sizeof(bname));
    EXPECT_EQ(std::strcmp(bname, "GABIGOL"), 0, "FormatBroadcastName without shirt number wrong");

    // 5. Physical incident detection (victim contact reaction + engine causer) + clustering + official merge
    auto& st = VarState::Instance();
    st.ResetMatch();
    PitchDims dims{};
    const float dirs[2] = { -1.0f, +1.0f }; // Home defends -X, Away defends +X

    PlaySnapshot snap{};
    snap.matchPhase = 0;
    snap.time60Hz = 5200.0f;
    snap.matchTick = 5200;
    snap.ballPos = Vec3{ 43.0f, 0.0f, 3.0f };
    for (int i = 0; i < 22; ++i) {
        snap.playerOnPitch[i] = 1;
        snap.playerPos[i] = Vec3{ -20.0f + static_cast<float>(i), 0.0f, -15.0f };
    }
    snap.playerPos[9]  = Vec3{ 43.0f, 0.0f, 3.0f };  // Home #10 attacker inside Away +X box
    snap.playerPos[14] = Vec3{ 42.3f, 0.0f, 3.2f };  // Away defender sliding in!
    snap.ballOwnerSlot = 9;
    snap.recentBallOwnerSlot = 9;
    // Engine model: the victim plays a contact reaction (17 fall) and PlayerAction + 0x3D names the causer (14),
    // whose own action is a slide tackle (11)
    std::memset(snap.playerCauser, -1, sizeof(snap.playerCauser));
    snap.playerActionId[14] = PlayerActionId::Sliding;
    snap.playerActionId[9]  = PlayerActionId::FallDown;
    snap.playerCauser[9]    = 14;
    VarState::ComputeClosestContact(snap);

    DetectedIncident inc{};
    EXPECT_TRUE(VarState::DetectPhysicalIncident(snap, dims, dirs, inc), "Slide tackle in box must be detected");
    EXPECT_EQ(inc.kind, IncidentKind::SlideTackle, "Incident kind must be SlideTackle");
    EXPECT_EQ(inc.offenderSlot, 14, "Slide tackler (slot 14) must be offender");
    EXPECT_EQ(inc.victimSlot, 9, "Ball carrier (slot 9) must be victim");
    EXPECT_TRUE(inc.insideBox, "Slide tackle at (42.6, 3.1) in +X box must set insideBox=true");
    st.PushSnapshot(snap);
    st.PushDetectedIncident(inc);

    // Push a weaker BodyCharge 30 ticks later (0.5s, inside 1.2s cluster window) -> must merge & keep higher priority SlideTackle
    DetectedIncident weak = inc;
    weak.time60Hz = 5230.0f;
    weak.kind = IncidentKind::BodyCharge;
    weak.priority = 30;
    st.PushDetectedIncident(weak);

    // Push an official foul at 5220 missing victimSlot (-1) -> GetReplayWindowMarkers must enrich victimSlot from physical incident!
    VarEvent offFoul{};
    offFoul.type = VarEventType::Foul;
    offFoul.matchPhase = 0;
    offFoul.time60Hz = 5220.0f;
    offFoul.offenderSlot = 14;
    offFoul.victimSlot = -1;
    offFoul.benefitedTeam = 0;
    offFoul.pos = Vec3{ 42.6f, 0.0f, 3.1f };
    const uint32_t offFoulId = st.PushEvent(offFoul);

    const auto markers = st.GetReplayWindowMarkers(0, 5000.0f, 5600.0f);
    EXPECT_EQ(markers.size(), static_cast<size_t>(1), "Clustered physical incident + official foul within 0.3s must deduplicate to 1 marker");
    EXPECT_TRUE(markers[0].isOfficialEvent, "Merged marker must retain official event flag");
    EXPECT_EQ(markers[0].offenderSlot, 14, "Merged marker offender mismatch");
    EXPECT_EQ(markers[0].victimSlot, 9, "Official foul missing victimSlot must inherit physical victim slot 9");

    // Verify overturned official foul no longer labels itself as "FALTA MARCADA"
    VarVerdict overturnV{};
    overturnV.action = VarAction::OverturnCancel;
    overturnV.sourceEventId = offFoulId;
    st.UpdateEventAfterVerdict(overturnV);
    const auto markersAfterOverturn = st.GetReplayWindowMarkers(0, 5000.0f, 5600.0f);
    EXPECT_EQ(markersAfterOverturn.size(), static_cast<size_t>(1), "Overturned marker count mismatch");
    EXPECT_EQ(markersAfterOverturn[0].kind, IncidentKind::None, "Overturned foul must clear IncidentKind::Foul");
    EXPECT_TRUE(std::strstr(markersAfterOverturn[0].label, "Anulado") != nullptr,
                "Overturned marker must show Anulado label");

    // Verify PushDetectedIncident upgrades timestamp when priority jumps by >= 8 (oldPriority bug regression)
    DetectedIncident low{};
    low.matchPhase = 0; low.time60Hz = 6000.0f; low.kind = IncidentKind::BodyCharge;
    low.priority = 52; low.offenderSlot = 14; low.victimSlot = 9; low.pos = Vec3{ 10.0f, 0.0f, 0.0f };
    st.PushDetectedIncident(low);
    DetectedIncident high = low;
    high.time60Hz = 6025.0f; high.kind = IncidentKind::SlideTackle; high.priority = 80;
    st.PushDetectedIncident(high);
    const auto upMarkers = st.GetReplayWindowMarkers(0, 5950.0f, 6100.0f);
    EXPECT_EQ(upMarkers.size(), static_cast<size_t>(1), "Upgraded cluster must remain 1 marker");
    EXPECT_TRUE(std::fabs(upMarkers[0].time60Hz - 6025.0f) < 0.01f,
                "Upgraded high-severity SlideTackle must update cluster timestamp to impact moment");

    // 6. ReplayControlCommand queue
    st.RequestReplaySeekToTime(0, 5220.0f, 0.37f, true);
    st.RequestReplayCameraFocus(15);
    ReplayControlCommand cmd{};
    EXPECT_TRUE(st.ConsumeReplayCommand(cmd), "ConsumeReplayCommand must return true");
    EXPECT_TRUE(cmd.hasSeek && cmd.seekByTime && cmd.pauseAfterSeek, "Seek command fields wrong");
    EXPECT_TRUE(std::fabs(cmd.targetTime60Hz - 5220.0f) < 0.01f, "Seek targetTime wrong");
    EXPECT_TRUE(cmd.hasFocusTarget && cmd.focusSlot == 15, "Combined camera focus command lost");
    EXPECT_TRUE(!st.ConsumeReplayCommand(cmd), "Second ConsumeReplayCommand must return false");

    MarkPass("TestV4EngineAndDataExtraction");
}

// ---------------------------------------------------------------------------
// Test 14: v4.0 Broadcast HUD, 1:1 2D Pitch Radar Auto Box Zoom, Offside Line,
// Native Timeline Seek, Camera Focus, Role Inversion & 6-row INI Persistence
// ---------------------------------------------------------------------------
void TestV4BroadcastHudAndRadar() {
    PitchDims dims{};
    const float aspect = 452.0f / 184.0f;

    // 1. PitchRadarViewport: Full pitch & Auto Penalty-Box Zoom (strict 1:1 metric scale + round-trip)
    const PitchRadarViewport vpFull = PitchRadarViewport::Compute(dims, Vec3{ 0.0f, 0.0f, 0.0f }, true, 0, aspect);
    EXPECT_TRUE(!vpFull.isZoomed && !vpFull.isPlayView && vpFull.zoomSide == 0, "Full pitch override must show the whole pitch");
    const PitchRadarViewport vpPlay = PitchRadarViewport::Compute(dims, Vec3{ -10.0f, 0.0f, 5.0f }, true, -1, aspect);
    EXPECT_TRUE(vpPlay.isPlayView && !vpPlay.isZoomed, "Away from the boxes the automatic view frames the play");
    EXPECT_TRUE(std::fabs((vpPlay.maxX - vpPlay.minX) / (vpPlay.maxZ - vpPlay.minZ) - aspect) < 0.02f ||
                (vpPlay.maxX - vpPlay.minX) >= 2.0f * (dims.halfLength + 3.0f) - 0.01f, "Play view keeps the 1:1 metric aspect");
    EXPECT_TRUE(vpPlay.minX <= -10.0f && vpPlay.maxX >= -10.0f && vpPlay.minZ <= 5.0f && vpPlay.maxZ >= 5.0f,
                "Play view contains the play");
    const PitchRadarViewport vpNoAuto = PitchRadarViewport::Compute(dims, Vec3{ -10.0f, 0.0f, 5.0f }, false, -1, aspect);
    EXPECT_TRUE(!vpNoAuto.isPlayView && !vpNoAuto.isZoomed, "Automatic zoom off keeps the whole pitch");
    const float fullRatio = (vpFull.maxX - vpFull.minX) / (vpFull.maxZ - vpFull.minZ);
    EXPECT_TRUE(std::fabs(fullRatio - aspect) < 0.02f, "Full pitch viewport must preserve exact 1:1 metric aspect");

    const PitchRadarViewport vpBoxR = PitchRadarViewport::Compute(dims, Vec3{ 44.0f, 0.0f, 8.0f }, true, -1, aspect);
    EXPECT_TRUE(vpBoxR.isZoomed && vpBoxR.zoomSide == 1, "Spot inside +X box must auto-zoom to right goal");
    const float zoomRatio = (vpBoxR.maxX - vpBoxR.minX) / (vpBoxR.maxZ - vpBoxR.minZ);
    EXPECT_TRUE(std::fabs(zoomRatio - aspect) < 0.02f, "Zoomed box viewport must preserve exact 1:1 metric aspect");

    const PitchRadarViewport vpBoxL = PitchRadarViewport::Compute(dims, Vec3{ -42.0f, 0.0f, -12.0f }, true, -1, aspect);
    EXPECT_TRUE(vpBoxL.isZoomed && vpBoxL.zoomSide == -1, "Spot inside -X box must auto-zoom to left goal");

    float u = 0.0f, v = 0.0f;
    vpBoxR.WorldToNorm(44.0f, 8.0f, u, v);
    const Vec3 roundTrip = vpBoxR.NormToWorld(u, v, dims);
    EXPECT_TRUE(std::fabs(roundTrip.x - 44.0f) < 0.05f && std::fabs(roundTrip.z - 8.0f) < 0.05f,
                "PitchRadarViewport WorldToNorm/NormToWorld round-trip inaccurate");

    // 2. Second-to-last defender X calculation (ignores GK and off-pitch players)
    PlaySnapshot snap{};
    for (int i = 0; i < 22; ++i) {
        snap.playerOnPitch[i] = 1;
        snap.playerPos[i] = Vec3{ 0.0f, 0.0f, 0.0f };
    }
    snap.playerPos[11] = Vec3{ 50.0f, 0.0f, 0.0f }; // Away GK closest to +X goal
    snap.playerPos[12] = Vec3{ 39.5f, 0.0f, 5.0f }; // Away CB 2nd-last defender
    snap.playerPos[13] = Vec3{ 36.0f, 0.0f, -4.0f };
    snap.playerPos[14] = Vec3{ 48.0f, 0.0f, 1.0f };
    snap.playerOnPitch[14] = 0; // Expelled player must be ignored!
    float offsideX = 0.0f;
    EXPECT_TRUE(VarHudController::ComputeSecondLastDefenderX(snap, 1, +1.0f, offsideX), "Offside calc failed");
    EXPECT_TRUE(std::fabs(offsideX - 39.5f) < 0.01f, "Second-last defender X must be 39.5m");

    // 3. VarHudController v4.0 end-to-end (real names, markers, native Seek, Camera Focus, Role Invert, AutoZoom)
    auto& st = VarState::Instance();
    st.ResetMatch();
    auto players = st.GetPlayers();
    players[9].shirtNumber = 10;
    std::snprintf(players[9].name, sizeof(players[9].name), "GABIGOL");
    players[14].shirtNumber = 15;
    std::snprintf(players[14].name, sizeof(players[14].name), "GOMEZ");
    st.UpdatePlayers(players);

    auto teams = st.GetTeams();
    std::snprintf(teams[0].fullName, sizeof(teams[0].fullName), "FLAMENGO");
    std::snprintf(teams[0].shortName, sizeof(teams[0].shortName), "FLA");
    std::snprintf(teams[1].fullName, sizeof(teams[1].fullName), "PALMEIRAS");
    std::snprintf(teams[1].shortName, sizeof(teams[1].shortName), "PAL");
    st.UpdateTeams(teams);

    LiveTelemetry tel{};
    tel.enabled = true;
    tel.offlineAllowed = true;
    tel.matchActive = true;
    tel.matchPhase = 0;
    tel.fsmState = 0x1A;
    tel.replayMode = 2;
    tel.replayManualFlag = 1;
    tel.replayIsPlaying = 0;
    tel.replayCursorPhase = 0;
    tel.replayCursorTime = 5000.0f;
    tel.replayWindowStartTime = 4800.0f;
    tel.replayWindowEndTime = 5600.0f;
    tel.teamAttackDir[0] = -1.0f;
    tel.teamAttackDir[1] = +1.0f;

    ReplayMarker m0{};
    m0.matchPhase = 0;
    m0.time60Hz = 5100.0f;
    m0.timelineNorm = 0.375f;
    m0.kind = IncidentKind::SlideTackle;
    m0.rolesConfirmed = true; // a proven contact: offender / victim are roles
    m0.offenderSlot = 14;
    m0.victimSlot = 9;
    m0.pos = Vec3{ 43.0f, 0.0f, 4.0f };
    std::snprintf(m0.label, sizeof(m0.label), "CARRINHO NA AREA");

    ReplayMarker m1{};
    m1.matchPhase = 0;
    m1.time60Hz = 5400.0f;
    m1.timelineNorm = 0.75f;
    m1.kind = IncidentKind::BodyCharge;
    m1.rolesConfirmed = true;
    m1.offenderSlot = 9;
    m1.victimSlot = 14;
    m1.pos = Vec3{ 5.0f, 0.0f, -2.0f };
    std::snprintf(m1.label, sizeof(m1.label), "CHOQUE FISICO");

    VarHudController ctrl;
    ctrl.UpdateContext(tel, ReplaySyncResult{}, players, teams, {}, MatchVarStats{}, HudConfig{});
    ctrl.UpdateReplayMarkers({ m0, m1 });

    // Select marker 0 -> triggers native Seek command, locks marker preview time, auto-zooms radar!
    ctrl.SelectReplayMarker(0, st, true);
    EXPECT_EQ(ctrl.SelectedMarkerIndex(), 0, "Selected marker should be 0");
    EXPECT_EQ(ctrl.DetectedOffenderSlot(), 14, "Marker 0 offender must auto-fill slot 14");
    EXPECT_EQ(ctrl.DetectedVictimSlot(), 9, "Marker 0 victim must auto-fill slot 9");
    EXPECT_TRUE(ctrl.IsEffectiveRadarZoomed(), "Marker 0 inside box must trigger Auto Box Zoom");
    int32_t lockPh = -1;
    float lockT = 0.0f;
    EXPECT_TRUE(ctrl.GetLockedMarkerTime(lockPh, lockT) && std::fabs(lockT - 5100.0f) < 0.01f,
                "GetLockedMarkerTime must expose marker 0 timestamp for instant radar preview");

    ReplayControlCommand seekCmd{};
    EXPECT_TRUE(st.ConsumeReplayCommand(seekCmd) && seekCmd.hasSeek && std::fabs(seekCmd.targetTime60Hz - 5100.0f) < 0.01f,
                "SelectReplayMarker must enqueue native Seek command");

    // Step next incident (]) -> selects marker 1
    ctrl.HandleAction(NavAction::NextIncident, st);
    EXPECT_EQ(ctrl.SelectedMarkerIndex(), 1, "NextIncident must advance to marker 1");
    EXPECT_EQ(ctrl.DetectedOffenderSlot(), 9, "Marker 1 offender must auto-fill slot 9");
    EXPECT_TRUE(!ctrl.IsEffectiveRadarZoomed(), "Midfield marker 1 must zoom back out to Full Pitch");
    EXPECT_TRUE(st.ConsumeReplayCommand(seekCmd) && std::fabs(seekCmd.targetTime60Hz - 5400.0f) < 0.01f,
                "NextIncident must enqueue native Seek to marker 1");

    // Invert roles on Main -> must persist across subsequent UpdateContext frame!
    ctrl.HandleAction(NavAction::QuickSwap, st);
    EXPECT_EQ(ctrl.DetectedOffenderSlot(), 14, "QuickSwap on Main must swap offender to 14");
    EXPECT_EQ(ctrl.DetectedVictimSlot(), 9, "QuickSwap on Main must swap victim to 9");
    ctrl.UpdateContext(tel, ReplaySyncResult{}, players, teams, {}, MatchVarStats{}, HudConfig{});
    EXPECT_EQ(ctrl.DetectedOffenderSlot(), 14, "Swapped offender MUST persist across next UpdateContext");

    // Camera Focus cycle: Offender (15) -> Victim (10) -> Ball (0)
    ctrl.CycleCameraFocus(st);
    ReplayControlCommand camCmd{};
    EXPECT_TRUE(st.ConsumeReplayCommand(camCmd) && camCmd.hasFocusTarget && camCmd.focusSlot == 15,
                "First camera focus must target offender (slot 14 + 1 = 15)");
    tel.replayFocusSlot = 15;
    ctrl.UpdateContext(tel, ReplaySyncResult{}, players, teams, {}, MatchVarStats{}, HudConfig{});
    ctrl.CycleCameraFocus(st);
    EXPECT_TRUE(st.ConsumeReplayCommand(camCmd) && camCmd.hasFocusTarget && camCmd.focusSlot == 10,
                "Second camera focus must target victim (slot 9 + 1 = 10)");

    // Play/Pause toggle (P)
    ctrl.HandleAction(NavAction::TogglePlayPause, st);
    ReplayControlCommand playCmd{};
    EXPECT_TRUE(st.ConsumeReplayCommand(playCmd) && playCmd.hasPlayToggle && playCmd.targetPlaying,
                "TogglePlayPause while paused must request Play");

    // Real team short name in verdict summary
    ctrl.SelectReplayMarker(0, st, false); // offender = 14 (PAL) inside box: fixed order, option 1 is Marcar Penalti for FLA
    ctrl.SelectMainOption(1, st);
    EXPECT_EQ(ctrl.DraftAction(), VarAction::RetroactivePenalty, "NoFoul option 1 is always Marcar Penalti (fixed order)");
    const std::string sum = ctrl.FormatVerdictSummary();
    EXPECT_TRUE(sum.find("FLA") != std::string::npos, "Verdict summary must display real team abbrev FLA");
    EXPECT_EQ(ctrl.FormatPlayerBroadcast(9), std::string("#10 GABIGOL"), "Broadcast player format mismatch");

    // 6-row OPCOES INI persistence including auto_zoom_box
    wchar_t tmpDir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmpDir);
    const std::wstring ini = std::wstring(tmpDir) + L"var_mod_v4_test.ini";
    DeleteFileW(ini.c_str());
    st.SetIniPath(ini);
    HudConfig cfg4{};
    cfg4.autoZoomBox = false;
    cfg4.autoOpenPanel = false;
    cfg4.dockLeft = true;
    st.SetHudConfig(cfg4, true);
    st.SetHudConfig(HudConfig{}, false);
    st.LoadHudConfig();
    const HudConfig loaded4 = st.GetHudConfig();
    DeleteFileW(ini.c_str());
    st.SetIniPath(L"");
    st.SetHudConfig(HudConfig{}, false);
    EXPECT_TRUE(!loaded4.autoZoomBox && !loaded4.autoOpenPanel && loaded4.dockLeft,
                "v4.0 INI round-trip for auto_zoom_box/auto_open_panel failed");

    MarkPass("TestV4BroadcastHudAndRadar");
}

// ---------------------------------------------------------------------------
// Test 15: Performance overhaul equivalence (early-exit sync, HUD snapshot cache, verdict flag)
// ---------------------------------------------------------------------------
void PatchReplayTelemetryForTest(bool active, int32_t cursorPhase, float cursorTime,
                                 float winStart, float winEnd) {
    VarState::Instance().PatchLiveReplayTelemetry(
        active, 0, 0, 0, cursorPhase, 0, -1, 0,
        active ? 1 : 0, active ? 1 : 0, 0, 0,
        cursorTime, cursorPhase, winStart, cursorPhase, winEnd, cursorPhase,
        0, 0.0f, 0, 0, 0, 0, 0, -1, -1);
}

// Protected score codec (0x1408C98F0 / 0x1408CA5F0) and the goal record layout
void TestGoalRecordCodec() {
    using namespace GoalRecord;
    // Known answers from the engine itself: AddTeamStat's reset (0x1408CAAA0) stores this hash for 0 and
    // encodes every byte as k*0x3E; the overflow branch stores this hash for 0xFFFFFFFF.
    {
        const uint8_t zero[4] = { 0, 0, 0, 0 };
        EXPECT_EQ(Fnv1a64(zero), 0x4D25767F9DCE13F5ULL, "FNV of 0 matches the engine's reset hash");
        const uint8_t ff[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
        EXPECT_EQ(Fnv1a64(ff), static_cast<uint64_t>(-0x66B0899AC1D5C6AFLL), "FNV of 0xFFFFFFFF matches the engine");
        uint8_t blk[0x20] = {};
        const uint32_t seed = 0x12345678u;
        std::memcpy(blk + 8, &seed, 4);
        EncodeProtectedU32(blk, 0, 2);
        uint8_t k[4]; ProtectedKeys(seed, k);
        uint32_t enc = 0; std::memcpy(&enc, blk + 0x10 + 2 * 4, 4);
        for (int i = 0; i < 4; ++i) {
            EXPECT_EQ(static_cast<uint8_t>(enc >> (8 * i)), static_cast<uint8_t>(k[i] * 0x3E), "reset encoding is k*0x3E");
        }
    }
    // Round trip over many seeds, keys and values; any tampering fails the hash instead of decoding garbage
    uint32_t rng = 0xC0FFEEu;
    auto next = [&]() { rng = rng * 1664525u + 1013904223u; return rng; };
    for (int it = 0; it < 2000; ++it) {
        uint8_t blk[0x20] = {};
        const uint32_t seed = next(), key = next(), value = (it % 3 == 0) ? (next() & 0xFF) : next();
        std::memcpy(blk + 8, &seed, 4);
        EncodeProtectedU32(blk, value, key);
        uint32_t out = 0;
        EXPECT_TRUE(DecodeProtectedU32(blk, out), "encoded value decodes");
        EXPECT_EQ(out, value, "round trip keeps the value");
        uint8_t bad[0x20]; std::memcpy(bad, blk, sizeof(bad));
        bad[0x10 + (key & 3u) * 4u] ^= 0x01;
        EXPECT_TRUE(!DecodeProtectedU32(bad, out), "a changed slot fails the hash");
    }
    // Team goals in a history image: periods 0..7 summed, period 8 (shootout) ignored, both teams separate
    std::vector<uint8_t> h(kHistoryBytes, 0);
    auto put = [&](int team, int period, uint32_t v) {
        uint8_t* b = h.data() + History_StatsRoot + static_cast<size_t>(team) * Stats_TeamStride + Team_GoalsProtected +
                     static_cast<size_t>(period) * Protected_Stride;
        const uint32_t seed = 0x9E3779B9u * static_cast<uint32_t>(team * 9 + period + 1);
        std::memcpy(b + 8, &seed, 4);
        EncodeProtectedU32(b, v, seed ^ 0x55u);
    };
    for (int t = 0; t < 2; ++t) for (int p = 0; p <= 8; ++p) put(t, p, 0);
    put(0, 0, 1); put(0, 3, 2); put(0, 8, 4); put(1, 5, 1);
    uint32_t g0 = 0, g1 = 0;
    EXPECT_TRUE(ReadTeamGoals(h.data(), 0, g0) && ReadTeamGoals(h.data(), 1, g1), "team goals decode");
    EXPECT_EQ(g0, 3u, "home goals = periods 0..7");
    EXPECT_EQ(g1, 1u, "away goals");
    EXPECT_TRUE(!ReadTeamGoals(h.data(), 2, g0), "team index checked");
    put(1, 2, 300);
    EXPECT_TRUE(!ReadTeamGoals(h.data(), 1, g1), "an impossible per-period count is rejected");
    // Layout constants stay inside the copied image
    EXPECT_TRUE(History_StatsRoot + Stats_TeamStride + Team_GoalsProtected + 9 * Protected_Stride <= kHistoryBytes,
                "both score tables are inside the copy");
    EXPECT_TRUE(History_PendingCount + 8 <= kHistoryBytes, "pending list inside the copy");
    MarkPass("TestGoalRecordCodec");
}

// Goal annulment in the HUD: options first, the draft never inherits the event under the cursor nor a player of
// the conceding team as offender, the offender stays in the scoring team, the kick goes to the defence
void TestGoalAnnulHud() {
    auto& st = VarState::Instance();
    st.ResetMatch();
    VarHudController ctrl;
    LiveTelemetry tel{};
    tel.enabled = true;
    tel.offlineAllowed = true;
    tel.matchActive = true;
    tel.matchStatus = static_cast<int32_t>(MatchStatus::OutOfPlay);
    tel.fsmState = 0x1D;
    tel.replayMode = 2;
    tel.replayManualFlag = 1;
    tel.replayCursorTime = 5000.0f;
    tel.goalAnnulAvailable = true;
    tel.goalCreditedTeam = 1; // away scored

    // Cursor on an earlier home foul with a yellow: nothing of it may reach the annulment
    VarEvent foul{};
    foul.id = 91;
    foul.type = VarEventType::Foul;
    foul.time60Hz = 5000.0f;
    foul.offenderSlot = 4;
    foul.victimSlot = 15;
    foul.benefitedTeam = 1;
    foul.cardKind = CardKind::Yellow;
    ReplaySyncResult sync{};
    sync.hasEvent = true;
    sync.event = foul;
    std::vector<VarEvent> hist = { foul };
    ctrl.UpdateContext(tel, sync, st.GetPlayers(), st.GetTeams(), hist, MatchVarStats{}, HudConfig{});

    const auto& opts = ctrl.MainOptions();
    EXPECT_TRUE(opts.size() >= 2, "options exist");
    EXPECT_EQ(static_cast<int>(opts[0].annulGoal), 0, "annul (foul) comes first during a goal stoppage");
    EXPECT_EQ(static_cast<int>(opts[1].annulGoal), 1, "annul (offside) second");
    for (size_t i = 2; i < opts.size(); ++i) EXPECT_EQ(static_cast<int>(opts[i].annulGoal), -1, "regular options unchanged");

    ctrl.SelectMainOption(0, st);
    EXPECT_EQ(ctrl.Page(), HudPage::Configure, "annulment opens the configure page");
    EXPECT_EQ(static_cast<int>(ctrl.DraftAnnulGoal()), 0, "draft is an annulment");
    EXPECT_EQ(ctrl.DraftSourceEventId(), 0u, "no source event inherited");
    EXPECT_EQ(ctrl.DraftOrigCard(), CardKind::None, "no original card inherited");
    EXPECT_EQ(ctrl.DraftBenefitedTeam(), 0, "kick goes to the defence");
    // Detected pair was a home offender on an away player: no reason to annul, the offender is left to the user
    EXPECT_TRUE(!IsValidSlot(ctrl.DraftOffenderSlot()), "no offender of the conceding team, none invented");
    for (ConfigField f : ctrl.ActiveConfigFields()) EXPECT_TRUE(f != ConfigField::Team, "no team field for an annulment");

    // Cycling the offender never leaves the scoring team
    int offRow = -1;
    for (size_t i = 0; i < ctrl.ActiveConfigFields().size(); ++i)
        if (ctrl.ActiveConfigFields()[i] == ConfigField::Offender) offRow = static_cast<int>(i);
    EXPECT_TRUE(offRow >= 0, "offender row present");
    while (ctrl.SelectedIndex() < offRow) ctrl.HandleAction(NavAction::Down, st);
    for (int k = 0; k < 25; ++k) {
        ctrl.HandleAction((k % 3 == 0) ? NavAction::Left : NavAction::Right, st);
        EXPECT_TRUE(SlotToTeam(ctrl.DraftOffenderSlot()) == 1, "offender cycling stays in the scoring team");
        EXPECT_EQ(ctrl.DraftBenefitedTeam(), 0, "kick stays with the defence");
    }

    int confirmRow = -1;
    for (size_t i = 0; i < ctrl.ActiveConfigFields().size(); ++i)
        if (ctrl.ActiveConfigFields()[i] == ConfigField::Confirm) confirmRow = static_cast<int>(i);
    while (ctrl.SelectedIndex() < confirmRow) ctrl.HandleAction(NavAction::Down, st);
    ctrl.HandleAction(NavAction::Confirm, st);
    const VerdictPool pool = st.PeekVerdictPool();
    EXPECT_TRUE(pool.hasPlayDecision, "annulment armed as the play decision");
    EXPECT_EQ(static_cast<int>(pool.playDecision.annulGoal), 0, "armed play carries annulGoal");
    EXPECT_EQ(pool.playDecision.action, VarAction::RetroactiveFreeKick, "free kick action");
    EXPECT_EQ(pool.playDecision.benefitedTeam, 0, "armed kick for the defence");
    EXPECT_EQ(pool.playDecision.sourceEventId, 0u, "armed without a source event");
    EXPECT_EQ(pool.playDecision.origCard, CardKind::None, "no card revocation");
    // Leave the replay and enter a new one: the old list must not steer the selection away from "Anular gol"
    tel.replayMode = 0;
    tel.replayManualFlag = 0;
    ctrl.UpdateContext(tel, sync, st.GetPlayers(), st.GetTeams(), hist, MatchVarStats{}, HudConfig{});
    tel.replayMode = 2;
    tel.replayManualFlag = 1;
    ctrl.UpdateContext(tel, sync, st.GetPlayers(), st.GetTeams(), hist, MatchVarStats{}, HudConfig{});
    EXPECT_EQ(ctrl.SelectedIndex(), 0, "a new replay of the goal stoppage opens on the first option");
    EXPECT_EQ(static_cast<int>(ctrl.MainOptions()[0].annulGoal), 0, "the first option is the annulment");
    // The offside variant has no card row and no fouled player
    ctrl.SelectMainOption(1, st);
    EXPECT_EQ(static_cast<int>(ctrl.DraftAnnulGoal()), 1, "offside annulment draft");
    for (ConfigField f : ctrl.ActiveConfigFields()) EXPECT_TRUE(f != ConfigField::Card, "no card for offside");
    EXPECT_TRUE(!IsValidSlot(ctrl.DraftVictimSlot()), "no fouled player for offside");
    st.ResetMatch();
    MarkPass("TestGoalAnnulHud");
}

void TestDecisionReplayWindow() {
    using namespace PitchGeometry::ReplayClipMath;
    // Ring of 660 frames, 600 recorded, match clock 2.3 ticks per frame, newest frame at writeIdx - 1 (wrapped)
    constexpr uint32_t kCap = 660;
    constexpr uint32_t kWrite = 250;
    std::vector<float> t(kCap, 0.0f);
    std::vector<uint8_t> ph(kCap, 2), ok(kCap, 0);
    auto at = [&](uint32_t back) { return (kWrite + kCap - 1 - back) % kCap; };
    for (uint32_t back = 0; back < 600; ++back) {
        ok[at(back)] = 1;
        t[at(back)] = 90000.0f - 2.3f * static_cast<float>(back);
    }
    uint16_t a = 0, b = 0;
    Result r = ComputeWindow(t.data(), ph.data(), ok.data(), kCap, kWrite, 2, 90000.0f - 2.3f * 200.0f, 150, 90, a, b);
    EXPECT_TRUE(r == Result::Ok && a == 351 && b == 111, "Window around a moment 200 frames back");
    r = ComputeWindow(t.data(), ph.data(), ok.data(), kCap, kWrite, 2, 90000.0f - 2.3f * 550.0f, 150, 90, a, b);
    EXPECT_TRUE(r == Result::Ok && a == 599 && b == 461, "Start clamped to the oldest recorded frame");
    r = ComputeWindow(t.data(), ph.data(), ok.data(), kCap, kWrite, 2, 90000.0f - 2.3f * 30.0f, 150, 90, a, b);
    EXPECT_TRUE(r == Result::Ok && a == 181 && b == 1, "End clamped to the newest frame");
    r = ComputeWindow(t.data(), ph.data(), ok.data(), kCap, kWrite, 2, 90000.0f - 2.3f * 900.0f, 150, 90, a, b);
    EXPECT_TRUE(r == Result::TooOld, "Moment older than the ring must be refused");
    // Stopped clock: the newest 60 frames (a cutscene) all carry the stoppage time. Counting by position keeps the
    // window right; counting by time would have treated those 60 frames as not newer than the moment.
    std::vector<float> t2 = t;
    for (uint32_t back = 0; back < 60; ++back) t2[at(back)] = 90000.0f - 2.3f * 60.0f;
    r = ComputeWindow(t2.data(), ph.data(), ok.data(), kCap, kWrite, 2, 90000.0f - 2.3f * 200.0f, 150, 90, a, b);
    EXPECT_TRUE(r == Result::Ok && a == 351 && b == 111, "Stopped clock frames must not skew the window");
    // A hole in the ring ends the frames the engine copies: the start is limited to the contiguous run
    std::vector<uint8_t> ok2 = ok;
    ok2[at(300)] = 0;
    r = ComputeWindow(t.data(), ph.data(), ok2.data(), kCap, kWrite, 2, 90000.0f - 2.3f * 200.0f, 150, 90, a, b);
    EXPECT_TRUE(r == Result::Ok && a == 299 && b == 111, "Start limited to the contiguous recorded frames");
    // Frames of an older phase at the far end are skipped, the moment in the current phase still found
    std::vector<uint8_t> ph2 = ph;
    for (uint32_t back = 500; back < 600; ++back) ph2[at(back)] = 0;
    r = ComputeWindow(t.data(), ph2.data(), ok.data(), kCap, kWrite, 2, 90000.0f - 2.3f * 200.0f, 150, 90, a, b);
    EXPECT_TRUE(r == Result::Ok && a == 351, "Older phase frames at the far end do not block the window");
    std::vector<uint8_t> none(kCap, 0);
    r = ComputeWindow(t.data(), ph.data(), none.data(), kCap, kWrite, 2, 90000.0f, 150, 90, a, b);
    EXPECT_TRUE(r == Result::NoFrames, "Empty ring must be refused");

    // Engine clock: rounded up minute, phase offsets (0x140A6C330)
    int mm = 0, ss = 0;
    PitchGeometry::EngineMatchClock(60.0f * (23 * 60 + 16), 1, mm, ss);
    EXPECT_TRUE(mm == 24 && ss == 16, "Minute rounds up when seconds are not zero");
    PitchGeometry::EngineMatchClock(60.0f * (10 * 60), 1, mm, ss);
    EXPECT_TRUE(mm == 10 && ss == 0, "Exact minute stays");
    PitchGeometry::EngineMatchClock(60.0f * (5 * 60 + 1), 3, mm, ss);
    EXPECT_TRUE(mm == 51 && ss == 1, "Phase 3 adds 45 minutes");
    MarkPass("TestDecisionReplayWindow");
}

// The cutscene map repair rebuilds exactly the links found zeroed in the crash dump and leaves a healthy tree alone
void TestMsvcTreeRepair() {
    struct Node { uint8_t* left; uint8_t* parent; uint8_t* right; uint8_t color; uint8_t isnil; uint8_t pad[6]; uint64_t key; };
    static Node head{}, n6e{}, n78{}, n82{}, nc8{};
    auto P = [](Node& n) { return reinterpret_cast<uint8_t*>(&n); };
    auto reset = [&]() {
        head = Node{}; n6e = Node{}; n78 = Node{}; n82 = Node{}; nc8 = Node{};
        head.isnil = 1; head.color = 1;
        // root 0x6e, right child 0xc8, whose left child is 0x82, whose left child is 0x78 (shape of the dump)
        n6e = Node{ P(head), P(head), P(nc8), 1, 0, {}, 0x6e };
        nc8 = Node{ P(n82), P(n6e), P(head), 1, 0, {}, 0xc8 };
        n82 = Node{ P(n78), P(nc8), P(head), 0, 0, {}, 0x82 };
        n78 = Node{ P(head), P(n82), P(head), 1, 0, {}, 0x78 };
        head.left = P(n6e); head.parent = P(n6e); head.right = P(nc8);
    };
    auto readable = [](const void* p, size_t) { return p != nullptr; };
    reset();
    EXPECT_EQ(MsvcTree::RepairMsvcTreeLinks(P(head), 4, readable), 0, "a healthy tree is left alone");
    // The corruption of the dump: parent and right child of 0x82 zeroed
    n82.parent = nullptr; n82.right = nullptr;
    const int fixes = MsvcTree::RepairMsvcTreeLinks(P(head), 4, readable);
    EXPECT_TRUE(fixes == 2, "parent and right child rebuilt");
    EXPECT_TRUE(n82.parent == P(nc8) && n82.right == P(head) && n82.left == P(n78), "links of 0x82 restored");
    // A wrong head->left (where the engine starts its walk) is rebuilt to the real smallest node
    head.left = P(n82);
    EXPECT_TRUE(MsvcTree::RepairMsvcTreeLinks(P(head), 4, readable) == 1 && head.left == P(n6e), "head->left rebuilt");
    // A cycle (child pointing back up) is refused instead of walked forever
    reset();
    n78.left = P(n6e);
    EXPECT_TRUE(MsvcTree::RepairMsvcTreeLinks(P(head), 4, readable) == -1, "a cycle is not walked");
    MarkPass("TestMsvcTreeRepair");
}

// A play the referee called is never offered again as a separate or "likely missed" foul: the contact seconds
// before the whistle belongs to the call, also for the foul the match is stopped for before it is recorded
// Automatic update: tag parsing, version order, throttle, signed manifest checks and the file swap on a loaded
// DLL (with rollback). Never touches the network here: the key pair is generated by the test.
namespace {
struct TestSigner {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    uint8_t pub[64] = {};
    bool ok = false;
    TestSigner() {
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) != 0) return;
        if (BCryptGenerateKeyPair(alg, &key, 256, 0) != 0 || BCryptFinalizeKeyPair(key, 0) != 0) return;
        uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64];
        ULONG cb = 0;
        if (BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob, sizeof(blob), &cb, 0) != 0 || cb != sizeof(blob)) return;
        std::memcpy(pub, blob + sizeof(BCRYPT_ECCKEY_BLOB), 64);
        ok = true;
    }
    ~TestSigner() {
        if (key) BCryptDestroyKey(key);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    }
    std::string SignHex(const std::string& msg) const {
        uint8_t digest[32], sig[64];
        ULONG cb = 0;
        if (!UpdateChecker::Sha256(msg.data(), msg.size(), digest) ||
            BCryptSignHash(key, nullptr, digest, 32, sig, 64, &cb, 0) != 0 || cb != 64) return "";
        static const char* hx = "0123456789abcdef";
        std::string out;
        for (uint8_t b : sig) { out.push_back(hx[b >> 4]); out.push_back(hx[b & 15]); }
        return out + "\n";
    }
};

std::string HexOf(const uint8_t* p, size_t n) {
    static const char* hx = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < n; ++i) { out.push_back(hx[p[i] >> 4]); out.push_back(hx[p[i] & 15]); }
    return out;
}

std::string MakeManifest(const char* version, const std::string& dll, const std::string& lua) {
    uint8_t h1[32], h2[32];
    UpdateChecker::Sha256(dll.data(), dll.size(), h1);
    UpdateChecker::Sha256(lua.data(), lua.size(), h2);
    return std::string("pes21-var-mod update 1\nversion = ") + version + "\nvar_core.dll = " + HexOf(h1, 32) + " " +
           std::to_string(dll.size()) + "\nvar_mod.lua = " + HexOf(h2, 32) + " " + std::to_string(lua.size()) + "\n";
}

std::string ReadAll(const std::wstring& p) {
    std::string out;
    HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return "<missing>";
    char buf[65536];
    DWORD n = 0;
    while (ReadFile(f, buf, sizeof(buf), &n, nullptr) && n > 0) out.append(buf, n);
    CloseHandle(f);
    return out;
}

void WriteAll(const std::wstring& p, const std::string& d) {
    HANDLE f = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD n = 0;
    WriteFile(f, d.data(), static_cast<DWORD>(d.size()), &n, nullptr);
    CloseHandle(f);
}
} // namespace

void TestUpdateChecker() {
    using namespace UpdateChecker;
    char tag[32];
    const char* js = "{\"url\":\"x\",\"tag_name\" : \"v4.6.0\",\"name\":\"v4.6.0\"}";
    EXPECT_TRUE(ParseTagName(js, std::strlen(js), tag, sizeof(tag)) && std::strcmp(tag, "4.6.0") == 0, "tag_name parsed, v removed");
    const char* bad = "{\"tag_name\":\"v4.6.0<script>\"}";
    EXPECT_TRUE(!ParseTagName(bad, std::strlen(bad), tag, sizeof(tag)) && tag[0] == '\0', "only a dotted version is accepted");
    const char* cut = "{\"tag_name\":\"v4.6";
    EXPECT_TRUE(!ParseTagName(cut, std::strlen(cut), tag, sizeof(tag)), "truncated body refused");
    const char* none = "{\"message\":\"Not Found\"}";
    EXPECT_TRUE(!ParseTagName(none, std::strlen(none), tag, sizeof(tag)), "no tag_name");
    char tiny[4];
    EXPECT_TRUE(!ParseTagName(js, std::strlen(js), tiny, sizeof(tiny)), "a tag longer than the buffer is refused");
    EXPECT_TRUE(CompareVersions("4.5.0", "4.5.0") == 0, "equal");
    EXPECT_TRUE(CompareVersions("v4.10.0", "4.9.9") > 0, "numeric, not text order");
    EXPECT_TRUE(CompareVersions("4.5", "4.5.0") == 0, "missing part is 0");
    EXPECT_TRUE(CompareVersions("4.5.0", "4.5.1") < 0, "patch");
    EXPECT_TRUE(CompareVersions("5.0.0", VAR_MOD_VERSION) > 0, "major above the installed one");
    EXPECT_TRUE(CheckDue(0, 1000000), "never checked");
    EXPECT_TRUE(!CheckDue(1000000, 1000000 + 120), "2 min later (quick restart): not due");
    EXPECT_TRUE(CheckDue(1000000, 1000000 + kCheckIntervalSec), "interval elapsed: due");
    EXPECT_TRUE(CheckDue(1000000 + 86400 * 5, 1000000), "clock moved back: due");
    // No base dir in the tests: Start never starts a request, and nothing newer is known
    Start(true);
    EXPECT_TRUE(NewerVersion() == nullptr && CurrentPhase() == Phase::Idle, "nothing published without a check");

    // ---- Signed manifest
    uint8_t h[32];
    EXPECT_TRUE(Sha256("abc", 3, h) && HexOf(h, 32) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 known answer");
    TestSigner signer;
    EXPECT_TRUE(signer.ok, "test key pair");
    const std::string dll = std::string("MZ") + std::string(5000, '\x90');
    const std::string lua = "local m = { version = \"99.0.0\" }\n";
    const std::string man = MakeManifest("99.0.0", dll, lua);
    const std::string sig = signer.SignHex(man);
    Manifest m;
    const char* why = nullptr;
    EXPECT_TRUE(ValidateUpdate(man, sig, "99.0.0", dll, lua, signer.pub, m, &why) && m.dllSize == dll.size(), "valid signed update accepted");
    std::string tampered = man;
    tampered[tampered.find("99.0.0")] = '8';
    EXPECT_TRUE(!ValidateUpdate(tampered, sig, "89.0.0", dll, lua, signer.pub, m, &why), "edited manifest refused");
    TestSigner other;
    EXPECT_TRUE(!ValidateUpdate(man, other.SignHex(man), "99.0.0", dll, lua, signer.pub, m, &why), "signature of another key refused");
    EXPECT_TRUE(!ValidateUpdate(man, sig, "99.0.1", dll, lua, signer.pub, m, &why), "tag different from the manifest refused");
    EXPECT_TRUE(!ValidateUpdate(man, sig, "99.0.0", dll + "x", lua, signer.pub, m, &why), "DLL not matching the manifest refused");
    EXPECT_TRUE(!ValidateUpdate(man, sig, "99.0.0", dll, lua + " ", signer.pub, m, &why), "Lua not matching the manifest refused");
    EXPECT_TRUE(!ValidateUpdate(man, "zz", "99.0.0", dll, lua, signer.pub, m, &why), "malformed signature refused");
    const std::string oldMan = MakeManifest("1.0.0", dll, lua);
    EXPECT_TRUE(!ValidateUpdate(oldMan, signer.SignHex(oldMan), "1.0.0", dll, lua, signer.pub, m, &why), "signed downgrade refused");
    const std::string extra = man + "run = calc.exe\n";
    EXPECT_TRUE(!ValidateUpdate(extra, signer.SignHex(extra), "99.0.0", dll, lua, signer.pub, m, &why), "unknown manifest key refused");
    const std::string notDll = "XX" + dll.substr(2);
    const std::string notDllMan = MakeManifest("99.0.0", notDll, lua);
    EXPECT_TRUE(!ValidateUpdate(notDllMan, signer.SignHex(notDllMan), "99.0.0", notDll, lua, signer.pub, m, &why), "non-PE file refused");

    // ---- File swap on a loaded DLL, then rollback when the Lua cannot be replaced
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring root = std::wstring(tmp) + L"var_update_test_" + std::to_wstring(GetCurrentProcessId()) + L"\\";
    const std::wstring base = root + L"var_mod\\";
    CreateDirectoryW(root.c_str(), nullptr);
    CreateDirectoryW(base.c_str(), nullptr);
    const std::wstring dllPath = base + L"var_core.dll", luaPath = root + L"var_mod.lua";
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    EXPECT_TRUE(CopyFileW((std::wstring(sys) + L"\\version.dll").c_str(), dllPath.c_str(), FALSE) != 0, "test DLL copied");
    const std::string origDll = ReadAll(dllPath);
    WriteAll(luaPath, "old lua");
    HMODULE loaded = LoadLibraryW(dllPath.c_str());
    EXPECT_TRUE(loaded != nullptr, "test DLL loaded");
    // Rollback: the Lua is held open without sharing, the swap must leave both files as they were
    HANDLE lock = CreateFileW(luaPath.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    EXPECT_TRUE(!ApplyUpdate(dllPath, luaPath, dll, lua), "swap fails while the Lua is locked");
    CloseHandle(lock);
    EXPECT_TRUE(ReadAll(dllPath) == origDll && ReadAll(luaPath) == "old lua", "rollback restored the running files");
    EXPECT_TRUE(GetFileAttributesW((dllPath + L".old").c_str()) == INVALID_FILE_ATTRIBUTES &&
                GetFileAttributesW((dllPath + L".new").c_str()) == INVALID_FILE_ATTRIBUTES, "no leftovers after the rollback");
    // Real swap with the DLL still loaded
    EXPECT_TRUE(ApplyUpdate(dllPath, luaPath, dll, lua), "swap succeeds with the DLL loaded");
    EXPECT_TRUE(ReadAll(dllPath) == dll && ReadAll(luaPath) == lua, "new files in place");
    EXPECT_TRUE(ReadAll(dllPath + L".old") == origDll && ReadAll(luaPath + L".old") == "old lua", "old files kept");
    if (loaded) FreeLibrary(loaded);
    // Next start: the replaced files move to the previous folder
    WriteAll(base + L"var_core.next.dll", dll); // copy left for the session, not loaded at the next start
    EXPECT_TRUE(FinishPreviousUpdate(base, luaPath), "an update had just been installed");
    EXPECT_TRUE(GetFileAttributesW((base + L"var_core.next.dll").c_str()) == INVALID_FILE_ATTRIBUTES, "session copy removed");
    EXPECT_TRUE(!FinishPreviousUpdate(base, luaPath), "nothing to finish the second time");
    EXPECT_TRUE(ReadAll(base + L"previous\\var_core.dll") == origDll && ReadAll(base + L"previous\\var_mod.lua") == "old lua",
                "previous version kept for a manual rollback");
    EXPECT_TRUE(GetFileAttributesW((dllPath + L".old").c_str()) == INVALID_FILE_ATTRIBUTES, "no .old left");
    DeleteFileW((base + L"previous\\var_core.dll").c_str());
    DeleteFileW((base + L"previous\\var_mod.lua").c_str());
    RemoveDirectoryW((base + L"previous").c_str());
    DeleteFileW(dllPath.c_str());
    DeleteFileW(luaPath.c_str());
    RemoveDirectoryW(base.c_str());
    RemoveDirectoryW(root.c_str());
    MarkPass("TestUpdateChecker");
}

void TestCalledPlayNotSuspect() {
    auto& st = VarState::Instance();
    st.ResetMatch();
    LiveTelemetry tel{};
    tel.matchActive = true;
    tel.matchPhase = 1;
    tel.matchStatus = static_cast<int32_t>(MatchStatus::InPlay);
    st.UpdateTelemetry(tel);
    auto fall = [&](float t, int32_t off, int32_t vic) {
        DetectedIncident inc{};
        inc.matchPhase = 1;
        inc.time60Hz = t;
        inc.kind = IncidentKind::SlideTackle;
        inc.priority = 88;
        inc.offenderSlot = off;
        inc.victimSlot = vic;
        inc.nearFoul = true;
        inc.rolesConfirmed = true;
        inc.contactDistM = 0.7f;
        inc.boxDistM = -3.0f; // in the box: the strongest suspicion when nobody called it
        inc.boxDistValid = true;
        st.PushDetectedIncident(inc);
    };
    // Contact 3 s before the whistle (frames of the same play), foul called on the same players
    fall(9820.0f, 3, 15);
    fall(9840.0f, 3, 15);
    VarEvent ev{};
    ev.type = VarEventType::Foul;
    ev.matchPhase = 1;
    ev.time60Hz = 10000.0f;
    ev.offenderSlot = 3;
    ev.victimSlot = 15;
    st.PushEvent(ev);
    auto markers = st.GetReplayWindowMarkers(1, 9000.0f, 10600.0f);
    int official = 0, loose = 0, suspects = 0;
    for (const auto& m : markers) {
        if (m.isOfficialEvent) ++official; else ++loose;
        if (m.topSuspect) ++suspects;
    }
    EXPECT_EQ(official, 1, "the called foul is on the timeline");
    EXPECT_EQ(loose, 0, "the contact of the called play is not a separate incident");
    EXPECT_EQ(suspects, 0, "a called play is never a likely missed foul");

    // An unrelated contact long before (other players, 12 s earlier) is still offered
    fall(9280.0f, 6, 18);
    markers = st.GetReplayWindowMarkers(1, 9000.0f, 10600.0f);
    loose = 0;
    for (const auto& m : markers) if (!m.isOfficialEvent) ++loose;
    EXPECT_EQ(loose, 1, "an earlier unrelated contact stays on the timeline");

    // The foul the match is stopped for, before it reaches the event history
    st.ResetMatch();
    tel.matchStatus = static_cast<int32_t>(MatchStatus::OutOfPlay);
    tel.time60Hz = 20000.0f;
    tel.activeFoulValid = 1;
    tel.activeOffenderSlot = 14;
    tel.activeVictimSlot = 2;
    st.UpdateTelemetry(tel);
    fall(19850.0f, 14, 2);
    markers = st.GetReplayWindowMarkers(1, 19000.0f, 20100.0f);
    suspects = 0;
    loose = 0;
    for (const auto& m : markers) { if (!m.isOfficialEvent) ++loose; if (m.topSuspect) ++suspects; }
    EXPECT_EQ(loose, 0, "the play of the live stoppage foul is not a separate incident");
    EXPECT_EQ(suspects, 0, "the live stoppage foul is never a likely missed foul");
    st.UpdateTelemetry(LiveTelemetry{}); // no live call leaks into later tests
    st.ResetMatch();
    MarkPass("TestCalledPlayNotSuspect");
}

// Incident detection needs a proven contact for roles: a reaction caused by a nearby opponent. A challenge
// nobody reacted to is a duel within reach, and two players merely close are nothing.
void TestContactDetectionRules() {
    PitchDims dims{};
    const float dirs[2] = { 1.0f, -1.0f };
    auto base = []() {
        PlaySnapshot sn{};
        for (int i = 0; i < 22; ++i) {
            sn.playerOnPitch[i] = 1;
            sn.playerCauser[i] = -1;
            sn.playerPos[i] = Vec3{ -40.0f + 3.5f * static_cast<float>(i), 0.0f, (i < 11) ? -25.0f : 25.0f };
        }
        sn.ballOwnerSlot = 5;
        return sn;
    };
    DetectedIncident inc{};
    // Standing tackle 1.3 m from the carrier, nobody reacts: out of reach, nothing
    PlaySnapshot sn = base();
    sn.playerPos[5] = Vec3{ 0.0f, 0.0f, 0.0f };
    sn.playerPos[16] = Vec3{ 1.3f, 0.0f, 0.0f };
    sn.playerActionId[16] = PlayerActionId::Tackle;
    EXPECT_TRUE(!VarState::DetectPhysicalIncident(sn, dims, dirs, inc), "a tackle out of reach is not an incident");
    // Same tackle at 1.0 m: a duel, with no roles
    sn.playerPos[16] = Vec3{ 1.0f, 0.0f, 0.0f };
    EXPECT_TRUE(VarState::DetectPhysicalIncident(sn, dims, dirs, inc), "a tackle within reach is a duel");
    EXPECT_TRUE(!inc.rolesConfirmed && !inc.nearFoul, "a duel nobody reacted to has no offender / victim");
    // The carrier goes down and the engine names a causer 0.8 m away: proven contact, roles confirmed
    sn.playerActionId[5] = PlayerActionId::FallDown;
    sn.playerCauser[5] = 16;
    sn.playerPos[16] = Vec3{ 0.8f, 0.0f, 0.0f };
    EXPECT_TRUE(VarState::DetectPhysicalIncident(sn, dims, dirs, inc), "a fall caused by an opponent is an incident");
    EXPECT_TRUE(inc.rolesConfirmed && inc.nearFoul && inc.offenderSlot == 16 && inc.victimSlot == 5, "roles of a proven contact");
    // The recorded causer is 5 m away (stale) and nobody else challenges within reach: no incident
    sn.playerPos[16] = Vec3{ 5.0f, 0.0f, 0.0f };
    sn.playerActionId[16] = 0;
    EXPECT_TRUE(!VarState::DetectPhysicalIncident(sn, dims, dirs, inc), "a stale causer far away is ignored");
    // ...but an opponent tackling right next to him (1.2 m) is taken as the causer
    sn.playerPos[17] = Vec3{ -1.2f, 0.0f, 0.0f };
    sn.playerActionId[17] = PlayerActionId::Tackle;
    EXPECT_TRUE(VarState::DetectPhysicalIncident(sn, dims, dirs, inc) && inc.offenderSlot == 17 && inc.rolesConfirmed,
                "an adjacent challenger causes the fall");
    MarkPass("TestContactDetectionRules");
}

void TestSuspectAndCameraFollow() {
    auto mk = [](IncidentKind kind, bool nearFoul, float boxDist, float contact, float t, bool official) {
        ReplayMarker m{};
        m.kind = kind;
        m.nearFoul = nearFoul;
        m.signedBoxDistM = boxDist;
        m.contactDistM = contact;
        m.time60Hz = t;
        m.isOfficialEvent = official;
        return m;
    };
    // Scores
    EXPECT_EQ(VarState::SuspicionScore(mk(IncidentKind::SlideTackle, true, -3.0f, 0.8f, 0, false)), 100,
              "Slide that floored the opponent in the box must be the maximum");
    EXPECT_EQ(VarState::SuspicionScore(mk(IncidentKind::SlideTackle, true, 20.0f, 1.2f, 0, false)), 73,
              "Slide that floored the opponent in midfield");
    EXPECT_EQ(VarState::SuspicionScore(mk(IncidentKind::CloseContact, false, 20.0f, 0.8f, 0, false)), 0,
              "A plain close contest is never a suspect");
    EXPECT_TRUE(VarState::SuspicionScore(mk(IncidentKind::SlideTackle, false, -3.0f, 0.8f, 0, false)) < VarState::kSuspectThreshold,
                "A clean slide in the box (nobody reacted to a contact) is not a likely missed foul");
    EXPECT_EQ(VarState::SuspicionScore(mk(IncidentKind::SlideTackle, true, -3.0f, 0.5f, 0, true)), 0,
              "An official decision is never a missed foul");
    EXPECT_TRUE(VarState::SuspicionScore(mk(IncidentKind::Dive, true, -2.0f, 0.5f, 0, false)) < VarState::kSuspectThreshold,
                "A dive is not a missed foul even in the box");
    EXPECT_TRUE(VarState::SuspicionScore(mk(IncidentKind::BodyCharge, true, 20.0f, 1.2f, 0, false)) < VarState::kSuspectThreshold,
                "A shoulder charge that floors the opponent in midfield stays below the threshold");
    EXPECT_TRUE(VarState::SuspicionScore(mk(IncidentKind::StandTackle, true, 20.0f, 1.2f, 0, false)) >= VarState::kSuspectThreshold,
                "A standing tackle that floors the opponent is a suspect");

    // Exactly one top suspect: strongest, then tighter contact, then latest
    std::vector<ReplayMarker> ms;
    ms.push_back(mk(IncidentKind::StandTackle, true, 20.0f, 1.2f, 100.0f, false));   // 65
    ms.push_back(mk(IncidentKind::SlideTackle, true, 20.0f, 1.2f, 200.0f, false));   // 73
    ms.push_back(mk(IncidentKind::SlideTackle, true, 20.0f, 1.2f, 300.0f, false));   // 73, later
    ms.push_back(mk(IncidentKind::SlideTackle, true, -2.0f, 0.5f, 400.0f, true));    // official: 0
    VarState::FlagTopSuspect(ms);
    int flagged = 0;
    for (const auto& m : ms) flagged += m.topSuspect ? 1 : 0;
    EXPECT_EQ(flagged, 1, "Exactly one marker must be the top suspect");
    EXPECT_TRUE(ms[2].topSuspect, "Tie on score and contact goes to the latest play");
    ms[1].contactDistM = 0.9f;  // tighter contact now wins (score 78)
    VarState::FlagTopSuspect(ms);
    EXPECT_TRUE(ms[1].topSuspect && !ms[2].topSuspect, "Higher score must win");
    std::vector<ReplayMarker> weak{ mk(IncidentKind::CloseContact, false, 20.0f, 1.4f, 10.0f, false) };
    VarState::FlagTopSuspect(weak);
    EXPECT_TRUE(!weak[0].topSuspect, "No suspect below the threshold");

    // Replay camera follow: focus accepted with the game's own bounds, never NaN or the all-zero vector
    using PitchGeometry::ReplayCameraMath::IsUsableFocus;
    const float okFocus[3] = { -30.0f, 0.5f, 8.0f };
    const float centreBall[3] = { 0.0f, 0.11f, 0.0f };
    const float nanFocus[3] = { std::nanf(""), 0.0f, 0.0f };
    const float offX[3] = { 200.0f, 0.0f, 0.0f };
    const float offZ[3] = { 10.0f, 0.0f, -100.0f };
    const float zero[3] = { 0.0f, 0.0f, 0.0f };
    EXPECT_TRUE(IsUsableFocus(okFocus) && IsUsableFocus(centreBall), "Pitch positions (ball on the centre spot too) are usable");
    EXPECT_TRUE(!IsUsableFocus(nanFocus) && !IsUsableFocus(offX) && !IsUsableFocus(offZ) && !IsUsableFocus(zero),
                "NaN, out-of-bounds and all-zero focus must be rejected");
    MarkPass("TestSuspectAndCameraFollow");
}

void TestSnapshotInterpolation() {
    auto& st = VarState::Instance();
    st.ResetMatch();
    auto frame = [&st](float time, uint32_t tick, float px, float bx, uint8_t onPitch, int8_t action) {
        PlaySnapshot sn{};
        sn.matchPhase = 0;
        sn.time60Hz = time;
        sn.matchTick = tick;
        sn.simFrame = tick;
        sn.ballPos = Vec3{ bx, 0.2f, 1.0f };
        sn.playerPos[0] = Vec3{ px, 0.0f, 2.0f };
        sn.playerOnPitch[0] = onPitch;
        sn.playerActionId[0] = action;
        st.PushSnapshot(sn);
    };
    frame(100.0f, 1, 0.0f, 10.0f, 1, 5);
    frame(101.0f, 2, 1.0f, 12.0f, 1, 13);
    frame(102.0f, 3, 9.0f, 12.5f, 1, 13);   // 8 m in one frame: teleport, never blended
    frame(110.0f, 12, 9.5f, 13.0f, 1, 13);  // 9 simulation frames later: not neighbours
    frame(111.0f, 13, 9.7f, 13.2f, 0, 13);  // player off the pitch in the newer frame

    ReplaySyncResult r = st.SyncToReplayCursor(0, 100.25f);
    EXPECT_TRUE(r.hasSnapshot && r.snapshot.matchTick == 1, "Interpolated sync must keep the nearest frame identity");
    EXPECT_TRUE(std::fabs(r.snapshot.playerPos[0].x - 0.25f) < 1e-4f, "Player position not interpolated at 1/4 frame");
    EXPECT_TRUE(std::fabs(r.snapshot.ballPos.x - 10.5f) < 1e-4f, "Ball position not interpolated at 1/4 frame");
    EXPECT_TRUE(r.snapshot.playerActionId[0] == 5, "Discrete fields must come from the nearest frame");

    r = st.SyncToReplayCursor(0, 100.75f);
    EXPECT_TRUE(r.snapshot.matchTick == 2 && std::fabs(r.snapshot.playerPos[0].x - 0.75f) < 1e-4f,
                "Interpolation from the newer side of the pair");
    EXPECT_TRUE(r.snapshot.playerActionId[0] == 13, "Nearest frame on the newer side keeps its action");

    r = st.SyncToReplayCursor(0, 101.0f);
    EXPECT_TRUE(r.snapshot.playerPos[0].x == 1.0f && r.snapshot.ballPos.x == 12.0f, "Exact frame must be returned as is");

    r = st.SyncToReplayCursor(0, 101.4f);
    EXPECT_TRUE(r.snapshot.playerPos[0].x == 1.0f, "Teleport between frames must not be blended");
    EXPECT_TRUE(std::fabs(r.snapshot.ballPos.x - 12.2f) < 1e-4f, "Ball still blends when only a player teleports");

    r = st.SyncToReplayCursor(0, 104.0f);
    EXPECT_TRUE(r.snapshot.matchTick == 3 && r.snapshot.playerPos[0].x == 9.0f, "Gap between frames must not be blended");

    r = st.SyncToReplayCursor(0, 110.4f);
    EXPECT_TRUE(r.snapshot.playerPos[0].x == 9.5f, "Player leaving the pitch must not be blended");

    r = st.SyncToReplayCursor(0, 115.0f);
    EXPECT_TRUE(r.snapshot.matchTick == 13 && r.snapshot.ballPos.x == 13.2f, "Cursor past the newest frame keeps it");
    r = st.SyncToReplayCursor(0, 90.0f);
    EXPECT_TRUE(r.snapshot.matchTick == 1 && r.snapshot.ballPos.x == 10.0f, "Cursor before the oldest frame keeps it");

    // Snapshot pushes must not invalidate the event history cache, but must refresh sync
    PatchReplayTelemetryForTest(true, 0, 100.5f, 90.0f, 120.0f);
    HudRenderSnapshot snap;
    st.CaptureHudRenderSnapshot(snap);
    const uint32_t histRev = snap.historyRev;
    const uint32_t snapRev = snap.snapRev;
    frame(112.0f, 14, 9.8f, 13.4f, 1, 13);
    PatchReplayTelemetryForTest(true, 0, 111.5f, 90.0f, 120.0f);
    st.CaptureHudRenderSnapshot(snap);
    EXPECT_TRUE(snap.historyRev == histRev, "Snapshot push must not invalidate the event history");
    EXPECT_TRUE(snap.snapRev != snapRev, "Snapshot push must bump the snapshot revision");
    EXPECT_TRUE(snap.sync.hasSnapshot && std::fabs(snap.sync.snapshot.ballPos.x - 13.3f) < 1e-4f,
                "Cached sync must see the new frame");
    PatchReplayTelemetryForTest(false, 0, 0.0f, 0.0f, 0.0f);

    // Short match lengths: the match clock runs faster than real time (2.3 ticks per simulation frame in a user
    // log). Consecutive simulation frames must still blend, and the motion history must still be 0.15 s apart.
    st.ResetMatch();
    for (int i = 0; i < 200; ++i) {
        PlaySnapshot sn{};
        sn.matchPhase = 2;
        sn.time60Hz = 5000.0f + 2.3f * static_cast<float>(i);
        sn.matchTick = 9000u + static_cast<uint32_t>(i);
        sn.simFrame = sn.matchTick;
        sn.playerOnPitch[0] = 1;
        sn.playerPos[0] = Vec3{ 0.1f * static_cast<float>(i), 0.0f, 0.0f }; // 6 m/s
        st.PushSnapshot(sn);
    }
    r = st.SyncToReplayCursor(2, 5000.0f + 2.3f * 100.5f);
    EXPECT_TRUE(std::fabs(r.snapshot.playerPos[0].x - 10.05f) < 1e-3f, "Fast match clock must still interpolate consecutive frames");
    MotionSample hist[VarState::kMotionSamples];
    st.SampleMotionHistory(2, 5000.0f + 2.3f * 150.0f, hist);
    bool histOk = true;
    for (int k = 0; k < VarState::kMotionSamples; ++k) {
        const uint32_t wantTick = 9150u - static_cast<uint32_t>(k + 1) * VarState::kMotionStepFrames;
        histOk = histOk && hist[k].valid && hist[k].simFrame == wantTick &&
                 std::fabs(hist[k].pos[0].x - 0.1f * static_cast<float>(wantTick - 9000u)) < 1e-3f;
    }
    EXPECT_TRUE(histOk, "Motion history must step back 9 simulation frames per sample");
    st.SampleMotionHistory(2, 5000.0f + 2.3f * 5.0f, hist);
    EXPECT_TRUE(!hist[0].valid, "No history before the first recorded frame");
    st.SampleMotionHistory(0, 5000.0f, hist);
    EXPECT_TRUE(!hist[0].valid, "No history from another match phase");
    st.ResetMatch();
    MarkPass("TestSnapshotInterpolation");
}

void TestPerfOverhaulEquivalence() {
    auto& st = VarState::Instance();
    st.ResetMatch();

    // 1. Early-exit SyncToReplayCursor must equal a brute-force full scan (ring wrap, time plateaus,
    //    sub-6-tick spacing, both halves).
    struct RefSnap { int32_t phase; float t; uint32_t tick; };
    std::vector<RefSnap> pushed;
    uint32_t lcg = 12345u;
    auto rnd = [&lcg](uint32_t mod) { lcg = lcg * 1664525u + 1013904223u; return (lcg >> 8) % mod; };
    float t = 0.0f;
    float maxT = 0.0f;
    uint32_t tick = 1;
    // More pushes than the ring holds, so the search also runs across the wrap
    const int total = static_cast<int>(VarState::kMaxSnapshots) + 900;
    const int halfSplit = total * 11 / 20;
    for (int i = 0; i < total; ++i) {
        const int32_t phase = (i < halfSplit) ? 0 : 2;
        if (i == halfSplit) t = 0.0f;                 // 2nd half clock restarts
        const uint32_t r = rnd(10);
        t += (r < 2) ? 0.0f : static_cast<float>(r);  // plateaus (clock stopped) and 2..9 tick steps
        PlaySnapshot sn{};
        sn.matchPhase = phase;
        sn.time60Hz = t;
        sn.matchTick = tick++;
        st.PushSnapshot(sn);
        pushed.push_back({ phase, t, sn.matchTick });
        maxT = std::max(maxT, t);
    }
    const size_t keep = std::min<size_t>(pushed.size(), VarState::kMaxSnapshots);
    const std::vector<RefSnap> live(pushed.end() - static_cast<std::ptrdiff_t>(keep), pushed.end());

    for (int q = 0; q < 3000; ++q) {
        const int32_t cph = (q % 3 == 0) ? 0 : 2;
        const float ct = static_cast<float>(rnd(static_cast<uint32_t>(maxT * 4.0f / 3.0f) + 200)) * 0.75f - 50.0f;
        // Brute force: newest-first, strict < (newest wins ties), identical to the v4.0 full scan
        bool refHas = false;
        float refBest = 999999.0f;
        uint32_t refTick = 0;
        for (size_t k = live.size(); k-- > 0;) {
            if ((live[k].phase / 2) != (cph / 2)) continue;
            const float dt = std::fabs(live[k].t - ct);
            if (dt < refBest) { refBest = dt; refHas = true; refTick = live[k].tick; }
        }
        const ReplaySyncResult r = st.SyncToReplayCursor(cph, ct);
        EXPECT_EQ(r.hasSnapshot, refHas, "Early-exit sync hasSnapshot differs from brute force");
        if (refHas) {
            EXPECT_EQ(r.snapshot.matchTick, refTick, "Early-exit sync picked a different snapshot than brute force");
        }
    }

    // 2. CaptureHudRenderSnapshot: identical data to the individual getters + revision caching
    PatchReplayTelemetryForTest(true, 2, 1500.0f, 900.0f, 2100.0f);
    VarEvent ev{};
    ev.type = VarEventType::Foul;
    ev.matchPhase = 2;
    ev.time60Hz = 1490.0f;
    ev.matchTick = 999999;
    ev.offenderSlot = 3;
    ev.victimSlot = 15;
    st.PushEvent(ev);

    HudRenderSnapshot snap;
    st.CaptureHudRenderSnapshot(snap);
    const ReplaySyncResult direct = st.SyncToReplayCursor(2, 1500.0f);
    const auto directMarkers = st.GetReplayWindowMarkers(2, 900.0f, 2100.0f);
    EXPECT_TRUE(snap.sync.hasSnapshot && direct.hasSnapshot, "Snapshot sync missing");
    EXPECT_EQ(snap.sync.snapshot.matchTick, direct.snapshot.matchTick, "Cached sync differs from direct sync");
    EXPECT_EQ(snap.sync.hasEvent, direct.hasEvent, "Cached sync event flag differs");
    EXPECT_EQ(snap.markers.size(), directMarkers.size(), "Cached markers differ from direct markers");
    EXPECT_EQ(snap.history.size(), st.GetRecentEvents(16).size(), "Cached history size differs");

    const uint32_t histRev = snap.historyRev;
    const uint32_t rosterRev = snap.rosterRev;
    const ReplayMarker* markersData = snap.markers.data();
    st.CaptureHudRenderSnapshot(snap);
    EXPECT_EQ(snap.historyRev, histRev, "Steady frame must not bump history revision");
    EXPECT_EQ(snap.markers.data(), markersData, "Steady frame must not reallocate markers");

    ev.time60Hz = 1520.0f;
    ev.matchTick = 1000600; // outside the 30-tick dedup window so it is a genuinely new event
    st.PushEvent(ev);
    st.CaptureHudRenderSnapshot(snap);
    EXPECT_TRUE(snap.historyRev != histRev, "New event must invalidate history cache");
    EXPECT_EQ(snap.history.size(), st.GetRecentEvents(16).size(), "History not refreshed after new event");

    std::array<PlayerSlotInfo, 22> players{};
    std::snprintf(players[3].name, sizeof(players[3].name), "%s", "TESTE");
    st.UpdatePlayers(players);
    st.CaptureHudRenderSnapshot(snap);
    EXPECT_TRUE(snap.rosterRev != rosterRev, "UpdatePlayers must invalidate roster cache");
    EXPECT_TRUE(std::strcmp(snap.players[3].name, "TESTE") == 0, "Roster not refreshed in HUD snapshot");

    // Incident merge that only flips insideBox must still invalidate the marker cache
    DetectedIncident inc{};
    inc.kind = IncidentKind::StandTackle;
    inc.priority = 60;
    inc.offenderSlot = 4;
    inc.victimSlot = 16;
    inc.matchPhase = 2;
    inc.time60Hz = 1600.0f;
    inc.contactDistM = 0.5f;
    inc.insideBox = false;
    st.PushDetectedIncident(inc);
    st.CaptureHudRenderSnapshot(snap);
    const uint32_t revBeforeBox = snap.historyRev;
    inc.priority = 40;           // weaker contact: no upgrade, only the box flag merges
    inc.time60Hz = 1610.0f;
    inc.insideBox = true;
    st.PushDetectedIncident(inc);
    st.CaptureHudRenderSnapshot(snap);
    EXPECT_TRUE(snap.historyRev != revBeforeBox, "insideBox merge must invalidate HUD marker cache");

    // Cursor move invalidates sync, leaving replay clears sync/markers
    PatchReplayTelemetryForTest(true, 2, 1000.0f, 900.0f, 2100.0f);
    st.CaptureHudRenderSnapshot(snap);
    EXPECT_EQ(snap.sync.snapshot.matchTick, st.SyncToReplayCursor(2, 1000.0f).snapshot.matchTick,
              "Cursor move did not refresh cached sync");
    PatchReplayTelemetryForTest(false, 0, 0.0f, 0.0f, 0.0f);
    st.CaptureHudRenderSnapshot(snap);
    EXPECT_TRUE(!snap.sync.hasSnapshot && snap.markers.empty(), "Leaving replay must clear sync and markers");

    // 3. Lock-free verdict flag mirrors pendingVerdict_.armed in every transition
    EXPECT_TRUE(!st.HasArmedVerdict(), "No verdict should be armed");
    VarVerdict v{};
    v.action = VarAction::RetroactiveFreeKick;
    v.offenderSlot = 3;
    st.ArmVerdict(v);
    EXPECT_TRUE(st.HasArmedVerdict() && st.PeekVerdict().armed, "ArmVerdict must set flag");
    st.CancelVerdict();
    EXPECT_TRUE(!st.HasArmedVerdict() && !st.PeekVerdict().armed, "CancelVerdict must clear flag");
    st.ArmVerdict(v);
    VarVerdict out{};
    EXPECT_TRUE(st.ConsumeVerdict(out) && !st.HasArmedVerdict(), "ConsumeVerdict must clear flag");
    EXPECT_TRUE(!st.ConsumeVerdict(out), "Double consume must fail");
    VarVerdict none{};
    st.ArmVerdict(none);
    EXPECT_TRUE(!st.ConsumeVerdict(out), "Armed None verdict must not execute");
    EXPECT_TRUE(!st.HasArmedVerdict() && !st.PeekVerdict().armed, "A None verdict is rejected and never arms the pool");
    st.ResetMatch();
    EXPECT_TRUE(!st.HasArmedVerdict() && !st.PeekVerdict().armed, "ResetMatch must clear flag");

    MarkPass("TestPerfOverhaulEquivalence");
}

// ---------------------------------------------------------------------------
// Test 16: Shortcut remap (bindings, conflicts, lock-out guard, INI) + PT-BR / English UI
// ---------------------------------------------------------------------------
void TestRemapAndLanguage() {
    auto& st = VarState::Instance();
    st.ResetMatch();
    st.SetIniPath(L"");
    st.SetHudConfig(HudConfig{}, false);

    using BA = BindAction;
    auto keyOf = [](const InputBindings& b, BA a, int s) { return b.keys[static_cast<size_t>(a)][static_cast<size_t>(s)]; };
    auto padOf = [](const InputBindings& b, BA a) { return b.pad[static_cast<size_t>(a)]; };

    // 1. Defaults are exactly the v4.0 shortcuts
    const InputBindings def = InputBindings::Defaults();
    EXPECT_TRUE(keyOf(def, BA::ToggleFocus, 0) == 'V' && keyOf(def, BA::ToggleFocus, 1) == VK_F7 &&
                padOf(def, BA::ToggleFocus) == PadButtons::RightThumb, "ToggleFocus default must be V / F7 / R3");
    EXPECT_TRUE(keyOf(def, BA::Confirm, 0) == VK_RETURN && padOf(def, BA::Confirm) == 0, "Confirm default (keyboard only)");
    for (int a = 0; a < kBindActionCount; ++a) {
        if (static_cast<BA>(a) != BA::ToggleFocus) {
            EXPECT_EQ(padOf(def, static_cast<BA>(a)), static_cast<uint16_t>(0), "Gamepad must only open / collapse the panel");
        }
    }
    EXPECT_TRUE(keyOf(def, BA::PrevIncident, 0) == VK_OEM_4 && keyOf(def, BA::NextIncident, 0) == VK_OEM_6, "[ ] defaults");
    EXPECT_TRUE(keyOf(def, BA::ToggleDebug, 0) == VK_F10, "Debug default must be F10");

    // 2. Plain assign, then conflict = swap
    InputBindings b = def;
    EXPECT_EQ(AssignBinding(b, BA::PlayPause, 1, 'K'), BindEditResult::Saved, "Free key must be saved");
    EXPECT_EQ(keyOf(b, BA::PlayPause, 1), static_cast<uint8_t>('K'), "Assigned key missing");
    EXPECT_EQ(AssignBinding(b, BA::PlayPause, 0, 'V'), BindEditResult::Moved, "Taken key must be moved");
    EXPECT_TRUE(keyOf(b, BA::PlayPause, 0) == 'V' && keyOf(b, BA::ToggleFocus, 0) == 'P', "Conflict must swap keys");
    EXPECT_EQ(AssignBinding(b, BA::PlayPause, 2, PadButtons::A), BindEditResult::Rejected, "Pad never drives panel actions");
    EXPECT_EQ(AssignBinding(b, BA::ToggleFocus, 2, PadButtons::Start), BindEditResult::Saved, "Open/collapse pad button is remappable");
    EXPECT_EQ(padOf(b, BA::ToggleFocus), PadButtons::Start, "Open/collapse pad remap not stored");

    // 3. Lock-out guard: an essential action can never lose its last shortcut
    b = def;
    EXPECT_EQ(ClearBinding(b, BA::Back, 1), BindEditResult::Saved, "Clearing one of two shortcuts is allowed");
    const InputBindings beforeSteal = b;
    EXPECT_EQ(AssignBinding(b, BA::NextIncident, 1, VK_RETURN), BindEditResult::Rejected,
              "Stealing Confirm's last key into an empty slot must be rejected");
    EXPECT_TRUE(b == beforeSteal, "Rejected assign must leave bindings untouched");
    EXPECT_EQ(ClearBinding(b, BA::Confirm, 0), BindEditResult::Rejected, "Clearing the last Confirm key must be rejected");
    EXPECT_EQ(ClearBinding(b, BA::PlayPause, 0), BindEditResult::Saved, "Non-essential action can be fully cleared");

    // 4. Invalid inputs
    b = def;
    EXPECT_EQ(AssignBinding(b, BA::Confirm, 0, VK_LBUTTON), BindEditResult::Rejected, "Mouse buttons are reserved");
    EXPECT_EQ(AssignBinding(b, BA::Up, 2, PadButtons::A), BindEditResult::Rejected, "Directions are keyboard only");
    EXPECT_EQ(AssignBinding(b, BA::ToggleFocus, 2, PadButtons::A | PadButtons::B), BindEditResult::Rejected, "One button only");

    // 5. INI round-trip of bindings + language
    wchar_t tmpDir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmpDir);
    const std::wstring ini = std::wstring(tmpDir) + L"var_mod_remap_test.ini";
    DeleteFileW(ini.c_str());
    st.SetIniPath(ini);
    HudConfig cfg{};
    AssignBinding(cfg.bindings, BA::PlayPause, 1, 'K');
    AssignBinding(cfg.bindings, BA::ToggleFocus, 2, PadButtons::Start);
    cfg.language = 1;
    st.SetHudConfig(cfg, true);
    st.SetHudConfig(HudConfig{}, false);
    st.LoadHudConfig();
    const HudConfig loaded = st.GetHudConfig();
    EXPECT_TRUE(loaded.bindings == cfg.bindings, "Bindings INI round-trip failed");
    EXPECT_EQ(loaded.language, 1, "Language INI round-trip failed");
    EXPECT_TRUE(i18n::GetLanguage() == Language::English, "Loading language=en must switch the UI language");

    // 6. Hand-edited INI is sanitized: duplicates removed, essential actions restored
    {
        FILE* fp = _wfopen(ini.c_str(), L"wb");
        EXPECT_TRUE(fp != nullptr, "Cannot write test INI");
        std::fprintf(fp, "language = pt\nkey_confirm = 0x00,0x00\npad_confirm = 0x0000\nkey_back = 0x0D,0x01\n");
        std::fclose(fp);
    }
    st.SetHudConfig(HudConfig{}, false);
    st.LoadHudConfig();
    const HudConfig san = st.GetHudConfig();
    EXPECT_EQ(keyOf(san.bindings, BA::Confirm, 0), static_cast<uint8_t>(VK_RETURN), "Empty essential Confirm must be restored");
    EXPECT_TRUE(keyOf(san.bindings, BA::Back, 0) != VK_RETURN && keyOf(san.bindings, BA::Back, 1) != VK_LBUTTON,
                "Duplicate Enter and mouse button must be stripped from Back");
    EXPECT_TRUE(HasAnyBinding(san.bindings, BA::Back), "Back must stay reachable after duplicate removal");
    EXPECT_TRUE(i18n::GetLanguage() == Language::PtBr, "language = pt must switch back to PT-BR");
    DeleteFileW(ini.c_str());
    st.SetIniPath(L"");

    // 7. Translation layer
    i18n::SetLanguage(Language::English);
    EXPECT_TRUE(std::strcmp(Tr("Marcar falta"), "Award free kick") == 0, "EN translation missing");
    EXPECT_TRUE(std::strcmp(VarHudController::CardLabel(CardKind::Yellow), "Yellow") == 0, "CardLabel must follow language");
    char lbl[48] = "CARRINHO NA ÁREA"; // stored PT-BR incident label (runtime buffer)
    EXPECT_TRUE(std::strcmp(Tr(lbl), "SLIDE TACKLE IN THE BOX") == 0, "Stored labels must translate at display");
    const char* unknown = "Flamengo";
    EXPECT_TRUE(Tr(unknown) == unknown, "Unknown text (club/player names) must never be altered");
    char keyName[32];
    InputManager::FormatKeyName(VK_UP, keyName, sizeof(keyName));
    EXPECT_TRUE(std::strcmp(keyName, "Up arrow") == 0, "Key names must follow language");
    {
        VarHudController c2;
        std::array<PlayerSlotInfo, 22> pl{};
        pl[4].shirtNumber = 0; // completely unresolved slot
        pl[13].team = 1;       // unresolved away slot
        pl[7].shirtNumber = 7; // number known, name not yet
        std::snprintf(pl[15].name, sizeof(pl[15].name), "%s", "Fora Cardoso"); // real name that starts with "Fora "
        pl[15].shirtNumber = 9;
        c2.UpdateContext(LiveTelemetry{}, ReplaySyncResult{}, pl, st.GetTeams(), {}, MatchVarStats{}, st.GetHudConfig());
        EXPECT_TRUE(c2.FormatPlayerBroadcast(4) == "Home 5", "Unresolved home player must fall back to localized side + number");
        EXPECT_TRUE(c2.FormatPlayerBroadcast(15 - 2) == "Away 3", "Unresolved away player must fall back to localized side + number");
        EXPECT_TRUE(c2.FormatPlayerBroadcast(7) == "#7", "Number-only player");
        EXPECT_TRUE(c2.FormatPlayerBroadcast(15) == "#9 Fora Cardoso", "Real player names must never be translated");
        VarState::Instance().ResetMatch();
        VarHudController c3;
        c3.UpdateContext(LiveTelemetry{}, ReplaySyncResult{}, pl, VarState::Instance().GetTeams(), {}, MatchVarStats{}, st.GetHudConfig());
        EXPECT_TRUE(std::strcmp(c3.TeamFullName(0), "HOME") == 0 && std::strcmp(c3.TeamFullName(1), "AWAY") == 0,
                    "Unknown teams must fall back to localized CASA/FORA");
        EXPECT_TRUE(std::strcmp(c3.TeamShortName(0), "HOM") == 0 && std::strcmp(c3.TeamShortName(1), "AWY") == 0,
                    "Unknown short names must fall back to localized CAS/FOR");
    }
    i18n::SetLanguage(Language::PtBr);
    EXPECT_TRUE(Tr(lbl) == lbl, "PT-BR mode must return the input pointer (zero cost)");
    InputManager::FormatKeyName(VK_UP, keyName, sizeof(keyName));
    EXPECT_TRUE(std::strcmp(keyName, "Seta cima") == 0, "PT key name");
    InputManager::FormatKeyName(VK_F7, keyName, sizeof(keyName));
    EXPECT_TRUE(std::strcmp(keyName, "F7") == 0, "F-key name");
    st.SetHudConfig(HudConfig{}, false);
    char hint[64];
    InputManager::FormatActionHint(BA::ToggleFocus, hint, sizeof(hint));
    EXPECT_TRUE(std::strcmp(hint, "V / F7 / R3") == 0, "Default panel hint must read V / F7 / R3");

    // 8. Controller: OPCOES rows (Idioma, Controles) and the Controls page edit flow
    VarHudController ctrl;
    LiveTelemetry tel{};
    tel.enabled = true;
    tel.offlineAllowed = true;
    tel.matchActive = true;
    tel.replayMode = 2;
    tel.replayManualFlag = 1;
    auto refresh = [&]() {
        ctrl.UpdateContext(tel, ReplaySyncResult{}, st.GetPlayers(), st.GetTeams(), {}, MatchVarStats{}, st.GetHudConfig());
    };
    refresh();
    ctrl.SelectTab(HudPage::Config);
    ctrl.SetSelectedIndex(kSettingsRowLanguage);
    ctrl.HandleAction(NavAction::Right, st);
    EXPECT_EQ(st.GetHudConfig().language, 1, "Idioma row must switch to English");
    EXPECT_TRUE(i18n::GetLanguage() == Language::English, "Language switch must be instant");
    refresh();
    ctrl.HandleAction(NavAction::Left, st);
    EXPECT_EQ(st.GetHudConfig().language, 0, "Idioma row must switch back to PT-BR");
    refresh();

    ctrl.SetSelectedIndex(kSettingsRowControls);
    ctrl.HandleAction(NavAction::Confirm, st);
    EXPECT_EQ(ctrl.Page(), HudPage::Controls, "Controles row must open the Controls page");
    ctrl.HandleAction(NavAction::NextTab, st);
    EXPECT_EQ(ctrl.Page(), HudPage::Main, "NextTab from Controls (inside OPCOES) must wrap to REVISAO");
    ctrl.SelectTab(HudPage::Config);
    ctrl.SetSelectedIndex(kSettingsRowControls);
    ctrl.HandleAction(NavAction::Confirm, st);

    const int playRow = static_cast<int>(BA::PlayPause);
    ctrl.BeginBindingCapture(playRow, 1, st);
    EXPECT_TRUE(ctrl.IsCapturingBinding() && InputManager::IsCapturing(), "Capture must start");
    ctrl.HandleAction(NavAction::Back, st);
    EXPECT_EQ(ctrl.Page(), HudPage::Controls, "Navigation must be ignored while capturing");
    ctrl.ApplyCapturedBinding('K', st);
    EXPECT_TRUE(!ctrl.IsCapturingBinding() && !InputManager::IsCapturing(), "Capture must end after apply");
    EXPECT_EQ(keyOf(st.GetHudConfig().bindings, BA::PlayPause, 1), static_cast<uint8_t>('K'), "Captured key not stored");
    refresh();

    VarVerdict v{};
    v.action = VarAction::RetroactiveFreeKick;
    v.offenderSlot = 2;
    st.ArmVerdict(v);
    ctrl.SetSelectedIndex(playRow);
    ctrl.SetControlsColumn(1);
    ctrl.HandleAction(NavAction::CancelVerdict, st);
    EXPECT_EQ(keyOf(st.GetHudConfig().bindings, BA::PlayPause, 1), static_cast<uint8_t>(0), "Del/X must clear the cell");
    EXPECT_TRUE(st.HasArmedVerdict(), "Del/X on Controls must NOT cancel the armed verdict");
    st.CancelVerdict();
    refresh();

    ctrl.BeginBindingCapture(static_cast<int>(BA::Up), 2, st);
    EXPECT_TRUE(!ctrl.IsCapturingBinding() && ctrl.ControlsMessageIsError(), "Fixed D-Pad cell must not start capture");

    ctrl.SetSelectedIndex(kControlsRowRestore);
    ctrl.HandleAction(NavAction::Confirm, st);
    EXPECT_TRUE(st.GetHudConfig().bindings == InputBindings::Defaults(), "Restaurar Padrao must reset every shortcut");
    ctrl.HandleAction(NavAction::Back, st);
    EXPECT_EQ(ctrl.Page(), HudPage::Config, "Back from Controls must return to OPCOES");
    EXPECT_EQ(ctrl.SelectedIndex(), kSettingsRowControls, "Back must land on the Controles row");

    st.SetHudConfig(HudConfig{}, false);
    MarkPass("TestRemapAndLanguage");
}

// Optional visual check: when VAR_HUD_SNAPSHOT_DIR is set, ImDrawData is rasterized in software
// (textured triangles, clip rects, alpha blend over a dark pitch-like background) into a 24-bit BMP.
void DumpDrawDataBmp(const char* name, const unsigned char* fontPx, int fontW, int fontH) {
    char dir[MAX_PATH] = {};
    if (GetEnvironmentVariableA("VAR_HUD_SNAPSHOT_DIR", dir, MAX_PATH) == 0) return;
    const ImDrawData* dd = ImGui::GetDrawData();
    if (!dd) return;
    const int W = static_cast<int>(dd->DisplaySize.x);
    const int H = static_cast<int>(dd->DisplaySize.y);
    std::vector<float> fb(static_cast<size_t>(W) * H * 3);
    char flatBg[8] = {};
    const bool flat = GetEnvironmentVariableA("VAR_HUD_SNAPSHOT_FLAT", flatBg, sizeof(flatBg)) > 0; // docs images
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            float* p = &fb[(static_cast<size_t>(y) * W + x) * 3];
            if (flat) {
                p[0] = 0.051f; p[1] = 0.067f; p[2] = 0.090f;
                continue;
            }
            const bool stripe = ((x / 120) % 2) == 0;
            p[0] = stripe ? 0.10f : 0.12f; p[1] = stripe ? 0.30f : 0.34f; p[2] = stripe ? 0.16f : 0.19f;
        }
    }
    auto sampleA = [&](float u, float v) {
        int tx = static_cast<int>(u * fontW), ty = static_cast<int>(v * fontH);
        tx = std::clamp(tx, 0, fontW - 1); ty = std::clamp(ty, 0, fontH - 1);
        const unsigned char* t = fontPx + (static_cast<size_t>(ty) * fontW + tx) * 4;
        return std::array<float, 4>{ t[0] / 255.0f, t[1] / 255.0f, t[2] / 255.0f, t[3] / 255.0f };
    };
    for (int n = 0; n < dd->CmdListsCount; ++n) {
        const ImDrawList* cl = dd->CmdLists[n];
        for (const ImDrawCmd& cmd : cl->CmdBuffer) {
            if (cmd.UserCallback) continue;
            const int cx0 = std::max(0, static_cast<int>(cmd.ClipRect.x)), cy0 = std::max(0, static_cast<int>(cmd.ClipRect.y));
            const int cx1 = std::min(W, static_cast<int>(cmd.ClipRect.z)), cy1 = std::min(H, static_cast<int>(cmd.ClipRect.w));
            for (unsigned int e = 0; e + 2 < cmd.ElemCount; e += 3) {
                const ImDrawVert* v[3];
                for (int k = 0; k < 3; ++k) v[k] = &cl->VtxBuffer[cmd.VtxOffset + cl->IdxBuffer[cmd.IdxOffset + e + k]];
                const float area = (v[1]->pos.x - v[0]->pos.x) * (v[2]->pos.y - v[0]->pos.y) -
                                   (v[2]->pos.x - v[0]->pos.x) * (v[1]->pos.y - v[0]->pos.y);
                if (std::fabs(area) < 1e-6f) continue;
                const int bx0 = std::max(cx0, static_cast<int>(std::floor(std::min({ v[0]->pos.x, v[1]->pos.x, v[2]->pos.x }))));
                const int by0 = std::max(cy0, static_cast<int>(std::floor(std::min({ v[0]->pos.y, v[1]->pos.y, v[2]->pos.y }))));
                const int bx1 = std::min(cx1, static_cast<int>(std::ceil(std::max({ v[0]->pos.x, v[1]->pos.x, v[2]->pos.x }))));
                const int by1 = std::min(cy1, static_cast<int>(std::ceil(std::max({ v[0]->pos.y, v[1]->pos.y, v[2]->pos.y }))));
                for (int y = by0; y < by1; ++y) {
                    for (int x = bx0; x < bx1; ++x) {
                        const float px0 = x + 0.5f, py0 = y + 0.5f;
                        const float w0 = ((v[1]->pos.x - px0) * (v[2]->pos.y - py0) - (v[2]->pos.x - px0) * (v[1]->pos.y - py0)) / area;
                        const float w1 = ((v[2]->pos.x - px0) * (v[0]->pos.y - py0) - (v[0]->pos.x - px0) * (v[2]->pos.y - py0)) / area;
                        const float w2 = 1.0f - w0 - w1;
                        if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) continue;
                        const float u = w0 * v[0]->uv.x + w1 * v[1]->uv.x + w2 * v[2]->uv.x;
                        const float vv = w0 * v[0]->uv.y + w1 * v[1]->uv.y + w2 * v[2]->uv.y;
                        const auto tex = sampleA(u, vv);
                        float col[4];
                        for (int c = 0; c < 4; ++c) {
                            const float c0 = ((v[0]->col >> (8 * c)) & 0xFF) / 255.0f;
                            const float c1 = ((v[1]->col >> (8 * c)) & 0xFF) / 255.0f;
                            const float c2 = ((v[2]->col >> (8 * c)) & 0xFF) / 255.0f;
                            col[c] = (w0 * c0 + w1 * c1 + w2 * c2) * tex[c];
                        }
                        float* p = &fb[(static_cast<size_t>(y) * W + x) * 3];
                        for (int c = 0; c < 3; ++c) p[c] = col[c] * col[3] + p[c] * (1.0f - col[3]);
                    }
                }
            }
        }
    }
    char path[MAX_PATH * 2];
    std::snprintf(path, sizeof(path), "%s\\%s.bmp", dir, name);
    FILE* fp = std::fopen(path, "wb");
    if (!fp) return;
    const int rowBytes = (W * 3 + 3) & ~3;
    const uint32_t dataSize = static_cast<uint32_t>(rowBytes) * H;
    uint8_t hdr[54] = { 'B', 'M' };
    auto put32 = [&hdr](int off, uint32_t val) { std::memcpy(hdr + off, &val, 4); };
    put32(2, 54 + dataSize); put32(10, 54); put32(14, 40); put32(18, static_cast<uint32_t>(W));
    put32(22, static_cast<uint32_t>(H)); hdr[26] = 1; hdr[28] = 24; put32(34, dataSize);
    std::fwrite(hdr, 1, 54, fp);
    std::vector<uint8_t> row(static_cast<size_t>(rowBytes), 0);
    for (int y = H - 1; y >= 0; --y) {
        for (int x = 0; x < W; ++x) {
            const float* p = &fb[(static_cast<size_t>(y) * W + x) * 3];
            row[static_cast<size_t>(x) * 3 + 0] = static_cast<uint8_t>(std::clamp(p[2], 0.0f, 1.0f) * 255.0f);
            row[static_cast<size_t>(x) * 3 + 1] = static_cast<uint8_t>(std::clamp(p[1], 0.0f, 1.0f) * 255.0f);
            row[static_cast<size_t>(x) * 3 + 2] = static_cast<uint8_t>(std::clamp(p[0], 0.0f, 1.0f) * 255.0f);
        }
        std::fwrite(row.data(), 1, row.size(), fp);
    }
    std::fclose(fp);
}

// ---------------------------------------------------------------------------
// Test 17: Headless HUD render smoke (every page incl. Controls + capture, PT-BR and English).
// Runs the real VarHudRenderer::RenderFrame under ImGui with asserts enabled: any Begin/End,
// PushClipRect/PopClipRect, BeginChild/EndChild, font or ID stack imbalance aborts the suite.
// ---------------------------------------------------------------------------
float DistSq2DForTest(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return dx * dx + dz * dz;
}

void TestHudRenderSmoke() {
    auto& st = VarState::Instance();
    st.ResetMatch();
    st.SetIniPath(L"");
    st.SetHudConfig(HudConfig{}, false);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    // Fonts are built exactly like in game (VarHudRenderer::EnsureFonts per resolution)
    unsigned char* px = nullptr;
    int tw = 0, th = 0;
    char shotName[64];

    // A real-looking replay window: 1st half, 23:05 to 23:25, FLA (home) builds up from midfield and PAL (away)
    // defends its box. Three plays on the bar: a foul the referee gave in midfield (23:10), a
    // standing tackle with nobody going down (23:13) and a slide that floors the striker inside the box with
    // no call (23:16), the suspect. The cursor sits on that slide, so the pitch shows the contact in the box.
    constexpr float kWinStart = 83100.0f; // 23:05
    constexpr float kWinEnd   = 84300.0f; // 23:25
    constexpr float kOfficial = 83400.0f; // 23:10
    constexpr float kChallenge = 83580.0f; // 23:13
    constexpr float kSuspect  = 83760.0f; // 23:16
    PitchDims dims{};
    const float attackDir[2] = { -1.0f, 1.0f }; // per-team box side used by the engine geometry (PAL: +x)
    auto lerp = [](float a, float b, float k) { return a + (b - a) * std::clamp(k, 0.0f, 1.0f); };
    auto sceneAt = [&](float time, PlaySnapshot& sn) {
        // Two key formations (FLA attacks +x, PAL defends the +x box): build-up around midfield at 23:05 and the
        // attack settled around the box at the slide. Players glide between them, then drift slightly.
        static const float kStart[22][2] = {
            { -46.0f, 0.0f }, { -18.0f, 22.0f }, { -24.0f, -7.0f }, { -23.0f, 8.0f }, { -17.0f, -22.0f },
            { -4.0f, -9.0f }, { -6.0f, 7.0f }, { 5.0f, -2.0f }, { 12.0f, -20.0f }, { 13.0f, 19.0f }, { 16.0f, 2.0f },
            { 49.0f, 0.0f }, { 29.0f, -19.0f }, { 30.0f, -6.0f }, { 30.0f, 7.0f }, { 28.0f, 20.0f },
            { 4.0f, -9.0f }, { 6.0f, 4.0f }, { 8.0f, 17.0f }, { 14.0f, -12.0f }, { -8.0f, 6.0f }, { -10.0f, -12.0f },
        };
        static const float kBox[22][2] = {
            { -40.0f, 0.0f }, { 22.0f, 24.0f }, { 6.0f, -8.0f }, { 8.0f, 9.0f }, { 27.0f, -25.0f },
            { 20.0f, -6.0f }, { 25.0f, 10.0f }, { 32.0f, -2.0f }, { 39.0f, -16.0f }, { 38.0f, 16.5f }, { 43.6f, 3.6f },
            { 50.0f, 1.0f }, { 42.0f, -11.0f }, { 41.0f, -2.5f }, { 44.3f, 2.7f }, { 40.5f, 12.0f },
            { 33.0f, -9.0f }, { 30.5f, 4.5f }, { 34.0f, 17.0f }, { 24.0f, -3.0f }, { 14.0f, 7.0f }, { 16.0f, -15.0f },
        };
        const float s = (time - kWinStart) / (kSuspect - kWinStart);
        const float k = std::clamp(s, 0.0f, 1.0f);
        const float ease = k; // steady build-up: players still running at the moment of the slide
        const float after = std::max(0.0f, time - kSuspect) / 60.0f; // seconds after the slide
        for (int p = 0; p < 22; ++p) {
            const float wob = 0.6f * std::sin(time * 0.003f + static_cast<float>(p) * 1.7f);
            const float drift = (p == 10 || p == 14) ? 0.0f : std::min(after, 3.0f) * ((p < 11) ? 0.5f : -0.3f);
            sn.playerOnPitch[p] = 1;
            sn.playerPos[p] = Vec3{ lerp(kStart[p][0], kBox[p][0], ease) + drift, 0.0f, lerp(kStart[p][1], kBox[p][1], ease) + wob };
        }
        // The plays: Pulgar (5) on Anibal Moreno (16) in midfield, Andreas Pereira (17) into Arrascaeta (7)
        auto meet = [&](int a, int b, const Vec3& at, float when, float width) {
            const float w = 1.0f - std::clamp(std::fabs(time - when) / width, 0.0f, 1.0f);
            sn.playerPos[a] = Vec3{ lerp(sn.playerPos[a].x, at.x - 0.5f, w), 0.0f, lerp(sn.playerPos[a].z, at.z - 0.3f, w) };
            sn.playerPos[b] = Vec3{ lerp(sn.playerPos[b].x, at.x + 0.5f, w), 0.0f, lerp(sn.playerPos[b].z, at.z + 0.3f, w) };
        };
        meet(5, 16, Vec3{ 3.0f, 0.0f, -8.0f }, kOfficial, 70.0f);
        meet(17, 7, Vec3{ 21.0f, 0.0f, 0.5f }, kChallenge, 60.0f);
        // The slide: Murilo goes through and ends past Pedro, who stays down where he was hit
        if (time > kSuspect) {
            const float slide = std::min(after / 0.6f, 1.0f);
            sn.playerPos[14] = Vec3{ lerp(44.3f, 46.6f, slide), 0.0f, lerp(2.7f, 0.6f, slide) };
        }
        // Ball: with the man on it, then loose after the slide
        Vec3 ball{};
        if (time < kOfficial) ball = sn.playerPos[16];
        else if (time < kChallenge) ball = sn.playerPos[7];
        else if (time < kSuspect) ball = sn.playerPos[10];
        else ball = Vec3{ lerp(44.6f, 40.0f, after / 4.0f), 0.0f, lerp(4.4f, 11.0f, after / 4.0f) };
        sn.ballPos = Vec3{ ball.x + 0.6f, 0.11f, ball.z + 0.4f };
        sn.ballOwnerSlot = (time < kSuspect) ? ((time < kOfficial) ? 16 : (time < kChallenge) ? 7 : 10) : -1;
    };
    for (int i = 0; i < static_cast<int>(kWinEnd - kWinStart); ++i) {
        PlaySnapshot sn{};
        sn.matchPhase = 0;
        sn.time60Hz = kWinStart + static_cast<float>(i);
        sn.matchTick = static_cast<uint32_t>(50000 + i);
        sn.simFrame = sn.matchTick;
        sceneAt(sn.time60Hz, sn);
        VarState::ComputeClosestContact(sn);
        st.PushSnapshot(sn);
    }
    auto incident = [&](IncidentKind kind, int32_t prio, int off, int vic, float when, bool nearFoul) {
        PlaySnapshot sn{};
        sceneAt(when, sn);
        DetectedIncident inc{};
        inc.matchPhase = 0;
        inc.time60Hz = when;
        inc.lastSeenTime60Hz = when + 12.0f;
        inc.matchTick = static_cast<uint32_t>(50000 + static_cast<int>(when - kWinStart));
        inc.kind = kind;
        inc.priority = prio;
        inc.offenderSlot = off;
        inc.victimSlot = vic;
        inc.pos = Vec3{ 0.35f * sn.playerPos[off].x + 0.65f * sn.playerPos[vic].x, 0.0f,
                        0.35f * sn.playerPos[off].z + 0.65f * sn.playerPos[vic].z };
        inc.contactDistM = std::sqrt(DistSq2DForTest(sn.playerPos[off], sn.playerPos[vic]));
        inc.boxDistM = PitchGeometry::SignedDistanceToDefendingBox(dims, inc.pos, attackDir[SlotToTeam(off)]);
        inc.boxDistValid = true;
        inc.insideBox = inc.boxDistM < 0.0f;
        inc.nearFoul = nearFoul;
        st.PushDetectedIncident(inc);
    };
    incident(IncidentKind::BodyCharge, 66, 5, 16, kOfficial - 20.0f, true);   // folded into the official foul
    incident(IncidentKind::StandTackle, 70, 17, 7, kChallenge, false);       // challenge, nobody down
    incident(IncidentKind::SlideTackle, 88, 14, 10, kSuspect, true);         // the missed penalty
    VarEvent ev{};
    ev.type = VarEventType::Foul;
    ev.matchPhase = 0;
    ev.time60Hz = kOfficial;
    ev.matchTick = 50300;
    ev.offenderSlot = 5;
    ev.victimSlot = 16;
    ev.benefitedTeam = 1;
    {
        PlaySnapshot sn{};
        sceneAt(kOfficial, sn);
        ev.pos = sn.playerPos[16];
    }
    st.PushEvent(ev);

    // Flamengo (home) x Palmeiras with real squad names and numbers
    {
        std::array<TeamDisplayInfo, 2> tm{};
        std::snprintf(tm[0].fullName, sizeof(tm[0].fullName), "%s", "FLAMENGO");
        std::snprintf(tm[0].shortName, sizeof(tm[0].shortName), "%s", "FLA");
        tm[0].primaryColor = RgbColor{ 200, 16, 46 };
        tm[0].secondaryColor = RgbColor{ 20, 20, 20 };
        tm[0].hasRealData = true;
        std::snprintf(tm[1].fullName, sizeof(tm[1].fullName), "%s", "PALMEIRAS");
        std::snprintf(tm[1].shortName, sizeof(tm[1].shortName), "%s", "PAL");
        tm[1].primaryColor = RgbColor{ 0, 110, 64 };
        tm[1].secondaryColor = RgbColor{ 255, 255, 255 };
        tm[1].hasRealData = true;
        st.UpdateTeams(tm);
        static const struct { const char* n; uint8_t num; } kXI[22] = {
            { "AGUSTÍN ROSSI", 1 }, { "GUILLERMO VARELA", 2 }, { "LÉO ORTIZ", 3 }, { "LÉO PEREIRA", 4 }, { "ALEX SANDRO", 26 },
            { "ERICK PULGAR", 5 }, { "JORGINHO", 21 }, { "DE ARRASCAETA", 10 }, { "SAMUEL LINO", 16 }, { "LUIZ ARAUJO", 7 }, { "PEDRO", 9 },
            { "WEVERTON", 21 }, { "MARCOS ROCHA", 2 }, { "GUSTAVO GÓMEZ", 15 }, { "MURILO", 26 }, { "PIQUEREZ", 22 },
            { "ANÍBAL MORENO", 5 }, { "ANDREAS PEREIRA", 8 }, { "RAPHAEL VEIGA", 23 }, { "FACUNDO TORRES", 17 }, { "VITOR ROQUE", 9 }, { "FLACO LÓPEZ", 42 },
        };
        std::array<PlayerSlotInfo, 22> pl{};
        for (int i = 0; i < 22; ++i) {
            pl[i].slot = i;
            pl[i].team = (i < 11) ? 0 : 1;
            pl[i].onPitch = 1;
            pl[i].shirtNumber = kXI[i].num;
            std::snprintf(pl[i].name, sizeof(pl[i].name), "%s", kXI[i].n);
        }
        pl[5].yellows = 1;
        st.UpdatePlayers(pl);
    }

    // UI scale keeps the same share of the screen: proportional to the height on 16:9 and wider screens,
    // limited by the width on 4:3 / 5:4, never below the readable floor nor above the atlas ceiling
    {
        struct ScaleCase { float w, h, expect; };
        const ScaleCase cases[] = {
            { 1920.0f, 1080.0f, 1.00f }, { 1280.0f, 720.0f, 0.65f }, { 1366.0f, 768.0f, 0.70f },
            { 1600.0f, 900.0f, 0.85f }, { 2560.0f, 1440.0f, 1.35f }, { 3840.0f, 2160.0f, 2.00f },
            { 2560.0f, 1080.0f, 1.00f }, { 3440.0f, 1440.0f, 1.35f }, { 1024.0f, 768.0f, 0.70f },
            { 1280.0f, 1024.0f, 0.90f }, { 800.0f, 600.0f, 0.60f }, { 7680.0f, 4320.0f, 3.00f },
        };
        for (const auto& c : cases) {
            const float sc = VarHudRenderer::ComputeUiScale(c.w, c.h, 1);
            if (std::fabs(sc - c.expect) > 0.051f) {
                std::printf("[FAIL] UI scale %.0fx%.0f = %.2f, expected %.2f\n", c.w, c.h, sc, c.expect);
                ++g_failed;
                return;
            }
        }
    }

    const float sizes[][2] = { { 1920.0f, 1080.0f }, { 1280.0f, 720.0f }, { 3840.0f, 2160.0f },
                               { 1024.0f, 768.0f }, { 3440.0f, 1440.0f }, { 1280.0f, 1024.0f }, { 800.0f, 600.0f } };
    int framesRendered = 0;
    for (int lang = 0; lang < 2; ++lang) {
        for (const auto& sz : sizes) {
            HudConfig c{};
            c.language = lang;
            st.SetHudConfig(c, false);
            PatchReplayTelemetryForTest(true, 0, kSuspect, kWinStart, kWinEnd);
            st.SetPostApplyBanner("", 0); // the previous resolution's "applied" banner must not leak into these shots
            VarHudRenderer::RequestReset();
            io.DisplaySize = ImVec2(sz[0], sz[1]);
            VarHudRenderer::EnsureFonts(sz[0], sz[1]);
            io.Fonts->GetTexDataAsRGBA32(&px, &tw, &th);

            auto frame = [&]() {
                io.DeltaTime = 1.0f / 60.0f;
                ImGui::NewFrame();
                VarHudRenderer::RenderFrame(sz[0], sz[1]);
                ImGui::Render();
                ++framesRendered;
            };
            const char* langTag = lang ? "en" : "pt";
            const int resTag = static_cast<int>(sz[1]);
            // A resolution change settles in a few frames (panel measures its body, window auto-fits)
            for (int f = 0; f < 8; ++f) frame();
            std::snprintf(shotName, sizeof(shotName), "%s_%dx%d_1_revisao", langTag, static_cast<int>(sz[0]), resTag);
            DumpDrawDataBmp(shotName, px, tw, th);
            // Docs animation (VAR_HUD_SEQUENCE set): the replay cursor runs through the play at 1080p, the pitch
            // follows it frame by frame and the suspect marker pulses. Back to the slide afterwards.
            if (sz[1] == 1080.0f && sz[0] == 1920.0f && GetEnvironmentVariableA("VAR_HUD_SEQUENCE", nullptr, 0) > 0) {
                int seq = 0;
                for (float t = kSuspect - 420.0f; t <= kSuspect + 48.0f; t += 6.0f) {
                    PatchReplayTelemetryForTest(true, 0, t, kWinStart, kWinEnd);
                    frame();
                    std::snprintf(shotName, sizeof(shotName), "seq_%s_%03d", langTag, seq++);
                    DumpDrawDataBmp(shotName, px, tw, th);
                }
                PatchReplayTelemetryForTest(true, 0, kSuspect, kWinStart, kWinEnd);
                for (int f = 0; f < 8; ++f) frame();
            }
            // Every HUD window stays inside the screen and the panel keeps a sane share of it
            {
                ImGuiContext& g = *ImGui::GetCurrentContext();
                for (ImGuiWindow* w : g.Windows) {
                    if (!w->WasActive || w->Hidden || (w->Flags & ImGuiWindowFlags_ChildWindow)) continue;
                    if (std::strstr(w->Name, "Debug") != nullptr) continue; // ImGui fallback and the debug overlay
                    if (GetEnvironmentVariableA("VAR_HUD_SNAPSHOT_DIR", nullptr, 0) > 0) {
                        std::printf("[info] %.0fx%.0f %s pos (%.0f,%.0f) size (%.0f,%.0f) content %.0f\n", sz[0], sz[1], w->Name,
                                    w->Pos.x, w->Pos.y, w->Size.x, w->Size.y, w->ContentSize.y);
                    }
                    const bool inside = w->Pos.x >= -0.5f && w->Pos.y >= -0.5f &&
                                        w->Pos.x + w->Size.x <= sz[0] + 0.5f && w->Pos.y + w->Size.y <= sz[1] + 0.5f;
                    if (!inside) {
                        std::printf("[FAIL] HUD window '%s' outside %.0fx%.0f: pos (%.0f,%.0f) size (%.0f,%.0f)\n",
                                    w->Name, sz[0], sz[1], w->Pos.x, w->Pos.y, w->Size.x, w->Size.y);
                        ++g_failed;
                        return;
                    }
                    if (w->Size.x > sz[0] * 0.5f || w->Size.x < sz[0] * 0.12f) {
                        std::printf("[FAIL] HUD window '%s' width %.0f is not a sane share of %.0f\n", w->Name, w->Size.x, sz[0]);
                        ++g_failed;
                        return;
                    }
                }
            }
            EXPECT_EQ(VarHudRenderer::CurrentPage(), HudPage::Main, "Panel must open on REVISAO");
            EXPECT_TRUE(ImGui::GetDrawData()->TotalVtxCount > 0, "Panel produced no geometry");

            // Decision page (first main option) then back
            InputManager::QueueKeyboardAction(NavAction::Confirm);
            frame();
            frame();
            EXPECT_EQ(VarHudRenderer::CurrentPage(), HudPage::Configure, "Confirm must open the decision page");
            for (int f = 0; f < 6; ++f) frame(); // page change settles like a resolution change
            std::snprintf(shotName, sizeof(shotName), "%s_%dx%d_1b_decisao", langTag, static_cast<int>(sz[0]), resTag);
            DumpDrawDataBmp(shotName, px, tw, th);
            InputManager::QueueKeyboardAction(NavAction::Back);
            frame();
            frame();
            EXPECT_EQ(VarHudRenderer::CurrentPage(), HudPage::Main, "Back must return to REVISAO");

            InputManager::QueueKeyboardAction(NavAction::NextTab);
            frame();
            frame();
            EXPECT_EQ(VarHudRenderer::CurrentPage(), HudPage::Squad, "NextTab must reach CARTOES");
            std::snprintf(shotName, sizeof(shotName), "%s_%dx%d_2_cartoes", langTag, static_cast<int>(sz[0]), resTag);
            DumpDrawDataBmp(shotName, px, tw, th);

            InputManager::QueueKeyboardAction(NavAction::NextTab);
            frame();
            frame();
            EXPECT_EQ(VarHudRenderer::CurrentPage(), HudPage::Config, "NextTab must reach OPCOES");
            std::snprintf(shotName, sizeof(shotName), "%s_%dx%d_3_opcoes", langTag, static_cast<int>(sz[0]), resTag);
            DumpDrawDataBmp(shotName, px, tw, th);
            // Docs / visual check only (VAR_HUD_UPDATE_SHOT set): OPTIONS with a newer version known
            if (GetEnvironmentVariableA("VAR_HUD_UPDATE_SHOT", nullptr, 0) > 0) {
                UpdateChecker::NoteLatest("9.9.9");
                frame();
                frame();
                std::snprintf(shotName, sizeof(shotName), "%s_%dx%d_3b_opcoes_nova_versao", langTag, static_cast<int>(sz[0]), resTag);
                DumpDrawDataBmp(shotName, px, tw, th);
            }

            for (int i = 0; i < kSettingsRowControls; ++i) InputManager::QueueKeyboardAction(NavAction::Down);
            frame();
            InputManager::QueueKeyboardAction(NavAction::Confirm);
            frame();
            frame();
            EXPECT_EQ(VarHudRenderer::CurrentPage(), HudPage::Controls, "Controles row must open the Controls page");
            std::snprintf(shotName, sizeof(shotName), "%s_%dx%d_4_controles", langTag, static_cast<int>(sz[0]), resTag);
            DumpDrawDataBmp(shotName, px, tw, th);

            // Walk every row (forces scroll-to-selection on 720p) and every column
            for (int i = 0; i < kControlsRowCount; ++i) {
                InputManager::QueueKeyboardAction(NavAction::Down);
                InputManager::QueueKeyboardAction(NavAction::Right);
                frame();
            }

            // Capture state renders (pulsing cell + prompt), then a reset must disarm the capture
            for (int i = 0; i < kControlsRowCount; ++i) InputManager::QueueKeyboardAction(NavAction::Up); // back to row 0
            frame();
            InputManager::QueueKeyboardAction(NavAction::Confirm);
            frame();
            frame();
            EXPECT_TRUE(VarHudRenderer::IsCapturingBinding() && InputManager::IsCapturing(), "Enter on a cell must start capture");
            std::snprintf(shotName, sizeof(shotName), "%s_%d_5_captura", langTag, resTag);
            DumpDrawDataBmp(shotName, px, tw, th);
            VarHudRenderer::RequestReset();
            frame();
            EXPECT_TRUE(!InputManager::IsCapturing(), "Controller reset must cancel any armed capture");

            // Multi-item Decision Pool (1 play + 4 cards -> exercises >3 scrollable pool + CARTOES badges + collapsed pill)
            VarVerdict armedV{};
            armedV.action = VarAction::RetroactivePenalty;
            armedV.offenderSlot = 14;
            armedV.victimSlot = 10;
            std::snprintf(armedV.summary, sizeof(armedV.summary), "%s",
                          lang ? "Penalty \xC2\xB7 FLA \xC2\xB7 Yellow #26 Murilo"
                               : "P\xC3\xAAnalti \xC2\xB7 FLA \xC2\xB7 Amarelo #26 Murilo");
            st.ArmVerdict(armedV);
            struct CardEdit { int slot; int yellows; const char* pt; const char* en; };
            const CardEdit edits[] = {
                { 5,  0, "Cart\xC3\xA3o \xC2\xB7 #5 Erick Pulgar \xC2\xB7 sem cart\xC3\xA3o", "Card \xC2\xB7 #5 Erick Pulgar \xC2\xB7 no card" },
                { 17, 1, "Cart\xC3\xA3o \xC2\xB7 #8 Andreas Pereira \xC2\xB7 1 amarelo", "Card \xC2\xB7 #8 Andreas Pereira \xC2\xB7 1 yellow" },
                { 16, 1, "Cart\xC3\xA3o \xC2\xB7 #5 An\xC3\xAD" "bal Moreno \xC2\xB7 1 amarelo", "Card \xC2\xB7 #5 An\xC3\xAD" "bal Moreno \xC2\xB7 1 yellow" },
                { 6,  1, "Cart\xC3\xA3o \xC2\xB7 #21 Jorginho \xC2\xB7 1 amarelo", "Card \xC2\xB7 #21 Jorginho \xC2\xB7 1 yellow" },
            };
            for (const auto& ce : edits) {
                VarVerdict cv{};
                cv.action = VarAction::EditCard;
                cv.offenderSlot = ce.slot;
                cv.directCardEdit = true;
                cv.directYellows = ce.yellows;
                std::snprintf(cv.summary, sizeof(cv.summary), "%s", lang ? ce.en : ce.pt);
                st.ArmVerdict(cv);
            }
            st.SetPostApplyBanner("DECISÃO DO VAR APLICADA", 4000);
            InputManager::QueueKeyboardAction(NavAction::ToggleFocus);
            frame();
            frame();
            std::snprintf(shotName, sizeof(shotName), "%s_%d_6_recolhido", langTag, resTag);
            DumpDrawDataBmp(shotName, px, tw, th);
            InputManager::QueueKeyboardAction(NavAction::ToggleFocus);
            frame();
            frame();
            std::snprintf(shotName, sizeof(shotName), "%s_%d_7_aberto_pronta", langTag, resTag);
            DumpDrawDataBmp(shotName, px, tw, th);
            InputManager::QueueKeyboardAction(NavAction::NextTab); // CARTOES with POOL badges
            frame();
            frame();
            InputManager::QueueKeyboardAction(NavAction::PrevTab); // back to REVISAO
            frame();
            st.CancelVerdict();
            InputManager::QueueKeyboardAction(NavAction::ToggleFocus);
            frame();
            frame();
            std::snprintf(shotName, sizeof(shotName), "%s_%d_8_recolhido_vazio", langTag, resTag);
            DumpDrawDataBmp(shotName, px, tw, th);
            InputManager::QueueKeyboardAction(NavAction::ToggleFocus);
            st.SetDebugOverlayOpen(true);
            frame();
            frame();
            std::snprintf(shotName, sizeof(shotName), "%s_%d_9_telemetria", langTag, resTag);
            DumpDrawDataBmp(shotName, px, tw, th);
            st.SetDebugOverlayOpen(false);
        }
    }

    PatchReplayTelemetryForTest(false, 0, 0.0f, 0.0f, 0.0f);
    VarHudRenderer::RequestReset();
    VarHudRenderer::TickIdle();
    VarHudRenderer::ResetFonts();
    ImGui::DestroyContext();
    st.SetHudConfig(HudConfig{}, false);
    st.ResetMatch();
    EXPECT_TRUE(framesRendered > 100, "Render smoke did not run");
    MarkPass("TestHudRenderSmoke");
}

// ---------------------------------------------------------------------------
// Test 18: Composite VerdictPool (max 1 play/restart + 0..22 per-player cards,
// single-foul replacement guard, individual removal, official event dedup, RCU offsets)
// ---------------------------------------------------------------------------
void TestCompositeVerdictPoolAndRcuOffsets() {
    EXPECT_EQ(Offsets::MatchInfo_StructSize, static_cast<size_t>(0x2378),
              "MatchInfo RCU double-buffer stride must be 0x2378");
    EXPECT_TRUE(Offsets::MatchInfo_FoulSnapshotIds + 12 <= Offsets::MatchInfo_StructSize,
                "FoulSnapshotIds must reside inside MatchInfo RCU buffer");
    EXPECT_EQ(Offsets::MatchFSM_QuickRestartFlag, static_cast<uintptr_t>(0x27FC),
              "MatchFSM_QuickRestartFlag must be 0x27FC");

    auto& st = VarState::Instance();
    st.ResetMatch();
    st.SetIniPath(L"");
    st.SetHudConfig(HudConfig{}, false);

    auto players = st.GetPlayers();
    players[10].shirtNumber = 9;
    players[10].yellows = 1;
    std::snprintf(players[10].name, sizeof(players[10].name), "%s", "PEDRO");
    players[15].shirtNumber = 15;
    std::snprintf(players[15].name, sizeof(players[15].name), "%s", "ZARACHO");
    st.UpdatePlayers(players);

    auto teams = st.GetTeams();
    std::snprintf(teams[0].fullName, sizeof(teams[0].fullName), "%s", "FLAMENGO");
    std::snprintf(teams[0].shortName, sizeof(teams[0].shortName), "%s", "FLA");
    std::snprintf(teams[1].fullName, sizeof(teams[1].fullName), "%s", "ATLETICO");
    std::snprintf(teams[1].shortName, sizeof(teams[1].shortName), "%s", "CAM");
    st.UpdateTeams(teams);

    LiveTelemetry tel{};
    tel.enabled = true;
    tel.offlineAllowed = true;
    tel.matchActive = true;
    tel.matchPhase = 0;
    tel.matchStatus = static_cast<int32_t>(MatchStatus::SetPlayReady);
    tel.fsmState = 0x1A;
    tel.replayMode = 2;
    tel.replayManualFlag = 1;

    VarHudController ctrl;
    ctrl.UpdateContext(tel, ReplaySyncResult{}, players, teams, {}, MatchVarStats{}, HudConfig{});

    // 1. Remove card on #9 PEDRO (slot 10) via CARTOES -> returns to Squad so multi-edits stay fluid
    ctrl.SelectTab(HudPage::Squad);
    ctrl.SelectSquadPlayer(10);
    EXPECT_EQ(ctrl.Page(), HudPage::Configure, "Expected Configure for slot 10");
    EXPECT_EQ(ctrl.DraftDirectCardPreset(), 1, "Should sync to 1 yellow");
    ctrl.SetDraftDirectCardPreset(0); // Sem cartoes
    ctrl.TriggerConfigField(ConfigField::Confirm, 1, st);
    EXPECT_EQ(ctrl.Page(), HudPage::Squad, "Confirming card from CARTOES must return to CARTOES");

    VerdictPool pool = st.PeekVerdictPool();
    EXPECT_EQ(pool.Count(), static_cast<size_t>(1), "Pool must have 1 decision");
    EXPECT_TRUE(!pool.hasPlayDecision && pool.FindCardForSlot(10) != nullptr, "Slot 10 card removal must be active in pool");
    EXPECT_TRUE(ctrl.PoolNoticeMessage() == nullptr, "No play replacement notice yet");

    // 2. Switch to REVISAO and award Free Kick + Red Card against #15 ZARACHO (slot 15) -> BOTH coexist in Pool!
    ctrl.SelectTab(HudPage::Main);
    ReplayMarker mk{};
    mk.matchPhase = 0;
    mk.time60Hz = 1200.0f;
    mk.kind = IncidentKind::SlideTackle;
    mk.offenderSlot = 15;
    mk.victimSlot = 6;
    mk.pos = Vec3{ 20.0f, 0.0f, 4.0f };
    std::snprintf(mk.label, sizeof(mk.label), "%s", "CARRINHO");
    ctrl.UpdateReplayMarkers({ mk });
    ctrl.SelectReplayMarker(0, st, false);
    ctrl.SelectMainOption(0, st); // Marcar falta
    EXPECT_EQ(ctrl.DraftAction(), VarAction::RetroactiveFreeKick, "Expected RetroactiveFreeKick");
    ctrl.SetDraftCardDirect(CardKind::Red);
    ctrl.TriggerConfigField(ConfigField::Confirm, 1, st);
    EXPECT_EQ(ctrl.Page(), HudPage::Main, "Confirming play from REVISAO returns to REVISAO");

    pool = st.PeekVerdictPool();
    EXPECT_EQ(pool.Count(), static_cast<size_t>(2), "Pool must hold card removal + free kick together");
    EXPECT_TRUE(pool.hasPlayDecision && pool.playDecision.action == VarAction::RetroactiveFreeKick, "Free kick must be in pool");
    EXPECT_TRUE(pool.FindCardForSlot(10) && pool.FindCardForSlot(10)->verdict.directYellows == 0,
                "PEDRO card removal must NOT be overwritten by the foul decision");
    EXPECT_TRUE(pool.FindCardForSlot(10)->order < pool.playOrder, "Chronological order must be preserved");
    EXPECT_TRUE(ctrl.PoolNoticeMessage() == nullptr, "First play decision must not trigger replacement notice");

    // 3. Domain guardrail: adding a SECOND play decision (Marcar penalti) replaces the previous play decision
    //    (never two simultaneous fouls!), raises notice, and STILL preserves PEDRO's card removal!
    ctrl.SelectMainOption(1, st); // Marcar penalti
    EXPECT_EQ(ctrl.DraftAction(), VarAction::RetroactivePenalty, "Expected RetroactivePenalty");
    ctrl.SetDraftCardDirect(CardKind::Yellow);
    ctrl.TriggerConfigField(ConfigField::Confirm, 1, st);

    pool = st.PeekVerdictPool();
    EXPECT_EQ(pool.Count(), static_cast<size_t>(2), "Second play must replace first play, keeping count == 2");
    EXPECT_EQ(pool.playDecision.action, VarAction::RetroactivePenalty, "Play decision must update to Penalty");
    EXPECT_TRUE(pool.FindCardForSlot(10) != nullptr, "Independent card decision must remain intact on play replacement");
    // Same play re-decided (free kick -> penalty on the same moment) is an update of that play's decision:
    // no "another play was replaced" notice, and the confirmation toast says "atualizada"
    EXPECT_TRUE(ctrl.PoolNoticeMessage() == nullptr, "Re-deciding the same play must not claim another play was replaced");
    {
        uint64_t age = 0;
        const char* toast = ctrl.ConfirmToast(GetTickCount64(), age);
        EXPECT_TRUE(toast && std::strstr(toast, "atualizada") != nullptr, "Same play re-decided must toast 'atualizada'");
    }

    // 4. Individual item removal & per-slot deduplication
    VarVerdict c2{};
    c2.action = VarAction::EditCard;
    c2.offenderSlot = 3;
    c2.directCardEdit = true;
    c2.directYellows = 1;
    std::snprintf(c2.summary, sizeof(c2.summary), "%s", "Cartão · #4 · 1 amarelo");
    st.AddToVerdictPool(c2);
    c2.directYellows = 0;
    c2.directReds = 1;
    st.AddToVerdictPool(c2); // same slot 3 -> deduplicated in-place!
    pool = st.PeekVerdictPool();
    EXPECT_EQ(pool.CardCount(), static_cast<size_t>(2), "Same-slot card edit must deduplicate");
    EXPECT_EQ(pool.FindCardForSlot(3)->verdict.directReds, static_cast<uint8_t>(1), "Slot 3 card must update to Red");

    st.RemoveCardFromPool(3);
    pool = st.PeekVerdictPool();
    EXPECT_TRUE(pool.FindCardForSlot(3) == nullptr && pool.FindCardForSlot(10) != nullptr && pool.hasPlayDecision,
                "RemoveCardFromPool(3) must only remove slot 3");

    st.RemovePlayFromPool();
    pool = st.PeekVerdictPool();
    EXPECT_TRUE(!pool.hasPlayDecision && pool.FindCardForSlot(10) != nullptr && st.HasArmedVerdict(),
                "RemovePlayFromPool must keep remaining card entries armed");

    // 5. Official event card-tweak + play conversion deduplication (prevents double origCard subtraction)
    VarVerdict evCard{};
    evCard.action = VarAction::EditCard;
    evCard.sourceEventId = 88;
    evCard.offenderSlot = 15;
    evCard.directCardEdit = false;
    evCard.origCard = CardKind::Yellow;
    evCard.targetCard = CardKind::Red;
    st.AddToVerdictPool(evCard);
    EXPECT_EQ(st.PeekVerdictPool().CardCount(), static_cast<size_t>(2), "Should hold slot 10 direct + slot 15 event card");

    VarVerdict evPlay{};
    evPlay.action = VarAction::ConvertToPenalty;
    evPlay.sourceEventId = 88;
    evPlay.offenderSlot = 15;
    evPlay.benefitedTeam = 0;
    evPlay.origCard = CardKind::Yellow;
    evPlay.targetCard = CardKind::Red;
    st.AddToVerdictPool(evPlay);

    VerdictPool consumed{};
    EXPECT_TRUE(st.ConsumeVerdictPool(consumed), "ConsumeVerdictPool must succeed");
    EXPECT_TRUE(!st.HasArmedVerdict(), "ConsumeVerdictPool must disarm pool atomically");
    EXPECT_TRUE(consumed.hasPlayDecision && consumed.playDecision.action == VarAction::ConvertToPenalty,
                "Consumed pool must carry ConvertToPenalty");
    EXPECT_TRUE(consumed.FindCardForSlot(15) == nullptr,
                "Non-direct card tweak on same sourceEventId 88 must be superseded by ConvertToPenalty");
    EXPECT_TRUE(consumed.FindCardForSlot(10) != nullptr,
                "Direct card removal on slot 10 must be preserved in atomic consumption");
    EXPECT_TRUE(!st.ConsumeVerdictPool(consumed), "Second ConsumeVerdictPool must return false");

    // 6. Same official event edited twice with different players: only the latest edit survives
    {
        VarVerdict e1{};
        e1.action = VarAction::EditCard;
        e1.sourceEventId = 42;
        e1.origOffenderSlot = 4;
        e1.offenderSlot = 5;
        e1.origCard = CardKind::Yellow;
        e1.targetCard = CardKind::Yellow;
        st.AddToVerdictPool(e1);
        VarVerdict e2 = e1;
        e2.offenderSlot = 6;
        st.AddToVerdictPool(e2);
        const VerdictPool p6 = st.PeekVerdictPool();
        EXPECT_EQ(p6.CardCount(), static_cast<size_t>(1), "One event edited twice must keep a single card entry");
        EXPECT_TRUE(p6.FindCardForSlot(6) != nullptr && p6.FindCardForSlot(5) == nullptr,
                    "The latest player chosen for the event must replace the previous one");
        st.CancelVerdict();
    }

    // 7. Two different events of the same player: both edits are kept (no silent overwrite)
    {
        VarVerdict a{};
        a.action = VarAction::EditCard;
        a.sourceEventId = 10;
        a.offenderSlot = 7;
        a.origCard = CardKind::Simple;
        a.targetCard = CardKind::Yellow;
        st.AddToVerdictPool(a);
        VarVerdict b = a;
        b.sourceEventId = 11;
        st.AddToVerdictPool(b);
        const VerdictPool p7 = st.PeekVerdictPool();
        EXPECT_EQ(p7.CardCount(), static_cast<size_t>(2), "Edits of two different events must both stay in the pool");
        // remove one by its unique sequence
        const CardPoolEntry* latest = p7.FindCardForSlot(7);
        EXPECT_TRUE(latest != nullptr, "Latest entry for slot 7 must exist");
        st.RemoveCardPoolEntry(latest->verdict.sequence);
        EXPECT_EQ(st.PeekVerdictPool().CardCount(), static_cast<size_t>(1), "RemoveCardPoolEntry must remove exactly one entry");
        st.CancelVerdict();
    }

    // 9. Event scoping: latest official foul of a player, and removal of one event's card edits
    {
        VarEvent official{};
        official.type = VarEventType::Foul;
        official.offenderSlot = 9;
        official.matchTick = 1000;
        const uint32_t idOld = st.PushEvent(official);
        official.matchTick = 5000; // a later, separate foul (same-incident merge window is 30 ticks)
        const uint32_t idNew = st.PushEvent(official);
        VarEvent byVar = official;
        byVar.matchTick = 9000;
        byVar.reviewedByVar = true;
        st.PushEvent(byVar);
        EXPECT_EQ(st.FindLatestOfficialFoulEventId(9), idNew, "Latest official foul must ignore VAR-made events");
        EXPECT_TRUE(idOld != idNew, "Event ids must be unique");
        EXPECT_EQ(st.FindLatestOfficialFoulEventId(3), 0U, "No official foul for slot 3");

        VarVerdict ev1{};
        ev1.action = VarAction::EditCard;
        ev1.sourceEventId = idOld;
        ev1.offenderSlot = 9;
        ev1.origCard = CardKind::Simple;
        ev1.targetCard = CardKind::Yellow;
        st.AddToVerdictPool(ev1);
        VarVerdict direct9{};
        direct9.action = VarAction::EditCard;
        direct9.directCardEdit = true;
        direct9.offenderSlot = 9;
        direct9.directYellows = 1;
        st.AddToVerdictPool(direct9);
        st.RemoveCardEntriesForEvent(idOld);
        const VerdictPool p9 = st.PeekVerdictPool();
        EXPECT_EQ(p9.CardCount(), static_cast<size_t>(1), "Only the event edit is removed");
        EXPECT_TRUE(p9.FindCardForSlot(9) && p9.FindCardForSlot(9)->verdict.directCardEdit, "Direct edit must survive");
        st.CancelVerdict();
    }

    // 10. Incident classification follows the engine's own signals
    {
        PitchDims d{};
        const float sides[2] = { -1.0f, +1.0f };
        PlaySnapshot s{};
        s.time60Hz = 100.0f;
        std::memset(s.playerCauser, -1, sizeof(s.playerCauser));
        for (int i = 0; i < 22; ++i) {
            s.playerOnPitch[i] = 1;
            s.playerPos[i] = Vec3{ -30.0f + 2.5f * static_cast<float>(i), 0.0f, 20.0f };
        }
        s.playerPos[3]  = Vec3{ 0.0f, 0.0f, 0.0f };
        s.playerPos[15] = Vec3{ 0.8f, 0.0f, 0.3f };
        s.ballOwnerSlot = 3;
        s.recentBallOwnerSlot = 3;

        // a) standing tackle (13) and nobody goes down: a challenge, never a near foul, never "carrinho"
        s.playerActionId[15] = PlayerActionId::Tackle;
        DetectedIncident r{};
        EXPECT_TRUE(VarState::DetectPhysicalIncident(s, d, sides, r), "Standing tackle on the carrier is a challenge");
        EXPECT_EQ(r.kind, IncidentKind::StandTackle, "Action 13 is a standing tackle, not a slide");
        EXPECT_TRUE(!r.nearFoul, "No contact reaction: not a near foul");

        // b) the carrier falls (17) and the engine names the tackler as causer: near foul
        s.playerActionId[3] = PlayerActionId::FallDown;
        s.playerCauser[3] = 15;
        EXPECT_TRUE(VarState::DetectPhysicalIncident(s, d, sides, r), "Contact with fall must be detected");
        EXPECT_TRUE(r.nearFoul && r.offenderSlot == 15 && r.victimSlot == 3, "Causer from PlayerAction + 0x3D is the offender");

        // c) dive (15): simulation, the carrier is the "victim" of nothing
        s.playerActionId[3] = PlayerActionId::Dive;
        s.playerActionId[15] = 0;
        EXPECT_TRUE(VarState::DetectPhysicalIncident(s, d, sides, r), "Dive must be flagged");
        EXPECT_EQ(r.kind, IncidentKind::Dive, "Action 15 is a simulation");
        EXPECT_TRUE(!r.nearFoul, "A dive is never a near foul");

        // d) goalkeeper action next to an attacker: no incident
        s.playerActionId[3] = 0;
        s.playerActionId[15] = 56; // keeper catch
        EXPECT_TRUE(!VarState::DetectPhysicalIncident(s, d, sides, r), "Goalkeeper actions never create incidents");
    }

    // 8. UTF-8 safe truncation never leaves half a multi-byte character
    {
        char buf[7];
        std::snprintf(buf, sizeof(buf), "%s", "abcde\xC3\xA7z"); // 6 bytes fit: "abcde" + lead byte of "ç"
        Utf8TrimIncompleteTail(buf);
        EXPECT_TRUE(std::strcmp(buf, "abcde") == 0, "Truncated lead byte must be removed");
        char ok[8] = "ab\xC3\xA7";
        Utf8TrimIncompleteTail(ok);
        EXPECT_TRUE(std::strcmp(ok, "ab\xC3\xA7") == 0, "Complete sequences must be kept");
    }

    MarkPass("TestCompositeVerdictPoolAndRcuOffsets");
}

// ---------------------------------------------------------------------------
// v4.4.1: atomic release of engine heap segments (post-match crash at PES2021.exe+0x3FE31D)
// ---------------------------------------------------------------------------
struct FakeRun { uintptr_t base; uintptr_t allocBase; size_t size; uint32_t state; };
struct FakeMap { std::vector<FakeRun> runs; };

bool FakeQuery(uintptr_t addr, HeapRegionInfo& out, void* ctx) {
    const auto* m = static_cast<const FakeMap*>(ctx);
    for (const auto& r : m->runs) {
        if (addr >= r.base && addr < r.base + r.size) {
            out.base = r.base; out.allocBase = r.allocBase; out.size = r.size; out.state = r.state;
            return true;
        }
    }
    out = HeapRegionInfo{};
    out.base = addr; out.allocBase = 0; out.size = 0x1000; out.state = MEM_FREE;
    return true;
}

void TestHeapGuardAtomicSegmentRelease() {
    // Crash dump layout: one 0x50000 allocation with a PAGE_EXECUTE_READWRITE page in the middle
    FakeMap split{{ { 0x118550000, 0x118550000, 0x16000, MEM_COMMIT },
                    { 0x118566000, 0x118550000, 0x1000,  MEM_COMMIT },
                    { 0x118567000, 0x118550000, 0x39000, MEM_COMMIT } }};
    HeapSegmentPlan p = PlanHeapSegmentRelease(0x118550000, 0x50000, &FakeQuery, &split);
    EXPECT_TRUE(p.ok, "Allocation with mixed protection must be releasable as one allocation");
    EXPECT_EQ(p.allocations, static_cast<size_t>(1), "Mixed protection: one allocation");
    EXPECT_EQ(p.splitAllocations, static_cast<size_t>(1), "Mixed protection must be reported");

    // dlmalloc merged segment made of three adjacent 0x10000 allocations (0x100A60000 in the dump)
    FakeMap merged{{ { 0x100A60000, 0x100A60000, 0x10000, MEM_COMMIT },
                     { 0x100A70000, 0x100A70000, 0x10000, MEM_COMMIT },
                     { 0x100A80000, 0x100A80000, 0x10000, MEM_COMMIT } }};
    p = PlanHeapSegmentRelease(0x100A60000, 0x30000, &FakeQuery, &merged);
    EXPECT_TRUE(p.ok && p.allocations == 3 && p.splitAllocations == 0, "Merged segment of 3 allocations");

    // Range that ends inside an allocation: refused before anything is freed
    p = PlanHeapSegmentRelease(0x100A60000, 0x18000, &FakeQuery, &merged);
    EXPECT_TRUE(!p.ok, "Range ending inside an allocation must be refused");

    // Range that does not start an allocation, or contains reserved or free pages: refused
    p = PlanHeapSegmentRelease(0x100A68000, 0x8000, &FakeQuery, &merged);
    EXPECT_TRUE(!p.ok, "Range not starting an allocation must be refused");
    FakeMap reserved{{ { 0x200000000, 0x200000000, 0x10000, MEM_COMMIT },
                       { 0x200010000, 0x200000000, 0x10000, MEM_RESERVE } }};
    p = PlanHeapSegmentRelease(0x200000000, 0x20000, &FakeQuery, &reserved);
    EXPECT_TRUE(!p.ok, "Partly reserved allocation must be refused");
    p = PlanHeapSegmentRelease(0x100A60000, 0x40000, &FakeQuery, &merged);
    EXPECT_TRUE(!p.ok, "Range running into free memory must be refused");

    // Live: the native routine frees the whole allocation and then fails on the next run. The atomic one
    // releases it completely and reports success.
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const size_t page = si.dwPageSize;
    auto* mem = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x50000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    EXPECT_TRUE(mem != nullptr, "VirtualAlloc failed");
    DWORD oldProt = 0;
    EXPECT_TRUE(VirtualProtect(mem + 0x16000, page, PAGE_EXECUTE_READWRITE, &oldProt), "VirtualProtect failed");
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery(mem, &mbi, sizeof(mbi));
    EXPECT_TRUE(mbi.RegionSize < 0x50000, "Setup: first run must be shorter than the allocation");
    EXPECT_EQ(HeapGuard::ReleaseSegmentAtomic(mem, 0x50000), 0, "Mixed protection allocation must be released");
    VirtualQuery(mem, &mbi, sizeof(mbi));
    EXPECT_EQ(mbi.State, static_cast<DWORD>(MEM_FREE), "Allocation must be gone after release");

    // Live: a range that stops inside the allocation is refused and stays fully usable
    mem = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x20000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    EXPECT_TRUE(mem != nullptr, "VirtualAlloc failed");
    EXPECT_EQ(HeapGuard::ReleaseSegmentAtomic(mem, 0x10000), -1, "Partial range must be refused");
    mem[0x1FFFF] = 0x5A; // still mapped and writable: this write is what crashed the game
    EXPECT_EQ(mem[0x1FFFF], 0x5A, "Refused range must stay mapped");
    VirtualQuery(mem, &mbi, sizeof(mbi));
    EXPECT_EQ(mbi.State, static_cast<DWORD>(MEM_COMMIT), "Refused range must stay committed");
    VirtualFree(mem, 0, MEM_RELEASE);

    EXPECT_EQ(HeapGuard::ReleaseSegmentAtomic(nullptr, 0), 0, "Zero size is a native no-op success");
    // Refused ranges are remembered: dlmalloc retries the same segment on every trim pass
    {
        HeapGuard::RefusedRangeCache cache;
        EXPECT_TRUE(!cache.Contains(0x1711D0000ULL, 0x60000), "Empty cache must not contain anything");
        cache.Add(0x1711D0000ULL, 0x60000);
        cache.Add(0x1711D0000ULL, 0x60000);
        EXPECT_EQ(cache.Count(), static_cast<size_t>(1), "Duplicate ranges are stored once");
        EXPECT_TRUE(cache.Contains(0x1711D0000ULL, 0x60000), "Refused range must be found");
        EXPECT_TRUE(!cache.Contains(0x1711D0000ULL, 0x70000), "Same base with another size is another range");
        for (size_t i = 0; i < HeapGuard::RefusedRangeCache::kCapacity; ++i) cache.Add(0x200000000ULL + i * 0x10000, 0x10000);
        EXPECT_EQ(cache.Count(), HeapGuard::RefusedRangeCache::kCapacity, "Cache is bounded");
        EXPECT_TRUE(!cache.Contains(0x1711D0000ULL, 0x60000), "Oldest entry is replaced when full");
        cache.Clear();
        EXPECT_EQ(cache.Count(), static_cast<size_t>(0), "Clear forgets every range");
        cache.Add(0x500000ULL, 0x10000, 10000);
        EXPECT_TRUE(cache.ContainsFresh(0x500000ULL, 0x10000, 11999), "Refusal holds within its time to live");
        EXPECT_TRUE(!cache.ContainsFresh(0x500000ULL, 0x10000, 12000), "Refusal expires so the range is checked again");
        cache.Add(0x500000ULL, 0x10000, 12000);
        EXPECT_TRUE(cache.ContainsFresh(0x500000ULL, 0x10000, 13000) && cache.Count() == 1, "Refused again: refreshed, not duplicated");
        cache.Clear();
        cache.Add(0x100000ULL, 0x10000);
        cache.Add(0x200000ULL, 0x10000);
        cache.Add(0x300000ULL, 0x10000);
        cache.RemoveOverlapping(0x1F8000ULL, 0x10000); // overlaps only the middle range
        EXPECT_TRUE(cache.Count() == 2 && cache.Contains(0x100000ULL, 0x10000) && cache.Contains(0x300000ULL, 0x10000) &&
                    !cache.Contains(0x200000ULL, 0x10000), "Released range must forget only overlapping refusals");
        cache.RemoveOverlapping(0x110000ULL, 0x1F0000); // touches neither (half-open ranges)
        EXPECT_EQ(cache.Count(), static_cast<size_t>(2), "Adjacent ranges do not overlap");
    }
    // A real refused range: second call comes from the cache, both refuse, counters move
    {
        const HeapGuard::Stats before = HeapGuard::Totals();
        HeapGuard::ClearRefusedCache();
        void* blk = VirtualAlloc(nullptr, 0x20000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        EXPECT_TRUE(blk != nullptr, "VirtualAlloc for the cache test");
        if (blk) {
            // Asking for half of an allocation can never be released as a whole
            EXPECT_EQ(HeapGuard::ReleaseSegmentAtomic(blk, 0x10000), -1, "Partial allocation must be refused");
            EXPECT_EQ(HeapGuard::ReleaseSegmentAtomic(blk, 0x10000), -1, "Cached refusal must still refuse");
            const HeapGuard::Stats st = HeapGuard::Delta(HeapGuard::Totals(), before);
            EXPECT_TRUE(st.calls == 2 && st.refused == 1 && st.cacheHits == 1, "Second refusal must come from the cache");
            HeapGuard::ClearRefusedCache();
            EXPECT_EQ(HeapGuard::ReleaseSegmentAtomic(blk, 0x20000), 0, "Whole allocation must still be released");
        }
    }
    MarkPass("TestHeapGuardAtomicSegmentRelease");
}

// ---------------------------------------------------------------------------
// v4.4.2: final deep review regressions
// ---------------------------------------------------------------------------
void TestFinalReviewRegressions() {
    auto& st = VarState::Instance();
    st.ResetMatch();

    // A card edit on the event of a pending play decision merges into it (original card reverted once)
    VarVerdict play{};
    play.action = VarAction::ConvertToPenalty;
    play.sourceEventId = 7;
    play.offenderSlot = 3;
    play.origOffenderSlot = 3;
    play.origCard = CardKind::Yellow;
    play.targetCard = CardKind::Red;
    st.AddToVerdictPool(play);
    VarVerdict edit{};
    edit.action = VarAction::EditCard;
    edit.sourceEventId = 7;
    edit.offenderSlot = 4;
    edit.offenderRosterIdx = 12;
    edit.origCard = CardKind::Yellow;
    edit.targetCard = CardKind::None;
    st.AddToVerdictPool(edit);
    VerdictPool pool = st.PeekVerdictPool();
    EXPECT_TRUE(pool.hasPlayDecision, "Play decision must stay in the pool");
    EXPECT_EQ(pool.CardCount(), static_cast<size_t>(0), "Edit of the same event must not be a second entry");
    EXPECT_TRUE(pool.playDecision.targetCard == CardKind::None, "Edit must change the play's card");
    EXPECT_EQ(pool.playDecision.offenderSlot, 4, "Edit must move the play's offender");
    EXPECT_EQ(static_cast<int>(pool.playDecision.offenderRosterIdx), 12, "Edit must carry the player identity");

    EXPECT_EQ(pool.playDecision.benefitedTeam, 1, "Kick goes to the opponent of the new offender");
    const uint32_t playOrderBefore = pool.playOrder;

    // An edit on another event stays separate
    edit.sourceEventId = 9;
    st.AddToVerdictPool(edit);
    EXPECT_EQ(st.PeekVerdictPool().CardCount(), static_cast<size_t>(1), "Edit of another event is its own entry");
    EXPECT_EQ(st.PeekVerdictPool().playOrder, playOrderBefore, "Merge must not reorder the play's revocation");
    st.ResetMatch();

    // A restart carries no foul: an edit of its event stays a card entry (the card is not lost)
    VarVerdict corner{};
    corner.action = VarAction::RestartCorner;
    corner.sourceEventId = 7;
    corner.benefitedTeam = 1;
    st.AddToVerdictPool(corner);
    edit.sourceEventId = 7;
    edit.targetCard = CardKind::Red;
    st.AddToVerdictPool(edit);
    EXPECT_EQ(st.PeekVerdictPool().CardCount(), static_cast<size_t>(1), "Edit on a restart's event stays separate");
    st.ResetMatch();

    // Card counts follow the VAR's card change (timeline no longer shows a removed red)
    VarEvent ev{};
    ev.type = VarEventType::Foul;
    ev.offenderSlot = 3;
    ev.cardKind = CardKind::Red;
    ev.offenderRedsAfter = 1;
    ev.matchTick = 100;
    const uint32_t id = st.PushEvent(ev);
    VarVerdict toYellow{};
    toYellow.action = VarAction::EditCard;
    toYellow.sourceEventId = id;
    toYellow.offenderSlot = 3;
    toYellow.origCard = CardKind::Red;
    toYellow.targetCard = CardKind::Yellow;
    st.UpdateEventAfterVerdict(toYellow);
    auto evs = st.GetRecentEvents(8);
    EXPECT_TRUE(!evs.empty() && evs.front().id == id, "Event must exist");
    EXPECT_EQ(static_cast<int>(evs.front().offenderRedsAfter), 0, "Red removed: no red count");
    EXPECT_EQ(static_cast<int>(evs.front().offenderYellowsAfter), 1, "Yellow given: one yellow");
    VarVerdict toNone = toYellow;
    toNone.origCard = CardKind::Yellow;
    toNone.targetCard = CardKind::None;
    st.UpdateEventAfterVerdict(toNone);
    evs = st.GetRecentEvents(8);
    EXPECT_EQ(static_cast<int>(evs.front().offenderYellowsAfter), 0, "Yellow removed: no yellow count");
    EXPECT_EQ(static_cast<int>(evs.front().offenderRedsAfter), 0, "No card: no red count");
    st.ResetMatch();

    // Live foul links only to the same stoppage
    VarEvent a{}, b{};
    a.matchPhase = b.matchPhase = 1;
    a.time60Hz = 1000.0f;
    b.time60Hz = 1600.0f;
    EXPECT_TRUE(VarHudController::IsSameStoppage(a, b), "10 game seconds apart is the same stoppage");
    b.time60Hz = 1000.0f + 60.0f * 60.0f * 20.0f;
    EXPECT_TRUE(!VarHudController::IsSameStoppage(a, b), "20 game minutes apart is another foul");
    b.time60Hz = 1000.0f;
    b.matchPhase = 2;
    EXPECT_TRUE(!VarHudController::IsSameStoppage(a, b), "Other half is another foul");

    // Cancel of the stopped-for foul + a foul marked on an earlier play coexist in one pool, in either order
    for (int order = 0; order < 2; ++order) {
        st.ResetMatch();
        VarVerdict cancel{};
        cancel.action = VarAction::OverturnCancel;
        cancel.sourceEventId = 21;
        cancel.offenderSlot = 6;
        cancel.origOffenderSlot = 6;
        cancel.origCard = CardKind::Yellow;
        cancel.targetCard = CardKind::None;
        cancel.targetsLiveSetPlay = true;
        VarVerdict earlier{};
        earlier.action = VarAction::RetroactiveFreeKick;
        earlier.offenderSlot = 14;
        earlier.benefitedTeam = 0;
        earlier.targetCard = CardKind::Simple;
        if (order == 0) { st.AddToVerdictPool(cancel); st.AddToVerdictPool(earlier); }
        else            { st.AddToVerdictPool(earlier); st.AddToVerdictPool(cancel); }
        const VerdictPool cp = st.PeekVerdictPool();
        EXPECT_TRUE(cp.hasPlayDecision && cp.playDecision.action == VarAction::RetroactiveFreeKick,
                    "The earlier foul is the play (its set play replaces the current one)");
        EXPECT_EQ(cp.CardCount(), static_cast<size_t>(1), "The cancel stays as a card / ledger entry");
        const CardPoolEntry* ce = nullptr;
        for (const auto& e : cp.cardEntries) if (e.active) ce = &e;
        EXPECT_TRUE(ce && ce->verdict.action == VarAction::OverturnCancel && !ce->verdict.targetsLiveSetPlay,
                    "The kept cancel must not restart anything by itself");
    }
    st.ResetMatch();

    // Live cancel already in the pool + an edit of event 30 + a conversion of event 30: the edit is folded
    {
        st.ResetMatch();
        VarVerdict cancel{};
        cancel.action = VarAction::OverturnCancel;
        cancel.sourceEventId = 21;
        cancel.offenderSlot = 6;
        cancel.targetsLiveSetPlay = true;
        st.AddToVerdictPool(cancel);
        VarVerdict e30{};
        e30.action = VarAction::EditCard;
        e30.sourceEventId = 30;
        e30.offenderSlot = 2;
        e30.origCard = CardKind::Yellow;
        e30.targetCard = CardKind::Red;
        st.AddToVerdictPool(e30);
        VarVerdict conv{};
        conv.action = VarAction::ConvertToPenalty;
        conv.sourceEventId = 30;
        conv.offenderSlot = 2;
        conv.origCard = CardKind::Yellow;
        conv.targetCard = CardKind::Red;
        st.AddToVerdictPool(conv);
        const VerdictPool cp = st.PeekVerdictPool();
        EXPECT_TRUE(cp.hasPlayDecision && cp.playDecision.sourceEventId == 30, "Conversion is the play");
        int edits30 = 0, cancels = 0;
        for (const auto& e : cp.cardEntries) {
            if (!e.active) continue;
            if (e.verdict.sourceEventId == 30) ++edits30;
            if (e.verdict.action == VarAction::OverturnCancel) ++cancels;
        }
        EXPECT_EQ(edits30, 0, "Edit of the converted event must be folded (card reverted once)");
        EXPECT_EQ(cancels, 1, "Cancel stays as its own entry");
        st.ResetMatch();
    }

    // Direct edits are keyed by slot AND player: the substitute in the same slot is another entry
    VarVerdict d1{};
    d1.action = VarAction::EditCard;
    d1.directCardEdit = true;
    d1.offenderSlot = 5;
    d1.offenderRosterIdx = 4;
    d1.directYellows = 1;
    VarVerdict d2 = d1;
    d2.offenderRosterIdx = 17;
    st.AddToVerdictPool(d1);
    st.AddToVerdictPool(d2);
    EXPECT_EQ(st.PeekVerdictPool().CardCount(), static_cast<size_t>(2), "Two players of one slot are two direct edits");
    st.AddToVerdictPool(d2);
    EXPECT_EQ(st.PeekVerdictPool().CardCount(), static_cast<size_t>(2), "Same player again replaces his own edit");
    st.ResetMatch();

    // Real counts read back from the game win over the estimate
    VarEvent ev2{};
    ev2.type = VarEventType::Foul;
    ev2.offenderSlot = 8;
    ev2.cardKind = CardKind::Simple;
    ev2.matchTick = 500;
    const uint32_t id2 = st.PushEvent(ev2);
    VarVerdict moved{};
    moved.action = VarAction::EditCard;
    moved.sourceEventId = id2;
    moved.offenderSlot = 9;
    moved.targetCard = CardKind::Yellow;
    moved.ledgerYellows = 2;
    moved.ledgerReds = 1;
    st.UpdateEventAfterVerdict(moved);
    evs = st.GetRecentEvents(8);
    EXPECT_EQ(static_cast<int>(evs.front().offenderYellowsAfter), 2, "Ledger yellows from the game");
    EXPECT_EQ(static_cast<int>(evs.front().offenderRedsAfter), 1, "Second yellow shows as sent off");
    st.ResetMatch();

    // A play decision made in an earlier stoppage is dropped; its cards and old-call overturns stay
    {
        st.ResetMatch();
        VarVerdict fk{};
        fk.action = VarAction::RetroactiveFreeKick;
        fk.offenderSlot = 3;
        fk.armStoppage = 4;
        st.AddToVerdictPool(fk);
        VarVerdict card{};
        card.action = VarAction::EditCard;
        card.directCardEdit = true;
        card.offenderSlot = 7;
        card.directYellows = 1;
        st.AddToVerdictPool(card);
        EXPECT_TRUE(!st.DropStalePlayDecision(4), "Same stoppage keeps the play");
        EXPECT_TRUE(st.DropStalePlayDecision(5), "Later stoppage drops the play");
        const VerdictPool sp = st.PeekVerdictPool();
        EXPECT_TRUE(!sp.hasPlayDecision && sp.CardCount() == 1, "Cards stay after the play is dropped");
        st.ResetMatch();
        VarVerdict oldCancel{};
        oldCancel.action = VarAction::OverturnCancel;
        oldCancel.sourceEventId = 9;
        oldCancel.armStoppage = 1;
        st.AddToVerdictPool(oldCancel);
        EXPECT_TRUE(!st.DropStalePlayDecision(3) && st.PeekVerdictPool().hasPlayDecision,
                    "Overturn of an old call only revokes a card and is kept");
        st.ResetMatch();
    }

    // Arming is refused while the VAR is off
    st.SetEnabled(false);
    EXPECT_TRUE(!st.AddToVerdictPoolEx(d1, nullptr, nullptr), "Refused decision must report false");
    st.AddToVerdictPool(d1);
    EXPECT_TRUE(st.PeekVerdictPool().Empty(), "No decision can be armed with the VAR off");
    st.SetEnabled(true);
    st.ResetMatch();

    // Incident clustering: two duels alternating frame by frame stay two incidents (not one per frame)
    {
        st.ResetMatch();
        for (int t = 0; t < 120; ++t) {
            DetectedIncident a{};
            a.matchPhase = 1;
            a.time60Hz = 1000.0f + static_cast<float>(t);
            a.kind = IncidentKind::BodyCharge;
            a.priority = 60;
            if (t % 2 == 0) { a.offenderSlot = 2; a.victimSlot = 14; } else { a.offenderSlot = 5; a.victimSlot = 18; }
            st.PushDetectedIncident(a);
        }
        const auto mk = st.GetReplayWindowMarkers(1, 900.0f, 1200.0f);
        size_t phys = 0;
        for (const auto& m : mk) if (!m.isOfficialEvent) ++phys;
        EXPECT_EQ(phys, static_cast<size_t>(2), "Alternating duels must give exactly two markers");
        st.ResetMatch();
    }

    // Official foul whistled after an advantage: one marker, at the contact, with the strongest incident's players
    {
        st.ResetMatch();
        DetectedIncident weak{};
        weak.matchPhase = 1; weak.time60Hz = 2000.0f; weak.kind = IncidentKind::BodyCharge; weak.priority = 60;
        weak.offenderSlot = 3; weak.victimSlot = 15;
        st.PushDetectedIncident(weak);
        DetectedIncident slide{};
        slide.matchPhase = 1; slide.time60Hz = 2100.0f; slide.kind = IncidentKind::SlideTackle; slide.priority = 82;
        slide.offenderSlot = 4; slide.victimSlot = 16;
        st.PushDetectedIncident(slide);
        VarEvent ev{};
        ev.type = VarEventType::Foul;
        ev.matchPhase = 1;
        ev.time60Hz = 2340.0f; // whistle 4 s after the slide (advantage played)
        ev.matchTick = 900;
        ev.offenderSlot = 4;
        ev.victimSlot = 16;
        ev.cardKind = CardKind::Simple;
        ev.wasAdvantage = true;
        st.PushEvent(ev);
        const auto mk = st.GetReplayWindowMarkers(1, 1900.0f, 2400.0f);
        const ReplayMarker* om = nullptr;
        size_t slideMarkers = 0;
        for (const auto& m : mk) {
            if (m.isOfficialEvent) om = &m;
            else if (m.offenderSlot == 4) ++slideMarkers;
        }
        EXPECT_TRUE(om != nullptr, "Official marker must exist");
        EXPECT_TRUE(std::fabs(om->time60Hz - 2100.0f) < 0.5f, "Official marker must sit at the contact, not the whistle");
        EXPECT_EQ(slideMarkers, static_cast<size_t>(0), "The slide must not appear twice");
        EXPECT_EQ(st.RecentIncidentOffender(1, 2120.0f, 90.0f), 4, "Advantage team comes from the contact's offender");
        st.ResetMatch();
    }

    // Zone label keeps the sides of the moment it was recorded
    {
        st.ResetMatch();
        VarEvent ev{};
        ev.type = VarEventType::Foul;
        ev.matchPhase = 1;
        ev.time60Hz = 3000.0f;
        ev.matchTick = 1000;
        ev.offenderSlot = 2;
        ev.cardKind = CardKind::Simple;
        ev.boxDistM = -3.0f;
        ev.boxDistValid = true;
        st.PushEvent(ev);
        const auto mk = st.GetReplayWindowMarkers(1, 2900.0f, 3100.0f);
        EXPECT_TRUE(!mk.empty() && std::fabs(mk.front().signedBoxDistM + 3.0f) < 0.01f, "Stored box distance must be used");
        st.ResetMatch();
    }

    // A requeued pool merges with what the user armed meanwhile (nothing is lost)
    {
        st.ResetMatch();
        VarVerdict play{};
        play.action = VarAction::RetroactiveFreeKick;
        play.offenderSlot = 3;
        st.AddToVerdictPool(play);
        const VerdictPool consumedPool = st.PeekVerdictPool();
        st.CancelVerdict();
        VarVerdict card{};
        card.action = VarAction::EditCard;
        card.directCardEdit = true;
        card.offenderSlot = 9;
        card.directYellows = 1;
        st.AddToVerdictPool(card);
        EXPECT_TRUE(st.RequeueVerdictPool(consumedPool, st.MatchGeneration()), "Requeue must merge");
        const VerdictPool mp = st.PeekVerdictPool();
        EXPECT_TRUE(mp.hasPlayDecision && mp.CardCount() == 1, "Both the old play and the new card must be there");
        st.ResetMatch();
    }

    MarkPass("TestFinalReviewRegressions");
}

} // namespace

int main() {
    std::printf("=== PES 2021 VAR Mod v" VAR_MOD_VERSION " - Test Suite ===\n");
    TestAobScannerBasic();
    TestRealPes2021BinaryAobs();
    TestInlineHookEflagsAndRestore();
    TestVtableHook();
    TestPitchGeometry();
    TestVarStateAndSync();
    TestVarHudController();
    TestInlineHook16ByteAtomic();
    TestRound1Regressions();
    TestRound3Regressions();
    TestV2Features();
    TestV3Regressions();
    TestV4EngineAndDataExtraction();
    TestV4BroadcastHudAndRadar();
    TestPerfOverhaulEquivalence();
    TestSnapshotInterpolation();
    TestSuspectAndCameraFollow();
    TestDecisionReplayWindow();
    TestGoalRecordCodec();
    TestGoalAnnulHud();
    TestContactDetectionRules();
    TestCalledPlayNotSuspect();
    TestMsvcTreeRepair();
    TestUpdateChecker();
    TestEngineBuildsByImage();
    TestEngineGuards();
    TestRemapAndLanguage();
    TestHudRenderSmoke();
    TestCompositeVerdictPoolAndRcuOffsets();
    TestHeapGuardAtomicSegmentRelease();
    TestFinalReviewRegressions();

    std::printf("\nResumo: %d PASS, %d FAIL\n", g_passed, g_failed);
    return (g_failed == 0) ? 0 : 1;
}
