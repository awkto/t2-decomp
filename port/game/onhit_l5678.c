/* On-hit and kill callbacks of the level-5/6 (Cyberdyne lab), level-7 (freeway) and level-8 (steel mill)
 * objects: the routines the hit tests of ai_l56.c ($0069EE $006AE0 $006B32 $006B68 $006B96 $006BE2
 * $0063C0), ai_l7.c ($0062B0 SWAT van, $006E52) and ai_l8.c ($006C2E, $006EF8) load into a2 for HitBox,
 * and the kill routines they hand back in a0. Same scheme as onhit_l1.c: ShotHitTest ($005892, hittest.c)
 * calls them with `jsr (a0)` through hle_call68k and the nested instruction fetch hits the HLE_FUNC at
 * the entry address.
 *
 *   on-hit (a2)  $0062D4 $0062DC (van boxes 3 and 2, fall into $0062E2), $0062FA (van box 1), $0063CC
 *                $006A00 $006AEC $006B3E $006B74 $006BA2 $006BEE $006C42 $006E66 $006F04
 *   kill (a0)    $00630A (rts) $0063D2 $006A06 $006AF2 $006B44 $006B7A $006BD8 (rts) $006C24 (rts)
 *                $006C5E $006E7E (rts) $006F5A (rts)
 *
 * C callees (AddScore250/750/1000/2000) are called directly after the jsr is charged (the callee charges
 * its rts); sub_006870 and sub_006E32 are static C. The rest (sub_0072CE, sound/debris spawners
 * $01253C $013D04 $013DBE $01016C $00B192) runs as 68k through hle_call68k, so the entries that reach one
 * are plain HLE_FUNC and the others HLE_FUNC_PURE. Written instruction by instruction from the
 * disassembly (the address of the instruction is in a comment) with the GPGX costs. */
#include "hle.h"

#include <stdint.h>

void AddScore250(hle_ctx *ctx);
void AddScore750(hle_ctx *ctx);
void AddScore1000(hle_ctx *ctx);
void AddScore2000(hle_ctx *ctx);

#define OBJ_COUNT   0xFF80DAu
#define SHOT_XY     0xFF9E94u
#define SHOT_DAMAGE 0xFFA0CAu

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
static int c_ge(hle_ctx *c) { return c_n(c) == c_v(c); }
static int c_lt(hle_ctx *c) { return c_n(c) != c_v(c); }
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
	hle_call68k(c, addr);
}

/* bclr #n,Dn (Z = the bit before) */
static void bclr_d(hle_ctx *c, int r, int bit)
{
	cc_set(c, CC_Z, ((c->d[r] >> bit) & 1) ? 0 : CC_Z);
	c->d[r] &= ~(1u << bit);
	hle_insn(c, 12);
}
/* bset/bclr.b #n,<byte> (op 's' 'c'): the bit is n mod 8 of the byte (a #8..#15 on (a6) is the high byte
 * of the word) */
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
/* addq.w #n,(d16,a4) */
static void addq_a4(hle_ctx *c, uint32_t off, uint32_t n)
{
	RAM16(c->a[4] + off) = (uint16_t)alu(c, OP_ADD, RAM16(c->a[4] + off), n, 16, 1);
	hle_insn(c, 16);
}
/* cmpi.w #n,(abs).l */
static void cmpiw_abs(hle_ctx *c, uint32_t a, uint16_t n)
{
	alu(c, OP_SUB, RAM16(a), n, 16, 0);
	hle_insn(c, 20);
}
/* movem.w (abs).l,d0-d1 (sign extended into the whole registers) */
static void movem_shot(hle_ctx *c)
{
	c->d[0] = (uint32_t)(int32_t)(int16_t)RAM16(SHOT_XY);
	c->d[1] = (uint32_t)(int32_t)(int16_t)RAM16(SHOT_XY + 2);
	hle_movem_w(c, 20 + 4 * 2, 2);
}

/* asr.w #n,Dr */
static void asr_w(hle_ctx *c, int r, int n)
{
	uint32_t v = c->d[r] & 0xFFFF;
	uint32_t res = (uint32_t)((int32_t)(int16_t)v >> n) & 0xFFFF;
	unsigned f = ((v >> (n - 1)) & 1) ? (CC_C | CC_X) : 0;
	if (res & 0x8000) f |= CC_N;
	if (!res) f |= CC_Z;
	cc_set(c, CC_X | CC_N | CC_Z | CC_V | CC_C, f);
	set_w(&c->d[r], res);
	hle_insn(c, 6 + 2 * n);
}

/* muls.w #imm,Dn: 38 + 2n cycles (n = bit transitions of the source) + 4 for the immediate */
static void muls_w(hle_ctx *c, int r, uint16_t src)
{
	uint32_t n = 0;
	uint32_t res = (uint32_t)((int32_t)(int16_t)c->d[r] * (int32_t)(int16_t)src);
	uint32_t t = ((uint32_t)src << 1) & 0x1FFFF;
	for (int k = 0; k < 16; k++)
		if (((t >> k) & 1) != ((t >> (k + 1)) & 1))
			n++;
	c->d[r] = res;
	cc_set(c, CC_N | CC_Z | CC_V | CC_C, (res & 0x80000000u ? CC_N : 0) | (res ? 0 : CC_Z));
	hle_insn(c, 38 + 2 * n + 4);
}

/* sub_006870 (after its jsr was charged): remember this slot as the live enemy, freeing the previous one.
 * Charges its rts. */
static void sub_6870(hle_ctx *ctx)
{
	cc_logic8(ctx, RAM8(0xFF9EBE));
	hle_insn(ctx, 16);                                  /* 006870 tst.b (abs).l */
	if (!bs(ctx, RAM8(0xFF9EBE) == 0)) {                /* 006876 beq.s loc_006886 */
		ctx->a[1] = RAM32(0xFF9ED2);
		hle_insn(ctx, 20);                          /* 006878 movea.l (abs).l,a1 */
		RAM16(ctx->a[1]) = 0;
		cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
		hle_insn(ctx, 12);                          /* 00687E clr.w (a1) */
		RAM16(OBJ_COUNT) = (uint16_t)alu(ctx, OP_SUB, RAM16(OBJ_COUNT), 1, 16, 1);
		hle_insn(ctx, 20);                          /* 006880 subq.w #1,(abs).l */
	}
	RAM8(0xFF9EBE) = 0xFF;
	hle_insn(ctx, 20);                                  /* 006886 st.b (abs).l */
	RAM32_W(0xFF9ED2, A6);
	cc_logic32(ctx, A6);
	hle_insn(ctx, 20);                                  /* 00688C move.l a6,(abs).l */
	rts_(ctx);
}

/* sub_006E32 (after its bsr was charged): debris at the shot when the damage is >= 5. Charges its rts. */
static void sub_6e32(hle_ctx *ctx)
{
	cmpiw_abs(ctx, SHOT_DAMAGE, 5);                     /* 006E32 cmpi.w #5,(abs).l */
	if (!bs(ctx, c_lt(ctx))) {                          /* 006E3A blt.s loc_006E50 */
		movem_shot(ctx);                            /* 006E3C movem.w (abs).l,d0-d1 */
		set_w(&ctx->d[2], RAM16(A6 + 6));
		cc_logic16(ctx, ctx->d[2]);
		hle_insn(ctx, 12);                          /* 006E44 move.w (6,a6),d2 */
		set_w(&ctx->d[2], alu(ctx, OP_SUB, ctx->d[2], 1, 16, 1));
		hle_insn(ctx, 4);                           /* 006E48 subq.w #1,d2 */
		jsr68(ctx, 0x01253C, 20);                   /* 006E4A jsr (sub_01253C).l */
	}
	rts_(ctx);                                          /* 006E50 rts */
}

/* ---------------------------------------------------------------- SWAT van (level 7, HitSwatVan $0062B0) */
/* $0062FA: sound, then falls into $006304 */
static void van_62fa(hle_ctx *ctx)
{
	set_w(&ctx->d[0], 0x72);
	cc_logic16(ctx, 0x72);
	hle_insn(ctx, 8);                                   /* 0062FA move.w #$72,d0 */
	jsr68(ctx, 0x013D04, 20);                           /* 0062FE jsr (sub_013D04).l */
}

/* $006304: hand back the bare-rts kill routine */
static void van_6304(hle_ctx *ctx)
{
	lea_a0(ctx, 0x00630A);                              /* 006304 lea (sub_00630A,pc),a0 */
	rts_(ctx);                                          /* 006308 rts */
}

#ifdef ONHIT5678_UNPROVEN   /* no state in /opt/t2/vstates reaches it (0 calls in 92 states x 3000 frames) */
/* $0062E2: 250 points, debris at the shot, then falls into $0062FA */
static void van_62e2(hle_ctx *ctx)
{
	jsr_c(ctx, AddScore250);                            /* 0062E2 jsr AddScore250 */
	movem_shot(ctx);                                    /* 0062E6 movem.w (abs).l,d0-d1 */
	set_w(&ctx->d[2], RAM16(A6 + 6));
	cc_logic16(ctx, ctx->d[2]);
	hle_insn(ctx, 12);                                  /* 0062EE move.w (6,a6),d2 */
	set_w(&ctx->d[2], alu(ctx, OP_SUB, ctx->d[2], 1, 16, 1));
	hle_insn(ctx, 4);                                   /* 0062F2 subq.w #1,d2 */
	jsr68(ctx, 0x01253C, 20);                           /* 0062F4 jsr (sub_01253C).l */
	van_62fa(ctx);
}

HLE_FUNC(0x0062D4, Onhit5678_Van3, 4000)
{
	bit_m(ctx, 's', A6, 1, 16);                         /* 0062D4 bset.b #9,(a6) */
	if (bs(ctx, !c_z(ctx))) {                           /* 0062D8 bne.s sub_006304 */
		van_6304(ctx);
		return;
	}
	hle_insn(ctx, 10);                                  /* 0062DA bra.s sub_0062E2 */
	van_62e2(ctx);
	van_6304(ctx);
}

HLE_FUNC(0x0062DC, Onhit5678_Van2, 4000)
{
	bit_m(ctx, 's', A6, 0, 16);                         /* 0062DC bset.b #8,(a6) */
	if (bs(ctx, !c_z(ctx))) {                           /* 0062E0 bne.s sub_006304 */
		van_6304(ctx);
		return;
	}
	van_62e2(ctx);
	van_6304(ctx);
}

#endif /* ONHIT5678_UNPROVEN */

HLE_FUNC(0x0062FA, Onhit5678_Van1, 3000)
{
	van_62fa(ctx);
	van_6304(ctx);
}

#ifdef ONHIT5678_UNPROVEN   /* no state in /opt/t2/vstates reaches it (0 calls in 92 states x 3000 frames) */
HLE_FUNC_PURE(0x00630A, Onhit5678_VanKill, 30)
{
	rts_(ctx);
}

#endif /* ONHIT5678_UNPROVEN */

/* ---------------------------------------------------------------- level-5/6 fixture $20 (HitL56Lamp) */
HLE_FUNC_PURE(0x0063CC, Onhit5678_Lamp, 40)
{
	lea_a0(ctx, 0x0063D2);
	rts_(ctx);
}

HLE_FUNC(0x0063D2, Onhit5678_LampKill, 2500)
{
	jsr68(ctx, 0x0072CE, 18);                           /* 0063D2 jsr sub_0072CE */
	bclr_d(ctx, 5, 15);                                 /* 0063D6 bclr #15,d5 */
	mvw_d5_a6(ctx);                                     /* 0063DA move.w d5,(a6) */
	mvli_a6(ctx, 0x20, 0xFB52);                         /* 0063DC */
	addq_a4(ctx, 0x3A, 1);                              /* 0063E4 addq.w #1,(58,a4) */
	rts_(ctx);
}

/* ---------------------------------------------------------------- Cyberdyne equipment $0F (HitL56Equip) */
HLE_FUNC_PURE(0x006A00, Onhit5678_Equip, 40)
{
	lea_a0(ctx, 0x006A06);
	rts_(ctx);
}

HLE_FUNC(0x006A06, Onhit5678_EquipKill, 2500)
{
	jsr68(ctx, 0x0072CE, 18);                           /* 006A06 jsr sub_0072CE */
	mvli_a6(ctx, 0x20, 0xF8EA);                         /* 006A0A move.l #$F8EA,(32,a6) */
	addq_a4(ctx, 0x3A, 2);                              /* 006A12 addq.w #2,(58,a4) */
	rts_(ctx);
}

/* ---------------------------------------------------------------- endoskeleton $27/$01 (HitL56Endo) */
HLE_FUNC_PURE(0x006AEC, Onhit5678_Endo, 40)
{
	lea_a0(ctx, 0x006AF2);
	rts_(ctx);
}

HLE_FUNC_PURE(0x006AF2, Onhit5678_EndoKill, 300)
{
	jsr_c(ctx, AddScore250);                            /* 006AF2 jsr AddScore250 */
	bclr_d(ctx, 5, 15);                                 /* 006AF6 */
	mvw_d5_a6(ctx);                                     /* 006AFA */
	mvli_a6(ctx, 0x20, 0xFD64);                         /* 006AFC */
	alu(ctx, OP_SUB, RAM8(0xFF9EBE), 2, 8, 0);
	hle_insn(ctx, 20);                                  /* 006B04 cmpi.b #2,(abs).l */
	if (!bs(ctx, !c_z(ctx))) {                          /* 006B0C bne.s loc_006B18 */
		RAM16(A6) = 0;
		cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
		hle_insn(ctx, 12);                          /* 006B0E clr.w (a6) */
		RAM16(OBJ_COUNT) = (uint16_t)alu(ctx, OP_SUB, RAM16(OBJ_COUNT), 1, 16, 1);
		hle_insn(ctx, 20);                          /* 006B10 subq.w #1,(abs).l */
		hle_insn(ctx, 10);                          /* 006B16 bra.s loc_006B24 */
	} else {
		cc_logic8(ctx, RAM8(0xFFA0A5));
		hle_insn(ctx, 16);                          /* 006B18 tst.b (abs).l */
		if (!bs(ctx, !c_z(ctx))) {                  /* 006B1E bne.s loc_006B24 */
			hle_insn(ctx, 18);                  /* 006B20 jsr sub_006870 */
			sub_6870(ctx);
		}
	}
	addq_a4(ctx, 0x36, 1);                              /* 006B24 addq.w #1,(54,a4) */
	rts_(ctx);                                          /* 006B28 rts */
}

/* ---------------------------------------------------------------- endoskeleton $27 (HitL56Endo2) */
HLE_FUNC_PURE(0x006B3E, Onhit5678_Endo2, 40)
{
	lea_a0(ctx, 0x006B44);
	rts_(ctx);
}

HLE_FUNC_PURE(0x006B44, Onhit5678_Endo2Kill, 300)
{
	jsr_c(ctx, AddScore250);                            /* 006B44 */
	bclr_d(ctx, 5, 15);                                 /* 006B48 */
	mvw_d5_a6(ctx);                                     /* 006B4C */
	mvli_a6(ctx, 0x20, 0x1000C);                        /* 006B4E */
	hle_insn(ctx, 18);                                  /* 006B56 jsr sub_006870 */
	sub_6870(ctx);
	addq_a4(ctx, 0x36, 1);                              /* 006B5A */
	rts_(ctx);                                          /* 006B5E */
}

#ifdef ONHIT5678_UNPROVEN   /* no state in /opt/t2/vstates reaches it (0 calls in 92 states x 3000 frames) */
/* ---------------------------------------------------------------- endoskeleton $27 (HitL56Endo3) */
HLE_FUNC_PURE(0x006B74, Onhit5678_Endo3, 40)
{
	lea_a0(ctx, 0x006B7A);
	rts_(ctx);
}

HLE_FUNC(0x006B7A, Onhit5678_Endo3Kill, 2500)
{
	jsr_c(ctx, AddScore750);                            /* 006B7A jsr AddScore750 */
	cc_logic32(ctx, RAM32(0xFF9EDA));
	hle_insn(ctx, 20);                                  /* 006B7E tst.l (abs).l */
	if (!bs(ctx, !c_z(ctx)))                            /* 006B84 bne.s loc_006B8C */
		jsr68(ctx, 0x01016C, 20);                   /* 006B86 jsr (sub_01016C).l */
	rts_(ctx);                                          /* 006B8C rts */
}

#endif /* ONHIT5678_UNPROVEN */

/* ---------------------------------------------------------------- model $28 (HitL56Model, HitL56Model2) */
static void model_hit(hle_ctx *ctx, uint32_t ai1, uint32_t ai2, uint32_t kill)
{
	bit_m(ctx, 'c', A6, 7, 16);                         /* bclr.b #15,(a6) */
	mvli_a6(ctx, 0x20, ai1);
	mvwi_a6(ctx, 0x24, 0x18);
	cmpiw_abs(ctx, SHOT_DAMAGE, 6);                     /* cmpi.w #6,(abs).l */
	if (!bs(ctx, c_lt(ctx))) {                          /* blt.s */
		mvli_a6(ctx, 0x20, ai2);
		mvwi_a6(ctx, 0x24, 0x64);
	}
	jsr68(ctx, 0x013DBE, 20);                           /* jsr (sub_013DBE).l */
	lea_a0(ctx, kill);
	rts_(ctx);
}

HLE_FUNC(0x006BA2, Onhit5678_Model, 3000) { model_hit(ctx, 0x103A4, 0x103C6, 0x006BD8); }
HLE_FUNC(0x006BEE, Onhit5678_Model2, 3000) { model_hit(ctx, 0x105D4, 0x105F6, 0x006C24); }

#ifdef ONHIT5678_UNPROVEN   /* no state in /opt/t2/vstates reaches it (0 calls in 92 states x 3000 frames) */
HLE_FUNC_PURE(0x006BD8, Onhit5678_ModelKill, 30) { rts_(ctx); }
HLE_FUNC_PURE(0x006C24, Onhit5678_Model2Kill, 30) { rts_(ctx); }

#endif /* ONHIT5678_UNPROVEN */

/* ---------------------------------------------------------------- T-1000 in the steel mill (HitT1000Mill) */
HLE_FUNC(0x006C42, Onhit5678_Mill, 4000)
{
	bit_m(ctx, 'c', A6, 7, 16);                         /* 006C42 bclr.b #15,(a6) */
	mvli_a6(ctx, 0x20, 0x11514);                        /* 006C46 move.l #$11514,(32,a6) */
	mvwi_a6(ctx, 0x24, 0x0C);                           /* 006C4E */
	hle_insn(ctx, 18);                                  /* 006C54 bsr.w sub_006E32 */
	sub_6e32(ctx);
	lea_a0(ctx, 0x006C5E);                              /* 006C58 */
	rts_(ctx);                                          /* 006C5C */
}

#ifdef ONHIT5678_UNPROVEN   /* no state in /opt/t2/vstates reaches it (0 calls in 92 states x 3000 frames) */
HLE_FUNC_PURE(0x006C5E, Onhit5678_MillKill, 300)
{
	cmpiw_abs(ctx, SHOT_DAMAGE, 5);                     /* 006C5E cmpi.w #5,(abs).l */
	if (bs(ctx, c_lt(ctx))) {                           /* 006C66 blt.s loc_006C84 */
		rts_(ctx);
		return;
	}
	bit_m(ctx, 'c', A6, 7, 16);                         /* 006C68 bclr.b #15,(a6) */
	mvwi_a6(ctx, 0x08, 0x3A);                           /* 006C6C */
	mvli_a6(ctx, 0x20, 0x11580);                        /* 006C72 */
	mvwi_a6(ctx, 0x24, 0x64);                           /* 006C7A */
	jsr_c(ctx, AddScore2000);                           /* 006C80 jsr AddScore2000 */
	rts_(ctx);                                          /* 006C84 */
}

#endif /* ONHIT5678_UNPROVEN */

#ifdef ONHIT5678_UNPROVEN   /* no state in /opt/t2/vstates reaches it (0 calls in 92 states x 3000 frames) */
/* ---------------------------------------------------------------- T-1000 on the police vehicle (HitL7Heli) */
HLE_FUNC_PURE(0x006E66, Onhit5678_Heli, 300)
{
	jsr_c(ctx, AddScore1000);                           /* 006E66 jsr AddScore1000 */
	mvli_a6(ctx, 0x20, 0x10BE4);                        /* 006E6A move.l #$10BE4,(32,a6) */
	RAM16(0xFFA23C) = (uint16_t)alu(ctx, OP_ADD, RAM16(0xFFA23C), 1, 16, 1);
	hle_insn(ctx, 20);                                  /* 006E72 addq.w #1,(abs).l */
	lea_a0(ctx, 0x006E7E);                              /* 006E78 */
	rts_(ctx);                                          /* 006E7C */
}

HLE_FUNC_PURE(0x006E7E, Onhit5678_HeliKill, 30) { rts_(ctx); }

#endif /* ONHIT5678_UNPROVEN */

/* ---------------------------------------------------------------- tanker panel (HitL8Panel) */
HLE_FUNC(0x006F04, Onhit5678_Panel, 4000)
{
	uint32_t ea;
	movem_shot(ctx);                                    /* 006F04 movem.w (abs).l,d0-d1 */
	set_w(&ctx->d[1], alu(ctx, OP_SUB, ctx->d[1], 0x50, 16, 1));
	hle_insn(ctx, 8);                                   /* 006F0C subi.w #$50,d1 */
	asr_w(ctx, 1, 4);                                   /* 006F10 asr.w #4,d1 */
	muls_w(ctx, 1, 20);                                 /* 006F12 muls.w #20,d1 */
	asr_w(ctx, 0, 4);                                   /* 006F16 asr.w #4,d0 */
	set_w(&ctx->d[0], alu(ctx, OP_ADD, ctx->d[0], ctx->d[1], 16, 1));
	hle_insn(ctx, 4);                                   /* 006F18 add.w d1,d0 */
	set_w(&ctx->d[0], alu(ctx, OP_ADD, ctx->d[0], ctx->d[0], 16, 1));
	hle_insn(ctx, 4);                                   /* 006F1A add.w d0,d0 */
	ctx->a[0] = 0xFFFFAE84u;
	hle_insn(ctx, 12);                                  /* 006F1C lea (abs).l,a0 */
	ea = ctx->a[0] + (uint32_t)(int32_t)(int16_t)ctx->d[0];
	set_w(&ctx->d[1], RAM16(ea));
	cc_logic16(ctx, ctx->d[1]);
	hle_insn(ctx, 14);                                  /* 006F22 move.w (0,a0,d0.w),d1 */
	if (!bs(ctx, c_z(ctx))) {                           /* 006F26 beq.s loc_006F54 */
		set_w(&ctx->d[1], alu(ctx, OP_SUB, ctx->d[1], RAM16(SHOT_DAMAGE), 16, 1));
		hle_insn(ctx, 16);                          /* 006F28 sub.w (abs).l,d1 */
		if (bs(ctx, c_gt(ctx))) {                   /* 006F2E bgt.s loc_006F50 */
			RAM16(ea) = (uint16_t)ctx->d[1];
			cc_logic16(ctx, ctx->d[1]);
			hle_insn(ctx, 14);                  /* 006F50 move.w d1,(0,a0,d0.w) */
		} else {
			jsr_c(ctx, AddScore750);            /* 006F30 jsr AddScore750 */
			RAM16(ea) = 0;
			cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
			hle_insn(ctx, 18);                  /* 006F34 clr.w (0,a0,d0.w) */
			cmpiw_abs(ctx, OBJ_COUNT, 0x14);    /* 006F38 cmpi.w #20,(abs).l */
			if (bs(ctx, c_ge(ctx))) {           /* 006F40 bge.s loc_006F50 */
				RAM16(ea) = (uint16_t)ctx->d[1];
				cc_logic16(ctx, ctx->d[1]);
				hle_insn(ctx, 14);          /* 006F50 move.w d1,(0,a0,d0.w) */
			} else {
				jsr68(ctx, 0x00B192, 18);   /* 006F42 jsr sub_00B192 */
				ctx->d[0] = 0x70;
				cc_logic32(ctx, 0x70);
				hle_insn(ctx, 4);           /* 006F46 moveq #$70,d0 */
				jsr68(ctx, 0x013D04, 20);   /* 006F48 jsr (sub_013D04).l */
				hle_insn(ctx, 10);          /* 006F4E bra.s loc_006F54 */
			}
		}
	}
	lea_a0(ctx, 0x006F5A);                              /* 006F54 lea (sub_006F5A,pc),a0 */
	rts_(ctx);                                          /* 006F58 rts */
}

HLE_FUNC_PURE(0x006F5A, Onhit5678_PanelKill, 30) { rts_(ctx); }
