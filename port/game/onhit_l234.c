/* On-hit and kill callbacks of the level-2, -3 and -4 objects and the Skynet parts: the a2 targets that the
 * hit tests of ai_l2.c, ai_l3.c, ai_l4.c and ai_skynet.c load for HitBox (`lea onhit,a2 ; lea box,a3 ;
 * jmp HitBox`) and the kill routines they hand back in a0. Same scheme as onhit_l1.c: ShotHitTest
 * ($005892, hittest.c) calls them with `jsr (a0)` through hle_call68k, and the nested instruction fetch
 * hits the HLE_FUNC at the entry address. The bodies are written instruction by instruction from
 * src/t2.s with the GPGX costs; the score stubs (AddScore*) are called as C after the jsr is charged, the
 * 0072xx stubs, sounds, SetPlayerState and the object spawners run as 68k through hle_call68k (those
 * entries are plain HLE_FUNC, the rest HLE_FUNC_PURE).
 *
 *   level 2:  on-hit $00651E $006550 $006582 $0065B0 $006668 $0066B4 $006846 $0068A0 $0068D6
 *             kill   $006524 $006556 $006588 $0065B6 $006686 $00684C $0068A6 $0068DC
 *   level 3:  on-hit $005CD2 $005D9C $005DCC $006060   kill $005CD8 $005DA2 $005DD2 $006066
 *   level 4:  on-hit $00639A $006430 $006462 $0064A0 $0067D2 $006806 $00690C
 *             kill   $0063A0 $006436 $006468 $0064A6 $00680C
 *   Skynet:   on-hit $006770   kill $00677E
 *
 * Left out (never reached by the 92 lockstep states, so unproven): $0065DE/$0065E4 $006612/$006630
 * (human B) $0066BA $006332/$006346 $0067D8 $0069A2 $006730/$00673E (wall gun).
 */
#include "hle.h"

#include <stdint.h>

void AddScore250(hle_ctx *ctx);
void AddScore1000(hle_ctx *ctx);
void AddScore4000(hle_ctx *ctx);
void AddScore5000(hle_ctx *ctx);

#define OBJ_COUNT   0xFF80DAu
#define SHOT_XY     0xFF9E94u
#define SHOT_DAMAGE 0xFFA0CAu
#define OBJ_LIMIT   0xFF80DCu

#define A4 (ctx->a[4])
#define A6 (ctx->a[6])

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

static int c_z(hle_ctx *c) { return (c->sr & CC_Z) != 0; }
static int c_n(hle_ctx *c) { return (c->sr & CC_N) != 0; }
static int c_v(hle_ctx *c) { return (c->sr & CC_V) != 0; }
static int c_gt(hle_ctx *c) { return !c_z(c) && c_n(c) == c_v(c); }

/* conditional branches: charge, return whether taken */
static int bs(hle_ctx *c, int taken) { hle_insn(c, taken ? 10 : 8); return taken; }    /* Bcc.s */

static void rts_(hle_ctx *c) { hle_insn(c, 16); }

static void lea_a0(hle_ctx *c, uint32_t v)                     /* lea (d16,pc),a0 */
{
	c->a[0] = v;
	hle_insn(c, 8);
}

/* jsr/bsr to C: charge the jump, the callee charges its rts */
static void jsr_c(hle_ctx *c, void (*f)(hle_ctx *))
{
	hle_insn(c, 18);
	f(c);
}

/* jsr to 68k code: charge the jump (18 (d16,pc), 20 abs.l), run the callee as the original (its own rts) */
static void jsr68(hle_ctx *c, uint32_t addr, int cost)
{
	hle_insn(c, cost);
	hle_sync(c);
	hle_callc(c, addr);
}

/* bclr/bset/btst #n,Dn (Z = the bit before) */
static void bclr_d(hle_ctx *c, int r, int bit)
{
	cc_set(c, CC_Z, ((c->d[r] >> bit) & 1) ? 0 : CC_Z);
	c->d[r] &= ~(1u << bit);
	hle_insn(c, 12);
}
/* bset/bclr/btst.b #n,<byte> (op 's' 'c' 't'): the bit is n mod 8 of the byte (a #8..#15 on (a6) is the
 * high byte of the word) */
static void bit_m(hle_ctx *c, char op, uint32_t a, int bit, int cost)
{
	uint8_t o = RAM8(a);
	cc_set(c, CC_Z, ((o >> bit) & 1) ? 0 : CC_Z);
	if (op == 's')
		RAM8(a) = o | (uint8_t)(1u << bit);
	else if (op == 'c')
		RAM8(a) = o & (uint8_t)~(1u << bit);
	hle_insn(c, cost);
}

/* move.w d5,(a6) */
static void mvw_d5_a6(hle_ctx *c)
{
	RAM16(c->a[6]) = (uint16_t)c->d[5];
	cc_logic16(c, c->d[5]);
	hle_insn(c, 8);
}
/* move.w #imm,(d16,a6) 16 / move.l #imm,(d16,a6) 24 */
static void mvwi_a6(hle_ctx *c, uint32_t off, uint16_t v)
{
	RAM16(c->a[6] + off) = v;
	cc_logic16(c, v);
	hle_insn(c, 16);
}
static void mvli_a6(hle_ctx *c, uint32_t off, uint32_t v)
{
	RAM32_W(c->a[6] + off, v);
	cc_logic32(c, v);
	hle_insn(c, 24);
}
/* addq.w #1,(d16,a4) */
static void addq_a4(hle_ctx *c, uint32_t off)
{
	RAM16(c->a[4] + off) = (uint16_t)alu(c, OP_ADD, RAM16(c->a[4] + off), 1, 16, 1);
	hle_insn(c, 16);
}

/* ---------------------------------------------------------------- shared helpers */
/* movem.l d0-d7/a0-a6,-(sp) / (sp)+ : d0 lowest, a6 at +56 */
static void push_all(hle_ctx *ctx)
{
	ctx->a[7] -= 60;
	for (int i = 0; i < 8; i++)
		RAM32_W(ctx->a[7] + 4 * i, ctx->d[i]);
	for (int i = 0; i < 7; i++)
		RAM32_W(ctx->a[7] + 32 + 4 * i, ctx->a[i]);
	hle_movem_l(ctx, 8 + 8 * 15, 15);
}
static void pop_all(hle_ctx *ctx)
{
	for (int i = 0; i < 8; i++)
		ctx->d[i] = RAM32(ctx->a[7] + 4 * i);
	for (int i = 0; i < 7; i++)
		ctx->a[i] = RAM32(ctx->a[7] + 32 + 4 * i);
	ctx->a[7] += 60;
	hle_movem_l(ctx, 12 + 8 * 15, 15);
}

/* sub_006252 (after its jsr was charged): debris at the shot unless too many objects. Charges its rts. */
static void sub_6252(hle_ctx *ctx)
{
	push_all(ctx);
	alu(ctx, OP_SUB, RAM16(OBJ_LIMIT), 12, 16, 0);
	hle_insn(ctx, 20);                                  /* cmpi.w #12,(abs).l */
	if (!bs(ctx, c_gt(ctx))) {                          /* bgt.s loc_006274 */
		ctx->d[0] = (uint32_t)(int32_t)(int16_t)RAM16(SHOT_XY);
		ctx->d[1] = (uint32_t)(int32_t)(int16_t)RAM16(SHOT_XY + 2);
		hle_movem_w(ctx, 20 + 4 * 2, 2);            /* movem.w (abs).l,d0-d1 */
		set_w(&ctx->d[2], RAM16(A6 + 6));
		cc_logic16(ctx, ctx->d[2]);
		hle_insn(ctx, 12);                          /* move.w (6,a6),d2 */
		set_w(&ctx->d[2], alu(ctx, OP_SUB, ctx->d[2], 1, 16, 1));
		hle_insn(ctx, 4);                           /* subq.w #1,d2 */
		jsr68(ctx, 0x0124EC, 20);                   /* jsr (sub_0124EC).l */
	}
	pop_all(ctx);
	rts_(ctx);
}

/* ---------------------------------------------------------------- shared kill bodies */
/* the common tail: bclr #15,d5 ; move.w d5,(a6) ; move.l #state,(0x20,a6) ; addq.w #1,(off,a4) ; rts */
static void kill_tail(hle_ctx *ctx, uint32_t state, uint32_t a4off)
{
	bclr_d(ctx, 5, 15);
	mvw_d5_a6(ctx);
	mvli_a6(ctx, 0x20, state);
	addq_a4(ctx, a4off);
	rts_(ctx);
}

/* sub_006870 (after its jsr was charged): remember this slot as the live infiltrator, freeing the previous
 * one. Charges its rts. */
static void sub_6870(hle_ctx *ctx)
{
	cc_logic8(ctx, RAM8(0xFF9EBE));
	hle_insn(ctx, 16);                                  /* tst.b (abs).l */
	if (!bs(ctx, RAM8(0xFF9EBE) == 0)) {                /* beq.s loc_006886 */
		ctx->a[1] = RAM32(0xFF9ED2);
		hle_insn(ctx, 20);                          /* movea.l (abs).l,a1 */
		RAM16(ctx->a[1]) = 0;
		cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
		hle_insn(ctx, 12);                          /* clr.w (a1) */
		RAM16(OBJ_COUNT) = (uint16_t)alu(ctx, OP_SUB, RAM16(OBJ_COUNT), 1, 16, 1);
		hle_insn(ctx, 20);                          /* subq.w #1,(abs).l */
	}
	RAM8(0xFF9EBE) = 0xFF;
	hle_insn(ctx, 20);                                  /* st.b (abs).l */
	RAM32_W(0xFF9ED2, A6);
	cc_logic32(ctx, A6);
	hle_insn(ctx, 20);                                  /* move.l a6,(abs).l */
	rts_(ctx);
}

/* ================================================================== level 2 */
HLE_FUNC_PURE(0x00651E, Onhit2_651E, 40) { lea_a0(ctx, 0x006524); rts_(ctx); }
HLE_FUNC_PURE(0x006524, Onhit2Kill_6524, 300)
{
	jsr_c(ctx, AddScore250);
	kill_tail(ctx, 0xEA2E, 0x3A);
}

HLE_FUNC_PURE(0x006550, Onhit2_6550, 40) { lea_a0(ctx, 0x006556); rts_(ctx); }
HLE_FUNC(0x006556, Onhit2Kill_6556, 2500)
{
	jsr68(ctx, 0x0072DC, 18);                           /* jsr sub_0072DC */
	kill_tail(ctx, 0xEA72, 0x3A);
}

HLE_FUNC_PURE(0x006582, Onhit2_6582, 40) { lea_a0(ctx, 0x006588); rts_(ctx); }
HLE_FUNC_PURE(0x006588, Onhit2Kill_6588, 300) { kill_tail(ctx, 0xEB36, 0x3A); }

HLE_FUNC_PURE(0x0065B0, Onhit2_65B0, 40) { lea_a0(ctx, 0x0065B6); rts_(ctx); }
HLE_FUNC_PURE(0x0065B6, Onhit2Kill_65B6, 300) { kill_tail(ctx, 0xEB78, 0x3A); }



static void human_kill(hle_ctx *ctx, uint32_t state)
{
	bclr_d(ctx, 5, 15);
	mvw_d5_a6(ctx);
	mvli_a6(ctx, 0x20, state);
	addq_a4(ctx, 0x38);
	ctx->d[0] = 6;
	cc_logic32(ctx, 6);
	hle_insn(ctx, 4);                                   /* moveq #6,d0 */
	jsr68(ctx, 0x008FA8, 18);                           /* jsr SetPlayerState */
	rts_(ctx);
}

HLE_FUNC(0x006668, Onhit2_HumanC, 2500)
{
	jsr68(ctx, 0x013DA8, 20);                           /* jsr (sub_013DA8).l */
	mvli_a6(ctx, 0x20, 0x1522A);
	mvwi_a6(ctx, 0x24, 0x1E);
	bit_m(ctx, 'c', A6, 7, 16);                         /* bclr.b #15,(a6) */
	lea_a0(ctx, 0x006686);
	rts_(ctx);                                          /* loc_006684 */
}
HLE_FUNC(0x006686, Onhit2Kill_HumanC, 2500) { human_kill(ctx, 0x15208); }

HLE_FUNC_PURE(0x0066B4, Onhit2_66B4, 40) { lea_a0(ctx, 0x0066BA); rts_(ctx); }

/* infiltrators (three variants): 250 points, remember the slot as the live infiltrator */
static void infil_kill(hle_ctx *ctx, uint32_t state)
{
	jsr_c(ctx, AddScore250);
	bclr_d(ctx, 5, 15);
	mvw_d5_a6(ctx);
	mvli_a6(ctx, 0x20, state);
	hle_insn(ctx, 18);                                  /* jsr sub_006870 */
	sub_6870(ctx);
	addq_a4(ctx, 0x3E);
	rts_(ctx);
}
HLE_FUNC_PURE(0x006846, Onhit2_6846, 40) { lea_a0(ctx, 0x00684C); rts_(ctx); }
HLE_FUNC_PURE(0x00684C, Onhit2Kill_684C, 400) { infil_kill(ctx, 0xEED4); }
HLE_FUNC_PURE(0x0068A0, Onhit2_68A0, 40) { lea_a0(ctx, 0x0068A6); rts_(ctx); }
HLE_FUNC_PURE(0x0068A6, Onhit2Kill_68A6, 400) { infil_kill(ctx, 0xF094); }
HLE_FUNC_PURE(0x0068D6, Onhit2_68D6, 40) { lea_a0(ctx, 0x0068DC); rts_(ctx); }
HLE_FUNC_PURE(0x0068DC, Onhit2Kill_68DC, 400) { infil_kill(ctx, 0xF27A); }

/* ================================================================== level 3 */
HLE_FUNC_PURE(0x005CD2, Onhit3_5CD2, 40) { lea_a0(ctx, 0x005CD8); rts_(ctx); }

/* sub_005DA2 (after its jsr/bsr was charged): flying HK / structure kill, also the kill of $005D9C */
static void sub_5da2(hle_ctx *ctx)
{
	jsr_c(ctx, AddScore5000);
	bclr_d(ctx, 5, 15);
	mvw_d5_a6(ctx);
	mvwi_a6(ctx, 0x24, 0x04);
	mvli_a6(ctx, 0x20, 0xC264);
	addq_a4(ctx, 0x34);
	rts_(ctx);
}
HLE_FUNC_PURE(0x005CD8, Onhit3Kill_5CD8, 400)
{
	hle_insn(ctx, 18);                                  /* bsr.w sub_005DA2 */
	sub_5da2(ctx);
	mvli_a6(ctx, 0x20, 0xC252);
	rts_(ctx);
}
HLE_FUNC_PURE(0x005D9C, Onhit3_5D9C, 40) { lea_a0(ctx, 0x005DA2); rts_(ctx); }
HLE_FUNC_PURE(0x005DA2, Onhit3Kill_5DA2, 400) { sub_5da2(ctx); }

HLE_FUNC_PURE(0x005DCC, Onhit3_5DCC, 40) { lea_a0(ctx, 0x005DD2); rts_(ctx); }
HLE_FUNC_PURE(0x005DD2, Onhit3Kill_5DD2, 300)
{
	jsr_c(ctx, AddScore5000);
	kill_tail(ctx, 0xBD22, 0x34);
}

HLE_FUNC_PURE(0x006060, Onhit3_6060, 40) { lea_a0(ctx, 0x006066); rts_(ctx); }
HLE_FUNC(0x006066, Onhit3Kill_6066, 2500)
{
	mvli_a6(ctx, 0x20, 0xC54A);
	jsr68(ctx, 0x0072CE, 18);                           /* jsr sub_0072CE */
	rts_(ctx);
}

/* ================================================================== Skynet parts */
/* $006730 wall gun, $006770 CPU part: 50 points, debris, hand back the kill routine */
static void sky_hit(hle_ctx *ctx, uint32_t kill)
{
	jsr68(ctx, 0x0072B2, 18);                           /* jsr sub_0072B2 */
	hle_insn(ctx, 18);                                  /* jsr sub_006252 */
	sub_6252(ctx);
	lea_a0(ctx, kill);
	rts_(ctx);
}
HLE_FUNC(0x006770, OnhitSky_Part, 4000) { sky_hit(ctx, 0x00677E); }
HLE_FUNC_PURE(0x00677E, OnhitSkyKill_Part, 300)
{
	jsr_c(ctx, AddScore5000);
	kill_tail(ctx, 0xE7C0, 0x3A);
}


HLE_FUNC_PURE(0x00639A, Onhit4_639A, 40) { lea_a0(ctx, 0x0063A0); rts_(ctx); }
HLE_FUNC(0x0063A0, Onhit4Kill_63A0, 2500)
{
	jsr68(ctx, 0x0072EA, 18);                           /* jsr sub_0072EA */
	kill_tail(ctx, 0xDBC4, 0x3A);
}

HLE_FUNC_PURE(0x006430, Onhit4_6430, 40) { lea_a0(ctx, 0x006436); rts_(ctx); }
HLE_FUNC_PURE(0x006436, Onhit4Kill_6436, 300)
{
	jsr_c(ctx, AddScore4000);
	kill_tail(ctx, 0xDD64, 0x3A);
}

HLE_FUNC_PURE(0x006462, Onhit4_6462, 40) { lea_a0(ctx, 0x006468); rts_(ctx); }
HLE_FUNC_PURE(0x006468, Onhit4Kill_6468, 300)
{
	jsr_c(ctx, AddScore4000);
	kill_tail(ctx, 0xE174, 0x3A);
}

HLE_FUNC_PURE(0x0064A0, Onhit4_64A0, 40) { lea_a0(ctx, 0x0064A6); rts_(ctx); }
HLE_FUNC_PURE(0x0064A6, Onhit4Kill_64A6, 400)
{
	jsr_c(ctx, AddScore1000);
	RAM16(0xFFA23C) = (uint16_t)alu(ctx, OP_SUB, RAM16(0xFFA23C), 1, 16, 1);
	hle_insn(ctx, 20);                                  /* subq.w #1,(abs).l */
	if (!bs(ctx, !c_z(ctx))) {                          /* bne.s loc_0064BC */
		ctx->a[1] = RAM32(0xFF9EDA);
		hle_insn(ctx, 20);                          /* movea.l (abs).l,a1 */
		bit_m(ctx, 's', ctx->a[1], 7, 16);          /* bset.b #15,(a1) */
	}
	kill_tail(ctx, 0xE1CA, 0x3A);
}

HLE_FUNC_PURE(0x0067D2, Onhit4_67D2, 40) { lea_a0(ctx, 0x0067D8); rts_(ctx); }

HLE_FUNC_PURE(0x006806, Onhit4_6806, 40) { lea_a0(ctx, 0x00680C); rts_(ctx); }
HLE_FUNC_PURE(0x00680C, Onhit4Kill_680C, 400)
{
	jsr_c(ctx, AddScore1000);
	bclr_d(ctx, 5, 15);
	mvw_d5_a6(ctx);
	mvli_a6(ctx, 0x20, 0xED2E);
	mvwi_a6(ctx, 0x24, 0x14);
	addq_a4(ctx, 0x3E);
	rts_(ctx);
}

/* cmpi.l #imm,d0 */
static void cmpi_l_d0(hle_ctx *ctx, uint32_t v)
{
	alu(ctx, OP_SUB, ctx->d[0], v, 32, 0);
	hle_insn(ctx, 14);
}
/* Bcc.w: 10 taken, 12 not taken */
static int bw(hle_ctx *c, int taken) { hle_insn(c, taken ? 10 : 12); return taken; }
/* tst.w (0x26,a6) */
static void tst_26(hle_ctx *ctx)
{
	cc_logic16(ctx, RAM16(A6 + 0x26));
	hle_insn(ctx, 12);
}

/* $00690C orb: hit reaction by the current +$20 state ($E026 $DFF0 $DE08 / $DF46 $DF9C / other) */
HLE_FUNC(0x00690C, Onhit4_690C, 3000)
{
	ctx->d[0] = RAM32(A6 + 0x20);
	cc_logic32(ctx, ctx->d[0]);
	hle_insn(ctx, 16);                                  /* move.l (0x20,a6),d0 */
	cmpi_l_d0(ctx, 0xE026);
	if (!bs(ctx, c_z(ctx))) {                           /* beq.s loc_006928 */
		cmpi_l_d0(ctx, 0xDFF0);
		if (!bs(ctx, c_z(ctx))) {                   /* beq.s loc_006928 */
			cmpi_l_d0(ctx, 0xDE08);
			if (bs(ctx, !c_z(ctx)))             /* bne.s loc_00695A */
				goto L_695A;
		}
	}
	/* loc_006928 */
	tst_26(ctx);
	if (bs(ctx, !c_n(ctx)))                             /* bpl.s loc_00699C */
		goto L_699C;
	set_w(&ctx->d[5], RAM16(A6 + 2));
	cc_logic16(ctx, ctx->d[5]);
	hle_insn(ctx, 12);                                  /* move.w (2,a6),d5 */
	set_w(&ctx->d[1], RAM16(A6 + 4));
	cc_logic16(ctx, ctx->d[1]);
	hle_insn(ctx, 12);                                  /* move.w (4,a6),d1 */
	jsr68(ctx, 0x009A5E, 18);                           /* jsr sub_009A5E */
	ctx->a[5] = 0x12EE6;
	hle_insn(ctx, 12);                                  /* movea.l #$12EE6,a5 */
	RAM16(A6 + 0x1A) = ROM16(ctx->a[5]);
	cc_logic16(ctx, ROM16(ctx->a[5]));
	ctx->a[5] += 2;
	hle_insn(ctx, 16);                                  /* move.w (a5)+,(0x1a,a6) */
	RAM32_W(A6 + 0x16, ctx->a[5]);
	cc_logic32(ctx, ctx->a[5]);
	hle_insn(ctx, 16);                                  /* move.l a5,(0x16,a6) */
	mvwi_a6(ctx, 0x14, 0x100);
	mvli_a6(ctx, 0x20, 0xE026);
	hle_insn(ctx, 10);                                  /* bra.w loc_006982 */
	goto L_6982;
L_695A:
	cmpi_l_d0(ctx, 0xDF46);
	if (bw(ctx, c_z(ctx)))                              /* beq.w loc_00699C */
		goto L_699C;
	cmpi_l_d0(ctx, 0xDF9C);
	if (bw(ctx, c_z(ctx)))                              /* beq.w loc_00699C */
		goto L_699C;
	tst_26(ctx);
	if (!bs(ctx, !c_n(ctx))) {                          /* bpl.s loc_00698E */
		mvwi_a6(ctx, 0x14, 0);
		mvli_a6(ctx, 0x20, 0xDF46);
L_6982:
		set_w(&ctx->d[0], 0x66);
		cc_logic16(ctx, 0x66);
		hle_insn(ctx, 8);                           /* move.w #$66,d0 */
		jsr68(ctx, 0x013D04, 20);                   /* jsr (sub_013D04).l */
		hle_insn(ctx, 10);                          /* bra.s loc_00699C */
		goto L_699C;
	}
	/* loc_00698E */
	mvli_a6(ctx, 0x20, 0xE05C);
	mvwi_a6(ctx, 0x24, 0x1E);
L_699C:
	lea_a0(ctx, 0x0069A2);
	rts_(ctx);
}
