#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace var_mod {

// Self-contained x64 inline hook with near-allocated relay + EFLAGS-preserving trampoline.
// Only supports stolen prologues with no RIP-relative displacements (verified per hook site).
class InlineHook {
public:
    InlineHook() = default;
    ~InlineHook();

    InlineHook(const InlineHook&) = delete;
    InlineHook& operator=(const InlineHook&) = delete;

    bool Install(uintptr_t targetAddr, void* detourFunc, size_t stolenLen);
    bool Uninstall();

    bool IsInstalled() const { return installed_; }
    uintptr_t Target() const { return target_; }

    static size_t MeasureSafeStolenLen(uintptr_t& inOutAddr);

    template <typename Fn>
    Fn Original() const {
        return reinterpret_cast<Fn>(trampoline_);
    }

private:
    static void* AllocNear(uintptr_t targetAddr, size_t size);

    uintptr_t target_ = 0;
    size_t stolenLen_ = 0;
    uint8_t* stubMem_ = nullptr;
    void* trampoline_ = nullptr;
    std::vector<uint8_t> originalBytes_;
    bool installed_ = false;
};

// Atomic vtable slot hook (used for IDXGISwapChain::Present & ResizeBuffers).
class VtableHook {
public:
    VtableHook() = default;
    ~VtableHook();

    VtableHook(const VtableHook&) = delete;
    VtableHook& operator=(const VtableHook&) = delete;

    bool Install(void** vtable, size_t index, void* detourFunc);
    bool Uninstall();

    bool IsInstalled() const { return installed_; }

    template <typename Fn>
    Fn Original() const {
        return reinterpret_cast<Fn>(original_.load(std::memory_order_acquire));
    }

private:
    void** slot_ = nullptr;
    std::atomic<void*> original_{ nullptr };
    bool installed_ = false;
};

// PE Import Address Table hook (supports import by name or by ordinal, e.g. xinput1_3.dll ord 2).
class IatHook {
public:
    IatHook() = default;
    ~IatHook();

    IatHook(const IatHook&) = delete;
    IatHook& operator=(const IatHook&) = delete;

    bool InstallByName(void* moduleBase, const char* dllName, const char* funcName, void* detourFunc);
    bool InstallByOrdinal(void* moduleBase, const char* dllName, uint16_t ordinal, void* detourFunc);
    bool Uninstall();

    bool IsInstalled() const { return installed_; }

    template <typename Fn>
    Fn Original() const {
        return reinterpret_cast<Fn>(original_.load(std::memory_order_acquire));
    }

private:
    bool InstallInternal(void* moduleBase, const char* dllName, const char* funcName, uint16_t ordinal, bool byOrdinal, void* detourFunc);

    void** slot_ = nullptr;
    std::atomic<void*> original_{ nullptr };
    bool installed_ = false;
};

} // namespace var_mod
