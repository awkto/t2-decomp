/* Level-7 (freeway chase) object types: the SWAT van ($27, $0108E8/$010906/$010AB4/$010AE0 and the
 * debris pieces $0109C8/$010A92) with its hit test HitSwatVan ($0062B0), and the T-1000 on the police
 * vehicle ($27: $010AF8, $010B84, $010BE4, $010C1C, the crash $010D46/$010DD6/$010DEE, the chase pass
 * $010E4A/$010E82/$010EF6/$010F86 and the scrolling finale $010FB0) with its hit test $006E52.
 * docs/objects_types.md ("Level 7"), src/t2.s.
 *
 * Same scheme as ai_l56.c: every AI state is a range from its entry (reached by the `jmp (a0)` of
 * ObjRunAll) to `bra.w ObjNext` ($00B3BE, reached with hle_jump); C callees (ObjScrollX, ObjAlloc,
 * ObjQueueSprite, QueueSprite, Random, HitBox) are called directly after their jsr is charged; the
 * helper subroutines that only touch work RAM (sub_00D236, sub_00E10C, sub_00C48E, sub_009EF8, ...) are
 * static C functions; every other callee (sounds, damage, VDP queues, ScriptNext, ...) runs as 68k
 * through hle_call68k, so a state that reaches one is a plain HLE_RANGE and the others
 * HLE_RANGE_PURE. Every instruction is on the instruction timeline (GPGX costs); the bodies were
 * produced instruction by instruction from the disassembly (a comment on every line names the 68k
 * instruction). The on-hit callbacks the hit tests hand to ShotHitTest stay 68k. */
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

#define EXIT_NEXT 0x00B3BEu

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

/* mulu.w/muls.w #imm,Dn: 38 + 2n cycles (n = ones in the source / bit transitions) + 4 for the immediate */
static void mul_w(hle_ctx *c, int r, uint16_t src, int sign)
{
	uint32_t res, n = 0;
	if (sign) {
		res = (uint32_t)((int32_t)(int16_t)c->d[r] * (int32_t)(int16_t)src);
		uint32_t t = ((uint32_t)src << 1) & 0x1FFFF;
		for (int k = 0; k < 16; k++)
			if (((t >> k) & 1) != ((t >> (k + 1)) & 1)) n++;
	} else {
		res = (c->d[r] & 0xFFFFu) * (uint32_t)src;
		for (int k = 0; k < 16; k++) n += (src >> k) & 1;
	}
	c->d[r] = res;
	cc_set(c, CC_N | CC_Z | CC_V | CC_C, (res & 0x80000000u ? CC_N : 0) | (res ? 0 : CC_Z));
	hle_insn(c, 38 + 2 * n + 4);
}

static void sub_00D236(hle_ctx *ctx);
static void sub_00E10C(hle_ctx *ctx);
static void sub_00C48E(hle_ctx *ctx);
static void sub_009EF8(hle_ctx *ctx);
static void sub_0109AC(hle_ctx *ctx);
static void sub_011072(hle_ctx *ctx);
static void sub_011146(hle_ctx *ctx);
static void sub_01114C(hle_ctx *ctx);

/* sub_00D236 (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_00D236(hle_ctx *ctx)
{
	{ ctx->d[4] = 0x3u; cc_logic32(ctx, ctx->d[4]); hle_insn(ctx, 4); } /* 00D236 moveq #3,%d4 */
	{ uint32_t _v = ld8((ctx->a[6] + (uint32_t)(int32_t)15)); set_b(&ctx->d[2], _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 00D238 moveb %fp@(15),%d2 */
	{ set_w(&ctx->d[2], (uint32_t)(int32_t)(int8_t)ctx->d[2]); cc_logic16(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 00D23C extw %d2 */
	{ uint32_t _v = ld8((ctx->a[6] + (uint32_t)(int32_t)12)); set_b(&ctx->d[3], _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 00D23E moveb %fp@(12),%d3 */
L_00D242:;
	{ uint32_t _s = (ctx->d[3] & 0xFFu); uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)13); uint32_t _d = ld8(_da); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 8, 1); st8(_da, _r); hle_insn(ctx, 16); } /* 00D242 addb %d3,%fp@(13) */
	if (bs(ctx, !c_c(ctx))) goto L_00D24C; /* 00D246 bccs 0xd24c */
	{ uint32_t _s = (ctx->d[2] & 0xFFFFu); uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)4); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00D248 addw %d2,%fp@(4) */
L_00D24C:;
	{ uint16_t _c = (uint16_t)ctx->d[4] - 1; set_w(&ctx->d[4], _c); if (_c != 0xFFFF) { hle_insn(ctx, 10); goto L_00D242; } hle_insn(ctx, 14); } /* 00D24C dbf %d4,0xd242 */
	{ uint32_t _v = ld8((ctx->a[6] + (uint32_t)(int32_t)18)); set_b(&ctx->d[0], _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 00D250 moveb %fp@(18),%d0 */
	if (bs(ctx, c_n(ctx))) goto L_00D26A; /* 00D254 bmis 0xd26a */
	{ uint32_t _s = (ctx->d[0] & 0xFFu); uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)12); uint32_t _d = ld8(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 8, 1); st8(_da, _r); hle_insn(ctx, 16); } /* 00D256 subb %d0,%fp@(12) */
	if (bs(ctx, !c_c(ctx))) goto L_00D278; /* 00D25A bccs 0xd278 */
	{ uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)15); uint32_t _r = alu(ctx, OP_SUB, 0, ld8(_da), 8, 1); st8(_da, _r); hle_insn(ctx, 16); } /* 00D25C negb %fp@(15) */
	{ uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)18); uint32_t _r = alu(ctx, OP_SUB, 0, ld8(_da), 8, 1); st8(_da, _r); hle_insn(ctx, 16); } /* 00D260 negb %fp@(18) */
	{ st8((ctx->a[6] + (uint32_t)(int32_t)12), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 00D264 clrb %fp@(12) */
	hle_insn(ctx, 10); goto L_00D278; /* 00D268 bras 0xd278 */
L_00D26A:;
	{ uint32_t _s = (ctx->d[0] & 0xFFu); uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)12); uint32_t _d = ld8(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 8, 1); st8(_da, _r); hle_insn(ctx, 16); } /* 00D26A subb %d0,%fp@(12) */
	if (bs(ctx, c_c(ctx))) goto L_00D278; /* 00D26E bcss 0xd278 */
	{ uint32_t _s = (ctx->d[0] & 0xFFu); uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)12); uint32_t _d = ld8(_da); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 8, 1); st8(_da, _r); hle_insn(ctx, 16); } /* 00D270 addb %d0,%fp@(12) */
	{ st8((ctx->a[6] + (uint32_t)(int32_t)18), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 00D274 clrb %fp@(18) */
L_00D278:;
	hle_insn(ctx, 16); return; /* 00D278 rts */
}

/* sub_00E10C (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_00E10C(hle_ctx *ctx)
{
	{ ctx->d[4] = 0x3u; cc_logic32(ctx, ctx->d[4]); hle_insn(ctx, 4); } /* 00E10C moveq #3,%d4 */
	{ uint32_t _v = ld8((ctx->a[6] + (uint32_t)(int32_t)14)); set_b(&ctx->d[2], _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 00E10E moveb %fp@(14),%d2 */
	{ set_w(&ctx->d[2], (uint32_t)(int32_t)(int8_t)ctx->d[2]); cc_logic16(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 00E112 extw %d2 */
	{ uint32_t _v = ld8((ctx->a[6] + (uint32_t)(int32_t)10)); set_b(&ctx->d[3], _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 00E114 moveb %fp@(10),%d3 */
L_00E118:;
	{ uint32_t _s = (ctx->d[3] & 0xFFu); uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)11); uint32_t _d = ld8(_da); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 8, 1); st8(_da, _r); hle_insn(ctx, 16); } /* 00E118 addb %d3,%fp@(11) */
	if (bs(ctx, !c_c(ctx))) goto L_00E122; /* 00E11C bccs 0xe122 */
	{ uint32_t _s = (ctx->d[2] & 0xFFFFu); uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)2); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00E11E addw %d2,%fp@(2) */
L_00E122:;
	{ uint16_t _c = (uint16_t)ctx->d[4] - 1; set_w(&ctx->d[4], _c); if (_c != 0xFFFF) { hle_insn(ctx, 10); goto L_00E118; } hle_insn(ctx, 14); } /* 00E122 dbf %d4,0xe118 */
	{ uint32_t _v = ld8((ctx->a[6] + (uint32_t)(int32_t)16)); set_b(&ctx->d[0], _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 00E126 moveb %fp@(16),%d0 */
	if (bs(ctx, c_n(ctx))) goto L_00E140; /* 00E12A bmis 0xe140 */
	{ uint32_t _s = (ctx->d[0] & 0xFFu); uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)10); uint32_t _d = ld8(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 8, 1); st8(_da, _r); hle_insn(ctx, 16); } /* 00E12C subb %d0,%fp@(10) */
	if (bs(ctx, !c_c(ctx))) goto L_00E144; /* 00E130 bccs 0xe144 */
	{ uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)14); uint32_t _r = alu(ctx, OP_SUB, 0, ld8(_da), 8, 1); st8(_da, _r); hle_insn(ctx, 16); } /* 00E132 negb %fp@(14) */
	{ uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)16); uint32_t _r = alu(ctx, OP_SUB, 0, ld8(_da), 8, 1); st8(_da, _r); hle_insn(ctx, 16); } /* 00E136 negb %fp@(16) */
	{ st8((ctx->a[6] + (uint32_t)(int32_t)10), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 00E13A clrb %fp@(10) */
	hle_insn(ctx, 10); goto L_00E144; /* 00E13E bras 0xe144 */
L_00E140:;
	{ uint32_t _s = (ctx->d[0] & 0xFFu); uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)10); uint32_t _d = ld8(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 8, 1); st8(_da, _r); hle_insn(ctx, 16); } /* 00E140 subb %d0,%fp@(10) */
L_00E144:;
	hle_insn(ctx, 16); return; /* 00E144 rts */
}

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

/* sub_009EF8 (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_009EF8(hle_ctx *ctx)
{
	{ ctx->a[4] = 0xFFFFAA5Cu; hle_insn(ctx, 12); } /* 009EF8 lea 0xffffaa5c,%a4 */
	{ ctx->d[1] = 0x1Fu; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 009EFE moveq #31,%d1 */
L_009F00:;
	{ uint32_t _v = (ctx->d[2] & 0xFFFFu); st16(ctx->a[4], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 009F00 movew %d2,%a4@ */
	{ ctx->a[4] = (ctx->a[4] + (uint32_t)(int32_t)32); hle_insn(ctx, 8); } /* 009F02 lea %a4@(32),%a4 */
	{ uint16_t _c = (uint16_t)ctx->d[1] - 1; set_w(&ctx->d[1], _c); if (_c != 0xFFFF) { hle_insn(ctx, 10); goto L_009F00; } hle_insn(ctx, 14); } /* 009F06 dbf %d1,0x9f00 */
	hle_insn(ctx, 16); return; /* 009F0A rts */
}

/* sub_0109AC (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_0109AC(hle_ctx *ctx)
{
	{ uint32_t _s = 0x18u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 0109AC andiw #24,%d1 */
	{ sh_w(ctx, 1, 'r', 3); hle_insn(ctx, 12); } /* 0109B0 lsrw #3,%d1 */
	{ uint32_t _s = 0x270u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 0109B2 addiw #624,%d1 */
	{ uint32_t _a = (ctx->a[6] + (uint32_t)(int32_t)2); ctx->d[2] = (uint32_t)(int32_t)(int16_t)ld16(_a + 0); ctx->d[3] = (uint32_t)(int32_t)(int16_t)ld16(_a + 2); hle_insn(ctx, 24); } /* 0109B6 movemw %fp@(2),%d2-%d3 */
	{ ctx->d[5] = 0x0u; cc_logic32(ctx, ctx->d[5]); hle_insn(ctx, 4); } /* 0109BC moveq #0,%d5 */
	{ uint32_t _v = ld8((ctx->a[6] + (uint32_t)(int32_t)7)); set_b(&ctx->d[5], _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 0109BE moveb %fp@(7),%d5 */
	{ uint32_t _s = 0x78u; uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[3], _r); hle_insn(ctx, 8); } /* 0109C2 subiw #120,%d3 */
	hle_insn(ctx, 16); return; /* 0109C6 rts */
}

/* sub_011072 (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_011072(hle_ctx *ctx)
{
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 011072 movew %fp@(36),%d1 */
	{ uint32_t _s = 0x6u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 011076 andiw #6,%d1 */
	{ sh_w(ctx, 1, 'a', 1); hle_insn(ctx, 8); } /* 01107A asrw #1,%d1 */
	{ uint32_t _s = 0x3u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d ^ _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 01107C eoriw #3,%d1 */
	{ uint32_t _s = 0xDDu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 011080 addiw #221,%d1 */
	{ ctx->d[5] = 0xFu; cc_logic32(ctx, ctx->d[5]); hle_insn(ctx, 4); } /* 011084 moveq #15,%d5 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 011086 movew %fp@(4),%d3 */
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[3], _r); hle_insn(ctx, 4); } /* 01108A subqw #1,%d3 */
	{ hle_insn(ctx, 18); QueueSprite(ctx); } /* 01108C jsr %pc@(0x13240) */
	hle_insn(ctx, 16); return; /* 011090 rts */
}

/* sub_011146 (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_011146(hle_ctx *ctx)
{
	{ uint32_t _v = 0xEBF4u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011146 movew #-5132,%d1 */
	hle_insn(ctx, 10); goto L_011150; /* 01114A bras 0x11150 */
L_011150:;
	{ ctx->d[2] = 0x5u; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 011150 moveq #5,%d2 */
	{ ctx->d[3] = 0x5u; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 011152 moveq #5,%d3 */
	{ uint32_t _v = 0xA000u; ctx->d[6] = _v; cc_logic32(ctx, _v); hle_insn(ctx, 12); } /* 011154 movel #40960,%d6 */
	{ uint32_t _v = (ctx->d[7] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 01115A movew %d7,%sp@- */
	{ uint32_t _v = ld16(0xFFFF9E74u); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 01115C movew 0xffff9e74,%d7 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x012234); } /* 011162 jsr %pc@(0x12234) */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011166 movew %sp@+,%d7 */
	hle_insn(ctx, 16); return; /* 011168 rts */
}

/* sub_01114C (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_01114C(hle_ctx *ctx)
{
	{ uint32_t _v = 0xEBEAu; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 01114C movew #-5142,%d1 */
	{ ctx->d[2] = 0x5u; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 011150 moveq #5,%d2 */
	{ ctx->d[3] = 0x5u; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 011152 moveq #5,%d3 */
	{ uint32_t _v = 0xA000u; ctx->d[6] = _v; cc_logic32(ctx, _v); hle_insn(ctx, 12); } /* 011154 movel #40960,%d6 */
	{ uint32_t _v = (ctx->d[7] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 01115A movew %d7,%sp@- */
	{ uint32_t _v = ld16(0xFFFF9E74u); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 01115C movew 0xffff9e74,%d7 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x012234); } /* 011162 jsr %pc@(0x12234) */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011166 movew %sp@+,%d7 */
	hle_insn(ctx, 16); return; /* 011168 rts */
}

/* $0108E8 $27 SWAT van driving in to X=$E2, then parked ($010906) */
HLE_RANGE_PURE(0x0108E8, L7VanDrive, 1000, EXIT_NEXT)
{
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0108E8 movew %fp@(2),%d0 */
	{ uint32_t _s = 0x2u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 0108EC addqw #2,%d0 */
	{ uint32_t _s = 0xE2u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 0108EE cmpiw #226,%d0 */
	if (bw(ctx, !c_ge(ctx))) goto L_0108FE; /* 0108F2 bltw 0x108fe */
	{ uint32_t _v = 0x10906u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 0108F6 movel #67846,%fp@(32) */
L_0108FE:;
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)2), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0108FE movew %d0,%fp@(2) */
	hle_insn(ctx, 10); goto L_010912; /* 010902 braw 0x10912 */
L_010912:;
	{ uint32_t _v = 0x26Au; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010912 movew #618,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 010916 jsr %pc@(0x13228) */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 01091A subqw #1,%fp@(36) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01091E movew %fp@(36),%d1 */
	{ uint32_t _s = 0x6u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 010922 andiw #6,%d1 */
	{ sh_w(ctx, 1, 'a', 1); hle_insn(ctx, 8); } /* 010926 asrw #1,%d1 */
	{ uint32_t _s = 0x3u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d ^ _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 010928 eoriw #3,%d1 */
	{ uint32_t _s = 0xDDu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 01092C addiw #221,%d1 */
	{ uint32_t _v = (ctx->d[1] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010930 movew %d1,%sp@- */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010932 movew %fp@(2),%d2 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010936 movew %fp@(4),%d3 */
	{ ctx->d[5] = 0x0u; cc_logic32(ctx, ctx->d[5]); hle_insn(ctx, 4); } /* 01093A moveq #0,%d5 */
	{ uint32_t _v = ld8((ctx->a[6] + (uint32_t)(int32_t)7)); set_b(&ctx->d[5], _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 01093C moveb %fp@(7),%d5 */
	{ uint32_t _s = 0x42u; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 010940 subiw #66,%d2 */
	{ uint32_t _s = 0x2u; uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[3], _r); hle_insn(ctx, 8); } /* 010944 subiw #2,%d3 */
	{ uint32_t _v = (ctx->d[3] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010948 movew %d3,%sp@- */
	{ hle_insn(ctx, 18); QueueSprite(ctx); } /* 01094A jsr %pc@(0x13240) */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 01094E movew %sp@+,%d3 */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010950 movew %sp@+,%d1 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010952 movew %fp@(2),%d2 */
	{ uint32_t _s = 0x12Cu; uint32_t _d = (ctx->d[2] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 010956 cmpiw #300,%d2 */
	{ ctx->d[5] = 0x0u; cc_logic32(ctx, ctx->d[5]); hle_insn(ctx, 4); } /* 01095A moveq #0,%d5 */
	{ uint32_t _v = ld8((ctx->a[6] + (uint32_t)(int32_t)7)); set_b(&ctx->d[5], _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 01095C moveb %fp@(7),%d5 */
	{ uint32_t _s = 0x2Du; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 010960 addiw #45,%d2 */
	{ hle_insn(ctx, 18); QueueSprite(ctx); } /* 010964 jsr %pc@(0x13240) */
	{ uint32_t _v = 0x269u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010968 movew #617,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 01096C jsr %pc@(0x13228) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010970 movew %fp@(36),%d1 */
	{ hle_insn(ctx, 18); sub_0109AC(ctx); } /* 010974 bsrw 0x109ac */
	{ uint32_t _s = 0x50u; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 010978 subiw #80,%d2 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 0) & 1) ? 0 : CC_Z); hle_insn(ctx, 12); } /* 01097C btst #8,%fp@ */
	if (bs(ctx, c_z(ctx))) goto L_010986; /* 010980 beqs 0x10986 */
	{ uint32_t _v = 0x37Fu; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010982 movew #895,%d1 */
L_010986:;
	{ hle_insn(ctx, 18); QueueSprite(ctx); } /* 010986 jsr %pc@(0x13240) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01098A movew %fp@(36),%d1 */
	{ uint32_t _s = 0xEu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 01098E addiw #14,%d1 */
	{ hle_insn(ctx, 18); sub_0109AC(ctx); } /* 010992 bsrw 0x109ac */
	{ uint32_t _s = 0x28u; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 010996 addiw #40,%d2 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 1) & 1) ? 0 : CC_Z); hle_insn(ctx, 12); } /* 01099A btst #9,%fp@ */
	if (bs(ctx, c_z(ctx))) goto L_0109A4; /* 01099E beqs 0x109a4 */
	{ uint32_t _v = 0x37Fu; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0109A0 movew #895,%d1 */
L_0109A4:;
	{ hle_insn(ctx, 18); QueueSprite(ctx); } /* 0109A4 jsr %pc@(0x13240) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 0109A8 braw 0xb3be */
}

/* $010906 $27 SWAT van parked: sprites, wheel animation */
HLE_RANGE_PURE(0x010906, L7VanParked, 1000, EXIT_NEXT)
{
	hle_insn(ctx, 10); goto L_010912; /* 010906 bras 0x10912 */
L_010912:;
	{ uint32_t _v = 0x26Au; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010912 movew #618,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 010916 jsr %pc@(0x13228) */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 01091A subqw #1,%fp@(36) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01091E movew %fp@(36),%d1 */
	{ uint32_t _s = 0x6u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 010922 andiw #6,%d1 */
	{ sh_w(ctx, 1, 'a', 1); hle_insn(ctx, 8); } /* 010926 asrw #1,%d1 */
	{ uint32_t _s = 0x3u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d ^ _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 010928 eoriw #3,%d1 */
	{ uint32_t _s = 0xDDu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 01092C addiw #221,%d1 */
	{ uint32_t _v = (ctx->d[1] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010930 movew %d1,%sp@- */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010932 movew %fp@(2),%d2 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010936 movew %fp@(4),%d3 */
	{ ctx->d[5] = 0x0u; cc_logic32(ctx, ctx->d[5]); hle_insn(ctx, 4); } /* 01093A moveq #0,%d5 */
	{ uint32_t _v = ld8((ctx->a[6] + (uint32_t)(int32_t)7)); set_b(&ctx->d[5], _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 01093C moveb %fp@(7),%d5 */
	{ uint32_t _s = 0x42u; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 010940 subiw #66,%d2 */
	{ uint32_t _s = 0x2u; uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[3], _r); hle_insn(ctx, 8); } /* 010944 subiw #2,%d3 */
	{ uint32_t _v = (ctx->d[3] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010948 movew %d3,%sp@- */
	{ hle_insn(ctx, 18); QueueSprite(ctx); } /* 01094A jsr %pc@(0x13240) */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 01094E movew %sp@+,%d3 */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010950 movew %sp@+,%d1 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010952 movew %fp@(2),%d2 */
	{ uint32_t _s = 0x12Cu; uint32_t _d = (ctx->d[2] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 010956 cmpiw #300,%d2 */
	{ ctx->d[5] = 0x0u; cc_logic32(ctx, ctx->d[5]); hle_insn(ctx, 4); } /* 01095A moveq #0,%d5 */
	{ uint32_t _v = ld8((ctx->a[6] + (uint32_t)(int32_t)7)); set_b(&ctx->d[5], _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 01095C moveb %fp@(7),%d5 */
	{ uint32_t _s = 0x2Du; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 010960 addiw #45,%d2 */
	{ hle_insn(ctx, 18); QueueSprite(ctx); } /* 010964 jsr %pc@(0x13240) */
	{ uint32_t _v = 0x269u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010968 movew #617,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 01096C jsr %pc@(0x13228) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010970 movew %fp@(36),%d1 */
	{ hle_insn(ctx, 18); sub_0109AC(ctx); } /* 010974 bsrw 0x109ac */
	{ uint32_t _s = 0x50u; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 010978 subiw #80,%d2 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 0) & 1) ? 0 : CC_Z); hle_insn(ctx, 12); } /* 01097C btst #8,%fp@ */
	if (bs(ctx, c_z(ctx))) goto L_010986; /* 010980 beqs 0x10986 */
	{ uint32_t _v = 0x37Fu; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010982 movew #895,%d1 */
L_010986:;
	{ hle_insn(ctx, 18); QueueSprite(ctx); } /* 010986 jsr %pc@(0x13240) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01098A movew %fp@(36),%d1 */
	{ uint32_t _s = 0xEu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 01098E addiw #14,%d1 */
	{ hle_insn(ctx, 18); sub_0109AC(ctx); } /* 010992 bsrw 0x109ac */
	{ uint32_t _s = 0x28u; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 010996 addiw #40,%d2 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 1) & 1) ? 0 : CC_Z); hle_insn(ctx, 12); } /* 01099A btst #9,%fp@ */
	if (bs(ctx, c_z(ctx))) goto L_0109A4; /* 01099E beqs 0x109a4 */
	{ uint32_t _v = 0x37Fu; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0109A0 movew #895,%d1 */
L_0109A4:;
	{ hle_insn(ctx, 18); QueueSprite(ctx); } /* 0109A4 jsr %pc@(0x13240) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 0109A8 braw 0xb3be */
}

/* $0109C8 $27 explosion/debris: spawn a flying piece, fall into the countdown */
HLE_RANGE(0x0109C8, L7ExplodeInit, 30000, EXIT_NEXT)
{
	goto L_0109C8;
L_010908:;
	{ uint32_t _v = 0x26Bu; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010908 movew #619,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 01090C jsr %pc@(0x13228) */
	hle_insn(ctx, 10); goto L_01091A; /* 010910 bras 0x1091a */
L_01091A:;
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 01091A subqw #1,%fp@(36) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01091E movew %fp@(36),%d1 */
	{ uint32_t _s = 0x6u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 010922 andiw #6,%d1 */
	{ sh_w(ctx, 1, 'a', 1); hle_insn(ctx, 8); } /* 010926 asrw #1,%d1 */
	{ uint32_t _s = 0x3u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d ^ _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 010928 eoriw #3,%d1 */
	{ uint32_t _s = 0xDDu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 01092C addiw #221,%d1 */
	{ uint32_t _v = (ctx->d[1] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010930 movew %d1,%sp@- */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010932 movew %fp@(2),%d2 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010936 movew %fp@(4),%d3 */
	{ ctx->d[5] = 0x0u; cc_logic32(ctx, ctx->d[5]); hle_insn(ctx, 4); } /* 01093A moveq #0,%d5 */
	{ uint32_t _v = ld8((ctx->a[6] + (uint32_t)(int32_t)7)); set_b(&ctx->d[5], _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 01093C moveb %fp@(7),%d5 */
	{ uint32_t _s = 0x42u; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 010940 subiw #66,%d2 */
	{ uint32_t _s = 0x2u; uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[3], _r); hle_insn(ctx, 8); } /* 010944 subiw #2,%d3 */
	{ uint32_t _v = (ctx->d[3] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010948 movew %d3,%sp@- */
	{ hle_insn(ctx, 18); QueueSprite(ctx); } /* 01094A jsr %pc@(0x13240) */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 01094E movew %sp@+,%d3 */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010950 movew %sp@+,%d1 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010952 movew %fp@(2),%d2 */
	{ uint32_t _s = 0x12Cu; uint32_t _d = (ctx->d[2] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 010956 cmpiw #300,%d2 */
	{ ctx->d[5] = 0x0u; cc_logic32(ctx, ctx->d[5]); hle_insn(ctx, 4); } /* 01095A moveq #0,%d5 */
	{ uint32_t _v = ld8((ctx->a[6] + (uint32_t)(int32_t)7)); set_b(&ctx->d[5], _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 01095C moveb %fp@(7),%d5 */
	{ uint32_t _s = 0x2Du; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 010960 addiw #45,%d2 */
	{ hle_insn(ctx, 18); QueueSprite(ctx); } /* 010964 jsr %pc@(0x13240) */
	{ uint32_t _v = 0x269u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010968 movew #617,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 01096C jsr %pc@(0x13228) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010970 movew %fp@(36),%d1 */
	{ hle_insn(ctx, 18); sub_0109AC(ctx); } /* 010974 bsrw 0x109ac */
	{ uint32_t _s = 0x50u; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 010978 subiw #80,%d2 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 0) & 1) ? 0 : CC_Z); hle_insn(ctx, 12); } /* 01097C btst #8,%fp@ */
	if (bs(ctx, c_z(ctx))) goto L_010986; /* 010980 beqs 0x10986 */
	{ uint32_t _v = 0x37Fu; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010982 movew #895,%d1 */
L_010986:;
	{ hle_insn(ctx, 18); QueueSprite(ctx); } /* 010986 jsr %pc@(0x13240) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01098A movew %fp@(36),%d1 */
	{ uint32_t _s = 0xEu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 01098E addiw #14,%d1 */
	{ hle_insn(ctx, 18); sub_0109AC(ctx); } /* 010992 bsrw 0x109ac */
	{ uint32_t _s = 0x28u; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 010996 addiw #40,%d2 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 1) & 1) ? 0 : CC_Z); hle_insn(ctx, 12); } /* 01099A btst #9,%fp@ */
	if (bs(ctx, c_z(ctx))) goto L_0109A4; /* 01099E beqs 0x109a4 */
	{ uint32_t _v = 0x37Fu; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0109A0 movew #895,%d1 */
L_0109A4:;
	{ hle_insn(ctx, 18); QueueSprite(ctx); } /* 0109A4 jsr %pc@(0x13240) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 0109A8 braw 0xb3be */
L_0109C8:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0109C8 movew %fp@(36),%d0 */
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0109CC andiw #1,%d0 */
	if (bw(ctx, !c_z(ctx))) goto L_010A52; /* 0109D0 bnew 0x10a52 */
	{ uint32_t _v = ctx->a[6]; push32(ctx, _v); cc_logic32(ctx, _v); hle_insn(ctx, 12); } /* 0109D4 movel %fp,%sp@- */
	{ uint32_t _v = 0xA0u; set_w(&ctx->d[5], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0109D6 movew #160,%d5 */
	{ uint32_t _v = 0x58u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0109DA movew #88,%d1 */
	{ hle_insn(ctx, 18); ObjAlloc(ctx); } /* 0109DE jsr %pc@(0x930e) */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x009A5E); } /* 0109E2 jsr %pc@(0x9a5e) */
	{ uint32_t _v = 0x12EE6u; ctx->a[5] = _v; hle_insn(ctx, 12); } /* 0109E6 moveal #77542,%a5 */
	{ uint32_t _p = ld16(ctx->a[5]); ctx->a[5] += 2; uint32_t _v = _p; st16((ctx->a[6] + (uint32_t)(int32_t)26), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 0109EC movew %a5@+,%fp@(26) */
	{ uint32_t _v = ctx->a[5]; st32((ctx->a[6] + (uint32_t)(int32_t)22), _v); cc_logic32(ctx, _v); hle_insn(ctx, 16); } /* 0109F0 movel %a5,%fp@(22) */
	{ uint32_t _v = 0x100u; st16((ctx->a[6] + (uint32_t)(int32_t)20), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 0109F4 movew #256,%fp@(20) */
	{ uint32_t _v = 0x9u; st16(ctx->a[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0109FA movew #9,%fp@ */
	{ uint32_t _v = 0x10A92u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 0109FE movel #68242,%fp@(32) */
	{ uint32_t _v = 0x597Au; st32((ctx->a[6] + (uint32_t)(int32_t)28), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 010A06 movel #22906,%fp@(28) */
	{ hle_insn(ctx, 20); Random(ctx); } /* 010A0E jsr 0x881c */
	{ uint32_t _s = 0x5Fu; uint32_t _d = (ctx->d[0] & 0xFFu); uint32_t _r = (_d & _s) & 0xFFu; cc_logic8(ctx, _r); set_b(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 010A14 andib #95,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFu); st8((ctx->a[6] + (uint32_t)(int32_t)10), _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 010A18 moveb %d0,%fp@(10) */
	{ hle_insn(ctx, 20); Random(ctx); } /* 010A1C jsr 0x881c */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 010A22 movew %d0,%d3 */
	{ uint32_t _s = 0x5Fu; uint32_t _d = (ctx->d[3] & 0xFFu); uint32_t _r = (_d & _s) & 0xFFu; cc_logic8(ctx, _r); set_b(&ctx->d[3], _r); hle_insn(ctx, 8); } /* 010A24 andib #95,%d3 */
	{ uint32_t _v = (ctx->d[3] & 0xFFu); st8((ctx->a[6] + (uint32_t)(int32_t)12), _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 010A28 moveb %d3,%fp@(12) */
	{ uint32_t _s = 0x6u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 010A2C andiw #6,%d0 */
	{ uint32_t _v = ld16((0x010A8Au + (uint32_t)((int32_t)(int16_t)ctx->d[0]))); st16((ctx->a[6] + (uint32_t)(int32_t)38), _v); cc_logic16(ctx, _v); hle_insn(ctx, 22); } /* 010A30 movew %pc@(0x10a8a,%d0:w),%fp@(38) */
	{ uint32_t _v = 0x3Cu; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 010A36 movew #60,%fp@(36) */
	{ uint32_t _v = 0x10u; st8((ctx->a[6] + (uint32_t)(int32_t)6), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 010A3C moveb #16,%fp@(6) */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DAu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 010A42 addqw #1,0xffff80da */
	{ uint32_t _v = pop32(ctx); ctx->a[5] = _v; hle_insn(ctx, 12); } /* 010A48 moveal %sp@+,%a5 */
	{ uint32_t _v = ld32((ctx->a[5] + (uint32_t)(int32_t)2)); st32((ctx->a[6] + (uint32_t)(int32_t)2), _v); cc_logic32(ctx, _v); hle_insn(ctx, 28); } /* 010A4A movel %a5@(2),%fp@(2) */
	{ uint32_t _v = ctx->a[5]; ctx->a[6] = _v; hle_insn(ctx, 4); } /* 010A50 moveal %a5,%fp */
L_010A52:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010A52 movew %fp@(36),%d0 */
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 010A56 subiw #1,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010A5A movew %d0,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_010A6E; /* 010A5E bnes 0x10a6e */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x00902E); } /* 010A60 jsr %pc@(0x902e) */
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 010A64 clrw %fp@ */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DAu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 24); } /* 010A66 subiw #1,0xffff80da */
L_010A6E:;
	{ uint32_t _s = 0x2Cu; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 010A6E cmpiw #44,%d0 */
	if (bw(ctx, c_gt(ctx))) goto L_010908; /* 010A72 bgtw 0x10908 */
	{ uint32_t _s = 0x16u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 010A76 cmpiw #22,%d0 */
	if (bw(ctx, !c_ge(ctx))) { hle_jump(ctx, 0x00B3BE); return; } /* 010A7A bltw 0xb3be */
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 010A7E andiw #1,%d0 */
	if (bw(ctx, c_z(ctx))) goto L_010908; /* 010A82 beqw 0x10908 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 010A86 braw 0xb3be */
}

/* $010A92 $09 flying debris piece along a path */
HLE_RANGE_PURE(0x010A92, L7Debris, 1000, EXIT_NEXT)
{
	{ hle_insn(ctx, 18); sub_00C48E(ctx); } /* 010A92 jsr %pc@(0xc48e) */
	{ cc_logic8(ctx, (ctx->d[4] & 0xFFu)); hle_insn(ctx, 4); } /* 010A96 tstb %d4 */
	if (bw(ctx, c_z(ctx))) goto L_010AA8; /* 010A98 beqw 0x10aa8 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)38)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010A9C movew %fp@(38),%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 010AA0 jsr %pc@(0x13228) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 010AA4 braw 0xb3be */
L_010AA8:;
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 010AA8 clrw %fp@ */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DAu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 010AAA subqw #1,0xffff80da */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 010AB0 braw 0xb3be */
}

/* $010AB4 $27 SWAT van: speed up and clamp at X=$E2 */
HLE_RANGE_PURE(0x010AB4, L7VanLeave, 1000, EXIT_NEXT)
{
	goto L_010AB4;
L_010908:;
	{ uint32_t _v = 0x26Bu; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010908 movew #619,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 01090C jsr %pc@(0x13228) */
	hle_insn(ctx, 10); goto L_01091A; /* 010910 bras 0x1091a */
L_01091A:;
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 01091A subqw #1,%fp@(36) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01091E movew %fp@(36),%d1 */
	{ uint32_t _s = 0x6u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 010922 andiw #6,%d1 */
	{ sh_w(ctx, 1, 'a', 1); hle_insn(ctx, 8); } /* 010926 asrw #1,%d1 */
	{ uint32_t _s = 0x3u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d ^ _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 010928 eoriw #3,%d1 */
	{ uint32_t _s = 0xDDu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 01092C addiw #221,%d1 */
	{ uint32_t _v = (ctx->d[1] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010930 movew %d1,%sp@- */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010932 movew %fp@(2),%d2 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010936 movew %fp@(4),%d3 */
	{ ctx->d[5] = 0x0u; cc_logic32(ctx, ctx->d[5]); hle_insn(ctx, 4); } /* 01093A moveq #0,%d5 */
	{ uint32_t _v = ld8((ctx->a[6] + (uint32_t)(int32_t)7)); set_b(&ctx->d[5], _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 01093C moveb %fp@(7),%d5 */
	{ uint32_t _s = 0x42u; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 010940 subiw #66,%d2 */
	{ uint32_t _s = 0x2u; uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[3], _r); hle_insn(ctx, 8); } /* 010944 subiw #2,%d3 */
	{ uint32_t _v = (ctx->d[3] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010948 movew %d3,%sp@- */
	{ hle_insn(ctx, 18); QueueSprite(ctx); } /* 01094A jsr %pc@(0x13240) */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 01094E movew %sp@+,%d3 */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010950 movew %sp@+,%d1 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010952 movew %fp@(2),%d2 */
	{ uint32_t _s = 0x12Cu; uint32_t _d = (ctx->d[2] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 010956 cmpiw #300,%d2 */
	{ ctx->d[5] = 0x0u; cc_logic32(ctx, ctx->d[5]); hle_insn(ctx, 4); } /* 01095A moveq #0,%d5 */
	{ uint32_t _v = ld8((ctx->a[6] + (uint32_t)(int32_t)7)); set_b(&ctx->d[5], _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 01095C moveb %fp@(7),%d5 */
	{ uint32_t _s = 0x2Du; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 010960 addiw #45,%d2 */
	{ hle_insn(ctx, 18); QueueSprite(ctx); } /* 010964 jsr %pc@(0x13240) */
	{ uint32_t _v = 0x269u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010968 movew #617,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 01096C jsr %pc@(0x13228) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010970 movew %fp@(36),%d1 */
	{ hle_insn(ctx, 18); sub_0109AC(ctx); } /* 010974 bsrw 0x109ac */
	{ uint32_t _s = 0x50u; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 010978 subiw #80,%d2 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 0) & 1) ? 0 : CC_Z); hle_insn(ctx, 12); } /* 01097C btst #8,%fp@ */
	if (bs(ctx, c_z(ctx))) goto L_010986; /* 010980 beqs 0x10986 */
	{ uint32_t _v = 0x37Fu; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010982 movew #895,%d1 */
L_010986:;
	{ hle_insn(ctx, 18); QueueSprite(ctx); } /* 010986 jsr %pc@(0x13240) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01098A movew %fp@(36),%d1 */
	{ uint32_t _s = 0xEu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 01098E addiw #14,%d1 */
	{ hle_insn(ctx, 18); sub_0109AC(ctx); } /* 010992 bsrw 0x109ac */
	{ uint32_t _s = 0x28u; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 010996 addiw #40,%d2 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 1) & 1) ? 0 : CC_Z); hle_insn(ctx, 12); } /* 01099A btst #9,%fp@ */
	if (bs(ctx, c_z(ctx))) goto L_0109A4; /* 01099E beqs 0x109a4 */
	{ uint32_t _v = 0x37Fu; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0109A0 movew #895,%d1 */
L_0109A4:;
	{ hle_insn(ctx, 18); QueueSprite(ctx); } /* 0109A4 jsr %pc@(0x13240) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 0109A8 braw 0xb3be */
L_010AB4:;
	{ hle_insn(ctx, 18); sub_00E10C(ctx); } /* 010AB4 jsr %pc@(0xe10c) */
	{ cc_logic8(ctx, ld8((ctx->a[6] + (uint32_t)(int32_t)14))); hle_insn(ctx, 12); } /* 010AB8 tstb %fp@(14) */
	if (bs(ctx, !c_n(ctx))) goto L_010ACA; /* 010ABC bpls 0x10aca */
	{ uint32_t _s = 0x20u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)10); uint32_t _d = ld8(_da); alu(ctx, OP_SUB, _d, _s, 8, 0); hle_insn(ctx, 16); } /* 010ABE cmpib #32,%fp@(10) */
	if (bs(ctx, !c_ge(ctx))) goto L_010ACA; /* 010AC4 blts 0x10aca */
	{ st8((ctx->a[6] + (uint32_t)(int32_t)16), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 010AC6 clrb %fp@(16) */
L_010ACA:;
	{ uint32_t _s = 0xE2u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)2); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 010ACA cmpiw #226,%fp@(2) */
	if (bs(ctx, c_ge(ctx))) goto L_010ADC; /* 010AD0 bges 0x10adc */
	{ uint32_t _v = 0xE2u; st16((ctx->a[6] + (uint32_t)(int32_t)2), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 010AD2 movew #226,%fp@(2) */
	{ st8((ctx->a[6] + (uint32_t)(int32_t)10), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 010AD8 clrb %fp@(10) */
L_010ADC:;
	hle_insn(ctx, 10); goto L_010908; /* 010ADC braw 0x10908 */
}

/* $010AE0 $27 SWAT van drives off to the right and is freed */
HLE_RANGE_PURE(0x010AE0, L7VanGone, 1000, EXIT_NEXT)
{
	goto L_010AE0;
L_010908:;
	{ uint32_t _v = 0x26Bu; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010908 movew #619,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 01090C jsr %pc@(0x13228) */
	hle_insn(ctx, 10); goto L_01091A; /* 010910 bras 0x1091a */
L_01091A:;
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 01091A subqw #1,%fp@(36) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01091E movew %fp@(36),%d1 */
	{ uint32_t _s = 0x6u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 010922 andiw #6,%d1 */
	{ sh_w(ctx, 1, 'a', 1); hle_insn(ctx, 8); } /* 010926 asrw #1,%d1 */
	{ uint32_t _s = 0x3u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d ^ _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 010928 eoriw #3,%d1 */
	{ uint32_t _s = 0xDDu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 01092C addiw #221,%d1 */
	{ uint32_t _v = (ctx->d[1] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010930 movew %d1,%sp@- */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010932 movew %fp@(2),%d2 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010936 movew %fp@(4),%d3 */
	{ ctx->d[5] = 0x0u; cc_logic32(ctx, ctx->d[5]); hle_insn(ctx, 4); } /* 01093A moveq #0,%d5 */
	{ uint32_t _v = ld8((ctx->a[6] + (uint32_t)(int32_t)7)); set_b(&ctx->d[5], _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 01093C moveb %fp@(7),%d5 */
	{ uint32_t _s = 0x42u; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 010940 subiw #66,%d2 */
	{ uint32_t _s = 0x2u; uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[3], _r); hle_insn(ctx, 8); } /* 010944 subiw #2,%d3 */
	{ uint32_t _v = (ctx->d[3] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010948 movew %d3,%sp@- */
	{ hle_insn(ctx, 18); QueueSprite(ctx); } /* 01094A jsr %pc@(0x13240) */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 01094E movew %sp@+,%d3 */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010950 movew %sp@+,%d1 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010952 movew %fp@(2),%d2 */
	{ uint32_t _s = 0x12Cu; uint32_t _d = (ctx->d[2] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 010956 cmpiw #300,%d2 */
	{ ctx->d[5] = 0x0u; cc_logic32(ctx, ctx->d[5]); hle_insn(ctx, 4); } /* 01095A moveq #0,%d5 */
	{ uint32_t _v = ld8((ctx->a[6] + (uint32_t)(int32_t)7)); set_b(&ctx->d[5], _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 01095C moveb %fp@(7),%d5 */
	{ uint32_t _s = 0x2Du; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 010960 addiw #45,%d2 */
	{ hle_insn(ctx, 18); QueueSprite(ctx); } /* 010964 jsr %pc@(0x13240) */
	{ uint32_t _v = 0x269u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010968 movew #617,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 01096C jsr %pc@(0x13228) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010970 movew %fp@(36),%d1 */
	{ hle_insn(ctx, 18); sub_0109AC(ctx); } /* 010974 bsrw 0x109ac */
	{ uint32_t _s = 0x50u; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 010978 subiw #80,%d2 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 0) & 1) ? 0 : CC_Z); hle_insn(ctx, 12); } /* 01097C btst #8,%fp@ */
	if (bs(ctx, c_z(ctx))) goto L_010986; /* 010980 beqs 0x10986 */
	{ uint32_t _v = 0x37Fu; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010982 movew #895,%d1 */
L_010986:;
	{ hle_insn(ctx, 18); QueueSprite(ctx); } /* 010986 jsr %pc@(0x13240) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01098A movew %fp@(36),%d1 */
	{ uint32_t _s = 0xEu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 01098E addiw #14,%d1 */
	{ hle_insn(ctx, 18); sub_0109AC(ctx); } /* 010992 bsrw 0x109ac */
	{ uint32_t _s = 0x28u; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 010996 addiw #40,%d2 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 1) & 1) ? 0 : CC_Z); hle_insn(ctx, 12); } /* 01099A btst #9,%fp@ */
	if (bs(ctx, c_z(ctx))) goto L_0109A4; /* 01099E beqs 0x109a4 */
	{ uint32_t _v = 0x37Fu; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0109A0 movew #895,%d1 */
L_0109A4:;
	{ hle_insn(ctx, 18); QueueSprite(ctx); } /* 0109A4 jsr %pc@(0x13240) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 0109A8 braw 0xb3be */
L_010AE0:;
	{ hle_insn(ctx, 18); sub_00E10C(ctx); } /* 010AE0 jsr %pc@(0xe10c) */
	{ uint32_t _s = 0x1A4u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)2); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 010AE4 cmpiw #420,%fp@(2) */
	if (bs(ctx, !c_ge(ctx))) goto L_010AF4; /* 010AEA blts 0x10af4 */
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 010AEC clrw %fp@ */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DAu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 010AEE subqw #1,0xffff80da */
L_010AF4:;
	hle_insn(ctx, 10); goto L_010908; /* 010AF4 braw 0x10908 */
}

/* $010AF8 $27 T-1000 on the police vehicle: pick the approach side */
HLE_RANGE(0x010AF8, L7HeliStart, 9300, EXIT_NEXT)
{
	{ cc_logic16(ctx, ld16((ctx->a[6] + (uint32_t)(int32_t)36))); hle_insn(ctx, 12); } /* 010AF8 tstw %fp@(36) */
	if (bw(ctx, !c_z(ctx))) goto L_010B80; /* 010AFC bnew 0x10b80 */
	{ uint32_t _v = 0x0u; st16((ctx->a[6] + (uint32_t)(int32_t)2), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 010B00 movew #0,%fp@(2) */
	{ uint32_t _v = 0x10B84u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 010B06 movel #68484,%fp@(32) */
	{ uint32_t _v = ld8(0xFFFF9F6Cu); st8((ctx->a[6] + (uint32_t)(int32_t)10), _v); cc_logic8(ctx, _v); hle_insn(ctx, 24); } /* 010B0E moveb 0xffff9f6c,%fp@(10) */
	{ st8((ctx->a[6] + (uint32_t)(int32_t)16), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 010B16 clrb %fp@(16) */
	{ uint32_t _v = 0x1u; st8((ctx->a[6] + (uint32_t)(int32_t)14), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 010B1A moveb #1,%fp@(14) */
	{ st8((ctx->a[6] + (uint32_t)(int32_t)11), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 010B20 clrb %fp@(11) */
	{ uint32_t _v = 0x80u; st8((ctx->a[6] + (uint32_t)(int32_t)12), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 010B24 moveb #-128,%fp@(12) */
	{ st8((ctx->a[6] + (uint32_t)(int32_t)18), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 010B2A clrb %fp@(18) */
	{ uint32_t _v = 0x0u; st8((ctx->a[6] + (uint32_t)(int32_t)15), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 010B2E moveb #0,%fp@(15) */
	{ st8((ctx->a[6] + (uint32_t)(int32_t)13), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 010B34 clrb %fp@(13) */
	{ hle_insn(ctx, 20); Random(ctx); } /* 010B38 jsr 0x881c */
	{ cc_logic16(ctx, (ctx->d[0] & 0xFFFFu)); hle_insn(ctx, 4); } /* 010B3E tstw %d0 */
	if (bs(ctx, c_n(ctx))) goto L_010B5C; /* 010B40 bmis 0x10b5c */
	{ uint32_t _v = 0x0u; st16((ctx->a[6] + (uint32_t)(int32_t)2), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 010B42 movew #0,%fp@(2) */
	{ uint32_t _s = 0x3Fu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 010B48 andiw #63,%d0 */
	{ uint32_t _s = 0x78u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 010B4C addiw #120,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)4), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010B50 movew %d0,%fp@(4) */
	{ uint32_t _v = 0xF0u; st8((ctx->a[6] + (uint32_t)(int32_t)10), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 010B54 moveb #-16,%fp@(10) */
	hle_insn(ctx, 10); goto L_010B74; /* 010B5A bras 0x10b74 */
L_010B5C:;
	{ uint32_t _v = 0x0u; st16((ctx->a[6] + (uint32_t)(int32_t)4), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 010B5C movew #0,%fp@(4) */
	{ uint32_t _s = 0x3Fu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 010B62 andiw #63,%d0 */
	{ uint32_t _s = 0x5Au; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 010B66 addiw #90,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)2), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010B6A movew %d0,%fp@(2) */
	{ uint32_t _v = 0x1u; st8((ctx->a[6] + (uint32_t)(int32_t)15), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 010B6E moveb #1,%fp@(15) */
L_010B74:;
	{ uint32_t _v = 0x10B84u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 010B74 movel #68484,%fp@(32) */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o | 0x80); hle_insn(ctx, 16); } /* 010B7C bset #15,%fp@ */
L_010B80:;
	hle_insn(ctx, 10); goto L_010C96; /* 010B80 braw 0x10c96 */
L_010C96:;
	{ ctx->d[2] = 0x18u; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 010C96 moveq #24,%d2 */
	{ ctx->d[3] = 0x5u; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 010C98 moveq #5,%d3 */
	{ uint32_t _v = 0xEBD0u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010C9A movew #-5168,%d1 */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_call68k(ctx, 0x012212); } /* 010C9E jsr 0x12212 */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 010CA4 subqw #1,%fp@(36) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010CA8 movew %fp@(36),%d1 */
	{ uint32_t _s = 0xCu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 010CAC andiw #12,%d1 */
	{ ctx->a[1] = 0x010CFCu; hle_insn(ctx, 8); } /* 010CB0 lea %pc@(0x10cfc),%a1 */
	{ uint32_t _v = ld32((ctx->a[1] + (uint32_t)(int32_t)0 + (uint32_t)((int32_t)(int16_t)ctx->d[1]))); ctx->a[0] = _v; hle_insn(ctx, 18); } /* 010CB4 moveal %a1@(0,%d1:w),%a0 */
	{ ctx->d[3] = 0x5u; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 010CB8 moveq #5,%d3 */
	{ uint32_t _v = 0xEBD0u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010CBA movew #-5168,%d1 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010CBE movew %fp@(4),%d0 */
	{ uint32_t _s = 0x20u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 010CC2 cmpiw #32,%d0 */
	if (bs(ctx, c_ge(ctx))) goto L_010CE2; /* 010CC6 bges 0x10ce2 */
	{ sh_w(ctx, 0, 'a', 3); hle_insn(ctx, 12); } /* 010CC8 asrw #3,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 010CCA movew %d0,%d6 */
	{ uint32_t _s = 0x4u; uint32_t _d = (ctx->d[6] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[6], _r); hle_insn(ctx, 4); } /* 010CCC subqw #4,%d6 */
	{ uint32_t _r = alu(ctx, OP_SUB, 0, ctx->d[6], 16, 1); set_w(&ctx->d[6], _r); hle_insn(ctx, 4); } /* 010CCE negw %d6 */
	{ uint32_t _v = (ctx->d[6] & 0xFFFFu); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 010CD0 movew %d6,%d2 */
	{ sh_w(ctx, 6, 'l', 7); hle_insn(ctx, 20); } /* 010CD2 aslw #7,%d6 */
	{ mul_w(ctx, 2, 0x50, 0);  } /* 010CD4 muluw #80,%d2 */
	{ uint32_t _s = (ctx->d[6] & 0xFFFFu); uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 4); } /* 010CD8 addw %d6,%d1 */
	{ ctx->a[0] = (ctx->a[0] + (uint32_t)(int32_t)0 + (uint32_t)((int32_t)(int16_t)ctx->d[6])); hle_insn(ctx, 12); } /* 010CDA lea %a0@(0,%d6:w),%a0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 010CDE movew %d0,%d3 */
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[3], _r); hle_insn(ctx, 4); } /* 010CE0 addqw #1,%d3 */
L_010CE2:;
	{ uint32_t _v = 0x18u; set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010CE2 movew #24,%d2 */
	{ uint32_t _v = 0x2000u; set_w(&ctx->d[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010CE6 movew #8192,%d6 */
	{ uint32_t _v = (ctx->d[7] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010CEA movew %d7,%sp@- */
	{ uint32_t _v = ld16(0xFFFF9E34u); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 010CEC movew 0xffff9e34,%d7 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x012234); } /* 010CF2 jsr %pc@(0x12234) */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010CF6 movew %sp@+,%d7 */
	hle_insn(ctx, 10); goto L_010D20; /* 010CF8 braw 0x10d20 */
L_010D20:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010D20 movew %fp@(2),%d2 */
	if (bs(ctx, !c_n(ctx))) goto L_010D2C; /* 010D24 bpls 0x10d2c */
	{ set_w(&ctx->d[2], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 4); } /* 010D26 clrw %d2 */
	{ uint32_t _v = (ctx->d[2] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)2), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010D28 movew %d2,%fp@(2) */
L_010D2C:;
	{ hle_insn(ctx, 18); sub_009EF8(ctx); } /* 010D2C jsr %pc@(0x9ef8) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010D30 movew %fp@(4),%d0 */
	{ uint32_t _s = 0xFFu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 010D34 andiw #255,%d0 */
	{ uint32_t _s = 0xFFu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d ^ _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 010D38 eoriw #255,%d0 */
	{ ctx->d[1] = 0x0u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 010D3C moveq #0,%d1 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x011F58); } /* 010D3E jsr %pc@(0x11f58) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 010D42 braw 0xb3be */
}

/* $010B84 $27 T-1000 on the police vehicle: approach */
HLE_RANGE(0x010B84, L7HeliApproach, 10700, EXIT_NEXT)
{
	{ uint32_t _v = 0x73u; st16(0xFFFF9EA4u, _v); cc_logic16(ctx, _v); hle_insn(ctx, 20); } /* 010B84 movew #115,0xffff9ea4 */
	{ hle_insn(ctx, 18); sub_00E10C(ctx); } /* 010B8C jsr %pc@(0xe10c) */
	{ hle_insn(ctx, 18); sub_00D236(ctx); } /* 010B90 jsr %pc@(0xd236) */
	{ cc_logic8(ctx, ld8((ctx->a[6] + (uint32_t)(int32_t)15))); hle_insn(ctx, 12); } /* 010B94 tstb %fp@(15) */
	if (bw(ctx, c_z(ctx))) goto L_010BA6; /* 010B98 beqw 0x10ba6 */
	{ uint32_t _s = 0x70u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)4); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 010B9C cmpiw #112,%fp@(4) */
	if (bs(ctx, !c_ge(ctx))) goto L_010BE0; /* 010BA2 blts 0x10be0 */
	hle_insn(ctx, 10); goto L_010BAE; /* 010BA4 bras 0x10bae */
L_010BA6:;
	{ uint32_t _s = 0x9Au; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)2); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 010BA6 cmpiw #154,%fp@(2) */
	if (bs(ctx, !c_ge(ctx))) goto L_010BE0; /* 010BAC blts 0x10be0 */
L_010BAE:;
	{ uint32_t _v = 0x0u; st16(0xFFFF9EA4u, _v); cc_logic16(ctx, _v); hle_insn(ctx, 20); } /* 010BAE movew #0,0xffff9ea4 */
	{ uint32_t _v = 0x80u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010BB6 movew #128,%d0 */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_call68k(ctx, 0x013D04); } /* 010BBA jsr 0x13d04 */
	{ uint32_t _v = 0x10D46u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 010BC0 movel #68934,%fp@(32) */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o & 0x7F); hle_insn(ctx, 16); } /* 010BC8 bclr #15,%fp@ */
	{ uint32_t _v = ld32(0xFFFF9ECAu); ctx->a[1] = _v; hle_insn(ctx, 20); } /* 010BCC moveal 0xffff9eca,%a1 */
	{ uint32_t _v = 0x109C8u; st32((ctx->a[1] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 010BD2 movel #68040,%a1@(32) */
	{ uint32_t _v = 0x42u; st16((ctx->a[1] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 010BDA movew #66,%a1@(36) */
L_010BE0:;
	hle_insn(ctx, 10); goto L_010C96; /* 010BE0 braw 0x10c96 */
L_010C96:;
	{ ctx->d[2] = 0x18u; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 010C96 moveq #24,%d2 */
	{ ctx->d[3] = 0x5u; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 010C98 moveq #5,%d3 */
	{ uint32_t _v = 0xEBD0u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010C9A movew #-5168,%d1 */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_call68k(ctx, 0x012212); } /* 010C9E jsr 0x12212 */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 010CA4 subqw #1,%fp@(36) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010CA8 movew %fp@(36),%d1 */
	{ uint32_t _s = 0xCu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 010CAC andiw #12,%d1 */
	{ ctx->a[1] = 0x010CFCu; hle_insn(ctx, 8); } /* 010CB0 lea %pc@(0x10cfc),%a1 */
	{ uint32_t _v = ld32((ctx->a[1] + (uint32_t)(int32_t)0 + (uint32_t)((int32_t)(int16_t)ctx->d[1]))); ctx->a[0] = _v; hle_insn(ctx, 18); } /* 010CB4 moveal %a1@(0,%d1:w),%a0 */
	{ ctx->d[3] = 0x5u; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 010CB8 moveq #5,%d3 */
	{ uint32_t _v = 0xEBD0u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010CBA movew #-5168,%d1 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010CBE movew %fp@(4),%d0 */
	{ uint32_t _s = 0x20u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 010CC2 cmpiw #32,%d0 */
	if (bs(ctx, c_ge(ctx))) goto L_010CE2; /* 010CC6 bges 0x10ce2 */
	{ sh_w(ctx, 0, 'a', 3); hle_insn(ctx, 12); } /* 010CC8 asrw #3,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 010CCA movew %d0,%d6 */
	{ uint32_t _s = 0x4u; uint32_t _d = (ctx->d[6] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[6], _r); hle_insn(ctx, 4); } /* 010CCC subqw #4,%d6 */
	{ uint32_t _r = alu(ctx, OP_SUB, 0, ctx->d[6], 16, 1); set_w(&ctx->d[6], _r); hle_insn(ctx, 4); } /* 010CCE negw %d6 */
	{ uint32_t _v = (ctx->d[6] & 0xFFFFu); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 010CD0 movew %d6,%d2 */
	{ sh_w(ctx, 6, 'l', 7); hle_insn(ctx, 20); } /* 010CD2 aslw #7,%d6 */
	{ mul_w(ctx, 2, 0x50, 0);  } /* 010CD4 muluw #80,%d2 */
	{ uint32_t _s = (ctx->d[6] & 0xFFFFu); uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 4); } /* 010CD8 addw %d6,%d1 */
	{ ctx->a[0] = (ctx->a[0] + (uint32_t)(int32_t)0 + (uint32_t)((int32_t)(int16_t)ctx->d[6])); hle_insn(ctx, 12); } /* 010CDA lea %a0@(0,%d6:w),%a0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 010CDE movew %d0,%d3 */
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[3], _r); hle_insn(ctx, 4); } /* 010CE0 addqw #1,%d3 */
L_010CE2:;
	{ uint32_t _v = 0x18u; set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010CE2 movew #24,%d2 */
	{ uint32_t _v = 0x2000u; set_w(&ctx->d[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010CE6 movew #8192,%d6 */
	{ uint32_t _v = (ctx->d[7] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010CEA movew %d7,%sp@- */
	{ uint32_t _v = ld16(0xFFFF9E34u); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 010CEC movew 0xffff9e34,%d7 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x012234); } /* 010CF2 jsr %pc@(0x12234) */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010CF6 movew %sp@+,%d7 */
	hle_insn(ctx, 10); goto L_010D20; /* 010CF8 braw 0x10d20 */
L_010D20:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010D20 movew %fp@(2),%d2 */
	if (bs(ctx, !c_n(ctx))) goto L_010D2C; /* 010D24 bpls 0x10d2c */
	{ set_w(&ctx->d[2], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 4); } /* 010D26 clrw %d2 */
	{ uint32_t _v = (ctx->d[2] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)2), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010D28 movew %d2,%fp@(2) */
L_010D2C:;
	{ hle_insn(ctx, 18); sub_009EF8(ctx); } /* 010D2C jsr %pc@(0x9ef8) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010D30 movew %fp@(4),%d0 */
	{ uint32_t _s = 0xFFu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 010D34 andiw #255,%d0 */
	{ uint32_t _s = 0xFFu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d ^ _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 010D38 eoriw #255,%d0 */
	{ ctx->d[1] = 0x0u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 010D3C moveq #0,%d1 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x011F58); } /* 010D3E jsr %pc@(0x11f58) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 010D42 braw 0xb3be */
}

/* $010BE4 $27 T-1000 on the police vehicle shot: fly off */
HLE_RANGE(0x010BE4, L7HeliHit, 12000, EXIT_NEXT)
{
	{ uint32_t _v = 0x0u; st8((ctx->a[6] + (uint32_t)(int32_t)10), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 010BE4 moveb #0,%fp@(10) */
	{ uint32_t _v = 0xFDu; st8((ctx->a[6] + (uint32_t)(int32_t)16), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 010BEA moveb #-3,%fp@(16) */
	{ uint32_t _v = 0xFFu; st8((ctx->a[6] + (uint32_t)(int32_t)14), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 010BF0 moveb #-1,%fp@(14) */
	{ st8((ctx->a[6] + (uint32_t)(int32_t)11), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 010BF6 clrb %fp@(11) */
	{ uint32_t _v = 0x0u; st8((ctx->a[6] + (uint32_t)(int32_t)12), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 010BFA moveb #0,%fp@(12) */
	{ uint32_t _v = 0xFDu; st8((ctx->a[6] + (uint32_t)(int32_t)18), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 010C00 moveb #-3,%fp@(18) */
	{ uint32_t _v = 0xFFu; st8((ctx->a[6] + (uint32_t)(int32_t)15), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 010C06 moveb #-1,%fp@(15) */
	{ st8((ctx->a[6] + (uint32_t)(int32_t)11), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 010C0C clrb %fp@(11) */
	{ uint32_t _v = 0x10C1Cu; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 010C10 movel #68636,%fp@(32) */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o & 0x7F); hle_insn(ctx, 16); } /* 010C18 bclr #15,%fp@ */
	{ uint32_t _v = 0x73u; st16(0xFFFF9EA4u, _v); cc_logic16(ctx, _v); hle_insn(ctx, 20); } /* 010C1C movew #115,0xffff9ea4 */
	{ hle_insn(ctx, 18); sub_00E10C(ctx); } /* 010C24 jsr %pc@(0xe10c) */
	{ hle_insn(ctx, 18); sub_00D236(ctx); } /* 010C28 jsr %pc@(0xd236) */
	{ cc_logic8(ctx, ld8((ctx->a[6] + (uint32_t)(int32_t)15))); hle_insn(ctx, 12); } /* 010C2C tstb %fp@(15) */
	if (bw(ctx, c_z(ctx))) goto L_010C3C; /* 010C30 beqw 0x10c3c */
	{ cc_logic16(ctx, ld16((ctx->a[6] + (uint32_t)(int32_t)4))); hle_insn(ctx, 12); } /* 010C34 tstw %fp@(4) */
	if (bs(ctx, !c_n(ctx))) goto L_010C76; /* 010C38 bpls 0x10c76 */
	hle_insn(ctx, 10); goto L_010C42; /* 010C3A bras 0x10c42 */
L_010C3C:;
	{ cc_logic16(ctx, ld16((ctx->a[6] + (uint32_t)(int32_t)2))); hle_insn(ctx, 12); } /* 010C3C tstw %fp@(2) */
	if (bs(ctx, !c_n(ctx))) goto L_010C76; /* 010C40 bpls 0x10c76 */
L_010C42:;
	{ st16((ctx->a[6] + (uint32_t)(int32_t)2), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 010C42 clrw %fp@(2) */
	{ st16((ctx->a[6] + (uint32_t)(int32_t)4), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 010C46 clrw %fp@(4) */
	{ uint32_t _v = 0x0u; st16(0xFFFF9EA4u, _v); cc_logic16(ctx, _v); hle_insn(ctx, 20); } /* 010C4A movew #0,0xffff9ea4 */
	{ uint32_t _v = 0x80u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010C52 movew #128,%d0 */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_call68k(ctx, 0x013D04); } /* 010C56 jsr 0x13d04 */
	{ uint32_t _v = 0x10AF8u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 010C5C movel #68344,%fp@(32) */
	{ uint32_t _v = 0x3Cu; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 010C64 movew #60,%fp@(36) */
	{ uint32_t _s = 0xDu; uint32_t _da = 0xFFFFA23Cu; uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 20); } /* 010C6A cmpiw #13,0xffffa23c */
	if (bw(ctx, c_ge(ctx))) goto L_010E18; /* 010C72 bgew 0x10e18 */
L_010C76:;
	{ cc_logic8(ctx, ld8((ctx->a[6] + (uint32_t)(int32_t)10))); hle_insn(ctx, 12); } /* 010C76 tstb %fp@(10) */
	if (bs(ctx, !c_n(ctx))) goto L_010C86; /* 010C7A bpls 0x10c86 */
	{ uint32_t _v = 0x80u; st8((ctx->a[6] + (uint32_t)(int32_t)10), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 010C7C moveb #-128,%fp@(10) */
	{ st8((ctx->a[6] + (uint32_t)(int32_t)16), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 010C82 clrb %fp@(16) */
L_010C86:;
	{ cc_logic8(ctx, ld8((ctx->a[6] + (uint32_t)(int32_t)12))); hle_insn(ctx, 12); } /* 010C86 tstb %fp@(12) */
	if (bs(ctx, !c_n(ctx))) goto L_010C96; /* 010C8A bpls 0x10c96 */
	{ uint32_t _v = 0x80u; st8((ctx->a[6] + (uint32_t)(int32_t)12), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 010C8C moveb #-128,%fp@(12) */
	{ st8((ctx->a[6] + (uint32_t)(int32_t)18), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 010C92 clrb %fp@(18) */
L_010C96:;
	{ ctx->d[2] = 0x18u; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 010C96 moveq #24,%d2 */
	{ ctx->d[3] = 0x5u; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 010C98 moveq #5,%d3 */
	{ uint32_t _v = 0xEBD0u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010C9A movew #-5168,%d1 */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_call68k(ctx, 0x012212); } /* 010C9E jsr 0x12212 */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 010CA4 subqw #1,%fp@(36) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010CA8 movew %fp@(36),%d1 */
	{ uint32_t _s = 0xCu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 010CAC andiw #12,%d1 */
	{ ctx->a[1] = 0x010CFCu; hle_insn(ctx, 8); } /* 010CB0 lea %pc@(0x10cfc),%a1 */
	{ uint32_t _v = ld32((ctx->a[1] + (uint32_t)(int32_t)0 + (uint32_t)((int32_t)(int16_t)ctx->d[1]))); ctx->a[0] = _v; hle_insn(ctx, 18); } /* 010CB4 moveal %a1@(0,%d1:w),%a0 */
	{ ctx->d[3] = 0x5u; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 010CB8 moveq #5,%d3 */
	{ uint32_t _v = 0xEBD0u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010CBA movew #-5168,%d1 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010CBE movew %fp@(4),%d0 */
	{ uint32_t _s = 0x20u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 010CC2 cmpiw #32,%d0 */
	if (bs(ctx, c_ge(ctx))) goto L_010CE2; /* 010CC6 bges 0x10ce2 */
	{ sh_w(ctx, 0, 'a', 3); hle_insn(ctx, 12); } /* 010CC8 asrw #3,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 010CCA movew %d0,%d6 */
	{ uint32_t _s = 0x4u; uint32_t _d = (ctx->d[6] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[6], _r); hle_insn(ctx, 4); } /* 010CCC subqw #4,%d6 */
	{ uint32_t _r = alu(ctx, OP_SUB, 0, ctx->d[6], 16, 1); set_w(&ctx->d[6], _r); hle_insn(ctx, 4); } /* 010CCE negw %d6 */
	{ uint32_t _v = (ctx->d[6] & 0xFFFFu); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 010CD0 movew %d6,%d2 */
	{ sh_w(ctx, 6, 'l', 7); hle_insn(ctx, 20); } /* 010CD2 aslw #7,%d6 */
	{ mul_w(ctx, 2, 0x50, 0);  } /* 010CD4 muluw #80,%d2 */
	{ uint32_t _s = (ctx->d[6] & 0xFFFFu); uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 4); } /* 010CD8 addw %d6,%d1 */
	{ ctx->a[0] = (ctx->a[0] + (uint32_t)(int32_t)0 + (uint32_t)((int32_t)(int16_t)ctx->d[6])); hle_insn(ctx, 12); } /* 010CDA lea %a0@(0,%d6:w),%a0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 010CDE movew %d0,%d3 */
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[3], _r); hle_insn(ctx, 4); } /* 010CE0 addqw #1,%d3 */
L_010CE2:;
	{ uint32_t _v = 0x18u; set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010CE2 movew #24,%d2 */
	{ uint32_t _v = 0x2000u; set_w(&ctx->d[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010CE6 movew #8192,%d6 */
	{ uint32_t _v = (ctx->d[7] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010CEA movew %d7,%sp@- */
	{ uint32_t _v = ld16(0xFFFF9E34u); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 010CEC movew 0xffff9e34,%d7 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x012234); } /* 010CF2 jsr %pc@(0x12234) */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010CF6 movew %sp@+,%d7 */
	hle_insn(ctx, 10); goto L_010D20; /* 010CF8 braw 0x10d20 */
L_010D20:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010D20 movew %fp@(2),%d2 */
	if (bs(ctx, !c_n(ctx))) goto L_010D2C; /* 010D24 bpls 0x10d2c */
	{ set_w(&ctx->d[2], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 4); } /* 010D26 clrw %d2 */
	{ uint32_t _v = (ctx->d[2] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)2), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010D28 movew %d2,%fp@(2) */
L_010D2C:;
	{ hle_insn(ctx, 18); sub_009EF8(ctx); } /* 010D2C jsr %pc@(0x9ef8) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010D30 movew %fp@(4),%d0 */
	{ uint32_t _s = 0xFFu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 010D34 andiw #255,%d0 */
	{ uint32_t _s = 0xFFu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d ^ _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 010D38 eoriw #255,%d0 */
	{ ctx->d[1] = 0x0u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 010D3C moveq #0,%d1 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x011F58); } /* 010D3E jsr %pc@(0x11f58) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 010D42 braw 0xb3be */
L_010E18:;
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_call68k(ctx, 0x013E04); } /* 010E18 jsr 0x13e04 */
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 010E1E clrw %fp@ */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DAu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 010E20 subqw #1,0xffff80da */
	{ st16(0xFFFFA23Cu, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 20); } /* 010E26 clrw 0xffffa23c */
	{ st16(0xFFFFA242u, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 20); } /* 010E2C clrw 0xffffa242 */
	{ st16(0xFFFFA244u, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 20); } /* 010E32 clrw 0xffffa244 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x00AF72); } /* 010E38 bsrw 0xaf72 */
	{ ctx->a[0] = 0x00020CBEu; hle_insn(ctx, 12); } /* 010E3C lea 0x20cbe,%a0 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x00933E); } /* 010E42 jsr %pc@(0x933e) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 010E46 braw 0xb3be */
}

/* $010C1C $27 T-1000 on the police vehicle: fly away */
HLE_RANGE(0x010C1C, L7HeliFlyOff, 27100, EXIT_NEXT)
{
	{ uint32_t _v = 0x73u; st16(0xFFFF9EA4u, _v); cc_logic16(ctx, _v); hle_insn(ctx, 20); } /* 010C1C movew #115,0xffff9ea4 */
	{ hle_insn(ctx, 18); sub_00E10C(ctx); } /* 010C24 jsr %pc@(0xe10c) */
	{ hle_insn(ctx, 18); sub_00D236(ctx); } /* 010C28 jsr %pc@(0xd236) */
	{ cc_logic8(ctx, ld8((ctx->a[6] + (uint32_t)(int32_t)15))); hle_insn(ctx, 12); } /* 010C2C tstb %fp@(15) */
	if (bw(ctx, c_z(ctx))) goto L_010C3C; /* 010C30 beqw 0x10c3c */
	{ cc_logic16(ctx, ld16((ctx->a[6] + (uint32_t)(int32_t)4))); hle_insn(ctx, 12); } /* 010C34 tstw %fp@(4) */
	if (bs(ctx, !c_n(ctx))) goto L_010C76; /* 010C38 bpls 0x10c76 */
	hle_insn(ctx, 10); goto L_010C42; /* 010C3A bras 0x10c42 */
L_010C3C:;
	{ cc_logic16(ctx, ld16((ctx->a[6] + (uint32_t)(int32_t)2))); hle_insn(ctx, 12); } /* 010C3C tstw %fp@(2) */
	if (bs(ctx, !c_n(ctx))) goto L_010C76; /* 010C40 bpls 0x10c76 */
L_010C42:;
	{ st16((ctx->a[6] + (uint32_t)(int32_t)2), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 010C42 clrw %fp@(2) */
	{ st16((ctx->a[6] + (uint32_t)(int32_t)4), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 010C46 clrw %fp@(4) */
	{ uint32_t _v = 0x0u; st16(0xFFFF9EA4u, _v); cc_logic16(ctx, _v); hle_insn(ctx, 20); } /* 010C4A movew #0,0xffff9ea4 */
	{ uint32_t _v = 0x80u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010C52 movew #128,%d0 */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_call68k(ctx, 0x013D04); } /* 010C56 jsr 0x13d04 */
	{ uint32_t _v = 0x10AF8u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 010C5C movel #68344,%fp@(32) */
	{ uint32_t _v = 0x3Cu; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 010C64 movew #60,%fp@(36) */
	{ uint32_t _s = 0xDu; uint32_t _da = 0xFFFFA23Cu; uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 20); } /* 010C6A cmpiw #13,0xffffa23c */
	if (bw(ctx, c_ge(ctx))) goto L_010E18; /* 010C72 bgew 0x10e18 */
L_010C76:;
	{ cc_logic8(ctx, ld8((ctx->a[6] + (uint32_t)(int32_t)10))); hle_insn(ctx, 12); } /* 010C76 tstb %fp@(10) */
	if (bs(ctx, !c_n(ctx))) goto L_010C86; /* 010C7A bpls 0x10c86 */
	{ uint32_t _v = 0x80u; st8((ctx->a[6] + (uint32_t)(int32_t)10), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 010C7C moveb #-128,%fp@(10) */
	{ st8((ctx->a[6] + (uint32_t)(int32_t)16), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 010C82 clrb %fp@(16) */
L_010C86:;
	{ cc_logic8(ctx, ld8((ctx->a[6] + (uint32_t)(int32_t)12))); hle_insn(ctx, 12); } /* 010C86 tstb %fp@(12) */
	if (bs(ctx, !c_n(ctx))) goto L_010C96; /* 010C8A bpls 0x10c96 */
	{ uint32_t _v = 0x80u; st8((ctx->a[6] + (uint32_t)(int32_t)12), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 010C8C moveb #-128,%fp@(12) */
	{ st8((ctx->a[6] + (uint32_t)(int32_t)18), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 010C92 clrb %fp@(18) */
L_010C96:;
	{ ctx->d[2] = 0x18u; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 010C96 moveq #24,%d2 */
	{ ctx->d[3] = 0x5u; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 010C98 moveq #5,%d3 */
	{ uint32_t _v = 0xEBD0u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010C9A movew #-5168,%d1 */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_call68k(ctx, 0x012212); } /* 010C9E jsr 0x12212 */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 010CA4 subqw #1,%fp@(36) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010CA8 movew %fp@(36),%d1 */
	{ uint32_t _s = 0xCu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 010CAC andiw #12,%d1 */
	{ ctx->a[1] = 0x010CFCu; hle_insn(ctx, 8); } /* 010CB0 lea %pc@(0x10cfc),%a1 */
	{ uint32_t _v = ld32((ctx->a[1] + (uint32_t)(int32_t)0 + (uint32_t)((int32_t)(int16_t)ctx->d[1]))); ctx->a[0] = _v; hle_insn(ctx, 18); } /* 010CB4 moveal %a1@(0,%d1:w),%a0 */
	{ ctx->d[3] = 0x5u; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 010CB8 moveq #5,%d3 */
	{ uint32_t _v = 0xEBD0u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010CBA movew #-5168,%d1 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010CBE movew %fp@(4),%d0 */
	{ uint32_t _s = 0x20u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 010CC2 cmpiw #32,%d0 */
	if (bs(ctx, c_ge(ctx))) goto L_010CE2; /* 010CC6 bges 0x10ce2 */
	{ sh_w(ctx, 0, 'a', 3); hle_insn(ctx, 12); } /* 010CC8 asrw #3,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 010CCA movew %d0,%d6 */
	{ uint32_t _s = 0x4u; uint32_t _d = (ctx->d[6] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[6], _r); hle_insn(ctx, 4); } /* 010CCC subqw #4,%d6 */
	{ uint32_t _r = alu(ctx, OP_SUB, 0, ctx->d[6], 16, 1); set_w(&ctx->d[6], _r); hle_insn(ctx, 4); } /* 010CCE negw %d6 */
	{ uint32_t _v = (ctx->d[6] & 0xFFFFu); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 010CD0 movew %d6,%d2 */
	{ sh_w(ctx, 6, 'l', 7); hle_insn(ctx, 20); } /* 010CD2 aslw #7,%d6 */
	{ mul_w(ctx, 2, 0x50, 0);  } /* 010CD4 muluw #80,%d2 */
	{ uint32_t _s = (ctx->d[6] & 0xFFFFu); uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 4); } /* 010CD8 addw %d6,%d1 */
	{ ctx->a[0] = (ctx->a[0] + (uint32_t)(int32_t)0 + (uint32_t)((int32_t)(int16_t)ctx->d[6])); hle_insn(ctx, 12); } /* 010CDA lea %a0@(0,%d6:w),%a0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 010CDE movew %d0,%d3 */
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[3], _r); hle_insn(ctx, 4); } /* 010CE0 addqw #1,%d3 */
L_010CE2:;
	{ uint32_t _v = 0x18u; set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010CE2 movew #24,%d2 */
	{ uint32_t _v = 0x2000u; set_w(&ctx->d[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010CE6 movew #8192,%d6 */
	{ uint32_t _v = (ctx->d[7] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010CEA movew %d7,%sp@- */
	{ uint32_t _v = ld16(0xFFFF9E34u); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 010CEC movew 0xffff9e34,%d7 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x012234); } /* 010CF2 jsr %pc@(0x12234) */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010CF6 movew %sp@+,%d7 */
	hle_insn(ctx, 10); goto L_010D20; /* 010CF8 braw 0x10d20 */
L_010D20:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010D20 movew %fp@(2),%d2 */
	if (bs(ctx, !c_n(ctx))) goto L_010D2C; /* 010D24 bpls 0x10d2c */
	{ set_w(&ctx->d[2], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 4); } /* 010D26 clrw %d2 */
	{ uint32_t _v = (ctx->d[2] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)2), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010D28 movew %d2,%fp@(2) */
L_010D2C:;
	{ hle_insn(ctx, 18); sub_009EF8(ctx); } /* 010D2C jsr %pc@(0x9ef8) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010D30 movew %fp@(4),%d0 */
	{ uint32_t _s = 0xFFu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 010D34 andiw #255,%d0 */
	{ uint32_t _s = 0xFFu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d ^ _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 010D38 eoriw #255,%d0 */
	{ ctx->d[1] = 0x0u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 010D3C moveq #0,%d1 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x011F58); } /* 010D3E jsr %pc@(0x11f58) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 010D42 braw 0xb3be */
L_010E18:;
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_call68k(ctx, 0x013E04); } /* 010E18 jsr 0x13e04 */
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 010E1E clrw %fp@ */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DAu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 010E20 subqw #1,0xffff80da */
	{ st16(0xFFFFA23Cu, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 20); } /* 010E26 clrw 0xffffa23c */
	{ st16(0xFFFFA242u, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 20); } /* 010E2C clrw 0xffffa242 */
	{ st16(0xFFFFA244u, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 20); } /* 010E32 clrw 0xffffa244 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x00AF72); } /* 010E38 bsrw 0xaf72 */
	{ ctx->a[0] = 0x00020CBEu; hle_insn(ctx, 12); } /* 010E3C lea 0x20cbe,%a0 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x00933E); } /* 010E42 jsr %pc@(0x933e) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 010E46 braw 0xb3be */
}

/* $010D46 $27 T-1000 on the police vehicle: crash (sparks, tile rect) */
HLE_RANGE(0x010D46, L7HeliCrash, 14800, EXIT_NEXT)
{
	goto L_010D46;
L_010D20:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010D20 movew %fp@(2),%d2 */
	if (bs(ctx, !c_n(ctx))) goto L_010D2C; /* 010D24 bpls 0x10d2c */
	{ set_w(&ctx->d[2], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 4); } /* 010D26 clrw %d2 */
	{ uint32_t _v = (ctx->d[2] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)2), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010D28 movew %d2,%fp@(2) */
L_010D2C:;
	{ hle_insn(ctx, 18); sub_009EF8(ctx); } /* 010D2C jsr %pc@(0x9ef8) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010D30 movew %fp@(4),%d0 */
	{ uint32_t _s = 0xFFu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 010D34 andiw #255,%d0 */
	{ uint32_t _s = 0xFFu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d ^ _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 010D38 eoriw #255,%d0 */
	{ ctx->d[1] = 0x0u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 010D3C moveq #0,%d1 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x011F58); } /* 010D3E jsr %pc@(0x11f58) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 010D42 braw 0xb3be */
L_010D46:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010D46 movew %fp@(2),%d0 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010D4A movew %fp@(4),%d1 */
	{ ctx->d[2] = 0xFu; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 010D4E moveq #15,%d2 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x01253C); } /* 010D50 jsr %pc@(0x1253c) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010D54 movew %fp@(2),%d0 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010D58 movew %fp@(4),%d1 */
	{ ctx->d[2] = 0xFu; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 010D5C moveq #15,%d2 */
	{ uint32_t _s = 0x28u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 010D5E subiw #40,%d0 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x01253C); } /* 010D62 jsr %pc@(0x1253c) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010D66 movew %fp@(2),%d0 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010D6A movew %fp@(4),%d1 */
	{ ctx->d[2] = 0xFu; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 010D6E moveq #15,%d2 */
	{ uint32_t _s = 0x50u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 010D70 subiw #80,%d0 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x01253C); } /* 010D74 jsr %pc@(0x1253c) */
	{ uint32_t _v = 0x0u; st8((ctx->a[6] + (uint32_t)(int32_t)14), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 010D78 moveb #0,%fp@(14) */
	{ st8((ctx->a[6] + (uint32_t)(int32_t)11), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 010D7E clrb %fp@(11) */
	{ uint32_t _v = 0x0u; st8((ctx->a[6] + (uint32_t)(int32_t)12), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 010D82 moveb #0,%fp@(12) */
	{ uint32_t _v = 0xFEu; st8((ctx->a[6] + (uint32_t)(int32_t)18), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 010D88 moveb #-2,%fp@(18) */
	{ uint32_t _v = 0x1u; st8((ctx->a[6] + (uint32_t)(int32_t)15), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 010D8E moveb #1,%fp@(15) */
	{ st8((ctx->a[6] + (uint32_t)(int32_t)11), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 010D94 clrb %fp@(11) */
	{ uint32_t _v = 0xEBDAu; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010D98 movew #-5158,%d1 */
	{ ctx->d[2] = 0x4u; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 010D9C moveq #4,%d2 */
	{ ctx->d[3] = 0x2u; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 010D9E moveq #2,%d3 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x012212); } /* 010DA0 jsr %pc@(0x12212) */
	{ ctx->a[0] = 0x000CB0F4u; hle_insn(ctx, 12); } /* 010DA4 lea 0xcb0f4,%a0 */
	{ uint32_t _v = 0xEBE2u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010DAA movew #-5150,%d1 */
	{ ctx->d[3] = 0x9u; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 010DAE moveq #9,%d3 */
	{ ctx->d[2] = 0xFu; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 010DB0 moveq #15,%d2 */
	{ uint32_t _v = 0x2000u; set_w(&ctx->d[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010DB2 movew #8192,%d6 */
	{ uint32_t _v = (ctx->d[7] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010DB6 movew %d7,%sp@- */
	{ uint32_t _v = ld16(0xFFFF9E34u); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 010DB8 movew 0xffff9e34,%d7 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x012234); } /* 010DBE jsr %pc@(0x12234) */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010DC2 movew %sp@+,%d7 */
	{ uint32_t _v = 0x10DD6u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 010DC4 movel #69078,%fp@(32) */
	{ uint32_t _v = 0x76u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010DCC movew #118,%d0 */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_call68k(ctx, 0x013D04); } /* 010DD0 jsr 0x13d04 */
	{ hle_insn(ctx, 18); sub_00D236(ctx); } /* 010DD6 jsr %pc@(0xd236) */
	{ uint32_t _s = 0xB0u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)4); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 010DDA cmpiw #176,%fp@(4) */
	if (bs(ctx, !c_ge(ctx))) goto L_010DEA; /* 010DE0 blts 0x10dea */
	{ uint32_t _v = 0x10DEEu; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 010DE2 movel #69102,%fp@(32) */
L_010DEA:;
	hle_insn(ctx, 10); goto L_010D20; /* 010DEA braw 0x10d20 */
}

/* $010DD6 $27 T-1000 on the police vehicle: crash descent */
HLE_RANGE(0x010DD6, L7HeliCrash2, 2100, EXIT_NEXT)
{
	goto L_010DD6;
L_010D20:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010D20 movew %fp@(2),%d2 */
	if (bs(ctx, !c_n(ctx))) goto L_010D2C; /* 010D24 bpls 0x10d2c */
	{ set_w(&ctx->d[2], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 4); } /* 010D26 clrw %d2 */
	{ uint32_t _v = (ctx->d[2] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)2), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010D28 movew %d2,%fp@(2) */
L_010D2C:;
	{ hle_insn(ctx, 18); sub_009EF8(ctx); } /* 010D2C jsr %pc@(0x9ef8) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010D30 movew %fp@(4),%d0 */
	{ uint32_t _s = 0xFFu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 010D34 andiw #255,%d0 */
	{ uint32_t _s = 0xFFu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d ^ _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 010D38 eoriw #255,%d0 */
	{ ctx->d[1] = 0x0u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 010D3C moveq #0,%d1 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x011F58); } /* 010D3E jsr %pc@(0x11f58) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 010D42 braw 0xb3be */
L_010DD6:;
	{ hle_insn(ctx, 18); sub_00D236(ctx); } /* 010DD6 jsr %pc@(0xd236) */
	{ uint32_t _s = 0xB0u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)4); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 010DDA cmpiw #176,%fp@(4) */
	if (bs(ctx, !c_ge(ctx))) goto L_010DEA; /* 010DE0 blts 0x10dea */
	{ uint32_t _v = 0x10DEEu; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 010DE2 movel #69102,%fp@(32) */
L_010DEA:;
	hle_insn(ctx, 10); goto L_010D20; /* 010DEA braw 0x10d20 */
}

/* $010DEE $27 T-1000 on the police vehicle: scroll off, next script */
HLE_RANGE(0x010DEE, L7HeliCrash3, 2100, EXIT_NEXT)
{
	goto L_010DEE;
L_010D20:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010D20 movew %fp@(2),%d2 */
	if (bs(ctx, !c_n(ctx))) goto L_010D2C; /* 010D24 bpls 0x10d2c */
	{ set_w(&ctx->d[2], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 4); } /* 010D26 clrw %d2 */
	{ uint32_t _v = (ctx->d[2] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)2), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010D28 movew %d2,%fp@(2) */
L_010D2C:;
	{ hle_insn(ctx, 18); sub_009EF8(ctx); } /* 010D2C jsr %pc@(0x9ef8) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010D30 movew %fp@(4),%d0 */
	{ uint32_t _s = 0xFFu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 010D34 andiw #255,%d0 */
	{ uint32_t _s = 0xFFu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d ^ _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 010D38 eoriw #255,%d0 */
	{ ctx->d[1] = 0x0u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 010D3C moveq #0,%d1 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x011F58); } /* 010D3E jsr %pc@(0x11f58) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 010D42 braw 0xb3be */
L_010DEE:;
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 010DEE jsr %pc@(0x12316) */
	{ cc_logic16(ctx, ld16((ctx->a[6] + (uint32_t)(int32_t)2))); hle_insn(ctx, 12); } /* 010DF2 tstw %fp@(2) */
	if (bs(ctx, !c_n(ctx))) goto L_010E14; /* 010DF6 bpls 0x10e14 */
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 010DF8 clrw %fp@ */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DAu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 010DFA subqw #1,0xffff80da */
	{ st16((ctx->a[6] + (uint32_t)(int32_t)2), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 010E00 clrw %fp@(2) */
	{ uint32_t _v = 0x20CFEu; ctx->a[0] = _v; hle_insn(ctx, 12); } /* 010E04 moveal #134398,%a0 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x00933E); } /* 010E0A jsr %pc@(0x933e) */
	{ st16(0xFFFFA23Cu, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 20); } /* 010E0E clrw 0xffffa23c */
L_010E14:;
	hle_insn(ctx, 10); goto L_010D20; /* 010E14 braw 0x10d20 */
}

/* $010E4A $27 helicopter chase pass: init */
HLE_RANGE(0x010E4A, L7PassInit, 6500, EXIT_NEXT)
{
	goto L_010E4A;
L_010D20:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010D20 movew %fp@(2),%d2 */
	if (bs(ctx, !c_n(ctx))) goto L_010D2C; /* 010D24 bpls 0x10d2c */
	{ set_w(&ctx->d[2], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 4); } /* 010D26 clrw %d2 */
	{ uint32_t _v = (ctx->d[2] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)2), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010D28 movew %d2,%fp@(2) */
L_010D2C:;
	{ hle_insn(ctx, 18); sub_009EF8(ctx); } /* 010D2C jsr %pc@(0x9ef8) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010D30 movew %fp@(4),%d0 */
	{ uint32_t _s = 0xFFu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 010D34 andiw #255,%d0 */
	{ uint32_t _s = 0xFFu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d ^ _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 010D38 eoriw #255,%d0 */
	{ ctx->d[1] = 0x0u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 010D3C moveq #0,%d1 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x011F58); } /* 010D3E jsr %pc@(0x11f58) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 010D42 braw 0xb3be */
L_010E4A:;
	{ cc_logic16(ctx, ld16((ctx->a[6] + (uint32_t)(int32_t)36))); hle_insn(ctx, 12); } /* 010E4A tstw %fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_010E7E; /* 010E4E bnes 0x10e7e */
	{ uint32_t _v = 0x0u; st16((ctx->a[6] + (uint32_t)(int32_t)2), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 010E50 movew #0,%fp@(2) */
	{ uint32_t _v = 0x10E82u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 010E56 movel #69250,%fp@(32) */
	{ st16(0xFFFFA23Eu, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 20); } /* 010E5E clrw 0xffffa23e */
	{ st16(0xFFFFA240u, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 20); } /* 010E64 clrw 0xffffa240 */
	{ uint32_t _v = 0xD0u; st8((ctx->a[6] + (uint32_t)(int32_t)10), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 010E6A moveb #-48,%fp@(10) */
	{ st8((ctx->a[6] + (uint32_t)(int32_t)16), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 010E70 clrb %fp@(16) */
	{ uint32_t _v = 0x1u; st8((ctx->a[6] + (uint32_t)(int32_t)14), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 010E74 moveb #1,%fp@(14) */
	{ st8((ctx->a[6] + (uint32_t)(int32_t)11), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 010E7A clrb %fp@(11) */
L_010E7E:;
	hle_insn(ctx, 10); goto L_011092; /* 010E7E braw 0x11092 */
L_011092:;
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 011092 subqw #1,%fp@(36) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)20)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 011096 movew %fp@(20),%d0 */
	if (bs(ctx, c_z(ctx))) goto L_0110DE; /* 01109A beqs 0x110de */
	if (bs(ctx, !c_n(ctx))) goto L_0110AE; /* 01109C bpls 0x110ae */
	{ ctx->a[0] = 0x000A1152u; hle_insn(ctx, 12); } /* 01109E lea 0xa1152,%a0 */
	{ hle_insn(ctx, 18); sub_01114C(ctx); } /* 0110A4 bsrw 0x1114c */
	{ uint32_t _v = 0x7u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0110A8 movew #7,%d0 */
	hle_insn(ctx, 10); goto L_0110DE; /* 0110AC bras 0x110de */
L_0110AE:;
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 0110AE subqw #1,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_0110DE; /* 0110B0 bnes 0x110de */
	{ ctx->a[0] = 0x000A12E2u; hle_insn(ctx, 12); } /* 0110B2 lea 0xa12e2,%a0 */
	{ uint32_t _s = 0x28u; uint32_t _da = 0xFFFFA242u; uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 20); } /* 0110B8 cmpiw #40,0xffffa242 */
	if (bs(ctx, !c_ge(ctx))) goto L_0110D8; /* 0110C0 blts 0x110d8 */
	{ ctx->a[0] = 0x000A1472u; hle_insn(ctx, 12); } /* 0110C2 lea 0xa1472,%a0 */
	{ uint32_t _s = 0x64u; uint32_t _da = 0xFFFFA242u; uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 20); } /* 0110C8 cmpiw #100,0xffffa242 */
	if (bs(ctx, !c_ge(ctx))) goto L_0110D8; /* 0110D0 blts 0x110d8 */
	{ ctx->a[0] = 0x000A1364u; hle_insn(ctx, 12); } /* 0110D2 lea 0xa1364,%a0 */
L_0110D8:;
	{ hle_insn(ctx, 18); sub_01114C(ctx); } /* 0110D8 bsrw 0x1114c */
	{ ctx->d[0] = 0x0u; cc_logic32(ctx, ctx->d[0]); hle_insn(ctx, 4); } /* 0110DC moveq #0,%d0 */
L_0110DE:;
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)20), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0110DE movew %d0,%fp@(20) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)26)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0110E2 movew %fp@(26),%d0 */
	if (bs(ctx, c_z(ctx))) goto L_01112A; /* 0110E6 beqs 0x1112a */
	if (bs(ctx, !c_n(ctx))) goto L_0110FA; /* 0110E8 bpls 0x110fa */
	{ ctx->a[0] = 0x000A115Cu; hle_insn(ctx, 12); } /* 0110EA lea 0xa115c,%a0 */
	{ hle_insn(ctx, 18); sub_011146(ctx); } /* 0110F0 bsrw 0x11146 */
	{ uint32_t _v = 0x7u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0110F4 movew #7,%d0 */
	hle_insn(ctx, 10); goto L_01112A; /* 0110F8 bras 0x1112a */
L_0110FA:;
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 0110FA subqw #1,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_01112A; /* 0110FC bnes 0x1112a */
	{ ctx->a[0] = 0x000A12ECu; hle_insn(ctx, 12); } /* 0110FE lea 0xa12ec,%a0 */
	{ uint32_t _s = 0x28u; uint32_t _da = 0xFFFFA242u; uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 20); } /* 011104 cmpiw #40,0xffffa242 */
	if (bs(ctx, !c_ge(ctx))) goto L_011124; /* 01110C blts 0x11124 */
	{ ctx->a[0] = 0x000A147Cu; hle_insn(ctx, 12); } /* 01110E lea 0xa147c,%a0 */
	{ uint32_t _s = 0x64u; uint32_t _da = 0xFFFFA242u; uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 20); } /* 011114 cmpiw #100,0xffffa242 */
	if (bs(ctx, !c_ge(ctx))) goto L_011124; /* 01111C blts 0x11124 */
	{ ctx->a[0] = 0x000A136Eu; hle_insn(ctx, 12); } /* 01111E lea 0xa136e,%a0 */
L_011124:;
	{ hle_insn(ctx, 18); sub_011146(ctx); } /* 011124 bsrw 0x11146 */
	{ ctx->d[0] = 0x0u; cc_logic32(ctx, ctx->d[0]); hle_insn(ctx, 4); } /* 011128 moveq #0,%d0 */
L_01112A:;
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)26), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01112A movew %d0,%fp@(26) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01112E movew %fp@(2),%d2 */
	{ uint32_t _s = 0x168u; uint32_t _d = (ctx->d[2] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 011132 cmpiw #360,%d2 */
	if (bw(ctx, c_gt(ctx))) goto L_011142; /* 011136 bgtw 0x11142 */
	{ uint32_t _s = 0x22u; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 01113A subiw #34,%d2 */
	{ hle_insn(ctx, 18); sub_011072(ctx); } /* 01113E bsrw 0x11072 */
L_011142:;
	hle_insn(ctx, 10); goto L_010D20; /* 011142 braw 0x10d20 */
}

/* $010E82 $27 helicopter chase pass: check X >= $88 (HeliPassCheck) */
HLE_RANGE(0x010E82, L7PassCheck, 3200, EXIT_NEXT)
{
	goto L_010E82;
L_010D20:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010D20 movew %fp@(2),%d2 */
	if (bs(ctx, !c_n(ctx))) goto L_010D2C; /* 010D24 bpls 0x10d2c */
	{ set_w(&ctx->d[2], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 4); } /* 010D26 clrw %d2 */
	{ uint32_t _v = (ctx->d[2] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)2), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010D28 movew %d2,%fp@(2) */
L_010D2C:;
	{ hle_insn(ctx, 18); sub_009EF8(ctx); } /* 010D2C jsr %pc@(0x9ef8) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010D30 movew %fp@(4),%d0 */
	{ uint32_t _s = 0xFFu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 010D34 andiw #255,%d0 */
	{ uint32_t _s = 0xFFu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d ^ _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 010D38 eoriw #255,%d0 */
	{ ctx->d[1] = 0x0u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 010D3C moveq #0,%d1 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x011F58); } /* 010D3E jsr %pc@(0x11f58) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 010D42 braw 0xb3be */
L_010E82:;
	{ hle_insn(ctx, 18); sub_00E10C(ctx); } /* 010E82 jsr %pc@(0xe10c) */
	{ uint32_t _s = 0x88u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)2); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 010E86 cmpiw #136,%fp@(2) */
	if (bs(ctx, !c_ge(ctx))) goto L_010EF2; /* 010E8C blts 0x10ef2 */
	{ uint32_t _v = 0x0u; st8((ctx->a[6] + (uint32_t)(int32_t)10), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 010E8E moveb #0,%fp@(10) */
	{ uint32_t _v = 0xFEu; st8((ctx->a[6] + (uint32_t)(int32_t)16), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 010E94 moveb #-2,%fp@(16) */
	{ uint32_t _v = 0xFFu; st8((ctx->a[6] + (uint32_t)(int32_t)14), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 010E9A moveb #-1,%fp@(14) */
	{ st8((ctx->a[6] + (uint32_t)(int32_t)11), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 010EA0 clrb %fp@(11) */
	{ uint32_t _v = 0x10EF6u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 010EA4 movel #69366,%fp@(32) */
	{ uint32_t _v = ld32(0xFFFF9ECAu); ctx->a[5] = _v; hle_insn(ctx, 20); } /* 010EAC moveal 0xffff9eca,%a5 */
	{ uint32_t _v = 0x10AB4u; st32((ctx->a[5] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 010EB2 movel #68276,%a5@(32) */
	{ uint32_t _s = 0x4u; uint32_t _da = 0xFFFFA23Eu; uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 20); } /* 010EBA cmpiw #4,0xffffa23e */
	if (bs(ctx, c_gt(ctx))) goto L_010EDC; /* 010EC2 bgts 0x10edc */
	{ uint32_t _v = 0x109C8u; st32((ctx->a[5] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 010EC4 movel #68040,%a5@(32) */
	{ uint32_t _v = 0x42u; st16((ctx->a[5] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 010ECC movew #66,%a5@(36) */
	{ uint32_t _v = 0x0u; st32(0xFFFF9ECEu, _v); cc_logic32(ctx, _v); hle_insn(ctx, 28); } /* 010ED2 movel #0,0xffff9ece */
L_010EDC:;
	{ uint32_t _v = 0xA0u; st8((ctx->a[5] + (uint32_t)(int32_t)10), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 010EDC moveb #-96,%a5@(10) */
	{ uint32_t _v = 0x4u; st8((ctx->a[5] + (uint32_t)(int32_t)16), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 010EE2 moveb #4,%a5@(16) */
	{ uint32_t _v = 0x1u; st8((ctx->a[5] + (uint32_t)(int32_t)14), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 010EE8 moveb #1,%a5@(14) */
	{ st8((ctx->a[5] + (uint32_t)(int32_t)11), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 010EEE clrb %a5@(11) */
L_010EF2:;
	hle_insn(ctx, 10); goto L_011092; /* 010EF2 braw 0x11092 */
L_011092:;
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 011092 subqw #1,%fp@(36) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)20)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 011096 movew %fp@(20),%d0 */
	if (bs(ctx, c_z(ctx))) goto L_0110DE; /* 01109A beqs 0x110de */
	if (bs(ctx, !c_n(ctx))) goto L_0110AE; /* 01109C bpls 0x110ae */
	{ ctx->a[0] = 0x000A1152u; hle_insn(ctx, 12); } /* 01109E lea 0xa1152,%a0 */
	{ hle_insn(ctx, 18); sub_01114C(ctx); } /* 0110A4 bsrw 0x1114c */
	{ uint32_t _v = 0x7u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0110A8 movew #7,%d0 */
	hle_insn(ctx, 10); goto L_0110DE; /* 0110AC bras 0x110de */
L_0110AE:;
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 0110AE subqw #1,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_0110DE; /* 0110B0 bnes 0x110de */
	{ ctx->a[0] = 0x000A12E2u; hle_insn(ctx, 12); } /* 0110B2 lea 0xa12e2,%a0 */
	{ uint32_t _s = 0x28u; uint32_t _da = 0xFFFFA242u; uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 20); } /* 0110B8 cmpiw #40,0xffffa242 */
	if (bs(ctx, !c_ge(ctx))) goto L_0110D8; /* 0110C0 blts 0x110d8 */
	{ ctx->a[0] = 0x000A1472u; hle_insn(ctx, 12); } /* 0110C2 lea 0xa1472,%a0 */
	{ uint32_t _s = 0x64u; uint32_t _da = 0xFFFFA242u; uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 20); } /* 0110C8 cmpiw #100,0xffffa242 */
	if (bs(ctx, !c_ge(ctx))) goto L_0110D8; /* 0110D0 blts 0x110d8 */
	{ ctx->a[0] = 0x000A1364u; hle_insn(ctx, 12); } /* 0110D2 lea 0xa1364,%a0 */
L_0110D8:;
	{ hle_insn(ctx, 18); sub_01114C(ctx); } /* 0110D8 bsrw 0x1114c */
	{ ctx->d[0] = 0x0u; cc_logic32(ctx, ctx->d[0]); hle_insn(ctx, 4); } /* 0110DC moveq #0,%d0 */
L_0110DE:;
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)20), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0110DE movew %d0,%fp@(20) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)26)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0110E2 movew %fp@(26),%d0 */
	if (bs(ctx, c_z(ctx))) goto L_01112A; /* 0110E6 beqs 0x1112a */
	if (bs(ctx, !c_n(ctx))) goto L_0110FA; /* 0110E8 bpls 0x110fa */
	{ ctx->a[0] = 0x000A115Cu; hle_insn(ctx, 12); } /* 0110EA lea 0xa115c,%a0 */
	{ hle_insn(ctx, 18); sub_011146(ctx); } /* 0110F0 bsrw 0x11146 */
	{ uint32_t _v = 0x7u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0110F4 movew #7,%d0 */
	hle_insn(ctx, 10); goto L_01112A; /* 0110F8 bras 0x1112a */
L_0110FA:;
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 0110FA subqw #1,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_01112A; /* 0110FC bnes 0x1112a */
	{ ctx->a[0] = 0x000A12ECu; hle_insn(ctx, 12); } /* 0110FE lea 0xa12ec,%a0 */
	{ uint32_t _s = 0x28u; uint32_t _da = 0xFFFFA242u; uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 20); } /* 011104 cmpiw #40,0xffffa242 */
	if (bs(ctx, !c_ge(ctx))) goto L_011124; /* 01110C blts 0x11124 */
	{ ctx->a[0] = 0x000A147Cu; hle_insn(ctx, 12); } /* 01110E lea 0xa147c,%a0 */
	{ uint32_t _s = 0x64u; uint32_t _da = 0xFFFFA242u; uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 20); } /* 011114 cmpiw #100,0xffffa242 */
	if (bs(ctx, !c_ge(ctx))) goto L_011124; /* 01111C blts 0x11124 */
	{ ctx->a[0] = 0x000A136Eu; hle_insn(ctx, 12); } /* 01111E lea 0xa136e,%a0 */
L_011124:;
	{ hle_insn(ctx, 18); sub_011146(ctx); } /* 011124 bsrw 0x11146 */
	{ ctx->d[0] = 0x0u; cc_logic32(ctx, ctx->d[0]); hle_insn(ctx, 4); } /* 011128 moveq #0,%d0 */
L_01112A:;
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)26), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01112A movew %d0,%fp@(26) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01112E movew %fp@(2),%d2 */
	{ uint32_t _s = 0x168u; uint32_t _d = (ctx->d[2] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 011132 cmpiw #360,%d2 */
	if (bw(ctx, c_gt(ctx))) goto L_011142; /* 011136 bgtw 0x11142 */
	{ uint32_t _s = 0x22u; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 01113A subiw #34,%d2 */
	{ hle_insn(ctx, 18); sub_011072(ctx); } /* 01113E bsrw 0x11072 */
L_011142:;
	hle_insn(ctx, 10); goto L_010D20; /* 011142 braw 0x10d20 */
}

/* $010EF6 $27 helicopter chase pass: wrap up */
HLE_RANGE(0x010EF6, L7PassEnd, 7200, EXIT_NEXT)
{
	goto L_010EF6;
L_010D20:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010D20 movew %fp@(2),%d2 */
	if (bs(ctx, !c_n(ctx))) goto L_010D2C; /* 010D24 bpls 0x10d2c */
	{ set_w(&ctx->d[2], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 4); } /* 010D26 clrw %d2 */
	{ uint32_t _v = (ctx->d[2] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)2), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010D28 movew %d2,%fp@(2) */
L_010D2C:;
	{ hle_insn(ctx, 18); sub_009EF8(ctx); } /* 010D2C jsr %pc@(0x9ef8) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010D30 movew %fp@(4),%d0 */
	{ uint32_t _s = 0xFFu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 010D34 andiw #255,%d0 */
	{ uint32_t _s = 0xFFu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d ^ _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 010D38 eoriw #255,%d0 */
	{ ctx->d[1] = 0x0u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 010D3C moveq #0,%d1 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x011F58); } /* 010D3E jsr %pc@(0x11f58) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 010D42 braw 0xb3be */
L_010EF6:;
	{ hle_insn(ctx, 18); sub_00E10C(ctx); } /* 010EF6 jsr %pc@(0xe10c) */
	{ cc_logic16(ctx, ld16((ctx->a[6] + (uint32_t)(int32_t)2))); hle_insn(ctx, 12); } /* 010EFA tstw %fp@(2) */
	if (bs(ctx, !c_n(ctx))) goto L_010F72; /* 010EFE bpls 0x10f72 */
	{ st16((ctx->a[6] + (uint32_t)(int32_t)2), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 010F00 clrw %fp@(2) */
	{ cc_logic32(ctx, ld32(0xFFFF9ECEu)); hle_insn(ctx, 20); } /* 010F04 tstl 0xffff9ece */
	if (bs(ctx, !c_z(ctx))) goto L_010F22; /* 010F0A bnes 0x10f22 */
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 010F0C clrw %fp@ */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DAu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 010F0E subqw #1,0xffff80da */
	{ uint32_t _v = 0x20CFEu; ctx->a[0] = _v; hle_insn(ctx, 12); } /* 010F14 moveal #134398,%a0 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x00933E); } /* 010F1A jsr %pc@(0x933e) */
	hle_insn(ctx, 10); goto L_010F82; /* 010F1E braw 0x10f82 */
L_010F22:;
	{ uint32_t _v = 0x10E4Au; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 010F22 movel #69194,%fp@(32) */
	{ uint32_t _v = 0x64u; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 010F2A movew #100,%fp@(36) */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFFA23Cu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 010F30 addqw #1,0xffffa23c */
	{ uint32_t _s = 0x9u; uint32_t _da = 0xFFFFA23Cu; uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 20); } /* 010F36 cmpiw #9,0xffffa23c */
	if (bs(ctx, !c_ge(ctx))) goto L_010F72; /* 010F3E blts 0x10f72 */
	{ uint32_t _v = ld32(0xFFFF9ECAu); ctx->a[5] = _v; hle_insn(ctx, 20); } /* 010F40 moveal 0xffff9eca,%a5 */
	{ uint32_t _v = 0x10AE0u; st32((ctx->a[5] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 010F46 movel #68320,%a5@(32) */
	{ uint32_t _v = 0x80u; st8((ctx->a[5] + (uint32_t)(int32_t)10), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 010F4E moveb #-128,%a5@(10) */
	{ uint32_t _v = 0x0u; st8((ctx->a[5] + (uint32_t)(int32_t)16), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 010F54 moveb #0,%a5@(16) */
	{ uint32_t _v = 0x1u; st8((ctx->a[5] + (uint32_t)(int32_t)14), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 010F5A moveb #1,%a5@(14) */
	{ st8((ctx->a[5] + (uint32_t)(int32_t)11), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 010F60 clrb %a5@(11) */
	{ uint32_t _v = 0x10F86u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 010F64 movel #69510,%fp@(32) */
	{ uint32_t _v = 0x32u; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 010F6C movew #50,%fp@(36) */
L_010F72:;
	{ cc_logic8(ctx, ld8((ctx->a[6] + (uint32_t)(int32_t)10))); hle_insn(ctx, 12); } /* 010F72 tstb %fp@(10) */
	if (bs(ctx, !c_n(ctx))) goto L_010F82; /* 010F76 bpls 0x10f82 */
	{ uint32_t _v = 0x80u; st8((ctx->a[6] + (uint32_t)(int32_t)10), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 010F78 moveb #-128,%fp@(10) */
	{ st8((ctx->a[6] + (uint32_t)(int32_t)16), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 010F7E clrb %fp@(16) */
L_010F82:;
	hle_insn(ctx, 10); goto L_011092; /* 010F82 braw 0x11092 */
L_011092:;
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 011092 subqw #1,%fp@(36) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)20)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 011096 movew %fp@(20),%d0 */
	if (bs(ctx, c_z(ctx))) goto L_0110DE; /* 01109A beqs 0x110de */
	if (bs(ctx, !c_n(ctx))) goto L_0110AE; /* 01109C bpls 0x110ae */
	{ ctx->a[0] = 0x000A1152u; hle_insn(ctx, 12); } /* 01109E lea 0xa1152,%a0 */
	{ hle_insn(ctx, 18); sub_01114C(ctx); } /* 0110A4 bsrw 0x1114c */
	{ uint32_t _v = 0x7u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0110A8 movew #7,%d0 */
	hle_insn(ctx, 10); goto L_0110DE; /* 0110AC bras 0x110de */
L_0110AE:;
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 0110AE subqw #1,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_0110DE; /* 0110B0 bnes 0x110de */
	{ ctx->a[0] = 0x000A12E2u; hle_insn(ctx, 12); } /* 0110B2 lea 0xa12e2,%a0 */
	{ uint32_t _s = 0x28u; uint32_t _da = 0xFFFFA242u; uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 20); } /* 0110B8 cmpiw #40,0xffffa242 */
	if (bs(ctx, !c_ge(ctx))) goto L_0110D8; /* 0110C0 blts 0x110d8 */
	{ ctx->a[0] = 0x000A1472u; hle_insn(ctx, 12); } /* 0110C2 lea 0xa1472,%a0 */
	{ uint32_t _s = 0x64u; uint32_t _da = 0xFFFFA242u; uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 20); } /* 0110C8 cmpiw #100,0xffffa242 */
	if (bs(ctx, !c_ge(ctx))) goto L_0110D8; /* 0110D0 blts 0x110d8 */
	{ ctx->a[0] = 0x000A1364u; hle_insn(ctx, 12); } /* 0110D2 lea 0xa1364,%a0 */
L_0110D8:;
	{ hle_insn(ctx, 18); sub_01114C(ctx); } /* 0110D8 bsrw 0x1114c */
	{ ctx->d[0] = 0x0u; cc_logic32(ctx, ctx->d[0]); hle_insn(ctx, 4); } /* 0110DC moveq #0,%d0 */
L_0110DE:;
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)20), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0110DE movew %d0,%fp@(20) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)26)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0110E2 movew %fp@(26),%d0 */
	if (bs(ctx, c_z(ctx))) goto L_01112A; /* 0110E6 beqs 0x1112a */
	if (bs(ctx, !c_n(ctx))) goto L_0110FA; /* 0110E8 bpls 0x110fa */
	{ ctx->a[0] = 0x000A115Cu; hle_insn(ctx, 12); } /* 0110EA lea 0xa115c,%a0 */
	{ hle_insn(ctx, 18); sub_011146(ctx); } /* 0110F0 bsrw 0x11146 */
	{ uint32_t _v = 0x7u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0110F4 movew #7,%d0 */
	hle_insn(ctx, 10); goto L_01112A; /* 0110F8 bras 0x1112a */
L_0110FA:;
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 0110FA subqw #1,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_01112A; /* 0110FC bnes 0x1112a */
	{ ctx->a[0] = 0x000A12ECu; hle_insn(ctx, 12); } /* 0110FE lea 0xa12ec,%a0 */
	{ uint32_t _s = 0x28u; uint32_t _da = 0xFFFFA242u; uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 20); } /* 011104 cmpiw #40,0xffffa242 */
	if (bs(ctx, !c_ge(ctx))) goto L_011124; /* 01110C blts 0x11124 */
	{ ctx->a[0] = 0x000A147Cu; hle_insn(ctx, 12); } /* 01110E lea 0xa147c,%a0 */
	{ uint32_t _s = 0x64u; uint32_t _da = 0xFFFFA242u; uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 20); } /* 011114 cmpiw #100,0xffffa242 */
	if (bs(ctx, !c_ge(ctx))) goto L_011124; /* 01111C blts 0x11124 */
	{ ctx->a[0] = 0x000A136Eu; hle_insn(ctx, 12); } /* 01111E lea 0xa136e,%a0 */
L_011124:;
	{ hle_insn(ctx, 18); sub_011146(ctx); } /* 011124 bsrw 0x11146 */
	{ ctx->d[0] = 0x0u; cc_logic32(ctx, ctx->d[0]); hle_insn(ctx, 4); } /* 011128 moveq #0,%d0 */
L_01112A:;
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)26), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01112A movew %d0,%fp@(26) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01112E movew %fp@(2),%d2 */
	{ uint32_t _s = 0x168u; uint32_t _d = (ctx->d[2] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 011132 cmpiw #360,%d2 */
	if (bw(ctx, c_gt(ctx))) goto L_011142; /* 011136 bgtw 0x11142 */
	{ uint32_t _s = 0x22u; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 01113A subiw #34,%d2 */
	{ hle_insn(ctx, 18); sub_011072(ctx); } /* 01113E bsrw 0x11072 */
L_011142:;
	hle_insn(ctx, 10); goto L_010D20; /* 011142 braw 0x10d20 */
}

/* $010F86 $27 helicopter chase: countdown to the finale */
HLE_RANGE_PURE(0x010F86, L7PassEnd2, 1000, EXIT_NEXT)
{
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 010F86 subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_010FAC; /* 010F8A bnes 0x10fac */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o & 0x7F); hle_insn(ctx, 16); } /* 010F8C bclr #15,%fp@ */
	{ uint32_t _v = 0x10FB0u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 010F90 movel #69552,%fp@(32) */
	{ uint32_t _v = 0x80u; st8((ctx->a[6] + (uint32_t)(int32_t)10), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 010F98 moveb #-128,%fp@(10) */
	{ st8((ctx->a[6] + (uint32_t)(int32_t)16), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 010F9E clrb %fp@(16) */
	{ uint32_t _v = 0x1u; st8((ctx->a[6] + (uint32_t)(int32_t)14), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 010FA2 moveb #1,%fp@(14) */
	{ st8((ctx->a[6] + (uint32_t)(int32_t)11), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 010FA8 clrb %fp@(11) */
L_010FAC:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 010FAC braw 0xb3be */
}

/* $010FB0 $27 freeway scroller finale */
HLE_RANGE(0x010FB0, L7Finale, 10000, EXIT_NEXT)
{
	goto L_010FB0;
L_010D20:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010D20 movew %fp@(2),%d2 */
	if (bs(ctx, !c_n(ctx))) goto L_010D2C; /* 010D24 bpls 0x10d2c */
	{ set_w(&ctx->d[2], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 4); } /* 010D26 clrw %d2 */
	{ uint32_t _v = (ctx->d[2] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)2), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010D28 movew %d2,%fp@(2) */
L_010D2C:;
	{ hle_insn(ctx, 18); sub_009EF8(ctx); } /* 010D2C jsr %pc@(0x9ef8) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010D30 movew %fp@(4),%d0 */
	{ uint32_t _s = 0xFFu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 010D34 andiw #255,%d0 */
	{ uint32_t _s = 0xFFu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d ^ _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 010D38 eoriw #255,%d0 */
	{ ctx->d[1] = 0x0u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 010D3C moveq #0,%d1 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x011F58); } /* 010D3E jsr %pc@(0x11f58) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 010D42 braw 0xb3be */
L_010FB0:;
	{ hle_insn(ctx, 18); sub_00E10C(ctx); } /* 010FB0 jsr %pc@(0xe10c) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010FB4 movew %fp@(2),%d2 */
	{ uint32_t _v = (ctx->d[2] & 0xFFFFu); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 010FB8 movew %d2,%d0 */
	{ uint32_t _s = 0x3Fu; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 010FBA andiw #63,%d2 */
	if (bw(ctx, !c_z(ctx))) goto L_011012; /* 010FBE bnew 0x11012 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 010FC2 movew %d0,%d2 */
	{ uint32_t _s = 0x1FFu; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 010FC4 andiw #511,%d2 */
	{ sh_w(ctx, 2, 'a', 2); hle_insn(ctx, 10); } /* 010FC8 asrw #2,%d2 */
	{ uint32_t _v = 0xE9D6u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010FCA movew #-5674,%d1 */
	{ uint32_t _s = (ctx->d[2] & 0xFFFFu); uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 4); } /* 010FCE subw %d2,%d1 */
	{ ctx->a[0] = 0x000A1652u; hle_insn(ctx, 12); } /* 010FD0 lea 0xa1652,%a0 */
	{ uint32_t _r = alu(ctx, OP_SUB, 0, ctx->d[2], 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 4); } /* 010FD6 negw %d2 */
	{ ctx->a[0] = (ctx->a[0] + (uint32_t)(int32_t)0 + (uint32_t)((int32_t)(int16_t)ctx->d[2])); hle_insn(ctx, 12); } /* 010FD8 lea %a0@(0,%d2:w),%a0 */
	{ ctx->d[2] = 0x8u; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 010FDC moveq #8,%d2 */
	{ ctx->d[3] = 0xDu; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 010FDE moveq #13,%d3 */
	{ uint32_t _v = 0xA000u; ctx->d[6] = _v; cc_logic32(ctx, _v); hle_insn(ctx, 12); } /* 010FE0 movel #40960,%d6 */
	{ uint32_t _v = (ctx->d[7] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010FE6 movew %d7,%sp@- */
	{ uint32_t _v = ld16(0xFFFF9E74u); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 010FE8 movew 0xffff9e74,%d7 */
	{ uint32_t _s = 0x160u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 010FEE cmpiw #352,%d0 */
	if (bs(ctx, c_gt(ctx))) goto L_010FFA; /* 010FF2 bgts 0x10ffa */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x012234); } /* 010FF4 jsr %pc@(0x12234) */
	hle_insn(ctx, 10); goto L_011010; /* 010FF8 bras 0x11010 */
L_010FFA:;
	{ uint32_t _s = 0xE9B6u; uint32_t _d = (ctx->d[1] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 010FFA cmpiw #-5706,%d1 */
	if (bs(ctx, !c_z(ctx))) goto L_011004; /* 010FFE bnes 0x11004 */
	{ uint32_t _s = 0x80u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 011000 subiw #128,%d1 */
L_011004:;
	{ uint32_t _s = 0x200u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 011004 subiw #512,%d1 */
	{ uint32_t _s = 0x5u; uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[3], _r); hle_insn(ctx, 8); } /* 011008 addiw #5,%d3 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x012212); } /* 01100C jsr %pc@(0x12212) */
L_011010:;
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011010 movew %sp@+,%d7 */
L_011012:;
	{ uint32_t _s = 0x334u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 011012 cmpiw #820,%d0 */
	if (bs(ctx, !c_ge(ctx))) goto L_011032; /* 011016 blts 0x11032 */
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 011018 clrw %fp@ */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DAu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 01101A subqw #1,0xffff80da */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_call68k(ctx, 0x010D0C); } /* 011020 jsr 0x10d0c */
	{ uint32_t _v = 0xFFu; st8(0xFFFFA234u, _v); cc_logic8(ctx, _v); hle_insn(ctx, 20); } /* 011026 moveb #-1,0xffffa234 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 01102E braw 0xb3be */
L_011032:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 011032 movew %fp@(2),%d2 */
	{ uint32_t _s = 0x50u; uint32_t _d = (ctx->d[2] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 011036 cmpiw #80,%d2 */
	if (bs(ctx, !c_ge(ctx))) goto L_011054; /* 01103A blts 0x11054 */
	{ uint32_t _s = 0x206u; uint32_t _d = (ctx->d[2] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 01103C cmpiw #518,%d2 */
	if (bw(ctx, c_gt(ctx))) goto L_011054; /* 011040 bgtw 0x11054 */
	{ uint32_t _s = 0x8Cu; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 011044 subiw #140,%d2 */
	{ hle_insn(ctx, 18); sub_011072(ctx); } /* 011048 bsrw 0x11072 */
	{ uint32_t _s = 0x2Au; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 01104C subiw #42,%d2 */
	{ hle_insn(ctx, 18); sub_011072(ctx); } /* 011050 bsrw 0x11072 */
L_011054:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 011054 movew %fp@(2),%d2 */
	{ uint32_t _s = 0x168u; uint32_t _d = (ctx->d[2] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 011058 cmpiw #360,%d2 */
	if (bs(ctx, !c_ge(ctx))) goto L_01106E; /* 01105C blts 0x1106e */
	{ uint32_t _s = 0x19Au; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 01105E subiw #410,%d2 */
	{ hle_insn(ctx, 18); sub_011072(ctx); } /* 011062 bsrw 0x11072 */
	{ uint32_t _s = 0x2Au; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 011066 addiw #42,%d2 */
	{ hle_insn(ctx, 18); sub_011072(ctx); } /* 01106A bsrw 0x11072 */
L_01106E:;
	hle_insn(ctx, 10); goto L_011092; /* 01106E braw 0x11092 */
L_011092:;
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 011092 subqw #1,%fp@(36) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)20)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 011096 movew %fp@(20),%d0 */
	if (bs(ctx, c_z(ctx))) goto L_0110DE; /* 01109A beqs 0x110de */
	if (bs(ctx, !c_n(ctx))) goto L_0110AE; /* 01109C bpls 0x110ae */
	{ ctx->a[0] = 0x000A1152u; hle_insn(ctx, 12); } /* 01109E lea 0xa1152,%a0 */
	{ hle_insn(ctx, 18); sub_01114C(ctx); } /* 0110A4 bsrw 0x1114c */
	{ uint32_t _v = 0x7u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0110A8 movew #7,%d0 */
	hle_insn(ctx, 10); goto L_0110DE; /* 0110AC bras 0x110de */
L_0110AE:;
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 0110AE subqw #1,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_0110DE; /* 0110B0 bnes 0x110de */
	{ ctx->a[0] = 0x000A12E2u; hle_insn(ctx, 12); } /* 0110B2 lea 0xa12e2,%a0 */
	{ uint32_t _s = 0x28u; uint32_t _da = 0xFFFFA242u; uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 20); } /* 0110B8 cmpiw #40,0xffffa242 */
	if (bs(ctx, !c_ge(ctx))) goto L_0110D8; /* 0110C0 blts 0x110d8 */
	{ ctx->a[0] = 0x000A1472u; hle_insn(ctx, 12); } /* 0110C2 lea 0xa1472,%a0 */
	{ uint32_t _s = 0x64u; uint32_t _da = 0xFFFFA242u; uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 20); } /* 0110C8 cmpiw #100,0xffffa242 */
	if (bs(ctx, !c_ge(ctx))) goto L_0110D8; /* 0110D0 blts 0x110d8 */
	{ ctx->a[0] = 0x000A1364u; hle_insn(ctx, 12); } /* 0110D2 lea 0xa1364,%a0 */
L_0110D8:;
	{ hle_insn(ctx, 18); sub_01114C(ctx); } /* 0110D8 bsrw 0x1114c */
	{ ctx->d[0] = 0x0u; cc_logic32(ctx, ctx->d[0]); hle_insn(ctx, 4); } /* 0110DC moveq #0,%d0 */
L_0110DE:;
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)20), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0110DE movew %d0,%fp@(20) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)26)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0110E2 movew %fp@(26),%d0 */
	if (bs(ctx, c_z(ctx))) goto L_01112A; /* 0110E6 beqs 0x1112a */
	if (bs(ctx, !c_n(ctx))) goto L_0110FA; /* 0110E8 bpls 0x110fa */
	{ ctx->a[0] = 0x000A115Cu; hle_insn(ctx, 12); } /* 0110EA lea 0xa115c,%a0 */
	{ hle_insn(ctx, 18); sub_011146(ctx); } /* 0110F0 bsrw 0x11146 */
	{ uint32_t _v = 0x7u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0110F4 movew #7,%d0 */
	hle_insn(ctx, 10); goto L_01112A; /* 0110F8 bras 0x1112a */
L_0110FA:;
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 0110FA subqw #1,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_01112A; /* 0110FC bnes 0x1112a */
	{ ctx->a[0] = 0x000A12ECu; hle_insn(ctx, 12); } /* 0110FE lea 0xa12ec,%a0 */
	{ uint32_t _s = 0x28u; uint32_t _da = 0xFFFFA242u; uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 20); } /* 011104 cmpiw #40,0xffffa242 */
	if (bs(ctx, !c_ge(ctx))) goto L_011124; /* 01110C blts 0x11124 */
	{ ctx->a[0] = 0x000A147Cu; hle_insn(ctx, 12); } /* 01110E lea 0xa147c,%a0 */
	{ uint32_t _s = 0x64u; uint32_t _da = 0xFFFFA242u; uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 20); } /* 011114 cmpiw #100,0xffffa242 */
	if (bs(ctx, !c_ge(ctx))) goto L_011124; /* 01111C blts 0x11124 */
	{ ctx->a[0] = 0x000A136Eu; hle_insn(ctx, 12); } /* 01111E lea 0xa136e,%a0 */
L_011124:;
	{ hle_insn(ctx, 18); sub_011146(ctx); } /* 011124 bsrw 0x11146 */
	{ ctx->d[0] = 0x0u; cc_logic32(ctx, ctx->d[0]); hle_insn(ctx, 4); } /* 011128 moveq #0,%d0 */
L_01112A:;
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)26), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01112A movew %d0,%fp@(26) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01112E movew %fp@(2),%d2 */
	{ uint32_t _s = 0x168u; uint32_t _d = (ctx->d[2] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 011132 cmpiw #360,%d2 */
	if (bw(ctx, c_gt(ctx))) goto L_011142; /* 011136 bgtw 0x11142 */
	{ uint32_t _s = 0x22u; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 01113A subiw #34,%d2 */
	{ hle_insn(ctx, 18); sub_011072(ctx); } /* 01113E bsrw 0x11072 */
L_011142:;
	hle_insn(ctx, 10); goto L_010D20; /* 011142 braw 0x10d20 */
}

/* $0062B0 hit test of SWAT van (three boxes) */
HLE_FUNC_PURE(0x0062B0, HitSwatVan, 300)
{
	{ ctx->a[3] = 0x00630Cu; hle_insn(ctx, 8); } /* 0062B0 lea %pc@(0x630c),%a3 */
	{ ctx->a[2] = 0x0062FAu; hle_insn(ctx, 8); } /* 0062B4 lea %pc@(0x62fa),%a2 */
	{ hle_insn(ctx, 18); HitBox(ctx); } /* 0062B8 jsr %pc@(0x5940) */
	{ ctx->a[3] = 0x006314u; hle_insn(ctx, 8); } /* 0062BC lea %pc@(0x6314),%a3 */
	{ ctx->a[2] = 0x0062DCu; hle_insn(ctx, 8); } /* 0062C0 lea %pc@(0x62dc),%a2 */
	{ hle_insn(ctx, 18); HitBox(ctx); } /* 0062C4 jsr %pc@(0x5940) */
	{ ctx->a[3] = 0x00631Cu; hle_insn(ctx, 8); } /* 0062C8 lea %pc@(0x631c),%a3 */
	{ ctx->a[2] = 0x0062D4u; hle_insn(ctx, 8); } /* 0062CC lea %pc@(0x62d4),%a2 */
	hle_insn(ctx, 10); HitBox(ctx); return; /* 0062D0 jmp %pc@(0x5940) */
}

/* $006E52 hit test of T-1000 on the police vehicle (two boxes) */
HLE_FUNC_PURE(0x006E52, HitL7Heli, 300)
{
	{ ctx->a[3] = 0x006E80u; hle_insn(ctx, 8); } /* 006E52 lea %pc@(0x6e80),%a3 */
	{ ctx->a[2] = 0x006E66u; hle_insn(ctx, 8); } /* 006E56 lea %pc@(0x6e66),%a2 */
	{ hle_insn(ctx, 18); HitBox(ctx); } /* 006E5A jsr %pc@(0x5940) */
	{ ctx->a[3] = 0x006E88u; hle_insn(ctx, 8); } /* 006E5E lea %pc@(0x6e88),%a3 */
	hle_insn(ctx, 10); HitBox(ctx); return; /* 006E62 jmp %pc@(0x5940) */
}

