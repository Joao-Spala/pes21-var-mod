#include "inline_hook.h"
#include "logger.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>

namespace var_mod {

namespace {

bool EqualsIgnoreCase(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (std::tolower(static_cast<unsigned char>(*a)) !=
            std::tolower(static_cast<unsigned char>(*b))) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

bool FitsRel32(uintptr_t fromNextRip, uintptr_t toAddr) {
    const intptr_t diff = static_cast<intptr_t>(toAddr) - static_cast<intptr_t>(fromNextRip);
    return diff >= INT32_MIN && diff <= INT32_MAX;
}

void AtomicWriteCode(uintptr_t addr, const uint8_t* bytes, size_t len) {
    if (len <= 8) {
        uint64_t origQword = 0;
        std::memcpy(&origQword, reinterpret_cast<const void*>(addr), 8);
        uint64_t newQword = origQword;
        std::memcpy(&newQword, bytes, len);
        InterlockedExchange64(reinterpret_cast<volatile LONG64*>(addr), static_cast<LONG64>(newQword));
    } else if (len <= 16 && (addr & 0xFULL) == 0) {
        // 16-byte aligned target (e.g. Judge::FinalizeFrame 0x14050CD70): single atomic cmpxchg16b
        __int128 expected = 0;
        std::memcpy(&expected, reinterpret_cast<const void*>(addr), 16);
        __int128 desired = expected;
        std::memcpy(&desired, bytes, len);
        auto* target128 = reinterpret_cast<volatile __int128*>(addr);
        while (!__sync_bool_compare_and_swap(target128, expected, desired)) {
            std::memcpy(&expected, reinterpret_cast<const void*>(addr), 16);
            desired = expected;
            std::memcpy(&desired, bytes, len);
        }
    } else {
        std::memcpy(reinterpret_cast<void*>(addr), bytes, len);
    }
}

// Shared 4KB near-page slab pool: packs up to 64 inline-hook trampolines (64B each) into ONE
// VirtualAlloc page within +/- 1.5 GB of PES2021.exe, eliminating 8/9 VirtualQuery walks!
struct NearPageSlab {
    uint8_t* base = nullptr;
    uint64_t usedMask = 0; // 1 bit per 64-byte slot (64 slots = 4096 bytes)
};

std::mutex g_slabMutex;
NearPageSlab g_slabs[8] = {};

uint8_t* AllocSlotFromPool(uintptr_t targetAddr, void* (*allocPageFn)(uintptr_t, size_t)) {
    std::lock_guard<std::mutex> lock(g_slabMutex);
    constexpr uintptr_t kMaxDist = 0x58000000ULL;

    for (auto& s : g_slabs) {
        if (!s.base || s.usedMask == ~0ULL) continue;
        const uintptr_t p = reinterpret_cast<uintptr_t>(s.base);
        const uintptr_t dist = (p > targetAddr) ? (p - targetAddr) : (targetAddr - p);
        if (dist > kMaxDist) continue;

        for (unsigned bit = 0; bit < 64; ++bit) {
            if ((s.usedMask & (1ULL << bit)) == 0) {
                s.usedMask |= (1ULL << bit);
                return s.base + static_cast<size_t>(bit) * 64;
            }
        }
    }

    uint8_t* newPage = static_cast<uint8_t*>(allocPageFn(targetAddr, 4096));
    if (!newPage) return nullptr;

    for (auto& s : g_slabs) {
        if (!s.base) {
            s.base = newPage;
            s.usedMask = 1ULL;
            return newPage;
        }
    }
    return newPage;
}

bool FreeSlotToPool(void* slotPtr) {
    if (!slotPtr) return true;
    std::lock_guard<std::mutex> lock(g_slabMutex);
    auto* p = static_cast<uint8_t*>(slotPtr);
    for (auto& s : g_slabs) {
        if (s.base && p >= s.base && p < s.base + 4096) {
            // Bytes are left intact: a thread still inside the old trampoline finishes safely.
            // AllocSlotFromPool re-fills the slot with 0xCC before reuse.
            const size_t idx = static_cast<size_t>(p - s.base) / 64;
            s.usedMask &= ~(1ULL << idx);
            return true;
        }
    }
    return false;
}

} // namespace

// ---------------- InlineHook ----------------

size_t InlineHook::MeasureSafeStolenLen(uintptr_t& inOutAddr) {
    if (!inOutAddr) return 0;

    const auto* code = reinterpret_cast<const uint8_t*>(inOutAddr);

    // If another overlay (e.g. Steam GameOverlayRenderer64) already placed a 5-byte E9 rel32 jump
    // at entry, InlineHook::Install can cleanly chain/relocate that exact 5-byte jump!
    if (code[0] == 0xE9) {
        return 5;
    }

    size_t total = 0;

    // Decode standard x64 function prologue instructions until total >= 5 (no RIP-relative!)
    while (total < 5 && total < 16) {
        const uint8_t* p = code + total;
        size_t insLen = 0;

        // Single-byte NOP / push r64 (0x50..0x57)
        if (p[0] == 0x90 || (p[0] >= 0x50 && p[0] <= 0x57)) {
            insLen = 1;
        }
        // REX (0x40..0x4F) + push r64 (0x50..0x57)
        else if ((p[0] & 0xF0) == 0x40 && (p[1] >= 0x50 && p[1] <= 0x57)) {
            insLen = 2;
        }
        // sub rsp, imm8 (48 83 EC xx)
        else if (p[0] == 0x48 && p[1] == 0x83 && p[2] == 0xEC) {
            insLen = 4;
        }
        // sub rsp, imm32 (48 81 EC xxxxxxxx)
        else if (p[0] == 0x48 && p[1] == 0x81 && p[2] == 0xEC) {
            insLen = 7;
        }
        // mov reg, reg (REX + 89/8B + ModRM mod=11)
        else if ((p[0] & 0xF0) == 0x40 && (p[1] == 0x89 || p[1] == 0x8B) && (p[2] & 0xC0) == 0xC0) {
            insLen = 3;
        }
        // mov [reg + disp8], reg / mov reg, [reg + disp8] (optional REX + 89/8B + ModRM mod=01)
        else if ((p[0] & 0xF0) == 0x40 && (p[1] == 0x89 || p[1] == 0x8B || p[1] == 0x8D) && (p[2] & 0xC0) == 0x40) {
            const bool hasSib = ((p[2] & 0x07) == 0x04);
            insLen = hasSib ? 5 : 4;
        }
        else if ((p[0] == 0x89 || p[0] == 0x8B) && (p[1] & 0xC0) == 0x40) {
            const bool hasSib = ((p[1] & 0x07) == 0x04);
            insLen = hasSib ? 4 : 3;
        }
        // mov [reg + disp32], reg (REX + 89/8B + ModRM mod=10)
        else if ((p[0] & 0xF0) == 0x40 && (p[1] == 0x89 || p[1] == 0x8B) && (p[2] & 0xC0) == 0x80) {
            const bool hasSib = ((p[2] & 0x07) == 0x04);
            insLen = hasSib ? 8 : 7;
        }
        // xor r32, r32 (33 C0 / 31 C0 etc., ModRM mod=11)
        else if ((p[0] == 0x31 || p[0] == 0x33) && (p[1] & 0xC0) == 0xC0) {
            insLen = 2;
        }
        // REX + xor r32/64, r32/64 (ModRM mod=11)
        else if ((p[0] & 0xF0) == 0x40 && (p[1] == 0x31 || p[1] == 0x33) && (p[2] & 0xC0) == 0xC0) {
            insLen = 3;
        }
        // mov dword/byte ptr [rsp+disp8], imm... not needed, bail safely
        else {
            return 0;
        }

        total += insLen;
    }

    if (total < 5 || total > 16) {
        return 0;
    }
    return total;
}

InlineHook::~InlineHook() {
    Uninstall();
}

void* InlineHook::AllocNear(uintptr_t targetAddr, size_t size) {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;

    // Search within +/- 1.5 GB around targetAddr to guarantee rel32 reachability.
    const uintptr_t maxSpan = 0x60000000ULL;
    const uintptr_t minAddr = std::max<uintptr_t>(
        reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress),
        targetAddr > maxSpan ? (targetAddr - maxSpan) : 0x10000ULL);
    const uintptr_t maxAddr = std::min<uintptr_t>(
        reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress),
        targetAddr + maxSpan);

    auto tryScan = [&](uintptr_t start, uintptr_t end, bool forward) -> void* {
        uintptr_t cur = start;
        while ((forward && cur < end) || (!forward && cur > end)) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<void*>(cur), &mbi, sizeof(mbi)) == 0) {
                break;
            }
            if (mbi.State == MEM_FREE && mbi.RegionSize >= size) {
                uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
                uintptr_t regionEnd  = regionBase + mbi.RegionSize;
                uintptr_t cand = forward
                    ? ((regionBase + gran - 1) & ~(gran - 1))
                    : ((regionEnd - size) & ~(gran - 1));
                if (cand >= regionBase && cand + size <= regionEnd &&
                    cand >= minAddr && cand + size <= maxAddr) {
                    void* mem = VirtualAlloc(
                        reinterpret_cast<void*>(cand),
                        size,
                        MEM_COMMIT | MEM_RESERVE,
                        PAGE_EXECUTE_READWRITE);
                    if (mem) {
                        return mem;
                    }
                }
            }
            if (forward) {
                uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
                if (next <= cur) break;
                cur = next;
            } else {
                uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
                if (base <= end + gran) break;
                cur = base - gran;
            }
        }
        return nullptr;
    };

    if (void* p = tryScan(targetAddr, maxAddr, true)) {
        return p;
    }
    return tryScan(targetAddr > gran ? targetAddr - gran : minAddr, minAddr, false);
}

bool InlineHook::Install(uintptr_t targetAddr, void* detourFunc, size_t stolenLen) {
    if (installed_) return true;
    if (!targetAddr || !detourFunc || stolenLen < 5 || stolenLen > 24) {
        VAR_LOG_ERR("InlineHook::Install invalid args target=0x%llX stolenLen=%zu",
                    static_cast<unsigned long long>(targetAddr), stolenLen);
        return false;
    }

    // Stub memory layout (64 bytes from shared 4KB near-page slab):
    // [0..31]  Trampoline: stolenBytes[0..stolenLen-1] + E9 rel32 (back to target + stolenLen)
    // [32..47] Relay:      FF 25 00 00 00 00 <uint64_t detourAddr>
    uint8_t* stub = AllocSlotFromPool(targetAddr, &InlineHook::AllocNear);
    if (!stub) {
        VAR_LOG_ERR("InlineHook::Install AllocNear failed for 0x%llX",
                    static_cast<unsigned long long>(targetAddr));
        return false;
    }

    std::memset(stub, 0xCC, 64);

    // Save original bytes
    originalBytes_.resize(stolenLen);
    std::memcpy(originalBytes_.data(), reinterpret_cast<const void*>(targetAddr), stolenLen);

    // Build trampoline (preserves all registers + EFLAGS including ZF)
    if (stolenLen == 5 && originalBytes_[0] == 0xE9) {
        // Entry already has a 5-byte E9 rel32 jump (e.g. Steam Overlay):
        // Relocate it cleanly as an absolute indirect jump to the original jump destination!
        int32_t origRel = 0;
        std::memcpy(&origRel, originalBytes_.data() + 1, sizeof(origRel));
        const uint64_t chainedTarget = static_cast<uint64_t>(
            static_cast<intptr_t>(targetAddr + 5) + static_cast<intptr_t>(origRel));
        stub[0] = 0xFF;
        stub[1] = 0x25;
        uint32_t zeroDisp = 0;
        std::memcpy(stub + 2, &zeroDisp, sizeof(zeroDisp));
        std::memcpy(stub + 6, &chainedTarget, sizeof(chainedTarget));
    } else {
        std::memcpy(stub, originalBytes_.data(), stolenLen);
        const uintptr_t trampJmpAddr = reinterpret_cast<uintptr_t>(stub + stolenLen);
        const uintptr_t resumeAddr = targetAddr + stolenLen;
        if (!FitsRel32(trampJmpAddr + 5, resumeAddr)) {
            if (!FreeSlotToPool(stub)) VirtualFree(stub, 0, MEM_RELEASE);
            VAR_LOG_ERR("InlineHook::Install trampoline out of rel32 range");
            return false;
        }
        stub[stolenLen] = 0xE9;
        const int32_t trampRel = static_cast<int32_t>(static_cast<intptr_t>(resumeAddr) - static_cast<intptr_t>(trampJmpAddr + 5));
        std::memcpy(stub + stolenLen + 1, &trampRel, sizeof(trampRel));
    }

    // Build 14-byte absolute relay at stub + 32: jmp qword ptr [rip + 0]; dq detourAddr
    uint8_t* relay = stub + 32;
    const uintptr_t relayAddr = reinterpret_cast<uintptr_t>(relay);
    if (!FitsRel32(targetAddr + 5, relayAddr)) {
        if (!FreeSlotToPool(stub)) VirtualFree(stub, 0, MEM_RELEASE);
        VAR_LOG_ERR("InlineHook::Install relay out of rel32 range");
        return false;
    }
    relay[0] = 0xFF;
    relay[1] = 0x25;
    uint32_t zeroDisp = 0;
    std::memcpy(relay + 2, &zeroDisp, sizeof(zeroDisp));
    uint64_t detourVal = reinterpret_cast<uint64_t>(detourFunc);
    std::memcpy(relay + 6, &detourVal, sizeof(detourVal));

    FlushInstructionCache(GetCurrentProcess(), stub, 64);

    // Patch target with E9 rel32 to relay + NOPs
    uint8_t patch[24];
    std::memset(patch, 0x90, stolenLen);
    patch[0] = 0xE9;
    const int32_t targetRel = static_cast<int32_t>(static_cast<intptr_t>(relayAddr) - static_cast<intptr_t>(targetAddr + 5));
    std::memcpy(patch + 1, &targetRel, sizeof(targetRel));

    DWORD oldProt = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(targetAddr), stolenLen, PAGE_EXECUTE_READWRITE, &oldProt)) {
        if (!FreeSlotToPool(stub)) VirtualFree(stub, 0, MEM_RELEASE);
        VAR_LOG_ERR("InlineHook::Install VirtualProtect failed err=%lu", GetLastError());
        return false;
    }

    // Publish trampoline_ & metadata BEFORE patching code so concurrent threads hitting
    // the detour immediately after AtomicWriteCode NEVER observe trampoline_ == nullptr!
    target_ = targetAddr;
    stolenLen_ = stolenLen;
    stubMem_ = stub;
    trampoline_ = stub;
    installed_ = true;
    MemoryBarrier();

    AtomicWriteCode(targetAddr, patch, stolenLen);

    DWORD tmpProt = 0;
    VirtualProtect(reinterpret_cast<void*>(targetAddr), stolenLen, oldProt, &tmpProt);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(targetAddr), stolenLen);
    return true;
}

bool InlineHook::Uninstall() {
    if (!installed_) return true;

    DWORD oldProt = 0;
    bool restored = false;
    if (VirtualProtect(reinterpret_cast<void*>(target_), stolenLen_, PAGE_EXECUTE_READWRITE, &oldProt)) {
        AtomicWriteCode(target_, originalBytes_.data(), stolenLen_);
        DWORD tmpProt = 0;
        VirtualProtect(reinterpret_cast<void*>(target_), stolenLen_, oldProt, &tmpProt);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(target_), stolenLen_);
        restored = true;
    }

    // If the original bytes could not be restored the target still jumps into the stub:
    // keep the hook fully intact (stub + trampoline) instead of freeing live code.
    if (!restored) {
        VAR_LOG_ERR("InlineHook::Uninstall VirtualProtect failed at 0x%llX err=%lu (hook kept)",
                    static_cast<unsigned long long>(target_), GetLastError());
        return false;
    }

    if (stubMem_) {
        if (!FreeSlotToPool(stubMem_)) {
            VirtualFree(stubMem_, 0, MEM_RELEASE);
        }
        stubMem_ = nullptr;
    }
    trampoline_ = nullptr;
    originalBytes_.clear();
    target_ = 0;
    stolenLen_ = 0;
    installed_ = false;
    return true;
}

// ---------------- VtableHook ----------------

VtableHook::~VtableHook() {
    Uninstall();
}

bool VtableHook::Install(void** vtable, size_t index, void* detourFunc) {
    if (installed_) return true;
    if (!vtable || !detourFunc) return false;

    void** slot = &vtable[index];
    DWORD oldProt = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProt)) {
        VAR_LOG_ERR("VtableHook::Install VirtualProtect failed err=%lu", GetLastError());
        return false;
    }

    // Pre-store original BEFORE swapping slot so concurrent calls never observe nullptr
    original_.store(*slot, std::memory_order_release);
    void* prev = InterlockedExchangePointer(slot, detourFunc);
    original_.store(prev, std::memory_order_release);
    DWORD tmpProt = 0;
    VirtualProtect(slot, sizeof(void*), oldProt, &tmpProt);

    slot_ = slot;
    installed_ = true;
    return true;
}

bool VtableHook::Uninstall() {
    if (!installed_ || !slot_) return true;

    void* orig = original_.load(std::memory_order_acquire);
    DWORD oldProt = 0;
    if (VirtualProtect(slot_, sizeof(void*), PAGE_READWRITE, &oldProt)) {
        InterlockedExchangePointer(slot_, orig);
        DWORD tmpProt = 0;
        VirtualProtect(slot_, sizeof(void*), oldProt, &tmpProt);
    }

    slot_ = nullptr;
    installed_ = false;
    return true;
}

// ---------------- IatHook ----------------

IatHook::~IatHook() {
    Uninstall();
}

bool IatHook::InstallByName(void* moduleBase, const char* dllName, const char* funcName, void* detourFunc) {
    return InstallInternal(moduleBase, dllName, funcName, 0, false, detourFunc);
}

bool IatHook::InstallByOrdinal(void* moduleBase, const char* dllName, uint16_t ordinal, void* detourFunc) {
    return InstallInternal(moduleBase, dllName, nullptr, ordinal, true, detourFunc);
}

bool IatHook::InstallInternal(void* moduleBase, const char* dllName, const char* funcName,
                              uint16_t ordinal, bool byOrdinal, void* detourFunc) {
    if (installed_) return true;
    if (!dllName || !detourFunc) return false;
    if (!byOrdinal && !funcName) return false;

    HMODULE mod = moduleBase ? static_cast<HMODULE>(moduleBase) : GetModuleHandleW(nullptr);
    if (!mod) return false;

    auto* base = reinterpret_cast<uint8_t*>(mod);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size) return false;

    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    for (; desc->Name != 0; ++desc) {
        const char* curDll = reinterpret_cast<const char*>(base + desc->Name);
        if (!EqualsIgnoreCase(curDll, dllName)) continue;

        auto* origThunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(
            base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
        auto* iatThunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->FirstThunk);

        for (; origThunk->u1.AddressOfData != 0; ++origThunk, ++iatThunk) {
            bool matched = false;
            if (byOrdinal) {
                if (IMAGE_SNAP_BY_ORDINAL64(origThunk->u1.Ordinal) &&
                    static_cast<uint16_t>(IMAGE_ORDINAL64(origThunk->u1.Ordinal)) == ordinal) {
                    matched = true;
                }
            } else {
                if (!IMAGE_SNAP_BY_ORDINAL64(origThunk->u1.Ordinal)) {
                    auto* imp = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + origThunk->u1.AddressOfData);
                    if (std::strcmp(reinterpret_cast<const char*>(imp->Name), funcName) == 0) {
                        matched = true;
                    }
                }
            }

            if (matched) {
                void** slot = reinterpret_cast<void**>(&iatThunk->u1.Function);
                DWORD oldProt = 0;
                if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProt)) {
                    VAR_LOG_ERR("IatHook VirtualProtect failed err=%lu", GetLastError());
                    return false;
                }
                // Pre-store original BEFORE swapping IAT slot so concurrent calls never observe nullptr
                original_.store(*slot, std::memory_order_release);
                void* prev = InterlockedExchangePointer(slot, detourFunc);
                original_.store(prev, std::memory_order_release);
                DWORD tmpProt = 0;
                VirtualProtect(slot, sizeof(void*), oldProt, &tmpProt);
                slot_ = slot;
                installed_ = true;
                return true;
            }
        }
    }
    return false;
}

bool IatHook::Uninstall() {
    if (!installed_ || !slot_) return true;

    void* orig = original_.load(std::memory_order_acquire);
    DWORD oldProt = 0;
    if (VirtualProtect(slot_, sizeof(void*), PAGE_READWRITE, &oldProt)) {
        InterlockedExchangePointer(slot_, orig);
        DWORD tmpProt = 0;
        VirtualProtect(slot_, sizeof(void*), oldProt, &tmpProt);
    }

    slot_ = nullptr;
    installed_ = false;
    return true;
}

} // namespace var_mod
