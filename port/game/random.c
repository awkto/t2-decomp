/* Random $00881C: advance the 32-bit LFSR-ish RNG at rng_state and return the low word.
 *
 * Original (src/t2.s):
 *     Random:
 *         movem.l d0-d2,-(sp)              ; save scratch (d1/d2 high words are garbage)
 *         move.l  (rng_state).l,d0         ; d0 = rng_state
 *         subq.l  #1,d0                    ; d0 = rng_state - 1
 *         move.w  d0,d1                    ; d1.lo = lo(rng_state - 1)
 *         subq.l  #1,d0                    ; d0 = rng_state - 2
 *         swap    d0
 *         move.w  d0,d2                    ; d2.lo = hi(rng_state - 2)
 *         add.w   d2,d1                    ; d1.lo = lo(rng_state - 1) + hi(rng_state - 2)
 *         swap    d1
 *         move.w  d2,d1                    ; d1 = (sum << 16) | hi(rng_state - 2)
 *         swap    d0                       ; d0 = rng_state - 2
 *         sub.l   d1,d0                    ; new state
 *         move.l  d0,(rng_state).l
 *         movem.l (sp)+,d0-d2              ; d0-d2 back (d0.hi restored)
 *         move.w  (rng_state+2).l,d0       ; d0.lo = low word of the new state
 *         rts
 *
 * Only rng_state ($FFA0F0, 4 bytes) is touched; d0-d2 are saved and restored, so the
 * high word of d0 is the entry value. Flags: the final move.w sets N/Z from the returned
 * low word and clears V/C; X is left by the sub.l (borrow). No caller reads d1/d2. */
#include "hle.h"

#define ld32s(a) RAM32(a)
#define RNG_STATE 0xFFA0F0   /* data/ram.txt: rng_state, ResetCheatsRng sets it to $87654321 */

HLE_FUNC_PURE(0x00881C, Random, 200)
{
	/* Interrupt-exact (docs/porting.md, "Yielding"): the effect of an instruction first, then its charge, so
	 * a handler that runs at an instruction boundary sees what the 68k has done by then (the saved registers
	 * on the stack, the new rng_state after the move.l, the stack pointer). */
	uint32_t sd0 = ctx->d[0], sd1 = ctx->d[1], sd2 = ctx->d[2];
	ctx->a[7] -= 4; RAM32_W(ctx->a[7], sd2);
	ctx->a[7] -= 4; RAM32_W(ctx->a[7], sd1);
	ctx->a[7] -= 4; RAM32_W(ctx->a[7], sd0);
	hle_movem_l(ctx, 32, 3);                    /* movem.l d0-d2,-(sp) */
	uint32_t rng = RAM32(RNG_STATE);            /* move.l (rng_state).l,d0 */
	hle_insn(ctx, 20);
	uint32_t a = rng - 2;                       /* subq.l #1 / subq.l #1 */
	uint32_t hi = (a >> 16) & 0xFFFF;           /* hi(rng_state - 2) */
	uint32_t sum = (((rng - 1) & 0xFFFF) + hi) & 0xFFFF;  /* add.w, 16-bit */
	uint32_t d1 = (sum << 16) | hi;             /* the value sub.l subtracts */
	uint32_t borrow = d1 > a;                   /* sub.l sets X = C = borrow */
	uint32_t nd = a - d1;                       /* sub.l d1,d0 */
	HLE_SEQ(ctx, 8, 4, 8, 4, 4, 4, 4, 4, 4, 8);   /* subq.l, move.w, subq.l, swap, move.w, add.w, swap, move.w, swap, sub.l */
	RAM32_W(RNG_STATE, nd);                     /* move.l d0,(rng_state).l */
	hle_insn(ctx, 20);
	ctx->d[0] = ld32s(ctx->a[7]); ctx->a[7] += 4;       /* movem.l (sp)+,d0-d2 */
	ctx->d[1] = ld32s(ctx->a[7]); ctx->a[7] += 4;
	ctx->d[2] = ld32s(ctx->a[7]); ctx->a[7] += 4;
	hle_movem_l(ctx, 36, 3);
	set_w(&ctx->d[0], nd);                      /* move.w (rng_state+2).l,d0 */
	cc_set(ctx, CC_X | CC_N | CC_Z | CC_V | CC_C,
	       (borrow ? CC_X : 0) | ((nd & 0x8000) ? CC_N : 0) | ((nd & 0xFFFF) ? 0 : CC_Z));
	hle_insn(ctx, 16);
	hle_insn(ctx, 16);                          /* rts */
}
