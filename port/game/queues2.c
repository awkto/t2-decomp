/* Small list/spawn routines that BgStreamLayers and ScrTextClear call: VdpQueueFillRows $012212 (one
 * fill-rows command per row), the fixed list entry sub_00BF46, and the two object row spawners
 * sub_013470 / sub_01385A (they share the tail sub_013866).
 *
 * Each is one function (HLE_FUNC_YIELD): every instruction is charged right after its effect and the jsr's
 * are direct C calls through hle_callc, so the frame loop can interrupt them at any instruction boundary
 * and no call needs the 68k. Generated instruction by instruction from the disassembly by
 * tools/py/piecegen.py (queues, whole mode); `costcheck.py` checks every charge. */
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


HLE_FUNC_YIELD(0x00BF46, VdpQueueBand, 4000)
{
L_00BF46:
	{ uint32_t eaS = 0xFFFF80BCu; ctx->a[5] = ld32(eaS); } hle_insn(ctx, 20);  /* 00BF46 moveal ffff80bc,%a5 */
	{ uint32_t eaD = ctx->a[5]; ctx->a[5] += 2; uint32_t v = 0x2u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00BF4C movew #2,%a5@+ */
	{ uint32_t eaD = ctx->a[5]; ctx->a[5] += 2; uint32_t v = 0x500u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00BF50 movew #1280,%a5@+ */
	{ uint32_t eaD = ctx->a[5]; ctx->a[5] += 2; uint32_t v = 0xE100u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00BF54 movew #-7936,%a5@+ */
	{ uint32_t eaD = 0xFFFF80BCu; uint32_t v = (ctx->a[5] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 20);  /* 00BF58 movel %a5,ffff80bc */
	hle_insn(ctx, 16); return;  /* 00BF5E rts */
}

HLE_FUNC_YIELD(0x012212, VdpQueueFillRows, 4000)
{
L_012212:
	{ uint32_t eaS = 0xFFFF80BCu; ctx->a[5] = ld32(eaS); } hle_insn(ctx, 20);  /* 012212 moveal ffff80bc,%a5 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[3] & 0xFFFFu), 0x1u, 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 4);  /* 012218 subqw #1,%d3 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[2] & 0xFFFFu), (ctx->d[2] & 0xFFFFu), 16, 1); set_w(&ctx->d[2], v); } hle_insn(ctx, 4);  /* 01221A addw %d2,%d2 */
L_01221C:
	{ uint32_t eaD = ctx->a[5]; ctx->a[5] += 2; uint32_t v = 0x2u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 01221C movew #2,%a5@+ */
	{ uint32_t eaD = ctx->a[5]; ctx->a[5] += 2; uint32_t v = (ctx->d[2] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 012220 movew %d2,%a5@+ */
	{ uint32_t eaD = ctx->a[5]; ctx->a[5] += 2; uint32_t v = (ctx->d[1] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 012222 movew %d1,%a5@+ */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0x80u, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 012224 addiw #128,%d1 */
	{ uint16_t v = (uint16_t)ctx->d[3] - 1; set_w(&ctx->d[3], v); if (v != 0xFFFF) { hle_insn(ctx, 10); goto L_01221C; } hle_insn(ctx, 14); }  /* 012228 dbf %d3,1221c */
	{ uint32_t eaD = 0xFFFF80BCu; uint32_t v = (ctx->a[5] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 20);  /* 01222C movel %a5,ffff80bc */
	hle_insn(ctx, 16); return;  /* 012232 rts */
}

HLE_FUNC_YIELD(0x013470, SpawnRowA, 4000)
{
L_013470:
	{ ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[6]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[5]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[4]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[3]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[2]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[1]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[0]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[7]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[6]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[5]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[4]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[3]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[2]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[1]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[0]); } hle_movem_l(ctx, 128, 15);  /* 013470 moveml %d0-%fp,%sp@- */
	{ uint32_t eaS = 0x00013480u; ctx->a[4] = eaS; } hle_insn(ctx, 8);  /* 013474 lea %pc@(13480),%a4 */
	{ uint32_t eaS = ctx->a[4] + (uint32_t)(int32_t)(0) + (uint32_t)(int32_t)(int16_t)ctx->d[5]; ctx->a[4] = ld32(eaS); } hle_insn(ctx, 18);  /* 013478 moveal %a4@(0,%d5:w),%a4 */
	hle_insn(ctx, 10); goto L_013866;  /* 01347C braw 13866 */
L_013866:
	{ uint32_t eaS = 0xFFFFA238u; uint32_t v = ld16(eaS); set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 013866 movew ffffa238,%d6 */
	{ uint32_t v = alu(ctx, OP_SUB, 0, (ctx->d[6] & 0xFFFFu), 16, 1); set_w(&ctx->d[6], v); } hle_insn(ctx, 4);  /* 01386C negw %d6 */
	{ uint32_t v = ((ctx->d[6] & 0xFFFFu) & 0x1FFu) & 0xFFFFu; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 01386E andiw #511,%d6 */
	{ uint32_t eaD = 0xFFFFA238u; uint32_t v = (ctx->d[6] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 013872 movew %d6,ffffa238 */
	{ uint32_t eaS = ctx->a[4]; ctx->a[4] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 013878 movew %a4@+,%d3 */
L_01387A:
	{ uint32_t eaD = ctx->a[4]; alu(ctx, OP_SUB, ld16(eaD), 0xFFFFu, 16, 0); } hle_insn(ctx, 12);  /* 01387A cmpiw #-1,%a4@ */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_013908; } hle_insn(ctx, 12);  /* 01387E beqw 13908 */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_callc(ctx, 0x00930E); }  /* 013882 jsr 930e */
	{ uint32_t eaS = ctx->a[4]; ctx->a[4] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 013888 movew %a4@+,%d6 */
	{ uint32_t eaS = 0xFFFFA238u; uint32_t v = alu(ctx, OP_ADD, (ctx->d[6] & 0xFFFFu), ld16(eaS), 16, 1); set_w(&ctx->d[6], v); } hle_insn(ctx, 16);  /* 01388A addw ffffa238,%d6 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = (ctx->d[6] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 013890 movew %d6,%fp@(2) */
	{ uint32_t v = 0x10u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 013894 movew #16,%d6 */
	{ uint32_t eaS = ctx->a[4]; ctx->a[4] += 2; uint32_t v = alu(ctx, OP_ADD, (ctx->d[6] & 0xFFFFu), ld16(eaS), 16, 1); set_w(&ctx->d[6], v); } hle_insn(ctx, 8);  /* 013898 addw %a4@+,%d6 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(4); uint32_t v = (ctx->d[6] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 01389A movew %d6,%fp@(4) */
	{ uint32_t eaS = ctx->a[4]; ctx->a[4] += 2; uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(26); uint32_t v = ld16(eaS); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 01389E movew %a4@+,%fp@(26) */
	{ uint32_t eaS = ctx->a[4]; ctx->a[4] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0138A2 movew %a4@+,%d6 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[6] & 0xFFFFu), 0x5A0u, 16, 1); set_w(&ctx->d[6], v); } hle_insn(ctx, 8);  /* 0138A4 addiw #1440,%d6 */
	{ uint32_t eaS = ctx->a[0] + (uint32_t)(int32_t)(0) + (uint32_t)(int32_t)(int16_t)ctx->d[6]; ctx->a[1] = eaS; } hle_insn(ctx, 12);  /* 0138A8 lea %a0@(0,%d6:w),%a1 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(22); uint32_t v = (ctx->a[1] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 16);  /* 0138AC movel %a1,%fp@(22) */
	{ uint32_t v = (ctx->d[1] & 0xFFFFu); set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 0138B0 movew %d1,%d6 */
	{ uint32_t eaS = ctx->a[4]; ctx->a[4] += 2; uint32_t v = alu(ctx, OP_ADD, (ctx->d[6] & 0xFFFFu), ld16(eaS), 16, 1); set_w(&ctx->d[6], v); } hle_insn(ctx, 8);  /* 0138B2 addw %a4@+,%d6 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(10); uint32_t v = (ctx->d[6] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 0138B4 movew %d6,%fp@(10) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(8); uint32_t v = 0x0u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 0138B8 movew #0,%fp@(8) */
	{ uint32_t v = 0xFu; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0138BE movew #15,%d6 */
	{ unsigned b = 15; uint32_t m = 1u << b; cc_set(ctx, CC_Z, (ctx->d[6] & m) ? 0 : CC_Z); ctx->d[6] |= m; hle_insn(ctx, 10 + ((b > 15) ? 2 : 0)); }  /* 0138C2 bset #15,%d6 */
	{ uint32_t eaD = ctx->a[6]; uint32_t v = (ctx->d[6] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0138C6 movew %d6,%fp@ */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0xF8A2u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 0138C8 movel #63650,%fp@(32) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(28); uint32_t v = 0x69EEu; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 0138D0 movel #27118,%fp@(28) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(6); uint32_t v = (ctx->d[3] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 0138D8 movew %d3,%fp@(6) */
	{ uint32_t eaS = ctx->a[4]; ctx->a[4] += 2; uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(38); uint32_t v = ld16(eaS); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 0138DC movew %a4@+,%fp@(38) */
	{ uint32_t eaS = ctx->a[4]; ctx->a[4] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0138E0 movew %a4@+,%d0 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(40); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 0138E2 movew %d0,%fp@(40) */
	{ unsigned b = 1; uint32_t m = 1u << b; cc_set(ctx, CC_Z, (ctx->d[0] & m) ? 0 : CC_Z);  hle_insn(ctx, 10); }  /* 0138E6 btst #1,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0138F2; } hle_insn(ctx, 8);  /* 0138EA bnes 138f2 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(8); uint32_t v = 0x5u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 0138EC movew #5,%fp@(8) */
L_0138F2:
	{ unsigned b = 2; uint32_t m = 1u << b; cc_set(ctx, CC_Z, (ctx->d[0] & m) ? 0 : CC_Z);  hle_insn(ctx, 10); }  /* 0138F2 btst #2,%d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0138FE; } hle_insn(ctx, 8);  /* 0138F6 beqs 138fe */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(8); uint32_t v = 0x19u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 0138F8 movew #25,%fp@(8) */
L_0138FE:
	{ uint32_t eaD = 0xFFFF80DAu; uint32_t v = alu(ctx, OP_ADD, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 20);  /* 0138FE addqw #1,ffff80da */
	hle_insn(ctx, 10); goto L_01387A;  /* 013904 braw 1387a */
L_013908:
	{ ctx->d[0] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->d[1] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->d[2] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->d[3] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->d[4] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->d[5] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->d[6] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->d[7] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->a[0] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->a[1] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->a[2] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->a[3] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->a[4] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->a[5] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->a[6] = ld32(ctx->a[7]); ctx->a[7] += 4; } hle_movem_l(ctx, 132, 15);  /* 013908 moveml %sp@+,%d0-%fp */
	hle_insn(ctx, 16); return;  /* 01390C rts */
}

HLE_FUNC_YIELD(0x01385A, SpawnRowB, 4000)
{
L_01385A:
	{ ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[6]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[5]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[4]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[3]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[2]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[1]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[0]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[7]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[6]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[5]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[4]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[3]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[2]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[1]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[0]); } hle_movem_l(ctx, 128, 15);  /* 01385A moveml %d0-%fp,%sp@- */
	{ uint32_t eaS = 0x0001390Eu; ctx->a[4] = eaS; } hle_insn(ctx, 8);  /* 01385E lea %pc@(1390e),%a4 */
	{ uint32_t eaS = ctx->a[4] + (uint32_t)(int32_t)(0) + (uint32_t)(int32_t)(int16_t)ctx->d[5]; ctx->a[4] = ld32(eaS); } hle_insn(ctx, 18);  /* 013862 moveal %a4@(0,%d5:w),%a4 */
	{ uint32_t eaS = 0xFFFFA238u; uint32_t v = ld16(eaS); set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 013866 movew ffffa238,%d6 */
	{ uint32_t v = alu(ctx, OP_SUB, 0, (ctx->d[6] & 0xFFFFu), 16, 1); set_w(&ctx->d[6], v); } hle_insn(ctx, 4);  /* 01386C negw %d6 */
	{ uint32_t v = ((ctx->d[6] & 0xFFFFu) & 0x1FFu) & 0xFFFFu; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 01386E andiw #511,%d6 */
	{ uint32_t eaD = 0xFFFFA238u; uint32_t v = (ctx->d[6] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 013872 movew %d6,ffffa238 */
	{ uint32_t eaS = ctx->a[4]; ctx->a[4] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 013878 movew %a4@+,%d3 */
L_01387A:
	{ uint32_t eaD = ctx->a[4]; alu(ctx, OP_SUB, ld16(eaD), 0xFFFFu, 16, 0); } hle_insn(ctx, 12);  /* 01387A cmpiw #-1,%a4@ */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_013908; } hle_insn(ctx, 12);  /* 01387E beqw 13908 */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_callc(ctx, 0x00930E); }  /* 013882 jsr 930e */
	{ uint32_t eaS = ctx->a[4]; ctx->a[4] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 013888 movew %a4@+,%d6 */
	{ uint32_t eaS = 0xFFFFA238u; uint32_t v = alu(ctx, OP_ADD, (ctx->d[6] & 0xFFFFu), ld16(eaS), 16, 1); set_w(&ctx->d[6], v); } hle_insn(ctx, 16);  /* 01388A addw ffffa238,%d6 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = (ctx->d[6] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 013890 movew %d6,%fp@(2) */
	{ uint32_t v = 0x10u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 013894 movew #16,%d6 */
	{ uint32_t eaS = ctx->a[4]; ctx->a[4] += 2; uint32_t v = alu(ctx, OP_ADD, (ctx->d[6] & 0xFFFFu), ld16(eaS), 16, 1); set_w(&ctx->d[6], v); } hle_insn(ctx, 8);  /* 013898 addw %a4@+,%d6 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(4); uint32_t v = (ctx->d[6] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 01389A movew %d6,%fp@(4) */
	{ uint32_t eaS = ctx->a[4]; ctx->a[4] += 2; uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(26); uint32_t v = ld16(eaS); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 01389E movew %a4@+,%fp@(26) */
	{ uint32_t eaS = ctx->a[4]; ctx->a[4] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0138A2 movew %a4@+,%d6 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[6] & 0xFFFFu), 0x5A0u, 16, 1); set_w(&ctx->d[6], v); } hle_insn(ctx, 8);  /* 0138A4 addiw #1440,%d6 */
	{ uint32_t eaS = ctx->a[0] + (uint32_t)(int32_t)(0) + (uint32_t)(int32_t)(int16_t)ctx->d[6]; ctx->a[1] = eaS; } hle_insn(ctx, 12);  /* 0138A8 lea %a0@(0,%d6:w),%a1 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(22); uint32_t v = (ctx->a[1] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 16);  /* 0138AC movel %a1,%fp@(22) */
	{ uint32_t v = (ctx->d[1] & 0xFFFFu); set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 0138B0 movew %d1,%d6 */
	{ uint32_t eaS = ctx->a[4]; ctx->a[4] += 2; uint32_t v = alu(ctx, OP_ADD, (ctx->d[6] & 0xFFFFu), ld16(eaS), 16, 1); set_w(&ctx->d[6], v); } hle_insn(ctx, 8);  /* 0138B2 addw %a4@+,%d6 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(10); uint32_t v = (ctx->d[6] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 0138B4 movew %d6,%fp@(10) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(8); uint32_t v = 0x0u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 0138B8 movew #0,%fp@(8) */
	{ uint32_t v = 0xFu; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0138BE movew #15,%d6 */
	{ unsigned b = 15; uint32_t m = 1u << b; cc_set(ctx, CC_Z, (ctx->d[6] & m) ? 0 : CC_Z); ctx->d[6] |= m; hle_insn(ctx, 10 + ((b > 15) ? 2 : 0)); }  /* 0138C2 bset #15,%d6 */
	{ uint32_t eaD = ctx->a[6]; uint32_t v = (ctx->d[6] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0138C6 movew %d6,%fp@ */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0xF8A2u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 0138C8 movel #63650,%fp@(32) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(28); uint32_t v = 0x69EEu; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 0138D0 movel #27118,%fp@(28) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(6); uint32_t v = (ctx->d[3] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 0138D8 movew %d3,%fp@(6) */
	{ uint32_t eaS = ctx->a[4]; ctx->a[4] += 2; uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(38); uint32_t v = ld16(eaS); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 0138DC movew %a4@+,%fp@(38) */
	{ uint32_t eaS = ctx->a[4]; ctx->a[4] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0138E0 movew %a4@+,%d0 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(40); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 0138E2 movew %d0,%fp@(40) */
	{ unsigned b = 1; uint32_t m = 1u << b; cc_set(ctx, CC_Z, (ctx->d[0] & m) ? 0 : CC_Z);  hle_insn(ctx, 10); }  /* 0138E6 btst #1,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0138F2; } hle_insn(ctx, 8);  /* 0138EA bnes 138f2 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(8); uint32_t v = 0x5u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 0138EC movew #5,%fp@(8) */
L_0138F2:
	{ unsigned b = 2; uint32_t m = 1u << b; cc_set(ctx, CC_Z, (ctx->d[0] & m) ? 0 : CC_Z);  hle_insn(ctx, 10); }  /* 0138F2 btst #2,%d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0138FE; } hle_insn(ctx, 8);  /* 0138F6 beqs 138fe */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(8); uint32_t v = 0x19u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 0138F8 movew #25,%fp@(8) */
L_0138FE:
	{ uint32_t eaD = 0xFFFF80DAu; uint32_t v = alu(ctx, OP_ADD, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 20);  /* 0138FE addqw #1,ffff80da */
	hle_insn(ctx, 10); goto L_01387A;  /* 013904 braw 1387a */
L_013908:
	{ ctx->d[0] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->d[1] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->d[2] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->d[3] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->d[4] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->d[5] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->d[6] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->d[7] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->a[0] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->a[1] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->a[2] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->a[3] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->a[4] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->a[5] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->a[6] = ld32(ctx->a[7]); ctx->a[7] += 4; } hle_movem_l(ctx, 132, 15);  /* 013908 moveml %sp@+,%d0-%fp */
	hle_insn(ctx, 16); return;  /* 01390C rts */
}

