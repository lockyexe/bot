# Macro Refiner

Turns a bot macro into a clean, human-looking one and checks it in-game.

1. Pick your macro in the settings (<cy>.gdr</c>, <cy>.gdr.json</c> or <cy>.gdr2</c>).
2. Open the level, pause, press <cg>Refine Macro</c>.
3. Wait for the 100% check. The result is saved as <cg>realistic_output</c> next to your macro.

What it changes:
- deletes mid-air cube/robot spam that does nothing
- merges 50 ms re-clicks into smooth holds when that's safe
- fixes every noclip death by moving nearby clicks (rollback search)
- lengthens tiny taps and nudges jumps away from spikes

Everything runs on the game's real physics in practice mode, so nothing counts as a completion.
