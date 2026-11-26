# 01. GMATCH, MatchInfo, Refereeing and Cards (PES2021.exe Steam 1.7.2.0)

Technical reference document with addresses, RVAs, VTables and struct layouts verified by static decompilation (`pes2021-re` / Ghidra) and Capstone disassembly of the executable `<Steam>\steamapps\common\eFootball PES 2021\PES2021.exe` (`ImageBase = 0x140000000`).

> Verification status: addresses, offsets, hook prologues and the `MatchRoster` table layout were confirmed by static decompilation (`pes2021-re` / Ghidra) and validated live in memory with Cheat Engine against a running `PES2021.exe`.

## 1. Global Match Root (`GMATCH`)

- **Global pointer:** `GMATCH = *(uint64_t*)0x143702208` (RVA `0x3702208`)
- **Auxiliary match data pointer (player names / roster):** returned by `0x1414BC550()` (`*(ret + 0x50) + 0x35960`, verified at `0x1420F4580`)
- **Auxiliary replay singletons:** `0x143703558` and `0x143703568` (used by `Replay@match` and `MatchFSM`)

### Verified fields in `GMATCH`

| Offset | Type | Description | Evidence |
|---|---|---|---|
| `+0x18` | `void*` | Match settings / rules (`+0x1038` team flags) | `0x140452F00`, `0x1420D46A0` |
| `+0x40` | `Ball*` | Ball object (`0x140A57ED0` returns the position `Vec3*`, `+0x27B8` previous position, `+0x2920` state) | `0x14050F120`, `0x140445740`, `0x14050EE50` |
| `+0x180` | `PlayerStatus*` | Array of 24 entries (`0x18`), stride `0x74` (human and CPU controllers/slots) | `0x140422A3F`, `0x140448D70`, `0x1405B8A10` |
| `+0x1A8` | `void*` | Transition/camera controller (`0x140A60D10`, `0x140A60C40`) | `0x140446DF0`, `0x14044C310` |
| `+0x270` | `MatchInfo*` | Public record of match state, set piece, foul and advantage | `0x140422A3F`, `0x1404530C0`, `0x140A141F0` |
| `+0x298` | `void*` | Pause controller / pause request (`0x140A67E60`) | `0x140444F90` |
| `+0x2C0` | `void*` | Competition rules (`+0x17` substitution/sending-off limit, `0x140A3FBB0`) | `0x140452F00`, `0x1405B7E80` |
| `+0x310` | `void*` | Penalty shootout controller (`0x140A6A9F0`) | `0x14050BE40`, `0x14050D5F0` |
| `+0x388` | `ReplayManager*` | Replay state, mode, camera focus and timeline | `0x1420F3D40`, `0x1420F4580`, `0x140A86C80` |
| `+0x3B0` | `void*` | Training / tutorial mode flag/controller (`0x140A436E0`) | `0x14044DB80`, `0x14050F120` |
| `+0x400` | `void*` | Special mode / demo replay controller (`0x140A59E20`) | `0x14044DB80`, `0x1404541A0` |
| `+0x1928` | `TeamTactics*[2]` | `GMATCH_GetTeamTactics`: `(idx + 0xA1) * 0x28` (`+0x254` attack direction `float` `+1.0/-1.0`) | `0x140A49020` |
| `+0x1978` | `TeamInfo*[2]` | `GMATCH_GetTeamInfo`: `(idx + 0xA3) * 0x28` (`+0x04` `int32_t[11]` active rosterIdx, `+0xA8 + idx * 0x128` `PlayerRecord`) | `0x140A48FC0`, `0x140A3B500` |
| `+0x19C8` | `PlayerAction*[22]` | `GMATCH_GetPlayerAction`: `(idx + 0xA5) * 0x28` (`+0x01` actionId: 13=Slide Tackle, 11/12=Standing Tackle, 15=Fall, 56/61=Charge and `+0x08` bit 17 = hit) | `0x140A48F00`, `0x1405B9400` |
| `+0x1DD8` | `PlayerSpatial*[22]` | `GMATCH_GetPlayerSpatial`: `(idx + 0xBF) * 0x28` (`+0x55C` Vec3 pos) | `0x140A49000` |
| `+0x25F8` | `PlayerEntity*[22]` | `GMATCH_GetPlayerEntity`: `(idx + 0xF3) * 0x28` | `0x140A48FE0` |
| `+0x2AA8` | `float*` | Pitch dimensions (`[0]` half width/length, `[1]` touchline, `[4..8]` penalty area/spot) | `0x14050F120`, `0x1405B8A10`, `0x14050CE40` |

### Native getters based on `GMATCH`

| Function | VA | RVA | Signature / Effect |
|---|---|---|---|
| `GMATCH_GetTeamInfo` | `0x140A48FC0` | `0xA48FC0` | `TeamInfo* (GMATCH*, int teamIdx)` (`0` = Home, `1` = Away) |
| `GMATCH_GetTeamTactics` | `0x140A49020` | `0xA49020` | `TeamTactics* (GMATCH*, int teamIdx)` (`+0x254` attack direction `float` `+1.0/-1.0`) |
| `GMATCH_GetPlayerEntity` | `0x140A48FE0` | `0xA48FE0` | `PlayerEntity* (GMATCH*, uint32_t slot)` (`slot 0..21`) |
| `GMATCH_GetPlayerAction` | `0x140A48F00` | `0xA48F00` | `PlayerAction* (GMATCH*, uint32_t slot)` |
| `GMATCH_GetPlayerSpatial` | `0x140A49000` | `0xA49000` | `PlayerSpatial* (GMATCH*, uint32_t slot)` (`+0x55C` Vec3 pos) |

---

## 2. Resolved VTables (MSVC x64 RTTI)

| RTTI Class | VTable VA | Relevant methods |
|---|---|---|
| `Referee@referee@match` | `0x14264AF18` | Visual actor of the on-pitch referee |
| `RefereeChief@referee@match` | `0x14264B190` | `[2]=0x140A36D90`, `[4]=0x140A37120`, `[8]=0x140A37A20` |
| `Judge@ai@match` | `0x1425CE1A0` | Embedded in `MatchFSM + 0x908`. Rules and foul logic |
| `MatchControl@match` | `0x1425A15A0` | `[6]=0x1404238F0`, `[8]=0x140424390`, `[9]=0x140422670` |
| `Match@match` | `0x14259FEA8` | Main match controller |
| `ObserverFoul@record@match` | `0x1426498F0` | `[1]=0x140A141F0` (reads `MatchInfo + 0x1584`, `+0x11BB`, `+0x135C`) |
| `ObserverPenaltyKick@record@match` | `0x142649940` | `[1]=0x140A14BA0` |
| `ObserverFreeKickKind@record@match` | `0x142649878` | `[1]=0x140A14350` (reads `MatchInfo + 0x1480`) |
| `ObserverRestart@record@match` | `0x142649918` | `[1]=0x140A14F40` (reads `MatchInfo + 0x11D4`) |
| `ObserverOutOfPlay@record@match` | `0x142649118` | `[1]=0x140A14AE0` (reads `MatchInfo + 0x1554`) |
| `ObserverReplay@record@match` | `0x142649990` | `[1]=0x140A14CE0` (reads `ReplayManager + 0x00`) |
| `ObserverTackle@record@match` | `0x14264A170` | `[1]=0x140A1B120` |
| `ModelYellowCard@Screen@match2D` | `0x142BEBD40` | `[8]=0x1420EAC40` (yellow card UI) |

---

## 3. `MatchInfo` Layout (`*(GMATCH + 0x270)`)

| Offset | Type | Field | Verified semantics |
|---|---|---|---|
| `+0x1168` | `int32_t` | `MatchPhase` | `0/1` = 1H (+0m), `2/3` = 2H (+45m), `4/5` = ET1 (+90m), `6/7` = ET2 (+105m), `8/9/10` = Penalties / End |
| `+0x116C` | `int32_t` | `MatchStatus` | `1` = `InPlay` (ball rolling), `2` = `OutOfPlay` (stoppage/cutscene), `3` = `SetPlayReady` (waiting for the kick) |
| `+0x1174` | `float` | `PhaseElapsedTime` | Base used by `0x140A3C680(MatchInfo + 0x1168)` (returns a float in 60Hz ticks of the current half) |
| `+0x1198` | `uint32_t` | `MatchTick` | Monotonic simulation frame counter (60Hz) |
| `+0x11B0` | `uint8_t` | `RestartSyncPhase` | Used in the synchronization of states `0x14` (`0x14`) and `0x15` (`0x15/0x16`) |
| `+0x11BB` | `uint8_t` | `FoulNotifyFlagA` | Read by `ObserverFoul` (`0x140A141F0`) |
| `+0x11D4` | `int32_t` | `RestartSubState` | `== 2` triggers `ObserverRestart` |
| `+0x135C` | `uint8_t` | `FoulNotifyFlagB` | Read by `ObserverFoul` (`0x140A141F0`) |
| `+0x1480` | `int32_t` | `FreeKickKind` | Foul type (direct/indirect), read by `ObserverFreeKickKind` |
| `+0x1544` | `16B` | `AdvantageInfo` | `+0x1548` and `+0x154C` verified by `CheckAdvantageData` (`0x14145F360`) |
| `+0x1554` | `0x30B` | `SetPlayInfo` | Main stoppage / set piece block (see table 3.1) |
| `+0x1584` | `0x3C B` | `FoulInfo` | Main foul / card block (see table 3.2) |
| `+0x15C0` | `0xCC B` | `GoalAndCardExtra` | Additional goal/scorer information (`+0x15C0..0x168B`) |
| `+0x1690` | `uint8_t` | `DemoSceneKind` | `0x13` = no special demo, used at `0x140444A80` and `0x1404530C0` |
| `+0x173C` | `int32_t[2]` | `KickerSlots` | Slots `0..21` of the designated kickers (primary/secondary) |
| `+0x1748` | `uint8_t[22]` | `PlayerOnPitch` | `1` = player active on the pitch, `0` = sent off/absent. Written via `0x140A3D0E0(MatchInfo, slot, bool)` |
| `+0x175E` | `uint8_t[22]` | `PlayerInjuredOff` | `1` = temporarily off the pitch due to injury (`0x14044B430`, `0x14044CA20`) |
| `+0x1775` | `uint8_t` | `SpecialModeKind` | `0` = normal match. Values `> 0` = training, tutorial or special scenarios |
| `+0x17AC` | `RingBuf` | `FoulSnapshotPool` | Pool of 3 snapshots of the positions of the 22 players (stride `0x3E4`, entry `0x2C`), ball (`+0x3CC`) and IDs at `+0x2358` (`0x17AC + 3 * 0x3E4`), indexed by `FoulInfo+0x28` (`0x1404B4B40` / `0x140A82120`) |

### 3.1. `SetPlayInfo` Layout (`MatchInfo + 0x1554` / `JudgeResult + 0x00`)

| Offset in `JudgeResult` | Offset in `MatchInfo` | Type | Field | Verified values |
|---|---|---|---|---|
| `+0x00` | `+0x1554` | `int32_t` | `SetPlayKind` | `0`=None, `1`=Kickoff, `2`=Throw-In, `3`=Corner, `4`=Goal Kick, `5`=Free Kick, `6`=Penalty Kick, `7`=Half-Time/Full-Time Whistle |
| `+0x04` | `+0x1558` | `int32_t` | `BenefitedTeam` | `0` = Home, `1` = Away |
| `+0x08` | `+0x155C` | `float[3]` | `SetPlayPosA` | 3D coordinate `(x, y, z)` written by `0x140A3D330` |
| `+0x14` | `+0x1568` | `float[3]` | `SetPlayPosB` | Mirror of the 3D coordinate written by `0x140A3D330` |
| `+0x20` | `+0x1574` | `int32_t` | `StoppageReason` | `0`=Kickoff/Init, `1`=End of Half, `2`=Quick/Out, `3`=Goal, `4`=Ball Out, `5`=Foul, `6`=RebuildSetPlay, `7`=Direct restart without cutscene, `8/9`=Shootout/Special, `10`=Special |
| `+0x28` | `+0x157C` | `uint32_t` | `EventPlayerSlot` | Slot `0..21` or `0xFF` |

### 3.2. `FoulInfo` Layout (`MatchInfo + 0x1584` / `JudgeResult + 0x30`)

| Offset in `JudgeResult` | Offset in `MatchInfo` | Type | Field | Verified values |
|---|---|---|---|---|
| `+0x30` | `+0x1584` | `uint8_t` | `IsValid` | `1` when there is a valid foul event (`0x1405B7E80`) |
| `+0x34` | `+0x1588` | `int32_t` | `FoulKind` | `1` = Offside, `2` = Standard Foul, `3` = Simulation/Unsporting (forces yellow), `4` = Handball/Indirect (`0x14050F640`) |
| `+0x38` | `+0x158C` | `int32_t` | `FoulJudgementKind` | `0`=None, `1`=Simple foul (no card), `2`=Verbal warning, `3`=**Yellow Card**, `4`=**Direct Red Card** |
| `+0x3C` | `+0x1590` | `int32_t` | `OffenderSlot` | Offender slot: `0..10` = Home, `11..21` = Away (`0xFF` if invalid) |
| `+0x40` | `+0x1594` | `int32_t` | `VictimSlot` | Victim slot: `0..21` (`0xFF` if invalid) |
| `+0x50` | `+0x15A4` | `uint8_t` | `WasAdvantageFoul` | `0` = immediate foul, `1` = foul whistled after a failed advantage (`0x14050BE40`) |
| `+0x51` | `+0x15A5` | `uint8_t` | `GoalOnAdvantage` | `!= 0` redirects the foul stoppage to the goal branch at `0x1404530C0` |
| `+0x58` | `+0x15AC` | `uint32_t` | `SnapshotTickIndex` | Snapshot index in the ring buffer `MatchInfo + 0x17AC` (`0x140A82070` / `0x140A82120`) |
| `+0x5C` | `+0x15B0` | `uint32_t` | `ContactCategory` | Contact category (`0..8`) written by `0x1405B7E80` |

---

## 4. Refereeing and Card Decision Pipeline

### 4.1. Flow in `Judge@ai@match` (`MatchFSM + 0x908`)

1. In `MatchFSM::State_InPlay` (`0x140449F40`), on every live frame it runs:
   - `0x14050BC60(Judge, MatchFSM + 0x49C0, ctx)` (binds the pointer `*(Judge + 0x3C0) = MatchFSM + 0x49C0`)
   - `0x14050CD90(Judge)` (`Judge::Evaluate`): calls `0x14050DFC0` and, during normal play, `0x14050BE40(Judge)`
   - `0x14050CD70(Judge)` (Judge frame finalization)
2. When a collision/foul event occurs (`0x0F` at `0x14050EE50`), it calls `0x1405B7E80(Judge, offenderSlot, victimSlot)` -> `0x1405B8A10`:
   - Classifies the pitch zone via `Pitch_ClassifyZone` (`0x140A73470(PitchDims, x, z, &attackDir)`):
     - **Bit 11 (`(flags >> 11) & 1`) == 1:** incident inside the penalty area -> `SetPlayKind = 6` (Penalty), coordinate obtained via `Judge_CalcPenaltySpot` (`0x14050DC00(outVec3, 6, PitchDims, posVec3)`).
     - **Bit 11 == 0:** incident outside the penalty area -> `SetPlayKind = 5` (Foul), keeping the contact coordinate `(x, y, z)`.
   - Stores the candidate in `Judge + 0x14` (immediate) or promotes it to `Judge + 0x58` (advantage active, countdown in `Judge + 0x0C`).
3. When the referee whistles the foul (`0x14050BE40`), it calls `0x14050F640(Judge, candidate)` -> `0x14050F120` (`Judge::CommitDecision`):
   - Fills `JudgeResult` (`MatchFSM + 0x49C0`) with `SetPlayInfo` and `FoulInfo`.
   - **Critical binary safety note:** `0x14050F640` and `0x14050F890` contain constants obfuscated via XOR in regions outside `.text` (`0x14BC236xx` / `0x143FA9xxx`). Never install hooks or patch inside these two functions. Always use the clean points in `.text` inside `MatchFSM` and `MatchControl`.

### 4.2. Official Card and Sending-Off Registry (`MatchControl::CommitToRegistry`)

In the frame commit in `MatchControl::CommitToRegistry` (`0x140422A3F`, clean native code in `.text`):
1. Copies `MatchControl + 0x2910` (`SetPlayInfo`), `+0x2940` (`FoulInfo`), `+0x297C` (`AdvantageInfo`) and `+0x298C` into `MatchInfo`.
2. Updates the table `MatchInfo + 0x1748[0..21]` (`PlayerOnPitch`) via a loop at `0x140423142..0x14042316E` calling `0x140A3D0E0(MatchInfo, slot, flag)`.
3. At **`0x1404231DC..0x140423205`** it applies the cards directly to the `PlayerRecord` of the offending team:
   - Resolves `PlayerRecord*` via `TeamInfo_GetPlayerBySlot` (`0x140A3B5B0(teamInfo, offenderSlot)`).
   - `PlayerRecord_GetCards` (`0x141032A40`) returns `rcx` directly (`mov rax, rcx; ret`), proving that:
     - `*(uint8_t*)(PlayerRecord + 0x00)` = the player's **Yellow Cards** counter in the match.
     - `*(uint8_t*)(PlayerRecord + 0x01)` = the player's **Red Cards** counter in the match.
   - Exact logic at `0x1404231EF..0x140423205`:
     - If `FoulJudgementKind == 3` (Yellow): `yellows++`. If `yellows >= 2`, it also executes `reds++`.
     - If `FoulJudgementKind == 4` (Direct Red): `reds++`.
4. All eligibility/sending-off reads in the engine (e.g. `0x140444C40`, `0x140452F00` lines 98-105 and `0x140444ED2`) consistently check `(reds != 0) || (yellows >= 2)`.
5. Late yellow card after advantage: implemented natively by state `0x20` (`0x14044B9D0`), which reassembles `FoulInfo` at `MatchFSM + 0x49F0` with `FoulKind = 2`, `FoulJudgementKind = 3` at the subsequent stoppage.

### 4.3. Validated `MatchRoster` Layout (Teams, Kits, Shirts and Players)

Verified at `0x1414BC550`, `0x1414C6A30`, `0x1414C9580` and validated live in real matches (patched game):
- `matchRoster = *(uint64_t*)0x143714240` (returned by `0x1414BC550()`)
- `dataPtr = *(uint64_t*)(matchRoster + 0x50)` and `rosterTable = dataPtr + 0x35960`
- `editedKitPtr = *(uint64_t*)(matchRoster + 0x58)` and `balNameString = matchRoster + 0x98` (MSVC `std::string`)
- **Per-team entry (`teamEntry = rosterTable + 0x138 + teamIdx * 0x690`):**
  - `+0x00`: `uint32_t TeamId`
  - `+0x04`: `char[70] FullName` (UTF-8)
  - `+0x4A`: `char[16] ShortName` (3-letter scoreboard abbreviation)
  - `+0x29A`: `uint16_t[40] ShirtNumbers` (shirt numbers in the same order as the 40 player entries)
  - `+0x5D8`: licensed `KitTable` (10 entries of 8 bytes, primary RGB at `+2..+4`, secondary at `+5..+7`). For kits edited by patches, `editedKitPtr + teamIdx * 0x108` provides primary RGB at `+0x1991` and secondary at `+0x1994`.
- **Per-player entry (`entry = rosterTable + 0x1308 + teamIdx * 0x3D40 + i * 0x188`, up to 40 per team):**
  - `+0x2C`: `uint64_t PlayerKey` (`key >> 32` matches `*(uint64_t*)(PlayerRecord + 0x30) >> 32`, with a fallback on `TeamInfo + 0x04` for live substitutions)
  - `+0x38`: UTF-8 `DisplayName`
  - `+0x75`: `ShirtName`
  - `+0x144`: `int32_t BalAvatarFlag` (`< 0` indicates a Become a Legend avatar, whose name comes from `matchRoster + 0x98`)
- Additionally, `ReplayMgr + 0x1C` (slot `1..22`) and `ReplayMgr + 0x20` (`PlayerKey`) report in real time which player is focused by the Replay camera.
