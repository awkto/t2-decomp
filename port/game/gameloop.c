/* The per-frame driver: GameLoop $00765E (docs/subsystems/mainloop.md), its pause check
 * GameLoopPauseCheck $007676 and the pause loop, and ClearObjectPools $008B3C.
 *
 * GameLoop is never called with a `jsr`: it is entered by falling out of NextLevel and by its own
 * `bra.w GameLoop`, and every call inside it (PalCycle, WaitVBlank, ScriptTick, ScrollRows, the
 * BgStream*, ObjRunAll, VdpListRun ...) is a `jsr` that must keep its own HLE entry/decline logic and
 * its own interrupt timing. So the frame is registered as HLE_RANGE_PURE pieces that cover only
 * GameLoop's OWN instructions: each piece runs from an entry (the loop top, or the return address of a
 * `jsr`) to the next `jsr` (or LevelEnd, or the loop top) and leaves with hle_jump to that instruction,
 * which the real 68k then executes (so every callee is dispatched exactly as in the original, nested
 * or declined on its own). The pieces only touch work RAM and registers, so the layer measures their
 * real length with a dry run and declines one when a VBlank/ExtInt would fall inside it: each is a few
 * dozen cycles, so VBlank always arrives in the 68k code between them or in the wait.
 *
 * FramePaceWait $0078A6 stays entirely 68k: the wait for vblank_count (`cmp.w ; bgt.s`, and the
 * sign-wrap `jsr WaitVBlank`) is a real 68k loop, so VBlank interrupts arrive in it exactly as in the
 * original. The piece before it ($007890, the compare of frame_target with vblank_count) stops at the
 * loop / the jsr, and the pieces after it ($0078A4 / $0078AE) resume at the first instruction after
 * the wait (the hook fires there when the loop falls out). The pacing (no catch-up, VdpListRun after
 * the wait) is untouched.
 *
 * The pause loop ($0076E8..$007780) is the same scheme around its WaitVBlank / ReadInput / sound calls.
 *
 * ClearObjectPools is one HLE_FUNC_PURE (two 2944/763-word clear loops and a 64-word dbra loop, about
 * 105000 cycles: the layer declines it when an interrupt or the frame end falls inside, which is
 * almost always, so it mostly runs as 68k; it is exact when it does run).
 *
 * Generated instruction by instruction from the disassembly; every charge is the GPGX cost of the
 * opcode word (tools/py/costcheck.py checks them) and every register and flag is mirrored. */
#include "hle.h"

#include <stdint.h>

enum { OP_ADD, OP_SUB };

static uint32_t alu(hle_ctx *c, int op, uint32_t a, uint32_t b, int bits, int setx)
{
	uint32_t m = (bits == 32) ? 0xFFFFFFFFu : ((1u << bits) - 1), sg = 1u << (bits - 1);
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

static int c_z(hle_ctx *c) { return (c->sr & CC_Z) != 0; }
static int c_n(hle_ctx *c) { return (c->sr & CC_N) != 0; }
static int c_c(hle_ctx *c) { return (c->sr & CC_C) != 0; }

/* Bcc.s / Bcc.w: charge, return whether taken */
static int bs(hle_ctx *c, int taken) { hle_insn(c, taken ? 10 : 8); return taken; }
static int bw(hle_ctx *c, int taken) { hle_insn(c, taken ? 10 : 12); return taken; }

/* btst #bit,Dn: Z = bit clear */
static void btst_d(hle_ctx *c, int r, int bit) { cc_set(c, CC_Z, (c->d[r] >> bit) & 1 ? 0 : CC_Z); }

#define LEVEL        0xFF80CCu
#define CONTROL_MODE 0xFF9DF5u
#define LEVEL_EXIT   0xFFA234u
#define VBLANK       0xFFA0EEu
#define FRAME_TARGET 0xFFA0AEu
#define SAVED_VBLANK 0xFFA238u
#define PAL_CYCLE_ON 0xFFA0A4u
#define CHEAT_SKIP   0xFFA0A6u
#define SFX_REQUEST  0xFF9EA4u
#define P1           0xFFFF8000u
#define P2           0xFFFF8050u
#define P1_GUNPOWER  0xFF801Cu
#define P2_GUNPOWER  0xFF806Cu

#define LEVEL_END 0x0078D0u
#define LOOP_TOP  0x00765Eu

/* every place a piece can stop: the next jsr (executed by the 68k), the 68k wait loop at
 * FramePaceWait, LevelEnd and the loop top */
#define GL_EXITS \
	0x007666, 0x0076E2, 0x007706, 0x00770E, 0x007714, 0x007732, 0x007776, 0x0077A4, 0x0077DC, \
	0x0077FA, 0x007812, 0x007820, 0x00782E, 0x00783C, 0x00784A, 0x00784E, 0x0078A0, 0x0078A6, \
	0x0078BC, LEVEL_END, LOOP_TOP

static void p_766A(hle_ctx *ctx);
static void p_7676(hle_ctx *ctx);
static void p_778A(hle_ctx *ctx);
static void p_77B0(hle_ctx *ctx);
static void p_7768(hle_ctx *ctx);
static void p_7774(hle_ctx *ctx);
static void p_78AE(hle_ctx *ctx);
static void p_76A8(hle_ctx *ctx);
static void p_76CE(hle_ctx *ctx);

/* ---- top of the frame: tst.b pal_cycle_on ; beq.s ; jsr PalCycle */
static void p_765E(hle_ctx *ctx)
{
	uint8_t v = RAM8(PAL_CYCLE_ON);
	cc_logic8(ctx, v);
	hle_insn(ctx, 16);                                          /* 00765E tst.b (0xffffa0a4).l */
	if (bs(ctx, v == 0)) {                                      /* 007664 beq.s 0x766a */
		p_766A(ctx);
		return;
	}
	hle_jump(ctx, 0x007666);                                    /* jsr PalCycle (68k) */
}

static void p_766A(hle_ctx *ctx)
{
	uint16_t v = RAM16(LEVEL);
	alu(ctx, OP_SUB, v, 0x0A, 16, 0);
	hle_insn(ctx, 20);                                          /* 00766A cmpi.w #10,(0xffff80cc).l */
	if (bw(ctx, c_z(ctx))) {                                    /* 007672 beq.w 0x778a */
		p_778A(ctx);
		return;
	}
	p_7676(ctx);
}

/* ---- GameLoopPauseCheck $007676: Start edge of a playing player */
static void p_7676(hle_ctx *ctx)
{
	ctx->a[3] = P1;
	hle_insn(ctx, 12);                                          /* 007676 lea.l (0xffff8000).l,a3 */
	set_w(&ctx->d[0], RAM16(ctx->a[3]));
	cc_logic16(ctx, ctx->d[0]);
	hle_insn(ctx, 12);                                          /* 00767C move.w 0(a3),d0 */
	btst_d(ctx, 0, 7);
	hle_insn(ctx, 10);                                          /* 007680 btst.b #7,d0 */
	if (bw(ctx, c_z(ctx))) {                                    /* 007684 beq.w 0x76a8 */
		p_76A8(ctx);
		return;
	}
	set_w(&ctx->d[1], RAM16(ctx->a[3] + 2));
	cc_logic16(ctx, ctx->d[1]);
	hle_insn(ctx, 12);                                          /* 007688 move.w 2(a3),d1 */
	btst_d(ctx, 1, 7);
	hle_insn(ctx, 10);                                          /* 00768C btst.b #7,d1 */
	if (bw(ctx, !c_z(ctx))) {                                   /* 007690 bne.w 0x76a8 */
		p_76A8(ctx);
		return;
	}
	alu(ctx, OP_SUB, RAM8(CONTROL_MODE), 7, 8, 0);
	hle_insn(ctx, 20);                                          /* 007694 cmpi.b #7,(0xffff9df5).l */
	if (bw(ctx, c_z(ctx))) {                                    /* 00769C beq.w 0x76ce */
		p_76CE(ctx);
		return;
	}
	uint16_t t = RAM16(ctx->a[3] + 0x24);
	cc_logic16(ctx, t);
	hle_insn(ctx, 12);                                          /* 0076A0 tst.w 36(a3) */
	if (bs(ctx, c_n(ctx))) {                                    /* 0076A4 bmi.s 0x76a8 */
		p_76A8(ctx);
		return;
	}
	hle_insn(ctx, 10);                                          /* 0076A6 bra.s 0x76ce */
	p_76CE(ctx);
}

static void p_76A8(hle_ctx *ctx)
{
	ctx->a[3] = P2;
	hle_insn(ctx, 12);                                          /* 0076A8 lea.l (0xffff8050).l,a3 */
	set_w(&ctx->d[0], RAM16(ctx->a[3]));
	cc_logic16(ctx, ctx->d[0]);
	hle_insn(ctx, 12);                                          /* 0076AE move.w 0(a3),d0 */
	btst_d(ctx, 0, 7);
	hle_insn(ctx, 10);                                          /* 0076B2 btst.b #7,d0 */
	if (bw(ctx, c_z(ctx))) {                                    /* 0076B6 beq.w 0x778a */
		p_778A(ctx);
		return;
	}
	set_w(&ctx->d[1], RAM16(ctx->a[3] + 2));
	cc_logic16(ctx, ctx->d[1]);
	hle_insn(ctx, 12);                                          /* 0076BA move.w 2(a3),d1 */
	btst_d(ctx, 1, 7);
	hle_insn(ctx, 10);                                          /* 0076BE btst.b #7,d1 */
	if (bw(ctx, !c_z(ctx))) {                                   /* 0076C2 bne.w 0x778a */
		p_778A(ctx);
		return;
	}
	uint16_t t = RAM16(ctx->a[3] + 0x24);
	cc_logic16(ctx, t);
	hle_insn(ctx, 12);                                          /* 0076C6 tst.w 36(a3) */
	if (bw(ctx, c_n(ctx))) {                                    /* 0076CA bmi.w 0x778a */
		p_778A(ctx);
		return;
	}
	p_76CE(ctx);
}

static void p_76CE(hle_ctx *ctx)
{
	alu(ctx, OP_SUB, RAM16(ctx->a[3] + 0x22), 7, 16, 0);
	hle_insn(ctx, 16);                                          /* 0076CE cmpi.w #7,34(a3) */
	if (bw(ctx, c_z(ctx))) {                                    /* 0076D4 beq.w 0x778a */
		p_778A(ctx);
		return;
	}
	uint16_t v = RAM16(VBLANK);
	RAM16(SAVED_VBLANK) = v;
	cc_logic16(ctx, v);
	hle_insn(ctx, 28);                                          /* 0076D8 move.w (0xffffa0ee).l,(0xffffa238).l */
	hle_jump(ctx, 0x0076E2);                                    /* jsr WaitVBlank (68k) */
}

/* ---- after the pause WaitVBlank: demo interrupt, else sound pause + "PAUSE" */
static void p_76E8(hle_ctx *ctx)
{
	alu(ctx, OP_SUB, RAM8(CONTROL_MODE), 7, 8, 0);
	hle_insn(ctx, 20);                                          /* 0076E8 cmpi.b #7,(0xffff9df5).l */
	if (bs(ctx, !c_z(ctx))) {                                   /* 0076F0 bne.s 0x7706 */
		hle_jump(ctx, 0x007706);                                /* jsr sub_0FBAD2 (68k) */
		return;
	}
	RAM8(LEVEL_EXIT) = 0xFF;
	cc_logic8(ctx, 0xFF);
	hle_insn(ctx, 20);                                          /* 0076F2 move.b #-1,(0xffffa234).l */
	RAM8(CONTROL_MODE) = 8;
	cc_logic8(ctx, 8);
	hle_insn(ctx, 20);                                          /* 0076FA move.b #8,(0xffff9df5).l */
	hle_insn(ctx, 10);                                          /* 007702 bra.w LevelEnd */
	hle_jump(ctx, LEVEL_END);
}

static void p_770C(hle_ctx *ctx)
{
	ctx->a[4] = ctx->a[3];
	hle_insn(ctx, 4);                                           /* 00770C movea.l a3,a4 */
	hle_jump(ctx, 0x00770E);                                    /* jsr sub_007AFE (68k) */
}

/* ---- wait for Start release: after ReadInput ($00771A) */
static void p_771E(hle_ctx *ctx)
{
	set_w(&ctx->d[0], RAM16(ctx->a[3]));
	cc_logic16(ctx, ctx->d[0]);
	hle_insn(ctx, 12);                                          /* 00771E move.w 0(a3),d0 */
	set_w(&ctx->d[1], RAM16(ctx->a[3] + 2));
	cc_logic16(ctx, ctx->d[1]);
	hle_insn(ctx, 12);                                          /* 007722 move.w 2(a3),d1 */
	btst_d(ctx, 0, 7);
	hle_insn(ctx, 10);                                          /* 007726 btst.b #7,d0 */
	if (bs(ctx, !c_z(ctx))) {                                   /* 00772A bne.s 0x7714 */
		hle_jump(ctx, 0x007714);
		return;
	}
	btst_d(ctx, 1, 7);
	hle_insn(ctx, 10);                                          /* 00772C btst.b #7,d1 */
	if (bs(ctx, c_z(ctx))) {                                    /* 007730 beq.s 0x7714 */
		hle_jump(ctx, 0x007714);
		return;
	}
	hle_jump(ctx, 0x007732);                                    /* jsr WaitVBlank (68k) */
}

/* ---- paused: after ReadInput ($007738) */
static void p_773C(hle_ctx *ctx)
{
	set_w(&ctx->d[0], RAM16(ctx->a[3]));
	cc_logic16(ctx, ctx->d[0]);
	hle_insn(ctx, 12);                                          /* 00773C move.w 0(a3),d0 */
	set_w(&ctx->d[1], RAM16(ctx->a[3] + 2));
	cc_logic16(ctx, ctx->d[1]);
	hle_insn(ctx, 12);                                          /* 007740 move.w 2(a3),d1 */
	uint8_t cheat = RAM8(CHEAT_SKIP);
	cc_logic8(ctx, cheat);
	hle_insn(ctx, 16);                                          /* 007744 tst.b (0xffffa0a6).l */
	if (bs(ctx, c_z(ctx))) {                                    /* 00774A beq.s 0x7768 */
		p_7768(ctx);
		return;
	}
	btst_d(ctx, 0, 5);
	hle_insn(ctx, 10);                                          /* 00774C btst.b #5,d0 */
	if (bs(ctx, c_z(ctx))) {                                    /* 007750 beq.s 0x7768 */
		p_7768(ctx);
		return;
	}
	btst_d(ctx, 0, 4);
	hle_insn(ctx, 10);                                          /* 007752 btst.b #4,d0 */
	if (bs(ctx, c_z(ctx))) {                                    /* 007756 beq.s 0x7768 */
		p_7768(ctx);
		return;
	}
	btst_d(ctx, 0, 7);
	hle_insn(ctx, 10);                                          /* 007758 btst.b #7,d0 */
	if (bs(ctx, !c_z(ctx))) {                                   /* 00775C bne.s 0x7768 */
		p_7768(ctx);
		return;
	}
	RAM8(LEVEL_EXIT) = 0xFF;
	cc_logic8(ctx, 0xFF);
	hle_insn(ctx, 20);                                          /* 00775E move.b #-1,(0xffffa234).l */
	hle_insn(ctx, 10);                                          /* 007766 bra.s 0x7774 */
	p_7774(ctx);
}

static void p_7768(hle_ctx *ctx)
{
	btst_d(ctx, 0, 7);
	hle_insn(ctx, 10);                                          /* 007768 btst.b #7,d0 */
	if (bs(ctx, c_z(ctx))) {                                    /* 00776C beq.s 0x7732 */
		hle_jump(ctx, 0x007732);
		return;
	}
	btst_d(ctx, 1, 7);
	hle_insn(ctx, 10);                                          /* 00776E btst.b #7,d1 */
	if (bs(ctx, !c_z(ctx))) {                                   /* 007772 bne.s 0x7732 */
		hle_jump(ctx, 0x007732);
		return;
	}
	p_7774(ctx);
}

static void p_7774(hle_ctx *ctx)
{
	ctx->a[4] = ctx->a[3];
	hle_insn(ctx, 4);                                           /* 007774 movea.l a3,a4 */
	hle_jump(ctx, 0x007776);                                    /* jsr sub_002B5C (68k) */
}

/* ---- after sub_0FBAE2: restore vblank_count */
static void p_7780(hle_ctx *ctx)
{
	uint16_t v = RAM16(SAVED_VBLANK);
	RAM16(VBLANK) = v;
	cc_logic16(ctx, v);
	hle_insn(ctx, 28);                                          /* 007780 move.w (0xffffa238).l,(0xffffa0ee).l */
	p_778A(ctx);
}

/* ---- sfx request, gunpower, up to the first HUD jsr */
static void p_778A(hle_ctx *ctx)
{
	uint16_t sfx = RAM16(SFX_REQUEST);
	set_w(&ctx->d[0], sfx);
	cc_logic16(ctx, sfx);
	hle_insn(ctx, 16);                                          /* 00778A move.w (0xffff9ea4).l,d0 */
	if (bs(ctx, c_z(ctx))) {                                    /* 007790 beq.s 0x77b0 */
		p_77B0(ctx);
		return;
	}
	uint16_t busy = RAM16(0xFF0026u);
	set_w(&ctx->d[1], busy);
	cc_logic16(ctx, busy);
	hle_insn(ctx, 16);                                          /* 007792 move.w (0xffff0026).l,d1 */
	if (bs(ctx, !c_z(ctx))) {                                   /* 007798 bne.s 0x77b0 */
		p_77B0(ctx);
		return;
	}
	alu(ctx, OP_SUB, RAM16(LEVEL), 0x0A, 16, 0);
	hle_insn(ctx, 20);                                          /* 00779A cmpi.w #10,(0xffff80cc).l */
	if (bs(ctx, c_z(ctx))) {                                    /* 0077A2 beq.s 0x77b0 */
		p_77B0(ctx);
		return;
	}
	hle_jump(ctx, 0x0077A4);                                    /* jsr sub_013D04 (68k) */
}

static void p_77AA(hle_ctx *ctx)
{
	RAM16(SFX_REQUEST) = 0;
	cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	hle_insn(ctx, 20);                                          /* 0077AA clr.w (0xffff9ea4).l */
	p_77B0(ctx);
}

static void p_77B0(hle_ctx *ctx)
{
	uint32_t d0 = RAM16(P1_GUNPOWER);
	set_w(&ctx->d[0], d0);
	cc_logic16(ctx, d0);
	hle_insn(ctx, 16);                                          /* 0077B0 move.w (0xffff801c).l,d0 */
	d0 = alu(ctx, OP_ADD, d0, 0x60, 16, 1);
	set_w(&ctx->d[0], d0);
	hle_insn(ctx, 8);                                           /* 0077B6 addi.w #96,d0 */
	if (!bs(ctx, !c_c(ctx))) {                                  /* 0077BA bcc.s 0x77c0 */
		set_w(&ctx->d[0], 0xFFFF);
		cc_logic16(ctx, 0xFFFF);
		hle_insn(ctx, 8);                                       /* 0077BC move.w #-1,d0 */
	}
	RAM16(P1_GUNPOWER) = (uint16_t)ctx->d[0];
	cc_logic16(ctx, ctx->d[0]);
	hle_insn(ctx, 16);                                          /* 0077C0 move.w d0,(0xffff801c).l */
	d0 = RAM16(P2_GUNPOWER);
	set_w(&ctx->d[0], d0);
	cc_logic16(ctx, d0);
	hle_insn(ctx, 16);                                          /* 0077C6 move.w (0xffff806c).l,d0 */
	d0 = alu(ctx, OP_ADD, d0, 0x60, 16, 1);
	set_w(&ctx->d[0], d0);
	hle_insn(ctx, 8);                                           /* 0077CC addi.w #96,d0 */
	if (!bs(ctx, !c_c(ctx))) {                                  /* 0077D0 bcc.s 0x77d6 */
		set_w(&ctx->d[0], 0xFFFF);
		cc_logic16(ctx, 0xFFFF);
		hle_insn(ctx, 8);                                       /* 0077D2 move.w #-1,d0 */
	}
	RAM16(P2_GUNPOWER) = (uint16_t)ctx->d[0];
	cc_logic16(ctx, ctx->d[0]);
	hle_insn(ctx, 16);                                          /* 0077D6 move.w d0,(0xffff806c).l */
	hle_jump(ctx, 0x0077DC);                                    /* jsr sub_007C9A (68k) */
}

/* ---- after the HUD calls: level_exit test */
static void p_77F0(hle_ctx *ctx)
{
	uint8_t v = RAM8(LEVEL_EXIT);
	cc_logic8(ctx, v);
	hle_insn(ctx, 16);                                          /* 0077F0 tst.b (0xffffa234).l */
	if (bw(ctx, !c_z(ctx))) {                                   /* 0077F6 bne.w LevelEnd */
		hle_jump(ctx, LEVEL_END);
		return;
	}
	hle_jump(ctx, 0x0077FA);                                    /* jsr VdpListReset (68k) */
}

/* ---- the background streamer chain: level 1, 2, 4, 5, 6 (cmpi.w #n,level ; bne.s next ; jsr) */
static void bg_chain(hle_ctx *ctx, int level, uint32_t jsr_addr, void (*next)(hle_ctx *), uint32_t after)
{
	alu(ctx, OP_SUB, RAM16(LEVEL), level, 16, 0);
	hle_insn(ctx, 20);                                          /* cmpi.w #n,(0xffff80cc).l */
	if (bs(ctx, !c_z(ctx))) {                                   /* bne.s next */
		if (next)
			next(ctx);
		else
			hle_jump(ctx, after);
		return;
	}
	hle_jump(ctx, jsr_addr);
}
static void p_7840(hle_ctx *ctx) { bg_chain(ctx, 6, 0x00784A, NULL, 0x00784E); }
static void p_7832(hle_ctx *ctx) { bg_chain(ctx, 5, 0x00783C, p_7840, 0); }
static void p_7824(hle_ctx *ctx) { bg_chain(ctx, 4, 0x00782E, p_7832, 0); }
static void p_7816(hle_ctx *ctx) { bg_chain(ctx, 2, 0x007820, p_7824, 0); }
static void p_7808(hle_ctx *ctx) { bg_chain(ctx, 1, 0x007812, p_7816, 0); }

/* ---- FramePaceWait entry: compare frame_target with vblank_count ($007890) */
static void p_7890(hle_ctx *ctx)
{
	uint16_t t = RAM16(FRAME_TARGET);
	set_w(&ctx->d[0], t);
	cc_logic16(ctx, t);
	hle_insn(ctx, 16);                                          /* 007890 move.w (0xffffa0ae).l,d0 */
	uint16_t v = RAM16(VBLANK);
	set_w(&ctx->d[1], v);
	cc_logic16(ctx, v);
	hle_insn(ctx, 16);                                          /* 007896 move.w (0xffffa0ee).l,d1 */
	uint16_t r = (uint16_t)(ctx->d[1] ^ ctx->d[0]);
	set_w(&ctx->d[1], r);
	cc_logic16(ctx, r);
	hle_insn(ctx, 4);                                           /* 00789C eor.w d0,d1 */
	if (bs(ctx, !c_n(ctx))) {                                   /* 00789E bpl.s FramePaceWait */
		hle_jump(ctx, 0x0078A6);                                /* the wait loop, 68k */
		return;
	}
	hle_jump(ctx, 0x0078A0);                                    /* jsr WaitVBlank (68k) */
}

/* ---- after the wait: frame_target = vblank_count + 1, then VdpListRun */
static void p_78AE(hle_ctx *ctx)
{
	uint16_t v = RAM16(VBLANK);
	set_w(&ctx->d[0], v);
	cc_logic16(ctx, v);
	hle_insn(ctx, 16);                                          /* 0078AE move.w (0xffffa0ee).l,d0 */
	uint32_t d0 = alu(ctx, OP_ADD, v, 1, 16, 1);
	set_w(&ctx->d[0], d0);
	hle_insn(ctx, 4);                                           /* 0078B4 addq.w #1,d0 */
	RAM16(FRAME_TARGET) = (uint16_t)d0;
	cc_logic16(ctx, d0);
	hle_insn(ctx, 16);                                          /* 0078B6 move.w d0,(0xffffa0ae).l */
	hle_jump(ctx, 0x0078BC);                                    /* jsr VdpListRun (68k) */
}

static void p_78A2(hle_ctx *ctx)
{
	hle_insn(ctx, 10);                                          /* 0078A4 bra.s 0x78ae */
	p_78AE(ctx);
}

static void p_78C2(hle_ctx *ctx)
{
	uint8_t v = RAM8(LEVEL_EXIT);
	cc_logic8(ctx, v);
	hle_insn(ctx, 16);                                          /* 0078C2 tst.b (0xffffa234).l */
	if (bw(ctx, !c_z(ctx))) {                                   /* 0078C8 bne.w LevelEnd */
		hle_jump(ctx, LEVEL_END);
		return;
	}
	hle_insn(ctx, 10);                                          /* 0078CC bra.w GameLoop */
	hle_jump(ctx, LOOP_TOP);
}

HLE_RANGE_PURE(0x00765E, GameLoop, 60, GL_EXITS)             { p_765E(ctx); }
HLE_RANGE_PURE(0x00766A, GameLoopAfterPal, 60, GL_EXITS)     { p_766A(ctx); }
HLE_RANGE_PURE(0x007676, GameLoopPauseCheck, 250, GL_EXITS)  { p_7676(ctx); }
HLE_RANGE_PURE(0x0076E8, GameLoopPauseDemo, 80, GL_EXITS)    { p_76E8(ctx); }
HLE_RANGE_PURE(0x00770C, GameLoopPauseHud, 10, GL_EXITS)     { p_770C(ctx); }
HLE_RANGE_PURE(0x00771E, GameLoopPauseRel, 60, GL_EXITS)     { p_771E(ctx); }
HLE_RANGE_PURE(0x00773C, GameLoopPauseWait, 150, GL_EXITS)   { p_773C(ctx); }
HLE_RANGE_PURE(0x007780, GameLoopPauseEnd, 60, GL_EXITS)     { p_7780(ctx); }
HLE_RANGE_PURE(0x00778A, GameLoopSfx, 150, GL_EXITS)         { p_778A(ctx); }
HLE_RANGE_PURE(0x0077AA, GameLoopGunpower, 140, GL_EXITS)    { p_77AA(ctx); }
HLE_RANGE_PURE(0x0077F0, GameLoopHudDone, 40, GL_EXITS)      { p_77F0(ctx); }
HLE_RANGE_PURE(0x007808, GameLoopBg1, 150, GL_EXITS)         { p_7808(ctx); }
HLE_RANGE_PURE(0x007816, GameLoopBg2, 130, GL_EXITS)         { p_7816(ctx); }
HLE_RANGE_PURE(0x007824, GameLoopBg4, 110, GL_EXITS)         { p_7824(ctx); }
HLE_RANGE_PURE(0x007832, GameLoopBg5, 90, GL_EXITS)          { p_7832(ctx); }
HLE_RANGE_PURE(0x007840, GameLoopBg6, 60, GL_EXITS)          { p_7840(ctx); }
HLE_RANGE_PURE(0x007890, FramePaceCheck, 60, GL_EXITS)       { p_7890(ctx); }
HLE_RANGE_PURE(0x0078A4, FramePaceSkip, 80, GL_EXITS)        { p_78A2(ctx); }
HLE_RANGE_PURE(0x0078AE, FramePaceDone, 40, GL_EXITS)        { p_78AE(ctx); }
HLE_RANGE_PURE(0x0078C2, GameLoopEnd, 40, GL_EXITS)          { p_78C2(ctx); }

/* ------------------------------------------------------------------ ClearObjectPools $008B3C */
HLE_FUNC_PURE(0x008B3C, ClearObjectPools, 105000)
{
	static const uint32_t b[] = { 0xFFA0A4, 0xFF9EBE, 0xFF9EC0, 0xFF80B4 };
	for (int i = 0; i < 4; i++) {
		RAM8(b[i]) = 0;
		hle_insn(ctx, 20);                                      /* 008B3C clr.b (abs).l x4 */
	}
	hle_insn(ctx, 12);                                          /* 008B54 lea.l (0xffff80f2).l,a0 */
	hle_insn(ctx, 12);                                          /* 008B5A lea.l (0xffff9df2).l,a1 */
	uint32_t p = 0xFF80F2, e = 0xFF9DF2;
	do {                                                        /* 008B60 clr.w (a0)+ ; cmpa.l a0,a1 ; bne.s */
		RAM16(p) = 0;
		p += 2;
		hle_insn(ctx, 12);                                      /* 008B60 clr.w (a0)+ */
		hle_insn(ctx, 6);                                       /* 008B62 cmpa.l a0,a1 */
		hle_insn(ctx, p != e ? 10 : 8);                         /* 008B64 bne.s 0x8b60 */
	} while (p != e);
	hle_insn(ctx, 12);                                          /* 008B66 lea.l (0xffffae5c).l,a0 */
	hle_insn(ctx, 12);                                          /* 008B6C lea.l (0xffffb452).l,a1 */
	p = 0xFFAE5C; e = 0xFFB452;
	do {                                                        /* 008B72 clr.w (a0)+ ; cmpa.l a0,a1 ; bne.s */
		RAM16(p) = 0;
		p += 2;
		hle_insn(ctx, 12);                                      /* 008B72 clr.w (a0)+ */
		hle_insn(ctx, 6);                                       /* 008B74 cmpa.l a0,a1 */
		hle_insn(ctx, p != e ? 10 : 8);                         /* 008B76 bne.s 0x8b72 */
	} while (p != e);
	ctx->a[0] = 0xFF000000u | p;
	ctx->a[1] = 0xFF000000u | e;
	hle_insn(ctx, 12);                                          /* 008B78 lea.l (0xffffafd2).l,a3 */
	hle_insn(ctx, 8);                                           /* 008B7E move.w #63,d0 */
	p = 0xFFAFD2;
	for (int i = 63; i >= 0; i--) {                             /* 008B82 clr.w (a3)+ ; dbf d0 */
		RAM16(p) = 0;
		p += 2;
		hle_insn(ctx, 12);                                      /* 008B82 clr.w (a3)+ */
		hle_insn(ctx, i ? 10 : 14);                             /* 008B84 dbf d0,0x8b82 */
	}
	ctx->a[3] = 0xFF000000u | p;
	set_w(&ctx->d[0], 0xFFFF);
	static const uint32_t w[] = { 0xFF80D6, 0xFF80D8, 0xFF80DA, 0xFF80DC, 0xFF80DE, 0xFF80E0 };
	for (int i = 0; i < 6; i++) {
		RAM16(w[i]) = 0;
		hle_insn(ctx, 20);                                      /* 008B88 clr.w (abs).l x6 */
	}
	RAM8(LEVEL_EXIT) = 0;
	cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	hle_insn(ctx, 20);                                          /* 008BAC clr.b (0xffffa234).l */
	hle_insn(ctx, 16);                                          /* 008BB2 rts */
}
