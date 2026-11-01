# Object types on screen, level by level (issue #7 follow-up)

What each object-table type (`$FF87B2`, `$2C`-byte slots; type in the low byte of +0) is on
screen, per level. Method: load each level's start state, run frames (gun-bot or plain `run`),
dump the live table with the harness `objs` command and screenshot the same frame (`shot`), then
match each object's type/X/Y to the sprite drawn at that spot. Evidence is the state/frame and the
screenshot; hit/AI addresses come from the dumps and `src/t2.s`. Slot layout and hit-test rules are
in `docs/subsystems/objects.md`; levels in `docs/levels.md`.

Confidence: **high** = sprite and role obvious; **medium** = role clear, exact sprite ambiguous;
**low** = inferred from code/data only. Guesses are marked with `?`.

## Level 1 - Los Angeles, 2029 (future war street)

Evidence: `gun_01` state; frames 1500-25500 (dumps `G1_*`, `N1_*`, screenshots `G1_*_an.png`,
`N1_*_an.png`).

| type | name | description | hp | hit routine | AI routine(s) | confidence |
| --- | --- | --- | --- | --- | --- | --- |
| `$01` | endoskeleton (foot soldier) | chrome T-800 walking in from the screen edges, foreground and background variants; several AI states (walk/aim/shoot/die) | 3-5 | `$0059AC` (head+body), `$005A48`, `$005BEC`, `$005AF8` | `$00B586`, `$00B5EC`, `$00C564`, `$00C7C8`, `$00C6B0`, `$00B52E`, `$00B6D0` | high |
| `$0F` | crate | white supply crate on the ground, shot open to release a pickup; kill state `$00C8BA` writes the opening tiles | 0 | `$00597C` | `$00C886` (`$00C8BA`, `$00C8F0`, `$00C91E` = opening) | high |
| `$13` | human resistance soldier | kneeling green-clad human; shooting one is the "human casualty" penalty (`HitHumanSoldier`) | 20 | `$005F04` | `$00C9A2` | high |
| `$14` | destructible wall section | invisible object sitting on the brick wall (Y~152, row 20); it draws no sprite, and its kill state `$00CA12` queues replacement background tiles, i.e. the wall is shot away piece by piece | 10 | `$005F56` (multi-box by +$2A) | `$00CA0A` (idle), `$00CA12` (kill/tiles) | medium |
| `$16` | HK tank body | the boss chassis/treads; not shootable until the other parts are gone | 1120 | `$006126` | `$00CB9E`, `$00CB34` | high |
| `$17` | HK tank hull | lower/upper hull part of the boss between body and turret | 900 | `$006174` | `$00CDFE` | medium |
| `$18` | HK tank turret | the rotating gun turret on top of the boss | 635 | `$0061CA` | `$00CE46` | high |
| `$19` | HK tank arm | the mechanical arm/cannon of the boss (750 hp each) | 750 | `$006208` | `$00CF9E` | high |
| `$1A` | tank weak point | small shootable target on the boss (hp 0, dies in one hit, score `$0072CE`); likely the turret hatch/gun mount | 0 | `$006004` | `$00D028`, `$00D08C` | low |
| `$0D` | endoskeleton (variant) | rare level-1 enemy (frame 10500, `G1_5`); endoskeleton that climbs/enters | 28 | `$005E1C` | `$00B896` | low |
| `$11` | endoskeleton (variant) | rare level-1 enemy (frame 13500, `G1_7`) | ? | `$005ABC` | `$00C6B0` | low |
| `$00` | effect/projectile? | one-off object (frame 7500, `G1_4`, hp 11, depth 8); an explosion or the tank shell | 11 | `$005AF8` | `$00DA48` | low |

Notes: the background endoskeletons (depth ~140) use the same `$01` type; the "wall" is a
background tile layer, so `$14` is only visible through its effect on the tiles. The `$16`-`$19`
parts were all alive together in `G1_13`/`G1_14` at the boss.

## Summary table

Filled in level by level below.
