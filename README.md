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
3. Press **F6**. The label in the top-right shows `[Pathfinder: ON | CPs | Seg Attempts | Mode]`.
4. Let it run. It places checkpoints by itself; your recorder's practice fix handles respawns.

Your own clicks are ignored while it's on (setting), so they don't end up in the macro.

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
