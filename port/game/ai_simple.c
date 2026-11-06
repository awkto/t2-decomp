/* AI states of three simple level-1 object types. docs/objects_types.md, src/t2.s.
 *
 *   type $0F crate:            AI $00C886 (ai_crate), opening states $00C8BA, $00C8F0, $00C91E
 *   type $13 resistance human: AI $00C9A2, flinch $00C968, kill $00C95E
 *   type $14 wall section:     AI $00CA0A (idle), kill/tiles $00CA12
 *   hit tests (the +$1C routines): $00597C, $005F04, $005F56
 *
 * Each AI state is an HLE_RANGE from its entry (reached by the `jmp (a0)` of ObjRunAll/ObjNext)
 * to the `bra.w ObjNext` ($00B3BE, a C range itself), which this code reaches with hle_jump.
 * Shared tails inside the 68k code ($00C8A8 crate draw, $00C99A/$00C9B8/$00C9C2 human tiles)
 * are C helpers that mirror the fall-through / bra exactly.
 *
 * Callees: ObjScrollX, ObjKillIfLeftOf, ObjQueueSprite, Random and HitBox are C (called
 * directly, their jsr charged here, their rts charged by them). The others (VdpQueueTileRect,
 * the debris spawners, SetPlayerState...) stay 68k and run through hle_call68k after hle_sync,
 * so a state that may reach one is a plain HLE_RANGE and the others HLE_RANGE_PURE.
 * Every instruction is on the instruction timeline with the GPGX cost (m68kops.h table). After
 * a nested call every register is re-read from ctx (the callees clobber them). */
#include "hle.h"

#include <stdint.h>

#define OBJ_COUNT   0xFF80DAu
#define SHOT_DAMAGE 0xFFA0CAu
#define TILE_BASE   0xFF9E46u        /* move.w (abs).l,d7 : tile base added to the rect words */
#define EXIT_NEXT   0x00B3BEu        /* ObjNext */
#define TILE_RECT   0x012234u        /* VdpQueueTileRect */
#define DAT_04A7E8  0x04A7E8u

void ObjScrollX(hle_ctx *ctx);
void ObjKillIfLeftOf(hle_ctx *ctx);
void ObjQueueSprite(hle_ctx *ctx);
void Random(hle_ctx *ctx);
void HitBox(hle_ctx *ctx);

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

/* ROM data or (not expected) RAM */
static inline uint16_t rd16(uint32_t a)
{
	return ((a & 0xFFFFFFu) >= 0xFF0000u) ? RAM16(a) : ROM16(a);
}

/* 16-bit shift by an immediate n on the low word of *r; kind 'l' asl, 'r' lsr, 'a' asr.
 * Flags: X=C = last bit out, N/Z from the result, V only for asl (msb changed). */
static void shift16(hle_ctx *c, uint32_t *r, char kind, int n)
{
	uint32_t v = *r & 0xFFFF, res;
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
	set_w(r, res);
}

static inline int cc_z(hle_ctx *c) { return (c->sr & CC_Z) != 0; }
static inline int cc_gt(hle_ctx *c)
{
	int n = (c->sr & CC_N) != 0, v = (c->sr & CC_V) != 0;
	return !cc_z(c) && n == v;
}
static inline int cc_ge(hle_ctx *c)
{
	return ((c->sr & CC_N) != 0) == ((c->sr & CC_V) != 0);
}

/* moveq #imm,Dn */
static void moveq(hle_ctx *c, int reg, int imm)
{
	c->d[reg] = (uint32_t)(int32_t)imm;
	cc_logic32(c, c->d[reg]);
	hle_insn(c, 4);
}

/* subq.w #1,(abs/d16,a6) -> the new value; X N Z V C */
static uint16_t subq_mem(hle_ctx *c, uint32_t addr, int cost)
{
	uint16_t r = (uint16_t)alu(c, OP_SUB, RAM16(addr), 1, 16, 1);
	RAM16(addr) = r;
	hle_insn(c, cost);
	return r;
}

/* bne.s / beq.s: returns taken; charges 10 / 8 */
static int bcc_s(hle_ctx *c, int taken)
{
	hle_insn(c, taken ? 10 : 8);
	return taken;
}

static void to_next(hle_ctx *c)                 /* bra.w ObjNext */
{
	hle_insn(c, 10);
	hle_jump(c, EXIT_NEXT);
}

/* clr.w (a6) ; subq.w #1,(obj_count).l  : free the slot */
static void free_slot(hle_ctx *c)
{
	RAM16(c->a[6]) = 0;
	cc_set(c, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	hle_insn(c, 12);
	RAM16(OBJ_COUNT) = (uint16_t)alu(c, OP_SUB, RAM16(OBJ_COUNT), 1, 16, 1);
	hle_insn(c, 20);
}

/* ------------------------------------------------------------------ crate */
/* sub_00C8A8 (d1 = sprite frame): ObjScrollX, ObjKillIfLeftOf(-12), ObjQueueSprite, bra ObjNext */
static void crate_draw(hle_ctx *c)
{
	hle_insn(c, 18);                                /* jsr (ObjScrollX,pc) */
	ObjScrollX(c);
	moveq(c, 0, -0xC);
	hle_insn(c, 18);                                /* jsr (ObjKillIfLeftOf,pc) */
	ObjKillIfLeftOf(c);
	hle_insn(c, 18);                                /* bsr.w ObjQueueSprite */
	ObjQueueSprite(c);
	to_next(c);
}

/* the common "move.w d0,d1 ; bra.w sub_00C8A8" after d0 = $16 + (counter >> 2) */
static void crate_frame_from_counter(hle_ctx *c, int xor3)
{
	uint32_t a6 = c->a[6];
	uint16_t w = RAM16(a6 + 0x14);
	set_w(&c->d[0], w);
	cc_logic16(c, w);
	hle_insn(c, 12);                                /* move.w (0x14,a6),d0 */
	shift16(c, &c->d[0], 'a', 2);
	hle_insn(c, 10);                                /* asr.w #2,d0 */
	if (xor3) {
		set_w(&c->d[0], c->d[0] ^ 3);
		cc_logic16(c, c->d[0]);
		hle_insn(c, 8);                         /* eori.w #3,d0 */
	}
	set_w(&c->d[0], alu(c, OP_ADD, c->d[0], 0x16, 16, 1));
	hle_insn(c, 8);                                 /* addi.w #$16,d0 */
	set_w(&c->d[1], c->d[0]);
	cc_logic16(c, c->d[0]);
	hle_insn(c, 4);                                 /* move.w d0,d1 */
	hle_insn(c, 10);                                /* bra.w sub_00C8A8 */
	crate_draw(c);
}

/* $00C886 ai_crate */
HLE_RANGE_PURE(0x00C886, ai_crate, 700, EXIT_NEXT)
{
	uint32_t a6 = ctx->a[6];
	uint16_t r = subq_mem(ctx, a6 + 0x14, 16);      /* subq.w #1,(0x14,a6) */
	if (!bcc_s(ctx, r != 0)) {                      /* bne.s loc_00C894 */
		free_slot(ctx);
	}
	moveq(ctx, 1, 0x16);                            /* loc_00C894: moveq #$16,d1 */
	alu(ctx, OP_SUB, RAM16(a6 + 0x14), 0x64, 16, 0);
	hle_insn(ctx, 16);                              /* cmpi.w #$64,(0x14,a6) */
	if (bcc_s(ctx, cc_gt(ctx))) {                   /* bgt.s sub_00C8A8 */
		crate_draw(ctx);
		return;
	}
	cc_set(ctx, CC_Z, (RAM8(a6 + 0x15) & 8) ? 0 : CC_Z);
	hle_insn(ctx, 16);                              /* btst.b #3,(0x15,a6) */
	if (bcc_s(ctx, !cc_z(ctx))) {                   /* bne.s sub_00C8A8 */
		crate_draw(ctx);
		return;
	}
	moveq(ctx, 1, 0x1A);
	crate_draw(ctx);
}

/* $00C8BA: opening, counter runs down; at 0 spawn debris and go to $00C8F0 */
HLE_RANGE(0x00C8BA, crate_open1, 6000, EXIT_NEXT)
{
	uint32_t a6 = ctx->a[6];
	uint16_t r = subq_mem(ctx, a6 + 0x14, 16);      /* subq.w #1,(0x14,a6) */
	if (!bcc_s(ctx, r != 0)) {                      /* bne.s loc_00C8DC */
		ctx->d[0] = (uint32_t)(int32_t)(int16_t)RAM16(a6 + 2);   /* movem.w (0x2,a6),d0-d1 */
		ctx->d[1] = (uint32_t)(int32_t)(int16_t)RAM16(a6 + 4);
		hle_insn(ctx, 24);
		set_w(&ctx->d[2], RAM16(a6 + 0x2A));
		cc_logic16(ctx, ctx->d[2]);
		hle_insn(ctx, 12);                      /* move.w (0x2a,a6),d2 */
		hle_insn(ctx, 18);                      /* jsr (sub_012A16,pc) */
		hle_sync(ctx);
		hle_call68k(ctx, 0x012A16);
		a6 = ctx->a[6];
		RAM32_W(a6 + 0x20, 0xC8F0);
		cc_logic32(ctx, 0xC8F0);
		hle_insn(ctx, 24);                      /* move.l #$c8f0,(0x20,a6) */
		RAM16(a6 + 0x14) = 0x1E;
		cc_logic16(ctx, 0x1E);
		hle_insn(ctx, 16);                      /* move.w #$1e,(0x14,a6) */
	}
	crate_frame_from_counter(ctx, 1);               /* loc_00C8DC */
}

/* $00C8F0 */
HLE_RANGE_PURE(0x00C8F0, crate_open2, 700, EXIT_NEXT)
{
	uint32_t a6 = ctx->a[6];
	uint16_t r = subq_mem(ctx, a6 + 0x14, 16);      /* subq.w #1,(0x14,a6) */
	if (!bcc_s(ctx, r != 0)) {                      /* bne.s loc_00C904 */
		uint16_t r2 = subq_mem(ctx, a6 + 0x24, 16);   /* subq.w #1,(0x24,a6) */
		if (bcc_s(ctx, r2 != 0)) {              /* bne.s loc_00C90A */
			RAM32_W(a6 + 0x20, 0xC91E);
			cc_logic32(ctx, 0xC91E);
			hle_insn(ctx, 24);              /* move.l #$c91e,(0x20,a6) */
			RAM16(a6 + 0x14) = 0x10;
			cc_logic16(ctx, 0x10);
			hle_insn(ctx, 16);              /* move.w #$10,(0x14,a6) */
			moveq(ctx, 1, 0x19);
			hle_insn(ctx, 10);              /* bra.w sub_00C8A8 */
			crate_draw(ctx);
			return;
		}
		free_slot(ctx);
	}
	moveq(ctx, 1, 0x19);                            /* loc_00C904 */
	hle_insn(ctx, 10);                              /* bra.w sub_00C8A8 */
	crate_draw(ctx);
}

/* $00C91E: countdown, then back to $00C886 with a pickup count of 1..2 */
HLE_RANGE_PURE(0x00C91E, crate_open3, 800, EXIT_NEXT)
{
	uint32_t a6 = ctx->a[6];
	uint16_t r = subq_mem(ctx, a6 + 0x14, 16);      /* subq.w #1,(0x14,a6) */
	if (bcc_s(ctx, r != 0)) {                       /* bne.s loc_00C94E */
		crate_frame_from_counter(ctx, 0);
		return;
	}
	hle_insn(ctx, 18);                              /* jsr (Random,pc) */
	Random(ctx);
	set_w(&ctx->d[0], ctx->d[0] & 1);
	cc_logic16(ctx, ctx->d[0]);
	hle_insn(ctx, 8);                               /* andi.w #1,d0 */
	set_w(&ctx->d[0], alu(ctx, OP_ADD, ctx->d[0], 1, 16, 1));
	hle_insn(ctx, 4);                               /* addq.w #1,d0 */
	RAM16(a6 + 0x2A) = (uint16_t)ctx->d[0];
	cc_logic16(ctx, ctx->d[0]);
	hle_insn(ctx, 12);                              /* move.w d0,(0x2a,a6) */
	RAM32_W(a6 + 0x20, 0xC886);
	cc_logic32(ctx, 0xC886);
	hle_insn(ctx, 24);                              /* move.l #$c886,(0x20,a6) */
	uint16_t w = RAM16(a6);
	set_w(&ctx->d[0], w);
	cc_logic16(ctx, w);
	hle_insn(ctx, 8);                               /* move.w (a6),d0 */
	cc_set(ctx, CC_Z, (ctx->d[0] & 0x8000) ? 0 : CC_Z);
	set_w(&ctx->d[0], ctx->d[0] | 0x8000);
	hle_insn(ctx, 10);                              /* bset #15,d0 */
	RAM16(a6) = (uint16_t)ctx->d[0];
	cc_logic16(ctx, ctx->d[0]);
	hle_insn(ctx, 8);                               /* move.w d0,(a6) */
	RAM16(a6 + 0x14) = 0;
	cc_logic16(ctx, 0);
	hle_insn(ctx, 16);                              /* move.w #0,(0x14,a6) */
	moveq(ctx, 1, 0x16);
	hle_insn(ctx, 10);                              /* bra.w sub_00C8A8 */
	crate_draw(ctx);
}

/* ------------------------------------------------------------------ human */
/* sub_00C9C2: queue the 6x7 tile rect, bra ObjNext. d1 = offset into dat_04A7E8 */
static void human_tiles(hle_ctx *c)
{
	push16(c, (uint16_t)c->d[7]);
	hle_insn(c, 8);                                 /* move.w d7,-(sp) */
	c->a[0] = DAT_04A7E8;
	hle_insn(c, 12);                                /* lea.l (dat_04A7E8).l,a0 */
	c->a[0] += (uint32_t)(int32_t)(int16_t)c->d[1];
	hle_insn(c, 12);                                /* lea.l (0,a0,d1.w),a0 */
	set_w(&c->d[1], RAM16(c->a[6] + 0xA));
	cc_logic16(c, c->d[1]);
	hle_insn(c, 12);                                /* move.w (0xa,a6),d1 */
	moveq(c, 2, 6);
	moveq(c, 3, 7);
	set_w(&c->d[6], 0xA000);
	cc_logic16(c, 0xA000);
	hle_insn(c, 8);                                 /* move.w #$a000,d6 */
	set_w(&c->d[7], RAM16(TILE_BASE));
	cc_logic16(c, c->d[7]);
	hle_insn(c, 16);                                /* move.w (abs).l,d7 */
	hle_insn(c, 18);                                /* jsr (VdpQueueTileRect,pc) */
	hle_sync(c);
	hle_call68k(c, TILE_RECT);
	set_w(&c->d[7], pop16(c));
	cc_logic16(c, c->d[7]);
	hle_insn(c, 8);                                 /* move.w (sp)+,d7 */
	to_next(c);                                     /* sub_00C9E6 */
}

/* sub_00C9B8: d1 = counter; table word at $00C9EA + ((counter & $f0) >> 3) */
static void human_table(hle_ctx *c)
{
	set_w(&c->d[1], c->d[1] & 0xF0);
	cc_logic16(c, c->d[1]);
	hle_insn(c, 8);                                 /* andi.w #$f0,d1 */
	shift16(c, &c->d[1], 'r', 3);
	hle_insn(c, 12);                                /* lsr.w #3,d1 */
	set_w(&c->d[1], ROM16(0x00C9EA + (uint32_t)(int32_t)(int16_t)c->d[1]));
	cc_logic16(c, c->d[1]);
	hle_insn(c, 14);                                /* move.w (sub_00C9EA,pc,d1.w),d1 */
	human_tiles(c);
}

/* sub_00C99A: d1 = $2e0 ; bra.w sub_00C9C2 */
static void human_c99a(hle_ctx *c)
{
	set_w(&c->d[1], 0x2E0);
	cc_logic16(c, 0x2E0);
	hle_insn(c, 8);                                 /* move.w #$2e0,d1 */
	hle_insn(c, 10);                                /* bra.w sub_00C9C2 */
	human_tiles(c);
}

/* the d0 = --(0x24,a6) steps shared by $00C9A2 and $00C968; leaves d0/d1 */
static void human_dec24(hle_ctx *c)
{
	uint32_t a6 = c->a[6];
	set_w(&c->d[0], RAM16(a6 + 0x24));
	cc_logic16(c, c->d[0]);
	hle_insn(c, 12);                                /* move.w (0x24,a6),d0 */
	set_w(&c->d[0], alu(c, OP_SUB, c->d[0], 1, 16, 1));
	hle_insn(c, 4);                                 /* subq.w #1,d0 */
	RAM16(a6 + 0x24) = (uint16_t)c->d[0];
	cc_logic16(c, c->d[0]);
	hle_insn(c, 12);                                /* move.w d0,(0x24,a6) */
}

/* $00C9A2 ai_human_soldier */
HLE_RANGE(0x00C9A2, ai_human_soldier, 3000, EXIT_NEXT)
{
	hle_insn(ctx, 18);                              /* jsr (ObjScrollX,pc) */
	ObjScrollX(ctx);
	human_dec24(ctx);
	set_w(&ctx->d[1], ctx->d[0]);
	cc_logic16(ctx, ctx->d[0]);
	hle_insn(ctx, 4);                               /* move.w d0,d1 */
	set_w(&ctx->d[0], ctx->d[0] & 0xF);
	cc_logic16(ctx, ctx->d[0]);
	hle_insn(ctx, 8);                               /* andi.w #$f,d0 */
	if (bcc_s(ctx, !cc_z(ctx))) {                   /* bne.s sub_00C9E6 */
		to_next(ctx);
		return;
	}
	human_table(ctx);
}

/* $00C968 flinch after a hit (countdown in +$14), then back to $00C9A2 */
HLE_RANGE(0x00C968, human_flinch, 3000, EXIT_NEXT)
{
	hle_insn(ctx, 18);                              /* jsr (ObjScrollX,pc) */
	ObjScrollX(ctx);
	uint32_t a6 = ctx->a[6];
	uint16_t r = subq_mem(ctx, a6 + 0x14, 16);      /* subq.w #1,(0x14,a6) */
	if (!bcc_s(ctx, r != 0)) {                      /* bne.s loc_00C97E */
		RAM32_W(a6 + 0x20, 0xC9A2);
		cc_logic32(ctx, 0xC9A2);
		hle_insn(ctx, 24);                      /* move.l #$c9a2,(0x20,a6) */
		cc_set(ctx, CC_Z, (RAM8(a6) & 0x80) ? 0 : CC_Z);
		RAM8(a6) |= 0x80;
		hle_insn(ctx, 16);                      /* bset.b #7,(a6) */
	}
	human_dec24(ctx);                               /* loc_00C97E */
	set_w(&ctx->d[2], ctx->d[0]);
	cc_logic16(ctx, ctx->d[0]);
	hle_insn(ctx, 4);                               /* move.w d0,d2 */
	set_w(&ctx->d[1], ctx->d[0]);
	cc_logic16(ctx, ctx->d[0]);
	hle_insn(ctx, 4);                               /* move.w d0,d1 */
	set_w(&ctx->d[2], ctx->d[2] & 3);
	cc_logic16(ctx, ctx->d[2]);
	hle_insn(ctx, 8);                               /* andi.w #3,d2 */
	if (bcc_s(ctx, !cc_z(ctx))) {                   /* bne.s sub_00C9E6 */
		to_next(ctx);
		return;
	}
	cc_set(ctx, CC_Z, (ctx->d[0] & 4) ? 0 : CC_Z);
	hle_insn(ctx, 10);                              /* btst.b #2,d0 */
	if (!cc_z(ctx)) {                               /* bne.w sub_00C9B8 */
		hle_insn(ctx, 10);
		human_table(ctx);
		return;
	}
	hle_insn(ctx, 12);
	human_c99a(ctx);                                /* sub_00C99A */
}

/* $00C95E kill: free the slot, draw the last frame ($00C99A) */
HLE_RANGE(0x00C95E, human_kill, 3000, EXIT_NEXT)
{
	free_slot(ctx);
	hle_insn(ctx, 10);                              /* bra.s sub_00C99A */
	human_c99a(ctx);
}

/* ------------------------------------------------------------------ wall section */
/* $00CA0A idle: just scroll */
HLE_RANGE_PURE(0x00CA0A, ai_wall_section, 300, EXIT_NEXT)
{
	hle_insn(ctx, 18);                              /* jsr (ObjScrollX,pc) */
	ObjScrollX(ctx);
	to_next(ctx);
}

/* d0 = (2,a6), d1 = (4,a6) - $10, d0 += (a3) ; the common prefix of the debris spawns */
static void wall_xy(hle_ctx *c, int d1_sub)
{
	uint32_t a6 = c->a[6];
	set_w(&c->d[0], RAM16(a6 + 2));
	cc_logic16(c, c->d[0]);
	hle_insn(c, 12);                                /* move.w (0x2,a6),d0 */
	set_w(&c->d[1], RAM16(a6 + 4));
	cc_logic16(c, c->d[1]);
	hle_insn(c, 12);                                /* move.w (0x4,a6),d1 */
	set_w(&c->d[1], alu(c, OP_SUB, c->d[1], d1_sub, 16, 1));
	hle_insn(c, 8);                                 /* subi.w #n,d1 */
}

static void wall_addx(hle_ctx *c)
{
	set_w(&c->d[0], alu(c, OP_ADD, c->d[0], rd16(c->a[3]), 16, 1));
	hle_insn(c, 8);                                 /* add.w (a3),d0 */
}

static void wall_call(hle_ctx *c, uint32_t addr)
{
	hle_insn(c, 18);                                /* jsr (sub_xxxxxx,pc) */
	hle_sync(c);
	hle_call68k(c, addr);
}

/* dbra d5 at the loop end; returns 1 when it loops */
static int dbra_d5(hle_ctx *c)
{
	uint16_t d = (uint16_t)(c->d[5] - 1);
	set_w(&c->d[5], d);
	if (d == 0xFFFF) {
		hle_insn(c, 14);
		return 0;
	}
	hle_insn(c, 10);
	return 1;
}

/* $00CA12: shot away - debris at the wall, then replace the wall tiles and free the slot */
HLE_RANGE(0x00CA12, wall_kill, 24000, EXIT_NEXT)   /* T2_HLE_BOUNDS+NODECLINE max 15923 (misc_c) x 1.5 */
{
	uint32_t a6 = ctx->a[6];
	set_w(&ctx->d[3], RAM16(a6 + 0x2A));
	cc_logic16(ctx, ctx->d[3]);
	hle_insn(ctx, 12);                              /* move.w (0x2a,a6),d3 */
	shift16(ctx, &ctx->d[3], 'l', 3);
	hle_insn(ctx, 12);                              /* asl.w #3,d3 */
	ctx->a[3] = 0x00CA9A;
	hle_insn(ctx, 8);                               /* lea.l (dat_00CA9A,pc),a3 */
	ctx->a[3] += (uint32_t)(int32_t)(int16_t)ctx->d[3];
	hle_insn(ctx, 12);                              /* lea.l (0,a3,d3.w),a3 */
	moveq(ctx, 5, 5);
	do {                                            /* loc_00CA22 */
		wall_xy(ctx, 0x10);
		wall_addx(ctx);
		wall_call(ctx, 0x012772);
	} while (dbra_d5(ctx));
	moveq(ctx, 5, 2);
	do {                                            /* loc_00CA3A */
		wall_xy(ctx, 0x10);
		wall_addx(ctx);
		wall_call(ctx, 0x01276C);
	} while (dbra_d5(ctx));
	a6 = ctx->a[6];
	set_w(&ctx->d[0], RAM16(a6 + 2));
	cc_logic16(ctx, ctx->d[0]);
	hle_insn(ctx, 12);                              /* move.w (0x2,a6),d0 */
	set_w(&ctx->d[1], RAM16(a6 + 4));
	cc_logic16(ctx, ctx->d[1]);
	hle_insn(ctx, 12);                              /* move.w (0x4,a6),d1 */
	set_w(&ctx->d[2], RAM16(a6 + 6));
	cc_logic16(ctx, ctx->d[2]);
	hle_insn(ctx, 12);                              /* move.w (0x6,a6),d2 */
	set_b(&ctx->d[2], alu(ctx, OP_SUB, ctx->d[2], 1, 8, 1));
	hle_insn(ctx, 4);                               /* subq.b #1,d2 */
	set_w(&ctx->d[1], alu(ctx, OP_SUB, ctx->d[1], 8, 16, 1));
	hle_insn(ctx, 8);                               /* subi.w #8,d1 */
	wall_addx(ctx);
	wall_call(ctx, 0x01254E);
	ctx->a[3] += 2;
	hle_insn(ctx, 8);                               /* addq.l #2,a3 */
	push16(ctx, (uint16_t)ctx->d[7]);
	hle_insn(ctx, 8);                               /* move.w d7,-(sp) */
	set_w(&ctx->d[1], rd16(ctx->a[3]));
	ctx->a[3] += 2;
	cc_logic16(ctx, ctx->d[1]);
	hle_insn(ctx, 8);                               /* move.w (a3)+,d1 */
	set_w(&ctx->d[2], rd16(ctx->a[3]));
	ctx->a[3] += 2;
	cc_logic16(ctx, ctx->d[2]);
	hle_insn(ctx, 8);                               /* move.w (a3)+,d2 */
	ctx->a[0] = DAT_04A7E8;
	hle_insn(ctx, 12);                              /* lea.l (dat_04A7E8).l,a0 */
	ctx->a[0] += (uint32_t)(int32_t)(int16_t)ctx->d[2];
	hle_insn(ctx, 12);                              /* lea.l (0,a0,d2.w),a0 */
	set_w(&ctx->d[2], rd16(ctx->a[3]));
	ctx->a[3] += 2;
	cc_logic16(ctx, ctx->d[2]);
	hle_insn(ctx, 8);                               /* move.w (a3)+,d2 */
	moveq(ctx, 3, 8);
	set_w(&ctx->d[6], 0xA000);
	cc_logic16(ctx, 0xA000);
	hle_insn(ctx, 8);                               /* move.w #$a000,d6 */
	set_w(&ctx->d[7], RAM16(TILE_BASE));
	cc_logic16(ctx, ctx->d[7]);
	hle_insn(ctx, 16);                              /* move.w (abs).l,d7 */
	wall_call(ctx, TILE_RECT);
	set_w(&ctx->d[7], pop16(ctx));
	cc_logic16(ctx, ctx->d[7]);
	hle_insn(ctx, 8);                               /* move.w (sp)+,d7 */
	free_slot(ctx);
	to_next(ctx);
}

/* ------------------------------------------------------------------ hit tests */
/* $00597C hit_destructible_prop (type $0F) */
HLE_FUNC_PURE(0x00597C, hit_destructible_prop, 250)
{
	ctx->a[2] = 0x005988;
	hle_insn(ctx, 8);                               /* lea.l (sub_005988,pc),a2 */
	ctx->a[3] = 0x0059A4;
	hle_insn(ctx, 8);                               /* lea.l (dat_0059A4,pc),a3 */
	hle_insn(ctx, 10);                              /* jmp (HitBox,pc) */
	HitBox(ctx);
}

/* $005F04 HitHumanSoldier (type $13): damage >= 6 is not a hit */
HLE_FUNC_PURE(0x005F04, HitHumanSoldier, 250)
{
	ctx->a[3] = 0x005F4E;
	hle_insn(ctx, 8);                               /* lea.l (dat_005F4E,pc),a3 */
	ctx->a[2] = 0x005F1A;
	hle_insn(ctx, 8);                               /* lea.l (sub_005F1A,pc),a2 */
	alu(ctx, OP_SUB, RAM16(SHOT_DAMAGE), 6, 16, 0);
	hle_insn(ctx, 20);                              /* cmpi.w #6,(shot_damage).l */
	if (bcc_s(ctx, cc_ge(ctx))) {                   /* bge.s sub_005F32 (rts) */
		hle_insn(ctx, 16);
		return;
	}
	hle_insn(ctx, 10);                              /* jmp (HitBox,pc) */
	HitBox(ctx);
}

/* $005F56 hit_wall_section (type $14): two boxes per section index (+$2A), a3/a1 */
HLE_FUNC_PURE(0x005F56, hit_wall_section, 450)
{
	set_w(&ctx->d[0], RAM16(ctx->a[6] + 0x2A));
	cc_logic16(ctx, ctx->d[0]);
	hle_insn(ctx, 12);                              /* move.w (0x2a,a6),d0 */
	alu(ctx, OP_SUB, ctx->d[0], 3, 16, 0);
	hle_insn(ctx, 8);                               /* cmpi.w #3,d0 */
	if (cc_gt(ctx)) {                               /* bgt.w loc_005FCE */
		hle_insn(ctx, 10);
		set_w(&ctx->d[0], alu(ctx, OP_SUB, ctx->d[0], 4, 16, 1));
		hle_insn(ctx, 4);                       /* subq.w #4,d0 */
		shift16(ctx, &ctx->d[0], 'l', 3);
		hle_insn(ctx, 12);                      /* asl.w #3,d0 */
		ctx->a[3] = 0x005FE4 + (uint32_t)(int32_t)(int16_t)ctx->d[0];
		hle_insn(ctx, 12);                      /* lea.l (dat_005FE4,pc,d0.w),a3 */
		ctx->a[2] = 0x005FDE;
		hle_insn(ctx, 8);                       /* lea.l (sub_005FDE,pc),a2 */
		hle_insn(ctx, 10);                      /* jmp (HitBox,pc) */
		HitBox(ctx);
		return;
	}
	hle_insn(ctx, 12);
	shift16(ctx, &ctx->d[0], 'l', 4);
	hle_insn(ctx, 14);                              /* asl.w #4,d0 */
	ctx->a[3] = 0x005F8E + (uint32_t)(int32_t)(int16_t)ctx->d[0];
	hle_insn(ctx, 12);                              /* lea.l (dat_005F8E,pc,d0.w),a3 */
	ctx->a[1] = ctx->a[3] + 8;
	hle_insn(ctx, 8);                               /* lea.l (0x8,a3),a1 */
	ctx->a[2] = 0x005F7A;
	hle_insn(ctx, 8);                               /* lea.l (sub_005F7A,pc),a2 */
	hle_insn(ctx, 18);                              /* jsr (HitBox,pc) */
	HitBox(ctx);
	ctx->a[3] = ctx->a[1];
	hle_insn(ctx, 4);                               /* movea.l a1,a3 */
	hle_insn(ctx, 10);                              /* jmp (HitBox,pc) */
	HitBox(ctx);
}
