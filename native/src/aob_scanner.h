#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace var_mod {

struct AobMatchResult {
    uintptr_t address = 0;
    size_t matchCount = 0;
    bool unique() const { return address != 0 && matchCount == 1; }
};

class AobScanner {
public:
    // Scans a raw memory buffer for an IDA/CE style pattern (e.g. "48 89 ?? 24 20").
    static AobMatchResult ScanBuffer(const uint8_t* data, size_t size, const char* pattern, size_t maxMatches = 2);

    // Scans every executable section of the given loaded PE module (nullptr = main EXE), never by section name
    // (builds of the same engine name their code section differently). Sections above 128 MB are skipped. A
    // match counts only when it is unique across all of them.
    static AobMatchResult ScanModuleText(void* moduleBase, const char* pattern);

    // Fast O(1) hint-first scan: validates the pattern at moduleBase + hintRva (the reference build's address,
    // inside an executable section) first, falling back to the full scan on mismatch (any other build).
    // The hint is used only in the reference build (IsReferenceBuild), where every pattern was proven unique
    // offline: in any other executable the full scan decides. allowEntryJump: the pattern is the entry of a
    // function the mod hooks, which another mod may already have turned into a 5-byte jump (see the .cpp).
    static AobMatchResult ScanModuleTextWithHint(void* moduleBase, const char* pattern, uintptr_t hintRva,
                                                 bool allowEntryJump = false);

    // The Steam v1.7.2.0 executable the hints were taken from (PE timestamp and SizeOfImage)
    static constexpr uint32_t kReferenceTimestamp = 0x62787C6A;
    static constexpr uint32_t kReferenceImageSize = 0x1B48A000;
    static bool IsReferenceBuild(void* moduleBase);

    // Address of the vtable of a class from its MSVC RTTI mangled name (e.g. ".?AVSimMain@Screen@match2D@@"):
    // TypeDescriptor -> CompleteObjectLocator (offset 0) -> vtable. hintRva (reference build) is checked by
    // its own RTTI name first. 0 when not found or ambiguous.
    static uintptr_t FindVtableByRtti(void* moduleBase, const char* typeName, uintptr_t hintRva);

    // True when [address, address + len) lies in a writable, non-executable section of the module: where a
    // global resolved from a `mov rax, [rip+disp]` must be. Any other target is refused.
    static bool IsInWritableSection(void* moduleBase, uintptr_t address, size_t len);

    // Resolves a 32-bit RIP-relative displacement inside an instruction.
    // instrAddr: VA of instruction start, dispOffset: offset of rel32 inside instruction, instrSize: total length.
    static uintptr_t ResolveRipRelative(uintptr_t instrAddr, size_t dispOffset, size_t instrSize);

    static bool ParsePattern(const char* pattern, std::vector<uint8_t>& bytes, std::vector<uint8_t>& mask);
};

} // namespace var_mod
