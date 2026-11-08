/* Text and HUD printing: VramWriteWord $0087C4 (VRAM[d2] = d0.w), the BCD digit printer sub_007D0E, the
 * HUD score and lives printers sub_007C9A / sub_007CD8, the string printer sub_007EB2 and the text
 * printer sub_007E08 (tile base $FF9E2C, newline and colour codes). docs/porting.md, "Text printing".
 *
 * These write the VDP ports, so they use the exact clock model of game/vdplist.c (see there): every
 * original instruction advances the real CPU clock in the core's order (bus refresh check, the access,
 * the cost), the data/control writes are timed (FIFO stalls), and the function runs inside an exact
 * session. The timing guard `safe_run` leaves a call to the real 68k when a line-start event (VBlank,
 * frame wrap, H-Int, light gun, DMA) could fall inside it; the bound only has to be an upper bound of
 * the call's duration (a loose one just means the 68k runs it). VramWriteWord is a static C body (`vww`,
 * the `jsr` charged by the caller, ending with the `rts` charge) used by the printers directly and
 * registered for every other caller: the 68k, and C callers that reach it with hle_call68k
 * (vdplist.c command 3, player.c PrintText), where the nested dispatch lands here.
 *
 * Registers and flags are mirrored instruction by instruction (every width, the final CCR of the last
 * flag setter). PrintText ($007DEE) falls into sub_007E08 on the 68k; it is C in player.c and keeps its
 * own timeline. sub_007D0E has no leading-zero blanking in the ROM: every digit nibble is printed as
 * tile $1A + digit + base.
 */
#include "hle.h"
#include "gx.h"

#include <stdint.h>

#define TILE_BASE 0xFFFF9E2Cu

enum { OP_ADD, OP_SUB };

/* a (op) b at `bits` width with the 68k flags (X set as well when setx) */
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

static __attribute__((unused)) int c_z(hle_ctx *c) { return (c->sr & CC_Z) != 0; }
static __attribute__((unused)) int c_n(hle_ctx *c) { return (c->sr & CC_N) != 0; }
static __attribute__((unused)) int c_c(hle_ctx *c) { return (c->sr & CC_C) != 0; }
static __attribute__((unused)) int c_v(hle_ctx *c) { return (c->sr & CC_V) != 0; }
static __attribute__((unused)) int c_ge(hle_ctx *c) { return c_n(c) == c_v(c); }
static __attribute__((unused)) int c_le(hle_ctx *c) { return c_z(c) || c_n(c) != c_v(c); }
static __attribute__((unused)) int c_gt(hle_ctx *c) { return !c_z(c) && c_n(c) == c_v(c); }

/* work RAM or ROM by address */
static inline int is_ram(uint32_t a) { return (a & 0xFFFFFFu) >= 0xE00000u; }
static inline uint8_t ld8(uint32_t a) { return is_ram(a) ? RAM8(a) : ROM8(a); }
static inline uint16_t ld16(uint32_t a) { return is_ram(a) ? RAM16(a) : ROM16(a); }
static inline void st16(uint32_t a, uint32_t v) { RAM16(a) = (uint16_t)v; }

/* ---------------------------------------------------------------- exact clock model (as vdplist.c) */
static void pre(void)
{
	gx_vdp_follow_cpu_lines();
	int c = gx_m68k_cycles(), r = gx_m68k_refresh_cycles();
	if (c >= r) {
		gx_m68k_set_refresh_cycles(c + 128 * 7);
		gx_m68k_set_cycles(c + 2 * 7);
	}
}
static void post(int c) { gx_m68k_set_cycles(gx_m68k_cycles() + c * 7); }
static void tick(int c) { pre(); post(c); }
static void wr_ctrl(uint16_t v, int c) { pre(); vdp_ctrl_w(v); post(c); }
static void wr_data(uint16_t v, int c)
{
	pre();
	hle_vdp_timed(1);
	vdp_data_w(v);
	hle_vdp_timed(0);
	post(c);
}
static void sess_begin(void)
{
	hle_dma_begin();
	hle_exact_session();
}
static void sess_finish(void) { hle_dma_end(); }

static int safe_run(int bound_cycles)
{
	if (hle_chk_on)
		hle_chk_note(bound_cycles);
	if (hle_post_access)
		return 1;
	if (gx_dma_length())
		return 0;
	long left = (long)gx_vdp_next_toggle_clock() - gx_m68k_cycles();
	return left > (long)bound_cycles * 7 + 3420;
}

/* conditional branches: charge, return whether taken */
static int bs(hle_ctx *c, int taken) { (void)c; tick(taken ? 10 : 8); return taken; }   /* Bcc.s */
static int bw(hle_ctx *c, int taken) { (void)c; tick(taken ? 10 : 12); return taken; }   /* Bcc.w */

/* per character upper bound (cycles): ~100 own instructions + VramWriteWord ~100 + FIFO stalls */
#define CHAR_COST 320

/* ---------------------------------------------------------------- VramWriteWord $0087C4 */
/* entered after the caller's `jsr` was charged; ends with the rts charge */
static void vww(hle_ctx *ctx)
{
	ctx->a[5] = 0xC00000u;                                        /* 0087C4 lea (0xc00000).l,a5 */
	tick(12);
	uint16_t addr = (uint16_t)ctx->d[2];
	set_w(&ctx->d[1], addr);                                      /* 0087CA move.w d2,d1 */
	tick(4);
	set_w(&ctx->d[2], addr & 0x3FFF);                             /* 0087CC andi.w #$3fff,d2 */
	tick(8);
	uint16_t d2 = (uint16_t)((addr & 0x3FFF) | 0x4000);
	set_w(&ctx->d[2], d2);                                        /* 0087D0 ori.w #$4000,d2 */
	tick(8);
	uint16_t d1 = (uint16_t)((addr << 2) | (addr >> 14));
	set_w(&ctx->d[1], d1);                                        /* 0087D4 rol.w #2,d1 */
	tick(10);
	d1 &= 3;
	set_w(&ctx->d[1], d1);                                        /* 0087D6 andi.w #3,d1 */
	tick(8);
	wr_ctrl(d2, 12);                                              /* 0087DA move.w d2,(4,a5) */
	wr_ctrl(d1, 12);                                              /* 0087DE move.w d1,(4,a5) */
	uint16_t d0 = (uint16_t)ctx->d[0];
	wr_data(d0, 8);                                               /* 0087E2 move.w d0,(a5) */
	cc_logic16(ctx, d0);
	tick(16);                                                     /* 0087E4 rts */
}

HLE_FUNC(0x0087C4, VramWriteWord, 0)
{
	if (!safe_run(400)) {
		hle_decline();
		return;
	}
	sess_begin();
	vww(ctx);
	sess_finish();
}

/* ---------------------------------------------------------------- sub_007D0E digit printer */
/* (a0)+ BCD bytes, d7+1 of them, two tiles each at VRAM d3 (+2 per tile), d6 = palette/priority bits.
 * Entered after the caller's jsr/jmp/bra was charged; ends with the rts charge. */
static void p7d0e(hle_ctx *ctx)
{
	ctx->d[4] = 0; cc_logic32(ctx, 0); tick(4);                   /* 007D0E moveq #0,d4 */
	for (;;) {
		/* high nibble */
		uint32_t v = ld8(ctx->a[0]); ctx->a[0] += 1;
		set_b(&ctx->d[5], v); cc_logic8(ctx, v); tick(8);     /* 007D10 move.b (a0)+,d5 */
		v = ctx->d[5] & 0xFFFF;
		set_w(&ctx->d[0], v); cc_logic16(ctx, v); tick(4);    /* 007D12 move.w d5,d0 */
		v = lsr16(ctx, ctx->d[0] & 0xFFFF, 4);
		set_w(&ctx->d[0], v); tick(14);                       /* 007D14 lsr.w #4,d0 */
		v &= 0xF;
		set_w(&ctx->d[0], v); cc_logic16(ctx, v); tick(8);    /* 007D16 andi.w #$f,d0 */
		if (bs(ctx, v != 0) == 0) {                           /* 007D1A bne.s 7D1E */
			cc_logic16(ctx, ctx->d[4] & 0xFFFF); tick(4); /* 007D1C tst.w d4 */
		}
		v = alu(ctx, OP_ADD, ctx->d[4] & 0xFFFF, 1, 16, 1);
		set_w(&ctx->d[4], v); tick(4);                        /* 007D1E addq.w #1,d4 */
		v = alu(ctx, OP_ADD, ctx->d[0] & 0xFFFF, 0x1A, 16, 1);
		set_w(&ctx->d[0], v); tick(8);                        /* 007D20 addi.w #$1a,d0 */
		v = alu(ctx, OP_ADD, ctx->d[0] & 0xFFFF, ld16(TILE_BASE), 16, 1);
		set_w(&ctx->d[0], v); tick(16);                       /* 007D24 add.w ($ff9e2c).l,d0 */
		v = (ctx->d[0] | ctx->d[6]) & 0xFFFF;
		set_w(&ctx->d[0], v); cc_logic16(ctx, v); tick(4);    /* 007D2A or.w d6,d0 */
		v = ctx->d[3] & 0xFFFF;
		set_w(&ctx->d[2], v); cc_logic16(ctx, v); tick(4);    /* 007D2C move.w d3,d2 */
		tick(18);                                             /* 007D2E jsr (VramWriteWord,pc) */
		vww(ctx);
		v = alu(ctx, OP_ADD, ctx->d[3] & 0xFFFF, 2, 16, 1);
		set_w(&ctx->d[3], v); tick(4);                        /* 007D32 addq.w #2,d3 */
		/* low nibble */
		v = ctx->d[5] & 0xFFFF;
		set_w(&ctx->d[0], v); cc_logic16(ctx, v); tick(4);    /* 007D34 move.w d5,d0 */
		v &= 0xF;
		set_w(&ctx->d[0], v); cc_logic16(ctx, v); tick(8);    /* 007D36 andi.w #$f,d0 */
		if (bs(ctx, v != 0) == 0) {                           /* 007D3A bne.s 7D3E */
			cc_logic16(ctx, ctx->d[4] & 0xFFFF); tick(4); /* 007D3C tst.w d4 */
		}
		v = alu(ctx, OP_ADD, ctx->d[4] & 0xFFFF, 1, 16, 1);
		set_w(&ctx->d[4], v); tick(4);                        /* 007D3E addq.w #1,d4 */
		v = alu(ctx, OP_ADD, ctx->d[0] & 0xFFFF, 0x1A, 16, 1);
		set_w(&ctx->d[0], v); tick(8);                        /* 007D40 addi.w #$1a,d0 */
		v = alu(ctx, OP_ADD, ctx->d[0] & 0xFFFF, ld16(TILE_BASE), 16, 1);
		set_w(&ctx->d[0], v); tick(16);                       /* 007D44 add.w ($ff9e2c).l,d0 */
		v = (ctx->d[0] | ctx->d[6]) & 0xFFFF;
		set_w(&ctx->d[0], v); cc_logic16(ctx, v); tick(4);    /* 007D4A or.w d6,d0 */
		v = ctx->d[3] & 0xFFFF;
		set_w(&ctx->d[2], v); cc_logic16(ctx, v); tick(4);    /* 007D4C move.w d3,d2 */
		tick(18);                                             /* 007D4E jsr (VramWriteWord,pc) */
		vww(ctx);
		v = alu(ctx, OP_ADD, ctx->d[3] & 0xFFFF, 2, 16, 1);
		set_w(&ctx->d[3], v); tick(4);                        /* 007D52 addq.w #2,d3 */
		uint16_t c = (uint16_t)ctx->d[7] - 1;                 /* 007D54 dbra d7,7D10 */
		set_w(&ctx->d[7], c);
		if (c != 0xFFFF) {
			tick(10);
			continue;
		}
		tick(14);
		break;
	}
	tick(16);                                                     /* 007D58 rts */
}

static int bound_7d0e(uint32_t d7)
{
	return 100 + ((int)(d7 & 0xFFFF) + 1) * 2 * CHAR_COST;
}

HLE_FUNC(0x007D0E, DigitPrint, 0)
{
	if (!safe_run(bound_7d0e(ctx->d[7]))) {
		hle_decline();
		return;
	}
	sess_begin();
	p7d0e(ctx);
	sess_finish();
}

/* ---------------------------------------------------------------- HUD score / lives printers */
/* tst.w of a work RAM word */
static void tstw(hle_ctx *ctx, uint32_t a) { cc_logic16(ctx, ld16(a)); tick(16); }
static void movew_d(hle_ctx *ctx, int r, uint32_t v) { set_w(&ctx->d[r], v); cc_logic16(ctx, v); tick(8); }
static void lea_a0(hle_ctx *ctx, uint32_t a) { ctx->a[0] = a; tick(12); }
static void moveq_d7(hle_ctx *ctx, uint32_t v) { ctx->d[7] = v; cc_logic32(ctx, v); tick(4); }

/* sub_007C9A: P1 and P2 score (2 BCD bytes each) */
HLE_FUNC(0x007C9A, HudScores, 0)
{
	if (!safe_run(2 * bound_7d0e(1) + 200)) {
		hle_decline();
		return;
	}
	sess_begin();
	tstw(ctx, 0xFFFF8024u);                                       /* 007C9A tst.w ($ffff8024).l */
	if (!bw(ctx, c_n(ctx))) {                                     /* 007CA0 bmi.w 7CB8 */
		movew_d(ctx, 3, 0xC084);                              /* 007CA4 move.w #$c084,d3 */
		lea_a0(ctx, 0xFFFF8018u);                             /* 007CA8 lea ($ffff8018).l,a0 */
		movew_d(ctx, 6, 0xC000);                              /* 007CAE move.w #$c000,d6 */
		moveq_d7(ctx, 1);                                     /* 007CB2 moveq #1,d7 */
		tick(18);                                             /* 007CB4 jsr (sub_007D0E,pc) */
		p7d0e(ctx);
	}
	tstw(ctx, 0xFFFF8074u);                                       /* 007CB8 tst.w ($ffff8074).l */
	if (bw(ctx, c_n(ctx))) {                                      /* 007CBE bmi.w 7CD6 */
		tick(16);                                             /* 007CD6 rts */
	} else {
		movew_d(ctx, 3, 0xC0B0);                              /* 007CC2 move.w #$c0b0,d3 */
		lea_a0(ctx, 0xFFFF8068u);                             /* 007CC6 lea ($ffff8068).l,a0 */
		movew_d(ctx, 6, 0xC000);                              /* 007CCC move.w #$c000,d6 */
		moveq_d7(ctx, 1);                                     /* 007CD0 moveq #1,d7 */
		tick(10);                                             /* 007CD2 jmp (sub_007D0E,pc) */
		p7d0e(ctx);
	}
	sess_finish();
}

/* sub_007CD8: P1 and P2 lives/credits (4 BCD bytes each) */
HLE_FUNC(0x007CD8, HudLives, 0)
{
	if (!safe_run(2 * bound_7d0e(3) + 200)) {
		hle_decline();
		return;
	}
	sess_begin();
	movew_d(ctx, 6, 0xC000);                                      /* 007CD8 move.w #$c000,d6 */
	tstw(ctx, 0xFFFF8024u);                                       /* 007CDC tst.w ($ffff8024).l */
	if (!bs(ctx, c_n(ctx))) {                                     /* 007CE2 bmi.s 7CF4 */
		movew_d(ctx, 3, 0xC092);                              /* 007CE4 move.w #$c092,d3 */
		lea_a0(ctx, 0xFFFF9EA8u);                             /* 007CE8 lea ($ffff9ea8).l,a0 */
		moveq_d7(ctx, 3);                                     /* 007CEE moveq #3,d7 */
		tick(18);                                             /* 007CF0 jsr (sub_007D0E,pc) */
		p7d0e(ctx);
	}
	tstw(ctx, 0xFFFF8074u);                                       /* 007CF4 tst.w ($ffff8074).l */
	if (bw(ctx, c_n(ctx))) {                                      /* 007CFA bmi.w 7D0C */
		tick(16);                                             /* 007D0C rts */
	} else {
		movew_d(ctx, 3, 0xC0BE);                              /* 007CFE move.w #$c0be,d3 */
		lea_a0(ctx, 0xFFFF9EB0u);                             /* 007D02 lea ($ffff9eb0).l,a0 */
		moveq_d7(ctx, 3);                                     /* 007D08 moveq #3,d7 */
		tick(10);                                             /* 007D0A bra.s sub_007D0E */
		p7d0e(ctx);
	}
	sess_finish();
}

/* ---------------------------------------------------------------- sub_007EB2 string printer */
/* bytes of (a0) up to the 0 terminator become tiles (byte + base | d6) at VRAM d3, +2 each */
static void p7eb2(hle_ctx *ctx)
{
	uint32_t v = ld16(TILE_BASE);
	set_w(&ctx->d[5], v); cc_logic16(ctx, v); tick(16);           /* 007EB2 move.w ($ff9e2c).l,d5 */
	for (;;) {
		v = ld8(ctx->a[0]); ctx->a[0] += 1;
		set_b(&ctx->d[0], v); cc_logic8(ctx, v); tick(8);     /* 007EB8 move.b (a0)+,d0 */
		v &= 0xFF;
		set_w(&ctx->d[0], v); cc_logic16(ctx, v); tick(8);    /* 007EBA andi.w #$ff,d0 */
		if (bs(ctx, v == 0))                                  /* 007EBE beq.s 7ED0 */
			break;
		v = alu(ctx, OP_ADD, ctx->d[0] & 0xFFFF, ctx->d[5] & 0xFFFF, 16, 1);
		set_w(&ctx->d[0], v); tick(4);                        /* 007EC0 add.w d5,d0 */
		v = (ctx->d[0] | ctx->d[6]) & 0xFFFF;
		set_w(&ctx->d[0], v); cc_logic16(ctx, v); tick(4);    /* 007EC2 or.w d6,d0 */
		v = ctx->d[3] & 0xFFFF;
		set_w(&ctx->d[2], v); cc_logic16(ctx, v); tick(4);    /* 007EC4 move.w d3,d2 */
		tick(20);                                             /* 007EC6 jsr (VramWriteWord).l */
		vww(ctx);
		v = alu(ctx, OP_ADD, ctx->d[3] & 0xFFFF, 2, 16, 1);
		set_w(&ctx->d[3], v); tick(4);                        /* 007ECC addq.w #2,d3 */
		tick(10);                                             /* 007ECE bra.s 7EB8 */
	}
	tick(16);                                                     /* 007ED0 rts */
}

/* length of the 0-terminated string at a (capped); the printers' bound */
static int str_len(uint32_t a)
{
	int n = 0;
	while (n < 4096 && ld8(a + (uint32_t)n) != 0)
		n++;
	return n;
}

HLE_FUNC(0x007EB2, StringPrint, 0)
{
	int n = str_len(ctx->a[0]);
	if (n >= 4096 || !safe_run(200 + (n + 1) * CHAR_COST)) {
		hle_decline();
		return;
	}
	sess_begin();
	p7eb2(ctx);
	sess_finish();
}

/* ---------------------------------------------------------------- sub_007E08 text printer */
static void p7e08(hle_ctx *ctx)
{
	{ uint32_t _v = ld16(0xFFFF9E2Cu); set_w(&ctx->d[5], _v); cc_logic16(ctx, _v); tick(16); } /* 007E08 movew 0xffff9e2c,%d5 */
L_007E0E:;
	{ uint32_t _p = ld8(ctx->a[0]); ctx->a[0] += 1; uint32_t _v = _p; set_b(&ctx->d[0], _v); cc_logic8(ctx, _v); tick(8); } /* 007E0E moveb %a0@+,%d0 */
	if (bs(ctx, !c_n(ctx))) goto L_007E24; /* 007E10 bpls 0x7e24 */
	{ uint32_t _v = ld16(0xFFFF80C0u); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); tick(16); } /* 007E12 movew 0xffff80c0,%d3 */
	{ uint32_t _s = 0x80u; uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[3], _r); tick(8); } /* 007E18 addiw #128,%d3 */
	{ uint32_t _v = (ctx->d[3] & 0xFFFFu); st16(0xFFFF80C0u, _v); cc_logic16(ctx, _v); tick(16); } /* 007E1C movew %d3,0xffff80c0 */
	tick(10); goto L_007E0E; /* 007E22 bras 0x7e0e */
L_007E24:;
	{ uint32_t _s = 0xFFu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); tick(8); } /* 007E24 andiw #255,%d0 */
	if (bw(ctx, c_z(ctx))) goto L_007EB0; /* 007E28 beqw 0x7eb0 */
	{ uint32_t _s = 0x41u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); tick(8); } /* 007E2C cmpiw #65,%d0 */
	if (bs(ctx, !c_ge(ctx))) goto L_007E3A; /* 007E30 blts 0x7e3a */
	{ uint32_t _s = 0x5Au; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); tick(8); } /* 007E32 cmpiw #90,%d0 */
	if (bw(ctx, c_le(ctx))) goto L_007E9A; /* 007E36 blew 0x7e9a */
L_007E3A:;
	{ uint32_t _s = 0xDu; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); tick(8); } /* 007E3A cmpiw #13,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_007E4A; /* 007E3E bnes 0x7e4a */
	{ uint32_t _s = 0xFF80u; uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[3], _r); tick(8); } /* 007E40 andiw #-128,%d3 */
	{ uint32_t _s = 0x80u; uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[3], _r); tick(8); } /* 007E44 addiw #128,%d3 */
	tick(10); goto L_007E0E; /* 007E48 bras 0x7e0e */
L_007E4A:;
	{ uint32_t _s = 0x39u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); tick(8); } /* 007E4A cmpiw #57,%d0 */
	if (bs(ctx, c_gt(ctx))) goto L_007E5C; /* 007E4E bgts 0x7e5c */
	{ uint32_t _s = 0x30u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); tick(8); } /* 007E50 cmpiw #48,%d0 */
	if (bs(ctx, !c_ge(ctx))) goto L_007E5C; /* 007E54 blts 0x7e5c */
	{ uint32_t _s = 0x16u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[0], _r); tick(8); } /* 007E56 subiw #22,%d0 */
	tick(10); goto L_007E9E; /* 007E5A bras 0x7e9e */
L_007E5C:;
	{ uint32_t _s = 0x20u; uint32_t _d = (ctx->d[0] & 0xFFu); alu(ctx, OP_SUB, _d, _s, 8, 0); tick(8); } /* 007E5C cmpib #32,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_007E68; /* 007E60 bnes 0x7e68 */
	{ uint32_t _v = 0x27u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); tick(8); } /* 007E62 movew #39,%d0 */
	tick(10); goto L_007E9E; /* 007E66 bras 0x7e9e */
L_007E68:;
	{ ctx->d[1] = 0xFu; cc_logic32(ctx, ctx->d[1]); tick(4); } /* 007E68 moveq #15,%d1 */
L_007E6A:;
	{ uint32_t _s = ld8((0x007E7Au + (uint32_t)((int32_t)(int16_t)ctx->d[1]))); uint32_t _d = (ctx->d[0] & 0xFFu); alu(ctx, OP_SUB, _d, _s, 8, 0); tick(14); } /* 007E6A cmpb %pc@(0x7e7a,%d1:w),%d0 */
	if (bs(ctx, c_z(ctx))) goto L_007E74; /* 007E6E beqs 0x7e74 */
	{ uint16_t _c = (uint16_t)ctx->d[1] - 1; set_w(&ctx->d[1], _c); if (_c != 0xFFFF) { tick(10); goto L_007E6A; } tick(14); } /* 007E70 dbf %d1,0x7e6a */
L_007E74:;
	{ uint32_t _v = ld8((0x007E8Au + (uint32_t)((int32_t)(int16_t)ctx->d[1]))); set_b(&ctx->d[0], _v); cc_logic8(ctx, _v); tick(14); } /* 007E74 moveb %pc@(0x7e8a,%d1:w),%d0 */
	tick(10); goto L_007E9E; /* 007E78 bras 0x7e9e */
L_007E9A:;
	{ uint32_t _s = 0x41u; uint32_t _d = (ctx->d[0] & 0xFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 8, 1); set_b(&ctx->d[0], _r); tick(8); } /* 007E9A subib #65,%d0 */
L_007E9E:;
	{ uint32_t _s = (ctx->d[5] & 0xFFFFu); uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); tick(4); } /* 007E9E addw %d5,%d0 */
	{ uint32_t _s = (ctx->d[6] & 0xFFFFu); uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d | _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); tick(4); } /* 007EA0 orw %d6,%d0 */
	{ uint32_t _v = (ctx->d[3] & 0xFFFFu); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); tick(4); } /* 007EA2 movew %d3,%d2 */
	{ tick(20); vww(ctx); } /* 007EA4 jsr 0x87c4 */
	{ uint32_t _s = 0x2u; uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[3], _r); tick(4); } /* 007EAA addqw #2,%d3 */
	tick(10); goto L_007E0E; /* 007EAC braw 0x7e0e */
L_007EB0:;
	tick(16); return; /* 007EB0 rts */
}

HLE_FUNC(0x007E08, TextPrint, 0)
{
	int n = str_len(ctx->a[0]);
	if (n >= 2048 || !safe_run(300 + (n + 1) * CHAR_COST)) {
		hle_decline();
		return;
	}
	sess_begin();
	p7e08(ctx);
	sess_finish();
}
