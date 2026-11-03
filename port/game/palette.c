/* Palette path: PalFadeStep ($008970), Copy32 ($008A7C), PalCycle ($00E9FA) and
 * PalFade ($008A4C, as a range around its waits). docs/subsystems/vdp.md, "Palettes".
 *
 * PalFadeStep (src/t2.s $008970):
 *     tst.b   (pal_fading).l              ; 0 = nothing to do
 *     beq.w   $0089FE
 *     lea.l   (pal_target).l,a0          ; 64 words the fade moves to
 *     lea.l   (pal_cur).l,a1             ; 64 words currently in CRAM
 *     lea.l   (pal_changed).l,a2
 *     moveq   #$3f,d0
 *     moveq   #0,d1                      ; changed count
 * $008990:
 *     move.w  (a0)+,d2 / move.w (a1)+,d3
 *     sf.b    (a2)+                      ; pal_changed[i] = 0
 *     cmp.w   d2,d3 / beq $0089F4         ; equal: nothing
 *     st.b    (-1,a2)                    ; else flag the entry
 *     ; for each of the three 3-bit channels (mask $E, $E0, $E00, step 2, $20, $200):
 *     ;   d4 = target & mask, d5 = current & mask, d6 = +step
 *     ;   d5 > d4 -> keep +step (move down), d5 < d4 -> d6 = -step (move up)
 *     ;   d5 -= d6; d7 = (d7 | d5)
 *     move.w  d7,(-2,a1)                 ; write the stepped colour back to pal_cur
 *     addq.w  #1,d1
 * $0089F4:
 *     dbra    d0,$008990
 *     tst.w   d1 / beq $0089FE           ; nothing changed anywhere
 *     rts
 * $0089FE:
 *     sf.b    (pal_fading).l             ; fade finished
 *     rts
 *
 * Every channel step moves by 2 in the 3-bit field (one CRAM level), independently for
 * B ($E), G ($E0) and R ($E00); a colour takes at most 7 steps. d2-d5/d7 are written with
 * `move.w` (high words survive), d6 with `moveq`/`move.w` (high word 0), d1 with
 * `moveq #0,d1` (whole register), d0 ends $FFFF from the dbra. Final CCR: `tst.w d1` when
 * something changed (N from d1, Z=0, V=C=0, X = the loop's last addq carry = 0), or the
 * final `sf.b pal_fading` when nothing changed (N=0, Z=1, V=C=0, X from the entry).
 * a0/a1/a2 end one past the three arrays; a3-a7 are untouched.
 *
 * Copy32 (src/t2.s $008A7C): eight `move.l (a0)+,(a1)+` = 32 bytes. `move.l` sets N/Z,
 * V=C=0 (X unchanged), so the final CCR is the 8th move's; no d regs are touched.
 *
 * PalCycle (src/t2.s $00E9FA), called by GameLoop while pal_cycle_on is set:
 *     move.w  (vblank_count).l,d0
 *     andi.w  #$18,d0                    ; 4 steps, one per 8 frames
 *     lea.l   (dat_0084FA).l,a0          ; 4 x 8-byte palette steps
 *     lea.l   (0,a0,d0.w),a0
 *     lea.l   (pal_cur+$18).l,a1         ; colours 12..15 of line 0
 *     move.l  (a0)+,(a1)+ / move.l (a0)+,(a1)+
 *     lea.l   (pal_cur).l,a0
 *     jsr     (CramUpload).l             ; 64 CRAM words from (a0)
 *     rts
 * It rotates the 4 colours from the table at $0084FA + (vblank_count & $18) into
 * pal_cur colours 12..15 and uploads the whole palette. CramUpload is not ported: it
 * writes the VDP ports directly (not through the VDP command list, vdp.md). The C range
 * stops right before the `jsr CramUpload` so that call stays 68k (see the HLE_RANGE note
 * at PalCycle below).
 *
 * PalFade (src/t2.s $008A4C) is a blocking loop: WaitVBlank, PalFadeStep, WaitVBlank,
 * a short delay, CramUpload of pal_cur, until pal_fading is clear (at most 7 steps). A C
 * function cannot wait for an interrupt (docs/porting.md, and scooby-decomp's "Waiting for
 * VBlank"), so the two WaitVBlank calls and the PalFadeStep call stay 68k. The non-blocking
 * body after the second wait ($008A5E..$008A7A) is registered as an HLE_RANGE with two
 * exits: back to the first WaitVBlank ($008A4C) or the rts ($008A7A). The `jsr PalFadeStep`
 * at $008A52 reaches the C PalFadeStep through the normal dispatch, and CramUpload again
 * runs through hle_call68k. This is scooby's rule "stop in front of the blocking call".
 *
 * RAM (data/ram.txt): pal_cycle_on $FFA0A4, pal_changed $FFA0F4 (64 B), pal_target $FFA134
 * (128 B), pal_cur $FFA1B4 (128 B), pal_fading $FFA246; vblank_count $FFA0EE. */
#include "hle.h"

#include <stdio.h>
#include <stdlib.h>

#define VBLANK_COUNT   0xFFFFA0EE
#define PAL_CHANGED    0xFFFFA0F4
#define PAL_TARGET     0xFFFFA134
#define PAL_CUR        0xFFFFA1B4
#define PAL_FADING     0xFFFFA246
#define PAL_CYCLE_TAB  0x000084FA   /* ROM: 4 x 8-byte palette steps */
#define PAL_CUR_12     0xFFFFA1CC   /* pal_cur + $18: colours 12..15 of line 0 */
#define CRAM_UPLOAD    0x000080FE

/* How many calls took the early exit (pal_fading == 0), changed nothing, or moved at
 * least one colour; and the Copy32/PalCycle call counts. T2_PALETTE_STATS=1 prints them,
 * used to show the captured states cover every path. */
static unsigned long g_fs_off, g_fs_none, g_fs_move, g_fs_min, g_fs_max;
static unsigned long g_copy32, g_cycle, g_pf_loop, g_pf_done;
static int g_stats = -1;

static void palette_stats_print(void)
{
	if (g_stats != 1)
		return;
	fprintf(stderr,
	        "PalFadeStep: early(pal_fading=0)=%lu none-changed=%lu moved=%lu (changed entries min=%lu max=%lu)\n"
	        "PalFade: loop-back=$008A4C %lu  rts=$008A7A %lu\n"
	        "Copy32 calls=%lu  PalCycle calls=%lu\n",
	        g_fs_off, g_fs_none, g_fs_move, g_fs_min, g_fs_max,
	        g_pf_loop, g_pf_done, g_copy32, g_cycle);
}

static int palette_stats_on(void)
{
	if (g_stats < 0) {
		g_stats = getenv("T2_PALETTE_STATS") != NULL;
		if (g_stats)
			atexit(palette_stats_print);
	}
	return g_stats == 1;
}

/* ------------------------------------------------------------------ PalFadeStep $008970 */
/* One 3-bit channel: target/current already masked, `step` is the base (+2/+$20/+$200).
 * Returns the stepped current value and leaves the effective step in *step_out (the 68k
 * leaves d6 at the last channel's value, +step or -step). */
static uint32_t pal_channel(hle_ctx *ctx, uint32_t target, uint32_t current, uint32_t step, uint32_t *step_out)
{
	uint32_t d6 = step;
	if (current != target) {
		hle_insn(ctx, 8);              /* beq.s not taken */
		if (current < target) {        /* cmp.w d4,d5 / bgt keeps +step, else neg.w d6 */
			HLE_SEQ(ctx, 8, 4);    /* bgt.s not taken / neg.w d6 */
			d6 = (uint32_t)(-(int32_t)step) & 0xFFFF;
		} else {
			hle_insn(ctx, 10);     /* bgt.s taken */
		}
		hle_insn(ctx, 4);              /* sub.w d6,d5 */
		current = (current - d6) & 0xFFFF;
	} else {
		hle_insn(ctx, 10);             /* beq.s taken */
	}
	*step_out = d6;
	return current;
}

HLE_FUNC_PURE(0x008970, PalFadeStep, 16000)
{
	uint8_t fading = RAM8(PAL_FADING);
	if (fading == 0) {                  /* tst.b pal_fading / beq loc_0089FE */
		if (palette_stats_on())
			g_fs_off++;
		RAM8(PAL_FADING) = 0;       /* sf.b (already 0): N=0 Z=1 V=C=0, X unchanged */
		cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
		HLE_SEQ(ctx, 16, 10, 20, 16);   /* tst.b (abs).l / beq.w taken / sf.b (abs).l / rts */
		return;
	}

	/* tst.b / beq.w not taken / lea.l x3 / moveq x2 */
	HLE_SEQ(ctx, 16, 12, 12, 12, 12, 4, 4);
	uint32_t a0 = PAL_TARGET, a1 = PAL_CUR, a2 = PAL_CHANGED;
	uint32_t d1 = 0;                    /* moveq #0,d1 */
	/* d2-d7 keep their entry values until a `move.w`/`moveq` writes them (an entry whose
	 * colours are all equal never touches d4-d7) */
	uint32_t d2 = ctx->d[2], d3 = ctx->d[3], d4 = ctx->d[4],
	         d5 = ctx->d[5], d6 = ctx->d[6], d7 = ctx->d[7];

	for (int i = 0; i < 64; i++) {
		d2 = RAM16(a0); a0 += 2;    /* move.w (a0)+,d2 */
		d3 = RAM16(a1); a1 += 2;    /* move.w (a1)+,d3 */
		RAM8(a2) = 0; a2 += 1;      /* sf.b (a2)+ */
		HLE_SEQ(ctx, 8, 8, 12, 4);  /* move.w (a0)+,d2 / move.w (a1)+,d3 / sf.b (a2)+ / cmp.w d2,d3 */
		if (d3 == d2) {             /* cmp.w d2,d3 / beq loc_0089F4 */
			hle_insn(ctx, 10);                     /* beq.s taken */
			hle_insn(ctx, i == 63 ? 14 : 10);      /* dbra */
			continue;
		}
		hle_insn(ctx, 8);           /* beq.s not taken */
		RAM8(a2 - 1) = 0xFF;        /* st.b (-1,a2) */
		hle_insn(ctx, 16);

		/* channel 1: mask $E, step 2 (moveq #2,d6) */
		uint32_t t = d2, c = d3;
		d4 = t & 0x000E; d5 = c & 0x000E;
		HLE_SEQ(ctx, 4, 4, 4, 8, 8, 4);   /* move.w d2,d4 / move.w d3,d5 / moveq #2,d6 / andi.w x2 / cmp.w d4,d5 */
		d5 = pal_channel(ctx, d4, d5, 0x0002, &d6);
		d7 = d5;                    /* move.w d5,d7 */
		/* channel 2: mask $E0, step $20 (moveq #$20,d6) */
		d4 = t & 0x00E0; d5 = c & 0x00E0;
		HLE_SEQ(ctx, 4, 4, 4, 4, 8, 8, 4);   /* move.w d5,d7 / move.w / move.w / moveq #$20,d6 / andi.w x2 / cmp.w */
		d5 = pal_channel(ctx, d4, d5, 0x0020, &d6);
		d7 |= d5;                   /* or.w d5,d7 */
		/* channel 3: mask $E00, step $200 (move.w #$200,d6) */
		d4 = t & 0x0E00; d5 = c & 0x0E00;
		HLE_SEQ(ctx, 4, 4, 4, 8, 8, 8, 4);   /* or.w d5,d7 / move.w / move.w / move.w #$200,d6 / andi.w x2 / cmp.w */
		d5 = pal_channel(ctx, d4, d5, 0x0200, &d6);
		d7 |= d5;
		/* or.w d5,d7 4, move.w d7,(-2,a1) 12, addq.w #1,d1 4, dbra */
		HLE_SEQ(ctx, 4, 12, 4);
		hle_insn(ctx, i == 63 ? 14 : 10);       /* dbra */

		RAM16(a1 - 2) = d7 & 0xFFFF;    /* move.w d7,(-2,a1) */
		d1 = (d1 + 1) & 0xFFFF;         /* addq.w #1,d1 */
	}

	ctx->a[0] = a0; ctx->a[1] = a1; ctx->a[2] = a2;
	set_w(&ctx->d[2], d2);              /* move.w writes: high words survive */
	set_w(&ctx->d[3], d3);
	set_w(&ctx->d[4], d4);
	set_w(&ctx->d[5], d5);
	set_w(&ctx->d[7], d7);
	ctx->d[6] = d6;                     /* moveq/move.w: high word 0 */
	ctx->d[1] = d1;                     /* moveq #0,d1 cleared the high word */
	ctx->d[0] = 0x0000FFFF;             /* dbra ran d0 down to -1 */

	if (palette_stats_on()) {
		if (d1 == 0) {
			g_fs_none++;
		} else {
			g_fs_move++;
			if (!g_fs_min || d1 < g_fs_min) g_fs_min = d1;
			if (d1 > g_fs_max) g_fs_max = d1;
		}
	}

	hle_insn(ctx, 4);                       /* tst.w d1 */
	if (d1 != 0) {
		HLE_SEQ(ctx, 8, 16);            /* beq.s not taken / rts */
		/* tst.w d1: N from d1 (<= 64, so 0), Z=0, V=C=0; X = the loop's last addq = 0 */
		cc_set(ctx, CC_X | CC_N | CC_Z | CC_V | CC_C, 0);
	} else {
		HLE_SEQ(ctx, 10, 20, 16);       /* beq.s taken / sf.b (abs).l / rts */
		RAM8(PAL_FADING) = 0;       /* beq loc_0089FE / sf.b */
		cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	}
}

/* ------------------------------------------------------------------ Copy32 $008A7C */
/* `move.l` sets N/Z from the moved long, V=C=0, X unchanged; the final CCR is the 8th
 * move's. */
HLE_FUNC_PURE(0x008A7C, Copy32, 200)
{
	if (palette_stats_on())
		g_copy32++;
	uint32_t last = 0;
	HLE_SEQ(ctx, 20, 20, 20, 20, 20, 20, 20, 20, 16);   /* move.l (a0)+,(a1)+ x8 / rts */
	for (int i = 0; i < 8; i++) {       /* move.l (a0)+,(a1)+ x8 */
		uint32_t v = bus_r32(ctx->a[0]);
		bus_w32(ctx->a[1], v);
		ctx->a[0] += 4;
		ctx->a[1] += 4;
		last = v;
	}
	cc_logic32(ctx, last);
}

/* ------------------------------------------------------------------ PalCycle $00E9FA */
/* Registered as a range that stops right before the `jsr CramUpload` at $00EA1E, so that
 * call runs as real 68k at the correct raster time. CramUpload polls the VDP status (DMA
 * busy) and its last read is returned in d1; from C the nested call would start ~126 cycles
 * early (the prelude is instant in C) and the VDP HBLANK status bit would differ in d1.
 * Keeping the call in 68k (scooby's "stop in front of the blocking/timed call") makes the
 * range, and the caller's d1, exactly equal. The `rts` at $00EA24 is trivial 68k. */
HLE_RANGE_PURE(0x00E9FA, PalCycle, 160, 0x00EA1E)
{
	if (palette_stats_on())
		g_cycle++;
	/* move.w (abs).l,d0 16, andi.w 8, lea.l x3 12, move.l (a0)+,(a1)+ x2 20, lea.l (pal_cur).l,a0 12 */
	HLE_SEQ(ctx, 16, 8, 12, 12, 12, 20, 20, 12);
	uint16_t d0 = RAM16(VBLANK_COUNT) & 0x18;    /* move.w / andi.w #$18 */
	set_w(&ctx->d[0], d0);
	uint32_t a0 = PAL_CYCLE_TAB + (int16_t)d0;   /* lea.l (0,a0,d0.w),a0 */
	uint32_t a1 = PAL_CUR_12;
	uint32_t last = 0;
	for (int i = 0; i < 2; i++) {                /* move.l (a0)+,(a1)+ x2 */
		uint32_t v = ROM32(a0);
		RAM32_W(a1, v);
		a0 += 4;
		a1 += 4;
		last = v;
	}
	cc_logic32(ctx, last);                       /* the 2nd move.l's N/Z, V=C=0 */
	ctx->a[0] = PAL_CUR;                         /* lea.l (pal_cur).l,a0 */
	ctx->a[1] = a1;
	hle_jump(ctx, 0x00EA1E);                     /* the jsr CramUpload runs as 68k */
}

/* ------------------------------------------------------------------ PalFade $008A4C */
/* Run the original 68k from `start` up to `stop` (a timing-sensitive point: the delay and
 * then CramUpload must happen at the real raster time), then leave ctx at `stop` for C. */
static void pal_run_to(hle_ctx *ctx, uint32_t start, uint32_t stop)
{
	uint32_t exits[2] = { stop, 0 };
	ctx->pc = start;
	hle_run68k_until(ctx, exits);
	ctx->jumped = 0;
	ctx->pc = stop;
}

/* The body after the two WaitVBlank calls: the moveq #$64 + 101-iteration nop/dbra delay
 * (only d0 is observable and CramUpload overwrites it, but its ~1422 cycles are real timing:
 * CramUpload polls the VDP status and must run after them, so the delay is run as 68k with
 * hle_run68k_until), then lea.l (abs).l,a0 (12), jsr (CramUpload).l (20, its own time added
 * by the layer), tst.b (abs).l (16) and the bne.s (10 back to $008A4C / 8 not taken, falling
 * to the rts at $008A7A): 58 or 56 non-nested cycles. */
HLE_RANGE(0x008A5E, PalFade, 14000, 0x008A4C, 0x008A7A)
{
	pal_run_to(ctx, 0x008A5E, 0x008A66); /* moveq #$64,d0 + the nop/dbra delay, as 68k */
	hle_exact_session();                 /* the session's time (refresh, VDP FIFO stalls) is real: keep it as is */
	ctx->a[0] = PAL_CUR;                 /* lea.l (pal_cur).l,a0 */
	HLE_SEQ(ctx, 12, 20);                /* lea.l (abs).l,a0 / jsr (CramUpload).l: before the call, so it starts on time */
	hle_sync(ctx);
	hle_call68k(ctx, CRAM_UPLOAD);       /* jsr (CramUpload).l */
	uint8_t f = RAM8(PAL_FADING);
	hle_insn(ctx, 16);                   /* tst.b (abs).l */
	/* tst.b pal_fading: N/Z from the byte, V=C=0, X unchanged */
	cc_set(ctx, CC_N | CC_Z | CC_V | CC_C,
	       ((f & 0x80) ? CC_N : 0) | (f ? 0 : CC_Z));
	if (f) {
		hle_insn(ctx, 10);                 /* bne.s taken */
		if (palette_stats_on())
			g_pf_loop++;
		hle_jump(ctx, 0x008A4C);     /* bne.w PalFade: back to the first WaitVBlank */
	} else {
		hle_insn(ctx, 8);                  /* bne.s not taken */
		if (palette_stats_on())
			g_pf_done++;
		hle_jump(ctx, 0x008A7A);     /* fall through to rts */
	}
}
