/* More small helper subroutines (hit/onhit, pickup, spark and sound-request wrappers)
 *
 * Small 68k helpers that the exact-timeline functions (HLE_F_YIELD) call, as C: one HLE_FUNC_YIELD each,
 * every instruction charged right after its effect, the jsr/bsr are direct C calls through hle_callc (a
 * branch or jmp out of the routine is hle_jump), so the frame loop can interrupt a caller and its callee
 * at any instruction boundary and no call needs the 68k. Generated instruction by instruction from the
 * disassembly by tools/py/piecegen.py (funcs mode); `costcheck.py` checks every charge. */
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


HLE_FUNC_YIELD(0x005A80, Sub005A80, 4000)
{
L_005A80:
	{ uint32_t eaS = 0x00005A8Cu; ctx->a[2] = eaS; } hle_insn(ctx, 8);  /* 005A80 lea %pc@(5a8c),%a2 */
	{ uint32_t eaS = 0x00005AB4u; ctx->a[3] = eaS; } hle_insn(ctx, 8);  /* 005A84 lea %pc@(5ab4),%a3 */
	{ hle_insn(ctx, 10); hle_jump(ctx, 0x005940); return; }  /* 005A88 jmp %pc@(5940) */
}

HLE_FUNC_YIELD(0x005A8C, Sub005A8C, 4000)
{
L_005A8C:
	{ uint32_t eaS = 0x00005A92u; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 005A8C lea %pc@(5a92),%a0 */
	hle_insn(ctx, 16); return;  /* 005A90 rts */
}

HLE_FUNC_YIELD(0x005A92, Sub005A92, 4000)
{
L_005A92:
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x0072F8); }  /* 005A92 jsr %pc@(72f8) */
	{ unsigned b = 15; uint32_t m = 1u << b; cc_set(ctx, CC_Z, (ctx->d[5] & m) ? 0 : CC_Z); ctx->d[5] &= ~m; hle_insn(ctx, 12 + ((b > 15) ? 2 : 0)); }  /* 005A96 bclr #15,%d5 */
	{ uint32_t eaD = ctx->a[6]; uint32_t v = (ctx->d[5] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 005A9A movew %d5,%fp@ */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(20); uint32_t v = 0x20u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 005A9C movew #32,%fp@(20) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0xB78Cu; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 005AA2 movel #46988,%fp@(32) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(50); uint32_t v = alu(ctx, OP_ADD, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 16);  /* 005AAA addqw #1,%a4@(50) */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x005A24); }  /* 005AAE jsr %pc@(5a24) */
	hle_insn(ctx, 16); return;  /* 005AB2 rts */
}

HLE_FUNC_YIELD(0x00635C, Sub00635C, 4000)
{
L_00635C:
	{ uint32_t eaS = 0x00006386u; ctx->a[3] = eaS; } hle_insn(ctx, 8);  /* 00635C lea %pc@(6386),%a3 */
	{ uint32_t eaS = 0x00006368u; ctx->a[2] = eaS; } hle_insn(ctx, 8);  /* 006360 lea %pc@(6368),%a2 */
	{ hle_insn(ctx, 10); hle_jump(ctx, 0x005940); return; }  /* 006364 jmp %pc@(5940) */
}

HLE_FUNC_YIELD(0x0063F2, Sub0063F2, 4000)
{
L_0063F2:
	{ uint32_t eaS = 0x0000641Cu; ctx->a[3] = eaS; } hle_insn(ctx, 8);  /* 0063F2 lea %pc@(641c),%a3 */
	{ uint32_t eaS = 0x000063FEu; ctx->a[2] = eaS; } hle_insn(ctx, 8);  /* 0063F6 lea %pc@(63fe),%a2 */
	{ hle_insn(ctx, 10); hle_jump(ctx, 0x005940); return; }  /* 0063FA jmp %pc@(5940) */
}

HLE_FUNC_YIELD(0x0064E0, Sub0064E0, 4000)
{
L_0064E0:
	{ uint32_t eaS = 0x0000650Au; ctx->a[3] = eaS; } hle_insn(ctx, 8);  /* 0064E0 lea %pc@(650a),%a3 */
	{ uint32_t eaS = 0x000064ECu; ctx->a[2] = eaS; } hle_insn(ctx, 8);  /* 0064E4 lea %pc@(64ec),%a2 */
	{ hle_insn(ctx, 10); hle_jump(ctx, 0x005940); return; }  /* 0064E8 jmp %pc@(5940) */
}

HLE_FUNC_YIELD(0x00700A, Sub00700A, 4000)
{
L_00700A:
	hle_insn(ctx, 16); return;  /* 00700A rts */
}

HLE_FUNC_YIELD(0x0072C0, Sub0072C0, 4000)
{
L_0072C0:
	{ ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[1]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[0]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[0]); } hle_movem_l(ctx, 32, 3);  /* 0072C0 moveml %d0/%a0-%a1,%sp@- */
	{ uint32_t v = 0x50u; ctx->d[0] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 0072C4 movel #80,%d0 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x007376); return;  /* 0072CA braw 7376 */
}

HLE_FUNC_YIELD(0x0072DC, Sub0072DC, 4000)
{
L_0072DC:
	{ ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[1]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[0]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[0]); } hle_movem_l(ctx, 32, 3);  /* 0072DC moveml %d0/%a0-%a1,%sp@- */
	{ uint32_t v = 0x150u; ctx->d[0] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 0072E0 movel #336,%d0 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x007376); return;  /* 0072E6 braw 7376 */
}

HLE_FUNC_YIELD(0x007B28, Sub007B28, 4000)
{
L_007B28:
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x007BB8); }  /* 007B28 jsr %pc@(7bb8) */
	{ uint32_t eaS = 0xFFFF80C0u; uint32_t v = ld16(eaS); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 007B2C movew ffff80c0,%d3 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[3] & 0xFFFFu), 0x6u, 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 4);  /* 007B32 addqw #6,%d3 */
	{ uint32_t eaS = 0x00002CA4u; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 007B34 lea %pc@(2ca4),%a0 */
	{ hle_insn(ctx, 10); hle_jump(ctx, 0x007E08); return; }  /* 007B38 jmp %pc@(7e08) */
}

HLE_FUNC_YIELD(0x007B3C, Sub007B3C, 4000)
{
L_007B3C:
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x007BB8); }  /* 007B3C jsr %pc@(7bb8) */
	{ uint32_t eaS = 0xFFFF80C0u; uint32_t v = ld16(eaS); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 007B40 movew ffff80c0,%d3 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[3] & 0xFFFFu), 0x6u, 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 4);  /* 007B46 addqw #6,%d3 */
	{ uint32_t eaS = 0x00002C99u; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 007B48 lea %pc@(2c99),%a0 */
	{ hle_insn(ctx, 10); hle_jump(ctx, 0x007E08); return; }  /* 007B4C jmp %pc@(7e08) */
}

HLE_FUNC_YIELD(0x00902E, Sub00902E, 4000)
{
L_00902E:
	{ ctx->a[7] -= 2; st16(ctx->a[7], ctx->d[3]); ctx->a[7] -= 2; st16(ctx->a[7], ctx->d[2]); ctx->a[7] -= 2; st16(ctx->a[7], ctx->d[1]); ctx->a[7] -= 2; st16(ctx->a[7], ctx->d[0]); } hle_movem_w(ctx, 24, 4);  /* 00902E movemw %d0-%d3,%sp@- */
	{ uint32_t v = 0x8001u; set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 009032 movew #-32767,%d2 */
	{ uint32_t eaS = 0xFFFF8000u; ctx->a[4] = eaS; } hle_insn(ctx, 12);  /* 009036 lea ffff8000,%a4 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x0090B2); }  /* 00903C bsrw 90b2 */
	{ uint32_t v = 0x8001u; set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 009040 movew #-32767,%d2 */
	{ uint32_t eaS = 0xFFFF8050u; ctx->a[4] = eaS; } hle_insn(ctx, 12);  /* 009044 lea ffff8050,%a4 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x0090B2); }  /* 00904A bsrw 90b2 */
	{ ctx->d[0] = (uint32_t)(int32_t)(int16_t)ld16(ctx->a[7]); ctx->a[7] += 2; ctx->d[1] = (uint32_t)(int32_t)(int16_t)ld16(ctx->a[7]); ctx->a[7] += 2; ctx->d[2] = (uint32_t)(int32_t)(int16_t)ld16(ctx->a[7]); ctx->a[7] += 2; ctx->d[3] = (uint32_t)(int32_t)(int16_t)ld16(ctx->a[7]); ctx->a[7] += 2; } hle_movem_w(ctx, 28, 4);  /* 00904E movemw %sp@+,%d0-%d3 */
	hle_insn(ctx, 16); return;  /* 009052 rts */
}

HLE_FUNC_YIELD(0x00D322, TruckDamage, 4000)
{
L_00D322:
	{ uint32_t eaS = 0xFFFFAE84u; ctx->a[1] = eaS; } hle_insn(ctx, 12);  /* 00D322 lea ffffae84,%a1 */
	{ ctx->d[2] = 0x0000000Fu; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 00D328 moveq #15,%d2 */
L_00D32A:
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x00881C); }  /* 00D32A jsr %pc@(881c) */
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0xFu) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00D32E andiw #15,%d0 */
	{ uint32_t eaD = ctx->a[1] + (uint32_t)(int32_t)(0) + (uint32_t)(int32_t)(int16_t)ctx->d[0]; cc_logic8(ctx, ld8(eaD)); } hle_insn(ctx, 14);  /* 00D332 tstb %a1@(0,%d0:w) */
	if (COND_NE) { hle_insn(ctx, 10); goto L_00D358; } hle_insn(ctx, 8);  /* 00D336 bnes d358 */
	{ uint16_t v = (uint16_t)ctx->d[2] - 1; set_w(&ctx->d[2], v); if (v != 0xFFFF) { hle_insn(ctx, 10); goto L_00D32A; } hle_insn(ctx, 14); }  /* 00D338 dbf %d2,d32a */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x00D46E); }  /* 00D33C jsr %pc@(d46e) */
	{ ctx->a[0] = 0x1F7DAu; } hle_insn(ctx, 12);  /* 00D340 moveal #128986,%a0 */
	{ uint32_t eaD = 0xFFFF80D2u; st16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 20);  /* 00D346 clrw ffff80d2 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x00933E); }  /* 00D34C jsr %pc@(933e) */
	{ uint32_t eaD = ctx->a[6];  unsigned b = 4; uint32_t m = 1u << b; uint8_t v = ld8(eaD); cc_set(ctx, CC_Z, (v & m) ? 0 : CC_Z); st8(eaD, v | m); } hle_insn(ctx, 16);  /* 00D350 bset #4,%fp@ */
	hle_insn(ctx, 10); goto L_00D3E2;  /* 00D354 braw d3e2 */
L_00D358:
	{ uint32_t eaD = ctx->a[1] + (uint32_t)(int32_t)(0) + (uint32_t)(int32_t)(int16_t)ctx->d[0]; uint32_t v = alu(ctx, OP_SUB, ld8(eaD), 0x1u, 8, 1); st8(eaD, v); } hle_insn(ctx, 18);  /* 00D358 subqb #1,%a1@(0,%d0:w) */
	if (COND_NE) { hle_insn(ctx, 10); goto L_00D3E0; } hle_insn(ctx, 12);  /* 00D35C bnew d3e0 */
	{ uint32_t eaD = 0xFFFF9F68u; uint32_t v = alu(ctx, OP_ADD, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 20);  /* 00D360 addqw #1,ffff9f68 */
	{ uint32_t v = shf(ctx, 'L', (ctx->d[0] & 0xFFFFu), 2, 16); set_w(&ctx->d[0], v); } hle_insn(ctx, 10);  /* 00D366 aslw #2,%d0 */
	{ ctx->d[2] = 0x00000000u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 00D368 moveq #0,%d2 */
	{ ctx->d[1] = 0x00000000u; cc_logic32(ctx, ctx->d[1]); } hle_insn(ctx, 4);  /* 00D36A moveq #0,%d1 */
	{ uint32_t eaS = 0x0000D3E5u + (uint32_t)(int32_t)(int16_t)ctx->d[0]; uint32_t v = ld8(eaS); set_b(&ctx->d[2], v); cc_logic8(ctx, v); } hle_insn(ctx, 14);  /* 00D36C moveb %pc@(d3e5,%d0:w),%d2 */
	{ uint32_t eaS = 0x0000D3E4u + (uint32_t)(int32_t)(int16_t)ctx->d[0]; uint32_t v = ld8(eaS); set_b(&ctx->d[1], v); cc_logic8(ctx, v); } hle_insn(ctx, 14);  /* 00D370 moveb %pc@(d3e4,%d0:w),%d1 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x00D444); }  /* 00D374 jsr %pc@(d444) */
	{ uint32_t v = shf(ctx, 'L', (ctx->d[1] & 0xFFFFu), 4, 16); set_w(&ctx->d[1], v); } hle_insn(ctx, 14);  /* 00D378 aslw #4,%d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFFu); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 00D37A movew %d1,%d3 */
	{ uint32_t v = shf(ctx, 'L', (ctx->d[1] & 0xFFFFu), 2, 16); set_w(&ctx->d[1], v); } hle_insn(ctx, 10);  /* 00D37C aslw #2,%d1 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[3] & 0xFFFFu), (ctx->d[1] & 0xFFFFu), 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 4);  /* 00D37E addw %d1,%d3 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), (ctx->d[1] & 0xFFFFu), 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 4);  /* 00D380 addw %d1,%d1 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[2] & 0xFFFFu), (ctx->d[2] & 0xFFFFu), 16, 1); set_w(&ctx->d[2], v); } hle_insn(ctx, 4);  /* 00D382 addw %d2,%d2 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), (ctx->d[2] & 0xFFFFu), 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 4);  /* 00D384 addw %d2,%d1 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[3] & 0xFFFFu), (ctx->d[2] & 0xFFFFu), 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 4);  /* 00D386 addw %d2,%d3 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0xE500u, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 00D388 addiw #-6912,%d1 */
	{ uint32_t eaS = 0x00053D40u; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 00D38C lea 53d40,%a0 */
	{ uint32_t eaS = ctx->a[0] + (uint32_t)(int32_t)(0) + (uint32_t)(int32_t)(int16_t)ctx->d[3]; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 00D392 lea %a0@(0,%d3:w),%a0 */
	{ ctx->d[2] = 0x00000000u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 00D396 moveq #0,%d2 */
	{ ctx->d[3] = 0x00000000u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 00D398 moveq #0,%d3 */
	{ uint32_t eaS = 0x0000D3E6u + (uint32_t)(int32_t)(int16_t)ctx->d[0]; uint32_t v = ld8(eaS); set_b(&ctx->d[2], v); cc_logic8(ctx, v); } hle_insn(ctx, 14);  /* 00D39A moveb %pc@(d3e6,%d0:w),%d2 */
	{ uint32_t eaS = 0x0000D3E7u + (uint32_t)(int32_t)(int16_t)ctx->d[0]; uint32_t v = ld8(eaS); set_b(&ctx->d[3], v); cc_logic8(ctx, v); } hle_insn(ctx, 14);  /* 00D39E moveb %pc@(d3e7,%d0:w),%d3 */
	{ uint32_t v = 0xE000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00D3A2 movew #-8192,%d6 */
	{ ctx->a[7] -= 2; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->d[7] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00D3A6 movew %d7,%sp@- */
	{ uint32_t eaS = 0xFFFF9E4Au; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00D3A8 movew ffff9e4a,%d7 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x012234); }  /* 00D3AE jsr %pc@(12234) */
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00D3B2 movew %sp@+,%d7 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x00881C); }  /* 00D3B4 jsr %pc@(881c) */
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0x7u) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00D3B8 andiw #7,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_00D3C2; } hle_insn(ctx, 8);  /* 00D3BC bnes d3c2 */
	{ uint32_t eaD = ctx->a[6];  unsigned b = 3; uint32_t m = 1u << b; uint8_t v = ld8(eaD); cc_set(ctx, CC_Z, (v & m) ? 0 : CC_Z); st8(eaD, v | m); } hle_insn(ctx, 16);  /* 00D3BE bset #3,%fp@ */
L_00D3C2:
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x2u, 16, 0); } hle_insn(ctx, 8);  /* 00D3C2 cmpiw #2,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_00D3CC; } hle_insn(ctx, 8);  /* 00D3C6 bnes d3cc */
	{ uint32_t eaD = ctx->a[6];  unsigned b = 6; uint32_t m = 1u << b; uint8_t v = ld8(eaD); cc_set(ctx, CC_Z, (v & m) ? 0 : CC_Z); st8(eaD, v | m); } hle_insn(ctx, 16);  /* 00D3C8 bset #6,%fp@ */
L_00D3CC:
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x4u, 16, 0); } hle_insn(ctx, 8);  /* 00D3CC cmpiw #4,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_00D3E0; } hle_insn(ctx, 8);  /* 00D3D0 bnes d3e0 */
	{ uint32_t eaD = ctx->a[6];  unsigned b = 4; uint32_t m = 1u << b; uint8_t v = ld8(eaD); cc_set(ctx, CC_Z, (v & m) ? 0 : CC_Z); st8(eaD, v | m); } hle_insn(ctx, 16);  /* 00D3D2 bset #4,%fp@ */
	if (COND_NE) { hle_insn(ctx, 10); goto L_00D3E0; } hle_insn(ctx, 8);  /* 00D3D6 bnes d3e0 */
	{ ctx->d[2] = 0x00000005u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 00D3D8 moveq #5,%d2 */
	{ ctx->d[1] = 0x00000001u; cc_logic32(ctx, ctx->d[1]); } hle_insn(ctx, 4);  /* 00D3DA moveq #1,%d1 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x00D444); }  /* 00D3DC jsr %pc@(d444) */
L_00D3E0:
	hle_insn(ctx, 16); return;  /* 00D3E0 rts */
L_00D3E2:
	hle_insn(ctx, 16); return;  /* 00D3E2 rts */
}

HLE_FUNC_YIELD(0x010666, Sub010666, 4000)
{
L_010666:
	{ uint32_t v = (ctx->d[1] & 0xFFFFu); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 010666 movew %d1,%d0 */
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x22u, 16, 0); } hle_insn(ctx, 8);  /* 010668 cmpiw #34,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_010678; } hle_insn(ctx, 8);  /* 01066C bnes 10678 */
	{ uint32_t v = 0xAEu; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 01066E movew #174,%d1 */
	{ uint32_t v = 0x11u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 010672 movew #17,%d0 */
	hle_insn(ctx, 10); goto L_010682;  /* 010676 bras 10682 */
L_010678:
	{ uint32_t eaD = ctx->a[6];  unsigned b = 6; uint32_t m = 1u << b; uint8_t v = ld8(eaD); cc_set(ctx, CC_Z, (v & m) ? 0 : CC_Z);  } hle_insn(ctx, 12);  /* 010678 btst #14,%fp@ */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_010682; } hle_insn(ctx, 8);  /* 01067C beqs 10682 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0x11u, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 01067E addiw #17,%d1 */
L_010682:
	{ uint32_t eaS = 0xFFFF80BCu; ctx->a[5] = ld32(eaS); } hle_insn(ctx, 20);  /* 010682 moveal ffff80bc,%a5 */
	{ uint32_t eaD = ctx->a[5]; ctx->a[5] += 2; uint32_t v = 0x1u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 010688 movew #1,%a5@+ */
	{ uint32_t v = shf(ctx, 'L', (ctx->d[0] & 0xFFFFu), 2, 16); set_w(&ctx->d[0], v); } hle_insn(ctx, 10);  /* 01068C aslw #2,%d0 */
	{ uint32_t eaS = 0x000106C4u; ctx->a[1] = eaS; } hle_insn(ctx, 8);  /* 01068E lea %pc@(106c4),%a1 */
	{ uint32_t eaS = ctx->a[1] + (uint32_t)(int32_t)(0) + (uint32_t)(int32_t)(int16_t)ctx->d[0]; uint32_t v = ld16(eaS); set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 14);  /* 010692 movew %a1@(0,%d0:w),%d2 */
	{ uint32_t v = shf(ctx, 'a', (ctx->d[2] & 0xFFFFu), 1, 16); set_w(&ctx->d[2], v); } hle_insn(ctx, 8);  /* 010696 asrw #1,%d2 */
	{ uint32_t eaD = ctx->a[5]; ctx->a[5] += 2; uint32_t v = (ctx->d[2] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 010698 movew %d2,%a5@+ */
	{ uint32_t eaS = ctx->a[1] + (uint32_t)(int32_t)(2) + (uint32_t)(int32_t)(int16_t)ctx->d[0]; uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 14);  /* 01069A movew %a1@(2,%d0:w),%d0 */
	{ uint32_t eaS = 0x000A931Au; ctx->a[1] = eaS; } hle_insn(ctx, 12);  /* 01069E lea a931a,%a1 */
	{ uint32_t eaS = ctx->a[1] + (uint32_t)(int32_t)(0) + (uint32_t)(int32_t)(int16_t)ctx->d[0]; ctx->a[1] = eaS; } hle_insn(ctx, 12);  /* 0106A4 lea %a1@(0,%d0:w),%a1 */
	{ uint32_t eaD = ctx->a[5]; ctx->a[5] += 4; uint32_t v = (ctx->a[1] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 0106A8 movel %a1,%a5@+ */
	{ uint32_t eaS = 0xFFFF9E5Eu; uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 0106AA movew ffff9e5e,%d0 */
	{ uint32_t v = shf(ctx, 'L', (ctx->d[0] & 0xFFFFu), 5, 16); set_w(&ctx->d[0], v); } hle_insn(ctx, 16);  /* 0106B0 aslw #5,%d0 */
	{ uint32_t eaD = ctx->a[5]; ctx->a[5] += 2; uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0106B2 movew %d0,%a5@+ */
	{ uint32_t eaD = 0xFFFF80BCu; uint32_t v = (ctx->a[5] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 20);  /* 0106B4 movel %a5,ffff80bc */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0x2C1u, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 0106BA addiw #705,%d1 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x013228); }  /* 0106BE jsr %pc@(13228) */
	hle_insn(ctx, 16); return;  /* 0106C2 rts */
}

HLE_FUNC_YIELD(0x012A16, Sub012A16, 4000)
{
L_012A16:
	{ ctx->a[7] -= 4; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->a[6] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 012A16 movel %fp,%sp@- */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(6); uint32_t v = ld16(eaS); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 012A18 movew %fp@(6),%d3 */
	{ alu(ctx, OP_SUB, (ctx->d[2] & 0xFFu), 0x3u, 8, 0); } hle_insn(ctx, 8);  /* 012A1C cmpib #3,%d2 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_012A3E; } hle_insn(ctx, 8);  /* 012A20 bnes 12a3e */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x5u, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 4);  /* 012A22 subqw #5,%d0 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[1] & 0xFFFFu), 0x2u, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 4);  /* 012A24 subqw #2,%d1 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x012A46); }  /* 012A26 jsr %pc@(12a46) */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[1] & 0xFFFFu), 0xAu, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 012A2A subiw #10,%d1 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x012A46); }  /* 012A2E jsr %pc@(12a46) */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), 0xAu, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 8);  /* 012A32 addiw #10,%d0 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x012A46); }  /* 012A36 jsr %pc@(12a46) */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0xAu, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 012A3A addiw #10,%d1 */
L_012A3E:
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x012A46); }  /* 012A3E jsr %pc@(12a46) */
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 4; ctx->a[6] = ld32(eaS); } hle_insn(ctx, 12);  /* 012A42 moveal %sp@+,%fp */
	hle_insn(ctx, 16); return;  /* 012A44 rts */
}

HLE_FUNC_YIELD(0x012A46, Sub012A46, 4000)
{
L_012A46:
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x01296E); }  /* 012A46 jsr %pc@(1296e) */
	{ cc_logic16(ctx, (ctx->d[6] & 0xFFFFu)); } hle_insn(ctx, 4);  /* 012A4A tstw %d6 */
	if (COND_MI) { hle_insn(ctx, 10); goto L_012A76; } hle_insn(ctx, 12);  /* 012A4C bmiw 12a76 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 012A50 movew %d0,%fp@(2) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(4); uint32_t v = (ctx->d[1] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 012A54 movew %d1,%fp@(4) */
	{ unsigned b = 9; uint32_t m = 1u << b; cc_set(ctx, CC_Z, (ctx->d[2] & m) ? 0 : CC_Z); ctx->d[2] |= m; hle_insn(ctx, 10 + ((b > 15) ? 2 : 0)); }  /* 012A58 bset #9,%d2 */
	{ unsigned b = 11; uint32_t m = 1u << b; cc_set(ctx, CC_Z, (ctx->d[2] & m) ? 0 : CC_Z); ctx->d[2] |= m; hle_insn(ctx, 10 + ((b > 15) ? 2 : 0)); }  /* 012A5C bset #11,%d2 */
	{ uint32_t eaD = ctx->a[6]; uint32_t v = (ctx->d[2] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 012A60 movew %d2,%fp@ */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(20); uint32_t v = 0x100u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 012A62 movew #256,%fp@(20) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(6); uint32_t v = (ctx->d[3] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 012A68 movew %d3,%fp@(6) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(22); st16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 16);  /* 012A6C clrw %fp@(22) */
	{ uint32_t eaD = 0xFFFF80E0u; uint32_t v = alu(ctx, OP_ADD, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 20);  /* 012A70 addqw #1,ffff80e0 */
L_012A76:
	hle_insn(ctx, 16); return;  /* 012A76 rts */
}

HLE_FUNC_YIELD(0x012AF4, Sub012AF4, 4000)
{
L_012AF4:
	{ uint32_t eaD = ctx->a[6]; uint32_t v = 0x1u; st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 12);  /* 012AF4 moveb #1,%fp@ */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(2); ctx->d[0] = (uint32_t)(int32_t)(int16_t)ld16(eaS + 0); ctx->d[1] = (uint32_t)(int32_t)(int16_t)ld16(eaS + 2); } hle_insn(ctx, 24);  /* 012AF8 movemw %fp@(2),%d0-%d1 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(1); alu(ctx, OP_SUB, ld8(eaD), 0xAu, 8, 0); } hle_insn(ctx, 16);  /* 012AFE cmpib #10,%fp@(1) */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_012B5C; } hle_insn(ctx, 12);  /* 012B04 beqw 12b5c */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(36); cc_logic16(ctx, ld16(eaD)); } hle_insn(ctx, 12);  /* 012B08 tstw %a4@(36) */
	if (COND_NE) { hle_insn(ctx, 10); goto L_012B18; } hle_insn(ctx, 8);  /* 012B0C bnes 12b18 */
	{ uint32_t v = 0x40u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 012B0E movew #64,%d3 */
	{ uint32_t v = 0x0u; set_w(&ctx->d[4], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 012B12 movew #0,%d4 */
	hle_insn(ctx, 10); goto L_012B20;  /* 012B16 bras 12b20 */
L_012B18:
	{ uint32_t v = 0x100u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 012B18 movew #256,%d3 */
	{ uint32_t v = 0x0u; set_w(&ctx->d[4], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 012B1C movew #0,%d4 */
L_012B20:
	{ uint32_t v = 0x1u; set_b(&ctx->d[5], v); cc_logic8(ctx, v); } hle_insn(ctx, 8);  /* 012B20 moveb #1,%d5 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[3] & 0xFFFFu), (ctx->d[0] & 0xFFFFu), 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 4);  /* 012B24 subw %d0,%d3 */
	if (COND_PL) { hle_insn(ctx, 10); goto L_012B2C; } hle_insn(ctx, 8);  /* 012B26 bpls 12b2c */
	{ uint32_t v = alu(ctx, OP_SUB, 0, (ctx->d[5] & 0xFFu), 8, 1); set_b(&ctx->d[5], v); } hle_insn(ctx, 4);  /* 012B28 negb %d5 */
	{ uint32_t v = alu(ctx, OP_SUB, 0, (ctx->d[3] & 0xFFFFu), 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 4);  /* 012B2A negw %d3 */
L_012B2C:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(14); uint32_t v = (ctx->d[5] & 0xFFu); st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 12);  /* 012B2C moveb %d5,%fp@(14) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 012B30 movew %d0,%fp@(2) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(10); uint32_t v = (ctx->d[3] & 0xFFu); st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 12);  /* 012B34 moveb %d3,%fp@(10) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(11); st8(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 16);  /* 012B38 clrb %fp@(11) */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[4] & 0xFFFFu), (ctx->d[1] & 0xFFFFu), 16, 1); set_w(&ctx->d[4], v); } hle_insn(ctx, 4);  /* 012B3C subw %d1,%d4 */
	if (COND_PL) { hle_insn(ctx, 10); goto L_012B42; } hle_insn(ctx, 8);  /* 012B3E bpls 12b42 */
	{ uint32_t v = alu(ctx, OP_SUB, 0, (ctx->d[4] & 0xFFFFu), 16, 1); set_w(&ctx->d[4], v); } hle_insn(ctx, 4);  /* 012B40 negw %d4 */
L_012B42:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(15); uint32_t v = 0xFFu; st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 16);  /* 012B42 moveb #-1,%fp@(15) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(4); uint32_t v = (ctx->d[1] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 012B48 movew %d1,%fp@(4) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(12); uint32_t v = (ctx->d[4] & 0xFFu); st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 12);  /* 012B4C moveb %d4,%fp@(12) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(13); st8(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 16);  /* 012B50 clrb %fp@(13) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(20); uint32_t v = 0x100u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 012B54 movew #256,%fp@(20) */
	hle_insn(ctx, 16); return;  /* 012B5A rts */
L_012B5C:
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(36); cc_logic16(ctx, ld16(eaD)); } hle_insn(ctx, 12);  /* 012B5C tstw %a4@(36) */
	if (COND_NE) { hle_insn(ctx, 10); goto L_012B6C; } hle_insn(ctx, 8);  /* 012B60 bnes 12b6c */
	{ uint32_t v = 0xA0u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 012B62 movew #160,%d3 */
	{ uint32_t v = 0xE0u; set_w(&ctx->d[4], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 012B66 movew #224,%d4 */
	hle_insn(ctx, 10); goto L_012B74;  /* 012B6A bras 12b74 */
L_012B6C:
	{ uint32_t v = 0xA0u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 012B6C movew #160,%d3 */
	{ uint32_t v = 0xE0u; set_w(&ctx->d[4], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 012B70 movew #224,%d4 */
L_012B74:
	{ uint32_t v = 0x1u; set_b(&ctx->d[5], v); cc_logic8(ctx, v); } hle_insn(ctx, 8);  /* 012B74 moveb #1,%d5 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[3] & 0xFFFFu), (ctx->d[0] & 0xFFFFu), 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 4);  /* 012B78 subw %d0,%d3 */
	if (COND_PL) { hle_insn(ctx, 10); goto L_012B80; } hle_insn(ctx, 8);  /* 012B7A bpls 12b80 */
	{ uint32_t v = alu(ctx, OP_SUB, 0, (ctx->d[5] & 0xFFu), 8, 1); set_b(&ctx->d[5], v); } hle_insn(ctx, 4);  /* 012B7C negb %d5 */
	{ uint32_t v = alu(ctx, OP_SUB, 0, (ctx->d[3] & 0xFFFFu), 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 4);  /* 012B7E negw %d3 */
L_012B80:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(14); uint32_t v = (ctx->d[5] & 0xFFu); st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 12);  /* 012B80 moveb %d5,%fp@(14) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 012B84 movew %d0,%fp@(2) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(10); uint32_t v = (ctx->d[3] & 0xFFu); st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 12);  /* 012B88 moveb %d3,%fp@(10) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(11); st8(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 16);  /* 012B8C clrb %fp@(11) */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[4] & 0xFFFFu), (ctx->d[1] & 0xFFFFu), 16, 1); set_w(&ctx->d[4], v); } hle_insn(ctx, 4);  /* 012B90 subw %d1,%d4 */
	if (COND_PL) { hle_insn(ctx, 10); goto L_012B96; } hle_insn(ctx, 8);  /* 012B92 bpls 12b96 */
	{ uint32_t v = alu(ctx, OP_SUB, 0, (ctx->d[4] & 0xFFFFu), 16, 1); set_w(&ctx->d[4], v); } hle_insn(ctx, 4);  /* 012B94 negw %d4 */
L_012B96:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(15); uint32_t v = 0x1u; st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 16);  /* 012B96 moveb #1,%fp@(15) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(4); uint32_t v = (ctx->d[1] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 012B9C movew %d1,%fp@(4) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(12); uint32_t v = (ctx->d[4] & 0xFFu); st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 12);  /* 012BA0 moveb %d4,%fp@(12) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(13); st8(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 16);  /* 012BA4 clrb %fp@(13) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(20); uint32_t v = 0x100u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 012BA8 movew #256,%fp@(20) */
	hle_insn(ctx, 16); return;  /* 012BAE rts */
}

HLE_FUNC_YIELD(0x013D92, Sub013D92, 4000)
{
L_013D92:
	{ uint32_t v = 0xE0000u; ctx->d[0] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 013D92 movel #917504,%d0 */
	{ uint32_t v = 0x20C2u; ctx->d[2] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 013D98 movel #8386,%d2 */
	{ uint32_t v = 0xCAu; set_b(&ctx->d[1], v); cc_logic8(ctx, v); } hle_insn(ctx, 8);  /* 013D9E moveb #-54,%d1 */
	{ ctx->d[3] = 0x00000002u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 013DA2 moveq #2,%d3 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x013D56); return;  /* 013DA4 braw 13d56 */
}

