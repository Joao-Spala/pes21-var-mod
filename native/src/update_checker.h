#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Automatic update. Option "Atualizacao automatica" (on by default). At most every 6 h a background thread reads
// the tag of the latest release of the public repository (HTTPS GET to api.github.com, no cookies, no identifiers,
// nothing about the user or the game is sent). When it is newer than the running version it downloads the
// release's update_manifest.txt, its signature, var_core.dll and var_mod.lua, and installs them only if:
//   - the manifest signature verifies with the public key compiled into this DLL (ECDSA P-256 / SHA-256, the
//     private key never leaves the maintainer's PC), so a compromised GitHub account alone cannot push code,
//   - the manifest version equals the release tag and is above the running version (no downgrade),
//   - both files match the SHA-256 and size written in the signed manifest.
// The running DLL is renamed to var_core.dll.old (Windows allows it while loaded), the new files take the
// original names, and the new version runs from the next game start. Any failure rolls back and leaves the
// notice "new version available" with the release page instead. Next start, the previous files are kept in
// modules\var_mod\previous\ for a manual rollback.
namespace var_mod::UpdateChecker {

constexpr const char* kReleasesUrl = "github.com/Joao-Spala/pes21-var-mod/releases";
constexpr int64_t kCheckIntervalSec = 6 * 60 * 60;

enum class Phase : int {
    Idle       = 0, // nothing newer known
    Available  = 1, // newer version known, not installed (option off, unsigned release or a failure)
    Installing = 2, // downloading and checking
    Installed  = 3, // new files in place, active from the next game start
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
// Next start after an update: replaced files go to <baseDir>previous\, leftovers are removed
void FinishPreviousUpdate(const std::wstring& baseDir, const std::wstring& luaPath);

} // namespace var_mod::UpdateChecker
