/* The tiny helpers of the VDP command list and the sprite queue, called from many 68k and C
 * callers (docs/remaining.md): VdpListReset $011D12, VdpListEnd $011D28, SpriteQueueReset $013376,
 * and the command fetch VdpListNext $011D38 (`move.w (a6)+,d0 ; asl.w #2,d0 ; jmp VdpListJump(pc,d0.w)`).
 *
 * Every instruction is charged on the instruction timeline with its GPGX cost; the final flags are those of
 * the last flag-setting instruction. They only touch work RAM, so they are PURE.
 *
 * VdpListNext is a range, not a subroutine: it ends in the `jmp` into the bra table at VdpListJump
 * $011D40 (one `bra.w` per command 0..8), which the 68k then executes (the handlers are registered
 * ranges/functions of their own, game/vdplist.c). A command word above 8 would run into the code after
 * the table, so such a list is left to the 68k (declined before anything changed).
 */
#include "hle.h"

#include <stdint.h>

#define VDP_LIST_PTR    0xFF80BCu
#define VDP_LIST_DATA   0xFF80B8u
#define SPR_QUEUE_N     0xFF9DF2u
#define SPR_QUEUE_PTR   0xFF9DF6u
#define SPR_QUEUE_DATA  0xFF9DFAu

/* $011D12: vdp_list_ptr = vdp_list, vdp_list_data = $FFB452 */
HLE_FUNC_PURE(0x011D12, VdpListReset, 72)
{
	RAM32_W(VDP_LIST_PTR, 0xFFFFB252u);          /* move.l #i,(abs).l: each charged right after its effect */
	hle_insn(ctx, 28);
	RAM32_W(VDP_LIST_DATA, 0xFFFFB452u);
	cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_N);    /* flags of the last move.l: #$FFFFB452 */
	hle_insn(ctx, 28);
	hle_insn(ctx, 16);                           /* rts */
}

/* $011D28: write command 0 at the list pointer; a5 = the pointer */
HLE_FUNC_PURE(0x011D28, VdpListEnd, 48)
{
	uint32_t a5 = RAM32(VDP_LIST_PTR);           /* movea.l (abs).l,a5 */
	ctx->a[5] = a5;
	hle_insn(ctx, 20);
	RAM16(a5) = 0;                               /* clr.w (a5) */
	cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);    /* clr: N=0 Z=1 V=0 C=0, X kept */
	hle_insn(ctx, 12);
	hle_insn(ctx, 16);                           /* rts */
}

/* $013376: sprite queue empty */
HLE_FUNC_PURE(0x013376, SpriteQueueReset, 92)
{
	RAM32_W(SPR_QUEUE_PTR, 0xFFFFA250u);         /* move.l #i,(abs).l x2 ; move.b #0,(abs).l ; rts, each */
	hle_insn(ctx, 28);                           /* charged right after its effect */
	RAM32_W(SPR_QUEUE_DATA, 0xFFFFA318u);
	hle_insn(ctx, 28);
	RAM8(SPR_QUEUE_N) = 0;
	cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	hle_insn(ctx, 20);
	hle_insn(ctx, 16);
}

/* $011D38: fetch the command word and jump through the bra table */
HLE_RANGE_PURE(0x011D38, VdpListNext, 100,
               0x011D40, 0x011D44, 0x011D48, 0x011D4C, 0x011D50, 0x011D54, 0x011D58, 0x011D5C, 0x011D60)
{
	uint32_t a6 = ctx->a[6];
	uint16_t cmd = RAM16(a6);
	if (cmd > 8) {
		hle_decline();
		return;
	}
	HLE_SEQ(ctx, 8, 10, 14);                     /* move.w (a6)+,d0 ; asl.w #2,d0 ; jmp (d8,pc,d0.w) */
	ctx->a[6] = a6 + 2;
	set_w(&ctx->d[0], cmd);
	uint16_t r = (uint16_t)(cmd << 2);           /* asl.w #2: cmd <= 8, no bits leave and V is clear */
	set_w(&ctx->d[0], r);
	cc_set(ctx, CC_X | CC_N | CC_Z | CC_V | CC_C, (r ? 0 : CC_Z));
	hle_jump(ctx, 0x011D40u + r);
}
