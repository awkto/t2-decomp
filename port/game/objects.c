/* Object runner and leaf helpers. docs/subsystems/objects.md, src/t2.s.
 *
 *   ObjRunAll $00B398: obj_count -> d7 (-1 = none), a6 = obj_table, then the loop at $00B3AE
 *   ObjNext   $00B3BE: a6 += $2C, dbra d7 into the loop; the loop skips free slots (word at a6 = 0)
 *                      and for a live one loads a0 = (+$20,a6) and runs `jmp (a0)` (the AI state)
 *   The AI states stay 68k. Like the level-script interpreter (levelscript.c) the two entries are
 *   HLE_RANGE_PURE pieces that stop right BEFORE the `jmp (a0)` at $00B3B6: the real 68k executes
 *   the jump (8 cycles), the AI runs natively and its closing `bra ObjNext` re-enters the C
 *   ObjNext. Live across the AI call: a6 = slot, d7.w = remaining count, a0 = AI, d0 = slot word.
 *   The other exit is the `rts` at $00B3C6 (real 68k, 16 cycles).
 *   Flags: X comes from the subq.w in ObjRunAll (ObjNext keeps the entry X); N/Z/V/C from the
 *   last `move.w (a6),d0`, or from the subq.w when the loop is not entered.
 *
 *   ObjMoveX $00B4A6, ObjScrollX $012316, ObjKillIfLeftOf $012332, ObjKillIfRightOf $012342,
 *   ObjAlloc $00930E, ObjSpawnTemplate $0092DC (calls the C ObjAlloc directly; its `jsr` is charged).
 * All touch work RAM and registers only (HLE_FUNC_PURE); every instruction is on the timeline. */
#include "hle.h"

#include <stdint.h>

#define OBJ_TABLE     0xFFFF87B2u   /* lea.l (abs).l sign-extends a $FFxxxx address */
#define OBJ_COUNT     0xFF80DAu
#define OBJ_SCRATCH   0xFF9EBAu   /* clr.w at ObjRunAll entry */
#define ROW_DELTA     0xFFFFB0D2u   /* row_delta[0] (word table), index +6 + 28 */

#define EXIT_JMP 0x00B3B6u
#define EXIT_RTS 0x00B3C6u

enum { OP_ADD, OP_SUB };

/* a (op) b at `bits` width with the 68k flags (X set as well when setx) */
static uint32_t alu(hle_ctx *c, int op, uint32_t a, uint32_t b, int bits, int setx)
{
	uint32_t m = (1u << bits) - 1, sg = 1u << (bits - 1);
	a &= m; b &= m;
	uint32_t r;
	unsigned cy, ov;
	if (op == OP_ADD) {
		uint64_t w = (uint64_t)a + b;
		r = (uint32_t)w & m;
		cy = (unsigned)((w >> bits) & 1);
		ov = (((a ^ r) & (b ^ r)) & sg) != 0;
	} else {
		r = (a - b) & m;
		cy = a < b;
		ov = (((a ^ b) & (a ^ r)) & sg) != 0;
	}
	unsigned f = (cy ? CC_C : 0) | (ov ? CC_V : 0) | ((r & sg) ? CC_N : 0) | (r ? 0 : CC_Z);
	if (setx && cy)
		f |= CC_X;
	cc_set(c, (setx ? CC_X : 0) | CC_N | CC_Z | CC_V | CC_C, f);
	return r;
}

/* ------------------------------------------------------------------ ObjRunAll / ObjNext */
/* the loop at $00B3AE, entered with a6 at a slot; ends at the jmp (a0) exit */
static void obj_loop(hle_ctx *ctx)
{
	for (;;) {
		uint32_t a6 = ctx->a[6];
		uint16_t w = RAM16(a6);                 /* move.w (a6),d0 */
		set_w(&ctx->d[0], w);
		cc_logic16(ctx, w);
		hle_insn(ctx, 8);
		if (w == 0) {
			hle_insn(ctx, 10);              /* beq.s taken */
			ctx->a[6] = a6 + 0x2C;
			hle_insn(ctx, 8);               /* lea.l (0x2c,a6),a6 */
			hle_insn(ctx, 10);              /* bra.s */
			continue;
		}
		hle_insn(ctx, 8);                       /* beq.s not taken */
		ctx->a[0] = RAM32(a6 + 0x20);           /* movea.l (0x20,a6),a0 */
		hle_insn(ctx, 16);
		hle_jump(ctx, EXIT_JMP);                /* jmp (a0) runs as real 68k */
		return;
	}
}

HLE_RANGE_PURE(0x00B398, ObjRunAll, 400, EXIT_JMP, EXIT_RTS)
{
	RAM16(OBJ_SCRATCH) = 0;                         /* clr.w (abs).l */
	hle_insn(ctx, 20);
	ctx->a[6] = OBJ_TABLE;                          /* lea.l (abs).l,a6 */
	hle_insn(ctx, 12);
	set_w(&ctx->d[7], RAM16(OBJ_COUNT));            /* move.w (abs).l,d7 */
	hle_insn(ctx, 16);
	uint32_t r = alu(ctx, OP_SUB, ctx->d[7], 1, 16, 1);   /* subq.w #1,d7 */
	set_w(&ctx->d[7], r);
	hle_insn(ctx, 4);
	if (r & 0x8000) {                               /* bmi.s taken */
		hle_insn(ctx, 10);
		hle_jump(ctx, EXIT_RTS);
		return;
	}
	hle_insn(ctx, 8);
	obj_loop(ctx);
}

HLE_RANGE_PURE(0x00B3BE, ObjNext, 400, EXIT_JMP, EXIT_RTS)
{
	ctx->a[6] += 0x2C;                              /* lea.l (0x2c,a6),a6 */
	hle_insn(ctx, 8);
	uint16_t d7 = (uint16_t)(ctx->d[7] - 1);        /* dbra d7,loop */
	set_w(&ctx->d[7], d7);
	if (d7 == 0xFFFF) {
		hle_insn(ctx, 14);
		hle_jump(ctx, EXIT_RTS);
		return;
	}
	hle_insn(ctx, 10);
	obj_loop(ctx);
}

/* ------------------------------------------------------------------ ObjMoveX $00B4A6 */
HLE_FUNC_PURE(0x00B4A6, ObjMoveX, 300)
{
	uint32_t a6 = ctx->a[6];
	set_b(&ctx->d[0], RAM8(a6 + 0xE));              /* move.b (0xe,a6),d0 */
	hle_insn(ctx, 12);
	set_w(&ctx->d[0], (uint32_t)(int32_t)(int8_t)ctx->d[0]);   /* ext.w d0 */
	hle_insn(ctx, 4);
	set_b(&ctx->d[1], RAM8(a6 + 0xA));              /* move.b (0xa,a6),d1 */
	hle_insn(ctx, 12);
	ctx->d[4] = 7;                                  /* moveq #7,d4 */
	hle_insn(ctx, 4);
	uint16_t d4 = 7;
	for (;;) {
		uint32_t r = alu(ctx, OP_ADD, RAM8(a6 + 0xB), ctx->d[1], 8, 1);   /* add.b d1,(0xb,a6) */
		int carry = (ctx->sr & CC_C) != 0;
		RAM8(a6 + 0xB) = (uint8_t)r;
		hle_insn(ctx, 16);
		if (!carry) {
			hle_insn(ctx, 10);              /* bcc.s taken */
		} else {
			hle_insn(ctx, 8);
			RAM16(a6 + 2) = (uint16_t)alu(ctx, OP_ADD, RAM16(a6 + 2), ctx->d[0], 16, 1);
			hle_insn(ctx, 16);              /* add.w d0,(0x2,a6) */
		}
		d4--;
		if (d4 == 0xFFFF) {
			hle_insn(ctx, 14);              /* dbra falls through */
			break;
		}
		hle_insn(ctx, 10);
	}
	set_w(&ctx->d[4], d4);
	hle_insn(ctx, 16);                              /* rts */
}

/* ------------------------------------------------------------------ ObjScrollX $012316 */
HLE_FUNC_PURE(0x012316, ObjScrollX, 120)
{
	uint32_t a6 = ctx->a[6];
	set_b(&ctx->d[0], RAM8(a6 + 6));                /* move.b (0x6,a6),d0 */
	hle_insn(ctx, 12);
	set_w(&ctx->d[0], (uint32_t)(int32_t)(int8_t)ctx->d[0]);   /* ext.w d0 */
	hle_insn(ctx, 4);
	set_w(&ctx->d[0], ctx->d[0] + 0x1C);            /* addi.w #$1c,d0 */
	hle_insn(ctx, 8);
	set_w(&ctx->d[0], ctx->d[0] + ctx->d[0]);       /* add.w d0,d0 */
	hle_insn(ctx, 4);
	ctx->a[1] = ROW_DELTA;                          /* lea.l (abs).l,a1 */
	hle_insn(ctx, 12);
	set_w(&ctx->d[0], RAM16(ROW_DELTA + (uint32_t)(int32_t)(int16_t)ctx->d[0]));
	hle_insn(ctx, 14);                              /* move.w (0,a1,d0.w),d0 */
	RAM16(a6 + 2) = (uint16_t)alu(ctx, OP_SUB, RAM16(a6 + 2), ctx->d[0], 16, 1);
	hle_insn(ctx, 16);                              /* sub.w d0,(0x2,a6) */
	hle_insn(ctx, 16);                              /* rts */
}

/* ------------------------------------------------------------------ ObjKillIf* */
static void obj_kill(hle_ctx *ctx, int right)
{
	uint32_t a6 = ctx->a[6];
	alu(ctx, OP_SUB, ctx->d[0], RAM16(a6 + 2), 16, 0);   /* cmp.w (0x2,a6),d0 */
	hle_insn(ctx, 12);
	unsigned sr = ctx->sr;
	int n = (sr & CC_N) != 0, v = (sr & CC_V) != 0, z = (sr & CC_Z) != 0;
	int skip = right ? (!z && n == v) : (n != v);   /* bgt.s / blt.s taken: nothing to kill */
	if (skip) {
		hle_insn(ctx, 10);
	} else {
		hle_insn(ctx, 8);
		RAM16(a6) = 0;                          /* clr.w (a6) */
		hle_insn(ctx, 12);
		RAM16(OBJ_COUNT) = (uint16_t)alu(ctx, OP_SUB, RAM16(OBJ_COUNT), 1, 16, 1);
		hle_insn(ctx, 20);                      /* subq.w #1,(obj_count).l */
	}
	hle_insn(ctx, 16);                              /* rts */
}

HLE_FUNC_PURE(0x012332, ObjKillIfLeftOf, 80) { obj_kill(ctx, 0); }
HLE_FUNC_PURE(0x012342, ObjKillIfRightOf, 80) { obj_kill(ctx, 1); }

/* ------------------------------------------------------------------ ObjAlloc $00930E */
HLE_FUNC_PURE(0x00930E, ObjAlloc, 3600)
{
	uint32_t a6 = OBJ_TABLE;                        /* lea.l (abs).l,a6 */
	hle_insn(ctx, 12);
	uint16_t d2 = 0x4F;                             /* move.w #$4f,d2 */
	hle_insn(ctx, 8);
	for (;;) {
		uint16_t w = RAM16(a6);                 /* tst.w (a6) */
		cc_logic16(ctx, w);
		hle_insn(ctx, 8);
		if (w == 0) {
			hle_insn(ctx, 10);              /* beq.s taken */
			break;
		}
		hle_insn(ctx, 8);
		a6 += 0x2C;                             /* lea.l (0x2c,a6),a6 */
		hle_insn(ctx, 8);
		d2--;
		if (d2 == 0xFFFF) {
			hle_insn(ctx, 14);
			break;
		}
		hle_insn(ctx, 10);
	}
	ctx->a[6] = a6;
	set_w(&ctx->d[2], d2);
	hle_insn(ctx, 16);                              /* rts */
}

/* ------------------------------------------------------------------ ObjSpawnTemplate $0092DC */
/* the template is in ROM; allow a RAM one */
static inline uint16_t rd16(uint32_t a)
{
	return ((a & 0xFFFFFFu) >= 0xFF0000u) ? RAM16(a) : ROM16(a);
}
static inline uint32_t rd32(uint32_t a) { return ((uint32_t)rd16(a) << 16) | rd16(a + 2); }
static inline uint8_t rd8(uint32_t a)
{
	uint16_t w = rd16(a & ~1u);
	return (a & 1) ? (uint8_t)w : (uint8_t)(w >> 8);
}

HLE_FUNC_PURE(0x0092DC, ObjSpawnTemplate, 3900)
{
	hle_insn(ctx, 20);                              /* jsr (ObjAlloc).l */
	ObjAlloc(ctx);
	uint32_t a6 = ctx->a[6], a1 = ctx->a[1];
	uint16_t t = rd16(a1);                          /* move.w (a1)+,d2 */
	a1 += 2;
	set_w(&ctx->d[2], t);
	hle_insn(ctx, 8);
	set_w(&ctx->d[2], t | 0x8000);                  /* bset.b #$f,d2 (long, bit 15) */
	hle_insn(ctx, 10);
	RAM16(a6) = (uint16_t)(t | 0x8000);             /* move.w d2,(a6) */
	hle_insn(ctx, 8);
	RAM16(a6 + 2) = rd16(a1); a1 += 2;              /* move.w (a1)+,(2,a6) */
	hle_insn(ctx, 16);
	RAM16(a6 + 4) = rd16(a1); a1 += 2;
	hle_insn(ctx, 16);
	RAM8(a6 + 6) = rd8(a1); a1 += 1;                /* move.b (a1)+,(6,a6) */
	hle_insn(ctx, 16);
	RAM8(a6 + 7) = rd8(a1); a1 += 1;
	hle_insn(ctx, 16);
	RAM32_W(a6 + 0x20, rd32(a1)); a1 += 4;          /* move.l (a1)+,(0x20,a6) */
	hle_insn(ctx, 24);
	RAM32_W(a6 + 0x1C, rd32(a1)); a1 += 4;
	hle_insn(ctx, 24);
	RAM16(a6 + 8) = rd16(a1); a1 += 2;              /* move.w (a1)+,(8,a6) */
	hle_insn(ctx, 16);
	ctx->a[1] = a1;
	RAM16(OBJ_COUNT) = (uint16_t)alu(ctx, OP_ADD, RAM16(OBJ_COUNT), 1, 16, 1);
	hle_insn(ctx, 20);                              /* addq.w #1,(obj_count).l */
	hle_insn(ctx, 16);                              /* rts */
}
