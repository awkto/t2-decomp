/* BgStreamLayers $00498A: one pass per background layer record (a0 = the layer list, a1/a2/a6 the
 * position tables, d0..d2 the plane/stride): when the scroll position crossed a block it queues the new
 * column/row with VdpQueueTileRect (or the Mirror / FillRows variants) after saving the registers.
 *
 * One function (HLE_FUNC_YIELD): every instruction is charged right after its effect, the jsr's are direct
 * C calls through hle_callc (the VdpQueue routines are C), so the frame loop can interrupt the pass at any
 * instruction boundary and the call never needs the 68k. Generated instruction by instruction from the
 * disassembly by tools/py/piecegen.py (bgstream, whole mode); `costcheck.py` checks every charge. */
#include "hle.h"

#include <stdint.h>

enum { OP_ADD, OP_SUB };

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

/* asl ('L') / lsl ('l') / lsr ('r') / asr ('a') by an immediate count (1..8) on a register */
static uint32_t shf(hle_ctx *c, char kind, uint32_t v, int n, int bits)
{
	uint32_t m = (bits == 32) ? 0xFFFFFFFFu : ((1u << bits) - 1), sg = 1u << (bits - 1);
	uint32_t res = v & m;
	unsigned cb = 0, ov = 0;
	for (int k = 0; k < n; k++) {
		if (kind == 'l' || kind == 'L') {
			cb = (res & sg) ? 1 : 0;
			res = (res << 1) & m;
			if (kind == 'L' && ((res & sg) != 0) != (cb != 0))
				ov = 1;
		} else if (kind == 'r') {
			cb = res & 1;
			res >>= 1;
		} else {
			cb = res & 1;
			res = (res >> 1) | (res & sg);
		}
	}
	unsigned f = (cb ? CC_C | CC_X : 0) | ((res & sg) ? CC_N : 0) | (res ? 0 : CC_Z) | (ov ? CC_V : 0);
	cc_set(c, CC_X | CC_N | CC_Z | CC_V | CC_C, f);
	return res;
}

#define COND_EQ ((ctx->sr & CC_Z) != 0)
#define COND_NE ((ctx->sr & CC_Z) == 0)
#define COND_CC ((ctx->sr & CC_C) == 0)
#define COND_CS ((ctx->sr & CC_C) != 0)
#define COND_PL ((ctx->sr & CC_N) == 0)
#define COND_MI ((ctx->sr & CC_N) != 0)
#define COND_VC ((ctx->sr & CC_V) == 0)
#define COND_VS ((ctx->sr & CC_V) != 0)
#define COND_HI ((ctx->sr & (CC_C | CC_Z)) == 0)
#define COND_LS ((ctx->sr & (CC_C | CC_Z)) != 0)
#define COND_GE (((ctx->sr & CC_N) != 0) == ((ctx->sr & CC_V) != 0))
#define COND_LT (((ctx->sr & CC_N) != 0) != ((ctx->sr & CC_V) != 0))
#define COND_GT (!(ctx->sr & CC_Z) && COND_GE)
#define COND_LE (((ctx->sr & CC_Z) != 0) || COND_LT)

/* work RAM or ROM by address (the code reads both: slot fields and the ROM tables) */
static inline int is_ram(uint32_t a) { return (a & 0xFFFFFFu) >= 0xE00000u; }
static inline uint8_t ld8(uint32_t a) { return is_ram(a) ? RAM8(a) : ROM8(a); }
static inline uint16_t ld16(uint32_t a) { return is_ram(a) ? RAM16(a) : ROM16(a); }
static inline uint32_t ld32(uint32_t a) { return is_ram(a) ? RAM32(a) : ROM32(a); }
static inline void st8(uint32_t a, uint32_t v) { RAM8(a) = (uint8_t)v; }
static inline void st16(uint32_t a, uint32_t v) { RAM16(a) = (uint16_t)v; }
static inline void st32(uint32_t a, uint32_t v) { RAM32_W(a, v); }


HLE_FUNC_YIELD(0x00498A, BgStreamLayers, 4000)
{
L_00498A:
	{ uint32_t eaS = ctx->a[2]; ctx->a[2] += 4; ctx->a[3] = ld32(eaS); } hle_insn(ctx, 12);  /* 00498A moveal %a2@+,%a3 */
	{ uint32_t eaS = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00498C movew %a0@+,%d7 */
	if (COND_LE) { hle_insn(ctx, 10); goto L_004A62; } hle_insn(ctx, 12);  /* 00498E blew 4a62 */
	{ uint32_t eaD = ctx->a[0]; ctx->a[0] += 2; cc_logic16(ctx, ld16(eaD)); } hle_insn(ctx, 8);  /* 004992 tstw %a0@+ */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_004A52; } hle_insn(ctx, 12);  /* 004994 beqw 4a52 */
	{ uint32_t v = (ctx->a[3] & 0xFFFFFFFFu); ctx->d[6] = v; cc_logic32(ctx, v); } hle_insn(ctx, 4);  /* 004998 movel %a3,%d6 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_004A52; } hle_insn(ctx, 12);  /* 00499A beqw 4a52 */
	{ uint32_t eaS = ctx->a[1]; uint32_t v = ld16(eaS); set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00499E movew %a1@,%d6 */
	{ uint32_t v = (ctx->d[6] & 0xFFFFu); set_w(&ctx->d[5], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 0049A0 movew %d6,%d5 */
	{ uint32_t eaS = ctx->a[6]; alu(ctx, OP_SUB, (ctx->d[6] & 0xFFFFu), ld16(eaS), 16, 0); } hle_insn(ctx, 8);  /* 0049A2 cmpw %fp@,%d6 */
	if (COND_LT) { hle_insn(ctx, 10); goto L_004A52; } hle_insn(ctx, 12);  /* 0049A4 bltw 4a52 */
	{ uint32_t eaS = ctx->a[6]; uint32_t v = ld16(eaS); set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0049A8 movew %fp@,%d6 */
	{ uint32_t eaD = ctx->a[6]; uint32_t v = alu(ctx, OP_ADD, ld16(eaD), (ctx->d[1] & 0xFFFFu), 16, 1); st16(eaD, v); } hle_insn(ctx, 12);  /* 0049AA addw %d1,%fp@ */
	{ uint32_t eaD = 0xFFFFA238u; uint32_t v = (ctx->d[1] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 0049AC movew %d1,ffffa238 */
	{ uint32_t v = (ctx->d[5] & 0xFFFFu); set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 0049B2 movew %d5,%d6 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[6] & 0xFFFFu), (ctx->d[1] & 0xFFFFu), 16, 1); set_w(&ctx->d[6], v); } hle_insn(ctx, 4);  /* 0049B4 subw %d1,%d6 */
	{ uint32_t v = shf(ctx, 'a', (ctx->d[6] & 0xFFFFu), 2, 16); set_w(&ctx->d[6], v); } hle_insn(ctx, 10);  /* 0049B6 asrw #2,%d6 */
	{ uint32_t v = ((ctx->d[6] & 0xFFFFu) & 0x7Fu) & 0xFFFFu; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0049B8 andiw #127,%d6 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[6] & 0xFFFFu), (ctx->d[0] & 0xFFFFu), 16, 1); set_w(&ctx->d[6], v); } hle_insn(ctx, 4);  /* 0049BC addw %d0,%d6 */
	{ alu(ctx, OP_SUB, (ctx->d[2] & 0xFFFFu), 0x8u, 16, 0); } hle_insn(ctx, 8);  /* 0049BE cmpiw #8,%d2 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0049C8; } hle_insn(ctx, 8);  /* 0049C2 bnes 49c8 */
	{ uint32_t v = shf(ctx, 'a', (ctx->d[5] & 0xFFFFu), 6, 16); set_w(&ctx->d[5], v); } hle_insn(ctx, 18);  /* 0049C4 asrw #6,%d5 */
	hle_insn(ctx, 10); goto L_0049CC;  /* 0049C6 bras 49cc */
L_0049C8:
	{ uint32_t v = shf(ctx, 'a', (ctx->d[5] & 0xFFFFu), 7, 16); set_w(&ctx->d[5], v); } hle_insn(ctx, 20);  /* 0049C8 asrw #7,%d5 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[5] & 0xFFFFu), 0x3u, 16, 1); set_w(&ctx->d[5], v); } hle_insn(ctx, 4);  /* 0049CA addqw #3,%d5 */
L_0049CC:
	{ ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[6]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[5]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[4]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[3]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[2]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[1]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[0]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[7]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[3]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[2]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[1]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[0]); } hle_movem_l(ctx, 104, 12);  /* 0049CC moveml %d0-%d3/%d7-%fp,%sp@- */
	{ uint32_t v = (ctx->d[7] & 0xFFFFu); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 0049D0 movew %d7,%d3 */
	{ uint32_t v = (ctx->d[6] & 0xFFFFu); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 0049D2 movew %d6,%d1 */
	{ uint32_t eaS = 0xFFFFA0AAu; uint32_t v = ld16(eaS); set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 0049D4 movew ffffa0aa,%d6 */
	{ uint32_t eaS = ctx->a[3] + (uint32_t)(int32_t)(0) + (uint32_t)(int32_t)(int16_t)ctx->d[5]; uint32_t v = ld8(eaS); set_b(&ctx->d[5], v); cc_logic8(ctx, v); } hle_insn(ctx, 14);  /* 0049DA moveb %a3@(0,%d5:w),%d5 */
	{ set_w(&ctx->d[5], (uint32_t)(int32_t)(int8_t)ctx->d[5]); cc_logic16(ctx, ctx->d[5] & 0xFFFFu); } hle_insn(ctx, 4);  /* 0049DE extw %d5 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0049EA; } hle_insn(ctx, 8);  /* 0049E0 bnes 49ea */
	{ uint32_t eaD = 0xFFFFA234u; uint32_t v = 0xFFu; st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 20);  /* 0049E2 moveb #-1,ffffa234 */
L_0049EA:
	if (COND_PL) { hle_insn(ctx, 10); goto L_004A16; } hle_insn(ctx, 8);  /* 0049EA bpls 4a16 */
	{ alu(ctx, OP_SUB, (ctx->d[5] & 0xFFu), 0x80u, 8, 0); } hle_insn(ctx, 8);  /* 0049EC cmpib #-128,%d5 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0049FA; } hle_insn(ctx, 8);  /* 0049F0 bnes 49fa */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_callc(ctx, 0x012212); }  /* 0049F2 jsr 12212 */
	hle_insn(ctx, 10); goto L_004A4E;  /* 0049F8 bras 4a4e */
L_0049FA:
	{ uint32_t v = alu(ctx, OP_SUB, 0, (ctx->d[5] & 0xFFFFu), 16, 1); set_w(&ctx->d[5], v); } hle_insn(ctx, 4);  /* 0049FA negw %d5 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[5] & 0xFFFFu), (ctx->d[5] & 0xFFFFu), 16, 1); set_w(&ctx->d[5], v); } hle_insn(ctx, 4);  /* 0049FC addw %d5,%d5 */
	{ ctx->d[7] = 0xFFFFFFFFu; cc_logic32(ctx, ctx->d[7]); } hle_insn(ctx, 4);  /* 0049FE moveq #-1,%d7 */
	{ uint32_t eaS = ctx->a[5] + (uint32_t)(int32_t)(0) + (uint32_t)(int32_t)(int16_t)ctx->d[5]; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 14);  /* 004A00 movew %a5@(0,%d5:w),%d7 */
	{ ctx->a[3] = (ctx->d[7] & 0xFFFFFFFFu); } hle_insn(ctx, 4);  /* 004A04 moveal %d7,%a3 */
	{ uint32_t eaS = ctx->a[3]; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 004A06 movew %a3@,%d7 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[5] & 0xFFFFu), (ctx->d[5] & 0xFFFFu), 16, 1); set_w(&ctx->d[5], v); } hle_insn(ctx, 4);  /* 004A08 addw %d5,%d5 */
	{ uint32_t eaS = ctx->a[4] + (uint32_t)(int32_t)(0) + (uint32_t)(int32_t)(int16_t)ctx->d[5]; ctx->a[0] = ld32(eaS); } hle_insn(ctx, 18);  /* 004A0A moveal %a4@(0,%d5:w),%a0 */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_callc(ctx, 0x01227A); }  /* 004A0E jsr 1227a */
	hle_insn(ctx, 10); goto L_004A4E;  /* 004A14 bras 4a4e */
L_004A16:
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[5] & 0xFFFFu), (ctx->d[5] & 0xFFFFu), 16, 1); set_w(&ctx->d[5], v); } hle_insn(ctx, 4);  /* 004A16 addw %d5,%d5 */
	{ ctx->d[7] = 0xFFFFFFFFu; cc_logic32(ctx, ctx->d[7]); } hle_insn(ctx, 4);  /* 004A18 moveq #-1,%d7 */
	{ uint32_t eaS = ctx->a[5] + (uint32_t)(int32_t)(0) + (uint32_t)(int32_t)(int16_t)ctx->d[5]; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 14);  /* 004A1A movew %a5@(0,%d5:w),%d7 */
	{ ctx->a[3] = (ctx->d[7] & 0xFFFFFFFFu); } hle_insn(ctx, 4);  /* 004A1E moveal %d7,%a3 */
	{ uint32_t eaS = ctx->a[3]; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 004A20 movew %a3@,%d7 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[5] & 0xFFFFu), (ctx->d[5] & 0xFFFFu), 16, 1); set_w(&ctx->d[5], v); } hle_insn(ctx, 4);  /* 004A22 addw %d5,%d5 */
	{ uint32_t eaS = ctx->a[4] + (uint32_t)(int32_t)(0) + (uint32_t)(int32_t)(int16_t)ctx->d[5]; ctx->a[0] = ld32(eaS); } hle_insn(ctx, 18);  /* 004A24 moveal %a4@(0,%d5:w),%a0 */
	{ uint32_t eaD = 0xFFFF80CCu; alu(ctx, OP_SUB, ld16(eaD), 0x6u, 16, 0); } hle_insn(ctx, 20);  /* 004A28 cmpiw #6,ffff80cc */
	if (COND_NE) { hle_insn(ctx, 10); goto L_004A38; } hle_insn(ctx, 8);  /* 004A30 bnes 4a38 */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_callc(ctx, 0x013470); }  /* 004A32 jsr 13470 */
L_004A38:
	{ uint32_t eaD = 0xFFFF80CCu; alu(ctx, OP_SUB, ld16(eaD), 0x5u, 16, 0); } hle_insn(ctx, 20);  /* 004A38 cmpiw #5,ffff80cc */
	if (COND_NE) { hle_insn(ctx, 10); goto L_004A48; } hle_insn(ctx, 8);  /* 004A40 bnes 4a48 */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_callc(ctx, 0x01385A); }  /* 004A42 jsr 1385a */
L_004A48:
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_callc(ctx, 0x012234); }  /* 004A48 jsr 12234 */
L_004A4E:
	{ ctx->d[0] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->d[1] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->d[2] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->d[3] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->d[7] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->a[0] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->a[1] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->a[2] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->a[3] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->a[4] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->a[5] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->a[6] = ld32(ctx->a[7]); ctx->a[7] += 4; } hle_movem_l(ctx, 108, 12);  /* 004A4E moveml %sp@+,%d0-%d3/%d7-%fp */
L_004A52:
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[7] & 0xFFFFu), (ctx->d[7] & 0xFFFFu), 16, 1); set_w(&ctx->d[7], v); } hle_insn(ctx, 4);  /* 004A52 addw %d7,%d7 */
	{ uint32_t eaS = ctx->a[1] + (uint32_t)(int32_t)(0) + (uint32_t)(int32_t)(int16_t)ctx->d[7]; ctx->a[1] = eaS; } hle_insn(ctx, 12);  /* 004A54 lea %a1@(0,%d7:w),%a1 */
	{ uint32_t v = shf(ctx, 'L', (ctx->d[7] & 0xFFFFu), 6, 16); set_w(&ctx->d[7], v); } hle_insn(ctx, 18);  /* 004A58 aslw #6,%d7 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), (ctx->d[7] & 0xFFFFu), 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 4);  /* 004A5A addw %d7,%d0 */
	{ ctx->a[6] += 2; } hle_insn(ctx, 8);  /* 004A5C addql #2,%fp */
	hle_insn(ctx, 10); goto L_00498A;  /* 004A5E braw 498a */
L_004A62:
	hle_insn(ctx, 16); return;  /* 004A62 rts */
}
