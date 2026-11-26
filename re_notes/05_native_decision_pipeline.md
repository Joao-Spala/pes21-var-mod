# Native decision pipeline (foul, card, sending off and set pieces)

Basis of the v4.3 execution engine (`match_hooks.cpp`, section "Native-first VAR execution"). Addresses of `PES2021.exe` Steam `1.7.2.0`. **[P]** proven by disassembly, **[I]** inferred.

## 1. Card commit

- Every frame the engine zeroes `MatchControl + 0x6E0` (`0x1404228D0` calls `0x140441810`) and the FSM `JudgeResult` at `MatchFSM + 0x49C0` (`0x140444A20` calls `0x14050B2D0`), before the `MainTick` called at `0x140422AFE` **[P]**.
- `CommitToRegistry` (`0x140422A3F`) runs after `MainTick`, in the same function and on the same thread. It calls `UpdateFoulCounters` (`0x1404231A8`) and right after applies the card of the committed foul (`0x1404231DC..0x140423202`) **[P]**:
  - judgment 3: `yellows++` and, if `yellows >= 2`, `reds++`
  - judgment 4: `reds++`
- Since the buffers are zeroed every frame, an injected `JudgeResult` gets the card **exactly once**, on the injection tick **[P]**. The "next tick" cleanup the mod used to do was redundant.
- The same commit copies the snapshot pool from `MatchControl + 0x2A58` over `MatchInfo + 0x17AC` (`0x140423196` calls `0x140A3D3B0`) **[P]**.

## 2. Who removes the player from the pitch

- State `0x12` (inline at `0x14044D442`) calls `0x140449050`, which recomputes `MatchInfo + 0x1748` from the cards in the `PlayerRecord`. With a red or two yellows, the player leaves. With no sending-off card and no injury, the player returns according to `PlayerRecord + 0x18` **[P]**.
- State `0x13` (`0x1404485B0`) removes the actor (`0x1408C66C0`) and handles a sent-off goalkeeper (`0x140446480`) **[P]** for the calls, **[I]** for the effect.
- The mod never writes `PlayerOnPitch`. It only writes cards to the `PlayerRecord` and lets states `0x12` and `0x13` do the rest.

## 3. Set piece queues

- `BuildOutOfPlayQueue` (`0x1404530C0`) reads the reason at `MatchInfo + 0x1574` and the type at `+0x1554` **[P]**.
- Reason 7 (`SilentRestart`, table `0x14045333C`): every type builds `0x0F, 0x45, 0x16, 0x12, 0x13, [0x17 foul only], 0x14, 0x15, 0x0C, 0x19, 0x1A`. No cutscene, no replay **[P]**.
- Reason 5 (foul, table `0x453354`): a foul with a card (judgment 3 or 4) or a penalty does not become a quick free kick (`0x140445740`) and goes through `0x140452F00`, which also enqueues `0x12` and `0x13` **[P]**.
- `MatchFSM_RebuildSetPlay` (`0x140454280`) rebuilds the queue of the same set piece with `0x12` and `0x13` and does not record a foul. The engine uses it in `SetPlayReady` for taker changes and tactical changes (`0x14044E286`, `0x14044E328`, `0x14044E3AF`) **[P]**.

## 4. Default values of the structures

Native resets `0x140A3CAE0` (FoulInfo) and `0x140A3CB50` (SetPlayInfo) **[P]**:

| Field | Default |
|---|---|
| FoulInfo `+0x0C` / `+0x10` (offender, victim) | `0xFF` |
| FoulInfo `+0x14` / `+0x18` (actors, sentinel) | `0x18` |
| FoulInfo `+0x2C` (byte) | `9` |
| FoulInfo `+0x30` | `4` |
| SetPlayInfo `+0x04` (team) | `2` |
| SetPlayInfo `+0x28` (event player) | `0xFF` |

## 5. What does not exist natively

- There is no card decrement **[I]** (pattern search over the 711 callers of the getter `0x141032A40`). Revoking a card is the only manual card write left in the mod.

## 6. How the mod applies each decision

| Decision | Path |
|---|---|
| Retroactive foul or penalty with the ball rolling | `JudgeResult` with reason 5 in `Judge::FinalizeFrame`. The engine applies the card and sends the player off |
| Convert, invert or cancel at a set piece | Manually revokes the already counted card and injects with reason 7. The engine applies the new card |
| Corner, goal kick or throw-in | Reason 7 with the restart type and default FoulInfo |
| Card only, without changing who is on the pitch | Write to the `PlayerRecord` |
| Card only, sending off or reinstating, at a set piece | Write to the `PlayerRecord` and `RebuildSetPlay` |
| Card only, sending off or reinstating, with the ball rolling | Waits for the next set piece |

Each step is verified by reading the FSM back: the decision only counts as applied, and the history is only recorded, when the FSM leaves the state in which the request was made.
