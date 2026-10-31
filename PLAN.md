# T2: The Arcade Game (Genesis): mouse controls, decomp, port: PLAN

Status: planning done and feasibility proven (2026-10-31). Nothing committed or pushed yet.

## 1. What we have

| | |
|---|---|
| ROM | `Terminator 2 - The Arcade Game (JUE) [!].smd`: interleaved SMD with a 512-byte header |
| Raw ROM | `rom/baserom.md` (`tools/py/smd2bin.py` de-interleaves it), 1 MiB, sha1 `85cc1cf3379d3ce23ca3c03d84fe6e2b3adc9c56`, header checksum $D5D4 OK |
| Header | `GM T-081156 01`, "(C)T-81 1992.OCT", region JUE, I/O field `J` |
| Credits | Programmed by **Probe Software** for Acclaim / Arena, 1992, hand-written 68k |
| Code (first boot + first stage coverage) | $000200-$020000, plus a block at $0F8000-$0FD000. Probably 1.5-2x the size of Scooby's $200-$11000. |

## 2. Key finding: the game already supports a mouse-like device

The title menu offers **START 1 PLAYER - USE MENACER** (the Sega Menacer light gun). It is the default
option. Genesis Plus GX emulates the Menacer on port 2. Proven headless (`tools/harness/harness.c`,
`tools/harness/scripts/aimtest.txt`, screenshot `proto/menacer-aim-test.png`):

* The game draws its own crosshair, and it lands exactly on the commanded gun position
  ((40,60), (280,180), (80,160) all checked).
* The trigger fires. Hits register: soldiers died ("HUMAN CASUALTY") and the score went up.
* Gun Start skips the intro text.
* **Gotcha:** the GPGX libretro core reads the gun as `RETRO_DEVICE_POINTER` (pointer gun mode), not
  `RETRO_DEVICE_LIGHTGUN`. A host has to answer the pointer queries, or set the gun-mode core option.

**Consequence.** Basic mouse aim and fire needs no ROM patch and no decomp: mouse position → Menacer
position, left button → trigger. This works today in RetroArch with GPGX, Menacer on port 2 and the
mouse as the lightgun. Scooby needed path-finding and verb-bar automation to get there. The decomp is
for everything around that core.

## 3. Gaps to close for a good mouse experience (still to investigate)

1. **Missiles.** Which Menacer button (A/B/C) fires a missile is unclear. Map it to right click. Read
   the gun-read routine to find out.
2. **Menus with the mouse.** At the title menu we used pad 1. Options, continue, high-score initials
   and the 2-player menu need mouse clicks. Either the game accepts the gun there, or the host
   injects pad presses (as Scooby's `mouse.c` does).
3. **Accuracy over the whole screen.** GPGX scales Menacer X by 289/320, and the game applies its own
   offsets. Run a grid test (crosshair sprite position vs commanded position) and correct in the host
   if needed.
4. **Menacer mode vs pad mode differences.** Autofire, difficulty, crosshair speed, whether gunpower
   behaves the same. Hold-to-autofire on left click may need host help.
5. **Mouse + pad together.** Player 1 mouse (Menacer) and player 2 pad. The menu's
   "2 PLAYER: 1 MENACER / 2 JOYPAD 1" option is exactly this.
6. **Native (no-68k) build.** No VDP or HV-counter emulation, so the gun read becomes a C replacement
   that returns mouse coordinates directly. Simpler than the light-gun path.
7. **Feel.** Hide the OS cursor and use the game's crosshair. Measure latency (the latch is per line,
   then the game reads it in VBlank: about 1 frame).

## 4. Lessons from scooby-decomp (about 27 h wall clock, 36 issues, 176 commits)

* **Mouse came early.** Mouse control (#23) closed about 4.5 h after the start, on top of the
  emulated core through pad and RAM injection, long before the C port. Do the same here: it is even
  cheaper, because of the Menacer.
* **Reuse, don't rebuild.** These pieces carry over almost unchanged:
  * `tools/py/disasm.py` (recursive descent, coverage-guided), `indirect.py`, `symtab.py`, `xref.py`
  * the headless harness, now with gun support (`tools/harness/harness.c`)
  * the GPGX cpuhook and vdptrace patches, `romscan.py`, `tools/vm/*` (lease/vmrun/vmpull/release)
  * the port host: SDL2 with static GPGX, options ini, romfind by sha1, overlay, F-keys, savestates
  * the HLE layer, difftest, the web/wasm build and the Pages workflow, `package.sh` and `disttest`

  Copy them and change names and the ROM sha1. Extract a shared "gens-decomp-kit" only if a third
  game follows.
* **Do from day 1 what Scooby had to fix at the end.** Public identity awkto noreply, no personal
  paths, no ROM bytes (`.incbin` plus `romscan --tracked`) and an MIT licence from the first commit.
  Scooby needed a history rewrite and a scrub (#34).
* **Waves of worktree subagents.** The orchestrator reviews and merges. Use Sonnet for specified
  tasks (tool ports, docs, tests) and Opus for hard RE (frame loop, object system, gun path). In the
  Scooby bakeoff, both were about equally correct. An Opus agent once ignored the swap rule, so
  restate the pro machine rules in every brief.
* **The frame loop and timing were where the bugs were** (#30, #31, #33: dropped VBlanks, hangs).
  Give the main loop / VBlank decomp to Opus, with irq tests before it merges.
* **Closed-loop gates paid off.** Scooby's `storytest` ran full walkthroughs. T2's equivalent is a
  **gun-bot**: it reads enemy positions from RAM and aims and fires, so it clears levels headless. It
  drives coverage of later levels, and later it serves as the regression gate for C replacements.
  A level-select or debug cheat in the code would shortcut this; search for one early.

## 5. Phases

| Phase | What | Agent | Est. |
|---|---|---|---|
| **P0** | Feasibility: Menacer + mouse proven headless | done | 0.5 h |
| **P1** | **Playable with the mouse.** `t2port` = Scooby port host retargeted. Menacer on port 2 fed from the mouse, left = trigger, right = missile, middle = Start, pad injection for menus, ini/options, F-keys, savestates, tests (aim grid, menu flow, 2P). Web build reusing Scooby's wasm pipeline, mouse cursor hidden. | Sonnet (host), Opus (gaps 1-4 RE) | 3-5 h |
| **P2** | Byte-matching disassembly: copy and parametrize the Scooby tools, coverage campaign on a VM (attract, boot, gun-bot playthrough of every level, savestate per level), symbols, RAM map, call graph | Sonnet + Opus for the indirect jumps | 3-5 h |
| **P3** | Understand the engine: gun/input path, main loop / VBlank, object/enemy system, level scripts and scrolling, HUD, sound driver, asset formats | Opus (core), Sonnet (formats, sound) | 4-6 h |
| **P4** | C replacements through HLE + difftest. Gun read becomes C returning mouse coordinates. Main loop, objects, render. | mixed, in waves | 6-10 h |
| **P5** | Optional extras, as Scooby: native runner without 68k, native renderer, widescreen (harder in a scrolling shooter), high frame rate, packaging, Pages | per feature | 4-8 h |

P1 alone answers "desperately needs mouse controls". P2-P5 rebuild Scooby's full depth. Because the
tooling is reused, the whole thing should take about as long as Scooby did (about a day) despite the
larger code size.

## 6. Infrastructure

* Heavy work goes on DigitalOcean (`doctl --context teamadf`, tag `claude-test`, syd1, s-4vcpu-8gb,
  provisioned with Scooby's `tools/vm/cloud-init.yaml` + `provision`). **`vc2-re-1` already holds
  one of the 2 allowed droplets**, so this project gets 1 unless the cap is raised (Scooby ran 3 with
  approval).
* Pro is used only for light work: short headless runs under `nice` and reviews.
* Tracking: GitHub repo `awkto/t2-decomp` (private) and issues in `gitlab.dnsif.ca/github/t2-decomp`,
  labelled `wave::1..5`, epics per phase. Created once the scope is confirmed.

## 7. Open decisions

1. Scope: P1 only (mouse) and then decide, or go straight for full Scooby depth (P1-P5)?
2. Droplet cap for this project: 1 (current room) or more?
3. Repo name: `t2-decomp`?
