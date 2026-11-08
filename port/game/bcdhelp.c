/* sub_007C68, the gunpower bar clear: (0x1a,a4) gets the word 1 and then two SBCD steps (a BCD decrement of
 * the 4-byte counter's low bytes) -
 *     lea (0x1a,a4),a0 / movea.l a0,a1 / move.w #1,(a1)+ / andi.b #$ef,ccr / sbcd -(a1),-(a0) x2 / rts
 * One HLE_FUNC_YIELD with every instruction charged right after its effect (docs/porting.md, "Yielding").
 * SBCD follows GPGX (m68k_op_sbcd_8_mm): low nibble difference minus X, decimal adjust, borrow into X/C, N from
 * the result, Z cleared when the result is not zero and kept otherwise. */
#include "hle.h"

#include <stdint.h>

static void sbcd_mm(hle_ctx *ctx)
{
	ctx->a[1] -= 1;
	uint32_t src = RAM8(ctx->a[1]);
	ctx->a[0] -= 1;
	uint32_t dst = RAM8(ctx->a[0]);
	uint32_t x = (ctx->sr & CC_X) ? 1 : 0;
	uint32_t res = (dst & 0xF) - (src & 0xF) - x;
	if (res > 9)
		res -= 6;
	res += (dst & 0xF0) - (src & 0xF0);
	unsigned c = res > 0x99;
	if (c)
		res += 0xA0;
	res &= 0xFF;
	unsigned f = (c ? (CC_X | CC_C) : 0) | ((res & 0x80) ? CC_N : 0);
	unsigned keep_z = (ctx->sr & CC_Z) && res == 0;
	cc_set(ctx, CC_X | CC_N | CC_Z | CC_C, f | (keep_z ? CC_Z : 0));
	RAM8(ctx->a[0]) = (uint8_t)res;
}

HLE_FUNC_YIELD(0x007C68, Sub007C68, 4000)
{
	ctx->a[0] = ctx->a[4] + 0x1A;                  /* lea.l (0x1a,a4),a0 */
	hle_insn(ctx, 8);
	ctx->a[1] = ctx->a[0];                         /* movea.l a0,a1 */
	hle_insn(ctx, 4);
	RAM16(ctx->a[1]) = 1;                          /* move.w #1,(a1)+ */
	ctx->a[1] += 2;
	cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, 0);
	hle_insn(ctx, 12);
	ctx->sr &= (uint16_t)~CC_X;                    /* andi.b #$ef,ccr */
	hle_insn(ctx, 20);
	sbcd_mm(ctx);                                  /* sbcd -(a1),-(a0) */
	hle_insn(ctx, 18);
	sbcd_mm(ctx);
	hle_insn(ctx, 18);
	hle_insn(ctx, 16);                             /* rts */
}
