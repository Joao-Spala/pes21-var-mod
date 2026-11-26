# Pending clip replay (replay of the reviewed play after a decision)

Addresses of `PES2021.exe` Steam `1.7.2.0`. **[P]** proven by disassembly, **[I]** inferred, needs live validation.

## 1. The request

- Four replay requests at `MatchInfo + 0x168C`, stride `0x2C`, reset by `0x140A68200` (status -1, type 0x13, `+0x18 = 0xA`, `+0x24 = 0x29`, `+0x28 = 0xFFFF`) **[P]**.
- Fields: `+0x00` status (-1 idle, -2 committed, 0 commit now, N delay), `+0x04` type, `+0x05` priority, `+0x07` phase, `+0x08` minute, `+0x09` second, `+0x0C`/`+0x0E` frames back from the newest ring frame (start and end of the clip), `+0x10`/`+0x12` segment 1 **[P]**.
- The slot 3 builder `0x140A68D40(req, minute, second, phase, status, A, B)` writes type 0x11 with priority 0x5B, clears segment 1 and flag bit 4 **[P]**. The mod writes the same fields directly, status last.

## 2. Commit and playback

- The recorder (`0x14042F8D0`, `0x14042E850`) works on the MatchInfo write buffer and commits a request with status 0 on its next active pass (`rec+0x40 == 0`), unless the phase is 9 **[P]**. `0x14042E580` accepts type 0x11 only in slot 3 **[P]**.
- `0x14042F0E0` / `0x140465450` copy the clip from the live ring (buffer 0, inline at replay system + 0x8): `n` contiguous valid frames from `writeIdx - 1`, `skip = n > A ? n - A - 1 : 0`, `len = min(A - B + 1, n - skip)` **[P]**. The record lands at `*(0x143703568) + 0x1A340`, `+0x10` = type (0x13 when empty) **[P]**.
- FSM state 0x1F (`0x14044BDE0`) waits while the read-buffer request status is not -1, then plays the record unless its phase is older than the match phase or `(minute + 3) * 60 + second` is before now, both computed like `0x140A6C330` (minute rounded up, +45 / +90 / +105 for phases 3 / 5 / 7) **[P]**.
- 0x1F is queued by the stoppage queue rebuild `0x14044F140` only when slot 0 holds a stoppage request and `MatchInfo + 0x177F` is clear **[P]**. On the live foul path the order is card cutscene, 0x11, 0x1D (engine instant replay), then 0x1F **[P]**.

## 3. How the mod uses it

- On the injection tick (reason 5, ball in play) it only remembers the reviewed moment. It writes the request once 0x1F appears in the FSM queue (linear array at `MatchFSM + 0x878`, head `+0x8F0`, tail `+0x8F1`, tail exclusive) within 60 ticks, and only if slot 3 and the record are free and `+0x177F` is clear.
- Until the recorder commits, the window is recomputed every tick from the live ring, so frames recorded in between do not shift it.
- A watchdog counts ticks only while the current state is 0x1F. After 120 of them the request is reset to the idle state of `0x140A68200`, and 0x1F finishes with an empty record **[P]**.

## 4. To validate live

- Status 0, then -2, then -1 in both MatchInfo buffers right after the arm, during the stoppage cutscenes.
- That the record is filled (type 0x11) and played by 0x1F (director `+0x2D == 6`, controller source `0x0E`), and the order on screen against the 0x1D instant replay.
- That the record type returns to 0x13 after playback.
