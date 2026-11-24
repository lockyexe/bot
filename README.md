# Macro Refiner (Geode mod)

Cleans up a Geometry Dash 2.2081 bot macro — 199 CPS mid-air spam, noclip deaths, robotic 1-tick
timing — and only saves the result once it beats the level at **100%** with noclip off, in the real game.
Geode 5.11.2, Windows + Android.

## Download

Every push to the `macro-refiner` branch builds on GitHub Actions. Grab `lockyexe.macro-refiner.geode` from the
[`macro-refiner-latest` release](../../releases/tag/macro-refiner-latest) (or the run's **Macro Refiner** artifact) and
put it in your Geode `mods` folder.

## How to use

1. Mod settings → **Macro file** → pick the macro (`.gdr` / `.gdr.json` from xdBot, or `.gdr2`).
2. Open the level (from the start, no start position) and pause.
3. Press **Refine Macro** (bottom-right of the pause menu). A progress line shows at the top; **Stop** cancels.
4. When it's verified, a popup shows where `realistic_output.<ext>` was saved (next to your macro, same format).

It runs in practice mode, so nothing counts as a real completion and your stats aren't touched.
Turn off other bots' playback/noclip while it runs; your own clicks are ignored.

## What it does

All physics come from the game itself: inputs go to `PlayerObject::pushButton` / `releaseButton` on exact ticks
(`GJGameState::m_currentProgress`, 240 TPS), deaths come from `PlayLayer::destroyPlayer`, and snapshots are
GD's own practice checkpoints (`CheckpointObject`: `GJGameState`, P1/P2 `PlayerCheckpoint`, every object and
trigger state). Snapshots are taken every 20 ticks into a ring buffer (64 by default) together with the P1/P2
position, Y velocity, rotation, speed, size and mode flags, which are compared after each rollback.

| Step | |
| --- | --- |
| Read | One noclip run with your inputs, logging each player's ground/mode/orb state per tick. xdBot macros (level-time frames) are converted to exact ticks here. |
| 1A | Cube/Robot/Spider presses made in mid-air that touched no orb within ±3 ticks are deleted. Presses held into a landing (buffered jumps) are kept. |
| 1B | Re-presses less than 12 ticks after a release are merged into one hold; a merge is kept if noclip-off play survives 120 ticks after it (or dies no earlier than without it). |
| 2 | Noclip off from 0%. On a death at tick F: roll back to a snapshot at or before F−60, then try shifting the nearest press/release by ±2, ±4 … ±12 ticks (or insert a jump F−24 … F−8 if nothing was pressed in the last 60 ticks). The first edit that survives past F+80 is kept and the run continues. If nothing works it looks further back (F−140, F−240) and also tries adding/removing jumps. |
| 3 | Holds shorter than 14 ticks get +20/+16/…/+4 ticks longer releases if that survives and doesn't cause a re-jump on landing. Jump starts are tried at −3…+3 ticks and moved to the timing with the most room from hazard hitboxes. Any death this causes is repaired as in step 2. |
| Verify | One clean run from 0% with noclip off must reach the end; otherwise step 2 runs again (up to 3 times). |

The output is a normal GDR macro with bot name `Macro Refiner`, so xdBot plays it on `m_currentProgress` ticks.
Position "frame fixes" from the original are removed (they belong to the old inputs).

## Speed

**Physics ticks per frame** (default 500) and **Max work per frame** (default 30 ms) in the settings. The driver
steps the level from `CCScheduler::update` with the level's own scheduler paused, in 20-tick chunks, and stops a
frame's work early when the time cap is hit, so phones don't freeze (no Android ANR).

## Limits

- Classic levels only (no platformer), 240 TPS macros only, from the level start.
- Snapshots are GD practice checkpoints; if a level's triggers don't restore perfectly from a checkpoint, the
  final from-0% check catches it and repairs again.
- Clearance for step 3 uses hazards' positions as indexed at level start (moving hazards are checked at their live
  position only when they're still near where they started).

## Build

```
geode build
```
or push and let `.github/workflows/build.yml` build Windows, Android32 and Android64.

Not for the Geode index (AI-written); personal use.
