<div align="center">

<img src="docs/images/hero_en.png" alt="VAR for PES 2021: pause, review, decide. The game obeys." width="100%">

![Version](https://img.shields.io/github/v/release/Joao-Spala/pes21-var-mod?style=for-the-badge&label=Version&color=38BDF8)
![PES 2021](https://img.shields.io/badge/eFootball_PES_2021-v1.7.2.0_(Steam)-0095D9?style=for-the-badge)
![Sider](https://img.shields.io/badge/Sider_7.3+-Compatible-F59E0B?style=for-the-badge)
![Native](https://img.shields.io/badge/Native-C%2B%2B20_%2B_DX11_%2B_ImGui-84CC16?style=for-the-badge)
![Languages](https://img.shields.io/badge/Languages-PT--BR_%7C_English-2EA043?style=for-the-badge)
![License](https://img.shields.io/badge/License-MIT-8B5CF6?style=for-the-badge)

**English** · [Português (BR)](README.pt-BR.md) · [Support](#support-the-project)

### [Download the latest release](https://github.com/Joao-Spala/pes21-var-mod/releases/latest)

[![Ko-fi](https://img.shields.io/badge/Ko--fi-Buy_me_a_coffee-FF5E5B?style=for-the-badge&logo=ko-fi&logoColor=white)](https://ko-fi.com/AlfaceJao)

</div>

---

## Overview

VAR for PES 2021 is a video review module for offline matches of eFootball PES 2021 (Steam `v1.7.2.0`). It adds a review panel to the Manual Replay. Decisions confirmed there are applied by the game engine itself: restarts, cards and sendings off follow the engine's own flow.

It ships as a Sider module: a native x64 DLL and a Lua script.

## Demo

<p align="center"><img src="docs/images/en/replay.gif" alt="Review panel following the replay" width="450"></p>

<p align="center"><sub>Review panel during the Manual Replay. The tactical pitch follows the replay frame by frame and the gold marker indicates the play with the highest missed-foul score.</sub></p>

## Features

| Area | Description |
| --- | --- |
| **Incident detection** | The match is sampled at 60 Hz. Slide tackles, standing tackles, shoulder charges, falls and simulation are marked on the replay bar together with the referee's fouls, cards and advantages, each with offender, fouled player, location and zone. |
| **Missed-foul indicator** | Plays the referee did not penalize are scored by challenge type, whether the opponent went down from the contact, location relative to the box and contact distance. The highest score is highlighted on the replay bar and labelled on the incident card. |
| **Decisions** | Award a foul or penalty on an earlier play, overturn a foul or penalty, upgrade to penalty, reverse a foul, edit any player's cards and award a corner, goal kick or throw-in. |
| **Decision pool** | One play decision and up to 32 card edits, applied together and in order when leaving the replay. |
| **Engine integration** | Decisions go through the match state machine (`MatchFSM`) and the engine's own card commit. If the engine does not set up the restart, every change made on that tick is rolled back. |
| **Replay after the decision** | For a foul or penalty given with the ball in play, the engine replays the reviewed moment after the stoppage cutscenes, through its own pending clip mechanism. Can be disabled. |
| **Tactical pitch** | Automatic framing (play or penalty box), shirt numbers, heading arrows, 1.5 s trails for the two players involved and the ball, attack direction per team, offside line and restart location. |
| **Replay camera** | Selecting a play on the replay bar moves the 3D replay camera to that frame. |
| **Player identity** | Names, numbers and kits are read from game memory, including patch-edited data. Cards and fouls follow the player, not the position, across substitutions. |
| **Interface** | PT-BR and English. Scales from 720p to 4K, ultrawide and 4:3, in three sizes. Keyboard and mouse inside the panel. |

## Detail

<table>
<tr>
<td width="50%"><img src="docs/images/en/feat_timeline.png" alt="Replay bar"></td>
<td><b>Replay bar.</b> Markers for official decisions and detected incidents. The gold marker is the missed-foul indicator.</td>
</tr>
<tr>
<td><img src="docs/images/en/feat_card.png" alt="Incident card"></td>
<td><b>Incident card.</b> Incident type, distance to the box, offender (orange) and fouled player (cyan).</td>
</tr>
<tr>
<td><img src="docs/images/en/feat_pitch.png" alt="Tactical pitch"></td>
<td><b>Tactical pitch.</b> Positions at the replay frame, heading arrows, trails, offside line and role labels.</td>
</tr>
</table>

## Workflow

```mermaid
flowchart LR
    A["Pause the match"] --> B["Open the<br/>Manual Replay"]
    B --> C["Select a play<br/>on the replay bar"]
    C --> D["Confirm<br/>the decision"]
    D --> E["Leave the replay:<br/>the engine applies it"]
```

The panel opens automatically in the Manual Replay. Detected plays come with offender, fouled player, location and zone already filled in and can be adjusted before confirming. Confirmed decisions are stored in the decision pool and applied when the replay is closed.

## Screenshots

<table>
<tr>
<td align="center" width="33%"><img src="docs/images/en/revisao.png" alt="Review"><br><b>Review</b><br><sub>Replay bar, incident card and tactical pitch.</sub></td>
<td align="center" width="33%"><img src="docs/images/en/decisao.png" alt="Decision"><br><b>Decision</b><br><sub>Card, restart team, player and restart location.</sub></td>
<td align="center" width="33%"><img src="docs/images/en/pool.png" alt="Decision pool"><br><b>Decision pool</b><br><sub>Pending decisions, applied when leaving the replay.</sub></td>
</tr>
<tr>
<td align="center"><img src="docs/images/en/cartoes.png" alt="Cards"><br><b>Cards</b><br><sub>All 22 players with names and numbers.</sub></td>
<td align="center"><img src="docs/images/en/opcoes.png" alt="Options"><br><b>Options</b><br><sub>Panel, match and language settings.</sub></td>
<td align="center"><img src="docs/images/en/controles.png" alt="Controls"><br><b>Controls</b><br><sub>Two keys per action.</sub></td>
</tr>
</table>

## Requirements

| Item | Version |
| --- | --- |
| Game | eFootball PES 2021 `v1.7.2.0` (Steam). Other executable versions are not supported. |
| Sider | 7.3 or later with LuaJIT, standalone or inside a patch |
| System | Windows 10 or 11, x64 |

## Installation

Close the game, download **`PES2021_VAR_Mod_v4.4.12.zip`** from [Releases](https://github.com/Joao-Spala/pes21-var-mod/releases/latest) and extract it. Then install it by hand (option A) or with the setup script (option B). Both give the same result.

### Option A: manual installation

1. Find your **Sider folder**: the folder that contains `sider.dll`. With a patch it is usually the patch's own folder. With standalone Sider it is wherever you extracted Sider.
2. Copy the package's `modules` folder into the Sider folder, so that you end up with:
   ```text
   <Sider folder>\modules\var_mod.lua
   <Sider folder>\modules\var_mod\var_core.dll
   ```
3. Make a copy of `<Sider folder>\sider.ini`, then open it in a text editor and make sure these three lines are present. If `lua.enabled` or `luajit.ext.enabled` already exist with `= 0`, change them to `= 1`. Put the `lua.module` line next to any other `lua.module` lines. If the folder also has `sider-<name>.ini` variants that a launcher switches between, do the same in each one.
   ```ini
   lua.enabled = 1
   luajit.ext.enabled = 1
   lua.module = "var_mod.lua"
   ```
4. Start the game through Sider (or through the patch launcher). The mod loads on the first match.

To **disable** the mod, put a `;` at the start of the `lua.module = "var_mod.lua"` line. To **uninstall**, delete that line, `modules\var_mod.lua` and the `modules\var_mod\` folder (your settings are in `modules\var_mod\var_mod.ini`).

### Option B: setup script

Run **`VAR_Mod_Setup.bat`** and choose **`1 - Install or update`**. It runs `VAR_Mod_Setup.ps1`, a plain-text PowerShell script you can open in Notepad before running. Its header lists everything it does: it finds the Sider folder (or asks for it), copies the two files, edits `sider.ini` and its `sider-<name>.ini` variants as in option A after saving a dated backup of each one, and changes nothing while the game is running. No network access and no registry changes.

| Option | Action |
| --- | --- |
| **1 - Install or update** | Copies the files and enables the mod in `sider.ini` |
| **2 - Enable** | Writes the same `sider.ini` lines without copying files |
| **3 - Disable** | Comments out the mod's line in `sider.ini` and keeps the files |
| **4 - Uninstall** | Saves your settings as `var_mod.ini.before-uninstall-<date>.bak` next to `sider.ini`, then removes the mod's line, `modules\var_mod.lua` and the `modules\var_mod\` folder. `lua.enabled` and `luajit.ext.enabled` stay at 1 |

## Options

Settings are saved in `<sider folder>\modules\var_mod\var_mod.ini`.

| Option | Default |
| --- | --- |
| Open in replay | On |
| Tactical pitch | On |
| Auto box zoom | On |
| Position | Right |
| Size (Compact, Default, Large) | Default |
| Live alerts | On |
| Replay the play after the decision | On |
| Language | Português (BR) |
| Controls | Two keys per action, remappable |

## Shortcuts

| Action | Keyboard | Gamepad |
| --- | --- | --- |
| Open or collapse the panel | `V` or `F7` | `R3` |
| Confirm / Back | `Enter` / `Esc` or `Backspace` | |
| Previous / next tab | `Page Up` or `Q` / `Page Down` or `E` | |
| Previous / next incident | `[` / `]` | |
| Swap roles or switch team | `Tab` | |
| Replay play / pause | `P` | |
| Navigate | Arrow keys or `W` `A` `S` `D` | |
| Clear the pool | `Delete` | |
| Telemetry | `F10` | |

All shortcuts can be changed in **OPTIONS > Controls**. While the panel has focus, keyboard and mouse input does not reach the game. In the Sider menu, `7` toggles the mod and `8` shows live telemetry.

## Limitations

- Supports only the Steam executable `v1.7.2.0`.
- Offline matches only: Exhibition, Master League, Become a Legend and Cups. Network, DRM and online modes are not touched.
- A decision applies to the stoppage in which it was made. Goals cannot be overturned.
- The replay after the decision requires a foul or penalty given with the ball in play and the reviewed moment still in the game's replay memory (about 11 s of play). The engine's own instant replay may play first.
- The gamepad opens and collapses the panel and controls the replay. Navigation inside the panel uses keyboard and mouse.

## Troubleshooting

| Symptom | Action |
| --- | --- |
| Sider menu shows `Status: ERROR (...)` | The DLL did not load. Check that `var_core.dll` is in `<sider folder>\modules\var_mod\`, that `lua.enabled = 1` and `luajit.ext.enabled = 1` are set in `sider.ini`, and check `sider.log`. |
| The panel does not open in the replay | Confirm the match is offline and the mod is enabled (`7` in the Sider menu). `V`, `F7` or `R3` reopen a collapsed panel. |
| No replay after a decision | It applies only to fouls and penalties given with the ball in play and can be disabled in **OPTIONS**. Otherwise a `Decision replay: ...` line in the log gives the reason. |
| Unexpected behaviour in a match | The log is `<sider folder>\modules\var_mod\var_core.log` and is recreated at every game start. Copy it before restarting the game and attach it to an [issue](https://github.com/Joao-Spala/pes21-var-mod/issues). |
| Reset settings | Delete `<sider folder>\modules\var_mod\var_mod.ini`. |

---

## Technical overview

```mermaid
sequenceDiagram
    participant P as Panel
    participant Pool as Pool
    participant X as Executor
    participant E as Engine (MatchFSM)
    participant R as Engine replay
    P->>Pool: Confirm decision
    X->>Pool: At the matching stoppage
    X->>E: Referee result
    E->>E: Restart set-up and card commit
    X-->>E: Verification on the same tick
    Note over X,E: Rolled back if the restart was not set up
    X->>R: Pending clip of the reviewed moment
    R->>R: Played after the stoppage cutscenes
```

| | |
| --- | --- |
| **Binary** | C++20 x64 DLL, about 780 KB. One render hook on `GrSwapChain::Present0`, no additional DX11 device, no UI work outside the replay. |
| **Cost** | Measured on the real code paths in a benchmark: 3 to 6 µs per simulation tick and 2 µs per frame. Match history of 30 s (about 0.87 MB). |
| **Safety** | Hooks are installed with the other threads suspended. Writes go only to validated structures. Includes a fix for an engine allocator fault that crashed the game after the final whistle. |
| **Tests** | 23 suites, including contract checks against the real executable and panel renders at 7 resolutions in both languages, also run under AddressSanitizer and UndefinedBehaviorSanitizer. |

Reverse engineering notes are in [`re_notes/`](re_notes). Release history is in [`CHANGELOG.md`](CHANGELOG.md).

## Build and tests

Requires **LLVM-MinGW UCRT x86_64** (`x86_64-w64-mingw32-clang++`). The build compiles the DLL, runs the tests and then stages the package in `dist/`.

```powershell
powershell -ExecutionPolicy Bypass -File native\build.ps1
```

With `PES2021_EXE` set, the tests check signatures and engine contracts against the executable. With `VAR_HUD_SNAPSHOT_DIR` set, they render the panel at 7 resolutions in both languages (and the replay sequence with `VAR_HUD_SEQUENCE`). `docs/assets/make_readme_art.py` builds the images of this README from those renders.

```powershell
$env:PES2021_EXE = "C:\Program Files (x86)\Steam\steamapps\common\eFootball PES 2021\PES2021.exe"
$env:VAR_HUD_SNAPSHOT_DIR = "$env:TEMP\var_hud"
$env:VAR_HUD_SEQUENCE = "1"
native\build_tmp\test_var_core.exe
python docs\assets\make_readme_art.py "$env:TEMP\var_hud"
```

---

## Support the project

The mod is free and open source. Donations are welcome and optional. Bug reports are equally useful.

[![Ko-fi](https://img.shields.io/badge/Ko--fi-Buy_me_a_coffee-FF5E5B?style=for-the-badge&logo=ko-fi&logoColor=white)](https://ko-fi.com/AlfaceJao)

A Pix QR code is available in the [Portuguese README](README.pt-BR.md#apoie-o-projeto).

---

<div align="center">

Independent project, not affiliated with Konami. No game files are distributed or modified.

[MIT](LICENSE) · Includes [Dear ImGui](https://github.com/ocornut/imgui) ([MIT](native/third_party/imgui/LICENSE.txt))

</div>
