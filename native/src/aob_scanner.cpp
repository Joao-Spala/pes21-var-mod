#include "aob_scanner.h"

#include <windows.h>
#include <cctype>
#include <cstring>

namespace var_mod {

namespace {
int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
} // namespace

bool AobScanner::ParsePattern(const char* pattern, std::vector<uint8_t>& bytes, std::vector<uint8_t>& mask) {
    bytes.clear();
    mask.clear();
    if (!pattern) return false;

    const char* p = pattern;
    while (*p) {
        while (*p && std::isspace(static_cast<unsigned char>(*p))) ++p;
        if (!*p) break;

        if (*p == '?' || *p == '*') {
            bytes.push_back(0);
            mask.push_back(0);
            ++p;
            if (*p == '?' || *p == '*') ++p;
            continue;
        }

        int hi = HexVal(p[0]);
        int lo = p[1] ? HexVal(p[1]) : -1;
        if (hi < 0 || lo < 0) return false;

        bytes.push_back(static_cast<uint8_t>((hi << 4) | lo));
        mask.push_back(1);
        p += 2;
    }
    return !bytes.empty();
}

AobMatchResult AobScanner::ScanBuffer(const uint8_t* data, size_t size, const char* pattern, size_t maxMatches) {
    AobMatchResult res{};
    if (!data || size == 0) return res;

    std::vector<uint8_t> bytes;
    std::vector<uint8_t> mask;
    if (!ParsePattern(pattern, bytes, mask)) return res;

    const size_t patLen = bytes.size();
    if (patLen > size) return res;

    // Pick first fixed (non-wildcard) byte to accelerate scanning via memchr (10-15x faster)
    size_t anchor = patLen;
    for (size_t k = 0; k < patLen; ++k) {
        if (mask[k]) {
            anchor = k;
            break;
        }
    }

    const size_t limit = size - patLen;
    if (anchor == patLen) {
        res.address = reinterpret_cast<uintptr_t>(data);
        res.matchCount = (limit + 1 < maxMatches) ? (limit + 1) : maxMatches;
        return res;
    }

    const uint8_t anchorByte = bytes[anchor];
    size_t i = 0;
    while (i <= limit) {
        const void* hit = std::memchr(data + i + anchor, anchorByte, (limit - i) + 1);
        if (!hit) break;
        const size_t pos = static_cast<size_t>(static_cast<const uint8_t*>(hit) - data) - anchor;

        bool ok = true;
        for (size_t j = 0; j < patLen; ++j) {
            if (mask[j] && data[pos + j] != bytes[j]) {
                ok = false;
                break;
            }
        }
        if (ok) {
            if (res.matchCount == 0) {
                res.address = reinterpret_cast<uintptr_t>(data + pos);
            }
            ++res.matchCount;
            if (res.matchCount >= maxMatches) {
                break;
            }
        }
        i = pos + 1;
    }
    return res;
}

namespace {
bool LocateTextSection(void* moduleBase, const uint8_t*& outModBase, const uint8_t*& outTextStart, size_t& outTextLen) {
    HMODULE mod = moduleBase ? static_cast<HMODULE>(moduleBase) : GetModuleHandleW(nullptr);
    if (!mod) return false;

    const auto* base = reinterpret_cast<const uint8_t*>(mod);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (std::memcmp(sec->Name, ".text", 5) == 0) {
            outModBase = base;
            outTextStart = base + sec->VirtualAddress;
            outTextLen = sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData;
            return true;
        }
    }
    return false;
}
} // namespace

AobMatchResult AobScanner::ScanModuleText(void* moduleBase, const char* pattern) {
    const uint8_t* modBase = nullptr;
    const uint8_t* textStart = nullptr;
    size_t textLen = 0;
    if (!LocateTextSection(moduleBase, modBase, textStart, textLen)) {
        return AobMatchResult{};
    }
    return ScanBuffer(textStart, textLen, pattern, 2);
}

AobMatchResult AobScanner::ScanModuleTextWithHint(void* moduleBase, const char* pattern, uintptr_t hintRva) {
    const uint8_t* modBase = nullptr;
    const uint8_t* textStart = nullptr;
    size_t textLen = 0;
    if (!LocateTextSection(moduleBase, modBase, textStart, textLen)) {
        return AobMatchResult{};
    }

    if (hintRva != 0) {
        std::vector<uint8_t> bytes;
        std::vector<uint8_t> mask;
        if (ParsePattern(pattern, bytes, mask)) {
            const uint8_t* cand = modBase + hintRva;
            const uint8_t* textEnd = textStart + textLen;
            if (cand >= textStart && cand + bytes.size() <= textEnd) {
                bool ok = true;
                for (size_t j = 0; j < bytes.size(); ++j) {
                    if (mask[j] && cand[j] != bytes[j]) {
                        ok = false;
                        break;
                    }
                }
                if (ok) {
                    AobMatchResult res{};
                    res.address = reinterpret_cast<uintptr_t>(cand);
                    res.matchCount = 1;
                    return res;
                }
            }
        }
    }

    return ScanBuffer(textStart, textLen, pattern, 2);
}

uintptr_t AobScanner::ResolveRipRelative(uintptr_t instrAddr, size_t dispOffset, size_t instrSize) {
    int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<const void*>(instrAddr + dispOffset), sizeof(rel));
    return static_cast<uintptr_t>(static_cast<intptr_t>(instrAddr + instrSize) + static_cast<intptr_t>(rel));
}

} // namespace var_mod
