#include "aob_scanner.h"

#include <windows.h>
#include <cctype>
#include <cstdio>
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

    // memchr anchor: the first fixed byte that is not one of the most frequent x64 code bytes (REX prefixes,
    // mov / lea opcodes, ModRM, padding), so memchr stops rarely. Falls back to the first fixed byte.
    auto common = [](uint8_t b) {
        switch (b) {
            case 0x00: case 0x0F: case 0x24: case 0x41: case 0x44: case 0x45: case 0x48: case 0x49: case 0x4C:
            case 0x83: case 0x89: case 0x8B: case 0x8D: case 0xC0: case 0xCC: case 0xE8: case 0xFF:
                return true;
            default:
                return false;
        }
    };
    size_t anchor = patLen;
    for (size_t k = 0; k < patLen; ++k) {
        if (mask[k] && !common(bytes[k])) {
            anchor = k;
            break;
        }
    }
    if (anchor == patLen) {
        for (size_t k = 0; k < patLen; ++k) {
            if (mask[k]) {
                anchor = k;
                break;
            }
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
// Sections larger than this are not engine code: some builds carry a ~400 MB RWX block whose random bytes
// match short patterns. The main code section of every known build is ~37 MB.
constexpr size_t kMaxScanSection = 128u * 1024u * 1024u;

struct ImageView {
    const uint8_t* base = nullptr;
    const IMAGE_NT_HEADERS64* nt = nullptr;
};

bool OpenImage(void* moduleBase, ImageView& out) {
    HMODULE mod = moduleBase ? static_cast<HMODULE>(moduleBase) : GetModuleHandleW(nullptr);
    if (!mod) return false;
    const auto* base = reinterpret_cast<const uint8_t*>(mod);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
    out.base = base;
    out.nt = nt;
    return true;
}

bool ProtectReadable(DWORD protect) {
    if (protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    // PAGE_EXECUTE alone cannot be read
    return (protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                       PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

// Whole range committed and readable (a guard, no-access or execute-only page is never touched)
bool RangeReadable(const uint8_t* p, size_t len) {
    const uint8_t* end = p + len;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
        if (mbi.State != MEM_COMMIT || !ProtectReadable(mbi.Protect)) return false;
        p = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return true;
}

// Visits the sections worth scanning, as runs of readable pages (an unreadable page inside a section only
// removes that page, never the whole section). Section names are not used: they differ between builds of the
// same engine (the main code section is ".text" in one build and ".trace" in another), only the flags count.
template <typename Fn>
void ForEachSection(const ImageView& img, bool executableOnly, Fn&& fn) {
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(img.nt);
    for (WORD i = 0; i < img.nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (executableOnly && !(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const size_t len = sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData;
        if (len == 0 || len > kMaxScanSection) continue;
        const uint8_t* p = img.base + sec->VirtualAddress;
        const uint8_t* end = p + len;
        const uint8_t* runStart = nullptr;
        while (p < end) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) break;
            const uint8_t* regionEnd = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
            if (regionEnd > end) regionEnd = end;
            const bool ok = mbi.State == MEM_COMMIT && ProtectReadable(mbi.Protect);
            if (ok && !runStart) runStart = p;
            if (!ok && runStart) {
                if (!fn(runStart, static_cast<size_t>(p - runStart))) return;
                runStart = nullptr;
            }
            p = regionEnd;
        }
        if (runStart && !fn(runStart, static_cast<size_t>(p - runStart))) return;
    }
}

bool InExecutableSection(const ImageView& img, const uint8_t* p, size_t len) {
    bool inside = false;
    ForEachSection(img, true, [&](const uint8_t* start, size_t size) {
        if (p >= start && p + len <= start + size) { inside = true; return false; }
        return true;
    });
    return inside;
}

// Plain byte search (no wildcards) collecting up to maxHits positions. The memchr anchor is a byte of the
// needle that is rare in code and data (never 0x00 / 0xFF / 0x01 / 0x40 when another byte exists): RVAs and
// addresses have zero bytes that would stop memchr on almost every byte.
void FindBytes(const uint8_t* data, size_t size, const void* needle, size_t n, std::vector<const uint8_t*>& hits, size_t maxHits) {
    if (n == 0 || size < n) return;
    const auto* nd = static_cast<const uint8_t*>(needle);
    size_t a = 0;
    for (size_t k = 0; k < n; ++k) {
        const uint8_t b = nd[k];
        if (b != 0x00 && b != 0xFF && b != 0x01 && b != 0x40) { a = k; break; }
    }
    const uint8_t* p = data + a;
    const uint8_t* lastAnchor = data + (size - n) + a;
    while (p <= lastAnchor && hits.size() < maxHits) {
        const void* h = std::memchr(p, nd[a], static_cast<size_t>(lastAnchor - p) + 1);
        if (!h) break;
        const auto* q = static_cast<const uint8_t*>(h) - a;
        if (std::memcmp(q, nd, n) == 0) hits.push_back(q);
        p = q + a + 1;
    }
}
} // namespace

AobMatchResult AobScanner::ScanModuleText(void* moduleBase, const char* pattern) {
    ImageView img;
    if (!OpenImage(moduleBase, img)) return AobMatchResult{};
    AobMatchResult total{};
    ForEachSection(img, true, [&](const uint8_t* start, size_t len) {
        const AobMatchResult r = ScanBuffer(start, len, pattern, 2);
        if (r.matchCount > 0 && total.matchCount == 0) total.address = r.address;
        total.matchCount += r.matchCount;
        return total.matchCount < 2; // unique across all executable sections, or nothing
    });
    return total;
}

bool AobScanner::IsReferenceBuild(void* moduleBase) {
    ImageView img;
    return OpenImage(moduleBase, img) && img.nt->FileHeader.TimeDateStamp == kReferenceTimestamp &&
           img.nt->OptionalHeader.SizeOfImage == kReferenceImageSize;
}

AobMatchResult AobScanner::ScanModuleTextWithHint(void* moduleBase, const char* pattern, uintptr_t hintRva,
                                                  bool allowEntryJump) {
    ImageView img;
    if (!OpenImage(moduleBase, img)) return AobMatchResult{};
    if (hintRva != 0 && IsReferenceBuild(moduleBase)) {
        std::vector<uint8_t> bytes;
        std::vector<uint8_t> mask;
        if (ParsePattern(pattern, bytes, mask)) {
            const uint8_t* cand = img.base + hintRva;
            // The hint is the address in the reference build, where the pattern was verified unique offline.
            // Other builds miss it and take the full scan.
            if (InExecutableSection(img, cand, bytes.size())) {
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
    const AobMatchResult full = ScanModuleText(moduleBase, pattern);
    if (full.matchCount != 0 || !allowEntryJump) return full;
    // Not found: another mod may have put a 5-byte jump (E9 rel32) at the entry of this function. A prologue
    // pattern (first 5 bytes fixed, 16 bytes or more) is then searched with the jump in place of those bytes.
    // InlineHook chains an existing entry jump, so the VAR hook still runs after the other mod's.
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> mask;
    if (!ParsePattern(pattern, bytes, mask) || bytes.size() < 16) return full;
    for (size_t k = 0; k < 5; ++k) {
        if (!mask[k]) return full;
    }
    std::string alt = "E9 ?? ?? ?? ??";
    for (size_t k = 5; k < bytes.size(); ++k) {
        char t[4];
        if (mask[k]) std::snprintf(t, sizeof(t), " %02X", bytes[k]);
        else std::snprintf(t, sizeof(t), " ??");
        alt += t;
    }
    const AobMatchResult hooked = ScanModuleText(moduleBase, alt.c_str());
    return hooked.unique() ? hooked : full;
}

bool AobScanner::IsInWritableSection(void* moduleBase, uintptr_t address, size_t len) {
    ImageView img;
    if (!OpenImage(moduleBase, img) || address < reinterpret_cast<uintptr_t>(img.base)) return false;
    const uint64_t rva = address - reinterpret_cast<uintptr_t>(img.base);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(img.nt);
    for (WORD i = 0; i < img.nt->FileHeader.NumberOfSections; ++i, ++sec) {
        const uint64_t size = sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData;
        if (rva >= sec->VirtualAddress && rva + len <= sec->VirtualAddress + size) {
            // A global variable: writable data of this image, never code
            return (sec->Characteristics & IMAGE_SCN_MEM_WRITE) && !(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE);
        }
    }
    return false;
}

uintptr_t AobScanner::FindVtableByRtti(void* moduleBase, const char* typeName, uintptr_t hintRva) {
    ImageView img;
    if (!OpenImage(moduleBase, img) || !typeName || !typeName[0]) return 0;
    const size_t nameLen = std::strlen(typeName) + 1; // the terminator is part of the match
    // Absolute pointers in the image (vtable[-1] -> CompleteObjectLocator) use the image base the loader wrote
    // into the mapped header, so this also works on an image mapped elsewhere for tests
    const uint64_t imageBase = img.nt->OptionalHeader.ImageBase;
    const uint32_t imageSize = img.nt->OptionalHeader.SizeOfImage;
    auto colTypeName = [&](uint32_t colRva) -> const char* {
        if (colRva == 0 || uint64_t{ colRva } + 24 > imageSize || !RangeReadable(img.base + colRva, 24)) return nullptr;
        const auto* col = reinterpret_cast<const uint32_t*>(img.base + colRva);
        if (col[0] != 1 || col[1] != 0 || col[5] != colRva) return nullptr; // x64 COL: signature 1, offset 0, pSelf
        const uint32_t td = col[3];
        if (uint64_t{ td } + 0x10 + nameLen > imageSize || !RangeReadable(img.base + td + 0x10, nameLen)) return nullptr;
        return reinterpret_cast<const char*>(img.base + td + 0x10);
    };
    auto colOfVtable = [&](uint32_t vtRva) -> uint32_t {
        if (vtRva < 8 || vtRva >= imageSize || !RangeReadable(img.base + vtRva - 8, 8)) return 0;
        const uint64_t colVa = *reinterpret_cast<const uint64_t*>(img.base + vtRva - 8);
        if (colVa < imageBase || colVa - imageBase >= imageSize) return 0;
        return static_cast<uint32_t>(colVa - imageBase);
    };
    // Reference build: the hinted vtable is checked by its own RTTI name (no scan)
    if (hintRva) {
        const char* n = colTypeName(colOfVtable(static_cast<uint32_t>(hintRva)));
        if (n && std::memcmp(n, typeName, nameLen) == 0) return reinterpret_cast<uintptr_t>(img.base + hintRva);
    }
    // 1. TypeDescriptor: the mangled name sits at +0x10
    std::vector<const uint8_t*> names;
    ForEachSection(img, false, [&](const uint8_t* start, size_t len) {
        FindBytes(start, len, typeName, nameLen, names, 4);
        return names.size() < 4;
    });
    uintptr_t found = 0;
    size_t vtables = 0;
    for (const uint8_t* nm : names) {
        const uint32_t tdRva = static_cast<uint32_t>(nm - img.base) - 0x10;
        // 2. CompleteObjectLocator of the primary base (offset 0) pointing at this TypeDescriptor
        std::vector<const uint8_t*> refs;
        ForEachSection(img, false, [&](const uint8_t* start, size_t len) {
            FindBytes(start, len, &tdRva, sizeof(tdRva), refs, 64);
            return refs.size() < 64;
        });
        for (const uint8_t* r : refs) {
            if (r - img.base < 12) continue;
            const uint32_t colRva = static_cast<uint32_t>(r - img.base) - 12;
            if (!colTypeName(colRva)) continue;
            // 3. vtable: the slot right before it holds the absolute address of the COL
            const uint64_t colVa = imageBase + colRva;
            std::vector<const uint8_t*> slots;
            ForEachSection(img, false, [&](const uint8_t* start, size_t len) {
                FindBytes(start, len, &colVa, sizeof(colVa), slots, 4);
                return slots.size() < 4;
            });
            for (const uint8_t* s : slots) {
                found = reinterpret_cast<uintptr_t>(s + 8);
                ++vtables;
            }
        }
    }
    return vtables == 1 ? found : 0; // none or ambiguous: the caller keeps the feature off
}

uintptr_t AobScanner::ResolveRipRelative(uintptr_t instrAddr, size_t dispOffset, size_t instrSize) {
    int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<const void*>(instrAddr + dispOffset), sizeof(rel));
    return static_cast<uintptr_t>(static_cast<intptr_t>(instrAddr + instrSize) + static_cast<intptr_t>(rel));
}

} // namespace var_mod
