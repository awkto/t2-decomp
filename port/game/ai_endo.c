/* AI states and hit routines of object type $01 (the endoskeleton foot soldier).
 * docs/objects_types.md, docs/subsystems/objects.md (src/t2.s).
 *
 * An AI state is entered by the `jmp (a0)` in ObjRunAll (a6 = slot) and ends `bra.w ObjNext`
 * ($00B3BE), so each state is an HLE_RANGE_PURE from its entry to that bra (exit = ObjNext, which
 * is itself a C range). The callees that are C already (ObjMoveX, ObjScrollX, ObjKillIf*,
 * ObjQueueSprite, QueueSprite, Random, HitBox) are called directly after the `jsr`/`bsr` is
 * charged (their closing `rts` is charged by them); the helpers of the type (sub_00C652,
 * sub_00B772, sub_00C48E, sub_00D236, ...) are inlined as C functions with their own timelines.
 *
 * Where a state calls 68k code that is not C (the warning sound sub_013D04 and PlayerDamageNearest
 * sub_009054, sub_01237A, sub_01277C) the range stops right BEFORE that `jsr`, the real 68k runs
 * the call, and a second range registered at the instruction after the call (the return address)
 * continues. Both parts only touch work RAM and registers: HLE_RANGE_PURE.
 *
 * States: $00B586 walk (falls into $00B5C2), $00B5EC / $00B65A / $00B71E / $00B6D0 (walk, stand and
 * shoot variants), $00B52E (death flash), $00B504 (death start), $00B4C2 / $00B78C / $00C66C
 * (knocked down), $00C6B0 / $00C7C8 (the fall), $00C564 (second walk) and $00C5E2.
 * Hit routines $0059AC, $005A48, $005AF8, $005BEC (box tables + HitBox).
 *
 * 68000 costs (GPGX table): jsr/bsr (d16,pc) 18, bra 10, Bcc.s 10/8, Bcc.w 10/12, moveq 4,
 * move.w #i,Dn 8, move.w (d16,An),Dn 12, move.b (d16,An),Dn 12, move.w #i,(d16,An) 16,
 * move.l #i,(d16,An) 24, move.b #i,(d16,An) 16, subq.w #1,(d16,An) 16, subq.w #1,(abs).l 20,
 * cmpi.w #i,(d16,An) 16, cmpi.w #i,Dn 8, btst #n,(An) 12, btst #n,(d16,An) 16, btst #n,Dn 10,
 * bset/bclr #n,(An) 16, shifts 6+2n. */
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
static void cmpiw_m(hle_ctx *c, uint32_t a, uint16_t i)
{
	alu(c, OP_SUB, RAM16(a), i, 16, 0);
	hle_insn(c, 16);
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

/* -------- shared pieces */
static void kill_right_360(hle_ctx *ctx)
{
	movew_i_d(ctx, 0, 0x168);
	jsr(ctx, ObjKillIfRightOf);
}
static void kill_left_40(hle_ctx *ctx)
{
	moveq(ctx, 0, -0x28);
	jsr(ctx, ObjKillIfLeftOf);
}

/* sub_00C652 (after its jsr was charged): state = $B586, timer = $40 + rnd & $3F */
static void sub_c652(hle_ctx *ctx)
{
	movel_i_m(ctx, A6 + 0x20, 0xB586);
	jsr(ctx, Random);
	andiw_d(ctx, 0, 0x3F);
	addiw_d(ctx, 0, 0x40);
	RAM16(A6 + 0x24) = (uint16_t)ctx->d[0];     /* move.w d0,(0x24,a6) */
	cc_logic16(ctx, ctx->d[0]);
	hle_insn(ctx, 12);
	hle_insn(ctx, 16);                          /* rts */
}

/* sub_00B772: state = $B6D0, timer = $96 + rnd & $3F */
static void sub_b772(hle_ctx *ctx)
{
	movel_i_m(ctx, A6 + 0x20, 0xB6D0);
	jsr(ctx, Random);
	andiw_d(ctx, 0, 0x3F);
	addiw_d(ctx, 0, 0x96);
	RAM16(A6 + 0x24) = (uint16_t)ctx->d[0];
	cc_logic16(ctx, ctx->d[0]);
	hle_insn(ctx, 12);
	hle_insn(ctx, 16);
}

/* ------------------------------------------------------------------ $00B586 walk, $00B6D0 */
/* ObjMoveX, ObjScrollX, frame d1 = set/clear by b14, kill limits, frame += (timer & $38) >> 3 ^ 7,
 * queue, timer--; returns 1 when the timer reached 0 (caller does the state switch) */
static int walk_body(hle_ctx *ctx, int d1_b14, int d1_nob14)
{
	jsr(ctx, ObjMoveX);
	jsr(ctx, ObjScrollX);
	btst_m(ctx, A6, 6, 12);
	if (RAM8(A6) & 0x40) {
		bs(ctx, 0);
		moveq(ctx, 1, d1_b14);
		hle_insn(ctx, 10);                  /* bra.s */
	} else {
		bs(ctx, 1);
		moveq(ctx, 1, d1_nob14);
	}
	kill_right_360(ctx);
	kill_left_40(ctx);
	movew_m_d(ctx, 0, A6 + 0x24);
	andiw_d(ctx, 0, 0x38);
	set_w(&ctx->d[0], lsr16(ctx, ctx->d[0] & 0xFFFF, 3));
	hle_insn(ctx, 12);                          /* lsr.w #3,d0 */
	eoriw_d(ctx, 0, 7);
	set_w(&ctx->d[1], alu(ctx, OP_ADD, ctx->d[1], ctx->d[0], 16, 1));
	hle_insn(ctx, 4);                           /* add.w d0,d1 */
	hle_insn(ctx, 18);                          /* bsr.w ObjQueueSprite */
	ObjQueueSprite(ctx);
	subq1w_m(ctx, A6 + 0x24, 16);
	return !c_z(ctx) ? 0 : 1;
}

/* $00B5C2: stand-and-shoot (b4 set: $B65A, timer $20) or stand and wait ($B5EC, timer $64) */
static void sub_b5c2(hle_ctx *ctx)
{
	btst_m(ctx, A6, 4, 12);
	if (RAM8(A6) & 0x10) {
		bw(ctx, 0);
		movel_i_m(ctx, A6 + 0x20, 0xB65A);
		movew_i_m(ctx, A6 + 0x24, 0x20);
		hle_insn(ctx, 10);                  /* bra.s $B5E8 */
	} else {
		bw(ctx, 1);
		movel_i_m(ctx, A6 + 0x20, 0xB5EC);
		movew_i_m(ctx, A6 + 0x24, 0x64);
	}
	exit_next(ctx);
}

HLE_RANGE_PURE(0x00B586, EndoWalk, 800, EXIT_NEXT)
{
	if (walk_body(ctx, 0x34, 0x3C)) {
		bs(ctx, 0);
		sub_b5c2(ctx);
	} else {
		bs(ctx, 1);
		exit_next(ctx);                     /* $B5E8: bra.w ObjNext */
	}
}

HLE_RANGE_PURE(0x00B6D0, EndoWalk2, 800, EXIT_NEXT)
{
	if (walk_body(ctx, 0x44, 0x4C)) {
		bs(ctx, 0);
		movel_i_m(ctx, A6 + 0x20, 0xB71E);
		movew_i_m(ctx, A6 + 0x24, 0x64);
	} else {
		bs(ctx, 1);
	}
	exit_next(ctx);
}

/* ------------------------------------------------------------------ stand and shoot states */
/* from loc_00B614 / loc_00B72C: kill limits, then the warning flash when timer in $14..$50 and
 * bit 2 set. The flash calls sub_013D04 and sub_009054 (68k): the range exits before the jsr. */
static void shoot_mid(hle_ctx *ctx, uint32_t call_exit, uint16_t spr_b);
static void shoot_tail(hle_ctx *ctx, uint16_t spr_b)
{
	movew_i_d(ctx, 1, spr_b);
	jsr(ctx, ObjQueueSprite);
	exit_next(ctx);
}
static void shoot_mid(hle_ctx *ctx, uint32_t call_exit, uint16_t spr_b)
{
	kill_right_360(ctx);
	kill_left_40(ctx);
	movew_m_d(ctx, 0, A6 + 0x24);
	cmpiw_d(ctx, 0, 0x50);
	if (c_gt(ctx)) { bs(ctx, 1); shoot_tail(ctx, spr_b); return; }
	bs(ctx, 0);
	cmpiw_d(ctx, 0, 0x14);
	if (c_lt(ctx)) { bs(ctx, 1); shoot_tail(ctx, spr_b); return; }
	bs(ctx, 0);
	btst_d(ctx, 0, 2);
	if (c_z(ctx)) { bs(ctx, 1); shoot_tail(ctx, spr_b); return; }
	bs(ctx, 0);
	movew_i_d(ctx, 0, 0x60);
	hle_jump(ctx, call_exit);                   /* the real jsr sub_013D04 follows */
}

/* $00B5EC */
HLE_RANGE_PURE(0x00B5EC, EndoShoot, 800, 0x00B63C, EXIT_NEXT)
{
	jsr(ctx, ObjScrollX);
	subq1w_m(ctx, A6 + 0x24, 16);
	if (!c_z(ctx)) {
		bs(ctx, 1);
	} else {
		bs(ctx, 0);
		hle_insn(ctx, 18);                  /* jsr sub_00C652 */
		sub_c652(ctx);
		bset_m(ctx, A6, 6);
		moveb_i_m(ctx, A6 + 0xE, 1);
		cmpiw_m(ctx, A6 + 2, 0xA0);
		if (c_gt(ctx)) {
			bs(ctx, 1);
		} else {
			bs(ctx, 0);
			bclr_m(ctx, A6, 6);
			RAM8(A6 + 0xE) = (uint8_t)alu(ctx, OP_SUB, 0, RAM8(A6 + 0xE), 8, 1);   /* neg.b */
			hle_insn(ctx, 16);
		}
	}
	shoot_mid(ctx, 0x00B63C, 0x16A);
}
HLE_RANGE_PURE(0x00B646, EndoShootAfterSound, 400, EXIT_NEXT)
{
	movew_i_d(ctx, 1, 0x16B);
	jsr(ctx, ObjQueueSprite);
	shoot_tail(ctx, 0x16A);
}

/* $00B71E */
HLE_RANGE_PURE(0x00B71E, EndoShoot2, 800, 0x00B754, EXIT_NEXT)
{
	jsr(ctx, ObjScrollX);
	subq1w_m(ctx, A6 + 0x24, 16);
	if (!c_z(ctx)) {
		bs(ctx, 1);
	} else {
		bs(ctx, 0);
		hle_insn(ctx, 18);                  /* jsr sub_00B772 */
		sub_b772(ctx);
	}
	shoot_mid(ctx, 0x00B754, 0x16C);
}
HLE_RANGE_PURE(0x00B75E, EndoShoot2AfterSound, 400, EXIT_NEXT)
{
	movew_i_d(ctx, 1, 0x16D);
	jsr(ctx, ObjQueueSprite);
	shoot_tail(ctx, 0x16C);
}

/* ------------------------------------------------------------------ $00B65A */
static const uint32_t B68A = 0x00B68A;
HLE_RANGE_PURE(0x00B65A, EndoAim, 600, 0x00B682, EXIT_NEXT)
{
	jsr(ctx, ObjScrollX);
	subq1w_m(ctx, A6 + 0x24, 16);
	if (!c_z(ctx)) {
		bs(ctx, 1);
	} else {
		bs(ctx, 0);
		hle_insn(ctx, 18);
		sub_c652(ctx);
	}
	movew_m_d(ctx, 1, A6 + 0x24);
	set_w(&ctx->d[1], asr16(ctx, ctx->d[1] & 0xFFFF, 1));
	andiw_d(ctx, 1, 0xE);
	set_w(&ctx->d[1], rom16(B68A + (uint32_t)(int32_t)sx(ctx->d[1])));   /* move.w (pc,d1.w),d1 */
	cc_logic16(ctx, ctx->d[1]);
	hle_insn(ctx, 14);
	jsr(ctx, ObjQueueSprite);
	cmpiw_m(ctx, A6 + 0x24, 0xF);
	if (!c_z(ctx)) {
		bs(ctx, 1);
		exit_next(ctx);                     /* $B686 */
		return;
	}
	bs(ctx, 0);
	hle_jump(ctx, 0x00B682);                    /* the real jsr sub_01237A, then bra.w ObjNext */
}

/* ------------------------------------------------------------------ $00B52E / $00B504 */
static void death_flash(hle_ctx *ctx)
{
	moveq(ctx, 5, 2);
	btst_m(ctx, A6 + 0x25, 1, 16);
	if (RAM8(A6 + 0x25) & 2) {
		bs(ctx, 0);
		set_w(&ctx->d[5], alu(ctx, OP_SUB, 0, ctx->d[5], 16, 1));   /* neg.w d5 */
		hle_insn(ctx, 4);
	} else {
		bs(ctx, 1);
	}
	/* movem.w (2,a6),d2-d3: sign-extends into the whole registers */
	ctx->d[2] = (uint32_t)(int32_t)(int16_t)RAM16(A6 + 2);
	ctx->d[3] = (uint32_t)(int32_t)(int16_t)RAM16(A6 + 4);
	hle_insn(ctx, 24);
	set_w(&ctx->d[2], alu(ctx, OP_ADD, ctx->d[2], ctx->d[5], 16, 1));
	hle_insn(ctx, 4);                           /* add.w d5,d2 */
	moveq(ctx, 5, 0);
	moveb_m_d(ctx, 5, A6 + 7);
	btst_m(ctx, A6, 5, 12);                     /* btst #$d,(a6) = bit 5 of the high byte */
	if (RAM8(A6) & 0x20) {
		bs(ctx, 0);
		moveq(ctx, 1, 0x6C);
		jsr(ctx, QueueSprite);
		hle_insn(ctx, 10);                  /* bra.s */
	} else {
		bs(ctx, 1);
		btst_m(ctx, A6, 6, 12);
		if (RAM8(A6) & 0x40) {
			bw(ctx, 0);
			moveq(ctx, 1, 6);
			jsr(ctx, QueueSprite);
			hle_insn(ctx, 10);          /* bra.s */
		} else {
			bw(ctx, 1);
			moveq(ctx, 1, 0xE);
			jsr(ctx, QueueSprite);
		}
	}
	movew_i_d(ctx, 0, 0xFFE0);
	jsr(ctx, ObjKillIfLeftOf);
	subq1w_m(ctx, A6 + 0x24, 16);
	if (!c_z(ctx)) {
		bs(ctx, 1);
	} else {
		bs(ctx, 0);
		RAM16(A6) = 0;                      /* clr.w (a6) */
		cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
		hle_insn(ctx, 12);
		RAM16(OBJ_COUNT) = (uint16_t)alu(ctx, OP_SUB, RAM16(OBJ_COUNT), 1, 16, 1);
		hle_insn(ctx, 20);
	}
	exit_next(ctx);
}
HLE_RANGE_PURE(0x00B52E, EndoDeathFlash, 800, EXIT_NEXT) { death_flash(ctx); }

/* $00B504: load d0/d1/a2, then the real jsr sub_01277C at $B51A */
HLE_RANGE_PURE(0x00B504, EndoDeath, 200, 0x00B51A)
{
	movew_m_d(ctx, 0, A6 + 2);
	movew_m_d(ctx, 1, A6 + 4);
	ctx->a[2] = 0x012836;                       /* lea (pc),a2 */
	hle_insn(ctx, 8);
	btst_m(ctx, A6, 6, 12);
	if (RAM8(A6) & 0x40) {
		bs(ctx, 1);
	} else {
		bs(ctx, 0);
		ctx->a[2] = 0x01283E;
		hle_insn(ctx, 8);
	}
	hle_jump(ctx, 0x00B51A);
}
HLE_RANGE_PURE(0x00B520, EndoDeathAfter, 800, EXIT_NEXT)
{
	movel_i_m(ctx, A6 + 0x20, 0xB52E);
	movew_i_m(ctx, A6 + 0x24, 0x20);
	death_flash(ctx);
}

/* ------------------------------------------------------------------ $00C66C, $00C6B0, $00C7C8 */
/* sub_00C6FA: d1 = frame base, queue a VDP-list command 1 (tile copy), then the sprite */
static void sub_c6fa(hle_ctx *ctx)
{
	movew_m_d(ctx, 0, A6 + 0x14);
	{
		uint8_t b = rom8(0xC758 + (uint32_t)(int32_t)sx(ctx->d[0]));
		set_b(&ctx->d[1], alu(ctx, OP_ADD, ctx->d[1], b, 8, 1));   /* add.b (pc,d0.w),d1 */
		hle_insn(ctx, 14);
	}
	ctx->a[5] = RAM32(VDP_LIST_PTR);            /* movea.l (abs).l,a5 */
	hle_insn(ctx, 20);
	RAM16(ctx->a[5]) = 1;                       /* move.w #1,(a5)+ */
	ctx->a[5] += 2;
	cc_logic16(ctx, 1);
	hle_insn(ctx, 12);
	set_w(&ctx->d[0], ctx->d[1]);               /* move.w d1,d0 */
	cc_logic16(ctx, ctx->d[0]);
	hle_insn(ctx, 4);
	set_w(&ctx->d[0], asl16(ctx, ctx->d[0] & 0xFFFF, 2));
	ctx->a[1] = 0xC76A;                         /* lea (pc),a1 */
	hle_insn(ctx, 8);
	set_w(&ctx->d[2], rom16(ctx->a[1] + (uint32_t)(int32_t)sx(ctx->d[0])));   /* move.w (0,a1,d0.w),d2 */
	cc_logic16(ctx, ctx->d[2]);
	hle_insn(ctx, 14);
	set_w(&ctx->d[2], asr16(ctx, ctx->d[2] & 0xFFFF, 1));
	RAM16(ctx->a[5]) = (uint16_t)ctx->d[2];     /* move.w d2,(a5)+ */
	ctx->a[5] += 2;
	cc_logic16(ctx, ctx->d[2]);
	hle_insn(ctx, 8);
	set_w(&ctx->d[0], rom16(ctx->a[1] + 2 + (uint32_t)(int32_t)sx(ctx->d[0])));   /* move.w (2,a1,d0.w),d0 */
	cc_logic16(ctx, ctx->d[0]);
	hle_insn(ctx, 14);
	ctx->a[1] = 0x40000;                        /* lea.l (abs).l,a1 */
	hle_insn(ctx, 12);
	ctx->a[1] = 0x40000 + (uint32_t)(int32_t)sx(ctx->d[0]);   /* lea (0,a1,d0.w),a1 */
	hle_insn(ctx, 12);
	RAM32_W(ctx->a[5], ctx->a[1]);              /* move.l a1,(a5)+ */
	ctx->a[5] += 4;
	cc_logic32(ctx, ctx->a[1]);
	hle_insn(ctx, 12);
	set_w(&ctx->d[0], RAM16(0xFF9E24));         /* move.w (abs).l,d0 */
	cc_logic16(ctx, ctx->d[0]);
	hle_insn(ctx, 16);
	set_w(&ctx->d[0], asl16(ctx, ctx->d[0] & 0xFFFF, 5));
	RAM16(ctx->a[5]) = (uint16_t)ctx->d[0];     /* move.w d0,(a5)+ */
	ctx->a[5] += 2;
	cc_logic16(ctx, ctx->d[0]);
	hle_insn(ctx, 8);
	RAM32_W(VDP_LIST_PTR, ctx->a[5]);           /* move.l a5,(abs).l */
	cc_logic32(ctx, ctx->a[5]);
	hle_insn(ctx, 20);
	addiw_d(ctx, 1, 0x96);
	jsr(ctx, ObjQueueSprite);
	exit_next(ctx);
}

/* clr.w (a6); subq.w #1,(obj_count); clr.b (endo_flag) */
static void remove_obj(hle_ctx *ctx)
{
	RAM16(A6) = 0;
	cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	hle_insn(ctx, 12);
	RAM16(OBJ_COUNT) = (uint16_t)alu(ctx, OP_SUB, RAM16(OBJ_COUNT), 1, 16, 1);
	hle_insn(ctx, 20);
	RAM8(ENDO_FLAG) = 0;
	cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	hle_insn(ctx, 20);
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

/* $00C7C8 as a function: reached from the range and by the fall-through of $00C6B0 ($C7BA) */
static void st_c7c8(hle_ctx *ctx)
{
	jsr(ctx, ObjScrollX);
	cmpiw_m(ctx, A6 + 2, 0xFFE2);
	if (c_gt(ctx)) {
		bs(ctx, 1);
	} else {
		bs(ctx, 0);
		remove_obj(ctx);
		exit_next(ctx);
		return;
	}
	subq1w_m(ctx, A6 + 0x24, 16);
	if (!c_z(ctx)) {
		bs(ctx, 1);
	} else {
		bs(ctx, 0);
		remove_obj(ctx);
	}
	movew_i_d(ctx, 1, 4);
	hle_insn(ctx, 10);                                  /* bra.w sub_00C6FA */
	sub_c6fa(ctx);
}
HLE_RANGE_PURE(0x00C7C8, EndoFall2, 800, EXIT_NEXT) { st_c7c8(ctx); }

static void st_c6b0(hle_ctx *ctx)
{
	jsr(ctx, ObjScrollX);
	cmpiw_m(ctx, A6 + 2, 0xFFE2);
	if (c_gt(ctx)) {
		bs(ctx, 1);
	} else {
		bs(ctx, 0);
		remove_obj(ctx);
		exit_next(ctx);
		return;
	}
	hle_insn(ctx, 18);                                  /* jsr sub_00D236 */
	sub_d236(ctx);
	subq1w_m(ctx, A6 + 0x24, 16);
	if (!c_n(ctx)) {                                    /* bpl.s taken */
		bs(ctx, 1);
		movew_m_d(ctx, 1, A6 + 0x24);
		set_w(&ctx->d[1], asr16(ctx, ctx->d[1] & 0xFFFF, 1));
		eoriw_d(ctx, 1, 0xF);
		set_b(&ctx->d[1], rom8(0xC748 + (uint32_t)(int32_t)sx(ctx->d[1])));
		cc_logic8(ctx, ctx->d[1]);
		hle_insn(ctx, 14);
		sub_c6fa(ctx);
		return;
	}
	bs(ctx, 0);
	moveq(ctx, 1, 4);
	movew_m_d(ctx, 0, A6 + 4);
	alu(ctx, OP_SUB, ctx->d[0], RAM16(A6 + 0x28), 16, 0);   /* cmp.w (0x28,a6),d0 */
	hle_insn(ctx, 12);
	if (c_gt(ctx)) {
		bw(ctx, 1);
		movew_i_m(ctx, A6 + 0x24, 0x40);
		movel_i_m(ctx, A6 + 0x20, 0xC7C8);
		st_c7c8(ctx);
		return;
	}
	bw(ctx, 0);
	RAM16(A6 + 0x12) = 0;                               /* clr.w (0x12,a6) */
	cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	hle_insn(ctx, 16);
	hle_insn(ctx, 10);                                  /* bra.s sub_00C6FA */
	sub_c6fa(ctx);
}
HLE_RANGE_PURE(0x00C6B0, EndoFall, 800, EXIT_NEXT) { st_c6b0(ctx); }

static void st_c66c(hle_ctx *ctx)
{
	jsr(ctx, ObjScrollX);
	moveb_i_m(ctx, A6 + 0xF, 0xFF);
	RAM16(A6 + 0x28) = RAM16(A6 + 4);                   /* move.w (4,a6),(0x28,a6) */
	cc_logic16(ctx, RAM16(A6 + 4));
	hle_insn(ctx, 20);                                  /* move.w (d16,a6),(d16,a6) is 20, not 24 */
	movew_m_d(ctx, 0, A6 + 0x14);
	set_w(&ctx->d[0], alu(ctx, OP_ADD, ctx->d[0], ctx->d[0], 16, 1));
	hle_insn(ctx, 4);
	set_w(&ctx->d[0], alu(ctx, OP_ADD, ctx->d[0], ctx->d[0], 16, 1));
	hle_insn(ctx, 4);
	addiw_d(ctx, 0, 0x68);
	RAM8(A6 + 0xC) = (uint8_t)ctx->d[0];                /* move.b d0,(0xc,a6) */
	cc_logic8(ctx, ctx->d[0]);
	hle_insn(ctx, 12);
	RAM8(A6 + 0xD) = 0;                                 /* clr.b (0xd,a6) */
	cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	hle_insn(ctx, 16);
	bclr_m(ctx, A6, 7);                                 /* bclr #$f,(a6): bit 7 of the byte */
	moveb_i_m(ctx, A6 + 0x12, 7);
	RAM8(A6 + 7) = (uint8_t)alu(ctx, OP_ADD, RAM8(A6 + 7), 1, 8, 1);   /* addq.b #1,(7,a6) */
	hle_insn(ctx, 16);
	RAM32_W(A6 + 0x16, ctx->a[5]);                      /* move.l a5,(0x16,a6) */
	cc_logic32(ctx, ctx->a[5]);
	hle_insn(ctx, 16);
	movew_i_m(ctx, A6 + 0x24, 0x20);
	movel_i_m(ctx, A6 + 0x20, 0xC6B0);
	st_c6b0(ctx);
}
HLE_RANGE_PURE(0x00C66C, EndoDown, 800, EXIT_NEXT) { st_c66c(ctx); }

HLE_RANGE_PURE(0x00B4C2, EndoHitDown, 800, EXIT_NEXT)
{
	RAM16(A6 + 0x28) = RAM16(A6 + 4);                   /* move.w (4,a6),(0x28,a6) */
	cc_logic16(ctx, RAM16(A6 + 4));
	hle_insn(ctx, 20);                                  /* move.w (d16,a6),(d16,a6) is 20, not 24 */
	movew_i_m(ctx, A6 + 0x14, 0x11);
	hle_insn(ctx, 10);                                  /* bra.w sub_00C66C */
	st_c66c(ctx);
}
HLE_RANGE_PURE(0x00B78C, EndoHitDown2, 800, EXIT_NEXT)
{
	movew_i_m(ctx, A6 + 0x14, 5);
	hle_insn(ctx, 10);                                  /* bra.w sub_00C66C */
	st_c66c(ctx);
}

/* ------------------------------------------------------------------ $00C564 and $00C5E2 */
static const uint32_t DAT_C5C0 = 0x00C5C0;

/* sub_00C48E (after its jsr was charged) */
static void sub_c48e(hle_ctx *ctx)
{
	moveb_m_d(ctx, 0, A6 + 0xE);
	extw(ctx, 0);
	moveb_m_d(ctx, 1, A6 + 0xA);
	moveb_m_d(ctx, 2, A6 + 0xF);
	extw(ctx, 2);
	moveb_m_d(ctx, 3, A6 + 0xC);
	ctx->a[0] = RAM32(A6 + 0x16);                       /* movea.l (0x16,a6),a0 */
	hle_insn(ctx, 16);
	{
		uint16_t w = rd16(ctx->a[0]);                   /* move.w (a0)+,d4 */
		ctx->a[0] += 2;
		set_w(&ctx->d[4], w);
		cc_logic16(ctx, w);
		hle_insn(ctx, 8);
	}
	set_w(&ctx->d[4], alu(ctx, OP_SUB, ctx->d[4], 1, 16, 1));
	hle_insn(ctx, 4);
	if (c_n(ctx)) {                                     /* bmi.w taken */
		bw(ctx, 1);
		moveq(ctx, 4, 1);
		hle_insn(ctx, 16);
		return;
	}
	bw(ctx, 0);
	subq1w_m(ctx, A6 + 0x1A, 16);
	if (!c_z(ctx)) {
		bs(ctx, 1);
	} else {
		bs(ctx, 0);
		uint16_t w = rd16(ctx->a[0]);                   /* move.w (a0)+,(0x1a,a6) */
		ctx->a[0] += 2;
		RAM16(A6 + 0x1A) = w;
		cc_logic16(ctx, w);
		hle_insn(ctx, 16);
		RAM32_W(A6 + 0x16, ctx->a[0]);                  /* move.l a0,(0x16,a6) */
		cc_logic32(ctx, ctx->a[0]);
		hle_insn(ctx, 16);
	}
	for (;;) {
		RAM8(A6 + 0xB) = (uint8_t)alu(ctx, OP_ADD, RAM8(A6 + 0xB), ctx->d[1], 8, 1);
		hle_insn(ctx, 16);
		if (!c_c(ctx)) {
			bs(ctx, 1);
		} else {
			bs(ctx, 0);
			RAM16(A6 + 2) = (uint16_t)alu(ctx, OP_ADD, RAM16(A6 + 2), ctx->d[0], 16, 1);
			hle_insn(ctx, 16);
		}
		RAM8(A6 + 0xD) = (uint8_t)alu(ctx, OP_ADD, RAM8(A6 + 0xD), ctx->d[3], 8, 1);
		hle_insn(ctx, 16);
		if (!c_c(ctx)) {
			bs(ctx, 1);
		} else {
			bs(ctx, 0);
			RAM16(A6 + 4) = (uint16_t)alu(ctx, OP_ADD, RAM16(A6 + 4), ctx->d[2], 16, 1);
			hle_insn(ctx, 16);
		}
		subq1w_m(ctx, A6 + 0x14, 16);
		if (c_z(ctx)) {
			bs(ctx, 1);
			moveq(ctx, 4, 0);
			hle_insn(ctx, 16);
			return;
		}
		bs(ctx, 0);
		uint16_t d4 = (uint16_t)(ctx->d[4] - 1);        /* dbra d4 */
		set_w(&ctx->d[4], d4);
		if (d4 == 0xFFFF) {
			hle_insn(ctx, 14);
			hle_insn(ctx, 16);
			return;
		}
		hle_insn(ctx, 10);
	}
}

/* loc_00C584: height/priority from the animation step and the frame, queue */
static void c584(hle_ctx *ctx)
{
	movew_m_d(ctx, 1, A6 + 0x14);
	set_w(&ctx->d[1], asr16(ctx, ctx->d[1] & 0xFFFF, 3));
	set_w(&ctx->d[2], ctx->d[1]);
	cc_logic16(ctx, ctx->d[2]);
	hle_insn(ctx, 4);
	addiw_d(ctx, 2, 0x4E);
	RAM8(A6 + 7) = (uint8_t)ctx->d[2];
	cc_logic8(ctx, ctx->d[2]);
	hle_insn(ctx, 12);
	movew_m_d(ctx, 1, A6 + 0x14);
	set_w(&ctx->d[1], asr16(ctx, ctx->d[1] & 0xFFFF, 4));
	set_b(&ctx->d[1], rom8(DAT_C5C0 + (uint32_t)(int32_t)sx(ctx->d[1])));
	cc_logic8(ctx, ctx->d[1]);
	hle_insn(ctx, 14);
	addiw_d(ctx, 1, 0x84);
	jsr(ctx, ObjQueueSprite);
}

HLE_RANGE_PURE(0x00C564, EndoWalkAnim, 1200, EXIT_NEXT)
{
	jsr(ctx, ObjScrollX);
	kill_right_360(ctx);
	kill_left_40(ctx);
	hle_insn(ctx, 18);                                  /* jsr sub_00C48E */
	sub_c48e(ctx);
	cc_logic8(ctx, ctx->d[4]);                          /* tst.b d4 */
	hle_insn(ctx, 4);
	if (c_z(ctx)) {
		bw(ctx, 1);
		movew_i_m(ctx, A6 + 0x24, 0x64);
		movel_i_m(ctx, A6 + 0x20, 0xC5E2);
		hle_insn(ctx, 10);                          /* bra.w loc_00C584 */
		c584(ctx);
	} else {
		bw(ctx, 0);
		if (!c_n(ctx)) {
			bw(ctx, 1);                         /* bpl.w loc_00C5A6 */
		} else {
			bw(ctx, 0);
			c584(ctx);
		}
	}
	subq1w_m(ctx, A6 + 0x24, 16);
	exit_next(ctx);
}

HLE_RANGE_PURE(0x00C5E2, EndoStand, 1200, EXIT_NEXT, 0x00F74C)
{
	bclr_m(ctx, A6, 6);
	moveb_i_m(ctx, A6 + 1, 1);
	movew_m_d(ctx, 1, A6 + 0x14);
	set_w(&ctx->d[1], asr16(ctx, ctx->d[1] & 0xFFFF, 4));
	set_b(&ctx->d[1], rom8(DAT_C5C0 + (uint32_t)(int32_t)sx(ctx->d[1])));
	cc_logic8(ctx, ctx->d[1]);
	hle_insn(ctx, 14);
	addiw_d(ctx, 1, 0x84);
	jsr(ctx, ObjQueueSprite);
	moveb_i_m(ctx, A6 + 6, 0x14);
	moveb_i_m(ctx, A6 + 7, 0x11);
	movel_i_m(ctx, A6 + 0x1C, 0x5A48);
	movew_i_m(ctx, A6 + 8, 5);
	moveb_i_m(ctx, A6 + 0xA, 0x28);
	moveb_i_m(ctx, A6 + 0xE, 0xFF);
	cmpiw_m(ctx, A6 + 2, 0xA0);
	if (c_lt(ctx)) {
		bs(ctx, 1);
	} else {
		bs(ctx, 0);
		RAM8(A6 + 0xE) = (uint8_t)alu(ctx, OP_SUB, 0, RAM8(A6 + 0xE), 8, 1);   /* neg.b */
		hle_insn(ctx, 16);
		bset_m(ctx, A6, 6);
	}
	RAM8(A6 + 0xB) = 0;                                 /* clr.b (0xb,a6) */
	cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	hle_insn(ctx, 16);
	btst_m(ctx, A6, 3, 12);                             /* btst #$b,(a6) = bit 3 of the high byte */
	if (RAM8(A6) & 0x08) {
		bw(ctx, 1);
		hle_jump(ctx, 0x00F74C);
		return;
	}
	bw(ctx, 0);
	hle_insn(ctx, 18);                                  /* jsr sub_00C652 */
	sub_c652(ctx);
	andiw_d(ctx, 0, 6);
	if (c_z(ctx)) {
		bs(ctx, 1);
		exit_next(ctx);                             /* $C64E */
		return;
	}
	bs(ctx, 0);
	hle_insn(ctx, 10);                                  /* bra.w sub_00B5C2 */
	sub_b5c2(ctx);
}

/* ------------------------------------------------------------------ hit routines */
/* lea onhit,a2 ; lea box,a3 ; jmp HitBox */
static void lea_lea_jmp(hle_ctx *ctx, uint32_t onhit, uint32_t box)
{
	ctx->a[2] = onhit;
	ctx->a[3] = box;
	HLE_SEQ(ctx, 8, 8, 10);
	HitBox(ctx);
}

HLE_FUNC_PURE(0x005A48, EndoHit, 400) { lea_lea_jmp(ctx, 0x5A54, 0x5A78); }

/* $0059AC: two boxes (head kills at once, body), `jsr HitBox` twice */
HLE_FUNC_PURE(0x0059AC, EndoHitHead, 600)
{
	ctx->a[2] = 0x59E6;
	ctx->a[3] = 0x59CE;
	HLE_SEQ(ctx, 8, 8, 18);
	HitBox(ctx);
	ctx->a[2] = 0x5A02;
	ctx->a[3] = 0x59C6;
	HLE_SEQ(ctx, 8, 8, 18);
	HitBox(ctx);
	hle_insn(ctx, 16);                                  /* rts */
}

/* $005AF8: two boxes, the second by `jmp HitBox` */
HLE_FUNC_PURE(0x005AF8, EndoHit3, 600)
{
	ctx->a[2] = 0x5B10;
	ctx->a[3] = 0x5BE4;
	HLE_SEQ(ctx, 8, 8, 18);
	HitBox(ctx);
	lea_lea_jmp(ctx, 0x5B2E, 0x5BDC);
}

/* $005BEC: box row chosen by the animation counter (+$14) */
HLE_FUNC_PURE(0x005BEC, EndoHit2, 400)
{
	ctx->a[3] = 0x5C3E;
	hle_insn(ctx, 8);                                   /* lea (pc),a3 */
	movew_m_d(ctx, 0, A6 + 0x14);
	andiw_d(ctx, 0, 0xF0);
	eoriw_d(ctx, 0, 0xF0);
	set_w(&ctx->d[0], asr16(ctx, ctx->d[0] & 0xFFFF, 1));
	ctx->a[3] = ctx->a[3] + (uint32_t)(int32_t)sx(ctx->d[0]);   /* lea (0,a3,d0.w),a3 */
	hle_insn(ctx, 12);
	ctx->a[2] = 0x5C0A;
	hle_insn(ctx, 8);
	hle_insn(ctx, 10);                                  /* jmp HitBox */
	HitBox(ctx);
}
