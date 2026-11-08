/* Work-RAM-only routines that were still 68k after the pools and AI files (docs/remaining.md, "Other"): the
 * object path stepper $00C48E (registered at its own address so the 68k callers get C too; the AI files
 * inline their own copy), the pickup pool runner PickupsRun $012BB0 and the VDP list builders
 * VdpQueueTileRect $012234 / VdpQueueTileRectMirror $01227A (command 6: a rect of tilemap words), and the
 * per-player state handler (now game/playerstate.c).
 *
 * The generated bodies were produced instruction by instruction from the disassembly (a comment on every
 * line names the 68k instruction and its address) with the GPGX cost of each opcode word on the
 * instruction timeline.
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
static void sub_00D236(hle_ctx *ctx);

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

/* $012234 */
HLE_FUNC_PURE(0x012234, VdpQueueTileRect, 4000)
{
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[3], _r); hle_insn(ctx, 4); } /* 012234 subqw #1,%d3 */
	{ uint32_t _v = ld32(0xFFFF80B8u); ctx->a[1] = _v; hle_insn(ctx, 20); } /* 012236 moveal 0xffff80b8,%a1 */
	{ uint32_t _v = ld32(0xFFFF80BCu); ctx->a[5] = _v; hle_insn(ctx, 20); } /* 01223C moveal 0xffff80bc,%a5 */
	{ uint32_t _v = 0x6u; st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012242 movew #6,%a5@+ */
	{ uint32_t _v = (ctx->d[2] & 0xFFFFu); st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 012246 movew %d2,%a5@+ */
	{ uint32_t _v = ctx->a[1]; st32(ctx->a[5], _v); ctx->a[5] += 4; cc_logic32(ctx, _v); hle_insn(ctx, 12); } /* 012248 movel %a1,%a5@+ */
	{ uint32_t _v = (ctx->d[1] & 0xFFFFu); st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 01224A movew %d1,%a5@+ */
	{ uint32_t _v = (ctx->d[3] & 0xFFFFu); st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 01224C movew %d3,%a5@+ */
L_01224E:;
	{ uint32_t _v = (ctx->d[2] & 0xFFFFu); set_w(&ctx->d[4], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 01224E movew %d2,%d4 */
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[4] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[4], _r); hle_insn(ctx, 4); } /* 012250 subqw #1,%d4 */
	{ uint32_t _v = ctx->a[0]; ctx->a[4] = _v; hle_insn(ctx, 4); } /* 012252 moveal %a0,%a4 */
L_012254:;
	{ uint32_t _p = ld16(ctx->a[4]); ctx->a[4] += 2; uint32_t _v = _p; set_w(&ctx->d[5], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 012254 movew %a4@+,%d5 */
	{ uint32_t _s = (ctx->d[7] & 0xFFFFu); uint32_t _d = (ctx->d[5] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[5], _r); hle_insn(ctx, 4); } /* 012256 addw %d7,%d5 */
	{ uint32_t _s = (ctx->d[6] & 0xFFFFu); uint32_t _d = (ctx->d[5] & 0xFFFFu); uint32_t _r = (_d | _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[5], _r); hle_insn(ctx, 4); } /* 012258 orw %d6,%d5 */
	{ uint32_t _v = (ctx->d[5] & 0xFFFFu); st16(ctx->a[1], _v); ctx->a[1] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 01225A movew %d5,%a1@+ */
	{ uint16_t _c = (uint16_t)ctx->d[4] - 1; set_w(&ctx->d[4], _c); if (_c != 0xFFFF) { hle_insn(ctx, 10); goto L_012254; } hle_insn(ctx, 14); } /* 01225C dbf %d4,0x12254 */
	{ ctx->a[0] = (ctx->a[0] + (uint32_t)(int32_t)80); hle_insn(ctx, 8); } /* 012260 lea %a0@(80),%a0 */
	{ uint32_t _s = 0x80u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 012264 addiw #128,%d1 */
	{ uint16_t _c = (uint16_t)ctx->d[3] - 1; set_w(&ctx->d[3], _c); if (_c != 0xFFFF) { hle_insn(ctx, 10); goto L_01224E; } hle_insn(ctx, 14); } /* 012268 dbf %d3,0x1224e */
	{ uint32_t _v = ctx->a[5]; st32(0xFFFF80BCu, _v); cc_logic32(ctx, _v); hle_insn(ctx, 20); } /* 01226C movel %a5,0xffff80bc */
	{ uint32_t _v = ctx->a[1]; st32(0xFFFF80B8u, _v); cc_logic32(ctx, _v); hle_insn(ctx, 20); } /* 012272 movel %a1,0xffff80b8 */
	hle_insn(ctx, 16); return; /* 012278 rts */
}

/* $01227A */
HLE_FUNC_PURE(0x01227A, VdpQueueTileRectMirror, 4000)
{
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[3], _r); hle_insn(ctx, 4); } /* 01227A subqw #1,%d3 */
	{ uint32_t _v = ld32(0xFFFF80B8u); ctx->a[1] = _v; hle_insn(ctx, 20); } /* 01227C moveal 0xffff80b8,%a1 */
	{ uint32_t _v = ld32(0xFFFF80BCu); ctx->a[5] = _v; hle_insn(ctx, 20); } /* 012282 moveal 0xffff80bc,%a5 */
	{ uint32_t _v = 0x6u; st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012288 movew #6,%a5@+ */
	{ uint32_t _v = (ctx->d[2] & 0xFFFFu); st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 01228C movew %d2,%a5@+ */
	{ uint32_t _v = ctx->a[1]; st32(ctx->a[5], _v); ctx->a[5] += 4; cc_logic32(ctx, _v); hle_insn(ctx, 12); } /* 01228E movel %a1,%a5@+ */
	{ uint32_t _v = (ctx->d[1] & 0xFFFFu); st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 012290 movew %d1,%a5@+ */
	{ uint32_t _v = (ctx->d[3] & 0xFFFFu); st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 012292 movew %d3,%a5@+ */
L_012294:;
	{ uint32_t _v = (ctx->d[2] & 0xFFFFu); set_w(&ctx->d[4], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 012294 movew %d2,%d4 */
	{ sh_w(ctx, 4, 'l', 1); hle_insn(ctx, 8); } /* 012296 aslw #1,%d4 */
	{ ctx->a[4] = (ctx->a[0] + (uint32_t)(int32_t)0 + (uint32_t)((int32_t)(int16_t)ctx->d[4])); hle_insn(ctx, 12); } /* 012298 lea %a0@(0,%d4:w),%a4 */
	{ uint32_t _v = (ctx->d[2] & 0xFFFFu); set_w(&ctx->d[4], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 01229C movew %d2,%d4 */
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[4] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[4], _r); hle_insn(ctx, 4); } /* 01229E subqw #1,%d4 */
L_0122A0:;
	{ ctx->a[4] -= 2; uint32_t _p = ld16(ctx->a[4]); uint32_t _v = _p; set_w(&ctx->d[5], _v); cc_logic16(ctx, _v); hle_insn(ctx, 10); } /* 0122A0 movew %a4@-,%d5 */
	{ uint32_t _s = (ctx->d[7] & 0xFFFFu); uint32_t _d = (ctx->d[5] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[5], _r); hle_insn(ctx, 4); } /* 0122A2 addw %d7,%d5 */
	{ uint32_t _s = (ctx->d[6] & 0xFFFFu); uint32_t _d = (ctx->d[5] & 0xFFFFu); uint32_t _r = (_d | _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[5], _r); hle_insn(ctx, 4); } /* 0122A4 orw %d6,%d5 */
	{ uint32_t _s = 0x800u; uint32_t _d = (ctx->d[5] & 0xFFFFu); uint32_t _r = (_d ^ _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[5], _r); hle_insn(ctx, 8); } /* 0122A6 eoriw #2048,%d5 */
	{ uint32_t _v = (ctx->d[5] & 0xFFFFu); st16(ctx->a[1], _v); ctx->a[1] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0122AA movew %d5,%a1@+ */
	{ uint16_t _c = (uint16_t)ctx->d[4] - 1; set_w(&ctx->d[4], _c); if (_c != 0xFFFF) { hle_insn(ctx, 10); goto L_0122A0; } hle_insn(ctx, 14); } /* 0122AC dbf %d4,0x122a0 */
	{ ctx->a[0] = (ctx->a[0] + (uint32_t)(int32_t)80); hle_insn(ctx, 8); } /* 0122B0 lea %a0@(80),%a0 */
	{ uint32_t _s = 0x80u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 0122B4 addiw #128,%d1 */
	{ uint16_t _c = (uint16_t)ctx->d[3] - 1; set_w(&ctx->d[3], _c); if (_c != 0xFFFF) { hle_insn(ctx, 10); goto L_012294; } hle_insn(ctx, 14); } /* 0122B8 dbf %d3,0x12294 */
	{ uint32_t _v = ctx->a[5]; st32(0xFFFF80BCu, _v); cc_logic32(ctx, _v); hle_insn(ctx, 20); } /* 0122BC movel %a5,0xffff80bc */
	{ uint32_t _v = ctx->a[1]; st32(0xFFFF80B8u, _v); cc_logic32(ctx, _v); hle_insn(ctx, 20); } /* 0122C2 movel %a1,0xffff80b8 */
	hle_insn(ctx, 16); return; /* 0122C8 rts */
}

/* $012BB0 */
HLE_FUNC_PURE(0x012BB0, PickupsRun, 4000)
{
	{ uint32_t _v = ld16(0xFFFF80E0u); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 012BB0 movew 0xffff80e0,%d7 */
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[7] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[7], _r); hle_insn(ctx, 4); } /* 012BB6 subqw #1,%d7 */
	if (bw(ctx, c_n(ctx))) goto L_012C9E; /* 012BB8 bmiw 0x12c9e */
	{ ctx->a[6] = 0xFFFF9AB2u; hle_insn(ctx, 12); } /* 012BBC lea 0xffff9ab2,%fp */
L_012BC2:;
	{ uint32_t _v = ld16(ctx->a[6]); set_w(&ctx->d[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 012BC2 movew %fp@,%d6 */
	if (bw(ctx, c_z(ctx))) goto L_012CBC; /* 012BC4 beqw 0x12cbc */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)22); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 012BC8 addqw #1,%fp@(22) */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 0) & 1) ? 0 : CC_Z); hle_insn(ctx, 12); } /* 012BCC btst #0,%fp@ */
	if (bs(ctx, !c_z(ctx))) goto L_012C18; /* 012BD0 bnes 0x12c18 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 4) & 1) ? 0 : CC_Z); hle_insn(ctx, 12); } /* 012BD2 btst #4,%fp@ */
	if (bw(ctx, !c_z(ctx))) goto L_012CE2; /* 012BD6 bnew 0x12ce2 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)20)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012BDA movew %fp@(20),%d0 */
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 012BDE subqw #1,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)20), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012BE0 movew %d0,%fp@(20) */
	{ uint32_t _s = 0x96u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 012BE4 cmpiw #150,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_012BEE; /* 012BE8 bnes 0x12bee */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 1) & 1) ? 0 : CC_Z); st8(_a, _o & 0xFD); hle_insn(ctx, 16); } /* 012BEA bclr #1,%fp@ */
L_012BEE:;
	{ uint32_t _s = 0x32u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 012BEE cmpiw #50,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_012BF8; /* 012BF2 bnes 0x12bf8 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 2) & 1) ? 0 : CC_Z); st8(_a, _o | 0x04); hle_insn(ctx, 16); } /* 012BF4 bset #2,%fp@ */
L_012BF8:;
	{ cc_logic16(ctx, (ctx->d[0] & 0xFFFFu)); hle_insn(ctx, 4); } /* 012BF8 tstw %d0 */
	if (bs(ctx, c_z(ctx))) goto L_012C08; /* 012BFA beqs 0x12c08 */
	{ hle_insn(ctx, 18); ObjScrollX(ctx); } /* 012BFC jsr %pc@(0x12316) */
	{ uint32_t _s = 0xFFF0u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)2); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 012C00 cmpiw #-16,%fp@(2) */
	if (bs(ctx, c_gt(ctx))) goto L_012C14; /* 012C06 bgts 0x12c14 */
L_012C08:;
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 012C08 clrw %fp@ */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80E0u; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 012C0A subqw #1,0xffff80e0 */
	hle_insn(ctx, 10); goto L_012C96; /* 012C10 braw 0x12c96 */
L_012C14:;
	hle_insn(ctx, 10); goto L_012C54; /* 012C14 braw 0x12c54 */
L_012C18:;
	{ uint32_t _v = ld8((ctx->a[6] + (uint32_t)(int32_t)14)); set_b(&ctx->d[0], _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 012C18 moveb %fp@(14),%d0 */
	{ set_w(&ctx->d[0], (uint32_t)(int32_t)(int8_t)ctx->d[0]); cc_logic16(ctx, ctx->d[0]); hle_insn(ctx, 4); } /* 012C1C extw %d0 */
	{ uint32_t _v = ld8((ctx->a[6] + (uint32_t)(int32_t)10)); set_b(&ctx->d[1], _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 012C1E moveb %fp@(10),%d1 */
	{ uint32_t _v = ld8((ctx->a[6] + (uint32_t)(int32_t)15)); set_b(&ctx->d[2], _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 012C22 moveb %fp@(15),%d2 */
	{ set_w(&ctx->d[2], (uint32_t)(int32_t)(int8_t)ctx->d[2]); cc_logic16(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 012C26 extw %d2 */
	{ uint32_t _v = ld8((ctx->a[6] + (uint32_t)(int32_t)12)); set_b(&ctx->d[3], _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 012C28 moveb %fp@(12),%d3 */
	{ ctx->d[4] = 0x3u; cc_logic32(ctx, ctx->d[4]); hle_insn(ctx, 4); } /* 012C2C moveq #3,%d4 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 5) & 1) ? 0 : CC_Z); hle_insn(ctx, 12); } /* 012C2E btst #5,%fp@ */
	if (bs(ctx, c_z(ctx))) goto L_012C36; /* 012C32 beqs 0x12c36 */
	{ ctx->d[4] = 0x1u; cc_logic32(ctx, ctx->d[4]); hle_insn(ctx, 4); } /* 012C34 moveq #1,%d4 */
L_012C36:;
	{ uint32_t _s = (ctx->d[1] & 0xFFu); uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)11); uint32_t _d = ld8(_da); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 8, 1); st8(_da, _r); hle_insn(ctx, 16); } /* 012C36 addb %d1,%fp@(11) */
	if (bs(ctx, !c_c(ctx))) goto L_012C40; /* 012C3A bccs 0x12c40 */
	{ uint32_t _s = (ctx->d[0] & 0xFFFFu); uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)2); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 012C3C addw %d0,%fp@(2) */
L_012C40:;
	{ uint32_t _s = (ctx->d[3] & 0xFFu); uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)13); uint32_t _d = ld8(_da); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 8, 1); st8(_da, _r); hle_insn(ctx, 16); } /* 012C40 addb %d3,%fp@(13) */
	if (bs(ctx, !c_c(ctx))) goto L_012C4A; /* 012C44 bccs 0x12c4a */
	{ uint32_t _s = (ctx->d[2] & 0xFFFFu); uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)4); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 012C46 addw %d2,%fp@(4) */
L_012C4A:;
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)20); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 012C4A subqw #1,%fp@(20) */
	if (bs(ctx, c_z(ctx))) goto L_012CC4; /* 012C4E beqs 0x12cc4 */
	{ uint16_t _c = (uint16_t)ctx->d[4] - 1; set_w(&ctx->d[4], _c); if (_c != 0xFFFF) { hle_insn(ctx, 10); goto L_012C36; } hle_insn(ctx, 14); } /* 012C50 dbf %d4,0x12c36 */
L_012C54:;
	{ ctx->d[1] = 0x0u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 012C54 moveq #0,%d1 */
	{ uint32_t _v = ld8((ctx->a[6] + (uint32_t)(int32_t)1)); set_b(&ctx->d[1], _v); cc_logic8(ctx, _v); hle_insn(ctx, 12); } /* 012C56 moveb %fp@(1),%d1 */
	{ uint32_t _s = 0xAu; uint32_t _d = (ctx->d[1] & 0xFFu); alu(ctx, OP_SUB, _d, _s, 8, 0); hle_insn(ctx, 8); } /* 012C5A cmpib #10,%d1 */
	if (bs(ctx, !c_z(ctx))) goto L_012C78; /* 012C5E bnes 0x12c78 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)20)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012C60 movew %fp@(20),%d2 */
	{ uint32_t _s = 0x18u; uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[2], _r); hle_insn(ctx, 8); } /* 012C64 andiw #24,%d2 */
	{ sh_w(ctx, 2, 'a', 2); hle_insn(ctx, 10); } /* 012C68 asrw #2,%d2 */
	{ uint32_t _v = ld16((0x012C70u + (uint32_t)((int32_t)(int16_t)ctx->d[2]))); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 14); } /* 012C6A movew %pc@(0x12c70,%d2:w),%d1 */
	hle_insn(ctx, 10); goto L_012C92; /* 012C6E bras 0x12c92 */
L_012C78:;
	{ uint32_t _s = (ctx->d[1] & 0xFFFFu); uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 4); } /* 012C78 addw %d1,%d1 */
	{ uint32_t _v = ld16((0x012CA0u + (uint32_t)((int32_t)(int16_t)ctx->d[1]))); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 14); } /* 012C7A movew %pc@(0x12ca0,%d1:w),%d1 */
	{ cc_set(ctx, CC_Z, ((ctx->d[6] >> 9) & 1) ? 0 : CC_Z); hle_insn(ctx, 10); } /* 012C7E btst #9,%d6 */
	if (bs(ctx, c_z(ctx))) goto L_012C92; /* 012C82 beqs 0x12c92 */
	{ uint32_t _v = ld16(0xFFFFA0EEu); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 012C84 movew 0xffffa0ee,%d2 */
	{ cc_set(ctx, CC_Z, ((ctx->d[2] >> 3) & 1) ? 0 : CC_Z); hle_insn(ctx, 10); } /* 012C8A btst #3,%d2 */
	if (bs(ctx, !c_z(ctx))) goto L_012C92; /* 012C8E bnes 0x12c92 */
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 4); } /* 012C90 addqw #1,%d1 */
L_012C92:;
	{ hle_insn(ctx, 18); ObjQueueSprite(ctx); } /* 012C92 jsr %pc@(0x13228) */
L_012C96:;
	{ ctx->a[6] = (ctx->a[6] + (uint32_t)(int32_t)26); hle_insn(ctx, 8); } /* 012C96 lea %fp@(26),%fp */
	{ uint16_t _c = (uint16_t)ctx->d[7] - 1; set_w(&ctx->d[7], _c); if (_c != 0xFFFF) { hle_insn(ctx, 10); goto L_012BC2; } hle_insn(ctx, 14); } /* 012C9A dbf %d7,0x12bc2 */
L_012C9E:;
	hle_insn(ctx, 16); return; /* 012C9E rts */
L_012CBC:;
	{ ctx->a[6] = (ctx->a[6] + (uint32_t)(int32_t)26); hle_insn(ctx, 8); } /* 012CBC lea %fp@(26),%fp */
	hle_insn(ctx, 10); goto L_012BC2; /* 012CC0 braw 0x12bc2 */
L_012CC4:;
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 1) & 1) ? 0 : CC_Z); hle_insn(ctx, 12); } /* 012CC4 btst #1,%fp@ */
	if (bs(ctx, c_z(ctx))) goto L_012CD8; /* 012CC8 beqs 0x12cd8 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 0) & 1) ? 0 : CC_Z); st8(_a, _o & 0xFE); hle_insn(ctx, 16); } /* 012CCA bclr #0,%fp@ */
	{ uint32_t _v = 0x64u; st16((ctx->a[6] + (uint32_t)(int32_t)20), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 012CCE movew #100,%fp@(20) */
	hle_insn(ctx, 10); goto L_012C54; /* 012CD4 braw 0x12c54 */
L_012CD8:;
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 012CD8 clrw %fp@ */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80E0u; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 012CDA subqw #1,0xffff80e0 */
	hle_insn(ctx, 10); goto L_012C96; /* 012CE0 bras 0x12c96 */
L_012CE2:;
	{ hle_insn(ctx, 18); sub_00D236(ctx); } /* 012CE2 jsr %pc@(0xd236) */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 012CE6 movew %fp@(4),%d0 */
	{ uint32_t _s = ld16((ctx->a[6] + (uint32_t)(int32_t)24)); uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 12); } /* 012CEA cmpw %fp@(24),%d0 */
	if (bw(ctx, !c_ge(ctx))) goto L_012C54; /* 012CEE bltw 0x12c54 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 4) & 1) ? 0 : CC_Z); st8(_a, _o & 0xEF); hle_insn(ctx, 16); } /* 012CF2 bclr #4,%fp@ */
	{ uint32_t _v = 0x64u; st16((ctx->a[6] + (uint32_t)(int32_t)20), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 012CF6 movew #100,%fp@(20) */
	hle_insn(ctx, 10); goto L_012C54; /* 012CFC braw 0x12c54 */
}

/* $00C48E */
HLE_FUNC_PURE(0x00C48E, ObjPathStep, 600)
{
	sub_00C48E(ctx);
}
