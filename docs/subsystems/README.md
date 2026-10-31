# Engine subsystems (P3, GitLab #7)

How the game works inside, one page per subsystem. Names: `data/symbols.txt` and `data/ram.txt`
(sections "# --- engine (issue #7)"); input and the Menacer: `docs/gun.md`.

| page | covers |
| --- | --- |
| [mainloop.md](mainloop.md) | interrupts, boot, attract/title/menu flow, `GameStart`, the per-frame order, frame pacing and lag, pause, level start/end |
| [objects.md](objects.md) | the six object pools, the 44-byte object slot, spawn/despawn, AI and hit-test dispatch, damage and kills, scoring, humans, pickups |
| [sprite.md](sprite.md) | sprite queue, mappings, SAT build, priorities |
| [vdp.md](vdp.md) | VRAM layout, the VDP command list, cell-row parallax scrolling, background map streaming, palettes and fades |
| [levels.md](levels.md) | level script format and interpreter, opcodes, where the level data is (`tools/py/levelscript.py`) |

## Frame in one line

`GameLoop`: pause check → HUD → `VdpListReset` → `ScriptTick` → `ScrollRows` → `BgStream*` →
input/shooting → pickups, missiles, effects, **objects**, sparks, debris → `BuildSpriteTable` →
`VdpListEnd` → wait for VBlank → `VdpListRun`. VBlank itself only counts frames, reads the Menacer
buttons and ticks the sound driver.

## Candidates for C replacement (P4)

Ranked by value for the port against the risk of replacing them (how much 68k code calls them or is
called by them, and whether they reach through code pointers). "Leaf" = calls nothing that is not
also replaced.

| # | routine(s) | value | risk | notes |
| --- | --- | --- | --- | --- |
| 1 | `MenacerReadXY` `$01E442` (+ `MenacerInit`, `MenacerBuildHTable`) | high: the native build has no HV counter; return mouse X/Y and buttons directly, zero lag | low: leaf, writes `gun_x/gun_y/gun_btn` only | contract in `docs/gun.md`; replaces the 8/16-latch averaging |
| 2 | `ReadInput` `$0003DE`, `ReadInputAim` `$0004B6`, `ReadPad1/2`, `PadMoveCrosshair` | high: host input without port emulation | low: leaves, fixed RAM outputs (`$FF8000`/`$FF8050`) | keep the mode table and clamps bit-exact |
| 3 | `Random` `$00881C`, `BcdAdd` `$007A92`, `AddScore*` `$0072F8..$007376` | low, but ideal first difftest targets | very low | pure functions of RAM |
| 4 | `PalFadeStep` `$008970`, `PalCycle` `$00E9FA`, `CramUpload` | medium (renderer palette path) | low | RAM → CRAM only |
| 5 | `QueueSprite` `$013240`, `BuildSpriteTable` `$013272` | high: sprite output for a native renderer; the bubble sort is a hot spot | low: RAM in, RAM + one list command out | keep the 79-piece cap and the priority-bit rules (`sprite.md`) |
| 6 | `VdpListRun` `$011D32` (9 commands) and the immediate VDP helpers | high: this is the VDP boundary of the whole game | medium: every command must be exact; HUD writes VRAM directly outside it | natural seam between "game" and "renderer" |
| 7 | `HitBox` `$005940`, `ShotHitTest` `$005892`, `PlayerShoot`/`PlayerButtons` | medium | medium: dispatches through +$1C, the on-hit/kill pair in a0/a2 | needs a call-back bridge into 68k handlers until those are ported |
| 8 | `ScrollRows` `$004752`, `BgStreamLayers` `$00498A` | medium (widescreen/hi-fps later) | medium: per-level tables, list commands, `level_exit` side effect | |
| 9 | `ScriptTick`/`ScriptExec` `$009358` | medium | medium: the interpreter is trivial, the 62 opcode handlers and 46 native targets are 68k | port the interpreter with a 68k-call bridge, then handlers by opcode |
| 10 | `ObjRunAll` `$00B398` and the AI/hit states (about 480 seeded entry points: 300 AI states, 59 hit tests, 122 on-hit/kill) | high in the end | high: state machines through `jmp` and immediates, per-type code | one type at a time, behind a dispatch table keyed by the +$20 address |
| 11 | `GameLoop` `$00765E`, `FramePaceWait`, `NextLevel`, `LevelEnd`, pause | the frame driver for the native build | high: touches everything; the pacing (no catch-up, list run after the wait) must stay as is | last |
