#pragma once

namespace var_mod {

class Dx11Hook {
public:
    static bool StartAsync();
    static void Shutdown();
    static bool IsHooked();
    static bool IsInitFinished();
};

} // namespace var_mod
