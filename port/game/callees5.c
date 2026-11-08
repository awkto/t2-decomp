/* More whole-function ports of small 68k helpers that coroutines called as nested 68k (found with T2_HLE_DECLINES over the 92 states): sub_0099C8, sub_00636E, sub_0122CA, sub_0072EA, sub_010D0C.
 *
 * These routines call back into code that waits for VBlank, prints text, spawns sprites or loads
 * level data (WaitFrames, WaitVBlank, ReadInput, PalFade, TextPrint, QueueSprite, VdpQueueTileRect, Random,
 * the sound requests): all of that stays whatever it is (C where it is registered, else the real 68k).
 * The routines are registered as HLE_RANGE_PURE pieces that cover only their OWN instructions: a piece
 * runs from an entry (the routine start, or the return address of a `jsr`/`bsr`) up to the next
 * `jsr`/`bsr`/`jmp`/`rts` and leaves with hle_jump to that instruction, which the 68k executes (so every
 * callee is entered exactly as in the original and a callee that is itself replaced is dispatched on its
 * own). A branch that leaves the routine's range is an exit as well. The pieces touch only work RAM, ROM
 * tables and registers, so the layer measures each one with a dry run and declines it when a VBlank would
 * fall inside: no hand-made bound is needed. Stack-pushing pieces declare HLE_EXIT_SP exits.
 *
 * Generated instruction by instruction from the disassembly by tools/py/piecegen.py (objdump with
 * relocations and the GPGX opcode cycle table); `costcheck.py` checks every charge. All the pieces are in
 * one function, pc_run(), entered through a switch on the entry address; branches inside the routines
 * are gotos. */
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


HLE_FUNC_YIELD(0x00636E, Sub00636E, 4000)
{
L_00636E:
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x0072CE); }  /* 00636E jsr %pc@(72ce) */
	{ unsigned b = 15; uint32_t m = 1u << b; cc_set(ctx, CC_Z, (ctx->d[5] & m) ? 0 : CC_Z); ctx->d[5] &= ~m; hle_insn(ctx, 12 + ((b > 15) ? 2 : 0)); }  /* 006372 bclr #15,%d5 */
	{ uint32_t eaD = ctx->a[6]; uint32_t v = (ctx->d[5] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 006376 movew %d5,%fp@ */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0xDB5Au; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 006378 movel #56154,%fp@(32) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(58); uint32_t v = alu(ctx, OP_ADD, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 16);  /* 006380 addqw #1,%a4@(58) */
	hle_insn(ctx, 16); return;  /* 006384 rts */
}

HLE_FUNC_YIELD(0x0072EA, Sub0072EA, 4000)
{
L_0072EA:
	{ ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[1]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[0]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[0]); } hle_movem_l(ctx, 32, 3);  /* 0072EA moveml %d0/%a0-%a1,%sp@- */
	{ uint32_t v = 0x200u; ctx->d[0] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 0072EE movel #512,%d0 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x007376); return;  /* 0072F4 braw 7376 */
}

HLE_FUNC_YIELD(0x0099C8, Sub0099C8, 4000)
{
L_0099C8:
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x0099D4); }  /* 0099C8 bsrw 99d4 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(38); uint32_t v = 0xFFFFu; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 0099CC movew #-1,%fp@(38) */
	hle_insn(ctx, 16); return;  /* 0099D2 rts */
}

HLE_FUNC_YIELD(0x010D0C, Sub010D0C, 4000)
{
L_010D0C:
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x00D0F8); }  /* 010D0C jsr %pc@(d0f8) */
	{ ctx->d[2] = 0x00000000u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 010D10 moveq #0,%d2 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x009EF8); }  /* 010D12 jsr %pc@(9ef8) */
	{ ctx->d[0] = 0x00000000u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 010D16 moveq #0,%d0 */
	{ ctx->d[1] = 0x00000000u; cc_logic32(ctx, ctx->d[1]); } hle_insn(ctx, 4);  /* 010D18 moveq #0,%d1 */
	{ hle_insn(ctx, 12); hle_jump(ctx, 0x0081B6); return; }  /* 010D1A jmp 81b6 */
}

HLE_FUNC_YIELD(0x0122CA, Sub0122CA, 4000)
{
L_0122CA:
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[3] & 0xFFFFu), 0x1u, 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 4);  /* 0122CA subqw #1,%d3 */
	{ uint32_t eaS = 0xFFFF80B8u; ctx->a[1] = ld32(eaS); } hle_insn(ctx, 20);  /* 0122CC moveal ffff80b8,%a1 */
	{ uint32_t eaS = 0xFFFF80BCu; ctx->a[5] = ld32(eaS); } hle_insn(ctx, 20);  /* 0122D2 moveal ffff80bc,%a5 */
	{ uint32_t eaD = ctx->a[5]; ctx->a[5] += 2; uint32_t v = 0x6u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 0122D8 movew #6,%a5@+ */
	{ uint32_t eaD = ctx->a[5]; ctx->a[5] += 2; uint32_t v = (ctx->d[2] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0122DC movew %d2,%a5@+ */
	{ uint32_t eaD = ctx->a[5]; ctx->a[5] += 4; uint32_t v = (ctx->a[1] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 0122DE movel %a1,%a5@+ */
	{ uint32_t eaD = ctx->a[5]; ctx->a[5] += 2; uint32_t v = (ctx->d[1] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0122E0 movew %d1,%a5@+ */
	{ uint32_t eaD = ctx->a[5]; ctx->a[5] += 2; uint32_t v = (ctx->d[3] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0122E2 movew %d3,%a5@+ */
L_0122E4:
	{ uint32_t v = (ctx->d[2] & 0xFFFFu); set_w(&ctx->d[4], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 0122E4 movew %d2,%d4 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[4] & 0xFFFFu), 0x1u, 16, 1); set_w(&ctx->d[4], v); } hle_insn(ctx, 4);  /* 0122E6 subqw #1,%d4 */
	{ ctx->a[4] = (ctx->a[0] & 0xFFFFFFFFu); } hle_insn(ctx, 4);  /* 0122E8 moveal %a0,%a4 */
L_0122EA:
	{ uint32_t eaS = ctx->a[4]; ctx->a[4] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[5], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0122EA movew %a4@+,%d5 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[5] & 0xFFFFu), (ctx->d[7] & 0xFFFFu), 16, 1); set_w(&ctx->d[5], v); } hle_insn(ctx, 4);  /* 0122EC addw %d7,%d5 */
	{ uint32_t v = ((ctx->d[5] & 0xFFFFu) | (ctx->d[6] & 0xFFFFu)) & 0xFFFFu; set_w(&ctx->d[5], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 0122EE orw %d6,%d5 */
	{ uint32_t eaD = ctx->a[1]; ctx->a[1] += 2; uint32_t v = (ctx->d[5] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0122F0 movew %d5,%a1@+ */
	{ uint16_t v = (uint16_t)ctx->d[4] - 1; set_w(&ctx->d[4], v); if (v != 0xFFFF) { hle_insn(ctx, 10); goto L_0122EA; } hle_insn(ctx, 14); }  /* 0122F2 dbf %d4,122ea */
	{ unsigned b = 0; uint32_t m = 1u << b; cc_set(ctx, CC_Z, (ctx->d[2] & m) ? 0 : CC_Z);  hle_insn(ctx, 10); }  /* 0122F6 btst #0,%d2 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0122FE; } hle_insn(ctx, 8);  /* 0122FA beqs 122fe */
	{ ctx->a[4] += 2; } hle_insn(ctx, 8);  /* 0122FC addql #2,%a4 */
L_0122FE:
	{ ctx->a[0] = (ctx->a[4] & 0xFFFFFFFFu); } hle_insn(ctx, 4);  /* 0122FE moveal %a4,%a0 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0x80u, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 012300 addiw #128,%d1 */
	{ uint16_t v = (uint16_t)ctx->d[3] - 1; set_w(&ctx->d[3], v); if (v != 0xFFFF) { hle_insn(ctx, 10); goto L_0122E4; } hle_insn(ctx, 14); }  /* 012304 dbf %d3,122e4 */
	{ uint32_t eaD = 0xFFFF80BCu; uint32_t v = (ctx->a[5] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 20);  /* 012308 movel %a5,ffff80bc */
	{ uint32_t eaD = 0xFFFF80B8u; uint32_t v = (ctx->a[1] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 20);  /* 01230E movel %a1,ffff80b8 */
	hle_insn(ctx, 16); return;  /* 012314 rts */
}

