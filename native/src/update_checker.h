#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Automatic update. Option "Atualizacao automatica" (on by default). Each time the game opens (at most once every
// 10 min) a background thread reads
// the tag of the latest release of the public repository (HTTPS GET to api.github.com, no cookies, no identifiers,
// nothing about the user or the game is sent). When it is newer than the running version it downloads the
// release's update_manifest.txt, its signature, var_core.dll and var_mod.lua, and installs them only if:
//   - the manifest signature verifies with the public key compiled into this DLL (ECDSA P-256 / SHA-256, the
//     private key never leaves the maintainer's PC), so a compromised GitHub account alone cannot push code,
//   - the manifest version equals the release tag and is above the running version (no downgrade),
//   - both files match the SHA-256 and size written in the signed manifest.
// The running DLL is renamed to var_core.dll.old (Windows allows it while loaded), the new files take the
// original names, and a copy var_core.next.dll is left for this session: the Lua module starts the check when
// the game opens and, if the update is in place before the first match, loads that copy instead of the old DLL,
// so the fixes apply in the same session. Otherwise they apply at the next start. Any failure rolls back and
// leaves the notice "new version available" with the release page instead. Next start, the previous files are
// kept in modules\var_mod\previous\ for a manual rollback.
namespace var_mod::UpdateChecker {

constexpr const char* kReleasesUrl = "github.com/Joao-Spala/pes21-var-mod/releases";
constexpr int64_t kCheckIntervalSec = 10 * 60; // every game start, quick restarts aside

enum class Phase : int {
    Idle       = 0, // nothing newer known
    Available  = 1, // newer version known, not installed (option off, unsigned release or a failure)
    Installing = 2, // downloading and checking
    Installed  = 3, // new files in place (var_core.next.dll ready for this session)
    Updated    = 4, // this DLL is the version an earlier update installed (announced once)
};

// Folder of var_mod.ini and var_core.dll (modules\var_mod\). var_mod.lua is one level up. Without it Start does
// nothing (unit tests). Also finishes a previous update: moves the replaced files to previous\.
void SetBaseDir(const std::wstring& baseDir);
// Starts the background check (and install) when enabled and due. Safe to call again: once per process at most.
void Start(bool enabled);

Phase CurrentPhase();
// Version the phase refers to ("4.6.0" form), nullptr in Idle
const char* NewerVersion();
// Records the latest version read (cache or GitHub). Ignored unless above the running one. The HUD snapshot of
// the tests uses it to draw the notice.
void NoteLatest(const char* version);

// ---- Pure helpers (unit tested)
int CompareVersions(const char* a, const char* b);
bool ParseTagName(const char* json, size_t len, char* out, size_t outSize);
bool CheckDue(int64_t lastCheckSec, int64_t nowSec);
bool Sha256(const void* data, size_t len, uint8_t out[32]);
// ECDSA P-256 over SHA-256(msg), signature r||s (64 bytes), public key X||Y (64 bytes)
bool VerifySignature(const void* msg, size_t len, const uint8_t sig[64], const uint8_t pubXY[64]);
// Hex text (whitespace around allowed) into exactly outLen bytes
bool ParseHex(const char* text, size_t len, uint8_t* out, size_t outLen);

struct Manifest {
    char     version[32] = {};
    uint8_t  dllHash[32] = {};
    uint64_t dllSize = 0;
    uint8_t  luaHash[32] = {};
    uint64_t luaSize = 0;
};
// "pes21-var-mod update 1" / "version = X" / "var_core.dll = <sha256> <size>" / "var_mod.lua = <sha256> <size>"
bool ParseManifest(const char* text, size_t len, Manifest& out);
// Checks a downloaded update end to end (signature with pubXY, tag, version above the running one, hashes and
// sizes). On failure *why names the reason.
bool ValidateUpdate(const std::string& manifest, const std::string& sigHex, const char* tag, const std::string& dll,
                    const std::string& lua, const uint8_t pubXY[64], Manifest& out, const char** why);
// Puts both files in place (see the header comment). Rolls back on any failure.
bool ApplyUpdate(const std::wstring& dllPath, const std::wstring& luaPath, const std::string& dll, const std::string& lua);
// After an update (same session from var_core.next.dll, or the next start): replaced files go to
// <baseDir>previous\, leftovers are removed. True when an update had just been installed.
bool FinishPreviousUpdate(const std::wstring& baseDir, const std::wstring& luaPath);

} // namespace var_mod::UpdateChecker
