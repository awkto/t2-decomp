/* Miscellaneous object AI states and hit tests that the per-level files skipped: John Connor's truck
 * (level 3, type $1B: $00D112/$00D27A bouncing truck, $00D2AC scroller, $00D56E damage-cell panel, the debris
 * pieces $00D4E6, the wreck $00D0D4, hit test HitTruck $00627A), the rare level-1 objects (ground trooper
 * $00D978 -> $00D9B2 -> $00DA18, type $00 $00DA48, type $0D $00B86A/$00B896/$00B8C2/$00B92E/$00B932 with hit
 * test $005E1C, type $11 hit test $005ABC -> $00B860), the endoskeleton death variant $00B69A and walk/shoot
 * variants $00B796/$00B7E4 (left out of ai_endo.c) and the crate rise states $00C802/$00C826/$00C846 (left out
 * of ai_simple.c). docs/objects_types.md, src/t2.s.
 *
 * Same scheme as ai_l7.c / ai_l8.c: every AI state is a range from its entry (reached by the `jmp (a0)` of
 * ObjRunAll) to `bra.w ObjNext` ($00B3BE, reached with hle_jump). C callees (ObjScrollX, ObjMoveX,
 * ObjQueueSprite, QueueSprite, Random, ObjAlloc, ObjKillIfLeftOf/RightOf, EndoDown, HitBox) are called
 * directly after their jsr is charged; the helper subroutines that only touch work RAM (sub_00D236,
 * sub_00D0F8, sub_00BABC, sub_00B772, sub_00B846, sub_0098FC) and sub_00D1D2 are static C functions; every
 * other callee (sounds, damage, VDP queues, TruckDamage, debris spawners) runs as 68k through hle_call68k, so a
 * state that reaches one is a plain HLE_RANGE and the others HLE_RANGE_PURE. Every instruction is on the
 * instruction timeline (GPGX costs); the bodies were produced instruction by instruction from the disassembly
 * (a comment on every line names the 68k instruction). The on-hit callbacks the hit tests hand to ShotHitTest
 * stay 68k. */
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
void Copy32(hle_ctx *ctx);
void PalFadeStep(hle_ctx *ctx);
void EndoDown(hle_ctx *ctx);

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


/* divu.w #imm,Dn (src != 0): GPGX UseDivuCycles = 76 cycles + 4 per zero-shift step - 2 per subtract, plus the
 * table's 4 for the immediate; the bus-refresh re-check (SKIP_BUS_REFRESH) happens after the dynamic part. An
 * overflowing quotient costs 10 and sets N and V (C clear), leaving Dn and Z alone. */
static void div_w(hle_ctx *c, int r, uint16_t src)
{
	uint32_t dst = c->d[r];
	uint32_t q = dst / src, rem = dst % src;
	if (q < 0x10000) {
		uint32_t d = dst, s = (uint32_t)src << 16;
		int cyc = 76;
		for (int i = 0; i < 15; i++) {
			if ((int32_t)d < 0) {
				d <<= 1;
				d -= s;
			} else {
				d <<= 1;
				cyc += 4;
				if (d >= s) {
					d -= s;
					cyc -= 2;
				}
			}
		}
		c->d[r] = (q & 0xFFFFu) | (rem << 16);
		cc_set(c, CC_N | CC_Z | CC_V | CC_C, ((q & 0x8000) ? CC_N : 0) | (q ? 0 : CC_Z));
		hle_insn_x(c, 4 + cyc, cyc);
	} else {
		cc_set(c, CC_N | CC_V | CC_C, CC_N | CC_V);
		hle_insn(c, 4 + 10);
	}
}
static void sub_00D236(hle_ctx *ctx);
static void sub_00D1D2(hle_ctx *ctx);
static void sub_00D0F8(hle_ctx *ctx);
static void sub_00BABC(hle_ctx *ctx);
static void sub_00B772(hle_ctx *ctx);
static void sub_00B846(hle_ctx *ctx);
static void sub_0098FC(hle_ctx *ctx);

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

/* sub_00D1D2 (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_00D1D2(hle_ctx *ctx)
{
	{ hle_insn(ctx, 18); sub_00D236(ctx); } /* 00D1D2 jsr %pc@(0xd236) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00D1D6 movew %fp@(4),%d0 */
	{ uint32_t _v = 0xB0u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00D1DA movew #176,%d0 */
	{ uint32_t _s = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 12); } /* 00D1DE subw %fp@(4),%d0 */
	{ ctx->d[1] = 0x0u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 00D1E2 moveq #0,%d1 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x011F58); } /* 00D1E4 jsr %pc@(0x11f58) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00D1E8 movew %fp@(36),%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 00D1EC movew %d0,%d1 */
	{ uint32_t _s = 0x180u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 00D1EE andiw #384,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_00D214; /* 00D1F2 bnes 0xd214 */
	{ cc_set(ctx, CC_Z, ((ctx->d[1] >> 2) & 1) ? 0 : CC_Z); hle_insn(ctx, 10); } /* 00D1F4 btst #2,%d1 */
	if (bs(ctx, c_z(ctx))) goto L_00D214; /* 00D1F8 beqs 0xd214 */
	{ uint32_t _v = 0xE1u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00D1FA movew #225,%d1 */
	{ uint32_t _a = (ctx->a[6] + (uint32_t)(int32_t)2); ctx->d[2] = (uint32_t)(int32_t)(int16_t)ld16(_a + 0); ctx->d[3] = (uint32_t)(int32_t)(int16_t)ld16(_a + 2); hle_insn(ctx, 24); } /* 00D1FE movemw %fp@(2),%d2-%d3 */
	{ uint32_t _s = 0x68u; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 00D204 addiw #104,%d2 */
	{ uint32_t _s = 0x37u; uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[3], _r); hle_insn(ctx, 8); } /* 00D208 subiw #55,%d3 */
	{ uint32_t _v = 0x63u; set_w(&ctx->d[5], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00D20C movew #99,%d5 */
	{ hle_insn(ctx, 18); QueueSprite(ctx); } /* 00D210 jsr %pc@(0x13240) */
L_00D214:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00D214 movew %fp@(4),%d3 */
	{ uint32_t _s = 0x4u; uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[3], _r); hle_insn(ctx, 8); } /* 00D218 subiw #4,%d3 */
	{ uint32_t _s = 0xA6u; uint32_t _d = (ctx->d[3] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 00D21C cmpiw #166,%d3 */
	if (bs(ctx, !c_ge(ctx))) goto L_00D230; /* 00D220 blts 0xd230 */
	{ hle_insn(ctx, 18); Random(ctx); } /* 00D222 jsr %pc@(0x881c) */
	{ uint32_t _s = 0x7u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 00D226 andiw #7,%d0 */
	{ uint32_t _v = 0xABu; set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00D22A movew #171,%d3 */
	{ uint32_t _s = (ctx->d[0] & 0xFFFFu); uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[3], _r); hle_insn(ctx, 4); } /* 00D22E subw %d0,%d3 */
L_00D230:;
	hle_insn(ctx, 16); return; /* 00D230 rts */
}

/* sub_00D0F8 (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_00D0F8(hle_ctx *ctx)
{
	{ uint32_t _v = ld32(0xFFFF80BCu); ctx->a[5] = _v; hle_insn(ctx, 20); } /* 00D0F8 moveal 0xffff80bc,%a5 */
	{ uint32_t _v = 0x2u; st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00D0FE movew #2,%a5@+ */
	{ uint32_t _v = 0xF80u; st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00D102 movew #3968,%a5@+ */
	{ uint32_t _v = 0xE000u; st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00D106 movew #-8192,%a5@+ */
	{ uint32_t _v = ctx->a[5]; st32(0xFFFF80BCu, _v); cc_logic32(ctx, _v); hle_insn(ctx, 20); } /* 00D10A movel %a5,0xffff80bc */
	hle_insn(ctx, 16); return; /* 00D110 rts */
}

/* sub_00BABC (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_00BABC(hle_ctx *ctx)
{
	{ uint32_t _s = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 12); } /* 00BABC addw %fp@(36),%d1 */
	{ div_w(ctx, 1, 0x18);  } /* 00BAC0 divuw #24,%d1 */
	{ ctx->d[1] = (ctx->d[1] >> 16) | (ctx->d[1] << 16); cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 00BAC4 swap %d1 */
	{ sh_w(ctx, 1, 'a', 2); hle_insn(ctx, 10); } /* 00BAC6 asrw #2,%d1 */
	{ uint32_t _a = (ctx->a[6] + (uint32_t)(int32_t)2); ctx->d[2] = (uint32_t)(int32_t)(int16_t)ld16(_a + 0); ctx->d[3] = (uint32_t)(int32_t)(int16_t)ld16(_a + 2); hle_insn(ctx, 24); } /* 00BAC8 movemw %fp@(2),%d2-%d3 */
	{ ctx->d[5] = 0x0u; cc_logic32(ctx, ctx->d[5]); hle_insn(ctx, 4); } /* 00BACE moveq #0,%d5 */
	{ uint32_t _v = ld8((ctx->a[6] + (uint32_t)(int32_t)7)); set_b(&ctx->d[5], _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 00BAD0 moveb %fp@(7),%d5 */
	hle_insn(ctx, 16); return; /* 00BAD4 rts */
}

/* sub_00B772 (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_00B772(hle_ctx *ctx)
{
	{ uint32_t _v = 0xB6D0u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00B772 movel #46800,%fp@(32) */
	{ hle_insn(ctx, 18); Random(ctx); } /* 00B77A jsr %pc@(0x881c) */
	{ uint32_t _s = 0x3Fu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 00B77E andiw #63,%d0 */
	{ uint32_t _s = 0x96u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 00B782 addiw #150,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00B786 movew %d0,%fp@(36) */
	hle_insn(ctx, 16); return; /* 00B78A rts */
}

/* sub_00B846 (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_00B846(hle_ctx *ctx)
{
	{ uint32_t _v = 0xB796u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00B846 movel #46998,%fp@(32) */
	{ hle_insn(ctx, 18); Random(ctx); } /* 00B84E jsr %pc@(0x881c) */
	{ uint32_t _s = 0x3Fu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 00B852 andiw #63,%d0 */
	{ uint32_t _s = 0x80u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 00B856 addiw #128,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00B85A movew %d0,%fp@(36) */
	hle_insn(ctx, 16); return; /* 00B85E rts */
}

/* sub_0098FC (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_0098FC(hle_ctx *ctx)
{
	{ ctx->a[4] = 0xFFFFAB9Eu; hle_insn(ctx, 12); } /* 0098FC lea 0xffffab9e,%a4 */
	{ ctx->d[0] = 0x0u; cc_logic32(ctx, ctx->d[0]); hle_insn(ctx, 4); } /* 009902 moveq #0,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16(ctx->a[4], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 009904 movew %d0,%a4@ */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[4] + (uint32_t)(int32_t)32), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 009906 movew %d0,%a4@(32) */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[4] + (uint32_t)(int32_t)64), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00990A movew %d0,%a4@(64) */
	hle_insn(ctx, 16); return; /* 00990E rts */
}

/* $00D112 John Connor truck (level 3): damage check, tail/front sprites, bounce, then the shared sprite part */
/* bound 30200 = 1.5 x the T2_HLE_BOUNDS max observed 20088 (the destroy path with TruckDamage; dt max 17136) */
HLE_RANGE(0x00D112, TruckMain, 30200, EXIT_NEXT)
{
	{ uint32_t _v = ld16(0xFFFF9E92u); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00D112 movew 0xffff9e92,%d0 */
	{ uint32_t _s = ld16(0xFFFF9F6Au); uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 00D118 cmpw 0xffff9f6a,%d0 */
	if (bs(ctx, !c_ge(ctx))) goto L_00D136; /* 00D11E blts 0xd136 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x00D322); } /* 00D120 jsr %pc@(0xd322) */
	{ uint32_t _v = ld16(0xFFFF9E92u); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00D124 movew 0xffff9e92,%d0 */
	{ uint32_t _s = ld16(0xFFFF9F6Au); uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 16); } /* 00D12A subw 0xffff9f6a,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16(0xFFFF9E92u, _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00D130 movew %d0,0xffff9e92 */
L_00D136:;
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00D136 subqw #1,%fp@(36) */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 6) & 1) ? 0 : CC_Z); hle_insn(ctx, 12); } /* 00D13A btst #6,%fp@ */
	if (bs(ctx, c_z(ctx))) goto L_00D168; /* 00D13E beqs 0xd168 */
	{ ctx->d[1] = 0x0u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 00D140 moveq #0,%d1 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00D142 movew %fp@(36),%d1 */
	{ div_w(ctx, 1, 0x18);  } /* 00D146 divuw #24,%d1 */
	{ ctx->d[1] = (ctx->d[1] >> 16) | (ctx->d[1] << 16); cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 00D14A swap %d1 */
	{ sh_w(ctx, 1, 'a', 2); hle_insn(ctx, 10); } /* 00D14C asrw #2,%d1 */
	{ uint32_t _s = 0xE2u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00D14E addiw #226,%d1 */
	{ uint32_t _a = (ctx->a[6] + (uint32_t)(int32_t)2); ctx->d[2] = (uint32_t)(int32_t)(int16_t)ld16(_a + 0); ctx->d[3] = (uint32_t)(int32_t)(int16_t)ld16(_a + 2); hle_insn(ctx, 24); } /* 00D152 movemw %fp@(2),%d2-%d3 */
	{ uint32_t _s = 0x40u; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 00D158 addiw #64,%d2 */
	{ uint32_t _s = 0x20u; uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[3], _r); hle_insn(ctx, 8); } /* 00D15C subiw #32,%d3 */
	{ uint32_t _v = 0x63u; set_w(&ctx->d[5], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00D160 movew #99,%d5 */
	{ hle_insn(ctx, 18); QueueSprite(ctx); } /* 00D164 jsr %pc@(0x13240) */
L_00D168:;
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 3) & 1) ? 0 : CC_Z); hle_insn(ctx, 12); } /* 00D168 btst #3,%fp@ */
	if (bs(ctx, c_z(ctx))) goto L_00D196; /* 00D16C beqs 0xd196 */
	{ ctx->d[1] = 0x3u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 00D16E moveq #3,%d1 */
	{ uint32_t _s = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 12); } /* 00D170 addw %fp@(36),%d1 */
	{ div_w(ctx, 1, 0x18);  } /* 00D174 divuw #24,%d1 */
	{ ctx->d[1] = (ctx->d[1] >> 16) | (ctx->d[1] << 16); cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 00D178 swap %d1 */
	{ sh_w(ctx, 1, 'a', 2); hle_insn(ctx, 10); } /* 00D17A asrw #2,%d1 */
	{ uint32_t _s = 0xE2u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00D17C addiw #226,%d1 */
	{ uint32_t _a = (ctx->a[6] + (uint32_t)(int32_t)2); ctx->d[2] = (uint32_t)(int32_t)(int16_t)ld16(_a + 0); ctx->d[3] = (uint32_t)(int32_t)(int16_t)ld16(_a + 2); hle_insn(ctx, 24); } /* 00D180 movemw %fp@(2),%d2-%d3 */
	{ uint32_t _s = 0x58u; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 00D186 subiw #88,%d2 */
	{ uint32_t _s = 0x20u; uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[3], _r); hle_insn(ctx, 8); } /* 00D18A subiw #32,%d3 */
	{ uint32_t _v = 0x63u; set_w(&ctx->d[5], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00D18E movew #99,%d5 */
	{ hle_insn(ctx, 18); QueueSprite(ctx); } /* 00D192 jsr %pc@(0x13240) */
L_00D196:;
	{ uint32_t _s = 0xB0u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)4); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 00D196 cmpiw #176,%fp@(4) */
	if (bs(ctx, !c_ge(ctx))) goto L_00D1CA; /* 00D19C blts 0xd1ca */
	{ st8((ctx->a[6] + (uint32_t)(int32_t)13), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 00D19E clrb %fp@(13) */
	{ uint32_t _v = 0x2u; st8((ctx->a[6] + (uint32_t)(int32_t)18), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 00D1A2 moveb #2,%fp@(18) */
	{ hle_insn(ctx, 18); Random(ctx); } /* 00D1A8 jsr %pc@(0x881c) */
	{ uint32_t _s = 0x3Fu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 00D1AC andiw #63,%d0 */
	{ uint32_t _s = 0x37u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 00D1B0 cmpiw #55,%d0 */
	if (bs(ctx, !c_ge(ctx))) goto L_00D1BC; /* 00D1B4 blts 0xd1bc */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 00D1B6 movew %d0,%d1 */
	{ sh_w(ctx, 1, 'a', 1); hle_insn(ctx, 8); } /* 00D1B8 asrw #1,%d1 */
	{ uint32_t _s = (ctx->d[1] & 0xFFFFu); uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 00D1BA addw %d1,%d0 */
L_00D1BC:;
	{ uint32_t _v = (ctx->d[0] & 0xFFu); st8((ctx->a[6] + (uint32_t)(int32_t)12), _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 00D1BC moveb %d0,%fp@(12) */
	{ uint32_t _v = 0xFFu; st8((ctx->a[6] + (uint32_t)(int32_t)15), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 00D1C0 moveb #-1,%fp@(15) */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)4); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00D1C6 subqw #1,%fp@(4) */
L_00D1CA:;
	{ hle_insn(ctx, 18); sub_00D1D2(ctx); } /* 00D1CA jsr %pc@(0xd1d2) */
	hle_insn(ctx, 10); goto L_00D2B8; /* 00D1CE braw 0xd2b8 */
L_00D2B8:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00D2B8 movew %fp@(36),%d1 */
	{ uint32_t _s = 0x6u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00D2BC andiw #6,%d1 */
	{ sh_w(ctx, 1, 'a', 1); hle_insn(ctx, 8); } /* 00D2C0 asrw #1,%d1 */
	{ uint32_t _s = 0x3u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d ^ _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00D2C2 eoriw #3,%d1 */
	{ uint32_t _s = 0xDDu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00D2C6 addiw #221,%d1 */
	{ uint32_t _v = (ctx->d[1] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00D2CA movew %d1,%sp@- */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00D2CC movew %fp@(2),%d2 */
	if (bs(ctx, c_n(ctx))) goto L_00D2E0; /* 00D2D0 bmis 0xd2e0 */
	{ ctx->d[5] = 0x63u; cc_logic32(ctx, ctx->d[5]); hle_insn(ctx, 4); } /* 00D2D2 moveq #99,%d5 */
	{ uint32_t _s = 0x3Cu; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 00D2D4 subiw #60,%d2 */
	{ uint32_t _v = (ctx->d[3] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00D2D8 movew %d3,%sp@- */
	{ hle_insn(ctx, 18); QueueSprite(ctx); } /* 00D2DA jsr %pc@(0x13240) */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00D2DE movew %sp@+,%d3 */
L_00D2E0:;
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00D2E0 movew %sp@+,%d1 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00D2E2 movew %fp@(2),%d2 */
	{ uint32_t _s = 0x12Cu; uint32_t _d = (ctx->d[2] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 00D2E6 cmpiw #300,%d2 */
	if (bw(ctx, c_gt(ctx))) goto L_00D2F8; /* 00D2EA bgtw 0xd2f8 */
	{ ctx->d[5] = 0x63u; cc_logic32(ctx, ctx->d[5]); hle_insn(ctx, 4); } /* 00D2EE moveq #99,%d5 */
	{ uint32_t _s = 0x5Cu; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 00D2F0 addiw #92,%d2 */
	{ hle_insn(ctx, 18); QueueSprite(ctx); } /* 00D2F4 jsr %pc@(0x13240) */
L_00D2F8:;
	{ cc_logic16(ctx, ld16(0xFFFF9EA6u)); hle_insn(ctx, 16); } /* 00D2F8 tstw 0xffff9ea6 */
	if (bs(ctx, c_z(ctx))) goto L_00D306; /* 00D2FE beqs 0xd306 */
	{ ctx->d[1] = 0x5u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 00D300 moveq #5,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00D302 jsr %pc@(0x13228) */
L_00D306:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00D306 braw 0xb3be */
}

/* $00D27A truck: settle (no bounce) then the shared sprite part */
HLE_RANGE(0x00D27A, TruckSettle, 4000, EXIT_NEXT)
{
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00D27A subqw #1,%fp@(36) */
	{ uint32_t _s = 0xB0u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)4); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 00D27E cmpiw #176,%fp@(4) */
	if (bs(ctx, !c_ge(ctx))) goto L_00D29C; /* 00D284 blts 0xd29c */
	{ st8((ctx->a[6] + (uint32_t)(int32_t)13), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 00D286 clrb %fp@(13) */
	{ uint32_t _v = 0x0u; st8((ctx->a[6] + (uint32_t)(int32_t)18), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 00D28A moveb #0,%fp@(18) */
	{ uint32_t _v = 0x0u; st8((ctx->a[6] + (uint32_t)(int32_t)12), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 00D290 moveb #0,%fp@(12) */
	{ uint32_t _v = 0x0u; st8((ctx->a[6] + (uint32_t)(int32_t)15), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 00D296 moveb #0,%fp@(15) */
L_00D29C:;
	{ hle_insn(ctx, 18); sub_00D1D2(ctx); } /* 00D29C jsr %pc@(0xd1d2) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00D2A0 movew %fp@(4),%d3 */
	{ uint32_t _s = 0xAu; uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[3], _r); hle_insn(ctx, 8); } /* 00D2A4 subiw #10,%d3 */
	hle_insn(ctx, 10); goto L_00D2B8; /* 00D2A8 braw 0xd2b8 */
L_00D2B8:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00D2B8 movew %fp@(36),%d1 */
	{ uint32_t _s = 0x6u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00D2BC andiw #6,%d1 */
	{ sh_w(ctx, 1, 'a', 1); hle_insn(ctx, 8); } /* 00D2C0 asrw #1,%d1 */
	{ uint32_t _s = 0x3u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d ^ _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00D2C2 eoriw #3,%d1 */
	{ uint32_t _s = 0xDDu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00D2C6 addiw #221,%d1 */
	{ uint32_t _v = (ctx->d[1] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00D2CA movew %d1,%sp@- */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00D2CC movew %fp@(2),%d2 */
	if (bs(ctx, c_n(ctx))) goto L_00D2E0; /* 00D2D0 bmis 0xd2e0 */
	{ ctx->d[5] = 0x63u; cc_logic32(ctx, ctx->d[5]); hle_insn(ctx, 4); } /* 00D2D2 moveq #99,%d5 */
	{ uint32_t _s = 0x3Cu; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 00D2D4 subiw #60,%d2 */
	{ uint32_t _v = (ctx->d[3] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00D2D8 movew %d3,%sp@- */
	{ hle_insn(ctx, 18); QueueSprite(ctx); } /* 00D2DA jsr %pc@(0x13240) */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00D2DE movew %sp@+,%d3 */
L_00D2E0:;
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00D2E0 movew %sp@+,%d1 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00D2E2 movew %fp@(2),%d2 */
	{ uint32_t _s = 0x12Cu; uint32_t _d = (ctx->d[2] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 00D2E6 cmpiw #300,%d2 */
	if (bw(ctx, c_gt(ctx))) goto L_00D2F8; /* 00D2EA bgtw 0xd2f8 */
	{ ctx->d[5] = 0x63u; cc_logic32(ctx, ctx->d[5]); hle_insn(ctx, 4); } /* 00D2EE moveq #99,%d5 */
	{ uint32_t _s = 0x5Cu; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 00D2F0 addiw #92,%d2 */
	{ hle_insn(ctx, 18); QueueSprite(ctx); } /* 00D2F4 jsr %pc@(0x13240) */
L_00D2F8:;
	{ cc_logic16(ctx, ld16(0xFFFF9EA6u)); hle_insn(ctx, 16); } /* 00D2F8 tstw 0xffff9ea6 */
	if (bs(ctx, c_z(ctx))) goto L_00D306; /* 00D2FE beqs 0xd306 */
	{ ctx->d[1] = 0x5u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 00D300 moveq #5,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00D302 jsr %pc@(0x13228) */
L_00D306:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00D306 braw 0xb3be */
}

/* $00D2AC truck: scroll and wheel/body sprites */
HLE_RANGE_PURE(0x00D2AC, TruckSprites, 1000, EXIT_NEXT)
{
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 00D2AC jsr %pc@(0x12316) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00D2B0 movew %fp@(4),%d3 */
	{ uint32_t _s = 0xAu; uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[3], _r); hle_insn(ctx, 8); } /* 00D2B4 subiw #10,%d3 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00D2B8 movew %fp@(36),%d1 */
	{ uint32_t _s = 0x6u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00D2BC andiw #6,%d1 */
	{ sh_w(ctx, 1, 'a', 1); hle_insn(ctx, 8); } /* 00D2C0 asrw #1,%d1 */
	{ uint32_t _s = 0x3u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d ^ _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00D2C2 eoriw #3,%d1 */
	{ uint32_t _s = 0xDDu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00D2C6 addiw #221,%d1 */
	{ uint32_t _v = (ctx->d[1] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00D2CA movew %d1,%sp@- */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00D2CC movew %fp@(2),%d2 */
	if (bs(ctx, c_n(ctx))) goto L_00D2E0; /* 00D2D0 bmis 0xd2e0 */
	{ ctx->d[5] = 0x63u; cc_logic32(ctx, ctx->d[5]); hle_insn(ctx, 4); } /* 00D2D2 moveq #99,%d5 */
	{ uint32_t _s = 0x3Cu; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 00D2D4 subiw #60,%d2 */
	{ uint32_t _v = (ctx->d[3] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00D2D8 movew %d3,%sp@- */
	{ hle_insn(ctx, 18); QueueSprite(ctx); } /* 00D2DA jsr %pc@(0x13240) */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00D2DE movew %sp@+,%d3 */
L_00D2E0:;
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00D2E0 movew %sp@+,%d1 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00D2E2 movew %fp@(2),%d2 */
	{ uint32_t _s = 0x12Cu; uint32_t _d = (ctx->d[2] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 00D2E6 cmpiw #300,%d2 */
	if (bw(ctx, c_gt(ctx))) goto L_00D2F8; /* 00D2EA bgtw 0xd2f8 */
	{ ctx->d[5] = 0x63u; cc_logic32(ctx, ctx->d[5]); hle_insn(ctx, 4); } /* 00D2EE moveq #99,%d5 */
	{ uint32_t _s = 0x5Cu; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 00D2F0 addiw #92,%d2 */
	{ hle_insn(ctx, 18); QueueSprite(ctx); } /* 00D2F4 jsr %pc@(0x13240) */
L_00D2F8:;
	{ cc_logic16(ctx, ld16(0xFFFF9EA6u)); hle_insn(ctx, 16); } /* 00D2F8 tstw 0xffff9ea6 */
	if (bs(ctx, c_z(ctx))) goto L_00D306; /* 00D2FE beqs 0xd306 */
	{ ctx->d[1] = 0x5u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 00D300 moveq #5,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00D302 jsr %pc@(0x13228) */
L_00D306:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00D306 braw 0xb3be */
}

/* $00D56E truck: damage-cell panel (tile rect of the damaged body) */
/* bound 6500 = 1.5 x observed max 4273 */
HLE_RANGE(0x00D56E, TruckPanel, 6500, EXIT_NEXT)
{
	{ uint32_t _v = (ctx->d[7] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00D56E movew %d7,%sp@- */
	{ uint32_t _v = ld32(0xFFFF9ECAu); ctx->a[5] = _v; hle_insn(ctx, 20); } /* 00D570 moveal 0xffff9eca,%a5 */
	{ uint32_t _v = ld16(ctx->a[5]); set_w(&ctx->d[5], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00D576 movew %a5@,%d5 */
	{ uint32_t _s = 0x1Bu; uint32_t _d = (ctx->d[5] & 0xFFu); alu(ctx, OP_SUB, _d, _s, 8, 0); hle_insn(ctx, 8); } /* 00D578 cmpib #27,%d5 */
	if (bs(ctx, !c_z(ctx))) goto L_00D584; /* 00D57C bnes 0xd584 */
	{ cc_set(ctx, CC_Z, ((ctx->d[5] >> 12) & 1) ? 0 : CC_Z); hle_insn(ctx, 10); } /* 00D57E btst #12,%d5 */
	if (bs(ctx, c_z(ctx))) goto L_00D5B0; /* 00D582 beqs 0xd5b0 */
L_00D584:;
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_callc(ctx, 0x013DA8); } /* 00D584 jsr 0x13da8 */
	{ uint32_t _v = 0xE382u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00D58A movew #-7294,%d1 */
	{ ctx->d[2] = 0x9u; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 00D58E moveq #9,%d2 */
	{ ctx->d[3] = 0x8u; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 00D590 moveq #8,%d3 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x012212); } /* 00D592 jsr %pc@(0x12212) */
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 00D596 clrw %fp@ */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DAu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 00D598 subqw #1,0xffff80da */
	{ uint32_t _s = 0x1Bu; uint32_t _d = (ctx->d[5] & 0xFFu); alu(ctx, OP_SUB, _d, _s, 8, 0); hle_insn(ctx, 8); } /* 00D59E cmpib #27,%d5 */
	if (bw(ctx, !c_z(ctx))) goto L_00D636; /* 00D5A2 bnew 0xd636 */
	{ ctx->a[1] = 0x0000D66Cu; hle_insn(ctx, 12); } /* 00D5A6 lea 0xd66c,%a1 */
	hle_insn(ctx, 10); goto L_00D60A; /* 00D5AC braw 0xd60a */
L_00D5B0:;
	{ uint32_t _v = 0xE382u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00D5B0 movew #-7294,%d1 */
	{ ctx->d[2] = 0x9u; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 00D5B4 moveq #9,%d2 */
	{ ctx->d[3] = 0x8u; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 00D5B6 moveq #8,%d3 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x012212); } /* 00D5B8 jsr %pc@(0x12212) */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00D5BC subqw #1,%fp@(36) */
	{ uint32_t _v = ld32(0xFFFF9ECEu); ctx->a[1] = _v; hle_insn(ctx, 20); } /* 00D5C0 moveal 0xffff9ece,%a1 */
	{ uint32_t _v = ld8((ctx->a[1] + (uint32_t)(int32_t)1)); set_b(&ctx->d[0], _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 00D5C6 moveb %a1@(1),%d0 */
	{ ctx->d[1] = 0x3u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 00D5CA moveq #3,%d1 */
	{ uint32_t _s = 0x7u; uint32_t _d = (ctx->d[0] & 0xFFu); alu(ctx, OP_SUB, _d, _s, 8, 0); hle_insn(ctx, 8); } /* 00D5CC cmpib #7,%d0 */
	if (bw(ctx, c_z(ctx))) goto L_00D5DA; /* 00D5D0 beqw 0xd5da */
	{ uint32_t _s = 0x8u; uint32_t _d = (ctx->d[0] & 0xFFu); alu(ctx, OP_SUB, _d, _s, 8, 0); hle_insn(ctx, 8); } /* 00D5D4 cmpib #8,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_00D600; /* 00D5D8 bnes 0xd600 */
L_00D5DA:;
	{ uint32_t _v = ld16((ctx->a[1] + (uint32_t)(int32_t)2)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00D5DA movew %a1@(2),%d0 */
	{ uint32_t _s = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 12); } /* 00D5DE subw %fp@(2),%d0 */
	{ ctx->d[1] = 0x4u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 00D5E2 moveq #4,%d1 */
	{ uint32_t _s = 0x3Cu; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 00D5E4 cmpiw #60,%d0 */
	if (bs(ctx, c_gt(ctx))) goto L_00D5F4; /* 00D5E8 bgts 0xd5f4 */
	{ ctx->d[1] = 0x0u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 00D5EA moveq #0,%d1 */
	{ uint32_t _s = 0xFFECu; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 00D5EC cmpiw #-20,%d0 */
	if (bs(ctx, !c_ge(ctx))) goto L_00D5F4; /* 00D5F0 blts 0xd5f4 */
	{ ctx->d[1] = 0x2u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 00D5F2 moveq #2,%d1 */
L_00D5F4:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00D5F4 movew %fp@(36),%d0 */
	{ cc_set(ctx, CC_Z, ((ctx->d[0] >> 2) & 1) ? 0 : CC_Z); hle_insn(ctx, 10); } /* 00D5F8 btst #2,%d0 */
	if (bs(ctx, c_z(ctx))) goto L_00D600; /* 00D5FC beqs 0xd600 */
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 4); } /* 00D5FE addqw #1,%d1 */
L_00D600:;
	{ sh_w(ctx, 1, 'l', 3); hle_insn(ctx, 12); } /* 00D600 aslw #3,%d1 */
	{ ctx->a[1] = 0x00D63Cu; hle_insn(ctx, 8); } /* 00D602 lea %pc@(0xd63c),%a1 */
	{ ctx->a[1] = (ctx->a[1] + (uint32_t)(int32_t)0 + (uint32_t)((int32_t)(int16_t)ctx->d[1])); hle_insn(ctx, 12); } /* 00D606 lea %a1@(0,%d1:w),%a1 */
L_00D60A:;
	{ uint32_t _p = ld16(ctx->a[1]); ctx->a[1] += 2; uint32_t _v = _p; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00D60A movew %a1@+,%d0 */
	{ ctx->a[0] = 0x000539D0u; hle_insn(ctx, 12); } /* 00D60C lea 0x539d0,%a0 */
	{ uint32_t _p = ld16(ctx->a[1]); ctx->a[1] += 2; uint32_t _v = _p; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00D612 movew %a1@+,%d1 */
	{ ctx->a[0] = (ctx->a[0] + (uint32_t)(int32_t)0 + (uint32_t)((int32_t)(int16_t)ctx->d[0])); hle_insn(ctx, 12); } /* 00D614 lea %a0@(0,%d0:w),%a0 */
	{ uint32_t _p = ld16(ctx->a[1]); ctx->a[1] += 2; uint32_t _v = _p; set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00D618 movew %a1@+,%d2 */
	{ uint32_t _v = 0xA000u; set_w(&ctx->d[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00D61A movew #-24576,%d6 */
	{ uint32_t _v = ld16(0xFFFF9E4Au); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00D61E movew 0xffff9e4a,%d7 */
	{ uint32_t _p = ld16(ctx->a[1]); ctx->a[1] += 2; uint32_t _v = _p; set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00D624 movew %a1@+,%d3 */
	if (bs(ctx, c_n(ctx))) goto L_00D630; /* 00D626 bmis 0xd630 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x012234); } /* 00D628 jsr %pc@(0x12234) */
	hle_insn(ctx, 10); goto L_00D636; /* 00D62C braw 0xd636 */
L_00D630:;
	{ uint32_t _r = alu(ctx, OP_SUB, 0, ctx->d[3], 16, 1); set_w(&ctx->d[3], _r); hle_insn(ctx, 4); } /* 00D630 negw %d3 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x01227A); } /* 00D632 jsr %pc@(0x1227a) */
L_00D636:;
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00D636 movew %sp@+,%d7 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00D638 braw 0xb3be */
}

/* $00D4E6 truck: flying debris tile that falls apart */
HLE_RANGE(0x00D4E6, TruckDebris, 4000, EXIT_NEXT)
{
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00D4E6 subqw #1,%fp@(36) */
	if (bw(ctx, c_z(ctx))) goto L_00D564; /* 00D4EA beqw 0xd564 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00D4EE movew %fp@(36),%d0 */
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 00D4F2 andiw #1,%d0 */
	if (bw(ctx, !c_z(ctx))) goto L_00D50A; /* 00D4F6 bnew 0xd50a */
	{ uint32_t _v = ld8((ctx->a[6] + (uint32_t)(int32_t)14)); set_b(&ctx->d[0], _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 00D4FA moveb %fp@(14),%d0 */
	{ uint32_t _s = (ctx->d[0] & 0xFFu); uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)3); uint32_t _d = ld8(_da); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 8, 1); st8(_da, _r); hle_insn(ctx, 16); } /* 00D4FE addb %d0,%fp@(3) */
	{ uint32_t _v = ld8((ctx->a[6] + (uint32_t)(int32_t)15)); set_b(&ctx->d[0], _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 00D502 moveb %fp@(15),%d0 */
	{ uint32_t _s = (ctx->d[0] & 0xFFu); uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)5); uint32_t _d = ld8(_da); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 8, 1); st8(_da, _r); hle_insn(ctx, 16); } /* 00D506 addb %d0,%fp@(5) */
L_00D50A:;
	{ uint32_t _v = ld8((ctx->a[6] + (uint32_t)(int32_t)2)); set_b(&ctx->d[2], _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 00D50A moveb %fp@(2),%d2 */
	{ set_w(&ctx->d[2], (uint32_t)(int32_t)(int8_t)ctx->d[2]); cc_logic16(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 00D50E extw %d2 */
	{ uint32_t _v = ld8((ctx->a[6] + (uint32_t)(int32_t)4)); set_b(&ctx->d[1], _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 00D510 moveb %fp@(4),%d1 */
	{ set_w(&ctx->d[1], (uint32_t)(int32_t)(int8_t)ctx->d[1]); cc_logic16(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 00D514 extw %d1 */
	{ sh_w(ctx, 1, 'l', 4); hle_insn(ctx, 14); } /* 00D516 aslw #4,%d1 */
	{ uint32_t _v = (ctx->d[1] & 0xFFFFu); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 00D518 movew %d1,%d3 */
	{ sh_w(ctx, 1, 'l', 2); hle_insn(ctx, 10); } /* 00D51A aslw #2,%d1 */
	{ uint32_t _s = (ctx->d[1] & 0xFFFFu); uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[3], _r); hle_insn(ctx, 4); } /* 00D51C addw %d1,%d3 */
	{ uint32_t _s = (ctx->d[2] & 0xFFFFu); uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 4); } /* 00D51E addw %d2,%d2 */
	{ uint32_t _s = (ctx->d[2] & 0xFFFFu); uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[3], _r); hle_insn(ctx, 4); } /* 00D520 addw %d2,%d3 */
	{ ctx->a[0] = 0x00053D40u; hle_insn(ctx, 12); } /* 00D522 lea 0x53d40,%a0 */
	{ ctx->a[0] = (ctx->a[0] + (uint32_t)(int32_t)0 + (uint32_t)((int32_t)(int16_t)ctx->d[3])); hle_insn(ctx, 12); } /* 00D528 lea %a0@(0,%d3:w),%a0 */
	{ uint32_t _v = ld8((ctx->a[6] + (uint32_t)(int32_t)3)); set_b(&ctx->d[2], _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 00D52C moveb %fp@(3),%d2 */
	{ set_w(&ctx->d[2], (uint32_t)(int32_t)(int8_t)ctx->d[2]); cc_logic16(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 00D530 extw %d2 */
	{ uint32_t _v = ld8((ctx->a[6] + (uint32_t)(int32_t)5)); set_b(&ctx->d[1], _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 00D532 moveb %fp@(5),%d1 */
	{ set_w(&ctx->d[1], (uint32_t)(int32_t)(int8_t)ctx->d[1]); cc_logic16(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 00D536 extw %d1 */
	{ sh_w(ctx, 1, 'l', 7); hle_insn(ctx, 20); } /* 00D538 aslw #7,%d1 */
	{ uint32_t _s = (ctx->d[2] & 0xFFFFu); uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 4); } /* 00D53A addw %d2,%d2 */
	{ uint32_t _s = (ctx->d[2] & 0xFFFFu); uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 4); } /* 00D53C addw %d2,%d1 */
	{ uint32_t _s = 0xE500u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00D53E addiw #-6912,%d1 */
	{ ctx->d[2] = 0x0u; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 00D542 moveq #0,%d2 */
	{ ctx->d[3] = 0x0u; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 00D544 moveq #0,%d3 */
	{ uint32_t _v = ld8((ctx->a[6] + (uint32_t)(int32_t)11)); set_b(&ctx->d[2], _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 00D546 moveb %fp@(11),%d2 */
	{ uint32_t _v = ld8((ctx->a[6] + (uint32_t)(int32_t)13)); set_b(&ctx->d[3], _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 00D54A moveb %fp@(13),%d3 */
	{ uint32_t _v = 0xE000u; set_w(&ctx->d[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00D54E movew #-8192,%d6 */
	{ uint32_t _v = (ctx->d[7] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00D552 movew %d7,%sp@- */
	{ uint32_t _v = ld16(0xFFFF9E4Au); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00D554 movew 0xffff9e4a,%d7 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x012234); } /* 00D55A jsr %pc@(0x12234) */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00D55E movew %sp@+,%d7 */
L_00D560:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00D560 braw 0xb3be */
L_00D564:;
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 00D564 clrw %fp@ */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DAu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 00D566 subqw #1,0xffff80da */
	hle_insn(ctx, 10); goto L_00D560; /* 00D56C bras 0xd560 */
}

/* $00D0D4 truck: destroyed, explosion list entry */
/* bound 15500 = 1.5 x observed max 10258 (sub_00902E) */
HLE_RANGE(0x00D0D4, TruckDie, 15500, EXIT_NEXT)
{
	{ hle_insn(ctx, 18); sub_00D0F8(ctx); } /* 00D0D4 jsr %pc@(0xd0f8) */
	{ uint32_t _s = 0x2Bu; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 00D0D8 cmpiw #43,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_00D0E6; /* 00D0DE bnes 0xd0e6 */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_callc(ctx, 0x00902E); } /* 00D0E0 jsr 0x902e */
L_00D0E6:;
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00D0E6 subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_00D0F4; /* 00D0EA bnes 0xd0f4 */
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 00D0EC clrw %fp@ */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DAu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 00D0EE subqw #1,0xffff80da */
L_00D0F4:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00D0F4 braw 0xb3be */
}

/* $00DA48 level-1 type $00: sprite trio, countdown into $D9B2 */
HLE_RANGE_PURE(0x00DA48, TrooperDA48, 1000, EXIT_NEXT)
{
	goto L_00DA48;
L_00D9FC:;
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00D9FC subqw #1,%fp@(36) */
	if (bs(ctx, !c_n(ctx))) goto L_00DA14; /* 00DA00 bpls 0xda14 */
	{ uint32_t _v = 0xDA18u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00DA02 movel #55832,%fp@(32) */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o & 0x7F); hle_insn(ctx, 16); } /* 00DA0A bclr #7,%fp@ */
	{ uint32_t _v = 0x10u; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00DA0E movew #16,%fp@(36) */
L_00DA14:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00DA14 braw 0xb3be */
L_00DA48:;
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 00DA48 jsr %pc@(0x12316) */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o & 0x7F); hle_insn(ctx, 16); } /* 00DA4C bclr #15,%fp@ */
	{ uint32_t _v = 0x10Au; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00DA50 movew #266,%d1 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)20)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00DA54 movew %fp@(20),%d0 */
	{ uint32_t _s = 0xAu; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 00DA58 cmpiw #10,%d0 */
	if (bs(ctx, c_gt(ctx))) goto L_00DA66; /* 00DA5C bgts 0xda66 */
	{ uint32_t _v = 0x10Bu; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00DA5E movew #267,%d1 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o | 0x80); hle_insn(ctx, 16); } /* 00DA62 bset #7,%fp@ */
L_00DA66:;
	{ ctx->d[0] = 0xFFFFFFD8u; cc_logic32(ctx, ctx->d[0]); hle_insn(ctx, 4); } /* 00DA66 moveq #-40,%d0 */
	{ hle_insn(ctx, 18); ObjKillIfLeftOf(ctx); } /* 00DA68 jsr %pc@(0x12332) */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00DA6C bsrw 0x13228 */
	{ uint32_t _v = 0x10Cu; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00DA70 movew #268,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00DA74 bsrw 0x13228 */
	{ uint32_t _v = 0x108u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00DA78 movew #264,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00DA7C bsrw 0x13228 */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)20); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00DA80 subqw #1,%fp@(20) */
	if (bs(ctx, !c_z(ctx))) goto L_00DA8E; /* 00DA84 bnes 0xda8e */
	{ uint32_t _v = 0xD9B2u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00DA86 movel #55730,%fp@(32) */
L_00DA8E:;
	hle_insn(ctx, 10); goto L_00D9FC; /* 00DA8E braw 0xd9fc */
}

/* $00D978 level-1 ground trooper: rise, countdown into $D9B2 */
HLE_RANGE_PURE(0x00D978, TrooperD978, 1000, EXIT_NEXT)
{
	{ uint32_t _s = 0x8u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)4); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 00D978 subiw #8,%fp@(4) */
	{ uint32_t _v = 0x10Du; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00D97E movew #269,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00D982 bsrw 0x13228 */
	{ uint32_t _v = 0x109u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00D986 movew #265,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00D98A bsrw 0x13228 */
	{ uint32_t _v = 0x108u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00D98E movew #264,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00D992 bsrw 0x13228 */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00D996 subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_00D9AE; /* 00D99A bnes 0xd9ae */
	{ uint32_t _v = 0xD9B2u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00D99C movel #55730,%fp@(32) */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o | 0x80); hle_insn(ctx, 16); } /* 00D9A4 bset #15,%fp@ */
	{ uint32_t _v = 0x12Cu; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00D9A8 movew #300,%fp@(36) */
L_00D9AE:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00D9AE braw 0xb3be */
}

/* $00D9B2 ground trooper: shooting/animation */
HLE_RANGE(0x00D9B2, TrooperD9B2, 4000, EXIT_NEXT)
{
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 00D9B2 jsr %pc@(0x12316) */
	{ ctx->d[0] = 0xFFFFFFD8u; cc_logic32(ctx, ctx->d[0]); hle_insn(ctx, 4); } /* 00D9B6 moveq #-40,%d0 */
	{ hle_insn(ctx, 18); ObjKillIfLeftOf(ctx); } /* 00D9B8 jsr %pc@(0x12332) */
	{ uint32_t _v = 0x10Du; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00D9BC movew #269,%d1 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00D9C0 movew %fp@(36),%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 00D9C4 movew %d0,%d2 */
	{ uint32_t _s = 0x40u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 00D9C6 andiw #64,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_00D9E8; /* 00D9CA bnes 0xd9e8 */
	{ uint32_t _s = 0x6u; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 00D9CC andiw #6,%d2 */
	{ sh_w(ctx, 2, 'a', 1); hle_insn(ctx, 8); } /* 00D9D0 asrw #1,%d2 */
	{ uint32_t _s = 0x3u; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = (_d ^ _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 00D9D2 eoriw #3,%d2 */
	if (bs(ctx, !c_z(ctx))) goto L_00D9E6; /* 00D9D6 bnes 0xd9e6 */
	{ uint32_t _v = 0x60u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00D9D8 movew #96,%d0 */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_callc(ctx, 0x013D04); } /* 00D9DC jsr 0x13d04 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x009054); } /* 00D9E2 bsrw 0x9054 */
L_00D9E6:;
	{ uint32_t _s = (ctx->d[2] & 0xFFFFu); uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 4); } /* 00D9E6 addw %d2,%d1 */
L_00D9E8:;
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00D9E8 bsrw 0x13228 */
	{ uint32_t _v = 0x109u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00D9EC movew #265,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00D9F0 bsrw 0x13228 */
	{ uint32_t _v = 0x108u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00D9F4 movew #264,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00D9F8 bsrw 0x13228 */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00D9FC subqw #1,%fp@(36) */
	if (bs(ctx, !c_n(ctx))) goto L_00DA14; /* 00DA00 bpls 0xda14 */
	{ uint32_t _v = 0xDA18u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00DA02 movel #55832,%fp@(32) */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o & 0x7F); hle_insn(ctx, 16); } /* 00DA0A bclr #7,%fp@ */
	{ uint32_t _v = 0x10u; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00DA0E movew #16,%fp@(36) */
L_00DA14:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00DA14 braw 0xb3be */
}

/* $00DA18 ground trooper: leaves */
HLE_RANGE_PURE(0x00DA18, TrooperDA18, 1000, EXIT_NEXT)
{
	{ uint32_t _s = 0x8u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)4); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 00DA18 addiw #8,%fp@(4) */
	{ uint32_t _v = 0x10Du; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00DA1E movew #269,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00DA22 bsrw 0x13228 */
	{ uint32_t _v = 0x109u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00DA26 movew #265,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00DA2A bsrw 0x13228 */
	{ uint32_t _v = 0x108u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00DA2E movew #264,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00DA32 bsrw 0x13228 */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00DA36 subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_00DA44; /* 00DA3A bnes 0xda44 */
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 00DA3C clrw %fp@ */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DAu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 00DA3E subqw #1,0xffff80da */
L_00DA44:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00DA44 braw 0xb3be */
}

/* $00B69A endoskeleton death variant: debris and kill */
/* bound 9400 = 1.5 x observed max 6204 */
HLE_RANGE(0x00B69A, EndoDieDebris, 9400, EXIT_NEXT)
{
	{ ctx->d[5] = 0x3u; cc_logic32(ctx, ctx->d[5]); hle_insn(ctx, 4); } /* 00B69A moveq #3,%d5 */
L_00B69C:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00B69C movew %fp@(2),%d0 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00B6A0 movew %fp@(4),%d1 */
	{ uint32_t _s = 0x20u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00B6A4 subiw #32,%d1 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x012778); } /* 00B6A8 jsr %pc@(0x12778) */
	{ uint16_t _c = (uint16_t)ctx->d[5] - 1; set_w(&ctx->d[5], _c); if (_c != 0xFFFF) { hle_insn(ctx, 10); goto L_00B69C; } hle_insn(ctx, 14); } /* 00B6AC dbf %d5,0xb69c */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00B6B0 movew %fp@(2),%d0 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00B6B4 movew %fp@(4),%d1 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)6)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00B6B8 movew %fp@(6),%d2 */
	{ uint32_t _s = 0x20u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00B6BC subiw #32,%d1 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x01254E); } /* 00B6C0 jsr %pc@(0x1254e) */
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 00B6C4 clrw %fp@ */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DAu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 00B6C6 subqw #1,0xffff80da */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00B6CC braw 0xb3be */
}

/* $00B796 endoskeleton walk variant (second set of frames) */
HLE_RANGE_PURE(0x00B796, EndoWalk3, 1000, EXIT_NEXT)
{
	{ hle_insn(ctx, 18); ObjMoveX(ctx); } /* 00B796 jsr %pc@(0xb4a6) */
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 00B79A jsr %pc@(0x12316) */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 6) & 1) ? 0 : CC_Z); hle_insn(ctx, 12); } /* 00B79E btst #6,%fp@ */
	if (bs(ctx, c_z(ctx))) goto L_00B7A8; /* 00B7A2 beqs 0xb7a8 */
	{ ctx->d[1] = 0x54u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 00B7A4 moveq #84,%d1 */
	hle_insn(ctx, 10); goto L_00B7AA; /* 00B7A6 bras 0xb7aa */
L_00B7A8:;
	{ ctx->d[1] = 0x5Cu; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 00B7A8 moveq #92,%d1 */
L_00B7AA:;
	{ uint32_t _v = 0x168u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00B7AA movew #360,%d0 */
	{ hle_insn(ctx, 18); ObjKillIfRightOf(ctx); } /* 00B7AE jsr %pc@(0x12342) */
	{ ctx->d[0] = 0xFFFFFFD8u; cc_logic32(ctx, ctx->d[0]); hle_insn(ctx, 4); } /* 00B7B2 moveq #-40,%d0 */
	{ hle_insn(ctx, 18); ObjKillIfLeftOf(ctx); } /* 00B7B4 jsr %pc@(0x12332) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00B7B8 movew %fp@(36),%d0 */
	{ uint32_t _s = 0x38u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 00B7BC andiw #56,%d0 */
	{ sh_w(ctx, 0, 'r', 3); hle_insn(ctx, 12); } /* 00B7C0 lsrw #3,%d0 */
	{ uint32_t _s = 0x7u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d ^ _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 00B7C2 eoriw #7,%d0 */
	{ uint32_t _s = (ctx->d[0] & 0xFFFFu); uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 4); } /* 00B7C6 addw %d0,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00B7C8 bsrw 0x13228 */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00B7CC subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_00B7E0; /* 00B7D0 bnes 0xb7e0 */
	{ uint32_t _v = 0xB7E4u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00B7D2 movel #47076,%fp@(32) */
	{ uint32_t _v = 0x64u; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00B7DA movew #100,%fp@(36) */
L_00B7E0:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00B7E0 braw 0xb3be */
}

/* $00B7E4 endoskeleton shoot variant */
HLE_RANGE(0x00B7E4, EndoShoot3, 4000, EXIT_NEXT)
{
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 00B7E4 jsr %pc@(0x12316) */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00B7E8 subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_00B800; /* 00B7EC bnes 0xb800 */
	{ hle_insn(ctx, 18); sub_00B846(ctx); } /* 00B7EE jsr %pc@(0xb846) */
	{ uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)14); uint32_t _r = alu(ctx, OP_SUB, 0, ld8(_da), 8, 1); st8(_da, _r); hle_insn(ctx, 16); } /* 00B7F2 negb %fp@(14) */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 6) & 1) ? 0 : CC_Z); st8(_a, _o ^ 0x40); hle_insn(ctx, 16); } /* 00B7F6 bchg #6,%fp@ */
	{ uint32_t _v = 0x2710u; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00B7FA movew #10000,%fp@(36) */
L_00B800:;
	{ uint32_t _v = 0x168u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00B800 movew #360,%d0 */
	{ hle_insn(ctx, 18); ObjKillIfRightOf(ctx); } /* 00B804 jsr %pc@(0x12342) */
	{ ctx->d[0] = 0xFFFFFFD8u; cc_logic32(ctx, ctx->d[0]); hle_insn(ctx, 4); } /* 00B808 moveq #-40,%d0 */
	{ hle_insn(ctx, 18); ObjKillIfLeftOf(ctx); } /* 00B80A jsr %pc@(0x12332) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00B80E movew %fp@(36),%d0 */
	{ uint32_t _s = 0x50u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 00B812 cmpiw #80,%d0 */
	if (bs(ctx, c_gt(ctx))) goto L_00B83A; /* 00B816 bgts 0xb83a */
	{ uint32_t _s = 0x14u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 00B818 cmpiw #20,%d0 */
	if (bs(ctx, !c_ge(ctx))) goto L_00B83A; /* 00B81C blts 0xb83a */
	{ cc_set(ctx, CC_Z, ((ctx->d[0] >> 2) & 1) ? 0 : CC_Z); hle_insn(ctx, 10); } /* 00B81E btst #2,%d0 */
	if (bs(ctx, c_z(ctx))) goto L_00B83A; /* 00B822 beqs 0xb83a */
	{ uint32_t _v = 0x60u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00B824 movew #96,%d0 */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_callc(ctx, 0x013D04); } /* 00B828 jsr 0x13d04 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x009054); } /* 00B82E bsrw 0x9054 */
	{ uint32_t _v = 0x16Fu; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00B832 movew #367,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00B836 jsr %pc@(0x13228) */
L_00B83A:;
	{ uint32_t _v = 0x16Eu; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00B83A movew #366,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00B83E jsr %pc@(0x13228) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00B842 braw 0xb3be */
}

/* $00B860 type $11 after the hit: down */
HLE_RANGE_PURE(0x00B860, EndoHitDown3, 1000, EXIT_NEXT)
{
	{ uint32_t _v = 0x1u; st16((ctx->a[6] + (uint32_t)(int32_t)20), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00B860 movew #1,%fp@(20) */
	hle_insn(ctx, 10); goto L_00C66C; /* 00B866 braw 0xc66c */
L_00C66C:;
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 00C66C jsr %pc@(0x12316) */
	{ uint32_t _v = 0xFFu; st8((ctx->a[6] + (uint32_t)(int32_t)15), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 00C670 moveb #-1,%fp@(15) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); st16((ctx->a[6] + (uint32_t)(int32_t)40), _v); cc_logic16(ctx, _v); hle_insn(ctx, 20); } /* 00C676 movew %fp@(4),%fp@(40) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)20)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00C67C movew %fp@(20),%d0 */
	{ uint32_t _s = (ctx->d[0] & 0xFFFFu); uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 00C680 addw %d0,%d0 */
	{ uint32_t _s = (ctx->d[0] & 0xFFFFu); uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 00C682 addw %d0,%d0 */
	{ uint32_t _s = 0x68u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 00C684 addiw #104,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFu); st8((ctx->a[6] + (uint32_t)(int32_t)12), _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 00C688 moveb %d0,%fp@(12) */
	{ st8((ctx->a[6] + (uint32_t)(int32_t)13), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 00C68C clrb %fp@(13) */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o & 0x7F); hle_insn(ctx, 16); } /* 00C690 bclr #15,%fp@ */
	{ uint32_t _v = 0x7u; st8((ctx->a[6] + (uint32_t)(int32_t)18), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 00C694 moveb #7,%fp@(18) */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)7); uint32_t _d = ld8(_da); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 8, 1); st8(_da, _r); hle_insn(ctx, 16); } /* 00C69A addqb #1,%fp@(7) */
	{ uint32_t _v = ctx->a[5]; st32((ctx->a[6] + (uint32_t)(int32_t)22), _v); cc_logic32(ctx, _v); hle_insn(ctx, 16); } /* 00C69E movel %a5,%fp@(22) */
	{ uint32_t _v = 0x20u; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00C6A2 movew #32,%fp@(36) */
	{ uint32_t _v = 0xC6B0u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00C6A8 movel #50864,%fp@(32) */
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 00C6B0 jsr %pc@(0x12316) */
	{ uint32_t _s = 0xFFE2u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)2); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 00C6B4 cmpiw #-30,%fp@(2) */
	if (bs(ctx, c_gt(ctx))) goto L_00C6CE; /* 00C6BA bgts 0xc6ce */
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 00C6BC clrw %fp@ */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DAu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 00C6BE subqw #1,0xffff80da */
	{ st8(0xFFFF9EC0u, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 20); } /* 00C6C4 clrb 0xffff9ec0 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00C6CA braw 0xb3be */
L_00C6CE:;
	{ hle_insn(ctx, 18); sub_00D236(ctx); } /* 00C6CE jsr %pc@(0xd236) */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00C6D2 subqw #1,%fp@(36) */
	if (bs(ctx, !c_n(ctx))) goto L_00C6EC; /* 00C6D6 bpls 0xc6ec */
	{ ctx->d[1] = 0x4u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 00C6D8 moveq #4,%d1 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00C6DA movew %fp@(4),%d0 */
	{ uint32_t _s = ld16((ctx->a[6] + (uint32_t)(int32_t)40)); uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 12); } /* 00C6DE cmpw %fp@(40),%d0 */
	if (bw(ctx, c_gt(ctx))) goto L_00C7BA; /* 00C6E2 bgtw 0xc7ba */
	{ st16((ctx->a[6] + (uint32_t)(int32_t)18), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 00C6E6 clrw %fp@(18) */
	hle_insn(ctx, 10); goto L_00C6FA; /* 00C6EA bras 0xc6fa */
L_00C6EC:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00C6EC movew %fp@(36),%d1 */
	{ sh_w(ctx, 1, 'a', 1); hle_insn(ctx, 8); } /* 00C6F0 asrw #1,%d1 */
	{ uint32_t _s = 0xFu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d ^ _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00C6F2 eoriw #15,%d1 */
	{ uint32_t _v = ld8((0x00C748u + (uint32_t)((int32_t)(int16_t)ctx->d[1]))); set_b(&ctx->d[1], _v); cc_logic8(ctx, _v); hle_insn(ctx, 14); } /* 00C6F6 moveb %pc@(0xc748,%d1:w),%d1 */
L_00C6FA:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)20)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00C6FA movew %fp@(20),%d0 */
	{ uint32_t _s = ld8((0x00C758u + (uint32_t)((int32_t)(int16_t)ctx->d[0]))); uint32_t _d = (ctx->d[1] & 0xFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 8, 1); set_b(&ctx->d[1], _r); hle_insn(ctx, 14); } /* 00C6FE addb %pc@(0xc758,%d0:w),%d1 */
	{ uint32_t _v = ld32(0xFFFF80BCu); ctx->a[5] = _v; hle_insn(ctx, 20); } /* 00C702 moveal 0xffff80bc,%a5 */
	{ uint32_t _v = 0x1u; st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00C708 movew #1,%a5@+ */
	{ uint32_t _v = (ctx->d[1] & 0xFFFFu); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 00C70C movew %d1,%d0 */
	{ sh_w(ctx, 0, 'l', 2); hle_insn(ctx, 10); } /* 00C70E aslw #2,%d0 */
	{ ctx->a[1] = 0x00C76Au; hle_insn(ctx, 8); } /* 00C710 lea %pc@(0xc76a),%a1 */
	{ uint32_t _v = ld16((ctx->a[1] + (uint32_t)(int32_t)0 + (uint32_t)((int32_t)(int16_t)ctx->d[0]))); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 14); } /* 00C714 movew %a1@(0,%d0:w),%d2 */
	{ sh_w(ctx, 2, 'a', 1); hle_insn(ctx, 8); } /* 00C718 asrw #1,%d2 */
	{ uint32_t _v = (ctx->d[2] & 0xFFFFu); st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00C71A movew %d2,%a5@+ */
	{ uint32_t _v = ld16((ctx->a[1] + (uint32_t)(int32_t)2 + (uint32_t)((int32_t)(int16_t)ctx->d[0]))); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 14); } /* 00C71C movew %a1@(2,%d0:w),%d0 */
	{ ctx->a[1] = 0x00040000u; hle_insn(ctx, 12); } /* 00C720 lea 0x40000,%a1 */
	{ ctx->a[1] = (ctx->a[1] + (uint32_t)(int32_t)0 + (uint32_t)((int32_t)(int16_t)ctx->d[0])); hle_insn(ctx, 12); } /* 00C726 lea %a1@(0,%d0:w),%a1 */
	{ uint32_t _v = ctx->a[1]; st32(ctx->a[5], _v); ctx->a[5] += 4; cc_logic32(ctx, _v); hle_insn(ctx, 12); } /* 00C72A movel %a1,%a5@+ */
	{ uint32_t _v = ld16(0xFFFF9E24u); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00C72C movew 0xffff9e24,%d0 */
	{ sh_w(ctx, 0, 'l', 5); hle_insn(ctx, 16); } /* 00C732 aslw #5,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00C734 movew %d0,%a5@+ */
	{ uint32_t _v = ctx->a[5]; st32(0xFFFF80BCu, _v); cc_logic32(ctx, _v); hle_insn(ctx, 20); } /* 00C736 movel %a5,0xffff80bc */
	{ uint32_t _s = 0x96u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00C73C addiw #150,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00C740 jsr %pc@(0x13228) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00C744 braw 0xb3be */
L_00C7BA:;
	{ uint32_t _v = 0x40u; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00C7BA movew #64,%fp@(36) */
	{ uint32_t _v = 0xC7C8u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00C7C0 movel #51144,%fp@(32) */
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 00C7C8 jsr %pc@(0x12316) */
	{ uint32_t _s = 0xFFE2u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)2); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 00C7CC cmpiw #-30,%fp@(2) */
	if (bs(ctx, c_gt(ctx))) goto L_00C7E6; /* 00C7D2 bgts 0xc7e6 */
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 00C7D4 clrw %fp@ */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DAu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 00C7D6 subqw #1,0xffff80da */
	{ st8(0xFFFF9EC0u, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 20); } /* 00C7DC clrb 0xffff9ec0 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00C7E2 braw 0xb3be */
L_00C7E6:;
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00C7E6 subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_00C7FA; /* 00C7EA bnes 0xc7fa */
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 00C7EC clrw %fp@ */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DAu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 00C7EE subqw #1,0xffff80da */
	{ st8(0xFFFF9EC0u, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 20); } /* 00C7F4 clrb 0xffff9ec0 */
L_00C7FA:;
	{ uint32_t _v = 0x4u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00C7FA movew #4,%d1 */
	hle_insn(ctx, 10); goto L_00C6FA; /* 00C7FE braw 0xc6fa */
}

/* $00B86A type $0D: first frame tile rect */
HLE_RANGE(0x00B86A, Type0DInit, 4000, EXIT_NEXT)
{
	{ uint32_t _v = (ctx->d[7] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00B86A movew %d7,%sp@- */
	{ uint32_t _v = 0xC550u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00B86C movew #-15024,%d1 */
	{ ctx->a[0] = 0x000A0FC2u; hle_insn(ctx, 12); } /* 00B870 lea 0xa0fc2,%a0 */
	{ ctx->d[2] = 0x8u; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 00B876 moveq #8,%d2 */
	{ ctx->d[3] = 0x3u; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 00B878 moveq #3,%d3 */
	{ uint32_t _v = 0x0u; set_w(&ctx->d[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00B87A movew #0,%d6 */
	{ uint32_t _v = ld16(0xFFFF9E34u); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00B87E movew 0xffff9e34,%d7 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x01227A); } /* 00B884 jsr %pc@(0x1227a) */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00B888 movew %sp@+,%d7 */
	{ uint32_t _v = 0xB8C2u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00B88A movel #47298,%fp@(32) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00B892 braw 0xb3be */
}

/* $00B896 type $0D: two sprites then the walk */
HLE_RANGE(0x00B896, Type0DSprites, 4000, EXIT_NEXT)
{
	{ ctx->d[1] = 0x0u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 00B896 moveq #0,%d1 */
	{ hle_insn(ctx, 18); sub_00BABC(ctx); } /* 00B898 jsr %pc@(0xbabc) */
	{ uint32_t _s = 0x121u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00B89C addiw #289,%d1 */
	{ uint32_t _s = 0x4u; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 00B8A0 subiw #4,%d2 */
	{ uint32_t _s = 0x8u; uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[3], _r); hle_insn(ctx, 8); } /* 00B8A4 subiw #8,%d3 */
	{ hle_insn(ctx, 18); QueueSprite(ctx); } /* 00B8A8 jsr %pc@(0x13240) */
	{ ctx->d[1] = 0x3u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 00B8AC moveq #3,%d1 */
	{ hle_insn(ctx, 18); sub_00BABC(ctx); } /* 00B8AE jsr %pc@(0xbabc) */
	{ uint32_t _s = 0x121u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00B8B2 addiw #289,%d1 */
	{ uint32_t _s = 0x1Au; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 00B8B6 addiw #26,%d2 */
	{ uint32_t _s = 0x9u; uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[3], _r); hle_insn(ctx, 8); } /* 00B8BA subiw #9,%d3 */
	{ hle_insn(ctx, 18); QueueSprite(ctx); } /* 00B8BE jsr %pc@(0x13240) */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x009078); } /* 00B8C2 bsrw 0x9078 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00B8C6 movew %fp@(36),%d0 */
	{ uint32_t _s = 0x8u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 00B8CA andiw #8,%d0 */
	{ sh_w(ctx, 0, 'r', 1); hle_insn(ctx, 8); } /* 00B8CE lsrw #1,%d0 */
	{ ctx->a[1] = 0x00BA5Au; hle_insn(ctx, 8); } /* 00B8D0 lea %pc@(0xba5a),%a1 */
	{ uint32_t _v = ld32((ctx->a[1] + (uint32_t)(int32_t)0 + (uint32_t)((int32_t)(int16_t)ctx->d[0]))); ctx->a[0] = _v; hle_insn(ctx, 18); } /* 00B8D4 moveal %a1@(0,%d0:w),%a0 */
	{ uint32_t _v = 0x4u; set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00B8D8 movew #4,%d2 */
	{ uint32_t _v = 0x3u; set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00B8DC movew #3,%d3 */
	{ uint32_t _v = 0xC558u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00B8E0 movew #-15016,%d1 */
	{ uint32_t _v = 0x0u; set_w(&ctx->d[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00B8E4 movew #0,%d6 */
	{ uint32_t _v = (ctx->d[7] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00B8E8 movew %d7,%sp@- */
	{ uint32_t _v = ld16(0xFFFF9E34u); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00B8EA movew 0xffff9e34,%d7 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x01227A); } /* 00B8F0 jsr %pc@(0x1227a) */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00B8F4 movew %sp@+,%d7 */
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 00B8F6 jsr %pc@(0x12316) */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00B8FA subqw #1,%fp@(36) */
	{ ctx->a[0] = 0xFFFFAB9Eu; hle_insn(ctx, 12); } /* 00B8FE lea 0xffffab9e,%a0 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00B904 movew %fp@(2),%d0 */
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 00B908 subqw #1,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)2), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00B90A movew %d0,%fp@(2) */
	{ uint32_t _s = 0xFFE0u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 00B90E cmpiw #-32,%d0 */
	if (bs(ctx, c_gt(ctx))) goto L_00B91C; /* 00B912 bgts 0xb91c */
	{ uint32_t _v = 0xB932u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00B914 movel #47410,%fp@(32) */
L_00B91C:;
	{ uint32_t _s = 0x160u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 00B91C subiw #352,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16(ctx->a[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00B920 movew %d0,%a0@ */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[0] + (uint32_t)(int32_t)32), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00B922 movew %d0,%a0@(32) */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[0] + (uint32_t)(int32_t)64), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00B926 movew %d0,%a0@(64) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00B92A braw 0xb3be */
}

/* $00B8C2 type $0D: walk across */
HLE_RANGE(0x00B8C2, Type0DWalk, 4000, EXIT_NEXT)
{
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x009078); } /* 00B8C2 bsrw 0x9078 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00B8C6 movew %fp@(36),%d0 */
	{ uint32_t _s = 0x8u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 00B8CA andiw #8,%d0 */
	{ sh_w(ctx, 0, 'r', 1); hle_insn(ctx, 8); } /* 00B8CE lsrw #1,%d0 */
	{ ctx->a[1] = 0x00BA5Au; hle_insn(ctx, 8); } /* 00B8D0 lea %pc@(0xba5a),%a1 */
	{ uint32_t _v = ld32((ctx->a[1] + (uint32_t)(int32_t)0 + (uint32_t)((int32_t)(int16_t)ctx->d[0]))); ctx->a[0] = _v; hle_insn(ctx, 18); } /* 00B8D4 moveal %a1@(0,%d0:w),%a0 */
	{ uint32_t _v = 0x4u; set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00B8D8 movew #4,%d2 */
	{ uint32_t _v = 0x3u; set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00B8DC movew #3,%d3 */
	{ uint32_t _v = 0xC558u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00B8E0 movew #-15016,%d1 */
	{ uint32_t _v = 0x0u; set_w(&ctx->d[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00B8E4 movew #0,%d6 */
	{ uint32_t _v = (ctx->d[7] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00B8E8 movew %d7,%sp@- */
	{ uint32_t _v = ld16(0xFFFF9E34u); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00B8EA movew 0xffff9e34,%d7 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x01227A); } /* 00B8F0 jsr %pc@(0x1227a) */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00B8F4 movew %sp@+,%d7 */
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 00B8F6 jsr %pc@(0x12316) */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00B8FA subqw #1,%fp@(36) */
	{ ctx->a[0] = 0xFFFFAB9Eu; hle_insn(ctx, 12); } /* 00B8FE lea 0xffffab9e,%a0 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00B904 movew %fp@(2),%d0 */
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 00B908 subqw #1,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)2), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00B90A movew %d0,%fp@(2) */
	{ uint32_t _s = 0xFFE0u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 00B90E cmpiw #-32,%d0 */
	if (bs(ctx, c_gt(ctx))) goto L_00B91C; /* 00B912 bgts 0xb91c */
	{ uint32_t _v = 0xB932u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00B914 movel #47410,%fp@(32) */
L_00B91C:;
	{ uint32_t _s = 0x160u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 00B91C subiw #352,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16(ctx->a[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00B920 movew %d0,%a0@ */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[0] + (uint32_t)(int32_t)32), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00B922 movew %d0,%a0@(32) */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[0] + (uint32_t)(int32_t)64), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00B926 movew %d0,%a0@(64) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00B92A braw 0xb3be */
}

/* $00B92E type $0D killed: debris then the cleanup */
/* bound 15500 = 1.5 x observed max 10194 */
HLE_RANGE(0x00B92E, Type0DDieDebris, 15500, EXIT_NEXT)
{
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x00BCC6); } /* 00B92E bsrw 0xbcc6 */
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 00B932 clrw %fp@ */
	{ hle_insn(ctx, 18); sub_0098FC(ctx); } /* 00B934 jsr %pc@(0x98fc) */
	{ uint32_t _v = (ctx->d[7] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00B938 movew %d7,%sp@- */
	{ uint32_t _v = 0xC550u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00B93A movew #-15024,%d1 */
	{ ctx->a[0] = 0x00046EC4u; hle_insn(ctx, 12); } /* 00B93E lea 0x46ec4,%a0 */
	{ ctx->d[2] = 0x8u; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 00B944 moveq #8,%d2 */
	{ ctx->d[3] = 0x3u; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 00B946 moveq #3,%d3 */
	{ uint32_t _v = 0x0u; set_w(&ctx->d[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00B948 movew #0,%d6 */
	{ uint32_t _v = ld16(0xFFFF9E38u); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00B94C movew 0xffff9e38,%d7 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x012234); } /* 00B952 jsr %pc@(0x12234) */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00B956 movew %sp@+,%d7 */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DAu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 00B958 subqw #1,0xffff80da */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00B95E braw 0xb3be */
}

/* $00B932 type $0D: cleanup */
HLE_RANGE(0x00B932, Type0DGone, 4000, EXIT_NEXT)
{
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 00B932 clrw %fp@ */
	{ hle_insn(ctx, 18); sub_0098FC(ctx); } /* 00B934 jsr %pc@(0x98fc) */
	{ uint32_t _v = (ctx->d[7] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00B938 movew %d7,%sp@- */
	{ uint32_t _v = 0xC550u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00B93A movew #-15024,%d1 */
	{ ctx->a[0] = 0x00046EC4u; hle_insn(ctx, 12); } /* 00B93E lea 0x46ec4,%a0 */
	{ ctx->d[2] = 0x8u; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 00B944 moveq #8,%d2 */
	{ ctx->d[3] = 0x3u; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 00B946 moveq #3,%d3 */
	{ uint32_t _v = 0x0u; set_w(&ctx->d[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00B948 movew #0,%d6 */
	{ uint32_t _v = ld16(0xFFFF9E38u); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00B94C movew 0xffff9e38,%d7 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x012234); } /* 00B952 jsr %pc@(0x12234) */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00B956 movew %sp@+,%d7 */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DAu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 00B958 subqw #1,0xffff80da */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00B95E braw 0xb3be */
}

/* $00C802 crate rise (1) */
HLE_RANGE_PURE(0x00C802, CrateRise, 1000, EXIT_NEXT)
{
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00C802 movew %fp@(4),%d0 */
	{ uint32_t _s = 0x10u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 00C806 addiw #16,%d0 */
	{ uint32_t _s = 0xD0u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 00C80A cmpiw #208,%d0 */
	if (bs(ctx, !c_ge(ctx))) goto L_00C81C; /* 00C80E blts 0xc81c */
	{ uint32_t _v = 0xC826u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00C810 movel #51238,%fp@(32) */
	{ uint32_t _v = 0xCFu; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00C818 movew #207,%d0 */
L_00C81C:;
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)4), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00C81C movew %d0,%fp@(4) */
	{ ctx->d[1] = 0x16u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 00C820 moveq #22,%d1 */
	hle_insn(ctx, 10); goto L_00C8A8; /* 00C822 braw 0xc8a8 */
L_00C8A8:;
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 00C8A8 jsr %pc@(0x12316) */
	{ ctx->d[0] = 0xFFFFFFF4u; cc_logic32(ctx, ctx->d[0]); hle_insn(ctx, 4); } /* 00C8AC moveq #-12,%d0 */
	{ hle_insn(ctx, 18); ObjKillIfLeftOf(ctx); } /* 00C8AE jsr %pc@(0x12332) */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00C8B2 bsrw 0x13228 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00C8B6 braw 0xb3be */
}

/* $00C826 crate rise (2) */
HLE_RANGE_PURE(0x00C826, CrateRise2, 1000, EXIT_NEXT)
{
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00C826 movew %fp@(4),%d0 */
	{ uint32_t _s = 0x2u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 00C82A subiw #2,%d0 */
	{ uint32_t _s = 0xC7u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 00C82E cmpiw #199,%d0 */
	if (bs(ctx, c_gt(ctx))) goto L_00C83C; /* 00C832 bgts 0xc83c */
	{ uint32_t _v = 0xC846u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00C834 movel #51270,%fp@(32) */
L_00C83C:;
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)4), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00C83C movew %d0,%fp@(4) */
	{ ctx->d[1] = 0x16u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 00C840 moveq #22,%d1 */
	hle_insn(ctx, 10); goto L_00C8A8; /* 00C842 braw 0xc8a8 */
L_00C8A8:;
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 00C8A8 jsr %pc@(0x12316) */
	{ ctx->d[0] = 0xFFFFFFF4u; cc_logic32(ctx, ctx->d[0]); hle_insn(ctx, 4); } /* 00C8AC moveq #-12,%d0 */
	{ hle_insn(ctx, 18); ObjKillIfLeftOf(ctx); } /* 00C8AE jsr %pc@(0x12332) */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00C8B2 bsrw 0x13228 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00C8B6 braw 0xb3be */
}

/* $00C846 crate rise (3) */
HLE_RANGE_PURE(0x00C846, CrateRise3, 1000, EXIT_NEXT)
{
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00C846 movew %fp@(4),%d0 */
	{ uint32_t _s = 0x2u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 00C84A addiw #2,%d0 */
	{ uint32_t _s = 0xD0u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 00C84E cmpiw #208,%d0 */
	if (bs(ctx, !c_ge(ctx))) goto L_00C87C; /* 00C852 blts 0xc87c */
	{ uint32_t _v = 0xC886u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00C854 movel #51334,%fp@(32) */
	{ uint32_t _v = ld16(ctx->a[6]); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00C85C movew %fp@,%d0 */
	{ cc_set(ctx, CC_Z, ((ctx->d[0] >> 15) & 1) ? 0 : CC_Z); ctx->d[0] |= 0x8000u; hle_insn(ctx, 10); } /* 00C85E bset #15,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16(ctx->a[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00C862 movew %d0,%fp@ */
	{ uint32_t _v = 0x1F4u; st16((ctx->a[6] + (uint32_t)(int32_t)20), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00C864 movew #500,%fp@(20) */
	{ uint32_t _v = 0xD0u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00C86A movew #208,%d0 */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 00C86E cmpiw #1,%fp@(36) */
	if (bs(ctx, c_z(ctx))) goto L_00C87C; /* 00C874 beqs 0xc87c */
	{ uint32_t _v = 0x0u; st16((ctx->a[6] + (uint32_t)(int32_t)20), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00C876 movew #0,%fp@(20) */
L_00C87C:;
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)4), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00C87C movew %d0,%fp@(4) */
	{ ctx->d[1] = 0x16u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 00C880 moveq #22,%d1 */
	hle_insn(ctx, 10); goto L_00C8A8; /* 00C882 braw 0xc8a8 */
L_00C8A8:;
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 00C8A8 jsr %pc@(0x12316) */
	{ ctx->d[0] = 0xFFFFFFF4u; cc_logic32(ctx, ctx->d[0]); hle_insn(ctx, 4); } /* 00C8AC moveq #-12,%d0 */
	{ hle_insn(ctx, 18); ObjKillIfLeftOf(ctx); } /* 00C8AE jsr %pc@(0x12332) */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00C8B2 bsrw 0x13228 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00C8B6 braw 0xb3be */
}

/* $00627A hit test of John Connor truck (two boxes) */
HLE_FUNC_PURE(0x00627A, HitTruck, 300)
{
	{ ctx->a[3] = 0x0062A0u; hle_insn(ctx, 8); } /* 00627A lea %pc@(0x62a0),%a3 */
	{ ctx->a[2] = 0x00628Eu; hle_insn(ctx, 8); } /* 00627E lea %pc@(0x628e),%a2 */
	{ hle_insn(ctx, 18); HitBox(ctx); } /* 006282 jsr %pc@(0x5940) */
	{ ctx->a[3] = 0x0062A8u; hle_insn(ctx, 8); } /* 006286 lea %pc@(0x62a8),%a3 */
	hle_insn(ctx, 10); HitBox(ctx); return; /* 00628A jmp %pc@(0x5940) */
}

/* $005ABC hit test of type $11 */
HLE_FUNC_PURE(0x005ABC, HitType11, 300)
{
	{ ctx->a[2] = 0x005AC8u; hle_insn(ctx, 8); } /* 005ABC lea %pc@(0x5ac8),%a2 */
	{ ctx->a[3] = 0x005AF0u; hle_insn(ctx, 8); } /* 005AC0 lea %pc@(0x5af0),%a3 */
	hle_insn(ctx, 10); HitBox(ctx); return; /* 005AC4 jmp %pc@(0x5940) */
}

/* $005E1C hit test of type $0D */
HLE_FUNC_PURE(0x005E1C, HitType0D, 300)
{
	{ ctx->a[3] = 0x005E4Eu; hle_insn(ctx, 8); } /* 005E1C lea %pc@(0x5e4e),%a3 */
	{ ctx->a[2] = 0x005E28u; hle_insn(ctx, 8); } /* 005E20 lea %pc@(0x5e28),%a2 */
	hle_insn(ctx, 10); HitBox(ctx); return; /* 005E24 jmp %pc@(0x5940) */
}

