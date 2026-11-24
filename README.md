# Practice Pathfinder (Geode mod)

Humanized Practice Mode auto-solver for Geometry Dash 2.2081 (Geode 5.11.2, Windows).
It plays the level by trial and error and sends its clicks through `GJBaseGameLayer::handleButton`,
so external macro recorders (xdBot, Mega Hack, Eclipse) capture them as normal player input.

## Download

Every push to `main` builds on GitHub Actions. Grab `lockyexe.practice-pathfinder.geode` from the
[`latest` release](../../releases/tag/latest) and drop it in your Geode `mods` folder.

## How to use

1. Open the level, enter **Practice Mode**.
2. Set your speed (e.g. 0.3x) and start recording in your macro bot.
3. Press **F6**, pick a mode with **F7**. The label in the top-right shows `[Pathfinder: ON (F6) | Mode: ... (F7) | CPs | Attempts]`.
4. Let it run. It places checkpoints by itself; your recorder's practice fix handles respawns.

Your own clicks are ignored while it's on (setting), so they don't end up in the macro.

## Modes (F7 cycles 1 -> 2 -> 3)

| | 1-Human | 2-Prodigy 15CPS | 3-Unreal ILL 15CPS |
| --- | --- | --- | --- |
| Min hold / release | 12-16 ticks (2-4 after >15 fails) | 6-8 ticks, never micro | 1-tick swift releases allowed |
| Click cap | none | >= 16 ticks between presses (15 CPS) | >= 16 ticks between presses (15 CPS) |
| Ship/wave band | ±10-16 + drift | ±2.5-5 + damped curve | ±0.8-1.5, no drift |
| Timing bias | 45-65% of window | 35-55% ±1 tick | none (chronological) |
| Jitter | 0-3 ticks | 0-1 tick | none |
| Auto checkpoint | 192-288 ticks | 150-210 ticks | 85-130 ticks |
| Backtrack after | 35 fails (setting) | 65 fails | 120 fails |
| Extra | | | >10 crashes within ±5 units → exact -8..+8 tick sweep (swift + sustained) |

All timings are physics ticks (240/s of game time), so it behaves the same at 1.0x or any speedhack.
F6/F7 are read by virtual-key code, so they work on Turkish Q/F and any other layout.

## Brain (v1.3)

The bot learns Geometry Dash physics while it plays (gravity, jump and UFO tap strength, ship thrust, robot boost,
terminal speeds, wave slope - per mode and size) and saves them between sessions. With that model it simulates
ahead against the real hitboxes:

- **Ground modes** (cube, robot, ball, spider): if doing nothing is predicted to crash, it finds the range of ticks
  where a click survives and clicks at the profile's natural spot inside it - before ever dying there.
- **Flying modes** (ship, wave, UFO, swing): a guard simulates "keep doing this" vs "switch now" and overrides the
  corridor controller when it predicts a crash.
- **After a death**: every candidate fix is simulated; predicted passes are tried first. The brain is only trusted
  at a spot if it reproduces the death that actually happened there. The label shows how often it does (`Brain 87%`).
- Orbs, pads, portals and slopes aren't simulated; there it falls back to the search.

## Speed & accuracy features (v1.2)

- **Instant respawn**: skips the game's respawn delay (auto-disables if your setup doesn't support it).
- **Fast-forward**: the solved part of a retry runs at 8x (setting) on top of your speedhack; it slows down 40 ticks before the spot being fixed.
- **Live objects**: reads the game's own collision sections every tick, so moving blocks and hazards are where they really are.
- **Progress-guided search**: tries clicks next to whichever try got furthest, on the side that did better.
- **Stuck detector**: 10 tries without progress → two-click setups → change inputs further back → backtrack.
- **Dead-end memory**: the exact player state at a dead checkpoint is remembered; no new checkpoint is placed in that state.
- **Replay drift check**: if a replay of known inputs doesn't reproduce, the death isn't blamed on the candidate.
- **Checkpoint recheck**: safety is re-checked at the exact moment the checkpoint is placed.
- **Natural taps**: tap lengths vary like real clicks (Mode 1 ~50-90 ms, Mode 2 ~25-55 ms).
- **Polish pass**: a found click is moved to a natural spot inside its working window; kept only if the run survives.

## How it works

| Piece | What it does |
| --- | --- |
| Segments | Every checkpoint starts a segment with its own input plan (press/release events at physics ticks). Practice checkpoints restore `m_currentProgress`, so a replay lands on the exact same ticks. |
| Live play | Ground modes do nothing by default (optional jump assist over visible spikes). Ship / wave / UFO / swing use a corridor controller: safe band between nearby blocks/hazards and the mode's floor/ceiling, hysteresis of ±10-16 units around a Perlin/sine-drifting line. |
| Deaths | The death tick opens a *cluster*. Inputs before a mode-dependent look-back window are kept; candidates for the window are tried in this order: no input → one click at window-centered ticks (orb windows, platform edges, usual reaction distance) → longer holds → two clicks → micro-clicks (only after >15 fails). Flying deaths first try steering the controller higher/lower, tighter, or with more look-ahead. |
| Checkpoints | Placed after 0.8-1.2 s of safe progress or right after clearing a portal/orb/pad, only when grounded (ground modes) or in the inner half of the corridor (flying), never before the spot that's currently being fixed. |
| Backtracking | More than `max-failures` (35) deaths on one checkpoint → it is deleted, the parent segment's prefix is blacklisted and the parent is forced to try different inputs into it. |
| Humanizer | 12-16 tick hold/release floor (2-4 only on spots that failed >15 times), 0-3 tick jitter when safe, extra jitter when the rhythm turns periodic, candidates with machine-regular rhythm get sorted later. |

## Limits

- It's a search, not a perfect physics simulator: hard parts take many deaths. At 0.3x that takes a while.
- Platformer levels aren't supported. 2-player levels get a separate plan per player; regular dual uses one.
- Moving objects are indexed where they were at respawn time.

## Build

```
geode build
```
or push and let `.github/workflows/build.yml` build it.
