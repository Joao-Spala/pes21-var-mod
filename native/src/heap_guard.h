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
// The engine trims in a loop (0x1403FBF00: mspace_trim again while it released anything) and every pass runs
// release_unused_segments (0x1403FE140), which retries each free segment it could not release before. A refused
// range is remembered and refused again in O(1) instead of walking its pages each time. Refusing is always safe:
// every caller keeps or leaks the range on -1, none treats it as fatal (0x1403FC717, 0x1403FC77A, 0x1403FD410,
// 0x1403FE2C3, 0x1403FE6D3, 0x1403FE7D8). A later successful release forgets the overlapping cached ranges.
struct Stats {
    uint64_t calls = 0;        // win32munmap calls handled
    uint64_t released = 0;     // ranges released
    uint64_t refused = 0;      // ranges refused after a page walk
    uint64_t cacheHits = 0;    // ranges refused again from the cache (no page walk)
    uint64_t microseconds = 0; // total time spent inside the replacement
};
Stats Totals();              // cumulative since start: each reporter keeps its own previous value
Stats Delta(const Stats& now, const Stats& before);
void ClearRefusedCache();    // forget refused ranges (new match: allocations may have changed)
// Pure cache used by the replacement, exposed for tests
class RefusedRangeCache {
public:
    static constexpr size_t kCapacity = 64;
    static constexpr uint64_t kRefusalTtlMs = 2000;
    bool Contains(uintptr_t base, size_t size) const;
    // Same range refused less than kRefusalTtlMs ago (the trim loop retries within milliseconds)
    bool ContainsFresh(uintptr_t base, size_t size, uint64_t nowMs) const;
    void Add(uintptr_t base, size_t size, uint64_t nowMs = 0); // oldest entry is replaced when full
    void RemoveOverlapping(uintptr_t base, size_t size);
    void Clear();
    size_t Count() const { return count_; }
private:
    uintptr_t base_[kCapacity] = {};
    size_t size_[kCapacity] = {};
    uint64_t addedMs_[kCapacity] = {};
    size_t count_ = 0;
    size_t next_ = 0;
};
} // namespace HeapGuard

} // namespace var_mod
