/* Hit testing: ShotHitTest ($005892), HitBox ($005940), PickupHitBox ($007086).
 * docs/subsystems/objects.md, "Hit testing, damage, kills"; RAM in data/ram.txt.
 *
 * HitBox (src/t2.s $005940) is the per-object box test. a3 points at a ROM box
 * (x-off, y-off, w, h) relative to the object's (X, Y) at a6+2/a6+4; the shot is
 * shot_xy $FF9E94. On a hit it sets d6 = -1 and appends the pair (a6 object, a2
 * on-hit routine) to hit_list $FFA958 (a5 is the running write pointer, so a5
 * ends +8). On a miss d6 = 0. It is a plain leaf: no calls.
 *
 * PickupHitBox ($007086) is the same box test for the pickup pool (a6 = pickup
 * slot). On a hit it calls sub_012AF4 (the pickup's "shot" reaction: sets +0 = 1
 * and gives it a velocity) and sub_013E30 (queues the hit sound with d0 = d6, the
 * pickup type the caller set), then returns d6 = -1; on a miss d6 = 0. Both callees
 * are not ported, so they run as 68k through hle_call68k.
 *
 * ShotHitTest ($005892) resolves one shot:
 *   1. walk the object table $FF87B2 (obj_count $FF80DA live slots, stride $2C,
 *      free slots skipped), calling every live slot's +$1C hit-test routine
 *      through hle_call68k; each one that hits appends to hit_list via a5;
 *   2. if hit_list is empty return d5 = 0;
 *   3. bubble-sort hit_list by the depth byte (+7), smallest (nearest) first,
 *      stable (equal depths are not swapped);
 *   4. take the front-most entry; if its b15 (shootable) is set call the on-hit
 *      routine (a0), then hp (+8) -= shot_damage $FFA0CA; if hp went negative call
 *      a0 again (the on-hit routine returns the kill routine in a0);
 *   5. return d5 = -1 (hit) or 0 (nothing hit).
 *
 * Calls (all through hle_call68k, none ported yet):
 *   - the per-object +$1C routines (jsr (a0)): they stay 68k. HitBox itself is
 *     ported, so when HitBox is enabled the HLE dispatch runs the C version inside
 *     the nested call - exactly as the original jsr would run the 68k one.
 *   - the on-hit and kill routines (jsr (a0) twice): 68k.
 *   - PickupHitBox's sub_012AF4 / sub_013E30: 68k.
 * No ported function is called directly here, so every nested call goes through
 * hle_call68k.
 *
 * Registers. Every instruction is mirrored, including GPGX's sign-extension of
 * `movem.w` into the whole register (HitBox/PickupHitBox d0-d3), the surviving high
 * words of `move.w` writes (ShotHitTest d5/d7, HitBox d2/d3), a3/a5 advancing over
 * the boxes/hit_list and the final CCR:
 *   - HitBox: the hit path's last flag setter is `move.l a2,(a5)+` (N/Z from a2,
 *     V=C=0, X kept); the miss path's is `moveq #0,d6` (N=0,Z=1,V=C=0).
 *   - PickupHitBox: same, but the hit path's last setter is `moveq #-1,d6` after
 *     the two nested calls (which clobber the flags first).
 *   - ShotHitTest: both returns end in `moveq` (d5 = -1 or 0).
 *
 * Cycles. Charged per instruction on the instruction timeline (hle_insn, hle.h) with the 68000
 * cost (m68kops.h table, plus the movem/shift/dbcc extras), like sprite.c. Before a nested
 * 68k call the jsr is charged and the timeline put on the clock (hle_sync), so the callee
 * starts at the original's time. The HLE_FUNC cycle count is only the window the layer uses to
 * decide whether an interrupt could land inside the call (hle.c, "Interrupt atomicity").
 */
#include "hle.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define OBJ_TABLE   0xFFFF87B2u
#define OBJ_COUNT   0xFF80DAu
#define HIT_LIST    0xFFFFA958u
#define SHOT_XY     0xFFFF9E94u
#define SHOT_DAMAGE 0xFFA0CAu
#define PICKUP_EXPLODE 0x00012AF4u
#define PICKUP_SOUND   0x00013E30u

/* GPGX 68000 costs (m68kops.h cycle table; movem/shift/dbcc extras included). */
#define C_LEA_AL     12
#define C_LEA_D16     8
#define C_MOVEW_AL   16
#define C_MOVEW_AN    8
#define C_MOVEW_DD    4
#define C_MOVE_AL_D  20
#define C_MOVE_A_D    4
#define C_MOVE_A_PI  12   /* move.l An,(An)+ (m68k_op_move_32_pi_a) */
#define C_MOVE_A_AL  20
#define C_MOVEA_D16  16
#define C_MOVEA_IX   18
#define C_MOVEA_PI   12
#define C_MOVEB_D16  12
#define C_CMPB_D16   12
#define C_MOVE_IX_D  18
#define C_MOVE_IX_IX 32
#define C_MOVE_D_IX  18
#define C_MOVE_A_IX  18
#define C_SUBW_D16   16
#define C_SUBW_PI_D   8
#define C_ADDW_PI_D   8
#define C_MOVEQ       4
#define C_JSR_AN     16
#define C_JSR_AL     20
#define C_SUBI_L     16
#define C_SUBQ_W      4
#define C_ASR_W3     12
#define C_ADDI_W      8
#define C_STB_D       6
#define C_TSTB_D      4
#define C_BTST       10
#define C_DBRA_CONT  10
#define C_DBRA_EXP   14
#define C_BCC_S_TAKEN 10
#define C_BCC_S_NOT   8
#define C_BCC_W_TAKEN 10
#define C_BCC_W_NOT  12
#define C_RTS        16

/* movem.w: table base (abs 20 / (d16,An) 16) + 4 per register. */
#define C_MOVEMW_AL_2  28
#define C_MOVEMW_DI_2  24

/* Path counters (T2_HITTEST_STATS=1), to show the captured states cover every path. */
static unsigned long g_sht_calls, g_sht_none, g_sht_hit, g_sht_multi, g_sht_kill;
static unsigned long g_sht_noshoot, g_sht_human, g_sht_freeslots;
static unsigned long g_sht_types[256];
static unsigned long g_hb_hit, g_hb_miss;
static unsigned long g_phb_hit, g_phb_miss;
static int g_stats = -1;

static void hittest_stats_print(void)
{
	if (g_stats != 1)
		return;
	fprintf(stderr,
	        "ShotHitTest: calls=%lu no-hit=%lu hit=%lu multi-hit(sort)=%lu kill=%lu "
	        "front-not-shootable=%lu human-obj=%lu free-slots=%lu\n"
	        "HitBox: hit=%lu miss=%lu\n"
	        "PickupHitBox: hit=%lu miss=%lu\n",
	        g_sht_calls, g_sht_none, g_sht_hit, g_sht_multi, g_sht_kill,
	        g_sht_noshoot, g_sht_human, g_sht_freeslots,
	        g_hb_hit, g_hb_miss, g_phb_hit, g_phb_miss);
	fprintf(stderr, "object types seen by ShotHitTest:");
	for (int t = 1; t < 256; t++)
		if (g_sht_types[t])
			fprintf(stderr, " $%02X=%lu", t, g_sht_types[t]);
	fprintf(stderr, "\n");
}

static int hittest_stats_on(void)
{
	if (g_stats < 0) {
		g_stats = getenv("T2_HITTEST_STATS") != NULL;
		if (g_stats)
			atexit(hittest_stats_print);
	}
	return g_stats == 1;
}

/* 68k SUB.W / ADD.W / SUBQ.W / ADDI.W: N/Z/V from the result, X=C (borrow / carry).
 * SUB.W and ADD.W also set the intermediate flags that the following CMP.W overwrites;
 * only X survives to the routine's end (the final MOVEQ/MOVE.L leaves X alone). */
static uint16_t sub_w(hle_ctx *ctx, uint16_t a, uint16_t b)
{
	uint32_t r = (uint32_t)a - (uint32_t)b;
	uint16_t rr = (uint16_t)r;
	unsigned borrow = ((uint32_t)a < (uint32_t)b) ? 1u : 0u;
	unsigned ov = (((a ^ b) & (a ^ rr)) >> 15) & 1u;
	cc_set(ctx, CC_X | CC_N | CC_Z | CC_V | CC_C,
	       (borrow ? (CC_X | CC_C) : 0) | ((rr & 0x8000) ? CC_N : 0) |
	       (rr ? 0 : CC_Z) | (ov ? CC_V : 0));
	return rr;
}

static uint16_t add_w(hle_ctx *ctx, uint16_t a, uint16_t b)
{
	uint32_t r = (uint32_t)a + (uint32_t)b;
	uint16_t rr = (uint16_t)r;
	unsigned carry = (r >> 16) & 1u;
	unsigned ov = (((a ^ rr) & (b ^ rr)) >> 15) & 1u;
	cc_set(ctx, CC_X | CC_N | CC_Z | CC_V | CC_C,
	       (carry ? (CC_X | CC_C) : 0) | ((rr & 0x8000) ? CC_N : 0) |
	       (rr ? 0 : CC_Z) | (ov ? CC_V : 0));
	return rr;
}

/* ASR.W #n,Dx: X=C=last bit shifted out, N/Z from the result, V=0 (GPGX asr_16_s). */
static uint16_t asr_w(hle_ctx *ctx, uint16_t v, int n)
{
	uint16_t r = (uint16_t)((int16_t)v >> n);
	unsigned xbit = (v >> (n - 1)) & 1u;
	cc_set(ctx, CC_X | CC_N | CC_Z | CC_V | CC_C,
	       (xbit ? (CC_X | CC_C) : 0) | ((r & 0x8000) ? CC_N : 0) | (r ? 0 : CC_Z));
	return r;
}

/* ---------------------------------------------------------------- HitBox $005940 */
HLE_FUNC_PURE(0x005940, HitBox, 200)
{
	/* movem.w (shot_xy).l,d0-d1 : GPGX sign-extends each word into the whole register */
	ctx->d[0] = (uint32_t)(int32_t)(int16_t)RAM16(SHOT_XY);
	ctx->d[1] = (uint32_t)(int32_t)(int16_t)RAM16(SHOT_XY + 2);
	/* movem.w (0x2,a6),d2-d3 */
	ctx->d[2] = (uint32_t)(int32_t)(int16_t)RAM16(ctx->a[6] + 2);
	ctx->d[3] = (uint32_t)(int32_t)(int16_t)RAM16(ctx->a[6] + 4);
	hle_movem_w(ctx, C_MOVEMW_AL_2, 2);
	hle_movem_w(ctx, C_MOVEMW_DI_2, 2);

	uint16_t d0 = (uint16_t)ctx->d[0], d1 = (uint16_t)ctx->d[1];
	uint16_t d2 = (uint16_t)ctx->d[2], d3 = (uint16_t)ctx->d[3];

	/* sub.w (a3)+,d2 / cmp.w d2,d0 / blt.w miss */
	d2 = sub_w(ctx, d2, bus_r16(ctx->a[3])); ctx->a[3] += 2;
	HLE_SEQ(ctx, C_SUBW_PI_D, 4);                             /* sub.w + cmp.w d2,d0 */
	if ((int16_t)d0 < (int16_t)d2) {
		hle_insn(ctx, C_BCC_W_TAKEN);
		goto miss;
	}
	hle_insn(ctx, C_BCC_W_NOT);
	/* sub.w (a3)+,d3 / cmp.w d3,d1 / blt.w miss */
	d3 = sub_w(ctx, d3, bus_r16(ctx->a[3])); ctx->a[3] += 2;
	HLE_SEQ(ctx, C_SUBW_PI_D, 4);
	if ((int16_t)d1 < (int16_t)d3) {
		hle_insn(ctx, C_BCC_W_TAKEN);
		goto miss;
	}
	hle_insn(ctx, C_BCC_W_NOT);
	/* add.w (a3)+,d2 / cmp.w d2,d0 / bgt.w miss */
	d2 = add_w(ctx, d2, bus_r16(ctx->a[3])); ctx->a[3] += 2;
	HLE_SEQ(ctx, C_ADDW_PI_D, 4);
	if ((int16_t)d0 > (int16_t)d2) {
		hle_insn(ctx, C_BCC_W_TAKEN);
		goto miss;
	}
	hle_insn(ctx, C_BCC_W_NOT);
	/* add.w (a3)+,d3 / cmp.w d3,d1 / bgt.w miss */
	d3 = add_w(ctx, d3, bus_r16(ctx->a[3])); ctx->a[3] += 2;
	HLE_SEQ(ctx, C_ADDW_PI_D, 4);
	if ((int16_t)d1 > (int16_t)d3) {
		hle_insn(ctx, C_BCC_W_TAKEN);
		goto miss;
	}
	hle_insn(ctx, C_BCC_W_NOT);

	set_w(&ctx->d[2], d2);
	set_w(&ctx->d[3], d3);
	ctx->d[6] = 0xFFFFFFFF;                       /* moveq #-1,d6 */
	RAM32_W(ctx->a[5], ctx->a[6]);                /* move.l a6,(a5)+ */
	ctx->a[5] += 4;
	RAM32_W(ctx->a[5], ctx->a[2]);                /* move.l a2,(a5)+ */
	ctx->a[5] += 4;
	HLE_SEQ(ctx, C_MOVEQ, C_MOVE_A_PI, C_MOVE_A_PI, C_RTS);
	cc_logic32(ctx, ctx->a[2]);                   /* last flag setter: move.l a2,(a5)+ */
	if (hittest_stats_on())
		g_hb_hit++;
	return;

miss:
	set_w(&ctx->d[2], d2);
	set_w(&ctx->d[3], d3);
	ctx->d[6] = 0;                                /* moveq #0,d6 */
	HLE_SEQ(ctx, C_MOVEQ, C_RTS);
	cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	if (hittest_stats_on())
		g_hb_miss++;
}

/* ---------------------------------------------------------- PickupHitBox $007086 */
HLE_FUNC(0x007086, PickupHitBox, 6000)
{
	ctx->d[0] = (uint32_t)(int32_t)(int16_t)RAM16(SHOT_XY);
	ctx->d[1] = (uint32_t)(int32_t)(int16_t)RAM16(SHOT_XY + 2);
	ctx->d[2] = (uint32_t)(int32_t)(int16_t)RAM16(ctx->a[6] + 2);
	ctx->d[3] = (uint32_t)(int32_t)(int16_t)RAM16(ctx->a[6] + 4);
	hle_movem_w(ctx, C_MOVEMW_AL_2, 2);
	hle_movem_w(ctx, C_MOVEMW_DI_2, 2);

	uint16_t d0 = (uint16_t)ctx->d[0], d1 = (uint16_t)ctx->d[1];
	uint16_t d2 = (uint16_t)ctx->d[2], d3 = (uint16_t)ctx->d[3];

	d2 = sub_w(ctx, d2, bus_r16(ctx->a[3])); ctx->a[3] += 2;
	HLE_SEQ(ctx, C_SUBW_PI_D, 4);
	if ((int16_t)d0 < (int16_t)d2) {
		hle_insn(ctx, C_BCC_W_TAKEN);
		goto miss;
	}
	hle_insn(ctx, C_BCC_W_NOT);
	d3 = sub_w(ctx, d3, bus_r16(ctx->a[3])); ctx->a[3] += 2;
	HLE_SEQ(ctx, C_SUBW_PI_D, 4);
	if ((int16_t)d1 < (int16_t)d3) {
		hle_insn(ctx, C_BCC_W_TAKEN);
		goto miss;
	}
	hle_insn(ctx, C_BCC_W_NOT);
	d2 = add_w(ctx, d2, bus_r16(ctx->a[3])); ctx->a[3] += 2;
	HLE_SEQ(ctx, C_ADDW_PI_D, 4);
	if ((int16_t)d0 > (int16_t)d2) {
		hle_insn(ctx, C_BCC_W_TAKEN);
		goto miss;
	}
	hle_insn(ctx, C_BCC_W_NOT);
	d3 = add_w(ctx, d3, bus_r16(ctx->a[3])); ctx->a[3] += 2;
	HLE_SEQ(ctx, C_ADDW_PI_D, 4);
	if ((int16_t)d1 > (int16_t)d3) {
		hle_insn(ctx, C_BCC_W_TAKEN);
		goto miss;
	}
	hle_insn(ctx, C_BCC_W_NOT);

	set_w(&ctx->d[2], d2);
	set_w(&ctx->d[3], d3);
	hle_insn(ctx, C_JSR_AL);
	hle_sync(ctx);
	hle_callc(ctx, PICKUP_EXPLODE);             /* jsr (sub_012AF4).l */
	set_w(&ctx->d[0], ctx->d[6]);                 /* move.w d6,d0 (d0's high word survives) */
	hle_insn(ctx, C_MOVEW_DD);
	hle_insn(ctx, C_JSR_AL);
	hle_sync(ctx);
	hle_callc(ctx, PICKUP_SOUND);               /* jsr (sub_013E30).l */
	ctx->d[6] = 0xFFFFFFFF;                       /* moveq #-1,d6 */
	HLE_SEQ(ctx, C_MOVEQ, C_RTS);
	cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_N);
	if (hittest_stats_on())
		g_phb_hit++;
	return;

miss:
	set_w(&ctx->d[2], d2);
	set_w(&ctx->d[3], d3);
	ctx->d[6] = 0;                                /* moveq #0,d6 */
	HLE_SEQ(ctx, C_MOVEQ, C_RTS);
	cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	if (hittest_stats_on())
		g_phb_miss++;
}

/* --------------------------------------------------------- ShotHitTest $005892 */
HLE_FUNC(0x005892, ShotHitTest, 24000)
{
	int free_slots = 0;

	ctx->a[6] = OBJ_TABLE;                        /* lea.l (obj_table).l,a6 */
	ctx->a[5] = HIT_LIST;                         /* lea.l (hit_list).l,a5 */
	set_w(&ctx->d[7], RAM16(OBJ_COUNT));          /* move.w (obj_count).l,d7 */
	HLE_SEQ(ctx, C_LEA_AL, C_LEA_AL, C_MOVEW_AL);
	set_w(&ctx->d[7], sub_w(ctx, (uint16_t)ctx->d[7], 1));  /* subq.w #1,d7 */
	hle_insn(ctx, C_SUBQ_W);
	if ((int16_t)(ctx->d[7] & 0xFFFF) < 0) {      /* bmi.w loc_005934 */
		HLE_SEQ(ctx, C_BCC_W_TAKEN, C_MOVEQ, C_RTS);
		goto no_hit;
	}
	hle_insn(ctx, C_BCC_W_NOT);

	for (;;) {
		/* loc_0058AA: */
		set_w(&ctx->d[5], RAM16(ctx->a[6]));      /* move.w (a6),d5 */
		hle_insn(ctx, C_MOVEW_AN);
		if ((ctx->d[5] & 0xFFFF) == 0) {          /* beq.w loc_005938: free slot */
			HLE_SEQ(ctx, C_BCC_W_TAKEN, C_LEA_D16, 10); /* beq taken + lea + bra.w */
			ctx->a[6] += 0x2C;
			free_slots++;
			continue;
		}
		hle_insn(ctx, C_BCC_W_NOT);
		if (hittest_stats_on()) {
			g_sht_types[ctx->d[5] & 0xFF]++;
			if ((ctx->d[5] & 0xFF) == 0x13)
				g_sht_human++;
		}
		ctx->a[0] = RAM32(ctx->a[6] + 0x1C);      /* movea.l (0x1c,a6),a0 */
		hle_insn(ctx, C_MOVEA_D16);
		hle_insn(ctx, C_JSR_AN);
		hle_sync(ctx);
		hle_callc(ctx, ctx->a[0]);              /* jsr (a0): per-object hit test */
		ctx->a[6] += 0x2C;                        /* lea.l (0x2c,a6),a6 */
		hle_insn(ctx, C_LEA_D16);
		set_w(&ctx->d[7], (ctx->d[7] - 1) & 0xFFFF);   /* dbra d7 */
		if ((ctx->d[7] & 0xFFFF) != 0xFFFF) {
			hle_insn(ctx, C_DBRA_CONT);
			continue;
		}
		hle_insn(ctx, C_DBRA_EXP);
		break;
	}

	/* move.l a5,d5 / subi.l #$ffffa958,d5 / beq.w loc_005934 */
	{
		uint32_t a = ctx->a[5], b = HIT_LIST, r = (a - b) & 0xFFFFFFFFu;
		ctx->d[5] = r;
		unsigned borrow = (a < b) ? 1u : 0u;
		unsigned ov = (((a ^ b) & (a ^ r)) >> 31) & 1u;
		cc_set(ctx, CC_X | CC_N | CC_Z | CC_V | CC_C,
		       (borrow ? (CC_X | CC_C) : 0) | ((r & 0x80000000u) ? CC_N : 0) |
		       (r ? 0 : CC_Z) | (ov ? CC_V : 0));
	}
	HLE_SEQ(ctx, C_MOVE_A_D, C_SUBI_L);
	if (ctx->d[5] == 0) {
		HLE_SEQ(ctx, C_BCC_W_TAKEN, C_MOVEQ, C_RTS);
		goto no_hit;
	}
	hle_insn(ctx, C_BCC_W_NOT);

	ctx->a[5] = HIT_LIST;                         /* lea.l (hit_list).l,a5 */
	hle_insn(ctx, C_LEA_AL);
	set_w(&ctx->d[5], asr_w(ctx, (uint16_t)ctx->d[5], 3));    /* asr.w #3,d5 */
	hle_insn(ctx, C_ASR_W3);
	set_w(&ctx->d[5], sub_w(ctx, (uint16_t)ctx->d[5], 2));    /* subq.w #2,d5 */
	hle_insn(ctx, C_SUBQ_W);
	if ((int16_t)(ctx->d[5] & 0xFFFF) < 0) {      /* bmi.w loc_005914 (fewer than 2 hits) */
		hle_insn(ctx, C_BCC_W_TAKEN);
		goto front;
	}
	hle_insn(ctx, C_BCC_W_NOT);

	unsigned n_hits = (unsigned)(ctx->d[5] & 0xFFFF) + 2;
	for (;;) {
		/* loc_0058D8: one bubble-sort pass */
		ctx->d[0] = 0;                            /* moveq #0,d0 */
		ctx->d[3] = 0;                            /* moveq #0,d3 */
		set_w(&ctx->d[7], ctx->d[5]);             /* move.w d5,d7 */
		HLE_SEQ(ctx, C_MOVEQ, C_MOVEQ, C_MOVEW_DD);
		for (;;) {
			/* loc_0058DE */
			uint32_t off = (uint32_t)(int16_t)(ctx->d[0] & 0xFFFF);
			ctx->a[0] = RAM32(ctx->a[5] + off);           /* movea.l (0,a5,d0.w),a0 */
			ctx->a[1] = RAM32(ctx->a[5] + off + 8);       /* movea.l (8,a5,d0.w),a1 */
			HLE_SEQ(ctx, C_MOVEA_IX, C_MOVEA_IX);
			uint8_t depth0 = RAM8(ctx->a[0] + 7);
			set_b(&ctx->d[1], depth0);                    /* move.b (0x7,a0),d1 */
			hle_insn(ctx, C_MOVEB_D16);
			uint8_t depth1 = RAM8(ctx->a[1] + 7);
			hle_insn(ctx, C_CMPB_D16);
			if ((int8_t)depth0 <= (int8_t)depth1) {       /* cmp.b (0x7,a1),d1 / ble.s */
				hle_insn(ctx, C_BCC_S_TAKEN);
			} else {
				hle_insn(ctx, C_BCC_S_NOT);
				uint32_t d2 = RAM32(ctx->a[5] + off + 4); /* move.l (4,a5,d0.w),d2 */
				ctx->d[2] = d2;
				hle_insn(ctx, C_MOVE_IX_D);
				RAM32_W(ctx->a[5] + off + 4, RAM32(ctx->a[5] + off + 12));  /* move.l (0xc,..),(0x4,..) */
				hle_insn(ctx, C_MOVE_IX_IX);
				RAM32_W(ctx->a[5] + off + 12, d2);        /* move.l d2,(0xc,a5,d0.w) */
				hle_insn(ctx, C_MOVE_D_IX);
				RAM32_W(ctx->a[5] + off + 8, ctx->a[0]);  /* move.l a0,(0x8,a5,d0.w) */
				hle_insn(ctx, C_MOVE_A_IX);
				RAM32_W(ctx->a[5] + off, ctx->a[1]);      /* move.l a1,(0x0,a5,d0.w) */
				hle_insn(ctx, C_MOVE_A_IX);
				set_b(&ctx->d[3], 0xFF);                  /* st.b d3 */
				hle_insn(ctx, C_STB_D);
			}
			set_w(&ctx->d[0], add_w(ctx, (uint16_t)ctx->d[0], 8));  /* addi.w #8,d0 */
			hle_insn(ctx, C_ADDI_W);
			set_w(&ctx->d[7], (ctx->d[7] - 1) & 0xFFFF);  /* dbra d7 */
			if ((ctx->d[7] & 0xFFFF) != 0xFFFF) {
				hle_insn(ctx, C_DBRA_CONT);
				continue;
			}
			hle_insn(ctx, C_DBRA_EXP);
			break;
		}
		hle_insn(ctx, C_TSTB_D);                              /* tst.b d3 */
		if ((ctx->d[3] & 0xFF) != 0) {                /* bne.s loc_0058D8 */
			hle_insn(ctx, C_BCC_S_TAKEN);
			continue;
		}
		hle_insn(ctx, C_BCC_S_NOT);
		break;
	}
	if (hittest_stats_on() && n_hits > 1)
		g_sht_multi++;

front:
	/* loc_005914: take the front-most entry */
	ctx->a[6] = RAM32(ctx->a[5]);                 /* movea.l (a5)+,a6 */
	ctx->a[5] += 4;
	ctx->a[0] = RAM32(ctx->a[5]);                 /* movea.l (a5)+,a0 */
	ctx->a[5] += 4;
	HLE_SEQ(ctx, C_MOVEA_PI, C_MOVEA_PI);
	set_w(&ctx->d[5], RAM16(ctx->a[6]));          /* move.w (a6),d5 */
	hle_insn(ctx, C_MOVEW_AN);
	hle_insn(ctx, C_BTST);                                /* btst.b #0xf,d5 */
	if (ctx->d[5] & 0x8000) {                     /* beq.s loc_005930 if clear */
		hle_insn(ctx, C_BCC_S_NOT);
		hle_insn(ctx, C_JSR_AN);
		hle_sync(ctx);
		hle_callc(ctx, ctx->a[0]);              /* jsr (a0): on-hit routine */
		set_w(&ctx->d[0], RAM16(SHOT_DAMAGE));    /* move.w (shot_damage).l,d0 */
		hle_insn(ctx, C_MOVEW_AL);
		uint16_t hp = RAM16(ctx->a[6] + 8);
		uint16_t res = sub_w(ctx, hp, (uint16_t)ctx->d[0]);
		RAM16(ctx->a[6] + 8) = res;               /* sub.w d0,(0x8,a6) */
		hle_insn(ctx, C_SUBW_D16);
		if (res & 0x8000) {                       /* bpl.s loc_005930 if >= 0 */
			hle_insn(ctx, C_BCC_S_NOT);
			hle_insn(ctx, C_JSR_AN);
			hle_sync(ctx);
			hle_callc(ctx, ctx->a[0]);          /* jsr (a0): kill routine */
			if (hittest_stats_on())
				g_sht_kill++;
		} else {
			hle_insn(ctx, C_BCC_S_TAKEN);
		}
	} else {
		hle_insn(ctx, C_BCC_S_TAKEN);
		if (hittest_stats_on())
			g_sht_noshoot++;
	}

	/* loc_005930: moveq #-1,d5 / rts */
	ctx->d[5] = 0xFFFFFFFF;
	HLE_SEQ(ctx, C_MOVEQ, C_RTS);
	cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_N);
	if (hittest_stats_on()) {
		g_sht_calls++;
		g_sht_hit++;
		g_sht_freeslots += free_slots;
	}
	return;

no_hit:
	ctx->d[5] = 0;                                /* moveq #0,d5 / rts */
	cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	if (hittest_stats_on()) {
		g_sht_calls++;
		g_sht_none++;
		g_sht_freeslots += free_slots;
	}
}
