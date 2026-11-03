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

#define RNG_STATE 0xFFA0F0   /* data/ram.txt: rng_state, ResetCheatsRng sets it to $87654321 */

HLE_FUNC_PURE(0x00881C, Random, 200)
{
	/* movem.l d0-d2,-(sp) 32, move.l (abs).l,d0 20, subq.l #1 8, move.w 4, subq.l #1 8, swap 4, move.w 4,
	 * add.w 4, swap 4, move.w 4, swap 4, sub.l 8, move.l d0,(abs).l 20, movem.l (sp)+,d0-d2 36,
	 * move.w (abs).l,d0 16, rts 16 */
	HLE_SEQ(ctx, 32, 20, 8, 4, 8, 4, 4, 4, 4, 4, 4, 8, 20, 36, 16, 16);
	uint32_t rng = RAM32(RNG_STATE);
	uint32_t a = rng - 2;                       /* subq.l #2 in two steps */
	uint32_t hi = (a >> 16) & 0xFFFF;           /* hi(rng_state - 2) */
	uint32_t sum = (((rng - 1) & 0xFFFF) + hi) & 0xFFFF;  /* add.w, 16-bit */
	uint32_t d1 = (sum << 16) | hi;             /* the value sub.l subtracts */
	uint32_t borrow = d1 > a;                   /* sub.l sets X = C = borrow */
	uint32_t nd = a - d1;                       /* sub.l d1,d0 */

	RAM32_W(RNG_STATE, nd);
	set_w(&ctx->d[0], nd);                      /* move.w (rng_state+2).l,d0 */
	cc_set(ctx, CC_X | CC_N | CC_Z | CC_V | CC_C,
	       (borrow ? CC_X : 0) | ((nd & 0x8000) ? CC_N : 0) | ((nd & 0xFFFF) ? 0 : CC_Z));
}
