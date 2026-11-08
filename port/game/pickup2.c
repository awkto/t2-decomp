/* The pickup/missile spawn helper sub_012D00 (called by PlayerButtons and PlayerShoot): it picks the
 * slot with MissileAlloc, fills it from the player's record and the table at $012DDC, and starts the
 * target search.
 *
 * One function (HLE_FUNC_YIELD): every instruction is charged right after its effect, the jsr's are direct
 * C calls through hle_callc (MissileAlloc, Random, MissileFindTarget are C), so the frame loop can
 * interrupt the helper at any instruction boundary and the call never needs the 68k. Generated
 * instruction by instruction from the disassembly by tools/py/piecegen.py (pickup, whole mode); `costcheck.py`
 * checks every charge. */
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


HLE_FUNC_YIELD(0x012D00, PickupSpawn, 4000)
{
L_012D00:
	{ uint32_t eaS = ctx->a[4] + (uint32_t)(int32_t)(38); uint32_t v = ld16(eaS); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 012D00 movew %a4@(38),%d3 */
	{ uint32_t eaS = ctx->a[4] + (uint32_t)(int32_t)(40); uint32_t v = ld16(eaS); set_w(&ctx->d[4], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 012D04 movew %a4@(40),%d4 */
	{ uint32_t eaS = ctx->a[4] + (uint32_t)(int32_t)(36); uint32_t v = ld16(eaS); set_w(&ctx->d[5], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 012D08 movew %a4@(36),%d5 */
	{ uint32_t t_ = ctx->d[0]; ctx->d[0] = ctx->d[3]; ctx->d[3] = t_; } hle_insn(ctx, 6);  /* 012D0C exg %d0,%d3 */
	{ uint32_t t_ = ctx->d[1]; ctx->d[1] = ctx->d[4]; ctx->d[4] = t_; } hle_insn(ctx, 6);  /* 012D0E exg %d1,%d4 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x01290E); }  /* 012D10 jsr %pc@(1290e) */
	{ cc_logic16(ctx, (ctx->d[6] & 0xFFFFu)); } hle_insn(ctx, 4);  /* 012D14 tstw %d6 */
	if (COND_MI) { hle_insn(ctx, 10); goto L_012DDA; } hle_insn(ctx, 12);  /* 012D16 bmiw 12dda */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(30); uint32_t v = (ctx->d[5] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 012D1A movew %d5,%fp@(30) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(38); uint32_t v = (ctx->d[3] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 012D1E movew %d3,%fp@(38) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(40); uint32_t v = (ctx->d[4] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 012D22 movew %d4,%fp@(40) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = 0xBu; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 012D26 movew #11,%fp@(36) */
	{ uint32_t eaD = 0xFFFF80CCu; alu(ctx, OP_SUB, ld16(eaD), 0x7u, 16, 0); } hle_insn(ctx, 20);  /* 012D2C cmpiw #7,ffff80cc */
	if (COND_LT) { hle_insn(ctx, 10); goto L_012D3C; } hle_insn(ctx, 8);  /* 012D34 blts 12d3c */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = 0x0u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 012D36 movew #0,%fp@(36) */
L_012D3C:
	{ uint32_t v = 0x1u; set_b(&ctx->d[5], v); cc_logic8(ctx, v); } hle_insn(ctx, 8);  /* 012D3C moveb #1,%d5 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[3] & 0xFFFFu), (ctx->d[0] & 0xFFFFu), 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 4);  /* 012D40 subw %d0,%d3 */
	if (COND_PL) { hle_insn(ctx, 10); goto L_012D48; } hle_insn(ctx, 8);  /* 012D42 bpls 12d48 */
	{ uint32_t v = alu(ctx, OP_SUB, 0, (ctx->d[5] & 0xFFu), 8, 1); set_b(&ctx->d[5], v); } hle_insn(ctx, 4);  /* 012D44 negb %d5 */
	{ uint32_t v = alu(ctx, OP_SUB, 0, (ctx->d[3] & 0xFFFFu), 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 4);  /* 012D46 negw %d3 */
L_012D48:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(14); uint32_t v = (ctx->d[5] & 0xFFu); st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 12);  /* 012D48 moveb %d5,%fp@(14) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 012D4C movew %d0,%fp@(2) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(10); uint32_t v = (ctx->d[3] & 0xFFu); st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 12);  /* 012D50 moveb %d3,%fp@(10) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(11); st8(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 16);  /* 012D54 clrb %fp@(11) */
	{ uint32_t v = (ctx->d[4] & 0xFFFFu); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 012D58 movew %d4,%d3 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[4] & 0xFFFFu), (ctx->d[1] & 0xFFFFu), 16, 1); set_w(&ctx->d[4], v); } hle_insn(ctx, 4);  /* 012D5A subw %d1,%d4 */
	if (COND_PL) { hle_insn(ctx, 10); goto L_012D60; } hle_insn(ctx, 8);  /* 012D5C bpls 12d60 */
	{ uint32_t v = alu(ctx, OP_SUB, 0, (ctx->d[4] & 0xFFFFu), 16, 1); set_w(&ctx->d[4], v); } hle_insn(ctx, 4);  /* 012D5E negw %d4 */
L_012D60:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(15); uint32_t v = 0xFFu; st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 16);  /* 012D60 moveb #-1,%fp@(15) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(4); uint32_t v = (ctx->d[1] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 012D66 movew %d1,%fp@(4) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(12); uint32_t v = (ctx->d[4] & 0xFFu); st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 12);  /* 012D6A moveb %d4,%fp@(12) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(13); st8(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 16);  /* 012D6E clrb %fp@(13) */
	{ uint32_t eaD = ctx->a[6]; uint32_t v = (ctx->d[2] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 012D72 movew %d2,%fp@ */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[2] & 0xFFFFu), (ctx->d[2] & 0xFFFFu), 16, 1); set_w(&ctx->d[2], v); } hle_insn(ctx, 4);  /* 012D74 addw %d2,%d2 */
	{ ctx->a[0] = 0x12DDCu; } hle_insn(ctx, 12);  /* 012D76 moveal #77276,%a0 */
	{ uint32_t eaS = ctx->a[0] + (uint32_t)(int32_t)(0) + (uint32_t)(int32_t)(int16_t)ctx->d[2]; uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = ld16(eaS); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 22);  /* 012D7C movew %a0@(0,%d2:w),%fp@(32) */
	{ ctx->a[0] = 0x12DEAu; } hle_insn(ctx, 12);  /* 012D82 moveal #77290,%a0 */
	{ uint32_t v = shf(ctx, 'L', (ctx->d[2] & 0xFFFFu), 1, 16); set_w(&ctx->d[2], v); } hle_insn(ctx, 8);  /* 012D88 aslw #1,%d2 */
	{ uint32_t eaS = ctx->a[0] + (uint32_t)(int32_t)(0) + (uint32_t)(int32_t)(int16_t)ctx->d[2]; ctx->a[0] = ld32(eaS); } hle_insn(ctx, 18);  /* 012D8A moveal %a0@(0,%d2:w),%a0 */
	{ alu(ctx, OP_SUB, (ctx->d[2] & 0xFFFFu), 0xCu, 16, 0); } hle_insn(ctx, 8);  /* 012D8E cmpiw #12,%d2 */
	if (COND_GT) { hle_insn(ctx, 10); goto L_012DA2; } hle_insn(ctx, 8);  /* 012D92 bgts 12da2 */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_callc(ctx, 0x00881C); }  /* 012D94 jsr 881c */
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0x30u) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 012D9A andiw #48,%d0 */
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 012D9E movew %d0,%d3 */
	hle_insn(ctx, 10); goto L_012DA8;  /* 012DA0 bras 12da8 */
L_012DA2:
	{ uint32_t v = shf(ctx, 'a', (ctx->d[3] & 0xFFFFu), 2, 16); set_w(&ctx->d[3], v); } hle_insn(ctx, 10);  /* 012DA2 asrw #2,%d3 */
	{ uint32_t v = ((ctx->d[3] & 0xFFFFu) & 0xFFF0u) & 0xFFFFu; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 012DA4 andiw #-16,%d3 */
L_012DA8:
	{ uint32_t eaS = ctx->a[0] + (uint32_t)(int32_t)(0) + (uint32_t)(int32_t)(int16_t)ctx->d[3]; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 012DA8 lea %a0@(0,%d3:w),%a0 */
	{ uint32_t eaS = ctx->a[0]; ctx->a[0] += 2; uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(26); uint32_t v = ld16(eaS); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 012DAC movew %a0@+,%fp@(26) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(22); uint32_t v = (ctx->a[0] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 16);  /* 012DB0 movel %a0,%fp@(22) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(20); uint32_t v = 0x100u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 012DB4 movew #256,%fp@(20) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(28); uint32_t v = 0xCu; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 012DBA movew #12,%fp@(28) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(6); uint32_t v = 0x5u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 012DC0 movew #5,%fp@(6) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); alu(ctx, OP_SUB, ld8(eaD), 0x4u, 8, 0); } hle_insn(ctx, 16);  /* 012DC6 cmpib #4,%fp@(32) */
	if (COND_NE) { hle_insn(ctx, 10); goto L_012DD4; } hle_insn(ctx, 8);  /* 012DCC bnes 12dd4 */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_callc(ctx, 0x0073A0); }  /* 012DCE jsr 73a0 */
L_012DD4:
	{ uint32_t eaD = 0xFFFF80D6u; uint32_t v = alu(ctx, OP_ADD, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 20);  /* 012DD4 addqw #1,ffff80d6 */
L_012DDA:
	hle_insn(ctx, 16); return;  /* 012DDA rts */
}
