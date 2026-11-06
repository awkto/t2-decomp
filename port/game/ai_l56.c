/* Level-5/6 (Cyberdyne lab) object types: AI states and hit tests of the Cyberdyne equipment
 * ($0F: $00F8A2 + kill $00F8EA), the ceiling fixture ($20: $00FB52/$00FB5A/$00FB60), the
 * endoskeleton ($27) and its foot-soldier variant ($01: $00FD40/$00FD64/$00FD98) of the lab, the lab
 * equipment / T-800 model ($28: $010238..$010582, $0105D4, $0105F6, ...) and the white-lab ceiling
 * fixture ($0A: $00C348/$00C3AC). docs/objects_types.md ("Level 5", "Level 6"), src/t2.s.
 *
 * Like ai_simple.c / ai_l3.c: every AI state is a range from its entry (reached by the `jmp (a0)` of
 * ObjRunAll) to `bra.w ObjNext` (the C range at $00B3BE, reached with hle_jump), or to the
 * `jmp (a1)` that enters the freshly chosen state ($00FE8E). C callees (ObjScrollX, ObjMoveX,
 * ObjKillIf*, ObjQueueSprite, QueueSprite, Random, HitBox) are called directly after their jsr is
 * charged; the small helper subroutines that only touch work RAM (sub_00D236, sub_00FD0C,
 * sub_01042C, sub_010666, ...) are static C functions below; every other callee (the sound and
 * damage calls sub_013D04/sub_009054/sub_00908A, the debris spawners sub_012A16/sub_012A46/
 * sub_01277C/sub_0124EC, VdpQueueTileRect, ...) runs as 68k through hle_call68k, so a state that
 * reaches one is a plain HLE_RANGE and the others HLE_RANGE_PURE. Every instruction is on the
 * instruction timeline (GPGX costs). The body of each function below was produced instruction by
 * instruction from the disassembly (a comment on every line names the 68k instruction).
 *
 * The on-hit callbacks the hit tests hand to ShotHitTest stay 68k. */
#include "hle.h"

#include <stdint.h>

void ObjQueueSprite(hle_ctx *ctx);
void QueueSprite(hle_ctx *ctx);
void Random(hle_ctx *ctx);
void HitBox(hle_ctx *ctx);
void ObjMoveX(hle_ctx *ctx);
void ObjScrollX(hle_ctx *ctx);
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

static void sub_00D236(hle_ctx *ctx);
static void sub_00FD0C(hle_ctx *ctx);
static void sub_00FFC0(hle_ctx *ctx);
static void sub_0102A6(hle_ctx *ctx);
static void sub_010408(hle_ctx *ctx);
static void sub_01042C(hle_ctx *ctx);
static void sub_0104EE(hle_ctx *ctx);
static void sub_010642(hle_ctx *ctx);
static void sub_010666(hle_ctx *ctx);

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

/* sub_00FD0C (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_00FD0C(hle_ctx *ctx)
{
	{ uint32_t _v = 0xFBAAu; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00FD0C movel #64426,%fp@(32) */
	{ hle_insn(ctx, 18); Random(ctx); } /* 00FD14 jsr %pc@(0x881c) */
	{ uint32_t _s = 0x3Fu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 00FD18 andiw #63,%d0 */
	{ uint32_t _s = 0x50u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 00FD1C addiw #80,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00FD20 movew %d0,%fp@(36) */
	hle_insn(ctx, 16); return; /* 00FD24 rts */
}

/* sub_00FFC0 (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_00FFC0(hle_ctx *ctx)
{
	{ uint32_t _v = 0xFF12u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00FFC0 movel #65298,%fp@(32) */
	{ hle_insn(ctx, 18); Random(ctx); } /* 00FFC8 jsr %pc@(0x881c) */
	{ uint32_t _s = 0x3Fu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 00FFCC andiw #63,%d0 */
	{ uint32_t _s = 0x50u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 00FFD0 addiw #80,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00FFD4 movew %d0,%fp@(36) */
	hle_insn(ctx, 16); return; /* 00FFD8 rts */
}

/* sub_0102A6 (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_0102A6(hle_ctx *ctx)
{
	{ uint32_t _s = 0xFFE2u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)2); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 0102A6 cmpiw #-30,%fp@(2) */
	if (bs(ctx, !c_ge(ctx))) goto L_0102B6; /* 0102AC blts 0x102b6 */
	{ uint32_t _s = 0x15Eu; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)2); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 0102AE cmpiw #350,%fp@(2) */
	if (bs(ctx, c_le(ctx))) goto L_0102CE; /* 0102B4 bles 0x102ce */
L_0102B6:;
	{ uint32_t _v = 0x1036Eu; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 0102B6 movel #66414,%fp@(32) */
	{ hle_insn(ctx, 18); Random(ctx); } /* 0102BE jsr %pc@(0x881c) */
	{ uint32_t _s = 0x3Fu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0102C2 andiw #63,%d0 */
	{ uint32_t _s = 0x64u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 0102C6 addiw #100,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0102CA movew %d0,%fp@(36) */
L_0102CE:;
	hle_insn(ctx, 16); return; /* 0102CE rts */
}

/* sub_010408 (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_010408(hle_ctx *ctx)
{
	{ uint32_t _v = 0x10238u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 010408 movel #66104,%fp@(32) */
	{ hle_insn(ctx, 18); Random(ctx); } /* 010410 jsr %pc@(0x881c) */
	{ uint32_t _s = 0x3Fu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 010414 andiw #63,%d0 */
	{ uint32_t _s = 0x50u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 010418 addiw #80,%d0 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 6) & 1) ? 0 : CC_Z); hle_insn(ctx, 12); } /* 01041C btst #14,%fp@ */
	if (bs(ctx, !c_z(ctx))) goto L_010426; /* 010420 bnes 0x10426 */
	{ uint32_t _s = 0x32u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 010422 subiw #50,%d0 */
L_010426:;
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010426 movew %d0,%fp@(36) */
	hle_insn(ctx, 16); return; /* 01042A rts */
}

/* sub_01042C (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_01042C(hle_ctx *ctx)
{
	{ uint32_t _v = (ctx->d[1] & 0xFFFFu); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 01042C movew %d1,%d0 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 6) & 1) ? 0 : CC_Z); hle_insn(ctx, 12); } /* 01042E btst #14,%fp@ */
	if (bs(ctx, c_z(ctx))) goto L_010438; /* 010432 beqs 0x10438 */
	{ uint32_t _s = 0xFu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 010434 addiw #15,%d1 */
L_010438:;
	{ uint32_t _v = ld32(0xFFFF80BCu); ctx->a[5] = _v; hle_insn(ctx, 20); } /* 010438 moveal 0xffff80bc,%a5 */
	{ uint32_t _v = 0x1u; st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01043E movew #1,%a5@+ */
	{ sh_w(ctx, 0, 'l', 2); hle_insn(ctx, 10); } /* 010442 aslw #2,%d0 */
	{ ctx->a[1] = 0x01047Au; hle_insn(ctx, 8); } /* 010444 lea %pc@(0x1047a),%a1 */
	{ uint32_t _v = ld16((ctx->a[1] + (uint32_t)(int32_t)0 + (uint32_t)((int32_t)(int16_t)ctx->d[0]))); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 14); } /* 010448 movew %a1@(0,%d0:w),%d2 */
	{ sh_w(ctx, 2, 'a', 1); hle_insn(ctx, 8); } /* 01044C asrw #1,%d2 */
	{ uint32_t _v = (ctx->d[2] & 0xFFFFu); st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 01044E movew %d2,%a5@+ */
	{ uint32_t _v = ld16((ctx->a[1] + (uint32_t)(int32_t)2 + (uint32_t)((int32_t)(int16_t)ctx->d[0]))); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 14); } /* 010450 movew %a1@(2,%d0:w),%d0 */
	{ ctx->a[1] = 0x000DCCBEu; hle_insn(ctx, 12); } /* 010454 lea 0xdccbe,%a1 */
	{ ctx->a[1] = (ctx->a[1] + (uint32_t)(int32_t)0 + (uint32_t)((int32_t)(int16_t)ctx->d[0])); hle_insn(ctx, 12); } /* 01045A lea %a1@(0,%d0:w),%a1 */
	{ uint32_t _v = ctx->a[1]; st32(ctx->a[5], _v); ctx->a[5] += 4; cc_logic32(ctx, _v); hle_insn(ctx, 12); } /* 01045E movel %a1,%a5@+ */
	{ uint32_t _v = ld16(0xFFFF9E5Au); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 010460 movew 0xffff9e5a,%d0 */
	{ sh_w(ctx, 0, 'l', 5); hle_insn(ctx, 16); } /* 010466 aslw #5,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010468 movew %d0,%a5@+ */
	{ uint32_t _v = ctx->a[5]; st32(0xFFFF80BCu, _v); cc_logic32(ctx, _v); hle_insn(ctx, 20); } /* 01046A movel %a5,0xffff80bc */
	{ uint32_t _s = 0x288u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 010470 addiw #648,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 010474 jsr %pc@(0x13228) */
	hle_insn(ctx, 16); return; /* 010478 rts */
}

/* sub_0104EE (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_0104EE(hle_ctx *ctx)
{
	{ uint32_t _s = 0xFFE2u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)2); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 0104EE cmpiw #-30,%fp@(2) */
	if (bs(ctx, !c_ge(ctx))) goto L_0104FE; /* 0104F4 blts 0x104fe */
	{ uint32_t _s = 0x15Eu; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)2); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 0104F6 cmpiw #350,%fp@(2) */
	if (bs(ctx, c_le(ctx))) goto L_010516; /* 0104FC bles 0x10516 */
L_0104FE:;
	{ uint32_t _v = 0x10582u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 0104FE movel #66946,%fp@(32) */
	{ hle_insn(ctx, 18); Random(ctx); } /* 010506 jsr %pc@(0x881c) */
	{ uint32_t _s = 0x3Fu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 01050A andiw #63,%d0 */
	{ uint32_t _s = 0x64u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 01050E addiw #100,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010512 movew %d0,%fp@(36) */
L_010516:;
	hle_insn(ctx, 16); return; /* 010516 rts */
}

/* sub_010642 (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_010642(hle_ctx *ctx)
{
	{ uint32_t _v = 0x104B6u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 010642 movel #66742,%fp@(32) */
	{ hle_insn(ctx, 18); Random(ctx); } /* 01064A jsr %pc@(0x881c) */
	{ uint32_t _s = 0x3Fu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 01064E andiw #63,%d0 */
	{ uint32_t _s = 0x50u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 010652 addiw #80,%d0 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 6) & 1) ? 0 : CC_Z); hle_insn(ctx, 12); } /* 010656 btst #14,%fp@ */
	if (bs(ctx, !c_z(ctx))) goto L_010660; /* 01065A bnes 0x10660 */
	{ uint32_t _s = 0x32u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 01065C subiw #50,%d0 */
L_010660:;
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010660 movew %d0,%fp@(36) */
	hle_insn(ctx, 16); return; /* 010664 rts */
}

/* sub_010666 (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_010666(hle_ctx *ctx)
{
	{ uint32_t _v = (ctx->d[1] & 0xFFFFu); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 010666 movew %d1,%d0 */
	{ uint32_t _s = 0x22u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 010668 cmpiw #34,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_010678; /* 01066C bnes 0x10678 */
	{ uint32_t _v = 0xAEu; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 01066E movew #174,%d1 */
	{ uint32_t _v = 0x11u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010672 movew #17,%d0 */
	hle_insn(ctx, 10); goto L_010682; /* 010676 bras 0x10682 */
L_010678:;
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 6) & 1) ? 0 : CC_Z); hle_insn(ctx, 12); } /* 010678 btst #14,%fp@ */
	if (bs(ctx, c_z(ctx))) goto L_010682; /* 01067C beqs 0x10682 */
	{ uint32_t _s = 0x11u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 01067E addiw #17,%d1 */
L_010682:;
	{ uint32_t _v = ld32(0xFFFF80BCu); ctx->a[5] = _v; hle_insn(ctx, 20); } /* 010682 moveal 0xffff80bc,%a5 */
	{ uint32_t _v = 0x1u; st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010688 movew #1,%a5@+ */
	{ sh_w(ctx, 0, 'l', 2); hle_insn(ctx, 10); } /* 01068C aslw #2,%d0 */
	{ ctx->a[1] = 0x0106C4u; hle_insn(ctx, 8); } /* 01068E lea %pc@(0x106c4),%a1 */
	{ uint32_t _v = ld16((ctx->a[1] + (uint32_t)(int32_t)0 + (uint32_t)((int32_t)(int16_t)ctx->d[0]))); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 14); } /* 010692 movew %a1@(0,%d0:w),%d2 */
	{ sh_w(ctx, 2, 'a', 1); hle_insn(ctx, 8); } /* 010696 asrw #1,%d2 */
	{ uint32_t _v = (ctx->d[2] & 0xFFFFu); st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010698 movew %d2,%a5@+ */
	{ uint32_t _v = ld16((ctx->a[1] + (uint32_t)(int32_t)2 + (uint32_t)((int32_t)(int16_t)ctx->d[0]))); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 14); } /* 01069A movew %a1@(2,%d0:w),%d0 */
	{ ctx->a[1] = 0x000A931Au; hle_insn(ctx, 12); } /* 01069E lea 0xa931a,%a1 */
	{ ctx->a[1] = (ctx->a[1] + (uint32_t)(int32_t)0 + (uint32_t)((int32_t)(int16_t)ctx->d[0])); hle_insn(ctx, 12); } /* 0106A4 lea %a1@(0,%d0:w),%a1 */
	{ uint32_t _v = ctx->a[1]; st32(ctx->a[5], _v); ctx->a[5] += 4; cc_logic32(ctx, _v); hle_insn(ctx, 12); } /* 0106A8 movel %a1,%a5@+ */
	{ uint32_t _v = ld16(0xFFFF9E5Eu); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 0106AA movew 0xffff9e5e,%d0 */
	{ sh_w(ctx, 0, 'l', 5); hle_insn(ctx, 16); } /* 0106B0 aslw #5,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0106B2 movew %d0,%a5@+ */
	{ uint32_t _v = ctx->a[5]; st32(0xFFFF80BCu, _v); cc_logic32(ctx, _v); hle_insn(ctx, 20); } /* 0106B4 movel %a5,0xffff80bc */
	{ uint32_t _s = 0x2C1u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 0106BA addiw #705,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 0106BE jsr %pc@(0x13228) */
	hle_insn(ctx, 16); return; /* 0106C2 rts */
}

/* $00F8A2 $0F Cyberdyne equipment: scroll, kill off the left, the lit-LED blink */
HLE_RANGE_PURE(0x00F8A2, L56EquipAi, 1000, EXIT_NEXT)
{
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 00F8A2 jsr %pc@(0x12316) */
	{ ctx->d[0] = 0xFFFFFFE0u; cc_logic32(ctx, ctx->d[0]); hle_insn(ctx, 4); } /* 00F8A6 moveq #-32,%d0 */
	{ cc_logic16(ctx, ld16((ctx->a[6] + (uint32_t)(int32_t)26))); hle_insn(ctx, 12); } /* 00F8A8 tstw %fp@(26) */
	if (bs(ctx, !c_z(ctx))) goto L_00F8B0; /* 00F8AC bnes 0xf8b0 */
	{ ctx->d[0] = 0xFFFFFF88u; cc_logic32(ctx, ctx->d[0]); hle_insn(ctx, 4); } /* 00F8AE moveq #-120,%d0 */
L_00F8B0:;
	{ hle_insn(ctx, 18); ObjKillIfLeftOf(ctx); } /* 00F8B0 jsr %pc@(0x12332) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)38)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00F8B4 movew %fp@(38),%d0 */
	{ uint32_t _s = 0xFFFEu; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 00F8B8 cmpiw #-2,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_00F8DE; /* 00F8BC bnes 0xf8de */
	{ uint32_t _v = ld16(0xFFFFA0EEu); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00F8BE movew 0xffffa0ee,%d1 */
	{ uint32_t _v = (ctx->d[1] & 0xFFFFu); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 00F8C4 movew %d1,%d3 */
	{ uint32_t _s = 0x40u; uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[3], _r); hle_insn(ctx, 8); } /* 00F8C6 andiw #64,%d3 */
	if (bs(ctx, c_z(ctx))) goto L_00F8DE; /* 00F8CA beqs 0xf8de */
	{ uint32_t _s = 0x38u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00F8CC andiw #56,%d1 */
	{ sh_w(ctx, 1, 'a', 3); hle_insn(ctx, 12); } /* 00F8D0 asrw #3,%d1 */
	{ uint32_t _v = ld8((0x00F8E2u + (uint32_t)((int32_t)(int16_t)ctx->d[1]))); set_b(&ctx->d[1], _v); cc_logic8(ctx, _v); hle_insn(ctx, 14); } /* 00F8D2 moveb %pc@(0xf8e2,%d1:w),%d1 */
	{ uint32_t _s = 0x23Cu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00F8D6 addiw #572,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00F8DA jsr %pc@(0x13228) */
L_00F8DE:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00F8DE braw 0xb3be */
}

/* $00F8EA $0F equipment destroyed: tile rect, debris/sparks */
HLE_RANGE(0x00F8EA, L56EquipKill, 36600, EXIT_NEXT)
{
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)26)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00F8EA movew %fp@(26),%d0 */
	{ uint32_t _s = 0x17u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 00F8EE cmpiw #23,%d0 */
	if (bs(ctx, !c_ge(ctx))) goto L_00F908; /* 00F8F2 blts 0xf908 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o & 0x7F); hle_insn(ctx, 16); } /* 00F8F4 bclr #15,%fp@ */
	{ uint32_t _v = 0xF8A2u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00F8F8 movel #63650,%fp@(32) */
	{ cc_logic16(ctx, ld16((ctx->a[6] + (uint32_t)(int32_t)40))); hle_insn(ctx, 12); } /* 00F900 tstw %fp@(40) */
	if (bw(ctx, !c_z(ctx))) goto L_00F952; /* 00F904 bnew 0xf952 */
L_00F908:;
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 00F908 clrw %fp@ */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DAu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 00F90A subqw #1,0xffff80da */
	{ sh_w(ctx, 0, 'l', 2); hle_insn(ctx, 10); } /* 00F910 aslw #2,%d0 */
	{ ctx->a[1] = 0x00FAEEu; hle_insn(ctx, 8); } /* 00F912 lea %pc@(0xfaee),%a1 */
	{ uint32_t _v = ld16((ctx->a[1] + (uint32_t)(int32_t)0 + (uint32_t)((int32_t)(int16_t)ctx->d[0]))); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 14); } /* 00F916 movew %a1@(0,%d0:w),%d2 */
	{ uint32_t _v = ld16((ctx->a[1] + (uint32_t)(int32_t)2 + (uint32_t)((int32_t)(int16_t)ctx->d[0]))); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 14); } /* 00F91A movew %a1@(2,%d0:w),%d3 */
	{ uint32_t _v = (ctx->d[7] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00F91E movew %d7,%sp@- */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)10)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00F920 movew %fp@(10),%d1 */
	{ uint32_t _v = ld32((ctx->a[6] + (uint32_t)(int32_t)22)); ctx->a[0] = _v; hle_insn(ctx, 16); } /* 00F924 moveal %fp@(22),%a0 */
	{ uint32_t _v = 0x0u; set_w(&ctx->d[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00F928 movew #0,%d6 */
	{ uint32_t _v = ld16(0xFFFF9E38u); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00F92C movew 0xffff9e38,%d7 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)38)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00F930 movew %fp@(38),%d0 */
	if (bs(ctx, !c_n(ctx))) goto L_00F94C; /* 00F934 bpls 0xf94c */
	{ uint32_t _v = 0x8000u; set_w(&ctx->d[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00F936 movew #-32768,%d6 */
	{ uint32_t _s = 0x6u; uint32_t _da = 0xFFFF80CCu; uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 20); } /* 00F93A cmpiw #6,0xffff80cc */
	if (bs(ctx, c_z(ctx))) goto L_00F948; /* 00F942 beqs 0xf948 */
	{ uint32_t _v = 0xE000u; set_w(&ctx->d[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00F944 movew #-8192,%d6 */
L_00F948:;
	{ ctx->a[0] = (ctx->a[0] + (uint32_t)(int32_t)-560); hle_insn(ctx, 8); } /* 00F948 lea %a0@(-560),%a0 */
L_00F94C:;
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x012234); } /* 00F94C jsr %pc@(0x12234) */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00F950 movew %sp@+,%d7 */
L_00F952:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)38)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00F952 movew %fp@(38),%d0 */
	{ uint32_t _s = 0x7u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 00F956 cmpiw #7,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_00F96A; /* 00F95A bnes 0xf96a */
	{ hle_insn(ctx, 18); Random(ctx); } /* 00F95C jsr %pc@(0x881c) */
	{ uint32_t _s = 0x3u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 00F960 andiw #3,%d0 */
	if (bs(ctx, c_z(ctx))) goto L_00F970; /* 00F964 beqs 0xf970 */
	hle_insn(ctx, 10); goto L_00FA86; /* 00F966 braw 0xfa86 */
L_00F96A:;
	{ uint32_t _s = 0x3u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 00F96A cmpiw #3,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_00F9AA; /* 00F96E bnes 0xf9aa */
L_00F970:;
	{ uint32_t _a = (ctx->a[6] + (uint32_t)(int32_t)2); ctx->d[0] = (uint32_t)(int32_t)(int16_t)ld16(_a + 0); ctx->d[1] = (uint32_t)(int32_t)(int16_t)ld16(_a + 2); hle_insn(ctx, 24); } /* 00F970 movemw %fp@(2),%d0-%d1 */
	{ uint32_t _s = 0x7u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 00F976 addqw #7,%d0 */
	{ uint32_t _s = 0x10u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00F978 addiw #16,%d1 */
	{ ctx->d[2] = 0xBu; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 00F97C moveq #11,%d2 */
	{ uint32_t _v = ctx->a[6]; push32(ctx, _v); cc_logic32(ctx, _v); hle_insn(ctx, 12); } /* 00F97E movel %fp,%sp@- */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)6)); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00F980 movew %fp@(6),%d3 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x012A46); } /* 00F984 jsr %pc@(0x12a46) */
	{ uint32_t _v = 0x5u; st8((ctx->a[6] + (uint32_t)(int32_t)18), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 00F988 moveb #5,%fp@(18) */
	{ uint32_t _v = 0x78u; st8((ctx->a[6] + (uint32_t)(int32_t)12), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 00F98E moveb #120,%fp@(12) */
	{ uint32_t _v = 0xFFu; st8((ctx->a[6] + (uint32_t)(int32_t)15), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 00F994 moveb #-1,%fp@(15) */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 4) & 1) ? 0 : CC_Z); st8(_a, _o | 0x10); hle_insn(ctx, 16); } /* 00F99A bset #4,%fp@ */
	{ uint32_t _v = 0xA0u; st16((ctx->a[6] + (uint32_t)(int32_t)24), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00F99E movew #160,%fp@(24) */
	{ uint32_t _v = pop32(ctx); ctx->a[6] = _v; hle_insn(ctx, 12); } /* 00F9A4 moveal %sp@+,%fp */
	hle_insn(ctx, 10); goto L_00FA86; /* 00F9A6 braw 0xfa86 */
L_00F9AA:;
	{ uint32_t _s = 0xFFFEu; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 00F9AA cmpiw #-2,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_00F9F2; /* 00F9AE bnes 0xf9f2 */
	{ hle_insn(ctx, 18); Random(ctx); } /* 00F9B0 jsr %pc@(0x881c) */
	{ cc_logic16(ctx, (ctx->d[0] & 0xFFFFu)); hle_insn(ctx, 4); } /* 00F9B4 tstw %d0 */
	if (bs(ctx, !c_n(ctx))) goto L_00F9F2; /* 00F9B6 bpls 0xf9f2 */
	{ uint32_t _a = (ctx->a[6] + (uint32_t)(int32_t)2); ctx->d[0] = (uint32_t)(int32_t)(int16_t)ld16(_a + 0); ctx->d[1] = (uint32_t)(int32_t)(int16_t)ld16(_a + 2); hle_insn(ctx, 24); } /* 00F9B8 movemw %fp@(2),%d0-%d1 */
	{ uint32_t _s = 0x7u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 00F9BE addqw #7,%d0 */
	{ uint32_t _s = 0x10u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00F9C0 addiw #16,%d1 */
	{ ctx->d[2] = 0x7u; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 00F9C4 moveq #7,%d2 */
	{ uint32_t _v = ctx->a[6]; push32(ctx, _v); cc_logic32(ctx, _v); hle_insn(ctx, 12); } /* 00F9C6 movel %fp,%sp@- */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)6)); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00F9C8 movew %fp@(6),%d3 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x012A46); } /* 00F9CC jsr %pc@(0x12a46) */
	{ uint32_t _v = 0x4u; st8((ctx->a[6] + (uint32_t)(int32_t)18), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 00F9D0 moveb #4,%fp@(18) */
	{ uint32_t _v = 0xB4u; st8((ctx->a[6] + (uint32_t)(int32_t)12), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 00F9D6 moveb #-76,%fp@(12) */
	{ uint32_t _v = 0xFFu; st8((ctx->a[6] + (uint32_t)(int32_t)15), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 00F9DC moveb #-1,%fp@(15) */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 4) & 1) ? 0 : CC_Z); st8(_a, _o | 0x10); hle_insn(ctx, 16); } /* 00F9E2 bset #4,%fp@ */
	{ uint32_t _v = 0xF0u; st16((ctx->a[6] + (uint32_t)(int32_t)24), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00F9E6 movew #240,%fp@(24) */
	{ uint32_t _v = pop32(ctx); ctx->a[6] = _v; hle_insn(ctx, 12); } /* 00F9EC moveal %sp@+,%fp */
	hle_insn(ctx, 10); goto L_00FA86; /* 00F9EE braw 0xfa86 */
L_00F9F2:;
	{ uint32_t _s = 0x4u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 00F9F2 cmpiw #4,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_00FA32; /* 00F9F6 bnes 0xfa32 */
	{ uint32_t _a = (ctx->a[6] + (uint32_t)(int32_t)2); ctx->d[0] = (uint32_t)(int32_t)(int16_t)ld16(_a + 0); ctx->d[1] = (uint32_t)(int32_t)(int16_t)ld16(_a + 2); hle_insn(ctx, 24); } /* 00F9F8 movemw %fp@(2),%d0-%d1 */
	{ uint32_t _s = 0x10u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 00F9FE addiw #16,%d0 */
	{ uint32_t _s = 0x18u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00FA02 addiw #24,%d1 */
	{ ctx->d[2] = 0xAu; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 00FA06 moveq #10,%d2 */
	{ uint32_t _v = ctx->a[6]; push32(ctx, _v); cc_logic32(ctx, _v); hle_insn(ctx, 12); } /* 00FA08 movel %fp,%sp@- */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)6)); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00FA0A movew %fp@(6),%d3 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x012A46); } /* 00FA0E jsr %pc@(0x12a46) */
	{ uint32_t _v = 0x5u; st8((ctx->a[6] + (uint32_t)(int32_t)18), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 00FA12 moveb #5,%fp@(18) */
	{ uint32_t _v = 0x78u; st8((ctx->a[6] + (uint32_t)(int32_t)12), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 00FA18 moveb #120,%fp@(12) */
	{ uint32_t _v = 0xFFu; st8((ctx->a[6] + (uint32_t)(int32_t)15), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 00FA1E moveb #-1,%fp@(15) */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 4) & 1) ? 0 : CC_Z); st8(_a, _o | 0x10); hle_insn(ctx, 16); } /* 00FA24 bset #4,%fp@ */
	{ uint32_t _v = 0xA0u; st16((ctx->a[6] + (uint32_t)(int32_t)24), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00FA28 movew #160,%fp@(24) */
	{ uint32_t _v = pop32(ctx); ctx->a[6] = _v; hle_insn(ctx, 12); } /* 00FA2E moveal %sp@+,%fp */
	hle_insn(ctx, 10); goto L_00FA86; /* 00FA30 bras 0xfa86 */
L_00FA32:;
	{ uint32_t _s = 0x5u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 00FA32 cmpiw #5,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_00FA56; /* 00FA36 bnes 0xfa56 */
	{ uint32_t _a = (ctx->a[6] + (uint32_t)(int32_t)2); ctx->d[0] = (uint32_t)(int32_t)(int16_t)ld16(_a + 0); ctx->d[1] = (uint32_t)(int32_t)(int16_t)ld16(_a + 2); hle_insn(ctx, 24); } /* 00FA38 movemw %fp@(2),%d0-%d1 */
	{ uint32_t _s = 0xBu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 00FA3E addiw #11,%d0 */
	{ uint32_t _s = 0x20u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00FA42 addiw #32,%d1 */
	{ ctx->d[2] = 0xBu; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 00FA46 moveq #11,%d2 */
	{ uint32_t _v = ctx->a[6]; push32(ctx, _v); cc_logic32(ctx, _v); hle_insn(ctx, 12); } /* 00FA48 movel %fp,%sp@- */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)6)); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00FA4A movew %fp@(6),%d3 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x012A46); } /* 00FA4E jsr %pc@(0x12a46) */
	{ uint32_t _v = pop32(ctx); ctx->a[6] = _v; hle_insn(ctx, 12); } /* 00FA52 moveal %sp@+,%fp */
	hle_insn(ctx, 10); goto L_00FA86; /* 00FA54 bras 0xfa86 */
L_00FA56:;
	{ uint32_t _s = 0x6u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 00FA56 cmpiw #6,%d0 */
	if (bs(ctx, c_z(ctx))) goto L_00FA62; /* 00FA5A beqs 0xfa62 */
	{ uint32_t _s = 0xFFFAu; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 00FA5C cmpiw #-6,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_00FA86; /* 00FA60 bnes 0xfa86 */
L_00FA62:;
	{ cc_logic16(ctx, ld16(0xFFFF9EC8u)); hle_insn(ctx, 16); } /* 00FA62 tstw 0xffff9ec8 */
	if (bs(ctx, c_z(ctx))) goto L_00FA86; /* 00FA68 beqs 0xfa86 */
	{ uint32_t _a = (ctx->a[6] + (uint32_t)(int32_t)2); ctx->d[0] = (uint32_t)(int32_t)(int16_t)ld16(_a + 0); ctx->d[1] = (uint32_t)(int32_t)(int16_t)ld16(_a + 2); hle_insn(ctx, 24); } /* 00FA6A movemw %fp@(2),%d0-%d1 */
	{ uint32_t _s = 0xCu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 00FA70 addiw #12,%d0 */
	{ uint32_t _s = 0x10u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00FA74 addiw #16,%d1 */
	{ ctx->d[2] = 0xCu; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 00FA78 moveq #12,%d2 */
	{ uint32_t _v = ctx->a[6]; push32(ctx, _v); cc_logic32(ctx, _v); hle_insn(ctx, 12); } /* 00FA7A movel %fp,%sp@- */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)6)); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00FA7C movew %fp@(6),%d3 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x012A46); } /* 00FA80 jsr %pc@(0x12a46) */
	{ uint32_t _v = pop32(ctx); ctx->a[6] = _v; hle_insn(ctx, 12); } /* 00FA84 moveal %sp@+,%fp */
L_00FA86:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)40)); set_w(&ctx->d[5], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00FA86 movew %fp@(40),%d5 */
	{ cc_set(ctx, CC_Z, ((ctx->d[5] >> 0) & 1) ? 0 : CC_Z); hle_insn(ctx, 10); } /* 00FA8A btst #0,%d5 */
	if (bs(ctx, c_z(ctx))) goto L_00FAA6; /* 00FA8E beqs 0xfaa6 */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFFA0A8u; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 00FA90 subqw #1,0xffffa0a8 */
	if (bs(ctx, !c_n(ctx))) goto L_00FA9E; /* 00FA96 bpls 0xfa9e */
	{ st16(0xFFFFA0A8u, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 20); } /* 00FA98 clrw 0xffffa0a8 */
L_00FA9E:;
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x0092A8); } /* 00FA9E jsr %pc@(0x92a8) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)40)); set_w(&ctx->d[5], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00FAA2 movew %fp@(40),%d5 */
L_00FAA6:;
	{ cc_set(ctx, CC_Z, ((ctx->d[5] >> 1) & 1) ? 0 : CC_Z); hle_insn(ctx, 10); } /* 00FAA6 btst #1,%d5 */
	if (bs(ctx, c_z(ctx))) goto L_00FAC6; /* 00FAAA beqs 0xfac6 */
	{ uint32_t _v = 0x9u; set_w(&ctx->d[4], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00FAAC movew #9,%d4 */
L_00FAB0:;
	{ uint32_t _a = 0xFFFF9E94u; ctx->d[0] = (uint32_t)(int32_t)(int16_t)ld16(_a + 0); ctx->d[1] = (uint32_t)(int32_t)(int16_t)ld16(_a + 2); hle_insn(ctx, 28); } /* 00FAB0 movemw 0xffff9e94,%d0-%d1 */
	{ ctx->a[2] = 0x012866u; hle_insn(ctx, 8); } /* 00FAB8 lea %pc@(0x12866),%a2 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x01277C); } /* 00FABC bsrw 0x1277c */
	{ uint16_t _c = (uint16_t)ctx->d[4] - 1; set_w(&ctx->d[4], _c); if (_c != 0xFFFF) { hle_insn(ctx, 10); goto L_00FAB0; } hle_insn(ctx, 14); } /* 00FAC0 dbf %d4,0xfab0 */
	hle_insn(ctx, 10); goto L_00FAE6; /* 00FAC4 bras 0xfae6 */
L_00FAC6:;
	{ uint32_t _s = 0x17u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)26); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 00FAC6 cmpiw #23,%fp@(26) */
	if (bs(ctx, c_ge(ctx))) goto L_00FAE6; /* 00FACC bges 0xfae6 */
	{ ctx->d[4] = 0x3u; cc_logic32(ctx, ctx->d[4]); hle_insn(ctx, 4); } /* 00FACE moveq #3,%d4 */
L_00FAD0:;
	{ uint32_t _a = 0xFFFF9E94u; ctx->d[0] = (uint32_t)(int32_t)(int16_t)ld16(_a + 0); ctx->d[1] = (uint32_t)(int32_t)(int16_t)ld16(_a + 2); hle_insn(ctx, 28); } /* 00FAD0 movemw 0xffff9e94,%d0-%d1 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)6)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00FAD8 movew %fp@(6),%d2 */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_call68k(ctx, 0x0124EC); } /* 00FADC jsr 0x124ec */
	{ uint16_t _c = (uint16_t)ctx->d[4] - 1; set_w(&ctx->d[4], _c); if (_c != 0xFFFF) { hle_insn(ctx, 10); goto L_00FAD0; } hle_insn(ctx, 14); } /* 00FAE2 dbf %d4,0xfad0 */
L_00FAE6:;
	{ st16((ctx->a[6] + (uint32_t)(int32_t)40), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 00FAE6 clrw %fp@(40) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00FAEA braw 0xb3be */
}

/* $00FB52 $20 fixture shot: next state $00FB5A (falls into it) */
HLE_RANGE_PURE(0x00FB52, L56LampSet, 1000, EXIT_NEXT)
{
	{ uint32_t _v = 0xFB5Au; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00FB52 movel #64346,%fp@(32) */
	{ uint32_t _v = 0x23Bu; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00FB5A movew #571,%d1 */
	hle_insn(ctx, 10); goto L_00FB64; /* 00FB5E bras 0xfb64 */
L_00FB64:;
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 00FB64 jsr %pc@(0x12316) */
	{ ctx->d[0] = 0xFFFFFFE0u; cc_logic32(ctx, ctx->d[0]); hle_insn(ctx, 4); } /* 00FB68 moveq #-32,%d0 */
	{ hle_insn(ctx, 18); ObjKillIfLeftOf(ctx); } /* 00FB6A jsr %pc@(0x12332) */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00FB6E bsrw 0x13228 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00FB72 braw 0xb3be */
}

/* $00FB5A $20 fixture, second frame */
HLE_RANGE_PURE(0x00FB5A, L56Lamp2, 1000, EXIT_NEXT)
{
	{ uint32_t _v = 0x23Bu; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00FB5A movew #571,%d1 */
	hle_insn(ctx, 10); goto L_00FB64; /* 00FB5E bras 0xfb64 */
L_00FB64:;
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 00FB64 jsr %pc@(0x12316) */
	{ ctx->d[0] = 0xFFFFFFE0u; cc_logic32(ctx, ctx->d[0]); hle_insn(ctx, 4); } /* 00FB68 moveq #-32,%d0 */
	{ hle_insn(ctx, 18); ObjKillIfLeftOf(ctx); } /* 00FB6A jsr %pc@(0x12332) */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00FB6E bsrw 0x13228 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00FB72 braw 0xb3be */
}

/* $00FB60 $20 ceiling fixture */
HLE_RANGE_PURE(0x00FB60, L56Lamp, 1000, EXIT_NEXT)
{
	{ uint32_t _v = 0x23Au; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00FB60 movew #570,%d1 */
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 00FB64 jsr %pc@(0x12316) */
	{ ctx->d[0] = 0xFFFFFFE0u; cc_logic32(ctx, ctx->d[0]); hle_insn(ctx, 4); } /* 00FB68 moveq #-32,%d0 */
	{ hle_insn(ctx, 18); ObjKillIfLeftOf(ctx); } /* 00FB6A jsr %pc@(0x12332) */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00FB6E bsrw 0x13228 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00FB72 braw 0xb3be */
}

/* $00FBAA $27 endoskeleton walking in, frame from the counter */
HLE_RANGE_PURE(0x00FBAA, L56EndoWalk, 1000, EXIT_NEXT)
{
	{ hle_insn(ctx, 18); ObjMoveX(ctx); } /* 00FBAA jsr %pc@(0xb4a6) */
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 00FBAE jsr %pc@(0x12316) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[5], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00FBB2 movew %fp@(36),%d5 */
	{ uint32_t _s = 0x38u; uint32_t _d = (ctx->d[5] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[5], _r); hle_insn(ctx, 8); } /* 00FBB6 andiw #56,%d5 */
	{ sh_w(ctx, 5, 'r', 3); hle_insn(ctx, 12); } /* 00FBBA lsrw #3,%d5 */
	{ uint32_t _s = 0x7u; uint32_t _d = (ctx->d[5] & 0xFFFFu); uint32_t _r = (_d ^ _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[5], _r); hle_insn(ctx, 8); } /* 00FBBC eoriw #7,%d5 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 6) & 1) ? 0 : CC_Z); hle_insn(ctx, 12); } /* 00FBC0 btst #6,%fp@ */
	if (bw(ctx, c_z(ctx))) goto L_00FBD4; /* 00FBC4 beqw 0xfbd4 */
	{ uint32_t _v = (ctx->d[5] & 0xFFFFu); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 00FBC8 movew %d5,%d1 */
	{ uint32_t _s = 0x259u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00FBCA addiw #601,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00FBCE bsrw 0x13228 */
	hle_insn(ctx, 10); goto L_00FBDE; /* 00FBD2 bras 0xfbde */
L_00FBD4:;
	{ uint32_t _v = (ctx->d[5] & 0xFFFFu); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 00FBD4 movew %d5,%d1 */
	{ uint32_t _s = 0x261u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00FBD6 addiw #609,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00FBDA bsrw 0x13228 */
L_00FBDE:;
	{ ctx->d[0] = 0xFFFFFFD8u; cc_logic32(ctx, ctx->d[0]); hle_insn(ctx, 4); } /* 00FBDE moveq #-40,%d0 */
	{ hle_insn(ctx, 18); ObjKillIfLeftOf(ctx); } /* 00FBE0 jsr %pc@(0x12332) */
	{ uint32_t _v = 0x168u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00FBE4 movew #360,%d0 */
	{ hle_insn(ctx, 18); ObjKillIfRightOf(ctx); } /* 00FBE8 jsr %pc@(0x12342) */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00FBEC subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_00FBFA; /* 00FBF0 bnes 0xfbfa */
	{ uint32_t _v = 0xFE8Eu; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00FBF2 movel #65166,%fp@(32) */
L_00FBFA:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00FBFA braw 0xb3be */
}

/* $00FBFE $27 endoskeleton walk (table frame) */
HLE_RANGE_PURE(0x00FBFE, L56EndoWalkB, 1000, EXIT_NEXT)
{
	{ hle_insn(ctx, 18); ObjMoveX(ctx); } /* 00FBFE jsr %pc@(0xb4a6) */
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 00FC02 jsr %pc@(0x12316) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[5], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00FC06 movew %fp@(36),%d5 */
	{ uint32_t _s = 0x38u; uint32_t _d = (ctx->d[5] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[5], _r); hle_insn(ctx, 8); } /* 00FC0A andiw #56,%d5 */
	{ sh_w(ctx, 5, 'r', 3); hle_insn(ctx, 12); } /* 00FC0E lsrw #3,%d5 */
	{ uint32_t _s = 0x7u; uint32_t _d = (ctx->d[5] & 0xFFFFu); uint32_t _r = (_d ^ _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[5], _r); hle_insn(ctx, 8); } /* 00FC10 eoriw #7,%d5 */
	{ uint32_t _v = ld8((0x00FC1Cu + (uint32_t)((int32_t)(int16_t)ctx->d[5]))); set_b(&ctx->d[5], _v); cc_logic8(ctx, _v); hle_insn(ctx, 14); } /* 00FC14 moveb %pc@(0xfc1c,%d5:w),%d5 */
	hle_insn(ctx, 10); goto L_00FC3A; /* 00FC18 braw 0xfc3a */
L_00FC3A:;
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 6) & 1) ? 0 : CC_Z); hle_insn(ctx, 12); } /* 00FC3A btst #6,%fp@ */
	if (bw(ctx, c_z(ctx))) goto L_00FC4E; /* 00FC3E beqw 0xfc4e */
	{ uint32_t _v = (ctx->d[5] & 0xFFFFu); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 00FC42 movew %d5,%d1 */
	{ uint32_t _s = 0x33Au; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00FC44 addiw #826,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00FC48 bsrw 0x13228 */
	hle_insn(ctx, 10); goto L_00FC58; /* 00FC4C bras 0xfc58 */
L_00FC4E:;
	{ uint32_t _v = (ctx->d[5] & 0xFFFFu); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 00FC4E movew %d5,%d1 */
	{ uint32_t _s = 0x332u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00FC50 addiw #818,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00FC54 bsrw 0x13228 */
L_00FC58:;
	{ ctx->d[0] = 0xFFFFFFD8u; cc_logic32(ctx, ctx->d[0]); hle_insn(ctx, 4); } /* 00FC58 moveq #-40,%d0 */
	{ hle_insn(ctx, 18); ObjKillIfLeftOf(ctx); } /* 00FC5A jsr %pc@(0x12332) */
	{ uint32_t _v = 0x168u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00FC5E movew #360,%d0 */
	{ hle_insn(ctx, 18); ObjKillIfRightOf(ctx); } /* 00FC62 jsr %pc@(0x12342) */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00FC66 subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_00FC74; /* 00FC6A bnes 0xfc74 */
	{ uint32_t _v = 0xFE8Eu; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00FC6C movel #65166,%fp@(32) */
L_00FC74:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00FC74 braw 0xb3be */
}

/* $00FC24 $27 endoskeleton walk */
HLE_RANGE_PURE(0x00FC24, L56EndoWalkC, 1000, EXIT_NEXT)
{
	{ hle_insn(ctx, 18); ObjMoveX(ctx); } /* 00FC24 jsr %pc@(0xb4a6) */
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 00FC28 jsr %pc@(0x12316) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[5], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00FC2C movew %fp@(36),%d5 */
	{ uint32_t _s = 0x38u; uint32_t _d = (ctx->d[5] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[5], _r); hle_insn(ctx, 8); } /* 00FC30 andiw #56,%d5 */
	{ sh_w(ctx, 5, 'r', 3); hle_insn(ctx, 12); } /* 00FC34 lsrw #3,%d5 */
	{ uint32_t _s = 0x7u; uint32_t _d = (ctx->d[5] & 0xFFFFu); uint32_t _r = (_d ^ _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[5], _r); hle_insn(ctx, 8); } /* 00FC36 eoriw #7,%d5 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 6) & 1) ? 0 : CC_Z); hle_insn(ctx, 12); } /* 00FC3A btst #6,%fp@ */
	if (bw(ctx, c_z(ctx))) goto L_00FC4E; /* 00FC3E beqw 0xfc4e */
	{ uint32_t _v = (ctx->d[5] & 0xFFFFu); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 00FC42 movew %d5,%d1 */
	{ uint32_t _s = 0x33Au; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00FC44 addiw #826,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00FC48 bsrw 0x13228 */
	hle_insn(ctx, 10); goto L_00FC58; /* 00FC4C bras 0xfc58 */
L_00FC4E:;
	{ uint32_t _v = (ctx->d[5] & 0xFFFFu); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 00FC4E movew %d5,%d1 */
	{ uint32_t _s = 0x332u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00FC50 addiw #818,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00FC54 bsrw 0x13228 */
L_00FC58:;
	{ ctx->d[0] = 0xFFFFFFD8u; cc_logic32(ctx, ctx->d[0]); hle_insn(ctx, 4); } /* 00FC58 moveq #-40,%d0 */
	{ hle_insn(ctx, 18); ObjKillIfLeftOf(ctx); } /* 00FC5A jsr %pc@(0x12332) */
	{ uint32_t _v = 0x168u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00FC5E movew #360,%d0 */
	{ hle_insn(ctx, 18); ObjKillIfRightOf(ctx); } /* 00FC62 jsr %pc@(0x12342) */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00FC66 subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_00FC74; /* 00FC6A bnes 0xfc74 */
	{ uint32_t _v = 0xFE8Eu; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00FC6C movel #65166,%fp@(32) */
L_00FC74:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00FC74 braw 0xb3be */
}

/* $00FC78 $27 endoskeleton standing, firing */
HLE_RANGE(0x00FC78, L56EndoFire, 1800, EXIT_NEXT)
{
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 00FC78 jsr %pc@(0x12316) */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00FC7C subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_00FC8A; /* 00FC80 bnes 0xfc8a */
	{ uint32_t _v = 0xFE8Eu; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00FC82 movel #65166,%fp@(32) */
L_00FC8A:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00FC8A movew %fp@(36),%d0 */
	{ uint32_t _s = 0x50u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 00FC8E cmpiw #80,%d0 */
	if (bs(ctx, c_gt(ctx))) goto L_00FCB6; /* 00FC92 bgts 0xfcb6 */
	{ uint32_t _s = 0x14u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 00FC94 cmpiw #20,%d0 */
	if (bs(ctx, !c_ge(ctx))) goto L_00FCB6; /* 00FC98 blts 0xfcb6 */
	{ cc_set(ctx, CC_Z, ((ctx->d[0] >> 2) & 1) ? 0 : CC_Z); hle_insn(ctx, 10); } /* 00FC9A btst #2,%d0 */
	if (bs(ctx, c_z(ctx))) goto L_00FCB6; /* 00FC9E beqs 0xfcb6 */
	{ uint32_t _v = 0x60u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00FCA0 movew #96,%d0 */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_call68k(ctx, 0x013D04); } /* 00FCA4 jsr 0x13d04 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x009054); } /* 00FCAA bsrw 0x9054 */
	{ uint32_t _v = 0x34Bu; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00FCAE movew #843,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00FCB2 jsr %pc@(0x13228) */
L_00FCB6:;
	{ uint32_t _v = 0x348u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00FCB6 movew #840,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00FCBA jsr %pc@(0x13228) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00FCBE braw 0xb3be */
}

/* $00FCC2 $27 endoskeleton standing, firing (variant) */
HLE_RANGE(0x00FCC2, L56EndoFire2, 1800, EXIT_NEXT)
{
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 00FCC2 jsr %pc@(0x12316) */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00FCC6 subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_00FCD4; /* 00FCCA bnes 0xfcd4 */
	{ uint32_t _v = 0xFE8Eu; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00FCCC movel #65166,%fp@(32) */
L_00FCD4:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00FCD4 movew %fp@(36),%d0 */
	{ uint32_t _s = 0x50u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 00FCD8 cmpiw #80,%d0 */
	if (bs(ctx, c_gt(ctx))) goto L_00FD00; /* 00FCDC bgts 0xfd00 */
	{ uint32_t _s = 0x14u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 00FCDE cmpiw #20,%d0 */
	if (bs(ctx, !c_ge(ctx))) goto L_00FD00; /* 00FCE2 blts 0xfd00 */
	{ cc_set(ctx, CC_Z, ((ctx->d[0] >> 2) & 1) ? 0 : CC_Z); hle_insn(ctx, 10); } /* 00FCE4 btst #2,%d0 */
	if (bs(ctx, c_z(ctx))) goto L_00FD00; /* 00FCE8 beqs 0xfd00 */
	{ uint32_t _v = 0x60u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00FCEA movew #96,%d0 */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_call68k(ctx, 0x013D04); } /* 00FCEE jsr 0x13d04 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x009054); } /* 00FCF4 bsrw 0x9054 */
	{ uint32_t _v = 0x34Cu; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00FCF8 movew #844,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00FCFC jsr %pc@(0x13228) */
L_00FD00:;
	{ uint32_t _v = 0x34Au; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00FD00 movew #842,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00FD04 jsr %pc@(0x13228) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00FD08 braw 0xb3be */
}

/* $00FD40 $01 foot soldier: last frame then free */
HLE_RANGE_PURE(0x00FD40, L56SoldierGone, 1000, EXIT_NEXT)
{
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 00FD40 jsr %pc@(0x12316) */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00FD44 subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_00FD58; /* 00FD48 bnes 0xfd58 */
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 00FD4A clrw %fp@ */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DAu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 00FD4C subqw #1,0xffff80da */
	{ st8(0xFFFF9EBEu, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 20); } /* 00FD52 clrb 0xffff9ebe */
L_00FD58:;
	{ uint32_t _v = 0x301u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00FD58 movew #769,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00FD5C jsr %pc@(0x13228) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00FD60 braw 0xb3be */
}

/* $00FD64 $01 foot soldier: shot (init, falls into $00FD98) */
HLE_RANGE_PURE(0x00FD64, L56SoldierInit, 1000, EXIT_NEXT)
{
	goto L_00FD64;
L_00FD26:;
	{ uint32_t _v = 0x28u; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00FD26 movew #40,%fp@(36) */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 3) & 1) ? 0 : CC_Z); hle_insn(ctx, 12); } /* 00FD2C btst #11,%fp@ */
	if (bs(ctx, c_z(ctx))) goto L_00FD38; /* 00FD30 beqs 0xfd38 */
	{ uint32_t _v = 0x1u; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00FD32 movew #1,%fp@(36) */
L_00FD38:;
	{ uint32_t _v = 0xFD40u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00FD38 movel #64832,%fp@(32) */
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 00FD40 jsr %pc@(0x12316) */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00FD44 subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_00FD58; /* 00FD48 bnes 0xfd58 */
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 00FD4A clrw %fp@ */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DAu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 00FD4C subqw #1,0xffff80da */
	{ st8(0xFFFF9EBEu, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 20); } /* 00FD52 clrb 0xffff9ebe */
L_00FD58:;
	{ uint32_t _v = 0x301u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00FD58 movew #769,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00FD5C jsr %pc@(0x13228) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00FD60 braw 0xb3be */
L_00FD64:;
	{ st8((ctx->a[6] + (uint32_t)(int32_t)13), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 00FD64 clrb %fp@(13) */
	{ uint32_t _v = 0x8u; st8((ctx->a[6] + (uint32_t)(int32_t)18), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 00FD68 moveb #8,%fp@(18) */
	{ uint32_t _v = 0xC8u; st8((ctx->a[6] + (uint32_t)(int32_t)12), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 00FD6E moveb #-56,%fp@(12) */
	{ uint32_t _v = 0xFFu; st8((ctx->a[6] + (uint32_t)(int32_t)15), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 00FD74 moveb #-1,%fp@(15) */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o & 0x7F); hle_insn(ctx, 16); } /* 00FD7A bclr #15,%fp@ */
	{ uint32_t _v = 0x1u; st8((ctx->a[6] + (uint32_t)(int32_t)1), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 00FD7E moveb #1,%fp@(1) */
	{ uint32_t _s = 0xAu; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)7); uint32_t _d = ld8(_da); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 8, 1); st8(_da, _r); hle_insn(ctx, 20); } /* 00FD84 addib #10,%fp@(7) */
	{ uint32_t _v = 0x30u; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00FD8A movew #48,%fp@(36) */
	{ uint32_t _v = 0xFD98u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00FD90 movel #64920,%fp@(32) */
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 00FD98 jsr %pc@(0x12316) */
	{ hle_insn(ctx, 18); sub_00D236(ctx); } /* 00FD9C jsr %pc@(0xd236) */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00FDA0 subqw #1,%fp@(36) */
	if (bs(ctx, !c_n(ctx))) goto L_00FDB6; /* 00FDA4 bpls 0xfdb6 */
	{ ctx->d[1] = 0x5u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 00FDA6 moveq #5,%d1 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00FDA8 movew %fp@(4),%d0 */
	{ uint32_t _s = ld16((ctx->a[6] + (uint32_t)(int32_t)40)); uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 12); } /* 00FDAC cmpw %fp@(40),%d0 */
	if (bw(ctx, c_gt(ctx))) goto L_00FD26; /* 00FDB0 bgtw 0xfd26 */
	hle_insn(ctx, 10); goto L_00FDC6; /* 00FDB4 bras 0xfdc6 */
L_00FDB6:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00FDB6 movew %fp@(36),%d1 */
	{ sh_w(ctx, 1, 'a', 1); hle_insn(ctx, 8); } /* 00FDBA asrw #1,%d1 */
	{ uint32_t _s = 0x17u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00FDBC subiw #23,%d1 */
	{ uint32_t _r = alu(ctx, OP_SUB, 0, ctx->d[1], 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 4); } /* 00FDC0 negw %d1 */
	{ uint32_t _v = ld8((0x00FE0Cu + (uint32_t)((int32_t)(int16_t)ctx->d[1]))); set_b(&ctx->d[1], _v); cc_logic8(ctx, _v); hle_insn(ctx, 14); } /* 00FDC2 moveb %pc@(0xfe0c,%d1:w),%d1 */
L_00FDC6:;
	{ uint32_t _v = ld32(0xFFFF80BCu); ctx->a[5] = _v; hle_insn(ctx, 20); } /* 00FDC6 moveal 0xffff80bc,%a5 */
	{ uint32_t _v = 0x1u; st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00FDCC movew #1,%a5@+ */
	{ uint32_t _v = (ctx->d[1] & 0xFFFFu); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 00FDD0 movew %d1,%d0 */
	{ sh_w(ctx, 0, 'l', 2); hle_insn(ctx, 10); } /* 00FDD2 aslw #2,%d0 */
	{ ctx->a[1] = 0x00FE24u; hle_insn(ctx, 8); } /* 00FDD4 lea %pc@(0xfe24),%a1 */
	{ uint32_t _v = ld16((ctx->a[1] + (uint32_t)(int32_t)0 + (uint32_t)((int32_t)(int16_t)ctx->d[0]))); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 14); } /* 00FDD8 movew %a1@(0,%d0:w),%d2 */
	{ sh_w(ctx, 2, 'a', 1); hle_insn(ctx, 8); } /* 00FDDC asrw #1,%d2 */
	{ uint32_t _v = (ctx->d[2] & 0xFFFFu); st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00FDDE movew %d2,%a5@+ */
	{ uint32_t _v = ld16((ctx->a[1] + (uint32_t)(int32_t)2 + (uint32_t)((int32_t)(int16_t)ctx->d[0]))); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 14); } /* 00FDE0 movew %a1@(2,%d0:w),%d0 */
	{ ctx->a[1] = 0x000AFA60u; hle_insn(ctx, 12); } /* 00FDE4 lea 0xafa60,%a1 */
	{ ctx->a[1] = (ctx->a[1] + (uint32_t)(int32_t)0 + (uint32_t)((int32_t)(int16_t)ctx->d[0])); hle_insn(ctx, 12); } /* 00FDEA lea %a1@(0,%d0:w),%a1 */
	{ uint32_t _v = ctx->a[1]; st32(ctx->a[5], _v); ctx->a[5] += 4; cc_logic32(ctx, _v); hle_insn(ctx, 12); } /* 00FDEE movel %a1,%a5@+ */
	{ uint32_t _v = ld16(0xFFFF9E58u); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00FDF0 movew 0xffff9e58,%d0 */
	{ sh_w(ctx, 0, 'l', 5); hle_insn(ctx, 16); } /* 00FDF6 aslw #5,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00FDF8 movew %d0,%a5@+ */
	{ uint32_t _v = ctx->a[5]; st32(0xFFFF80BCu, _v); cc_logic32(ctx, _v); hle_insn(ctx, 20); } /* 00FDFA movel %a5,0xffff80bc */
	{ uint32_t _s = 0x2FCu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00FE00 addiw #764,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00FE04 jsr %pc@(0x13228) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00FE08 braw 0xb3be */
}

/* $00FD98 $01 foot soldier: fall animation */
HLE_RANGE_PURE(0x00FD98, L56Soldier, 1000, EXIT_NEXT)
{
	goto L_00FD98;
L_00FD26:;
	{ uint32_t _v = 0x28u; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00FD26 movew #40,%fp@(36) */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 3) & 1) ? 0 : CC_Z); hle_insn(ctx, 12); } /* 00FD2C btst #11,%fp@ */
	if (bs(ctx, c_z(ctx))) goto L_00FD38; /* 00FD30 beqs 0xfd38 */
	{ uint32_t _v = 0x1u; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00FD32 movew #1,%fp@(36) */
L_00FD38:;
	{ uint32_t _v = 0xFD40u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00FD38 movel #64832,%fp@(32) */
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 00FD40 jsr %pc@(0x12316) */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00FD44 subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_00FD58; /* 00FD48 bnes 0xfd58 */
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 00FD4A clrw %fp@ */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DAu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 00FD4C subqw #1,0xffff80da */
	{ st8(0xFFFF9EBEu, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 20); } /* 00FD52 clrb 0xffff9ebe */
L_00FD58:;
	{ uint32_t _v = 0x301u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00FD58 movew #769,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00FD5C jsr %pc@(0x13228) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00FD60 braw 0xb3be */
L_00FD98:;
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 00FD98 jsr %pc@(0x12316) */
	{ hle_insn(ctx, 18); sub_00D236(ctx); } /* 00FD9C jsr %pc@(0xd236) */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00FDA0 subqw #1,%fp@(36) */
	if (bs(ctx, !c_n(ctx))) goto L_00FDB6; /* 00FDA4 bpls 0xfdb6 */
	{ ctx->d[1] = 0x5u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 00FDA6 moveq #5,%d1 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00FDA8 movew %fp@(4),%d0 */
	{ uint32_t _s = ld16((ctx->a[6] + (uint32_t)(int32_t)40)); uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 12); } /* 00FDAC cmpw %fp@(40),%d0 */
	if (bw(ctx, c_gt(ctx))) goto L_00FD26; /* 00FDB0 bgtw 0xfd26 */
	hle_insn(ctx, 10); goto L_00FDC6; /* 00FDB4 bras 0xfdc6 */
L_00FDB6:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00FDB6 movew %fp@(36),%d1 */
	{ sh_w(ctx, 1, 'a', 1); hle_insn(ctx, 8); } /* 00FDBA asrw #1,%d1 */
	{ uint32_t _s = 0x17u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00FDBC subiw #23,%d1 */
	{ uint32_t _r = alu(ctx, OP_SUB, 0, ctx->d[1], 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 4); } /* 00FDC0 negw %d1 */
	{ uint32_t _v = ld8((0x00FE0Cu + (uint32_t)((int32_t)(int16_t)ctx->d[1]))); set_b(&ctx->d[1], _v); cc_logic8(ctx, _v); hle_insn(ctx, 14); } /* 00FDC2 moveb %pc@(0xfe0c,%d1:w),%d1 */
L_00FDC6:;
	{ uint32_t _v = ld32(0xFFFF80BCu); ctx->a[5] = _v; hle_insn(ctx, 20); } /* 00FDC6 moveal 0xffff80bc,%a5 */
	{ uint32_t _v = 0x1u; st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00FDCC movew #1,%a5@+ */
	{ uint32_t _v = (ctx->d[1] & 0xFFFFu); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 00FDD0 movew %d1,%d0 */
	{ sh_w(ctx, 0, 'l', 2); hle_insn(ctx, 10); } /* 00FDD2 aslw #2,%d0 */
	{ ctx->a[1] = 0x00FE24u; hle_insn(ctx, 8); } /* 00FDD4 lea %pc@(0xfe24),%a1 */
	{ uint32_t _v = ld16((ctx->a[1] + (uint32_t)(int32_t)0 + (uint32_t)((int32_t)(int16_t)ctx->d[0]))); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 14); } /* 00FDD8 movew %a1@(0,%d0:w),%d2 */
	{ sh_w(ctx, 2, 'a', 1); hle_insn(ctx, 8); } /* 00FDDC asrw #1,%d2 */
	{ uint32_t _v = (ctx->d[2] & 0xFFFFu); st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00FDDE movew %d2,%a5@+ */
	{ uint32_t _v = ld16((ctx->a[1] + (uint32_t)(int32_t)2 + (uint32_t)((int32_t)(int16_t)ctx->d[0]))); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 14); } /* 00FDE0 movew %a1@(2,%d0:w),%d0 */
	{ ctx->a[1] = 0x000AFA60u; hle_insn(ctx, 12); } /* 00FDE4 lea 0xafa60,%a1 */
	{ ctx->a[1] = (ctx->a[1] + (uint32_t)(int32_t)0 + (uint32_t)((int32_t)(int16_t)ctx->d[0])); hle_insn(ctx, 12); } /* 00FDEA lea %a1@(0,%d0:w),%a1 */
	{ uint32_t _v = ctx->a[1]; st32(ctx->a[5], _v); ctx->a[5] += 4; cc_logic32(ctx, _v); hle_insn(ctx, 12); } /* 00FDEE movel %a1,%a5@+ */
	{ uint32_t _v = ld16(0xFFFF9E58u); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00FDF0 movew 0xffff9e58,%d0 */
	{ sh_w(ctx, 0, 'l', 5); hle_insn(ctx, 16); } /* 00FDF6 aslw #5,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00FDF8 movew %d0,%a5@+ */
	{ uint32_t _v = ctx->a[5]; st32(0xFFFF80BCu, _v); cc_logic32(ctx, _v); hle_insn(ctx, 20); } /* 00FDFA movel %a5,0xffff80bc */
	{ uint32_t _s = 0x2FCu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00FE00 addiw #764,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00FE04 jsr %pc@(0x13228) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00FE08 braw 0xb3be */
}

/* $00FE3C $01 foot soldier: slide to the target X */
HLE_RANGE_PURE(0x00FE3C, L56SoldierMove, 1000, EXIT_NEXT)
{
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00FE3C movew %fp@(4),%d0 */
	{ uint32_t _s = 0x8u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 00FE40 addqw #8,%d0 */
	{ uint32_t _s = ld16((ctx->a[6] + (uint32_t)(int32_t)40)); uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 12); } /* 00FE42 cmpw %fp@(40),%d0 */
	if (bs(ctx, !c_ge(ctx))) goto L_00FE5A; /* 00FE46 blts 0xfe5a */
	{ uint32_t _v = 0xFE6Au; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00FE48 movel #65130,%fp@(32) */
	{ uint32_t _v = 0xFu; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00FE50 movew #15,%fp@(36) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)40)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00FE56 movew %fp@(40),%d0 */
L_00FE5A:;
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)4), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00FE5A movew %d0,%fp@(4) */
	{ uint32_t _v = 0x346u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00FE5E movew #838,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00FE62 bsrw 0x13228 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00FE66 braw 0xb3be */
}

/* $00FE6A $01 foot soldier: pause, then fire */
HLE_RANGE_PURE(0x00FE6A, L56SoldierWait, 1000, EXIT_NEXT)
{
	{ uint32_t _v = 0x347u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00FE6A movew #839,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00FE6E bsrw 0x13228 */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00FE72 subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_00FE8A; /* 00FE76 bnes 0xfe8a */
	{ uint32_t _v = 0xFC78u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00FE78 movel #64632,%fp@(32) */
	{ uint32_t _v = 0x4Bu; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00FE80 movew #75,%fp@(36) */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o | 0x80); hle_insn(ctx, 16); } /* 00FE86 bset #7,%fp@ */
L_00FE8A:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00FE8A braw 0xb3be */
}

/* $00FE8E $27: pick the next state and jump to it */
HLE_RANGE_PURE(0x00FE8E, L56SoldierPick, 1000, 0x00FBAA, 0x00FBFE, 0x00FC24, 0x00FC78, 0x00FCC2)
{
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o | 0x80); hle_insn(ctx, 16); } /* 00FE8E bset #15,%fp@ */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 1) & 1) ? 0 : CC_Z); hle_insn(ctx, 12); } /* 00FE92 btst #9,%fp@ */
	if (bs(ctx, !c_z(ctx))) goto L_00FEE0; /* 00FE96 bnes 0xfee0 */
	{ hle_insn(ctx, 20); Random(ctx); } /* 00FE98 jsr 0x881c */
	{ uint32_t _s = 0xFFu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 00FE9E andiw #255,%d0 */
	{ uint32_t _s = 0x55u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 00FEA2 cmpiw #85,%d0 */
	if (bs(ctx, c_gt(ctx))) goto L_00FEB0; /* 00FEA6 bgts 0xfeb0 */
	{ hle_insn(ctx, 18); sub_00FD0C(ctx); } /* 00FEA8 bsrw 0xfd0c */
	hle_insn(ctx, 10); goto L_00FEDA; /* 00FEAC braw 0xfeda */
L_00FEB0:;
	{ uint32_t _s = 0xAAu; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 00FEB0 cmpiw #170,%d0 */
	if (bs(ctx, c_gt(ctx))) goto L_00FEC8; /* 00FEB4 bgts 0xfec8 */
	{ uint32_t _v = 0x3Fu; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00FEB6 movew #63,%fp@(36) */
	{ uint32_t _v = 0xFC24u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00FEBC movel #64548,%fp@(32) */
	hle_insn(ctx, 10); goto L_00FEDA; /* 00FEC4 braw 0xfeda */
L_00FEC8:;
	{ uint32_t _v = 0x75u; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00FEC8 movew #117,%fp@(36) */
	{ uint32_t _v = 0xFC78u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00FECE movel #64632,%fp@(32) */
	hle_insn(ctx, 10); goto L_00FEDA; /* 00FED6 braw 0xfeda */
L_00FEDA:;
	{ uint32_t _v = ld32((ctx->a[6] + (uint32_t)(int32_t)32)); ctx->a[1] = _v; hle_insn(ctx, 16); } /* 00FEDA moveal %fp@(32),%a1 */
	hle_insn(ctx, 8); hle_jump(ctx, ctx->a[1]); return; /* 00FEDE jmp %a1@ */
L_00FEE0:;
	{ hle_insn(ctx, 20); Random(ctx); } /* 00FEE0 jsr 0x881c */
	{ cc_logic16(ctx, (ctx->d[0] & 0xFFFFu)); hle_insn(ctx, 4); } /* 00FEE6 tstw %d0 */
	if (bs(ctx, c_n(ctx))) goto L_00FF00; /* 00FEE8 bmis 0xff00 */
	{ uint32_t _v = 0x1Du; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00FEEA movew #29,%fp@(36) */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o & 0x7F); hle_insn(ctx, 16); } /* 00FEF0 bclr #15,%fp@ */
	{ uint32_t _v = 0xFBFEu; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00FEF4 movel #64510,%fp@(32) */
	hle_insn(ctx, 10); goto L_00FEDA; /* 00FEFC braw 0xfeda */
L_00FF00:;
	{ uint32_t _v = 0x75u; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00FF00 movew #117,%fp@(36) */
	{ uint32_t _v = 0xFCC2u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00FF06 movel #64706,%fp@(32) */
	hle_insn(ctx, 10); goto L_00FEDA; /* 00FF0E braw 0xfeda */
}

/* $00FF12 $27 endoskeleton walk (level 6) */
HLE_RANGE_PURE(0x00FF12, L56Endo2Walk, 1000, EXIT_NEXT)
{
	{ hle_insn(ctx, 18); ObjMoveX(ctx); } /* 00FF12 jsr %pc@(0xb4a6) */
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 00FF16 jsr %pc@(0x12316) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[5], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00FF1A movew %fp@(36),%d5 */
	{ uint32_t _s = 0x38u; uint32_t _d = (ctx->d[5] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[5], _r); hle_insn(ctx, 8); } /* 00FF1E andiw #56,%d5 */
	{ sh_w(ctx, 5, 'r', 3); hle_insn(ctx, 12); } /* 00FF22 lsrw #3,%d5 */
	{ uint32_t _s = 0x7u; uint32_t _d = (ctx->d[5] & 0xFFFFu); uint32_t _r = (_d ^ _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[5], _r); hle_insn(ctx, 8); } /* 00FF24 eoriw #7,%d5 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 6) & 1) ? 0 : CC_Z); hle_insn(ctx, 12); } /* 00FF28 btst #6,%fp@ */
	if (bw(ctx, c_z(ctx))) goto L_00FF3C; /* 00FF2C beqw 0xff3c */
	{ uint32_t _v = (ctx->d[5] & 0xFFFFu); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 00FF30 movew %d5,%d1 */
	{ uint32_t _s = 0x2E3u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00FF32 addiw #739,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00FF36 bsrw 0x13228 */
	hle_insn(ctx, 10); goto L_00FF46; /* 00FF3A bras 0xff46 */
L_00FF3C:;
	{ uint32_t _v = (ctx->d[5] & 0xFFFFu); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 00FF3C movew %d5,%d1 */
	{ uint32_t _s = 0x2EBu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00FF3E addiw #747,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00FF42 bsrw 0x13228 */
L_00FF46:;
	{ ctx->d[0] = 0xFFFFFFD8u; cc_logic32(ctx, ctx->d[0]); hle_insn(ctx, 4); } /* 00FF46 moveq #-40,%d0 */
	{ hle_insn(ctx, 18); ObjKillIfLeftOf(ctx); } /* 00FF48 jsr %pc@(0x12332) */
	{ uint32_t _v = 0x168u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00FF4C movew #360,%d0 */
	{ hle_insn(ctx, 18); ObjKillIfRightOf(ctx); } /* 00FF50 jsr %pc@(0x12342) */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00FF54 subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_00FF68; /* 00FF58 bnes 0xff68 */
	{ uint32_t _v = 0xFF6Cu; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00FF5A movel #65388,%fp@(32) */
	{ uint32_t _v = 0x40u; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00FF62 movew #64,%fp@(36) */
L_00FF68:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00FF68 braw 0xb3be */
}

/* $00FF6C $27 endoskeleton stand (level 6) */
HLE_RANGE(0x00FF6C, L56Endo2Stand, 3500, EXIT_NEXT)
{
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 00FF6C jsr %pc@(0x12316) */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00FF70 subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_00FF7A; /* 00FF74 bnes 0xff7a */
	{ hle_insn(ctx, 18); sub_00FFC0(ctx); } /* 00FF76 jsr %pc@(0xffc0) */
L_00FF7A:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00FF7A movew %fp@(36),%d1 */
	{ sh_w(ctx, 1, 'a', 1); hle_insn(ctx, 8); } /* 00FF7E asrw #1,%d1 */
	{ uint32_t _s = 0x1Eu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00FF80 andiw #30,%d1 */
	{ uint32_t _s = 0x1Eu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d ^ _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00FF84 eoriw #30,%d1 */
	{ uint32_t _v = ld16((0x00FFA0u + (uint32_t)((int32_t)(int16_t)ctx->d[1]))); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 14); } /* 00FF88 movew %pc@(0xffa0,%d1:w),%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00FF8C jsr %pc@(0x13228) */
	{ uint32_t _s = 0x14u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 00FF90 cmpiw #20,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_00FF9C; /* 00FF96 bnes 0xff9c */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x012352); } /* 00FF98 jsr %pc@(0x12352) */
L_00FF9C:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00FF9C braw 0xb3be */
}

/* $00FFE8 $27 endoskeleton: last frame then free */
HLE_RANGE_PURE(0x00FFE8, L56Endo2Gone, 1000, EXIT_NEXT)
{
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 00FFE8 jsr %pc@(0x12316) */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00FFEC subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_010000; /* 00FFF0 bnes 0x10000 */
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 00FFF2 clrw %fp@ */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DAu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 00FFF4 subqw #1,0xffff80da */
	{ st8(0xFFFF9EBEu, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 20); } /* 00FFFA clrb 0xffff9ebe */
L_010000:;
	{ uint32_t _v = 0x2FBu; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010000 movew #763,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 010004 jsr %pc@(0x13228) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 010008 braw 0xb3be */
}

/* $01000C $27 endoskeleton shot (init, falls into $0001003A) */
HLE_RANGE_PURE(0x01000C, L56Endo2Init, 1000, EXIT_NEXT)
{
	goto L_01000C;
L_00FFDA:;
	{ uint32_t _v = 0x40u; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00FFDA movew #64,%fp@(36) */
	{ uint32_t _v = 0xFFE8u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00FFE0 movel #65512,%fp@(32) */
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 00FFE8 jsr %pc@(0x12316) */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00FFEC subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_010000; /* 00FFF0 bnes 0x10000 */
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 00FFF2 clrw %fp@ */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DAu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 00FFF4 subqw #1,0xffff80da */
	{ st8(0xFFFF9EBEu, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 20); } /* 00FFFA clrb 0xffff9ebe */
L_010000:;
	{ uint32_t _v = 0x2FBu; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010000 movew #763,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 010004 jsr %pc@(0x13228) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 010008 braw 0xb3be */
L_01000C:;
	{ st8((ctx->a[6] + (uint32_t)(int32_t)13), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 01000C clrb %fp@(13) */
	{ uint32_t _v = 0xAu; st8((ctx->a[6] + (uint32_t)(int32_t)18), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 010010 moveb #10,%fp@(18) */
	{ uint32_t _v = 0xC8u; st8((ctx->a[6] + (uint32_t)(int32_t)12), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 010016 moveb #-56,%fp@(12) */
	{ uint32_t _v = 0xFFu; st8((ctx->a[6] + (uint32_t)(int32_t)15), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 01001C moveb #-1,%fp@(15) */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o & 0x7F); hle_insn(ctx, 16); } /* 010022 bclr #15,%fp@ */
	{ uint32_t _s = 0xAu; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)7); uint32_t _d = ld8(_da); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 8, 1); st8(_da, _r); hle_insn(ctx, 20); } /* 010026 addib #10,%fp@(7) */
	{ uint32_t _v = 0x20u; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 01002C movew #32,%fp@(36) */
	{ uint32_t _v = 0x1003Au; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 010032 movel #65594,%fp@(32) */
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 01003A jsr %pc@(0x12316) */
	{ hle_insn(ctx, 18); sub_00D236(ctx); } /* 01003E jsr %pc@(0xd236) */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 010042 subqw #1,%fp@(36) */
	if (bs(ctx, !c_n(ctx))) goto L_010058; /* 010046 bpls 0x10058 */
	{ ctx->d[1] = 0x4u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 010048 moveq #4,%d1 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01004A movew %fp@(4),%d0 */
	{ uint32_t _s = ld16((ctx->a[6] + (uint32_t)(int32_t)40)); uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 12); } /* 01004E cmpw %fp@(40),%d0 */
	if (bw(ctx, c_gt(ctx))) goto L_00FFDA; /* 010052 bgtw 0xffda */
	hle_insn(ctx, 10); goto L_010066; /* 010056 bras 0x10066 */
L_010058:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010058 movew %fp@(36),%d1 */
	{ sh_w(ctx, 1, 'a', 1); hle_insn(ctx, 8); } /* 01005C asrw #1,%d1 */
	{ uint32_t _s = 0xFu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d ^ _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 01005E eoriw #15,%d1 */
	{ uint32_t _v = ld8((0x0100ACu + (uint32_t)((int32_t)(int16_t)ctx->d[1]))); set_b(&ctx->d[1], _v); cc_logic8(ctx, _v); hle_insn(ctx, 14); } /* 010062 moveb %pc@(0x100ac,%d1:w),%d1 */
L_010066:;
	{ uint32_t _v = ld32(0xFFFF80BCu); ctx->a[5] = _v; hle_insn(ctx, 20); } /* 010066 moveal 0xffff80bc,%a5 */
	{ uint32_t _v = 0x1u; st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01006C movew #1,%a5@+ */
	{ uint32_t _v = (ctx->d[1] & 0xFFFFu); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 010070 movew %d1,%d0 */
	{ sh_w(ctx, 0, 'l', 2); hle_insn(ctx, 10); } /* 010072 aslw #2,%d0 */
	{ ctx->a[1] = 0x0100BCu; hle_insn(ctx, 8); } /* 010074 lea %pc@(0x100bc),%a1 */
	{ uint32_t _v = ld16((ctx->a[1] + (uint32_t)(int32_t)0 + (uint32_t)((int32_t)(int16_t)ctx->d[0]))); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 14); } /* 010078 movew %a1@(0,%d0:w),%d2 */
	{ sh_w(ctx, 2, 'a', 1); hle_insn(ctx, 8); } /* 01007C asrw #1,%d2 */
	{ uint32_t _v = (ctx->d[2] & 0xFFFFu); st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 01007E movew %d2,%a5@+ */
	{ uint32_t _v = ld16((ctx->a[1] + (uint32_t)(int32_t)2 + (uint32_t)((int32_t)(int16_t)ctx->d[0]))); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 14); } /* 010080 movew %a1@(2,%d0:w),%d0 */
	{ ctx->a[1] = 0x000AEE7Eu; hle_insn(ctx, 12); } /* 010084 lea 0xaee7e,%a1 */
	{ ctx->a[1] = (ctx->a[1] + (uint32_t)(int32_t)0 + (uint32_t)((int32_t)(int16_t)ctx->d[0])); hle_insn(ctx, 12); } /* 01008A lea %a1@(0,%d0:w),%a1 */
	{ uint32_t _v = ctx->a[1]; st32(ctx->a[5], _v); ctx->a[5] += 4; cc_logic32(ctx, _v); hle_insn(ctx, 12); } /* 01008E movel %a1,%a5@+ */
	{ uint32_t _v = ld16(0xFFFF9E6Cu); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 010090 movew 0xffff9e6c,%d0 */
	{ sh_w(ctx, 0, 'l', 5); hle_insn(ctx, 16); } /* 010096 aslw #5,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010098 movew %d0,%a5@+ */
	{ uint32_t _v = ctx->a[5]; st32(0xFFFF80BCu, _v); cc_logic32(ctx, _v); hle_insn(ctx, 20); } /* 01009A movel %a5,0xffff80bc */
	{ uint32_t _s = 0x2F7u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 0100A0 addiw #759,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 0100A4 jsr %pc@(0x13228) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 0100A8 braw 0xb3be */
}

/* $01003A $27 endoskeleton fall animation (level 6) */
HLE_RANGE_PURE(0x01003A, L56Endo2Fall, 1000, EXIT_NEXT)
{
	goto L_01003A;
L_00FFDA:;
	{ uint32_t _v = 0x40u; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00FFDA movew #64,%fp@(36) */
	{ uint32_t _v = 0xFFE8u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00FFE0 movel #65512,%fp@(32) */
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 00FFE8 jsr %pc@(0x12316) */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00FFEC subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_010000; /* 00FFF0 bnes 0x10000 */
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 00FFF2 clrw %fp@ */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DAu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 00FFF4 subqw #1,0xffff80da */
	{ st8(0xFFFF9EBEu, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 20); } /* 00FFFA clrb 0xffff9ebe */
L_010000:;
	{ uint32_t _v = 0x2FBu; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010000 movew #763,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 010004 jsr %pc@(0x13228) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 010008 braw 0xb3be */
L_01003A:;
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 01003A jsr %pc@(0x12316) */
	{ hle_insn(ctx, 18); sub_00D236(ctx); } /* 01003E jsr %pc@(0xd236) */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 010042 subqw #1,%fp@(36) */
	if (bs(ctx, !c_n(ctx))) goto L_010058; /* 010046 bpls 0x10058 */
	{ ctx->d[1] = 0x4u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 010048 moveq #4,%d1 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01004A movew %fp@(4),%d0 */
	{ uint32_t _s = ld16((ctx->a[6] + (uint32_t)(int32_t)40)); uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 12); } /* 01004E cmpw %fp@(40),%d0 */
	if (bw(ctx, c_gt(ctx))) goto L_00FFDA; /* 010052 bgtw 0xffda */
	hle_insn(ctx, 10); goto L_010066; /* 010056 bras 0x10066 */
L_010058:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010058 movew %fp@(36),%d1 */
	{ sh_w(ctx, 1, 'a', 1); hle_insn(ctx, 8); } /* 01005C asrw #1,%d1 */
	{ uint32_t _s = 0xFu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d ^ _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 01005E eoriw #15,%d1 */
	{ uint32_t _v = ld8((0x0100ACu + (uint32_t)((int32_t)(int16_t)ctx->d[1]))); set_b(&ctx->d[1], _v); cc_logic8(ctx, _v); hle_insn(ctx, 14); } /* 010062 moveb %pc@(0x100ac,%d1:w),%d1 */
L_010066:;
	{ uint32_t _v = ld32(0xFFFF80BCu); ctx->a[5] = _v; hle_insn(ctx, 20); } /* 010066 moveal 0xffff80bc,%a5 */
	{ uint32_t _v = 0x1u; st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01006C movew #1,%a5@+ */
	{ uint32_t _v = (ctx->d[1] & 0xFFFFu); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 010070 movew %d1,%d0 */
	{ sh_w(ctx, 0, 'l', 2); hle_insn(ctx, 10); } /* 010072 aslw #2,%d0 */
	{ ctx->a[1] = 0x0100BCu; hle_insn(ctx, 8); } /* 010074 lea %pc@(0x100bc),%a1 */
	{ uint32_t _v = ld16((ctx->a[1] + (uint32_t)(int32_t)0 + (uint32_t)((int32_t)(int16_t)ctx->d[0]))); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 14); } /* 010078 movew %a1@(0,%d0:w),%d2 */
	{ sh_w(ctx, 2, 'a', 1); hle_insn(ctx, 8); } /* 01007C asrw #1,%d2 */
	{ uint32_t _v = (ctx->d[2] & 0xFFFFu); st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 01007E movew %d2,%a5@+ */
	{ uint32_t _v = ld16((ctx->a[1] + (uint32_t)(int32_t)2 + (uint32_t)((int32_t)(int16_t)ctx->d[0]))); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 14); } /* 010080 movew %a1@(2,%d0:w),%d0 */
	{ ctx->a[1] = 0x000AEE7Eu; hle_insn(ctx, 12); } /* 010084 lea 0xaee7e,%a1 */
	{ ctx->a[1] = (ctx->a[1] + (uint32_t)(int32_t)0 + (uint32_t)((int32_t)(int16_t)ctx->d[0])); hle_insn(ctx, 12); } /* 01008A lea %a1@(0,%d0:w),%a1 */
	{ uint32_t _v = ctx->a[1]; st32(ctx->a[5], _v); ctx->a[5] += 4; cc_logic32(ctx, _v); hle_insn(ctx, 12); } /* 01008E movel %a1,%a5@+ */
	{ uint32_t _v = ld16(0xFFFF9E6Cu); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 010090 movew 0xffff9e6c,%d0 */
	{ sh_w(ctx, 0, 'l', 5); hle_insn(ctx, 16); } /* 010096 aslw #5,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010098 movew %d0,%a5@+ */
	{ uint32_t _v = ctx->a[5]; st32(0xFFFF80BCu, _v); cc_logic32(ctx, _v); hle_insn(ctx, 20); } /* 01009A movel %a5,0xffff80bc */
	{ uint32_t _s = 0x2F7u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 0100A0 addiw #759,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 0100A4 jsr %pc@(0x13228) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 0100A8 braw 0xb3be */
}

/* $0100D0 $27 endoskeleton slide to the target X */
HLE_RANGE_PURE(0x0100D0, L56Endo2Move, 1000, EXIT_NEXT)
{
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0100D0 movew %fp@(4),%d0 */
	{ uint32_t _s = 0x8u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 0100D4 addqw #8,%d0 */
	{ uint32_t _s = ld16((ctx->a[6] + (uint32_t)(int32_t)40)); uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 12); } /* 0100D6 cmpw %fp@(40),%d0 */
	if (bs(ctx, !c_ge(ctx))) goto L_0100EE; /* 0100DA blts 0x100ee */
	{ uint32_t _v = 0x100FEu; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 0100DC movel #65790,%fp@(32) */
	{ uint32_t _v = 0xFu; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 0100E4 movew #15,%fp@(36) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)40)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0100EA movew %fp@(40),%d0 */
L_0100EE:;
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)4), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0100EE movew %d0,%fp@(4) */
	{ uint32_t _v = 0x324u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0100F2 movew #804,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 0100F6 bsrw 0x13228 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 0100FA braw 0xb3be */
}

/* $0100FE $27 endoskeleton pause */
HLE_RANGE_PURE(0x0100FE, L56Endo2Wait, 1000, EXIT_NEXT)
{
	{ uint32_t _v = 0x2F6u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0100FE movew #758,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 010102 bsrw 0x13228 */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 010106 subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_01011E; /* 01010A bnes 0x1011e */
	{ uint32_t _v = 0xFF6Cu; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 01010C movel #65388,%fp@(32) */
	{ uint32_t _v = 0x40u; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 010114 movew #64,%fp@(36) */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o | 0x80); hle_insn(ctx, 16); } /* 01011A bset #7,%fp@ */
L_01011E:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 01011E braw 0xb3be */
}

/* $010122 sprite $7C, scroll, free off the left */
HLE_RANGE_PURE(0x010122, L56ModelLeave, 1000, EXIT_NEXT)
{
	{ uint32_t _v = 0x7Cu; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 010122 movew #124,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 010126 jsr %pc@(0x13228) */
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 01012A jsr %pc@(0x12316) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01012E movew %fp@(2),%d0 */
	{ uint32_t _s = 0xFFECu; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 010132 cmpiw #-20,%d0 */
	if (bs(ctx, c_gt(ctx))) goto L_010146; /* 010136 bgts 0x10146 */
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 010138 clrw %fp@ */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DAu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 01013A subqw #1,0xffff80da */
	{ st32(0xFFFF9EDAu, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 28); } /* 010140 clrl 0xffff9eda */
L_010146:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 010146 braw 0xb3be */
}

/* $01014A sprite from a table, scroll, free off the left */
HLE_RANGE_PURE(0x01014A, L56ModelLeaveB, 1000, EXIT_NEXT)
{
	goto L_01014A;
L_010126:;
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 010126 jsr %pc@(0x13228) */
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 01012A jsr %pc@(0x12316) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 01012E movew %fp@(2),%d0 */
	{ uint32_t _s = 0xFFECu; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 010132 cmpiw #-20,%d0 */
	if (bs(ctx, c_gt(ctx))) goto L_010146; /* 010136 bgts 0x10146 */
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 010138 clrw %fp@ */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DAu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 01013A subqw #1,0xffff80da */
	{ st32(0xFFFF9EDAu, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 28); } /* 010140 clrl 0xffff9eda */
L_010146:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 010146 braw 0xb3be */
L_01014A:;
	{ uint32_t _v = ld16(0xFFFFA0EEu); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 01014A movew 0xffffa0ee,%d1 */
	{ uint32_t _s = 0xEu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 010150 andiw #14,%d1 */
	{ uint32_t _v = ld16((0x01015Cu + (uint32_t)((int32_t)(int16_t)ctx->d[1]))); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 14); } /* 010154 movew %pc@(0x1015c,%d1:w),%d1 */
	hle_insn(ctx, 10); goto L_010126; /* 010158 braw 0x10126 */
}

/* $010238 $28 equipment machine: walk */
HLE_RANGE_PURE(0x010238, L56ModelWalk, 1000, EXIT_NEXT)
{
	{ hle_insn(ctx, 18); ObjMoveX(ctx); } /* 010238 jsr %pc@(0xb4a6) */
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 01023C jsr %pc@(0x12316) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[5], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010240 movew %fp@(36),%d5 */
	{ uint32_t _s = 0x38u; uint32_t _d = (ctx->d[5] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[5], _r); hle_insn(ctx, 8); } /* 010244 andiw #56,%d5 */
	{ sh_w(ctx, 5, 'r', 3); hle_insn(ctx, 12); } /* 010248 lsrw #3,%d5 */
	{ uint32_t _s = 0x7u; uint32_t _d = (ctx->d[5] & 0xFFFFu); uint32_t _r = (_d ^ _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[5], _r); hle_insn(ctx, 8); } /* 01024A eoriw #7,%d5 */
	{ uint32_t _v = (ctx->d[5] & 0xFFFFu); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 01024E movew %d5,%d1 */
	{ hle_insn(ctx, 18); sub_01042C(ctx); } /* 010250 jsr %pc@(0x1042c) */
	{ ctx->d[1] = 0x10u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 010254 moveq #16,%d1 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 6) & 1) ? 0 : CC_Z); hle_insn(ctx, 12); } /* 010256 btst #6,%fp@ */
	if (bs(ctx, !c_z(ctx))) goto L_01025E; /* 01025A bnes 0x1025e */
	{ ctx->d[1] = 0xFFFFFFF0u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 01025C moveq #-16,%d1 */
L_01025E:;
	{ uint32_t _v = ld32(0xFFFF9EDAu); ctx->d[0] = _v; cc_logic32(ctx, _v); hle_insn(ctx, 20); } /* 01025E movel 0xffff9eda,%d0 */
	if (bs(ctx, c_z(ctx))) goto L_01028A; /* 010264 beqs 0x1028a */
	{ uint32_t _v = ctx->d[0]; ctx->a[1] = _v; hle_insn(ctx, 4); } /* 010266 moveal %d0,%a1 */
	{ uint32_t _v = ld16((ctx->a[1] + (uint32_t)(int32_t)2)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010268 movew %a1@(2),%d0 */
	{ uint32_t _s = (ctx->d[1] & 0xFFFFu); uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 01026C subw %d1,%d0 */
	{ uint32_t _s = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 12); } /* 01026E subw %fp@(2),%d0 */
	if (bs(ctx, !c_n(ctx))) goto L_010276; /* 010272 bpls 0x10276 */
	{ uint32_t _r = alu(ctx, OP_SUB, 0, ctx->d[0], 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 010274 negw %d0 */
L_010276:;
	{ uint32_t _s = 0x6u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 010276 cmpiw #6,%d0 */
	if (bs(ctx, c_gt(ctx))) goto L_01028A; /* 01027A bgts 0x1028a */
	{ uint32_t _v = 0x102D0u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 01027C movel #66256,%fp@(32) */
	{ uint32_t _v = 0x50u; st16((ctx->a[6] + (uint32_t)(int32_t)20), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 010284 movew #80,%fp@(20) */
L_01028A:;
	{ hle_insn(ctx, 18); sub_0102A6(ctx); } /* 01028A jsr %pc@(0x102a6) */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 01028E subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_0102A2; /* 010292 bnes 0x102a2 */
	{ uint32_t _v = 0x1031Eu; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 010294 movel #66334,%fp@(32) */
	{ uint32_t _v = 0x50u; st16((ctx->a[6] + (uint32_t)(int32_t)20), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 01029C movew #80,%fp@(20) */
L_0102A2:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 0102A2 braw 0xb3be */
}

/* $0102D0 $28 equipment machine state */
HLE_RANGE_PURE(0x0102D0, L56ModelA, 1000, EXIT_NEXT)
{
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 0102D0 jsr %pc@(0x12316) */
	{ hle_insn(ctx, 18); sub_0102A6(ctx); } /* 0102D4 jsr %pc@(0x102a6) */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)20); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 0102D8 subqw #1,%fp@(20) */
	if (bs(ctx, !c_z(ctx))) goto L_0102E6; /* 0102DC bnes 0x102e6 */
	{ uint32_t _v = 0x10238u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 0102DE movel #66104,%fp@(32) */
L_0102E6:;
	{ uint32_t _s = 0x20u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)20); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 0102E6 cmpiw #32,%fp@(20) */
	if (bs(ctx, !c_z(ctx))) goto L_010302; /* 0102EC bnes 0x10302 */
	{ uint32_t _v = ld32(0xFFFF9EDAu); ctx->a[1] = _v; hle_insn(ctx, 20); } /* 0102EE moveal 0xffff9eda,%a1 */
	{ uint32_t _v = 0x1014Au; st32((ctx->a[1] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 0102F4 movel #65866,%a1@(32) */
	{ st32(0xFFFF9EDAu, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 28); } /* 0102FC clrl 0xffff9eda */
L_010302:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)20)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010302 movew %fp@(20),%d1 */
	{ sh_w(ctx, 1, 'a', 3); hle_insn(ctx, 12); } /* 010306 asrw #3,%d1 */
	{ uint32_t _v = ld8((0x010314u + (uint32_t)((int32_t)(int16_t)ctx->d[1]))); set_b(&ctx->d[1], _v); cc_logic8(ctx, _v); hle_insn(ctx, 14); } /* 010308 moveb %pc@(0x10314,%d1:w),%d1 */
	{ hle_insn(ctx, 18); sub_01042C(ctx); } /* 01030C jsr %pc@(0x1042c) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 010310 braw 0xb3be */
}

/* $01031E $28 equipment machine state */
HLE_RANGE(0x01031E, L56ModelB, 1500, EXIT_NEXT)
{
	goto L_01031E;
L_010302:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)20)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010302 movew %fp@(20),%d1 */
	{ sh_w(ctx, 1, 'a', 3); hle_insn(ctx, 12); } /* 010306 asrw #3,%d1 */
	{ uint32_t _v = ld8((0x010314u + (uint32_t)((int32_t)(int16_t)ctx->d[1]))); set_b(&ctx->d[1], _v); cc_logic8(ctx, _v); hle_insn(ctx, 14); } /* 010308 moveb %pc@(0x10314,%d1:w),%d1 */
	{ hle_insn(ctx, 18); sub_01042C(ctx); } /* 01030C jsr %pc@(0x1042c) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 010310 braw 0xb3be */
L_01031E:;
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 01031E jsr %pc@(0x12316) */
	{ hle_insn(ctx, 18); sub_0102A6(ctx); } /* 010322 jsr %pc@(0x102a6) */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)20); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 010326 subqw #1,%fp@(20) */
	if (bs(ctx, !c_z(ctx))) goto L_010330; /* 01032A bnes 0x10330 */
	{ hle_insn(ctx, 18); sub_010408(ctx); } /* 01032C jsr %pc@(0x10408) */
L_010330:;
	{ uint32_t _s = 0x20u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)20); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 010330 cmpiw #32,%fp@(20) */
	if (bs(ctx, !c_z(ctx))) goto L_01036A; /* 010336 bnes 0x1036a */
	{ uint32_t _a = (ctx->a[6] + (uint32_t)(int32_t)2); ctx->d[0] = (uint32_t)(int32_t)(int16_t)ld16(_a + 0); ctx->d[1] = (uint32_t)(int32_t)(int16_t)ld16(_a + 2); hle_insn(ctx, 24); } /* 010338 movemw %fp@(2),%d0-%d1 */
	{ uint32_t _s = 0x10u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 01033E addiw #16,%d0 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 6) & 1) ? 0 : CC_Z); hle_insn(ctx, 12); } /* 010342 btst #14,%fp@ */
	if (bs(ctx, !c_z(ctx))) goto L_01034C; /* 010346 bnes 0x1034c */
	{ uint32_t _s = 0x20u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 010348 subiw #32,%d0 */
L_01034C:;
	{ uint32_t _v = ld16(0xFFFF9EC6u); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 01034C movew 0xffff9ec6,%d2 */
	if (bs(ctx, c_z(ctx))) goto L_01035C; /* 010352 beqs 0x1035c */
	{ st16(0xFFFF9EC6u, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 20); } /* 010354 clrw 0xffff9ec6 */
	hle_insn(ctx, 10); goto L_010366; /* 01035A bras 0x10366 */
L_01035C:;
	{ ctx->d[2] = 0x2u; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 01035C moveq #2,%d2 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 5) & 1) ? 0 : CC_Z); st8(_a, _o ^ 0x20); hle_insn(ctx, 16); } /* 01035E bchg #13,%fp@ */
	if (bs(ctx, !c_z(ctx))) goto L_010366; /* 010362 bnes 0x10366 */
	{ ctx->d[2] = 0xBu; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 010364 moveq #11,%d2 */
L_010366:;
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x012A16); } /* 010366 jsr %pc@(0x12a16) */
L_01036A:;
	hle_insn(ctx, 10); goto L_010302; /* 01036A braw 0x10302 */
}

/* $01036E $28 equipment machine state */
HLE_RANGE_PURE(0x01036E, L56ModelC, 1000, EXIT_NEXT)
{
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 01036E subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_0103A0; /* 010372 bnes 0x103a0 */
	{ uint32_t _s = 0x2u; uint32_t _da = 0xFFFF9EBEu; uint32_t _d = ld8(_da); alu(ctx, OP_SUB, _d, _s, 8, 0); hle_insn(ctx, 20); } /* 010374 cmpib #2,0xffff9ebe */
	if (bs(ctx, !c_z(ctx))) goto L_010384; /* 01037C bnes 0x10384 */
	{ st8(0xFFFF9EBEu, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 20); } /* 01037E clrb 0xffff9ebe */
L_010384:;
	{ uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)14); uint32_t _r = alu(ctx, OP_SUB, 0, ld8(_da), 8, 1); st8(_da, _r); hle_insn(ctx, 16); } /* 010384 negb %fp@(14) */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 6) & 1) ? 0 : CC_Z); st8(_a, _o ^ 0x40); hle_insn(ctx, 16); } /* 010388 bchg #14,%fp@ */
	if (bs(ctx, !c_z(ctx))) goto L_010396; /* 01038C bnes 0x10396 */
	{ uint32_t _v = 0xFFECu; st16((ctx->a[6] + (uint32_t)(int32_t)2), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 01038E movew #-20,%fp@(2) */
	hle_insn(ctx, 10); goto L_01039C; /* 010394 bras 0x1039c */
L_010396:;
	{ uint32_t _v = 0x14Au; st16((ctx->a[6] + (uint32_t)(int32_t)2), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 010396 movew #330,%fp@(2) */
L_01039C:;
	{ hle_insn(ctx, 18); sub_010408(ctx); } /* 01039C bsrw 0x10408 */
L_0103A0:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 0103A0 braw 0xb3be */
}

/* $0103A4 $28 equipment machine state */
HLE_RANGE_PURE(0x0103A4, L56ModelD, 1000, EXIT_NEXT)
{
	{ ctx->d[1] = 0xAu; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 0103A4 moveq #10,%d1 */
	{ uint32_t _s = 0x6u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 0103A6 cmpiw #6,%fp@(36) */
	if (bs(ctx, c_gt(ctx))) goto L_0103B0; /* 0103AC bgts 0x103b0 */
	{ ctx->d[1] = 0x9u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 0103AE moveq #9,%d1 */
L_0103B0:;
	{ hle_insn(ctx, 18); sub_01042C(ctx); } /* 0103B0 bsrw 0x1042c */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 0103B4 subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_0103C2; /* 0103B8 bnes 0x103c2 */
	{ hle_insn(ctx, 18); sub_010408(ctx); } /* 0103BA bsrw 0x10408 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o | 0x80); hle_insn(ctx, 16); } /* 0103BE bset #15,%fp@ */
L_0103C2:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 0103C2 braw 0xb3be */
}

/* $0103C6 $28 equipment machine state */
HLE_RANGE_PURE(0x0103C6, L56ModelE, 1000, EXIT_NEXT)
{
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 0103C6 jsr %pc@(0x12316) */
	{ hle_insn(ctx, 18); sub_0102A6(ctx); } /* 0103CA jsr %pc@(0x102a6) */
	{ ctx->d[1] = 0xCu; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 0103CE moveq #12,%d1 */
	{ uint32_t _s = 0x50u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 0103D0 cmpiw #80,%fp@(36) */
	if (bs(ctx, !c_ge(ctx))) goto L_0103E4; /* 0103D6 blts 0x103e4 */
	{ ctx->d[1] = 0xBu; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 0103D8 moveq #11,%d1 */
	{ uint32_t _s = 0x5Au; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 0103DA cmpiw #90,%fp@(36) */
	if (bs(ctx, !c_ge(ctx))) goto L_0103E4; /* 0103E0 blts 0x103e4 */
	{ ctx->d[1] = 0xAu; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 0103E2 moveq #10,%d1 */
L_0103E4:;
	{ hle_insn(ctx, 18); sub_01042C(ctx); } /* 0103E4 bsrw 0x1042c */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 0103E8 subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_010404; /* 0103EC bnes 0x10404 */
	{ hle_insn(ctx, 18); sub_010408(ctx); } /* 0103EE bsrw 0x10408 */
	{ uint32_t _v = 0x17u; st16((ctx->a[6] + (uint32_t)(int32_t)20), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 0103F2 movew #23,%fp@(20) */
	{ uint32_t _v = 0x1031Eu; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 0103F8 movel #66334,%fp@(32) */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o | 0x80); hle_insn(ctx, 16); } /* 010400 bset #15,%fp@ */
L_010404:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 010404 braw 0xb3be */
}

/* $0104B6 $28 equipment machine (level 6): walk */
HLE_RANGE_PURE(0x0104B6, L56Model2Walk, 1000, EXIT_NEXT)
{
	{ hle_insn(ctx, 18); ObjMoveX(ctx); } /* 0104B6 jsr %pc@(0xb4a6) */
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 0104BA jsr %pc@(0x12316) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[5], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0104BE movew %fp@(36),%d5 */
	{ uint32_t _s = 0x38u; uint32_t _d = (ctx->d[5] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[5], _r); hle_insn(ctx, 8); } /* 0104C2 andiw #56,%d5 */
	{ sh_w(ctx, 5, 'r', 3); hle_insn(ctx, 12); } /* 0104C6 lsrw #3,%d5 */
	{ uint32_t _s = 0x7u; uint32_t _d = (ctx->d[5] & 0xFFFFu); uint32_t _r = (_d ^ _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[5], _r); hle_insn(ctx, 8); } /* 0104C8 eoriw #7,%d5 */
	{ uint32_t _v = (ctx->d[5] & 0xFFFFu); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 0104CC movew %d5,%d1 */
	{ hle_insn(ctx, 18); sub_010666(ctx); } /* 0104CE jsr %pc@(0x10666) */
	{ hle_insn(ctx, 18); sub_0104EE(ctx); } /* 0104D2 jsr %pc@(0x104ee) */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 0104D6 subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_0104EA; /* 0104DA bnes 0x104ea */
	{ uint32_t _v = 0x10532u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 0104DC movel #66866,%fp@(32) */
	{ uint32_t _v = 0x40u; st16((ctx->a[6] + (uint32_t)(int32_t)20), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 0104E4 movew #64,%fp@(20) */
L_0104EA:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 0104EA braw 0xb3be */
}

/* $010532 $28 equipment machine (level 6) */
HLE_RANGE(0x010532, L56Model2B, 1700, EXIT_NEXT)
{
	goto L_010532;
L_010518:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)20)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 010518 movew %fp@(20),%d1 */
	{ sh_w(ctx, 1, 'a', 3); hle_insn(ctx, 12); } /* 01051C asrw #3,%d1 */
	{ uint32_t _v = ld8((0x01052Au + (uint32_t)((int32_t)(int16_t)ctx->d[1]))); set_b(&ctx->d[1], _v); cc_logic8(ctx, _v); hle_insn(ctx, 14); } /* 01051E moveb %pc@(0x1052a,%d1:w),%d1 */
	{ hle_insn(ctx, 18); sub_010666(ctx); } /* 010522 jsr %pc@(0x10666) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 010526 braw 0xb3be */
L_010532:;
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 010532 jsr %pc@(0x12316) */
	{ hle_insn(ctx, 18); sub_0104EE(ctx); } /* 010536 jsr %pc@(0x104ee) */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)20); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 01053A subqw #1,%fp@(20) */
	if (bs(ctx, !c_z(ctx))) goto L_010544; /* 01053E bnes 0x10544 */
	{ hle_insn(ctx, 18); sub_010642(ctx); } /* 010540 jsr %pc@(0x10642) */
L_010544:;
	{ uint32_t _s = 0x20u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)20); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 010544 cmpiw #32,%fp@(20) */
	if (bs(ctx, !c_z(ctx))) goto L_01057E; /* 01054A bnes 0x1057e */
	{ uint32_t _a = (ctx->a[6] + (uint32_t)(int32_t)2); ctx->d[0] = (uint32_t)(int32_t)(int16_t)ld16(_a + 0); ctx->d[1] = (uint32_t)(int32_t)(int16_t)ld16(_a + 2); hle_insn(ctx, 24); } /* 01054C movemw %fp@(2),%d0-%d1 */
	{ uint32_t _s = 0x10u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 010552 addiw #16,%d0 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 6) & 1) ? 0 : CC_Z); hle_insn(ctx, 12); } /* 010556 btst #14,%fp@ */
	if (bs(ctx, !c_z(ctx))) goto L_010560; /* 01055A bnes 0x10560 */
	{ uint32_t _s = 0x20u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 01055C subiw #32,%d0 */
L_010560:;
	{ uint32_t _v = ld16(0xFFFF9EC6u); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 010560 movew 0xffff9ec6,%d2 */
	if (bs(ctx, c_le(ctx))) goto L_010570; /* 010566 bles 0x10570 */
	{ st16(0xFFFF9EC6u, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 20); } /* 010568 clrw 0xffff9ec6 */
	hle_insn(ctx, 10); goto L_01057A; /* 01056E bras 0x1057a */
L_010570:;
	{ ctx->d[2] = 0x2u; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 010570 moveq #2,%d2 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 5) & 1) ? 0 : CC_Z); st8(_a, _o ^ 0x20); hle_insn(ctx, 16); } /* 010572 bchg #13,%fp@ */
	if (bs(ctx, !c_z(ctx))) goto L_01057A; /* 010576 bnes 0x1057a */
	{ ctx->d[2] = 0xBu; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 010578 moveq #11,%d2 */
L_01057A:;
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x012A16); } /* 01057A jsr %pc@(0x12a16) */
L_01057E:;
	hle_insn(ctx, 10); goto L_010518; /* 01057E braw 0x10518 */
}

/* $010582 $28 equipment machine (level 6) */
HLE_RANGE_PURE(0x010582, L56Model2C, 1000, EXIT_NEXT)
{
	{ uint32_t _v = ld16(0xFFFF9EC6u); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 010582 movew 0xffff9ec6,%d0 */
	if (bs(ctx, !c_n(ctx))) goto L_0105AE; /* 010588 bpls 0x105ae */
	{ uint32_t _s = 0xFFFFu; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 01058A cmpiw #-1,%d0 */
	if (bs(ctx, c_z(ctx))) goto L_0105D0; /* 01058E beqs 0x105d0 */
	{ uint32_t _v = 0x1070Cu; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 010590 movel #67340,%fp@(32) */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 6) & 1) ? 0 : CC_Z); st8(_a, _o | 0x40); hle_insn(ctx, 16); } /* 010598 bset #14,%fp@ */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o & 0x7F); hle_insn(ctx, 16); } /* 01059C bclr #15,%fp@ */
	{ uint32_t _v = 0x1u; st8((ctx->a[6] + (uint32_t)(int32_t)14), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 0105A0 moveb #1,%fp@(14) */
	{ uint32_t _v = 0xFFECu; st16((ctx->a[6] + (uint32_t)(int32_t)2), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 0105A6 movew #-20,%fp@(2) */
	hle_insn(ctx, 10); goto L_0105D0; /* 0105AC bras 0x105d0 */
L_0105AE:;
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 0105AE subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_0105D0; /* 0105B2 bnes 0x105d0 */
	{ uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)14); uint32_t _r = alu(ctx, OP_SUB, 0, ld8(_da), 8, 1); st8(_da, _r); hle_insn(ctx, 16); } /* 0105B4 negb %fp@(14) */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 6) & 1) ? 0 : CC_Z); st8(_a, _o ^ 0x40); hle_insn(ctx, 16); } /* 0105B8 bchg #14,%fp@ */
	if (bs(ctx, !c_z(ctx))) goto L_0105C6; /* 0105BC bnes 0x105c6 */
	{ uint32_t _v = 0xFFECu; st16((ctx->a[6] + (uint32_t)(int32_t)2), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 0105BE movew #-20,%fp@(2) */
	hle_insn(ctx, 10); goto L_0105CC; /* 0105C4 bras 0x105cc */
L_0105C6:;
	{ uint32_t _v = 0x14Au; st16((ctx->a[6] + (uint32_t)(int32_t)2), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 0105C6 movew #330,%fp@(2) */
L_0105CC:;
	{ hle_insn(ctx, 18); sub_010642(ctx); } /* 0105CC bsrw 0x10642 */
L_0105D0:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 0105D0 braw 0xb3be */
}

/* $0105D4 $28 equipment machine (level 6) */
HLE_RANGE_PURE(0x0105D4, L56Model2D, 1000, EXIT_NEXT)
{
	{ ctx->d[1] = 0x9u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 0105D4 moveq #9,%d1 */
	{ uint32_t _s = 0x6u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 0105D6 cmpiw #6,%fp@(36) */
	if (bs(ctx, c_gt(ctx))) goto L_0105E0; /* 0105DC bgts 0x105e0 */
	{ ctx->d[1] = 0x8u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 0105DE moveq #8,%d1 */
L_0105E0:;
	{ hle_insn(ctx, 18); sub_010666(ctx); } /* 0105E0 bsrw 0x10666 */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 0105E4 subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_0105F2; /* 0105E8 bnes 0x105f2 */
	{ hle_insn(ctx, 18); sub_010642(ctx); } /* 0105EA bsrw 0x10642 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o | 0x80); hle_insn(ctx, 16); } /* 0105EE bset #15,%fp@ */
L_0105F2:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 0105F2 braw 0xb3be */
}

/* $0105F6 $28 equipment machine (level 6) */
HLE_RANGE_PURE(0x0105F6, L56Model2E, 1000, EXIT_NEXT)
{
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 0105F6 jsr %pc@(0x12316) */
	{ hle_insn(ctx, 18); sub_0104EE(ctx); } /* 0105FA jsr %pc@(0x104ee) */
	{ ctx->d[1] = 0xCu; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 0105FE moveq #12,%d1 */
	{ uint32_t _s = 0x46u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 010600 cmpiw #70,%fp@(36) */
	if (bs(ctx, !c_ge(ctx))) goto L_01061E; /* 010606 blts 0x1061e */
	{ ctx->d[1] = 0xBu; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 010608 moveq #11,%d1 */
	{ uint32_t _s = 0x50u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 01060A cmpiw #80,%fp@(36) */
	if (bs(ctx, !c_ge(ctx))) goto L_01061E; /* 010610 blts 0x1061e */
	{ ctx->d[1] = 0xAu; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 010612 moveq #10,%d1 */
	{ uint32_t _s = 0x5Au; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 010614 cmpiw #90,%fp@(36) */
	if (bs(ctx, !c_ge(ctx))) goto L_01061E; /* 01061A blts 0x1061e */
	{ ctx->d[1] = 0x9u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 01061C moveq #9,%d1 */
L_01061E:;
	{ hle_insn(ctx, 18); sub_010666(ctx); } /* 01061E bsrw 0x10666 */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 010622 subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_01063E; /* 010626 bnes 0x1063e */
	{ hle_insn(ctx, 18); sub_010642(ctx); } /* 010628 bsrw 0x10642 */
	{ uint32_t _v = 0x17u; st16((ctx->a[6] + (uint32_t)(int32_t)20), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 01062C movew #23,%fp@(20) */
	{ uint32_t _v = 0x10532u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 010632 movel #66866,%fp@(32) */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o | 0x80); hle_insn(ctx, 16); } /* 01063A bset #15,%fp@ */
L_01063E:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 01063E braw 0xb3be */
}

/* $00C348 $0A fixture blink */
HLE_RANGE(0x00C348, L56Fixture, 16300, EXIT_NEXT)
{
	{ hle_insn(ctx, 18); sub_00D236(ctx); } /* 00C348 jsr %pc@(0xd236) */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00C34C subqw #1,%fp@(36) */
	if (bw(ctx, c_z(ctx))) goto L_00C37E; /* 00C350 beqw 0xc37e */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00C354 movew %fp@(36),%d1 */
	{ uint32_t _v = (ctx->d[1] & 0xFFFFu); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 00C358 movew %d1,%d2 */
	{ sh_w(ctx, 2, 'a', 2); hle_insn(ctx, 10); } /* 00C35A asrw #2,%d2 */
	{ uint32_t _s = 0xDu; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 00C35C addiw #13,%d2 */
	{ uint32_t _v = (ctx->d[2] & 0xFFu); st8((ctx->a[6] + (uint32_t)(int32_t)7), _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 00C360 moveb %d2,%fp@(7) */
	{ sh_w(ctx, 1, 'a', 1); hle_insn(ctx, 8); } /* 00C364 asrw #1,%d1 */
	{ uint32_t _s = 0xFu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00C366 andiw #15,%d1 */
	{ uint32_t _s = 0xFu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d ^ _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00C36A eoriw #15,%d1 */
	{ uint32_t _v = ld8((0x00C39Cu + (uint32_t)((int32_t)(int16_t)ctx->d[1]))); set_b(&ctx->d[1], _v); cc_logic8(ctx, _v); hle_insn(ctx, 14); } /* 00C36E moveb %pc@(0xc39c,%d1:w),%d1 */
	{ uint32_t _s = 0x325u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00C372 addiw #805,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00C376 jsr %pc@(0x13228) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00C37A braw 0xb3be */
L_00C37E:;
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x00908A); } /* 00C37E bsrw 0x908a */
	{ uint32_t _v = 0x7Eu; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00C382 movew #126,%d0 */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_call68k(ctx, 0x013D04); } /* 00C386 jsr 0x13d04 */
	{ uint32_t _v = 0x14u; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00C38C movew #20,%fp@(36) */
	{ uint32_t _v = 0xC3ACu; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00C392 movel #50092,%fp@(32) */
	hle_insn(ctx, 10); goto L_00C3AC; /* 00C39A bras 0xc3ac */
L_00C3AC:;
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00C3AC subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_00C3BA; /* 00C3B0 bnes 0xc3ba */
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 00C3B2 clrw %fp@ */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DAu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 00C3B4 subqw #1,0xffff80da */
L_00C3BA:;
	{ uint32_t _v = 0x331u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00C3BA movew #817,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00C3BE jsr %pc@(0x13228) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00C3C2 braw 0xb3be */
}

/* $00C3AC $0A fixture second state */
HLE_RANGE_PURE(0x00C3AC, L56FixtureFlash, 1000, EXIT_NEXT)
{
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00C3AC subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_00C3BA; /* 00C3B0 bnes 0xc3ba */
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 00C3B2 clrw %fp@ */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DAu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 00C3B4 subqw #1,0xffff80da */
L_00C3BA:;
	{ uint32_t _v = 0x331u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00C3BA movew #817,%d1 */
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 00C3BE jsr %pc@(0x13228) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00C3C2 braw 0xb3be */
}

/* $0069EE hit test of $0F Cyberdyne equipment */
HLE_FUNC_PURE(0x0069EE, HitL56Equip, 300)
{
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)26)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0069EE movew %fp@(26),%d0 */
	{ sh_w(ctx, 0, 'l', 3); hle_insn(ctx, 12); } /* 0069F2 aslw #3,%d0 */
	{ ctx->a[3] = (0x006A18u + (uint32_t)((int32_t)(int16_t)ctx->d[0])); hle_insn(ctx, 12); } /* 0069F4 lea %pc@(0x6a18,%d0:w),%a3 */
	{ ctx->a[2] = 0x006A00u; hle_insn(ctx, 8); } /* 0069F8 lea %pc@(0x6a00),%a2 */
	hle_insn(ctx, 10); HitBox(ctx); return; /* 0069FC jmp %pc@(0x5940) */
}

/* $006AE0 hit test of $27 / $01 endoskeleton (level 5) */
HLE_FUNC_PURE(0x006AE0, HitL56Endo, 300)
{
	{ ctx->a[3] = 0x006B2Au; hle_insn(ctx, 8); } /* 006AE0 lea %pc@(0x6b2a),%a3 */
	{ ctx->a[2] = 0x006AECu; hle_insn(ctx, 8); } /* 006AE4 lea %pc@(0x6aec),%a2 */
	hle_insn(ctx, 10); HitBox(ctx); return; /* 006AE8 jmp %pc@(0x5940) */
}

/* $006B32 hit test of $27 endoskeleton */
HLE_FUNC_PURE(0x006B32, HitL56Endo2, 300)
{
	{ ctx->a[3] = 0x006B60u; hle_insn(ctx, 8); } /* 006B32 lea %pc@(0x6b60),%a3 */
	{ ctx->a[2] = 0x006B3Eu; hle_insn(ctx, 8); } /* 006B36 lea %pc@(0x6b3e),%a2 */
	hle_insn(ctx, 10); HitBox(ctx); return; /* 006B3A jmp %pc@(0x5940) */
}

/* $006B68 hit test of $27 endoskeleton */
HLE_FUNC_PURE(0x006B68, HitL56Endo3, 300)
{
	{ ctx->a[3] = 0x006B8Eu; hle_insn(ctx, 8); } /* 006B68 lea %pc@(0x6b8e),%a3 */
	{ ctx->a[2] = 0x006B74u; hle_insn(ctx, 8); } /* 006B6C lea %pc@(0x6b74),%a2 */
	hle_insn(ctx, 10); HitBox(ctx); return; /* 006B70 jmp %pc@(0x5940) */
}

/* $006B96 hit test of $28 */
HLE_FUNC_PURE(0x006B96, HitL56Model, 300)
{
	{ ctx->a[3] = 0x006BDAu; hle_insn(ctx, 8); } /* 006B96 lea %pc@(0x6bda),%a3 */
	{ ctx->a[2] = 0x006BA2u; hle_insn(ctx, 8); } /* 006B9A lea %pc@(0x6ba2),%a2 */
	hle_insn(ctx, 10); HitBox(ctx); return; /* 006B9E jmp %pc@(0x5940) */
}

/* $006BE2 hit test of $28 */
HLE_FUNC_PURE(0x006BE2, HitL56Model2, 300)
{
	{ ctx->a[3] = 0x006C26u; hle_insn(ctx, 8); } /* 006BE2 lea %pc@(0x6c26),%a3 */
	{ ctx->a[2] = 0x006BEEu; hle_insn(ctx, 8); } /* 006BE6 lea %pc@(0x6bee),%a2 */
	hle_insn(ctx, 10); HitBox(ctx); return; /* 006BEA jmp %pc@(0x5940) */
}

/* $0063C0 hit test of $20 fixture */
HLE_FUNC_PURE(0x0063C0, HitL56Lamp, 300)
{
	{ ctx->a[3] = 0x0063EAu; hle_insn(ctx, 8); } /* 0063C0 lea %pc@(0x63ea),%a3 */
	{ ctx->a[2] = 0x0063CCu; hle_insn(ctx, 8); } /* 0063C4 lea %pc@(0x63cc),%a2 */
	hle_insn(ctx, 10); HitBox(ctx); return; /* 0063C8 jmp %pc@(0x5940) */
}

