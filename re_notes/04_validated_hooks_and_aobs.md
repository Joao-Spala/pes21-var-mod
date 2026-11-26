# 04. Validated Hook Sites, AOB Signatures and Sider/Patch Collision Matrix

All 17 AOBs and hook sites below were validated in `PES2021.exe` (Steam 1.7.2.0) using `pes2021-re:verify_hook_site` and `decompile_function`, confirmed by an automated test on the real binary (`test_var_core.exe`) and cross-checked against the active modules of Sider 7.3.3 and of a full patch.

## 1. Addresses Occupied by Patch Modules (Forbidden for Hooks)

Extracted directly from `<patch folder>\modules\goal_pyro.lua` and `PESCustomCelebrationsReload.lua`:

| VA | Patch module | Use |
|---|---|---|
| `0x140423974` | `goal_pyro.lua` | `SOURCE_RECORD_SITE` inside `MatchControl::vfunc[6]` (`0x1404238F0`) |
| `0x140478C3F` | `goal_pyro.lua` | `native_get_scorer` call site |
| `0x140478C52` | `goal_pyro.lua` | `scorerpost` hook |
| `0x141AC7214` | `goal_pyro.lua` | `SmokeBomb2` hook |
| `0x141AC9050` | `goal_pyro.lua` | Function audited as the canonical original |
| `0x141E2xxxx..0x142xxxxxx` | `PESCustomCelebrationsReload.lua` | Celebration tables |

**VAR Mod v4.0 collision status:** `0` collisions with any patch or Sider module.

---

## 2. Final Native Hook Architecture (`var_core.dll` v4.0)

To eliminate any fragility from volatile registers and guarantee instant startup (zero static imports of `d3d11.dll` / `dxgi.dll`, zero dummy DX11 device), v4.0 uses exclusively Microsoft x64 ABI function-entry hooks with atomic writes (`InterlockedExchange64` / `cmpxchg16b`) and prior trampoline publication with a memory barrier.

### Hook 1: `MatchFSM::MainTick` (60 Hz Monitoring, 10 Hz Snapshots and Execution of Scenarios 1, 2 and 4)

- **VA / RVA:** `0x1404468F0` / `0x4468F0`
- **Unique AOB (30 bytes):**
  `48 89 5C 24 20 4C 89 44 24 18 48 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 60`
- **Stolen bytes (5 bytes, 0 NOPs):** `48 89 5C 24 20` (`mov qword ptr [rsp + 0x20], rbx`)
- **Sider/patch conflicts:** none (`[]`)
- **Role in the VAR Mod:** captures `MatchFSM*`, resolves `GMATCH` (`0x143702208`) at `MainTick + 0x30`, runs physical detection at 60 Hz (`PlayerAction + 0x01 / +0x08` and `BallState + 0x2601C`), records tactical snapshots at 10 Hz (90 s), synchronizes real names (`#N NAME`), shirt numbers, substitutions and kit colors (licensed and patch-edited) and applies the verdicts of Scenarios 1, 2 and 4.

### Hook 2: `Judge::FinalizeFrame` (Native Injection of Scenario 3 in `InPlay`)

- **VA / RVA:** `0x14050CD70` / `0x50CD70`
- **Unique AOB (17 bytes):**
  `33 C0 48 89 81 B8 03 00 00 48 89 81 C0 03 00 00 C3`
- **Stolen bytes (9 bytes, 4 NOPs, `cmpxchg16b` on a 16-byte aligned address):** `33 C0 48 89 81 B8 03 00 00`
- **Sider/patch conflicts:** none (`[]`)
- **Role in the VAR Mod:** injects `RawSetPlayInfo` + `RawFoulInfo` into `MatchFSM + 0x49C0` / `+0x49F0` during `InPlay` (`0x1C`) for retroactive fouls and penalties.

### Hook 3: `MatchControl::UpdateFoulCounters` (Official Audit and Anti-Duplication)

- **VA / RVA:** `0x140424410` / `0x424410`
- **Unique AOB (16 bytes):**
  `41 80 B8 60 22 00 00 00 74 13 8B 82 44 01 00 00`
- **Stolen bytes (8 bytes, 3 NOPs):** `41 80 B8 60 22 00 00 00` (the trampoline fully preserves `EFLAGS`/`ZF`)
- **Sider/patch conflicts:** none (`[]`)
- **Role in the VAR Mod:** records official foul, penalty and card events (filtering offside) and suppresses double counting during VAR rebuilds.

### Hook 4: `ManualReplayController::SyncToReplayManager` (Native Seek, Play/Pause and 3D Camera Focus)

- **VA / RVA:** `0x1420C16A0` / `0x20C16A0` (vtable `0x142BDFD60`)
- **Unique AOB (20 bytes):**
  `40 53 55 56 57 41 54 41 56 41 57 48 83 EC 40 48 8B 99 30 04`
- **Stolen bytes (5 bytes, 0 NOPs, zero RIP-relative):** `40 53 55 56 57` (`push rbx`, `push rbp`, `push rsi`, `push rdi`)
- **Sider/patch conflicts:** none (`[]`)
- **Role in the VAR Mod:** runs on the Replay thread itself, immediately before mirroring the state into `ReplayManager`, executing commands queued by the interface:
  - **Native seek:** finds the exact frame in the circular window `[winStart..winEnd]` of the `ReplaySingleton` (`0x143703558`, `+0x6A8 -> +0x40`, stride `0x1FB0`) matching the match half, calls `ReplayPlayer::SetFrame` (`0x14217E800`), optionally pauses (`ctrl + 0x5C = 0`, `ctrl + 0x310 = 0.0f`) and updates the 3D camera `LookAtPos`.
  - **Play/Pause:** toggles `ctrl + 0x5C` (`0`/`1`) and `ctrl + 0x310` (`0.0f`/`1.0f`).
  - **3D Camera Focus:** toggles `ctrl + 0x64D` (`FocusSelector`), `ctrl + 0x80` (`ReplayPlayer + 0x30`), leaves Free Cam if `ctrl + 0x64C == 3` and updates `ReplayCamera` (`+0x18 -> +0x00 / +0x44`) via `ReplayPlayer::GetFocusPos` (`0x14217EC70`).

### Hook 5: `GrSwapChain::Present0` (Native DX11 Overlay)

- **VA / RVA:** `0x141FF46A0` / `0x1FF46A0`
- **Unique AOB (14 bytes):**
  `48 8B 49 20 45 33 C0 48 8B 01 48 FF 60 40`
- **Stolen bytes (7 bytes):** `48 8B 49 20 45 33 C0`
- **Role in the VAR Mod:** intercepts the real `IDXGISwapChain*` at `[rcx + 0x20]`, with a lock-free fast path outside the Replay and a transient per-frame RTV (compatible with `ResizeBuffers`, Alt+Tab, Sider and Steam Overlay).

### Input Hooks in the Manual Replay (Keyboard/Mouse Blocking and `R3` Shortcut on the Controller)

- **`DInputKeyboard_Poll` (`0x141B638B0`, 5 bytes):** zeroes the 256-byte buffer at `[self + 0x268]` while the VAR panel is focused or during the 260 ms grace window.
- **`DInputMouse_Poll` (`0x141B63950`, 5 bytes):** zeroes deltas and buttons in `DIMOUSESTATE` (`[self + 0x268]`) while the VAR panel is focused.
- **`DInputJoystick_Poll` (`0x141B63900`, 5 bytes) and `PesPad_Update` (`0x1403F86E0`, 8 bytes):** pass the original state through without zeroing the gamepad, keeping the controller free to operate the 3D Replay while keyboard and mouse operate the panel.
- **IAT Hooks (`xinput1_3.dll` ordinal `2` and `user32.dll!GetKeyboardState`):** read the configurable controller shortcut (`R3` to open/collapse the panel) and block `GetKeyboardState` in the game while the panel is focused without affecting the Sider overlay.

---

## 3. Native Functions and Globals Resolved by AOB (17/17 Validated)

| Internal Symbol | VA | RVA | Use |
|---|---|---|---|
| `MatchFSM::MainTick` | `0x1404468F0` | `0x4468F0` | Main match hook + `GMATCH` (`+0x30` -> `0x143702208`) |
| `Judge::FinalizeFrame` | `0x14050CD70` | `0x50CD70` | Scenario 3 hook in `InPlay` |
| `MatchControl::UpdateFoulCounters` | `0x140424410` | `0x424410` | Official foul/card audit hook |
| `ManualReplayController::SyncToReplayManager` | `0x1420C16A0` | `0x20C16A0` | Native Seek/Play/Camera hook + `ReplaySingleton` (`+0x32` -> `0x143703558`) |
| `ReplayPlayer::SetFrame` | `0x14217E800` | `0x217E800` | Circular frame jump in the 3D Replay |
| `ReplayPlayer::GetFocusPos` | `0x14217EC70` | `0x217EC70` | 3D position of the ball (0) or a player (1..22) at the current frame |
| `MatchFSM_EnterOutOfPlay` | `0x1404541A0` | `0x4541A0` | Enter a full stoppage (cutscene + setplay) |
| `MatchFSM_RebuildSetPlay` | `0x140454280` | `0x454280` | Rebuild a clean setplay without a cutscene |
| `MatchInfo_GetTimeFloat` | `0x140A3C680` | `0xA3C680` | Current half time as a 60 Hz float |
| `MatchInfo_SetPlayerOnPitch` | `0x140A3D0E0` | `0xA3D0E0` | Update `MatchInfo + 0x1748[slot]` |
| `TeamInfo_GetPlayerBySlot` | `0x140A3B5B0` | `0xA3B5B0` | Get `PlayerRecord*` via a rel32 call at `0x1404231DC + 6` |
| `MatchRosterGetter` | `0x1414BC550` | `0x14BC550` | Resolve `MatchRoster*` (`0x143714240`: names, shirts, teams and kits) |
| `GrSwapChain::Present0` | `0x141FF46A0` | `0x1FF46A0` | Native DX11 hook without a dummy device |
| `DInputKeyboard_Poll` | `0x141B638B0` | `0x1B638B0` | Native keyboard blocking in the Replay when the panel is focused |
| `DInputJoystick_Poll` | `0x141B63900` | `0x1B63900` | Clean DInput joystick pass-through for free Replay control |
| `DInputMouse_Poll` | `0x141B63950` | `0x1B63950` | Native DInput mouse blocking in the Replay when the panel is focused |
| `PesPad_Update` | `0x1403F86E0` | `0x3F86E0` | Unified gamepad pass-through keeping XInput/DInput active |
