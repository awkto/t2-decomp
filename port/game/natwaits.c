/* Wait loops as C blocks, for the native runner only (t2-decomp#11, docs/native.md "Waits").
 *
 * The game has no frame-locked main loop: it spins on counters the VBlank handler changes. With a
 * 68k these spins are the original code (the phase of the loop decides which instruction boundary
 * the interrupt is taken at, so lockstep needs the original). The native runner has no instruction
 * to interrupt: a wait block tests the condition like the original, and if it still holds it calls
 * hle_idle() (charges the rest of the CPU's time slice) and jumps to itself; the frame loop then
 * delivers the interrupt and the block runs again. The blocks belong to the group "native"
 * (hle_native_mode enables them), so --hle all on the emulated build is unchanged.
 *
 * Every instruction boundary of a spin is a block entry: a savestate made by the 68k can be stopped
 * in the middle of a spin.
 *
 * native covers $0086E2-$0086F2, native covers $0078A6-$0078AC (read by tools/py/remaining68k.py)
 *
 *   WaitVBlank $0086E2   move.w d0,-(sp) / move.w (vblank_count),d0
 *   $0086EA              cmp.w (vblank_count).l,d0 / beq.s $0086EA
 *   $0086F0              move.w (sp)+,d0 / rts
 *   FramePaceWait $0078A6  cmp.w (vblank_count).l,d0 / bgt.s $0078A6  -> $0078AE
 */
#include "hle.h"

#include <stdint.h>

#define VBLANK_COUNT 0xFFA0EE

static void cmp16(hle_ctx *c, uint16_t dst, uint16_t src)
{
	uint32_t r = (uint16_t)(dst - src);
	unsigned f = (dst < src ? CC_C : 0) | ((((dst ^ src) & (dst ^ r)) & 0x8000) ? CC_V : 0) |
	             ((r & 0x8000) ? CC_N : 0) | (r ? 0 : CC_Z);
	cc_set(c, CC_N | CC_Z | CC_V | CC_C, f);
}

/* $0086E2 WaitVBlank: remember the frame counter */
HLE_FUNC_GROUP(0x0086E2, nat_WaitVBlank, 24, "native")
{
	push16(ctx, (uint16_t)ctx->d[0]);
	set_w(&ctx->d[0], RAM16(VBLANK_COUNT));
	cc_logic16(ctx, ctx->d[0]);
	ctx->cycles = 8 + 16;
	hle_jump(ctx, 0x0086EA);
}

static void pop_d0_rts(hle_ctx *ctx)   /* $0086F0 move.w (sp)+,d0 ; rts (the layer pops the return address) */
{
	set_w(&ctx->d[0], pop16(ctx));
	cc_logic16(ctx, ctx->d[0]);
	ctx->cycles = 8 + 16;
}

/* $0086EA cmp.w (vblank_count).l,d0 / beq.s: spin until the counter changes */
HLE_WAIT_FUNC_GROUP(0x0086EA, nat_WaitVBlankSpin, 24, "native")
{
	cmp16(ctx, (uint16_t)ctx->d[0], RAM16(VBLANK_COUNT));
	if (ctx->sr & CC_Z) {
		hle_idle(ctx);
		hle_jump(ctx, 0x0086EA);
		return;
	}
	pop_d0_rts(ctx);
	ctx->cycles += 14 + 8;            /* cmp + beq.s not taken, then the pop and rts */
}

/* $0086EC beq.s (only reachable from a savestate stopped between the two instructions) */
HLE_FUNC_GROUP(0x0086EC, nat_WaitVBlankBeq, 10, "native")
{
	if (ctx->sr & CC_Z) {
		ctx->cycles = 10;
		hle_jump(ctx, 0x0086EA);
		return;
	}
	pop_d0_rts(ctx);
	ctx->cycles += 8;
}

HLE_FUNC_GROUP(0x0086F0, nat_WaitVBlankEnd, 24, "native") { pop_d0_rts(ctx); }

/* $0078A6 FramePaceWait: spin while the target frame is ahead of the counter (d0 = frame target) */
static bool gt(hle_ctx *c)
{
	bool n = (c->sr & CC_N) != 0, v = (c->sr & CC_V) != 0, z = (c->sr & CC_Z) != 0;
	return !z && n == v;
}

HLE_WAIT_FUNC_GROUP(0x0078A6, nat_FramePaceWait, 24, "native")
{
	cmp16(ctx, (uint16_t)ctx->d[0], RAM16(VBLANK_COUNT));
	if (gt(ctx)) {
		hle_idle(ctx);
		hle_jump(ctx, 0x0078A6);
		return;
	}
	ctx->cycles = 14 + 8;
	hle_jump(ctx, 0x0078AE);
}

/* $0078AC bgt.s (savestate stopped between the two instructions) */
HLE_FUNC_GROUP(0x0078AC, nat_FramePaceBgt, 10, "native")
{
	if (gt(ctx)) {
		ctx->cycles = 10;
		hle_jump(ctx, 0x0078A6);
		return;
	}
	ctx->cycles = 8;
	hle_jump(ctx, 0x0078AE);
}
