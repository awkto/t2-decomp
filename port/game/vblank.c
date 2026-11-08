/* The VBlank / external interrupt handlers and the 68k-side sound request routines.
 *
 *   VBlankInt $000308   level 6: Menacer button handshake on port 2 ($A10005), latch count copy,
 *                       vblank_count++, then `jsr sub_0FBBD0` (the sound engine tick) and rte
 *   ExtInt    $00039E   level 2 (Menacer TH): first TH per frame stores the HV counter in the ring
 *   sub_013D04          sound request (d0 = sound id, priority from dat_013D30): queues it with
 *                       sub_0FBB96 unless a request of higher priority is already waiting
 *   sub_013CC4          music request with priority check (sub_0FBBAE)
 *   sub_013D56          request through the command slots (sub_0FC27C stores them)
 *   sub_0FBAD2, sub_0FBAE2, sub_0FBB6C, sub_0FBB96, sub_0FBBAE, sub_0FC0FE, sub_0FC27C
 *                       the small work-RAM-only sound queue routines they use
 *
 * The sound engine tick (sub_0FBBD0 -> sub_0FBBFE ..., which bus-requests the Z80, writes Z80 RAM and
 * spins on BUSACK / the Z80 command byte) is in sound.c. VBlankInt is an HLE_RANGE from its entry to
 * right before `jsr sub_0FBBD0` ($000396): the real 68k runs the jsr (which the sound.c hook takes) and
 * the rte. Interrupt entry (stacking SR/PC, the mask) is done by the core before the range starts.
 *
 * VBlankInt and ExtInt reach hardware (I/O port 2 data, the VDP HV counter), so every such access
 * goes through hle_pre()/hle_post() with the original's instruction cost, and the dbra delay loops
 * are charged one iteration at a time (the 68k bus refresh falls between iterations). The sound
 * queue routines only touch work RAM ($FF0000.. is the sound driver's variable block) and are pure.
 *
 * Generated instruction by instruction from the disassembly; costs are the GPGX opcode costs
 * (tools/py/costcheck.py checks them). */
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
static int c_ne(hle_ctx *c) { return !c_z(c); }
/* blt: N != V */
static int c_lt(hle_ctx *c) { return ((c->sr & CC_N) != 0) != ((c->sr & CC_V) != 0); }
/* ble: Z or N != V */
static int c_le(hle_ctx *c) { return c_z(c) || c_lt(c); }

/* Bcc.s / Bcc.w: charge, return whether taken */
static int bs(hle_ctx *c, int taken) { hle_insn(c, taken ? 10 : 8); return taken; }
static int bw(hle_ctx *c, int taken) { hle_insn(c, taken ? 10 : 12); return taken; }

/* movem.w d1-d2,-(sp) / (sp)+,d1-d2 (the pop sign-extends) */
static void push_d1d2(hle_ctx *c)
{
	hle_insn(c, 16);
	c->a[7] -= 2; bus_w16(c->a[7], (uint16_t)c->d[2]);
	c->a[7] -= 2; bus_w16(c->a[7], (uint16_t)c->d[1]);
}
static void pop_d1d2(hle_ctx *c)
{
	hle_insn(c, 20);
	c->d[1] = (uint32_t)(int32_t)(int16_t)bus_r16(c->a[7]); c->a[7] += 2;
	c->d[2] = (uint32_t)(int32_t)(int16_t)bus_r16(c->a[7]); c->a[7] += 2;
}

#define DAT_013D30 0x013D30u
#define DAT_013D84 0x013D84u
/* byte at (dat, Dn.w): the d8(pc,Xn.w) index is the sign-extended low word */
static uint8_t tab(uint32_t base, uint32_t d) { return ROM8(base + (uint32_t)(int32_t)(int16_t)d); }

#define SND_A4     0xFF00A4u
#define SND_9EC4   0xFF9EC4u
#define SND_QUEUED 0xFF0038u   /* sound request id + 1 (sub_0FBB96) */
#define SND_0026   0xFF0026u
#define SND_0016   0xFF0016u

/* ---- sub_0FC0FE: clear.w $FF0090 / $FF008E */
static void s_0FC0FE(hle_ctx *c)
{
	hle_insn(c, 20); RAM16(0xFF0090) = 0; cc_logic16(c, 0);     /* 0FC0FE clr.w (0xffff0090).l */
	hle_insn(c, 20); RAM16(0xFF008E) = 0;                       /* 0FC104 clr.w (0xffff008e).l */
	hle_insn(c, 16);                                            /* 0FC10A rts */
}
HLE_FUNC_PURE(0x0FC0FE, Snd_0FC0FE, 56) { s_0FC0FE(ctx); }

/* ---- sub_0FBB96: queue request d0 unless the driver is busy ($FF000C) */
static void s_0FBB96(hle_ctx *c)
{
	uint16_t v = RAM16(0xFF000C);
	cc_logic16(c, v);
	hle_insn(c, 16);                                            /* 0FBB96 tst.w (0xffff000c).l */
	if (bs(c, v != 0)) {                                        /* 0FBB9C bne.s 0xfbbac */
		hle_insn(c, 16);                                    /* 0FBBAC rts */
		return;
	}
	uint32_t r = alu(c, OP_ADD, c->d[0], 1, 16, 1);
	hle_insn(c, 4); set_w(&c->d[0], r);                         /* 0FBB9E addq.w #1,d0 */
	hle_insn(c, 16); RAM16(SND_QUEUED) = (uint16_t)c->d[0];     /* 0FBBA0 move.w d0,(0xffff0038).l */
	cc_logic16(c, c->d[0]);
	hle_insn(c, 20); RAM8(0xFF002E) = 0;                        /* 0FBBA6 sf (0xffff002e).l */
	hle_insn(c, 16);                                            /* 0FBBAC rts */
}
HLE_FUNC_PURE(0x0FBB96, Snd_0FBB96, 120) { s_0FBB96(ctx); }

/* ---- sub_0FBB6C: queue music d0 */
HLE_FUNC_PURE(0x0FBB6C, Snd_0FBB6C, 140)
{
	uint16_t v = RAM16(0xFF000C);
	cc_logic16(ctx, v);
	hle_insn(ctx, 16);                                          /* 0FBB6C tst.w (0xffff000c).l */
	if (bs(ctx, v != 0)) {                                      /* 0FBB72 bne.s 0xfbb94 */
		hle_insn(ctx, 16);                                  /* 0FBB94 rts */
		return;
	}
	uint32_t r = alu(ctx, OP_ADD, ctx->d[0], 1, 16, 1);
	hle_insn(ctx, 4); set_w(&ctx->d[0], r);                     /* 0FBB74 addq.w #1,d0 */
	hle_insn(ctx, 16); RAM16(0xFF000E) = (uint16_t)ctx->d[0];   /* 0FBB76 move.w d0,(0xffff000e).l */
	cc_logic16(ctx, ctx->d[0]);
	hle_insn(ctx, 20); RAM8(0xFF00A0) = 0xFF;                   /* 0FBB7C st (0xffff00a0).l */
	hle_insn(ctx, 20); RAM8(0xFF0072) = 0;                      /* 0FBB82 sf (0xffff0072).l */
	hle_insn(ctx, 20); RAM8(0xFF002E) = 0;                      /* 0FBB88 sf (0xffff002e).l */
	hle_insn(ctx, 20); RAM8(0xFF009A) = 0;                      /* 0FBB8E clr.b (0xffff009a).l */
	cc_logic8(ctx, 0);
	hle_insn(ctx, 16);                                          /* 0FBB94 rts */
}

/* ---- sub_0FBAD2 / sub_0FBAE2 */
HLE_FUNC_PURE(0x0FBAD2, Snd_0FBAD2, 56)
{
	hle_insn(ctx, 20); RAM16(0xFF000A) = 1;                     /* 0FBAD2 move.w #1,(0xffff000a).l */
	cc_logic16(ctx, 1);
	hle_insn(ctx, 20); RAM8(0xFF000C) = 0xFF;                   /* 0FBADA st (0xffff000c).l */
	hle_insn(ctx, 16);                                          /* 0FBAE0 rts */
}

HLE_FUNC_PURE(0x0FBAE2, Snd_0FBAE2, 90)
{
	uint16_t v = RAM16(0xFF002E);
	cc_logic16(ctx, v);
	hle_insn(ctx, 16);                                          /* 0FBAE2 tst.w (0xffff002e).l */
	if (bs(ctx, v == 0)) {                                      /* 0FBAE8 beq.s 0xfbafe */
		hle_insn(ctx, 16);                                  /* 0FBAFE rts */
		return;
	}
	hle_insn(ctx, 20); RAM16(0xFF000A) = 2;                     /* 0FBAEA move.w #2,(0xffff000a).l */
	cc_logic16(ctx, 2);
	hle_insn(ctx, 20); RAM8(0xFF002E) = 0;                      /* 0FBAF2 sf (0xffff002e).l */
	hle_insn(ctx, 20); RAM8(0xFF000C) = 0;                      /* 0FBAF8 sf (0xffff000c).l */
	hle_insn(ctx, 16);                                          /* 0FBAFE rts */
}

/* ---- sub_0FBBAE: music-change request d0 */
static void s_0FBBAE(hle_ctx *c)
{
	uint32_t r = alu(c, OP_ADD, c->d[0], 1, 16, 1);
	hle_insn(c, 4); set_w(&c->d[0], r);                         /* 0FBBAE addq.w #1,d0 */
	hle_insn(c, 16); RAM16(SND_0016) = (uint16_t)c->d[0];       /* 0FBBB0 move.w d0,(0xffff0016).l */
	cc_logic16(c, c->d[0]);
	hle_insn(c, 20); RAM16(0xFF0018) = 0;                       /* 0FBBB6 clr.w (0xffff0018).l */
	cc_logic16(c, 0);
	hle_insn(c, 18);                                            /* 0FBBBC bsr.w sub_0FC0FE */
	s_0FC0FE(c);
	hle_insn(c, 20); RAM16(0xFF001C) = 1;                       /* 0FBBC0 move.w #1,(0xffff001c).l */
	cc_logic16(c, 1);
	hle_insn(c, 20); RAM16(0xFF001E) = 0;                       /* 0FBBC8 clr.w (0xffff001e).l */
	cc_logic16(c, 0);
	hle_insn(c, 16);                                            /* 0FBBCE rts */
}
HLE_FUNC_PURE(0x0FBBAE, Snd_0FBBAE, 160) { s_0FBBAE(ctx); }

/* ---- sub_0FC27C: split d0 into bank/offset and store d1,d0,d2,d3 into $FF001E..$FF0024 */
static void s_0FC290(hle_ctx *c)
{
	hle_insn(c, 16); RAM16(0xFF001E) = (uint16_t)c->d[1];       /* 0FC290 move.w d1,(0xffff001e).l */
	cc_logic16(c, c->d[1]);
	hle_insn(c, 16); RAM16(0xFF0022) = (uint16_t)c->d[0];       /* 0FC296 move.w d0,(0xffff0022).l */
	cc_logic16(c, c->d[0]);
	hle_insn(c, 16); RAM16(0xFF0020) = (uint16_t)c->d[2];       /* 0FC29C move.w d2,(0xffff0020).l */
	cc_logic16(c, c->d[2]);
	hle_insn(c, 16); RAM16(0xFF0024) = (uint16_t)c->d[3];       /* 0FC2A2 move.w d3,(0xffff0024).l */
	cc_logic16(c, c->d[3]);
	hle_insn(c, 16);                                            /* 0FC2A8 rts */
}
static void s_0FC27C(hle_ctx *c)
{
	c->d[3] = c->d[0];
	cc_logic32(c, c->d[3]);
	hle_insn(c, 4);                                             /* 0FC27C move.l d0,d3 */
	{
		/* lsr.l #n,Dn: 8 + 2n; X=C=last bit out */
		uint32_t v = c->d[3];
		unsigned cb = (v >> 7) & 1;
		v >>= 8;
		cc_set(c, CC_X | CC_N | CC_Z | CC_V | CC_C, (cb ? CC_C | CC_X : 0) | (v ? 0 : CC_Z));
		c->d[3] = v;
		hle_insn(c, 24);                                    /* 0FC27E lsr.l #8,d3 */
		cb = (v >> 6) & 1;
		v >>= 7;
		cc_set(c, CC_X | CC_N | CC_Z | CC_V | CC_C, (cb ? CC_C | CC_X : 0) | (v ? 0 : CC_Z));
		c->d[3] = v;
		hle_insn(c, 22);                                    /* 0FC280 lsr.l #7,d3 */
	}
	set_w(&c->d[0], c->d[0] & 0x7FFF);
	cc_logic16(c, c->d[0]);
	hle_insn(c, 8);                                             /* 0FC282 andi.w #$7fff,d0 */
	set_w(&c->d[0], c->d[0] | 0x8000);
	cc_logic16(c, c->d[0]);
	hle_insn(c, 8);                                             /* 0FC286 ori.w #$8000,d0 */
	hle_insn(c, 10);                                            /* 0FC28A bra.s 0xfc290 */
	s_0FC290(c);
}
HLE_FUNC_PURE(0x0FC27C, Snd_0FC27C, 160) { s_0FC27C(ctx); }

/* ---- sub_013D04: sound request d0 ---- */
HLE_FUNC_PURE(0x013D04, SndRequest, 200)
{
	push_d1d2(ctx);                                             /* 013D04 movem.w d1-d2,-(sp) */
	set_w(&ctx->d[1], ctx->d[0]);
	cc_logic16(ctx, ctx->d[1]);
	hle_insn(ctx, 4);                                           /* 013D08 move.w d0,d1 */
	set_w(&ctx->d[1], alu(ctx, OP_SUB, ctx->d[1], 0x5E, 16, 1));
	hle_insn(ctx, 8);                                           /* 013D0A subi.w #$5e,d1 */
	set_b(&ctx->d[2], tab(DAT_013D30, ctx->d[1]));
	cc_logic8(ctx, ctx->d[2]);
	hle_insn(ctx, 14);                                          /* 013D0E move.b (pc,d1.w),d2 */
	set_w(&ctx->d[1], RAM16(SND_0026));
	cc_logic16(ctx, ctx->d[1]);
	hle_insn(ctx, 16);                                          /* 013D12 move.w (0xffff0026).l,d1 */
	if (!bs(ctx, c_z(ctx))) {                                   /* 013D18 beq.s 0x13d24 */
		set_w(&ctx->d[1], alu(ctx, OP_SUB, ctx->d[1], 0x5E, 16, 1));
		hle_insn(ctx, 8);                                   /* 013D1A subi.w #$5e,d1 */
		alu(ctx, OP_SUB, ctx->d[2], tab(DAT_013D30, ctx->d[1]), 8, 0);
		hle_insn(ctx, 14);                                  /* 013D1E cmp.b (pc,d1.w),d2 */
		if (bs(ctx, c_lt(ctx))) {                           /* 013D22 blt.s 0x13d2a */
			pop_d1d2(ctx);
			hle_insn(ctx, 16);                          /* 013D2E rts */
			return;
		}
	}
	hle_insn(ctx, 20);                                          /* 013D24 jsr (sub_0FBB96).l */
	s_0FBB96(ctx);
	pop_d1d2(ctx);                                              /* 013D2A movem.w (sp)+,d1-d2 */
	hle_insn(ctx, 16);                                          /* 013D2E rts */
}

/* ---- sub_013CC4: music request with priority check ---- */
HLE_FUNC_PURE(0x013CC4, SndMusicRequest, 260)
{
	push_d1d2(ctx);                                             /* 013CC4 movem.w d1-d2,-(sp) */
	uint8_t b = RAM8(SND_A4);
	cc_logic8(ctx, b);
	hle_insn(ctx, 16);                                          /* 013CC8 tst.b (0xffff00a4).l */
	if (!bs(ctx, b == 0)) {                                     /* 013CCE beq.s 0x13cf2 */
		uint16_t w = RAM16(SND_9EC4);
		cc_logic16(ctx, w);
		hle_insn(ctx, 16);                                  /* 013CD0 tst.w (0xffff9ec4).l */
		if (bs(ctx, w != 0))                                /* 013CD6 bne.s 0x13cfe */
			goto done;
		set_w(&ctx->d[1], ctx->d[0]);
		cc_logic16(ctx, ctx->d[1]);
		hle_insn(ctx, 4);                                   /* 013CD8 move.w d0,d1 */
		set_w(&ctx->d[1], alu(ctx, OP_SUB, ctx->d[1], 0x5E, 16, 1));
		hle_insn(ctx, 8);                                   /* 013CDA subi.w #$5e,d1 */
		set_b(&ctx->d[2], tab(DAT_013D30, ctx->d[1]));
		cc_logic8(ctx, ctx->d[2]);
		hle_insn(ctx, 14);                                  /* 013CDE move.b (pc,d1.w),d2 */
		set_w(&ctx->d[1], RAM16(SND_0016));
		cc_logic16(ctx, ctx->d[1]);
		hle_insn(ctx, 16);                                  /* 013CE2 move.w (0xffff0016).l,d1 */
		set_w(&ctx->d[1], alu(ctx, OP_SUB, ctx->d[1], 0x5E, 16, 1));
		hle_insn(ctx, 8);                                   /* 013CE8 subi.w #$5e,d1 */
		alu(ctx, OP_SUB, ctx->d[2], tab(DAT_013D30, ctx->d[1]), 8, 0);
		hle_insn(ctx, 14);                                  /* 013CEC cmp.b (pc,d1.w),d2 */
		if (bs(ctx, c_lt(ctx)))                             /* 013CF0 blt.s 0x13cfe */
			goto done;
	}
	RAM16(SND_9EC4) = 0;
	cc_logic16(ctx, 0);
	hle_insn(ctx, 20);                                          /* 013CF2 clr.w (0xffff9ec4).l */
	hle_insn(ctx, 20);                                          /* 013CF8 jsr (sub_0FBBAE).l */
	s_0FBBAE(ctx);
done:
	pop_d1d2(ctx);                                              /* 013CFE movem.w (sp)+,d1-d2 */
	hle_insn(ctx, 16);                                          /* 013D02 rts */
}

/* ---- sub_013D56: request through the command slots (d3 = id, d0-d2 = args) ---- */
HLE_FUNC_PURE(0x013D56, SndSlotRequest, 340)
{
	uint8_t b = RAM8(SND_A4);
	cc_logic8(ctx, b);
	hle_insn(ctx, 16);                                          /* 013D56 tst.b (0xffff00a4).l */
	if (!bs(ctx, b == 0)) {                                     /* 013D5C beq.s 0x13d72 */
		set_w(&ctx->d[4], RAM16(SND_9EC4));
		cc_logic16(ctx, ctx->d[4]);
		hle_insn(ctx, 16);                                  /* 013D5E move.w (0xffff9ec4).l,d4 */
		if (!bs(ctx, c_z(ctx))) {                           /* 013D64 beq.s 0x13d72 */
			if (!bs(ctx, (ctx->sr & CC_N) != 0)) {      /* 013D66 blt.s 0x13d72 (V=0 after the move) */
				set_b(&ctx->d[4], tab(DAT_013D84, ctx->d[4]));
				cc_logic8(ctx, ctx->d[4]);
				hle_insn(ctx, 14);                  /* 013D68 move.b (pc,d4.w),d4 */
				alu(ctx, OP_SUB, ctx->d[4], tab(DAT_013D84, ctx->d[3]), 8, 0);
				hle_insn(ctx, 14);                  /* 013D6C cmp.b (pc,d3.w),d4 */
				if (bs(ctx, c_le(ctx))) {           /* 013D70 ble.s 0x13d82 */
					hle_insn(ctx, 16);          /* 013D82 rts */
					return;
				}
			}
		}
	}
	RAM16(SND_9EC4) = (uint16_t)ctx->d[3];
	cc_logic16(ctx, ctx->d[3]);
	hle_insn(ctx, 16);                                          /* 013D72 move.w d3,(0xffff9ec4).l */
	ctx->a[7] -= 2; bus_w16(ctx->a[7], (uint16_t)ctx->d[7]);
	hle_insn(ctx, 8);                                           /* 013D78 move.w d7,-(sp) */
	hle_insn(ctx, 20);                                          /* 013D7A jsr (sub_0FC27C).l */
	s_0FC27C(ctx);
	set_w(&ctx->d[7], bus_r16(ctx->a[7])); ctx->a[7] += 2;
	hle_insn(ctx, 8);                                           /* 013D80 move.w (sp)+,d7 */
	hle_insn(ctx, 16);                                          /* 013D82 rts */
}

/* ===================================================================== interrupt handlers */
#define PORT2_DATA 0xA10005u
#define CONTROL_MODE 0xFF9DF5u
#define GUN_LATCH_CUR 0xFF9F76u
#define GUN_LATCH_COUNT 0xFF9F74u
#define GUN_BTN_RAW 0xFF9F77u
#define VBLANK_COUNT 0xFFA0EEu

static void dbra_loop(hle_ctx *c, int n)
{
	for (int i = 0; i < n; i++)
		hle_insn(c, 10);                                    /* dbf d0,loop (taken) */
	hle_insn(c, 14);                                            /* dbf d0,loop (expired) */
}

static void io_w8(hle_ctx *c, uint8_t v)
{
	hle_pre(c);
	bus_w8(PORT2_DATA, v);
	hle_post(c, 20);                                            /* move.b #imm,(0xa10005).l */
}

/* $000308 .. $000396: everything but `jsr sub_0FBBD0` and the rte */
HLE_RANGE_YS(0x000308, VBlankInt, 14000, 0x000396)
{
	uint8_t cm = RAM8(CONTROL_MODE);
	cc_set(ctx, CC_Z, (cm & 1) ? 0 : CC_Z);
	hle_insn(ctx, 20);                                          /* 000308 btst #0,(0xffff9df5).l */
	if (!bs(ctx, !(ctx->sr & CC_Z))) {                          /* 000310 bne.s 0x390 */
		hle_movem_l(ctx, 24, 2);                            /* 000312 movem.l d0/a0,-(sp) */
		ctx->a[7] -= 4; bus_w32(ctx->a[7], ctx->a[0]);
		ctx->a[7] -= 4; bus_w32(ctx->a[7], ctx->d[0]);
		hle_insn(ctx, 8);                                   /* 000316 move.w #$a1,d0 */
		dbra_loop(ctx, 0xA1);                               /* 00031A dbf d0,0x31a */
		hle_insn(ctx, 4);                                   /* 00031E nop */
		io_w8(ctx, 0xFF);                                   /* 000320 move.b #-1,(0xa10005).l */
		RAM8(GUN_LATCH_COUNT) = RAM8(GUN_LATCH_CUR);        /* each effect first, then its charge (a coroutine) */
		hle_insn(ctx, 28);                                  /* 000328 move.b (0xffff9f76).l,(0xffff9f74).l */
		RAM8(GUN_LATCH_CUR) = 0;
		hle_insn(ctx, 20);                                  /* 000332 clr.b (0xffff9f76).l */
		for (int i = 0; i < 5; i++)
			hle_insn(ctx, 4);                           /* 000338..000340 nop x5 */
		hle_insn(ctx, 8);                                   /* 000342 move.w #$3c,d0 */
		io_w8(ctx, 0xDF);                                   /* 000346 move.b #-33,(0xa10005).l */
		dbra_loop(ctx, 0x3C);                               /* 00034E dbf d0,0x34e */
		io_w8(ctx, 0xFF);                                   /* 000352 move.b #-1,(0xa10005).l */
		hle_insn(ctx, 4);                                   /* 00035A nop */
		io_w8(ctx, 0xDF);                                   /* 00035C move.b #-33,(0xa10005).l */
		hle_insn(ctx, 8);                                   /* 000364 move.w #$42c,d0 */
		dbra_loop(ctx, 0x42C);                              /* 000368 dbf d0,0x368 */
		hle_pre(ctx);
		uint8_t v = bus_r8(PORT2_DATA);
		hle_post(ctx, 16);                                  /* 00036C move.b (0xa10005).l,d0 */
		io_w8(ctx, 0xFF);                                   /* 000372 move.b #-1,(0xa10005).l */
		hle_insn(ctx, 4);                                   /* 00037A nop */
		io_w8(ctx, 0xCF);                                   /* 00037C move.b #-49,(0xa10005).l */
		hle_insn(ctx, 4);                                   /* 000384 not.b d0 */
		RAM8(GUN_BTN_RAW) = (uint8_t)~v;
		hle_insn(ctx, 16);                                  /* 000386 move.b d0,(0xffff9f77).l */
		hle_movem_l(ctx, 28, 2);                            /* 00038C movem.l (sp)+,d0/a0 */
		/* d0 and a0 are restored from the stack (the flags are overwritten by the addq below) */
		ctx->d[0] = bus_r32(ctx->a[7]); ctx->a[7] += 4;
		ctx->a[0] = bus_r32(ctx->a[7]); ctx->a[7] += 4;
	}
	uint32_t r = alu(ctx, OP_ADD, RAM16(VBLANK_COUNT), 1, 16, 1);
	RAM16(VBLANK_COUNT) = (uint16_t)r;
	hle_insn(ctx, 20);                                          /* 000390 addq.w #1,(0xffffa0ee).l */
	hle_jump(ctx, 0x000396);                                    /* the real jsr sub_0FBBD0 + rte follow */
}

/* $00039E .. $0003DC: first TH of the frame stores the HV counter in the 16-word ring */
HLE_RANGE_YS(0x00039E, ExtInt, 220, 0x0003DC)
{
	uint8_t cur = RAM8(GUN_LATCH_CUR);
	alu(ctx, OP_SUB, cur, 0, 8, 0);
	hle_insn(ctx, 20);                                          /* 00039E cmpi.b #0,(0xffff9f76).l */
	if (bw(ctx, c_ne(ctx))) {                                   /* 0003A6 bne.w 0x3dc */
		hle_jump(ctx, 0x0003DC);                            /* rte (68k) */
		return;
	}
	ctx->a[7] -= 2; bus_w16(ctx->a[7], (uint16_t)ctx->d[0]);
	hle_insn(ctx, 8);                                           /* 0003AA move.w d0,-(sp) */
	ctx->a[7] -= 4; bus_w32(ctx->a[7], ctx->a[0]);
	hle_insn(ctx, 12);                                          /* 0003AC move.l a0,-(sp) */
	hle_pre(ctx);
	uint16_t hv = bus_r16(0xC00008);                            /* 0003AE move.w (0xc00008).l,d0 */
	hle_post(ctx, 16);
	uint32_t a0 = RAM32(0xFFA09E);
	hle_insn(ctx, 20);                                          /* 0003B4 movea.l (0xffffa09e).l,a0 */
	bus_w16(a0 & 0xFFFFFF, hv);
	a0 += 2;
	hle_insn(ctx, 8);                                           /* 0003BA move.w d0,(a0)+ */
	alu(ctx, OP_SUB, a0, 0xFFFFA09E, 32, 0);
	hle_insn(ctx, 14);                                          /* 0003BC cmpa.l #0xffffa09e,a0 */
	if (!bw(ctx, a0 != 0xFFFFA09E)) {                           /* 0003C2 bne.w 0x3cc */
		a0 = 0xFFFFA07E;
		hle_insn(ctx, 12);                                  /* 0003C6 lea (0xffffa07e).l,a0 */
	}
	RAM32_W(0xFFA09E, a0);
	hle_insn(ctx, 20);                                          /* 0003CC move.l a0,(0xffffa09e).l */
	uint32_t r = alu(ctx, OP_ADD, RAM8(GUN_LATCH_CUR), 1, 8, 1);
	RAM8(GUN_LATCH_CUR) = (uint8_t)r;
	hle_insn(ctx, 20);                                          /* 0003D2 addq.b #1,(0xffff9f76).l */
	ctx->a[7] += 4;
	hle_insn(ctx, 12);                                          /* 0003D8 movea.l (sp)+,a0 */
	set_w(&ctx->d[0], bus_r16(ctx->a[7]));
	ctx->a[7] += 2;
	cc_logic16(ctx, ctx->d[0]);
	hle_insn(ctx, 8);                                           /* 0003DA move.w (sp)+,d0 */
	hle_jump(ctx, 0x0003DC);                                    /* rte (68k) */
}
