/* Small helper subroutines of the spawn / aim / pickup / damage-bar code
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


HLE_FUNC_YIELD(0x005DEA, Sub005DEA, 4000)
{
L_005DEA:
	{ uint32_t eaS = 0x00005DF6u; ctx->a[2] = eaS; } hle_insn(ctx, 8);  /* 005DEA lea %pc@(5df6),%a2 */
	{ uint32_t eaS = 0x00005E14u; ctx->a[3] = eaS; } hle_insn(ctx, 8);  /* 005DEE lea %pc@(5e14),%a3 */
	{ hle_insn(ctx, 10); hle_jump(ctx, 0x005940); return; }  /* 005DF2 jmp %pc@(5940) */
}

HLE_FUNC_YIELD(0x0069BC, Sub0069BC, 4000)
{
L_0069BC:
	{ uint32_t eaS = 0x000069E6u; ctx->a[3] = eaS; } hle_insn(ctx, 8);  /* 0069BC lea %pc@(69e6),%a3 */
	{ uint32_t eaS = 0x000069C8u; ctx->a[2] = eaS; } hle_insn(ctx, 8);  /* 0069C0 lea %pc@(69c8),%a2 */
	{ hle_insn(ctx, 10); hle_jump(ctx, 0x005940); return; }  /* 0069C4 jmp %pc@(5940) */
}

HLE_FUNC_YIELD(0x0070CA, Sub0070CA, 4000)
{
L_0070CA:
	{ uint32_t eaS = 0x000070E6u; ctx->a[3] = eaS; } hle_insn(ctx, 8);  /* 0070CA lea %pc@(70e6),%a3 */
	{ ctx->d[6] = 0x00000001u; cc_logic32(ctx, ctx->d[6]); } hle_insn(ctx, 4);  /* 0070CE moveq #1,%d6 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x007086); }  /* 0070D0 jsr %pc@(7086) */
	{ cc_logic16(ctx, (ctx->d[6] & 0xFFFFu)); } hle_insn(ctx, 4);  /* 0070D4 tstw %d6 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0070E4; } hle_insn(ctx, 8);  /* 0070D6 beqs 70e4 */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(42); uint32_t v = 0xFC18u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 0070D8 movew #-1000,%a4@(42) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(28); uint32_t v = 0xFFFFu; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 0070DE movew #-1,%a4@(28) */
L_0070E4:
	hle_insn(ctx, 16); return;  /* 0070E4 rts */
}

HLE_FUNC_YIELD(0x0070EE, Sub0070EE, 4000)
{
L_0070EE:
	{ uint32_t eaS = 0x00007104u; ctx->a[3] = eaS; } hle_insn(ctx, 8);  /* 0070EE lea %pc@(7104),%a3 */
	{ ctx->d[6] = 0x00000002u; cc_logic32(ctx, ctx->d[6]); } hle_insn(ctx, 4);  /* 0070F2 moveq #2,%d6 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x007086); }  /* 0070F4 jsr %pc@(7086) */
	{ cc_logic16(ctx, (ctx->d[6] & 0xFFFFu)); } hle_insn(ctx, 4);  /* 0070F8 tstw %d6 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_007102; } hle_insn(ctx, 8);  /* 0070FA beqs 7102 */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(28); uint32_t v = 0xFFFFu; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 0070FC movew #-1,%a4@(28) */
L_007102:
	hle_insn(ctx, 16); return;  /* 007102 rts */
}

HLE_FUNC_YIELD(0x00710C, Sub00710C, 4000)
{
L_00710C:
	{ uint32_t eaS = 0x00007122u; ctx->a[3] = eaS; } hle_insn(ctx, 8);  /* 00710C lea %pc@(7122),%a3 */
	{ ctx->d[6] = 0x00000003u; cc_logic32(ctx, ctx->d[6]); } hle_insn(ctx, 4);  /* 007110 moveq #3,%d6 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x007086); }  /* 007112 jsr %pc@(7086) */
	{ cc_logic16(ctx, (ctx->d[6] & 0xFFFFu)); } hle_insn(ctx, 4);  /* 007116 tstw %d6 */
	if (COND_EQ) { hle_insn(ctx, 10); hle_jump(ctx, 0x007120); return; } hle_insn(ctx, 8); hle_jump(ctx, 0x00711A); return;  /* 007118 beqs 7120 */
}

HLE_FUNC_YIELD(0x00712A, Sub00712A, 4000)
{
L_00712A:
	{ uint32_t eaS = 0x00007166u; ctx->a[3] = eaS; } hle_insn(ctx, 8);  /* 00712A lea %pc@(7166),%a3 */
	{ ctx->d[6] = 0x00000000u; cc_logic32(ctx, ctx->d[6]); } hle_insn(ctx, 4);  /* 00712E moveq #0,%d6 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x007086); }  /* 007130 jsr %pc@(7086) */
	{ cc_logic16(ctx, (ctx->d[6] & 0xFFFFu)); } hle_insn(ctx, 4);  /* 007134 tstw %d6 */
	if (COND_EQ) { hle_insn(ctx, 10); hle_jump(ctx, 0x007164); return; } hle_insn(ctx, 8); hle_jump(ctx, 0x007138); return;  /* 007136 beqs 7164 */
}

HLE_FUNC_YIELD(0x00720C, Sub00720C, 4000)
{
L_00720C:
	{ uint32_t eaS = 0x00007228u; ctx->a[3] = eaS; } hle_insn(ctx, 8);  /* 00720C lea %pc@(7228),%a3 */
	{ ctx->d[6] = 0x00000006u; cc_logic32(ctx, ctx->d[6]); } hle_insn(ctx, 4);  /* 007210 moveq #6,%d6 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x007086); }  /* 007212 jsr %pc@(7086) */
	{ cc_logic16(ctx, (ctx->d[6] & 0xFFFFu)); } hle_insn(ctx, 4);  /* 007216 tstw %d6 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_007226; } hle_insn(ctx, 8);  /* 007218 beqs 7226 */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(22); alu(ctx, OP_SUB, ld16(eaD), 0x6u, 16, 0); } hle_insn(ctx, 16);  /* 00721A cmpiw #6,%a4@(22) */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_007226; } hle_insn(ctx, 8);  /* 007220 beqs 7226 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00711A); return;  /* 007222 braw 711a */
L_007226:
	hle_insn(ctx, 16); return;  /* 007226 rts */
}

HLE_FUNC_YIELD(0x007230, Sub007230, 4000)
{
L_007230:
	{ uint32_t eaS = 0x00007258u; ctx->a[3] = eaS; } hle_insn(ctx, 8);  /* 007230 lea %pc@(7258),%a3 */
	{ ctx->d[6] = 0x00000009u; cc_logic32(ctx, ctx->d[6]); } hle_insn(ctx, 4);  /* 007234 moveq #9,%d6 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x007086); }  /* 007236 jsr %pc@(7086) */
	{ cc_logic16(ctx, (ctx->d[6] & 0xFFFFu)); } hle_insn(ctx, 4);  /* 00723A tstw %d6 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_007256; } hle_insn(ctx, 8);  /* 00723C beqs 7256 */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(20); uint32_t v = 0x3u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00723E movew #3,%a4@(20) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(28); uint32_t v = 0xFFFFu; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 007244 movew #-1,%a4@(28) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(44); uint32_t v = 0xFC18u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00724A movew #-1000,%a4@(44) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(42); uint32_t v = 0xFC18u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 007250 movew #-1000,%a4@(42) */
L_007256:
	hle_insn(ctx, 16); return;  /* 007256 rts */
}

HLE_FUNC_YIELD(0x0072B2, Sub0072B2, 4000)
{
L_0072B2:
	{ ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[1]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[0]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[0]); } hle_movem_l(ctx, 32, 3);  /* 0072B2 moveml %d0/%a0-%a1,%sp@- */
	{ uint32_t v = 0x50u; ctx->d[0] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 0072B6 movel #80,%d0 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x007376); return;  /* 0072BC braw 7376 */
}

HLE_FUNC_YIELD(0x0072CE, Sub0072CE, 4000)
{
L_0072CE:
	{ ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[1]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[0]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[0]); } hle_movem_l(ctx, 32, 3);  /* 0072CE moveml %d0/%a0-%a1,%sp@- */
	{ uint32_t v = 0x100u; ctx->d[0] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 0072D2 movel #256,%d0 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x007376); return;  /* 0072D8 braw 7376 */
}

HLE_FUNC_YIELD(0x0073A0, MissileFindTarget, 4000)
{
L_0073A0:
	{ uint32_t eaS = ctx->a[4] + (uint32_t)(int32_t)(4); uint32_t eaD = 0xFFFF9E94u; uint32_t v = ld32(eaS); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 32);  /* 0073A0 movel %a4@(4),ffff9e94 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(34); st32(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 24);  /* 0073A8 clrl %fp@(34) */
	{ ctx->a[7] -= 4; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->a[6] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 0073AC movel %fp,%sp@- */
	{ uint32_t eaS = 0xFFFF87B2u; ctx->a[6] = eaS; } hle_insn(ctx, 12);  /* 0073AE lea ffff87b2,%fp */
	{ uint32_t eaS = 0xFFFF80DAu; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 0073B4 movew ffff80da,%d7 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[7] & 0xFFFFu), 0x1u, 16, 1); set_w(&ctx->d[7], v); } hle_insn(ctx, 4);  /* 0073BA subqw #1,%d7 */
	if (COND_MI) { hle_insn(ctx, 10); goto L_0073E4; } hle_insn(ctx, 12);  /* 0073BC bmiw 73e4 */
L_0073C0:
	{ uint32_t eaS = ctx->a[6]; uint32_t v = ld16(eaS); set_w(&ctx->d[5], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0073C0 movew %fp@,%d5 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0073E8; } hle_insn(ctx, 12);  /* 0073C2 beqw 73e8 */
	{ alu(ctx, OP_SUB, (ctx->d[5] & 0xFFu), 0x22u, 8, 0); } hle_insn(ctx, 8);  /* 0073C6 cmpib #34,%d5 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0073DC; } hle_insn(ctx, 8);  /* 0073CA beqs 73dc */
	{ uint32_t eaS = 0xFFFFA958u; ctx->a[5] = eaS; } hle_insn(ctx, 12);  /* 0073CC lea ffffa958,%a5 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(28); ctx->a[0] = ld32(eaS); } hle_insn(ctx, 16);  /* 0073D2 moveal %fp@(28),%a0 */
	{ hle_insn(ctx, 16); hle_sync(ctx); hle_callc(ctx, ctx->a[0]); }  /* 0073D6 jsr %a0@ */
	{ cc_logic16(ctx, (ctx->d[6] & 0xFFFFu)); } hle_insn(ctx, 4);  /* 0073D8 tstw %d6 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0073F0; } hle_insn(ctx, 8);  /* 0073DA bnes 73f0 */
L_0073DC:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(44); ctx->a[6] = eaS; } hle_insn(ctx, 8);  /* 0073DC lea %fp@(44),%fp */
	{ uint16_t v = (uint16_t)ctx->d[7] - 1; set_w(&ctx->d[7], v); if (v != 0xFFFF) { hle_insn(ctx, 10); goto L_0073C0; } hle_insn(ctx, 14); }  /* 0073E0 dbf %d7,73c0 */
L_0073E4:
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 4; ctx->a[6] = ld32(eaS); } hle_insn(ctx, 12);  /* 0073E4 moveal %sp@+,%fp */
	hle_insn(ctx, 16); return;  /* 0073E6 rts */
L_0073E8:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(44); ctx->a[6] = eaS; } hle_insn(ctx, 8);  /* 0073E8 lea %fp@(44),%fp */
	hle_insn(ctx, 10); goto L_0073C0;  /* 0073EC braw 73c0 */
L_0073F0:
	{ ctx->d[0] = 0x00000062u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 0073F0 moveq #98,%d0 */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_callc(ctx, 0x013D04); }  /* 0073F2 jsr 13d04 */
	{ uint32_t eaS = ctx->a[7]; ctx->a[1] = ld32(eaS); } hle_insn(ctx, 12);  /* 0073F8 moveal %sp@,%a1 */
	{ uint32_t eaD = ctx->a[1] + (uint32_t)(int32_t)(34); uint32_t v = (ctx->a[6] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 16);  /* 0073FA movel %fp,%a1@(34) */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t eaD = ctx->a[1] + (uint32_t)(int32_t)(38); uint32_t v = ld32(eaS); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 28);  /* 0073FE movel %fp@(2),%a1@(38) */
	hle_insn(ctx, 10); goto L_0073E4;  /* 007404 bras 73e4 */
}

HLE_FUNC_YIELD(0x007B14, Sub007B14, 4000)
{
L_007B14:
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x007BB8); }  /* 007B14 jsr %pc@(7bb8) */
	{ uint32_t eaS = 0xFFFF80C0u; uint32_t v = ld16(eaS); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 007B18 movew ffff80c0,%d3 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[3] & 0xFFFFu), 0x2u, 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 4);  /* 007B1E addqw #2,%d3 */
	{ uint32_t eaS = 0x00002CAFu; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 007B20 lea %pc@(2caf),%a0 */
	{ hle_insn(ctx, 10); hle_jump(ctx, 0x007E08); return; }  /* 007B24 jmp %pc@(7e08) */
}

HLE_FUNC_YIELD(0x009054, Sub009054, 4000)
{
L_009054:
	{ ctx->a[7] -= 2; st16(ctx->a[7], ctx->d[3]); ctx->a[7] -= 2; st16(ctx->a[7], ctx->d[2]); ctx->a[7] -= 2; st16(ctx->a[7], ctx->d[1]); ctx->a[7] -= 2; st16(ctx->a[7], ctx->d[0]); } hle_movem_w(ctx, 24, 4);  /* 009054 movemw %d0-%d3,%sp@- */
	{ uint32_t v = 0x40u; set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 009058 movew #64,%d2 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x00909C); }  /* 00905C bsrw 909c */
	{ ctx->d[0] = (uint32_t)(int32_t)(int16_t)ld16(ctx->a[7]); ctx->a[7] += 2; ctx->d[1] = (uint32_t)(int32_t)(int16_t)ld16(ctx->a[7]); ctx->a[7] += 2; ctx->d[2] = (uint32_t)(int32_t)(int16_t)ld16(ctx->a[7]); ctx->a[7] += 2; ctx->d[3] = (uint32_t)(int32_t)(int16_t)ld16(ctx->a[7]); ctx->a[7] += 2; } hle_movem_w(ctx, 28, 4);  /* 009060 movemw %sp@+,%d0-%d3 */
	hle_insn(ctx, 16); return;  /* 009064 rts */
}

HLE_FUNC_YIELD(0x009078, Sub009078, 4000)
{
L_009078:
	{ ctx->a[7] -= 2; st16(ctx->a[7], ctx->d[3]); ctx->a[7] -= 2; st16(ctx->a[7], ctx->d[2]); ctx->a[7] -= 2; st16(ctx->a[7], ctx->d[1]); ctx->a[7] -= 2; st16(ctx->a[7], ctx->d[0]); } hle_movem_w(ctx, 24, 4);  /* 009078 movemw %d0-%d3,%sp@- */
	{ uint32_t v = 0x4u; set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00907C movew #4,%d2 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x00909C); }  /* 009080 bsrw 909c */
	{ ctx->d[0] = (uint32_t)(int32_t)(int16_t)ld16(ctx->a[7]); ctx->a[7] += 2; ctx->d[1] = (uint32_t)(int32_t)(int16_t)ld16(ctx->a[7]); ctx->a[7] += 2; ctx->d[2] = (uint32_t)(int32_t)(int16_t)ld16(ctx->a[7]); ctx->a[7] += 2; ctx->d[3] = (uint32_t)(int32_t)(int16_t)ld16(ctx->a[7]); ctx->a[7] += 2; } hle_movem_w(ctx, 28, 4);  /* 009084 movemw %sp@+,%d0-%d3 */
	hle_insn(ctx, 16); return;  /* 009088 rts */
}

HLE_FUNC_YIELD(0x00908A, Sub00908A, 4000)
{
L_00908A:
	{ ctx->a[7] -= 2; st16(ctx->a[7], ctx->d[3]); ctx->a[7] -= 2; st16(ctx->a[7], ctx->d[2]); ctx->a[7] -= 2; st16(ctx->a[7], ctx->d[1]); ctx->a[7] -= 2; st16(ctx->a[7], ctx->d[0]); } hle_movem_w(ctx, 24, 4);  /* 00908A movemw %d0-%d3,%sp@- */
	{ uint32_t v = 0x800u; set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00908E movew #2048,%d2 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x00909C); }  /* 009092 bsrw 909c */
	{ ctx->d[0] = (uint32_t)(int32_t)(int16_t)ld16(ctx->a[7]); ctx->a[7] += 2; ctx->d[1] = (uint32_t)(int32_t)(int16_t)ld16(ctx->a[7]); ctx->a[7] += 2; ctx->d[2] = (uint32_t)(int32_t)(int16_t)ld16(ctx->a[7]); ctx->a[7] += 2; ctx->d[3] = (uint32_t)(int32_t)(int16_t)ld16(ctx->a[7]); ctx->a[7] += 2; } hle_movem_w(ctx, 28, 4);  /* 009096 movemw %sp@+,%d0-%d3 */
	hle_insn(ctx, 16); return;  /* 00909A rts */
}

HLE_FUNC_YIELD(0x0092A8, Sub0092A8, 4000)
{
L_0092A8:
	{ ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[6]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[7]); } hle_movem_l(ctx, 24, 2);  /* 0092A8 moveml %d7/%fp,%sp@- */
	{ ctx->d[4] = 0x00000000u; cc_logic32(ctx, ctx->d[4]); } hle_insn(ctx, 4);  /* 0092AC moveq #0,%d4 */
	{ uint32_t eaS = 0xFFFFA0A8u; uint32_t v = ld16(eaS); set_w(&ctx->d[4], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 0092AE movew ffffa0a8,%d4 */
	{ ctx->d[3] = 0x00000002u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 0092B2 moveq #2,%d3 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x007A92); }  /* 0092B4 jsr %pc@(7a92) */
	{ uint32_t eaD = 0xFFFFA0B8u; uint32_t v = 0x1Cu; st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 20);  /* 0092B8 moveb #28,ffffa0b8 */
	{ uint32_t eaD = 0xFFFFA0B9u; uint32_t v = 0x2u; st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 20);  /* 0092C0 moveb #2,ffffa0b9 */
	{ uint32_t eaS = 0xFFFFA0B8u; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 0092C8 lea ffffa0b8,%a0 */
	{ uint32_t v = 0xC000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0092CE movew #-16384,%d6 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x007DEE); }  /* 0092D2 jsr %pc@(7dee) */
	{ ctx->d[7] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->a[6] = ld32(ctx->a[7]); ctx->a[7] += 4; } hle_movem_l(ctx, 28, 2);  /* 0092D6 moveml %sp@+,%d7/%fp */
	hle_insn(ctx, 16); return;  /* 0092DA rts */
}

HLE_FUNC_YIELD(0x009866, Sub009866, 4000)
{
L_009866:
	{ uint32_t eaS = 0xFFFFAA9Eu; ctx->a[4] = eaS; } hle_insn(ctx, 12);  /* 009866 lea ffffaa9e,%a4 */
	{ ctx->d[0] = 0x00000000u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 00986C moveq #0,%d0 */
	{ uint32_t eaD = ctx->a[4]; uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00986E movew %d0,%a4@ */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(32); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 009870 movew %d0,%a4@(32) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(64); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 009874 movew %d0,%a4@(64) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(96); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 009878 movew %d0,%a4@(96) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(128); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00987C movew %d0,%a4@(128) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(160); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 009880 movew %d0,%a4@(160) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(192); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 009884 movew %d0,%a4@(192) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(224); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 009888 movew %d0,%a4@(224) */
	hle_insn(ctx, 16); return;  /* 00988C rts */
}

HLE_FUNC_YIELD(0x009A02, Sub009A02, 4000)
{
L_009A02:
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x00930E); }  /* 009A02 jsr %pc@(930e) */
	{ cc_logic16(ctx, (ctx->d[2] & 0xFFFFu)); } hle_insn(ctx, 4);  /* 009A06 tstw %d2 */
	if (COND_MI) { hle_insn(ctx, 10); goto L_009A5C; } hle_insn(ctx, 12);  /* 009A08 bmiw 9a5c */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x009A5E); }  /* 009A0C jsr %pc@(9a5e) */
	{ ctx->a[5] = 0x12E86u; } hle_insn(ctx, 12);  /* 009A10 moveal #77446,%a5 */
	{ uint32_t eaS = ctx->a[5]; ctx->a[5] += 2; uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(26); uint32_t v = ld16(eaS); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 009A16 movew %a5@+,%fp@(26) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(22); uint32_t v = (ctx->a[5] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 16);  /* 009A1A movel %a5,%fp@(22) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(20); uint32_t v = 0x100u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 009A1E movew #256,%fp@(20) */
	{ uint32_t v = 0x9u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 009A24 movew #9,%d0 */
	{ unsigned b = 15; uint32_t m = 1u << b; cc_set(ctx, CC_Z, (ctx->d[0] & m) ? 0 : CC_Z); ctx->d[0] |= m; hle_insn(ctx, 10 + ((b > 15) ? 2 : 0)); }  /* 009A28 bset #15,%d0 */
	{ uint32_t eaD = ctx->a[6]; uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 009A2C movew %d0,%fp@ */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = 0x12Cu; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 009A2E movew #300,%fp@(36) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0xC442u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 009A34 movel #50242,%fp@(32) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(28); uint32_t v = 0x6042u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 009A3C movel #24642,%fp@(28) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(6); uint32_t v = 0x10u; st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 16);  /* 009A44 moveb #16,%fp@(6) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(7); uint32_t v = 0x96u; st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 16);  /* 009A4A moveb #-106,%fp@(7) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(8); uint32_t v = 0x0u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 009A50 movew #0,%fp@(8) */
	{ uint32_t eaD = 0xFFFF80DAu; uint32_t v = alu(ctx, OP_ADD, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 20);  /* 009A56 addqw #1,ffff80da */
L_009A5C:
	hle_insn(ctx, 16); return;  /* 009A5C rts */
}

HLE_FUNC_YIELD(0x009A5E, Sub009A5E, 4000)
{
L_009A5E:
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x00881C); }  /* 009A5E jsr %pc@(881c) */
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0x3Fu) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 009A62 andiw #63,%d0 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), 0x38u, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 8);  /* 009A66 addiw #56,%d0 */
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[4], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 009A6A movew %d0,%d4 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x00881C); }  /* 009A6C jsr %pc@(881c) */
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0x7Fu) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 009A70 andiw #127,%d0 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), 0x60u, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 8);  /* 009A74 addiw #96,%d0 */
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 4); hle_jump(ctx, 0x009A7A); return;  /* 009A78 movew %d0,%d3 */
}

HLE_FUNC_YIELD(0x011F58, VdpQueueVScroll, 4000)
{
L_011F58:
	{ uint32_t eaS = 0xFFFF80BCu; ctx->a[5] = ld32(eaS); } hle_insn(ctx, 20);  /* 011F58 moveal ffff80bc,%a5 */
	{ uint32_t eaD = ctx->a[5]; ctx->a[5] += 2; uint32_t v = 0x8u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 011F5E movew #8,%a5@+ */
	{ uint32_t eaD = ctx->a[5]; ctx->a[5] += 2; uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 011F62 movew %d0,%a5@+ */
	{ uint32_t eaD = ctx->a[5]; ctx->a[5] += 2; uint32_t v = (ctx->d[1] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 011F64 movew %d1,%a5@+ */
	{ uint32_t eaD = 0xFFFF80BCu; uint32_t v = (ctx->a[5] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 20);  /* 011F66 movel %a5,ffff80bc */
	hle_insn(ctx, 16); return;  /* 011F6C rts */
}

HLE_FUNC_YIELD(0x013DA8, Sub013DA8, 4000)
{
L_013DA8:
	{ uint32_t v = 0xE20C2u; ctx->d[0] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 013DA8 movel #925890,%d0 */
	{ uint32_t v = 0x11E2u; ctx->d[2] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 013DAE movel #4578,%d2 */
	{ uint32_t v = 0xCAu; set_b(&ctx->d[1], v); cc_logic8(ctx, v); } hle_insn(ctx, 8);  /* 013DB4 moveb #-54,%d1 */
	{ ctx->d[3] = 0x00000003u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 013DB8 moveq #3,%d3 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x013D56); return;  /* 013DBA braw 13d56 */
}

HLE_FUNC_YIELD(0x013DBE, Sub013DBE, 4000)
{
L_013DBE:
	{ uint32_t v = 0xE32A4u; ctx->d[0] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 013DBE movel #930468,%d0 */
	{ uint32_t v = 0x27C2u; ctx->d[2] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 013DC4 movel #10178,%d2 */
	{ uint32_t v = 0xCAu; set_b(&ctx->d[1], v); cc_logic8(ctx, v); } hle_insn(ctx, 8);  /* 013DCA moveb #-54,%d1 */
	{ ctx->d[3] = 0x00000004u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 013DCE moveq #4,%d3 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x013D56); return;  /* 013DD0 braw 13d56 */
}

