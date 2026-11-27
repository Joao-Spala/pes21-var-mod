#include "update_checker.h"

#include "logger.h"
#include "version.h"

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>
#include <vector>

namespace var_mod::UpdateChecker {

namespace {

std::mutex g_mutex;
// Two buffers: a published string is never written again (the HUD may be reading it). At most two values are
// published per process (the cached one, then the fetched one).
char g_buf[2][32] = {};
int g_nextBuf = 0;
std::atomic<const char*> g_newer{ nullptr };
std::atomic<bool> g_started{ false };
std::wstring g_cachePath; // set once at init, before any Start

bool IsDottedVersion(const char* s) {
    if (!s || !std::isdigit(static_cast<unsigned char>(s[0]))) return false;
    for (const char* p = s; *p; ++p) {
        if (!std::isdigit(static_cast<unsigned char>(*p)) && *p != '.') return false;
    }
    return true;
}

} // namespace

void NoteLatest(const char* version) {
    if (!IsDottedVersion(version) || CompareVersions(version, VAR_MOD_VERSION) <= 0) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    const char* cur = g_newer.load(std::memory_order_acquire);
    if (cur && CompareVersions(version, cur) <= 0) return;
    if (g_nextBuf >= 2) return;
    char* buf = g_buf[g_nextBuf++];
    std::snprintf(buf, sizeof(g_buf[0]), "%s", version);
    g_newer.store(buf, std::memory_order_release);
}

namespace {

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

// One GET of the latest release. Returns false on any network or HTTP error.
bool FetchLatestTag(char* tag, size_t tagSize) {
    bool ok = false;
    HINTERNET session = WinHttpOpen(L"pes21-var-mod-update-check/" VAR_MOD_VERSION,
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 5000, 5000, 8000, 8000);
    HINTERNET connect = WinHttpConnect(session, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connect
        ? WinHttpOpenRequest(connect, L"GET", L"/repos/Joao-Spala/pes21-var-mod/releases/latest", nullptr,
                             WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
        : nullptr;
    if (request) {
        // No cookies, no credentials, no redirects to other hosts are needed for this public endpoint
        DWORD opt = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
        WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &opt, sizeof(opt));
        const wchar_t* headers = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
        if (WinHttpSendRequest(request, headers, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
            DWORD status = 0;
            DWORD statusSize = sizeof(status);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
            if (status == 200) {
                std::vector<char> body;
                constexpr size_t kMaxBody = 512 * 1024;
                for (;;) {
                    DWORD avail = 0;
                    if (!WinHttpQueryDataAvailable(request, &avail) || avail == 0) break;
                    if (body.size() + avail > kMaxBody) break;
                    const size_t old = body.size();
                    body.resize(old + avail);
                    DWORD read = 0;
                    if (!WinHttpReadData(request, body.data() + old, avail, &read)) { body.resize(old); break; }
                    body.resize(old + read);
                    if (read == 0) break;
                }
                ok = ParseTagName(body.data(), body.size(), tag, tagSize);
            } else {
                VAR_LOG_INF("New version check: HTTP %lu", static_cast<unsigned long>(status));
            }
        }
        WinHttpCloseHandle(request);
    }
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return ok;
}

void CheckThread(int64_t now, std::string cachedLatest) {
    char tag[32] = {};
    if (FetchLatestTag(tag, sizeof(tag))) {
        NoteLatest(tag);
        WriteCache(now, tag);
        if (CompareVersions(tag, VAR_MOD_VERSION) > 0) VAR_LOG_INF("New version available: v%s (installed v" VAR_MOD_VERSION ")", tag);
        else VAR_LOG_INF("New version check: v" VAR_MOD_VERSION " is the latest");
    } else {
        // Offline or blocked: tried again in 24 h, the version already known stays
        WriteCache(now, cachedLatest.c_str());
        VAR_LOG_INF("New version check: no answer from GitHub (offline or blocked), next try in 24 h");
    }
}

} // namespace

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

const char* NewerVersion() {
    const char* v = g_newer.load(std::memory_order_acquire);
    // Only versions above the installed one are ever published (a cached value the user already installed is not)
    return v;
}

void SetBaseDir(const std::wstring& baseDir) {
    if (!g_started.load(std::memory_order_acquire)) g_cachePath = baseDir + L"update_check.ini";
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
        VAR_LOG_INF("New version check: last one less than 24 h ago%s%s", latest[0] ? ", latest known v" : "", latest);
        return;
    }
    // Detached: the module is pinned for the life of the process, and WinHTTP's own timeouts end the thread
    try {
        std::thread(CheckThread, now, std::string(latest)).detach();
    } catch (...) {
        VAR_LOG_WRN("New version check: thread not started");
    }
}

} // namespace var_mod::UpdateChecker
