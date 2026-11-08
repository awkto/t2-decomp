/* The object pools other than the object table (docs/subsystems/objects.md, "Pools"): the per-frame runners
 * MissilesRun $0123F0 (player missiles and their explosions, state word 1..7, bra table $01240C),
 * EffectsRun $01243C (fixed-spot animations riding a scroll row), SparksRun $0126B4 (8.8 velocity
 * animations) and DebrisRun $012886 (velocity and gravity), and the leaf spawners the AI states call:
 * the spark spawners ($01248C..$012568 around the core $01257A), the debris spawners ($01272A..$01277C),
 * the bomb object $01237A and the pool allocators.
 *
 * The bodies were produced instruction by instruction from the disassembly (a comment on every line
 * names the 68k instruction and its address) with the GPGX cost of each opcode word on the instruction
 * timeline. EffectsRun/SparksRun/DebrisRun and the spawners only touch work RAM (the C ObjScrollX,
 * QueueSprite, ObjQueueSprite, Random, ObjAlloc are called after their jsr is charged), so they are
 * HLE_FUNC_PURE; the spawners that end in `jmp sub_013CC4` (a sound call) are ranges to that jump, the
 * sound code stays 68k. MissilesRun is a plain HLE_FUNC: the explosion and homing states call
 * ShotHitTest ($005892), which runs through hle_call68k (its own C version when enabled), so the frame
 * is judged by its declared bound (measured with T2_HLE_BOUNDS).
 */
#include "hle.h"

#include <stdint.h>

void ObjQueueSprite(hle_ctx *ctx);
void QueueSprite(hle_ctx *ctx);
void Random(hle_ctx *ctx);
void HitBox(hle_ctx *ctx);
void ObjMoveX(hle_ctx *ctx);
void ObjScrollX(hle_ctx *ctx);
void ObjAlloc(hle_ctx *ctx);
void ObjKillIfLeftOf(hle_ctx *ctx);
void ObjKillIfRightOf(hle_ctx *ctx);


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
static int c_c(hle_ctx *c) { return (c->sr & CC_C) != 0; }
static int c_v(hle_ctx *c) { return (c->sr & CC_V) != 0; }
static int c_ge(hle_ctx *c) { return c_n(c) == c_v(c); }
static int c_le(hle_ctx *c) { return c_z(c) || c_n(c) != c_v(c); }
static int c_hi(hle_ctx *c) { return !c_c(c) && !c_z(c); }
static int c_gt(hle_ctx *c) { return !c_z(c) && c_n(c) == c_v(c); }

/* conditional branches: charge, return whether taken */
static int bs(hle_ctx *c, int taken) { hle_insn(c, taken ? 10 : 8); return taken; }    /* Bcc.s */
static int bw(hle_ctx *c, int taken) { hle_insn(c, taken ? 10 : 12); return taken; }   /* Bcc.w */

/* work RAM or ROM by address (the code reads both: slot fields and the ROM tables) */
static inline int is_ram(uint32_t a) { return (a & 0xFFFFFFu) >= 0xE00000u; }
static inline uint8_t ld8(uint32_t a) { return is_ram(a) ? RAM8(a) : ROM8(a); }
static inline uint16_t ld16(uint32_t a) { return is_ram(a) ? RAM16(a) : ROM16(a); }
static inline uint32_t ld32(uint32_t a) { return is_ram(a) ? RAM32(a) : ROM32(a); }
static inline void st8(uint32_t a, uint32_t v) { RAM8(a) = (uint8_t)v; }
static inline void st16(uint32_t a, uint32_t v) { RAM16(a) = (uint16_t)v; }
static inline void st32(uint32_t a, uint32_t v) { RAM32_W(a, v); }

/* asl/asr/lsr.w #n,Dr (1..8): kind 'l' asl, 'a' asr, 'r' lsr; the caller charges 6 + 2n */
static void sh_w(hle_ctx *c, int r, char kind, int n)
{
	uint32_t v = c->d[r] & 0xFFFF, res;
	unsigned f = 0;
	if (kind == 'l') {
		res = (v << n) & 0xFFFF;
		if ((v >> (16 - n)) & 1) f |= CC_C | CC_X;
		uint32_t top = (v >> (15 - n)) & ((1u << (n + 1)) - 1);
		if (top != 0 && top != ((1u << (n + 1)) - 1)) f |= CC_V;
	} else if (kind == 'o') {
		res = ((v << n) | (v >> (16 - n))) & 0xFFFF;
		if (res & 1) f |= CC_C;
		f |= (c->sr & CC_X);
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
}

static void sub_00C48E(hle_ctx *ctx);
static void sub_01293E(hle_ctx *ctx);
static void sub_012926(hle_ctx *ctx);
static void sub_012956(hle_ctx *ctx);
static void sub_01257A(hle_ctx *ctx);
static void sub_012502(hle_ctx *ctx);
static void sub_01237A(hle_ctx *ctx);
static void sub_01277C(hle_ctx *ctx);
static void sub_013184(hle_ctx *ctx);
static void sub_01317E(hle_ctx *ctx);
static void sub_013178(hle_ctx *ctx);
static void sub_012FC2(hle_ctx *ctx);
static void sub_013110(hle_ctx *ctx);
static void sub_0130A2(hle_ctx *ctx);

/* sub_00C48E (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_00C48E(hle_ctx *ctx)
{
	{ uint32_t _v = ld8((ctx->a[6] + (uint32_t)(int32_t)14)); set_b(&ctx->d[0], _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 00C48E moveb %fp@(14),%d0 */
	{ set_w(&ctx->d[0], (uint32_t)(int32_t)(int8_t)ctx->d[0]); cc_logic16(ctx, ctx->d[0]); hle_insn(ctx, 4); } /* 00C492 extw %d0 */
	{ uint32_t _v = ld8((ctx->a[6] + (uint32_t)(int32_t)10)); set_b(&ctx->d[1], _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 00C494 moveb %fp@(10),%d1 */
	{ uint32_t _v = ld8((ctx->a[6] + (uint32_t)(int32_t)15)); set_b(&ctx->d[2], _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 00C498 moveb %fp@(15),%d2 */
	{ set_w(&ctx->d[2], (uint32_t)(int32_t)(int8_t)ctx->d[2]); cc_logic16(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 00C49C extw %d2 */
	{ uint32_t _v = ld8((ctx->a[6] + (uint32_t)(int32_t)12)); set_b(&ctx->d[3], _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 00C49E moveb %fp@(12),%d3 */
	{ uint32_t _v = ld32((ctx->a[6] + (uint32_t)(int32_t)22)); ctx->a[0] = _v; hle_insn(ctx, 16); } /* 00C4A2 moveal %fp@(22),%a0 */
	{ uint32_t _p = ld16(ctx->a[0]); ctx->a[0] += 2; uint32_t _v = _p; set_w(&ctx->d[4], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00C4A6 movew %a0@+,%d4 */
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[4] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[4], _r); hle_insn(ctx, 4); } /* 00C4A8 subqw #1,%d4 */
	if (bw(ctx, c_n(ctx))) goto L_00C4E0; /* 00C4AA bmiw 0xc4e0 */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)26); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00C4AE subqw #1,%fp@(26) */
	if (bs(ctx, !c_z(ctx))) goto L_00C4BC; /* 00C4B2 bnes 0xc4bc */
	{ uint32_t _p = ld16(ctx->a[0]); ctx->a[0] += 2; uint32_t _v = _p; st16((ctx->a[6] + (uint32_t)(int32_t)26), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00C4B4 movew %a0@+,%fp@(26) */
	{ uint32_t _v = ctx->a[0]; st32((ctx->a[6] + (uint32_t)(int32_t)22), _v); cc_logic32(ctx, _v); hle_insn(ctx, 16); } /* 00C4B8 movel %a0,%fp@(22) */
L_00C4BC:;
	{ uint32_t _s = (ctx->d[1] & 0xFFu); uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)11); uint32_t _d = ld8(_da); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 8, 1); st8(_da, _r); hle_insn(ctx, 16); } /* 00C4BC addb %d1,%fp@(11) */
	if (bs(ctx, !c_c(ctx))) goto L_00C4C6; /* 00C4C0 bccs 0xc4c6 */
	{ uint32_t _s = (ctx->d[0] & 0xFFFFu); uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)2); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00C4C2 addw %d0,%fp@(2) */
L_00C4C6:;
	{ uint32_t _s = (ctx->d[3] & 0xFFu); uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)13); uint32_t _d = ld8(_da); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 8, 1); st8(_da, _r); hle_insn(ctx, 16); } /* 00C4C6 addb %d3,%fp@(13) */
	if (bs(ctx, !c_c(ctx))) goto L_00C4D0; /* 00C4CA bccs 0xc4d0 */
	{ uint32_t _s = (ctx->d[2] & 0xFFFFu); uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)4); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00C4CC addw %d2,%fp@(4) */
L_00C4D0:;
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)20); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00C4D0 subqw #1,%fp@(20) */
	if (bs(ctx, c_z(ctx))) goto L_00C4DC; /* 00C4D4 beqs 0xc4dc */
	{ uint16_t _c = (uint16_t)ctx->d[4] - 1; set_w(&ctx->d[4], _c); if (_c != 0xFFFF) { hle_insn(ctx, 10); goto L_00C4BC; } hle_insn(ctx, 14); } /* 00C4D6 dbf %d4,0xc4bc */
	hle_insn(ctx, 16); return; /* 00C4DA rts */
L_00C4DC:;
	{ ctx->d[4] = 0x0u; cc_logic32(ctx, ctx->d[4]); hle_insn(ctx, 4); } /* 00C4DC moveq #0,%d4 */
	hle_insn(ctx, 16); return; /* 00C4DE rts */
L_00C4E0:;
	{ ctx->d[4] = 0x1u; cc_logic32(ctx, ctx->d[4]); hle_insn(ctx, 4); } /* 00C4E0 moveq #1,%d4 */
	hle_insn(ctx, 16); return; /* 00C4E2 rts */
}

/* sub_01293E (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_01293E(hle_ctx *ctx)
{
	{ ctx->a[5] = 0xFFFF8632u; hle_insn(ctx, 12); } /* 01293E lea 0xffff8632,%a5 */
	{ uint32_t _v = 0x1Fu; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 012944 movew #31,%d0 */
L_012948:;
	{ cc_logic16(ctx, ld16(ctx->a[5])); hle_insn(ctx, 8); } /* 012948 tstw %a5@ */
	if (bs(ctx, c_z(ctx))) goto L_012954; /* 01294A beqs 0x12954 */
	{ ctx->a[5] = (ctx->a[5] + (uint32_t)(int32_t)12); hle_insn(ctx, 8); } /* 01294C lea %a5@(12),%a5 */
	{ uint16_t _c = (uint16_t)ctx->d[0] - 1; set_w(&ctx->d[0], _c); if (_c != 0xFFFF) { hle_insn(ctx, 10); goto L_012948; } hle_insn(ctx, 14); } /* 012950 dbf %d0,0x12948 */
L_012954:;
	hle_insn(ctx, 16); return; /* 012954 rts */
}

/* sub_012926 (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_012926(hle_ctx *ctx)
{
	{ ctx->a[5] = 0xFFFF9572u; hle_insn(ctx, 12); } /* 012926 lea 0xffff9572,%a5 */
	{ uint32_t _v = 0x1Fu; set_w(&ctx->d[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 01292C movew #31,%d6 */
L_012930:;
	{ cc_logic16(ctx, ld16(ctx->a[5])); hle_insn(ctx, 8); } /* 012930 tstw %a5@ */
	if (bs(ctx, c_z(ctx))) goto L_01293C; /* 012932 beqs 0x1293c */
	{ ctx->a[5] = (ctx->a[5] + (uint32_t)(int32_t)18); hle_insn(ctx, 8); } /* 012934 lea %a5@(18),%a5 */
	{ uint16_t _c = (uint16_t)ctx->d[6] - 1; set_w(&ctx->d[6], _c); if (_c != 0xFFFF) { hle_insn(ctx, 10); goto L_012930; } hle_insn(ctx, 14); } /* 012938 dbf %d6,0x12930 */
L_01293C:;
	hle_insn(ctx, 16); return; /* 01293C rts */
}

/* sub_012956 (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_012956(hle_ctx *ctx)
{
	{ ctx->a[5] = 0xFFFF97B2u; hle_insn(ctx, 12); } /* 012956 lea 0xffff97b2,%a5 */
	{ uint32_t _v = 0x1Fu; set_w(&ctx->d[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 01295C movew #31,%d6 */
L_012960:;
	{ cc_logic16(ctx, ld16(ctx->a[5])); hle_insn(ctx, 8); } /* 012960 tstw %a5@ */
	if (bs(ctx, c_z(ctx))) goto L_01296C; /* 012962 beqs 0x1296c */
	{ ctx->a[5] = (ctx->a[5] + (uint32_t)(int32_t)24); hle_insn(ctx, 8); } /* 012964 lea %a5@(24),%a5 */
	{ uint16_t _c = (uint16_t)ctx->d[6] - 1; set_w(&ctx->d[6], _c); if (_c != 0xFFFF) { hle_insn(ctx, 10); goto L_012960; } hle_insn(ctx, 14); } /* 012968 dbf %d6,0x12960 */
L_01296C:;
	hle_insn(ctx, 16); return; /* 01296C rts */
}

/* sub_01257A (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_01257A(hle_ctx *ctx)
{
	{ hle_insn(ctx, 18); sub_012926(ctx); } /* 01257A jsr %pc@(0x12926) */
	{ cc_logic16(ctx, (ctx->d[6] & 0xFFFFu)); hle_insn(ctx, 4); } /* 01257E tstw %d6 */
	if (bs(ctx, c_n(ctx))) goto L_0125B4; /* 012580 bmis 0x125b4 */
	{ uint32_t _v = ctx->a[2]; st32((ctx->a[5] + (uint32_t)(int32_t)8), _v); cc_logic32(ctx, _v); hle_insn(ctx, 16); } /* 012582 movel %a2,%a5@(8) */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)2), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012586 movew %d0,%a5@(2) */
	{ uint32_t _v = (ctx->d[1] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)4), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01258A movew %d1,%a5@(4) */
	{ uint32_t _v = (ctx->d[2] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)6), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01258E movew %d2,%a5@(6) */
	{ uint32_t _v = 0x1u; st16(ctx->a[5], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012592 movew #1,%a5@ */
	{ st8((ctx->a[5] + (uint32_t)(int32_t)12), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 012596 clrb %a5@(12) */
	{ st8((ctx->a[5] + (uint32_t)(int32_t)13), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 01259A clrb %a5@(13) */
	{ st8((ctx->a[5] + (uint32_t)(int32_t)16), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 01259E clrb %a5@(16) */
	{ st8((ctx->a[5] + (uint32_t)(int32_t)14), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 0125A2 clrb %a5@(14) */
	{ st8((ctx->a[5] + (uint32_t)(int32_t)15), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 0125A6 clrb %a5@(15) */
	{ st8((ctx->a[5] + (uint32_t)(int32_t)17), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 0125AA clrb %a5@(17) */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DCu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 0125AE addqw #1,0xffff80dc */
L_0125B4:;
	hle_insn(ctx, 16); return; /* 0125B4 rts */
}

/* sub_012502 (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_012502(hle_ctx *ctx)
{
	{ uint32_t _v = 0x1u; st8((ctx->a[5] + (uint32_t)(int32_t)16), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 012502 moveb #1,%a5@(16) */
	{ hle_insn(ctx, 20); Random(ctx); } /* 012508 jsr 0x881c */
	if (bs(ctx, c_n(ctx))) goto L_012514; /* 01250E bmis 0x12514 */
	{ uint32_t _da = (ctx->a[5] + (uint32_t)(int32_t)16); uint32_t _r = alu(ctx, OP_SUB, 0, ld8(_da), 8, 1); st8(_da, _r); hle_insn(ctx, 16); } /* 012510 negb %a5@(16) */
L_012514:;
	{ uint32_t _s = 0x80u; uint32_t _d = (ctx->d[0] & 0xFFu); uint32_t _r = (_d | _s) & 0xFFu; cc_logic8(ctx, _r); set_b(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 012514 orib #-128,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFu); st8((ctx->a[5] + (uint32_t)(int32_t)12), _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 012518 moveb %d0,%a5@(12) */
	{ st8((ctx->a[5] + (uint32_t)(int32_t)13), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 01251C clrb %a5@(13) */
	{ uint32_t _v = 0x1u; st8((ctx->a[5] + (uint32_t)(int32_t)17), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 012520 moveb #1,%a5@(17) */
	{ hle_insn(ctx, 20); Random(ctx); } /* 012526 jsr 0x881c */
	if (bs(ctx, c_n(ctx))) goto L_012532; /* 01252C bmis 0x12532 */
	{ uint32_t _da = (ctx->a[5] + (uint32_t)(int32_t)17); uint32_t _r = alu(ctx, OP_SUB, 0, ld8(_da), 8, 1); st8(_da, _r); hle_insn(ctx, 16); } /* 01252E negb %a5@(17) */
L_012532:;
	{ uint32_t _v = (ctx->d[0] & 0xFFu); st8((ctx->a[5] + (uint32_t)(int32_t)14), _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 012532 moveb %d0,%a5@(14) */
	{ st8((ctx->a[5] + (uint32_t)(int32_t)15), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 012536 clrb %a5@(15) */
	hle_insn(ctx, 16); return; /* 01253A rts */
}

/* sub_01237A (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_01237A(hle_ctx *ctx)
{
	{ uint32_t _v = ctx->a[6]; ctx->a[4] = _v; hle_insn(ctx, 4); } /* 01237A moveal %fp,%a4 */
	{ hle_insn(ctx, 20); ObjAlloc(ctx); } /* 01237C jsr 0x930e */
	{ cc_logic16(ctx, (ctx->d[2] & 0xFFFFu)); hle_insn(ctx, 4); } /* 012382 tstw %d2 */
	if (bw(ctx, c_n(ctx))) goto L_0123EC; /* 012384 bmiw 0x123ec */
	{ uint32_t _v = ld16((ctx->a[4] + (uint32_t)(int32_t)2)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012388 movew %a4@(2),%d0 */
	{ uint32_t _s = 0xAu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 01238C addiw #10,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)2), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012390 movew %d0,%fp@(2) */
	{ uint32_t _v = ld16((ctx->a[4] + (uint32_t)(int32_t)4)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012394 movew %a4@(4),%d0 */
	{ uint32_t _s = 0x36u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 012398 subiw #54,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)4), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01239C movew %d0,%fp@(4) */
	{ uint32_t _v = 0xAu; st16(ctx->a[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0123A0 movew #10,%fp@ */
	{ uint32_t _v = 0x40u; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 0123A4 movew #64,%fp@(36) */
	{ uint32_t _v = 0xC3C6u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 0123AA movel #50118,%fp@(32) */
	{ uint32_t _v = 0x6042u; st32((ctx->a[6] + (uint32_t)(int32_t)28), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 0123B2 movel #24642,%fp@(28) */
	{ uint32_t _v = 0x10u; st8((ctx->a[6] + (uint32_t)(int32_t)6), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 0123BA moveb #16,%fp@(6) */
	{ uint32_t _v = 0x96u; st8((ctx->a[6] + (uint32_t)(int32_t)7), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 0123C0 moveb #-106,%fp@(7) */
	{ uint32_t _v = 0x0u; st16((ctx->a[6] + (uint32_t)(int32_t)8), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 0123C6 movew #0,%fp@(8) */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o | 0x80); hle_insn(ctx, 16); } /* 0123CC bset #15,%fp@ */
	{ st8((ctx->a[6] + (uint32_t)(int32_t)13), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 0123D0 clrb %fp@(13) */
	{ uint32_t _v = 0x4u; st8((ctx->a[6] + (uint32_t)(int32_t)18), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 0123D4 moveb #4,%fp@(18) */
	{ uint32_t _v = 0xAAu; st8((ctx->a[6] + (uint32_t)(int32_t)12), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 0123DA moveb #-86,%fp@(12) */
	{ uint32_t _v = 0xFFu; st8((ctx->a[6] + (uint32_t)(int32_t)15), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 0123E0 moveb #-1,%fp@(15) */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DAu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 0123E6 addqw #1,0xffff80da */
L_0123EC:;
	{ uint32_t _t = ctx->a[4]; ctx->a[4] = ctx->a[6]; ctx->a[6] = _t; hle_insn(ctx, 6); } /* 0123EC exg %a4,%fp */
	hle_insn(ctx, 16); return; /* 0123EE rts */
}

/* sub_01277C (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_01277C(hle_ctx *ctx)
{
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)6)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01277C movew %fp@(6),%d2 */
	{ hle_insn(ctx, 18); sub_012956(ctx); } /* 012780 jsr %pc@(0x12956) */
	{ cc_logic16(ctx, (ctx->d[6] & 0xFFFFu)); hle_insn(ctx, 4); } /* 012784 tstw %d6 */
	if (bs(ctx, c_n(ctx))) goto L_0127F4; /* 012786 bmis 0x127f4 */
	{ uint32_t _v = 0x1u; st16(ctx->a[5], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012788 movew #1,%a5@ */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)2), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01278C movew %d0,%a5@(2) */
	{ uint32_t _v = (ctx->d[1] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)4), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012790 movew %d1,%a5@(4) */
	{ uint32_t _v = (ctx->d[2] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)6), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012794 movew %d2,%a5@(6) */
	{ st16((ctx->a[5] + (uint32_t)(int32_t)16), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 012798 clrw %a5@(16) */
	{ st16((ctx->a[5] + (uint32_t)(int32_t)20), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 01279C clrw %a5@(20) */
	{ hle_insn(ctx, 20); Random(ctx); } /* 0127A0 jsr 0x881c */
	{ uint32_t _s = 0x3u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127A6 andiw #3,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)12), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0127AA movew %d0,%a5@(12) */
	{ hle_insn(ctx, 20); Random(ctx); } /* 0127AE jsr 0x881c */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 0127B4 movew %d0,%d1 */
	{ uint32_t _s = 0xFFu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127B6 andiw #255,%d0 */
	{ cc_logic16(ctx, (ctx->d[1] & 0xFFFFu)); hle_insn(ctx, 4); } /* 0127BA tstw %d1 */
	if (bs(ctx, !c_n(ctx))) goto L_0127C0; /* 0127BC bpls 0x127c0 */
	{ uint32_t _r = alu(ctx, OP_SUB, 0, ctx->d[0], 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 0127BE negw %d0 */
L_0127C0:;
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)14), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0127C0 movew %d0,%a5@(14) */
	{ hle_insn(ctx, 20); Random(ctx); } /* 0127C4 jsr 0x881c */
	{ uint32_t _s = 0xF0u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127CA andiw #240,%d0 */
	{ uint32_t _s = 0x100u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d | _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127CE oriw #256,%d0 */
	{ uint32_t _r = alu(ctx, OP_SUB, 0, ctx->d[0], 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 0127D2 negw %d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)18), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0127D4 movew %d0,%a5@(18) */
	{ hle_insn(ctx, 20); Random(ctx); } /* 0127D8 jsr 0x881c */
	{ uint32_t _s = 0x7u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127DE andiw #7,%d0 */
	{ uint32_t _s = 0x18u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d | _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127E2 oriw #24,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)22), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0127E6 movew %d0,%a5@(22) */
	{ uint32_t _v = ctx->a[2]; st32((ctx->a[5] + (uint32_t)(int32_t)8), _v); cc_logic32(ctx, _v); hle_insn(ctx, 16); } /* 0127EA movel %a2,%a5@(8) */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DEu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 0127EE addqw #1,0xffff80de */
L_0127F4:;
	hle_insn(ctx, 16); return; /* 0127F4 rts */
}

/* sub_013184 (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_013184(hle_ctx *ctx)
{
	{ ctx->a[5] = 0x0131B2u; hle_insn(ctx, 8); } /* 013184 lea %pc@(0x131b2),%a5 */
	{ hle_insn(ctx, 18); sub_00C48E(ctx); } /* 013188 jsr %pc@(0xc48e) */
	{ cc_logic16(ctx, (ctx->d[4] & 0xFFFFu)); hle_insn(ctx, 4); } /* 01318C tstw %d4 */
	if (bw(ctx, c_z(ctx))) goto L_0131A8; /* 01318E beqw 0x131a8 */
L_013192:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)20)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 013192 movew %fp@(20),%d1 */
	{ sh_w(ctx, 1, 'r', 4); hle_insn(ctx, 14); } /* 013196 lsrw #4,%d1 */
	{ uint32_t _s = 0xEu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 013198 andiw #14,%d1 */
	{ uint32_t _v = ld16((ctx->a[5] + (uint32_t)(int32_t)0 + (uint32_t)((int32_t)(int16_t)ctx->d[1]))); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 14); } /* 01319C movew %a5@(0,%d1:w),%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 0131A0 jsr %pc@(0x13228) */
	hle_insn(ctx, 10); return; /* 0131A4 braw 0x12432 */
L_0131A8:;
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 0131A8 clrw %fp@ */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80D6u; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 0131AA subqw #1,0xffff80d6 */
	hle_insn(ctx, 10); goto L_013192; /* 0131B0 bras 0x13192 */
}

/* sub_01317E (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_01317E(hle_ctx *ctx)
{
	{ ctx->a[5] = 0x0131C2u; hle_insn(ctx, 8); } /* 01317E lea %pc@(0x131c2),%a5 */
	hle_insn(ctx, 10); goto L_013188; /* 013182 bras 0x13188 */
L_013188:;
	{ hle_insn(ctx, 18); sub_00C48E(ctx); } /* 013188 jsr %pc@(0xc48e) */
	{ cc_logic16(ctx, (ctx->d[4] & 0xFFFFu)); hle_insn(ctx, 4); } /* 01318C tstw %d4 */
	if (bw(ctx, c_z(ctx))) goto L_0131A8; /* 01318E beqw 0x131a8 */
L_013192:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)20)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 013192 movew %fp@(20),%d1 */
	{ sh_w(ctx, 1, 'r', 4); hle_insn(ctx, 14); } /* 013196 lsrw #4,%d1 */
	{ uint32_t _s = 0xEu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 013198 andiw #14,%d1 */
	{ uint32_t _v = ld16((ctx->a[5] + (uint32_t)(int32_t)0 + (uint32_t)((int32_t)(int16_t)ctx->d[1]))); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 14); } /* 01319C movew %a5@(0,%d1:w),%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 0131A0 jsr %pc@(0x13228) */
	hle_insn(ctx, 10); return; /* 0131A4 braw 0x12432 */
L_0131A8:;
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 0131A8 clrw %fp@ */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80D6u; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 0131AA subqw #1,0xffff80d6 */
	hle_insn(ctx, 10); goto L_013192; /* 0131B0 bras 0x13192 */
}

/* sub_013178 (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_013178(hle_ctx *ctx)
{
	{ ctx->a[5] = 0x0131D2u; hle_insn(ctx, 8); } /* 013178 lea %pc@(0x131d2),%a5 */
	hle_insn(ctx, 10); goto L_013188; /* 01317C bras 0x13188 */
L_013188:;
	{ hle_insn(ctx, 18); sub_00C48E(ctx); } /* 013188 jsr %pc@(0xc48e) */
	{ cc_logic16(ctx, (ctx->d[4] & 0xFFFFu)); hle_insn(ctx, 4); } /* 01318C tstw %d4 */
	if (bw(ctx, c_z(ctx))) goto L_0131A8; /* 01318E beqw 0x131a8 */
L_013192:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)20)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 013192 movew %fp@(20),%d1 */
	{ sh_w(ctx, 1, 'r', 4); hle_insn(ctx, 14); } /* 013196 lsrw #4,%d1 */
	{ uint32_t _s = 0xEu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 013198 andiw #14,%d1 */
	{ uint32_t _v = ld16((ctx->a[5] + (uint32_t)(int32_t)0 + (uint32_t)((int32_t)(int16_t)ctx->d[1]))); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 14); } /* 01319C movew %a5@(0,%d1:w),%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 0131A0 jsr %pc@(0x13228) */
	hle_insn(ctx, 10); return; /* 0131A4 braw 0x12432 */
L_0131A8:;
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 0131A8 clrw %fp@ */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80D6u; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 0131AA subqw #1,0xffff80d6 */
	hle_insn(ctx, 10); goto L_013192; /* 0131B0 bras 0x13192 */
}

/* $013188: the shared tail of the missile states 1..3 (a5 = the frame table): one step of the movement
 * script (C), the slot is freed when it ended, the frame is queued and the loop goes on at sub_012432.
 * The 68k reaches it by `bra.s` from the three state stubs when MissilesRun bailed or declined. */
HLE_RANGE_PURE(0x013188, MissileTail, 1000, 0x012432)
{
	{ hle_insn(ctx, 18); sub_00C48E(ctx); } /* 013188 jsr %pc@(0xc48e) */
	{ cc_logic16(ctx, (ctx->d[4] & 0xFFFFu)); hle_insn(ctx, 4); } /* 01318C tstw %d4 */
	if (bw(ctx, c_z(ctx))) goto L_0131A8; /* 01318E beqw 0x131a8 */
L_013192:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)20)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 013192 movew %fp@(20),%d1 */
	{ sh_w(ctx, 1, 'r', 4); hle_insn(ctx, 14); } /* 013196 lsrw #4,%d1 */
	{ uint32_t _s = 0xEu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 013198 andiw #14,%d1 */
	{ uint32_t _v = ld16((ctx->a[5] + (uint32_t)(int32_t)0 + (uint32_t)((int32_t)(int16_t)ctx->d[1]))); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 14); } /* 01319C movew %a5@(0,%d1:w),%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 0131A0 jsr %pc@(0x13228) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x012432); return; /* 0131A4 braw 0x12432 */
L_0131A8:;
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 0131A8 clrw %fp@ */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80D6u; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 0131AA subqw #1,0xffff80d6 */
	hle_insn(ctx, 10); goto L_013192; /* 0131B0 bras 0x13192 */
}

/* sub_012FC2 (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_012FC2(hle_ctx *ctx)
{
	{ uint32_t _v = 0x12F50u; ctx->d[5] = _v; cc_logic32(ctx, _v); hle_insn(ctx, 12); } /* 012FC2 movel #77648,%d5 */
	{ hle_insn(ctx, 18); sub_00C48E(ctx); } /* 012FC8 jsr %pc@(0xc48e) */
	{ cc_logic16(ctx, (ctx->d[4] & 0xFFFFu)); hle_insn(ctx, 4); } /* 012FCC tstw %d4 */
	if (bw(ctx, c_z(ctx))) goto L_013052; /* 012FCE beqw 0x13052 */
	{ uint32_t _v = ld32((ctx->a[6] + (uint32_t)(int32_t)34)); ctx->d[0] = _v; cc_logic32(ctx, _v); hle_insn(ctx, 16); } /* 012FD2 movel %fp@(34),%d0 */
	if (bs(ctx, c_z(ctx))) goto L_012FF8; /* 012FD6 beqs 0x12ff8 */
	{ uint32_t _v = ctx->d[0]; ctx->a[1] = _v; hle_insn(ctx, 4); } /* 012FD8 moveal %d0,%a1 */
	{ uint32_t _v = ld16((ctx->a[1] + (uint32_t)(int32_t)2)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012FDA movew %a1@(2),%d0 */
	{ uint32_t _s = ld16((ctx->a[6] + (uint32_t)(int32_t)38)); uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 12); } /* 012FDE subw %fp@(38),%d0 */
	{ uint32_t _s = (ctx->d[0] & 0xFFFFu); uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)2); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 012FE2 addw %d0,%fp@(2) */
	{ uint32_t _v = ld16((ctx->a[1] + (uint32_t)(int32_t)4)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012FE6 movew %a1@(4),%d0 */
	{ uint32_t _s = ld16((ctx->a[6] + (uint32_t)(int32_t)40)); uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 12); } /* 012FEA subw %fp@(40),%d0 */
	{ uint32_t _s = (ctx->d[0] & 0xFFFFu); uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)4); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 012FEE addw %d0,%fp@(4) */
	{ uint32_t _v = ld32((ctx->a[1] + (uint32_t)(int32_t)2)); st32((ctx->a[6] + (uint32_t)(int32_t)38), _v); cc_logic32(ctx, _v); hle_insn(ctx, 28); } /* 012FF2 movel %a1@(2),%fp@(38) */
L_012FF8:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)20)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012FF8 movew %fp@(20),%d1 */
	{ sh_w(ctx, 1, 'r', 4); hle_insn(ctx, 14); } /* 012FFC lsrw #4,%d1 */
	{ uint32_t _s = 0xEu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 012FFE andiw #14,%d1 */
	{ uint32_t _v = ld16((0x013042u + (uint32_t)((int32_t)(int16_t)ctx->d[1]))); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 14); } /* 013002 movew %pc@(0x13042,%d1:w),%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 013006 jsr %pc@(0x13228) */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)33); uint32_t _d = ld8(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 8, 1); st8(_da, _r); hle_insn(ctx, 16); } /* 01300A subqb #1,%fp@(33) */
	if (bs(ctx, !c_z(ctx))) goto L_01303E; /* 01300E bnes 0x1303e */
	{ uint32_t _v = ld8((ctx->a[6] + (uint32_t)(int32_t)32)); st8((ctx->a[6] + (uint32_t)(int32_t)33), _v); cc_logic8(ctx, _v); hle_insn(ctx, 20); } /* 013010 moveb %fp@(32),%fp@(33) */
	{ hle_insn(ctx, 18); sub_01293E(ctx); } /* 013016 jsr %pc@(0x1293e) */
	{ cc_logic16(ctx, (ctx->d[0] & 0xFFFFu)); hle_insn(ctx, 4); } /* 01301A tstw %d0 */
	if (bs(ctx, c_n(ctx))) goto L_01303E; /* 01301C bmis 0x1303e */
	{ uint32_t _v = 0x1u; st16(ctx->a[5], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01301E movew #1,%a5@ */
	{ uint32_t _v = ctx->d[5]; st32((ctx->a[5] + (uint32_t)(int32_t)8), _v); cc_logic32(ctx, _v); hle_insn(ctx, 16); } /* 013022 movel %d5,%a5@(8) */
	{ uint32_t _v = ld32((ctx->a[6] + (uint32_t)(int32_t)2)); st32((ctx->a[5] + (uint32_t)(int32_t)2), _v); cc_logic32(ctx, _v); hle_insn(ctx, 28); } /* 013026 movel %fp@(2),%a5@(2) */
	{ uint32_t _v = 0x15u; st8((ctx->a[5] + (uint32_t)(int32_t)6), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 01302C moveb #21,%a5@(6) */
	{ uint32_t _v = 0x5u; st8((ctx->a[5] + (uint32_t)(int32_t)7), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 013032 moveb #5,%a5@(7) */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80D8u; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 013038 addqw #1,0xffff80d8 */
L_01303E:;
	hle_insn(ctx, 10); return; /* 01303E braw 0x12432 */
L_013052:;
	{ uint32_t _v = (ctx->d[7] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 013052 movew %d7,%sp@- */
	{ uint32_t _v = ctx->a[6]; push32(ctx, _v); cc_logic32(ctx, _v); hle_insn(ctx, 12); } /* 013054 movel %fp,%sp@- */
	{ ctx->a[4] = 0xFFFF8000u; hle_insn(ctx, 12); } /* 013056 lea 0xffff8000,%a4 */
	{ cc_logic16(ctx, ld16((ctx->a[6] + (uint32_t)(int32_t)30))); hle_insn(ctx, 12); } /* 01305C tstw %fp@(30) */
	if (bs(ctx, c_z(ctx))) goto L_013068; /* 013060 beqs 0x13068 */
	{ ctx->a[4] = 0xFFFF8050u; hle_insn(ctx, 12); } /* 013062 lea 0xffff8050,%a4 */
L_013068:;
	{ uint32_t _v = ld32((ctx->a[6] + (uint32_t)(int32_t)2)); st32(0xFFFF9E94u, _v); cc_logic32(ctx, _v); hle_insn(ctx, 32); } /* 013068 movel %fp@(2),0xffff9e94 */
	{ uint32_t _v = 0x29u; st16(0xFFFFA0CAu, _v); cc_logic16(ctx, _v); hle_insn(ctx, 20); } /* 013070 movew #41,0xffffa0ca */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_call68k(ctx, 0x005892); } /* 013078 jsr 0x5892 */
	{ uint32_t _v = pop32(ctx); ctx->a[6] = _v; hle_insn(ctx, 12); } /* 01307E moveal %sp@+,%fp */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 013080 movew %sp@+,%d7 */
	{ uint32_t _s = 0x2u; uint32_t _da = 0xFFFF80CCu; uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 20); } /* 013082 cmpiw #2,0xffff80cc */
	if (bs(ctx, c_z(ctx))) goto L_013096; /* 01308A beqs 0x13096 */
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 01308C clrw %fp@ */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80D6u; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 01308E subqw #1,0xffff80d6 */
	hle_insn(ctx, 10); goto L_01303E; /* 013094 bras 0x1303e */
L_013096:;
	{ uint32_t _v = 0x7u; st16(ctx->a[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 013096 movew #7,%fp@ */
	{ uint32_t _v = 0xCu; st16((ctx->a[6] + (uint32_t)(int32_t)20), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 01309A movew #12,%fp@(20) */
	hle_insn(ctx, 10); goto L_01303E; /* 0130A0 bras 0x1303e */
}

/* sub_013110 (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_013110(hle_ctx *ctx)
{
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 013110 subqw #1,%fp@(36) */
	if (bs(ctx, c_n(ctx))) goto L_013172; /* 013114 bmis 0x13172 */
	{ ctx->a[4] = 0xFFFF8000u; hle_insn(ctx, 12); } /* 013116 lea 0xffff8000,%a4 */
	{ cc_logic16(ctx, ld16((ctx->a[6] + (uint32_t)(int32_t)30))); hle_insn(ctx, 12); } /* 01311C tstw %fp@(30) */
	if (bs(ctx, c_z(ctx))) goto L_013128; /* 013120 beqs 0x13128 */
	{ ctx->a[4] = 0xFFFF8050u; hle_insn(ctx, 12); } /* 013122 lea 0xffff8050,%a4 */
L_013128:;
	{ hle_insn(ctx, 20); Random(ctx); } /* 013128 jsr 0x881c */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 01312E movew %d0,%d1 */
	{ uint32_t _s = 0xFu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 013130 andiw #15,%d0 */
	{ cc_logic8(ctx, (ctx->d[1] & 0xFFu)); hle_insn(ctx, 4); } /* 013134 tstb %d1 */
	if (bs(ctx, !c_n(ctx))) goto L_01313A; /* 013136 bpls 0x1313a */
	{ uint32_t _r = alu(ctx, OP_SUB, 0, ctx->d[0], 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 013138 negw %d0 */
L_01313A:;
	{ uint32_t _s = ld16((ctx->a[6] + (uint32_t)(int32_t)38)); uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 12); } /* 01313A addw %fp@(38),%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16(0xFFFF9E94u, _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 01313E movew %d0,0xffff9e94 */
	{ sh_w(ctx, 1, 'r', 8); hle_insn(ctx, 22); } /* 013144 lsrw #8,%d1 */
	{ uint32_t _v = (ctx->d[1] & 0xFFFFu); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 013146 movew %d1,%d0 */
	{ uint32_t _s = 0xFu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 013148 andiw #15,%d0 */
	{ cc_logic8(ctx, (ctx->d[1] & 0xFFu)); hle_insn(ctx, 4); } /* 01314C tstb %d1 */
	if (bs(ctx, !c_n(ctx))) goto L_013152; /* 01314E bpls 0x13152 */
	{ uint32_t _r = alu(ctx, OP_SUB, 0, ctx->d[0], 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 013150 negw %d0 */
L_013152:;
	{ uint32_t _s = ld16((ctx->a[6] + (uint32_t)(int32_t)40)); uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 12); } /* 013152 addw %fp@(40),%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16(0xFFFF9E96u, _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 013156 movew %d0,0xffff9e96 */
	{ uint32_t _v = 0x8u; st16(0xFFFFA0CAu, _v); cc_logic16(ctx, _v); hle_insn(ctx, 20); } /* 01315C movew #8,0xffffa0ca */
	{ uint32_t _v = ctx->a[6]; push32(ctx, _v); cc_logic32(ctx, _v); hle_insn(ctx, 12); } /* 013164 movel %fp,%sp@- */
	{ uint32_t _v = (ctx->d[7] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 013166 movew %d7,%sp@- */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_call68k(ctx, 0x005892); } /* 013168 jsr 0x5892 */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 01316E movew %sp@+,%d7 */
	{ uint32_t _v = pop32(ctx); ctx->a[6] = _v; hle_insn(ctx, 12); } /* 013170 moveal %sp@+,%fp */
L_013172:;
	{ ctx->a[5] = 0x0131E2u; hle_insn(ctx, 8); } /* 013172 lea %pc@(0x131e2),%a5 */
	hle_insn(ctx, 10); goto L_013188; /* 013176 bras 0x13188 */
L_013188:;
	{ hle_insn(ctx, 18); sub_00C48E(ctx); } /* 013188 jsr %pc@(0xc48e) */
	{ cc_logic16(ctx, (ctx->d[4] & 0xFFFFu)); hle_insn(ctx, 4); } /* 01318C tstw %d4 */
	if (bw(ctx, c_z(ctx))) goto L_0131A8; /* 01318E beqw 0x131a8 */
L_013192:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)20)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 013192 movew %fp@(20),%d1 */
	{ sh_w(ctx, 1, 'r', 4); hle_insn(ctx, 14); } /* 013196 lsrw #4,%d1 */
	{ uint32_t _s = 0xEu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 013198 andiw #14,%d1 */
	{ uint32_t _v = ld16((ctx->a[5] + (uint32_t)(int32_t)0 + (uint32_t)((int32_t)(int16_t)ctx->d[1]))); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 14); } /* 01319C movew %a5@(0,%d1:w),%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 0131A0 jsr %pc@(0x13228) */
	hle_insn(ctx, 10); return; /* 0131A4 braw 0x12432 */
L_0131A8:;
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 0131A8 clrw %fp@ */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80D6u; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 0131AA subqw #1,0xffff80d6 */
	hle_insn(ctx, 10); goto L_013192; /* 0131B0 bras 0x13192 */
}

/* sub_0130A2 (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_0130A2(hle_ctx *ctx)
{
	{ uint32_t _v = (ctx->d[7] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0130A2 movew %d7,%sp@- */
	{ uint32_t _v = ctx->a[6]; push32(ctx, _v); cc_logic32(ctx, _v); hle_insn(ctx, 12); } /* 0130A4 movel %fp,%sp@- */
	{ ctx->a[4] = 0xFFFF8000u; hle_insn(ctx, 12); } /* 0130A6 lea 0xffff8000,%a4 */
	{ cc_logic16(ctx, ld16((ctx->a[6] + (uint32_t)(int32_t)30))); hle_insn(ctx, 12); } /* 0130AC tstw %fp@(30) */
	if (bs(ctx, c_z(ctx))) goto L_0130B8; /* 0130B0 beqs 0x130b8 */
	{ ctx->a[4] = 0xFFFF8050u; hle_insn(ctx, 12); } /* 0130B2 lea 0xffff8050,%a4 */
L_0130B8:;
	{ hle_insn(ctx, 20); Random(ctx); } /* 0130B8 jsr 0x881c */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 0130BE movew %d0,%d1 */
	{ uint32_t _s = 0x1Fu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0130C0 andiw #31,%d0 */
	{ cc_logic8(ctx, (ctx->d[1] & 0xFFu)); hle_insn(ctx, 4); } /* 0130C4 tstb %d1 */
	if (bs(ctx, !c_n(ctx))) goto L_0130CA; /* 0130C6 bpls 0x130ca */
	{ uint32_t _r = alu(ctx, OP_SUB, 0, ctx->d[0], 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 0130C8 negw %d0 */
L_0130CA:;
	{ uint32_t _s = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 12); } /* 0130CA addw %fp@(2),%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16(0xFFFF9E94u, _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 0130CE movew %d0,0xffff9e94 */
	{ sh_w(ctx, 1, 'r', 8); hle_insn(ctx, 22); } /* 0130D4 lsrw #8,%d1 */
	{ uint32_t _v = (ctx->d[1] & 0xFFFFu); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 0130D6 movew %d1,%d0 */
	{ uint32_t _s = 0x1Fu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0130D8 andiw #31,%d0 */
	{ cc_logic8(ctx, (ctx->d[1] & 0xFFu)); hle_insn(ctx, 4); } /* 0130DC tstb %d1 */
	if (bs(ctx, !c_n(ctx))) goto L_0130E2; /* 0130DE bpls 0x130e2 */
	{ uint32_t _r = alu(ctx, OP_SUB, 0, ctx->d[0], 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 0130E0 negw %d0 */
L_0130E2:;
	{ uint32_t _s = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 12); } /* 0130E2 addw %fp@(4),%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16(0xFFFF9E96u, _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 0130E6 movew %d0,0xffff9e96 */
	{ uint32_t _v = 0x3u; st16(0xFFFFA0CAu, _v); cc_logic16(ctx, _v); hle_insn(ctx, 20); } /* 0130EC movew #3,0xffffa0ca */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_call68k(ctx, 0x005892); } /* 0130F4 jsr 0x5892 */
	{ uint32_t _v = pop32(ctx); ctx->a[6] = _v; hle_insn(ctx, 12); } /* 0130FA moveal %sp@+,%fp */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0130FC movew %sp@+,%d7 */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)20); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 0130FE subqw #1,%fp@(20) */
	if (bs(ctx, !c_z(ctx))) goto L_01310C; /* 013102 bnes 0x1310c */
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 013104 clrw %fp@ */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80D6u; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 013106 subqw #1,0xffff80d6 */
L_01310C:;
	hle_insn(ctx, 10); return; /* 01310C braw 0x12432 */
}

HLE_FUNC_PURE(0x01237A, SpawnBomb, 600)
{
	sub_01237A(ctx);
}

HLE_FUNC_PURE(0x012502, SparkRandVel, 600)
{
	sub_012502(ctx);
}

HLE_FUNC_PURE(0x01257A, SparkSpawn, 600)
{
	sub_01257A(ctx);
}

HLE_FUNC_PURE(0x01277C, DebrisSpawn, 600)
{
	sub_01277C(ctx);
}

HLE_FUNC_PURE(0x012926, SparkAlloc, 600)
{
	sub_012926(ctx);
}

HLE_FUNC_PURE(0x01293E, EffectAlloc, 600)
{
	sub_01293E(ctx);
}

HLE_FUNC_PURE(0x012956, DebrisAlloc, 600)
{
	sub_012956(ctx);
}

/* $01243C */
HLE_FUNC_PURE(0x01243C, EffectsRun, 4000)
{
	{ ctx->a[6] = 0xFFFF8632u; hle_insn(ctx, 12); } /* 01243C lea 0xffff8632,%fp */
	{ uint32_t _v = ld16(0xFFFF80D8u); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 012442 movew 0xffff80d8,%d7 */
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[7] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[7], _r); hle_insn(ctx, 4); } /* 012448 subqw #1,%d7 */
	if (bs(ctx, c_n(ctx))) goto L_01247A; /* 01244A bmis 0x1247a */
L_01244C:;
	{ uint32_t _v = ld16(ctx->a[6]); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 01244C movew %fp@,%d0 */
	if (bs(ctx, c_z(ctx))) goto L_01247C; /* 01244E beqs 0x1247c */
	{ uint32_t _v = ld32((ctx->a[6] + (uint32_t)(int32_t)8)); ctx->a[5] = _v; hle_insn(ctx, 16); } /* 012450 moveal %fp@(8),%a5 */
	{ uint32_t _p = ld16(ctx->a[5]); ctx->a[5] += 2; uint32_t _v = _p; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 012454 movew %a5@+,%d1 */
	if (bw(ctx, c_z(ctx))) goto L_012482; /* 012456 beqw 0x12482 */
	{ uint32_t _v = ctx->a[5]; st32((ctx->a[6] + (uint32_t)(int32_t)8), _v); cc_logic32(ctx, _v); hle_insn(ctx, 16); } /* 01245A movel %a5,%fp@(8) */
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 01245E jsr %pc@(0x12316) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012462 movew %fp@(2),%d2 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012466 movew %fp@(4),%d3 */
	{ uint32_t _v = 0x5u; set_w(&ctx->d[5], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 01246A movew #5,%d5 */
	{ hle_insn(ctx, 18); QueueSprite(ctx); } /* 01246E bsrw 0x13240 */
L_012472:;
	{ ctx->a[6] = (ctx->a[6] + (uint32_t)(int32_t)12); hle_insn(ctx, 8); } /* 012472 lea %fp@(12),%fp */
	{ uint16_t _c = (uint16_t)ctx->d[7] - 1; set_w(&ctx->d[7], _c); if (_c != 0xFFFF) { hle_insn(ctx, 10); goto L_01244C; } hle_insn(ctx, 14); } /* 012476 dbf %d7,0x1244c */
L_01247A:;
	hle_insn(ctx, 16); return; /* 01247A rts */
L_01247C:;
	{ ctx->a[6] = (ctx->a[6] + (uint32_t)(int32_t)12); hle_insn(ctx, 8); } /* 01247C lea %fp@(12),%fp */
	hle_insn(ctx, 10); goto L_01244C; /* 012480 bras 0x1244c */
L_012482:;
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 012482 clrw %fp@ */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80D8u; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 012484 subqw #1,0xffff80d8 */
	hle_insn(ctx, 10); goto L_012472; /* 01248A bras 0x12472 */
}

/* $0126B4 */
HLE_FUNC_PURE(0x0126B4, SparksRun, 4000)
{
	{ ctx->a[6] = 0xFFFF9572u; hle_insn(ctx, 12); } /* 0126B4 lea 0xffff9572,%fp */
	{ uint32_t _v = ld16(0xFFFF80DCu); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 0126BA movew 0xffff80dc,%d7 */
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[7] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[7], _r); hle_insn(ctx, 4); } /* 0126C0 subqw #1,%d7 */
	if (bs(ctx, c_n(ctx))) goto L_012718; /* 0126C2 bmis 0x12718 */
L_0126C4:;
	{ uint32_t _v = ld16(ctx->a[6]); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0126C4 movew %fp@,%d0 */
	if (bs(ctx, c_z(ctx))) goto L_01271A; /* 0126C6 beqs 0x1271a */
	{ uint32_t _v = ld32((ctx->a[6] + (uint32_t)(int32_t)8)); ctx->a[5] = _v; hle_insn(ctx, 16); } /* 0126C8 moveal %fp@(8),%a5 */
	{ ctx->d[6] = 0x1u; cc_logic32(ctx, ctx->d[6]); hle_insn(ctx, 4); } /* 0126CC moveq #1,%d6 */
L_0126CE:;
	{ uint32_t _v = ld8((ctx->a[6] + (uint32_t)(int32_t)16)); set_b(&ctx->d[0], _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 0126CE moveb %fp@(16),%d0 */
	{ set_w(&ctx->d[0], (uint32_t)(int32_t)(int8_t)ctx->d[0]); cc_logic16(ctx, ctx->d[0]); hle_insn(ctx, 4); } /* 0126D2 extw %d0 */
	{ uint32_t _v = ld8((ctx->a[6] + (uint32_t)(int32_t)12)); set_b(&ctx->d[1], _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 0126D4 moveb %fp@(12),%d1 */
	{ uint32_t _v = ld8((ctx->a[6] + (uint32_t)(int32_t)17)); set_b(&ctx->d[2], _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 0126D8 moveb %fp@(17),%d2 */
	{ set_w(&ctx->d[2], (uint32_t)(int32_t)(int8_t)ctx->d[2]); cc_logic16(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 0126DC extw %d2 */
	{ uint32_t _v = ld8((ctx->a[6] + (uint32_t)(int32_t)14)); set_b(&ctx->d[3], _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 0126DE moveb %fp@(14),%d3 */
	{ uint32_t _s = (ctx->d[1] & 0xFFu); uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)13); uint32_t _d = ld8(_da); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 8, 1); st8(_da, _r); hle_insn(ctx, 16); } /* 0126E2 addb %d1,%fp@(13) */
	if (bs(ctx, !c_c(ctx))) goto L_0126EC; /* 0126E6 bccs 0x126ec */
	{ uint32_t _s = (ctx->d[0] & 0xFFFFu); uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)2); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 0126E8 addw %d0,%fp@(2) */
L_0126EC:;
	{ uint32_t _s = (ctx->d[3] & 0xFFu); uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)15); uint32_t _d = ld8(_da); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 8, 1); st8(_da, _r); hle_insn(ctx, 16); } /* 0126EC addb %d3,%fp@(15) */
	if (bs(ctx, !c_c(ctx))) goto L_0126F6; /* 0126F0 bccs 0x126f6 */
	{ uint32_t _s = (ctx->d[2] & 0xFFFFu); uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)4); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 0126F2 addw %d2,%fp@(4) */
L_0126F6:;
	{ uint16_t _c = (uint16_t)ctx->d[6] - 1; set_w(&ctx->d[6], _c); if (_c != 0xFFFF) { hle_insn(ctx, 10); goto L_0126CE; } hle_insn(ctx, 14); } /* 0126F6 dbf %d6,0x126ce */
	{ uint32_t _p = ld16(ctx->a[5]); ctx->a[5] += 2; uint32_t _v = _p; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0126FA movew %a5@+,%d1 */
	if (bw(ctx, c_z(ctx))) goto L_012720; /* 0126FC beqw 0x12720 */
	{ uint32_t _v = ctx->a[5]; st32((ctx->a[6] + (uint32_t)(int32_t)8), _v); cc_logic32(ctx, _v); hle_insn(ctx, 16); } /* 012700 movel %a5,%fp@(8) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012704 movew %fp@(2),%d2 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012708 movew %fp@(4),%d3 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 01270C bsrw 0x13228 */
L_012710:;
	{ ctx->a[6] = (ctx->a[6] + (uint32_t)(int32_t)18); hle_insn(ctx, 8); } /* 012710 lea %fp@(18),%fp */
	{ uint16_t _c = (uint16_t)ctx->d[7] - 1; set_w(&ctx->d[7], _c); if (_c != 0xFFFF) { hle_insn(ctx, 10); goto L_0126C4; } hle_insn(ctx, 14); } /* 012714 dbf %d7,0x126c4 */
L_012718:;
	hle_insn(ctx, 16); return; /* 012718 rts */
L_01271A:;
	{ ctx->a[6] = (ctx->a[6] + (uint32_t)(int32_t)18); hle_insn(ctx, 8); } /* 01271A lea %fp@(18),%fp */
	hle_insn(ctx, 10); goto L_0126C4; /* 01271E bras 0x126c4 */
L_012720:;
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 012720 clrw %fp@ */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DCu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 012722 subqw #1,0xffff80dc */
	hle_insn(ctx, 10); goto L_012710; /* 012728 bras 0x12710 */
}

/* $012886 */
HLE_FUNC_PURE(0x012886, DebrisRun, 4000)
{
	{ ctx->a[6] = 0xFFFF97B2u; hle_insn(ctx, 12); } /* 012886 lea 0xffff97b2,%fp */
	{ uint32_t _v = ld16(0xFFFF80DEu); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 01288C movew 0xffff80de,%d7 */
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[7] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[7], _r); hle_insn(ctx, 4); } /* 012892 subqw #1,%d7 */
	if (bs(ctx, c_n(ctx))) goto L_0128FC; /* 012894 bmis 0x128fc */
L_012896:;
	{ uint32_t _v = ld16(ctx->a[6]); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 012896 movew %fp@,%d0 */
	if (bs(ctx, c_z(ctx))) goto L_0128FE; /* 012898 beqs 0x128fe */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)14)); set_w(&ctx->d[4], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01289A movew %fp@(14),%d4 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)16)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01289E movew %fp@(16),%d2 */
	{ uint32_t _s = (ctx->d[4] & 0xFFFFu); uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 4); } /* 0128A2 addw %d4,%d2 */
	{ uint32_t _v = (ctx->d[2] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)16), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0128A4 movew %d2,%fp@(16) */
	{ sh_w(ctx, 2, 'a', 6); hle_insn(ctx, 18); } /* 0128A8 asrw #6,%d2 */
	{ uint32_t _s = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 12); } /* 0128AA addw %fp@(2),%d2 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)18)); set_w(&ctx->d[4], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0128AE movew %fp@(18),%d4 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)20)); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0128B2 movew %fp@(20),%d3 */
	{ uint32_t _s = (ctx->d[4] & 0xFFFFu); uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[3], _r); hle_insn(ctx, 4); } /* 0128B6 addw %d4,%d3 */
	{ uint32_t _v = (ctx->d[3] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)20), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0128B8 movew %d3,%fp@(20) */
	{ sh_w(ctx, 3, 'a', 6); hle_insn(ctx, 18); } /* 0128BC asrw #6,%d3 */
	{ uint32_t _s = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[3], _r); hle_insn(ctx, 12); } /* 0128BE addw %fp@(4),%d3 */
	{ uint32_t _s = ld16((ctx->a[6] + (uint32_t)(int32_t)22)); uint32_t _d = (ctx->d[4] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[4], _r); hle_insn(ctx, 12); } /* 0128C2 addw %fp@(22),%d4 */
	{ uint32_t _v = (ctx->d[4] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)18), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0128C6 movew %d4,%fp@(18) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)12)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0128CA movew %fp@(12),%d1 */
	{ uint32_t _v = ld32((ctx->a[6] + (uint32_t)(int32_t)8)); ctx->a[5] = _v; hle_insn(ctx, 16); } /* 0128CE moveal %fp@(8),%a5 */
	{ uint32_t _s = 0x18u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 0128D2 andiw #24,%d1 */
	{ sh_w(ctx, 1, 'r', 3); hle_insn(ctx, 12); } /* 0128D6 lsrw #3,%d1 */
	{ uint32_t _s = (ctx->d[1] & 0xFFFFu); uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 4); } /* 0128D8 addw %d1,%d1 */
	{ uint32_t _v = ld16((ctx->a[5] + (uint32_t)(int32_t)0 + (uint32_t)((int32_t)(int16_t)ctx->d[1]))); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 14); } /* 0128DA movew %a5@(0,%d1:w),%d1 */
	{ ctx->d[5] = 0x0u; cc_logic32(ctx, ctx->d[5]); hle_insn(ctx, 4); } /* 0128DE moveq #0,%d5 */
	{ uint32_t _v = ld8((ctx->a[6] + (uint32_t)(int32_t)7)); set_b(&ctx->d[5], _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 0128E0 moveb %fp@(7),%d5 */
	{ hle_insn(ctx, 18); QueueSprite(ctx); } /* 0128E4 bsrw 0x13240 */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)12); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 0128E8 addqw #1,%fp@(12) */
	{ uint32_t _s = 0x24u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)12); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 0128EC cmpiw #36,%fp@(12) */
	if (bs(ctx, c_z(ctx))) goto L_012904; /* 0128F2 beqs 0x12904 */
L_0128F4:;
	{ ctx->a[6] = (ctx->a[6] + (uint32_t)(int32_t)24); hle_insn(ctx, 8); } /* 0128F4 lea %fp@(24),%fp */
	{ uint16_t _c = (uint16_t)ctx->d[7] - 1; set_w(&ctx->d[7], _c); if (_c != 0xFFFF) { hle_insn(ctx, 10); goto L_012896; } hle_insn(ctx, 14); } /* 0128F8 dbf %d7,0x12896 */
L_0128FC:;
	hle_insn(ctx, 16); return; /* 0128FC rts */
L_0128FE:;
	{ ctx->a[6] = (ctx->a[6] + (uint32_t)(int32_t)24); hle_insn(ctx, 8); } /* 0128FE lea %fp@(24),%fp */
	hle_insn(ctx, 10); goto L_012896; /* 012902 bras 0x12896 */
L_012904:;
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 012904 clrw %fp@ */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DEu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 012906 subqw #1,0xffff80de */
	hle_insn(ctx, 10); goto L_0128F4; /* 01290C bras 0x128f4 */
}

/* $012352 */
HLE_FUNC_PURE(0x012352, BombStart, 4000)
{
	{ hle_insn(ctx, 18); sub_01237A(ctx); } /* 012352 bsrw 0x1237a */
	{ uint32_t _v = 0xC348u; st32((ctx->a[4] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 012356 movel #49992,%a4@(32) */
	{ uint32_t _v = 0x6004u; st32((ctx->a[4] + (uint32_t)(int32_t)28), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 01235E movel #24580,%a4@(28) */
	{ uint32_t _v = 0x20u; st16((ctx->a[4] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 012366 movew #32,%a4@(36) */
	{ uint32_t _s = 0x8u; uint32_t _da = (ctx->a[4] + (uint32_t)(int32_t)4); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 01236C addiw #8,%a4@(4) */
	{ uint32_t _v = 0x6u; st8((ctx->a[4] + (uint32_t)(int32_t)18), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 012372 moveb #6,%a4@(18) */
	hle_insn(ctx, 16); return; /* 012378 rts */
}

/* $01248C */
HLE_FUNC_PURE(0x01248C, SparkSpawn269A, 4000)
{
	{ ctx->a[2] = 0x01269Au; hle_insn(ctx, 8); } /* 01248C lea %pc@(0x1269a),%a2 */
	hle_insn(ctx, 10); goto L_01257A; /* 012490 braw 0x1257a */
L_01257A:;
	{ hle_insn(ctx, 18); sub_012926(ctx); } /* 01257A jsr %pc@(0x12926) */
	{ cc_logic16(ctx, (ctx->d[6] & 0xFFFFu)); hle_insn(ctx, 4); } /* 01257E tstw %d6 */
	if (bs(ctx, c_n(ctx))) goto L_0125B4; /* 012580 bmis 0x125b4 */
	{ uint32_t _v = ctx->a[2]; st32((ctx->a[5] + (uint32_t)(int32_t)8), _v); cc_logic32(ctx, _v); hle_insn(ctx, 16); } /* 012582 movel %a2,%a5@(8) */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)2), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012586 movew %d0,%a5@(2) */
	{ uint32_t _v = (ctx->d[1] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)4), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01258A movew %d1,%a5@(4) */
	{ uint32_t _v = (ctx->d[2] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)6), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01258E movew %d2,%a5@(6) */
	{ uint32_t _v = 0x1u; st16(ctx->a[5], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012592 movew #1,%a5@ */
	{ st8((ctx->a[5] + (uint32_t)(int32_t)12), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 012596 clrb %a5@(12) */
	{ st8((ctx->a[5] + (uint32_t)(int32_t)13), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 01259A clrb %a5@(13) */
	{ st8((ctx->a[5] + (uint32_t)(int32_t)16), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 01259E clrb %a5@(16) */
	{ st8((ctx->a[5] + (uint32_t)(int32_t)14), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 0125A2 clrb %a5@(14) */
	{ st8((ctx->a[5] + (uint32_t)(int32_t)15), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 0125A6 clrb %a5@(15) */
	{ st8((ctx->a[5] + (uint32_t)(int32_t)17), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 0125AA clrb %a5@(17) */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DCu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 0125AE addqw #1,0xffff80dc */
L_0125B4:;
	hle_insn(ctx, 16); return; /* 0125B4 rts */
}

/* $0124B8 */
HLE_FUNC_PURE(0x0124B8, SparkRand5B6, 4000)
{
	{ ctx->a[2] = 0x0125B6u; hle_insn(ctx, 8); } /* 0124B8 lea %pc@(0x125b6),%a2 */
	{ hle_insn(ctx, 18); sub_01257A(ctx); } /* 0124BC bsrw 0x1257a */
	{ hle_insn(ctx, 18); sub_012502(ctx); } /* 0124C0 bsrw 0x12502 */
	{ uint32_t _s = 0x80u; uint32_t _da = (ctx->a[5] + (uint32_t)(int32_t)14); uint32_t _d = ld8(_da); uint32_t _r = (_d | _s) & 0xFFu; cc_logic8(ctx, _r); st8(_da, _r); hle_insn(ctx, 20); } /* 0124C4 orib #-128,%a5@(14) */
	{ uint32_t _s = 0x80u; uint32_t _da = (ctx->a[5] + (uint32_t)(int32_t)12); uint32_t _d = ld8(_da); uint32_t _r = (_d | _s) & 0xFFu; cc_logic8(ctx, _r); st8(_da, _r); hle_insn(ctx, 20); } /* 0124CA orib #-128,%a5@(12) */
	hle_insn(ctx, 16); return; /* 0124D0 rts */
}

/* $0124D2 */
HLE_FUNC_PURE(0x0124D2, SparkRand618, 4000)
{
	{ ctx->a[2] = 0x012618u; hle_insn(ctx, 8); } /* 0124D2 lea %pc@(0x12618),%a2 */
	{ hle_insn(ctx, 18); sub_01257A(ctx); } /* 0124D6 bsrw 0x1257a */
	{ hle_insn(ctx, 18); sub_012502(ctx); } /* 0124DA bsrw 0x12502 */
	{ uint32_t _s = 0x80u; uint32_t _da = (ctx->a[5] + (uint32_t)(int32_t)14); uint32_t _d = ld8(_da); uint32_t _r = (_d | _s) & 0xFFu; cc_logic8(ctx, _r); st8(_da, _r); hle_insn(ctx, 20); } /* 0124DE orib #-128,%a5@(14) */
	{ uint32_t _s = 0x80u; uint32_t _da = (ctx->a[5] + (uint32_t)(int32_t)12); uint32_t _d = ld8(_da); uint32_t _r = (_d | _s) & 0xFFu; cc_logic8(ctx, _r); st8(_da, _r); hle_insn(ctx, 20); } /* 0124E4 orib #-128,%a5@(12) */
	hle_insn(ctx, 16); return; /* 0124EA rts */
}

/* $012560 */
HLE_FUNC_PURE(0x012560, SparkSpawn618, 4000)
{
	{ ctx->a[2] = 0x012618u; hle_insn(ctx, 8); } /* 012560 lea %pc@(0x12618),%a2 */
	hle_insn(ctx, 10); goto L_01257A; /* 012564 braw 0x1257a */
L_01257A:;
	{ hle_insn(ctx, 18); sub_012926(ctx); } /* 01257A jsr %pc@(0x12926) */
	{ cc_logic16(ctx, (ctx->d[6] & 0xFFFFu)); hle_insn(ctx, 4); } /* 01257E tstw %d6 */
	if (bs(ctx, c_n(ctx))) goto L_0125B4; /* 012580 bmis 0x125b4 */
	{ uint32_t _v = ctx->a[2]; st32((ctx->a[5] + (uint32_t)(int32_t)8), _v); cc_logic32(ctx, _v); hle_insn(ctx, 16); } /* 012582 movel %a2,%a5@(8) */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)2), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012586 movew %d0,%a5@(2) */
	{ uint32_t _v = (ctx->d[1] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)4), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01258A movew %d1,%a5@(4) */
	{ uint32_t _v = (ctx->d[2] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)6), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01258E movew %d2,%a5@(6) */
	{ uint32_t _v = 0x1u; st16(ctx->a[5], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012592 movew #1,%a5@ */
	{ st8((ctx->a[5] + (uint32_t)(int32_t)12), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 012596 clrb %a5@(12) */
	{ st8((ctx->a[5] + (uint32_t)(int32_t)13), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 01259A clrb %a5@(13) */
	{ st8((ctx->a[5] + (uint32_t)(int32_t)16), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 01259E clrb %a5@(16) */
	{ st8((ctx->a[5] + (uint32_t)(int32_t)14), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 0125A2 clrb %a5@(14) */
	{ st8((ctx->a[5] + (uint32_t)(int32_t)15), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 0125A6 clrb %a5@(15) */
	{ st8((ctx->a[5] + (uint32_t)(int32_t)17), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 0125AA clrb %a5@(17) */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DCu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 0125AE addqw #1,0xffff80dc */
L_0125B4:;
	hle_insn(ctx, 16); return; /* 0125B4 rts */
}

/* $01272A */
HLE_FUNC_PURE(0x01272A, DebrisTriple, 4000)
{
	{ ctx->a[2] = 0x012826u; hle_insn(ctx, 8); } /* 01272A lea %pc@(0x12826),%a2 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[4], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 01272E movew %d0,%d4 */
	{ uint32_t _v = (ctx->d[1] & 0xFFFFu); set_w(&ctx->d[5], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 012730 movew %d1,%d5 */
	{ hle_insn(ctx, 18); sub_01277C(ctx); } /* 012732 bsrw 0x1277c */
	{ uint32_t _v = (ctx->d[4] & 0xFFFFu); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 012736 movew %d4,%d0 */
	{ uint32_t _v = (ctx->d[5] & 0xFFFFu); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 012738 movew %d5,%d1 */
	{ ctx->a[2] = 0x012826u; hle_insn(ctx, 8); } /* 01273A lea %pc@(0x12826),%a2 */
	{ hle_insn(ctx, 18); sub_01277C(ctx); } /* 01273E bsrw 0x1277c */
	{ uint32_t _v = (ctx->d[4] & 0xFFFFu); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 012742 movew %d4,%d0 */
	{ uint32_t _v = (ctx->d[5] & 0xFFFFu); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 012744 movew %d5,%d1 */
	{ ctx->a[2] = 0x01282Eu; hle_insn(ctx, 8); } /* 012746 lea %pc@(0x1282e),%a2 */
	hle_insn(ctx, 10); goto L_01277C; /* 01274A braw 0x1277c */
L_01277C:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)6)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01277C movew %fp@(6),%d2 */
	{ hle_insn(ctx, 18); sub_012956(ctx); } /* 012780 jsr %pc@(0x12956) */
	{ cc_logic16(ctx, (ctx->d[6] & 0xFFFFu)); hle_insn(ctx, 4); } /* 012784 tstw %d6 */
	if (bs(ctx, c_n(ctx))) goto L_0127F4; /* 012786 bmis 0x127f4 */
	{ uint32_t _v = 0x1u; st16(ctx->a[5], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012788 movew #1,%a5@ */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)2), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01278C movew %d0,%a5@(2) */
	{ uint32_t _v = (ctx->d[1] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)4), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012790 movew %d1,%a5@(4) */
	{ uint32_t _v = (ctx->d[2] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)6), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012794 movew %d2,%a5@(6) */
	{ st16((ctx->a[5] + (uint32_t)(int32_t)16), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 012798 clrw %a5@(16) */
	{ st16((ctx->a[5] + (uint32_t)(int32_t)20), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 01279C clrw %a5@(20) */
	{ hle_insn(ctx, 20); Random(ctx); } /* 0127A0 jsr 0x881c */
	{ uint32_t _s = 0x3u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127A6 andiw #3,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)12), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0127AA movew %d0,%a5@(12) */
	{ hle_insn(ctx, 20); Random(ctx); } /* 0127AE jsr 0x881c */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 0127B4 movew %d0,%d1 */
	{ uint32_t _s = 0xFFu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127B6 andiw #255,%d0 */
	{ cc_logic16(ctx, (ctx->d[1] & 0xFFFFu)); hle_insn(ctx, 4); } /* 0127BA tstw %d1 */
	if (bs(ctx, !c_n(ctx))) goto L_0127C0; /* 0127BC bpls 0x127c0 */
	{ uint32_t _r = alu(ctx, OP_SUB, 0, ctx->d[0], 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 0127BE negw %d0 */
L_0127C0:;
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)14), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0127C0 movew %d0,%a5@(14) */
	{ hle_insn(ctx, 20); Random(ctx); } /* 0127C4 jsr 0x881c */
	{ uint32_t _s = 0xF0u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127CA andiw #240,%d0 */
	{ uint32_t _s = 0x100u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d | _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127CE oriw #256,%d0 */
	{ uint32_t _r = alu(ctx, OP_SUB, 0, ctx->d[0], 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 0127D2 negw %d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)18), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0127D4 movew %d0,%a5@(18) */
	{ hle_insn(ctx, 20); Random(ctx); } /* 0127D8 jsr 0x881c */
	{ uint32_t _s = 0x7u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127DE andiw #7,%d0 */
	{ uint32_t _s = 0x18u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d | _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127E2 oriw #24,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)22), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0127E6 movew %d0,%a5@(22) */
	{ uint32_t _v = ctx->a[2]; st32((ctx->a[5] + (uint32_t)(int32_t)8), _v); cc_logic32(ctx, _v); hle_insn(ctx, 16); } /* 0127EA movel %a2,%a5@(8) */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DEu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 0127EE addqw #1,0xffff80de */
L_0127F4:;
	hle_insn(ctx, 16); return; /* 0127F4 rts */
}

/* $01274E */
HLE_FUNC_PURE(0x01274E, DebrisSpawn281E, 4000)
{
	{ ctx->a[2] = 0x01281Eu; hle_insn(ctx, 8); } /* 01274E lea %pc@(0x1281e),%a2 */
	hle_insn(ctx, 10); goto L_01277C; /* 012752 bras 0x1277c */
L_01277C:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)6)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01277C movew %fp@(6),%d2 */
	{ hle_insn(ctx, 18); sub_012956(ctx); } /* 012780 jsr %pc@(0x12956) */
	{ cc_logic16(ctx, (ctx->d[6] & 0xFFFFu)); hle_insn(ctx, 4); } /* 012784 tstw %d6 */
	if (bs(ctx, c_n(ctx))) goto L_0127F4; /* 012786 bmis 0x127f4 */
	{ uint32_t _v = 0x1u; st16(ctx->a[5], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012788 movew #1,%a5@ */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)2), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01278C movew %d0,%a5@(2) */
	{ uint32_t _v = (ctx->d[1] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)4), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012790 movew %d1,%a5@(4) */
	{ uint32_t _v = (ctx->d[2] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)6), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012794 movew %d2,%a5@(6) */
	{ st16((ctx->a[5] + (uint32_t)(int32_t)16), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 012798 clrw %a5@(16) */
	{ st16((ctx->a[5] + (uint32_t)(int32_t)20), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 01279C clrw %a5@(20) */
	{ hle_insn(ctx, 20); Random(ctx); } /* 0127A0 jsr 0x881c */
	{ uint32_t _s = 0x3u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127A6 andiw #3,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)12), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0127AA movew %d0,%a5@(12) */
	{ hle_insn(ctx, 20); Random(ctx); } /* 0127AE jsr 0x881c */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 0127B4 movew %d0,%d1 */
	{ uint32_t _s = 0xFFu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127B6 andiw #255,%d0 */
	{ cc_logic16(ctx, (ctx->d[1] & 0xFFFFu)); hle_insn(ctx, 4); } /* 0127BA tstw %d1 */
	if (bs(ctx, !c_n(ctx))) goto L_0127C0; /* 0127BC bpls 0x127c0 */
	{ uint32_t _r = alu(ctx, OP_SUB, 0, ctx->d[0], 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 0127BE negw %d0 */
L_0127C0:;
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)14), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0127C0 movew %d0,%a5@(14) */
	{ hle_insn(ctx, 20); Random(ctx); } /* 0127C4 jsr 0x881c */
	{ uint32_t _s = 0xF0u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127CA andiw #240,%d0 */
	{ uint32_t _s = 0x100u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d | _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127CE oriw #256,%d0 */
	{ uint32_t _r = alu(ctx, OP_SUB, 0, ctx->d[0], 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 0127D2 negw %d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)18), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0127D4 movew %d0,%a5@(18) */
	{ hle_insn(ctx, 20); Random(ctx); } /* 0127D8 jsr 0x881c */
	{ uint32_t _s = 0x7u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127DE andiw #7,%d0 */
	{ uint32_t _s = 0x18u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d | _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127E2 oriw #24,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)22), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0127E6 movew %d0,%a5@(22) */
	{ uint32_t _v = ctx->a[2]; st32((ctx->a[5] + (uint32_t)(int32_t)8), _v); cc_logic32(ctx, _v); hle_insn(ctx, 16); } /* 0127EA movel %a2,%a5@(8) */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DEu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 0127EE addqw #1,0xffff80de */
L_0127F4:;
	hle_insn(ctx, 16); return; /* 0127F4 rts */
}

/* $012754 */
HLE_FUNC_PURE(0x012754, DebrisSpawn2816, 4000)
{
	{ ctx->a[2] = 0x012816u; hle_insn(ctx, 8); } /* 012754 lea %pc@(0x12816),%a2 */
	hle_insn(ctx, 10); goto L_01277C; /* 012758 bras 0x1277c */
L_01277C:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)6)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01277C movew %fp@(6),%d2 */
	{ hle_insn(ctx, 18); sub_012956(ctx); } /* 012780 jsr %pc@(0x12956) */
	{ cc_logic16(ctx, (ctx->d[6] & 0xFFFFu)); hle_insn(ctx, 4); } /* 012784 tstw %d6 */
	if (bs(ctx, c_n(ctx))) goto L_0127F4; /* 012786 bmis 0x127f4 */
	{ uint32_t _v = 0x1u; st16(ctx->a[5], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012788 movew #1,%a5@ */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)2), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01278C movew %d0,%a5@(2) */
	{ uint32_t _v = (ctx->d[1] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)4), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012790 movew %d1,%a5@(4) */
	{ uint32_t _v = (ctx->d[2] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)6), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012794 movew %d2,%a5@(6) */
	{ st16((ctx->a[5] + (uint32_t)(int32_t)16), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 012798 clrw %a5@(16) */
	{ st16((ctx->a[5] + (uint32_t)(int32_t)20), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 01279C clrw %a5@(20) */
	{ hle_insn(ctx, 20); Random(ctx); } /* 0127A0 jsr 0x881c */
	{ uint32_t _s = 0x3u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127A6 andiw #3,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)12), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0127AA movew %d0,%a5@(12) */
	{ hle_insn(ctx, 20); Random(ctx); } /* 0127AE jsr 0x881c */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 0127B4 movew %d0,%d1 */
	{ uint32_t _s = 0xFFu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127B6 andiw #255,%d0 */
	{ cc_logic16(ctx, (ctx->d[1] & 0xFFFFu)); hle_insn(ctx, 4); } /* 0127BA tstw %d1 */
	if (bs(ctx, !c_n(ctx))) goto L_0127C0; /* 0127BC bpls 0x127c0 */
	{ uint32_t _r = alu(ctx, OP_SUB, 0, ctx->d[0], 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 0127BE negw %d0 */
L_0127C0:;
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)14), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0127C0 movew %d0,%a5@(14) */
	{ hle_insn(ctx, 20); Random(ctx); } /* 0127C4 jsr 0x881c */
	{ uint32_t _s = 0xF0u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127CA andiw #240,%d0 */
	{ uint32_t _s = 0x100u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d | _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127CE oriw #256,%d0 */
	{ uint32_t _r = alu(ctx, OP_SUB, 0, ctx->d[0], 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 0127D2 negw %d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)18), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0127D4 movew %d0,%a5@(18) */
	{ hle_insn(ctx, 20); Random(ctx); } /* 0127D8 jsr 0x881c */
	{ uint32_t _s = 0x7u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127DE andiw #7,%d0 */
	{ uint32_t _s = 0x18u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d | _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127E2 oriw #24,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)22), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0127E6 movew %d0,%a5@(22) */
	{ uint32_t _v = ctx->a[2]; st32((ctx->a[5] + (uint32_t)(int32_t)8), _v); cc_logic32(ctx, _v); hle_insn(ctx, 16); } /* 0127EA movel %a2,%a5@(8) */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DEu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 0127EE addqw #1,0xffff80de */
L_0127F4:;
	hle_insn(ctx, 16); return; /* 0127F4 rts */
}

/* $01275A */
HLE_FUNC_PURE(0x01275A, DebrisSpawn280E, 4000)
{
	{ ctx->a[2] = 0x01280Eu; hle_insn(ctx, 8); } /* 01275A lea %pc@(0x1280e),%a2 */
	{ uint32_t _a = (ctx->a[4] + (uint32_t)(int32_t)4); ctx->d[0] = (uint32_t)(int32_t)(int16_t)ld16(_a + 0); ctx->d[1] = (uint32_t)(int32_t)(int16_t)ld16(_a + 2); hle_insn(ctx, 24); } /* 01275E movemw %a4@(4),%d0-%d1 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)6)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012764 movew %fp@(6),%d2 */
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 4); } /* 012768 subqw #1,%d2 */
	hle_insn(ctx, 10); goto L_012780; /* 01276A bras 0x12780 */
L_012780:;
	{ hle_insn(ctx, 18); sub_012956(ctx); } /* 012780 jsr %pc@(0x12956) */
	{ cc_logic16(ctx, (ctx->d[6] & 0xFFFFu)); hle_insn(ctx, 4); } /* 012784 tstw %d6 */
	if (bs(ctx, c_n(ctx))) goto L_0127F4; /* 012786 bmis 0x127f4 */
	{ uint32_t _v = 0x1u; st16(ctx->a[5], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012788 movew #1,%a5@ */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)2), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01278C movew %d0,%a5@(2) */
	{ uint32_t _v = (ctx->d[1] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)4), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012790 movew %d1,%a5@(4) */
	{ uint32_t _v = (ctx->d[2] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)6), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012794 movew %d2,%a5@(6) */
	{ st16((ctx->a[5] + (uint32_t)(int32_t)16), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 012798 clrw %a5@(16) */
	{ st16((ctx->a[5] + (uint32_t)(int32_t)20), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 01279C clrw %a5@(20) */
	{ hle_insn(ctx, 20); Random(ctx); } /* 0127A0 jsr 0x881c */
	{ uint32_t _s = 0x3u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127A6 andiw #3,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)12), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0127AA movew %d0,%a5@(12) */
	{ hle_insn(ctx, 20); Random(ctx); } /* 0127AE jsr 0x881c */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 0127B4 movew %d0,%d1 */
	{ uint32_t _s = 0xFFu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127B6 andiw #255,%d0 */
	{ cc_logic16(ctx, (ctx->d[1] & 0xFFFFu)); hle_insn(ctx, 4); } /* 0127BA tstw %d1 */
	if (bs(ctx, !c_n(ctx))) goto L_0127C0; /* 0127BC bpls 0x127c0 */
	{ uint32_t _r = alu(ctx, OP_SUB, 0, ctx->d[0], 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 0127BE negw %d0 */
L_0127C0:;
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)14), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0127C0 movew %d0,%a5@(14) */
	{ hle_insn(ctx, 20); Random(ctx); } /* 0127C4 jsr 0x881c */
	{ uint32_t _s = 0xF0u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127CA andiw #240,%d0 */
	{ uint32_t _s = 0x100u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d | _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127CE oriw #256,%d0 */
	{ uint32_t _r = alu(ctx, OP_SUB, 0, ctx->d[0], 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 0127D2 negw %d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)18), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0127D4 movew %d0,%a5@(18) */
	{ hle_insn(ctx, 20); Random(ctx); } /* 0127D8 jsr 0x881c */
	{ uint32_t _s = 0x7u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127DE andiw #7,%d0 */
	{ uint32_t _s = 0x18u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d | _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127E2 oriw #24,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)22), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0127E6 movew %d0,%a5@(22) */
	{ uint32_t _v = ctx->a[2]; st32((ctx->a[5] + (uint32_t)(int32_t)8), _v); cc_logic32(ctx, _v); hle_insn(ctx, 16); } /* 0127EA movel %a2,%a5@(8) */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DEu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 0127EE addqw #1,0xffff80de */
L_0127F4:;
	hle_insn(ctx, 16); return; /* 0127F4 rts */
}

/* $01276C */
HLE_FUNC_PURE(0x01276C, DebrisSpawn2806, 4000)
{
	{ ctx->a[2] = 0x012806u; hle_insn(ctx, 8); } /* 01276C lea %pc@(0x12806),%a2 */
	hle_insn(ctx, 10); goto L_01277C; /* 012770 bras 0x1277c */
L_01277C:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)6)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01277C movew %fp@(6),%d2 */
	{ hle_insn(ctx, 18); sub_012956(ctx); } /* 012780 jsr %pc@(0x12956) */
	{ cc_logic16(ctx, (ctx->d[6] & 0xFFFFu)); hle_insn(ctx, 4); } /* 012784 tstw %d6 */
	if (bs(ctx, c_n(ctx))) goto L_0127F4; /* 012786 bmis 0x127f4 */
	{ uint32_t _v = 0x1u; st16(ctx->a[5], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012788 movew #1,%a5@ */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)2), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01278C movew %d0,%a5@(2) */
	{ uint32_t _v = (ctx->d[1] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)4), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012790 movew %d1,%a5@(4) */
	{ uint32_t _v = (ctx->d[2] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)6), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012794 movew %d2,%a5@(6) */
	{ st16((ctx->a[5] + (uint32_t)(int32_t)16), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 012798 clrw %a5@(16) */
	{ st16((ctx->a[5] + (uint32_t)(int32_t)20), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 01279C clrw %a5@(20) */
	{ hle_insn(ctx, 20); Random(ctx); } /* 0127A0 jsr 0x881c */
	{ uint32_t _s = 0x3u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127A6 andiw #3,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)12), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0127AA movew %d0,%a5@(12) */
	{ hle_insn(ctx, 20); Random(ctx); } /* 0127AE jsr 0x881c */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 0127B4 movew %d0,%d1 */
	{ uint32_t _s = 0xFFu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127B6 andiw #255,%d0 */
	{ cc_logic16(ctx, (ctx->d[1] & 0xFFFFu)); hle_insn(ctx, 4); } /* 0127BA tstw %d1 */
	if (bs(ctx, !c_n(ctx))) goto L_0127C0; /* 0127BC bpls 0x127c0 */
	{ uint32_t _r = alu(ctx, OP_SUB, 0, ctx->d[0], 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 0127BE negw %d0 */
L_0127C0:;
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)14), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0127C0 movew %d0,%a5@(14) */
	{ hle_insn(ctx, 20); Random(ctx); } /* 0127C4 jsr 0x881c */
	{ uint32_t _s = 0xF0u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127CA andiw #240,%d0 */
	{ uint32_t _s = 0x100u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d | _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127CE oriw #256,%d0 */
	{ uint32_t _r = alu(ctx, OP_SUB, 0, ctx->d[0], 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 0127D2 negw %d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)18), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0127D4 movew %d0,%a5@(18) */
	{ hle_insn(ctx, 20); Random(ctx); } /* 0127D8 jsr 0x881c */
	{ uint32_t _s = 0x7u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127DE andiw #7,%d0 */
	{ uint32_t _s = 0x18u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d | _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127E2 oriw #24,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)22), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0127E6 movew %d0,%a5@(22) */
	{ uint32_t _v = ctx->a[2]; st32((ctx->a[5] + (uint32_t)(int32_t)8), _v); cc_logic32(ctx, _v); hle_insn(ctx, 16); } /* 0127EA movel %a2,%a5@(8) */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DEu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 0127EE addqw #1,0xffff80de */
L_0127F4:;
	hle_insn(ctx, 16); return; /* 0127F4 rts */
}

/* $012772 */
HLE_FUNC_PURE(0x012772, DebrisSpawn27FE, 4000)
{
	{ ctx->a[2] = 0x0127FEu; hle_insn(ctx, 8); } /* 012772 lea %pc@(0x127fe),%a2 */
	hle_insn(ctx, 10); goto L_01277C; /* 012776 bras 0x1277c */
L_01277C:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)6)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01277C movew %fp@(6),%d2 */
	{ hle_insn(ctx, 18); sub_012956(ctx); } /* 012780 jsr %pc@(0x12956) */
	{ cc_logic16(ctx, (ctx->d[6] & 0xFFFFu)); hle_insn(ctx, 4); } /* 012784 tstw %d6 */
	if (bs(ctx, c_n(ctx))) goto L_0127F4; /* 012786 bmis 0x127f4 */
	{ uint32_t _v = 0x1u; st16(ctx->a[5], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012788 movew #1,%a5@ */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)2), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01278C movew %d0,%a5@(2) */
	{ uint32_t _v = (ctx->d[1] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)4), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012790 movew %d1,%a5@(4) */
	{ uint32_t _v = (ctx->d[2] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)6), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012794 movew %d2,%a5@(6) */
	{ st16((ctx->a[5] + (uint32_t)(int32_t)16), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 012798 clrw %a5@(16) */
	{ st16((ctx->a[5] + (uint32_t)(int32_t)20), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 01279C clrw %a5@(20) */
	{ hle_insn(ctx, 20); Random(ctx); } /* 0127A0 jsr 0x881c */
	{ uint32_t _s = 0x3u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127A6 andiw #3,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)12), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0127AA movew %d0,%a5@(12) */
	{ hle_insn(ctx, 20); Random(ctx); } /* 0127AE jsr 0x881c */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 0127B4 movew %d0,%d1 */
	{ uint32_t _s = 0xFFu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127B6 andiw #255,%d0 */
	{ cc_logic16(ctx, (ctx->d[1] & 0xFFFFu)); hle_insn(ctx, 4); } /* 0127BA tstw %d1 */
	if (bs(ctx, !c_n(ctx))) goto L_0127C0; /* 0127BC bpls 0x127c0 */
	{ uint32_t _r = alu(ctx, OP_SUB, 0, ctx->d[0], 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 0127BE negw %d0 */
L_0127C0:;
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)14), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0127C0 movew %d0,%a5@(14) */
	{ hle_insn(ctx, 20); Random(ctx); } /* 0127C4 jsr 0x881c */
	{ uint32_t _s = 0xF0u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127CA andiw #240,%d0 */
	{ uint32_t _s = 0x100u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d | _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127CE oriw #256,%d0 */
	{ uint32_t _r = alu(ctx, OP_SUB, 0, ctx->d[0], 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 0127D2 negw %d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)18), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0127D4 movew %d0,%a5@(18) */
	{ hle_insn(ctx, 20); Random(ctx); } /* 0127D8 jsr 0x881c */
	{ uint32_t _s = 0x7u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127DE andiw #7,%d0 */
	{ uint32_t _s = 0x18u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d | _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127E2 oriw #24,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)22), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0127E6 movew %d0,%a5@(22) */
	{ uint32_t _v = ctx->a[2]; st32((ctx->a[5] + (uint32_t)(int32_t)8), _v); cc_logic32(ctx, _v); hle_insn(ctx, 16); } /* 0127EA movel %a2,%a5@(8) */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DEu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 0127EE addqw #1,0xffff80de */
L_0127F4:;
	hle_insn(ctx, 16); return; /* 0127F4 rts */
}

/* $012778 */
HLE_FUNC_PURE(0x012778, DebrisSpawn27F6, 4000)
{
	{ ctx->a[2] = 0x0127F6u; hle_insn(ctx, 8); } /* 012778 lea %pc@(0x127f6),%a2 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)6)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01277C movew %fp@(6),%d2 */
	{ hle_insn(ctx, 18); sub_012956(ctx); } /* 012780 jsr %pc@(0x12956) */
	{ cc_logic16(ctx, (ctx->d[6] & 0xFFFFu)); hle_insn(ctx, 4); } /* 012784 tstw %d6 */
	if (bs(ctx, c_n(ctx))) goto L_0127F4; /* 012786 bmis 0x127f4 */
	{ uint32_t _v = 0x1u; st16(ctx->a[5], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012788 movew #1,%a5@ */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)2), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01278C movew %d0,%a5@(2) */
	{ uint32_t _v = (ctx->d[1] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)4), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012790 movew %d1,%a5@(4) */
	{ uint32_t _v = (ctx->d[2] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)6), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012794 movew %d2,%a5@(6) */
	{ st16((ctx->a[5] + (uint32_t)(int32_t)16), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 012798 clrw %a5@(16) */
	{ st16((ctx->a[5] + (uint32_t)(int32_t)20), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 01279C clrw %a5@(20) */
	{ hle_insn(ctx, 20); Random(ctx); } /* 0127A0 jsr 0x881c */
	{ uint32_t _s = 0x3u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127A6 andiw #3,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)12), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0127AA movew %d0,%a5@(12) */
	{ hle_insn(ctx, 20); Random(ctx); } /* 0127AE jsr 0x881c */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 0127B4 movew %d0,%d1 */
	{ uint32_t _s = 0xFFu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127B6 andiw #255,%d0 */
	{ cc_logic16(ctx, (ctx->d[1] & 0xFFFFu)); hle_insn(ctx, 4); } /* 0127BA tstw %d1 */
	if (bs(ctx, !c_n(ctx))) goto L_0127C0; /* 0127BC bpls 0x127c0 */
	{ uint32_t _r = alu(ctx, OP_SUB, 0, ctx->d[0], 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 0127BE negw %d0 */
L_0127C0:;
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)14), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0127C0 movew %d0,%a5@(14) */
	{ hle_insn(ctx, 20); Random(ctx); } /* 0127C4 jsr 0x881c */
	{ uint32_t _s = 0xF0u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127CA andiw #240,%d0 */
	{ uint32_t _s = 0x100u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d | _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127CE oriw #256,%d0 */
	{ uint32_t _r = alu(ctx, OP_SUB, 0, ctx->d[0], 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 0127D2 negw %d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)18), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0127D4 movew %d0,%a5@(18) */
	{ hle_insn(ctx, 20); Random(ctx); } /* 0127D8 jsr 0x881c */
	{ uint32_t _s = 0x7u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127DE andiw #7,%d0 */
	{ uint32_t _s = 0x18u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d | _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0127E2 oriw #24,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[5] + (uint32_t)(int32_t)22), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0127E6 movew %d0,%a5@(22) */
	{ uint32_t _v = ctx->a[2]; st32((ctx->a[5] + (uint32_t)(int32_t)8), _v); cc_logic32(ctx, _v); hle_insn(ctx, 16); } /* 0127EA movel %a2,%a5@(8) */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DEu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 0127EE addqw #1,0xffff80de */
L_0127F4:;
	hle_insn(ctx, 16); return; /* 0127F4 rts */
}

/* $01290E */
HLE_FUNC_PURE(0x01290E, MissileAlloc, 4000)
{
	{ ctx->a[6] = 0xFFFF80F2u; hle_insn(ctx, 12); } /* 01290E lea 0xffff80f2,%fp */
	{ uint32_t _v = 0x9u; set_w(&ctx->d[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 012914 movew #9,%d6 */
L_012918:;
	{ cc_logic16(ctx, ld16(ctx->a[6])); hle_insn(ctx, 8); } /* 012918 tstw %fp@ */
	if (bs(ctx, c_z(ctx))) goto L_012924; /* 01291A beqs 0x12924 */
	{ ctx->a[6] = (ctx->a[6] + (uint32_t)(int32_t)42); hle_insn(ctx, 8); } /* 01291C lea %fp@(42),%fp */
	{ uint16_t _c = (uint16_t)ctx->d[6] - 1; set_w(&ctx->d[6], _c); if (_c != 0xFFFF) { hle_insn(ctx, 10); goto L_012918; } hle_insn(ctx, 14); } /* 012920 dbf %d6,0x12918 */
L_012924:;
	hle_insn(ctx, 16); return; /* 012924 rts */
}

/* $01296E */
HLE_FUNC_PURE(0x01296E, PickupAlloc, 4000)
{
	{ ctx->a[6] = 0xFFFF9AB2u; hle_insn(ctx, 12); } /* 01296E lea 0xffff9ab2,%fp */
	{ uint32_t _v = 0x1Fu; set_w(&ctx->d[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 012974 movew #31,%d6 */
L_012978:;
	{ cc_logic16(ctx, ld16(ctx->a[6])); hle_insn(ctx, 8); } /* 012978 tstw %fp@ */
	if (bs(ctx, c_z(ctx))) goto L_012984; /* 01297A beqs 0x12984 */
	{ ctx->a[6] = (ctx->a[6] + (uint32_t)(int32_t)26); hle_insn(ctx, 8); } /* 01297C lea %fp@(26),%fp */
	{ uint16_t _c = (uint16_t)ctx->d[6] - 1; set_w(&ctx->d[6], _c); if (_c != 0xFFFF) { hle_insn(ctx, 10); goto L_012978; } hle_insn(ctx, 14); } /* 012980 dbf %d6,0x12978 */
L_012984:;
	hle_insn(ctx, 16); return; /* 012984 rts */
}

/* $012494 */
HLE_RANGE_PURE(0x012494, SparkRand5B6Snd67, 1000, 0x013CC4)
{
	{ ctx->a[2] = 0x0125B6u; hle_insn(ctx, 8); } /* 012494 lea %pc@(0x125b6),%a2 */
	{ hle_insn(ctx, 18); sub_01257A(ctx); } /* 012498 bsrw 0x1257a */
	{ hle_insn(ctx, 18); sub_012502(ctx); } /* 01249C bsrw 0x12502 */
	{ uint32_t _s = 0x80u; uint32_t _da = (ctx->a[5] + (uint32_t)(int32_t)14); uint32_t _d = ld8(_da); uint32_t _r = (_d | _s) & 0xFFu; cc_logic8(ctx, _r); st8(_da, _r); hle_insn(ctx, 20); } /* 0124A0 orib #-128,%a5@(14) */
	{ uint32_t _s = 0x80u; uint32_t _da = (ctx->a[5] + (uint32_t)(int32_t)12); uint32_t _d = ld8(_da); uint32_t _r = (_d | _s) & 0xFFu; cc_logic8(ctx, _r); st8(_da, _r); hle_insn(ctx, 20); } /* 0124A6 orib #-128,%a5@(12) */
	{ uint32_t _v = 0x67u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0124AC movew #103,%d0 */
	hle_insn(ctx, 12); hle_jump(ctx, 0x013CC4); return; /* 0124B0 jmp 0x13cc4 */
}

/* $0124EC */
HLE_RANGE_PURE(0x0124EC, SparkRand65CSnd67, 1000, 0x013CC4)
{
	{ ctx->a[2] = 0x01265Cu; hle_insn(ctx, 8); } /* 0124EC lea %pc@(0x1265c),%a2 */
	{ hle_insn(ctx, 18); sub_01257A(ctx); } /* 0124F0 bsrw 0x1257a */
	{ hle_insn(ctx, 18); sub_012502(ctx); } /* 0124F4 bsrw 0x12502 */
	{ uint32_t _v = 0x67u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0124F8 movew #103,%d0 */
	hle_insn(ctx, 12); hle_jump(ctx, 0x013CC4); return; /* 0124FC jmp 0x13cc4 */
}

/* $01253C */
HLE_RANGE_PURE(0x01253C, SparkSpawn5B6Snd6A, 1000, 0x013CC4)
{
	{ ctx->a[2] = 0x0125B6u; hle_insn(ctx, 8); } /* 01253C lea %pc@(0x125b6),%a2 */
	{ hle_insn(ctx, 18); sub_01257A(ctx); } /* 012540 bsrw 0x1257a */
	{ uint32_t _v = 0x6Au; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 012544 movew #106,%d0 */
	hle_insn(ctx, 12); hle_jump(ctx, 0x013CC4); return; /* 012548 jmp 0x13cc4 */
}

/* $01254E */
HLE_RANGE_PURE(0x01254E, SparkSpawn618Snd68, 1000, 0x013CC4)
{
	{ ctx->a[2] = 0x012618u; hle_insn(ctx, 8); } /* 01254E lea %pc@(0x12618),%a2 */
	{ hle_insn(ctx, 18); sub_01257A(ctx); } /* 012552 bsrw 0x1257a */
	{ uint32_t _v = 0x68u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 012556 movew #104,%d0 */
	hle_insn(ctx, 12); hle_jump(ctx, 0x013CC4); return; /* 01255A jmp 0x13cc4 */
}

/* $012568 */
HLE_RANGE_PURE(0x012568, SparkSpawn65CSnd67, 1000, 0x013CC4)
{
	{ ctx->a[2] = 0x01265Cu; hle_insn(ctx, 8); } /* 012568 lea %pc@(0x1265c),%a2 */
	{ hle_insn(ctx, 18); sub_01257A(ctx); } /* 01256C bsrw 0x1257a */
	{ uint32_t _v = 0x67u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 012570 movew #103,%d0 */
	hle_insn(ctx, 12); hle_jump(ctx, 0x013CC4); return; /* 012574 jmp 0x13cc4 */
}

/* ------------------------------------------------------------------ MissilesRun $0123F0 */
/* state word 1..7 of a missile slot -> the body behind the `bra.w` of the table at $01240C
 * (state 4 and 6 share $012FC2); each body ends in `bra.w sub_012432`, i.e. returns here */
static void (*const msl_state[8])(hle_ctx *) = {
	0, sub_013184, sub_01317E, sub_013178, sub_012FC2, sub_013110, sub_012FC2, sub_0130A2,
};

#define MSL_BOUND 39000   /* T2_HLE_BOUNDS over 92 states x 3000 frames: observed max 25883 x 1.5 */

/* Early bail (docs/porting.md): an empty slot costs ~50 cycles, a live one runs its state body (nested 68k
 * and spawns, up to MSL_BOUND for the whole call). The pass over the slots checks at each slot head
 * ($012402, also an entry: the 68k reaching it after the event hands the rest back to C) and, for a live slot,
 * before the state dispatch ($012406); the 68k continues from there, every register and flag is in ctx.
 * Nothing charged yet = nothing changed: decline. */
#define MSL_BAIL(chunk, pc) do { if (hle_bail_due(ctx, (chunk))) { \
	if (!ctx->tl_used) hle_decline_why(2); else hle_bail_to(ctx, (pc)); return; } } while (0)

static void missiles_run(hle_ctx *ctx, uint32_t entry)
{
	if (entry == 0x012402)
		goto L_slot;
	MSL_BAIL(120, 0x0123F0);
	/* the table has seven entries and every writer stores 1..7; anything else would jump into the
	 * middle of the table in the original, so leave such a frame to the real 68k */
	for (uint32_t p = 0xFFFF80F2u, k = 0; k < 32; k++, p += 0x2A)
		if (ld16(p) > 7) {
			hle_decline();
			return;
		}
	ctx->a[6] = 0xFFFF80F2u; hle_insn(ctx, 12);                      /* 0123F0 lea.l (abs).l,a6 */
	{ uint32_t v = ld16(0xFFFF80D6u); set_w(&ctx->d[7], v); cc_logic16(ctx, v); hle_insn(ctx, 16); }   /* 0123F6 move.w (abs).l,d7 */
	{ uint32_t r = alu(ctx, OP_SUB, ctx->d[7], 1, 16, 1); set_w(&ctx->d[7], r); hle_insn(ctx, 4); }   /* 0123FC subq.w #1,d7 */
	if (bw(ctx, c_n(ctx))) goto L_01243A;                         /* 0123FE bmi.w sub_01243A */
	for (;;) {
		/* sub_012402 */
	L_slot:
		MSL_BAIL(120, 0x012402);
		{ uint32_t v = ld16(ctx->a[6]); set_w(&ctx->d[0], v); cc_logic16(ctx, v); hle_insn(ctx, 8); }   /* 012402 move.w (a6),d0 */
		if (bs(ctx, c_z(ctx))) {                                       /* 012404 beq.s loc_01242C */
			ctx->a[6] += 0x2A; hle_insn(ctx, 8);                   /* 01242C lea.l (0x2a,a6),a6 */
			hle_insn(ctx, 10);                                     /* 012430 bra.s sub_012402 */
			continue;
		}
		MSL_BAIL(MSL_BOUND, 0x012406);
		sh_w(ctx, 0, 'l', 2); hle_insn(ctx, 10);                       /* 012406 asl.w #2,d0 */
		hle_insn(ctx, 14);                                             /* 012408 jmp (sub_01240C,pc,d0.w) */
		hle_insn(ctx, 10);                                             /* bra.w <state> */
		msl_state[(ctx->d[0] & 0xFFFF) >> 2](ctx);
		/* sub_012432 */
		ctx->a[6] += 0x2A; hle_insn(ctx, 8);                           /* 012432 lea.l (0x2a,a6),a6 */
		{ uint16_t c = (uint16_t)ctx->d[7] - 1; set_w(&ctx->d[7], c);  /* 012436 dbra d7,sub_012402 */
		  if (c != 0xFFFF) { hle_insn(ctx, 10); continue; } hle_insn(ctx, 14); }
		break;
	}
L_01243A:;
	hle_insn(ctx, 16);                                                     /* 01243A rts */
}

HLE_FUNC_BAIL(0x0123F0, MissilesRun, MSL_BOUND) { missiles_run(ctx, 0x0123F0); }
HLE_FUNC_BAIL(0x012402, MissilesSlot, MSL_BOUND) { missiles_run(ctx, 0x012402); }
