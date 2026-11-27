# 09. Goal annulment: how a goal is recorded

Status: static analysis complete. Implemented in v4.5.0 (`GoalAnnul` in `match_hooks.cpp`), live validation pending.

## 1. Where a goal is recorded

All of it happens in one sim tick: the `MatchStatus` 1 to 2 edge with `StoppageReason` (`MatchInfo+0x1574`) = 3.

1. **Goal block**: `MatchControl::CommitToRegistry` (function at `0x1404228D0`) copies GoalInfo (`MatchControl+0x298C`, `JudgeResult+0x7C`) to `MatchInfo+0x15C0..0x168B`.
   - `+0x15C8` is the scorer's team and `+0x15CC` the scorer's roster index.
   - `+0x15D0` is a goal kind (0x18 = none).
2. **Event**: `ObserverGoal@record@match` (vtable `0x1426490F0`, `[1]=0x140A14510`) builds an event of category `0x1F` and copies the goal block into it.
   - Event `+0x14C` = scorer team, `+0x150` = scorer index, `+0x168` = credited team.
   - Own goal when `+0x168 != +0x14C`.
3. **Record manager update** (`0x140A0D820`): the observers live in an array at `+0xB600` (0xDF entries). Each event goes to two handlers:
   - **Stats dispatcher** `0x140A22760(mgr, stats, event)`, a switch on the category.
     - For a goal it calls `0x140A22D00`.
     - `stats = H + 0x750D0`, where `H = *(mgr + 0xBD08)`, through `0x140A3EEC0`.
   - **History handler** `0x140A3E260(H, event)`. For category `0x1F` it adds the event to:
     - a general list at `H+0x26FF0` (64 entries), count at `+0x271F0`, write index at `+0x271F4`.
     - a per-team goal list at `H + 0x27400 + team*0x26AC8` (99 records of 0x640 bytes), count at `+0x26AC0`, write index at `+0x26AC4`.
     - a goal list at `H+0x74990` (198 entries), count at `+0x74FC0`. `GetGoalCount` is `0x140A3EF10`, `GetGoal(n back)` is `0x140A3ECF0`.
     - a last event per category at `H + 0x1C258 + category*0x328` (`0x140A3E020`).
4. **Goal stats** (`0x140A22D00`):
   - Adds a goal record through `0x1408C7790` (scorers list).
   - Updates the lead markers at `stats+0x10ACAC..0x10ACAF` (first goal, equaliser, winner).
   - Calls `AddTeamGoal` (`0x1408C8C20`) on the credited team's table (`0x1408C7B70(stats, team)`, stride `0x59650`). This is the score.
   - Updates the scorer's player entry (`0x1408CA450(teamTable, index)`) through `0x1408CB210(entry, stat, period, ...)`:
     - not an own goal: stats 0 (goals), 6, 0xC and 0x11.
     - shot or body part: 9, 10, 0xB, 0x14, 0x15.
     - other flags: 2 and 3.
   - Own goals take another branch.

## 2. The score is a protected value

- `GetTeamStat(table, k)` is `0x1408CA550`. `k=5` is the match total: the sum of periods 0..7.
- Per period it reads `0x1408C98F0(table, 0, period)`. Stats other than 0 are plain `uint32` at `table + 0x20C + (stat*9 + period)*4`.
- **Goals (stat 0)** are stored per period at `table + 0x59530 + period*0x20`:

  | Offset | Content |
  | --- | --- |
  | `+0x00` | FNV-1a 64-bit hash of the 4 plain bytes |
  | `+0x08` | seed `s` |
  | `+0x0C` | key, `slot = key & 3` |
  | `+0x10` | 4 encoded slots |

- Decoding: `k0 = (3s) & 0xFF`, `k1 = ((3s+4) >> 8) & 0xFF`, `k2 = ((3s+8) >> 16) & 0xFF`, `k3 = ((3s+12) >> 24) & 0xFF`. Then `plain_i = (enc_i + k_i) ^ (k_i*0x3F)`.
- Encoding (`0x1408CA5F0`): `enc_i = (plain_i ^ (k_i*0x3F)) - k_i`.
  - The new key is a 32-bit FNV-1a of `KUSER_SHARED_DATA+0x8` (`0x7FFE0008`), seeded with `0x84222325` and multiplied by `0x1B3`.
  - The value goes to slot `key & 3`.
- **If the hash does not match, the getter calls `0x1415A6744` and executes `int3`.** A wrong write crashes the game by design.
- `AddTeamStat` (`0x1408CA5F0`) cannot subtract: a delta of `0xFFFFFFFF` saturates the value at `0xFFFFFFFF`.

## 3. Displayed score

- The HUD (`SimMain@Screen@match2D`, vtable `0x142BF58F8`) reads bytes `+0x9E/+0x9F` of its data object (`+0x38`).
- That data object is `match2D manager + 0x1818`, passed by the screen factory `0x1420D54A0`.
- `0x142101130` refreshes the cached values when they differ, so the HUD follows the source.
- Sider's `match.stats()` reads the same bytes.
- The bytes `+0x9D..0xA4` handled in `0x140AC2C60`/`0x140AC3600` belong to `KeyConfig@menu`: an unrelated offset collision.

## 4. Annulment design (original plan, see section 5 for what was built)

- **Snapshot:** hook the entry of `0x140A22760` (or `0x140A3E260`). When the event category is `0x1F`, copy all of `H` before the engine processes the goal (about 0x180760 bytes, once per goal).
- **Annul:** during the goal stoppage only, before the kickoff (state `0x1A`), on the sim thread.
  - Restore `H`. This reverts the score (the protected values restored stay self-consistent), the player stats, the lists and the lead markers in one step.
  - Clear the goal block in both `MatchInfo` buffers.
  - Replace the queued kickoff with the chosen restart through the existing injection path (`StoppageReason = 5`).
- **Restart choices:**
  - foul by the attacker: direct free kick to the defence.
  - offside: indirect free kick.
  - handball: direct free kick.
  - other: drop ball.
- **To validate live before enabling:**
  - `H` holds no heap pointers that change between the snapshot and the restore.
  - the snapshot hook runs before both handlers.
  - the HUD and the end-of-match result follow the restored score.
  - the encoder round-trips against a live value (decode the live value with our decoder and compare its hash before writing anything).
- `GoalProbe` was a temporary read-only diagnostic (removed).

## 5. Implementation (v4.5.0)

- **Snapshot.** `RecordHistory::AddEvent` (`0x140A3E260`) is hooked with an 11-byte prologue. For a category `0x1F` event it copies `H` (`kHistoryBytes`) together with the credited team, match tick and period.
- **Removal.** `RecordHistory::ResetPending` (`0x140A3E760`) is hooked with a 9-byte prologue. It is called only from the record manager update, so the copy-back runs in the same context the engine writes `H` in. There is no race with the engine's own writes or with readers of the protected score.
- **Sequence:**
  1. **Consume.** At the kickoff of the goal (state `0x1A`, set play `KickOff`), `ConsumeGate` lets only an annulment through. `BeginRemoval` checks the copy against the live `H`: the scores of both copies are decoded, the credited team must be exactly one goal higher, the other team the same. The removal is then marked pending.
  2. **Removal step.** The next record update step saves the live `H` (with the goal), writes the copy back and zeroes the pending list.
  3. **Injection.** The free kick goes in through the existing path, with `FoulKind::Offside` for offside.
  4. **Commit.** `ExecFinish(applied)` makes the removal final.
  5. **Any other exit.** Failure, undo, re-arm or the deadline go through `Cancel`. It drops a pending removal or schedules putting back the copy with the goal.
- **Safety:**
  - A failed check never writes anything.
  - A removal that never runs expires after 600 ticks.
  - The mod unloading mid-flight puts the goal back.
  - The banner reports a free kick whose goal could not be removed.
- **Known limitations:**
  - Restoring `H` also erases non-goal events the record got between the goal and the removal, such as a substitution made during the goal stoppage.
  - If the removal step fails after the engine already confirmed the free kick, the decision history still lists the annulment. Only the amber banner corrects it.
  - The HUD and the end-of-match result reading the restored score is expected (the HUD refreshes from the source) but not verified live.
- **Diagnostics:** `GoalProbe` (read only) was removed after it served its purpose. `MatchHooks::CheckSimStall` logs where the sim thread is if `MainTick` stops for 4 s outside replays.
- **Not cleared:** the `MatchInfo+0x15C0` goal block is not cleared by the mod. The injected free kick commits a new set play, and `CommitToRegistry` then copies the (empty) GoalInfo of that tick over it. This still has to be checked live.
