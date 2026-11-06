/* Level-3 (escort John Connor's truck) object types: AI states and hit tests of the flying HK
 * ($0E), the endoskeleton variant ($1D), the explosion ($09), the edge object ($1E) and the
 * background structure ($07). docs/objects_types.md ("Level 3"), src/t2.s.
 *
 *   $0E flying HK:  AI $00C0AA (fly in), $00C0E8 (flying, tile art), $00C332 (plain sprite),
 *                   after a hit $00C252 / $00C264 (dying, sparks), $00C242 (free + tile cleanup);
 *                   hit tests $005CC6, $005CEE (box by animation frame)
 *   $1D endoskeleton: AI $00D674 / $00D6CA (two-way walk with a shot sound)
 *   $09 explosion:  AI $00C4E4 / $00C4EA (+ shared $00C4F4), $00C54A (puff, hit state), hit $006042
 *   $1E edge object: AI $00D712 -> $00D746 -> $00D800 -> $00D89E -> $00D8DC, and $00D90E ->
 *                   $00D93E -> $00D962
 *   $07 background: AI $00BD54 (tile rects + eight column-scroll words), $00BD22 (after the hit),
 *                   $00BE2E (free); hit test $005DC0
 * (the truck $1B and the types ported elsewhere are not touched; the on-hit callbacks the hit
 * tests hand to ShotHitTest stay 68k.)
 *
 * Like ai_simple.c / ai_hktank.c: every AI state is a range from its entry (reached by the
 * `jmp (a0)` of ObjRunAll) to `bra.w ObjNext` (the C range at $00B3BE, reached with hle_jump).
 * C callees (ObjScrollX, ObjMoveX, ObjKillIf*, ObjQueueSprite, QueueSprite, Random, HitBox) are
 * called directly after their jsr is charged; the other callees (PlayerDamageNearest sub_009054/
 * sub_009078, the sound calls sub_013D04/sub_013DD6, VdpQueueTileRect/Mirror/FillRows, sub_010666,
 * sub_007DEE, sub_01253C/sub_012560/sub_01254E, sub_00BABC, ...) run as 68k through hle_call68k, so
 * a state that reaches one is a plain HLE_RANGE and the others HLE_RANGE_PURE. Every instruction is
 * on the instruction timeline (GPGX costs, see ai_endo.c / ai_hktank.c).
 *
 * Decline bounds (non-pure ranges) = observed max x 1.5 (T2_HLE_BOUNDS over the 42 states; the
 * difftest max of the original where it was higher): FlyHk 10.5k, FlyHkFree 8.5k, FlyHkDie2 8.6k,
 * Edge1eStrips 46k (sub_007DEE strips), Edge1eFlash 14.6k, BgStructFree 40k, BgStructHit 4.5k, the rest
 * 0.6k..3.1k. */
#include "hle.h"

#include <stdint.h>

void ObjQueueSprite(hle_ctx *ctx);
void Random(hle_ctx *ctx);
void HitBox(hle_ctx *ctx);

#define OBJ_COUNT   0xFF80DAu
#define EXIT_NEXT   0x00B3BEu
#define TILE_RECT   0x012234u        /* VdpQueueTileRect */
#define TILE_MIRROR 0x01227Au        /* VdpQueueTileRectMirror */

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

static int c_z(hle_ctx *c) { return (c->sr & CC_Z) != 0; }
static int c_n(hle_ctx *c) { return (c->sr & CC_N) != 0; }
static int c_c(hle_ctx *c) { return (c->sr & CC_C) != 0; }
static int c_v(hle_ctx *c) { return (c->sr & CC_V) != 0; }
static int c_ge(hle_ctx *c) { return c_n(c) == c_v(c); }
static int c_le(hle_ctx *c) { return c_z(c) || c_n(c) != c_v(c); }
static int c_gt(hle_ctx *c) { return !c_z(c) && c_n(c) == c_v(c); }

/* conditional branches: charge, return whether taken */
static int bs(hle_ctx *c, int taken) { hle_insn(c, taken ? 10 : 8); return taken; }    /* Bcc.s */
static int bw(hle_ctx *c, int taken) { hle_insn(c, taken ? 10 : 12); return taken; }   /* Bcc.w */

static void exit_next(hle_ctx *c)               /* bra.w ObjNext */
{
	hle_insn(c, 10);
	hle_jump(c, EXIT_NEXT);
}

static inline int16_t sx(uint32_t v) { return (int16_t)(v & 0xFFFF); }
static inline uint16_t rd16(uint32_t a)
{
	return ((a & 0xFFFFFFu) >= 0xFF0000u) ? RAM16(a) : ROM16(a);
}
static inline uint8_t rd8(uint32_t a)
{
	return ((a & 0xFFFFFFu) >= 0xFF0000u) ? RAM8(a) : ROM8(a);
}

/* ---- instruction helpers (each charges its own cost) */
static void moveq(hle_ctx *c, int r, int v)
{
	c->d[r] = (uint32_t)(int32_t)v;
	cc_logic32(c, c->d[r]);
	hle_insn(c, 4);
}
/* move.w <src>,Dn ; the cost depends on the source (#imm 8, (d16,An) 12, (abs).l 16, Dn 4) */
static void mvw_d(hle_ctx *c, int r, uint16_t v, int cost)
{
	set_w(&c->d[r], v);
	cc_logic16(c, v);
	hle_insn(c, cost);
}
static void mvw_i(hle_ctx *c, int r, uint16_t v) { mvw_d(c, r, v, 8); }
static void mvw_dd(hle_ctx *c, int s, int r) { mvw_d(c, r, (uint16_t)c->d[s], 4); }
static void mvw_m(hle_ctx *c, int r, uint32_t a) { mvw_d(c, r, RAM16(a), 12); }
static void mvw_abs(hle_ctx *c, int r, uint32_t a) { mvw_d(c, r, RAM16(a), 16); }
/* move.w Dn,(d16,An) 12 / (abs).l 16 */
static void mvw_st(hle_ctx *c, uint32_t a, uint16_t v, int cost)
{
	RAM16(a) = v;
	cc_logic16(c, v);
	hle_insn(c, cost);
}
/* move.w #imm,(d16,An) 16 / (abs).l 20 */
static void mvw_im(hle_ctx *c, uint32_t a, uint16_t v, int cost) { mvw_st(c, a, v, cost); }
static void mvl_im(hle_ctx *c, uint32_t a, uint32_t v)     /* move.l #imm,(d16,An) */
{
	RAM32_W(a, v);
	cc_logic32(c, v);
	hle_insn(c, 24);
}
static void andiw(hle_ctx *c, int r, uint16_t m)
{
	uint16_t v = (uint16_t)c->d[r] & m;
	set_w(&c->d[r], v);
	cc_logic16(c, v);
	hle_insn(c, 8);
}
static void addiw(hle_ctx *c, int r, uint16_t i)
{
	set_w(&c->d[r], alu(c, OP_ADD, c->d[r], i, 16, 1));
	hle_insn(c, 8);
}
static void subiw(hle_ctx *c, int r, uint16_t i)
{
	set_w(&c->d[r], alu(c, OP_SUB, c->d[r], i, 16, 1));
	hle_insn(c, 8);
}
static void eoriw(hle_ctx *c, int r, uint16_t i)
{
	uint16_t v = (uint16_t)c->d[r] ^ i;
	set_w(&c->d[r], v);
	cc_logic16(c, v);
	hle_insn(c, 8);
}
static void addq1w(hle_ctx *c, int r)
{
	set_w(&c->d[r], alu(c, OP_ADD, c->d[r], 1, 16, 1));
	hle_insn(c, 4);
}
static void subq1w(hle_ctx *c, int r)
{
	set_w(&c->d[r], alu(c, OP_SUB, c->d[r], 1, 16, 1));
	hle_insn(c, 4);
}
static void cmpiw_d(hle_ctx *c, int r, uint16_t i)
{
	alu(c, OP_SUB, c->d[r], i, 16, 0);
	hle_insn(c, 8);
}
static void cmpiw_m(hle_ctx *c, uint32_t a, uint16_t i, int cost)
{
	alu(c, OP_SUB, RAM16(a), i, 16, 0);
	hle_insn(c, cost);
}
static void tstw_d(hle_ctx *c, int r)
{
	cc_logic16(c, c->d[r]);
	hle_insn(c, 4);
}
/* subq.w #1,(d16,An) 16 / (abs).l 20 */
static void subq1w_m(hle_ctx *c, uint32_t a, int cost)
{
	RAM16(a) = (uint16_t)alu(c, OP_SUB, RAM16(a), 1, 16, 1);
	hle_insn(c, cost);
}
static void btst_m(hle_ctx *c, uint32_t a, int bit, int cost)   /* btst.b #n,<mem> */
{
	cc_set(c, CC_Z, ((RAM8(a) >> bit) & 1) ? 0 : CC_Z);
	hle_insn(c, cost);
}
static void btst_d(hle_ctx *c, int r, int bit)                  /* btst #n,Dn */
{
	cc_set(c, CC_Z, ((c->d[r] >> bit) & 1) ? 0 : CC_Z);
	hle_insn(c, 10);
}
static void bset_m(hle_ctx *c, uint32_t a, int bit)             /* bset.b #n,(An) */
{
	uint8_t o = RAM8(a);
	cc_set(c, CC_Z, ((o >> bit) & 1) ? 0 : CC_Z);
	RAM8(a) = o | (uint8_t)(1u << bit);
	hle_insn(c, 16);
}
static void bclr_m(hle_ctx *c, uint32_t a, int bit)             /* bclr.b #n,(An) */
{
	uint8_t o = RAM8(a);
	cc_set(c, CC_Z, ((o >> bit) & 1) ? 0 : CC_Z);
	RAM8(a) = o & (uint8_t)~(1u << bit);
	hle_insn(c, 16);
}
/* shifts on the low word by an immediate n (1..8); the cost is 6 + 2n; kind 'l' asl, 'r' lsr, 'a' asr */
static void shiftw(hle_ctx *c, int r, char kind, int n)
{
	uint32_t v = c->d[r] & 0xFFFF, res;
	unsigned f = 0;
	if (kind == 'l') {
		res = (v << n) & 0xFFFF;
		if ((v >> (16 - n)) & 1) f |= CC_C | CC_X;
		uint32_t top = (v >> (15 - n)) & ((1u << (n + 1)) - 1);
		if (top != 0 && top != ((1u << (n + 1)) - 1)) f |= CC_V;
	} else if (kind == 'r') {
		res = v >> n;
		if ((v >> (n - 1)) & 1) f |= CC_C | CC_X;
	} else {
		res = (uint32_t)((int32_t)(int16_t)v >> n) & 0xFFFF;
		if ((v >> (n - 1)) & 1) f |= CC_C | CC_X;
	}
	if (res & 0x8000) f |= CC_N;
	if (!res) f |= CC_Z;
	cc_set(c, CC_X | CC_N | CC_Z | CC_V | CC_C, f);
	set_w(&c->d[r], res);
	hle_insn(c, 6 + 2 * n);
}
static void lea_(hle_ctx *c, int an, uint32_t v, int cost)
{
	c->a[an] = v;
	hle_insn(c, cost);
}
static void push_w(hle_ctx *c, int r)                           /* move.w Dn,-(sp) */
{
	push16(c, (uint16_t)c->d[r]);
	cc_logic16(c, c->d[r]);
	hle_insn(c, 8);
}
static void pop_w(hle_ctx *c, int r)                            /* move.w (sp)+,Dn */
{
	mvw_d(c, r, pop16(c), 8);
}
/* jsr/bsr to C: charge the jump, the callee charges its rts */
static void jsr_c(hle_ctx *c, void (*f)(hle_ctx *))
{
	hle_insn(c, 18);
	f(c);
}
/* jsr/bsr to 68k code: charge the jump (18, (d16,pc)), run the callee as the original (its own rts) */
static void jsr68(hle_ctx *c, uint32_t addr)
{
	hle_insn(c, 18);
	hle_sync(c);
	hle_call68k(c, addr);
}

/* clr.w (a6) ; subq.w #1,(obj_count).l : free the slot */
static void free_slot(hle_ctx *c)
{
	RAM16(c->a[6]) = 0;
	cc_set(c, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	hle_insn(c, 12);
	subq1w_m(c, OBJ_COUNT, 20);
}


void ObjMoveX(hle_ctx *ctx);
void ObjScrollX(hle_ctx *ctx);
void ObjKillIfLeftOf(hle_ctx *ctx);
void ObjKillIfRightOf(hle_ctx *ctx);

#define TILE_BASE_34 0xFF9E34u
#define TILE_BASE_36 0xFF9E36u
#define TILE_BASE_38 0xFF9E38u
#define LEVEL_ID     0xFF80CCu
#define VDP_LIST_PTR 0xFF80BCu
#define FILL_ROWS    0x012212u
void QueueSprite(hle_ctx *ctx);

/* ---- helpers for this file */
static void jsr68_abs(hle_ctx *c, uint32_t addr)                /* jsr (abs).l to 68k code */
{
	hle_insn(c, 20);
	hle_sync(c);
	hle_call68k(c, addr);
}
static void addw_ea(hle_ctx *c, int r, uint16_t v, int cost)    /* add.w <ea>,Dn */
{
	set_w(&c->d[r], alu(c, OP_ADD, c->d[r], v, 16, 1));
	hle_insn(c, cost);
}
static void subw_ea(hle_ctx *c, int r, uint16_t v, int cost)    /* sub.w <ea>,Dn */
{
	set_w(&c->d[r], alu(c, OP_SUB, c->d[r], v, 16, 1));
	hle_insn(c, cost);
}
static void addq1w_abs(hle_ctx *c, uint32_t a)                  /* addq.w #1,(abs).l */
{
	RAM16(a) = (uint16_t)alu(c, OP_ADD, RAM16(a), 1, 16, 1);
	hle_insn(c, 20);
}
static void mvb_idx(hle_ctx *c, int r, uint32_t tab, int ix)    /* move.b (0,An,Dx.w),Dn from ROM */
{
	set_b(&c->d[r], rd8(tab + (uint32_t)(int32_t)sx(c->d[ix])));
	cc_logic8(c, c->d[r]);
	hle_insn(c, 14);
}
/* move.w #imm,Dn pairs of a tile-rect call: d2, d3, d1, d6 */
static void rect_call(hle_ctx *ctx, uint32_t fn, uint32_t tilebase)
{
	push_w(ctx, 7);
	mvw_abs(ctx, 7, tilebase);
	jsr68(ctx, fn);
	pop_w(ctx, 7);
}

static void kill_left(hle_ctx *ctx, int v)
{
	moveq(ctx, 0, v);
	jsr_c(ctx, ObjKillIfLeftOf);
}
static void kill_right_360(hle_ctx *ctx)
{
	mvw_i(ctx, 0, 0x168);
	jsr_c(ctx, ObjKillIfRightOf);
}

/* ------------------------------------------------------------------ $0E flying HK */
/* $00C332 / $00D962 (type $1E): scroll, kill left of -40, one fixed sprite frame */
static void plain_sprite(hle_ctx *ctx, uint16_t frame)
{
	jsr_c(ctx, ObjScrollX);
	kill_left(ctx, -0x28);
	mvw_i(ctx, 1, frame);
	jsr_c(ctx, ObjQueueSprite);
	exit_next(ctx);
}
HLE_RANGE_PURE(0x00C332, FlyHkPlain, 400, EXIT_NEXT) { plain_sprite(ctx, 0x18F); }

/* $00C0AA: flies in, the counter picks the frame; at 0 -> $00C0E8 with bit 7 set */
HLE_RANGE(0x00C0AA, FlyHkIn, 1200, EXIT_NEXT)
{
	jsr68(ctx, 0x009078);                                               /* bsr.w sub_009078 */
	subq1w_m(ctx, A6 + 0x24, 16);
	if (!bs(ctx, !c_z(ctx))) {
		mvl_im(ctx, A6 + 0x20, 0xC0E8);
		bset_m(ctx, A6, 7);
		mvw_im(ctx, A6 + 0x24, 0x50, 16);
	}
	mvw_m(ctx, 1, A6 + 0x24);                                           /* loc_00C0C6 */
	shiftw(ctx, 1, 'r', 2);
	andiw(ctx, 1, 3);
	addiw(ctx, 1, 0x191);
	jsr_c(ctx, ObjQueueSprite);
	btst_m(ctx, A6 + 0x25, 0, 16);
	if (!bs(ctx, c_z(ctx)))
		subq1w_m(ctx, A6 + 4, 16);
	exit_next(ctx);
}

/* $00C0E8: the winged HK proper: frame from the counter, art as two tile rects (plain and mirrored) */
HLE_RANGE(0x00C0E8, FlyHk, 10500, EXIT_NEXT)
{
	jsr68(ctx, 0x009078);                                               /* bsr.w sub_009078 */
	subq1w_m(ctx, A6 + 0x24, 16);
	if (!bs(ctx, !c_z(ctx)))
		mvl_im(ctx, A6 + 0x20, 0xC242);
	btst_m(ctx, A6 + 0x25, 0, 16);                                      /* loc_00C0FA */
	if (!bs(ctx, c_z(ctx)))
		subq1w_m(ctx, A6 + 4, 16);
	cmpiw_m(ctx, A6 + 0x24, 0xA, 16);                                   /* loc_00C106 */
	if (!bs(ctx, !c_z(ctx))) {
		mvw_i(ctx, 0, 0x63);
		jsr68_abs(ctx, 0x013D04);
	}
	mvw_i(ctx, 0, 0x4F);                                                /* loc_00C118 */
	subw_ea(ctx, 0, RAM16(A6 + 0x24), 12);
	lea_(ctx, 1, 0xC196, 8);
	mvb_idx(ctx, 0, 0xC196, 0);
	shiftw(ctx, 0, 'l', 3);
	lea_(ctx, 1, 0xC2B2, 8);
	ctx->a[1] += (uint32_t)(int32_t)sx(ctx->d[0]);
	hle_insn(ctx, 12);                                                  /* lea.l (0,a1,d0.w),a1 */
	push32(ctx, ctx->a[1]);
	cc_logic32(ctx, ctx->a[1]);
	hle_insn(ctx, 12);                                                  /* move.l a1,-(sp) */
	for (int pass = 0; pass < 2; pass++) {
		mvw_d(ctx, 0, ROM16(ctx->a[1]), 8);                         /* move.w (a1)+,d0 */
		ctx->a[1] += 2;
		lea_(ctx, 0, 0x0BF71C, 12);
		ctx->a[0] += (uint32_t)(int32_t)sx(ctx->d[0]);
		hle_insn(ctx, 12);                                          /* lea.l (0,a0,d0.w),a0 */
		mvw_d(ctx, 2, ROM16(ctx->a[1]), 8);
		ctx->a[1] += 2;
		mvw_d(ctx, 3, ROM16(ctx->a[1]), 8);
		ctx->a[1] += 2;
		mvw_i(ctx, 1, 0xC100);
		addw_ea(ctx, 1, RAM16(A6 + 0xA), 12);                       /* add.w (0xa,a6),d1 */
		addw_ea(ctx, 1, ROM16(ctx->a[1]), 8);                       /* add.w (a1)+,d1 */
		ctx->a[1] += 2;
		if (pass == 0) {
			subw_ea(ctx, 1, (uint16_t)ctx->d[2], 4);            /* sub.w d2,d1 */
			subw_ea(ctx, 1, (uint16_t)ctx->d[2], 4);
		}
		mvw_i(ctx, 6, 0);
		rect_call(ctx, pass == 0 ? TILE_RECT : TILE_MIRROR, TILE_BASE_36);
		if (pass == 0) {
			ctx->a[1] = pop32(ctx);
			hle_insn(ctx, 12);                                  /* movea.l (sp)+,a1 */
		}
	}
	exit_next(ctx);
}

/* sub_00C1E6 (after its jsr was charged): the 18x8 tile rect that clears the art, d7 = $FF9E38 base */
static void sub_c1e6(hle_ctx *ctx)
{
	lea_(ctx, 0, 0x065264, 12);
	moveq(ctx, 2, 0x12);
	moveq(ctx, 3, 8);
	mvw_m(ctx, 1, A6 + 0xA);
	addiw(ctx, 1, 0xC0EE);
	moveq(ctx, 6, 0);
	rect_call(ctx, TILE_RECT, TILE_BASE_38);
	hle_insn(ctx, 16);                                                  /* rts */
}

/* $00C242: free the slot, clear the art */
HLE_RANGE(0x00C242, FlyHkFree, 8500, EXIT_NEXT)
{
	RAM16(A6) = 0;
	cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	hle_insn(ctx, 12);                                                  /* clr.w (a6) */
	hle_insn(ctx, 18);                                                  /* jsr (sub_00C1E6,pc) */
	sub_c1e6(ctx);
	subq1w_m(ctx, OBJ_COUNT, 20);
	exit_next(ctx);
}

/* sub_00C278: store the counter (d0), every odd frame spark from Random via sub_01253C */
static void fly_sparks(hle_ctx *ctx)
{
	mvw_st(ctx, A6 + 0x24, (uint16_t)ctx->d[0], 12);                    /* loc_00C278 */
	btst_d(ctx, 0, 0);
	if (!bs(ctx, c_z(ctx))) {
		jsr_c(ctx, Random);
		andiw(ctx, 0, 0x1F);
		mvw_dd(ctx, 0, 1);
		jsr_c(ctx, Random);
		andiw(ctx, 0, 0x1F);
		addw_ea(ctx, 0, RAM16(A6 + 2), 12);
		subiw(ctx, 0, 0x10);
		addw_ea(ctx, 1, RAM16(A6 + 4), 12);
		subiw(ctx, 1, 0x20);
		mvw_m(ctx, 2, A6 + 6);
		set_b(&ctx->d[2], alu(ctx, OP_SUB, ctx->d[2], 1, 8, 1));
		hle_insn(ctx, 4);                                           /* subq.b #1,d2 */
		jsr68(ctx, 0x01253C);
	}
	exit_next(ctx);                                                     /* loc_00C2AE */
}


/* $00C252: dying counter; at 0 free the slot */
HLE_RANGE(0x00C252, FlyHkDie, 2100, EXIT_NEXT)
{
	mvw_m(ctx, 0, A6 + 0x24);
	subq1w(ctx, 0);
	if (!bs(ctx, !c_z(ctx))) {                                          /* bne.s sub_00C278 */
		free_slot(ctx);
		hle_insn(ctx, 10);                                          /* bra.s sub_00C278 */
	}
	fly_sparks(ctx);
}

/* $00C264: same, but the last frame also clears the art */
HLE_RANGE(0x00C264, FlyHkDie2, 8600, EXIT_NEXT)
{
	mvw_m(ctx, 0, A6 + 0x24);
	subq1w(ctx, 0);
	if (!bs(ctx, !c_z(ctx))) {
		hle_insn(ctx, 18);                                          /* jsr (sub_00C1E6,pc) */
		sub_c1e6(ctx);
		free_slot(ctx);
	}
	fly_sparks(ctx);
}

/* hit tests ($1C routines): a3 = box, a2 = on-hit routine, jmp HitBox */
HLE_FUNC_PURE(0x005CC6, HitFlyHkPlain, 250)
{
	lea_(ctx, 2, 0x005CD2, 8);
	lea_(ctx, 3, 0x005CE6, 8);
	hle_insn(ctx, 10);
	HitBox(ctx);
}

HLE_FUNC_PURE(0x005CEE, HitFlyHk, 300)
{
	lea_(ctx, 2, 0x005D9C, 8);
	mvw_i(ctx, 0, 0x4F);
	subw_ea(ctx, 0, RAM16(A6 + 0x24), 12);
	lea_(ctx, 1, 0xC196, 8);
	mvb_idx(ctx, 0, 0xC196, 0);
	shiftw(ctx, 0, 'l', 3);
	ctx->a[3] = 0x005D1C + (uint32_t)(int32_t)sx(ctx->d[0]);
	hle_insn(ctx, 12);                                                  /* lea.l (dat_005D1C,pc,d0.w),a3 */
	hle_insn(ctx, 10);
	HitBox(ctx);
}

/* ------------------------------------------------------------------ $09 explosion */
/* sub_00C4F4 (d1 = sprite frame): blink (no sprite while bit 2 of the counter), every 8th frame a
 * spark via sub_012560, counter--, free at 0 */
static void bomb_tail(hle_ctx *ctx)
{
	mvw_m(ctx, 0, A6 + 0x24);
	btst_d(ctx, 0, 2);
	if (!bs(ctx, !c_z(ctx)))
		jsr_c(ctx, ObjQueueSprite);
	mvw_m(ctx, 0, A6 + 0x24);                                           /* loc_00C502 */
	andiw(ctx, 0, 7);
	if (!bs(ctx, !c_z(ctx))) {
		jsr_c(ctx, Random);
		andiw(ctx, 0, 0x3F);
		mvw_dd(ctx, 0, 1);
		jsr_c(ctx, Random);
		andiw(ctx, 0, 0x3F);
		addw_ea(ctx, 0, RAM16(A6 + 2), 12);
		subiw(ctx, 0, 0x20);
		addw_ea(ctx, 1, RAM16(A6 + 4), 12);
		subiw(ctx, 1, 0x20);
		mvw_m(ctx, 2, A6 + 6);
		set_b(&ctx->d[2], alu(ctx, OP_SUB, ctx->d[2], 1, 8, 1));
		hle_insn(ctx, 4);                                           /* subq.b #1,d2 */
		jsr68(ctx, 0x012560);
	}
	subq1w_m(ctx, A6 + 0x24, 16);                                       /* loc_00C538 */
	if (!bs(ctx, !c_z(ctx)))
		free_slot(ctx);
	exit_next(ctx);                                                     /* loc_00C546 */
}

HLE_RANGE(0x00C4E4, BombFixed, 2200, EXIT_NEXT)
{
	mvw_i(ctx, 1, 0x15E);
	hle_insn(ctx, 10);                                                  /* bra.s sub_00C4F4 */
	bomb_tail(ctx);
}

HLE_RANGE(0x00C4EA, BombAnim, 2100, EXIT_NEXT)
{
	mvw_m(ctx, 1, A6 + 0x14);
	shiftw(ctx, 1, 'a', 4);
	addiw(ctx, 1, 0xB8);
	bomb_tail(ctx);
}

/* $00C54A: the puff after a hit (type $09 hit state): one spark at (x, y-16), free the slot */
HLE_RANGE(0x00C54A, BombPuff, 1500, EXIT_NEXT)
{
	ctx->d[0] = (uint32_t)(int32_t)(int16_t)RAM16(A6 + 2);              /* movem.w (0x2,a6),d0-d1 */
	ctx->d[1] = (uint32_t)(int32_t)(int16_t)RAM16(A6 + 4);
	hle_insn(ctx, 24);
	subiw(ctx, 1, 0x10);
	jsr68(ctx, 0x01254E);
	free_slot(ctx);
	exit_next(ctx);
}

/* $006042: the bomb's hit test (box row from the counter in +$14) */
HLE_FUNC_PURE(0x006042, HitBomb, 300)
{
	lea_(ctx, 3, 0x006074, 8);
	mvw_m(ctx, 0, A6 + 0x14);
	andiw(ctx, 0, 0xF0);
	eoriw(ctx, 0, 0xF0);
	shiftw(ctx, 0, 'a', 1);
	ctx->a[3] += (uint32_t)(int32_t)sx(ctx->d[0]);
	hle_insn(ctx, 12);                                                  /* lea.l (0,a3,d0.w),a3 */
	lea_(ctx, 2, 0x006060, 8);
	hle_insn(ctx, 10);
	HitBox(ctx);
}

static void addqw(hle_ctx *c, int r, int imm)                           /* addq.w #imm,Dn */
{
	set_w(&c->d[r], alu(c, OP_ADD, c->d[r], (uint16_t)imm, 16, 1));
	hle_insn(c, 4);
}
static void cmpiw_abs(hle_ctx *c, uint32_t a, uint16_t i)             /* cmpi.w #i,(abs).l */
{
	alu(c, OP_SUB, RAM16(a), i, 16, 0);
	hle_insn(c, 20);
}

/* ------------------------------------------------------------------ $1D endoskeleton variant */
/* common head of $D674/$D6CA: scroll + kill limits (right $168, left -$20) */
static void endo_head(hle_ctx *ctx)
{
	jsr_c(ctx, ObjScrollX);
	kill_right_360(ctx);
	kill_left(ctx, -0x20);
}

/* $00D674: walks (two sprites: body d5+6, legs d5+$64), timer 0 -> shot sound, state $D6CA */
HLE_RANGE(0x00D674, Endo1dWalk, 2500, EXIT_NEXT)
{
	endo_head(ctx);
	mvw_m(ctx, 5, A6 + 0x24);
	andiw(ctx, 5, 0xE);
	shiftw(ctx, 5, 'r', 1);
	eoriw(ctx, 5, 7);
	mvw_dd(ctx, 5, 1);
	addqw(ctx, 1, 6);
	jsr_c(ctx, ObjQueueSprite);                                         /* bsr.w ObjQueueSprite */
	mvw_dd(ctx, 5, 1);
	addiw(ctx, 1, 0x64);
	jsr_c(ctx, ObjQueueSprite);
	jsr_c(ctx, ObjMoveX);
	subq1w_m(ctx, A6 + 0x24, 16);
	if (!bs(ctx, !c_z(ctx))) {
		mvw_im(ctx, A6 + 0x24, 0x64, 16);
		mvl_im(ctx, A6 + 0x20, 0xD6CA);
		mvw_i(ctx, 0, 0x61);
		jsr68(ctx, 0x013D04);                                       /* bsr.w sub_013D04 */
	}
	exit_next(ctx);                                                     /* loc_00D6C6 */
}

/* $00D6CA: fires (sub_009054 hurts the player), timer 0 -> back to $D674 */
HLE_RANGE(0x00D6CA, Endo1dFire, 15300, EXIT_NEXT)   /* T2_HLE_BOUNDS max 10143 (misc_t5) x 1.5 */
{
	endo_head(ctx);
	mvw_m(ctx, 1, A6 + 0x24);
	andiw(ctx, 1, 0xE);
	shiftw(ctx, 1, 'r', 1);
	eoriw(ctx, 1, 7);
	addiw(ctx, 1, 0x100);
	jsr_c(ctx, ObjQueueSprite);
	jsr_c(ctx, ObjMoveX);
	subq1w_m(ctx, A6 + 0x24, 16);
	if (!bs(ctx, !c_z(ctx))) {
		mvw_im(ctx, A6 + 0x24, 0x64, 16);
		mvl_im(ctx, A6 + 0x20, 0xD674);
	}
	jsr68(ctx, 0x009054);                                               /* loc_00D70A: bsr.w sub_009054 */
	exit_next(ctx);
}

/* ------------------------------------------------------------------ $1E edge object */
/* bsr.w sub_010666 (d1 = frame): draws via the VDP list */
static void draw_010666(hle_ctx *ctx) { jsr68(ctx, 0x010666); }

/* $00D712: waits, then -> $D746 */
HLE_RANGE(0x00D712, Edge1eWait, 1000, EXIT_NEXT)
{
	subq1w_m(ctx, A6 + 0x24, 16);
	if (!bs(ctx, !c_z(ctx))) {
		mvl_im(ctx, A6 + 0x20, 0xD746);
		mvw_im(ctx, A6 + 0x24, 0x80, 16);
	}
	cmpiw_abs(ctx, LEVEL_ID, 5);                                        /* loc_00D726 */
	if (!bw(ctx, !c_z(ctx))) {
		moveq(ctx, 1, 8);
		draw_010666(ctx);
		hle_insn(ctx, 10);                                          /* bra.s loc_00D742 */
	} else {
		mvw_i(ctx, 1, 0x14A);                                       /* loc_00D73A */
		jsr_c(ctx, ObjQueueSprite);
	}
	exit_next(ctx);                                                     /* loc_00D742 */
}

/* sub_007DEE with d6 = $C000 and a0 = data: the tile strip writer (68k) */
static void strip(hle_ctx *ctx, uint32_t data)
{
	mvw_i(ctx, 6, 0xC000);
	lea_(ctx, 0, data, 12);                                             /* lea.l (abs).l,a0 */
	jsr68(ctx, 0x007DEE);
}

/* $00D746: the timer steps through four strips (at $5F, $4B, $37, $23), then -> $D800 */
HLE_RANGE(0x00D746, Edge1eStrips, 46000, EXIT_NEXT)
{
	static const struct { uint16_t v; uint32_t a0; } t[3] = {
		{ 0x5F, 0x002FFC }, { 0x4B, 0x003014 }, { 0x37, 0x003045 } };
	mvw_m(ctx, 0, A6 + 0x24);
	int done = 0;
	for (int i = 0; i < 3 && !done; i++) {
		cmpiw_d(ctx, 0, t[i].v);
		if (bs(ctx, !c_z(ctx)))
			continue;
		strip(ctx, t[i].a0);
		hle_insn(ctx, 10);                                          /* bra.s loc_00D7B6 */
		done = 1;
	}
	if (!done) {
		cmpiw_d(ctx, 0, 0x23);                                      /* loc_00D78C */
		if (!bs(ctx, !c_z(ctx))) {
			strip(ctx, 0x0030B9);
			cmpiw_abs(ctx, LEVEL_ID, 5);
			if (!bw(ctx, !c_z(ctx)))
				lea_(ctx, 0, 0x0030E1, 12);
			jsr68(ctx, 0x007DEE);                               /* loc_00D7B2 */
		}
	}
	subq1w_m(ctx, A6 + 0x24, 16);                                       /* loc_00D7B6 */
	if (!bs(ctx, !c_z(ctx))) {
		mvl_im(ctx, A6 + 0x20, 0xD800);
		mvw_im(ctx, A6 + 0x24, 0x80, 16);
		hle_insn(ctx, 10);                                          /* bra.s loc_00D7FC */
		exit_next(ctx);
		return;
	}
	cmpiw_abs(ctx, LEVEL_ID, 5);                                        /* loc_00D7CC */
	if (!bw(ctx, !c_z(ctx))) {
		moveq(ctx, 1, 8);
		btst_m(ctx, A6 + 0x25, 3, 16);
		if (!bs(ctx, c_z(ctx)))
			moveq(ctx, 1, 0x22);
		draw_010666(ctx);                                           /* loc_00D7E4 */
		hle_insn(ctx, 10);                                          /* bra.s loc_00D7FC */
	} else {
		mvw_i(ctx, 1, 0x14A);                                       /* loc_00D7EA */
		btst_m(ctx, A6 + 0x25, 3, 16);
		if (!bs(ctx, c_z(ctx)))
			addq1w(ctx, 1);
		jsr_c(ctx, ObjQueueSprite);                                 /* loc_00D7F8 */
	}
	exit_next(ctx);                                                     /* loc_00D7FC */
}

/* $00D800: clears tile rows, flashes strips + sound, -> $D89E (or $107AC on level 5) */
HLE_RANGE(0x00D800, Edge1eFlash, 14600, EXIT_NEXT)
{
	subq1w_m(ctx, A6 + 0x24, 16);
	if (!bs(ctx, !c_z(ctx))) {
		mvl_im(ctx, A6 + 0x20, 0xD89E);
		mvw_im(ctx, A6 + 0x24, 0x100, 16);
		mvw_i(ctx, 1, 0xE200);
		moveq(ctx, 2, 0x27);
		moveq(ctx, 3, 0xA);
		jsr68(ctx, FILL_ROWS);
		mvw_i(ctx, 1, 0xE82C);
		moveq(ctx, 2, 0x12);
		moveq(ctx, 3, 6);
		jsr68(ctx, FILL_ROWS);
		hle_insn(ctx, 10);                                          /* bra.s loc_00D86E */
	} else {
		btst_m(ctx, A6 + 0x25, 3, 16);                              /* loc_00D82E */
		if (!bs(ctx, c_z(ctx))) {
			strip(ctx, 0x0030B9);
			cmpiw_abs(ctx, LEVEL_ID, 5);
			if (!bw(ctx, !c_z(ctx)))
				lea_(ctx, 0, 0x0030E1, 12);
			jsr68(ctx, 0x007DEE);                               /* loc_00D856 */
			moveq(ctx, 0, 0x75);
			jsr68(ctx, 0x013D04);                               /* bsr.w sub_013D04 */
			hle_insn(ctx, 10);                                  /* bra.s loc_00D86E */
		} else {
			mvw_i(ctx, 1, 0xE494);                              /* loc_00D862 */
			moveq(ctx, 2, 0x1D);
			moveq(ctx, 3, 3);
			jsr68(ctx, FILL_ROWS);
		}
	}
	cmpiw_abs(ctx, LEVEL_ID, 5);                                        /* loc_00D86E */
	if (!bw(ctx, !c_z(ctx))) {
		cmpiw_m(ctx, A6 + 0x24, 0x100, 16);
		if (!bs(ctx, !c_z(ctx)))
			mvl_im(ctx, A6 + 0x20, 0x107AC);
		moveq(ctx, 1, 0x22);                                        /* loc_00D88A */
		draw_010666(ctx);
		hle_insn(ctx, 10);                                          /* bra.s loc_00D89A */
	} else {
		mvw_i(ctx, 1, 0x14B);                                       /* loc_00D892 */
		jsr_c(ctx, ObjQueueSprite);
	}
	exit_next(ctx);                                                     /* loc_00D89A */
}

/* $00D89E: counter down, 0 -> sound, state $D8DC; frame from the counter's bits 3..1 */
HLE_RANGE(0x00D89E, Edge1eBlink, 1000, EXIT_NEXT)
{
	subq1w_m(ctx, A6 + 0x24, 16);
	if (!bs(ctx, !c_z(ctx))) {
		mvl_im(ctx, A6 + 0x20, 0xD8DC);
		mvw_im(ctx, A6 + 0x24, 0x80, 16);
		hle_insn(ctx, 18);                                          /* jsr (sub_013DD6,pc) */
		hle_sync(ctx);
		hle_call68k(ctx, 0x013DD6);
	}
	mvw_m(ctx, 0, A6 + 0x24);                                           /* loc_00D8B6 */
	shiftw(ctx, 0, 'a', 3);
	andiw(ctx, 0, 0xE);
	mvw_d(ctx, 1, ROM16(0xD8CC + (uint32_t)(int32_t)sx(ctx->d[0])), 14);
	jsr_c(ctx, ObjQueueSprite);
	exit_next(ctx);
}

/* $00D8DC: flies off, free at 0 */
HLE_RANGE_PURE(0x00D8DC, Edge1eGone, 800, EXIT_NEXT)
{
	jsr_c(ctx, ObjScrollX);
	mvw_m(ctx, 5, A6 + 0x24);
	andiw(ctx, 5, 0x1C);
	shiftw(ctx, 5, 'r', 2);
	eoriw(ctx, 5, 7);
	mvw_dd(ctx, 5, 1);
	addiw(ctx, 1, 0x13F);
	jsr_c(ctx, ObjQueueSprite);
	jsr_c(ctx, ObjMoveX);
	subq1w_m(ctx, A6 + 0x24, 16);
	if (!bs(ctx, !c_z(ctx)))
		free_slot(ctx);
	exit_next(ctx);                                                     /* loc_00D90A */
}

/* $00D90E: second $1E variant: frame $18A/$18B, counter 0 -> sound, $D93E */
HLE_RANGE(0x00D90E, Edge1eB, 600, EXIT_NEXT)
{
	mvw_i(ctx, 1, 0x18A);
	cmpiw_m(ctx, A6 + 0x24, 0x20, 16);
	if (!bs(ctx, c_gt(ctx)))
		addq1w(ctx, 1);
	jsr_c(ctx, ObjQueueSprite);                                         /* loc_00D91C */
	subq1w_m(ctx, A6 + 0x24, 16);
	if (!bs(ctx, !c_z(ctx))) {
		jsr68_abs(ctx, 0x013DD6);
		mvl_im(ctx, A6 + 0x20, 0xD93E);
		mvw_im(ctx, A6 + 0x24, 0x1F, 16);
	}
	exit_next(ctx);                                                     /* loc_00D93A */
}

/* $00D93E: frames $18B..$18E from the counter, 0 -> $D962 */
HLE_RANGE_PURE(0x00D93E, Edge1eC, 800, EXIT_NEXT)
{
	mvw_m(ctx, 1, A6 + 0x24);
	shiftw(ctx, 1, 'r', 3);
	eoriw(ctx, 1, 3);
	addiw(ctx, 1, 0x18B);
	jsr_c(ctx, ObjQueueSprite);
	subq1w_m(ctx, A6 + 0x24, 16);
	if (!bs(ctx, !c_z(ctx)))
		mvl_im(ctx, A6 + 0x20, 0xD962);
	exit_next(ctx);                                                     /* loc_00D95E */
}

HLE_RANGE_PURE(0x00D962, Edge1eD, 500, EXIT_NEXT) { plain_sprite(ctx, 0x18E); }

/* ------------------------------------------------------------------ $07 background structure */
/* sub_00BD6C (a0 = tile data, a6 = object): two tile rects, timer--, scroll, eight column words */
static void bg_rects(hle_ctx *ctx)
{
	mvw_i(ctx, 2, 5);
	mvw_i(ctx, 3, 2);
	mvw_i(ctx, 1, 0xC360);
	mvw_i(ctx, 6, 0);
	rect_call(ctx, TILE_RECT, TILE_BASE_34);
	mvw_i(ctx, 2, 6);
	mvw_i(ctx, 3, 2);
	mvw_i(ctx, 1, 0xC460);
	mvw_i(ctx, 6, 0);
	rect_call(ctx, TILE_RECT, TILE_BASE_34);
	subq1w_m(ctx, A6 + 0x24, 16);
	lea_(ctx, 0, 0xFFFFAA9E, 12);
	jsr_c(ctx, ObjMoveX);
	mvw_m(ctx, 0, A6 + 2);
	cmpiw_d(ctx, 0, 0x1A0);
	if (!bs(ctx, !c_ge(ctx)))                                          /* blt.s loc_00BDC8 */
		mvl_im(ctx, A6 + 0x20, 0xBE2E);
	addiw(ctx, 0, 0x60);                                                /* loc_00BDC8 */
	mvw_st(ctx, ctx->a[0], (uint16_t)ctx->d[0], 8);
	for (int i = 1; i < 8; i++)
		mvw_st(ctx, ctx->a[0] + 0x20 * i, (uint16_t)ctx->d[0], 12);
	exit_next(ctx);
}

/* $00BD54: the tile art cycles by the timer (three data blocks) */
HLE_RANGE(0x00BD54, BgStruct, 3100, EXIT_NEXT)
{
	addq1w_abs(ctx, 0xFF9E92);
	mvw_m(ctx, 0, A6 + 0x24);
	andiw(ctx, 0, 0x78);
	shiftw(ctx, 0, 'r', 1);
	lea_(ctx, 1, 0x00BDEE, 8);
	ctx->a[0] = ROM32(0x00BDEE + (uint32_t)(int32_t)sx(ctx->d[0]));
	hle_insn(ctx, 18);                                                  /* movea.l (0,a1,d0.w),a0 */
	bg_rects(ctx);
}

/* $00BD22 (after the hit): two sprites via sub_00BABC + QueueSprite, then the tail of $BD54 */
HLE_RANGE(0x00BD22, BgStructHit, 4500, EXIT_NEXT)
{
	moveq(ctx, 1, 0);
	jsr68(ctx, 0x00BABC);
	addiw(ctx, 1, 0xE2);
	subiw(ctx, 2, 0x10);
	subiw(ctx, 3, 0x14);
	jsr_c(ctx, QueueSprite);
	moveq(ctx, 1, 3);
	jsr68(ctx, 0x00BABC);
	addiw(ctx, 1, 0xE2);
	subiw(ctx, 2, 0x4C);
	subiw(ctx, 3, 0x18);
	jsr_c(ctx, QueueSprite);
	ctx->a[0] = ROM32(0x00BDFA);                                        /* movea.l (dat_00BDFA,pc),a0 */
	hle_insn(ctx, 16);
	hle_insn(ctx, 10);                                                  /* bra.s sub_00BD6C */
	bg_rects(ctx);
}

/* $00BE2E: free the slot and clean up (68k helpers) */
HLE_RANGE(0x00BE2E, BgStructFree, 40000, EXIT_NEXT)
{
	RAM16(A6) = 0;
	cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	hle_insn(ctx, 12);                                                  /* clr.w (a6) */
	jsr68(ctx, 0x009866);
	jsr68(ctx, 0x00BE42);
	subq1w_m(ctx, OBJ_COUNT, 20);
	exit_next(ctx);
}

HLE_FUNC_PURE(0x005DC0, HitBgStruct, 250)
{
	lea_(ctx, 2, 0x005DCC, 8);
	lea_(ctx, 3, 0x005E14, 8);
	hle_insn(ctx, 10);
	HitBox(ctx);
}
