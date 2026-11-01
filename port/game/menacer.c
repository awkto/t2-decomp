/* MenacerReadXY $01E442: Menacer buttons + HV-latch averaging -> gun_x / gun_y (docs/gun.md).
 *
 * Original (src/t2.s, $01E442..$01E502):
 *     move.b  (gun_btn).l,(gun_btn_prev).l   ; shift the button byte
 *     move.b  (gun_btn_raw).l,(gun_btn).l
 *     tst.b   (gun_latch_count).l            ; 0 = gun off screen: position is kept
 *     beq.w   .done
 *     move.l  d7,-(sp)
 *     movea.l (gun_ring_ptr).l,a0            ; next ring write slot ($FFA07E..$FFA09E)
 *     lea.l   (gun_htable).l,a1              ; H -> X/2 table, 256 bytes
 *     ... 8 iterations: read the two bytes of each 16-bit latch backwards
 *         (low byte -> H table -> d0; high byte -> raw V -> d1)  = newest 8 latches
 *     ... 8 more iterations into d3/d4                            = the 8 before them
 *     ; X: |sum_new - sum_old| >= $28 (moving) -> X = sum_new/4
 *     ;                          otherwise (still) -> X = (sum_new+sum_old)/8
 *     ; Y: same with $14, /8 or /16, then + gun_y_offset
 *     move.w  d0,(gun_x).l
 *     move.w  d1,(gun_y).l
 *     move.l  (sp)+,d7
 * .done:
 *     rts
 *
 * The 68k registers d0-d4 have only their low words cleared (clr.w) and written, so their
 * entry high words survive; d2 ends as the (negated) Y difference. a0/a1 are clobbered,
 * d5/d6/d7 and a2-a7 are not. The final CCR comes from `move.l (sp)+,d7` (N/Z from the
 * restored d7, V=C=0) with X left by the last `add.w` (the Y offset add). On the gun-off
 * early exit the CCR is from `tst.b gun_latch_count` (N/Z, V=C=0) and X is untouched.
 *
 * The exact-equality note: the disassembled branch is `bmi.s` BEFORE the `neg.w`, so the
 * negated difference is compared UNSIGNED against $28/$14. That makes the "still" branch
 * (the 16-sample average) fire only when the two 8-sample sums are exactly equal; any
 * difference takes the "moving" branch. This is reproduced literally below.
 *
 * aim=fast (docs/gun.md, "Latency"): t2port's default patches the averaging body at
 * $01E460 in memory to convert only the newest latch. When the HLE layer is told aim=fast
 * (hle_aim_fast(), set by emu_set_aim_fast before the CPU is hooked) this function serves
 * that fast behaviour instead, so --hle all and the in-memory patch always agree. difftest
 * --aim game compares the original against the unpatched 68k; --aim fast compares the fast
 * path against the patched 68k. No helper is called: the routine is a leaf (no bsr/jsr).
 *
 * RAM (data/ram.txt): gun_x $FF9F6E, gun_y $FF9F70, gun_btn $FF9F72, gun_btn_prev $FF9F73,
 * gun_latch_count $FF9F74, gun_btn_raw $FF9F77, gun_y_offset $FF9F78, gun_htable $FF9F7E
 * (256 B), gun_ring $FFA07E (16 words), gun_ring_ptr $FFA09E (long). */
#include "hle.h"

#include <stdio.h>
#include <stdlib.h>

#define GUN_X          0xFF9F6E
#define GUN_Y          0xFF9F70
#define GUN_BTN        0xFF9F72
#define GUN_BTN_PREV   0xFF9F73
#define GUN_LATCH_COUNT 0xFF9F74
#define GUN_BTN_RAW    0xFF9F77
#define GUN_Y_OFFSET   0xFF9F78
#define GUN_HTABLE     0xFF9F7E
#define GUN_RING_PTR   0xFFA09E     /* long: next write slot; == one past the last word */
/* The walk keeps the register's full 32-bit value ($FFFFA0xx), so the wrap test compares
 * against $FFFFA07E exactly as `cmpa.l #$FFFFA07E,a0` does. */
#define RING_FIRST     0xFFFFA07E   /* first ring word ($FFA07E, sign/segment extended) */
#define RING_END       0xFFFFA09E   /* value the backwards walk wraps to (one past the last) */

/* How many calls took the 8-sample (moving) vs 16-sample (still) branch. Gated behind
 * T2_MENACER_STATS so it never affects a normal run; used to show both branches are hit. */
static unsigned long g_x8, g_x16, g_y8, g_y16;
static int g_stats = -1;

static void menacer_stats_print(void)
{
	if (g_stats == 1)
		fprintf(stderr, "MenacerReadXY paths: X moving(8)=%lu still(16)=%lu; Y moving(8)=%lu still(16)=%lu\n",
		        g_x8, g_x16, g_y8, g_y16);
}

static int menacer_stats_on(void)
{
	if (g_stats < 0) {
		g_stats = getenv("T2_MENACER_STATS") != NULL;
		if (g_stats)
			atexit(menacer_stats_print);
	}
	return g_stats == 1;
}

/* The two 68k loops: 16 backwards byte-pairs from a0 (wrapping at the ring start). For the
 * first 8 words accumulate table[low byte] into *sx and the high byte into *sy; for the
 * next 8 into *ox / *oy. a0 is the 32-bit register value ($FFFFA0xx), so the wrap compares
 * the full long exactly as `cmpa.l #$FFFFA07E` does. */
static uint32_t read_ring(uint32_t a0, uint32_t *sx, uint32_t *sy, uint32_t *ox, uint32_t *oy)
{
	uint32_t d0 = 0, d1 = 0, d3 = 0, d4 = 0;
	for (int i = 0; i < 8; i++) {
		if (a0 == RING_FIRST)
			a0 = RING_END;
		d0 = (d0 + RAM8(GUN_HTABLE + RAM8(--a0))) & 0xFFFF;
		d1 = (d1 + RAM8(--a0)) & 0xFFFF;
	}
	for (int i = 0; i < 8; i++) {
		if (a0 == RING_FIRST)
			a0 = RING_END;
		d3 = (d3 + RAM8(GUN_HTABLE + RAM8(--a0))) & 0xFFFF;
		d4 = (d4 + RAM8(--a0)) & 0xFFFF;
	}
	*sx = d0;
	*sy = d1;
	*ox = d3;
	*oy = d4;
	return a0;
}

HLE_FUNC(0x01E442, MenacerReadXY, 1588)   /* measured original cost (gun on screen, 164 insns) */
{
	/* common prologue: button byte shift and the gun-off early exit */
	RAM8(GUN_BTN_PREV) = RAM8(GUN_BTN);
	RAM8(GUN_BTN) = RAM8(GUN_BTN_RAW);
	uint8_t latch = RAM8(GUN_LATCH_COUNT);
	if (!latch) {
		/* tst.b gun_latch_count: N/Z from the byte, V=C=0, X untouched */
		cc_set(ctx, CC_N | CC_Z | CC_V | CC_C,
		       ((latch & 0x80) ? CC_N : 0) | (latch ? 0 : CC_Z));
		return;
	}

	uint32_t a0 = RAM32(GUN_RING_PTR);
	uint32_t a1 = 0xFFFF0000u | GUN_HTABLE;   /* lea.l (gun_htable).l,a1 */

	if (hle_aim_fast()) {
		/* the in-memory patch at $01E460: newest latch only, X = 2*table[H].
		 * Charge the patch's own cost (18 insns / ~250 cycles), not the original's: the
		 * fast path is short enough not to be interrupted, and charging 1588 would cross
		 * the HLE 1000-cycle slicing threshold (hle.c) and take a TH latch interrupt in
		 * the middle, corrupting the ring and the next frames' latches. */
		ctx->cycles = 250;
		if (a0 == RING_FIRST)
			a0 = RING_END;
		uint32_t d0 = RAM8(GUN_HTABLE + RAM8(--a0));   /* moveq #0,d0; move.b -(a0),d0; table */
		d0 = (d0 + d0) & 0xFFFF;                       /* add.w d0,d0 */
		RAM16(GUN_X) = d0;
		uint32_t d1 = RAM8(--a0);                      /* moveq #0,d1; move.b -(a0),d1 */
		uint32_t sum = d1 + RAM16(GUN_Y_OFFSET);
		uint32_t xf = (sum > 0xFFFF) ? CC_X : 0;       /* add.w (gun_y_offset).l,d1 */
		d1 = sum & 0xFFFF;
		RAM16(GUN_Y) = d1;
		/* moveq overwrites the whole register: d0/d1 high words are 0, d2-d7 keep theirs */
		ctx->d[0] = d0;
		ctx->d[1] = d1;
		ctx->a[0] = a0;
		ctx->a[1] = a1;
		/* final CCR from move.w d1,(gun_y).l: N/Z from gun_y, V=C=0, X from the add */
		cc_set(ctx, CC_X | CC_N | CC_Z | CC_V | CC_C,
		       xf | ((d1 & 0x8000) ? CC_N : 0) | (d1 ? 0 : CC_Z));
		return;
	}

	/* original: 8 newest latches into d0/d1, the 8 before into d3/d4 */
	uint32_t d0, d1, d3, d4;
	a0 = read_ring(a0, &d0, &d1, &d3, &d4);

	/* X: negated unsigned compare against $28 picks the 16- or 8-sample average */
	uint32_t tx = (d0 - d3) & 0xFFFF;
	if (!(tx & 0x8000))
		tx = (0u - tx) & 0xFFFF;               /* neg.w d2 (skipped by bmi when negative) */
	bool still_x = tx < 0x28;                  /* bcs: unsigned below $28 */
	if (still_x) {
		d0 = (d0 + d3) & 0xFFFF;
		d0 = (d0 >> 3) & 0xFFFF;
	} else {
		d0 = d0 >> 2;
	}
	RAM16(GUN_X) = d0;

	/* Y: same, threshold $14, /16 or /8, then + gun_y_offset */
	uint32_t ty = (d1 - d4) & 0xFFFF;
	if (!(ty & 0x8000))
		ty = (0u - ty) & 0xFFFF;
	bool still_y = ty < 0x14;
	if (still_y) {
		d1 = (d1 + d4) & 0xFFFF;
		d1 = (d1 >> 4) & 0xFFFF;
	} else {
		d1 = d1 >> 3;
	}
	uint32_t sum = d1 + RAM16(GUN_Y_OFFSET);
	uint32_t xf = (sum > 0xFFFF) ? CC_X : 0;   /* add.w (gun_y_offset).l,d1 sets X */
	d1 = sum & 0xFFFF;
	RAM16(GUN_Y) = d1;

	if (menacer_stats_on()) {
		if (still_x) g_x16++; else g_x8++;
		if (still_y) g_y16++; else g_y8++;
	}

	/* clr.w + partial writes: keep each register's entry high word */
	set_w(&ctx->d[0], d0);                     /* gun_x */
	set_w(&ctx->d[1], d1);                     /* gun_y */
	set_w(&ctx->d[2], ty);                     /* the Y difference (move.w d1,d2; sub; neg) */
	set_w(&ctx->d[3], d3);
	set_w(&ctx->d[4], d4);
	ctx->a[0] = a0;
	ctx->a[1] = a1;
	/* final CCR from move.l (sp)+,d7: N/Z from the restored (entry) d7, V=C=0, X from the add */
	uint32_t d7 = ctx->d[7];
	cc_set(ctx, CC_X | CC_N | CC_Z | CC_V | CC_C,
	       xf | ((d7 & 0x80000000u) ? CC_N : 0) | (d7 ? 0 : CC_Z));
}
