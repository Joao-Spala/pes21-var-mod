# Real Patch and Sider 7.3.3 Patterns

These patterns and addresses were extracted directly from the modules installed in `<patch folder>\modules\`.

## 1. Addresses Already Validated in `PES2021.exe` (Steam 1.7.2.0)

Extracted from `<patch folder>/modules/goal_pyro.lua` (table `m._gp.MAP`, which converts CPY/FL26 addresses to Steam 1.7.2.0):

| Symbol / Function | VA Steam 1.7.2.0 | VA CPY/FL26 | What it represents |
| :--- | :--- | :--- | :--- |
| EXE detection signature | File offset `0x3FF480` (`4188800`) | File offset `0x3FF430` (`4188720`) | 32 bytes used by `goal_pyro.lua` and `PESCustomCelebrationsReload.lua` to confirm the Steam vs CPY build on disk before unpacking |
| `GMATCH_GLOBAL` | `0x143702208` | `0x1436F3DC8` | Global pointer (`uint64_t*`) to the active match object (`GMATCH`) |
| `GMATCH_REPLAY_MANAGER_OFF` | `GMATCH + 0x388` | `GMATCH + 0x388` | Pointer to the active Replay Manager of the match |
| Replay Manager known fields | `+0x00` (state), `+0x0C` (seq), `+0x58` (source-current) | Same | State and sequence of the native Fox Engine replay |
| `SOURCE_RECORD_SITE` | `0x140423974` | `0x1404230D4` | Replay recording bookmark hook (`return = 0x140423984`, `writer = 0x140A8A4C0`) |
| `native_get_scorer` call site | `0x140478C3F` | `0x14047826F` | Native goal event flow (`call native_get_scorer`, `mov ebx, eax`, `call native_get_context` at `0x140478C46`, `mov rdx, [rax+0x438]`) |

**Critical note on `GMATCH+0x388`:** as documented in `goal_pyro.lua` itself, the Fox Engine temporarily clears `GMATCH+0x388` during certain camera transitions. Runtime reads must always validate the pointer with `VirtualQuery` / a range check before dereferencing.

## 2. How Patches Do Hooks and UI in Sider 7.3.3

There are two main proven patterns in the `modules\` folder of a full patch:

### Pattern A: Pure LuaJIT + FFI with an x64 trampoline (`goal_pyro.lua`, `PESCustomCelebrationsReload.lua`)
- Uses LuaJIT `ffi` with `VirtualAlloc(nil, PAGE_SIZE, 0x3000, 0x40)` and `VirtualProtect` to install x64 detours directly in the memory of `PES2021.exe`.
- Pros: a single `.lua` file, zero extra compilation.
- Cons: very limited for rich UI (only the monospaced text of the Sider overlay via `overlay_on`), complex x64 code is hard to maintain in hex strings, and LuaJIT has a limit of 200 local variables per scope.

### Pattern B: Sider Lua module as controller + native x64 DLL (championship control and initialization DLLs)
- A lightweight `.lua` in Sider registers events (`init`, `set_teams`, `overlay_on`, `key_down`, `gamepad_input`) and loads a C++ x64 DLL via `ffi.load(...)`.
- The C++ x64 DLL handles:
  - Safe x64 hooks (MinHook or structured C++ trampolines with a mutex/lock-free ring buffer).
  - A hook of `IDXGISwapChain::Present` (DirectX 11) to render a **native VAR broadcast visual interface with Dear ImGui** inside the game's Replay/Pause screen, with full Gamepad (XInput) and Keyboard control.
  - Persistent state for recording refereeing decisions during the match, exposed via FFI to the Sider module.
- **Assessment for the VAR Mod:** Pattern B (Sider Lua for on/off, config and lifecycle + C++ x64 DLL for refereeing/replay hooks and DX11 HUD inside the Replay) is drastically superior in stability, maintainability and visual quality.

## 3. Relevant Sider 7.3.3 Events and API

Verified in `ReplayCam.lua`, `goal_pyro.lua` and `doc\scripting.html`:
- `overlay_on`: Sider text menu (`Espaço`) to enable/disable the mod, configure shortcuts, display mode and diagnostics.
- `key_down` / `key_up`: keyboard capture.
- `gamepad_input`: controller capture in Sider.
- `set_teams`: fires when each match is prepared (ideal for clearing the VAR play history and revalidating `GMATCH` pointers).
- `memory.read`, `memory.write`, `memory.search`: native Sider utilities for AOB and memory.

## 4. Patch Pattern for Installation and Activation/Deactivation

Verified in `<patch folder>/GoalPyro_Opcional.ps1` and `<patch folder>/Comemoracoes_Opcional.ps1`:
- The package provides `VAR_Mod_Gerenciar.bat` + `VAR_Mod_Gerenciar.ps1` to install, enable, disable or remove the module (`luajit.ext.enabled = 1` and `lua.module = "var_mod.lua"` in the `sider*.ini` files with Latin-1 encoding `28591`), checking that `PES2021.exe` is closed and creating a dated backup before editing.
- Additionally, in game, the user can toggle the VAR in real time through the Sider menu (`overlay_on`).
