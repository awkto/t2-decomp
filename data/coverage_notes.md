How the coverage was gathered:

1. Initial pass (issue #6 part 1, `tools/py/campaign.py` / `tools/py/fuzzscript.py`): attract mode, and random play in a
   Menacer and a joypad game (3 seeds of 20000 frames each). 58.8 % of the then 14461 known instructions, first stage only.
2. Gun-bot level campaign (issue #6 part 2, `tools/harness/levels.sh runs/C boot chain full full2p fullpad ending menus
   attract honest`, on a VM, about 1.4 M frames in 35 runs): the four menu modes from power-on; the bot played 900 frames of
   every level and skipped it with the level-skip cheat, in all four modes; every level played to its end by the bot with
   the energy / protect / freeze cheats in 1P Menacer, 2P (Menacer + pad) and 1P pad mode; the forced ending in both text
   variants; OPTIONS (credits, sound tests), continue, game over, P2 joining a 1P game, the attract loop; honest (no cheat)
   1P Menacer and 1P pad games until the credits ran out. Results per level: `docs/levels.md`. The static seeds added for
   the hand-resolved indirect sites (issue #7) raised the instruction count to 17619.

What is left unexecuted and why: `data/gap_notes.txt` (comment block at the end).