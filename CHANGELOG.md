# Changelog

## v4.4.9

- **Tactical pitch at 60 Hz**: the match is recorded once per simulation frame (60 Hz, the replay's own rate) instead of 10 Hz, still covering the last 90 s. In the replay the pitch follows the play frame by frame, with no 100 ms steps.
- **Interpolation**: in slow motion and between two frames, ball and player positions are blended between the two neighbouring frames. Actions, contact and possession stay those of the nearest frame, and a stopped clock, a gap or a teleport (restart, substitution) is never blended.
- **Lighter search**: the replay cursor lookup walks an 8-byte key per frame instead of whole snapshots, and recording a frame no longer invalidates the event history cache. Measured: about 2.5 us per cursor move and 0.04 us per recorded frame, against a 16,600 us frame.
- Ring memory grows from 0.4 MB to 2.5 MB.
- **Camera follows the jump**: clicking a play on the replay bar now re-centres the 3D replay camera on its focus at the new frame. `ReplayPlayer::SetFrame` only stores the frame index, so the focus position read in the same call was the old one and the paused camera stayed put. For the next 8 frames the target moves onto the focus and the camera position moves by the same offset, keeping the angle and distance.
- **Suspect play**: every play the referee let go gets a missed-foul score (challenge type, whether the opponent went down from that contact, inside or near the box, contact distance). The strongest one at or above the threshold pulses in gold on the replay bar until you open it, then keeps a quiet gold ring. Its tooltip reads "LIKELY MISSED FOUL" and the incident card shows a gold dot. Official decisions and dives never qualify.

## v4.4.8

- **Generic installer**: auto-detection looks for any Steam library folder that has its own Sider, instead of a fixed folder name. Patch folders still take priority over the Sider in the game folder.
- **Documentation**: README, notes and comments refer to patches that use Sider, without naming a specific patch.
- No changes to in-match VAR behavior.

## v4.4.7

Triple stability check: crash and freeze audit across all native code, audit of every game memory write checked against the executable, release gate (DLL, Lua, `sider.ini`, logs and package) and the whole suite run with AddressSanitizer and UndefinedBehaviorSanitizer (20 of 20, no memory errors).

- **Complete set piece undo**: at the end of `MainTick` the engine already copies the referee result into the structure that records the card. The mod now also restores that copy when the restart is never set up, so no card stays recorded without the decision. The contract is locked by a test against the real executable.
- **Never a third yellow or a second red**, in any card adjustment path.
- **Hook installation without crash risk**: other threads are frozen while the code is written, and the mod checks that none is stopped in the middle of the replaced instructions, as the Detours library does.
- **No fps loss without a gamepad**: keyboard-only players no longer pay for the expensive query of empty XInput slots every frame while the panel is open.
- **Minimap protected against invalid positions** coming from the game (no infinite loop when drawing).

## v4.4.6

- **Interface at the same relative size at any resolution**: the scale follows the screen height (1.0 at 1080p) and is capped by the width, so the panel takes the same fraction of the screen from 720p to 4K, on ultrawide, 4:3 and 5:4. Before, at 720p the panel was proportionally larger than normal, at 4:3 and 5:4 the width was not considered, and above 4K it was small. There is a legibility floor on very small screens and a ceiling at 5K and 8K. The size chosen in Options (Compact, Default, Large) still applies on top.
- **Notices outside the panel**: the top and confirmation notices are centered in the free part of the screen when the open panel would end up underneath them.
- **Validated by test at 12 resolutions**: the proportional scale is checked at 12 resolutions, and at 7 of them, in both languages, the drawn panel fits entirely on screen with proportional width.

## v4.4.5

Final triple check from three new angles (executor timing against the binary, incident detection and classification, packaging and documentation) and a convergence round.

- **ImGui asserts really disabled**: the build cache did not recompile imgui when a header changed, so disabling the asserts in v4.4.2 never reached the published DLL. Now imgui is recompiled when its headers change, and the DLL shipped with no asserts (`_wassert` absent).
- **Set piece never left half done**: in `SetPlayReady` the engine can leave the state without setting up the restart (player switch, menu). The mod now checks this at the end of the same tick, before the card is recorded, and undoes everything it wrote (cards, restart and match foul, FSM fields). The decision goes back to waiting.
- **Faithful timeline**:
  - a foul whistled after an advantage stays at the moment of contact and not at the whistle
  - each official marker uses the strongest incident of the play
  - two simultaneous challenges no longer become dozens of markers
  - the zone (inside or outside the box) uses the side of the pitch at the moment of the incident, also after halftime
- **Advantage for the right team**: the benefiting team comes from whoever suffered the contact.
- **Dive**: the player who dived is the offender of the incident, and the box considered is the opponent's.
- **Re-queued package** merges with whatever was armed in the meantime, losing nothing.
- **Build**: the DLL only goes to `dist/` after the tests pass.
- **Installer**: an error in the menu (for example, the game being open) returns to the menu instead of closing. Uninstall keeps `var_mod.ini` with the shortcuts. `sider.ini` keeps its original end of file.
- **README**: manual install with `lua.enabled = 1`, limit of 32 card adjustments and open game warning.

## v4.4.4

New triple check: step-by-step simulation of eight decision combinations, audit of the panel state machine, review of the v4.4.3 changes against the executable (the TeamInfo double buffer was confirmed at every point) and a convergence round.

- **Decision never applied at the wrong time**: a foul or restart decided during a stoppage only holds for that stoppage. If the game moves on (restart, goal, halftime) before the decision runs, it is discarded with a notice and only the package cards remain. Before, a postponed decision could turn into a foul in the second half, with the spot mirrored, or change the kickoff after a goal.
- **Kickoff is never changed** by a VAR decision.
- **Second yellow and straight red**: a player with a straight red can still receive the yellow for the foul, with no third yellow or second red. The history shows the card actually given, and the second yellow uses the red notice.
- **Panel**:
  - confirming without a player shows the notice on the decision screen itself and moves to the Player field
  - clicking the minimap no longer changes the restart of an overturn
  - `[` and `]` do not switch the incident while a decision is open
  - the mouse no longer changes the selection of the page just opened
  - re-deciding the same incident appears as "Update" and not "Replace"
  - a refused decision warns instead of saying it was sent
  - long keys fit on the controls page
  - the pool texts are no longer rebuilt every frame.
- **Robustness**: validation of the team buffer happens before consuming the package, which stays armed until it can be applied. Re-queueing has a limit.

## v4.4.3

Triple check in successive rounds: four audits from new angles (end-to-end decision flow, contract with game memory checked against the binary, concurrency and lifecycle, interface and languages), then independent re-audits and diff reviews until no confirmed defect remained.

- **Cards now really persist**: the game keeps team data in two buffers and copies the write one over the read one every frame. The mod wrote only to the read one, so revoking or adjusting a card could be undone on the next frame. Now every card write goes to both buffers, validated beforehand, and every card read uses the write buffer. Without a validated buffer the mod applies nothing halfway.
- **Empty position never receives a card**: the game returns the record of the first squad player for a position with nobody in it. The mod now refuses that position.
- **Overturn the current foul and award an earlier foul in the same package**: the feature announced in v4.4.0 now works in both orders. The earlier foul replaces the restart and the overturn removes the card and marks the incident as overturned.
- **Offender is never invented**: without an identified player, the decision asks for the player to be chosen instead of assuming the goalkeeper.
- **Player identity everywhere**: replay incidents, direct edits and decisions armed before a substitution store who was in the position at that moment.
- **Foul card never doubled or lost**: the mod only completes the card by hand when the engine provably did not apply it (count equal to the one before the injection).
- **History faithful to the game**: card counts read from the game itself for the current incident, recorded even when only the card of a decision was applied, and an expired overturn (from another stoppage) only removes the card.
- **Decision is not lost**: if the engine generated its own result at the moment of application, the package goes back to waiting for the next set piece.
- **Runtime safety**: package from another match discarded, nothing armed with the VAR off, render reading suspended between the end of the match and the next game, match pointers revalidated every frame, truncated UTF-8 text never read past the end, HUD out of memory never closes the game, hooks never free code in use.
- **Interface**: names with Latin Extended letters (Modrić, Çalhanoğlu), long team names and titles with ellipsis, language selector and controls page without clipping, smaller minimap when space is short, "Replace in pool" button when the decision replaces another, pool updated in the same frame as the action, summary recalculated when an edit is merged.
- **Confirm decision**: the decision screen button now says what the user does ("Confirm decision", or "Update decision" and "Replace decision" when applicable), and no longer "Add to pool". On confirming, a short notice at the bottom of the screen says what happened: decision sent, updated or replaced in the decision pool, or card merged into the incident's decision.
- **Configuration**: `var_mod.ini` written atomically.

## v4.4.2

Final deep review with four independent audits (execution engine, state and panel, render and infrastructure, Lua module and installer). Each finding was checked in the code and in the binary before the fix.

- **Cards follow the player, not the position**: after a substitution, reviewing an old incident took the card from or gave it to the substitute who came on in the same position. Now each incident stores who was in the position at that moment, and cards and fouls go to that player. A substituted player never becomes the offender or the victim of a new foul.
- **Foul card never left half done**: if the taker kicked the free kick at the same instant the decision was applied, the original card could come out without the new one going in. Now the mod confirms whether the engine recorded the foul and, if it did not, applies the new card directly.
- **Coherent restart**: a foul with the ball in play that loses its offender, or that ended up without a card after a sending off in the same package, restarts without another foul's cutscene and without a quick free kick that would leave the sent-off player on the pitch.
- **Swap roles (Tab) and then convert** no longer leaves the offender and the benefiting team on the same side.
- **"Keep decision"** no longer erases a decision from another incident.
- **"Overturn current foul"** only binds to the current stoppage, never to an old foul by the same player. After it, the panel returns to the incident that was selected.
- **Editing the card of an incident that already has a decision** adjusts that decision instead of reverting the original card twice.
- **Timeline**: the "RED CARD" label is removed when the VAR withdraws the red.
- **Overturn a foul without an identified offender** uses the correct side of the pitch, and a foul injected without an offender is not recorded again as a referee incident.
- **New match without a Sider notice** (rematch, league and cup sequences) also clears the pool, the history and the panel.
- **Render stability**: ImGui asserts no longer close the game, a frame that cannot be drawn releases the input lock, match pointers are revalidated every frame and process shutdown no longer depends on locks.
- **Installer**: refuses to install or uninstall with PES open, copies the DLL before the Lua, preserves the encoding of `sider.ini` (BOM and line breaks), accepts folders with brackets, ensures `lua.enabled = 1` and prioritizes the installed patch's Sider during detection.
- **Key 7 in the Sider overlay**: holding the key no longer toggles on and off several times.

## v4.4.1

Analysis of the full dump of the freeze when advancing after the end of the match. Evidence in `re_notes/07_engine_heap_segment_release.md`.

- **Freeze and crash after the final whistle fixed**: on the post-match scene change the engine compacts the heaps and returns free blocks to Windows. The native release routine (`0x1403FC0D0`) released the whole allocation, failed halfway and then wrote to the block it had just released. The game froze while Windows wrote the dump and crashed at `PES2021.exe+0x3FE31D`.
- **Trigger**: patch Lua modules write to game memory through Sider and leave loose heap pages with execute protection. A single such page inside a block is enough for the native routine to get lost. The defect exists with or without the VAR.
- **Fix**: the mod replaces that routine with an atomic version that validates the whole block before releasing and releases per system allocation. Either the block goes entirely or nothing is touched. The protection applies to the whole process, including with the VAR off.

## v4.4.0

Research and evidence in `re_notes/06_replay_cutscene_and_classification.md`.

- **Pitch sides fixed**: the mod read the attack direction as a decimal number when the engine stores a signed byte. The side never flipped in the second half, so penalty spot, goal kick, corner, "inside or outside the box" and the minimap could end up on the wrong side, to the point of an awarded penalty being taken at the team's own goal. Now the side comes from the engine byte, calibrated at every kickoff by the player positions.
- **Incident classification redone**: the action table was swapped (the standing tackle became "slide tackle" and goalkeeper saves became "shoulder charge"). Now the incident follows the engine's own contact observer: the victim goes into a fall or loses balance and the engine reports who caused it. Slide tackle, tackle and body challenge come from the causer's action.
- **Near foul with yellow marker**: contact that brought a player down, with the causer identified and no whistle.
- **Dive**: a dive appears as "DIVE?". Goalkeeper actions and blocks no longer generate an incident, and different incidents are no longer merged just for being close.
- **Overturn the awarded foul or penalty in any context**: with the match stopped for a foul, the menu always offers to overturn that call, even with the cursor on another incident. This way you can award a foul on an earlier incident and overturn the current one in the same pool.
- **Overturn after the restart** only removes the card and says so, without swapping the current set piece for an old one.
- **Safeguards**: restart types that would lock the state machine are refused, and the advantage records the right team.
- **Replay diagnostics** (read only) in `var_core.log`, the basis for keeping the incident before the foul in the manual replay in a future version.

## v4.3.0

New decision execution engine, built on the engine's native flow (details in `re_notes/05_native_decision_pipeline.md`).

- **The engine applies the card and sends off**: the card for the foul awarded by the VAR goes through the native `CommitToRegistry`, which counts exactly once, and the sending off happens in the engine's own states `0x12` and `0x13`. The card pre-write, the cancellation of the engine count and the write of the "on pitch" flag are gone.
- **Sending off from a set piece card only** uses the native `RebuildSetPlay`, the same one the engine uses when changing taker.
- **Explicit state machine**: idle, waiting for the native hook, waiting for the transition and interval, with deadlines in simulation ticks. A match restart aborts the execution.
- **Nothing is written ahead of time**: cards are only written in the same tick in which the engine executes the decision. If it does not execute, nothing is left half done.
- **Real verification**: the decision only counts as applied, and the history is only recorded, when the FSM leaves the state in which the request was made and does not return to the ball in play.
- **Foul injected with the engine's default values** (sentinels `0xFF`, `0x18`, `9`, `4`), never with zeros.
- **Fixes from the independent review**: a foul converted against whom the original card sent off no longer loses the card, absorbed edits go to the chosen player, the referee result of the same frame is never overwritten and a sending off with the ball in play only runs when the incident itself makes the engine process the squad.
- **Pointers resolved by signature** are only accepted if the call lands in the executable's code section.

## v4.2.4

Deep review with three independent audits (game memory, new code and interface).

- **Nothing runs during cutscenes**: the mod called `EnterOutOfPlay` in the middle of cutscenes (states `0x03` to `0x11`), something the engine never does, aborting the cutscene and leaving objects hanging until the end of the match. Now the pool waits for the match to reach `SetPlayReady` or `InPlay`.
- **Sending off and revocation only at a set piece**: a cards-only pool that removes or returns a player from the pitch waits for the next stoppage, when the engine removes or replaces the player in its own flow.
- **Injected foul only with players on the pitch**: anyone already off the pitch (substituted or sent off earlier) never enters as offender or victim.
- **No field write without a native writer**: the mod no longer writes the `PlayerStatus + 0x0D` byte, and `QuickRestartFlag` is written with the same size the engine uses.
- **Card edit tied to the right incident**: editing the card of an old incident no longer changes the foul being taken.
- **Decision without a restart is not recorded**: if the incident changes before the injection, the history does not gain a penalty that did not happen, and the notice says only the cards were applied.
- **Pool and panel**: "Keep decision" also discards the card edits of that incident, the straight red respects the pending edit, the full pool warns when it discards the oldest edit, summaries follow the current language, the card result is no longer cut off and the panel does not clip the shortcut bar at 720p or with a full pool.
- **Enter on the decision screen** goes to the Confirm button instead of changing the row value.
- **Installer** enables `luajit.ext.enabled = 1` even when the line does not exist or is commented out.
- **README** with real error messages, DLL load timing and shortcut names matching the panel.

## v4.2.3

- **Spray line fixed**: awarding a foul through the VAR with the game already stopped at another foul no longer draws a spray line from the old spot to the new one. The referee spray (`VanishingSprayInfo`) is cleared by the same engine routine (`0x140A5CCE0`) before the new restart, as happens in every normal foul.

## v4.2.2

- **Ghost player fixed**: editing the cards of a player who was already off the pitch (substituted or injured) no longer puts them back on the pitch. The mod only takes a player off the pitch for a red or second yellow and only returns someone who had left by sending off. Before, this could leave a team with 12 players in the engine.

## v4.2.1

- **Sider module in Portuguese and English**: the module text in the Sider menu, including error messages, follows the language chosen in the panel. The choice is saved in `var_mod.ini` and applies the next time the game is opened.
- **Single version**: the version number comes from a single place (`native/src/version.h`) and appears identically in Sider, in the log and in `var_mod.ini`.
- **README with a complete guide**: requirements, installation from the Releases page, step-by-step usage, shortcut table and troubleshooting.

## v4.2

- **Fix for the crash when applying a foul** (`PES2021.exe+0x4B4C25`): the foul snapshot is now registered in the referee pool (`MatchFSM + 0x4B08`), which the engine copies to `MatchInfo` on every commit. Before, it was erased on the next commit and the event consumer read a null pointer. Additional protection in `FoulSnapshotPool::Find` (`0x140A82120`) for the 3 engine points that do not check for null.
- **Card never counted twice**: the engine's card commit is neutralized only for the VAR foul, even when it arrives late because of a cutscene.
- **Decision pool without losses**: editing the same incident again replaces the previous edit, different incidents by the same player coexist and "Keep decision" no longer erases the other decisions.
- **Safer execution**: injection fallback only in `InPlay` or `SetPlayReady`, FSM foul cleanup only when it is still the injected one, pitch flags changed only when the player enters or leaves and check of the `MatchInfo` allocation before writing to the write buffer.
- **Lifecycle**: DLL pinned in memory, game exit without uninstalling hooks under the loader lock and replay commands discarded on leaving the replay.

## v4.1

- **Composite Decision Pool (`VerdictPool`)**: allows stacking multiple decisions in the same trip to the Manual Replay (such as removing a player's card in the `CARDS` tab and awarding a foul or penalty with a card on another incident in the `REVIEW` tab) and applying them atomically on leaving the replay. Guarantees at most 1 active foul or restart at a time (replacing the previous incident with a visual notice) combined with independent per-player card adjustments, with an individual `[X]` button and `Clear`.
- **Definitive fix for fouls and set pieces in `SetPlayReady` (`0x1A`)**: identified and resolved the root cause where applying a foul when the taker was already positioned only produced a screen fade without changing the restart. The Fox Engine uses double RCU allocation in `MatchInfo` (`0x46F8` bytes, `Read Buffer [0]` at `+0x0000` and `Write Buffer [1]` at `+0x2378` copied by `0x14041BEC0` in `State_16` and `State_45`), and also requires passing through `State_01` (`0x14044DB80` -> `0x1404530C0` with `StoppageReason::SilentRestart = 7`, `0x14044F950` and `0x1404A8730`) and clearing `MatchFSM + 0x49C0` on the next tick after `CommitToRegistry`.

## v4.0

- **Fix for the crash when applying a decision**: the foul left in the match state machine now uses the engine's own sentinels (`0xFF`) and an existing snapshot (crash at `PES2021.exe+0x4B4C25`).
- **Real names and numbers**: shirt number read from the team sheet (`TeamEntry + 0x29A`). If the squad cannot be read, the panel falls back to `HOME` / `AWAY` and `Home N` / `Away N` (translated), never to empty text.
- **More robust squad reading**: memory check that crosses regions, team cache locked only with both names read and a new sync after every match restart.
- **Fix for the crash on closing the game** (`ucrtbase.dll`, `0xC0000409`) and persistent log with squad diagnostics (`RosterDiag`).
- **Gamepad only opens and collapses the panel** (`R3`, configurable). The panel is operated by keyboard and mouse and the gamepad stays in the replay.
- **New interface**: broadcast look, text in Portuguese with accents, keys drawn in the hints and fonts generated at the real resolution (720p to 4K).
- **Shortcut remap and language**: `OPTIONS > Controls` page and a Português (BR) / English selector.
- **Performance**: DLL from 1.88 MB down to ~860 KB, on-demand loading and 1 lock per frame in the replay.
- **Lua module compatible with Sider**: removed `pcall`, which does not exist in the Sider Lua environment.

## v3.0

- Simplified panel with keyboard, gamepad and mouse navigation and a cursor visible over the panel.
- Game input blocked while the panel is in focus.
- Fixes for slow startup and for the crash when applying a card.

## v2.0

- Redesign of the panel interface and features.

## v1.0

- First version: Sider module with native DLL, match hooks and VAR panel in the Manual Replay.
