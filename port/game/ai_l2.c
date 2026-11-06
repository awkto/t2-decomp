/* AI states and hit routines of the level-2 (human hideout) object types. docs/objects_types.md
 * ("Level 2"), src/t2.s.
 *
 *   type $26 infiltrator (three variants): walk $00EFA4/$00F18A/$00EDD4, stand+shoot $00EFFE/
 *            $00F1E4/$00EE2E, ceiling drop $00F69C/$00F6CA, $00F6F4/$00F722, death starts
 *            $00F094/$00F27A/$00EED4 with the fall states that follow ($00F0CA, $00F066, ...)
 *   type $20 ceiling lamp:   $00EA6A, $00EAEC, $0151F0 (+ the kill states $00EA72, $0151CC, ...)
 *   type $13 (level 2):      $00EAA8, $00EAD6, $00EAF4, $015212, $01522A
 *   type $0F (level 2):      $00EA26, $00EB2E, $00EB4C, $00EB62, $00EB96, $00EB9E, ... + kill starts
 *   hit tests (+$1C): $006512 $006544 $006576 $0065A4 $0065D2 $0065FC $006652 $0066A8 $00683A
 *            $0068CA $006894 $00597A
 *
 * Each state is an HLE_RANGE_PURE from its entry (reached by the `jmp (a0)` of ObjRunAll/ObjNext)
 * to the `bra.w ObjNext` ($00B3BE, a C range itself). C callees (ObjMoveX, ObjScrollX, ObjKillIf*,
 * ObjQueueSprite, Random, HitBox) are called directly after the jsr is charged. Where a state calls
 * 68k code that is not C (the warning sound sub_013D04 + PlayerDamageNearest sub_009054, the
 * debris spawners sub_01253C, sub_012A9C, ...) the range ends right BEFORE that `jsr` (the real 68k
 * runs it) and another range registered at the return address continues, as in ai_endo.c.
 * Costs are the GPGX 68000 costs (see ai_endo.c). */
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
#define ENDO_FLAG 0xFF9EC0u
#define VDP_LIST_PTR 0xFF80BCu
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
static void btst_m(hle_ctx *c, uint32_t a, int bit, int cost)
{
	cc_set(c, CC_Z, ((RAM8(a) >> bit) & 1) ? 0 : CC_Z);
	hle_insn(c, cost);
}
static void btst_d(hle_ctx *c, int r, int bit)
{
	cc_set(c, CC_Z, ((c->d[r] >> bit) & 1) ? 0 : CC_Z);
	hle_insn(c, 10);
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

static inline int16_t sx(uint32_t v) { return (int16_t)(v & 0xFFFF); }
static inline uint16_t rom16(uint32_t a) { return ROM16(a); }
static inline uint8_t rom8(uint32_t a) { return ROM8(a); }
static inline uint16_t rd16(uint32_t a)
{
	return ((a & 0xFFFFFFu) >= 0xFF0000u) ? RAM16(a) : ROM16(a);
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
#define L2_FLAG 0xFF9EBEu          /* the infiltrator death flag (clr.b / st.b) */
#define SHOT_DAMAGE 0xFFA0CAu
#define VDP_GFX_COUNT 0xFF9E60u

static int c_ge(hle_ctx *c) { return c_n(c) == c_v(c); }

static void exit_jmp(hle_ctx *c)
{
	hle_insn(c, 12);   /* jmp (ObjNext).l */
	hle_jump(c, EXIT_NEXT);
}
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
static void clr_flag(hle_ctx *ctx)                /* clr.b (L2_FLAG).l */
{
	RAM8(L2_FLAG) = 0;
	cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
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
/* sub_00EC16 (d1 = frame already): scroll, kill left of -$20, queue, ObjNext */
static void tail_ec16(hle_ctx *c)
{
	scroll_kill(c, -0x20);
	jsr(c, ObjQueueSprite);
	exit_next(c);
}
/* "scroll, kill at k, queue frame" state */
static void plain_state(hle_ctx *c, int k, uint16_t frame)
{
	scroll_kill(c, k);
	q(c, frame);
	exit_next(c);
}
/* a state that is "move.w #frame,d1; bra.w sub_00EC16" */
static void to_ec16(hle_ctx *c, uint16_t frame)
{
	movew_i_d(c, 1, frame);
	hle_insn(c, 10);
	tail_ec16(c);
}

/* ================================================================== type $0F / $20 / $13 states */
HLE_RANGE_PURE(0x00EA26, L2Crate, 300, EXIT_NEXT)      { to_ec16(ctx, 0x197); }
HLE_RANGE_PURE(0x00EA6A, L2Lamp, 300, EXIT_NEXT)       { to_ec16(ctx, 0x199); }
HLE_RANGE_PURE(0x00EAEC, L2Lamp2, 300, EXIT_NEXT)      { to_ec16(ctx, 0x19F); }
HLE_RANGE_PURE(0x00EB2E, L2Crate2, 300, EXIT_NEXT)     { to_ec16(ctx, 0x19B); }
HLE_RANGE_PURE(0x00EB96, L2Crate3, 300, EXIT_NEXT)     { to_ec16(ctx, 0x19E); }
HLE_RANGE_PURE(0x00EC16, L2Tail, 300, EXIT_NEXT)       { tail_ec16(ctx); }

HLE_RANGE_PURE(0x00EA54, L2St_EA54, 300, EXIT_NEXT)    { plain_state(ctx, -0x20, 0x198); }
HLE_RANGE_PURE(0x00EA92, L2St_EA92, 300, EXIT_NEXT)    { plain_state(ctx, -0x20, 0x19A); }
HLE_RANGE_PURE(0x00EB4C, L2St_EB4C, 300, EXIT_NEXT)    { plain_state(ctx, -0x20, 0x19C); }
HLE_RANGE_PURE(0x00EB62, L2St_EB62, 300, EXIT_NEXT)    { plain_state(ctx, -0x20, 0x19D); }
HLE_RANGE_PURE(0x00EBBC, L2St_EBBC, 300, EXIT_NEXT)    { plain_state(ctx, -0x20, 0x1A0); }
HLE_RANGE_PURE(0x00EAD6, L2Human_EAD6, 300, EXIT_NEXT) { plain_state(ctx, -0x32, 0x380); }

/* $00EAA8 human (level 2): timer--, scroll, kill, body $380, head $1A1 (+1 when timer & $1C == 0) */
HLE_RANGE_PURE(0x00EAA8, L2Human, 400, EXIT_NEXT)
{
	subq1w_m(ctx, A6 + 0x24, 16);
	scroll_kill(ctx, -0x32);
	q(ctx, 0x380);
	movew_i_d(ctx, 1, 0x1A1);
	movew_m_d(ctx, 0, A6 + 0x24);
	andiw_d(ctx, 0, 0x1C);
	if (!c_z(ctx)) {
		bs(ctx, 1);
	} else {
		bs(ctx, 0);
		set_w(&ctx->d[1], alu(ctx, OP_ADD, ctx->d[1], 1, 16, 1));
		hle_insn(ctx, 4);                       /* addq.w #1,d1 */
	}
	jsr(ctx, ObjQueueSprite);
	exit_next(ctx);
}

/* $00EAF4 human flinch: scroll, kill, timer--, on 0 -> $EAA8 + shootable; body, head blinks */
HLE_RANGE_PURE(0x00EAF4, L2HumanFlinch, 400, EXIT_NEXT)
{
	scroll_kill(ctx, -0x32);
	subq1w_m(ctx, A6 + 0x24, 16);
	if (!c_z(ctx)) {
		bs(ctx, 1);
	} else {
		bs(ctx, 0);
		movel_i_m(ctx, A6 + 0x20, 0xEAA8);
		bset_m(ctx, A6, 7);
	}
	q(ctx, 0x380);
	movew_m_d(ctx, 0, A6 + 0x24);
	btst_d(ctx, 0, 2);
	if (!c_z(ctx)) {
		bs(ctx, 1);
	} else {
		bs(ctx, 0);
		q(ctx, 0x1A1);
	}
	exit_next(ctx);
}

/* $00EB9E: move, queue $1A0, timer--; on 0 -> $EBBC */
HLE_RANGE_PURE(0x00EB9E, L2Crate4, 400, EXIT_NEXT)
{
	jsr(ctx, ObjMoveX);
	q(ctx, 0x1A0);
	subq1w_m(ctx, A6 + 0x24, 16);
	if (!c_z(ctx)) {
		bs(ctx, 1);
	} else {
		bs(ctx, 0);
		movel_i_m(ctx, A6 + 0x20, 0xEBBC);
	}
	exit_next(ctx);
}

/* type $20 lamp (level 2 variant) and the human states in $0151xx: jmp (ObjNext).l */
static void tail15(hle_ctx *c)
{
	scroll_kill(c, -0x20);
	jsr(c, ObjQueueSprite);
	exit_jmp(c);
}
HLE_RANGE_PURE(0x0151F0, L2Lamp3, 300, EXIT_NEXT)
{
	movew_i_d(ctx, 1, 0x374);                       /* falls into sub_0151F4 */
	tail15(ctx);
}
static void st_151ea(hle_ctx *c)
{
	movew_i_d(c, 1, 0x375);
	hle_insn(c, 10);
	tail15(c);
}
HLE_RANGE_PURE(0x0151EA, L2Lamp3b, 300, EXIT_NEXT)     { st_151ea(ctx); }
HLE_RANGE_PURE(0x015212, L2Human15, 300, EXIT_NEXT)
{
	movew_i_d(ctx, 1, 0x376);
	tail15(ctx);
}
HLE_RANGE_PURE(0x01522A, L2Human15b, 400, EXIT_NEXT)
{
	subq1w_m(ctx, A6 + 0x24, 16);
	if (!c_z(ctx)) {
		bs(ctx, 1);
	} else {
		bs(ctx, 0);
		movel_i_m(ctx, A6 + 0x20, 0x15212);
		bset_m(ctx, A6, 7);
	}
	movew_m_d(ctx, 0, A6 + 0x24);
	btst_d(ctx, 0, 2);
	if (c_z(ctx)) {
		bs(ctx, 1);                             /* beq.s sub_015212 */
		movew_i_d(ctx, 1, 0x376);
		tail15(ctx);
	} else {
		bs(ctx, 0);
		moveq(ctx, 1, 0);
		hle_insn(ctx, 10);                      /* bra.s sub_015216 */
		tail15(ctx);
	}
}
HLE_RANGE_PURE(0x015208, L2Human15kill, 300, EXIT_NEXT)
{
	clr_obj_slot(ctx);
	hle_insn(ctx, 10);                              /* bra.s sub_015224 */
	exit_jmp(ctx);
}

/* ---- kill starts: the debris spawners (68k) run for real between the ranges */
static void ea4c(hle_ctx *ctx)
{
	movel_i_m(ctx, A6 + 0x20, 0xEA54);
	plain_state(ctx, -0x20, 0x198);
}
HLE_RANGE_PURE(0x00EA2E, L2Kill_EA2E, 300, 0x00EA3E)
{
	movem_xy(ctx);
	movew_m_d(ctx, 2, A6 + 6);
	subq_d(ctx, 2, 1);
	addiw_d(ctx, 1, 0x20);
	hle_jump(ctx, 0x00EA3E);                        /* jsr sub_01253C */
}
HLE_RANGE_PURE(0x00EA42, L2Kill_EA42, 300, 0x00EA48, EXIT_NEXT)
{
	movew_m_d(ctx, 2, A6 + 0xA);
	if (c_z(ctx)) {
		bs(ctx, 1);
		ea4c(ctx);
	} else {
		bs(ctx, 0);
		hle_jump(ctx, 0x00EA48);                /* jsr sub_012A9C */
	}
}
HLE_RANGE_PURE(0x00EA4C, L2Kill_EA4C, 300, EXIT_NEXT)  { ea4c(ctx); }

HLE_RANGE_PURE(0x00EA72, L2Kill_EA72, 300, 0x00EA82)
{
	movem_xy(ctx);
	movew_m_d(ctx, 2, A6 + 6);
	subq_d(ctx, 2, 1);
	addiw_d(ctx, 1, 0x20);
	hle_jump(ctx, 0x00EA82);                        /* jsr sub_01253C, then jsr sub_012A78 */
}
HLE_RANGE_PURE(0x00EA8A, L2Kill_EA8A, 300, EXIT_NEXT)
{
	movel_i_m(ctx, A6 + 0x20, 0xEA92);
	plain_state(ctx, -0x20, 0x19A);
}
HLE_RANGE_PURE(0x00EB36, L2Kill_EB36, 300, 0x00EB40)
{
	movem_xy(ctx);
	subq_d(ctx, 1, 6);
	moveq(ctx, 2, 1);
	hle_jump(ctx, 0x00EB40);                        /* jsr sub_012A16 */
}
HLE_RANGE_PURE(0x00EB44, L2Kill_EB44, 300, EXIT_NEXT)
{
	movel_i_m(ctx, A6 + 0x20, 0xEB4C);
	plain_state(ctx, -0x20, 0x19C);
}
/* $00EB78: pushes a6 around the call (HLE_EXIT_SP) */
HLE_RANGE_PURE(0x00EB78, L2Kill_EB78, 300, HLE_EXIT_SP(0x00EB88, -4))
{
	movem_xy(ctx);
	subq_d(ctx, 1, 4);
	moveq(ctx, 2, 3);
	push32(ctx, ctx->a[6]);
	cc_logic32(ctx, ctx->a[6]);                     /* move.l a6,-(sp) sets N/Z */
	hle_insn(ctx, 12);
	movew_m_d(ctx, 3, A6 + 6);
	hle_jump(ctx, 0x00EB88);                        /* jsr sub_012A46 */
}
HLE_RANGE_PURE(0x00EB8C, L2Kill_EB8C, 300, HLE_EXIT_SP(EXIT_NEXT, 4))
{
	ctx->a[6] = pop32(ctx);
	hle_insn(ctx, 12);                              /* movea.l (sp)+,a6 */
	movel_i_m(ctx, A6 + 0x20, 0xEB96);
	to_ec16(ctx, 0x19E);
}
HLE_RANGE_PURE(0x00EBD2, L2Kill_EBD2, 300, 0x00EBDA)
{
	movem_xy(ctx);
	moveq(ctx, 2, 6);
	hle_jump(ctx, 0x00EBDA);                        /* jsr sub_012A16 */
}
HLE_RANGE_PURE(0x00EBDE, L2Kill_EBDE, 300, EXIT_NEXT)
{
	movel_i_m(ctx, A6 + 0x20, 0xEBBC);
	exit_next(ctx);
}
/* type $20 kill start $0151CC */
HLE_RANGE_PURE(0x0151CC, L2Kill_151CC, 300, 0x0151DC)
{
	movem_xy(ctx);
	movew_m_d(ctx, 2, A6 + 6);
	subq_d(ctx, 2, 1);
	set_w(&ctx->d[1], alu(ctx, OP_SUB, ctx->d[1], 0x14, 16, 1));
	hle_insn(ctx, 8);                               /* subi.w #$14,d1 */
	hle_jump(ctx, 0x0151DC);                        /* jsr (sub_01253C).l */
}
HLE_RANGE_PURE(0x0151E2, L2Kill_151E2, 300, EXIT_NEXT)
{
	movel_i_m(ctx, A6 + 0x20, 0x151EA);
	st_151ea(ctx);
}

/* ================================================================== type $26 infiltrator */
/* $00F170-style: state = next, timer = $50 + (rnd & $3f); rts */
static void rnd_state(hle_ctx *ctx, uint32_t state)
{
	movel_i_m(ctx, A6 + 0x20, state);
	jsr(ctx, Random);
	andiw_d(ctx, 0, 0x3F);
	addiw_d(ctx, 0, 0x50);
	RAM16(A6 + 0x24) = (uint16_t)ctx->d[0];
	cc_logic16(ctx, ctx->d[0]);
	hle_insn(ctx, 12);
	hle_insn(ctx, 16);                              /* rts */
}

/* walk: move, scroll, d5 = ((timer & $38) >> 3) ^ 7, frame by b14, kills, timer--, state */
static void walk26(hle_ctx *ctx, uint16_t fset, uint16_t fclr, int right_first, uint32_t next)
{
	jsr(ctx, ObjMoveX);
	jsr(ctx, ObjScrollX);
	movew_m_d(ctx, 5, A6 + 0x24);
	andiw_d(ctx, 5, 0x38);
	set_w(&ctx->d[5], lsr16(ctx, ctx->d[5] & 0xFFFF, 3));
	hle_insn(ctx, 12);
	eoriw_d(ctx, 5, 7);
	btst_m(ctx, A6, 6, 12);
	if (RAM8(A6) & 0x40) {
		bw(ctx, 0);
		movew_d_d(ctx, 1, 5);
		addiw_d(ctx, 1, fset);
		jsr(ctx, ObjQueueSprite);
		hle_insn(ctx, 10);                      /* bra.s */
	} else {
		bw(ctx, 1);
		movew_d_d(ctx, 1, 5);
		addiw_d(ctx, 1, fclr);
		jsr(ctx, ObjQueueSprite);
	}
	if (right_first) {
		movew_i_d(ctx, 0, 0x15E);
		jsr(ctx, ObjKillIfRightOf);
		moveq(ctx, 0, -0x28);
		jsr(ctx, ObjKillIfLeftOf);
	} else {
		moveq(ctx, 0, -0x28);
		jsr(ctx, ObjKillIfLeftOf);
		movew_i_d(ctx, 0, 0x168);
		jsr(ctx, ObjKillIfRightOf);
	}
	subq1w_m(ctx, A6 + 0x24, 16);
	if (!c_z(ctx)) {
		bs(ctx, 1);
	} else {
		bs(ctx, 0);
		movel_i_m(ctx, A6 + 0x20, next);
		movew_i_m(ctx, A6 + 0x24, 0x4B);
	}
	exit_next(ctx);
}
HLE_RANGE_PURE(0x00EFA4, L2InfWalk, 900, EXIT_NEXT)  { walk26(ctx, 0x1C3, 0x1E2, 1, 0xEFFE); }
HLE_RANGE_PURE(0x00F18A, L2InfWalk2, 900, EXIT_NEXT) { walk26(ctx, 0x1CD, 0x1EC, 1, 0xF1E4); }
HLE_RANGE_PURE(0x00EDD4, L2InfWalk3, 900, EXIT_NEXT) { walk26(ctx, 0x1B8, 0x1D7, 0, 0xEE2E); }

/* stand and shoot: the warning flash (timer in $14..$50, bit 2) calls the real sound + damage
 * routines, after which the range at the return address queues the warning frame + the body */
static void stand_body(hle_ctx *ctx, uint16_t fb1, uint16_t fb0)
{
	movew_i_d(ctx, 1, fb1);
	btst_m(ctx, A6, 6, 12);
	if (RAM8(A6) & 0x40) {
		bs(ctx, 1);
	} else {
		bs(ctx, 0);
		movew_i_d(ctx, 1, fb0);
	}
	jsr(ctx, ObjQueueSprite);
	exit_next(ctx);
}
static void stand_after_sound(hle_ctx *ctx, uint16_t fw1, uint16_t fw0, uint16_t fb1, uint16_t fb0)
{
	movew_i_d(ctx, 1, fw1);
	btst_m(ctx, A6, 6, 12);
	if (RAM8(A6) & 0x40) {
		bs(ctx, 1);
	} else {
		bs(ctx, 0);
		movew_i_d(ctx, 1, fw0);
	}
	jsr(ctx, ObjQueueSprite);
	stand_body(ctx, fb1, fb0);
}
static void stand26(hle_ctx *ctx, uint32_t walk_state, uint32_t sound_exit, uint16_t fb1, uint16_t fb0)
{
	jsr(ctx, ObjScrollX);
	subq1w_m(ctx, A6 + 0x24, 16);
	if (!c_z(ctx)) {
		bs(ctx, 1);
	} else {
		bs(ctx, 0);
		hle_insn(ctx, 18);                      /* jsr sub_00F170 / sub_00EE88 / sub_00F356 */
		rnd_state(ctx, walk_state);
	}
	movew_m_d(ctx, 0, A6 + 0x24);
	cmpiw_d(ctx, 0, 0x50);
	if (c_gt(ctx)) { bs(ctx, 1); stand_body(ctx, fb1, fb0); return; }
	bs(ctx, 0);
	cmpiw_d(ctx, 0, 0x14);
	if (c_lt(ctx)) { bs(ctx, 1); stand_body(ctx, fb1, fb0); return; }
	bs(ctx, 0);
	btst_d(ctx, 0, 2);
	if (c_z(ctx)) { bs(ctx, 1); stand_body(ctx, fb1, fb0); return; }
	bs(ctx, 0);
	movew_i_d(ctx, 0, 0x60);
	hle_jump(ctx, sound_exit);                      /* the real jsr sub_013D04 + bsr sub_009054 */
}
HLE_RANGE_PURE(0x00EFFE, L2InfStand, 900, 0x00F026, EXIT_NEXT)
{ stand26(ctx, 0xEFA4, 0xF026, 0x1CB, 0x1EA); }
HLE_RANGE_PURE(0x00F030, L2InfStandSnd, 600, EXIT_NEXT)
{ stand_after_sound(ctx, 0x1CC, 0x1EB, 0x1CB, 0x1EA); }
HLE_RANGE_PURE(0x00F1E4, L2InfStand2, 900, 0x00F20C, EXIT_NEXT)
{ stand26(ctx, 0xF18A, 0xF20C, 0x1D5, 0x1F4); }
HLE_RANGE_PURE(0x00F216, L2InfStand2Snd, 600, EXIT_NEXT)
{ stand_after_sound(ctx, 0x1D6, 0x1F5, 0x1D5, 0x1F4); }
HLE_RANGE_PURE(0x00EE2E, L2InfStand3, 900, 0x00EE56, EXIT_NEXT)
{ stand26(ctx, 0xEDD4, 0xEE56, 0x1C1, 0x1E0); }
HLE_RANGE_PURE(0x00EE60, L2InfStand3Snd, 600, EXIT_NEXT)
{ stand_after_sound(ctx, 0x1C2, 0x1E1, 0x1C1, 0x1E0); }

/* ceiling drop: y += 8 until it reaches (+$28), then the landing state */
static void drop26(hle_ctx *ctx, uint32_t land, uint16_t frame)
{
	movew_m_d(ctx, 0, A6 + 4);
	set_w(&ctx->d[0], alu(ctx, OP_ADD, ctx->d[0], 8, 16, 1));
	hle_insn(ctx, 4);                               /* addq.w #8,d0 */
	alu(ctx, OP_SUB, ctx->d[0], RAM16(A6 + 0x28), 16, 0);
	hle_insn(ctx, 12);                              /* cmp.w (0x28,a6),d0 */
	if (c_lt(ctx)) {
		bs(ctx, 1);
	} else {
		bs(ctx, 0);
		movel_i_m(ctx, A6 + 0x20, land);
		movew_i_m(ctx, A6 + 0x24, 0xF);
		movew_m_d(ctx, 0, A6 + 0x28);
	}
	RAM16(A6 + 4) = (uint16_t)ctx->d[0];
	cc_logic16(ctx, ctx->d[0]);
	hle_insn(ctx, 12);
	q(ctx, frame);
	exit_next(ctx);
}
static void land26(hle_ctx *ctx, uint16_t fbase, uint32_t stand)
{
	movew_m_d(ctx, 1, A6 + 0x24);
	set_w(&ctx->d[1], lsr16(ctx, ctx->d[1] & 0xFFFF, 3));
	hle_insn(ctx, 12);
	addiw_d(ctx, 1, fbase);
	jsr(ctx, ObjQueueSprite);
	subq1w_m(ctx, A6 + 0x24, 16);
	if (!c_z(ctx)) {
		bs(ctx, 1);
	} else {
		bs(ctx, 0);
		movel_i_m(ctx, A6 + 0x20, stand);
		movew_i_m(ctx, A6 + 0x24, 0x4B);
		bset_m(ctx, A6, 7);
	}
	exit_next(ctx);
}
HLE_RANGE_PURE(0x00F69C, L2InfDrop, 500, EXIT_NEXT)  { drop26(ctx, 0xF6CA, 0x22C); }
HLE_RANGE_PURE(0x00F6CA, L2InfLand, 500, EXIT_NEXT)  { land26(ctx, 0x22C, 0xEFFE); }
HLE_RANGE_PURE(0x00F6F4, L2InfDrop2, 500, EXIT_NEXT) { drop26(ctx, 0xF722, 0x22E); }
HLE_RANGE_PURE(0x00F722, L2InfLand2, 500, EXIT_NEXT) { land26(ctx, 0x22E, 0xF1E4); }

/* ---- death: init, fall (sub_00D236) and the VDP-list tile copy + sprite */
static void vdp_tail(hle_ctx *ctx, uint32_t tbl, uint32_t gfx, uint16_t base)
{
	ctx->a[5] = RAM32(VDP_LIST_PTR);
	hle_insn(ctx, 20);
	RAM16(ctx->a[5]) = 1;
	ctx->a[5] += 2;
	cc_logic16(ctx, 1);
	hle_insn(ctx, 12);
	movew_d_d(ctx, 0, 1);
	set_w(&ctx->d[0], asl16(ctx, ctx->d[0] & 0xFFFF, 2));
	ctx->a[1] = tbl;
	hle_insn(ctx, 8);
	set_w(&ctx->d[2], rom16(ctx->a[1] + (uint32_t)(int32_t)sx(ctx->d[0])));
	cc_logic16(ctx, ctx->d[2]);
	hle_insn(ctx, 14);
	set_w(&ctx->d[2], asr16(ctx, ctx->d[2] & 0xFFFF, 1));
	RAM16(ctx->a[5]) = (uint16_t)ctx->d[2];
	ctx->a[5] += 2;
	cc_logic16(ctx, ctx->d[2]);
	hle_insn(ctx, 8);
	set_w(&ctx->d[0], rom16(ctx->a[1] + 2 + (uint32_t)(int32_t)sx(ctx->d[0])));
	cc_logic16(ctx, ctx->d[0]);
	hle_insn(ctx, 14);
	ctx->a[1] = gfx;
	hle_insn(ctx, 12);
	ctx->a[1] = gfx + (uint32_t)(int32_t)sx(ctx->d[0]);
	hle_insn(ctx, 12);
	RAM32_W(ctx->a[5], ctx->a[1]);
	ctx->a[5] += 4;
	cc_logic32(ctx, ctx->a[1]);
	hle_insn(ctx, 12);
	set_w(&ctx->d[0], RAM16(VDP_GFX_COUNT));
	cc_logic16(ctx, ctx->d[0]);
	hle_insn(ctx, 16);
	set_w(&ctx->d[0], asl16(ctx, ctx->d[0] & 0xFFFF, 5));
	RAM16(ctx->a[5]) = (uint16_t)ctx->d[0];
	ctx->a[5] += 2;
	cc_logic16(ctx, ctx->d[0]);
	hle_insn(ctx, 8);
	RAM32_W(VDP_LIST_PTR, ctx->a[5]);
	cc_logic32(ctx, ctx->a[5]);
	hle_insn(ctx, 20);
	addiw_d(ctx, 1, base);
	jsr(ctx, ObjQueueSprite);
	exit_next(ctx);
}

static void init26(hle_ctx *ctx, uint8_t b12, uint8_t bc, uint32_t state)
{
	RAM8(A6 + 0xD) = 0;
	cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	hle_insn(ctx, 16);
	moveb_i_m(ctx, A6 + 0x12, b12);
	moveb_i_m(ctx, A6 + 0xC, bc);
	moveb_i_m(ctx, A6 + 0xF, 0xFF);
	bclr_m(ctx, A6, 7);
	ctx->a[5] = 0x12F1A;
	hle_insn(ctx, 12);
	RAM16(A6 + 0x1A) = ROM16(ctx->a[5]);
	cc_logic16(ctx, RAM16(A6 + 0x1A));
	ctx->a[5] += 2;
	hle_insn(ctx, 16);
	RAM32_W(A6 + 0x16, ctx->a[5]);
	cc_logic32(ctx, ctx->a[5]);
	hle_insn(ctx, 16);
	movew_i_m(ctx, A6 + 0x24, 0x20);
	movel_i_m(ctx, A6 + 0x20, state);
}

/* the fall state: scroll, sub_00D236, timer--; while >= 0 the frame comes from the ROM byte table,
 * at the end it either goes to `landed` (y > (+$28)) or stops falling and uses frame 4 */
static void fall26(hle_ctx *ctx, uint32_t tab, uint32_t tbl, uint32_t gfx, uint16_t base,
                   void (*landed)(hle_ctx *))
{
	jsr(ctx, ObjScrollX);
	hle_insn(ctx, 18);                              /* jsr sub_00D236 */
	sub_d236(ctx);
	subq1w_m(ctx, A6 + 0x24, 16);
	if (!c_n(ctx)) {
		bs(ctx, 1);                             /* bpl.s */
		movew_m_d(ctx, 1, A6 + 0x24);
		set_w(&ctx->d[1], asr16(ctx, ctx->d[1] & 0xFFFF, 1));
		eoriw_d(ctx, 1, 0xF);
		set_b(&ctx->d[1], rom8(tab + (uint32_t)(int32_t)sx(ctx->d[1])));
		cc_logic8(ctx, ctx->d[1]);
		hle_insn(ctx, 14);
		vdp_tail(ctx, tbl, gfx, base);
		return;
	}
	bs(ctx, 0);
	moveq(ctx, 1, 4);
	movew_m_d(ctx, 0, A6 + 4);
	alu(ctx, OP_SUB, ctx->d[0], RAM16(A6 + 0x28), 16, 0);
	hle_insn(ctx, 12);                              /* cmp.w (0x28,a6),d0 */
	if (c_gt(ctx)) {
		bw(ctx, 1);                             /* bgt.w */
		landed(ctx);
		return;
	}
	bw(ctx, 0);
	RAM16(A6 + 0x12) = 0;
	cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	hle_insn(ctx, 16);
	hle_insn(ctx, 10);                              /* bra.s tail */
	vdp_tail(ctx, tbl, gfx, base);
}

/* variant 1: ... -> $EEA2 -> $EEB0 (a plain sprite) */
static void st_eeb0(hle_ctx *ctx)
{
	jsr(ctx, ObjScrollX);
	subq1w_m(ctx, A6 + 0x24, 16);
	if (!c_z(ctx)) {
		bs(ctx, 1);
	} else {
		bs(ctx, 0);
		clr_obj_slot(ctx);
		clr_flag(ctx);
	}
	q(ctx, 0x1FA);
	exit_next(ctx);
}
static void ee_a2(hle_ctx *ctx)
{
	movew_i_m(ctx, A6 + 0x24, 0x40);
	movel_i_m(ctx, A6 + 0x20, 0xEEB0);
	st_eeb0(ctx);
}
/* variants 2/3: $F058/$F23E -> $F066/$F24C: frame from the timer, then the VDP tail */
static void st_last(hle_ctx *ctx, uint32_t tbl, uint32_t gfx, uint16_t base)
{
	jsr(ctx, ObjScrollX);
	subq1w_m(ctx, A6 + 0x24, 16);
	if (!c_z(ctx)) {
		bs(ctx, 1);
	} else {
		bs(ctx, 0);
		clr_obj_slot(ctx);
		clr_flag(ctx);
	}
	movew_m_d(ctx, 1, A6 + 0x24);
	set_w(&ctx->d[1], asr16(ctx, ctx->d[1] & 0xFFFF, 4));
	andiw_d(ctx, 1, 3);
	eoriw_d(ctx, 1, 3);
	addiw_d(ctx, 1, 4);
	hle_insn(ctx, 10);                              /* bra.w sub_xxxx */
	vdp_tail(ctx, tbl, gfx, base);
}
static void f058(hle_ctx *ctx)
{
	movew_i_m(ctx, A6 + 0x24, 0x40);
	movel_i_m(ctx, A6 + 0x20, 0xF066);
	st_last(ctx, 0xF150, 0x83128, 0x21C);
}
static void f23e(hle_ctx *ctx)
{
	movew_i_m(ctx, A6 + 0x24, 0x40);
	movel_i_m(ctx, A6 + 0x20, 0xF24C);
	st_last(ctx, 0xF336, 0x8446A, 0x224);
}
static void fall_v1(hle_ctx *c) { fall26(c, 0xEF80, 0xEF90, 0x7CDF0, 0x1F6, ee_a2); }
static void fall_v2(hle_ctx *c) { fall26(c, 0xF140, 0xF150, 0x83128, 0x21C, f058); }
static void fall_v3(hle_ctx *c) { fall26(c, 0xF326, 0xF336, 0x8446A, 0x224, f23e); }

HLE_RANGE_PURE(0x00EED4, L2InfDie, 900, EXIT_NEXT)  { init26(ctx, 8, 0x64, 0xEF0A); fall_v1(ctx); }
HLE_RANGE_PURE(0x00EF0A, L2InfFall, 900, EXIT_NEXT) { fall_v1(ctx); }
HLE_RANGE_PURE(0x00EEB0, L2InfLast, 500, EXIT_NEXT) { st_eeb0(ctx); }
HLE_RANGE_PURE(0x00F094, L2InfDie2, 900, EXIT_NEXT) { init26(ctx, 6, 0x4B, 0xF0CA); fall_v2(ctx); }
HLE_RANGE_PURE(0x00F0CA, L2InfFall2, 900, EXIT_NEXT) { fall_v2(ctx); }
HLE_RANGE_PURE(0x00F066, L2InfLast2, 900, EXIT_NEXT)
{ st_last(ctx, 0xF150, 0x83128, 0x21C); }
HLE_RANGE_PURE(0x00F27A, L2InfDie3, 900, EXIT_NEXT) { init26(ctx, 6, 0x4B, 0xF2B0); fall_v3(ctx); }
HLE_RANGE_PURE(0x00F2B0, L2InfFall3, 900, EXIT_NEXT) { fall_v3(ctx); }
HLE_RANGE_PURE(0x00F24C, L2InfLast3, 900, EXIT_NEXT)
{ st_last(ctx, 0xF336, 0x8446A, 0x224); }

/* ================================================================== hit routines */
/* lea box,a3 ; lea onhit,a2 ; jmp HitBox  (the +$1C routines) */
static void hit_lea(hle_ctx *ctx, uint32_t box, uint32_t onhit)
{
	ctx->a[3] = box;
	ctx->a[2] = onhit;
	HLE_SEQ(ctx, 8, 8, 10);
	HitBox(ctx);
}
HLE_FUNC_PURE(0x00597A, L2HitNone, 16)   { hle_insn(ctx, 16); }
HLE_FUNC_PURE(0x006512, L2Hit_6512, 300) { hit_lea(ctx, 0x653C, 0x651E); }
HLE_FUNC_PURE(0x006544, L2Hit_6544, 300) { hit_lea(ctx, 0x656E, 0x6550); }
HLE_FUNC_PURE(0x006576, L2Hit_6576, 300) { hit_lea(ctx, 0x659C, 0x6582); }
HLE_FUNC_PURE(0x0065A4, L2Hit_65A4, 300) { hit_lea(ctx, 0x65CA, 0x65B0); }
HLE_FUNC_PURE(0x0065D2, L2Hit_65D2, 300) { hit_lea(ctx, 0x65F4, 0x65DE); }
HLE_FUNC_PURE(0x0066A8, L2Hit_66A8, 300) { hit_lea(ctx, 0x66D2, 0x66B4); }
HLE_FUNC_PURE(0x00683A, L2Hit_683A, 300) { hit_lea(ctx, 0x6868, 0x6846); }
HLE_FUNC_PURE(0x006894, L2Hit_6894, 300) { hit_lea(ctx, 0x68C2, 0x68A0); }
HLE_FUNC_PURE(0x0068CA, L2Hit_68CA, 300) { hit_lea(ctx, 0x68F8, 0x68D6); }

/* human: damage >= 3 is not a hit */
static void hit_human(hle_ctx *ctx, uint32_t box, uint32_t onhit)
{
	ctx->a[3] = box;
	hle_insn(ctx, 8);
	ctx->a[2] = onhit;
	hle_insn(ctx, 8);
	alu(ctx, OP_SUB, RAM16(SHOT_DAMAGE), 3, 16, 0);
	hle_insn(ctx, 20);                              /* cmpi.w #3,(abs).l */
	if (c_ge(ctx)) {
		bs(ctx, 1);
		hle_insn(ctx, 16);                      /* rts */
		return;
	}
	bs(ctx, 0);
	hle_insn(ctx, 10);                              /* jmp HitBox */
	HitBox(ctx);
}
HLE_FUNC_PURE(0x0065FC, L2HitHumanB, 300) { hit_human(ctx, 0x664A, 0x6612); }
HLE_FUNC_PURE(0x006652, L2HitHumanC, 300) { hit_human(ctx, 0x66A0, 0x6668); }
