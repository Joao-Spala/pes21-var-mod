#pragma once

#include <cstdint>

namespace var_mod {

class Dx11Hook {
public:
    static bool StartAsync();
    static void Shutdown();
    static bool IsHooked();
    static bool IsInitFinished();
    // Time the mod spends in the present hook (the game's own Present excluded), since the previous call
    struct PresentStats {
        uint64_t frames = 0;
        uint64_t microseconds = 0;
        uint64_t maxMicroseconds = 0;
    };
    static PresentStats TakePresentStats();
};

} // namespace var_mod
