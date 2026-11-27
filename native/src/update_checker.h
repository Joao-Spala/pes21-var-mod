#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// New-version notice. At most once every 24 h a background thread reads the tag of the latest release of the
// public repository (one HTTPS GET to api.github.com, no cookies, no identifiers, nothing about the user or the
// game is sent). It never downloads or installs anything: the HUD only shows that a newer version exists and
// where to get it. The result is cached next to var_mod.ini so the notice also works without a new request.
namespace var_mod::UpdateChecker {

constexpr const char* kReleasesUrl = "github.com/Joao-Spala/pes21-var-mod/releases";
constexpr int64_t kCheckIntervalSec = 24 * 60 * 60;

// Folder of var_mod.ini (the cache update_check.ini lives there). Without it Start does nothing (unit tests).
void SetBaseDir(const std::wstring& baseDir);
// Reads the cache and starts the background check when it is due and enabled. Safe to call again (for example
// after the option is switched on): one check per process at most.
void Start(bool enabled);

// Newer version known (cached or just fetched), "4.6.0" form, or nullptr when the installed one is the latest
const char* NewerVersion();
// Records the latest version read (from the cache or GitHub). Ignored unless above the installed one. The HUD
// snapshot of the tests uses it to draw the notice.
void NoteLatest(const char* version);

// Pure helpers (unit tested)
// Compares dotted versions ("4.5.0", "v4.10"): <0, 0, >0. Missing parts count as 0.
int CompareVersions(const char* a, const char* b);
// Extracts "tag_name" from a GitHub release JSON into out (leading 'v' removed). False when absent or not a
// plain dotted version.
bool ParseTagName(const char* json, size_t len, char* out, size_t outSize);
// True when a check is due: never checked, interval elapsed, or a last check time in the future (clock change)
bool CheckDue(int64_t lastCheckSec, int64_t nowSec);

} // namespace var_mod::UpdateChecker
