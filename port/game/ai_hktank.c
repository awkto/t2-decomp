/* The level-1 boss, the HK tank: AI states and hit tests of its five object types.
 * docs/objects_types.md ("Level 1"), docs/subsystems/objects.md; the boss is spawned by
 * sub_009F24 (src/t2.s) and the parts share RAM:
 *
 *   $FF9ECA  (long)  slot of the body ($16), read by the arms to follow it
 *   $FF9EBA  (word)  the tank's X step for this frame (+1 / -1), written by the body, added to
 *                    X (+2) by the hull, turret and (while dying) every part
 *   $FF9EBC  (byte)  parts-destroyed bits: 0/1 arms, 2 turret, 3 hull ($0F = body can die);
 *                    set by the parts' kill routines ($006154.. in the 68k), $FF = body dead
 *   $FF9E48  (word)  tile base added to the tile rects (d7)
 *
 * Types: $16 body (AI $00CB34 approach, $00CB9E retreat, both end in the tile-animation tail
 * $00CBE8; death $00CC98 -> explosions $00CCE6, final countdown $00CD1C), $17 hull ($00CDFE,
 * death $00CE0C), $18 turret ($00CE46 + fire helper $00CF58, death $00CEC4), $19 arm ($00CF9E,
 * death $00CFDE), shared dying state $00CEFA (hull, turret, arms; the hull's last frame clears the
 * boss tile rows), $1A weak point ($00D028 follows a movement script via sub_00C48E, then $00D08C
 * blinks and hurts the player; $00D0C8 frees a slot). Hit tests $006126 $006174 $0061CA $006208
 * $006004 (a3 = box table, a2 = on-hit routine, `jmp HitBox`). The on-hit routines
 * ($006146 ..) and the callee sub_0072B2 stay 68k.
 *
 * Like ai_simple.c: every AI state is a range from its entry (reached by the `jmp (a0)` of ObjRunAll)
 * to `bra.w ObjNext`, which is the C range at $00B3BE (hle_jump). C callees (Random, ObjQueueSprite)
 * are called directly after their jsr is charged; the others (VdpQueueTileRect/Mirror/FillRows,
 * sub_0081B6, sub_009A02, sub_0099D4, sub_012494) run as 68k through hle_call68k, so a state
 * that reaches one is a plain HLE_RANGE (bound measured with T2_HLE_BOUNDS).
 *
 * The ScriptNext calls of $00CD1C (boss end, once per game) and PlayerDamageNearest
 * ($00908A) of $00D08C stay 68k: the range stops right before the `jsr`/`bsr` ($00CD36 / $00CD4A
 * / $00D0B4), the real instruction runs, and $00CD3A / $00CD4E continue as ranges.
 *
 * Decline bounds (non-pure ranges) = observed max x 1.5 (T2_HLE_BOUNDS over the boss states; HkBodyDie, HkBodyEnd2:
 * dt original max 23.2k / 13.6k): BodyLeft/Right 5.9k, Turret 2.3k, PartDying 1.9k, HullDie 1.9k, TurretDie 1.8k, ArmDie 2.3k.
 * 68000 costs (GPGX): see ai_endo.c / ai_simple.c; each hle_insn below is annotated. */
#include "hle.h"

#include <stdint.h>

void ObjQueueSprite(hle_ctx *ctx);
void Random(hle_ctx *ctx);
void HitBox(hle_ctx *ctx);

#define OBJ_COUNT   0xFF80DAu
#define BOSS_STEP   0xFF9EBAu
#define BOSS_PARTS  0xFF9EBCu
#define BOSS_BODY   0xFF9ECAu
#define TILE_BASE   0xFF9E48u
#define EXIT_NEXT   0x00B3BEu
#define TILE_RECT   0x012234u        /* VdpQueueTileRect */
#define TILE_MIRROR 0x01227Au        /* VdpQueueTileRectMirror */
#define FILL_ROWS   0x012212u        /* VdpQueueFillRows */
#define DAT_04E26A  0x04E26Au

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
/* shifts on the low word by an immediate n (1..8); the cost is 6 + 2n */
static void shiftw(hle_ctx *c, int r, char kind, int n)
{
	uint32_t v = c->d[r] & 0xFFFF, res;
	unsigned f = 0;
	if (kind == 'r') {
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
static void push_a6(hle_ctx *c)                                 /* move.l a6,-(sp) */
{
	push32(c, c->a[6]);
	cc_logic32(c, c->a[6]);
	hle_insn(c, 12);
}
static void pop_a6(hle_ctx *c)                                  /* movea.l (sp)+,a6 */
{
	c->a[6] = pop32(c);
	hle_insn(c, 12);
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

/* move.w (abs).l,d7 ; the d7 tile base of every rect */
static void d7_tilebase(hle_ctx *c) { mvw_abs(c, 7, TILE_BASE); }

/* ------------------------------------------------------------------ body tile animation */
/* sub_00CC3C / sub_00CC6A (after their jsr was charged): d1 = VDP address, d4 = frame offset,
 * the rect 4x3 from dat_04E26A + table word; Mirror selects VdpQueueTileRectMirror */
static void cc_rect(hle_ctx *c, uint32_t table, uint32_t fn)
{
	push_w(c, 4);
	mvw_d(c, 2, rd16(table + (uint32_t)(int32_t)sx(c->d[4])), 14);    /* move.w (tab,pc,d4.w),d2 */
	lea_(c, 0, DAT_04E26A, 12);
	c->a[0] += (uint32_t)(int32_t)sx(c->d[2]);
	hle_insn(c, 12);                                                    /* lea.l (0,a0,d2.w),a0 */
	moveq(c, 2, 4);
	moveq(c, 3, 3);
	mvw_i(c, 6, 0);
	d7_tilebase(c);
	jsr68(c, fn);
	pop_w(c, 4);
	hle_insn(c, 16);                                                    /* rts */
}

/* sub_00CBE8: shake the 16 row words at $FFAADC, every 4th frame queue the four tile rects;
 * ends bra.w ObjNext. d0 = X of the body */
static void body_tail(hle_ctx *ctx)
{
	lea_(ctx, 0, 0xFFFFAADC, 12);
	subiw(ctx, 0, 0x68);
	moveq(ctx, 1, 0xF);
	for (;;) {                                                          /* loc_00CBF4 */
		mvw_st(ctx, ctx->a[0], (uint16_t)ctx->d[0], 8);             /* move.w d0,(a0) */
		lea_(ctx, 0, ctx->a[0] + 0x20, 8);
		uint16_t d = (uint16_t)(ctx->d[1] - 1);                     /* dbra d1 */
		set_w(&ctx->d[1], d);
		if (d == 0xFFFF) {
			hle_insn(ctx, 14);
			break;
		}
		hle_insn(ctx, 10);
	}
	mvw_dd(ctx, 0, 4);
	andiw(ctx, 0, 3);
	if (bs(ctx, !c_z(ctx))) {                                           /* bne.s loc_00CC38 */
		exit_next(ctx);
		return;
	}
	andiw(ctx, 4, 0xC);
	shiftw(ctx, 4, 'r', 1);
	push_w(ctx, 7);
	cmpiw_m(ctx, A6 + 2, 0x140, 16);
	if (!bs(ctx, c_gt(ctx))) {                                          /* bgt.s loc_00CC1E */
		mvw_i(ctx, 1, 0xE8AC);
		hle_insn(ctx, 18);                                          /* jsr (sub_00CC3C,pc) */
		cc_rect(ctx, 0x00CC62, TILE_RECT);
	}
	mvw_i(ctx, 1, 0xE892);
	hle_insn(ctx, 18);
	cc_rect(ctx, 0x00CC62, TILE_RECT);
	mvw_i(ctx, 1, 0xE89A);
	hle_insn(ctx, 18);                                                  /* jsr (sub_00CC6A,pc) */
	cc_rect(ctx, 0x00CC90, TILE_MIRROR);
	mvw_i(ctx, 1, 0xE880);
	hle_insn(ctx, 18);
	cc_rect(ctx, 0x00CC90, TILE_MIRROR);
	pop_w(ctx, 7);
	exit_next(ctx);                                                     /* loc_00CC38 */
}

/* sub_00CD88 (d1 = table word): the small tile rect of the body's damage animation */
static void sub_cd88(hle_ctx *ctx)
{
	push_w(ctx, 7);
	lea_(ctx, 0, DAT_04E26A, 12);
	ctx->a[0] += (uint32_t)(int32_t)sx(ctx->d[1]);
	hle_insn(ctx, 12);                                                  /* lea.l (0,a0,d1.w),a0 */
	mvw_i(ctx, 1, 0xE698);
	moveq(ctx, 2, 2);
	moveq(ctx, 3, 2);
	mvw_i(ctx, 6, 0);
	d7_tilebase(ctx);
	jsr68(ctx, TILE_RECT);
	pop_w(ctx, 7);
	hle_insn(ctx, 10);                                                  /* bra.w sub_00CDFC */
	hle_insn(ctx, 16);                                                  /* rts */
}

/* sub_00CD66 (after its jsr was charged): once the turret is gone ($FF9EBC bit 2) the body
 * flickers and sparks; counter in +$2A */
static void sub_cd66(hle_ctx *ctx)
{
	subq1w_m(ctx, A6 + 0x2A, 16);
	mvw_m(ctx, 0, A6 + 0x2A);
	cmpiw_d(ctx, 0, 0x60);
	if (bw(ctx, c_ge(ctx))) {                                           /* bge.w sub_00CDFC */
		hle_insn(ctx, 16);
		return;
	}
	cmpiw_d(ctx, 0, 0x40);
	if (!bw(ctx, c_n(ctx) != c_v(ctx))) {                               /* blt.w sub_00CDC2 */
		andiw(ctx, 0, 0x18);
		shiftw(ctx, 0, 'r', 2);
		mvw_d(ctx, 1, rd16(0x00CDB8 + (uint32_t)(int32_t)sx(ctx->d[0])), 14);
		sub_cd88(ctx);
		return;
	}
	cmpiw_d(ctx, 0, 0x20);                                              /* sub_00CDC2 */
	if (!bw(ctx, c_ge(ctx))) {                                          /* bge.w loc_00CDE2 */
		tstw_d(ctx, 0);
		if (!bs(ctx, !c_z(ctx)))                                    /* bne.s loc_00CDD4 */
			mvw_im(ctx, A6 + 0x2A, 0x100, 16);
		andiw(ctx, 0, 0x18);
		shiftw(ctx, 0, 'r', 2);
		mvw_d(ctx, 1, rd16(0x00CDB0 + (uint32_t)(int32_t)sx(ctx->d[0])), 14);
		hle_insn(ctx, 10);                                          /* bra.w sub_00CD88 */
		sub_cd88(ctx);
		return;
	}
	andiw(ctx, 0, 7);                                                   /* loc_00CDE2 */
	if (bs(ctx, !c_z(ctx))) {                                           /* bne.s sub_00CDFC */
		hle_insn(ctx, 16);
		return;
	}
	push_a6(ctx);
	mvw_m(ctx, 1, A6 + 4);
	subiw(ctx, 1, 0x30);
	mvw_m(ctx, 5, A6 + 2);
	jsr68(ctx, 0x009A02);
	pop_a6(ctx);
	hle_insn(ctx, 16);                                                  /* rts */
}

/* the random delay: d5 = d0, d0 = $14 + (rnd & $7F) -> d4, d0 = d5, state = st */
static void body_retimer(hle_ctx *ctx, uint32_t st)
{
	mvw_dd(ctx, 0, 5);
	jsr_c(ctx, Random);                                                 /* bsr.w Random */
	andiw(ctx, 0, 0x7F);
	addiw(ctx, 0, 0x14);
	mvw_dd(ctx, 0, 4);
	mvw_dd(ctx, 5, 0);
	mvl_im(ctx, A6 + 0x20, st);
}

/* $00CB34 ai_hk_tank_body, first state: drive left ($FF9EBA = -1) until X = $5A */
HLE_RANGE(0x00CB34, HkBodyLeft, 8800, EXIT_NEXT)
{
	btst_m(ctx, BOSS_PARTS, 2, 20);
	if (!bs(ctx, c_z(ctx))) {                                           /* beq.s loc_00CB42 */
		hle_insn(ctx, 18);                                          /* jsr (sub_00CD66,pc) */
		sub_cd66(ctx);
	}
	mvw_m(ctx, 0, A6 + 2);
	subq1w(ctx, 0);
	mvw_im(ctx, BOSS_STEP, 0xFFFF, 20);
	mvw_st(ctx, A6 + 2, (uint16_t)ctx->d[0], 12);
	cmpiw_d(ctx, 0, 0x5A);
	if (bs(ctx, c_z(ctx))) {                                            /* beq.s loc_00CB7C */
		body_retimer(ctx, 0xCB9E);
	} else {
		mvw_m(ctx, 4, A6 + 0x24);
		cmpiw_d(ctx, 4, 0xA0);
		if (!bs(ctx, c_le(ctx))) {                                  /* ble.s loc_00CB78 */
			moveq(ctx, 1, 0);
			btst_d(ctx, 4, 2);
			if (!bs(ctx, c_z(ctx)))                             /* beq.s loc_00CB6E */
				moveq(ctx, 1, -2);
			push_w(ctx, 0);
			moveq(ctx, 0, 0);
			jsr68(ctx, 0x0081B6);
			pop_w(ctx, 0);
		}
		subq1w(ctx, 4);                                             /* loc_00CB78 */
		if (!bs(ctx, !c_z(ctx)))                                    /* bne.s loc_00CB96 */
			body_retimer(ctx, 0xCB9E);
	}
	mvw_st(ctx, A6 + 0x24, (uint16_t)ctx->d[4], 12);                    /* loc_00CB96 */
	hle_insn(ctx, 10);                                                  /* bra.w sub_00CBE8 */
	body_tail(ctx);
}

/* $00CB9E ai_hk_tank_body: drive right ($FF9EBA = +1) until X = $E6 or the timer runs out */
HLE_RANGE(0x00CB9E, HkBodyRight, 8800, EXIT_NEXT)
{
	btst_m(ctx, BOSS_PARTS, 2, 20);
	if (!bs(ctx, c_z(ctx))) {
		hle_insn(ctx, 18);
		sub_cd66(ctx);
	}
	mvw_m(ctx, 0, A6 + 2);
	addq1w(ctx, 0);
	mvw_im(ctx, BOSS_STEP, 1, 20);
	mvw_st(ctx, A6 + 2, (uint16_t)ctx->d[0], 12);
	cmpiw_d(ctx, 0, 0xE6);
	int retime = bs(ctx, c_z(ctx));                                     /* beq.s loc_00CBCA */
	if (!retime) {
		subq1w_m(ctx, A6 + 0x24, 16);
		retime = !bs(ctx, !c_z(ctx));                               /* bne.s loc_00CBE4 */
	}
	if (retime)
		body_retimer(ctx, 0xCB34);
	hle_insn(ctx, 10);                                                  /* loc_00CBE4: bra.w sub_00CBE8 */
	body_tail(ctx);
}

/* ------------------------------------------------------------------ body death */
/* sub_00CCE6: six explosions around the body, then $FF9EBC = $FF; bra ObjNext */
static void body_explode(hle_ctx *ctx)
{
	moveq(ctx, 4, 5);
	for (;;) {                                                          /* loc_00CCE8 */
		mvw_m(ctx, 1, A6 + 4);
		jsr_c(ctx, Random);
		andiw(ctx, 0, 0x10);
		set_w(&ctx->d[1], alu(ctx, OP_ADD, ctx->d[1], ctx->d[0], 16, 1));
		hle_insn(ctx, 4);                                           /* add.w d0,d1 */
		jsr_c(ctx, Random);
		andiw(ctx, 0, 0x10);
		set_w(&ctx->d[0], alu(ctx, OP_ADD, ctx->d[0], RAM16(A6 + 2), 16, 1));
		hle_insn(ctx, 12);                                          /* add.w (0x2,a6),d0 */
		mvw_m(ctx, 2, A6 + 6);
		subiw(ctx, 1, 0x10);
		jsr68(ctx, 0x012494);
		uint16_t d = (uint16_t)(ctx->d[4] - 1);                     /* dbra d4 */
		set_w(&ctx->d[4], d);
		if (d == 0xFFFF) {
			hle_insn(ctx, 14);
			break;
		}
		hle_insn(ctx, 10);
	}
	RAM8(BOSS_PARTS) = 0xFF;                                            /* st.b (abs).l */
	hle_insn(ctx, 20);
	exit_next(ctx);
}

/* $00CC98: the body dies: the two halves of the wreck are queued, then the countdown state */
HLE_RANGE(0x00CC98, HkBodyDie, 35000, EXIT_NEXT)
{
	push_w(ctx, 7);
	lea_(ctx, 0, 0x04E696, 12);
	moveq(ctx, 2, 0xD);
	moveq(ctx, 3, 8);
	mvw_i(ctx, 6, 0);
	d7_tilebase(ctx);
	mvw_i(ctx, 1, 0xE600);
	jsr68(ctx, TILE_MIRROR);
	lea_(ctx, 0, 0x04E696, 12);
	moveq(ctx, 2, 0xD);
	moveq(ctx, 3, 8);
	mvw_i(ctx, 6, 0);
	d7_tilebase(ctx);
	mvw_i(ctx, 1, 0xE61A);
	jsr68(ctx, TILE_RECT);
	pop_w(ctx, 7);
	bclr_m(ctx, A6, 7);
	mvl_im(ctx, A6 + 0x20, 0xCD1C);
	mvw_im(ctx, A6 + 0x24, 0x64, 16);
	body_explode(ctx);
}

/* $00CD52: the two "boss is dying" flags, bra ObjNext */
static void cd52(hle_ctx *ctx)
{
	mvw_im(ctx, 0xFF8044, 1, 20);
	mvw_im(ctx, 0xFF8094, 1, 20);
	exit_next(ctx);
}

/* $00CD1C: the countdown; at 0 the script goes on (ScriptNext stays 68k: the range stops
 * before the jsr at $00CD36 / $00CD4A) */
HLE_RANGE_PURE(0x00CD1C, HkBodyEnd, 800, EXIT_NEXT, 0x00CD36, 0x00CD4A)
{
	subq1w_m(ctx, A6 + 0x24, 16);
	if (bs(ctx, !c_z(ctx))) {                                           /* bne.s loc_00CD52 */
		cd52(ctx);
		return;
	}
	btst_m(ctx, A6, 3, 12);
	if (!bs(ctx, !c_z(ctx))) {                                          /* bne.s loc_00CD3C */
		free_slot(ctx);
		lea_(ctx, 0, 0x01F2E2, 12);
		hle_jump(ctx, 0x00CD36);                                    /* jsr (ScriptNext,pc) */
		return;
	}
	free_slot(ctx);                                                     /* loc_00CD3C */
	lea_(ctx, 0, 0x0001FEA0, 12);                                       /* movea.l #$1fea0,a0 */
	hle_jump(ctx, 0x00CD4A);
}

/* after the first ScriptNext: bra.s loc_00CD52 */
HLE_RANGE_PURE(0x00CD3A, HkBodyEnd1, 200, EXIT_NEXT)
{
	hle_insn(ctx, 10);
	cd52(ctx);
}

/* after the second ScriptNext: bra.w sub_00CCE6 */
HLE_RANGE(0x00CD4E, HkBodyEnd2, 20500, EXIT_NEXT)
{
	hle_insn(ctx, 10);
	body_explode(ctx);
}

/* ------------------------------------------------------------------ hull */
/* $00CDFE ai_hk_tank_hull: follow the tank's X step */
HLE_RANGE_PURE(0x00CDFE, HkHull, 120, EXIT_NEXT)
{
	mvw_abs(ctx, 0, BOSS_STEP);
	RAM16(A6 + 2) = (uint16_t)alu(ctx, OP_ADD, RAM16(A6 + 2), ctx->d[0], 16, 1);
	hle_insn(ctx, 16);
	exit_next(ctx);
}

/* $00CE0C: the hull dies: wreck tiles, then the dying state $00CEFA */
HLE_RANGE(0x00CE0C, HkHullDie, 2850, EXIT_NEXT)
{
	mvw_im(ctx, A6 + 0x24, 0x32, 16);
	push_w(ctx, 7);
	lea_(ctx, 0, 0x04E686, 12);
	moveq(ctx, 2, 8);
	moveq(ctx, 3, 5);
	mvw_i(ctx, 6, 0);
	d7_tilebase(ctx);
	mvw_i(ctx, 1, 0xE40C);
	jsr68(ctx, TILE_RECT);
	pop_w(ctx, 7);
	bclr_m(ctx, A6, 7);
	mvl_im(ctx, A6 + 0x20, 0xCEFA);
	exit_next(ctx);
}

/* ------------------------------------------------------------------ turret */
/* sub_00CF58 (after its jsr was charged): fires a shot every 16th frame while the counter < $30 */
static void turret_fire(hle_ctx *ctx)
{
	subq1w_m(ctx, A6 + 0x2A, 16);
	mvw_m(ctx, 0, A6 + 0x2A);
	cmpiw_d(ctx, 0, 0x30);
	if (bw(ctx, c_ge(ctx))) {                                           /* bge.w loc_00CF9C */
		hle_insn(ctx, 16);
		return;
	}
	tstw_d(ctx, 0);
	if (!bs(ctx, !c_z(ctx)))                                            /* bne.s loc_00CF72 */
		mvw_im(ctx, A6 + 0x2A, 0x100, 16);
	andiw(ctx, 0, 0xF);
	if (bs(ctx, !c_z(ctx))) {                                           /* bne.s loc_00CF9C */
		hle_insn(ctx, 16);
		return;
	}
	push_a6(ctx);
	mvw_m(ctx, 1, A6 + 4);
	subiw(ctx, 1, 0x10);
	mvw_m(ctx, 5, A6 + 2);
	addiw(ctx, 5, 0x18);
	jsr_c(ctx, Random);
	tstw_d(ctx, 0);
	if (!bs(ctx, c_n(ctx)))                                             /* bmi.s loc_00CF96 */
		subiw(ctx, 5, 0x30);
	jsr68(ctx, 0x0099D4);
	pop_a6(ctx);
	hle_insn(ctx, 16);                                                  /* rts */
}

/* $00CE46 ai_hk_tank_turret */
HLE_RANGE(0x00CE46, HkTurret, 3500, EXIT_NEXT)
{
	hle_insn(ctx, 18);                                                  /* jsr (sub_00CF58,pc) */
	turret_fire(ctx);
	mvw_abs(ctx, 0, BOSS_STEP);
	RAM16(A6 + 2) = (uint16_t)alu(ctx, OP_ADD, RAM16(A6 + 2), ctx->d[0], 16, 1);
	hle_insn(ctx, 16);                                                  /* add.w d0,(0x2,a6) */
	mvw_m(ctx, 0, A6 + 0x24);
	subq1w(ctx, 0);
	mvw_st(ctx, A6 + 0x24, (uint16_t)ctx->d[0], 12);
	mvw_dd(ctx, 0, 4);
	andiw(ctx, 0, 0xF);
	if (!bs(ctx, !c_z(ctx))) {                                          /* bne.s loc_00CEA0 */
		andiw(ctx, 4, 0xF0);
		shiftw(ctx, 4, 'r', 3);
		mvw_d(ctx, 1, rd16(0x00CEA4 + (uint32_t)(int32_t)sx(ctx->d[4])), 14);
		push_w(ctx, 7);
		lea_(ctx, 0, DAT_04E26A, 12);
		ctx->a[0] += (uint32_t)(int32_t)sx(ctx->d[1]);
		hle_insn(ctx, 12);
		mvw_i(ctx, 1, 0xE20A);
		moveq(ctx, 2, 0xA);
		moveq(ctx, 3, 4);
		mvw_i(ctx, 6, 0x8000);
		d7_tilebase(ctx);
		cmpiw_d(ctx, 4, 0x10);
		if (!bs(ctx, c_ge(ctx))) {                                  /* bge.s loc_00CE9A */
			jsr68(ctx, TILE_RECT);
			hle_insn(ctx, 10);                                  /* bra.s loc_00CE9E */
		} else {
			jsr68(ctx, TILE_MIRROR);
		}
		pop_w(ctx, 7);
	}
	exit_next(ctx);                                                     /* loc_00CEA0 */
}

/* $00CEC4: the turret dies */
HLE_RANGE(0x00CEC4, HkTurretDie, 2800, EXIT_NEXT)
{
	push_w(ctx, 7);
	lea_(ctx, 0, 0x04E664, 12);
	moveq(ctx, 2, 0xA);
	moveq(ctx, 3, 4);
	mvw_i(ctx, 6, 0x8000);
	d7_tilebase(ctx);
	mvw_i(ctx, 1, 0xE20A);
	jsr68(ctx, TILE_RECT);
	pop_w(ctx, 7);
	bclr_m(ctx, A6, 7);
	mvw_im(ctx, A6 + 0x24, 0x32, 16);
	mvl_im(ctx, A6 + 0x20, 0xCEFA);
	exit_next(ctx);
}

/* ------------------------------------------------------------------ dying state of hull, turret, arms */
/* $00CEFA: slide with the tank, explode every 8th frame; at 0 free the slot (the hull, type $17,
 * also clears the boss tile rows) */
HLE_RANGE(0x00CEFA, HkPartDying, 2800, EXIT_NEXT)
{
	mvw_abs(ctx, 0, BOSS_STEP);
	RAM16(A6 + 2) = (uint16_t)alu(ctx, OP_ADD, RAM16(A6 + 2), ctx->d[0], 16, 1);
	hle_insn(ctx, 16);
	subq1w_m(ctx, A6 + 0x24, 16);
	if (!bs(ctx, !c_z(ctx))) {                                          /* bne.s loc_00CF1C */
		alu(ctx, OP_SUB, RAM8(A6 + 1), 0x17, 8, 0);
		hle_insn(ctx, 16);                                          /* cmpi.b #$17,(0x1,a6) */
		if (bw(ctx, c_z(ctx))) {                                    /* beq.w loc_00CF40 */
			free_slot(ctx);                                     /* loc_00CF40 */
			moveq(ctx, 2, 0x14);
			moveq(ctx, 3, 8);
			mvw_i(ctx, 1, 0xE200);
			jsr68(ctx, FILL_ROWS);
			exit_next(ctx);
			return;
		}
		free_slot(ctx);
	}
	mvw_m(ctx, 0, A6 + 0x24);                                           /* loc_00CF1C */
	andiw(ctx, 0, 7);
	if (!bs(ctx, !c_z(ctx))) {                                          /* bne.s loc_00CF3C */
		moveq(ctx, 4, 3);
		mvw_m(ctx, 0, A6 + 2);
		mvw_m(ctx, 1, A6 + 4);
		mvw_m(ctx, 2, A6 + 6);
		subiw(ctx, 1, 0x10);
		jsr68(ctx, 0x012494);
	}
	exit_next(ctx);
}

/* ------------------------------------------------------------------ arm */
/* $00CF9E ai_hk_tank_arm: X follows the body ($FF9ECA), the frame by the counter */
HLE_RANGE_PURE(0x00CF9E, HkArm, 500, EXIT_NEXT)
{
	ctx->a[1] = RAM32(BOSS_BODY);                                       /* movea.l (abs).l,a1 */
	hle_insn(ctx, 20);
	mvw_d(ctx, 0, RAM16(ctx->a[1] + 2), 12);
	btst_m(ctx, A6, 6, 12);
	if (bs(ctx, c_z(ctx))) {                                            /* beq.s loc_00CFB4 */
		subiw(ctx, 0, 0x56);
	} else {
		addiw(ctx, 0, 0x26);
		hle_insn(ctx, 10);                                          /* bra.s loc_00CFB8 */
	}
	mvw_st(ctx, A6 + 2, (uint16_t)ctx->d[0], 12);
	subq1w_m(ctx, A6 + 0x24, 16);
	mvw_m(ctx, 1, A6 + 0x24);
	andiw(ctx, 1, 0x38);
	shiftw(ctx, 1, 'r', 3);
	{
		uint8_t b = ROM8(0x00CFD6 + (uint32_t)(int32_t)sx(ctx->d[1]));
		set_b(&ctx->d[1], b);                                       /* move.b (tab,pc,d1.w),d1 */
		cc_logic8(ctx, b);
		hle_insn(ctx, 14);
	}
	jsr_c(ctx, ObjQueueSprite);
	exit_next(ctx);
}

/* $00CFDE: the arm dies (bit 6 of +0 selects the side) */
HLE_RANGE(0x00CFDE, HkArmDie, 3500, EXIT_NEXT)
{
	push_w(ctx, 7);
	lea_(ctx, 0, 0x04E67A, 12);
	moveq(ctx, 2, 6);
	moveq(ctx, 3, 6);
	mvw_i(ctx, 6, 0);
	d7_tilebase(ctx);
	btst_m(ctx, A6, 6, 12);
	if (!bs(ctx, !c_z(ctx))) {                                          /* bne.s loc_00D004 */
		mvw_i(ctx, 1, 0xE300);
		jsr68(ctx, TILE_MIRROR);
		hle_insn(ctx, 10);                                          /* bra.s loc_00D00C */
	} else {
		mvw_i(ctx, 1, 0xE31C);
		jsr68(ctx, TILE_RECT);
	}
	pop_w(ctx, 7);
	bclr_m(ctx, A6, 7);
	mvl_im(ctx, A6 + 0x20, 0xCEFA);
	mvw_im(ctx, A6 + 0x24, 0x32, 16);
	exit_next(ctx);
}

/* ------------------------------------------------------------------ weak point ($1A) */
static void moveb_m_d(hle_ctx *c, int r, uint32_t a)
{
	uint8_t v = RAM8(a);
	set_b(&c->d[r], v);
	cc_logic8(c, v);
	hle_insn(c, 12);
}
static void extw(hle_ctx *c, int r)
{
	uint32_t v = (uint32_t)(int32_t)(int8_t)c->d[r];
	set_w(&c->d[r], v);
	cc_logic16(c, v);
	hle_insn(c, 4);
}

/* sub_00C48E (after its jsr was charged): one step of the movement script at +$16
 * (the same routine as in ai_endo.c); d4 = 0 when the counter +$14 ran out, 1 at the end
 * marker, else the dbra count ($FFFF) */
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
		mvw_d(ctx, 4, w, 8);
	}
	subq1w(ctx, 4);
	if (c_n(ctx)) {                                     /* bmi.w loc_00C4E0 */
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
		mvw_st(ctx, A6 + 0x1A, w, 16);
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

/* $00D028: follows its movement script and blinks; when the script ends goes to $00D08C */
HLE_RANGE_PURE(0x00D028, HkWeak, 1500, EXIT_NEXT)
{
	hle_insn(ctx, 18);                                                  /* jsr (sub_00C48E,pc) */
	sub_c48e(ctx);
	cc_logic8(ctx, ctx->d[4]);
	hle_insn(ctx, 4);                                                   /* tst.b d4 */
	if (bw(ctx, c_z(ctx))) {                                            /* beq.w loc_00D05A */
		mvw_im(ctx, A6 + 0x24, 0x19, 16);
		mvl_im(ctx, A6 + 0x20, 0xD08C);
		exit_next(ctx);
		return;
	}
	if (bs(ctx, !c_n(ctx))) {                                           /* bpl.s loc_00D056 */
		exit_next(ctx);
		return;
	}
	mvw_m(ctx, 1, A6 + 0x14);
	mvw_dd(ctx, 1, 2);
	shiftw(ctx, 2, 'a', 4);
	addiw(ctx, 2, 0xD);
	RAM8(A6 + 7) = (uint8_t)ctx->d[2];                                  /* move.b d2,(0x7,a6) */
	cc_logic8(ctx, ctx->d[2]);
	hle_insn(ctx, 12);
	shiftw(ctx, 1, 'a', 3);
	eoriw(ctx, 1, 0x1F);
	{
		uint8_t b = ROM8(0x00D06C + (uint32_t)(int32_t)sx(ctx->d[1]));
		set_b(&ctx->d[1], b);                                       /* move.b (tab,pc,d1.w),d1 */
		cc_logic8(ctx, b);
		hle_insn(ctx, 14);
	}
	addiw(ctx, 1, 0xF4);
	jsr_c(ctx, ObjQueueSprite);
	exit_next(ctx);
}

/* $00D08C: blinks for the countdown, then hurts the player (PlayerDamageNearest, sub_00908A, stays
 * 68k: the range stops before the bsr at $00D0B4) and frees the slot; +$26 != 0 frees it at once */
HLE_RANGE_PURE(0x00D08C, HkWeakBlink, 800, EXIT_NEXT, 0x00D0B4)
{
	cc_logic16(ctx, RAM16(A6 + 0x26));
	hle_insn(ctx, 12);                                                  /* tst.w (0x26,a6) */
	if (bs(ctx, !c_z(ctx))) {                                           /* bne.s loc_00D0BC */
		free_slot(ctx);
		exit_next(ctx);
		return;
	}
	subq1w_m(ctx, A6 + 0x24, 16);
	if (bs(ctx, c_z(ctx))) {                                            /* beq.s loc_00D0AC */
		free_slot(ctx);
		hle_jump(ctx, 0x00D0B4);                                    /* bsr.w sub_00908A */
		return;
	}
	mvw_m(ctx, 1, A6 + 0x24);
	andiw(ctx, 1, 2);
	shiftw(ctx, 1, 'a', 1);
	addiw(ctx, 1, 0xFE);
	jsr_c(ctx, ObjQueueSprite);
	hle_insn(ctx, 10);                                                  /* bra.s loc_00D0B8 */
	exit_next(ctx);
}

/* $00D0C8: free the slot */
HLE_RANGE_PURE(0x00D0C8, HkFree, 200, EXIT_NEXT)
{
	free_slot(ctx);
	exit_next(ctx);
}

/* ------------------------------------------------------------------ hit tests */
/* $006126 body / $006174 hull: while all the other parts are alive (parts byte != $0F / $07) the
 * on-hit routine is the "nothing happens" one; at the last step the object becomes shootable
 * (bit 15 of +0) and the on-hit is the damage routine */
static void hit_hull_or_body(hle_ctx *ctx, uint32_t box, uint32_t a2_idle, uint32_t a2_live, uint8_t want)
{
	lea_(ctx, 3, box, 8);
	lea_(ctx, 2, a2_idle, 8);
	{
		uint8_t b = RAM8(BOSS_PARTS);
		set_b(&ctx->d[0], b);                                       /* move.b (abs).l,d0 */
		cc_logic8(ctx, b);
		hle_insn(ctx, 16);
	}
	alu(ctx, OP_SUB, ctx->d[0], want, 8, 0);
	hle_insn(ctx, 8);                                                   /* cmpi.b #n,d0 */
	if (!bs(ctx, !c_z(ctx))) {                                          /* bne.s -> jmp */
		bset_m(ctx, A6, 7);
		lea_(ctx, 2, a2_live, 8);
	}
	hle_insn(ctx, 10);                                                  /* jmp (HitBox,pc) */
	HitBox(ctx);
}

HLE_FUNC_PURE(0x006126, HitHkBody, 250)
{
	hit_hull_or_body(ctx, 0x00616C, 0x006166, 0x006146, 0x0F);
}

HLE_FUNC_PURE(0x006174, HitHkHull, 250)
{
	hit_hull_or_body(ctx, 0x0061C2, 0x0061BC, 0x006194, 0x07);
}

/* $0061CA turret */
HLE_FUNC_PURE(0x0061CA, HitHkTurret, 250)
{
	lea_(ctx, 3, 0x006200, 8);
	lea_(ctx, 2, 0x0061D6, 8);
	hle_insn(ctx, 10);
	HitBox(ctx);
}

/* $006208 arm */
HLE_FUNC_PURE(0x006208, HitHkArm, 250)
{
	lea_(ctx, 3, 0x00624A, 8);
	lea_(ctx, 2, 0x006214, 8);
	hle_insn(ctx, 10);
	HitBox(ctx);
}

/* $006004 weak point (and the type-$0A explosion): the box depends on the blink phase in +$14 */
HLE_FUNC_PURE(0x006004, HitHkWeak, 300)
{
	lea_(ctx, 3, 0x006074, 8);
	mvw_m(ctx, 0, A6 + 0x14);
	andiw(ctx, 0, 0xF0);
	eoriw(ctx, 0, 0xF0);
	shiftw(ctx, 0, 'a', 1);
	ctx->a[3] += (uint32_t)(int32_t)sx(ctx->d[0]);
	hle_insn(ctx, 12);                                                  /* lea.l (0,a3,d0.w),a3 */
	lea_(ctx, 2, 0x006022, 8);
	hle_insn(ctx, 10);
	HitBox(ctx);
}
