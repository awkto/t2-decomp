/* AI states and hit routines of the level-4 (Skynet base approach) object types.
 * docs/objects_types.md ("Level 4"), src/t2.s.
 *
 * All states are difftest-exact (capture_l4.txt, capture_l4b.txt/capture_l4c.txt from the l4_* states made by the
 * gun-bot, tools/harness/scripts/l4).
 *
 *   type $1F supply container: $00DAC4, $00DAEE, kill $00DB0C; hit $006326
 *   type $21 ceiling fixture:  $00DB8C, $00DBE0, kill $00DBC4; hit $00638E
 *   type $24 background mover: $00E166; hit $006456
 *   type $22 wall panel:       $00DBF6, $00DC06, $00DC5E, $00DC7E, controllers $00DCDC/$00DCE4,
 *                              kill $00DD64; hit $006424
 *   type $29 pop-up endoskeleton: $00EC58, $00EC82, $00ECE8, $00ED2E, kill $00ED5C; hit $0067F2
 *   type $23 orb: $00F540, $00F560, $00F584, $00F5AC, $00F5D2, $00DDBA, $00DE08, $00DEBA, $00DF46,
 *                 $00DF9C, $00DFF0, $00E026, $00E05C, kills $00F65A/$00E0A4; hits $0067C6, $006900
 *
 * Each state is an HLE_RANGE from its entry (reached by the `jmp (a0)` of ObjRunAll/ObjNext) to the
 * `bra.w ObjNext` ($00B3BE, a C range itself). C callees (ObjMoveX, ObjScrollX, ObjKillIfLeftOf,
 * ObjQueueSprite, QueueSprite, Random, HitBox) are called directly after the jsr is charged; the
 * helper subroutines of the original (sub_00E0E8, sub_00E108, sub_00E146, sub_00C48E,
 * sub_00D236, sub_00ED04) are inlined. Hot states that only rarely or sometimes call 68k code that
 * is not C (VdpQueueTileRect, the warning sound sub_013D04 + sub_009054, the shot spawners
 * sub_009A5E/7A, sub_00AC06, sub_00A5FA) are HLE_RANGE_PURE and end right BEFORE that `jsr`
 * (the original runs it, with HLE_EXIT_SP when the range pushed something), as in ai_l2.c;
 * the once-per-object kill states call their 68k helpers with hle_call68k (plain HLE_RANGE with a
 * measured bound). Costs are the GPGX 68000 costs. */
#include "hle.h"

#include <stdint.h>

void ObjMoveX(hle_ctx *ctx);
void ObjScrollX(hle_ctx *ctx);
void ObjKillIfLeftOf(hle_ctx *ctx);
void ObjKillIfRightOf(hle_ctx *ctx);
void ObjQueueSprite(hle_ctx *ctx);
void QueueSprite(hle_ctx *ctx);
void Random(hle_ctx *ctx);
void HitBox(hle_ctx *ctx);

#define OBJ_COUNT 0xFF80DAu
#define EXIT_NEXT 0x00B3BEu

#define A6 (ctx->a[6])

enum { OP_ADD, OP_SUB };

/* a (op) b at `bits` width with the 68k flags (X set as well when setx) */
static uint32_t alu(hle_ctx *c, int op, uint32_t a, uint32_t b, int bits, int setx)
{
	uint32_t m = (1u << bits) - 1, sg = 1u << (bits - 1);
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

/* condition tests on the current CCR */
static int c_z(hle_ctx *c) { return (c->sr & CC_Z) != 0; }
static int c_n(hle_ctx *c) { return (c->sr & CC_N) != 0; }
static int c_v(hle_ctx *c) { return (c->sr & CC_V) != 0; }
static int c_c(hle_ctx *c) { return (c->sr & CC_C) != 0; }
static int c_gt(hle_ctx *c) { return !c_z(c) && c_n(c) == c_v(c); }
static int c_lt(hle_ctx *c) { return c_n(c) != c_v(c); }

/* branches: charge and return whether taken */
static void bs(hle_ctx *c, int taken) { hle_insn(c, taken ? 10 : 8); }   /* Bcc.s */
static void bw(hle_ctx *c, int taken) { hle_insn(c, taken ? 10 : 12); }  /* Bcc.w */

static void jsr(hle_ctx *c, void (*f)(hle_ctx *)) { hle_insn(c, 18); f(c); }

static void exit_next(hle_ctx *c)
{
	hle_insn(c, 10);   /* bra.w ObjNext */
	hle_jump(c, EXIT_NEXT);
}

/* -------- instruction helpers (each charges its own cost) */
static void moveq(hle_ctx *c, int r, int v)
{
	c->d[r] = (uint32_t)(int32_t)v;
	cc_logic32(c, c->d[r]);
	hle_insn(c, 4);
}
static void movew_i_d(hle_ctx *c, int r, uint16_t v)
{
	set_w(&c->d[r], v);
	cc_logic16(c, v);
	hle_insn(c, 8);
}
static void movew_m_d(hle_ctx *c, int r, uint32_t a)
{
	uint16_t v = RAM16(a);
	set_w(&c->d[r], v);
	cc_logic16(c, v);
	hle_insn(c, 12);
}
static void moveb_m_d(hle_ctx *c, int r, uint32_t a)
{
	uint8_t v = RAM8(a);
	set_b(&c->d[r], v);
	cc_logic8(c, v);
	hle_insn(c, 12);
}
static void movew_i_m(hle_ctx *c, uint32_t a, uint16_t v)
{
	RAM16(a) = v;
	cc_logic16(c, v);
	hle_insn(c, 16);
}
static void movel_i_m(hle_ctx *c, uint32_t a, uint32_t v)
{
	RAM32_W(a, v);
	cc_logic32(c, v);
	hle_insn(c, 24);
}
static void moveb_i_m(hle_ctx *c, uint32_t a, uint8_t v)
{
	RAM8(a) = v;
	cc_logic8(c, v);
	hle_insn(c, 16);
}
static void extw(hle_ctx *c, int r)
{
	uint32_t v = (uint32_t)(int32_t)(int8_t)c->d[r];
	set_w(&c->d[r], v);
	cc_logic16(c, v);
	hle_insn(c, 4);
}
static void andiw_d(hle_ctx *c, int r, uint16_t m)
{
	uint16_t v = (uint16_t)c->d[r] & m;
	set_w(&c->d[r], v);
	cc_logic16(c, v);
	hle_insn(c, 8);
}
static void addiw_d(hle_ctx *c, int r, uint16_t i)
{
	set_w(&c->d[r], alu(c, OP_ADD, c->d[r], i, 16, 1));
	hle_insn(c, 8);
}
static void eoriw_d(hle_ctx *c, int r, uint16_t i)
{
	uint16_t v = (uint16_t)c->d[r] ^ i;
	set_w(&c->d[r], v);
	cc_logic16(c, v);
	hle_insn(c, 8);
}
static void cmpiw_d(hle_ctx *c, int r, uint16_t i)
{
	alu(c, OP_SUB, c->d[r], i, 16, 0);
	hle_insn(c, 8);
}
static void subq1w_m(hle_ctx *c, uint32_t a, int cost)
{
	RAM16(a) = (uint16_t)alu(c, OP_SUB, RAM16(a), 1, 16, 1);
	hle_insn(c, cost);
}
static void bset_m(hle_ctx *c, uint32_t a, int bit)
{
	uint8_t o = RAM8(a);
	cc_set(c, CC_Z, ((o >> bit) & 1) ? 0 : CC_Z);
	RAM8(a) = o | (uint8_t)(1u << bit);
	hle_insn(c, 16);
}
static void bclr_m(hle_ctx *c, uint32_t a, int bit)
{
	uint8_t o = RAM8(a);
	cc_set(c, CC_Z, ((o >> bit) & 1) ? 0 : CC_Z);
	RAM8(a) = o & (uint8_t)~(1u << bit);
	hle_insn(c, 16);
}
/* shifts on a word, n = 1..8 */
static uint16_t asr16(hle_ctx *c, uint16_t v, int n)
{
	int16_t s = (int16_t)v;
	uint16_t r = (uint16_t)(s >> n);
	unsigned f = ((v >> (n - 1)) & 1) ? (CC_C | CC_X) : 0;
	if (r & 0x8000) f |= CC_N;
	if (!r) f |= CC_Z;
	cc_set(c, CC_X | CC_N | CC_Z | CC_V | CC_C, f);
	hle_insn(c, 6 + 2 * n);
	return r;
}
static uint16_t asl16(hle_ctx *c, uint16_t v, int n)
{
	uint16_t r = v;
	unsigned f = 0;
	for (int i = 0; i < n; i++) {
		unsigned out = (r >> 15) & 1;
		uint16_t nr = (uint16_t)(r << 1);
		if (((nr >> 15) & 1) != out) f |= CC_V;
		f = (f & CC_V) | (out ? (CC_C | CC_X) : 0);
		r = nr;
	}
	if (r & 0x8000) f |= CC_N;
	if (!r) f |= CC_Z;
	cc_set(c, CC_X | CC_N | CC_Z | CC_V | CC_C, f);
	hle_insn(c, 6 + 2 * n);
	return r;
}




/* sub_00D236 (after its jsr was charged) */
static void sub_d236(hle_ctx *ctx)
{
	moveq(ctx, 4, 3);
	moveb_m_d(ctx, 2, A6 + 0xF);
	extw(ctx, 2);
	moveb_m_d(ctx, 3, A6 + 0xC);
	for (;;) {
		RAM8(A6 + 0xD) = (uint8_t)alu(ctx, OP_ADD, RAM8(A6 + 0xD), ctx->d[3], 8, 1);
		hle_insn(ctx, 16);                          /* add.b d3,(0xd,a6) */
		if (!c_c(ctx)) {
			bs(ctx, 1);
		} else {
			bs(ctx, 0);
			RAM16(A6 + 4) = (uint16_t)alu(ctx, OP_ADD, RAM16(A6 + 4), ctx->d[2], 16, 1);
			hle_insn(ctx, 16);              /* add.w d2,(0x4,a6) */
		}
		uint16_t d4 = (uint16_t)(ctx->d[4] - 1);
		set_w(&ctx->d[4], d4);
		if (d4 == 0xFFFF) {
			hle_insn(ctx, 14);
			break;
		}
		hle_insn(ctx, 10);
	}
	moveb_m_d(ctx, 0, A6 + 0x12);
	if (c_n(ctx)) {                                     /* bmi.s */
		bs(ctx, 1);
		RAM8(A6 + 0xC) = (uint8_t)alu(ctx, OP_SUB, RAM8(A6 + 0xC), ctx->d[0], 8, 1);
		hle_insn(ctx, 16);                          /* sub.b d0,(0xc,a6) */
		if (c_c(ctx)) {                             /* bcs.s taken */
			bs(ctx, 1);
		} else {
			bs(ctx, 0);
			RAM8(A6 + 0xC) = (uint8_t)alu(ctx, OP_ADD, RAM8(A6 + 0xC), ctx->d[0], 8, 1);
			hle_insn(ctx, 16);              /* add.b d0,(0xc,a6) */
			RAM8(A6 + 0x12) = 0;
			cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
			hle_insn(ctx, 16);              /* clr.b (0x12,a6) */
		}
	} else {
		bs(ctx, 0);
		RAM8(A6 + 0xC) = (uint8_t)alu(ctx, OP_SUB, RAM8(A6 + 0xC), ctx->d[0], 8, 1);
		hle_insn(ctx, 16);
		if (!c_c(ctx)) {                            /* bcc.s taken */
			bs(ctx, 1);
		} else {
			bs(ctx, 0);
			RAM8(A6 + 0xF) = (uint8_t)alu(ctx, OP_SUB, 0, RAM8(A6 + 0xF), 8, 1);   /* neg.b */
			hle_insn(ctx, 16);
			RAM8(A6 + 0x12) = (uint8_t)alu(ctx, OP_SUB, 0, RAM8(A6 + 0x12), 8, 1);
			hle_insn(ctx, 16);
			RAM8(A6 + 0xC) = 0;
			cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
			hle_insn(ctx, 16);              /* clr.b (0xc,a6) */
			hle_insn(ctx, 10);              /* bra.s */
		}
	}
	hle_insn(ctx, 16);                                  /* rts */
}

/* ================================================================== helpers of this file */


static void movew_d_d(hle_ctx *c, int dst, int src)
{
	set_w(&c->d[dst], c->d[src] & 0xFFFF);
	cc_logic16(c, c->d[dst]);
	hle_insn(c, 4);
}
/* movem.w (2,a6),d0-d1 */
static void movem_xy(hle_ctx *ctx)
{
	ctx->d[0] = (uint32_t)(int32_t)(int16_t)RAM16(A6 + 2);
	ctx->d[1] = (uint32_t)(int32_t)(int16_t)RAM16(A6 + 4);
	hle_insn(ctx, 24);
}
static void subq_d(hle_ctx *c, int r, int n)
{
	set_w(&c->d[r], alu(c, OP_SUB, c->d[r], (uint32_t)n, 16, 1));
	hle_insn(c, 4);
}
static void clr_obj_slot(hle_ctx *ctx)            /* clr.w (a6); subq.w #1,(obj_count) */
{
	RAM16(A6) = 0;
	cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	hle_insn(ctx, 12);
	RAM16(OBJ_COUNT) = (uint16_t)alu(ctx, OP_SUB, RAM16(OBJ_COUNT), 1, 16, 1);
	hle_insn(ctx, 20);
}

static void q(hle_ctx *c, uint16_t frame)         /* move.w #frame,d1; jsr ObjQueueSprite */
{
	movew_i_d(c, 1, frame);
	jsr(c, ObjQueueSprite);
}
static void scroll_kill(hle_ctx *c, int k)        /* ObjScrollX; moveq #k,d0; ObjKillIfLeftOf */
{
	jsr(c, ObjScrollX);
	moveq(c, 0, k);
	jsr(c, ObjKillIfLeftOf);
}

/* ================================================================== level-4 helpers */
#define OBJ_COUNT_ADDR OBJ_COUNT
#define FRAME_CTR 0xFFA0EEu        /* (abs).w frame counter read by sub_00E146 */
#define TILE_BASE 0xFF9E4Au        /* move.w (abs).l,d7 before VdpQueueTileRect */
#define TILE_RECT 0x012234u        /* VdpQueueTileRect */
#define SPAWN_DEBRIS 0x01253Cu     /* sub_01253C */

static void movew_d_m(hle_ctx *c, int r, uint32_t a)       /* move.w Dn,(d16,a6) */
{
	RAM16(a) = (uint16_t)c->d[r];
	cc_logic16(c, c->d[r]);
	hle_insn(c, 12);
}
static void moveb_d_m(hle_ctx *c, int r, uint32_t a)       /* move.b Dn,(d16,a6) */
{
	RAM8(a) = (uint8_t)c->d[r];
	cc_logic8(c, c->d[r]);
	hle_insn(c, 12);
}
static void movew_m_dc(hle_ctx *c, int r, uint32_t a, int cost)
{
	uint16_t v = RAM16(a);
	set_w(&c->d[r], v);
	cc_logic16(c, v);
	hle_insn(c, cost);
}
static void subiw_d(hle_ctx *c, int r, uint16_t i)
{
	set_w(&c->d[r], alu(c, OP_SUB, c->d[r], i, 16, 1));
	hle_insn(c, 8);
}
static void addqw_d(hle_ctx *c, int r, int n)
{
	set_w(&c->d[r], alu(c, OP_ADD, c->d[r], (uint32_t)n, 16, 1));
	hle_insn(c, 4);
}
static void cmpiw_m(hle_ctx *c, uint32_t a, uint16_t i, int cost)
{
	alu(c, OP_SUB, RAM16(a), i, 16, 0);
	hle_insn(c, cost);
}
static void tstw_m(hle_ctx *c, uint32_t a, int cost)
{
	uint16_t v = RAM16(a);
	cc_logic16(c, v);
	hle_insn(c, cost);
}
static void addiw_m(hle_ctx *c, uint32_t a, uint16_t i)    /* addi.w #i,(d16,a6) */
{
	RAM16(a) = (uint16_t)alu(c, OP_ADD, RAM16(a), i, 16, 1);
	hle_insn(c, 20);
}
static void subiw_m(hle_ctx *c, uint32_t a, uint16_t i)    /* subi.w #i,(d16,a6) */
{
	RAM16(a) = (uint16_t)alu(c, OP_SUB, RAM16(a), i, 16, 1);
	hle_insn(c, 20);
}
static void addqw_m(hle_ctx *c, uint32_t a, int n)         /* addq.w #n,(d16,a6) */
{
	RAM16(a) = (uint16_t)alu(c, OP_ADD, RAM16(a), (uint32_t)n, 16, 1);
	hle_insn(c, 16);
}
static void negb_m(hle_ctx *c, uint32_t a)                 /* neg.b (d16,a6) */
{
	RAM8(a) = (uint8_t)alu(c, OP_SUB, 0, RAM8(a), 8, 1);
	hle_insn(c, 16);
}
static void lea_d(hle_ctx *c, int an, uint32_t v, int cost)
{
	c->a[an] = v;
	hle_insn(c, cost);
}
static void asrw_d(hle_ctx *c, int r, int n)
{
	set_w(&c->d[r], asr16(c, (uint16_t)c->d[r], n));
}
static void lsrw_d(hle_ctx *c, int r, int n)
{
	uint16_t v = (uint16_t)c->d[r];
	uint16_t res = (uint16_t)(v >> n);
	unsigned f = ((v >> (n - 1)) & 1) ? (CC_C | CC_X) : 0;
	if (!res) f |= CC_Z;
	cc_set(c, CC_X | CC_N | CC_Z | CC_V | CC_C, f);
	set_w(&c->d[r], res);
	hle_insn(c, 6 + 2 * n);
}
static void aslw_d(hle_ctx *c, int r, int n)
{
	set_w(&c->d[r], asl16(c, (uint16_t)c->d[r], n));
}
static void addw_dd(hle_ctx *c, int dst, int src)          /* add.w Ds,Dd */
{
	set_w(&c->d[dst], alu(c, OP_ADD, c->d[dst], c->d[src], 16, 1));
	hle_insn(c, 4);
}
static void cmpw_dd(hle_ctx *c, int rs, int rd)            /* cmp.w Ds,Dd */
{
	alu(c, OP_SUB, c->d[rd], c->d[rs], 16, 0);
	hle_insn(c, 4);
}
static void cmpw_md(hle_ctx *c, uint32_t a, int rd)        /* cmp.w (d16,a6),Dd */
{
	alu(c, OP_SUB, c->d[rd], RAM16(a), 16, 0);
	hle_insn(c, 12);
}
static void movem_xy23(hle_ctx *ctx)                       /* movem.w (2,a6),d2-d3 */
{
	ctx->d[2] = (uint32_t)(int32_t)(int16_t)RAM16(A6 + 2);
	ctx->d[3] = (uint32_t)(int32_t)(int16_t)RAM16(A6 + 4);
	hle_insn(ctx, 24);
}
static int c_le(hle_ctx *c) { return c_z(c) || c_n(c) != c_v(c); }
static int c_pl(hle_ctx *c) { return !c_n(c); }
static int c_mi(hle_ctx *c) { return c_n(c); }
/* dbf Dn: returns 1 when the loop goes on */
static int dbf(hle_ctx *c, int r)
{
	uint16_t d = (uint16_t)(c->d[r] - 1);
	set_w(&c->d[r], d);
	if (d == 0xFFFF) {
		hle_insn(c, 14);
		return 0;
	}
	hle_insn(c, 10);
	return 1;
}
/* jsr/bsr to 68k code that is not C: charged, then run for real (non-pure ranges only) */
static void call68(hle_ctx *c, uint32_t addr, int cost)
{
	hle_insn(c, cost);
	hle_sync(c);
	hle_callc(c, addr);
}
static void bra(hle_ctx *c) { hle_insn(c, 10); }          /* bra.s / bra.w */

/* ================================================================== shared 68k subroutines */
/* sub_00E0E8: d1 = frame (+$BF when (+$26) < 0), ObjQueueSprite; rts (the jsr is charged by the caller) */
static void sub_e0e8(hle_ctx *c)
{
	tstw_m(c, c->a[6] + 0x26, 12);
	if (c_pl(c)) {
		bs(c, 1);
	} else {
		bs(c, 0);
		addiw_d(c, 1, 0xBF);
	}
	jsr(c, ObjQueueSprite);
	hle_insn(c, 16);                                    /* rts */
}
/* sub_00E0F8: same with QueueSprite (d2/d3/d5 carry the position) */
static void sub_e0f8(hle_ctx *c)
{
	tstw_m(c, c->a[6] + 0x26, 12);
	if (c_pl(c)) {
		bs(c, 1);
	} else {
		bs(c, 0);
		addiw_d(c, 1, 0xBF);
	}
	jsr(c, QueueSprite);
	hle_insn(c, 16);                                    /* rts */
}
/* sub_00E108: X movement by (+$A)/(+$E)/(+$10), 1 step */
static void sub_e108(hle_ctx *c)
{
	moveq(c, 4, 0);
	bra(c);                                             /* bra.s sub_00E10E */
	moveb_m_d(c, 2, c->a[6] + 0xE);
	extw(c, 2);
	moveb_m_d(c, 3, c->a[6] + 0xA);
	do {
		RAM8(c->a[6] + 0xB) = (uint8_t)alu(c, OP_ADD, RAM8(c->a[6] + 0xB), c->d[3], 8, 1);
		hle_insn(c, 16);                            /* add.b d3,(0xb,a6) */
		if (!c_c(c)) {
			bs(c, 1);
		} else {
			bs(c, 0);
			RAM16(c->a[6] + 2) = (uint16_t)alu(c, OP_ADD, RAM16(c->a[6] + 2), c->d[2], 16, 1);
			hle_insn(c, 16);                    /* add.w d2,(0x2,a6) */
		}
	} while (dbf(c, 4));
	moveb_m_d(c, 0, c->a[6] + 0x10);
	if (c_mi(c)) {
		bs(c, 1);
		RAM8(c->a[6] + 0xA) = (uint8_t)alu(c, OP_SUB, RAM8(c->a[6] + 0xA), c->d[0], 8, 1);
		hle_insn(c, 16);                            /* sub.b d0,(0xa,a6) */
	} else {
		bs(c, 0);
		RAM8(c->a[6] + 0xA) = (uint8_t)alu(c, OP_SUB, RAM8(c->a[6] + 0xA), c->d[0], 8, 1);
		hle_insn(c, 16);
		if (!c_c(c)) {
			bs(c, 1);
		} else {
			bs(c, 0);
			negb_m(c, c->a[6] + 0xE);
			negb_m(c, c->a[6] + 0x10);
			RAM8(c->a[6] + 0xA) = 0;
			cc_set(c, CC_N | CC_Z | CC_V | CC_C, CC_Z);
			hle_insn(c, 16);                    /* clr.b (0xa,a6) */
			bra(c);
		}
	}
	hle_insn(c, 16);                                    /* rts */
}
/* sub_00E146: every few frames the sprite step (+8) goes up to 12; bra ObjNext */
static void sub_e146(hle_ctx *c)
{
	movew_m_dc(c, 0, FRAME_CTR, 16);
	andiw_d(c, 0, 3);
	if (!c_z(c)) {
		bs(c, 1);
	} else {
		bs(c, 0);
		movew_m_d(c, 0, c->a[6] + 8);
		cmpiw_d(c, 0, 12);
		if (c_z(c)) {
			bs(c, 1);
		} else {
			bs(c, 0);
			addqw_d(c, 0, 1);
			movew_d_m(c, 0, c->a[6] + 8);
		}
	}
	exit_next(c);
}
static inline uint16_t rd16(uint32_t a) { return ((a & 0xFFFFFFu) >= 0xFF0000u) ? RAM16(a) : ROM16(a); }
/* sub_00C48E (after its jsr): path stepper; d4 = 0 counter ran out, 1 path ended, -1 still going */
static void sub_c48e(hle_ctx *c)
{
	moveb_m_d(c, 0, c->a[6] + 0xE);
	extw(c, 0);
	moveb_m_d(c, 1, c->a[6] + 0xA);
	moveb_m_d(c, 2, c->a[6] + 0xF);
	extw(c, 2);
	moveb_m_d(c, 3, c->a[6] + 0xC);
	c->a[0] = RAM32(c->a[6] + 0x16);
	hle_insn(c, 16);                                    /* movea.l (0x16,a6),a0 */
	set_w(&c->d[4], rd16(c->a[0]));
	c->a[0] += 2;
	cc_logic16(c, c->d[4]);
	hle_insn(c, 8);                                     /* move.w (a0)+,d4 */
	set_w(&c->d[4], alu(c, OP_SUB, c->d[4], 1, 16, 1));
	hle_insn(c, 4);                                     /* subq.w #1,d4 */
	if (c_mi(c)) {
		bw(c, 1);
		moveq(c, 4, 1);
		hle_insn(c, 16);
		return;
	}
	bw(c, 0);
	subq1w_m(c, c->a[6] + 0x1A, 16);
	if (!c_z(c)) {
		bs(c, 1);
	} else {
		bs(c, 0);
		RAM16(c->a[6] + 0x1A) = rd16(c->a[0]);
		c->a[0] += 2;
		cc_logic16(c, RAM16(c->a[6] + 0x1A));
		hle_insn(c, 16);                            /* move.w (a0)+,(0x1a,a6) */
		RAM32_W(c->a[6] + 0x16, c->a[0]);
		cc_logic32(c, c->a[0]);
		hle_insn(c, 16);                            /* move.l a0,(0x16,a6) */
	}
	for (;;) {
		RAM8(c->a[6] + 0xB) = (uint8_t)alu(c, OP_ADD, RAM8(c->a[6] + 0xB), c->d[1], 8, 1);
		hle_insn(c, 16);                            /* add.b d1,(0xb,a6) */
		if (!c_c(c)) {
			bs(c, 1);
		} else {
			bs(c, 0);
			RAM16(c->a[6] + 2) = (uint16_t)alu(c, OP_ADD, RAM16(c->a[6] + 2), c->d[0], 16, 1);
			hle_insn(c, 16);                    /* add.w d0,(0x2,a6) */
		}
		RAM8(c->a[6] + 0xD) = (uint8_t)alu(c, OP_ADD, RAM8(c->a[6] + 0xD), c->d[3], 8, 1);
		hle_insn(c, 16);                            /* add.b d3,(0xd,a6) */
		if (!c_c(c)) {
			bs(c, 1);
		} else {
			bs(c, 0);
			RAM16(c->a[6] + 4) = (uint16_t)alu(c, OP_ADD, RAM16(c->a[6] + 4), c->d[2], 16, 1);
			hle_insn(c, 16);                    /* add.w d2,(0x4,a6) */
		}
		subq1w_m(c, c->a[6] + 0x14, 16);
		if (c_z(c)) {                               /* beq.s loc_00C4DC */
			bs(c, 1);
			moveq(c, 4, 0);
			hle_insn(c, 16);
			return;
		}
		bs(c, 0);
		if (!dbf(c, 4))
			break;
	}
	hle_insn(c, 16);                                    /* rts */
}

/* ================================================================== type $1F supply container */
/* $00DAC4: after the delay in +$14 the box becomes shootable (hit $006326) */
HLE_RANGE_PURE(0x00DAC4, L4Box, 500, EXIT_NEXT)
{
	movew_m_d(ctx, 0, A6 + 0x14);
	if (c_z(ctx)) {                                     /* beq.s loc_00DADE */
		bs(ctx, 1);
	} else {
		bs(ctx, 0);
		subq_d(ctx, 0, 1);
		if (!c_z(ctx)) {                            /* bne.s loc_00DADA */
			bs(ctx, 1);
		} else {
			bs(ctx, 0);
			bset_m(ctx, A6, 7);
			movel_i_m(ctx, A6 + 0x1C, 0x6326);
		}
		movew_d_m(ctx, 0, A6 + 0x14);
	}
	jsr(ctx, ObjScrollX);
	q(ctx, 0x17F);
	exit_next(ctx);
}
/* $00DAEE: hit flash for +$24 frames, then back to $00DAC4 */
HLE_RANGE_PURE(0x00DAEE, L4BoxFlash, 500, EXIT_NEXT)
{
	subq1w_m(ctx, A6 + 0x24, 16);
	if (!c_z(ctx)) {
		bs(ctx, 1);
	} else {
		bs(ctx, 0);
		movel_i_m(ctx, A6 + 0x20, 0xDAC4);
	}
	jsr(ctx, ObjScrollX);
	q(ctx, 0x180);
	exit_next(ctx);
}
/* $00DB0C: destroyed: debris, free the slot, level script on (ScriptNext stays as it is) */
HLE_RANGE(0x00DB0C, L4BoxKill, 1200, 0x00DB2E)   /* observed max 758 x 1.5 */
{
	movem_xy(ctx);
	movew_m_d(ctx, 2, A6 + 6);
	subiw_d(ctx, 1, 0x40);
	subq_d(ctx, 2, 1);
	call68(ctx, SPAWN_DEBRIS, 18);
	clr_obj_slot(ctx);
	lea_d(ctx, 0, 0x1F9B0, 12);                         /* movea.l #$1F9B0,a0 */
	hle_jump(ctx, 0x00DB2E);                            /* jsr (ScriptNext,pc) */
}

/* ================================================================== type $21 ceiling fixture */
/* $00DB8C: blinking lamp, frame $175 + table[(timer >> 2) & 15] */
HLE_RANGE_PURE(0x00DB8C, L4Lamp, 500, EXIT_NEXT)
{
	scroll_kill(ctx, -0x28);
	subq1w_m(ctx, A6 + 0x24, 16);
	movew_m_d(ctx, 1, A6 + 0x24);
	lsrw_d(ctx, 1, 2);
	andiw_d(ctx, 1, 0xF);
	{
		uint8_t b = ROM8(0xDBB4u + (uint32_t)(int32_t)(int16_t)ctx->d[1]);
		set_b(&ctx->d[1], b);
		cc_logic8(ctx, b);
		hle_insn(ctx, 14);                          /* move.b (dat_00DBB4,pc,d1.w),d1 */
	}
	addiw_d(ctx, 1, 0x175);
	jsr(ctx, ObjQueueSprite);
	exit_next(ctx);
}
static void lamp_dead(hle_ctx *ctx)                     /* sub_00DBE0 */
{
	scroll_kill(ctx, -0x28);
	q(ctx, 0x17E);
	exit_next(ctx);
}
HLE_RANGE_PURE(0x00DBE0, L4LampDead, 400, EXIT_NEXT) { lamp_dead(ctx); }
/* $00DBC4: shot: debris, then the dead lamp */
HLE_RANGE(0x00DBC4, L4LampKill, 1900, EXIT_NEXT)   /* observed max 1238 x 1.5 */
{
	movem_xy(ctx);
	movew_m_d(ctx, 2, A6 + 6);
	addiw_d(ctx, 1, 0x20);
	subq_d(ctx, 2, 1);
	call68(ctx, SPAWN_DEBRIS, 18);
	movel_i_m(ctx, A6 + 0x20, 0xDBE0);
	lamp_dead(ctx);
}

/* ================================================================== type $24 invisible mover */
HLE_RANGE_PURE(0x00E166, L4Mover, 300, EXIT_NEXT)
{
	scroll_kill(ctx, -0x28);
	exit_next(ctx);
}

/* ================================================================== type $22 wall panel */
/* sub_00DCB2: queue the 6x5 tile rect (a0 = pointer chosen by the caller), up to the real jsr
 * VdpQueueTileRect ($00DCD2); the code after it is the range at $00DCD6 */
static void panel_rect(hle_ctx *c)
{
	movew_m_d(c, 1, c->a[6] + 0xA);
	cmpiw_m(c, c->a[6] + 4, 0x70, 16);
	if (c_lt(c)) {
		bs(c, 1);
	} else {
		bs(c, 0);
		lea_d(c, 0, c->a[0] + 0x190, 8);
	}
	moveq(c, 2, 6);
	moveq(c, 3, 5);
	movew_i_d(c, 6, 0);
	push16(c, (uint16_t)c->d[7]);
	hle_insn(c, 8);                                     /* move.w d7,-(sp) */
	movew_m_dc(c, 7, TILE_BASE, 16);
	hle_jump(c, 0x00DCD2);
}
/* sub_00DC9E: a0 = $5FA92 + 3 * (d0 & 12) */
static void panel_pick(hle_ctx *c)
{
	andiw_d(c, 0, 0xC);
	lea_d(c, 0, 0x5FA92, 12);
	movew_d_d(c, 2, 0);
	addw_dd(c, 0, 0);
	addw_dd(c, 0, 2);
	lea_d(c, 0, c->a[0] + (uint32_t)(int32_t)(int16_t)c->d[0], 12);
	panel_rect(c);
}
HLE_RANGE_PURE(0x00DCD6, L4PanelRectDone, 100, HLE_EXIT_SP(EXIT_NEXT, 2))
{
	set_w(&ctx->d[7], pop16(ctx));
	cc_logic16(ctx, ctx->d[7]);
	hle_insn(ctx, 8);                                   /* move.w (sp)+,d7 */
	exit_next(ctx);
}
/* $00DBF6: wait */
HLE_RANGE_PURE(0x00DBF6, L4PanelWait, 300, EXIT_NEXT)
{
	subq1w_m(ctx, A6 + 0x24, 16);
	if (c_gt(ctx)) {
		bs(ctx, 1);
	} else {
		bs(ctx, 0);
		movew_i_m(ctx, A6 + 0x24, 0x10);
	}
	exit_next(ctx);
}
/* $00DC06: open (not shootable); at timer $C8 a spawn routine (68k) runs after the exits */
HLE_RANGE_PURE(0x00DC06, L4PanelOpen, 500, HLE_EXIT_SP(0x00DC34, -4), HLE_EXIT_SP(0x00DC54, -4), EXIT_NEXT)
{
	subq1w_m(ctx, A6 + 0x24, 16);
	if (!c_z(ctx)) {
		bs(ctx, 1);
	} else {
		bs(ctx, 0);
		movew_i_m(ctx, A6 + 0x24, 0x10);
		bclr_m(ctx, A6, 7);
		movel_i_m(ctx, A6 + 0x20, 0xDC5E);
	}
	cmpiw_m(ctx, A6 + 0x24, 0xC8, 16);
	if (!c_z(ctx)) {                                    /* bne.s loc_00DC5A */
		bs(ctx, 1);
		exit_next(ctx);
		return;
	}
	bs(ctx, 0);
	cmpiw_m(ctx, A6 + 4, 0x68, 16);
	if (!c_lt(ctx)) {
		bs(ctx, 0);
		movew_m_d(ctx, 0, A6 + 2);
		push32(ctx, ctx->a[6]);
		cc_logic32(ctx, ctx->a[6]);
		hle_insn(ctx, 12);                          /* move.l a6,-(sp) */
		hle_jump(ctx, 0x00DC34);                    /* jsr (sub_00AC06,pc) */
		return;
	}
	bs(ctx, 1);
	cmpiw_m(ctx, OBJ_COUNT, 0x12, 20);
	if (c_gt(ctx)) {                                    /* bgt.s loc_00DC5A */
		bs(ctx, 1);
		exit_next(ctx);
		return;
	}
	bs(ctx, 0);
	movew_m_d(ctx, 5, A6 + 2);
	movew_m_d(ctx, 1, A6 + 4);
	subiw_d(ctx, 1, 0xA);
	push32(ctx, ctx->a[6]);
	cc_logic32(ctx, ctx->a[6]);
	hle_insn(ctx, 12);                                  /* move.l a6,-(sp) */
	hle_jump(ctx, 0x00DC54);                            /* jsr (sub_00A5FA,pc) */
}
/* $00DC5E: closed, waiting; opens (shootable) when the timer runs out */
HLE_RANGE_PURE(0x00DC5E, L4PanelClosed, 700, HLE_EXIT_SP(0x00DCD2, -2))
{
	subq1w_m(ctx, A6 + 0x24, 16);
	if (!c_z(ctx)) {
		bs(ctx, 1);
	} else {
		bs(ctx, 0);
		movew_i_m(ctx, A6 + 0x24, 0x100);
		bset_m(ctx, A6, 7);
		movel_i_m(ctx, A6 + 0x20, 0xDBF6);
	}
	movew_m_d(ctx, 0, A6 + 0x24);
	bra(ctx);                                           /* bra.w sub_00DC9E */
	panel_pick(ctx);
}
/* $00DC7E: opening animation */
HLE_RANGE_PURE(0x00DC7E, L4PanelOpening, 700, HLE_EXIT_SP(0x00DCD2, -2))
{
	subq1w_m(ctx, A6 + 0x24, 16);
	if (!c_z(ctx)) {
		bs(ctx, 1);
	} else {
		bs(ctx, 0);
		movew_i_m(ctx, A6 + 0x24, 0x100);
		bclr_m(ctx, A6, 7);
		movel_i_m(ctx, A6 + 0x20, 0xDC06);
	}
	movew_m_d(ctx, 0, A6 + 0x24);
	eoriw_d(ctx, 0, 0xF);
	panel_pick(ctx);
}
/* $00DCDC / $00DCE4: controller; every $186 frames a random panel (type $22, in the table at a1)
 * is opened; with no panels left the level script goes on (ScriptNext stays 68k) */
static void panel_ctl(hle_ctx *ctx)
{
	subq1w_m(ctx, A6 + 0x24, 16);
	if (c_z(ctx)) {                                     /* bne.s loc_00DD2C */
		bs(ctx, 0);
		moveq(ctx, 1, 0x12);
		for (;;) {                                  /* loc_00DCF2 */
			jsr(ctx, Random);
			andiw_d(ctx, 0, 7);
			aslw_d(ctx, 0, 2);
			ctx->d[2] = RAM32(ctx->a[1] + (uint32_t)(int32_t)(int16_t)ctx->d[0]);
			cc_logic32(ctx, ctx->d[2]);
			hle_insn(ctx, 18);                  /* move.l (0,a1,d0.w),d2 */
			if (c_pl(ctx)) {
				bs(ctx, 1);
			} else {
				bs(ctx, 0);
				ctx->a[2] = ctx->d[2];
				hle_insn(ctx, 4);           /* movea.l d2,a2 */
				set_w(&ctx->d[2], RAM16(ctx->a[2]));
				cc_logic16(ctx, ctx->d[2]);
				hle_insn(ctx, 8);           /* move.w (a2),d2 */
				alu(ctx, OP_SUB, ctx->d[2], 0x22, 8, 0);
				hle_insn(ctx, 8);           /* cmpi.b #$22,d2 */
				if (c_z(ctx)) {
					bs(ctx, 1);
					movew_i_m(ctx, ctx->a[2] + 0x24, 0x10);
					bclr_m(ctx, ctx->a[2], 7);
					movel_i_m(ctx, ctx->a[2] + 0x20, 0xDC7E);
					break;
				}
				bs(ctx, 0);
			}
			if (!dbf(ctx, 1)) {
				bra(ctx);                   /* bra.w loc_00DD26 */
				break;
			}
		}
		movew_i_m(ctx, A6 + 0x24, 0x186);
	} else {
		bs(ctx, 1);
	}
	tstw_m(ctx, 0xFFAF18, 16);
	if (!c_z(ctx)) {                                    /* bne.s loc_00DD60 */
		bs(ctx, 1);
		exit_next(ctx);
		return;
	}
	bs(ctx, 0);
	lea_d(ctx, 0, 0x1FCBA, 12);                         /* movea.l #$1FCBA,a0 */
	hle_jump(ctx, 0x00DD3A);                            /* jsr (ScriptNext,pc) ... */
}
HLE_RANGE_PURE(0x00DCDC, L4PanelCtl1, 1500, 0x00DD3A, EXIT_NEXT)
{
	lea_d(ctx, 1, 0xFFFFAF24, 12);
	bra(ctx);
	panel_ctl(ctx);
}
HLE_RANGE_PURE(0x00DCE4, L4PanelCtl2, 1500, 0x00DD3A, EXIT_NEXT)
{
	lea_d(ctx, 1, 0xFFFFAF04, 12);
	panel_ctl(ctx);
}
/* $00DD64: shot away: debris (68k), then the empty tiles */
HLE_RANGE_PURE(0x00DD64, L4PanelKill, 500, 0x00DD90)
{
	clr_obj_slot(ctx);
	lea_d(ctx, 1, 0xFFFFAF18, 12);
	cmpiw_m(ctx, A6 + 4, 0x70, 16);
	if (c_lt(ctx)) {
		bs(ctx, 1);
	} else {
		bs(ctx, 0);
		lea_d(ctx, 1, ctx->a[1] + 2, 8);
	}
	RAM16(ctx->a[1]) = (uint16_t)alu(ctx, OP_SUB, RAM16(ctx->a[1]), 1, 16, 1);
	hle_insn(ctx, 12);                                  /* subq.w #1,(a1) */
	movew_m_d(ctx, 0, A6 + 2);
	movew_m_d(ctx, 1, A6 + 4);
	subiw_d(ctx, 1, 0x14);
	movew_i_d(ctx, 2, 6);
	hle_jump(ctx, 0x00DD90);                            /* jsr (sub_01253C).l */
}
HLE_RANGE_PURE(0x00DD96, L4PanelKill2, 300, HLE_EXIT_SP(0x00DCD2, -2))
{
	lea_d(ctx, 0, 0x5FAC2, 12);
	bra(ctx);                                           /* bra.w sub_00DCB2 */
	panel_rect(ctx);
}

/* ================================================================== type $29 pop-up endoskeleton */
/* sub_00ED04: body parts by +$8 (the rise counter) */
static void sub_ed04(hle_ctx *c)
{
	cmpiw_m(c, c->a[6] + 8, 0xF, 16);
	if (c_gt(c)) {
		bs(c, 1);
	} else {
		bs(c, 0);
		q(c, 0x1B3);
	}
	cmpiw_m(c, c->a[6] + 8, 0xA, 16);
	if (c_gt(c)) {
		bs(c, 1);
	} else {
		bs(c, 0);
		q(c, 0x1B4);
	}
	q(c, 0x1B2);
	hle_insn(c, 16);                                    /* rts */
}
/* $00EC58: rising out of the ground */
HLE_RANGE_PURE(0x00EC58, L4PopRise, 500, EXIT_NEXT)
{
	subiw_m(ctx, A6 + 4, 8);
	q(ctx, 0x1B2);
	subq1w_m(ctx, A6 + 0x24, 16);
	if (!c_z(ctx)) {
		bs(ctx, 1);
	} else {
		bs(ctx, 0);
		movel_i_m(ctx, A6 + 0x20, 0xEC82);
		bset_m(ctx, A6, 7);
		movew_i_m(ctx, A6 + 0x24, 0x12C);
	}
	exit_next(ctx);
}
/* the tail of $00EC82 from loc_00ECCC */
static void pop_tail(hle_ctx *c)
{
	subq1w_m(c, c->a[6] + 0x24, 16);
	if (c_pl(c)) {
		bs(c, 1);
	} else {
		bs(c, 0);
		movel_i_m(c, c->a[6] + 0x20, 0xECE8);
		bclr_m(c, c->a[6], 7);
		movew_i_m(c, c->a[6] + 0x24, 0x10);
	}
	exit_next(c);
}
/* $00EC82: up and shooting; the warning sound + damage (sub_013D04, sub_009054) run as 68k */
HLE_RANGE_PURE(0x00EC82, L4PopShoot, 800, 0x00ECA8, EXIT_NEXT)
{
	scroll_kill(ctx, -0x28);
	movew_m_d(ctx, 0, A6 + 0x24);
	movew_d_d(ctx, 1, 0);
	andiw_d(ctx, 0, 0x40);
	if (!c_z(ctx)) {                                    /* bne.s loc_00ECC8 */
		bs(ctx, 1);
		hle_insn(ctx, 18);                          /* bsr.w sub_00ED04 */
		sub_ed04(ctx);
		pop_tail(ctx);
		return;
	}
	bs(ctx, 0);
	andiw_d(ctx, 1, 6);
	asrw_d(ctx, 1, 1);
	eoriw_d(ctx, 1, 3);
	if (c_z(ctx)) {                                     /* beq.s loc_00ECC8 */
		bs(ctx, 1);
		hle_insn(ctx, 18);
		sub_ed04(ctx);
		pop_tail(ctx);
		return;
	}
	bs(ctx, 0);
	movew_i_d(ctx, 0, 0x60);
	hle_jump(ctx, 0x00ECA8);                            /* jsr (sub_013D04).l ; bsr sub_009054 */
}
HLE_RANGE_PURE(0x00ECB2, L4PopShootAfterSound, 500, EXIT_NEXT)
{
	addiw_d(ctx, 1, 0x1B4);
	addqw_m(ctx, A6 + 4, 1);
	hle_insn(ctx, 18);                                  /* bsr.w ObjQueueSprite */
	ObjQueueSprite(ctx);
	hle_insn(ctx, 18);                                  /* bsr.w sub_00ED04 */
	sub_ed04(ctx);
	subq1w_m(ctx, A6 + 4, 16);
	bra(ctx);                                           /* bra.s loc_00ECCC */
	pop_tail(ctx);
}
/* $00ECE8: sinking back */
HLE_RANGE_PURE(0x00ECE8, L4PopSink, 500, EXIT_NEXT)
{
	addiw_m(ctx, A6 + 4, 8);
	hle_insn(ctx, 18);                                  /* bsr.w sub_00ED04 */
	sub_ed04(ctx);
	subq1w_m(ctx, A6 + 0x24, 16);
	if (!c_z(ctx)) {
		bs(ctx, 1);
	} else {
		bs(ctx, 0);
		clr_obj_slot(ctx);
	}
	exit_next(ctx);
}
/* $00ED2E: hit flash */
HLE_RANGE_PURE(0x00ED2E, L4PopHit, 500, EXIT_NEXT)
{
	jsr(ctx, ObjScrollX);
	q(ctx, 0x1B3);
	q(ctx, 0x1B4);
	q(ctx, 0x1B2);
	subq1w_m(ctx, A6 + 0x24, 16);
	if (!c_z(ctx)) {
		bs(ctx, 1);
	} else {
		bs(ctx, 0);
		movel_i_m(ctx, A6 + 0x20, 0xED5C);
	}
	exit_next(ctx);
}
/* $00ED5C: dead: free the slot, debris and sparks (all 68k) */
HLE_RANGE(0x00ED5C, L4PopKill, 17500, EXIT_NEXT)   /* observed max 11416 x 1.5 */
{
	clr_obj_slot(ctx);
	movem_xy(ctx);
	subiw_d(ctx, 1, 0x1E);
	call68(ctx, 0x01272A, 18);
	moveq(ctx, 5, 1);
	do {                                                /* loc_00ED74 */
		movem_xy(ctx);
		subiw_d(ctx, 1, 0x14);
		lea_d(ctx, 2, 0x12846, 8);
		call68(ctx, 0x01277C, 18);
		movem_xy(ctx);
		subiw_d(ctx, 1, 0x50);
		lea_d(ctx, 2, 0x1284E, 8);
		call68(ctx, 0x01277C, 18);
	} while (dbf(ctx, 5));
	movem_xy(ctx);
	movew_m_d(ctx, 2, A6 + 6);
	subq_d(ctx, 2, 1);
	subiw_d(ctx, 1, 0x1E);
	subiw_d(ctx, 0, 8);
	call68(ctx, SPAWN_DEBRIS, 20);
	movem_xy(ctx);
	movew_m_d(ctx, 2, A6 + 6);
	subq_d(ctx, 2, 1);
	subiw_d(ctx, 1, 0x3C);
	addiw_d(ctx, 0, 8);
	call68(ctx, SPAWN_DEBRIS, 20);
	exit_next(ctx);
}

/* ================================================================== type $23 orb */
/* sub_00F648: scroll, kill at -$C, queue d1, ObjNext */
static void orb_draw(hle_ctx *c)
{
	scroll_kill(c, -0xC);
	hle_insn(c, 18);                                    /* bsr.w ObjQueueSprite */
	ObjQueueSprite(c);
	exit_next(c);
}
/* sub_00F5C0: frame $215, or $249 when (+$26) != 0 */
static void orb_frame(hle_ctx *c)
{
	movew_i_d(c, 1, 0x215);
	tstw_m(c, c->a[6] + 0x26, 12);
	if (c_z(c)) {
		bs(c, 1);
	} else {
		bs(c, 0);
		movew_i_d(c, 1, 0x249);
	}
	bra(c);                                             /* bra.w sub_00F648 */
	orb_draw(c);
}
/* $00F540: drop to the rest height (+$28) */
HLE_RANGE_PURE(0x00F540, L4OrbDrop, 500, EXIT_NEXT)
{
	movew_m_d(ctx, 0, A6 + 4);
	addqw_d(ctx, 0, 8);
	cmpw_md(ctx, A6 + 0x28, 0);
	if (c_lt(ctx)) {
		bs(ctx, 1);
	} else {
		bs(ctx, 0);
		movel_i_m(ctx, A6 + 0x20, 0xF560);
		movew_m_d(ctx, 0, A6 + 0x28);
	}
	movew_d_m(ctx, 0, A6 + 4);
	bra(ctx);
	orb_frame(ctx);
}
/* $00F560: rise 2 per frame to 12 above the rest height */
HLE_RANGE_PURE(0x00F560, L4OrbRise, 500, EXIT_NEXT)
{
	movew_m_d(ctx, 0, A6 + 4);
	movew_m_d(ctx, 1, A6 + 0x28);
	subiw_d(ctx, 1, 0xC);
	subiw_d(ctx, 0, 2);
	cmpw_dd(ctx, 1, 0);
	if (c_gt(ctx)) {
		bs(ctx, 1);
	} else {
		bs(ctx, 0);
		movel_i_m(ctx, A6 + 0x20, 0xF584);
	}
	movew_d_m(ctx, 0, A6 + 4);
	bra(ctx);
	orb_frame(ctx);
}
/* $00F584: and back down */
HLE_RANGE_PURE(0x00F584, L4OrbSettle, 500, EXIT_NEXT)
{
	movew_m_d(ctx, 0, A6 + 4);
	addiw_d(ctx, 0, 2);
	cmpw_md(ctx, A6 + 0x28, 0);
	if (c_lt(ctx)) {
		bs(ctx, 1);
	} else {
		bs(ctx, 0);
		movel_i_m(ctx, A6 + 0x20, 0xF5AC);
		movew_i_m(ctx, A6 + 0x24, 0x64);
		movew_m_d(ctx, 0, A6 + 0x28);
	}
	movew_d_m(ctx, 0, A6 + 4);
	bra(ctx);
	orb_frame(ctx);
}
/* $00F5AC: wait */
HLE_RANGE_PURE(0x00F5AC, L4OrbWait, 500, EXIT_NEXT)
{
	subq1w_m(ctx, A6 + 0x24, 16);
	if (!c_z(ctx)) {
		bs(ctx, 1);
	} else {
		bs(ctx, 0);
		movew_i_m(ctx, A6 + 0x24, 0x3F);
		movel_i_m(ctx, A6 + 0x20, 0xF5D2);
	}
	orb_frame(ctx);
}
/* $00F5D2: charging blink; at 0 the shot spawner (sub_009A5E, 68k) and the active orb state */
HLE_RANGE_PURE(0x00F5D2, L4OrbCharge, 700, HLE_EXIT_SP(0x00F5FA, -2), EXIT_NEXT)
{
	movew_m_d(ctx, 1, A6 + 0x24);
	asrw_d(ctx, 1, 2);
	{
		uint8_t b = ROM8(0xF638u + (uint32_t)(int32_t)(int16_t)ctx->d[1]);
		set_b(&ctx->d[1], b);
		cc_logic8(ctx, b);
		hle_insn(ctx, 14);                          /* move.b (dat_00F638,pc,d1.w),d1 */
	}
	addiw_d(ctx, 1, 0x216);
	tstw_m(ctx, A6 + 0x26, 12);
	if (c_z(ctx)) {
		bs(ctx, 1);
	} else {
		bs(ctx, 0);
		addiw_d(ctx, 1, 0x34);
	}
	push16(ctx, (uint16_t)ctx->d[1]);
	hle_insn(ctx, 8);                                   /* move.w d1,-(sp) */
	subq1w_m(ctx, A6 + 0x24, 16);
	if (!c_z(ctx)) {
		bs(ctx, 1);
		set_w(&ctx->d[1], pop16(ctx));
		cc_logic16(ctx, ctx->d[1]);
		hle_insn(ctx, 8);                           /* move.w (sp)+,d1 */
		bra(ctx);                                   /* bra.s sub_00F648 */
		orb_draw(ctx);
		return;
	}
	bs(ctx, 0);
	movew_m_d(ctx, 5, A6 + 2);
	movew_m_d(ctx, 1, A6 + 4);
	hle_jump(ctx, 0x00F5FA);                            /* bsr.w sub_009A5E, then $00DDBA */
}
/* $00F65A: shot: sparks, debris, free the slot (all 68k) */
HLE_RANGE(0x00F65A, L4OrbKill, 9300, EXIT_NEXT)   /* observed max 6128 x 1.5 */
{
	moveq(ctx, 5, 3);
	do {
		movem_xy(ctx);
		subiw_d(ctx, 1, 0x18);
		lea_d(ctx, 2, 0x1287E, 8);
		tstw_m(ctx, A6 + 0x26, 12);
		if (c_pl(ctx)) {
			bs(ctx, 1);
		} else {
			bs(ctx, 0);
			lea_d(ctx, 2, 0x12876, 8);
		}
		call68(ctx, 0x01277C, 18);
	} while (dbf(ctx, 5));
	movew_m_d(ctx, 0, A6 + 2);
	movew_m_d(ctx, 1, A6 + 4);
	movew_m_d(ctx, 2, A6 + 6);
	subiw_d(ctx, 1, 0x18);
	call68(ctx, SPAWN_DEBRIS, 18);
	clr_obj_slot(ctx);
	exit_next(ctx);
}
/* $00E0A4: the same for the active orb (sparks 32 up) */
HLE_RANGE(0x00E0A4, L4OrbKill2, 10600, EXIT_NEXT)   /* observed max 7064 x 1.5 */
{
	moveq(ctx, 5, 3);
	do {
		movew_m_d(ctx, 0, A6 + 2);
		movew_m_d(ctx, 1, A6 + 4);
		subiw_d(ctx, 1, 0x20);
		lea_d(ctx, 2, 0x1287E, 8);
		tstw_m(ctx, A6 + 0x26, 12);
		if (c_pl(ctx)) {
			bs(ctx, 1);
		} else {
			bs(ctx, 0);
			lea_d(ctx, 2, 0x12876, 8);
		}
		call68(ctx, 0x01277C, 18);
	} while (dbf(ctx, 5));
	movew_m_d(ctx, 0, A6 + 2);
	movew_m_d(ctx, 1, A6 + 4);
	movew_m_d(ctx, 2, A6 + 6);
	subiw_d(ctx, 1, 0x18);
	call68(ctx, SPAWN_DEBRIS, 18);
	clr_obj_slot(ctx);
	bra(ctx);                                           /* bra.w sub_00E146 */
	sub_e146(ctx);
}

/* sub_00DDC8: d1 = (+$14), (+$7) = (d1 >> 4) + $1E, frame $182 + (d1 >> 5), draw */
static void orb_grow_draw(hle_ctx *c)
{
	movew_m_d(c, 1, c->a[6] + 0x14);
	movew_d_d(c, 2, 1);
	asrw_d(c, 2, 4);
	addiw_d(c, 2, 0x1E);
	moveb_d_m(c, 2, c->a[6] + 7);
	asrw_d(c, 1, 5);
	addiw_d(c, 1, 0x182);
	hle_insn(c, 18);                                    /* jsr (sub_00E0E8,pc) */
	sub_e0e8(c);
}
/* $00DDBA: orb after the charge: follows its path (sub_00C48E) */
HLE_RANGE_PURE(0x00DDBA, L4OrbPath, 800, EXIT_NEXT)
{
	hle_insn(ctx, 18);                                  /* jsr (sub_00C48E,pc) */
	sub_c48e(ctx);
	cc_logic8(ctx, (uint8_t)ctx->d[4]);
	hle_insn(ctx, 4);                                   /* tst.b d4 */
	if (c_z(ctx)) {                                     /* beq.w loc_00DDE6 */
		bw(ctx, 1);
		movew_i_m(ctx, A6 + 0x24, 0xFA);
		movel_i_m(ctx, A6 + 0x20, 0xDEBA);
		bset_m(ctx, A6, 7);
		moveb_i_m(ctx, A6 + 0x12, 0xC);
		moveb_i_m(ctx, A6 + 0x10, 4);
		bra(ctx);                                   /* bra.w loc_00DDC8 */
		orb_grow_draw(ctx);
		bra(ctx);                                   /* bra.w sub_00E146 */
		sub_e146(ctx);
		return;
	}
	bw(ctx, 0);
	if (c_pl(ctx)) {                                    /* bpl.w loc_00DDE2 */
		bw(ctx, 1);
		bra(ctx);                                   /* bra.w sub_00E146 */
		sub_e146(ctx);
		return;
	}
	bw(ctx, 0);
	orb_grow_draw(ctx);
	bra(ctx);
	sub_e146(ctx);
}

/* the speed limits of the two bouncing orb states: after sub_00D236 the X position (+$C) and
 * the speed (+$A) are clamped; (hi_c, hi_a) = ($64,$96) idle, ($78,$C8) attacking */
static void orb_move(hle_ctx *c, uint16_t lim_c, uint8_t fix_c, uint16_t lim_a, uint8_t fix_a)
{
	hle_insn(c, 18);                                    /* jsr (sub_00D236,pc) */
	sub_d236(c);
	moveq(c, 0, 0);
	moveb_m_d(c, 0, c->a[6] + 0xC);
	cmpiw_d(c, 0, lim_c);
	if (c_le(c)) {
		bs(c, 1);
	} else {
		bs(c, 0);
		negb_m(c, c->a[6] + 0x12);
		moveb_i_m(c, c->a[6] + 0xC, fix_c);
	}
	hle_insn(c, 18);                                    /* jsr (sub_00E108,pc) */
	sub_e108(c);
	moveq(c, 0, 0);
	moveb_m_d(c, 0, c->a[6] + 0xA);
	cmpiw_d(c, 0, lim_a);
	if (c_le(c)) {
		bs(c, 1);
	} else {
		bs(c, 0);
		negb_m(c, c->a[6] + 0x10);
		moveb_i_m(c, c->a[6] + 0xA, fix_a);
	}
}
/* $00DE08: idle orb */
HLE_RANGE_PURE(0x00DE08, L4OrbIdle, 900, EXIT_NEXT)
{
	scroll_kill(ctx, -0x28);
	subq1w_m(ctx, A6 + 0x24, 16);
	if (!c_z(ctx)) {
		bs(ctx, 1);
	} else {
		bs(ctx, 0);
		movew_i_m(ctx, A6 + 0x14, 0xFF);
		movel_i_m(ctx, A6 + 0x20, 0xDF9C);
	}
	orb_move(ctx, 0x64, 0x64, 0x96, 0x96);
	movew_i_d(ctx, 1, 0x189);
	hle_insn(ctx, 18);                                  /* jsr (sub_00E0E8,pc) */
	sub_e0e8(ctx);
	lea_d(ctx, 2, 0xE026, 12);                          /* movea.l #$E026,a2 */
	bra(ctx);                                           /* bra.w sub_00E146 */
	sub_e146(ctx);
}
/* sub_00DE6C: 1 in 256 a shot is fired (sub_009A7A / sub_009A5E, 68k: the range ends before them,
 * with the return address of the bsr on the stack). Returns 1 when it jumped away. */
static int sub_de6c(hle_ctx *c)
{
	jsr(c, Random);
	set_b(&c->d[0], (uint8_t)c->d[0]);
	cc_logic8(c, (uint8_t)c->d[0]);
	hle_insn(c, 8);                                     /* andi.b #$ff,d0 */
	if (!c_z(c)) {
		bs(c, 1);
		hle_insn(c, 16);                            /* rts */
		return 0;
	}
	bs(c, 0);
	movew_m_d(c, 5, c->a[6] + 2);
	movew_m_d(c, 1, c->a[6] + 4);
	subq1w_m(c, c->a[6] + 0x28, 16);
	if (!c_z(c)) {
		bs(c, 1);
		hle_jump(c, 0x00DE9C);                      /* jsr (sub_009A5E,pc) */
		return 1;
	}
	bs(c, 0);
	moveq(c, 3, -0x28);
	cmpiw_m(c, c->a[6] + 2, 0xA0, 16);
	if (c_lt(c)) {
		bs(c, 1);
	} else {
		bs(c, 0);
		movew_i_d(c, 3, 0x168);
	}
	movew_i_d(c, 4, 0x64);
	hle_jump(c, 0x00DE96);                              /* jsr (sub_009A7A,pc) */
	return 1;
}
/* loc_00DF34: draw, a2 = $DFF0, bsr sub_00DE6C, bra sub_00E146 */
static void orb_attack_end(hle_ctx *c)
{
	hle_insn(c, 18);                                    /* jsr (sub_00E0E8,pc) */
	sub_e0e8(c);
	lea_d(c, 2, 0xDFF0, 12);
	hle_insn(c, 18);                                    /* bsr.w sub_00DE6C */
	push32(c, 0x00DF42);
	if (sub_de6c(c)) {
		return;
	}
	(void)pop32(c);
	bra(c);                                             /* bra.w sub_00E146 */
	sub_e146(c);
}
/* $00DEBA: attacking orb; the warning sound + damage (sub_013D04, sub_009054) run as 68k */
HLE_RANGE_PURE(0x00DEBA, L4OrbAttack, 1500, HLE_EXIT_SP(0x00DE96, -4), HLE_EXIT_SP(0x00DE9C, -4), 0x00DF26, EXIT_NEXT)
{
	scroll_kill(ctx, -0x28);
	subq1w_m(ctx, A6 + 0x24, 16);
	if (!c_z(ctx)) {
		bs(ctx, 1);
	} else {
		bs(ctx, 0);
		movew_i_m(ctx, A6 + 0x14, 0);
		movel_i_m(ctx, A6 + 0x20, 0xDF46);
	}
	orb_move(ctx, 0x78, 0x78, 0xC8, 0xC8);
	movew_i_d(ctx, 1, 0x182);
	movew_m_d(ctx, 0, A6 + 0x24);
	movew_d_d(ctx, 2, 0);
	andiw_d(ctx, 0, 0x180);
	if (!c_z(ctx)) {                                    /* bne.s loc_00DF34 */
		bs(ctx, 1);
		orb_attack_end(ctx);
		return;
	}
	bs(ctx, 0);
	andiw_d(ctx, 2, 4);
	if (c_z(ctx)) {                                     /* beq.s loc_00DF34 */
		bs(ctx, 1);
		orb_attack_end(ctx);
		return;
	}
	bs(ctx, 0);
	movew_i_d(ctx, 0, 0x60);
	hle_jump(ctx, 0x00DF26);                            /* jsr (sub_013D04).l ; bsr sub_009054 */
}
HLE_RANGE_PURE(0x00DF30, L4OrbAttackAfterSound, 1000, HLE_EXIT_SP(0x00DE96, -4), HLE_EXIT_SP(0x00DE9C, -4), EXIT_NEXT)
{
	movew_i_d(ctx, 1, 0x181);
	orb_attack_end(ctx);
}
/* $00DF46: swelling up (+$14 grows by 8 until $100), then the idle state */
HLE_RANGE_PURE(0x00DF46, L4OrbGrow, 700, EXIT_NEXT)
{
	scroll_kill(ctx, -0x28);
	orb_grow_draw(ctx);
	addiw_m(ctx, A6 + 0x14, 8);
	cmpiw_m(ctx, A6 + 0x14, 0x100, 16);
	if (c_z(ctx)) {                                     /* beq.w loc_00DF7E */
		bw(ctx, 1);
		movew_i_m(ctx, A6 + 0x24, 0xFA);
		movel_i_m(ctx, A6 + 0x20, 0xDE08);
		moveb_i_m(ctx, A6 + 0x12, 9);
		moveb_i_m(ctx, A6 + 0x10, 3);
		bra(ctx);
		sub_e146(ctx);
		return;
	}
	bw(ctx, 0);
	bra(ctx);                                           /* bra.w sub_00E146 */
	sub_e146(ctx);
}
/* $00DF9C: shrinking (+$14 down by 8), then the attacking state */
HLE_RANGE_PURE(0x00DF9C, L4OrbShrink, 700, EXIT_NEXT)
{
	scroll_kill(ctx, -0x28);
	orb_grow_draw(ctx);
	subiw_m(ctx, A6 + 0x14, 8);
	if (c_mi(ctx)) {                                    /* bmi.w loc_00DFCE */
		bw(ctx, 1);
		movew_i_m(ctx, A6 + 0x24, 0xFA);
		movel_i_m(ctx, A6 + 0x20, 0xDEBA);
		moveb_i_m(ctx, A6 + 0x12, 0xC);
		moveb_i_m(ctx, A6 + 0x10, 4);
		bset_m(ctx, A6, 7);
		bra(ctx);
		sub_e146(ctx);
		return;
	}
	bw(ctx, 0);
	bra(ctx);
	sub_e146(ctx);
}
/* $00DFF0 / $00E026: orb after a hit: follows its path, then (path ended: d4 == 0) the next state */
static void orb_path_state(hle_ctx *ctx, uint16_t frame, uint32_t next)
{
	hle_insn(ctx, 18);                                  /* jsr (sub_00C48E,pc) */
	sub_c48e(ctx);
	cc_logic8(ctx, (uint8_t)ctx->d[4]);
	hle_insn(ctx, 4);                                   /* tst.b d4 */
	if (c_z(ctx)) {                                     /* beq.w */
		bw(ctx, 1);
		movel_i_m(ctx, A6 + 0x20, next);
		bset_m(ctx, A6, 7);
		moveb_i_m(ctx, A6 + 0x12, 0xC);
		moveb_i_m(ctx, A6 + 0x10, 4);
		bra(ctx);
	} else {
		bw(ctx, 0);
		if (c_pl(ctx)) {                            /* bpl.w */
			bw(ctx, 1);
			bra(ctx);                           /* bra.w sub_00E146 */
			sub_e146(ctx);
			return;
		}
		bw(ctx, 0);
	}
	movew_i_d(ctx, 1, frame);
	hle_insn(ctx, 18);                                  /* jsr (sub_00E0E8,pc) */
	sub_e0e8(ctx);
	bra(ctx);                                           /* bra.w sub_00E146 */
	sub_e146(ctx);
}
HLE_RANGE_PURE(0x00DFF0, L4OrbPathA, 800, EXIT_NEXT) { orb_path_state(ctx, 0x182, 0xDEBA); }
HLE_RANGE_PURE(0x00E026, L4OrbPathB, 800, EXIT_NEXT) { orb_path_state(ctx, 0x189, 0xDE08); }
/* $00E05C: dying-orb flicker (QueueSprite with the +$7 depth) */
HLE_RANGE_PURE(0x00E05C, L4OrbFlicker, 800, EXIT_NEXT)
{
	scroll_kill(ctx, -0x28);
	subq1w_m(ctx, A6 + 0x24, 16);
	if (!c_z(ctx)) {
		bs(ctx, 1);
	} else {
		bs(ctx, 0);
		movew_i_m(ctx, A6 + 0x24, 0x64);
		movel_i_m(ctx, A6 + 0x20, 0xDEBA);
		bset_m(ctx, A6, 7);
	}
	movew_i_d(ctx, 1, 0x182);
	movem_xy23(ctx);
	moveq(ctx, 5, 0);
	moveb_m_d(ctx, 5, A6 + 7);
	addqw_d(ctx, 2, 3);
	movew_m_d(ctx, 0, A6 + 0x24);
	andiw_d(ctx, 0, 2);
	if (c_z(ctx)) {
		bs(ctx, 1);
	} else {
		bs(ctx, 0);
		set_w(&ctx->d[2], alu(ctx, OP_SUB, ctx->d[2], 6, 16, 1));
		hle_insn(ctx, 4);                           /* subq.w #6,d2 */
	}
	hle_insn(ctx, 18);                                  /* jsr (sub_00E0F8,pc) */
	sub_e0f8(ctx);
	bra(ctx);                                           /* bra.w sub_00E146 */
	sub_e146(ctx);
}

/* ================================================================== hit routines */
/* lea box,a3 ; lea onhit,a2 ; jmp HitBox  (the +$1C routines) */
static void hit_lea(hle_ctx *ctx, uint32_t box, uint32_t onhit)
{
	ctx->a[3] = box;
	ctx->a[2] = onhit;
	HLE_SEQ(ctx, 8, 8, 10);
	HitBox(ctx);
}
HLE_FUNC_PURE(0x006326, L4HitBox, 300)   { hit_lea(ctx, 0x6354, 0x6332); }
HLE_FUNC_PURE(0x00638E, L4HitLamp, 300)  { hit_lea(ctx, 0x63B8, 0x639A); }
HLE_FUNC_PURE(0x006424, L4HitPanel, 300) { hit_lea(ctx, 0x644E, 0x6430); }
HLE_FUNC_PURE(0x006456, L4HitMover, 300) { hit_lea(ctx, 0x6480, 0x6462); }
HLE_FUNC_PURE(0x0067C6, L4HitOrbA, 300)  { hit_lea(ctx, 0x67EA, 0x67D2); }
HLE_FUNC_PURE(0x006900, L4HitOrbB, 300)  { hit_lea(ctx, 0x69B4, 0x690C); }
/* $0067F2 pop-up endoskeleton: two boxes (head, body) */
HLE_FUNC_PURE(0x0067F2, L4HitPop, 600)
{
	ctx->a[3] = 0x6832;
	ctx->a[2] = 0x6806;
	HLE_SEQ(ctx, 8, 8, 18);                             /* lea, lea, jsr HitBox */
	HitBox(ctx);
	ctx->a[3] = 0x682A;
	HLE_SEQ(ctx, 8, 10);                                /* lea, jmp HitBox */
	HitBox(ctx);
}

/* ================================================================== type $24 kill states (once each) */
/* the six debris spawns at the offsets of the table at `tab`, then the slot is freed */
static void mover_debris(hle_ctx *c, uint32_t tab)
{
	moveq(c, 5, 5);
	lea_d(c, 1, tab, 8);
	do {
		movew_m_d(c, 0, c->a[6] + 2);
		movew_m_d(c, 1, c->a[6] + 4);
		set_w(&c->d[0], alu(c, OP_ADD, c->d[0], ROM16(c->a[1]), 16, 1));
		c->a[1] += 2;
		hle_insn(c, 8);                             /* add.w (a1)+,d0 */
		set_w(&c->d[1], alu(c, OP_ADD, c->d[1], ROM16(c->a[1]), 16, 1));
		c->a[1] += 2;
		hle_insn(c, 8);                             /* add.w (a1)+,d1 */
		movew_i_d(c, 2, 6);
		call68(c, SPAWN_DEBRIS, 18);
	} while (dbf(c, 5));
	clr_obj_slot(c);
}
/* $00E174 ($24 shot, first variant): debris, then 13 rows of fill (VdpQueueFillRows) */
HLE_RANGE(0x00E174, L4MoverKill1, 7900, EXIT_NEXT)   /* observed max 5282 x 1.5 */
{
	jsr(ctx, ObjScrollX);
	mover_debris(ctx, 0xE1B2);
	movew_m_d(ctx, 1, A6 + 0xA);
	movew_i_d(ctx, 2, 0x10);
	movew_i_d(ctx, 3, 0xD);
	call68(ctx, 0x012212, 18);
	exit_next(ctx);
}
/* $00E1CA ($24 shot, second variant): debris, then new tiles or fill, by +$2A */
HLE_RANGE(0x00E1CA, L4MoverKill2, 14500, EXIT_NEXT)   /* observed max 9626 x 1.5 */
{
	jsr(ctx, ObjScrollX);
	mover_debris(ctx, 0xE228);
	lea_d(ctx, 0, 0x666A4, 12);
	tstw_m(ctx, A6 + 0x2A, 12);
	if (!c_mi(ctx)) {                                   /* bmi.s loc_00E206 */
		bs(ctx, 0);
		if (!c_z(ctx)) {                            /* bne.s loc_00E240 */
			bs(ctx, 1);
			movew_m_d(ctx, 1, A6 + 0xA);
			movew_i_d(ctx, 2, 0x10);
			movew_i_d(ctx, 3, 0xC);
			call68(ctx, 0x012212, 18);
			exit_next(ctx);
			return;
		}
		bs(ctx, 0);
		lea_d(ctx, 0, ctx->a[0] + 0x3C0, 8);
	} else {
		bs(ctx, 1);
	}
	movew_m_d(ctx, 1, A6 + 0xA);
	movew_i_d(ctx, 2, 8);
	movew_i_d(ctx, 3, 0xC);
	movew_i_d(ctx, 6, 0x8000);
	push16(ctx, (uint16_t)ctx->d[7]);
	hle_insn(ctx, 8);                                   /* move.w d7,-(sp) */
	movew_m_dc(ctx, 7, 0xFF9E38, 16);
	call68(ctx, TILE_RECT, 18);
	set_w(&ctx->d[7], pop16(ctx));
	cc_logic16(ctx, ctx->d[7]);
	hle_insn(ctx, 8);                                   /* move.w (sp)+,d7 */
	exit_next(ctx);
}
/* $006488 ($24 variant b): box by +$2A */
HLE_FUNC_PURE(0x006488, L4HitMoverB, 300)
{
	ctx->a[3] = 0x64D0;
	hle_insn(ctx, 8);
	cmpiw_m(ctx, A6 + 0x2A, 1, 16);
	if (!c_z(ctx)) {
		bs(ctx, 1);
	} else {
		bs(ctx, 0);
		ctx->a[3] = 0x64D8;
		hle_insn(ctx, 8);
	}
	ctx->a[2] = 0x64A0;
	hle_insn(ctx, 8);
	hle_insn(ctx, 10);                                  /* jmp HitBox */
	HitBox(ctx);
}

