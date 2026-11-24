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

    // Scans the .text section of the given loaded PE module (nullptr = main EXE).
    static AobMatchResult ScanModuleText(void* moduleBase, const char* pattern);

    // Fast O(1) hint-first scan (like Sider hook cache): validates pattern at moduleBase + hintRva
    // inside .text first (< 1 us), falling back to full .text scan only on mismatch.
    static AobMatchResult ScanModuleTextWithHint(void* moduleBase, const char* pattern, uintptr_t hintRva);

    // Resolves a 32-bit RIP-relative displacement inside an instruction.
    // instrAddr: VA of instruction start, dispOffset: offset of rel32 inside instruction, instrSize: total length.
    static uintptr_t ResolveRipRelative(uintptr_t instrAddr, size_t dispOffset, size_t instrSize);

    static bool ParsePattern(const char* pattern, std::vector<uint8_t>& bytes, std::vector<uint8_t>& mask);
};

} // namespace var_mod
