# The Menacer and input path (issue #3)

What the game does with the Sega Menacer (port 2) and the pads, measured headless on the patched
GPGX core with `tools/harness/harness`. Routine and RAM names are in `data/symbols.txt` and
`data/ram.txt`. All scripts named below are in `tools/harness/scripts/`. The generated ones come from
`tools/py/gunscripts.py`. The section at the end shows how to reproduce everything.

## Short answers

| Question | Answer |
|---|---|
| Missile button | Menacer **D0** = GPGX `INPUT_B` = libretro `LIGHTGUN_AUX_A` (harness `BTN 2`). The game maps it to pad **B**. |
| Other gun buttons | Trigger (D1, pad A) fires and autofires while held. **Start** (D3) pauses/unpauses and continues. D2 (`AUX_B`, pad C) does nothing in game, except in the level-skip cheat. |
| Menus with the gun alone | **No.** Title, main menu, OPTIONS: pad 1 only. Continue: the gun's own **Start**. The trigger does not work there. High-score initials: **yes**, aim and shoot the letters. |
| Accuracy | Y is exact. X error is between -2 and +4 px (mean +1.3), rising slowly towards the right edge, and the game X is always even. A 320-entry LUT on the host brings it to within ±2 px (mostly 0/+1). |
| Latency | The game averages the last 8 (moving) or 16 (still) per-frame latches, so the crosshair trails the gun by 4-8 frames. A 60-byte ROM patch (`tools/py/gunpatch.py`) makes it 1 frame. |
| Menacer vs pad | Same weapons, autofire, gunpower and difficulty. The differences are the crosshair (absolute and smoothed, or pad acceleration 1..8 px/frame), a 2-frame debounce on the Menacer missile, and the clamps. |
| Cheat | **U D L R U D L R** on pad 1 at "PRESS START". In game, pause and press **B+C** (gun: AUX_A+AUX_B) on the pausing player's device: the level ends. |
| **Host gotcha** | GPGX's default gun input ("touchscreen", `RETRO_DEVICE_POINTER`) passes **only the trigger**: no missiles, no pause, and you **cannot continue**. Set the core option `genesis_plus_gx_gun_input = lightgun`, or emulate the Menacer directly. |

## 1. Read routines and RAM

### Hardware flow (Menacer modes)

1. **MenacerInit** `$01E346` is called from GameStart (`$007476`) when `control_mode` bit 0 is clear,
   that is in every mode with a Menacer. It sets `gun_y_offset $FF9F78 = 8` and `gun_h_offset = $84`, and
   builds the H→X table `$FF9F7E` (256 bytes) with **MenacerBuildHTable** `$01E3B0`. The table counts
   in X/2 units, starting at H = `$84` and stepping 1.125 per H unit (16.16 arg `$12000`). The values
   for H `$B6..$E4` (the H-counter jump in H40) are flat, and the table is capped at 160 (X = 320).
   MenacerInit also resets the latch ring pointer and writes VDP reg 1 = `$64` and reg 10 = `$08`. It
   sets port 2 ctrl `$A1000B = $B0` (TH interrupt enabled, TR/TL outputs) and VDP reg 0 = `$06`
   (HV latch on). Reg 11 = `$0A` in gameplay (external interrupt enabled). Reg 0 reads back `$04`
   later in gameplay, so the latch bit is off then (VDP dump, `vdp` command).
2. **ExtInt** `$00039E` (level 2, TH): only the **first** TH per frame counts
   (`gun_latch_cur $FF9F76 == 0`). It reads the HV counter `$C00008` into the 16-word ring `$FFA07E`
   (pointer `$FFA09E`) and increments `gun_latch_cur`.
3. **VBlankInt** `$000308` (level 6): if `control_mode` bit 0 is clear, it copies `gun_latch_cur` →
   `gun_latch_count $FF9F74` and clears it. Then it runs the Menacer button handshake on port 2 data
   `$A10005` ($FF, $DF, wait, $FF, $DF, wait, read, $FF, $CF), inverts the byte and stores it in
   `gun_btn_raw $FF9F77`.
4. **MenacerReadXY** `$01E442`, called by the input routines below: `gun_btn $FF9F72` ← raw buttons
   (previous in `$FF9F73`). If `gun_latch_count` is 0 (gun off screen), X/Y are left as they were.
   Otherwise it sums table[H] and V over the newest 8 latches and over the 8 before them:
   * X: if the two sums differ by ≥ `$28` (about 10 px of movement), `X = sum_new/4`, else
     `X = (sum_new+sum_old)/8`. Both are 2 × an average of table values, so **X is always even**.
   * Y: if they differ by ≥ `$14`, `Y = sum_new/8`, else `(sum_new+sum_old)/16`. Then `+ gun_y_offset`.
   * Results: `gun_x $FF9F6E`, `gun_y $FF9F70`.
5. **ReadInput** `$0003DE` (buttons only; pause, continue) and **ReadInputAim** `$0004B6` (main game
   loop, buttons plus crosshair) dispatch on `control_mode $FF9DF5`:

| mode | menu entry | P1 `$FF8000` | P2 `$FF8050` |
|---|---|---|---|
| 4 | START 1 PLAYER - USE MENACER | Menacer | pad 1 (read, P2 not playing) |
| 5 | START 1 PLAYER - USE JOYPAD 1 | pad 1 | pad 2 |
| 0 | START 2 PLAYER - 1 MENACER / 2 JOYPAD 1 | Menacer | pad 1 |
| 1 | START 2 PLAYER - JOYPAD 1 / JOYPAD 2 | pad 1 | pad 2 |
| 2 | (no menu entry) | Menacer only, Y not clamped | - |
| 7 / 8 | attract demo / demo interrupted | | |

   Bit 2 means "one player". When a second player joins, `andi #3` turns 4 → 0 and 5 → 1 (`$007A2C`).
   The menu table is `$000FF0`. The Menacer and 2P-Menacer rows are only enabled if
   **ReadPort2Id** `$001BE8` returns 0 (Menacer). The 2-pad row needs id `$D` (pad) on port 2.

   The Menacer buttons are remapped into the pad layout (b0 U, b1 D, b2 L, b3 R, b4 B, b5 C, b6 A,
   b7 Start, 1 = pressed): **trigger (D1) → A**, **D0 → B**, **D2 → C**, **Start (D3) → Start**.
   In modes 0 and 4 only the top nibble is kept, and if Start is held all the other buttons are
   dropped (`$0005C4`). ReadInputAim then copies `gun_x` → `p1_x $FF8004` and `gun_y` → `p1_y $FF8006`,
   clamping Y to 24..200 (not X, and not in mode 2). It also saves the buttons from two frames back in
   `btn_prev2 $FF9F66`.
6. The pads are read by **ReadPad1** `$0088A0` (port 1 → `$FF8050`) and **ReadPad2** `$008908`
   (port 2 → `$FF8000`). They do a standard 3-button TH read. In the two-pad modes the two structs
   are swapped around the call, so pad 1 lands in P1.
7. Crosshair: **QueueCrosshair** `$00071C` → **QueueSprite** `$013240`(id = player index + 1).
   In the VDP sprite table it is the first sprite: 16x16, tile `$7B6`, palette 2, top-left at
   **(X-8, Y-8)**. The crosshair is centred on `p1_x/p1_y`, and shots use exactly those coordinates
   (`$00086C` → `shot_xy $FF9E94` → **ShotHitTest** `$005892`). Checked with the new `sprites` command
   (X=102,Y=80 → sprite at 94,72).

There is **no calibration screen**. The offsets come from MenacerInit. The emulator matters here:
GPGX recognises T2 by name (`core/loadrom.c`), forces a Menacer on port 2, and uses the per-game
offsets `x_offset = 133`, `y_offset = -8`. It also scales the Menacer X by 289/320
(`core/input_hw/lightgun.c`) and forces the HV latch even when reg 0 has it off (the "HACK"
branch). The near-exact aim below is the result of that GPGX tuning.

### Libretro mapping in GPGX

| harness `gun ... BTN` | libretro (lightgun mode) | GPGX | Menacer data bit | game |
|---|---|---|---|---|
| 1 | `LIGHTGUN_TRIGGER` | `INPUT_A` | D1 | A: fire / autofire |
| 2 | `LIGHTGUN_AUX_A` | `INPUT_B` | D0 | B: **missile** |
| 4 | `LIGHTGUN_AUX_B` | `INPUT_C` | D2 | C: nothing (cheat combo only) |
| 8 | `LIGHTGUN_START` | `INPUT_START` | D3 | Start: pause, continue |

GPGX's own comment labels D0 and D2 "???", so which physical Menacer button sits on D0 is not
confirmed here. For the mouse: left = trigger, right = AUX_A (missile), middle = Start.

**Pointer-mode trap.** With no core option set, GPGX uses "touchscreen" gun input
(`RETRO_DEVICE_POINTER`). In the non-bitmask input path (`libretro.c` ~line 773) only
`POINTER_PRESSED` → trigger is read. `POINTER_COUNT` (which would give B/C/Start) is ignored there.
`gun_buttons.txt` without `GUNMODE=lightgun` shows missiles, Start and C having no effect at all.
The harness now answers `RETRO_ENVIRONMENT_GET_VARIABLE`: env `GUNMODE=lightgun` or
`CORE_OPTS="key=value,..."`.

## 2. Buttons in game (`gun_buttons.txt`)

* **Trigger (A)**: on the press edge a shot fires at once (`fire_flag $FFA0A2`). While held,
  `p1_fire_timer` counts down from `p1_fire_period` and fires again at 0 (autofire). Holding also
  drains gunpower (see 5).
* **D0 / AUX_A (B) = missile**: `p1_missiles $FF8018` (BCD, the `0025` in the HUD) goes 0025 → 0024
  one frame after the press. For the Menacer player (mode 4, or mode 0 P1) the button must be seen in
  two consecutive frames and not in the frame before (`btn_prev2` debounce, `$0007D6`), so a 1-frame
  tap fires nothing. Hold it for at least 2 frames. With 0 missiles you get an "empty" sound (`$77`).
  Continue refills to 0050.
* **Start**: pauses (`PAUSE` in the HUD) and unpauses. It is checked on P1, or on P2 if P2 is playing
  (`$007676`). During the continue countdown, Start continues.
* **D2 / AUX_B (C)**: reaches `$FF8000` b5 but no game code tests it, except the cheat (B+C in pause).
* In 1P Menacer mode, pad 1 does nothing in game: no pause and no join (tested: pad Start and A changed nothing).

## 3. Screens: which input they read (`gun_screens.txt`)

| Screen | Routine | Input | Gun alone? |
|---|---|---|---|
| Intro logo / "PRESS START" title | `$000E22`, `$001B72` | ReadPad1, Start; cheat code | no (gun trigger and Start ignored, pad Start works) |
| Main menu (1P/2P choice) | `$001164` | ReadPad1: U/D, Start | no |
| OPTIONS (credits 3-5, sound test) | `$0013A2` | ReadPad1: U/D, L/R, A/B/C, Start = exit | no |
| Level intro story text | - | none: advances on a timer, no input skipped it (first enemy at the same frame with gun Start, trigger, pad Start or nothing) | n/a |
| In game | `$0004B6` / `$00074C` | P1/P2 devices per mode | yes |
| Pause | `$007714` loop | ReadInput, pausing player's own device | yes (gun Start) |
| Continue countdown ("CONTINUE 5..0") | `$008DFE` | ReadInput, dead player's own device, Start only | gun **Start** only; trigger and pad 1 Start are ignored (credits 4 → 3 only on gun Start) |
| High-score initials (level `$0A`) | normal game loop | crosshair + fire: `PlayerShoot` hit-tests the letter grid | **yes**: aim + trigger on A, B, C, ENTER gave "ABC 500000" in the table |
| 2-player menus | same main menu | pad 1 | no. In-game 2P (mode 0) is gun = P1, pad 1 = P2, each player pauses/continues with its own Start |

So a mouse host must inject pad-1 presses for the title, menu and options (Up/Down/Left/Right/Start,
plus A for the sound test). It must also give the gun a Start button. With the
"1 MENACER / 2 JOYPAD 1" mode, a real pad on port 1 is P2.

## 4. Aim accuracy (`gunscripts.py grid`, `gunscripts.py xsweep`)

Each point reloads the level-1 state, holds the gun for 24 frames (the average then contains only new
latches) and reads `p1_x/p1_y`. The crosshair sprite is at exactly (X-8, Y-8), so RAM and the
sprite agree.

```
X error (game X - commanded x), 16 px grid, same for every row y = 16..223:
x:    0  16  32  48  64  80  96 112 128 144 160 176 192 208 224 240 256 272 288 304 319
err:  2   2   0   0   0   2   2   0   0   2   2   0   0   0   2   2   2   2   4   4   1
```

* **Y: exact** for y = 16..223 (raw `gun_y`). Game-side clamp to 24..200 in modes 0/4: the HUD
  rows cannot be aimed at. **y = 0 never latches** in GPGX: the position is kept.
* **X**: the full sweep (every x, y = 100) gives errors of -2 (1), -1 (18), 0 (63), +1 (99),
  +2 (87), +3 (43), +4 (9). The best linear fit is `X = 0.34 + 1.006·x`. X is always even, from 2 to
  320, and every 18 px one even value is unreachable (2·table step 1.125 vs GPGX's 289/320 pixel
  scale: 16, 34, 52, ... 304).
* The scale GPGX applies (×289/320) is almost exactly what the game expects. The 1.125-per-H-unit
  table and 2 px per H unit leave a residual 0.6 % gain plus a sawtooth.
* **Host correction (optional):** a 320-entry inverse LUT, measured by `gunscripts.py xsweep`, that
  picks the commanded x whose game X is nearest the mouse x. Residual: 0 (142), +1 (131), -1 (29),
  ±2 (18). A plain linear inverse is worse (up to ±3). The crosshair is 16 px and hit boxes are
  larger, so without a correction the error is still small (≤ 4 px). It is not worth it unless the
  port shows the OS cursor. In the native port (no VDP), write `gun_x/gun_y` directly and the error
  is 0.

### Latency (`gunscripts.py lag`)

The averaging is the bigger problem for a mouse:

* A jump of 200 px moves the crosshair in a linear ramp over **8 frames**, about 25 px per frame.
* A small move (4,2 px) takes up to **16 frames** to settle.
* Constant motion at 1 px/frame trails by about **6 px** (6-7 frames).
* Shots go to the trailing position, so flick-and-click shooting misses.

`tools/py/gunpatch.py IN OUT` writes a ROM copy in which MenacerReadXY converts only the newest latch
(60 bytes of new code at `$01E460`). The game has no checksum check: the patched ROM boots and plays.
With it the 200 px step lands in **1 frame** and the ramp no longer trails. This is a host/ROM-side
choice for P1. The decomp/native build gets it for free.

## 5. Menacer mode vs pad mode

* **Crosshair**: Menacer = absolute and smoothed (above). Pad (`PadMoveCrosshair` `$0005FC`) =
  acceleration table `$000738`: 1, 1, 1, 2, 2, 3, 4, 5, 6, 8 px/frame per held direction, so the
  speed peaks at 8 px/frame after about 9 frames. X is clamped to 8..312 and Y to 24..200 (measured:
  holding Right moves X 41 → 312 in 39 frames).
* **Fire / autofire**: identical code for both (`PlayerButtons` `$00076A`). Holding A or the trigger
  autofires with period `p1_fire_period = 2 + (7 - gunpower>>13)` frames (+1 for gun type 3): 2 frames
  (30 shots/s) at full gunpower, down to 9 frames (6.7/s) when empty.
* **Gunpower** `p1_gunpower $FF801C`: −`$C0` per frame while the trigger is held (unless the coolant
  timer `+$2A` is negative) and +`$60` per frame always. Holding from full empties it in about 700 frames
  (11.5 s). Measured over 600 frames (`gunscripts.py tap`): tapping 1 on / 1 off fired **296**
  shots with gunpower still full. 2 on / 2 off fired 150. Holding fired **148**, with gunpower down to
  `$1F5F`. So fast tapping beats holding. A "turbo" mouse button that pulses the trigger would be
  stronger than holding, so leave it to the player.
* **Missile**: Menacer needs B in 2 consecutive frames (debounce), the pad needs only the edge.
* **Difficulty**: no difference found. Every reference to `control_mode` is in input, setup,
  continue/out or the demo (`$00030C`, `$0003E0`, `$0004B8`, `$0007B6/$0007C2`, `$000926..$000978`,
  `$000FDA`, `$001C8C`, `$007436..$007A30`, `$008E6C/$008E74`, `$00B2FE..$00B360`). No enemy or
  damage code reads it.
* **Off screen**: the Menacer has no off-screen reload. With no latch the crosshair stays put.

## 6. RAM map, gun-bot and cheat

Full list: `data/ram.txt`. The essentials:

| What | Where |
|---|---|
| P1 / P2 struct | `$FF8000` / `$FF8050`: +0 buttons, +4 X, +6 Y, +$18 missiles (BCD), +$1C gunpower, +$1E energy, +$22 state, +$24 index (-1 = not playing) |
| Score P1 / P2 | `$FF9EA8` / `$FF9EB0`, BCD longs |
| Credits | `$FF80CA` (byte) |
| Level | `$FF80CC` (1.., `$0A` = high-score entry) |
| Players alive | `$FF9F6D` |
| Mode | `$FF9DF5` (table in 1) |
| Leave level / game over | `$FFA234` (`$FF` / `$FE`) |
| Frame counter | `$FFA0EE` (VBlank) |
| Objects | `$FF87B2`, stride `$2C`; `$FF80DA` = number of live slots. Per slot: +0 flags (b15 shootable), +2 X, +4 Y (screen px, the feet: aim ~20 px higher), +8 hit points, +$1C hit-test handler (identifies the type), +$20 AI routine |
| High scores | `$FFFF9E`, 10 × (3 initials, 0, BCD score) |

**Gun-bot recipe.** Each frame, walk `$FF87B2` until `$FF80DA` live slots have been seen (skip slots
with +0 = 0). Pick a slot with +0 b15 set. Aim at (+2, +4 − ~20) and hold the trigger. Fire a missile
(AUX_A for 2+ frames) when several objects are on screen. Keep `p1_energy` and `p1_gunpower` in view.
The hit box comes from the table passed to `HitBox` `$005940` by each object's +$1C handler.
Implemented as the harness `bot` command (`tools/harness/bot.c`): it learns the boxes, the dangerous
types and the human handlers through the cpu hook. Results per level: `docs/levels.md`.

**Cheat / level select.** There is no level select or debug menu in the strings. There is a
**level-skip cheat**: on the "PRESS START" title, enter pad 1 **Up, Down, Left, Right, Up, Down,
Left, Right** (`CheatCodeCheck` `$000C78`, table `$000CBA`; each press must be a clean new state, and
a wrong press resets). It sets `$FFA0A6`, and is cleared at reset (`$008B02`). Then, in game, pause
with Start and press **B+C** on the pausing player's device (gun: AUX_A+AUX_B). The level ends and
the stats screen follows. `gun_levelskip.txt` goes from level 1 to level 2 this way. This is the
coverage shortcut that PLAN.md asked for. Poking `$FFA0A6 = $FF` does the same without the code.

## Menus by mouse (issue #10)

How the host tells the screens apart (there is no screen variable in RAM). Between frames the 68k
is nearly always in `WaitVBlank` (`$0086E2`..`$0086F4`; it pushes d0 first), so the long word at
sp+2 is the return address into the loop that waits:

| Return address | Loop | Screen | Mouse action (`port/src/mouse.c`) |
|---|---|---|---|
| `$000E28` | WaitStartWindow | the three logo screens (300 frames each) | Start; only accepted for `vblank_count` 180..299, as an edge |
| `$0045E0` | `$0042CE` StoryIntro | story text, 550 frames | Start held skips it |
| `$001B9E` | PressStartLoop | PRESS START | Start edge |
| `$00116A` | MainMenuInput | main menu, 5 rows at y = 32 + 24 n, cursor `$FF9F60` | Up/Down until the cursor is the row, then Start |
| `$0013A8` | OptionsInput | OPTIONS, 5 rows, cursor `$FF9F60` (rows: credits `$FFAE84` 3..5, sound effect 0..35, sample 0..6, music 0..7, EXIT) | Up/Down, Right/Left for values, A plays a sound row, Start leaves |
| `$008DFE`..`$008E66` | ContinueCountdown | "CONTINUE 5..0" (1 player) | left click = the gun's Start |

In a 2 player game a dead gun player (`p1_state` = 7) also waits for Start in the game loop; the
left click is Start then too. `ReadPad1` writes the pad it read to `$FF8050` (previous: `$FF8052`);
the host holds each injected press until it shows there, and holds the release until it is gone,
because every one of these loops wants a clean edge (`btn & ~prev`). The "2 JOYPAD" row is refused
by the main menu loop when port 2 is a Menacer (`$000FC0`: it needs peripheral id `$D`).

## Harness changes

`tools/harness/harness.c` (documented in its header):

* `GUNMODE=lightgun|touchscreen` and `CORE_OPTS=...` answer the core options.
* `gunhold X Y BTN` sets the gun state without running frames.
* `watch ADDR LEN` / `unwatch` log every 68k write to a range, with its PC, to `watch.log`.
* `bp ADDR` / `bpclear` log executions of an address to `bp.log`.
* `peekeach LABEL ADDR [W]` logs a value whenever it changes.
* `waitfor ADDR W VAL MAX` runs until a RAM value matches.
* `sprites NAME` dumps the VDP sprite table in link order, in screen coordinates.

## Reproduce

```sh
gcc -O2 -I ext/gpgx/libretro/libretro-common/include -o tools/harness/harness tools/harness/harness.c -ldl
H="nice -n 19 ionice -c3 timeout 300 env GUN=1 GUNMODE=lightgun tools/harness/harness ext/gpgx/genesis_plus_gx_libretro.so rom/baserom.md"
mkdir -p runs/gun
$H tools/harness/scripts/gun_boot.txt runs/gun        # states title, menu, l1
$H tools/harness/scripts/gun_buttons.txt runs/gun     # 2
$H tools/harness/scripts/gun_screens.txt runs/gun     # 3 (continue, high-score entry)
$H tools/harness/scripts/gun_levelskip.txt runs/gun   # 6 (cheat) -> state l2
for k in grid xsweep tap lag; do
  python3 -I tools/py/gunscripts.py $k > runs/gun/$k.txt
  mkdir -p runs/gun-$k && cp runs/gun/l1.state runs/gun-$k/
  $H runs/gun/$k.txt runs/gun-$k
  python3 -I tools/py/gunscripts.py report runs/gun-$k
done
```

A run takes seconds. The grid is 7560 frames and the x-sweep 6400. `report` has no summary for `lag`: read `runs/gun-lag/peek.log` (frame, X, Y per change).
