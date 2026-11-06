/* Level-8 (steel mill) object type $27: the T-1000 ($0112FE walk, $011360, $011394/$0113D8 attacks,
 * $011448 action pick, $0114C0 turn, $011514 hit, $011558 down, $011580/$0115AC shatter + fall,
 * $011A44 slide, $011A76 flash, $011A90 frozen, $011AF8 gone) and the liquid-nitrogen tanker with its
 * panels ($01116A leak, $0111CA scroller, $011BF4 meter, $011C2C meter off, $011C44 panel, $011CD8
 * panel leak) with the hit tests HitT1000Mill ($006C2E) and $006EF8. docs/objects_types.md
 * ("Level 8"), src/t2.s.
 *
 * Same scheme as ai_l7.c: every AI state is a range from its entry (reached by the `jmp (a0)` of
 * ObjRunAll) to `bra.w ObjNext` ($00B3BE, reached with hle_jump); the action pick $011448 ends in
 * `jmp (a1)` into one of the T-1000 action states (declared as extra exits). C callees (ObjScrollX,
 * ObjMoveX, ObjQueueSprite, QueueSprite, Random, HitBox) are called directly after their jsr is
 * charged; the helper subroutines that only touch work RAM (sub_00D236, the sprite chooser
 * sub_011616 and its table helpers, ...) are static C functions; every other callee (sounds, damage,
 * VDP queues, ScriptNext, debris spawners) runs as 68k through hle_call68k, so a state that reaches
 * one is a plain HLE_RANGE and the others HLE_RANGE_PURE. Every instruction is on the instruction
 * timeline (GPGX costs); the bodies were produced instruction by instruction from the disassembly
 * (a comment on every line names the 68k instruction). The on-hit callbacks the hit tests hand to
 * ShotHitTest stay 68k. */
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
static void sub_011616(hle_ctx *ctx);
static void sub_0118EA(hle_ctx *ctx);
static void sub_0118DC(hle_ctx *ctx);
static void sub_0118C0(hle_ctx *ctx);
static void sub_0118CE(hle_ctx *ctx);
static void sub_0118F4(hle_ctx *ctx);
static void sub_011334(hle_ctx *ctx);
static void sub_0115FA(hle_ctx *ctx);
static void sub_011B4E(hle_ctx *ctx);
static void sub_0112A8(hle_ctx *ctx);
static void sub_0112BE(hle_ctx *ctx);
static void sub_0112C4(hle_ctx *ctx);
static void sub_0112DE(hle_ctx *ctx);

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

/* sub_011616 (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_011616(hle_ctx *ctx)
{
	{ uint32_t _v = (ctx->d[1] & 0xFFFFu); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 011616 movew %d1,%d0 */
	{ uint32_t _s = 0xFu; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 011618 cmpiw #15,%d0 */
	if (bs(ctx, c_gt(ctx))) goto L_011644; /* 01161C bgts 0x11644 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 01161E movew %d0,%d3 */
	{ uint32_t _s = 0x7u; uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[3], _r); hle_insn(ctx, 8); } /* 011620 andiw #7,%d3 */
	{ hle_insn(ctx, 18); sub_0118EA(ctx); } /* 011624 bsrw 0x118ea */
	{ uint32_t _v = 0x0u; set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011628 movew #0,%d3 */
	{ hle_insn(ctx, 18); sub_0118DC(ctx); } /* 01162C bsrw 0x118dc */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 011630 movew %d0,%d1 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 6) & 1) ? 0 : CC_Z); hle_insn(ctx, 12); } /* 011632 btst #14,%fp@ */
	if (bs(ctx, !c_z(ctx))) goto L_01163C; /* 011636 bnes 0x1163c */
	{ uint32_t _s = 0x8u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 011638 addiw #8,%d1 */
L_01163C:;
	{ uint32_t _s = 0x274u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 01163C addiw #628,%d1 */
	hle_insn(ctx, 10); ObjQueueSprite(ctx); return; /* 011640 braw 0x13228 */
L_011644:;
	{ uint32_t _s = 0x10u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 011644 cmpiw #16,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_011668; /* 011648 bnes 0x11668 */
	{ ctx->d[3] = 0x8u; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 01164A moveq #8,%d3 */
	{ hle_insn(ctx, 18); sub_0118EA(ctx); } /* 01164C bsrw 0x118ea */
	{ uint32_t _v = 0x1u; set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011650 movew #1,%d3 */
	{ hle_insn(ctx, 18); sub_0118DC(ctx); } /* 011654 bsrw 0x118dc */
	{ uint32_t _v = 0x303u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011658 movew #771,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 01165C bsrw 0x13228 */
	{ uint32_t _v = 0x302u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011660 movew #770,%d1 */
	hle_insn(ctx, 10); ObjQueueSprite(ctx); return; /* 011664 braw 0x13228 */
L_011668:;
	{ uint32_t _s = 0x11u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 011668 cmpiw #17,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_01167C; /* 01166C bnes 0x1167c */
	{ ctx->d[3] = 0x9u; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 01166E moveq #9,%d3 */
	{ hle_insn(ctx, 18); sub_0118EA(ctx); } /* 011670 bsrw 0x118ea */
	{ uint32_t _v = 0x30Cu; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011674 movew #780,%d1 */
	hle_insn(ctx, 10); ObjQueueSprite(ctx); return; /* 011678 braw 0x13228 */
L_01167C:;
	{ uint32_t _s = 0x13u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 01167C cmpiw #19,%d0 */
	if (bs(ctx, c_gt(ctx))) goto L_01169C; /* 011680 bgts 0x1169c */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 011682 movew %d0,%d3 */
	{ uint32_t _s = 0x8u; uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[3], _r); hle_insn(ctx, 8); } /* 011684 subiw #8,%d3 */
	{ hle_insn(ctx, 18); sub_0118EA(ctx); } /* 011688 bsrw 0x118ea */
	{ uint32_t _v = 0x4u; set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 01168C movew #4,%d3 */
	{ hle_insn(ctx, 18); sub_0118DC(ctx); } /* 011690 bsrw 0x118dc */
	{ uint32_t _v = 0x30Du; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011694 movew #781,%d1 */
	hle_insn(ctx, 10); ObjQueueSprite(ctx); return; /* 011698 braw 0x13228 */
L_01169C:;
	{ uint32_t _s = 0x1Cu; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 01169C cmpiw #28,%d0 */
	if (bs(ctx, c_gt(ctx))) goto L_0116D0; /* 0116A0 bgts 0x116d0 */
	if (bs(ctx, c_z(ctx))) goto L_0116C2; /* 0116A2 beqs 0x116c2 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 0116A4 movew %d0,%d3 */
	{ uint32_t _s = 0x7u; uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[3], _r); hle_insn(ctx, 8); } /* 0116A6 subiw #7,%d3 */
	{ hle_insn(ctx, 18); sub_0118EA(ctx); } /* 0116AA bsrw 0x118ea */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 0116AE movew %d0,%d1 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 6) & 1) ? 0 : CC_Z); hle_insn(ctx, 12); } /* 0116B0 btst #14,%fp@ */
	if (bs(ctx, c_z(ctx))) goto L_0116BA; /* 0116B4 beqs 0x116ba */
	{ uint32_t _s = 0xDu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 0116B6 addiw #13,%d1 */
L_0116BA:;
	{ uint32_t _s = 0x2FBu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 0116BA addiw #763,%d1 */
	hle_insn(ctx, 10); ObjQueueSprite(ctx); return; /* 0116BE braw 0x13228 */
L_0116C2:;
	{ ctx->d[3] = 0xCu; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 0116C2 moveq #12,%d3 */
	{ hle_insn(ctx, 18); sub_0118EA(ctx); } /* 0116C4 bsrw 0x118ea */
	{ uint32_t _v = 0x30Eu; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0116C8 movew #782,%d1 */
	hle_insn(ctx, 10); ObjQueueSprite(ctx); return; /* 0116CC braw 0x13228 */
L_0116D0:;
	{ uint32_t _s = 0x21u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 0116D0 cmpiw #33,%d0 */
	if (bs(ctx, c_gt(ctx))) goto L_0116EA; /* 0116D4 bgts 0x116ea */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 0116D6 movew %d0,%d3 */
	{ uint32_t _s = 0x8u; uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[3], _r); hle_insn(ctx, 8); } /* 0116D8 subiw #8,%d3 */
	{ hle_insn(ctx, 18); sub_0118EA(ctx); } /* 0116DC bsrw 0x118ea */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 0116E0 movew %d0,%d1 */
	{ uint32_t _s = 0x2FAu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 0116E2 addiw #762,%d1 */
	hle_insn(ctx, 10); ObjQueueSprite(ctx); return; /* 0116E6 braw 0x13228 */
L_0116EA:;
	{ uint32_t _s = 0x22u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 0116EA cmpiw #34,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_01170E; /* 0116EE bnes 0x1170e */
L_0116F0:;
	{ ctx->d[3] = 0x8u; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 0116F0 moveq #8,%d3 */
	{ hle_insn(ctx, 18); sub_0118EA(ctx); } /* 0116F2 bsrw 0x118ea */
	{ uint32_t _v = 0x3u; set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0116F6 movew #3,%d3 */
	{ hle_insn(ctx, 18); sub_0118DC(ctx); } /* 0116FA bsrw 0x118dc */
	{ uint32_t _v = 0x305u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0116FE movew #773,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 011702 bsrw 0x13228 */
	{ uint32_t _v = 0x302u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011706 movew #770,%d1 */
	hle_insn(ctx, 10); ObjQueueSprite(ctx); return; /* 01170A braw 0x13228 */
L_01170E:;
	{ uint32_t _s = 0x23u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 01170E cmpiw #35,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_011732; /* 011712 bnes 0x11732 */
	{ ctx->d[3] = 0x8u; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 011714 moveq #8,%d3 */
	{ hle_insn(ctx, 18); sub_0118EA(ctx); } /* 011716 bsrw 0x118ea */
	{ uint32_t _v = 0x2u; set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 01171A movew #2,%d3 */
	{ hle_insn(ctx, 18); sub_0118DC(ctx); } /* 01171E bsrw 0x118dc */
	{ uint32_t _v = 0x304u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011722 movew #772,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 011726 bsrw 0x13228 */
	{ uint32_t _v = 0x302u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 01172A movew #770,%d1 */
	hle_insn(ctx, 10); ObjQueueSprite(ctx); return; /* 01172E braw 0x13228 */
L_011732:;
	{ uint32_t _s = 0x29u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 011732 cmpiw #41,%d0 */
	if (bw(ctx, c_gt(ctx))) goto L_0117E6; /* 011736 bgtw 0x117e6 */
	{ uint32_t _s = 0x24u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 01173A cmpiw #36,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_011758; /* 01173E bnes 0x11758 */
	{ uint32_t _v = 0x424u; st16(0xFFFF9E58u, _v); cc_logic16(ctx, _v); hle_insn(ctx, 20); } /* 011740 movew #1060,0xffff9e58 */
	{ ctx->d[3] = 0x0u; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 011748 moveq #0,%d3 */
	{ hle_insn(ctx, 18); sub_0118C0(ctx); } /* 01174A bsrw 0x118c0 */
	{ uint32_t _v = 0x306u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 01174E movew #774,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 011752 bsrw 0x13228 */
	hle_insn(ctx, 10); goto L_0116F0; /* 011756 bras 0x116f0 */
L_011758:;
	{ uint32_t _s = 0x25u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 011758 cmpiw #37,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_011770; /* 01175C bnes 0x11770 */
	{ uint32_t _v = 0x430u; st16(0xFFFF9E58u, _v); cc_logic16(ctx, _v); hle_insn(ctx, 20); } /* 01175E movew #1072,0xffff9e58 */
	{ uint32_t _v = 0x309u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011766 movew #777,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 01176A bsrw 0x13228 */
	hle_insn(ctx, 10); goto L_0116F0; /* 01176E bras 0x116f0 */
L_011770:;
	{ uint32_t _s = 0x26u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 011770 cmpiw #38,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_01179A; /* 011774 bnes 0x1179a */
	{ uint32_t _v = 0x3E8u; st16(0xFFFF9E1Au, _v); cc_logic16(ctx, _v); hle_insn(ctx, 20); } /* 011776 movew #1000,0xffff9e1a */
	{ uint32_t _v = 0x307u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 01177E movew #775,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 011782 bsrw 0x13228 */
	{ uint32_t _v = 0x430u; st16(0xFFFF9E58u, _v); cc_logic16(ctx, _v); hle_insn(ctx, 20); } /* 011786 movew #1072,0xffff9e58 */
	{ uint32_t _v = 0x309u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 01178E movew #777,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 011792 bsrw 0x13228 */
	hle_insn(ctx, 10); goto L_0116F0; /* 011796 braw 0x116f0 */
L_01179A:;
	{ uint32_t _s = 0x27u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 01179A cmpiw #39,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_0117C4; /* 01179E bnes 0x117c4 */
	{ uint32_t _v = 0x3FCu; st16(0xFFFF9E1Au, _v); cc_logic16(ctx, _v); hle_insn(ctx, 20); } /* 0117A0 movew #1020,0xffff9e1a */
	{ uint32_t _v = 0x308u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0117A8 movew #776,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 0117AC bsrw 0x13228 */
	{ uint32_t _v = 0x430u; st16(0xFFFF9E58u, _v); cc_logic16(ctx, _v); hle_insn(ctx, 20); } /* 0117B0 movew #1072,0xffff9e58 */
	{ uint32_t _v = 0x309u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0117B8 movew #777,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 0117BC bsrw 0x13228 */
	hle_insn(ctx, 10); goto L_0116F0; /* 0117C0 braw 0x116f0 */
L_0117C4:;
	{ uint32_t _s = 0x28u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 0117C4 cmpiw #40,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_0117E6; /* 0117C8 bnes 0x117e6 */
	{ uint32_t _v = 0x3FCu; st16(0xFFFF9E1Au, _v); cc_logic16(ctx, _v); hle_insn(ctx, 20); } /* 0117CA movew #1020,0xffff9e1a */
	{ uint32_t _v = 0x430u; st16(0xFFFF9E58u, _v); cc_logic16(ctx, _v); hle_insn(ctx, 20); } /* 0117D2 movew #1072,0xffff9e58 */
	{ uint32_t _v = 0x30Au; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0117DA movew #778,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 0117DE bsrw 0x13228 */
	hle_insn(ctx, 10); goto L_0116F0; /* 0117E2 braw 0x116f0 */
L_0117E6:;
	{ uint32_t _s = 0x32u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 0117E6 cmpiw #50,%d0 */
	if (bs(ctx, c_ge(ctx))) goto L_011800; /* 0117EA bges 0x11800 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 0117EC movew %d0,%d3 */
	{ uint32_t _s = 0x1Du; uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[3], _r); hle_insn(ctx, 8); } /* 0117EE subiw #29,%d3 */
	{ hle_insn(ctx, 18); sub_0118EA(ctx); } /* 0117F2 bsrw 0x118ea */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 0117F6 movew %d0,%d1 */
	{ uint32_t _s = 0x2F2u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 0117F8 addiw #754,%d1 */
	hle_insn(ctx, 10); ObjQueueSprite(ctx); return; /* 0117FC braw 0x13228 */
L_011800:;
	{ uint32_t _s = 0x37u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 011800 cmpiw #55,%d0 */
	if (bs(ctx, c_gt(ctx))) goto L_01181A; /* 011804 bgts 0x1181a */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 011806 movew %d0,%d3 */
	{ uint32_t _s = 0x18u; uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[3], _r); hle_insn(ctx, 8); } /* 011808 subiw #24,%d3 */
	{ hle_insn(ctx, 18); sub_0118EA(ctx); } /* 01180C bsrw 0x118ea */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 011810 movew %d0,%d1 */
	{ uint32_t _s = 0x31Bu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 011812 addiw #795,%d1 */
	hle_insn(ctx, 10); ObjQueueSprite(ctx); return; /* 011816 braw 0x13228 */
L_01181A:;
	{ uint32_t _s = 0x3Du; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 01181A cmpiw #61,%d0 */
	if (bs(ctx, c_gt(ctx))) goto L_011834; /* 01181E bgts 0x11834 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 011820 movew %d0,%d3 */
	{ uint32_t _s = 0x18u; uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[3], _r); hle_insn(ctx, 8); } /* 011822 subiw #24,%d3 */
	{ hle_insn(ctx, 18); sub_0118EA(ctx); } /* 011826 bsrw 0x118ea */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 01182A movew %d0,%d1 */
	{ uint32_t _s = 0x327u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 01182C addiw #807,%d1 */
	hle_insn(ctx, 10); ObjQueueSprite(ctx); return; /* 011830 braw 0x13228 */
L_011834:;
	{ uint32_t _s = 0x43u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 011834 cmpiw #67,%d0 */
	if (bs(ctx, c_gt(ctx))) goto L_011856; /* 011838 bgts 0x11856 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 01183A movew %d0,%d3 */
	{ uint32_t _s = 0x18u; uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[3], _r); hle_insn(ctx, 8); } /* 01183C subiw #24,%d3 */
	{ hle_insn(ctx, 18); sub_0118EA(ctx); } /* 011840 bsrw 0x118ea */
	{ uint32_t _v = 0x5u; set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011844 movew #5,%d3 */
	{ hle_insn(ctx, 18); sub_0118DC(ctx); } /* 011848 bsrw 0x118dc */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 01184C movew %d0,%d1 */
	{ uint32_t _s = 0x315u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 01184E addiw #789,%d1 */
	hle_insn(ctx, 10); ObjQueueSprite(ctx); return; /* 011852 braw 0x13228 */
L_011856:;
	{ uint32_t _s = 0x49u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 011856 cmpiw #73,%d0 */
	if (bs(ctx, c_gt(ctx))) goto L_01186E; /* 01185A bgts 0x1186e */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 01185C movew %d0,%d3 */
	{ uint32_t _s = 0x18u; uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[3], _r); hle_insn(ctx, 8); } /* 01185E subiw #24,%d3 */
	{ hle_insn(ctx, 18); sub_0118EA(ctx); } /* 011862 bsrw 0x118ea */
	{ uint32_t _s = 0x315u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 011866 addiw #789,%d1 */
	hle_insn(ctx, 10); ObjQueueSprite(ctx); return; /* 01186A braw 0x13228 */
L_01186E:;
	{ uint32_t _s = 0x4Bu; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 01186E cmpiw #75,%d0 */
	if (bs(ctx, c_gt(ctx))) goto L_01189E; /* 011872 bgts 0x1189e */
	if (bs(ctx, !c_z(ctx))) goto L_01187E; /* 011874 bnes 0x1187e */
	{ uint32_t _v = 0x36Eu; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011876 movew #878,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 01187A bsrw 0x13228 */
L_01187E:;
	{ uint32_t _v = 0x1F4u; st16(0xFFFF9E76u, _v); cc_logic16(ctx, _v); hle_insn(ctx, 20); } /* 01187E movew #500,0xffff9e76 */
	{ uint32_t _v = 0x6u; set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011886 movew #6,%d3 */
	{ hle_insn(ctx, 18); sub_0118DC(ctx); } /* 01188A bsrw 0x118dc */
	{ uint32_t _v = 0x365u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 01188E movew #869,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 011892 bsrw 0x13228 */
	{ uint32_t _v = 0x366u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011896 movew #870,%d1 */
	hle_insn(ctx, 10); ObjQueueSprite(ctx); return; /* 01189A braw 0x13228 */
L_01189E:;
	{ uint32_t _v = 0x220u; st16(0xFFFF9E76u, _v); cc_logic16(ctx, _v); hle_insn(ctx, 20); } /* 01189E movew #544,0xffff9e76 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 0118A6 movew %d0,%d3 */
	{ uint32_t _s = 0x45u; uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[3], _r); hle_insn(ctx, 8); } /* 0118A8 subiw #69,%d3 */
	{ hle_insn(ctx, 18); sub_0118DC(ctx); } /* 0118AC bsrw 0x118dc */
	{ uint32_t _s = 0x31Bu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 0118B0 addiw #795,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 0118B4 bsrw 0x13228 */
	{ uint32_t _v = 0x365u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0118B8 movew #869,%d1 */
	hle_insn(ctx, 10); ObjQueueSprite(ctx); return; /* 0118BC braw 0x13228 */
}

/* sub_0118EA (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_0118EA(hle_ctx *ctx)
{
	{ ctx->a[1] = 0x011928u; hle_insn(ctx, 8); } /* 0118EA lea %pc@(0x11928),%a1 */
	{ uint32_t _v = ld16(0xFFFF9E76u); set_w(&ctx->d[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 0118EE movew 0xffff9e76,%d6 */
	{ uint32_t _v = ld32(0xFFFF80BCu); ctx->a[5] = _v; hle_insn(ctx, 20); } /* 0118F4 moveal 0xffff80bc,%a5 */
	{ uint32_t _v = 0x1u; st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0118FA movew #1,%a5@+ */
	{ sh_w(ctx, 3, 'l', 2); hle_insn(ctx, 10); } /* 0118FE aslw #2,%d3 */
	{ uint32_t _v = ld16((ctx->a[1] + (uint32_t)(int32_t)0 + (uint32_t)((int32_t)(int16_t)ctx->d[3]))); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 14); } /* 011900 movew %a1@(0,%d3:w),%d2 */
	{ sh_w(ctx, 2, 'a', 1); hle_insn(ctx, 8); } /* 011904 asrw #1,%d2 */
	{ uint32_t _v = (ctx->d[2] & 0xFFFFu); st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011906 movew %d2,%a5@+ */
	{ uint32_t _v = ld16((ctx->a[1] + (uint32_t)(int32_t)2 + (uint32_t)((int32_t)(int16_t)ctx->d[3]))); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 14); } /* 011908 movew %a1@(2,%d3:w),%d3 */
	{ ctx->a[2] = 0x000CB6B6u; hle_insn(ctx, 12); } /* 01190C lea 0xcb6b6,%a2 */
	{ uint32_t _s = 0xFFFFu; uint32_t _d = ctx->d[3]; uint32_t _r = (_d & _s) & 0xFFFFFFFFu; cc_logic32(ctx, _r); ctx->d[3] = _r; hle_insn(ctx, 16); } /* 011912 andil #65535,%d3 */
	{ ctx->a[2] = ctx->a[2] + ctx->d[3]; hle_insn(ctx, 8); } /* 011918 addal %d3,%a2 */
	{ uint32_t _v = ctx->a[2]; st32(ctx->a[5], _v); ctx->a[5] += 4; cc_logic32(ctx, _v); hle_insn(ctx, 12); } /* 01191A movel %a2,%a5@+ */
	{ sh_w(ctx, 6, 'l', 5); hle_insn(ctx, 16); } /* 01191C aslw #5,%d6 */
	{ uint32_t _v = (ctx->d[6] & 0xFFFFu); st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 01191E movew %d6,%a5@+ */
	{ uint32_t _v = ctx->a[5]; st32(0xFFFF80BCu, _v); cc_logic32(ctx, _v); hle_insn(ctx, 20); } /* 011920 movel %a5,0xffff80bc */
	hle_insn(ctx, 16); return; /* 011926 rts */
}

/* sub_0118DC (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_0118DC(hle_ctx *ctx)
{
	{ ctx->a[1] = 0x0119F8u; hle_insn(ctx, 8); } /* 0118DC lea %pc@(0x119f8),%a1 */
	{ uint32_t _v = ld16(0xFFFF9E5Cu); set_w(&ctx->d[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 0118E0 movew 0xffff9e5c,%d6 */
	hle_insn(ctx, 10); goto L_0118F4; /* 0118E6 braw 0x118f4 */
L_0118F4:;
	{ uint32_t _v = ld32(0xFFFF80BCu); ctx->a[5] = _v; hle_insn(ctx, 20); } /* 0118F4 moveal 0xffff80bc,%a5 */
	{ uint32_t _v = 0x1u; st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0118FA movew #1,%a5@+ */
	{ sh_w(ctx, 3, 'l', 2); hle_insn(ctx, 10); } /* 0118FE aslw #2,%d3 */
	{ uint32_t _v = ld16((ctx->a[1] + (uint32_t)(int32_t)0 + (uint32_t)((int32_t)(int16_t)ctx->d[3]))); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 14); } /* 011900 movew %a1@(0,%d3:w),%d2 */
	{ sh_w(ctx, 2, 'a', 1); hle_insn(ctx, 8); } /* 011904 asrw #1,%d2 */
	{ uint32_t _v = (ctx->d[2] & 0xFFFFu); st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011906 movew %d2,%a5@+ */
	{ uint32_t _v = ld16((ctx->a[1] + (uint32_t)(int32_t)2 + (uint32_t)((int32_t)(int16_t)ctx->d[3]))); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 14); } /* 011908 movew %a1@(2,%d3:w),%d3 */
	{ ctx->a[2] = 0x000CB6B6u; hle_insn(ctx, 12); } /* 01190C lea 0xcb6b6,%a2 */
	{ uint32_t _s = 0xFFFFu; uint32_t _d = ctx->d[3]; uint32_t _r = (_d & _s) & 0xFFFFFFFFu; cc_logic32(ctx, _r); ctx->d[3] = _r; hle_insn(ctx, 16); } /* 011912 andil #65535,%d3 */
	{ ctx->a[2] = ctx->a[2] + ctx->d[3]; hle_insn(ctx, 8); } /* 011918 addal %d3,%a2 */
	{ uint32_t _v = ctx->a[2]; st32(ctx->a[5], _v); ctx->a[5] += 4; cc_logic32(ctx, _v); hle_insn(ctx, 12); } /* 01191A movel %a2,%a5@+ */
	{ sh_w(ctx, 6, 'l', 5); hle_insn(ctx, 16); } /* 01191C aslw #5,%d6 */
	{ uint32_t _v = (ctx->d[6] & 0xFFFFu); st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 01191E movew %d6,%a5@+ */
	{ uint32_t _v = ctx->a[5]; st32(0xFFFF80BCu, _v); cc_logic32(ctx, _v); hle_insn(ctx, 20); } /* 011920 movel %a5,0xffff80bc */
	hle_insn(ctx, 16); return; /* 011926 rts */
}

/* sub_0118C0 (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_0118C0(hle_ctx *ctx)
{
	{ ctx->a[1] = 0x011A30u; hle_insn(ctx, 8); } /* 0118C0 lea %pc@(0x11a30),%a1 */
	{ uint32_t _v = ld16(0xFFFF9E58u); set_w(&ctx->d[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 0118C4 movew 0xffff9e58,%d6 */
	hle_insn(ctx, 10); goto L_0118F4; /* 0118CA braw 0x118f4 */
L_0118F4:;
	{ uint32_t _v = ld32(0xFFFF80BCu); ctx->a[5] = _v; hle_insn(ctx, 20); } /* 0118F4 moveal 0xffff80bc,%a5 */
	{ uint32_t _v = 0x1u; st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0118FA movew #1,%a5@+ */
	{ sh_w(ctx, 3, 'l', 2); hle_insn(ctx, 10); } /* 0118FE aslw #2,%d3 */
	{ uint32_t _v = ld16((ctx->a[1] + (uint32_t)(int32_t)0 + (uint32_t)((int32_t)(int16_t)ctx->d[3]))); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 14); } /* 011900 movew %a1@(0,%d3:w),%d2 */
	{ sh_w(ctx, 2, 'a', 1); hle_insn(ctx, 8); } /* 011904 asrw #1,%d2 */
	{ uint32_t _v = (ctx->d[2] & 0xFFFFu); st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011906 movew %d2,%a5@+ */
	{ uint32_t _v = ld16((ctx->a[1] + (uint32_t)(int32_t)2 + (uint32_t)((int32_t)(int16_t)ctx->d[3]))); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 14); } /* 011908 movew %a1@(2,%d3:w),%d3 */
	{ ctx->a[2] = 0x000CB6B6u; hle_insn(ctx, 12); } /* 01190C lea 0xcb6b6,%a2 */
	{ uint32_t _s = 0xFFFFu; uint32_t _d = ctx->d[3]; uint32_t _r = (_d & _s) & 0xFFFFFFFFu; cc_logic32(ctx, _r); ctx->d[3] = _r; hle_insn(ctx, 16); } /* 011912 andil #65535,%d3 */
	{ ctx->a[2] = ctx->a[2] + ctx->d[3]; hle_insn(ctx, 8); } /* 011918 addal %d3,%a2 */
	{ uint32_t _v = ctx->a[2]; st32(ctx->a[5], _v); ctx->a[5] += 4; cc_logic32(ctx, _v); hle_insn(ctx, 12); } /* 01191A movel %a2,%a5@+ */
	{ sh_w(ctx, 6, 'l', 5); hle_insn(ctx, 16); } /* 01191C aslw #5,%d6 */
	{ uint32_t _v = (ctx->d[6] & 0xFFFFu); st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 01191E movew %d6,%a5@+ */
	{ uint32_t _v = ctx->a[5]; st32(0xFFFF80BCu, _v); cc_logic32(ctx, _v); hle_insn(ctx, 20); } /* 011920 movel %a5,0xffff80bc */
	hle_insn(ctx, 16); return; /* 011926 rts */
}

/* sub_0118CE (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_0118CE(hle_ctx *ctx)
{
	{ ctx->a[1] = 0x011A3Cu; hle_insn(ctx, 8); } /* 0118CE lea %pc@(0x11a3c),%a1 */
	{ uint32_t _v = ld16(0xFFFF9E1Au); set_w(&ctx->d[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 0118D2 movew 0xffff9e1a,%d6 */
	hle_insn(ctx, 10); goto L_0118F4; /* 0118D8 braw 0x118f4 */
L_0118F4:;
	{ uint32_t _v = ld32(0xFFFF80BCu); ctx->a[5] = _v; hle_insn(ctx, 20); } /* 0118F4 moveal 0xffff80bc,%a5 */
	{ uint32_t _v = 0x1u; st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0118FA movew #1,%a5@+ */
	{ sh_w(ctx, 3, 'l', 2); hle_insn(ctx, 10); } /* 0118FE aslw #2,%d3 */
	{ uint32_t _v = ld16((ctx->a[1] + (uint32_t)(int32_t)0 + (uint32_t)((int32_t)(int16_t)ctx->d[3]))); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 14); } /* 011900 movew %a1@(0,%d3:w),%d2 */
	{ sh_w(ctx, 2, 'a', 1); hle_insn(ctx, 8); } /* 011904 asrw #1,%d2 */
	{ uint32_t _v = (ctx->d[2] & 0xFFFFu); st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011906 movew %d2,%a5@+ */
	{ uint32_t _v = ld16((ctx->a[1] + (uint32_t)(int32_t)2 + (uint32_t)((int32_t)(int16_t)ctx->d[3]))); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 14); } /* 011908 movew %a1@(2,%d3:w),%d3 */
	{ ctx->a[2] = 0x000CB6B6u; hle_insn(ctx, 12); } /* 01190C lea 0xcb6b6,%a2 */
	{ uint32_t _s = 0xFFFFu; uint32_t _d = ctx->d[3]; uint32_t _r = (_d & _s) & 0xFFFFFFFFu; cc_logic32(ctx, _r); ctx->d[3] = _r; hle_insn(ctx, 16); } /* 011912 andil #65535,%d3 */
	{ ctx->a[2] = ctx->a[2] + ctx->d[3]; hle_insn(ctx, 8); } /* 011918 addal %d3,%a2 */
	{ uint32_t _v = ctx->a[2]; st32(ctx->a[5], _v); ctx->a[5] += 4; cc_logic32(ctx, _v); hle_insn(ctx, 12); } /* 01191A movel %a2,%a5@+ */
	{ sh_w(ctx, 6, 'l', 5); hle_insn(ctx, 16); } /* 01191C aslw #5,%d6 */
	{ uint32_t _v = (ctx->d[6] & 0xFFFFu); st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 01191E movew %d6,%a5@+ */
	{ uint32_t _v = ctx->a[5]; st32(0xFFFF80BCu, _v); cc_logic32(ctx, _v); hle_insn(ctx, 20); } /* 011920 movel %a5,0xffff80bc */
	hle_insn(ctx, 16); return; /* 011926 rts */
}

/* sub_0118F4 (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_0118F4(hle_ctx *ctx)
{
	{ uint32_t _v = ld32(0xFFFF80BCu); ctx->a[5] = _v; hle_insn(ctx, 20); } /* 0118F4 moveal 0xffff80bc,%a5 */
	{ uint32_t _v = 0x1u; st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0118FA movew #1,%a5@+ */
	{ sh_w(ctx, 3, 'l', 2); hle_insn(ctx, 10); } /* 0118FE aslw #2,%d3 */
	{ uint32_t _v = ld16((ctx->a[1] + (uint32_t)(int32_t)0 + (uint32_t)((int32_t)(int16_t)ctx->d[3]))); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 14); } /* 011900 movew %a1@(0,%d3:w),%d2 */
	{ sh_w(ctx, 2, 'a', 1); hle_insn(ctx, 8); } /* 011904 asrw #1,%d2 */
	{ uint32_t _v = (ctx->d[2] & 0xFFFFu); st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011906 movew %d2,%a5@+ */
	{ uint32_t _v = ld16((ctx->a[1] + (uint32_t)(int32_t)2 + (uint32_t)((int32_t)(int16_t)ctx->d[3]))); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 14); } /* 011908 movew %a1@(2,%d3:w),%d3 */
	{ ctx->a[2] = 0x000CB6B6u; hle_insn(ctx, 12); } /* 01190C lea 0xcb6b6,%a2 */
	{ uint32_t _s = 0xFFFFu; uint32_t _d = ctx->d[3]; uint32_t _r = (_d & _s) & 0xFFFFFFFFu; cc_logic32(ctx, _r); ctx->d[3] = _r; hle_insn(ctx, 16); } /* 011912 andil #65535,%d3 */
	{ ctx->a[2] = ctx->a[2] + ctx->d[3]; hle_insn(ctx, 8); } /* 011918 addal %d3,%a2 */
	{ uint32_t _v = ctx->a[2]; st32(ctx->a[5], _v); ctx->a[5] += 4; cc_logic32(ctx, _v); hle_insn(ctx, 12); } /* 01191A movel %a2,%a5@+ */
	{ sh_w(ctx, 6, 'l', 5); hle_insn(ctx, 16); } /* 01191C aslw #5,%d6 */
	{ uint32_t _v = (ctx->d[6] & 0xFFFFu); st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 01191E movew %d6,%a5@+ */
	{ uint32_t _v = ctx->a[5]; st32(0xFFFF80BCu, _v); cc_logic32(ctx, _v); hle_insn(ctx, 20); } /* 011920 movel %a5,0xffff80bc */
	hle_insn(ctx, 16); return; /* 011926 rts */
}

/* sub_011334 (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_011334(hle_ctx *ctx)
{
	{ uint32_t _s = 0xFFE2u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)2); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 011334 cmpiw #-30,%fp@(2) */
	if (bs(ctx, !c_ge(ctx))) goto L_011344; /* 01133A blts 0x11344 */
	{ uint32_t _s = 0x15Eu; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)2); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 01133C cmpiw #350,%fp@(2) */
	if (bs(ctx, c_le(ctx))) goto L_01135E; /* 011342 bles 0x1135e */
L_011344:;
	{ uint32_t _v = 0x114C0u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 011344 movel #70848,%fp@(32) */
	{ hle_insn(ctx, 20); Random(ctx); } /* 01134C jsr 0x881c */
	{ uint32_t _s = 0xFu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 011352 andiw #15,%d0 */
	{ uint32_t _s = 0x1Eu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 011356 addiw #30,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01135A movew %d0,%fp@(36) */
L_01135E:;
	hle_insn(ctx, 16); return; /* 01135E rts */
}

/* sub_0115FA (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_0115FA(hle_ctx *ctx)
{
	{ uint32_t _v = 0x112FEu; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 0115FA movel #70398,%fp@(32) */
	{ hle_insn(ctx, 20); Random(ctx); } /* 011602 jsr 0x881c */
	{ uint32_t _s = 0x1Fu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 011608 andiw #31,%d0 */
	{ uint32_t _s = 0x1Eu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 01160C addiw #30,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 011610 movew %d0,%fp@(36) */
	hle_insn(ctx, 16); return; /* 011614 rts */
}

/* sub_011B4E (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_011B4E(hle_ctx *ctx)
{
	{ st16(0xFFFFAB1Cu, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 20); } /* 011B4E clrw 0xffffab1c */
	{ ctx->a[0] = 0x000A63B6u; hle_insn(ctx, 12); } /* 011B54 lea 0xa63b6,%a0 */
	{ uint32_t _v = 0xE310u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011B5A movew #-7408,%d1 */
	{ ctx->d[2] = 0x18u; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 011B5E moveq #24,%d2 */
	{ ctx->d[3] = 0x1u; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 011B60 moveq #1,%d3 */
	{ uint32_t _v = 0xA000u; ctx->d[6] = _v; cc_logic32(ctx, _v); hle_insn(ctx, 12); } /* 011B62 movel #40960,%d6 */
	{ uint32_t _v = (ctx->d[7] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011B68 movew %d7,%sp@- */
	{ uint32_t _v = ld16(0xFFFF9E38u); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 011B6A movew 0xffff9e38,%d7 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x012234); } /* 011B70 jsr %pc@(0x12234) */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011B74 movew %sp@+,%d7 */
	{ ctx->a[0] = 0x000A6396u; hle_insn(ctx, 12); } /* 011B76 lea 0xa6396,%a0 */
	{ uint32_t _v = 0xE318u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011B7C movew #-7400,%d1 */
	{ cc_logic8(ctx, ld8(0xFFFF9EC2u)); hle_insn(ctx, 16); } /* 011B80 tstb 0xffff9ec2 */
	if (bw(ctx, !c_z(ctx))) goto L_011BF2; /* 011B86 bnew 0x11bf2 */
	{ uint32_t _v = ld16(0xFFFF9F60u); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 011B8A movew 0xffff9f60,%d2 */
	{ uint32_t _s = 0xCu; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 011B90 addiw #12,%d2 */
	if (bs(ctx, !c_c(ctx))) goto L_011B9A; /* 011B94 bccs 0x11b9a */
	{ uint32_t _v = 0xFFFFu; set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011B96 movew #-1,%d2 */
L_011B9A:;
	{ ctx->d[3] = 0x1u; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 011B9A moveq #1,%d3 */
	{ uint32_t _v = (ctx->d[7] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011B9C movew %d7,%sp@- */
	{ uint32_t _v = ld16(0xFFFF9E38u); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 011B9E movew 0xffff9e38,%d7 */
	{ uint32_t _v = (ctx->d[2] & 0xFFFFu); st16(0xFFFF9F60u, _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 011BA4 movew %d2,0xffff9f60 */
	{ sh_w(ctx, 2, 'o', 4); hle_insn(ctx, 14); } /* 011BAA rolw #4,%d2 */
	{ uint32_t _s = 0xFu; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 011BAC andiw #15,%d2 */
	if (bs(ctx, c_z(ctx))) goto L_011BD2; /* 011BB0 beqs 0x11bd2 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x012234); } /* 011BB2 jsr %pc@(0x12234) */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011BB6 movew %sp@+,%d7 */
	{ ctx->d[2] = 0x0u; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 011BB8 moveq #0,%d2 */
	{ uint32_t _v = ld8(0xFFFF9F60u); set_b(&ctx->d[2], _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 011BBA moveb 0xffff9f60,%d2 */
	{ sh_w(ctx, 2, 'a', 1); hle_insn(ctx, 8); } /* 011BC0 asrw #1,%d2 */
	{ uint32_t _s = 0x58u; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 011BC2 addiw #88,%d2 */
	{ ctx->d[3] = 0x30u; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 011BC6 moveq #48,%d3 */
	{ ctx->d[1] = 0x71u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 011BC8 moveq #113,%d1 */
	{ ctx->d[5] = 0x78u; cc_logic32(ctx, ctx->d[5]); hle_insn(ctx, 4); } /* 011BCA moveq #120,%d5 */
	{ hle_insn(ctx, 18); QueueSprite(ctx); } /* 011BCC jsr %pc@(0x13240) */
	hle_insn(ctx, 16); return; /* 011BD0 rts */
L_011BD2:;
	{ ctx->d[2] = 0x1u; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 011BD2 moveq #1,%d2 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x012234); } /* 011BD4 jsr %pc@(0x12234) */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011BD8 movew %sp@+,%d7 */
	{ ctx->d[2] = 0x0u; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 011BDA moveq #0,%d2 */
	{ uint32_t _v = ld8(0xFFFF9F60u); set_b(&ctx->d[2], _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 011BDC moveb 0xffff9f60,%d2 */
	{ sh_w(ctx, 2, 'a', 1); hle_insn(ctx, 8); } /* 011BE2 asrw #1,%d2 */
	{ uint32_t _s = 0x60u; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 011BE4 addiw #96,%d2 */
	{ ctx->d[3] = 0x30u; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 011BE8 moveq #48,%d3 */
	{ ctx->d[1] = 0x70u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 011BEA moveq #112,%d1 */
	{ ctx->d[5] = 0x78u; cc_logic32(ctx, ctx->d[5]); hle_insn(ctx, 4); } /* 011BEC moveq #120,%d5 */
	{ hle_insn(ctx, 18); QueueSprite(ctx); } /* 011BEE jsr %pc@(0x13240) */
L_011BF2:;
	hle_insn(ctx, 16); return; /* 011BF2 rts */
}

/* sub_0112A8 (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_0112A8(hle_ctx *ctx)
{
	{ uint32_t _v = 0xA000u; ctx->d[6] = _v; cc_logic32(ctx, _v); hle_insn(ctx, 12); } /* 0112A8 movel #40960,%d6 */
	{ uint32_t _v = (ctx->d[7] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0112AE movew %d7,%sp@- */
	{ uint32_t _v = ld16(0xFFFF9E74u); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 0112B0 movew 0xffff9e74,%d7 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x012234); } /* 0112B6 jsr %pc@(0x12234) */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0112BA movew %sp@+,%d7 */
	hle_insn(ctx, 16); return; /* 0112BC rts */
}

/* sub_0112BE (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_0112BE(hle_ctx *ctx)
{
	{ ctx->a[0] = 0x0009C5BAu; hle_insn(ctx, 12); } /* 0112BE lea 0x9c5ba,%a0 */
	{ ctx->d[2] = 0x6u; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 0112C4 moveq #6,%d2 */
	{ ctx->d[3] = 0x5u; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 0112C6 moveq #5,%d3 */
	{ uint32_t _v = 0xA000u; ctx->d[6] = _v; cc_logic32(ctx, _v); hle_insn(ctx, 12); } /* 0112C8 movel #40960,%d6 */
	{ uint32_t _v = (ctx->d[7] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0112CE movew %d7,%sp@- */
	{ uint32_t _v = ld16(0xFFFF9E48u); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 0112D0 movew 0xffff9e48,%d7 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x0122CA); } /* 0112D6 jsr %pc@(0x122ca) */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0112DA movew %sp@+,%d7 */
	hle_insn(ctx, 16); return; /* 0112DC rts */
}

/* sub_0112C4 (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_0112C4(hle_ctx *ctx)
{
	{ ctx->d[2] = 0x6u; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 0112C4 moveq #6,%d2 */
	{ ctx->d[3] = 0x5u; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 0112C6 moveq #5,%d3 */
	{ uint32_t _v = 0xA000u; ctx->d[6] = _v; cc_logic32(ctx, _v); hle_insn(ctx, 12); } /* 0112C8 movel #40960,%d6 */
	{ uint32_t _v = (ctx->d[7] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0112CE movew %d7,%sp@- */
	{ uint32_t _v = ld16(0xFFFF9E48u); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 0112D0 movew 0xffff9e48,%d7 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x0122CA); } /* 0112D6 jsr %pc@(0x122ca) */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0112DA movew %sp@+,%d7 */
	hle_insn(ctx, 16); return; /* 0112DC rts */
}

/* sub_0112DE (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_0112DE(hle_ctx *ctx)
{
	{ ctx->a[0] = 0x000A63E6u; hle_insn(ctx, 12); } /* 0112DE lea 0xa63e6,%a0 */
	{ ctx->d[2] = 0xCu; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 0112E4 moveq #12,%d2 */
	{ ctx->d[3] = 0x1u; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 0112E6 moveq #1,%d3 */
	{ uint32_t _v = 0x2000u; ctx->d[6] = _v; cc_logic32(ctx, _v); hle_insn(ctx, 12); } /* 0112E8 movel #8192,%d6 */
	{ uint32_t _v = (ctx->d[7] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0112EE movew %d7,%sp@- */
	{ uint32_t _v = ld16(0xFFFF9E38u); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 0112F0 movew 0xffff9e38,%d7 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x012234); } /* 0112F6 jsr %pc@(0x12234) */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0112FA movew %sp@+,%d7 */
	hle_insn(ctx, 16); return; /* 0112FC rts */
}

/* $01116A $27 liquid-nitrogen tanker: periodic leak, spawn a spray piece */
HLE_RANGE(0x01116A, L8Leak, 1500, EXIT_NEXT)
{
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 01116A subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_0111C6; /* 01116E bnes 0x111c6 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)38)); st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 20); } /* 011170 movew %fp@(38),%fp@(36) */
	{ hle_insn(ctx, 20); Random(ctx); } /* 011176 jsr 0x881c */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 01117C movew %d0,%d3 */
	{ uint32_t _s = 0xFFu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 01117E andiw #255,%d0 */
	{ uint32_t _s = 0x20u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 011182 addiw #32,%d0 */
	{ uint32_t _v = 0x0u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011186 movew #0,%d1 */
	{ uint32_t _v = ld16(0xFFFF9EC6u); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 01118A movew 0xffff9ec6,%d2 */
	if (bs(ctx, c_le(ctx))) goto L_0111AC; /* 011190 bles 0x111ac */
	{ st16(0xFFFF9EC6u, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 20); } /* 011192 clrw 0xffff9ec6 */
	{ uint32_t _v = (ctx->d[3] & 0xFFFFu); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 011198 movew %d3,%d0 */
	{ uint32_t _s = 0x3Fu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 01119A andiw #63,%d0 */
	{ cc_logic16(ctx, (ctx->d[3] & 0xFFFFu)); hle_insn(ctx, 4); } /* 01119E tstw %d3 */
	if (bs(ctx, !c_n(ctx))) goto L_0111A6; /* 0111A0 bpls 0x111a6 */
	{ uint32_t _s = 0xF0u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0111A2 addiw #240,%d0 */
L_0111A6:;
	{ uint32_t _s = 0x10u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0111A6 addiw #16,%d0 */
	hle_insn(ctx, 10); goto L_0111AE; /* 0111AA bras 0x111ae */
L_0111AC:;
	{ ctx->d[2] = 0xBu; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 0111AC moveq #11,%d2 */
L_0111AE:;
	{ uint32_t _v = ctx->a[6]; push32(ctx, _v); cc_logic32(ctx, _v); hle_insn(ctx, 12); } /* 0111AE movel %fp,%sp@- */
	{ ctx->d[3] = 0x5u; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 0111B0 moveq #5,%d3 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x012A46); } /* 0111B2 jsr %pc@(0x12a46) */
	{ uint32_t _v = 0xF8u; set_w(&ctx->d[4], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0111B6 movew #248,%d4 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 0111BA movew %d0,%d3 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x012ABE); } /* 0111BC jsr %pc@(0x12abe) */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 5) & 1) ? 0 : CC_Z); st8(_a, _o | 0x20); hle_insn(ctx, 16); } /* 0111C0 bset #5,%fp@ */
	{ uint32_t _v = pop32(ctx); ctx->a[6] = _v; hle_insn(ctx, 12); } /* 0111C4 moveal %sp@+,%fp */
L_0111C6:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 0111C6 braw 0xb3be */
}

/* $0111CA $27 tanker/background scroller: queue the tile blocks at fixed X */
HLE_RANGE(0x0111CA, L8TankerScroll, 24000, EXIT_NEXT)
{
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 0111CA jsr %pc@(0x12316) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0111CE movew %fp@(2),%d0 */
	{ uint32_t _s = 0x140u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 0111D2 cmpiw #320,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_011210; /* 0111D6 bnes 0x11210 */
	{ uint32_t _v = 0xE480u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0111D8 movew #-7040,%d1 */
	{ ctx->a[0] = 0x000A162Au; hle_insn(ctx, 12); } /* 0111DC lea 0xa162a,%a0 */
	{ ctx->d[2] = 0x14u; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 0111E2 moveq #20,%d2 */
	{ ctx->d[3] = 0xDu; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 0111E4 moveq #13,%d3 */
	{ hle_insn(ctx, 18); sub_0112A8(ctx); } /* 0111E6 bsrw 0x112a8 */
	{ uint32_t _v = 0xE4A8u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0111EA movew #-7000,%d1 */
	{ ctx->a[0] = 0x000A1210u; hle_insn(ctx, 12); } /* 0111EE lea 0xa1210,%a0 */
	{ ctx->d[2] = 0x4u; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 0111F4 moveq #4,%d2 */
	{ ctx->d[3] = 0xDu; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 0111F6 moveq #13,%d3 */
	{ hle_insn(ctx, 18); sub_0112A8(ctx); } /* 0111F8 bsrw 0x112a8 */
	{ uint32_t _v = 0xE99Au; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0111FC movew #-5734,%d1 */
	{ hle_insn(ctx, 18); sub_0112BE(ctx); } /* 011200 bsrw 0x112be */
	{ uint32_t _v = 0xE9A6u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011204 movew #-5722,%d1 */
	{ hle_insn(ctx, 18); sub_0112BE(ctx); } /* 011208 bsrw 0x112be */
	hle_insn(ctx, 10); goto L_0112A4; /* 01120C braw 0x112a4 */
L_011210:;
	{ uint32_t _s = 0x80u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 011210 cmpiw #128,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_01125A; /* 011214 bnes 0x1125a */
	{ uint32_t _v = 0xE2B0u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011216 movew #-7504,%d1 */
	{ ctx->a[0] = 0x000A10D8u; hle_insn(ctx, 12); } /* 01121A lea 0xa10d8,%a0 */
	{ ctx->d[2] = 0x12u; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 011220 moveq #18,%d2 */
	{ ctx->d[3] = 0x11u; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 011222 moveq #17,%d3 */
	{ hle_insn(ctx, 18); sub_0112A8(ctx); } /* 011224 bsrw 0x112a8 */
	{ uint32_t _v = 0xE2D4u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011228 movew #-7468,%d1 */
	{ ctx->d[2] = 0x6u; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 01122C moveq #6,%d2 */
	{ ctx->d[3] = 0x12u; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 01122E moveq #18,%d3 */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_call68k(ctx, 0x012212); } /* 011230 jsr 0x12212 */
	{ uint32_t _v = 0xE53Eu; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011236 movew #-6850,%d1 */
	{ ctx->a[0] = 0x000A10FCu; hle_insn(ctx, 12); } /* 01123A lea 0xa10fc,%a0 */
	{ ctx->d[2] = 0x3u; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 011240 moveq #3,%d2 */
	{ ctx->d[3] = 0x3u; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 011242 moveq #3,%d3 */
	{ hle_insn(ctx, 18); sub_0112A8(ctx); } /* 011244 bsrw 0x112a8 */
	{ ctx->a[0] = 0x0009C5F6u; hle_insn(ctx, 12); } /* 011248 lea 0x9c5f6,%a0 */
	{ uint32_t _v = 0xE9C4u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 01124E movew #-5692,%d1 */
	{ hle_insn(ctx, 18); sub_0112C4(ctx); } /* 011252 bsrw 0x112c4 */
	hle_insn(ctx, 10); goto L_0112A4; /* 011256 braw 0x112a4 */
L_01125A:;
	{ uint32_t _s = 0xFFC0u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 01125A cmpiw #-64,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_011284; /* 01125E bnes 0x11284 */
	{ uint32_t _v = 0xE2E0u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011260 movew #-7456,%d1 */
	{ ctx->d[2] = 0x10u; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 011264 moveq #16,%d2 */
	{ ctx->d[3] = 0x12u; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 011266 moveq #18,%d3 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x012212); } /* 011268 jsr %pc@(0x12212) */
	{ uint32_t _v = 0xE280u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 01126C movew #-7552,%d1 */
	{ ctx->d[2] = 0x8u; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 011270 moveq #8,%d2 */
	{ ctx->d[3] = 0x12u; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 011272 moveq #18,%d3 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x012212); } /* 011274 jsr %pc@(0x12212) */
	{ uint32_t _v = 0xEBDCu; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011278 movew #-5156,%d1 */
	{ hle_insn(ctx, 18); sub_0112DE(ctx); } /* 01127C bsrw 0x112de */
	hle_insn(ctx, 10); goto L_0112A4; /* 011280 braw 0x112a4 */
L_011284:;
	{ uint32_t _s = 0xFF00u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 011284 cmpiw #-256,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_0112A2; /* 011288 bnes 0x112a2 */
	{ uint32_t _v = 0xE290u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 01128A movew #-7536,%d1 */
	{ ctx->d[2] = 0x18u; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 01128E moveq #24,%d2 */
	{ ctx->d[3] = 0x12u; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 011290 moveq #18,%d3 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x012212); } /* 011292 jsr %pc@(0x12212) */
	{ uint32_t _v = 0xEB9Au; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011296 movew #-5222,%d1 */
	{ hle_insn(ctx, 18); sub_0112DE(ctx); } /* 01129A bsrw 0x112de */
	hle_insn(ctx, 10); goto L_0112A4; /* 01129E braw 0x112a4 */
L_0112A2:;
	hle_insn(ctx, 4); /* 0112A2 nop */
L_0112A4:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 0112A4 braw 0xb3be */
}

/* $0112FE $27 T-1000: walk (move, scroll, animation) until the timer runs out */
HLE_RANGE_PURE(0x0112FE, L8T1000Walk, 1000, EXIT_NEXT)
{
	{ hle_insn(ctx, 18); ObjMoveX(ctx); } /* 0112FE jsr %pc@(0xb4a6) */
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 011302 jsr %pc@(0x12316) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 011306 movew %fp@(36),%d1 */
	{ uint32_t _s = 0x38u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 01130A andiw #56,%d1 */
	{ sh_w(ctx, 1, 'r', 3); hle_insn(ctx, 12); } /* 01130E lsrw #3,%d1 */
	{ uint32_t _s = 0x7u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d ^ _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 011310 eoriw #7,%d1 */
	{ hle_insn(ctx, 18); sub_011616(ctx); } /* 011314 jsr %pc@(0x11616) */
	{ hle_insn(ctx, 18); sub_011334(ctx); } /* 011318 jsr %pc@(0x11334) */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 01131C subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_011330; /* 011320 bnes 0x11330 */
	{ uint32_t _v = 0x11448u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 011322 movel #70728,%fp@(32) */
	{ uint32_t _v = 0x40u; st16((ctx->a[6] + (uint32_t)(int32_t)20), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 01132A movew #64,%fp@(20) */
L_011330:;
	hle_insn(ctx, 10); goto L_011B10; /* 011330 braw 0x11b10 */
L_011B10:;
	{ cc_logic8(ctx, ld8(0xFFFF9EC2u)); hle_insn(ctx, 16); } /* 011B10 tstb 0xffff9ec2 */
	if (bw(ctx, c_z(ctx))) goto L_011B4A; /* 011B16 beqw 0x11b4a */
	{ uint32_t _v = 0x11A90u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 011B1A movel #72336,%fp@(32) */
	{ uint32_t _v = 0xFFu; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 011B22 movew #255,%fp@(36) */
	{ uint32_t _v = ld16(0xFFFFAAFCu); st16(0xFFFFAB1Cu, _v); cc_logic16(ctx, _v); hle_insn(ctx, 28); } /* 011B28 movew 0xffffaafc,0xffffab1c */
	{ uint32_t _v = ld32(0xFFFF9ECAu); ctx->a[1] = _v; hle_insn(ctx, 20); } /* 011B32 moveal 0xffff9eca,%a1 */
	{ uint32_t _v = 0x11C2Cu; st32((ctx->a[1] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 011B38 movel #72748,%a1@(32) */
	{ st8(0xFFFF9EC2u, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 20); } /* 011B40 clrb 0xffff9ec2 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o & 0x7F); hle_insn(ctx, 16); } /* 011B46 bclr #15,%fp@ */
L_011B4A:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 011B4A braw 0xb3be */
}

/* $011360 $27 T-1000: walk, second animation set */
HLE_RANGE_PURE(0x011360, L8T1000Walk2, 1000, EXIT_NEXT)
{
	{ hle_insn(ctx, 18); ObjMoveX(ctx); } /* 011360 jsr %pc@(0xb4a6) */
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 011364 jsr %pc@(0x12316) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 011368 movew %fp@(36),%d1 */
	{ uint32_t _s = 0x38u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 01136C andiw #56,%d1 */
	{ sh_w(ctx, 1, 'r', 3); hle_insn(ctx, 12); } /* 011370 lsrw #3,%d1 */
	{ uint32_t _s = 0x7u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d ^ _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 011372 eoriw #7,%d1 */
	{ uint32_t _s = 0x14u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 011376 addiw #20,%d1 */
	{ hle_insn(ctx, 18); sub_011616(ctx); } /* 01137A jsr %pc@(0x11616) */
	{ hle_insn(ctx, 18); sub_011334(ctx); } /* 01137E jsr %pc@(0x11334) */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 011382 subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_011390; /* 011386 bnes 0x11390 */
	{ uint32_t _v = 0x11448u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 011388 movel #70728,%fp@(32) */
L_011390:;
	hle_insn(ctx, 10); goto L_011B10; /* 011390 braw 0x11b10 */
L_011B10:;
	{ cc_logic8(ctx, ld8(0xFFFF9EC2u)); hle_insn(ctx, 16); } /* 011B10 tstb 0xffff9ec2 */
	if (bw(ctx, c_z(ctx))) goto L_011B4A; /* 011B16 beqw 0x11b4a */
	{ uint32_t _v = 0x11A90u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 011B1A movel #72336,%fp@(32) */
	{ uint32_t _v = 0xFFu; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 011B22 movew #255,%fp@(36) */
	{ uint32_t _v = ld16(0xFFFFAAFCu); st16(0xFFFFAB1Cu, _v); cc_logic16(ctx, _v); hle_insn(ctx, 28); } /* 011B28 movew 0xffffaafc,0xffffab1c */
	{ uint32_t _v = ld32(0xFFFF9ECAu); ctx->a[1] = _v; hle_insn(ctx, 20); } /* 011B32 moveal 0xffff9eca,%a1 */
	{ uint32_t _v = 0x11C2Cu; st32((ctx->a[1] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 011B38 movel #72748,%a1@(32) */
	{ st8(0xFFFF9EC2u, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 20); } /* 011B40 clrb 0xffff9ec2 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o & 0x7F); hle_insn(ctx, 16); } /* 011B46 bclr #15,%fp@ */
L_011B4A:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 011B4A braw 0xb3be */
}

/* $011394 $27 T-1000: attack animation with sound $61 and sub_009066 */
HLE_RANGE(0x011394, L8T1000Act1, 15500, EXIT_NEXT)
{
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 011394 movew %fp@(36),%d1 */
	{ uint32_t _s = 0x38u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 011398 andiw #56,%d1 */
	{ sh_w(ctx, 1, 'r', 3); hle_insn(ctx, 12); } /* 01139C lsrw #3,%d1 */
	{ uint32_t _v = ld8((0x0113D0u + (uint32_t)((int32_t)(int16_t)ctx->d[1]))); set_b(&ctx->d[1], _v); cc_logic8(ctx, _v); hle_insn(ctx, 14); } /* 01139E moveb %pc@(0x113d0,%d1:w),%d1 */
	{ hle_insn(ctx, 18); sub_011616(ctx); } /* 0113A2 jsr %pc@(0x11616) */
	{ hle_insn(ctx, 18); sub_011334(ctx); } /* 0113A6 jsr %pc@(0x11334) */
	{ uint32_t _s = 0x30u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 0113AA cmpiw #48,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_0113B8; /* 0113B0 bnes 0x113b8 */
	{ ctx->d[0] = 0x61u; cc_logic32(ctx, ctx->d[0]); hle_insn(ctx, 4); } /* 0113B2 moveq #97,%d0 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x013D04); } /* 0113B4 jsr %pc@(0x13d04) */
L_0113B8:;
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_call68k(ctx, 0x009066); } /* 0113B8 jsr 0x9066 */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 0113BE subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_0113CC; /* 0113C2 bnes 0x113cc */
	{ uint32_t _v = 0x11448u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 0113C4 movel #70728,%fp@(32) */
L_0113CC:;
	hle_insn(ctx, 10); goto L_011B10; /* 0113CC braw 0x11b10 */
L_011B10:;
	{ cc_logic8(ctx, ld8(0xFFFF9EC2u)); hle_insn(ctx, 16); } /* 011B10 tstb 0xffff9ec2 */
	if (bw(ctx, c_z(ctx))) goto L_011B4A; /* 011B16 beqw 0x11b4a */
	{ uint32_t _v = 0x11A90u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 011B1A movel #72336,%fp@(32) */
	{ uint32_t _v = 0xFFu; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 011B22 movew #255,%fp@(36) */
	{ uint32_t _v = ld16(0xFFFFAAFCu); st16(0xFFFFAB1Cu, _v); cc_logic16(ctx, _v); hle_insn(ctx, 28); } /* 011B28 movew 0xffffaafc,0xffffab1c */
	{ uint32_t _v = ld32(0xFFFF9ECAu); ctx->a[1] = _v; hle_insn(ctx, 20); } /* 011B32 moveal 0xffff9eca,%a1 */
	{ uint32_t _v = 0x11C2Cu; st32((ctx->a[1] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 011B38 movel #72748,%a1@(32) */
	{ st8(0xFFFF9EC2u, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 20); } /* 011B40 clrb 0xffff9ec2 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o & 0x7F); hle_insn(ctx, 16); } /* 011B46 bclr #15,%fp@ */
L_011B4A:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 011B4A braw 0xb3be */
}

/* $0113D8 $27 T-1000: attack animation, sprays debris at timer $10 */
HLE_RANGE(0x0113D8, L8T1000Act2, 28500, EXIT_NEXT)
{
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0113D8 movew %fp@(36),%d1 */
	{ uint32_t _s = 0x3Cu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 0113DC andiw #60,%d1 */
	{ sh_w(ctx, 1, 'r', 2); hle_insn(ctx, 10); } /* 0113E0 lsrw #2,%d1 */
	{ uint32_t _v = ld8((0x011438u + (uint32_t)((int32_t)(int16_t)ctx->d[1]))); set_b(&ctx->d[1], _v); cc_logic8(ctx, _v); hle_insn(ctx, 14); } /* 0113E2 moveb %pc@(0x11438,%d1:w),%d1 */
	{ hle_insn(ctx, 18); sub_011616(ctx); } /* 0113E6 jsr %pc@(0x11616) */
	{ hle_insn(ctx, 18); sub_011334(ctx); } /* 0113EA jsr %pc@(0x11334) */
	{ uint32_t _s = 0x10u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 0113EE cmpiw #16,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_011426; /* 0113F4 bnes 0x11426 */
	{ uint32_t _v = 0x5u; set_w(&ctx->d[4], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0113F6 movew #5,%d4 */
L_0113FA:;
	{ uint32_t _a = (ctx->a[6] + (uint32_t)(int32_t)2); ctx->d[0] = (uint32_t)(int32_t)(int16_t)ld16(_a + 0); ctx->d[1] = (uint32_t)(int32_t)(int16_t)ld16(_a + 2); hle_insn(ctx, 24); } /* 0113FA movemw %fp@(2),%d0-%d1 */
	{ uint32_t _s = 0x4u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 011400 addqw #4,%d0 */
	{ uint32_t _s = 0x4Bu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 011402 subiw #75,%d1 */
	{ ctx->a[2] = 0x012866u; hle_insn(ctx, 8); } /* 011406 lea %pc@(0x12866),%a2 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x01277C); } /* 01140A bsrw 0x1277c */
	{ uint16_t _c = (uint16_t)ctx->d[4] - 1; set_w(&ctx->d[4], _c); if (_c != 0xFFFF) { hle_insn(ctx, 10); goto L_0113FA; } hle_insn(ctx, 14); } /* 01140E dbf %d4,0x113fa */
	{ ctx->d[0] = 0x7Bu; cc_logic32(ctx, ctx->d[0]); hle_insn(ctx, 4); } /* 011412 moveq #123,%d0 */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_call68k(ctx, 0x013D04); } /* 011414 jsr 0x13d04 */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_call68k(ctx, 0x00908A); } /* 01141A jsr 0x908a */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_call68k(ctx, 0x00908A); } /* 011420 jsr 0x908a */
L_011426:;
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 011426 subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_011434; /* 01142A bnes 0x11434 */
	{ uint32_t _v = 0x11448u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 01142C movel #70728,%fp@(32) */
L_011434:;
	hle_insn(ctx, 10); goto L_011B10; /* 011434 braw 0x11b10 */
L_011B10:;
	{ cc_logic8(ctx, ld8(0xFFFF9EC2u)); hle_insn(ctx, 16); } /* 011B10 tstb 0xffff9ec2 */
	if (bw(ctx, c_z(ctx))) goto L_011B4A; /* 011B16 beqw 0x11b4a */
	{ uint32_t _v = 0x11A90u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 011B1A movel #72336,%fp@(32) */
	{ uint32_t _v = 0xFFu; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 011B22 movew #255,%fp@(36) */
	{ uint32_t _v = ld16(0xFFFFAAFCu); st16(0xFFFFAB1Cu, _v); cc_logic16(ctx, _v); hle_insn(ctx, 28); } /* 011B28 movew 0xffffaafc,0xffffab1c */
	{ uint32_t _v = ld32(0xFFFF9ECAu); ctx->a[1] = _v; hle_insn(ctx, 20); } /* 011B32 moveal 0xffff9eca,%a1 */
	{ uint32_t _v = 0x11C2Cu; st32((ctx->a[1] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 011B38 movel #72748,%a1@(32) */
	{ st8(0xFFFF9EC2u, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 20); } /* 011B40 clrb 0xffff9ec2 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o & 0x7F); hle_insn(ctx, 16); } /* 011B46 bclr #15,%fp@ */
L_011B4A:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 011B4A braw 0xb3be */
}

/* $011448 $27 T-1000: pick the next action at random */
HLE_RANGE_PURE(0x011448, L8T1000Pick, 1000, 0x0112FE, 0x011360, 0x011394, 0x0113D8)
{
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o | 0x80); hle_insn(ctx, 16); } /* 011448 bset #15,%fp@ */
	{ hle_insn(ctx, 20); Random(ctx); } /* 01144C jsr 0x881c */
	{ uint32_t _s = 0x12Cu; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)2); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 011452 cmpiw #300,%fp@(2) */
	if (bs(ctx, c_gt(ctx))) goto L_011462; /* 011458 bgts 0x11462 */
	{ uint32_t _s = 0x14u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)2); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 01145A cmpiw #20,%fp@(2) */
	if (bs(ctx, c_gt(ctx))) goto L_011466; /* 011460 bgts 0x11466 */
L_011462:;
	{ uint32_t _s = 0x3u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 011462 andiw #3,%d0 */
L_011466:;
	{ uint32_t _s = 0x7u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 011466 andiw #7,%d0 */
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 01146A cmpiw #1,%d0 */
	if (bs(ctx, c_gt(ctx))) goto L_011486; /* 01146E bgts 0x11486 */
	{ uint32_t _v = 0x11360u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 011470 movel #70496,%fp@(32) */
	{ uint32_t _v = 0x3Fu; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 011478 movew #63,%fp@(36) */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o & 0x7F); hle_insn(ctx, 16); } /* 01147E bclr #15,%fp@ */
	hle_insn(ctx, 10); goto L_0114BA; /* 011482 braw 0x114ba */
L_011486:;
	{ uint32_t _s = 0x3u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 011486 cmpiw #3,%d0 */
	if (bs(ctx, c_gt(ctx))) goto L_011494; /* 01148A bgts 0x11494 */
	{ hle_insn(ctx, 18); sub_0115FA(ctx); } /* 01148C bsrw 0x115fa */
	hle_insn(ctx, 10); goto L_0114BA; /* 011490 braw 0x114ba */
L_011494:;
	{ uint32_t _s = 0x5u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 011494 cmpiw #5,%d0 */
	if (bs(ctx, c_gt(ctx))) goto L_0114AC; /* 011498 bgts 0x114ac */
	{ uint32_t _v = 0x113D8u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 01149A movel #70616,%fp@(32) */
	{ uint32_t _v = 0x3Fu; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 0114A2 movew #63,%fp@(36) */
	hle_insn(ctx, 10); goto L_0114BA; /* 0114A8 braw 0x114ba */
L_0114AC:;
	{ uint32_t _v = 0x11394u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 0114AC movel #70548,%fp@(32) */
	{ uint32_t _v = 0x40u; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 0114B4 movew #64,%fp@(36) */
L_0114BA:;
	{ uint32_t _v = ld32((ctx->a[6] + (uint32_t)(int32_t)32)); ctx->a[1] = _v; hle_insn(ctx, 16); } /* 0114BA moveal %fp@(32),%a1 */
	hle_insn(ctx, 8); hle_jump(ctx, ctx->a[1]); return; /* 0114BE jmp %a1@ */
}

/* $0114C0 $27 T-1000: turn around at the screen edge */
HLE_RANGE_PURE(0x0114C0, L8T1000Turn, 1000, EXIT_NEXT)
{
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 0114C0 subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_011510; /* 0114C4 bnes 0x11510 */
	{ uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)14); uint32_t _r = alu(ctx, OP_SUB, 0, ld8(_da), 8, 1); st8(_da, _r); hle_insn(ctx, 16); } /* 0114C6 negb %fp@(14) */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 6) & 1) ? 0 : CC_Z); st8(_a, _o ^ 0x40); hle_insn(ctx, 16); } /* 0114CA bchg #14,%fp@ */
	if (bs(ctx, !c_z(ctx))) goto L_0114D8; /* 0114CE bnes 0x114d8 */
	{ uint32_t _v = 0xFFECu; st16((ctx->a[6] + (uint32_t)(int32_t)2), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 0114D0 movew #-20,%fp@(2) */
	hle_insn(ctx, 10); goto L_0114DE; /* 0114D6 bras 0x114de */
L_0114D8:;
	{ uint32_t _v = 0x14Au; st16((ctx->a[6] + (uint32_t)(int32_t)2), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 0114D8 movew #330,%fp@(2) */
L_0114DE:;
	{ hle_insn(ctx, 20); Random(ctx); } /* 0114DE jsr 0x881c */
	{ cc_logic16(ctx, (ctx->d[0] & 0xFFFFu)); hle_insn(ctx, 4); } /* 0114E4 tstw %d0 */
	if (bs(ctx, !c_n(ctx))) goto L_0114EE; /* 0114E6 bpls 0x114ee */
	{ hle_insn(ctx, 18); sub_0115FA(ctx); } /* 0114E8 bsrw 0x115fa */
	hle_insn(ctx, 10); goto L_011510; /* 0114EC bras 0x11510 */
L_0114EE:;
	{ uint32_t _v = 0x11A44u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 0114EE movel #72260,%fp@(32) */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o & 0x7F); hle_insn(ctx, 16); } /* 0114F6 bclr #15,%fp@ */
	{ uint32_t _s = 0xFFu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0114FA andiw #255,%d0 */
	{ uint32_t _s = 0x20u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0114FE addiw #32,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)2), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 011502 movew %d0,%fp@(2) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); st16((ctx->a[6] + (uint32_t)(int32_t)40), _v); cc_logic16(ctx, _v); hle_insn(ctx, 20); } /* 011506 movew %fp@(4),%fp@(40) */
	{ st16((ctx->a[6] + (uint32_t)(int32_t)4), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 01150C clrw %fp@(4) */
L_011510:;
	hle_insn(ctx, 10); goto L_011B10; /* 011510 braw 0x11b10 */
L_011B10:;
	{ cc_logic8(ctx, ld8(0xFFFF9EC2u)); hle_insn(ctx, 16); } /* 011B10 tstb 0xffff9ec2 */
	if (bw(ctx, c_z(ctx))) goto L_011B4A; /* 011B16 beqw 0x11b4a */
	{ uint32_t _v = 0x11A90u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 011B1A movel #72336,%fp@(32) */
	{ uint32_t _v = 0xFFu; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 011B22 movew #255,%fp@(36) */
	{ uint32_t _v = ld16(0xFFFFAAFCu); st16(0xFFFFAB1Cu, _v); cc_logic16(ctx, _v); hle_insn(ctx, 28); } /* 011B28 movew 0xffffaafc,0xffffab1c */
	{ uint32_t _v = ld32(0xFFFF9ECAu); ctx->a[1] = _v; hle_insn(ctx, 20); } /* 011B32 moveal 0xffff9eca,%a1 */
	{ uint32_t _v = 0x11C2Cu; st32((ctx->a[1] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 011B38 movel #72748,%a1@(32) */
	{ st8(0xFFFF9EC2u, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 20); } /* 011B40 clrb 0xffff9ec2 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o & 0x7F); hle_insn(ctx, 16); } /* 011B46 bclr #15,%fp@ */
L_011B4A:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 011B4A braw 0xb3be */
}

/* $011514 $27 T-1000: shot (knocked back) */
HLE_RANGE_PURE(0x011514, L8T1000Hit, 1000, EXIT_NEXT)
{
	{ ctx->d[1] = 0x11u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 011514 moveq #17,%d1 */
	{ uint32_t _s = 0x3u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 011516 cmpiw #3,%fp@(36) */
	if (bs(ctx, c_gt(ctx))) goto L_011520; /* 01151C bgts 0x11520 */
	{ ctx->d[1] = 0x10u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 01151E moveq #16,%d1 */
L_011520:;
	{ hle_insn(ctx, 18); sub_011616(ctx); } /* 011520 bsrw 0x11616 */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 011524 subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_011532; /* 011528 bnes 0x11532 */
	{ uint32_t _v = 0x11448u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 01152A movel #70728,%fp@(32) */
L_011532:;
	hle_insn(ctx, 10); goto L_011B10; /* 011532 braw 0x11b10 */
L_011B10:;
	{ cc_logic8(ctx, ld8(0xFFFF9EC2u)); hle_insn(ctx, 16); } /* 011B10 tstb 0xffff9ec2 */
	if (bw(ctx, c_z(ctx))) goto L_011B4A; /* 011B16 beqw 0x11b4a */
	{ uint32_t _v = 0x11A90u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 011B1A movel #72336,%fp@(32) */
	{ uint32_t _v = 0xFFu; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 011B22 movew #255,%fp@(36) */
	{ uint32_t _v = ld16(0xFFFFAAFCu); st16(0xFFFFAB1Cu, _v); cc_logic16(ctx, _v); hle_insn(ctx, 28); } /* 011B28 movew 0xffffaafc,0xffffab1c */
	{ uint32_t _v = ld32(0xFFFF9ECAu); ctx->a[1] = _v; hle_insn(ctx, 20); } /* 011B32 moveal 0xffff9eca,%a1 */
	{ uint32_t _v = 0x11C2Cu; st32((ctx->a[1] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 011B38 movel #72748,%a1@(32) */
	{ st8(0xFFFF9EC2u, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 20); } /* 011B40 clrb 0xffff9ec2 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o & 0x7F); hle_insn(ctx, 16); } /* 011B46 bclr #15,%fp@ */
L_011B4A:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 011B4A braw 0xb3be */
}

/* $011558 $27 T-1000: knocked down, scroll */
HLE_RANGE_PURE(0x011558, L8T1000Down, 1000, EXIT_NEXT)
{
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 011558 jsr %pc@(0x12316) */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 01155C subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_011574; /* 011560 bnes 0x11574 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 5) & 1) ? 0 : CC_Z); st8(_a, _o | 0x20); hle_insn(ctx, 16); } /* 011562 bset #13,%fp@ */
	{ uint32_t _v = 0x11580u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 011566 movel #71040,%fp@(32) */
	{ uint32_t _v = 0x18u; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 01156E movew #24,%fp@(36) */
L_011574:;
	{ uint32_t _v = 0x21u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011574 movew #33,%d1 */
	{ hle_insn(ctx, 18); sub_011616(ctx); } /* 011578 jsr %pc@(0x11616) */
	hle_insn(ctx, 10); goto L_011B10; /* 01157C braw 0x11b10 */
L_011B10:;
	{ cc_logic8(ctx, ld8(0xFFFF9EC2u)); hle_insn(ctx, 16); } /* 011B10 tstb 0xffff9ec2 */
	if (bw(ctx, c_z(ctx))) goto L_011B4A; /* 011B16 beqw 0x11b4a */
	{ uint32_t _v = 0x11A90u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 011B1A movel #72336,%fp@(32) */
	{ uint32_t _v = 0xFFu; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 011B22 movew #255,%fp@(36) */
	{ uint32_t _v = ld16(0xFFFFAAFCu); st16(0xFFFFAB1Cu, _v); cc_logic16(ctx, _v); hle_insn(ctx, 28); } /* 011B28 movew 0xffffaafc,0xffffab1c */
	{ uint32_t _v = ld32(0xFFFF9ECAu); ctx->a[1] = _v; hle_insn(ctx, 20); } /* 011B32 moveal 0xffff9eca,%a1 */
	{ uint32_t _v = 0x11C2Cu; st32((ctx->a[1] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 011B38 movel #72748,%a1@(32) */
	{ st8(0xFFFF9EC2u, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 20); } /* 011B40 clrb 0xffff9ec2 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o & 0x7F); hle_insn(ctx, 16); } /* 011B46 bclr #15,%fp@ */
L_011B4A:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 011B4A braw 0xb3be */
}

/* $011580 $27 T-1000 shattered: init, falls into the fall state */
HLE_RANGE(0x011580, L8T1000Shatter, 2500, EXIT_NEXT, 0x0112FE, 0x011360, 0x011394, 0x0113D8)
{
	goto L_011580;
L_011448:;
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o | 0x80); hle_insn(ctx, 16); } /* 011448 bset #15,%fp@ */
	{ hle_insn(ctx, 20); Random(ctx); } /* 01144C jsr 0x881c */
	{ uint32_t _s = 0x12Cu; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)2); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 011452 cmpiw #300,%fp@(2) */
	if (bs(ctx, c_gt(ctx))) goto L_011462; /* 011458 bgts 0x11462 */
	{ uint32_t _s = 0x14u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)2); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 01145A cmpiw #20,%fp@(2) */
	if (bs(ctx, c_gt(ctx))) goto L_011466; /* 011460 bgts 0x11466 */
L_011462:;
	{ uint32_t _s = 0x3u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 011462 andiw #3,%d0 */
L_011466:;
	{ uint32_t _s = 0x7u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 011466 andiw #7,%d0 */
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 01146A cmpiw #1,%d0 */
	if (bs(ctx, c_gt(ctx))) goto L_011486; /* 01146E bgts 0x11486 */
	{ uint32_t _v = 0x11360u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 011470 movel #70496,%fp@(32) */
	{ uint32_t _v = 0x3Fu; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 011478 movew #63,%fp@(36) */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o & 0x7F); hle_insn(ctx, 16); } /* 01147E bclr #15,%fp@ */
	hle_insn(ctx, 10); goto L_0114BA; /* 011482 braw 0x114ba */
L_011486:;
	{ uint32_t _s = 0x3u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 011486 cmpiw #3,%d0 */
	if (bs(ctx, c_gt(ctx))) goto L_011494; /* 01148A bgts 0x11494 */
	{ hle_insn(ctx, 18); sub_0115FA(ctx); } /* 01148C bsrw 0x115fa */
	hle_insn(ctx, 10); goto L_0114BA; /* 011490 braw 0x114ba */
L_011494:;
	{ uint32_t _s = 0x5u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 011494 cmpiw #5,%d0 */
	if (bs(ctx, c_gt(ctx))) goto L_0114AC; /* 011498 bgts 0x114ac */
	{ uint32_t _v = 0x113D8u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 01149A movel #70616,%fp@(32) */
	{ uint32_t _v = 0x3Fu; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 0114A2 movew #63,%fp@(36) */
	hle_insn(ctx, 10); goto L_0114BA; /* 0114A8 braw 0x114ba */
L_0114AC:;
	{ uint32_t _v = 0x11394u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 0114AC movel #70548,%fp@(32) */
	{ uint32_t _v = 0x40u; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 0114B4 movew #64,%fp@(36) */
L_0114BA:;
	{ uint32_t _v = ld32((ctx->a[6] + (uint32_t)(int32_t)32)); ctx->a[1] = _v; hle_insn(ctx, 16); } /* 0114BA moveal %fp@(32),%a1 */
	hle_insn(ctx, 8); hle_jump(ctx, ctx->a[1]); return; /* 0114BE jmp %a1@ */
L_011536:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)40)); st16((ctx->a[6] + (uint32_t)(int32_t)4), _v); cc_logic16(ctx, _v); hle_insn(ctx, 20); } /* 011536 movew %fp@(40),%fp@(4) */
	{ uint32_t _v = 0x30u; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 01153C movew #48,%fp@(36) */
	{ uint32_t _v = 0x11558u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 011542 movel #71000,%fp@(32) */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 5) & 1) ? 0 : CC_Z); hle_insn(ctx, 12); } /* 01154A btst #13,%fp@ */
	if (bs(ctx, c_z(ctx))) goto L_011558; /* 01154E beqs 0x11558 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 5) & 1) ? 0 : CC_Z); st8(_a, _o & 0xDF); hle_insn(ctx, 16); } /* 011550 bclr #13,%fp@ */
	hle_insn(ctx, 10); goto L_011448; /* 011554 braw 0x11448 */
L_011558:;
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 011558 jsr %pc@(0x12316) */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 01155C subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_011574; /* 011560 bnes 0x11574 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 5) & 1) ? 0 : CC_Z); st8(_a, _o | 0x20); hle_insn(ctx, 16); } /* 011562 bset #13,%fp@ */
	{ uint32_t _v = 0x11580u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 011566 movel #71040,%fp@(32) */
	{ uint32_t _v = 0x18u; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 01156E movew #24,%fp@(36) */
L_011574:;
	{ uint32_t _v = 0x21u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011574 movew #33,%d1 */
	{ hle_insn(ctx, 18); sub_011616(ctx); } /* 011578 jsr %pc@(0x11616) */
	hle_insn(ctx, 10); goto L_011B10; /* 01157C braw 0x11b10 */
L_011580:;
	{ st8((ctx->a[6] + (uint32_t)(int32_t)13), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 011580 clrb %fp@(13) */
	{ uint32_t _v = 0xAu; st8((ctx->a[6] + (uint32_t)(int32_t)18), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 011584 moveb #10,%fp@(18) */
	{ uint32_t _v = 0x96u; st8((ctx->a[6] + (uint32_t)(int32_t)12), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 01158A moveb #-106,%fp@(12) */
	{ uint32_t _v = 0xFFu; st8((ctx->a[6] + (uint32_t)(int32_t)15), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 011590 moveb #-1,%fp@(15) */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o & 0x7F); hle_insn(ctx, 16); } /* 011596 bclr #15,%fp@ */
	{ uint32_t _v = 0x20u; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 01159A movew #32,%fp@(36) */
	{ uint32_t _v = 0x115ACu; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 0115A0 movel #71084,%fp@(32) */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x013DA8); } /* 0115A8 jsr %pc@(0x13da8) */
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 0115AC jsr %pc@(0x12316) */
	{ hle_insn(ctx, 18); sub_00D236(ctx); } /* 0115B0 jsr %pc@(0xd236) */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 0115B4 subqw #1,%fp@(36) */
	if (bs(ctx, !c_n(ctx))) goto L_0115CA; /* 0115B8 bpls 0x115ca */
	{ ctx->d[1] = 0x4u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 0115BA moveq #4,%d1 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0115BC movew %fp@(4),%d0 */
	{ uint32_t _s = ld16((ctx->a[6] + (uint32_t)(int32_t)40)); uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 12); } /* 0115C0 cmpw %fp@(40),%d0 */
	if (bw(ctx, c_gt(ctx))) goto L_011536; /* 0115C4 bgtw 0x11536 */
	hle_insn(ctx, 10); goto L_0115DE; /* 0115C8 bras 0x115de */
L_0115CA:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0115CA movew %fp@(36),%d1 */
	{ sh_w(ctx, 1, 'a', 1); hle_insn(ctx, 8); } /* 0115CE asrw #1,%d1 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 5) & 1) ? 0 : CC_Z); hle_insn(ctx, 12); } /* 0115D0 btst #13,%fp@ */
	if (bs(ctx, !c_z(ctx))) goto L_0115DA; /* 0115D4 bnes 0x115da */
	{ uint32_t _s = 0xFu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d ^ _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 0115D6 eoriw #15,%d1 */
L_0115DA:;
	{ uint32_t _v = ld8((0x0115EAu + (uint32_t)((int32_t)(int16_t)ctx->d[1]))); set_b(&ctx->d[1], _v); cc_logic8(ctx, _v); hle_insn(ctx, 14); } /* 0115DA moveb %pc@(0x115ea,%d1:w),%d1 */
L_0115DE:;
	{ uint32_t _s = 0x1Du; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 0115DE addiw #29,%d1 */
	{ hle_insn(ctx, 18); sub_011616(ctx); } /* 0115E2 jsr %pc@(0x11616) */
	hle_insn(ctx, 10); goto L_011B10; /* 0115E6 braw 0x11b10 */
L_011B10:;
	{ cc_logic8(ctx, ld8(0xFFFF9EC2u)); hle_insn(ctx, 16); } /* 011B10 tstb 0xffff9ec2 */
	if (bw(ctx, c_z(ctx))) goto L_011B4A; /* 011B16 beqw 0x11b4a */
	{ uint32_t _v = 0x11A90u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 011B1A movel #72336,%fp@(32) */
	{ uint32_t _v = 0xFFu; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 011B22 movew #255,%fp@(36) */
	{ uint32_t _v = ld16(0xFFFFAAFCu); st16(0xFFFFAB1Cu, _v); cc_logic16(ctx, _v); hle_insn(ctx, 28); } /* 011B28 movew 0xffffaafc,0xffffab1c */
	{ uint32_t _v = ld32(0xFFFF9ECAu); ctx->a[1] = _v; hle_insn(ctx, 20); } /* 011B32 moveal 0xffff9eca,%a1 */
	{ uint32_t _v = 0x11C2Cu; st32((ctx->a[1] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 011B38 movel #72748,%a1@(32) */
	{ st8(0xFFFF9EC2u, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 20); } /* 011B40 clrb 0xffff9ec2 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o & 0x7F); hle_insn(ctx, 16); } /* 011B46 bclr #15,%fp@ */
L_011B4A:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 011B4A braw 0xb3be */
}

/* $0115AC $27 T-1000: fall/get up animation */
HLE_RANGE_PURE(0x0115AC, L8T1000Fall, 1000, EXIT_NEXT, 0x0112FE, 0x011360, 0x011394, 0x0113D8)
{
	goto L_0115AC;
L_011448:;
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o | 0x80); hle_insn(ctx, 16); } /* 011448 bset #15,%fp@ */
	{ hle_insn(ctx, 20); Random(ctx); } /* 01144C jsr 0x881c */
	{ uint32_t _s = 0x12Cu; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)2); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 011452 cmpiw #300,%fp@(2) */
	if (bs(ctx, c_gt(ctx))) goto L_011462; /* 011458 bgts 0x11462 */
	{ uint32_t _s = 0x14u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)2); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 01145A cmpiw #20,%fp@(2) */
	if (bs(ctx, c_gt(ctx))) goto L_011466; /* 011460 bgts 0x11466 */
L_011462:;
	{ uint32_t _s = 0x3u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 011462 andiw #3,%d0 */
L_011466:;
	{ uint32_t _s = 0x7u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 011466 andiw #7,%d0 */
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 01146A cmpiw #1,%d0 */
	if (bs(ctx, c_gt(ctx))) goto L_011486; /* 01146E bgts 0x11486 */
	{ uint32_t _v = 0x11360u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 011470 movel #70496,%fp@(32) */
	{ uint32_t _v = 0x3Fu; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 011478 movew #63,%fp@(36) */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o & 0x7F); hle_insn(ctx, 16); } /* 01147E bclr #15,%fp@ */
	hle_insn(ctx, 10); goto L_0114BA; /* 011482 braw 0x114ba */
L_011486:;
	{ uint32_t _s = 0x3u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 011486 cmpiw #3,%d0 */
	if (bs(ctx, c_gt(ctx))) goto L_011494; /* 01148A bgts 0x11494 */
	{ hle_insn(ctx, 18); sub_0115FA(ctx); } /* 01148C bsrw 0x115fa */
	hle_insn(ctx, 10); goto L_0114BA; /* 011490 braw 0x114ba */
L_011494:;
	{ uint32_t _s = 0x5u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 011494 cmpiw #5,%d0 */
	if (bs(ctx, c_gt(ctx))) goto L_0114AC; /* 011498 bgts 0x114ac */
	{ uint32_t _v = 0x113D8u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 01149A movel #70616,%fp@(32) */
	{ uint32_t _v = 0x3Fu; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 0114A2 movew #63,%fp@(36) */
	hle_insn(ctx, 10); goto L_0114BA; /* 0114A8 braw 0x114ba */
L_0114AC:;
	{ uint32_t _v = 0x11394u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 0114AC movel #70548,%fp@(32) */
	{ uint32_t _v = 0x40u; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 0114B4 movew #64,%fp@(36) */
L_0114BA:;
	{ uint32_t _v = ld32((ctx->a[6] + (uint32_t)(int32_t)32)); ctx->a[1] = _v; hle_insn(ctx, 16); } /* 0114BA moveal %fp@(32),%a1 */
	hle_insn(ctx, 8); hle_jump(ctx, ctx->a[1]); return; /* 0114BE jmp %a1@ */
L_011536:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)40)); st16((ctx->a[6] + (uint32_t)(int32_t)4), _v); cc_logic16(ctx, _v); hle_insn(ctx, 20); } /* 011536 movew %fp@(40),%fp@(4) */
	{ uint32_t _v = 0x30u; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 01153C movew #48,%fp@(36) */
	{ uint32_t _v = 0x11558u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 011542 movel #71000,%fp@(32) */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 5) & 1) ? 0 : CC_Z); hle_insn(ctx, 12); } /* 01154A btst #13,%fp@ */
	if (bs(ctx, c_z(ctx))) goto L_011558; /* 01154E beqs 0x11558 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 5) & 1) ? 0 : CC_Z); st8(_a, _o & 0xDF); hle_insn(ctx, 16); } /* 011550 bclr #13,%fp@ */
	hle_insn(ctx, 10); goto L_011448; /* 011554 braw 0x11448 */
L_011558:;
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 011558 jsr %pc@(0x12316) */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 01155C subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_011574; /* 011560 bnes 0x11574 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 5) & 1) ? 0 : CC_Z); st8(_a, _o | 0x20); hle_insn(ctx, 16); } /* 011562 bset #13,%fp@ */
	{ uint32_t _v = 0x11580u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 011566 movel #71040,%fp@(32) */
	{ uint32_t _v = 0x18u; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 01156E movew #24,%fp@(36) */
L_011574:;
	{ uint32_t _v = 0x21u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011574 movew #33,%d1 */
	{ hle_insn(ctx, 18); sub_011616(ctx); } /* 011578 jsr %pc@(0x11616) */
	hle_insn(ctx, 10); goto L_011B10; /* 01157C braw 0x11b10 */
L_0115AC:;
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 0115AC jsr %pc@(0x12316) */
	{ hle_insn(ctx, 18); sub_00D236(ctx); } /* 0115B0 jsr %pc@(0xd236) */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 0115B4 subqw #1,%fp@(36) */
	if (bs(ctx, !c_n(ctx))) goto L_0115CA; /* 0115B8 bpls 0x115ca */
	{ ctx->d[1] = 0x4u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 0115BA moveq #4,%d1 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0115BC movew %fp@(4),%d0 */
	{ uint32_t _s = ld16((ctx->a[6] + (uint32_t)(int32_t)40)); uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 12); } /* 0115C0 cmpw %fp@(40),%d0 */
	if (bw(ctx, c_gt(ctx))) goto L_011536; /* 0115C4 bgtw 0x11536 */
	hle_insn(ctx, 10); goto L_0115DE; /* 0115C8 bras 0x115de */
L_0115CA:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0115CA movew %fp@(36),%d1 */
	{ sh_w(ctx, 1, 'a', 1); hle_insn(ctx, 8); } /* 0115CE asrw #1,%d1 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 5) & 1) ? 0 : CC_Z); hle_insn(ctx, 12); } /* 0115D0 btst #13,%fp@ */
	if (bs(ctx, !c_z(ctx))) goto L_0115DA; /* 0115D4 bnes 0x115da */
	{ uint32_t _s = 0xFu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d ^ _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 0115D6 eoriw #15,%d1 */
L_0115DA:;
	{ uint32_t _v = ld8((0x0115EAu + (uint32_t)((int32_t)(int16_t)ctx->d[1]))); set_b(&ctx->d[1], _v); cc_logic8(ctx, _v); hle_insn(ctx, 14); } /* 0115DA moveb %pc@(0x115ea,%d1:w),%d1 */
L_0115DE:;
	{ uint32_t _s = 0x1Du; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 0115DE addiw #29,%d1 */
	{ hle_insn(ctx, 18); sub_011616(ctx); } /* 0115E2 jsr %pc@(0x11616) */
	hle_insn(ctx, 10); goto L_011B10; /* 0115E6 braw 0x11b10 */
L_011B10:;
	{ cc_logic8(ctx, ld8(0xFFFF9EC2u)); hle_insn(ctx, 16); } /* 011B10 tstb 0xffff9ec2 */
	if (bw(ctx, c_z(ctx))) goto L_011B4A; /* 011B16 beqw 0x11b4a */
	{ uint32_t _v = 0x11A90u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 011B1A movel #72336,%fp@(32) */
	{ uint32_t _v = 0xFFu; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 011B22 movew #255,%fp@(36) */
	{ uint32_t _v = ld16(0xFFFFAAFCu); st16(0xFFFFAB1Cu, _v); cc_logic16(ctx, _v); hle_insn(ctx, 28); } /* 011B28 movew 0xffffaafc,0xffffab1c */
	{ uint32_t _v = ld32(0xFFFF9ECAu); ctx->a[1] = _v; hle_insn(ctx, 20); } /* 011B32 moveal 0xffff9eca,%a1 */
	{ uint32_t _v = 0x11C2Cu; st32((ctx->a[1] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 011B38 movel #72748,%a1@(32) */
	{ st8(0xFFFF9EC2u, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 20); } /* 011B40 clrb 0xffff9ec2 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o & 0x7F); hle_insn(ctx, 16); } /* 011B46 bclr #15,%fp@ */
L_011B4A:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 011B4A braw 0xb3be */
}

/* $011A44 $27 T-1000: slide to the stored X */
HLE_RANGE_PURE(0x011A44, L8T1000Slide, 1000, EXIT_NEXT)
{
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 011A44 movew %fp@(4),%d0 */
	{ uint32_t _s = 0x8u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 011A48 addqw #8,%d0 */
	{ uint32_t _s = ld16((ctx->a[6] + (uint32_t)(int32_t)40)); uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 12); } /* 011A4A cmpw %fp@(40),%d0 */
	if (bs(ctx, !c_ge(ctx))) goto L_011A66; /* 011A4E blts 0x11a66 */
	{ uint32_t _v = 0x11A76u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 011A50 movel #72310,%fp@(32) */
	{ uint32_t _v = 0xFu; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 011A58 movew #15,%fp@(36) */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o & 0x7F); hle_insn(ctx, 16); } /* 011A5E bclr #15,%fp@ */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)40)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 011A62 movew %fp@(40),%d0 */
L_011A66:;
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)4), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 011A66 movew %d0,%fp@(4) */
	{ uint32_t _v = 0x1Cu; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011A6A movew #28,%d1 */
	{ hle_insn(ctx, 18); sub_011616(ctx); } /* 011A6E bsrw 0x11616 */
	hle_insn(ctx, 10); goto L_011B10; /* 011A72 braw 0x11b10 */
L_011B10:;
	{ cc_logic8(ctx, ld8(0xFFFF9EC2u)); hle_insn(ctx, 16); } /* 011B10 tstb 0xffff9ec2 */
	if (bw(ctx, c_z(ctx))) goto L_011B4A; /* 011B16 beqw 0x11b4a */
	{ uint32_t _v = 0x11A90u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 011B1A movel #72336,%fp@(32) */
	{ uint32_t _v = 0xFFu; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 011B22 movew #255,%fp@(36) */
	{ uint32_t _v = ld16(0xFFFFAAFCu); st16(0xFFFFAB1Cu, _v); cc_logic16(ctx, _v); hle_insn(ctx, 28); } /* 011B28 movew 0xffffaafc,0xffffab1c */
	{ uint32_t _v = ld32(0xFFFF9ECAu); ctx->a[1] = _v; hle_insn(ctx, 20); } /* 011B32 moveal 0xffff9eca,%a1 */
	{ uint32_t _v = 0x11C2Cu; st32((ctx->a[1] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 011B38 movel #72748,%a1@(32) */
	{ st8(0xFFFF9EC2u, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 20); } /* 011B40 clrb 0xffff9ec2 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o & 0x7F); hle_insn(ctx, 16); } /* 011B46 bclr #15,%fp@ */
L_011B4A:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 011B4A braw 0xb3be */
}

/* $011A76 $27 T-1000: flash pose */
HLE_RANGE_PURE(0x011A76, L8T1000Flash, 1000, EXIT_NEXT)
{
	{ uint32_t _v = 0x1Du; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011A76 movew #29,%d1 */
	{ hle_insn(ctx, 18); sub_011616(ctx); } /* 011A7A bsrw 0x11616 */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 011A7E subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_011A8C; /* 011A82 bnes 0x11a8c */
	{ uint32_t _v = 0x11448u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 011A84 movel #70728,%fp@(32) */
L_011A8C:;
	hle_insn(ctx, 10); goto L_011B10; /* 011A8C braw 0x11b10 */
L_011B10:;
	{ cc_logic8(ctx, ld8(0xFFFF9EC2u)); hle_insn(ctx, 16); } /* 011B10 tstb 0xffff9ec2 */
	if (bw(ctx, c_z(ctx))) goto L_011B4A; /* 011B16 beqw 0x11b4a */
	{ uint32_t _v = 0x11A90u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 011B1A movel #72336,%fp@(32) */
	{ uint32_t _v = 0xFFu; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 011B22 movew #255,%fp@(36) */
	{ uint32_t _v = ld16(0xFFFFAAFCu); st16(0xFFFFAB1Cu, _v); cc_logic16(ctx, _v); hle_insn(ctx, 28); } /* 011B28 movew 0xffffaafc,0xffffab1c */
	{ uint32_t _v = ld32(0xFFFF9ECAu); ctx->a[1] = _v; hle_insn(ctx, 20); } /* 011B32 moveal 0xffff9eca,%a1 */
	{ uint32_t _v = 0x11C2Cu; st32((ctx->a[1] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 011B38 movel #72748,%a1@(32) */
	{ st8(0xFFFF9EC2u, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 20); } /* 011B40 clrb 0xffff9ec2 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o & 0x7F); hle_insn(ctx, 16); } /* 011B46 bclr #15,%fp@ */
L_011B4A:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 011B4A braw 0xb3be */
}

/* $011A90 $27 T-1000 frozen: countdown, then the shatter spray */
HLE_RANGE(0x011A90, L8T1000Frozen, 49500, EXIT_NEXT)
{
	{ uint32_t _v = 0xC4u; st16((ctx->a[6] + (uint32_t)(int32_t)4), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 011A90 movew #196,%fp@(4) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 011A96 movew %fp@(36),%d1 */
	{ uint32_t _s = 0xE0u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 011A9A andiw #224,%d1 */
	{ sh_w(ctx, 1, 'r', 5); hle_insn(ctx, 16); } /* 011A9E lsrw #5,%d1 */
	{ uint32_t _v = ld8((0x011AF0u + (uint32_t)((int32_t)(int16_t)ctx->d[1]))); set_b(&ctx->d[1], _v); cc_logic8(ctx, _v); hle_insn(ctx, 14); } /* 011AA0 moveb %pc@(0x11af0,%d1:w),%d1 */
	{ hle_insn(ctx, 18); sub_011616(ctx); } /* 011AA4 jsr %pc@(0x11616) */
	{ hle_insn(ctx, 18); sub_011334(ctx); } /* 011AA8 jsr %pc@(0x11334) */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 011AAC subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_011AEC; /* 011AB0 bnes 0x11aec */
	{ uint32_t _v = 0x11AF8u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 011AB2 movel #72440,%fp@(32) */
	{ uint32_t _v = 0x5Au; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 011ABA movew #90,%fp@(36) */
	{ uint32_t _v = 0x20DB2u; ctx->a[0] = _v; hle_insn(ctx, 12); } /* 011AC0 moveal #134578,%a0 */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_call68k(ctx, 0x00933E); } /* 011AC6 jsr 0x933e */
	{ uint32_t _v = 0x13u; set_w(&ctx->d[4], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011ACC movew #19,%d4 */
L_011AD0:;
	{ uint32_t _a = (ctx->a[6] + (uint32_t)(int32_t)2); ctx->d[0] = (uint32_t)(int32_t)(int16_t)ld16(_a + 0); ctx->d[1] = (uint32_t)(int32_t)(int16_t)ld16(_a + 2); hle_insn(ctx, 24); } /* 011AD0 movemw %fp@(2),%d0-%d1 */
	{ uint32_t _s = 0xAu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 011AD6 subiw #10,%d1 */
	{ ctx->a[2] = 0x01286Eu; hle_insn(ctx, 8); } /* 011ADA lea %pc@(0x1286e),%a2 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x01277C); } /* 011ADE bsrw 0x1277c */
	{ uint16_t _c = (uint16_t)ctx->d[4] - 1; set_w(&ctx->d[4], _c); if (_c != 0xFFFF) { hle_insn(ctx, 10); goto L_011AD0; } hle_insn(ctx, 14); } /* 011AE2 dbf %d4,0x11ad0 */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_call68k(ctx, 0x013E02); } /* 011AE6 jsr 0x13e02 */
L_011AEC:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 011AEC braw 0xb3be */
}

/* $011AF8 $27 T-1000 gone: free the slot */
HLE_RANGE(0x011AF8, L8T1000Gone, 160, EXIT_NEXT)
{
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 011AF8 subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_011B0C; /* 011AFC bnes 0x11b0c */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_call68k(ctx, 0x013D90); } /* 011AFE jsr 0x13d90 */
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 011B04 clrw %fp@ */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DAu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 011B06 subqw #1,0xffff80da */
L_011B0C:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 011B0C braw 0xb3be */
}

/* $011BF4 $27 tanker: nitrogen meter, upload the gauge */
HLE_RANGE(0x011BF4, L8TankerMeter, 3700, EXIT_NEXT)
{
	{ ctx->a[0] = 0x000085FAu; hle_insn(ctx, 12); } /* 011BF4 lea 0x85fa,%a0 */
	{ cc_logic8(ctx, ld8(0xFFFF9EC3u)); hle_insn(ctx, 16); } /* 011BFA tstb 0xffff9ec3 */
	if (bs(ctx, c_z(ctx))) goto L_011C08; /* 011C00 beqs 0x11c08 */
	{ ctx->a[0] = 0x0000865Au; hle_insn(ctx, 12); } /* 011C02 lea 0x865a,%a0 */
L_011C08:;
	{ uint32_t _v = ld32(0xFFFF80BCu); ctx->a[5] = _v; hle_insn(ctx, 20); } /* 011C08 moveal 0xffff80bc,%a5 */
	{ uint32_t _v = 0x4u; st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 011C0E movew #4,%a5@+ */
	{ uint32_t _v = 0x60u; st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 011C12 movew #96,%a5@+ */
	{ uint32_t _v = ctx->a[0]; st32(ctx->a[5], _v); ctx->a[5] += 4; cc_logic32(ctx, _v); hle_insn(ctx, 12); } /* 011C16 movel %a0,%a5@+ */
	{ uint32_t _v = ctx->a[5]; st32(0xFFFF80BCu, _v); cc_logic32(ctx, _v); hle_insn(ctx, 20); } /* 011C18 movel %a5,0xffff80bc */
	{ st8(0xFFFF9EC3u, 0); hle_insn(ctx, 20); } /* 011C1E sf 0xffff9ec3 */
	{ hle_insn(ctx, 18); sub_011B4E(ctx); } /* 011C24 bsrw 0x11b4e */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 011C28 braw 0xb3be */
}

/* $011C2C $27 tanker meter off: clear the gauge row and free */
HLE_RANGE(0x011C2C, L8MeterOff, 300, EXIT_NEXT)
{
	{ uint32_t _v = 0xE310u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011C2C movew #-7408,%d1 */
	{ ctx->d[2] = 0x18u; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 011C30 moveq #24,%d2 */
	{ ctx->d[3] = 0x1u; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 011C32 moveq #1,%d3 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x012212); } /* 011C34 jsr %pc@(0x12212) */
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 011C38 clrw %fp@ */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DAu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 011C3A subqw #1,0xffff80da */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 011C40 braw 0xb3be */
}

/* $011C44 $27 tanker panel: animation, hit by the T-1000 drains the meter */
HLE_RANGE_PURE(0x011C44, L8TankerPanel, 1000, EXIT_NEXT)
{
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 011C44 subqw #1,%fp@(36) */
	if (bw(ctx, !c_z(ctx))) goto L_011C5A; /* 011C48 bnew 0x11c5a */
	{ uint32_t _v = 0x11CD8u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 011C4C movel #72920,%fp@(32) */
	{ uint32_t _v = 0x96u; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 011C54 movew #150,%fp@(36) */
L_011C5A:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 011C5A movew %fp@(36),%d1 */
	{ uint32_t _s = 0xCu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 011C5E andiw #12,%d1 */
	{ sh_w(ctx, 1, 'a', 2); hle_insn(ctx, 10); } /* 011C62 asrw #2,%d1 */
	{ uint32_t _v = (ctx->d[1] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011C64 movew %d1,%sp@- */
	{ uint32_t _s = 0x2A8u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 011C66 addiw #680,%d1 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 011C6A movew %fp@(4),%d3 */
	{ uint32_t _s = 0x60u; uint32_t _d = (ctx->d[3] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 011C6E cmpiw #96,%d3 */
	if (bs(ctx, !c_ge(ctx))) goto L_011C8E; /* 011C72 blts 0x11c8e */
	{ uint32_t _s = 0x4u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 4); } /* 011C74 addqw #4,%d1 */
	{ uint32_t _s = 0x70u; uint32_t _d = (ctx->d[3] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 011C76 cmpiw #112,%d3 */
	if (bs(ctx, !c_ge(ctx))) goto L_011C8E; /* 011C7A blts 0x11c8e */
	{ uint32_t _s = 0x4u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 4); } /* 011C7C addqw #4,%d1 */
	{ uint32_t _s = 0x80u; uint32_t _d = (ctx->d[3] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 011C7E cmpiw #128,%d3 */
	if (bs(ctx, !c_ge(ctx))) goto L_011C8E; /* 011C82 blts 0x11c8e */
	{ uint32_t _s = 0x4u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 4); } /* 011C84 addqw #4,%d1 */
	{ uint32_t _s = 0x90u; uint32_t _d = (ctx->d[3] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 011C86 cmpiw #144,%d3 */
	if (bs(ctx, !c_ge(ctx))) goto L_011C8E; /* 011C8A blts 0x11c8e */
	{ uint32_t _s = 0x4u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 4); } /* 011C8C addqw #4,%d1 */
L_011C8E:;
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 011C8E jsr %pc@(0x13228) */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011C92 movew %sp@+,%d1 */
	{ uint32_t _s = 0x2BCu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 011C94 addiw #700,%d1 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 011C98 movew %fp@(2),%d2 */
	{ uint32_t _s = 0x8u; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 011C9C addiw #8,%d2 */
	{ uint32_t _v = 0xBEu; set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011CA0 movew #190,%d3 */
	{ ctx->d[5] = 0xEu; cc_logic32(ctx, ctx->d[5]); hle_insn(ctx, 4); } /* 011CA4 moveq #14,%d5 */
	{ hle_insn(ctx, 18); QueueSprite(ctx); } /* 011CA6 jsr %pc@(0x13240) */
	{ uint32_t _v = ld32(0xFFFF9ED6u); ctx->a[1] = _v; hle_insn(ctx, 20); } /* 011CAA moveal 0xffff9ed6,%a1 */
	{ uint32_t _s = ld16((ctx->a[1] + (uint32_t)(int32_t)2)); uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 12); } /* 011CB0 subw %a1@(2),%d2 */
	if (bs(ctx, !c_n(ctx))) goto L_011CB8; /* 011CB4 bpls 0x11cb8 */
	{ uint32_t _r = alu(ctx, OP_SUB, 0, ctx->d[2], 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 4); } /* 011CB6 negw %d2 */
L_011CB8:;
	{ uint32_t _s = 0xAu; uint32_t _d = (ctx->d[2] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 011CB8 cmpiw #10,%d2 */
	if (bs(ctx, c_gt(ctx))) goto L_011CD4; /* 011CBC bgts 0x11cd4 */
	{ uint32_t _s = 0x3Cu; uint32_t _da = 0xFFFF9F60u; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 24); } /* 011CBE subiw #60,0xffff9f60 */
	if (bs(ctx, !c_c(ctx))) goto L_011CCE; /* 011CC6 bccs 0x11cce */
	{ st8(0xFFFF9EC2u, 0xFF); hle_insn(ctx, 20); } /* 011CC8 st 0xffff9ec2 */
L_011CCE:;
	{ st8(0xFFFF9EC3u, 0xFF); hle_insn(ctx, 20); } /* 011CCE st 0xffff9ec3 */
L_011CD4:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 011CD4 braw 0xb3be */
}

/* $011CD8 $27 tanker panel leak: spray sprite, then free */
HLE_RANGE_PURE(0x011CD8, L8PanelLeak, 1000, EXIT_NEXT)
{
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 011CD8 subqw #1,%fp@(36) */
	if (bw(ctx, !c_z(ctx))) goto L_011CF8; /* 011CDC bnew 0x11cf8 */
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 011CE0 clrw %fp@ */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DAu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 011CE2 subqw #1,0xffff80da */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)38)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 011CE8 movew %fp@(38),%d0 */
	{ ctx->a[1] = 0xFFFFAE84u; hle_insn(ctx, 12); } /* 011CEC lea 0xffffae84,%a1 */
	{ uint32_t _v = 0xFu; st16((ctx->a[1] + (uint32_t)(int32_t)0 + (uint32_t)((int32_t)(int16_t)ctx->d[0])), _v); cc_logic16(ctx, _v); hle_insn(ctx, 18); } /* 011CF2 movew #15,%a1@(0,%d0:w) */
L_011CF8:;
	{ uint32_t _v = 0x2C0u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 011CF8 movew #704,%d1 */
	{ uint32_t _a = (ctx->a[6] + (uint32_t)(int32_t)2); ctx->d[2] = (uint32_t)(int32_t)(int16_t)ld16(_a + 0); ctx->d[3] = (uint32_t)(int32_t)(int16_t)ld16(_a + 2); hle_insn(ctx, 24); } /* 011CFC movemw %fp@(2),%d2-%d3 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)6)); set_w(&ctx->d[5], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 011D02 movew %fp@(6),%d5 */
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[5] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[5], _r); hle_insn(ctx, 4); } /* 011D06 addqw #1,%d5 */
	{ hle_insn(ctx, 20); QueueSprite(ctx); } /* 011D08 jsr 0x13240 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 011D0E braw 0xb3be */
}

/* $006C2E hit test of T-1000 in the steel mill (one box) */
HLE_FUNC_PURE(0x006C2E, HitT1000Mill, 300)
{
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); hle_insn(ctx, 12); } /* 006C2E btst #15,%fp@ */
	if (bs(ctx, !c_z(ctx))) goto L_006C36; /* 006C32 bnes 0x6c36 */
	hle_insn(ctx, 16); return; /* 006C34 rts */
L_006C36:;
	{ ctx->a[3] = 0x006C86u; hle_insn(ctx, 8); } /* 006C36 lea %pc@(0x6c86),%a3 */
	{ ctx->a[2] = 0x006C42u; hle_insn(ctx, 8); } /* 006C3A lea %pc@(0x6c42),%a2 */
	hle_insn(ctx, 10); HitBox(ctx); return; /* 006C3E jmp %pc@(0x5940) */
}

/* $006EF8 hit test of tanker panel (one box) */
HLE_FUNC_PURE(0x006EF8, HitL8Panel, 300)
{
	{ ctx->a[3] = 0x006F5Cu; hle_insn(ctx, 8); } /* 006EF8 lea %pc@(0x6f5c),%a3 */
	{ ctx->a[2] = 0x006F04u; hle_insn(ctx, 8); } /* 006EFC lea %pc@(0x6f04),%a2 */
	hle_insn(ctx, 10); HitBox(ctx); return; /* 006F00 jmp %pc@(0x5940) */
}

