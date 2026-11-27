#include "engine_guards.h"

#include "aob_scanner.h"
#include "logger.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <vector>

namespace var_mod {

namespace {
#include "engine_guards.inc"

constexpr size_t kMaxScanSection = 128u * 1024u * 1024u; // same rule as AobScanner: the ~400 MB RWX block is not code

struct Pat {
    std::vector<uint8_t> bytes, mask;
    uint32_t hint = 0;
    size_t anchor = 0;   // offset of a run of 4 fixed bytes
    uint32_t key = 0;    // those 4 bytes
    size_t count = 0;
};

bool ParseCompact(const char* s, Pat& p) {
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    p.bytes.clear();
    p.mask.clear();
    if (std::strlen(s) % 2 != 0) return false;
    for (; s[0] && s[1]; s += 2) {
        if (s[0] == '?' && s[1] == '?') { p.bytes.push_back(0); p.mask.push_back(0); continue; }
        const int hi = hex(s[0]), lo = hex(s[1]);
        if (hi < 0 || lo < 0) return false;
        p.bytes.push_back(static_cast<uint8_t>((hi << 4) | lo));
        p.mask.push_back(1);
    }
    // Longest run of fixed bytes: its first 4 bytes are the lookup key
    size_t best = 0, bestLen = 0, cur = 0, curLen = 0;
    for (size_t i = 0; i < p.mask.size(); ++i) {
        if (p.mask[i]) { if (!curLen) cur = i; if (++curLen > bestLen) { bestLen = curLen; best = cur; } }
        else curLen = 0;
    }
    if (bestLen < 4) return false;
    p.anchor = best;
    std::memcpy(&p.key, p.bytes.data() + best, 4);
    return true;
}

bool Readable(DWORD protect) {
    if (protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return (protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                       PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

bool MatchAt(const uint8_t* p, const Pat& pat) {
    for (size_t k = 0; k < pat.bytes.size(); ++k) {
        if (pat.mask[k] && p[k] != pat.bytes[k]) return false;
    }
    return true;
}

struct Image {
    const uint8_t* base = nullptr;
    const IMAGE_NT_HEADERS64* nt = nullptr;
};

bool Open(void* moduleBase, Image& img) {
    HMODULE mod = moduleBase ? static_cast<HMODULE>(moduleBase) : GetModuleHandleW(nullptr);
    if (!mod) return false;
    img.base = reinterpret_cast<const uint8_t*>(mod);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(img.base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    img.nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(img.base + dos->e_lfanew);
    return img.nt->Signature == IMAGE_NT_SIGNATURE && img.nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC;
}

// Readable runs of the executable sections
template <typename Fn>
void ForEachCodeRun(const Image& img, Fn&& fn) {
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(img.nt);
    for (WORD i = 0; i < img.nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const size_t len = sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData;
        if (len == 0 || len > kMaxScanSection) continue;
        const uint8_t* p = img.base + sec->VirtualAddress;
        const uint8_t* end = p + len;
        const uint8_t* run = nullptr;
        while (p < end) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) break;
            const uint8_t* rend = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
            if (rend > end) rend = end;
            const bool ok = mbi.State == MEM_COMMIT && Readable(mbi.Protect);
            if (ok && !run) run = p;
            if (!ok && run) { fn(run, static_cast<size_t>(p - run)); run = nullptr; }
            p = rend;
        }
        if (run) fn(run, static_cast<size_t>(p - run));
    }
}

using Runs = std::vector<std::pair<const uint8_t*, size_t>>;

bool HintInCode(const Image& img, const Runs& runs, const Pat& pat) {
    if (!pat.hint) return false;
    const uint8_t* cand = img.base + pat.hint;
    for (const auto& r : runs) {
        if (cand >= r.first && cand + pat.bytes.size() <= r.first + r.second) return MatchAt(cand, pat);
    }
    return false;
}

// Counts the matches (capped at 2) of every pattern in one pass over the code
void ScanAll(const Runs& runs, std::vector<Pat*>& pats) {
    if (pats.empty()) return;
    std::vector<std::pair<uint32_t, Pat*>> keys;
    keys.reserve(pats.size());
    std::vector<uint8_t> filter(65536 / 8, 0);
    for (Pat* p : pats) {
        keys.emplace_back(p->key, p);
        const uint32_t h = (p->key ^ (p->key >> 16)) & 0xFFFF;
        filter[h >> 3] |= static_cast<uint8_t>(1u << (h & 7));
    }
    std::sort(keys.begin(), keys.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& run : runs) {
        const uint8_t* start = run.first;
        const size_t len = run.second;
        if (len < 4) continue;
        for (size_t pos = 0; pos + 4 <= len; ++pos) {
            uint32_t v;
            std::memcpy(&v, start + pos, 4);
            const uint32_t h = (v ^ (v >> 16)) & 0xFFFF;
            if (!(filter[h >> 3] & (1u << (h & 7)))) continue;
            auto it = std::lower_bound(keys.begin(), keys.end(), v, [](const auto& a, uint32_t x) { return a.first < x; });
            for (; it != keys.end() && it->first == v; ++it) {
                Pat* p = it->second;
                if (p->count >= 2 || pos < p->anchor) continue;
                const size_t s = pos - p->anchor;
                if (s + p->bytes.size() > len) continue;
                if (MatchAt(start + s, *p)) ++p->count;
            }
        }
    }
}

std::mutex g_mutex;
bool g_done = false;
std::atomic<bool> g_rejected{ false };
EngineGuards::Report g_report;
} // namespace

namespace EngineGuards {

Report VerifyImage(void* moduleBase) {
    Report r;
    const uint64_t t0 = GetTickCount64();
    Image img;
    if (!Open(moduleBase, img)) {
        r.missingOffsets = "(no PE image)";
        return r;
    }
    r.timestamp = img.nt->FileHeader.TimeDateStamp;
    r.sizeOfImage = img.nt->OptionalHeader.SizeOfImage;

    // Readable runs of the executable sections, queried once
    Runs runs;
    ForEachCodeRun(img, [&](const uint8_t* start, size_t len) { runs.emplace_back(start, len); });

    const bool reference = img.nt->FileHeader.TimeDateStamp == AobScanner::kReferenceTimestamp &&
                           img.nt->OptionalHeader.SizeOfImage == AobScanner::kReferenceImageSize;
    const size_t nl = sizeof(kLayoutGuards) / sizeof(kLayoutGuards[0]);
    const size_t nb = sizeof(kBodyGuards) / sizeof(kBodyGuards[0]);
    std::vector<Pat> pats(nl + nb);
    std::vector<Pat*> pending;
    bool allHints = true;
    for (size_t i = 0; i < nl + nb; ++i) {
        const char* s = i < nl ? kLayoutGuards[i].pattern : kBodyGuards[i - nl].pattern;
        pats[i].hint = i < nl ? kLayoutGuards[i].hintRva : kBodyGuards[i - nl].hintRva;
        if (!ParseCompact(s, pats[i])) { pats[i].count = 0; allHints = false; continue; }
        // Reference build only (fingerprint): the guard sits at its studied address, proven unique there
        // offline. Any other executable proves uniqueness with the full scan.
        if (reference && HintInCode(img, runs, pats[i])) pats[i].count = 1;
        else { allHints = false; pending.push_back(&pats[i]); }
    }
    ScanAll(runs, pending);
    r.referenceBuild = allHints;

    // Another mod may already have put a 5-byte jump (E9 rel32) at the entry of a function the VAR hooks (the
    // inline hook chains it). Such a body is accepted when everything after the jump matches and the entry
    // really is an E9.
    {
        std::vector<Pat> variants;
        std::vector<size_t> owner;
        for (size_t i = 0; i < nb; ++i) {
            Pat& p = pats[nl + i];
            if (p.count != 0 || p.bytes.size() < 16) continue;
            Pat v;
            if (!ParseCompact(kBodyGuards[i].pattern, v)) continue;
            for (size_t k = 0; k < 5; ++k) v.mask[k] = 0;
            size_t best = 0, bestLen = 0, curLen = 0;
            for (size_t k = 0; k < v.mask.size(); ++k) {
                if (v.mask[k]) { if (++curLen > bestLen) { bestLen = curLen; best = k - curLen + 1; } }
                else curLen = 0;
            }
            if (bestLen < 4) continue;
            v.anchor = best;
            std::memcpy(&v.key, v.bytes.data() + best, 4);
            v.count = 0;
            variants.push_back(std::move(v));
            owner.push_back(nl + i);
        }
        std::vector<Pat*> vp;
        for (Pat& v : variants) vp.push_back(&v);
        // Matches are counted only where the entry byte is E9
        for (Pat& v : variants) { v.bytes[0] = 0xE9; v.mask[0] = 1; }
        ScanAll(runs, vp);
        for (size_t k = 0; k < variants.size(); ++k) {
            if (variants[k].count == 1) pats[owner[k]].count = 1;
        }
    }

    // Offsets: confirmed when at least one of their guards is found exactly once
    std::set<std::string> all, confirmed, optAll, optConfirmed;
    for (size_t i = 0; i < nl; ++i) {
        const bool found = pats[i].count == 1;
        r.layoutGuards++;
        if (found) r.layoutFound++;
        std::string names = kLayoutGuards[i].offsets;
        size_t a = 0;
        while (a <= names.size()) {
            size_t b = names.find(',', a);
            if (b == std::string::npos) b = names.size();
            const std::string n = names.substr(a, b - a);
            if (!n.empty()) {
                const bool optional = n.rfind("GoalRecord.", 0) == 0;
                (optional ? optAll : all).insert(n);
                if (found) (optional ? optConfirmed : confirmed).insert(n);
            }
            a = b + 1;
        }
    }
    r.offsets = all.size();
    r.offsetsConfirmed = confirmed.size();
    for (const auto& n : all) {
        if (!confirmed.count(n)) r.missingOffsets += (r.missingOffsets.empty() ? "" : ",") + n;
    }
    for (size_t i = 0; i < nb; ++i) {
        r.bodies++;
        if (pats[nl + i].count == 1) r.bodiesFound++;
        else r.missingBodies += (r.missingBodies.empty() ? "" : ",") + std::string(kBodyGuards[i].aob);
    }
    r.goalRecordOffsets = optAll.size();
    r.goalRecordConfirmed = optConfirmed.size();
    r.goalRecordOk = !optAll.empty() && optConfirmed.size() == optAll.size();
    r.ok = r.offsetsConfirmed == r.offsets && r.bodiesFound == r.bodies;
    r.elapsedMs = GetTickCount64() - t0;
    return r;
}

const Report& Verify() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_done) {
        g_report = VerifyImage(nullptr);
        g_done = true;
        g_rejected.store(!g_report.ok, std::memory_order_release);
        if (g_report.ok) {
            VAR_LOG_INF("Engine check: %s", Summary(g_report).c_str());
        } else {
            VAR_LOG_ERR("Engine check FAILED, the VAR stays off in this executable: %s", Summary(g_report).c_str());
            if (!g_report.missingOffsets.empty()) VAR_LOG_ERR("  unproven structure offsets: %s", g_report.missingOffsets.c_str());
            if (!g_report.missingBodies.empty()) VAR_LOG_ERR("  engine functions not found as studied: %s", g_report.missingBodies.c_str());
        }
        Logger::Flush();
    }
    return g_report;
}

bool Rejected() {
    return g_rejected.load(std::memory_order_acquire);
}

std::string Summary(const Report& r) {
    char buf[384];
    std::snprintf(buf, sizeof(buf),
                  "%s build (timestamp 0x%08X, image 0x%X): %zu/%zu structure offsets proven by %zu/%zu engine code guards, "
                  "%zu/%zu hooked functions identical, goal record %zu/%zu, %llu ms",
                  r.referenceBuild ? "reference" : "other", r.timestamp, r.sizeOfImage, r.offsetsConfirmed, r.offsets,
                  r.layoutFound, r.layoutGuards, r.bodiesFound, r.bodies, r.goalRecordConfirmed, r.goalRecordOffsets,
                  static_cast<unsigned long long>(r.elapsedMs));
    return buf;
}

} // namespace EngineGuards
} // namespace var_mod
