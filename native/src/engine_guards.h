#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace var_mod {

// Runtime proof that the running executable has the engine code and the structure layout the mod relies on.
// Generated from a study of two engine builds (engine_guards.inc): every guard is a byte pattern of real engine
// instructions, address fields masked, that was unique and identical in both. Any other build (an older game
// version, a patch that ships its own executable) is accepted only when it proves the same: if it does not, the
// VAR stays off and the log says which proof is missing, instead of reading or writing the wrong memory.
struct EngineLayoutGuard {
    const char* offsets;  // comma separated structure offsets (game_structs.h names) this code proves
    uint32_t    hintRva;  // address in the reference build (fast path)
    const char* pattern;  // two hex chars per byte, "??" = address field
};

struct EngineBodyGuard {
    const char* aob;      // the mod AOB that locates this hooked or called engine function
    uint32_t    hintRva;
    const char* pattern;  // whole function body
};

namespace EngineGuards {

struct Report {
    bool        ok = false;
    size_t      layoutGuards = 0, layoutFound = 0;
    size_t      offsets = 0, offsetsConfirmed = 0;
    size_t      bodies = 0, bodiesFound = 0;
    bool        referenceBuild = false; // every guard matched at its reference address
    uint32_t    timestamp = 0, sizeOfImage = 0;
    uint64_t    elapsedMs = 0;
    std::string missingOffsets; // comma separated
    std::string missingBodies;
    // Optional group "GoalRecord." (goal annulment only): not part of ok, the feature checks it on its own
    size_t      goalRecordOffsets = 0, goalRecordConfirmed = 0;
    bool        goalRecordOk = false;
};

// Runs once per process (later calls return the cached report). Thread safe.
const Report& Verify();
// Uncached check of any mapped image (tests)
Report VerifyImage(void* moduleBase);
// True after Verify() rejected this executable
bool Rejected();
// One line for logs and the Sider overlay
std::string Summary(const Report& r);

} // namespace EngineGuards
} // namespace var_mod
