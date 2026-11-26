#pragma once

#include <cstddef>
#include <cstdint>

namespace var_mod {

// One VirtualQuery run (only the fields the segment release needs).
struct HeapRegionInfo {
    uintptr_t base = 0;      // MEMORY_BASIC_INFORMATION::BaseAddress
    uintptr_t allocBase = 0; // MEMORY_BASIC_INFORMATION::AllocationBase
    size_t size = 0;         // MEMORY_BASIC_INFORMATION::RegionSize
    uint32_t state = 0;      // MEMORY_BASIC_INFORMATION::State
};

using HeapRegionQueryFn = bool (*)(uintptr_t addr, HeapRegionInfo& out, void* ctx);

struct HeapSegmentPlan {
    bool ok = false;          // the range is exactly a sequence of whole, fully committed allocations
    size_t allocations = 0;   // number of OS allocations in the range
    size_t splitAllocations = 0; // allocations whose pages report more than one run (mixed protection)
};

// Pure planning step of the engine's segment release (dlmalloc win32munmap, PES2021.exe 0x1403FC0D0).
// Walks [base, base + size) by OS allocation (AllocationBase), not by protection run. No side effects.
HeapSegmentPlan PlanHeapSegmentRelease(uintptr_t base, size_t size, HeapRegionQueryFn query, void* ctx);

// dlmalloc win32munmap(void* ptr, size_t size) in PES2021.exe Steam 1.7.2.0 (0x1403FC0D0): 0 on success, -1 on failure.
namespace HeapGuardAob {
constexpr const char* Win32Munmap =
    "48 89 5C 24 08 57 48 83 EC 50 48 8B FA 48 8B D9 48 85 D2 74 ?? 66 66 66 0F 1F 84 00 00 00 00 00 "
    "41 B8 30 00 00 00";
constexpr uintptr_t Win32MunmapHintRva = 0x03FC0D0;
} // namespace HeapGuardAob

namespace HeapGuard {
// Same contract as win32munmap: releases the whole range (0) or touches nothing (-1).
int ReleaseSegmentAtomic(void* ptr, size_t size);
// Installs the atomic segment release in place of the engine's. Independent from the match hooks:
// it stays active for the whole process and also protects matches where the VAR is disabled.
bool Install();
bool IsInstalled();
uint64_t RepairedReleases(); // releases the native code would have left half done (crash at 0x1403FE31D)
uint64_t RefusedReleases();  // inconsistent ranges kept mapped instead of partially released
} // namespace HeapGuard

} // namespace var_mod
