// Atomic release of the engine heap segments.
//
// PES2021.exe uses dlmalloc for its game heaps (mstate at 0x56F0010 in both crash dumps). Its win32munmap
// (0x1403FC0D0) releases a segment by walking VirtualQuery runs and calling VirtualFree(MEM_RELEASE) on
// each one, and returns -1 as soon as a run does not start its own allocation. VirtualFree(MEM_RELEASE)
// always frees the WHOLE allocation, so when an allocation has pages with a different protection (BMPES Lua
// modules write game memory through Sider and leave single pages PAGE_EXECUTE_READWRITE, including the
// mstate page itself) the first call frees everything, the next VirtualQuery finds free memory and the
// function reports failure. The caller (trim in release_unused_segments, 0x1403FE2C3) then puts the
// "unreleased" chunk back into its bins by writing into memory that no longer exists:
// write access violation at PES2021.exe+0x3FE31D, during the heap trim of the post-match scene change.
//
// The replacement validates the whole range first and frees it allocation by allocation. Either every
// allocation is released (return 0) or nothing is touched (return -1, dlmalloc keeps the chunk).

#include "heap_guard.h"

#include "aob_scanner.h"
#include "inline_hook.h"
#include "logger.h"

#include <windows.h>

#include <atomic>

namespace var_mod {

namespace {

// Extent of the allocation that starts at `p`, limited to `remaining` bytes. 0 when `p` is not the start of
// a fully committed allocation that fits in the range.
size_t AllocationExtent(uintptr_t p, size_t remaining, HeapRegionQueryFn query, void* ctx, size_t* runs) {
    HeapRegionInfo r{};
    if (!query(p, r, ctx) || r.base != p || r.allocBase != p || r.size == 0) return 0;
    size_t extent = 0;
    size_t count = 0;
    uintptr_t q = p;
    for (;;) {
        if (r.state != MEM_COMMIT) return 0;
        extent += r.size;
        ++count;
        if (extent > remaining) return 0; // allocation extends past the segment
        q = p + extent;
        if (extent == remaining) break;
        HeapRegionInfo next{};
        if (!query(q, next, ctx)) return 0;
        if (next.allocBase != p) break; // next allocation (or free memory) begins here
        if (next.base != q || next.size == 0) return 0;
        r = next;
    }
    if (runs) *runs = count;
    return extent;
}

bool QueryLive(uintptr_t addr, HeapRegionInfo& out, void*) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<const void*>(addr), &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    out.base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    out.allocBase = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
    out.size = mbi.RegionSize;
    out.state = mbi.State;
    return true;
}

constexpr uint64_t kMaxLoggedEvents = 16;

InlineHook g_hookMunmap;
std::atomic<bool> g_installed{false};
std::atomic<uint64_t> g_repaired{0};
std::atomic<uint64_t> g_refused{0};

int ReleaseSegmentAtomicImpl(void* ptr, size_t size) {
    if (size == 0) return 0; // native: nothing to do, success
    const uintptr_t base = reinterpret_cast<uintptr_t>(ptr);
    const HeapSegmentPlan plan = PlanHeapSegmentRelease(base, size, &QueryLive, nullptr);
    if (!plan.ok) {
        const uint64_t n = g_refused.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= kMaxLoggedEvents) {
            VAR_LOG_WRN("HeapGuard: segment 0x%llX size 0x%llX is not releasable as a whole, kept mapped",
                        static_cast<unsigned long long>(base), static_cast<unsigned long long>(size));
        }
        return -1;
    }

    size_t freed = 0;
    uintptr_t p = base;
    size_t remaining = size;
    while (remaining > 0) {
        const size_t extent = AllocationExtent(p, remaining, &QueryLive, nullptr, nullptr);
        if (extent == 0 || !VirtualFree(reinterpret_cast<void*>(p), 0, MEM_RELEASE)) {
            // Cannot happen after a successful plan unless another thread changed the range meanwhile.
            // Nothing freed yet: report failure so dlmalloc keeps the chunk. Already partly freed: report
            // success so dlmalloc unlinks it (the rest leaks, instead of a write into freed memory).
            VAR_LOG_ERR("HeapGuard: release of 0x%llX stopped at 0x%llX after %zu allocation(s)",
                        static_cast<unsigned long long>(base), static_cast<unsigned long long>(p), freed);
            return freed == 0 ? -1 : 0;
        }
        ++freed;
        p += extent;
        remaining -= extent;
    }

    if (plan.splitAllocations > 0) {
        const uint64_t n = g_repaired.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= kMaxLoggedEvents) {
            VAR_LOG_INF("HeapGuard: released segment 0x%llX size 0x%llX (%zu allocation(s), %zu with mixed "
                        "page protection; the native release would have crashed)",
                        static_cast<unsigned long long>(base), static_cast<unsigned long long>(size),
                        plan.allocations, plan.splitAllocations);
        }
    }
    return 0;
}

int __fastcall Detour_Win32Munmap(void* ptr, size_t size) {
    return ReleaseSegmentAtomicImpl(ptr, size);
}

} // namespace

HeapSegmentPlan PlanHeapSegmentRelease(uintptr_t base, size_t size, HeapRegionQueryFn query, void* ctx) {
    HeapSegmentPlan plan{};
    if (!query || base == 0 || size == 0) return plan;
    uintptr_t p = base;
    size_t remaining = size;
    while (remaining > 0) {
        size_t runs = 0;
        const size_t extent = AllocationExtent(p, remaining, query, ctx, &runs);
        if (extent == 0) return HeapSegmentPlan{};
        ++plan.allocations;
        if (runs > 1) ++plan.splitAllocations;
        p += extent;
        remaining -= extent;
    }
    plan.ok = true;
    return plan;
}

namespace HeapGuard {

bool Install() {
    if (g_installed.load(std::memory_order_acquire)) return true;
    const AobMatchResult r = AobScanner::ScanModuleTextWithHint(nullptr, HeapGuardAob::Win32Munmap, HeapGuardAob::Win32MunmapHintRva);
    if (!r.unique()) {
        VAR_LOG_ERR("HeapGuard: win32munmap AOB failed (matchCount=%zu), heap trim keeps the native release",
                    r.matchCount);
        return false;
    }
    // First instruction `mov [rsp+8], rbx` is exactly 5 bytes and has no RIP-relative operand.
    if (!g_hookMunmap.Install(r.address, reinterpret_cast<void*>(&Detour_Win32Munmap), 5)) {
        VAR_LOG_ERR("HeapGuard: failed to hook win32munmap at 0x%llX", static_cast<unsigned long long>(r.address));
        return false;
    }
    g_installed.store(true, std::memory_order_release);
    VAR_LOG_INF("HeapGuard: atomic heap segment release installed at 0x%llX",
                static_cast<unsigned long long>(r.address));
    return true;
}

int ReleaseSegmentAtomic(void* ptr, size_t size) { return ReleaseSegmentAtomicImpl(ptr, size); }
bool IsInstalled() { return g_installed.load(std::memory_order_acquire); }
uint64_t RepairedReleases() { return g_repaired.load(std::memory_order_relaxed); }
uint64_t RefusedReleases() { return g_refused.load(std::memory_order_relaxed); }

} // namespace HeapGuard

} // namespace var_mod
