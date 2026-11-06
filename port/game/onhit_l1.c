/* On-hit and kill callbacks of the level-1 objects (src/t2.s $005988..$006278): the routines the hit tests
 * (ai_endo.c, ai_simple.c, ai_misc.c, ai_hktank.c) load into a2 for HitBox, and the kill routines they hand
 * back in a0. docs/subsystems/objects.md, "Hit testing, damage, kills". ShotHitTest ($005892, hittest.c)
 * calls them with `jsr (a0)` through hle_call68k; HLE dispatch works inside those nested runs, so an
 * HLE_FUNC at the entry address is hit by the nested instruction fetch exactly like HitBox is.
 *
 *   on-hit (a2)  usually `lea kill,a0 ; rts` (the shot is accepted; ShotHitTest subtracts the damage and
 *                calls a0 when hp went negative): $005988 $0059E6 $005A02 $005A54 $005B10 $005B2E $005C0A
 *                $005E28 $005F1A $005F7A $005FDE $006022 $006146 $006194 $0061D6 $006214 $00628E
 *   kill (a0)    score, clear the shootable bit (bclr #15,d5 ; move.w d5,(a6)), switch the AI state at +$20:
 *                $00598E $005A5A $005C10 $005F34 $005F80 $006028 $006154 $0061A2 $0061E4 $00621E $00629E
 *
 * Left out (never reached by the 92 lockstep states, so unproven): $005A00 $005A08 $005AC8/$005ACE $005B56
 * $005BBE $005E36 $006166 $0061BC.
 *
 * C callees are called directly after the jsr is charged (AddScore250/5000/10000; the callee charges its
 * rts); the helpers $005A24 and $006252 are static C. Everything else (the tiny AddScore stubs $0072B2/C0/CE,
 * sounds, SetPlayerState, debris spawners) runs as 68k through hle_call68k, so the entries that reach one are
 * plain HLE_FUNC and the rest HLE_FUNC_PURE. The bodies were written instruction by instruction from the
 * disassembly with the GPGX costs (m68ki_cycles.h; movem.w/l add 4/8 per register); flags are mirrored. */
#include "hle.h"

#include <stdint.h>

void AddScore250(hle_ctx *ctx);
void AddScore5000(hle_ctx *ctx);
void AddScore10000(hle_ctx *ctx);

#define OBJ_COUNT   0xFF80DAu
#define LIVE_FLAG   0xFF9EC0u
#define LIVE_SLOT   0xFF9ED6u
#define SHOT_XY     0xFF9E94u
#define SHOT_DAMAGE 0xFFA0CAu
#define OBJ_LIMIT   0xFF80DCu
#define BOSS_PARTS  0xFF9EBCu

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
	hle_call68k(c, addr);
}

/* bclr/bset/btst #n,Dn (Z = the bit before) */
static void bclr_d(hle_ctx *c, int r, int bit)
{
	cc_set(c, CC_Z, ((c->d[r] >> bit) & 1) ? 0 : CC_Z);
	c->d[r] &= ~(1u << bit);
	hle_insn(c, 12);
}
static void bset_d(hle_ctx *c, int r, int bit)
{
	cc_set(c, CC_Z, ((c->d[r] >> bit) & 1) ? 0 : CC_Z);
	c->d[r] |= 1u << bit;
	hle_insn(c, 10);
}
static void btst_d(hle_ctx *c, int r, int bit)
{
	cc_set(c, CC_Z, ((c->d[r] >> bit) & 1) ? 0 : CC_Z);
	hle_insn(c, 10);
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
/* sub_005A24 (after its jsr was charged, or fallen into): remember this slot as the live enemy, freeing the
 * previous one. Charges its rts. */
static void sub_5a24(hle_ctx *ctx)
{
	cc_logic8(ctx, RAM8(LIVE_FLAG));
	hle_insn(ctx, 16);                                  /* tst.b (abs).l */
	if (!bs(ctx, RAM8(LIVE_FLAG) == 0)) {               /* beq.s loc_005A3A */
		ctx->a[1] = RAM32(LIVE_SLOT);
		hle_insn(ctx, 20);                          /* movea.l (abs).l,a1 */
		RAM16(ctx->a[1]) = 0;
		cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
		hle_insn(ctx, 12);                          /* clr.w (a1) */
		RAM16(OBJ_COUNT) = (uint16_t)alu(ctx, OP_SUB, RAM16(OBJ_COUNT), 1, 16, 1);
		hle_insn(ctx, 20);                          /* subq.w #1,(abs).l */
	}
	RAM8(LIVE_FLAG) = 0xFF;
	hle_insn(ctx, 20);                                  /* st.b (abs).l */
	RAM32_W(LIVE_SLOT, A6);
	cc_logic32(ctx, A6);
	hle_insn(ctx, 20);                                  /* move.l a6,(abs).l */
	rts_(ctx);
}

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

/* ---------------------------------------------------------------- destructible prop (type $0F) */
HLE_FUNC_PURE(0x005988, Onhit1_Prop, 40)
{
	lea_a0(ctx, 0x00598E);
	rts_(ctx);
}

HLE_FUNC_PURE(0x00598E, Onhit1_PropKill, 100)
{
	bclr_d(ctx, 5, 15);
	mvw_d5_a6(ctx);
	mvwi_a6(ctx, 0x14, 0x10);
	mvli_a6(ctx, 0x20, 0xC8BA);
	rts_(ctx);
}

/* ---------------------------------------------------------------- endoskeleton (head, body) */
HLE_FUNC_PURE(0x0059E6, Onhit1_EndoHead, 200)
{
	jsr_c(ctx, AddScore250);
	bit_m(ctx, 'c', A6, 7, 16);                         /* bclr.b #15,(a6) */
	mvli_a6(ctx, 0x20, 0xB504);
	addq_a4(ctx, 0x32);
	lea_a0(ctx, 0x005A00);
	rts_(ctx);
}

HLE_FUNC_PURE(0x005A02, Onhit1_EndoBody, 40)
{
	lea_a0(ctx, 0x005A08);
	rts_(ctx);
}

HLE_FUNC_PURE(0x005A54, Onhit1_Endo, 40)
{
	lea_a0(ctx, 0x005A5A);
	rts_(ctx);
}

HLE_FUNC_PURE(0x005A5A, Onhit1_EndoKill, 200)
{
	jsr_c(ctx, AddScore250);
	bclr_d(ctx, 5, 15);
	mvw_d5_a6(ctx);
	mvwi_a6(ctx, 0x14, 0x20);
	mvli_a6(ctx, 0x20, 0xB69A);
	addq_a4(ctx, 0x32);
	rts_(ctx);
}

/* ---------------------------------------------------------------- endoskeleton 3 (hit_endoskeleton3 pair) */
HLE_FUNC(0x005B10, Onhit1_Endo3A, 2000)
{
	jsr68(ctx, 0x0072C0, 18);                           /* jsr sub_0072C0 */
	bset_d(ctx, 5, 12);
	mvw_d5_a6(ctx);
	mvwi_a6(ctx, 0x14, 0x14);
	mvli_a6(ctx, 0x20, 0xDA48);
	lea_a0(ctx, 0x005B56);
	rts_(ctx);
}

HLE_FUNC(0x005B2E, Onhit1_Endo3B, 3000)
{
	bset_d(ctx, 5, 12);
	mvw_d5_a6(ctx);
	mvwi_a6(ctx, 0x14, 0x14);
	mvli_a6(ctx, 0x20, 0xDA48);
	push_all(ctx);
	jsr68(ctx, 0x01275A, 20);                           /* jsr (sub_01275A).l */
	pop_all(ctx);
	lea_a0(ctx, 0x005BBE);
	rts_(ctx);
}

/* ---------------------------------------------------------------- endoskeleton 2 (hit_endoskeleton2) */
HLE_FUNC_PURE(0x005C0A, Onhit1_Endo2, 40)
{
	lea_a0(ctx, 0x005C10);
	rts_(ctx);
}

HLE_FUNC_PURE(0x005C10, Onhit1_Endo2Kill, 300)
{
	jsr_c(ctx, AddScore250);
	bclr_d(ctx, 5, 15);
	mvw_d5_a6(ctx);
	set_w(&ctx->d[1], RAM16(A6 + 0x14));
	cc_logic16(ctx, ctx->d[1]);
	hle_insn(ctx, 12);                                  /* move.w (0x14,a6),d1 */
	{                                                   /* asr.w #4,d1 */
		uint32_t v = ctx->d[1] & 0xFFFF;
		uint32_t res = (uint32_t)((int32_t)(int16_t)v >> 4) & 0xFFFF;
		unsigned f = ((v >> 3) & 1) ? (CC_C | CC_X) : 0;
		if (res & 0x8000) f |= CC_N;
		if (!res) f |= CC_Z;
		cc_set(ctx, CC_X | CC_N | CC_Z | CC_V | CC_C, f);
		set_w(&ctx->d[1], res);
		hle_insn(ctx, 6 + 2 * 4);
	}
	ctx->a[2] = 0x00C5C0;
	hle_insn(ctx, 8);                                   /* lea (dat_00C5C0,pc),a2 */
	{
		uint8_t b = ROM8(ctx->a[2] + (uint32_t)(int32_t)(int16_t)ctx->d[1]);
		set_b(&ctx->d[1], b);
		cc_logic8(ctx, b);
		hle_insn(ctx, 14);                          /* move.b (0,a2,d1.w),d1 */
	}
	RAM16(A6 + 0x14) = (uint16_t)ctx->d[1];
	cc_logic16(ctx, ctx->d[1]);
	hle_insn(ctx, 12);                                  /* move.w d1,(0x14,a6) */
	mvli_a6(ctx, 0x20, 0xC66C);
	addq_a4(ctx, 0x32);
	jsr_c(ctx, sub_5a24);
	rts_(ctx);
}

/* ---------------------------------------------------------------- type $0D */
HLE_FUNC_PURE(0x005E28, Onhit1_Type0D, 80)
{
	mvli_a6(ctx, 0x20, 0xB896);
	lea_a0(ctx, 0x005E36);
	rts_(ctx);
}

/* ---------------------------------------------------------------- resistance human (type $13) */
HLE_FUNC(0x005F1A, Onhit1_Human, 2500)
{
	jsr68(ctx, 0x013DA8, 20);                           /* jsr (sub_013DA8).l */
	mvli_a6(ctx, 0x20, 0xC968);
	mvwi_a6(ctx, 0x14, 0x28);
	lea_a0(ctx, 0x005F34);
	rts_(ctx);
}

HLE_FUNC(0x005F34, Onhit1_HumanKill, 2500)
{
	bclr_d(ctx, 5, 15);
	mvw_d5_a6(ctx);
	mvli_a6(ctx, 0x20, 0xC95E);
	addq_a4(ctx, 0x38);
	ctx->d[0] = 6;
	cc_logic32(ctx, 6);
	hle_insn(ctx, 4);                                   /* moveq #6,d0 */
	jsr68(ctx, 0x008FA8, 18);                           /* jsr SetPlayerState */
	rts_(ctx);
}

/* ---------------------------------------------------------------- wall section (type $14) */
HLE_FUNC_PURE(0x005F7A, Onhit1_Wall, 40)
{
	lea_a0(ctx, 0x005F80);
	rts_(ctx);
}

HLE_FUNC_PURE(0x005F80, Onhit1_WallKill, 100)
{
	mvli_a6(ctx, 0x20, 0xCA12);
	addq_a4(ctx, 0x3A);
	rts_(ctx);
}

HLE_FUNC_PURE(0x005FDE, Onhit1_WallSelf, 40)
{
	lea_a0(ctx, 0x005FDE);
	rts_(ctx);
}

/* ---------------------------------------------------------------- weak point (type $1A) */
HLE_FUNC_PURE(0x006022, Onhit1_HkWeak, 40)
{
	lea_a0(ctx, 0x006028);
	rts_(ctx);
}

HLE_FUNC(0x006028, Onhit1_HkWeakKill, 2500)
{
	jsr68(ctx, 0x0072CE, 18);                           /* jsr sub_0072CE */
	bclr_d(ctx, 5, 15);
	mvw_d5_a6(ctx);
	mvwi_a6(ctx, 0x24, 0x19);
	mvli_a6(ctx, 0x20, 0xD0C8);
	rts_(ctx);
}

/* ---------------------------------------------------------------- HK tank parts */
/* $006146 body, $006194 hull, $0061D6 turret: 50 points, debris, hand back the kill routine */
static void hk_part_hit(hle_ctx *ctx, uint32_t kill)
{
	jsr68(ctx, 0x0072B2, 18);                           /* jsr sub_0072B2 */
	hle_insn(ctx, 18);                                  /* jsr sub_006252 */
	sub_6252(ctx);
	lea_a0(ctx, kill);
	rts_(ctx);
}

HLE_FUNC(0x006146, Onhit1_HkBody, 4000) { hk_part_hit(ctx, 0x006154); }
HLE_FUNC(0x006194, Onhit1_HkHull, 4000) { hk_part_hit(ctx, 0x0061A2); }
HLE_FUNC(0x0061D6, Onhit1_HkTurret, 4000) { hk_part_hit(ctx, 0x0061E4); }

HLE_FUNC_PURE(0x006154, Onhit1_HkBodyKill, 300)
{
	jsr_c(ctx, AddScore10000);
	bclr_d(ctx, 5, 15);
	mvw_d5_a6(ctx);
	mvli_a6(ctx, 0x20, 0xCC98);
	lea_a0(ctx, 0x006154);                              /* falls into loc_006166 */
	rts_(ctx);
}

HLE_FUNC_PURE(0x0061A2, Onhit1_HkHullKill, 300)
{
	jsr_c(ctx, AddScore5000);
	bclr_d(ctx, 5, 15);
	mvw_d5_a6(ctx);
	mvli_a6(ctx, 0x20, 0xCE0C);
	bit_m(ctx, 's', BOSS_PARTS, 3, 24);                 /* bset.b #3,(abs).l */
	lea_a0(ctx, 0x0061A2);                              /* falls into loc_0061BC */
	rts_(ctx);
}

HLE_FUNC_PURE(0x0061E4, Onhit1_HkTurretKill, 300)
{
	jsr_c(ctx, AddScore5000);
	bclr_d(ctx, 5, 15);
	mvw_d5_a6(ctx);
	mvli_a6(ctx, 0x20, 0xCEC4);
	bit_m(ctx, 's', BOSS_PARTS, 2, 24);
	rts_(ctx);
}

HLE_FUNC(0x006214, Onhit1_HkArm, 3000)
{
	hle_insn(ctx, 18);                                  /* jsr sub_006252 */
	sub_6252(ctx);
	lea_a0(ctx, 0x00621E);
	rts_(ctx);
}

HLE_FUNC_PURE(0x00621E, Onhit1_HkArmKill, 300)
{
	jsr_c(ctx, AddScore5000);
	bclr_d(ctx, 5, 15);
	mvw_d5_a6(ctx);
	mvli_a6(ctx, 0x20, 0xCFDE);
	btst_d(ctx, 5, 14);
	if (bs(ctx, !c_z(ctx))) {                           /* bne.s loc_006240 */
		bit_m(ctx, 's', BOSS_PARTS, 0, 24);
		rts_(ctx);
		return;
	}
	bit_m(ctx, 's', BOSS_PARTS, 1, 24);
	rts_(ctx);
}

/* ---------------------------------------------------------------- John Connor's truck */
HLE_FUNC(0x00628E, Onhit1_Truck, 2500)
{
	set_w(&ctx->d[0], 0x72);
	cc_logic16(ctx, 0x72);
	hle_insn(ctx, 8);                                   /* move.w #$72,d0 */
	jsr68(ctx, 0x013D04, 20);                           /* jsr (sub_013D04).l */
	lea_a0(ctx, 0x00629E);
	rts_(ctx);
}
/* $00629E is the bare `rts` (same as $005A00); the kill routine of the truck */
HLE_FUNC_PURE(0x00629E, Onhit1_TruckKill, 30)
{
	rts_(ctx);
}
