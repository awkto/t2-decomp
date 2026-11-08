/* Sprite queue + SAT builder: QueueSprite $013240, ObjQueueSprite $013228,
 * BuildSpriteTable $013272 (docs/subsystems/sprite.md; RAM in data/ram.txt).
 *
 * QueueSprite appends one entry (frame, X, Y, priority word) to spr_queue_args $FFA318
 * (8 bytes each) and a byte pair (index, priority) to spr_queue $FFA250, bumping
 * spr_queue_n $FF9DF2 and the two cursors spr_queue_data $FF9DFA / spr_queue_ptr $FF9DF6.
 *
 * ObjQueueSprite saves d5, takes X/Y from the object slot (a6+2/a6+4) and priority from
 * the depth byte (a6+7), then falls into QueueSprite. QueueSprite is in this file, so the
 * C ObjQueueSprite calls the C QueueSprite directly (proven equivalent, so the nested
 * call's registers/flags/RAM/IO are exactly the jsr's).
 *
 * BuildSpriteTable bubble-sorts the (index, priority) pairs by the priority byte, ascending,
 * NOT swapping equal priorities (the sort is stable), then expands each queued sprite's frame
 * id through SpriteMappings $0152A8 into SAT entries in spr_table $FFA638. Every piece is
 * 4 SAT words: Y+128+dy, (size<<8)|link, tile+bankword, X+128+dx. The link byte is the
 * running counter d6 (the index of the NEXT entry); the last entry's link is cleared to 0.
 * The priority bit (bit 15 of the tile word) is set while the queue priority <=
 * sprite_prio_depth $FF9EB8, then cleared again when the piece's Y >= $141. At most 79
 * pieces are expanded (d6 1..79); the rest of the queue is dropped. The pieces are then
 * queued as list command 1 (count = d6*4 words, src spr_table, VRAM $F400) into vdp_list at
 * vdp_list_ptr $FF80BC; an empty queue makes one dummy entry (Y=0 and link=0 only, the other
 * bytes are stale RAM).
 *
 * Registers: the originals leave a0-a5 (cursor/pointer registers) modified, and the high
 * words of d2/d3/d5 come from the entry state, so this mirrors the assembly register for
 * register. Final flags: the last flag-setting instruction is `move.l a5,(vdp_list_ptr).l`
 * (N/Z from a5, V=C=0); X is the carry shifted out by the `asl.w #2,d6` before it.
 *
 * Cycles. Every translated instruction is charged on the instruction timeline (hle_insn, hle.h) with
 * its 68000 cost (the m68kops table; movem.w, shifts and the taken/not-taken branches included), so
 * the empty, single-sprite, sorted, busy and 79-piece-capped paths each get their real cost and the
 * bus-refresh phase stays exact. `port/tools/seqcmp.py` diffs the timeline against the original.
 * The HLE_FUNC cycle count is only the interrupt window (docs/porting.md).
 */
#include "hle.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* spr_queue is addressed as 0xFFFFxxxx (the game stores full 32-bit work-RAM pointers). */
#define SPR_QUEUE        0xFFFFA250u
#define SPR_QUEUE_ARGS   0xFFFFA318u
#define SPR_TABLE        0xFFFFA638u
#define SPR_QUEUE_N      0xFF9DF2u
#define SPR_QUEUE_PTR    0xFF9DF6u
#define SPR_QUEUE_DATA   0xFF9DFAu
#define SPRITE_PRIO_DEPTH 0xFF9EB8u
#define VDP_LIST_PTR     0xFF80BCu
#define SPRITE_MAPPINGS  0x000152A8u

/* ---------------------------------------------------------------- path stats (T2_SPRITE_STATS) */
static unsigned long g_bst_empty, g_bst_single, g_bst_multi, g_bst_cap;
static unsigned long g_bst_passes1, g_bst_passes_sum;
static unsigned long g_bst_max_pieces, g_bst_max_sprites;
static unsigned long g_bst_prio_eq, g_bst_prio_lt, g_bst_prio_gt;
static int g_stats = -1;

static void sprite_stats_print(void)
{
	if (g_stats != 1)
		return;
	fprintf(stderr,
	        "BuildSpriteTable paths: empty=%lu single=%lu multi=%lu (79-piece cap hit %lu)\n"
	        "  sort passes over the multi states: 1 pass=%lu, average=%.2f\n"
	        "  busiest: %lu pieces, %lu queued sprites\n"
	        "  piece priority vs sprite_prio_depth: eq=%lu lt=%lu gt=%lu\n",
	        g_bst_empty, g_bst_single, g_bst_multi, g_bst_cap,
	        g_bst_passes1, g_bst_multi ? (double)g_bst_passes_sum / (double)g_bst_multi : 0.0,
	        g_bst_max_pieces, g_bst_max_sprites,
	        g_bst_prio_eq, g_bst_prio_lt, g_bst_prio_gt);
}

static int sprite_stats_on(void)
{
	if (g_stats < 0) {
		g_stats = getenv("T2_SPRITE_STATS") != NULL;
		if (g_stats)
			atexit(sprite_stats_print);
	}
	return g_stats == 1;
}

/* ------------------------------------------------------------------ QueueSprite $013240 */
/* Every instruction is charged after its effect, so a frame-loop event (the VBlank handler resets the
 * queue) that falls between two of them sees the same state as on the 68k (the call yields there). */
HLE_FUNC_PURE(0x013240, QueueSprite, 190)
{
	uint32_t a5 = RAM32(SPR_QUEUE_DATA);          /* movea.l (spr_queue_data).l,a5 */
	ctx->a[5] = a5;
	hle_insn(ctx, 20);
	uint32_t a4 = RAM32(SPR_QUEUE_PTR);           /* movea.l (spr_queue_ptr).l,a4 */
	ctx->a[4] = a4;
	hle_insn(ctx, 20);
	RAM16(a5) = ctx->d[1] & 0xFFFF;               /* move.w d1,(a5)+ */
	ctx->a[5] = a5 += 2;
	hle_insn(ctx, 8);
	RAM16(a5) = ctx->d[2] & 0xFFFF;               /* move.w d2,(a5)+ */
	ctx->a[5] = a5 += 2;
	hle_insn(ctx, 8);
	RAM16(a5) = ctx->d[3] & 0xFFFF;               /* move.w d3,(a5)+ */
	ctx->a[5] = a5 += 2;
	hle_insn(ctx, 8);
	RAM16(a5) = ctx->d[5] & 0xFFFF;               /* move.w d5,(a5)+ */
	ctx->a[5] = a5 += 2;
	hle_insn(ctx, 8);
	uint8_t n = RAM8(SPR_QUEUE_N);                /* move.b (spr_queue_n).l,d0 */
	set_b(&ctx->d[0], n);
	cc_logic8(ctx, n);
	hle_insn(ctx, 16);
	uint8_t nr = (uint8_t)(n + 1);                /* addq.b #1,(spr_queue_n).l */
	RAM8(SPR_QUEUE_N) = nr;
	unsigned carry = n == 0xFF;                   /* X=C=carry, N/Z/V from the result */
	unsigned ov = n == 0x7F;
	cc_set(ctx, CC_X | CC_N | CC_Z | CC_V | CC_C,
	       (carry ? (CC_X | CC_C) : 0) | ((nr & 0x80) ? CC_N : 0) | (nr ? 0 : CC_Z) |
	       (ov ? CC_V : 0));
	hle_insn(ctx, 20);
	RAM8(a4) = n;                                 /* move.b d0,(a4)+ */
	ctx->a[4] = a4 += 1;
	hle_insn(ctx, 8);
	RAM8(a4) = (uint8_t)ctx->d[5];                /* move.b d5,(a4)+ */
	ctx->a[4] = a4 += 1;
	hle_insn(ctx, 8);
	RAM32_W(SPR_QUEUE_DATA, ctx->a[5]);           /* move.l a5,(spr_queue_data).l */
	hle_insn(ctx, 20);
	RAM32_W(SPR_QUEUE_PTR, ctx->a[4]);            /* move.l a4,(spr_queue_ptr).l */
	/* final flags from the last move.l: N/Z from a4, V=C=0, X kept from addq.b */
	cc_set(ctx, CC_N | CC_Z | CC_V | CC_C,
	       ((ctx->a[4] & 0x80000000u) ? CC_N : 0) | (ctx->a[4] ? 0 : CC_Z));
	hle_insn(ctx, 20);
	hle_insn(ctx, 16);                            /* rts */
}

/* -------------------------------------------------------------- ObjQueueSprite $013228 */
HLE_FUNC_PURE(0x013228, ObjQueueSprite, 290)
{
	uint32_t a6 = ctx->a[6];
	push32(ctx, ctx->d[5]);                       /* move.l d5,-(sp) */
	hle_insn(ctx, 12);
	set_w(&ctx->d[2], RAM16(a6 + 2));             /* move.w (0x2,a6),d2 */
	hle_insn(ctx, 12);
	set_w(&ctx->d[3], RAM16(a6 + 4));             /* move.w (0x4,a6),d3 */
	hle_insn(ctx, 12);
	ctx->d[5] = 0;                                /* moveq #0,d5 */
	hle_insn(ctx, 4);
	set_b(&ctx->d[5], RAM8(a6 + 7));              /* move.b (0x7,a6),d5 */
	hle_insn(ctx, 12);
	hle_insn(ctx, 18);                            /* jsr (QueueSprite,pc) */
	QueueSprite(ctx);                             /* C version, ends with its rts */
	ctx->d[5] = pop32(ctx);                       /* move.l (sp)+,d5 */
	cc_logic32(ctx, ctx->d[5]);                   /* N/Z from d5, V=C=0, X kept */
	hle_insn(ctx, 12);
	hle_insn(ctx, 16);                            /* rts */
}

/* -------------------------------------------------------------- BuildSpriteTable $013272 */
/* 68000 cost (GPGX table, cycles): lea.l (abs).l 12, lea (d16,An) 8, lea (d8,An,Xn) 12,
 * lea (d16,PC) 8, move.l (abs).l,Dn 20, move.l Dn,(abs).l 20, move.l An,(abs).l 20,
 * movea.l (abs).l,An 20, movea.l Dn,An 4, movea.l An,An 4, sub.l An,Dn 8, subq.l #n,Dn 8,
 * subq.w #n,Dn 4, addq.w #n,Dn 4, moveq 4, move.w Dn,Dn 4, move.w (An)+,Dn 8,
 * move.w (An),Dn 8, move.w (An,Dn.w),Dn 14, move.w Dn,(An)+ 8, move.w Dn,(An) 8,
 * move.w Dn,(d16,An) 12, move.b (An)+,Dn 8, move.b Dn,(d16,An) 12, move.w (abs).l,Dn 16,
 * move.w #i,(An)+ 12, move.l #i,(An)+ 20, cmp.b Dn,Dn 4, cmp.w (abs).l,Dn 16, cmpi.w #i,Dn 8,
 * cmpi.w #i,(d16,An) 16, add.w Dn,Dn 4, add.w (An),Dn 8, addi.w #i,Dn 8, andi.l #i,Dn 16,
 * tst.b Dn 4, st.b Dn 6, bset.b #n,(d16,An) 20, bclr.b #n,(d16,An) 20, clr.b (d16,An) 16,
 * clr.w (An) 12, movem.w (An)+,regs 12+4*regs, asl/asr.w #n 6+2n, Bcc.s taken 10/not 8,
 * Bcc.w taken 10/not 12, bra 10, dbra 10/expire 14, rts 16. */
/* Early bail (docs/porting.md): the function runs in C up to the last bail point that is certain to be
 * reached before the first interrupt-raising slice end or the frame end, then the original 68k
 * continues from that instruction. The bail points are loop heads whose next instruction sets the flags
 * it uses (the flags and X before them are dead: the final X comes from the asl.w #2,d6), with every
 * register in ctx (the sort's d1/d2 are reloaded by its first two instructions). The chunk = the
 * longest stretch up to the next bail point (or the rts), rounded up. The bail points are also entries
 * (BuildSpriteTableSort ... BuildSpriteTableTail): when the 68k comes back to one after the event, the
 * C takes the rest over again ("resume"). Nothing charged yet = nothing changed: decline instead of a bail
 * (a bail to the very address the hook is at would loop). */
#define BST_BAIL(chunk, pc) do { if (hle_bail_due(ctx, (chunk))) { \
	if (!ctx->tl_used) hle_decline_why(2); else hle_bail_to(ctx, (pc)); return; } } while (0)

static void bst_run(hle_ctx *ctx, uint32_t entry)
{
	int passes = 0;

	switch (entry) {
	case 0x013296: goto loc_013296;
	case 0x0132C4: goto loc_0132C4;
	case 0x0132E8: goto loc_0132E8;
	case 0x013340: goto loc_013340;
	default: break;
	}
	BST_BAIL(220, 0x013272);

	ctx->a[0] = SPR_QUEUE;                        /* lea (0xffffa250).l,a0 */
	hle_insn(ctx, 12);
	ctx->d[6] = RAM32(SPR_QUEUE_PTR) - ctx->a[0]; /* move.l (spr_queue_ptr).l,d6 */
	hle_insn(ctx, 20);
	hle_insn(ctx, 8);                                     /* sub.l a0,d6 */
	set_w(&ctx->d[6], (uint16_t)((int16_t)(ctx->d[6] & 0xFFFF) >> 1));   /* asr.w #1,d6 */
	hle_insn(ctx, 6 + 2);
	set_w(&ctx->d[5], ctx->d[6] & 0xFFFF);        /* move.w d6,d5 */
	hle_insn(ctx, 4);
	if (sprite_stats_on() && (ctx->d[6] & 0xFFFF) > g_bst_max_sprites)
		g_bst_max_sprites = ctx->d[6] & 0xFFFF;
	set_w(&ctx->d[5], (ctx->d[5] - 1) & 0xFFFF);  /* subq.w #1,d5 */
	hle_insn(ctx, 4);
	if ((ctx->d[5] & 0xFFFF) == 0) {              /* beq.w loc_0132B2 (count == 1) */
		hle_insn(ctx, 10);
		if (sprite_stats_on()) g_bst_single++;
		goto loc_0132B2;
	}
	hle_insn(ctx, 12);
	if ((int16_t)(ctx->d[5] & 0xFFFF) < 0) {      /* bmi.w loc_013364 (count == 0) */
		hle_insn(ctx, 10);
		if (sprite_stats_on()) g_bst_empty++;
		goto loc_013364;
	}
	hle_insn(ctx, 12);
	set_w(&ctx->d[5], (ctx->d[5] - 1) & 0xFFFF);  /* subq.w #1,d5 (count - 2) */
	hle_insn(ctx, 4);
	if (sprite_stats_on()) g_bst_multi++;

loc_013290:                                       /* one bubble-sort pass */
	passes++;
	ctx->a[1] = ctx->a[0];                        /* movea.l a0,a1 */
	hle_insn(ctx, 4);
	set_b(&ctx->d[3], 0);                         /* moveq #0,d3 */
	hle_insn(ctx, 4);
	set_w(&ctx->d[7], ctx->d[5] & 0xFFFF);        /* move.w d5,d7 */
	hle_insn(ctx, 4);
loc_013296:
	BST_BAIL(170, 0x013296);
	{
		uint16_t d1 = RAM16(ctx->a[1]);           /* move.w (a1)+,d1 */
		ctx->a[1] += 2;
		hle_insn(ctx, 8);
		uint16_t d2 = RAM16(ctx->a[1]);           /* move.w (a1),d2 */
		hle_insn(ctx, 8);
		hle_insn(ctx, 4);                              /* cmp.b d2,d1 */
		if ((uint8_t)d1 == (uint8_t)d2) {
			hle_insn(ctx, 10);                            /* beq.s loc_0132A8 taken */
		} else {
			hle_insn(ctx, 8);                             /* beq.s not taken */
			if ((uint8_t)d1 < (uint8_t)d2) {
				hle_insn(ctx, 10);                        /* bcs.s loc_0132A8 taken */
			} else {
				hle_insn(ctx, 8);                         /* bcs.s not taken */
				RAM16(ctx->a[1]) = d1;            /* move.w d1,(a1) */
				hle_insn(ctx, 8);
				RAM16(ctx->a[1] - 2) = d2;        /* move.w d2,(-0x2,a1) */
				hle_insn(ctx, 12);
				set_b(&ctx->d[3], 0xFF);          /* st.b d3 */
				hle_insn(ctx, 6);
			}
		}
	}
	set_w(&ctx->d[7], (ctx->d[7] - 1) & 0xFFFF);  /* dbra d7 */
	if ((ctx->d[7] & 0xFFFF) != 0xFFFF) {
		hle_insn(ctx, 10);                                /* dbra continues */
		goto loc_013296;
	}
	hle_insn(ctx, 14);                                    /* dbra expires */
	if ((ctx->d[3] & 0xFF) != 0) {                /* tst.b d3 / bne.s loc_013290 */
		HLE_SEQ(ctx, 4, 10);
		goto loc_013290;
	}
	HLE_SEQ(ctx, 4, 8);                                 /* tst.b + bne.s not taken */
	set_w(&ctx->d[5], (ctx->d[5] + 1) & 0xFFFF);  /* addq.w #1,d5 */
	hle_insn(ctx, 4);

loc_0132B2:
	set_w(&ctx->d[7], ctx->d[5] & 0xFFFF);        /* move.w d5,d7 */
	hle_insn(ctx, 4);
	ctx->d[6] = 1;                                /* moveq #0x1,d6 (next link) */
	hle_insn(ctx, 4);
	ctx->d[4] = 0;                                /* moveq #0x0,d4 */
	hle_insn(ctx, 4);
	ctx->a[1] = SPR_QUEUE_ARGS;                   /* lea (0xffffa318).l,a1 */
	hle_insn(ctx, 12);
	ctx->a[2] = SPR_TABLE;                        /* lea (0xffffa638).l,a2 */
	hle_insn(ctx, 12);

loc_0132C4:                                       /* one queued sprite */
	BST_BAIL(210, 0x0132C4);
	ctx->d[0] = 0;                                /* moveq #0,d0 */
	hle_insn(ctx, 4);
	ctx->d[0] = RAM8(ctx->a[0]);                  /* move.b (a0)+,d0 */
	ctx->a[0] += 1;
	hle_insn(ctx, 8);
	set_b(&ctx->d[4], RAM8(ctx->a[0]));           /* move.b (a0)+,d4 */
	ctx->a[0] += 1;
	hle_insn(ctx, 8);
	set_w(&ctx->d[0], ((ctx->d[0] & 0xFFFF) << 3) & 0xFFFF);   /* asl.w #0x3,d0 */
	hle_insn(ctx, 6 + 2 * 3);
	ctx->a[3] = ctx->a[1] + (int16_t)(ctx->d[0] & 0xFFFF);     /* lea (0,a1,d0.w),a3 */
	hle_insn(ctx, 12);
	/* movem.w (a3)+,d1-d3/d5: GPGX sign-extends the loaded word into the whole register
	 * (m68k_op_movem_16_er_pi: REG_DA[i] = MAKE_INT_16(...)), unlike MOVE.W */
	ctx->d[1] = (uint32_t)(int32_t)(int16_t)RAM16(ctx->a[3]);
	ctx->a[3] += 2;
	ctx->d[2] = (uint32_t)(int32_t)(int16_t)RAM16(ctx->a[3]);
	ctx->a[3] += 2;
	ctx->d[3] = (uint32_t)(int32_t)(int16_t)RAM16(ctx->a[3]);
	ctx->a[3] += 2;
	ctx->d[5] = (uint32_t)(int32_t)(int16_t)RAM16(ctx->a[3]);
	ctx->a[3] += 2;
	hle_movem_w(ctx, 12 + 4 * 4, 4);                      /* movem.w: 12 + 4 regs * 4 */
	set_w(&ctx->d[1], ((ctx->d[1] & 0xFFFF) << 1) & 0xFFFF);   /* asl.w #0x1,d1 */
	hle_insn(ctx, 6 + 2);
	ctx->a[4] = SPRITE_MAPPINGS;                  /* lea (SpriteMappings,pc),a4 */
	hle_insn(ctx, 8);
	set_w(&ctx->d[1], ROM16(ctx->a[4] + (int16_t)(ctx->d[1] & 0xFFFF)));  /* move.w (0,a4,d1.w),d1 */
	hle_insn(ctx, 14);
	ctx->d[1] &= 0xFFFF;                          /* andi.l #0xffff,d1 */
	hle_insn(ctx, 16);
	ctx->a[4] += ctx->d[1];                       /* lea (0,a4,d1.l),a4 */
	hle_insn(ctx, 12);

loc_0132E8:                                       /* one piece */
	BST_BAIL(350, 0x0132E8);
	set_w(&ctx->d[0], ROM16(ctx->a[4]));          /* move.w (a4)+,d0 (size) */
	ctx->a[4] += 2;
	hle_insn(ctx, 8);
	if ((int16_t)(ctx->d[0] & 0xFFFF) < 0) {      /* bmi.w loc_01333C */
		hle_insn(ctx, 10);
		goto loc_01333C;
	}
	hle_insn(ctx, 12);
	set_w(&ctx->d[1], ROM16(ctx->a[4]));          /* move.w (a4)+,d1 (y offset) */
	ctx->a[4] += 2;
	hle_insn(ctx, 8);
	set_w(&ctx->d[1], (ctx->d[1] + 0x80) & 0xFFFF);            /* addi.w #0x80,d1 */
	hle_insn(ctx, 8);
	set_w(&ctx->d[1], (ctx->d[1] + ctx->d[3]) & 0xFFFF);       /* add.w d3,d1 */
	hle_insn(ctx, 4);
	RAM16(ctx->a[2]) = ctx->d[1] & 0xFFFF;        /* move.w d1,(a2)+ (Y) */
	ctx->a[2] += 2;
	hle_insn(ctx, 8);
	RAM16(ctx->a[2]) = ctx->d[0] & 0xFFFF;        /* move.w d0,(a2)+ (size) */
	ctx->a[2] += 2;
	hle_insn(ctx, 8);
	RAM8(ctx->a[2] - 1) = (uint8_t)ctx->d[6];     /* move.b d6,(-0x1,a2) (link) */
	hle_insn(ctx, 12);
	set_w(&ctx->d[0], ROM16(ctx->a[4]));          /* move.w (a4)+,d0 (tile) */
	ctx->a[4] += 2;
	hle_insn(ctx, 8);
	ctx->d[1] = 0xFFFFFFFF;                       /* moveq #-0x1,d1 */
	hle_insn(ctx, 4);
	set_w(&ctx->d[1], ROM16(ctx->a[4]));          /* move.w (a4)+,d1 (bank RAM word) */
	ctx->a[4] += 2;
	hle_insn(ctx, 8);
	ctx->a[3] = ctx->d[1];                        /* movea.l d1,a3 ($ffffxxxx) */
	hle_insn(ctx, 4);
	set_w(&ctx->d[0], (ctx->d[0] + RAM16(ctx->a[3])) & 0xFFFF); /* add.w (a3),d0 */
	hle_insn(ctx, 8);
	RAM16(ctx->a[2]) = ctx->d[0] & 0xFFFF;        /* move.w d0,(a2)+ (tile) */
	ctx->a[2] += 2;
	hle_insn(ctx, 8);
	set_w(&ctx->d[0], ROM16(ctx->a[4]));          /* move.w (a4)+,d0 (x offset) */
	ctx->a[4] += 2;
	hle_insn(ctx, 8);
	set_w(&ctx->d[0], (ctx->d[0] + 0x80) & 0xFFFF);            /* addi.w #0x80,d0 */
	hle_insn(ctx, 8);
	set_w(&ctx->d[0], (ctx->d[0] + ctx->d[2]) & 0xFFFF);       /* add.w d2,d0 */
	hle_insn(ctx, 4);
	RAM16(ctx->a[2]) = ctx->d[0] & 0xFFFF;        /* move.w d0,(a2)+ (X) */
	ctx->a[2] += 2;
	hle_insn(ctx, 8);
	hle_insn(ctx, 16);                                    /* cmp.w (sprite_prio_depth).l,d4 */
	{
		int16_t prio = (int16_t)(ctx->d[4] & 0xFFFF);
		int16_t depth = (int16_t)RAM16(SPRITE_PRIO_DEPTH);
		if (sprite_stats_on()) {
			if (prio == depth)
				g_bst_prio_eq++;
			else if (prio < depth)
				g_bst_prio_lt++;
			else
				g_bst_prio_gt++;
		}
		if (prio <= depth) {
			hle_insn(ctx, 8);                           /* bgt.s not taken */
			RAM8(ctx->a[2] - 4) |= 0x80;                /* bset.b #7,(-0x4,a2) */
			hle_insn(ctx, 20);
		} else {
			hle_insn(ctx, 10);                            /* bgt.s loc_013322 taken */
		}
	}
	hle_insn(ctx, 16);                                    /* cmpi.w #0x141,(-0x8,a2) */
	if ((int16_t)RAM16(ctx->a[2] - 8) >= 0x141) {
		hle_insn(ctx, 8);                               /* blt.s not taken */
		RAM8(ctx->a[2] - 4) &= (uint8_t)~0x80;          /* bclr.b #7,(-0x4,a2) */
		hle_insn(ctx, 20);
	} else {
		hle_insn(ctx, 10);                                /* blt.s loc_013330 taken */
	}
	set_w(&ctx->d[6], (ctx->d[6] + 1) & 0xFFFF);  /* addq.w #0x1,d6 */
	hle_insn(ctx, 4);
	hle_insn(ctx, 8);                                     /* cmpi.w #0x4f,d6 */
	if ((int16_t)(ctx->d[6] & 0xFFFF) > 0x4F) {   /* bgt.s loc_013340 taken: 79-piece cap */
		hle_insn(ctx, 10);
		if (sprite_stats_on()) g_bst_cap++;
		goto loc_013340;
	}
	HLE_SEQ(ctx, 8, 10);                                /* bgt.s not taken + bra.w loc_0132E8 */
	goto loc_0132E8;

loc_01333C:
	set_w(&ctx->d[7], (ctx->d[7] - 1) & 0xFFFF);  /* dbra d7 */
	if ((ctx->d[7] & 0xFFFF) != 0xFFFF) {
		hle_insn(ctx, 10);
		goto loc_0132C4;
	}
	hle_insn(ctx, 14);

loc_013340:                                       /* queue the SAT copy */
	BST_BAIL(210, 0x013340);
	if (sprite_stats_on() && (ctx->d[6] & 0xFFFF) > g_bst_max_pieces)
		g_bst_max_pieces = ctx->d[6] & 0xFFFF;
	if (sprite_stats_on() && passes) {
		g_bst_passes_sum += (unsigned long)passes;
		if (passes == 1)
			g_bst_passes1++;
	}
	RAM8(ctx->a[2] - 5) = 0;                      /* clr.b (-0x5,a2): last link = 0 */
	hle_insn(ctx, 16);
	ctx->a[5] = RAM32(VDP_LIST_PTR);              /* movea.l (vdp_list_ptr).l,a5 */
	hle_insn(ctx, 20);
	RAM16(ctx->a[5]) = 1;                         /* move.w #0x1,(a5)+ */
	ctx->a[5] += 2;
	hle_insn(ctx, 12);
	{
		uint32_t v = ctx->d[6] & 0xFFFF;          /* asl.w #0x2,d6: X=C=bit shifted out */
		unsigned xc = (v >> 14) & 1;
		set_w(&ctx->d[6], (v << 2) & 0xFFFF);
		cc_set(ctx, CC_X, xc ? CC_X : 0);
		hle_insn(ctx, 6 + 2 * 2);
	}
	RAM16(ctx->a[5]) = ctx->d[6] & 0xFFFF;        /* move.w d6,(a5)+ (word count) */
	ctx->a[5] += 2;
	hle_insn(ctx, 8);
	RAM32_W(ctx->a[5], SPR_TABLE);                /* move.l #0xffffa638,(a5)+ */
	ctx->a[5] += 4;
	hle_insn(ctx, 20);
	RAM16(ctx->a[5]) = 0xF400;                    /* move.w #0xf400,(a5)+ */
	ctx->a[5] += 2;
	hle_insn(ctx, 12);
	RAM32_W(VDP_LIST_PTR, ctx->a[5]);             /* move.l a5,(vdp_list_ptr).l */
	hle_insn(ctx, 20);
	hle_insn(ctx, 16);                                    /* rts (the layer performs it) */
	/* final flags from the last move.l: N/Z from a5, V=C=0, X kept from asl */
	cc_set(ctx, CC_N | CC_Z | CC_V | CC_C,
	       ((ctx->a[5] & 0x80000000u) ? CC_N : 0) | (ctx->a[5] ? 0 : CC_Z));
	return;

loc_013364:                                       /* empty queue: one dummy entry */
	ctx->a[2] = SPR_TABLE;                        /* lea (0xffffa638).l,a2 */
	hle_insn(ctx, 12);
	RAM16(ctx->a[2]) = 0;                         /* clr.w (a2) */
	hle_insn(ctx, 12);
	ctx->a[2] += 8;                               /* lea (0x8,a2),a2 */
	hle_insn(ctx, 8);
	ctx->d[6] = 1;                                /* moveq #0x1,d6 */
	hle_insn(ctx, 4);
	hle_insn(ctx, 10);                                    /* bra.w loc_013340 */
	goto loc_013340;
}

HLE_FUNC_BAIL_YIELD(0x013272, BuildSpriteTable, 60000) { bst_run(ctx, 0x013272); }
HLE_FUNC_BAIL_YIELD(0x013296, BuildSpriteTableSort, 60000) { bst_run(ctx, 0x013296); }
HLE_FUNC_BAIL_YIELD(0x0132C4, BuildSpriteTableSprite, 60000) { bst_run(ctx, 0x0132C4); }
HLE_FUNC_BAIL_YIELD(0x0132E8, BuildSpriteTablePiece, 60000) { bst_run(ctx, 0x0132E8); }
HLE_FUNC_BAIL_YIELD(0x013340, BuildSpriteTableTail, 60000) { bst_run(ctx, 0x013340); }
