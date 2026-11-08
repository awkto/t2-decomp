/* Whole-function ports of the last small 68k helpers that coroutines called as nested 68k: sub_00BE42, sub_00D444, sub_00D46E (the 16 ObjAlloc spawns of the truck), sub_009066.
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


HLE_FUNC_YIELD(0x009066, Sub009066, 4000)
{
L_009066:
	{ ctx->a[7] -= 2; st16(ctx->a[7], ctx->d[3]); ctx->a[7] -= 2; st16(ctx->a[7], ctx->d[2]); ctx->a[7] -= 2; st16(ctx->a[7], ctx->d[1]); ctx->a[7] -= 2; st16(ctx->a[7], ctx->d[0]); } hle_movem_w(ctx, 24, 4);  /* 009066 movemw %d0-%d3,%sp@- */
	{ uint32_t v = 0x80u; set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00906A movew #128,%d2 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x00909C); }  /* 00906E bsrw 909c */
	{ ctx->d[0] = (uint32_t)(int32_t)(int16_t)ld16(ctx->a[7]); ctx->a[7] += 2; ctx->d[1] = (uint32_t)(int32_t)(int16_t)ld16(ctx->a[7]); ctx->a[7] += 2; ctx->d[2] = (uint32_t)(int32_t)(int16_t)ld16(ctx->a[7]); ctx->a[7] += 2; ctx->d[3] = (uint32_t)(int32_t)(int16_t)ld16(ctx->a[7]); ctx->a[7] += 2; } hle_movem_w(ctx, 28, 4);  /* 009072 movemw %sp@+,%d0-%d3 */
	hle_insn(ctx, 16); return;  /* 009076 rts */
}

HLE_FUNC_YIELD(0x00BE42, Sub00BE42, 4000)
{
L_00BE42:
	{ ctx->a[7] -= 2; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->d[7] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BE42 movew %d7,%sp@- */
	{ uint32_t eaS = 0xFFFF80BCu; ctx->a[5] = ld32(eaS); } hle_insn(ctx, 20);  /* 00BE44 moveal ffff80bc,%a5 */
	{ uint32_t v = 0xC100u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BE4A movew #-16128,%d1 */
	{ uint32_t eaS = 0x00046C44u; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 00BE4E lea 46c44,%a0 */
	{ ctx->d[2] = 0x00000020u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 00BE54 moveq #32,%d2 */
	{ ctx->d[3] = 0x0000000Bu; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 00BE56 moveq #11,%d3 */
	{ uint32_t v = 0x0u; ctx->d[6] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 00BE58 movel #0,%d6 */
	{ uint32_t eaS = 0xFFFF9E38u; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00BE5E movew ffff9e38,%d7 */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_callc(ctx, 0x012234); }  /* 00BE64 jsr 12234 */
	{ uint32_t v = 0xC140u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BE6A movew #-16064,%d1 */
	{ uint32_t eaS = 0x00046C44u; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 00BE6E lea 46c44,%a0 */
	{ ctx->d[2] = 0x00000020u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 00BE74 moveq #32,%d2 */
	{ ctx->d[3] = 0x0000000Bu; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 00BE76 moveq #11,%d3 */
	{ uint32_t v = 0x0u; ctx->d[6] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 00BE78 movel #0,%d6 */
	{ uint32_t eaS = 0xFFFF9E38u; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00BE7E movew ffff9e38,%d7 */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_callc(ctx, 0x012234); }  /* 00BE84 jsr 12234 */
	{ uint32_t eaD = 0xFFFF80BCu; uint32_t v = (ctx->a[5] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 20);  /* 00BE8A movel %a5,ffff80bc */
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BE90 movew %sp@+,%d7 */
	hle_insn(ctx, 16); return;  /* 00BE92 rts */
}

HLE_FUNC_YIELD(0x00D444, Sub00D444, 4000)
{
L_00D444:
	{ ctx->a[7] -= 2; st16(ctx->a[7], ctx->d[2]); ctx->a[7] -= 2; st16(ctx->a[7], ctx->d[1]); ctx->a[7] -= 2; st16(ctx->a[7], ctx->d[0]); } hle_movem_w(ctx, 20, 3);  /* 00D444 movemw %d0-%d2,%sp@- */
	{ uint32_t v = shf(ctx, 'L', (ctx->d[1] & 0xFFFFu), 3, 16); set_w(&ctx->d[1], v); } hle_insn(ctx, 12);  /* 00D448 aslw #3,%d1 */
	{ uint32_t v = shf(ctx, 'L', (ctx->d[2] & 0xFFFFu), 3, 16); set_w(&ctx->d[2], v); } hle_insn(ctx, 12);  /* 00D44A aslw #3,%d2 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = alu(ctx, OP_ADD, (ctx->d[2] & 0xFFFFu), ld16(eaS), 16, 1); set_w(&ctx->d[2], v); } hle_insn(ctx, 12);  /* 00D44C addw %fp@(2),%d2 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[2] & 0xFFFFu), 0x68u, 16, 1); set_w(&ctx->d[2], v); } hle_insn(ctx, 8);  /* 00D450 subiw #104,%d2 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(4); uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), ld16(eaS), 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 12);  /* 00D454 addw %fp@(4),%d1 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[1] & 0xFFFFu), 0x50u, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 00D458 subiw #80,%d1 */
	{ uint32_t v = (ctx->d[2] & 0xFFFFu); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 00D45C movew %d2,%d0 */
	{ uint32_t v = 0x6u; set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00D45E movew #6,%d2 */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_callc(ctx, 0x01253C); }  /* 00D462 jsr 1253c */
	{ ctx->d[0] = (uint32_t)(int32_t)(int16_t)ld16(ctx->a[7]); ctx->a[7] += 2; ctx->d[1] = (uint32_t)(int32_t)(int16_t)ld16(ctx->a[7]); ctx->a[7] += 2; ctx->d[2] = (uint32_t)(int32_t)(int16_t)ld16(ctx->a[7]); ctx->a[7] += 2; } hle_movem_w(ctx, 24, 3);  /* 00D468 movemw %sp@+,%d0-%d2 */
	hle_insn(ctx, 16); return;  /* 00D46C rts */
}

HLE_FUNC_YIELD(0x00D46E, Sub00D46E, 4000)
{
L_00D46E:
	{ ctx->a[7] -= 4; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->a[6] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 00D46E movel %fp,%sp@- */
	{ ctx->d[4] = 0x0000000Fu; cc_logic32(ctx, ctx->d[4]); } hle_insn(ctx, 4);  /* 00D470 moveq #15,%d4 */
L_00D472:
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x00930E); }  /* 00D472 jsr %pc@(930e) */
	{ uint32_t eaD = ctx->a[6]; uint32_t v = 0x1Cu; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00D476 movew #28,%fp@ */
	{ uint32_t v = (ctx->d[4] & 0xFFFFu); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 00D47A movew %d4,%d0 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), (ctx->d[0] & 0xFFFFu), 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 4);  /* 00D47C addw %d0,%d0 */
	{ uint32_t eaS = 0x0000D424u + (uint32_t)(int32_t)(int16_t)ctx->d[0]; uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(14); uint32_t v = ld8(eaS); st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 22);  /* 00D47E moveb %pc@(d424,%d0:w),%fp@(14) */
	{ uint32_t eaS = 0x0000D425u + (uint32_t)(int32_t)(int16_t)ctx->d[0]; uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(15); uint32_t v = ld8(eaS); st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 22);  /* 00D484 moveb %pc@(d425,%d0:w),%fp@(15) */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), (ctx->d[0] & 0xFFFFu), 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 4);  /* 00D48A addw %d0,%d0 */
	{ uint32_t eaS = 0x0000D3E4u; ctx->a[1] = eaS; } hle_insn(ctx, 8);  /* 00D48C lea %pc@(d3e4),%a1 */
	{ uint32_t eaS = ctx->a[1] + (uint32_t)(int32_t)(1) + (uint32_t)(int32_t)(int16_t)ctx->d[0]; uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = ld8(eaS); st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 22);  /* 00D490 moveb %a1@(1,%d0:w),%fp@(2) */
	{ uint32_t eaS = ctx->a[1] + (uint32_t)(int32_t)(1) + (uint32_t)(int32_t)(int16_t)ctx->d[0]; uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(3); uint32_t v = ld8(eaS); st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 22);  /* 00D496 moveb %a1@(1,%d0:w),%fp@(3) */
	{ uint32_t eaS = ctx->a[1] + (uint32_t)(int32_t)(0) + (uint32_t)(int32_t)(int16_t)ctx->d[0]; uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(4); uint32_t v = ld8(eaS); st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 22);  /* 00D49C moveb %a1@(0,%d0:w),%fp@(4) */
	{ uint32_t eaS = ctx->a[1] + (uint32_t)(int32_t)(0) + (uint32_t)(int32_t)(int16_t)ctx->d[0]; uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(5); uint32_t v = ld8(eaS); st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 22);  /* 00D4A2 moveb %a1@(0,%d0:w),%fp@(5) */
	{ uint32_t eaS = ctx->a[1] + (uint32_t)(int32_t)(2) + (uint32_t)(int32_t)(int16_t)ctx->d[0]; uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(11); uint32_t v = ld8(eaS); st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 22);  /* 00D4A8 moveb %a1@(2,%d0:w),%fp@(11) */
	{ uint32_t eaS = ctx->a[1] + (uint32_t)(int32_t)(3) + (uint32_t)(int32_t)(int16_t)ctx->d[0]; uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(13); uint32_t v = ld8(eaS); st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 22);  /* 00D4AE moveb %a1@(3,%d0:w),%fp@(13) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0xD4E6u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 00D4B4 movel #54502,%fp@(32) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(28); uint32_t v = 0x627Au; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 00D4BC movel #25210,%fp@(28) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = 0x14u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00D4C4 movew #20,%fp@(36) */
	{ uint32_t eaD = 0xFFFF80DAu; uint32_t v = alu(ctx, OP_ADD, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 20);  /* 00D4CA addqw #1,ffff80da */
	{ uint16_t v = (uint16_t)ctx->d[4] - 1; set_w(&ctx->d[4], v); if (v != 0xFFFF) { hle_insn(ctx, 10); goto L_00D472; } hle_insn(ctx, 14); }  /* 00D4D0 dbf %d4,d472 */
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 4; ctx->a[6] = ld32(eaS); } hle_insn(ctx, 12);  /* 00D4D4 moveal %sp@+,%fp */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0xD0D4u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 00D4D6 movel #53460,%fp@(32) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = 0x32u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00D4DE movew #50,%fp@(36) */
	hle_insn(ctx, 16); return;  /* 00D4E4 rts */
}

