# 02. Match State Machine (`MatchFSM`) and VAR Intervention Analysis

Complete mapping of the `MatchFSM` state machine (`*(MatchControl + 0x38)`) in `PES2021.exe` (Steam 1.7.2.0) and a deterministic technical analysis of the 4 VAR intervention scenarios.

## 1. `MatchFSM` Architecture

### 1.1. Per-Frame Tick Cycle

1. `MatchControl::vfunc[9]` (`0x140422670`) prepares buffers and triggers the simulation cycle.
2. `MatchFSM::MainTick` (`0x1404468F0`):
   - Synchronizes `MatchInfo + 0x1748[0..21]` (`PlayerOnPitch`) to `MatchFSM + 0x2790` via `0x140441920`.
   - Processes player commands/flags and game mode.
   - Calls `MatchFSM::DispatchState` (`0x14044D2F0`), which runs the current state in the circular/linear queue `*(int32_t*)(MatchFSM + 0x878 + *(uint8_t*)(MatchFSM + 0x8F0) * 4)`.
   - Runs the post-state updaters (`0x14044A6C0`, `0x14044B430`, `0x14044D8F0`, `0x14044B040`, `0x14044C050`).
   - Copies the consolidated state `MatchFSM + 0x2790` -> `MatchControl + 0x6E0` via `0x140443DB0`.
3. `MatchControl::CommitToRegistry` (`0x140422A3F`) publishes `SetPlayInfo`, `FoulInfo`, `AdvantageInfo`, `PlayerOnPitch` and increments cards in the `PlayerRecord` (`0x1404231DC`).

### 1.2. Critical Offsets in `MatchFSM`

| Offset | Type | Field | Function |
|---|---|---|---|
| `+0x874` | `int32_t` | `SequenceCategory` | Category of the current queue (`1`=OutOfPlay/Rebuild, `3`=Kickoff, `4`=GoalKick, `5`=Corner, `6`=ThrowIn, `7`=Foul/Penalty, `8`=Half/FullTime, `10`=Goal) |
| `+0x878` | `int32_t[30]` | `StateQueue` | Queue of up to 30 states (`0x1E`), filled with `0x48` (empty) by `MatchFSM_ResetQueue` (`0x1404529F0`) |
| `+0x8F0` | `uint8_t` | `QueueHead` | Index of the running state |
| `+0x8F1` | `uint8_t` | `QueueTail` | Queue insertion index |
| `+0x8F2` | `uint8_t` | `SubState` | Internal sub-state of the current handler (reset to `0` when `QueueHead` advances) |
| `+0x908` | `Judge` | `EmbeddedJudge` | Instance of `Judge@ai@match` |
| `+0x2790` | `struct` | `WorkingMatchState` | Working buffer copied to `MatchControl` (`+0x2794` = `MatchStatus` 1/2/3) |
| `+0x27FC` | `uint8_t` | `QuickRestartFlag` | `1/2/3` when eligible or during a quick kick |
| `+0x49C0` | `JudgeResult` | `LiveJudgeResult` | `Judge` result for the frame (`+0x00` SetPlayInfo, `+0x30` FoulInfo, `+0x6C` AdvantageInfo, `+0x7C` GoalInfo) |
| `+0x57F8` | `uint8_t` | `DelayedCardPending` | Late card flag after advantage (triggers state `0x20`) |
| `+0x17860` | `void**` | `MatchContextPtr` | Pointer to the internal actor/event context |

### 1.3. Map of the Main States of `MatchFSM::DispatchState` (`0x14044D2F0`)

Extracted directly from the dispatch table (`idx` at `0x14044D7C0`, `rva` at `0x14044D734`, 72 states `0x00..0x47`):

| State | Handler VA | Role in the Match |
|---|---|---|
| `0x00` | `0x14044A560` | Initialization / period entry |
| `0x01` | `0x14044DB80` | `OutOfPlay` dispatch -> calls `MatchFSM::BuildOutOfPlayQueue` (`0x1404530C0`) |
| `0x03..0x0B` | `0x140446DF0` | Cutscenes and stoppage reactions (goal `0x04`, foul/card `0x06` `0x140446F80`, ball out/injury `0x09` `0x140447150`) |
| `0x0C..0x11` | `0x140449340` | Fade/camera transitions between cutscene and pitch |
| `0x12` | inline `0x14044D442` | Pre-substitution preparation / period synchronization |
| `0x13` | `0x1404485B0` | Application of substitutions, sent-off goalkeeper check (`0x140446480`) and kicker selection (`0x1404A8730`) |
| `0x14` | `0x14044AD00` | Entity synchronization and start of set piece ball positioning |
| `0x15` | `0x14044C5B0` | Finalization of the positioning of the 22 actors and setting of `MatchStatus = 3` |
| `0x16` | `0x14044A780` | Processing of pending substitutions and swap events |
| `0x17` | inline `0x14044D580` | Wall setup (queued only when `SetPlayKind == 5`) |
| `0x18` | `0x14044E810` | Quick Free Kick |
| `0x19` | `0x14044CA20` | Final entry into set piece (HUD/camera/notifications) |
| `0x1A` | `0x14044DD50` | **`SetPlayReady`:** waiting for the free kick, penalty, corner, throw-in or goal kick |
| `0x1C` | `0x140449F40` | **`InPlay`:** ball rolling, runs `Judge::Evaluate` every frame |
| `0x1D` | `0x14044BC30` | Automatic post-incident replay (goal / serious foul) |
| `0x20` | `0x14044B9D0` | Late yellow card after advantage |
| `0x23` | `0x1404498B0` | Set piece queue pre-setup cleanup (`0x140452F00`) |
| `0x33` | `0x14044C310` | Quick Throw-In |
| `0x34` | `0x14044AE40` | Kicker swap validation / pre-`0x19` state |
| `0x45` | `0x140446DB0` | HUD/transition flag reset |

---

## 2. Native `MatchFSM` Transition Primitives

Two clean functions of the Fox Engine itself govern almost all VAR interventions without manual positioning hacks:

1. **`MatchFSM_EnterOutOfPlay` (`0x1404541A0`, RVA `0x4541A0`)**:
   - Called automatically in `State_InPlay` (`0x14044A408`) and `State_SetPlayReady` (`0x14044E1C5`) whenever `*(int32_t*)(MatchFSM + 0x49C0) != 0`.
   - Calls `0x1404829E0` (injury/foul processing), resets `Queue` with `MatchFSM_ResetQueue(MatchFSM, 1)` (`0x1404529F0`), pushes state `1` (`0x14044DB80` -> `0x1404530C0`) and sets `*(MatchFSM + 0x2794) = 2` (`OutOfPlay`).
   - State `1` builds the entire native sequence: foul/card cutscene (if there is a card), wall setup (`0x17` if foul), positioning of kicker/goalkeeper/players (`0x16 -> 0x12 -> 0x13 -> [0x17] -> 0x14 -> 0x15 -> 0x0C/0x0E -> 0x34 -> 0x19 -> 0x1A`).

2. **`MatchFSM_RebuildSetPlay` (`0x140454280`, RVA `0x454280`)**:
   - Used natively in `State_SetPlayReady` (`0x14044DD50` lines 188, 199, 238) when the user changes the kicker or there is a substitution during a set piece.
   - Calls `MatchFSM_ResetQueue(MatchFSM, 1)` and pushes the clean sequence **without foul cutscene/replay**:
     `0x0F -> 0x16 -> 0x12 -> 0x13 -> [0x17 if SetPlayKind == 5] -> 0x14 -> 0x15 -> 0x0C -> 0x19 -> 0x1A`.
   - Ideal for reconfiguring an existing set piece (e.g. converting foul <-> penalty or applying a Drop Ball / possession return on an overturn) with a fast, deterministic transition free of soft-locks.

3. **`MatchFSM_EnterInPlay` (`0x140453F10`, RVA `0x453F10`)**:
   - Transitions `*(MatchFSM + 0x2794) = 1` (`InPlay`) from `SetPlayReady` or special modes. It must not be called arbitrarily in the middle of cutscenes `0x03..0x11` without disarming the set piece camera/actors, at the risk of locking the camera in place.

---

## 3. Deterministic Analysis of the 4 VAR Intervention Scenarios

### Scenario 1: Change Card on an Existing Stoppage (None / Yellow / Red)

- **Apply or upgrade a card (None -> Yellow, None -> Red, Yellow -> Red/2nd Yellow):**
  - **Feasibility:** High (deterministic).
  - **Mechanics:** Update `PlayerRecord[0]` (`yellows`) and `PlayerRecord[1]` (`reds`) via `0x140A3B5B0(TeamInfo, offenderSlot)`, synchronize `FoulInfo.FoulJudgementKind` (`MatchInfo + 0x158C` and `MatchFSM + 0x49F8`), suppress the duplicate increment in the hook `0x1404231DC` via the flag `var_suppress_card_commit`.
  - If it results in a sending-off (`reds >= 1` or `yellows >= 2`): set `MatchInfo_SetPlayerOnPitch(MatchInfo, offenderSlot, 0)` (`0x140A3D0E0`) and call `MatchFSM_RebuildSetPlay` (`0x140454280`) so that state `0x13` (`0x1404485B0`) removes the actor from the pitch and triggers the native goalkeeper logic (`0x140446480`) if the sent-off player is the goalkeeper.
- **Remove a Yellow card (no sending-off):**
  - **Feasibility:** High (deterministic).
  - **Mechanics:** Decrement `PlayerRecord[0]` (clamp `>= 0`), update `FoulJudgementKind = 1` and clear the indicator in `PlayerStatus` (`GMATCH + 0x180`).
- **Remove a Red card or reduce Red -> Yellow (Reinstatement of a Sent-Off Player):**
  - **Feasibility:** High before state `0x13` (during cutscenes `0x03..0x11`) and handled safely after state `0x13` (`0x1A` SetPlayReady or `0x1C` InPlay).
  - **Real engine limitation:** Before `0x13`, the 3D actor is still on the pitch: zeroing `PlayerRecord[1] = 0` and keeping `PlayerOnPitch[slot] = 1` avoids the removal entirely. After `0x13`, the actor has already been removed from the scene and the mod reinstates `PlayerRecord` + `PlayerOnPitch[slot] = 1`, showing a clear notice that the 3D actor returns at the next stoppage/rebuild.
  - **Product rule:** The option is never locked in the UI, preserving full control over the cards of any player.

### Scenario 2: Convert Foul <-> Penalty (with card option) on an Existing Stoppage

- **Feasibility:** High (deterministic).
- **Foul -> Penalty:**
  1. Compute the exact penalty spot coordinate via `Judge_CalcPenaltySpot` (`0x14050DC00(outPos, 6, PitchDims, origPos)`).
  2. Update `JudgeResult` (`MatchFSM + 0x49C0`) and `MatchInfo + 0x1554`: `SetPlayKind = 6`, `BenefitedTeam`, `SetPlayPosA/B = outPos`, `StoppageReason = 5`, `FoulKind = 2`, `FoulJudgementKind` (according to the user's decision).
  3. Adjust the card delta in the `PlayerRecord` (avoiding duplication via `0x1404231DC`).
  4. Trigger `MatchFSM_RebuildSetPlay` (`0x140454280`) (or `MatchFSM_EnterOutOfPlay` `0x1404541A0` if there is a new card and the user wants the card animation). The engine positions the taker on the penalty spot, the goalkeeper on the line and the remaining players outside the area.
- **Penalty -> Foul outside the area:**
  1. Use the contact coordinate recorded in the ring buffer. If `Pitch_ClassifyZone` (`0x140A73470`) still indicates bit 11 (inside the area), project `(x, z)` 0.5m outside the nearest edge of the penalty area.
  2. Update `SetPlayKind = 5`, `SetPlayPosA/B = adjustedPos`, `StoppageReason = 5`.
  3. Adjust the card delta in the `PlayerRecord`.
  4. Trigger `MatchFSM_RebuildSetPlay` (`0x140454280`). The function itself detects `SetPlayKind == 5` and inserts state `0x17` (wall setup) into the queue.

### Scenario 3: Retroactively Whistle a Foul or Penalty While the Ball Is Rolling ("No Foul")

- **Feasibility:** Very High (100% deterministic, uses the official `Judge` flow).
- **Mechanics:**
  1. During `InPlay`, the passive recorder saves snapshots of rolling play (positions of the 22 players + ball + closest contact pairs + 60Hz float timestamp).
  2. In Manual Replay, the user pauses on the exact frame of the unflagged incident, chooses the offender/victim (pre-suggested by the closest contact or by the camera focus `ReplayMgr + 0x1C`) and selects the verdict (Simple foul / Foul + Yellow / Foul + Red / Penalty + optional card).
  3. When leaving Replay and returning to `State_InPlay` (`0x140449F40`), the entry hooks in `Judge::FinalizeFrame` (`0x14050CD70`) and `MatchFSM::MainTick` (`0x1404468F0`) fill `MatchFSM + 0x49C0` (`JudgeResult`) with `SetPlayKind = 5 or 6`, `BenefitedTeam`, `SetPlayPosA/B`, `StoppageReason = 5`, `FoulInfo.IsValid = 1`, `FoulKind = 2`, `FoulJudgementKind = 1/3/4`, `OffenderSlot`, `VictimSlot`, `0xFF` sentinels in the engine's internal fields and a valid `SnapshotTickIndex` guaranteed in the pool `MatchInfo + 0x17AC` (`EnsureValidFoulSnapshotId`, eliminating any risk of a crash at `0x1404B4C25`).
  4. The Fox Engine itself triggers `MatchFSM_EnterOutOfPlay` (`0x1404541A0`), applies the card, builds the set piece and hands over control at `0x1A`.
- **Scope note on a goal already validated after the incident:** If the user pauses before the ball enters (still in `InPlay`), Scenario 3 works 100%. If the goal has already been validated (`StoppageReason == 3` and the score incremented), overturning the goal requires reverting the score + scorer history (and avoiding a collision with `goal_pyro.lua`), which is documented as a controlled extension for a later phase after live validation of the score.

### Scenario 4: Overturn a Foul or Penalty Call (Overturn / Cancel)

- **Real FSM risk analysis:**
  - Forcing `MatchStatus = 1` (`0x140453F10`) in the middle of `OutOfPlay` (`0x03..0x15`) or `SetPlayReady` (`0x1A`) carries a high risk of a camera/AI soft-lock, because the Fox Engine has already deactivated the live camera and positioned the actors in set piece mode.
- **100% Deterministic Mechanics Faithful to IFAB Law 8.2 (Drop Ball / Possession Returned):**
  1. Revert any card applied by the overturned incident (adjusting `PlayerRecord[0/1]` and `PlayerOnPitch`).
  2. Clear `FoulInfo` (`IsValid = 0`, `FoulJudgementKind = 0`) and suppress the card commit in the hook `0x1404231DC`.
  3. Define a clean restart returning possession to the team that suffered the overturned call (according to the official IFAB Drop Ball protocol after a VAR review):
     - **Overturned penalty:** `SetPlayKind = 4` (Goal Kick for the defending goalkeeper) or `SetPlayKind = 5` (defensive free kick inside the area for the goalkeeper/defense), `BenefitedTeam = defending team`, `StoppageReason = 7` (restart without foul cutscene).
     - **Overturned foul:** `SetPlayKind = 5`, `BenefitedTeam = team that suffered the overturned call` (or the team that had possession in the whistle snapshot), `SetPlayPosA/B` at the ball location, `StoppageReason = 7` (no cutscene).
  4. Trigger `MatchFSM_RebuildSetPlay` (`0x140454280`). The engine performs a short, deterministic transition to `0x1A` with the ball at the defense's feet, with no foul animation, no card and zero soft-lock risk.
  5. **Alternative of instant resumption at `0x1C`:** it will be tested in a sandbox in Phase 2 exclusively for pauses made in the first frames of the whistle (before `0x12`), but the **Drop Ball via `0x140454280`** will be the guaranteed default mechanism.

## 4. Anti-Crash and Anti-Soft-Lock Safeguards

1. **Execution exclusively on the simulation thread:** The ImGui UI (DX11 thread) never writes directly to `MatchFSM` or `MatchInfo`. It enqueues an atomic `VarCommand` (`std::atomic<uint32_t> seq`), consumed only at the synchronous hook sites `0x14044A065` (`InPlay`) or `0x14044E1B9` (`SetPlayReady`) / `0x1404468F0` (`MainTick`).
2. **Valid state gate:** Never inject commands if `GMATCH == 0`, `MatchInfo == 0`, `MatchInfo->SpecialModeKind != 0` (training/tutorial), `MatchPhase >= 8` (penalty shootout / end of match), or if `ReplayMgr->Mode != 0` (wait until fully out of Replay and Pause).
3. **Offline mode gate:** The Sider Lua controller blocks activation if it detects online mode or a lobby.
4. **Anti-reentrancy and anti-duplication of cards:** A transaction flag `var_tx_active` ensures that `MatchControl::CommitToRegistry` (`0x1404231DC`) never counts the same card twice when rebuilding a SetPlay.
5. **Coordinate clamp:** Every position injected into `SetPlayPosA/B` goes through a field bounds check (`GMATCH + 0x2AA8`) and `Pitch_ClassifyZone` (`0x140A73470`).

---

## 5. Critical Discovery v4.1: `MatchInfo` RCU Double-Buffer and Transition in `SetPlayReady` (`0x1A`)

Binary investigation of the functions `0x140412870`, `0x1404237C0`, `0x14042DCD0`, `0x14041BEC0`, `0x14044DD50`, `0x14044DB80`, `0x1404530C0` and `0x140454280` revealed why calling `MatchFSM_RebuildSetPlay` (`0x140454280`) directly at `0x1A` made the screen fade while keeping the old taker standing at the previous foul:

1. **RCU Double-Buffer in `MatchInfo` (`0x2378` bytes per buffer):**
   - The allocator `0x140412870` reserves `0x46F8` bytes (`8` header bytes + two `MatchInfo` buffers of `0x2378` bytes).
   - `GMATCH + 0x270` points to the **Read Buffer `[0]`** (`alloc + 0x08`). Adjacent at **`matchInfo + 0x2378`** (`alloc + 0x2380`, returned by `BeginWriteMatchInfo` `0x1404237C0`) is the **Write Buffer `[1]`**.
   - In states `0x45` (`0x140446DB0`) and `0x16` (`0x14044A780`), `EndWriteMatchInfo` (`0x14041BEC0`) fully copies the `0x2378` bytes of the **Write Buffer `[1]`** over the **Read Buffer `[0]`**. Writing only to `matchInfo` without updating `matchInfo + 0x2378` (or without going through `CommitToRegistry` `0x140422A3F`) causes the old foul to be restored on the first tick of the transition.
2. **Limitation of `MatchFSM_RebuildSetPlay` (`0x140454280`):**
   - `0x140454280` was designed only for a kicker swap at the same location: it skips `State_01` (`0x14044DB80`), does not recompute the foul's geometric mode (`0x14044F950`), does not rerun `SelectKicker` (`0x1404A8730`) with the real `SequenceCategory` and forces `stoppageReason = 6`.
3. **Unified Deterministic Flow (`Judge::FinalizeFrame` + `StoppageReason::SilentRestart = 7`):**
   - `State_SetPlayReady` (`0x14044DD50`) runs the `Judge` pipeline (`0x14050BC60` -> `0x14050D6F0` -> `0x14050CD70`) every frame and triggers `MatchFSM_EnterOutOfPlay` (`0x1404541A0`) whenever `*(int32_t*)(MatchFSM + 0x49C0) != 0`.
   - When `SetPlayInfo` + `FoulInfo` are injected in `Detour_JudgeFinalizeFrame` (with `StoppageReason::SilentRestart = 7` when at `0x1A` or `0x03..0x11`, also mirroring `SetPlayInfo`, `FoulInfo`, `PlayerOnPitch` and `FoulSnapshotPool` in `matchInfo + 0x2378` and zeroing `uint8_t QuickRestartFlag` at `MatchFSM + 0x27FC`), the engine itself executes `MatchFSM_EnterOutOfPlay` + `CommitToRegistry` on Tick `T`, runs the full `State_01` on Tick `T+1` (where `MatchFSM + 0x49C0` is cleared with `0xFF` sentinels to avoid re-commits during `0x0F..0x19`, since `SilentRestart` does not push `State_23`) and repositions the ball, wall, taker and the 22 players at the exact new location.
