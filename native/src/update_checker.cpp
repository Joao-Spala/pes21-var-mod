#include "update_checker.h"

#include "logger.h"
#include "version.h"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>

namespace var_mod::UpdateChecker {

namespace {

// Public half of the maintainer's release signing key (scripts/release/sign_update.ps1 -NewKey)
constexpr uint8_t kUpdatePublicKey[64] = { // X then Y
    0x53, 0x5C, 0xB8, 0x8B, 0xD4, 0x7E, 0x32, 0xC4, 0x55, 0xBA, 0xC8, 0x5B, 0xE0, 0xF9, 0x3F, 0x8A,
    0x93, 0x2D, 0x61, 0x7B, 0xB1, 0xB8, 0x99, 0xE8, 0x6C, 0xAE, 0xE8, 0xE5, 0x5A, 0x45, 0xF1, 0xB2,
    0x81, 0xC4, 0xDB, 0xE3, 0x4B, 0x9D, 0x8B, 0x55, 0xF7, 0xE8, 0x47, 0xCF, 0x16, 0x67, 0xFE, 0x11,
    0x7E, 0xD3, 0x23, 0x60, 0xE9, 0xBD, 0x38, 0xD3, 0x77, 0xB5, 0x4A, 0xC7, 0x95, 0x67, 0x09, 0xC0,
};

constexpr size_t kMaxJson = 512 * 1024;
constexpr size_t kMaxManifest = 4 * 1024;
constexpr size_t kMaxSig = 1024;
constexpr size_t kMaxDll = 16 * 1024 * 1024;
constexpr size_t kMaxLua = 2 * 1024 * 1024;

std::mutex g_mutex;
// Published strings are never written again (the HUD may be reading one): each value gets its own buffer
char g_buf[4][32] = {};
int g_nextBuf = 0;
std::atomic<const char*> g_version{ nullptr };
std::atomic<int> g_phase{ static_cast<int>(Phase::Idle) };
std::atomic<bool> g_started{ false };
// Set once at init, before any Start
std::wstring g_baseDir;
std::wstring g_cachePath;
std::wstring g_dllPath;
std::wstring g_luaPath;

bool IsDottedVersion(const char* s) {
    if (!s || !std::isdigit(static_cast<unsigned char>(s[0]))) return false;
    for (const char* p = s; *p; ++p) {
        if (!std::isdigit(static_cast<unsigned char>(*p)) && *p != '.') return false;
    }
    return true;
}

void Publish(const char* version, Phase phase) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const char* cur = g_version.load(std::memory_order_acquire);
    if (!cur || std::strcmp(cur, version) != 0) {
        if (g_nextBuf >= 4) return;
        char* buf = g_buf[g_nextBuf++];
        std::snprintf(buf, sizeof(g_buf[0]), "%s", version);
        g_version.store(buf, std::memory_order_release);
    }
    g_phase.store(static_cast<int>(phase), std::memory_order_release);
}

bool FileExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool WriteWholeFile(const std::wstring& path, const std::string& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
                    written == data.size() && FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

// Cache: "last_check = <unix seconds>" and "latest = <version>"
void ReadCache(int64_t& lastCheck, char* latest, size_t latestSize) {
    lastCheck = 0;
    latest[0] = '\0';
    FILE* f = _wfopen(g_cachePath.c_str(), L"rb");
    if (!f) return;
    char line[128];
    while (std::fgets(line, sizeof(line), f)) {
        char key[32] = {};
        char val[64] = {};
        if (std::sscanf(line, " %31[a-z_] = %63s", key, val) != 2) continue;
        if (std::strcmp(key, "last_check") == 0) lastCheck = std::strtoll(val, nullptr, 10);
        else if (std::strcmp(key, "latest") == 0 && IsDottedVersion(val)) std::snprintf(latest, latestSize, "%s", val);
    }
    std::fclose(f);
}

void WriteCache(int64_t lastCheck, const char* latest) {
    const std::wstring tmp = g_cachePath + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return;
    std::fprintf(f, "# PES 2021 VAR Mod - ultima verificacao de nova versao (apague para verificar de novo)\n");
    std::fprintf(f, "last_check = %lld\n", static_cast<long long>(lastCheck));
    if (latest && latest[0]) std::fprintf(f, "latest = %s\n", latest);
    const bool ok = (std::fflush(f) == 0) && !std::ferror(f);
    std::fclose(f);
    if (!ok || !MoveFileExW(tmp.c_str(), g_cachePath.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp.c_str());
}

// HTTPS GET of a github.com / api.github.com URL (redirects to GitHub's download hosts are followed by WinHTTP,
// never to plain HTTP). Body capped at maxBytes. False on any network or HTTP error.
bool HttpGet(const wchar_t* host, const std::wstring& path, size_t maxBytes, std::string& body, unsigned long* httpStatus) {
    body.clear();
    if (httpStatus) *httpStatus = 0;
    bool ok = false;
    HINTERNET session = WinHttpOpen(L"pes21-var-mod-update/" VAR_MOD_VERSION, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 8000, 8000, 15000, 30000);
    HINTERNET connect = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connect
        ? WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                             WINHTTP_FLAG_SECURE)
        : nullptr;
    if (request) {
        DWORD opt = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
        WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &opt, sizeof(opt));
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
        WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
        const wchar_t* headers = L"Accept: application/vnd.github+json, application/octet-stream\r\n";
        if (WinHttpSendRequest(request, headers, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
            DWORD status = 0;
            DWORD statusSize = sizeof(status);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
            if (httpStatus) *httpStatus = status;
            if (status == 200) {
                ok = true;
                for (;;) {
                    DWORD avail = 0;
                    if (!WinHttpQueryDataAvailable(request, &avail)) { ok = false; break; }
                    if (avail == 0) break;
                    if (body.size() + avail > maxBytes) { ok = false; break; }
                    const size_t old = body.size();
                    body.resize(old + avail);
                    DWORD read = 0;
                    if (!WinHttpReadData(request, body.data() + old, avail, &read)) { ok = false; break; }
                    body.resize(old + read);
                }
            }
        }
        WinHttpCloseHandle(request);
    }
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    if (!ok) body.clear();
    return ok;
}

std::wstring ReleaseAssetPath(const char* tag, const wchar_t* name) {
    std::wstring p = L"/Joao-Spala/pes21-var-mod/releases/download/v";
    for (const char* c = tag; *c; ++c) p.push_back(static_cast<wchar_t>(*c));
    p += L"/";
    p += name;
    return p;
}

// Downloads, checks and installs the release. Leaves the phase at Installed or Available.
void TryInstall(const char* tag) {
    if (g_dllPath.empty() || g_luaPath.empty() || !FileExists(g_dllPath) || !FileExists(g_luaPath)) {
        VAR_LOG_WRN("Automatic update: installed files not found, only the notice is shown");
        Publish(tag, Phase::Available);
        return;
    }
    Publish(tag, Phase::Installing);
    std::string manifest, sig, dll, lua;
    unsigned long st = 0;
    if (!HttpGet(L"github.com", ReleaseAssetPath(tag, L"update_manifest.txt"), kMaxManifest, manifest, &st) ||
        !HttpGet(L"github.com", ReleaseAssetPath(tag, L"update_manifest.sig"), kMaxSig, sig, &st)) {
        VAR_LOG_INF("Automatic update: v%s has no signed update (HTTP %lu), only the notice is shown", tag, st);
        Publish(tag, Phase::Available);
        return;
    }
    if (!HttpGet(L"github.com", ReleaseAssetPath(tag, L"var_core.dll"), kMaxDll, dll, &st) ||
        !HttpGet(L"github.com", ReleaseAssetPath(tag, L"var_mod.lua"), kMaxLua, lua, &st)) {
        VAR_LOG_WRN("Automatic update: download of v%s failed (HTTP %lu), tried again later", tag, st);
        Publish(tag, Phase::Available);
        return;
    }
    Manifest m;
    const char* why = "";
    if (!ValidateUpdate(manifest, sig, tag, dll, lua, kUpdatePublicKey, m, &why)) {
        VAR_LOG_WRN("Automatic update: v%s refused (%s), nothing was installed", tag, why);
        Publish(tag, Phase::Available);
        return;
    }
    if (!ApplyUpdate(g_dllPath, g_luaPath, dll, lua)) {
        VAR_LOG_WRN("Automatic update: v%s could not replace the files (error %lu), the installed version was kept",
                    tag, GetLastError());
        Publish(tag, Phase::Available);
        return;
    }
    // Copy for this session: the loaded var_core.dll keeps its name in the loader, so the new one is loaded from
    // another path. Without it the new version simply starts at the next game start.
    const std::wstring nextDll = g_baseDir + L"var_core.next.dll";
    if (!WriteWholeFile(nextDll, dll)) DeleteFileW(nextDll.c_str());
    VAR_LOG_INF("Automatic update: v%s verified (signature, SHA-256) and installed", tag);
    Publish(tag, Phase::Installed);
}

void CheckThread(int64_t now, std::string cachedLatest) {
    std::string json;
    unsigned long st = 0;
    char tag[32] = {};
    if (!HttpGet(L"api.github.com", L"/repos/Joao-Spala/pes21-var-mod/releases/latest", kMaxJson, json, &st) ||
        !ParseTagName(json.data(), json.size(), tag, sizeof(tag))) {
        // Offline or blocked: tried again after the interval, the version already known stays
        WriteCache(now, cachedLatest.c_str());
        VAR_LOG_INF("Update check: no answer from GitHub (HTTP %lu, offline or blocked), next try at the next game start", st);
        return;
    }
    WriteCache(now, tag);
    if (CompareVersions(tag, VAR_MOD_VERSION) <= 0) {
        VAR_LOG_INF("Update check: v" VAR_MOD_VERSION " is the latest");
        return;
    }
    VAR_LOG_INF("Update check: v%s available (running v" VAR_MOD_VERSION ")", tag);
    TryInstall(tag);
}

bool HexNibble(char c, uint8_t& v) {
    if (c >= '0' && c <= '9') { v = static_cast<uint8_t>(c - '0'); return true; }
    if (c >= 'a' && c <= 'f') { v = static_cast<uint8_t>(c - 'a' + 10); return true; }
    if (c >= 'A' && c <= 'F') { v = static_cast<uint8_t>(c - 'A' + 10); return true; }
    return false;
}

// "<64 hex> <size>" after "name = "
bool ParseHashLine(const char* v, uint8_t hash[32], uint64_t& size) {
    while (*v == ' ') ++v;
    if (std::strlen(v) < 66 || !ParseHex(v, 64, hash, 32) || v[64] != ' ') return false;
    const char* s = v + 65;
    if (!std::isdigit(static_cast<unsigned char>(*s))) return false;
    char* end = nullptr;
    size = std::strtoull(s, &end, 10);
    while (end && (*end == ' ' || *end == '\r')) ++end;
    return end && *end == '\0' && size > 0;
}

void LuaPathFromBase(const std::wstring& baseDir, std::wstring& luaPath) {
    luaPath.clear();
    std::wstring d = baseDir;
    while (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
    const size_t pos = d.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return;
    luaPath = d.substr(0, pos + 1) + L"var_mod.lua";
}

} // namespace

void NoteLatest(const char* version) {
    if (!IsDottedVersion(version) || CompareVersions(version, VAR_MOD_VERSION) <= 0) return;
    if (g_phase.load(std::memory_order_acquire) != static_cast<int>(Phase::Idle)) {
        const char* cur = g_version.load(std::memory_order_acquire);
        if (cur && CompareVersions(version, cur) <= 0) return;
    }
    Publish(version, Phase::Available);
}

Phase CurrentPhase() {
    return static_cast<Phase>(g_phase.load(std::memory_order_acquire));
}

const char* NewerVersion() {
    if (CurrentPhase() == Phase::Idle) return nullptr;
    return g_version.load(std::memory_order_acquire);
}

int CompareVersions(const char* a, const char* b) {
    if (!a) a = "";
    if (!b) b = "";
    if (*a == 'v' || *a == 'V') ++a;
    if (*b == 'v' || *b == 'V') ++b;
    for (int part = 0; part < 6; ++part) {
        long va = 0, vb = 0;
        if (std::isdigit(static_cast<unsigned char>(*a))) va = std::strtol(a, const_cast<char**>(&a), 10);
        if (std::isdigit(static_cast<unsigned char>(*b))) vb = std::strtol(b, const_cast<char**>(&b), 10);
        if (va != vb) return va < vb ? -1 : 1;
        if (*a == '.') ++a;
        if (*b == '.') ++b;
        if (!std::isdigit(static_cast<unsigned char>(*a)) && !std::isdigit(static_cast<unsigned char>(*b))) break;
    }
    return 0;
}

bool ParseTagName(const char* json, size_t len, char* out, size_t outSize) {
    if (!json || !out || outSize < 2) return false;
    out[0] = '\0';
    static constexpr char kKey[] = "\"tag_name\"";
    constexpr size_t kKeyLen = sizeof(kKey) - 1;
    for (size_t i = 0; i + kKeyLen <= len; ++i) {
        if (std::memcmp(json + i, kKey, kKeyLen) != 0) continue;
        size_t p = i + kKeyLen;
        while (p < len && std::isspace(static_cast<unsigned char>(json[p]))) ++p;
        if (p >= len || json[p] != ':') continue;
        ++p;
        while (p < len && std::isspace(static_cast<unsigned char>(json[p]))) ++p;
        if (p >= len || json[p] != '"') continue;
        ++p;
        if (p < len && (json[p] == 'v' || json[p] == 'V')) ++p;
        size_t n = 0;
        while (p < len && json[p] != '"' && n + 1 < outSize) out[n++] = json[p++];
        if (p >= len || json[p] != '"') { out[0] = '\0'; return false; }
        out[n] = '\0';
        if (!IsDottedVersion(out)) { out[0] = '\0'; return false; }
        return true;
    }
    return false;
}

bool CheckDue(int64_t lastCheckSec, int64_t nowSec) {
    if (lastCheckSec <= 0) return true;
    if (lastCheckSec > nowSec + 60) return true; // clock moved back
    return nowSec - lastCheckSec >= kCheckIntervalSec;
}

bool Sha256(const void* data, size_t len, uint8_t out[32]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return false;
    BCRYPT_HASH_HANDLE h = nullptr;
    bool ok = BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t left = len;
    while (ok && left > 0) {
        const ULONG chunk = static_cast<ULONG>(left > 0x40000000 ? 0x40000000 : left);
        ok = BCryptHashData(h, const_cast<PUCHAR>(p), chunk, 0) == 0;
        p += chunk;
        left -= chunk;
    }
    if (ok) ok = BCryptFinishHash(h, out, 32, 0) == 0;
    if (h) BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

bool VerifySignature(const void* msg, size_t len, const uint8_t sig[64], const uint8_t pubXY[64]) {
    uint8_t digest[32];
    if (!Sha256(msg, len, digest)) return false;
    struct { BCRYPT_ECCKEY_BLOB hdr; uint8_t xy[64]; } blob{};
    blob.hdr.dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    blob.hdr.cbKey = 32;
    std::memcpy(blob.xy, pubXY, 64);
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) != 0) return false;
    BCRYPT_KEY_HANDLE key = nullptr;
    bool ok = BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key, reinterpret_cast<PUCHAR>(&blob),
                                  sizeof(blob), 0) == 0;
    if (ok) {
        ok = BCryptVerifySignature(key, nullptr, digest, sizeof(digest), const_cast<PUCHAR>(sig), 64, 0) == 0;
        BCryptDestroyKey(key);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

bool ParseHex(const char* text, size_t len, uint8_t* out, size_t outLen) {
    size_t a = 0;
    while (a < len && std::isspace(static_cast<unsigned char>(text[a]))) ++a;
    size_t b = len;
    while (b > a && std::isspace(static_cast<unsigned char>(text[b - 1]))) --b;
    if (b - a != outLen * 2) return false;
    for (size_t i = 0; i < outLen; ++i) {
        uint8_t hi = 0, lo = 0;
        if (!HexNibble(text[a + i * 2], hi) || !HexNibble(text[a + i * 2 + 1], lo)) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

bool ParseManifest(const char* text, size_t len, Manifest& out) {
    out = Manifest{};
    if (!text || len == 0 || len > kMaxManifest) return false;
    std::string s(text, len);
    bool header = false, ver = false, dll = false, lua = false;
    size_t pos = 0;
    int lineNo = 0;
    while (pos < s.size()) {
        size_t nl = s.find('\n', pos);
        if (nl == std::string::npos) nl = s.size();
        std::string line = s.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (lineNo++ == 0) {
            if (line != "pes21-var-mod update 1") return false;
            header = true;
            continue;
        }
        const size_t eq = line.find(" = ");
        if (eq == std::string::npos) return false;
        const std::string k = line.substr(0, eq);
        const std::string v = line.substr(eq + 3);
        if (k == "version") {
            if (ver || v.size() >= sizeof(out.version) || !IsDottedVersion(v.c_str())) return false;
            std::snprintf(out.version, sizeof(out.version), "%s", v.c_str());
            ver = true;
        } else if (k == "var_core.dll") {
            if (dll || !ParseHashLine(v.c_str(), out.dllHash, out.dllSize)) return false;
            dll = true;
        } else if (k == "var_mod.lua") {
            if (lua || !ParseHashLine(v.c_str(), out.luaHash, out.luaSize)) return false;
            lua = true;
        } else {
            return false; // unknown key: a newer manifest format this DLL does not understand
        }
    }
    return header && ver && dll && lua;
}

bool ValidateUpdate(const std::string& manifest, const std::string& sigHex, const char* tag, const std::string& dll,
                    const std::string& lua, const uint8_t pubXY[64], Manifest& out, const char** why) {
    const char* dummy = nullptr;
    if (!why) why = &dummy;
    uint8_t sig[64];
    if (!ParseHex(sigHex.data(), sigHex.size(), sig, sizeof(sig))) { *why = "malformed signature"; return false; }
    if (!VerifySignature(manifest.data(), manifest.size(), sig, pubXY)) { *why = "signature does not verify"; return false; }
    if (!ParseManifest(manifest.data(), manifest.size(), out)) { *why = "malformed manifest"; return false; }
    if (!tag || std::strcmp(out.version, tag) != 0) { *why = "manifest version differs from the release tag"; return false; }
    if (CompareVersions(out.version, VAR_MOD_VERSION) <= 0) { *why = "not newer than the running version"; return false; }
    uint8_t h[32];
    if (dll.size() != out.dllSize || !Sha256(dll.data(), dll.size(), h) || std::memcmp(h, out.dllHash, 32) != 0) {
        *why = "var_core.dll differs from the signed manifest";
        return false;
    }
    if (lua.size() != out.luaSize || !Sha256(lua.data(), lua.size(), h) || std::memcmp(h, out.luaHash, 32) != 0) {
        *why = "var_mod.lua differs from the signed manifest";
        return false;
    }
    if (dll.size() < 2 || dll[0] != 'M' || dll[1] != 'Z') { *why = "var_core.dll is not a DLL"; return false; }
    *why = "";
    return true;
}

bool ApplyUpdate(const std::wstring& dllPath, const std::wstring& luaPath, const std::string& dll, const std::string& lua) {
    const std::wstring newDll = dllPath + L".new", oldDll = dllPath + L".old";
    const std::wstring newLua = luaPath + L".new", oldLua = luaPath + L".old";
    // Leftovers of an earlier attempt (never loaded: the running DLL is always var_core.dll at this point)
    DeleteFileW(newDll.c_str());
    DeleteFileW(newLua.c_str());
    if (FileExists(oldDll) && !DeleteFileW(oldDll.c_str())) return false;
    if (!WriteWholeFile(newDll, dll) || !WriteWholeFile(newLua, lua)) {
        DeleteFileW(newDll.c_str());
        DeleteFileW(newLua.c_str());
        return false;
    }
    // DLL first (renaming the loaded image is allowed, replacing or deleting it is not)
    if (!MoveFileExW(dllPath.c_str(), oldDll.c_str(), MOVEFILE_WRITE_THROUGH)) {
        const DWORD e = GetLastError();
        DeleteFileW(newDll.c_str());
        DeleteFileW(newLua.c_str());
        SetLastError(e);
        return false;
    }
    if (!MoveFileExW(newDll.c_str(), dllPath.c_str(), MOVEFILE_WRITE_THROUGH)) {
        const DWORD e = GetLastError();
        MoveFileExW(oldDll.c_str(), dllPath.c_str(), MOVEFILE_WRITE_THROUGH);
        DeleteFileW(newDll.c_str());
        DeleteFileW(newLua.c_str());
        SetLastError(e);
        return false;
    }
    // Lua: a copy of the old one is kept, then the new one replaces it. On failure the DLL goes back too: the two
    // files always belong to the same version.
    DeleteFileW(oldLua.c_str());
    if (!CopyFileW(luaPath.c_str(), oldLua.c_str(), FALSE) ||
        !MoveFileExW(newLua.c_str(), luaPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD e = GetLastError();
        DeleteFileW(dllPath.c_str()); // the new DLL, never loaded
        MoveFileExW(oldDll.c_str(), dllPath.c_str(), MOVEFILE_WRITE_THROUGH);
        DeleteFileW(newLua.c_str());
        DeleteFileW(oldLua.c_str());
        SetLastError(e);
        return false;
    }
    return true;
}

bool FinishPreviousUpdate(const std::wstring& baseDir, const std::wstring& luaPath) {
    const std::wstring dllPath = baseDir + L"var_core.dll";
    const std::wstring oldDll = dllPath + L".old";
    const std::wstring oldLua = luaPath + L".old";
    DeleteFileW((dllPath + L".new").c_str());
    DeleteFileW((baseDir + L"var_core.next.dll").c_str()); // fails (and stays) while it is the running copy
    if (!luaPath.empty()) DeleteFileW((luaPath + L".new").c_str());
    if (!FileExists(oldDll) && (luaPath.empty() || !FileExists(oldLua))) return false;
    const std::wstring prev = baseDir + L"previous\\";
    CreateDirectoryW(prev.c_str(), nullptr);
    if (FileExists(oldDll) && !MoveFileExW(oldDll.c_str(), (prev + L"var_core.dll").c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(oldDll.c_str());
    }
    if (!luaPath.empty() && FileExists(oldLua) &&
        !MoveFileExW(oldLua.c_str(), (prev + L"var_mod.lua").c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(oldLua.c_str());
    }
    VAR_LOG_INF("Automatic update: now running v" VAR_MOD_VERSION ", the previous version is kept in previous\\");
    return true;
}

void SetBaseDir(const std::wstring& baseDir) {
    if (g_started.load(std::memory_order_acquire) || baseDir.empty()) return;
    g_baseDir = baseDir;
    g_cachePath = baseDir + L"update_check.ini";
    g_dllPath = baseDir + L"var_core.dll";
    LuaPathFromBase(baseDir, g_luaPath);
    if (FinishPreviousUpdate(g_baseDir, g_luaPath)) Publish(VAR_MOD_VERSION, Phase::Updated);
}

void Start(bool enabled) {
    if (!enabled || g_cachePath.empty()) return;
    if (g_started.exchange(true, std::memory_order_acq_rel)) return;
    int64_t lastCheck = 0;
    char latest[32] = {};
    ReadCache(lastCheck, latest, sizeof(latest));
    NoteLatest(latest);
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    if (!CheckDue(lastCheck, now)) {
        VAR_LOG_INF("Update check: last one less than 10 min ago%s%s", latest[0] ? ", latest known v" : "", latest);
        return;
    }
    // Detached: the module is pinned for the life of the process, and WinHTTP's own timeouts end the thread
    try {
        std::thread(CheckThread, now, std::string(latest)).detach();
    } catch (...) {
        VAR_LOG_WRN("Update check: thread not started");
    }
}

} // namespace var_mod::UpdateChecker
