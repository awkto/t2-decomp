How the coverage was gathered (initial pass, on the dev machine, `tools/py/campaign.py` / `tools/py/fuzzscript.py`, harness with
`GUN=1`): attract mode from power-on (20000 frames); a Menacer game (title menu default item, gun Start through the intro text,
then random gun aim with trigger / Menacer A / B buttons, Start pressed now and then to get through game over and the title);
and a joypad game ("START 1 PLAYER USE JOYPAD 1", second menu item) with random pad input. Three seeds of 20000 frames each for
the two game modes. Only the first stage and the first minutes of play are covered; a deeper campaign (all levels, 2-player,
options, high-score entry) comes later with a gun-bot.
