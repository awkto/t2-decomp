# Objects: enemies, humans, pickups, effects (GitLab #7)

Everything that moves on the playfield lives in one of six fixed RAM pools. The big one is the object
table at `$FF87B2`: enemies, humans, crates and scenery that can be shot. Names are in
`data/symbols.txt` / `data/ram.txt` ("# --- engine (issue #7)"). Hit testing from the gun side
(`PlayerShoot`, `shot_xy`, damage) is in `docs/gun.md`.

## Pools

| pool | RAM | slots x size | count | runner (per frame, in this order) | contents |
| --- | --- | --- | --- | --- | --- |
| pickups | `$FF9AB2` | ? x `$1A` | `pickup_count` `$FF80E0` | `PickupsRun` `$012BB0` | pickups dropped by enemies/crates; shot with `PickupShotTest` |
| missiles | `$FF80F2` | 32 x `$2A` | `missile_count` `$FF80D6` | `MissilesRun` `$0123F0` | player missiles and their explosions; state word +0 = 1..7 (bra table `$01240C`) |
| effects | `$FF8632` | 32 x `$C` | `effect_count` `$FF80D8` | `EffectsRun` `$01243C` | animations at a fixed spot that ride a scroll row (+8 frame list, 0 ends) |
| **objects** | `$FF87B2` | 80 x `$2C` | `obj_count` `$FF80DA` | `ObjRunAll` `$00B398` | enemies, humans, crates, scenery |
| sparks | `$FF9572` | 32 x `$12` | `spark_count` `$FF80DC` | `SparksRun` `$0126B4` | animations with an 8.8 velocity |
| debris | `$FF97B2` | 32 x `$18` | `debris_count` `$FF80DE` | `DebrisRun` `$012886` | animations with velocity and gravity |

All pools share one convention: a slot is free when its first word is 0, the count is the number of
live slots (not a high-water mark), and a runner walks slots until it has seen `count` live ones,
skipping free slots. `ClearObjectPools` `$008B3C` zeroes all of them at level start.

## Object slot (`obj_table` `$FF87B2`, `$2C` bytes)

| off | size | field |
| --- | --- | --- |
| +0 | w | flags (high byte) and **type** (low byte). b15 = shootable (hit tests skip the slot when clear; cleared on death). b14 = facing/moving right (selects the frame set, cleared when spawned at X ≥ 300). b13 = dying/finished (set by AI). b12 = variant (op `$02` with a negative word). 0 = free slot |
| +2 | w | X, screen pixels |
| +4 | w | Y, screen pixels, at the feet (aim ~20 px higher; `docs/gun.md`) |
| +6 | b | **scroll row** the object is attached to (signed). `ObjScrollX` subtracts `row_delta[+6 + 28]` from X every frame, so the object moves with that strip of background |
| +7 | b | **depth**: sprite priority passed to `QueueSprite`, and the hit order. Smaller = nearer (drawn on top, hit first) |
| +8 | w | hit points (signed). `ShotHitTest` subtracts `shot_damage`; < 0 = killed |
| +A | b | X speed: added to the +B fraction 8 times per frame; each carry moves X by +E (`ObjMoveX` `$00B4A6`) |
| +B | b | X fraction |
| +C..+F | b | per type (animation counters; +E = direction, +1 or -1) |
| +$10..$1B | | per type (some use +$14 as a hit/flash timer, +$16/+$1A as frame list and first frame) |
| +$1C | l | **hit-test routine** (`jsr` from `ShotHitTest`, `HitAllShootable`, `MissileFindTarget`) |
| +$20 | l | **AI routine** (`jmp` from `ObjRunAll`; it must end with `bra ObjNext` `$00B3BE`) |
| +$24 | w | state timer |
| +$26..$2B | | per type |

Spawning: `ObjAlloc` `$00930E` returns the first free slot in a6 (80 slots); the caller fills it and
increments `obj_count`. `ObjSpawnTemplate` `$0092DC` copies an 18-byte ROM template:

    type.w (b15 forced on)  X.w  Y.w  row.b  depth.b  AI.l  hit.l  hp.w

The level-script spawn opcodes (`levels.md`) are thin wrappers: allocate, fill type/X/Y/row/depth/
AI/hit/hp, maybe take X/Y from the script, bump a per-type counter (`$FF9E06..`).

Despawn: AI routines call `ObjKillIfLeftOf` `$012332` / `ObjKillIfRightOf` `$012342` (d0 = limit;
clears +0 and decrements `obj_count`) or clear the slot themselves.

## Per-object dispatch (the resolved indirect jumps)

`ObjRunAll`: `move.w (a6),d0; beq skip; movea.l $20(a6),a0; jmp (a0)`. An AI routine is a state:
it changes state by storing a new pointer in +$20 (some jump to the new state at once,
`$00FEDE`/`$0114BE`). A typical state (`$00B3C8`):

    ObjMoveX; ObjScrollX; pick the frame from +$24 and b14; ObjQueueSprite (body, then head)
    ObjKillIfRightOf(360); ObjKillIfLeftOf(-40); timer--; on 0: +$20 = next state; bra ObjNext

There is no jump table: the code pointers are immediates in the code (`move.l #state,$20(a6)`) and in
the spawn templates. They are all in `tools/py/seeds.txt` (generated from the ROM by scanning every
`move.l #imm,($20|$1C,aN)`, every template passed to `ObjSpawnTemplate`, and the HitBox users below),
and `data/indirect_notes.txt` marks the sites SEEDED in `data/indirect.txt`. With the seeds the
disassembly went from 896 to 1298 function chunks (the region `$0059A4-$00700C` was one data blob
before; it is the hit-test code with its boxes in between).

## Hit testing, damage, kills

A hit-test routine (+$1C) is usually three instructions:

    lea onhit(pc),a2 ; lea box(pc),a3 ; jmp HitBox        (or jsr HitBox twice for two boxes)

`HitBox` `$005940` tests `shot_xy` against the object: box = (x-off, y-off, w, h) words at a3,
relative to (X, Y). On a hit it sets d6 = -1 and appends (object, onhit) to `hit_list` `$FFA958`.
Multi-box objects use one box per part (`$0059AC`: a head box whose on-hit kills at once, and a body
box). Types decide extra rules in the hit routine itself; humans (`$005F04`) are only tested when
`shot_damage < 6`, so missiles do not hit them.

`ShotHitTest` `$005892` (called with `shot_xy` and `shot_damage` set by `PlayerShoot` or a missile):

1. calls every live slot's +$1C routine (shootable or not; the on-hit side checks b15),
2. bubble-sorts `hit_list` by depth (+7), smallest first,
3. for the front-most entry only: if b15 is set, `jsr onhit` → the on-hit routine plays the hit
   reaction and returns the **kill routine in a0** (`lea kill(pc),a0; rts`); then
   `hp -= shot_damage`; if hp < 0, `jsr (a0)` again = the kill routine,
4. returns d5 = -1 if anything was hit.

So one shot damages one object, the nearest. `HitAllShootable` `$0057BA` (smart bomb pickup,
`PickupSmartBomb` `$007138`) calls on-hit then kill for every hit-list entry, except types `$0F`,
`$13`, `$14`. `MissileFindTarget` `$0073A0` uses the same +$1C routines to find what a missile is
aimed at.

The kill routine clears b15, sets a death AI state in +$20, and does the bookkeeping on the
shooting player (a4): score (`AddScore250` `$0072F8` .. `AddScore10000` `$007368`, BCD into
`p1_score`/`p2_score` by player index, `AddScoreBcd` `$007376`) and per-player kill counters in the
player struct (+$32 endoskeletons, +$38 humans, +$3A, ...) that feed the end-of-level stats.

### Humans vs machines

Humans (type `$13`, the kneeling resistance soldiers on level 1) are objects like the rest, with hit
routine `$005F04`: the on-hit `$005F1A` plays a voice (`$013DA8`) and a flinch state (`$00C968`);
killing one (`$005F34`) bumps the player's humans counter (+$38) and calls
`SetPlayerState(6)` `$008FA8` (the "you shot a human" penalty state, 200-frame timer). Missiles and
smart bombs never hit them.

## Types seen on level 1

From RAM dumps of the `l1` state every 150 frames and the screenshots (`tools/harness`):

| type | hit | AI (example) | depth | what |
| --- | --- | --- | --- | --- |
| `$01` | `$0059AC`, `$005A48`, `$005A80`, `$005BEC` | `$00B586`, `$00B5EC`, `$00C564` | `$0E..$79` | endoskeletons (foot soldiers), several variants |
| `$0A` | `$006042` | `$00C3C6`, `$00C4E4` | `$0D`, `$15` | HK aerial and the bomb it drops (?) |
| `$0F` | `$00597C` | `$00C886` | `$0C` | crates along the bottom edge (hp 0; shooting one starts `$00C8BA`, probably releasing a pickup) |
| `$13` | `$005F04` | `$00C9A2` | `$10` | resistance humans (do not shoot) |
| `$14` | `$005F56` | `$00CA0A` | `$10` | small targets on the wall tops (two boxes, hp 10; kill counted in +$3A) (?) |

The other spawn opcodes give the remaining types (`$07`, `$09`, `$0C`, `$0D`, `$0E`, `$11`, `$15`,
`$17`, `$1A`, `$1B`, `$1D`, `$1E`, `$1F`, `$23`, `$28`, ...); `tools/py/levelscript.py` lists which
level spawns which. Naming them needs a per-level look like the one above.

## Pickups

`pickup_pool` `$FF9AB2`, `$1A` bytes per slot, +0 low byte = pickup type. They are shot, not
touched: `PlayerShoot` → `PickupShotTest` `$00700C` → `PickupHitTable` `$007048[type]` (types
1..13) → `PickupHitBox` `$007086` (box test, sound `$013E30`, then the type's effect, e.g. `PickupSmartBomb`). The
individual effects were not traced one by one. `PickupsRun` ages them with a +$14 countdown (flag
changes at 150 and 50, the blinking phase).

## Animation

There is no shared animation engine. Objects pick a frame id per state (often from a timer, e.g.
`(+$24 & $38) >> 3`, and b14 for the facing) and queue it with `ObjQueueSprite` `$013228` (frame =
d1, X, Y, priority = depth). The effect/spark/debris pools step a frame list (+8 pointer to words,
0 = end). Frame ids index `SpriteMappings` `$0152A8` (`sprite.md`).
