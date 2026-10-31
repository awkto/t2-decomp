# Sprites (GitLab #7)

How objects become VDP sprites. Names in `data/symbols.txt` / `data/ram.txt`.

## Queue

`SpriteQueueReset` `$013376` at the start of the frame's object phase, then any number of

    QueueSprite $013240   d1 = frame id, d2 = X, d3 = Y, d5 = priority (byte)

calls. Each call appends 4 words (frame, X, Y, priority) to `spr_queue_args` `$FFA318` and a byte
pair (queue index, priority) to `spr_queue` `$FFA250`; `spr_queue_n` counts. `ObjQueueSprite`
`$013228` is the object version (X/Y from the slot, priority = depth +7). The crosshairs are queued
first (`QueueCrosshair`, frame = player index + 1, `docs/gun.md`).

## Building the SAT (`BuildSpriteTable` `$013272`)

1. Bubble sort of the (index, priority) pairs by priority, ascending. Smaller priority = nearer =
   earlier in the link list = drawn on top. The same depth decides which object a shot hits
   (`objects.md`), so what you see on top is what you hit.
2. For each queued sprite, its frame id selects a piece list through `SpriteMappings` `$0152A8`
   (word offsets from the table). A piece is 5 words, the list ends with a negative word:

       size.w  (VDP size byte in the high byte)
       y.w     offset from the sprite's Y
       tile.w  pattern word (palette, flips, tile number relative to a bank)
       base.w  RAM address ($FFxxxx) of a word holding the bank's tile index in VRAM; added to tile
       x.w     offset from the sprite's X

   The base indirection lets the same mapping work wherever the level loader put the graphics
   (`load_tiles` script commands write those words, e.g. `$FF9E36`, `$FF9E3A`).
3. Each piece becomes one SAT entry in `spr_table` `$FFA638` (Y+128, size, link, tile, X+128).
   Priority bit: set when the queue priority ≤ `sprite_prio_depth` `$FF9EB8` (scripts set 14..16), so
   near objects pass in front of high-priority plane tiles; cleared again when the piece's Y ≥ 193
   (raw ≥ `$141`), so the bottom HUD stays on top.
4. At most 79 pieces; the rest of the queue is dropped (no flicker multiplexing). An empty queue
   gives one dummy entry.
5. The last entry's link = 0, then list command 1 copies `pieces × 4` words to VRAM `$F400`
   (`vdp.md`).

The harness `sprites` command dumps the result in link order.
