# Replay, card cutscene and play classification

v4.4 research. Addresses of `PES2021.exe` Steam `1.7.2.0`. **[P]** proven by disassembly, **[I]** inferred.

## 1. Replay ring

- The replay system at `*(0x143703558)` (ctor `0x140A8A800`) has 21 buffers of `0x50` bytes at `+0x8`. The active slot index is at `+0x698` and the active buffer pointer at `+0x6A8` (`SelectBuffer` at `0x140A8C440`). Slot 0 is the live ring and slots 3 to 20 hold saved clips **[P]**.
- Buffer fields:

  | Offset | Contents |
  |---|---|
  | `+0x10` | u16 write index |
  | `+0x12` | u16 capacity |
  | `+0x14` | tick of the last write |
  | `+0x40` | frames, stride `0x1FB0` |

- The recorder `0x1404655C0` runs every tick when `MatchInfo + 0x11B0 == 0x16` **[P]**.
- The capacity comes from `0x140A8A710`: `*(u32*)0x143703554`, or `660` frames when it is zero. Nothing in the game writes this value **[P]**.
- Frame: `+0x00` time, phase in the low nibble of `+0x07`, `+0x24C == 0x7FFF` marks an invalid frame, 25 entities of `0x12C` bytes starting at `+0x264` **[P]**.
- The manual replay window is recomputed on every open from the valid frames (`0x14217D2A0` -> `0x14217D3C0`, `0x140A8D580`, `0x140A8D3E0`) **[P]**. Moving the window start recovers nothing.
- Cleanup: when `MatchInfo + 0x11B8` is active, the controller tick (`0x14042E9F0`) calls `0x140A8F310`, which invalidates all frames **[P]**. What activates `+0x11B8` was not found.
- The frames have no pointers, and the engine copies them as raw blocks between slots (`0x140A8EE70`) **[P]**. Keeping a private copy and reinjecting it is feasible, but depends on validating live which slot the manual replay reads and whether the roster matches after substitutions. v4.4 writes this data to `var_core.log` (`ReplayDiag` lines), read-only.

## 2. Card cutscene

- Reason 5 with an ordinary foul enqueues `0x03`, `0x05`, `0x45` (`0x140452C80`) **[P]**.
- State `0x05` picks the demo at `0x14047FB40`. The demo depends on the judgment of the most recent foul event in the history (category 4, recorded by the observer `0x140A141F0` on the status transition 1 to 2 with reason 5) and on the yellows **already recorded** in the `PlayerRecord` at the moment of the choice **[P]**.

  | Case | Foul | Penalty |
  |---|---|---|
  | single yellow | `0x20019` | `0x2000F` |
  | second yellow | `0x2001C` | `0x20012` |
  | red for someone who already had a yellow | `0x2001D` | `0x20013` |
  | straight red | `0x2001A` | `0x20010` |

- Since the engine records the card on the injection tick (v4.3), a foul called by the VAR with the ball rolling picks the correct demo, including the second yellow **[P]**. Versions up to v4.2 neutralized this commit, and therefore played the single yellow **[I]**.
- From `SetPlayReady` onward the mod uses reason 7, which has no cutscene. Using reason 5 there would require: fixing the demo (the observer does not fire on the 3 to 2 transition and the choice would read the old foul), avoiding the quick free kick for fouls without a card (`0x140445740`) and understanding the `MatchFSM + 0x904` flag read by state `0x23` **[P]** for the requirements, without live validation.

## 3. Automatic foul replay

- On the ordinary foul path, `0x1D` and `0x1F` are enqueued right after `0x11`, unless `MatchInfo + 0x177F` is active **[P]**.
- State `0x1D` (`0x14044BC30`) reads neither `FoulInfo` nor the snapshot tick **[P]**. The window it plays was not proven. v4.4 logs the real window of each `0x1D` compared to the stoppage tick.

## 4. Action IDs and classification

- `PlayerAction + 0x01` is the `ANIME_*` enum, read as a signed byte. Name table at `0x1434B3470`, categories at `0x142651590` **[P]**:

  | ID | Name |
  |---|---|
  | 11, 12 | `ANIME_SLIDING` (slide tackle) |
  | 13 | `ANIME_TACKLE` (standing tackle) |
  | 14 | `ANIME_BLOCK` |
  | 15 | `ANIME_DIVE` (outside the Judge filter) |
  | 16, 17 | `ANIME_STAGGER`, `ANIME_FALL_DOWN` |
  | 55 to 71 | goalkeeper |
  | 72 | `ANIME_JOSTLE` |

- `ObserverFallDownOrStagger` (`0x140A1AF50`) fires for the victim with a contact reaction and reads the culprit at `(int8)PlayerAction + 0x3D` **[P]**. v4.4 uses this same model.
- `TeamTactics + 0x254` is the team side as a signed byte (`movsbl` at `0x140438475`), and it flips at half time **[P]**. The mod used to read it as a float, the sign came from `+0x257` and it never flipped. The convention is calibrated at every kickoff from the player positions.
