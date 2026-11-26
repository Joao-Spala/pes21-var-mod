# 03. Replay Manager, Pause State, Patch Menu and DX11 Integration

Technical mapping of the Replay subsystem (`GMATCH + 0x388`), the Manual Replay screen (`Replay@Screen@match2D`), the Pause menu (`MatchPause@menu`) and the interaction with the patch ecosystem and Sider 7.3.3.

## 1. Verified Layout of `ReplayManager` (`*(GMATCH + 0x388)`)

Confirmed by decompiling `Replay@Screen@match2D::Init` (`0x1420F3D40`), `Replay@Screen@match2D::Update` (`0x1420F4580`), `ObserverReplay` (`0x140A14CE0`) and the getters `0x140A86C40..0x140A86C80`:

| Offset | Type | Field | Verified semantics |
|---|---|---|---|
| `+0x00` | `uint8_t` | `ReplayMode` | `0` = inactive. `1..4` = replay active (`CheckReplayPlayback` `0x140A86C80`: `(mode - 1) <= 3`). Getters: `0x140A86C50` (`==1`), `0x140A86C70` (`==2`), `0x140A86C40` (`==3`), `0x140A86C60` (`==4`) |
| `+0x07` | `uint8_t` | `ManualReplayScreenFlag` | **`!= 0` while the interactive Manual Replay screen is active.** `0x1420F4580` line 47 closes the screen if `*(+0x07) == 0` or `*(+0x00) == 4` |
| `+0x0B` | `uint8_t` | `UiHiddenOrFreeCam` | `!= 0` hides the name bar (`nameBarVisible_off` at `0x1420F4580` line 59) |
| `+0x0A` | `uint8_t` | `IsPlaying` | `1` = playing, `0` = paused (copied from `ReplayController + 0x5C` at `0x1420C1827`) |
| `+0x10` | `float` | `PlaybackSpeed` | Current playback speed (copied from `ReplayController + 0x310`) |
| `+0x14` | `uint32_t` | `TimelinePos` | Current relative frame (`(cur - start + cap) % cap`, written by `mov dword ptr [rbx+0x14], eax` at `0x1420C1788`) |
| `+0x18` | `uint32_t` | `TimelineDuration` | Total frames of the window (`(end - start + cap2) % cap2`, written by `mov dword ptr [rbx+0x18], eax` at `0x1420C17AA`) |
| `+0x1C` | `int32_t` | `CameraFocusSlot` | `0` = Ball, `1..11` = Home players, `12..22` = Away players (`0x1420F4580` lines 110-135) |
| `+0x20` | `uint64_t` | `CameraFocusPlayerId` | High dword (`+0x24`) holds the `PlayerID` used to look up the name shown in `name_str` |
| `+0x48` | `float` | `WindowStartTime` | Start of the replay window in 60Hz ticks of the current half |
| `+0x4C` | `int32_t` | `WindowStartPhase` | `MatchPhase` of the window start |
| `+0x50` | `float` | `WindowEndTime` | End of the replay window in 60Hz ticks |
| `+0x54` | `int32_t` | `WindowEndPhase` | `MatchPhase` of the window end |
| `+0x58` | `float` | `CurrentCursorTime` | **Current real-time Replay cursor** (60Hz ticks of the current half), updated during scrub, slow-motion and rewind (`0x1420F4580` lines 204-219) |
| `+0x5C` | `int32_t` | `CurrentCursorPhase` | **`MatchPhase` of the current Replay cursor** |
| `+0x60/+0x64` | `float/int32` | `SubRangeStart` | Start of the sub-range visible on the bar |
| `+0x68/+0x6C` | `float/int32` | `SubRangeEnd` | End of the sub-range visible on the bar |

### 1.1. Exact Synchronization Between the Replay Cursor and the VAR Ring Buffer

Verified at `0x1420C6760` and `0x140A6C3B0`:
- `0x140A6C3B0` converts a `float` via `cvttss2si` and divides by `60` (`mul 0x88888889; shr edx, 5`) to get total seconds, then divides by `60` again to get minutes and seconds.
- `0x1420C6760` adds `+45` minutes for `MatchPhase` `2/3` (2nd half), `+90` for `4/5` (1st extra time period) and `+105` for `6/7` (2nd extra time period).
- `MatchInfo_GetTimeFloat` (`0x140A3C680(MatchInfo + 0x1168)`) returns a `float` **in the exact same 60Hz unit as the current half**.
- **Result:** By recording every event and every running-game snapshot with the key `(MatchPhase, MatchTimeFloat, MatchTick)`, the mod synchronizes the Replay cursor (`+0x5C`, `+0x58`) with the recorded history to **1 frame (~16.6 ms)** precision, with no approximate heuristics.

### 1.2. Exact Condition for Detecting the Manual Replay

To distinguish a **Manual Replay** (triggered by the user through the pause menu) from an **Automatic Replay** (goal/foul/highlight):
- **Primary condition, verified and validated live:**
  `ReplayMgr != nullptr && ReplayMgr->Mode >= 1 && ReplayMgr->Mode <= 3 && ReplayMgr->ManualReplayScreenFlag (+0x07) != 0`
- In automatic goal/foul replays (`Mode == 1`, `ManualReplayScreenFlag == 0`) and post-match highlights (`Mode == 4`), the panel stays hidden.

---

## 2. Analysis of `MatchPause@menu` and Patch Menus

### 2.1. Evidence in the Binary (`MatchPause@menu` vtable `0x1425BCDB8`)
- `MatchPause@menu::vfunc[1]` (`0x140E3DFC0`) and `vfunc[2]` (`0x140E40ED0`) manage menu window events (`0x1080001..0x1080009`) driven by flow scripts (`common\script\flow\Match\MatchPrePause.json` and `MatchPostPause.json`) and binary layout files (`common\menu\general\game2dReplay.bin` and pause screens).
- Sider documents in `doc\scripting.html` that these flows go through `livecpk_data_ready`.

### 2.2. Evidence in an Installed Patch (`modules\MenuServer.lua`)
Direct inspection of `<patch folder>\modules\MenuServer.lua` (lines 9 and 104-118) and the patch folders:
- `MenuServer.lua` dynamically intercepts and replaces, per competition (`map_competitions.txt`), all files under:
  - `common\menu\system`
  - `common\menu\general`
  - `common\script\flow`
- The patch also includes `livecpk\Estilos de Menus` and `modules\MOD_MENU_SEM_LOBBIES.lua`.

### 2.3. Technical Verdict: Button in the Pause Menu vs Direct Integration in the Replay Screen
- **Injecting an extra button into the native Pause Menu: NOT RECOMMENDED.**
  - Patching menu `.bin`/`.json` files would conflict directly with dozens of `Servidor de Menus` and `Estilos de Menus` themes of a full patch, breaking patch updates and per-competition layouts.
  - Hacking the internal item vector at `MatchPause@menu+0x88` requires rebuilding focus handlers, text localization and the `ProcessMatchReplay` transition (`0x1412FF730`), adding a real risk of a pause menu soft-lock with no functional gain, since the user still has to open the Replay to see the play.
- **Integrating directly into the Manual Replay Screen: RECOMMENDED (100% safe and native).**
  - Natural flow: the user pauses, enters the native **Replay**, the VAR panel opens automatically (or collapses/reopens via `V`, `F7` or `R3`), navigates the plays with the native `Seek` and picks the verdict by keyboard/mouse while the controller keeps operating the Replay camera, then leaves the Replay normally to apply the decision on the pitch.
  - Zero changes to any patch menu file or game CPK.

---

## 3. Coexistence with Sider 7.3.3 (DX11 Overlay and Input)

Inspection of the imports and strings of `<patch folder>\sider.dll`:
- **Graphics:** Sider 7.3.3 hooks `IDXGISwapChain::Present` to render its top overlay strip (`overlay.enabled = 1`).
- **Input:** Sider imports `XINPUT9_1_0.dll` (`XInputGetState`), `DINPUT8.dll` (`DirectInput8Create`) and `USER32.dll` (`SetWindowsHookExW`).
- **Initialization order and safe chaining in v4.0:**
  - `var_mod.lua` registers only lightweight callbacks at boot and loads `var_core.dll` on demand (`set_teams` / `overlay_on`), guaranteeing zero impact on game startup time.
  - `var_core.dll` hooks `GrSwapChain::Present0` directly (`0x141FF46A0`, reading the real `IDXGISwapChain*` at `[rcx + 0x20]`), without creating a dummy DX11 device, using precompiled DXBC shaders and a transient per-frame RTV (native support for resolution changes, Alt+Tab and fullscreen/borderless without a `ResizeBuffers` hook).
  - If the user opens the Sider overlay (`Espaco` / `overlay_on`), `var_mod.lua` sends a heartbeat to `var_core.dll` so it yields the keyboard immediately.
