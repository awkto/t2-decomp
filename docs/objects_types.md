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

## Level 2 - human hideout interior

Evidence: `gun_02` state; frames 1410/1740 (`N2_4`, `N2_5`) and 3000-15000 (`G2_*`); screenshots
`N2_4_an.png`, `N2_5_an.png`, `G2_2_an.png`, `G2_4_an.png`, `G2_10_an.png`.

| type | name | description | hp | hit routine | AI routine(s) | confidence |
| --- | --- | --- | --- | --- | --- | --- |
| `$26` | endoskeleton (infiltrator) | dark-chrome T-800 that breaks into the hideout; three spawn variants (walking in at the edges, dropping from the ceiling); the big one in `G2_4` | 10 | `$006894`, `$0068CA`, `$00683A` | `$00EFA4`, `$00EFFE`, `$00F18A`, `$00F1E4`, `$00EDD4`, `$00F69C` | high |
| `$13` | human resistance fighter | unarmed/armed humans of the hideout; shooting one is the casualty penalty | 5 | `$0065FC`, `$006652` | `$00EAA8`, `$015212` | high |
| `$20` | hanging ceiling lamp | lamp fixture hanging from the ceiling (top of screen, Y~25, scrolls in from the right); shootable | 4 | `$006544`, `$0066A8`, `$00597A` | `$00EA6A`, `$00EAEC`, `$0151F0` | high |
| `$29` | endoskeleton (floor pop-up) | rises out of the floor (Y starts at 256, rises 8 px/frame), fires, then sinks; three hit-point stages drop parts | 20 | `$0067F2` | `$00EC58` (rise), `$00EC82` (fire), `$00ED2E` (dying) | medium |
| `$0F` | crate / ceiling fixture | wooden crate on the floor (and a variant at the ceiling, `spawn_t0fc`); generic destructible prop | 2-4 | `$006512`, `$006576`, `$0065A4`, `$0065D2` | `$00EA26`, `$00EB2E`, `$00EB4C`, `$00EB62`, `$00EB96`, `$00EB9C` | high |

## Summary table

Filled in level by level below.
