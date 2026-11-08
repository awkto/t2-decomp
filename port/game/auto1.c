/* Routines that were still 68k (docs/remaining68k.md "no C block"), translated without changes by
 * `piecegen.py auto`: 277 functions, 2901 instructions; the functions that touch hardware or use an instruction the
 * generator does not know stay 68k (docs/remaining68k.md).
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


#pragma GCC diagnostic ignored "-Wunused-label"
#pragma GCC diagnostic ignored "-Wunused-variable"

static void pc_run_0014DE(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x0014DE: goto L_0014DE;
	default: __builtin_trap();
	}
L_0014DE:
	{ uint32_t eaS = 0xFFFFAE84u; ctx->a[1] = eaS; } hle_insn(ctx, 12);  /* 0014DE lea ffffae84,%a1 */
	{ uint32_t eaS = 0x000014F2u; ctx->a[2] = eaS; } hle_insn(ctx, 8);  /* 0014E4 lea %pc@(14f2),%a2 */
	{ ctx->d[0] = 0x00000045u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 0014E8 moveq #69,%d0 */
L_0014EA:
	{ uint32_t eaS = ctx->a[2]; ctx->a[2] += 1; uint32_t eaD = ctx->a[1]; ctx->a[1] += 1; uint32_t v = ld8(eaS); st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 12);  /* 0014EA moveb %a2@+,%a1@+ */
	{ uint16_t v = (uint16_t)ctx->d[0] - 1; set_w(&ctx->d[0], v); if (v != 0xFFFF) { hle_insn(ctx, 10); goto L_0014EA; } hle_insn(ctx, 14); }  /* 0014EC dbf %d0,14ea */
	hle_jump(ctx, 0x0014F0); return;  /* 0014F0 rts */
}

HLE_RANGE_PURE(0x0014DE, Auto_0014DE, 200, 0x0014F0) { pc_run_0014DE(ctx, 0x0014DE); }
static void pc_run_001538(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x001538: goto L_001538;
	default: __builtin_trap();
	}
L_001538:
	{ uint32_t eaS = ctx->a[1]; uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 001538 movew %a1@,%d0 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), 0x5Eu, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 8);  /* 00153A addiw #94,%d0 */
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x67u, 16, 0); } hle_insn(ctx, 8);  /* 00153E cmpiw #103,%d0 */
	if (COND_LT) { hle_insn(ctx, 10); goto L_001550; } hle_insn(ctx, 8);  /* 001542 blts 1550 */
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x6Eu, 16, 0); } hle_insn(ctx, 8);  /* 001544 cmpiw #110,%d0 */
	if (COND_GT) { hle_insn(ctx, 10); goto L_001550; } hle_insn(ctx, 8);  /* 001548 bgts 1550 */
	hle_jump(ctx, 0x00154A); return;  /* 00154A jmp fbbae */
L_001550:
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x80u, 16, 0); } hle_insn(ctx, 8);  /* 001550 cmpiw #128,%d0 */
	if (COND_LT) { hle_insn(ctx, 10); goto L_001558; } hle_insn(ctx, 8);  /* 001554 blts 1558 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), 0x1u, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 4);  /* 001556 addqw #1,%d0 */
L_001558:
	hle_jump(ctx, 0x001558); return;  /* 001558 jmp fbb96 */
}

HLE_RANGE_PURE(0x001538, Auto_001538, 200, 0x00154A, 0x001558) { pc_run_001538(ctx, 0x001538); }
static void pc_run_001598(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x001598: goto L_001598;
	default: __builtin_trap();
	}
L_001598:
	{ uint32_t eaD = 0xFFFFA234u; uint32_t v = 0xFFu; st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 20);  /* 001598 moveb #-1,ffffa234 */
	hle_jump(ctx, 0x0015A0); return;  /* 0015A0 rts */
}

HLE_RANGE_PURE(0x001598, Auto_001598, 200, 0x0015A0) { pc_run_001598(ctx, 0x001598); }
static void pc_run_0015A2(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x0015AA: goto L_0015AA;
	case 0x0015B4: goto L_0015B4;
	case 0x0015DE: goto L_0015DE;
	default: __builtin_trap();
	}
L_0015A2:
	hle_jump(ctx, 0x0015A2); return;  /* 0015A2 bsrw 4212 */
L_0015A6:
	hle_jump(ctx, 0x0015A6); return;  /* 0015A6 jsr %pc@(871a) */
L_0015AA:
	{ uint32_t eaD = 0xFFFFA246u;  st8(eaD, 0xFF); } hle_insn(ctx, 20);  /* 0015AA st ffffa246 */
	hle_jump(ctx, 0x0015B0); return;  /* 0015B0 jsr %pc@(8a4c) */
L_0015B4:
	{ uint32_t v = 0xE614u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0015B4 movew #-6636,%d3 */
	{ uint32_t eaS = 0x00003295u; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 0015B8 lea %pc@(3295),%a0 */
	{ uint32_t v = 0x2000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0015BC movew #8192,%d6 */
L_0015C0:
	hle_jump(ctx, 0x0015C0); return;  /* 0015C0 jsr 11d12 */
L_0015C6:
	hle_jump(ctx, 0x0015C6); return;  /* 0015C6 jsr %pc@(7f00) */
L_0015CA:
	hle_jump(ctx, 0x0015CA); return;  /* 0015CA jsr 11d28 */
L_0015D0:
	hle_jump(ctx, 0x0015D0); return;  /* 0015D0 jsr %pc@(86e2) */
L_0015D4:
	hle_jump(ctx, 0x0015D4); return;  /* 0015D4 jsr %pc@(86e2) */
L_0015D8:
	hle_jump(ctx, 0x0015D8); return;  /* 0015D8 jsr 11d32 */
L_0015DE:
	{ uint32_t eaD = ctx->a[0]; cc_logic8(ctx, ld8(eaD)); } hle_insn(ctx, 8);  /* 0015DE tstb %a0@ */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0015C0; } hle_insn(ctx, 8);  /* 0015E0 bnes 15c0 */
	{ uint32_t v = 0xC8u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0015E2 movew #200,%d1 */
	hle_jump(ctx, 0x0015E6); return;  /* 0015E6 jsr %pc@(86d8) */
L_0015EA:
	hle_jump(ctx, 0x0015EA); return;  /* 0015EA rts */
}

HLE_RANGE_PURE(0x0015AA, Auto_0015AA, 200, 0x0015B0) { pc_run_0015A2(ctx, 0x0015AA); }
HLE_RANGE_PURE(0x0015B4, Auto_0015B4, 200, 0x0015C0) { pc_run_0015A2(ctx, 0x0015B4); }
HLE_RANGE_PURE(0x0015DE, Auto_0015DE, 200, 0x0015C0, 0x0015E6) { pc_run_0015A2(ctx, 0x0015DE); }
static void pc_run_001C4A(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x001C4E: goto L_001C4E;
	case 0x001C76: goto L_001C76;
	case 0x001C80: goto L_001C80;
	case 0x001CA6: goto L_001CA6;
	case 0x001CB0: goto L_001CB0;
	default: __builtin_trap();
	}
L_001C4A:
	hle_jump(ctx, 0x001C4A); return;  /* 001C4A bsrw 4212 */
L_001C4E:
	{ uint32_t eaD = 0xFFFF9E2Cu; st16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 20);  /* 001C4E clrw ffff9e2c */
	{ uint32_t v = 0xE084u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 001C54 movew #-8060,%d3 */
	{ uint32_t eaS = 0x00001CD6u; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 001C58 lea %pc@(1cd6),%a0 */
	{ uint32_t v = 0xA000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 001C5C movew #-24576,%d6 */
L_001C60:
	hle_jump(ctx, 0x001C60); return;  /* 001C60 jsr 11d12 */
L_001C66:
	hle_jump(ctx, 0x001C66); return;  /* 001C66 jsr %pc@(7f00) */
L_001C6A:
	hle_jump(ctx, 0x001C6A); return;  /* 001C6A jsr 11d28 */
L_001C70:
	hle_jump(ctx, 0x001C70); return;  /* 001C70 jsr 11d32 */
L_001C76:
	{ uint32_t eaD = ctx->a[0]; cc_logic8(ctx, ld8(eaD)); } hle_insn(ctx, 8);  /* 001C76 tstb %a0@ */
	if (COND_NE) { hle_insn(ctx, 10); goto L_001C60; } hle_insn(ctx, 8);  /* 001C78 bnes 1c60 */
	hle_jump(ctx, 0x001C7A); return;  /* 001C7A jsr b2fa */
L_001C80:
	{ uint32_t eaD = 0xFFFF9F6Du; uint32_t v = 0x2u; st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 20);  /* 001C80 moveb #2,ffff9f6d */
	{ uint32_t eaD = 0xFFFF9DF5u; alu(ctx, OP_SUB, ld8(eaD), 0x3u, 8, 0); } hle_insn(ctx, 20);  /* 001C88 cmpib #3,ffff9df5 */
	if (COND_LT) { hle_insn(ctx, 10); goto L_001C98; } hle_insn(ctx, 8);  /* 001C90 blts 1c98 */
	{ uint32_t eaD = 0xFFFF9F6Du; uint32_t v = alu(ctx, OP_SUB, ld8(eaD), 0x1u, 8, 1); st8(eaD, v); } hle_insn(ctx, 20);  /* 001C92 subqb #1,ffff9f6d */
L_001C98:
	{ uint32_t eaS = 0x000083DEu; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 001C98 lea %pc@(83de),%a0 */
	{ uint32_t eaS = 0xFFFFA134u; ctx->a[1] = eaS; } hle_insn(ctx, 12);  /* 001C9C lea ffffa134,%a1 */
	hle_jump(ctx, 0x001CA2); return;  /* 001CA2 jsr %pc@(8a7c) */
L_001CA6:
	{ uint32_t eaD = 0xFFFFA246u;  st8(eaD, 0xFF); } hle_insn(ctx, 20);  /* 001CA6 st ffffa246 */
	hle_jump(ctx, 0x001CAC); return;  /* 001CAC jsr %pc@(8a4c) */
L_001CB0:
	{ uint32_t eaS = 0xFFFF8000u; ctx->a[4] = eaS; } hle_insn(ctx, 12);  /* 001CB0 lea ffff8000,%a4 */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(20); uint32_t v = 0x0u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 001CB6 movew #0,%a4@(20) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(22); uint32_t v = 0x0u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 001CBC movew #0,%a4@(22) */
	{ uint32_t eaS = 0xFFFF8050u; ctx->a[4] = eaS; } hle_insn(ctx, 12);  /* 001CC2 lea ffff8050,%a4 */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(20); uint32_t v = 0x0u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 001CC8 movew #0,%a4@(20) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(22); uint32_t v = 0x0u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 001CCE movew #0,%a4@(22) */
	hle_jump(ctx, 0x001CD4); return;  /* 001CD4 rts */
}

HLE_RANGE_PURE(0x001C4E, Auto_001C4E, 200, 0x001C60) { pc_run_001C4A(ctx, 0x001C4E); }
HLE_RANGE_PURE(0x001C76, Auto_001C76, 200, 0x001C60, 0x001C7A) { pc_run_001C4A(ctx, 0x001C76); }
HLE_RANGE_PURE(0x001C80, Auto_001C80, 200, 0x001CA2) { pc_run_001C4A(ctx, 0x001C80); }
HLE_RANGE_PURE(0x001CA6, Auto_001CA6, 200, 0x001CAC) { pc_run_001C4A(ctx, 0x001CA6); }
HLE_RANGE_PURE(0x001CB0, Auto_001CB0, 200, 0x001CD4) { pc_run_001C4A(ctx, 0x001CB0); }
static void pc_run_00240E(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00240E: goto L_00240E;
	default: __builtin_trap();
	}
L_00240E:
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(24); alu(ctx, OP_SUB, ld16(eaD), 0x25u, 16, 0); } hle_insn(ctx, 16);  /* 00240E cmpiw #37,%a4@(24) */
	if (COND_LE) { hle_insn(ctx, 10); goto L_00241C; } hle_insn(ctx, 8);  /* 002414 bles 241c */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(24); uint32_t v = 0x25u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 002416 movew #37,%a4@(24) */
L_00241C:
	hle_jump(ctx, 0x00241C); return;  /* 00241C rts */
}

HLE_RANGE_PURE(0x00240E, Auto_00240E, 200, 0x00241C) { pc_run_00240E(ctx, 0x00240E); }
static void pc_run_002B08(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x002B08: goto L_002B08;
	case 0x002B18: goto L_002B18;
	case 0x002B24: goto L_002B24;
	case 0x002B30: goto L_002B30;
	default: __builtin_trap();
	}
L_002B08:
	{ uint32_t eaS = 0x00002BC8u; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 002B08 lea %pc@(2bc8),%a0 */
	{ uint32_t v = 0xC004u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 002B0C movew #-16380,%d3 */
	{ uint32_t v = 0xC000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 002B10 movew #-16384,%d6 */
	hle_jump(ctx, 0x002B14); return;  /* 002B14 jsr %pc@(7e08) */
L_002B18:
	{ uint32_t eaS = 0x00002BD8u; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 002B18 lea %pc@(2bd8),%a0 */
	{ uint32_t v = 0xC030u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 002B1C movew #-16336,%d3 */
	hle_jump(ctx, 0x002B20); return;  /* 002B20 jsr %pc@(7e08) */
L_002B24:
	{ uint32_t eaS = 0x00002BE8u; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 002B24 lea %pc@(2be8),%a0 */
	{ uint32_t v = 0xC080u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 002B28 movew #-16256,%d3 */
	hle_jump(ctx, 0x002B2C); return;  /* 002B2C jsr %pc@(7eb2) */
L_002B30:
	{ uint32_t eaS = 0x00002C11u; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 002B30 lea %pc@(2c11),%a0 */
	{ uint32_t v = 0xC024u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 002B34 movew #-16348,%d3 */
	hle_jump(ctx, 0x002B38); return;  /* 002B38 jsr %pc@(7eb2) */
L_002B3C:
	hle_jump(ctx, 0x002B3C); return;  /* 002B3C jsr %pc@(2b6c) */
L_002B40:
	hle_jump(ctx, 0x002B40); return;  /* 002B40 rts */
}

HLE_RANGE_PURE(0x002B08, Auto_002B08, 200, 0x002B14) { pc_run_002B08(ctx, 0x002B08); }
HLE_RANGE_PURE(0x002B18, Auto_002B18, 200, 0x002B20) { pc_run_002B08(ctx, 0x002B18); }
HLE_RANGE_PURE(0x002B24, Auto_002B24, 200, 0x002B2C) { pc_run_002B08(ctx, 0x002B24); }
HLE_RANGE_PURE(0x002B30, Auto_002B30, 200, 0x002B38) { pc_run_002B08(ctx, 0x002B30); }
static void pc_run_002B42(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x002B42: goto L_002B42;
	case 0x002B4E: goto L_002B4E;
	default: __builtin_trap();
	}
L_002B42:
	{ uint32_t eaD = 0xFFFF8022u; cc_logic16(ctx, ld16(eaD)); } hle_insn(ctx, 16);  /* 002B42 tstw ffff8022 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_002B4E; } hle_insn(ctx, 8);  /* 002B48 bnes 2b4e */
	hle_jump(ctx, 0x002B4A); return;  /* 002B4A jsr %pc@(2b70) */
L_002B4E:
	{ uint32_t eaD = 0xFFFF8072u; cc_logic16(ctx, ld16(eaD)); } hle_insn(ctx, 16);  /* 002B4E tstw ffff8072 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_002B5A; } hle_insn(ctx, 8);  /* 002B54 bnes 2b5a */
	hle_jump(ctx, 0x002B56); return;  /* 002B56 jsr %pc@(2b8e) */
L_002B5A:
	hle_jump(ctx, 0x002B5A); return;  /* 002B5A rts */
}

HLE_RANGE_PURE(0x002B42, Auto_002B42, 200, 0x002B4A, 0x002B56, 0x002B5A) { pc_run_002B42(ctx, 0x002B42); }
HLE_RANGE_PURE(0x002B4E, Auto_002B4E, 200, 0x002B56, 0x002B5A) { pc_run_002B42(ctx, 0x002B4E); }
static void pc_run_002B5C(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x002B5C: goto L_002B5C;
	default: __builtin_trap();
	}
L_002B5C:
	{ alu(ctx, OP_SUB, ctx->a[4], 0xFFFF8000u, 32, 0); } hle_insn(ctx, 14);  /* 002B5C cmpal #-32768,%a4 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_002B68; } hle_insn(ctx, 8);  /* 002B62 bnes 2b68 */
	hle_jump(ctx, 0x002B64); return;  /* 002B64 jmp %pc@(2b70) */
L_002B68:
	hle_jump(ctx, 0x002B68); return;  /* 002B68 jmp %pc@(2b8e) */
}

HLE_RANGE_PURE(0x002B5C, Auto_002B5C, 200, 0x002B64, 0x002B68) { pc_run_002B5C(ctx, 0x002B5C); }
static void pc_run_002B70(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x002B70: goto L_002B70;
	case 0x002B80: goto L_002B80;
	default: __builtin_trap();
	}
L_002B70:
	{ uint32_t v = 0xC000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 002B70 movew #-16384,%d6 */
	{ uint32_t eaS = 0x00002C16u; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 002B74 lea %pc@(2c16),%a0 */
	{ uint32_t v = 0xCD00u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 002B78 movew #-13056,%d3 */
	hle_jump(ctx, 0x002B7C); return;  /* 002B7C jsr %pc@(7eb2) */
L_002B80:
	{ uint32_t eaS = 0x00002C40u; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 002B80 lea %pc@(2c40),%a0 */
	{ uint32_t v = 0xCD80u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 002B84 movew #-12928,%d3 */
	hle_jump(ctx, 0x002B88); return;  /* 002B88 jsr %pc@(7eb2) */
L_002B8C:
	hle_jump(ctx, 0x002B8C); return;  /* 002B8C rts */
}

HLE_RANGE_PURE(0x002B70, Auto_002B70, 200, 0x002B7C) { pc_run_002B70(ctx, 0x002B70); }
HLE_RANGE_PURE(0x002B80, Auto_002B80, 200, 0x002B88) { pc_run_002B70(ctx, 0x002B80); }
static void pc_run_002B8E(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x002B8E: goto L_002B8E;
	case 0x002B9E: goto L_002B9E;
	case 0x002BAA: goto L_002BAA;
	case 0x002BBA: goto L_002BBA;
	default: __builtin_trap();
	}
L_002B8E:
	{ uint32_t v = 0xC000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 002B8E movew #-16384,%d6 */
	{ uint32_t eaS = 0x00002C2Bu; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 002B92 lea %pc@(2c2b),%a0 */
	{ uint32_t v = 0xCD28u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 002B96 movew #-13016,%d3 */
	hle_jump(ctx, 0x002B9A); return;  /* 002B9A jsr %pc@(7eb2) */
L_002B9E:
	{ uint32_t eaS = 0x00002C55u; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 002B9E lea %pc@(2c55),%a0 */
	{ uint32_t v = 0xCDA8u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 002BA2 movew #-12888,%d3 */
	hle_jump(ctx, 0x002BA6); return;  /* 002BA6 jsr %pc@(7eb2) */
L_002BAA:
	{ uint32_t eaS = 0x00002C6Au; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 002BAA lea %pc@(2c6a),%a0 */
	{ uint32_t v = 0xCD28u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 002BAE movew #-13016,%d3 */
	{ uint32_t v = 0xC800u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 002BB2 movew #-14336,%d6 */
	hle_jump(ctx, 0x002BB6); return;  /* 002BB6 jsr %pc@(7eb2) */
L_002BBA:
	{ uint32_t eaS = 0x00002C6Cu; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 002BBA lea %pc@(2c6c),%a0 */
	{ uint32_t v = 0xCDA8u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 002BBE movew #-12888,%d3 */
	hle_jump(ctx, 0x002BC2); return;  /* 002BC2 jsr %pc@(7eb2) */
L_002BC6:
	hle_jump(ctx, 0x002BC6); return;  /* 002BC6 rts */
}

HLE_RANGE_PURE(0x002B8E, Auto_002B8E, 200, 0x002B9A) { pc_run_002B8E(ctx, 0x002B8E); }
HLE_RANGE_PURE(0x002B9E, Auto_002B9E, 200, 0x002BA6) { pc_run_002B8E(ctx, 0x002B9E); }
HLE_RANGE_PURE(0x002BAA, Auto_002BAA, 200, 0x002BB6) { pc_run_002B8E(ctx, 0x002BAA); }
HLE_RANGE_PURE(0x002BBA, Auto_002BBA, 200, 0x002BC2) { pc_run_002B8E(ctx, 0x002BBA); }
static void pc_run_0032AC(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x0032AC: goto L_0032AC;
	case 0x0032B4: goto L_0032B4;
	case 0x0032CA: goto L_0032CA;
	case 0x0032DA: goto L_0032DA;
	case 0x0032EA: goto L_0032EA;
	case 0x003300: goto L_003300;
	case 0x003316: goto L_003316;
	case 0x003332: goto L_003332;
	default: __builtin_trap();
	}
L_0032AC:
	{ uint32_t v = 0x0u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0032AC movew #0,%d0 */
	hle_jump(ctx, 0x0032B0); return;  /* 0032B0 jsr %pc@(56fa) */
L_0032B4:
	{ uint32_t eaD = 0xFFFFA0A5u; st8(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 20);  /* 0032B4 clrb ffffa0a5 */
	{ uint32_t eaD = 0xFFFF9EA4u; st16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 20);  /* 0032BA clrw ffff9ea4 */
	{ uint32_t v = 0x80u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0032C0 movew #128,%d0 */
	hle_jump(ctx, 0x0032C4); return;  /* 0032C4 jsr 13d04 */
L_0032CA:
	{ uint32_t eaD = 0xFFFF80B6u; uint32_t v = 0x0u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 20);  /* 0032CA movew #0,ffff80b6 */
	hle_jump(ctx, 0x0032D2); return;  /* 0032D2 jsr %pc@(818c) */
L_0032D6:
	hle_jump(ctx, 0x0032D6); return;  /* 0032D6 jsr %pc@(800c) */
L_0032DA:
	{ uint32_t eaS = 0x0000576Au; ctx->a[1] = eaS; } hle_insn(ctx, 8);  /* 0032DA lea %pc@(576a),%a1 */
	hle_jump(ctx, 0x0032DE); return;  /* 0032DE jsr 867a */
L_0032E4:
	hle_jump(ctx, 0x0032E4); return;  /* 0032E4 jsr 121d6 */
L_0032EA:
	{ uint32_t eaS = 0x00024816u; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 0032EA lea 24816,%a0 */
	{ uint32_t v = 0x7B6u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0032F0 movew #1974,%d1 */
	{ uint32_t eaD = 0xFFFF9E26u; uint32_t v = (ctx->d[1] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 0032F4 movew %d1,ffff9e26 */
	hle_jump(ctx, 0x0032FA); return;  /* 0032FA jsr 1320a */
L_003300:
	{ uint32_t eaS = 0x00025138u; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 003300 lea 25138,%a0 */
	{ uint32_t v = 0x6C7u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 003306 movew #1735,%d1 */
	{ uint32_t eaD = 0xFFFF9E2Au; uint32_t v = (ctx->d[1] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00330A movew %d1,ffff9e2a */
	hle_jump(ctx, 0x003310); return;  /* 003310 jsr 1320a */
L_003316:
	{ uint32_t eaS = 0x00025DFCu; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 003316 lea 25dfc,%a0 */
	{ uint32_t v = 0x670u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00331C movew #1648,%d1 */
	{ uint32_t eaD = 0xFFFF9E2Cu; uint32_t v = (ctx->d[1] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 003320 movew %d1,ffff9e2c */
	hle_jump(ctx, 0x003326); return;  /* 003326 jsr 1320a */
L_00332C:
	hle_jump(ctx, 0x00332C); return;  /* 00332C jsr 11d12 */
L_003332:
	{ uint32_t eaD = 0xFFFFAA5Cu; uint32_t v = 0x0u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 28);  /* 003332 movel #0,ffffaa5c */
	{ uint32_t eaD = 0xFFFF9E98u; uint32_t v = 0x4A88u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 28);  /* 00333C movel #19080,ffff9e98 */
	hle_jump(ctx, 0x003346); return;  /* 003346 jsr %pc@(4752) */
L_00334A:
	hle_jump(ctx, 0x00334A); return;  /* 00334A jsr 11d28 */
L_003350:
	hle_jump(ctx, 0x003350); return;  /* 003350 jsr 11d32 */
L_003356:
	hle_jump(ctx, 0x003356); return;  /* 003356 jsr %pc@(2b08) */
L_00335A:
	hle_jump(ctx, 0x00335A); return;  /* 00335A jsr %pc@(7c54) */
L_00335E:
	hle_jump(ctx, 0x00335E); return;  /* 00335E rts */
}

HLE_RANGE_PURE(0x0032AC, Auto_0032AC, 200, 0x0032B0) { pc_run_0032AC(ctx, 0x0032AC); }
HLE_RANGE_PURE(0x0032B4, Auto_0032B4, 200, 0x0032C4) { pc_run_0032AC(ctx, 0x0032B4); }
HLE_RANGE_PURE(0x0032CA, Auto_0032CA, 200, 0x0032D2) { pc_run_0032AC(ctx, 0x0032CA); }
HLE_RANGE_PURE(0x0032DA, Auto_0032DA, 200, 0x0032DE) { pc_run_0032AC(ctx, 0x0032DA); }
HLE_RANGE_PURE(0x0032EA, Auto_0032EA, 200, 0x0032FA) { pc_run_0032AC(ctx, 0x0032EA); }
HLE_RANGE_PURE(0x003300, Auto_003300, 200, 0x003310) { pc_run_0032AC(ctx, 0x003300); }
HLE_RANGE_PURE(0x003316, Auto_003316, 200, 0x003326) { pc_run_0032AC(ctx, 0x003316); }
HLE_RANGE_PURE(0x003332, Auto_003332, 200, 0x003346) { pc_run_0032AC(ctx, 0x003332); }
static void pc_run_00368C(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x003690: goto L_003690;
	case 0x00369E: goto L_00369E;
	case 0x0036B0: goto L_0036B0;
	case 0x0036CE: goto L_0036CE;
	default: __builtin_trap();
	}
L_00368C:
	hle_jump(ctx, 0x00368C); return;  /* 00368C bsrw 3b4a */
L_003690:
	{ uint32_t eaS = 0xFFFF8000u; ctx->a[4] = eaS; } hle_insn(ctx, 12);  /* 003690 lea ffff8000,%a4 */
	{ uint32_t v = 0xE586u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 003696 movew #-6778,%d3 */
	hle_jump(ctx, 0x00369A); return;  /* 00369A bsrw 3b1a */
L_00369E:
	{ uint32_t eaS = 0xFFFF8050u; ctx->a[4] = eaS; } hle_insn(ctx, 12);  /* 00369E lea ffff8050,%a4 */
	{ uint32_t v = 0xE5AEu; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0036A4 movew #-6738,%d3 */
	hle_jump(ctx, 0x0036A8); return;  /* 0036A8 bsrw 3b1a */
L_0036AC:
	hle_jump(ctx, 0x0036AC); return;  /* 0036AC bsrw 3b4a */
L_0036B0:
	{ uint32_t eaD = 0xFFFFA0A8u; cc_logic16(ctx, ld16(eaD)); } hle_insn(ctx, 16);  /* 0036B0 tstw ffffa0a8 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0036C2; } hle_insn(ctx, 8);  /* 0036B6 bnes 36c2 */
	{ uint32_t v = 0x2710u; ctx->d[5] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 0036B8 movel #10000,%d5 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00388C); return;  /* 0036BE braw 388c */
L_0036C2:
	{ uint32_t v = 0x64u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0036C2 movew #100,%d1 */
	hle_jump(ctx, 0x0036C6); return;  /* 0036C6 jsr %pc@(86d8) */
L_0036CA:
	hle_jump(ctx, 0x0036CA); return;  /* 0036CA jsr %pc@(3c06) */
L_0036CE:
	{ uint32_t v = 0x14u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0036CE movew #20,%d1 */
	hle_jump(ctx, 0x0036D2); return;  /* 0036D2 jsr %pc@(86d8) */
L_0036D6:
	hle_jump(ctx, 0x0036D6); return;  /* 0036D6 rts */
}

HLE_RANGE_PURE(0x003690, Auto_003690, 200, 0x00369A) { pc_run_00368C(ctx, 0x003690); }
HLE_RANGE_PURE(0x00369E, Auto_00369E, 200, 0x0036A8) { pc_run_00368C(ctx, 0x00369E); }
HLE_RANGE_PURE(0x0036B0, Auto_0036B0, 200, 0x0036C6, 0x00388C) { pc_run_00368C(ctx, 0x0036B0); }
HLE_RANGE_PURE(0x0036CE, Auto_0036CE, 200, 0x0036D2) { pc_run_00368C(ctx, 0x0036CE); }
static void pc_run_0036D8(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x0036DC: goto L_0036DC;
	case 0x0036EA: goto L_0036EA;
	case 0x0036FC: goto L_0036FC;
	default: __builtin_trap();
	}
L_0036D8:
	hle_jump(ctx, 0x0036D8); return;  /* 0036D8 bsrw 3b4a */
L_0036DC:
	{ uint32_t eaS = 0xFFFF8000u; ctx->a[4] = eaS; } hle_insn(ctx, 12);  /* 0036DC lea ffff8000,%a4 */
	{ uint32_t v = 0xE586u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0036E2 movew #-6778,%d3 */
	hle_jump(ctx, 0x0036E6); return;  /* 0036E6 bsrw 3ae4 */
L_0036EA:
	{ uint32_t eaS = 0xFFFF8050u; ctx->a[4] = eaS; } hle_insn(ctx, 12);  /* 0036EA lea ffff8050,%a4 */
	{ uint32_t v = 0xE5AEu; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0036F0 movew #-6738,%d3 */
	hle_jump(ctx, 0x0036F4); return;  /* 0036F4 bsrw 3ae4 */
L_0036F8:
	hle_jump(ctx, 0x0036F8); return;  /* 0036F8 bsrw 3b4a */
L_0036FC:
	{ uint32_t v = 0x1388u; ctx->d[5] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 0036FC movel #5000,%d5 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00388C); return;  /* 003702 braw 388c */
}

HLE_RANGE_PURE(0x0036DC, Auto_0036DC, 200, 0x0036E6) { pc_run_0036D8(ctx, 0x0036DC); }
HLE_RANGE_PURE(0x0036EA, Auto_0036EA, 200, 0x0036F4) { pc_run_0036D8(ctx, 0x0036EA); }
HLE_RANGE_PURE(0x0036FC, Auto_0036FC, 200, 0x00388C) { pc_run_0036D8(ctx, 0x0036FC); }
static void pc_run_003706(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00370A: goto L_00370A;
	case 0x003718: goto L_003718;
	case 0x00372A: goto L_00372A;
	default: __builtin_trap();
	}
L_003706:
	hle_jump(ctx, 0x003706); return;  /* 003706 bsrw 3b4a */
L_00370A:
	{ uint32_t eaS = 0xFFFF8000u; ctx->a[4] = eaS; } hle_insn(ctx, 12);  /* 00370A lea ffff8000,%a4 */
	{ uint32_t v = 0xE586u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 003710 movew #-6778,%d3 */
	hle_jump(ctx, 0x003714); return;  /* 003714 bsrw 3a34 */
L_003718:
	{ uint32_t eaS = 0xFFFF8050u; ctx->a[4] = eaS; } hle_insn(ctx, 12);  /* 003718 lea ffff8050,%a4 */
	{ uint32_t v = 0xE5AEu; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00371E movew #-6738,%d3 */
	hle_jump(ctx, 0x003722); return;  /* 003722 bsrw 3a34 */
L_003726:
	hle_jump(ctx, 0x003726); return;  /* 003726 bsrw 3b4a */
L_00372A:
	{ uint32_t v = 0x1388u; ctx->d[5] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 00372A movel #5000,%d5 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00388C); return;  /* 003730 braw 388c */
}

HLE_RANGE_PURE(0x00370A, Auto_00370A, 200, 0x003714) { pc_run_003706(ctx, 0x00370A); }
HLE_RANGE_PURE(0x003718, Auto_003718, 200, 0x003722) { pc_run_003706(ctx, 0x003718); }
HLE_RANGE_PURE(0x00372A, Auto_00372A, 200, 0x00388C) { pc_run_003706(ctx, 0x00372A); }
static void pc_run_003734(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x003738: goto L_003738;
	case 0x003746: goto L_003746;
	case 0x003758: goto L_003758;
	default: __builtin_trap();
	}
L_003734:
	hle_jump(ctx, 0x003734); return;  /* 003734 bsrw 3b4a */
L_003738:
	{ uint32_t eaS = 0xFFFF8000u; ctx->a[4] = eaS; } hle_insn(ctx, 12);  /* 003738 lea ffff8000,%a4 */
	{ uint32_t v = 0xE586u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00373E movew #-6778,%d3 */
	hle_jump(ctx, 0x003742); return;  /* 003742 bsrw 3a7e */
L_003746:
	{ uint32_t eaS = 0xFFFF8050u; ctx->a[4] = eaS; } hle_insn(ctx, 12);  /* 003746 lea ffff8050,%a4 */
	{ uint32_t v = 0xE5AEu; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00374C movew #-6738,%d3 */
	hle_jump(ctx, 0x003750); return;  /* 003750 bsrw 3a7e */
L_003754:
	hle_jump(ctx, 0x003754); return;  /* 003754 bsrw 3b4a */
L_003758:
	{ uint32_t v = 0x1388u; ctx->d[5] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 003758 movel #5000,%d5 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00388C); return;  /* 00375E braw 388c */
}

HLE_RANGE_PURE(0x003738, Auto_003738, 200, 0x003742) { pc_run_003734(ctx, 0x003738); }
HLE_RANGE_PURE(0x003746, Auto_003746, 200, 0x003750) { pc_run_003734(ctx, 0x003746); }
HLE_RANGE_PURE(0x003758, Auto_003758, 200, 0x00388C) { pc_run_003734(ctx, 0x003758); }
static void pc_run_003762(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x003766: goto L_003766;
	case 0x003774: goto L_003774;
	case 0x003786: goto L_003786;
	case 0x00378E: goto L_00378E;
	case 0x003796: goto L_003796;
	case 0x0037A6: goto L_0037A6;
	case 0x0037B6: goto L_0037B6;
	case 0x0037CA: goto L_0037CA;
	default: __builtin_trap();
	}
L_003762:
	hle_jump(ctx, 0x003762); return;  /* 003762 bsrw 3b4a */
L_003766:
	{ uint32_t eaS = 0xFFFF8000u; ctx->a[4] = eaS; } hle_insn(ctx, 12);  /* 003766 lea ffff8000,%a4 */
	{ uint32_t v = 0xE586u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00376C movew #-6778,%d3 */
	hle_jump(ctx, 0x003770); return;  /* 003770 bsrw 39c6 */
L_003774:
	{ uint32_t eaS = 0xFFFF8050u; ctx->a[4] = eaS; } hle_insn(ctx, 12);  /* 003774 lea ffff8050,%a4 */
	{ uint32_t v = 0xE5AEu; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00377A movew #-6738,%d3 */
	hle_jump(ctx, 0x00377E); return;  /* 00377E bsrw 39c6 */
L_003782:
	hle_jump(ctx, 0x003782); return;  /* 003782 bsrw 3b4a */
L_003786:
	{ uint32_t v = 0x2u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 003786 movew #2,%d1 */
	hle_jump(ctx, 0x00378A); return;  /* 00378A jsr %pc@(86d8) */
L_00378E:
	{ ctx->d[0] = 0x00000078u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 00378E moveq #120,%d0 */
	hle_jump(ctx, 0x003790); return;  /* 003790 jsr 13d04 */
L_003796:
	{ ctx->d[7] = 0x00000000u; cc_logic32(ctx, ctx->d[7]); } hle_insn(ctx, 4);  /* 003796 moveq #0,%d7 */
	{ uint32_t eaS = 0xFFFF8000u; ctx->a[4] = eaS; } hle_insn(ctx, 12);  /* 003798 lea ffff8000,%a4 */
	{ uint32_t v = 0xEC0Cu; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00379E movew #-5108,%d3 */
	hle_jump(ctx, 0x0037A2); return;  /* 0037A2 bsrw 37d4 */
L_0037A6:
	{ ctx->d[7] = (ctx->d[7] << 16) | (ctx->d[7] >> 16); cc_logic32(ctx, ctx->d[7]); } hle_insn(ctx, 4);  /* 0037A6 swap %d7 */
	{ uint32_t eaS = 0xFFFF8050u; ctx->a[4] = eaS; } hle_insn(ctx, 12);  /* 0037A8 lea ffff8050,%a4 */
	{ uint32_t v = 0xEC34u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0037AE movew #-5068,%d3 */
	hle_jump(ctx, 0x0037B2); return;  /* 0037B2 bsrw 37d4 */
L_0037B6:
	{ alu(ctx, OP_SUB, (ctx->d[7] & 0xFFFFFFFFu), 0xFF00FFu, 32, 0); } hle_insn(ctx, 14);  /* 0037B6 cmpil #16711935,%d7 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_003786; } hle_insn(ctx, 8);  /* 0037BC bnes 3786 */
	{ uint32_t v = 0x64u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0037BE movew #100,%d1 */
	hle_jump(ctx, 0x0037C2); return;  /* 0037C2 jsr %pc@(86d8) */
L_0037C6:
	hle_jump(ctx, 0x0037C6); return;  /* 0037C6 jsr %pc@(3c06) */
L_0037CA:
	{ uint32_t v = 0x14u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0037CA movew #20,%d1 */
	hle_jump(ctx, 0x0037CE); return;  /* 0037CE jsr %pc@(86d8) */
L_0037D2:
	hle_jump(ctx, 0x0037D2); return;  /* 0037D2 rts */
}

HLE_RANGE_PURE(0x003766, Auto_003766, 200, 0x003770) { pc_run_003762(ctx, 0x003766); }
HLE_RANGE_PURE(0x003774, Auto_003774, 200, 0x00377E) { pc_run_003762(ctx, 0x003774); }
HLE_RANGE_PURE(0x003786, Auto_003786, 200, 0x00378A) { pc_run_003762(ctx, 0x003786); }
HLE_RANGE_PURE(0x00378E, Auto_00378E, 200, 0x003790) { pc_run_003762(ctx, 0x00378E); }
HLE_RANGE_PURE(0x003796, Auto_003796, 200, 0x0037A2) { pc_run_003762(ctx, 0x003796); }
HLE_RANGE_PURE(0x0037A6, Auto_0037A6, 200, 0x0037B2) { pc_run_003762(ctx, 0x0037A6); }
HLE_RANGE_PURE(0x0037B6, Auto_0037B6, 200, 0x00378A, 0x0037C2) { pc_run_003762(ctx, 0x0037B6); }
HLE_RANGE_PURE(0x0037CA, Auto_0037CA, 200, 0x0037CE) { pc_run_003762(ctx, 0x0037CA); }
static void pc_run_0037D4(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x0037D4: goto L_0037D4;
	case 0x0037FE: goto L_0037FE;
	default: __builtin_trap();
	}
L_0037D4:
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(36); cc_logic16(ctx, ld16(eaD)); } hle_insn(ctx, 12);  /* 0037D4 tstw %a4@(36) */
	if (COND_MI) { hle_insn(ctx, 10); goto L_003810; } hle_insn(ctx, 8);  /* 0037D8 bmis 3810 */
	{ ctx->a[7] -= 2; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->d[3] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0037DA movew %d3,%sp@- */
	{ uint32_t eaS = ctx->a[4] + (uint32_t)(int32_t)(70); uint32_t v = ld32(eaS); ctx->d[4] = v; cc_logic32(ctx, v); } hle_insn(ctx, 16);  /* 0037DC movel %a4@(70),%d4 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[4] & 0xFFFFFFFFu), 0xC350u, 32, 1); ctx->d[4] = v; } hle_insn(ctx, 16);  /* 0037E0 subil #50000,%d4 */
	{ uint32_t eaS = ctx->a[4] + (uint32_t)(int32_t)(74); alu(ctx, OP_SUB, (ctx->d[4] & 0xFFFFFFFFu), ld32(eaS), 32, 0); } hle_insn(ctx, 18);  /* 0037E6 cmpl %a4@(74),%d4 */
	if (COND_GT) { hle_insn(ctx, 10); goto L_0037F2; } hle_insn(ctx, 8);  /* 0037EA bgts 37f2 */
	{ uint32_t eaS = ctx->a[4] + (uint32_t)(int32_t)(74); uint32_t v = ld32(eaS); ctx->d[4] = v; cc_logic32(ctx, v); } hle_insn(ctx, 16);  /* 0037EC movel %a4@(74),%d4 */
	set_b(&ctx->d[7], 0xFF); hle_insn(ctx, 6);  /* 0037F0 st %d7 */
L_0037F2:
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(70); uint32_t v = (ctx->d[4] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 16);  /* 0037F2 movel %d4,%a4@(70) */
	{ ctx->d[3] = 0x00000007u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 0037F6 moveq #7,%d3 */
	hle_jump(ctx, 0x0037F8); return;  /* 0037F8 jsr 7a92 */
L_0037FE:
	{ uint32_t eaS = 0xFFFFA0BAu; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 0037FE lea ffffa0ba,%a0 */
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 003804 movew %sp@+,%d3 */
	{ uint32_t v = 0xC000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 003806 movew #-16384,%d6 */
	hle_jump(ctx, 0x00380A); return;  /* 00380A jsr %pc@(7e08) */
L_00380E:
	hle_jump(ctx, 0x00380E); return;  /* 00380E rts */
L_003810:
	set_b(&ctx->d[7], 0xFF); hle_insn(ctx, 6);  /* 003810 st %d7 */
	hle_jump(ctx, 0x003812); return;  /* 003812 rts */
}

HLE_RANGE_PURE(0x0037D4, Auto_0037D4, 200, HLE_EXIT_SP(0x0037F8, -2), 0x003812) { pc_run_0037D4(ctx, 0x0037D4); }
HLE_RANGE_PURE(0x0037FE, Auto_0037FE, 200, HLE_EXIT_SP(0x00380A, 2)) { pc_run_0037D4(ctx, 0x0037FE); }
static void pc_run_003814(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x003814: goto L_003814;
	default: __builtin_trap();
	}
L_003814:
	{ uint32_t eaS = ctx->a[4] + (uint32_t)(int32_t)(70); uint32_t v = ld32(eaS); ctx->d[4] = v; cc_logic32(ctx, v); } hle_insn(ctx, 16); hle_jump(ctx, 0x003818); return;  /* 003814 movel %a4@(70),%d4 */
}

HLE_RANGE_PURE(0x003814, Auto_003814, 200, 0x003818) { pc_run_003814(ctx, 0x003814); }
static void pc_run_003818(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x003818: goto L_003818;
	case 0x003820: goto L_003820;
	default: __builtin_trap();
	}
L_003818:
	{ ctx->d[3] = 0x00000007u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 003818 moveq #7,%d3 */
	hle_jump(ctx, 0x00381A); return;  /* 00381A jsr 7a92 */
L_003820:
	{ uint32_t eaS = 0xFFFFA0BAu; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 003820 lea ffffa0ba,%a0 */
	{ ctx->d[1] = 0x00000000u; cc_logic32(ctx, ctx->d[1]); } hle_insn(ctx, 4);  /* 003826 moveq #0,%d1 */
	{ ctx->d[3] = 0x00000007u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 003828 moveq #7,%d3 */
L_00382A:
	{ uint32_t v = shf(ctx, 'l', (ctx->d[1] & 0xFFFFFFFFu), 4, 32); ctx->d[1] = v; } hle_insn(ctx, 16);  /* 00382A lsll #4,%d1 */
	{ uint32_t eaS = ctx->a[0]; ctx->a[0] += 1; uint32_t v = ld8(eaS); set_b(&ctx->d[0], v); cc_logic8(ctx, v); } hle_insn(ctx, 8);  /* 00382C moveb %a0@+,%d0 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[0] & 0xFFu), 0x30u, 8, 1); set_b(&ctx->d[0], v); } hle_insn(ctx, 8);  /* 00382E subib #48,%d0 */
	{ uint32_t v = ((ctx->d[1] & 0xFFu) | (ctx->d[0] & 0xFFu)) & 0xFFu; set_b(&ctx->d[1], v); cc_logic8(ctx, v); } hle_insn(ctx, 4);  /* 003832 orb %d0,%d1 */
	{ uint16_t v = (uint16_t)ctx->d[3] - 1; set_w(&ctx->d[3], v); if (v != 0xFFFF) { hle_insn(ctx, 10); goto L_00382A; } hle_insn(ctx, 14); }  /* 003834 dbf %d3,382a */
	hle_jump(ctx, 0x003838); return;  /* 003838 rts */
}

HLE_RANGE_PURE(0x003818, Auto_003818, 200, 0x00381A) { pc_run_003818(ctx, 0x003818); }
HLE_RANGE_PURE(0x003820, Auto_003820, 200, 0x003838) { pc_run_003818(ctx, 0x003820); }
static void pc_run_00383A(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00383A: goto L_00383A;
	default: __builtin_trap();
	}
L_00383A:
	{ uint32_t eaS = 0x00003D2Cu; ctx->a[3] = eaS; } hle_insn(ctx, 8);  /* 00383A lea %pc@(3d2c),%a3 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x003862); return;  /* 00383E bras 3862 */
}

HLE_RANGE_PURE(0x00383A, Auto_00383A, 200, 0x003862) { pc_run_00383A(ctx, 0x00383A); }
static void pc_run_003840(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x003840: goto L_003840;
	default: __builtin_trap();
	}
L_003840:
	{ uint32_t eaS = 0x00003D0Eu; ctx->a[3] = eaS; } hle_insn(ctx, 8);  /* 003840 lea %pc@(3d0e),%a3 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x003862); return;  /* 003844 bras 3862 */
}

HLE_RANGE_PURE(0x003840, Auto_003840, 200, 0x003862) { pc_run_003840(ctx, 0x003840); }
static void pc_run_003846(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x003846: goto L_003846;
	default: __builtin_trap();
	}
L_003846:
	{ uint32_t eaS = 0x00003CD0u; ctx->a[3] = eaS; } hle_insn(ctx, 8);  /* 003846 lea %pc@(3cd0),%a3 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x003862); return;  /* 00384A bras 3862 */
}

HLE_RANGE_PURE(0x003846, Auto_003846, 200, 0x003862) { pc_run_003846(ctx, 0x003846); }
static void pc_run_00384C(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00384C: goto L_00384C;
	default: __builtin_trap();
	}
L_00384C:
	{ uint32_t eaS = 0x00003CB2u; ctx->a[3] = eaS; } hle_insn(ctx, 8);  /* 00384C lea %pc@(3cb2),%a3 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x003862); return;  /* 003850 bras 3862 */
}

HLE_RANGE_PURE(0x00384C, Auto_00384C, 200, 0x003862) { pc_run_00384C(ctx, 0x00384C); }
static void pc_run_003852(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x003852: goto L_003852;
	default: __builtin_trap();
	}
L_003852:
	{ uint32_t eaS = 0x00003CEEu; ctx->a[3] = eaS; } hle_insn(ctx, 8);  /* 003852 lea %pc@(3cee),%a3 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x003862); return;  /* 003856 bras 3862 */
}

HLE_RANGE_PURE(0x003852, Auto_003852, 200, 0x003862) { pc_run_003852(ctx, 0x003852); }
static void pc_run_003858(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x003858: goto L_003858;
	default: __builtin_trap();
	}
L_003858:
	{ uint32_t eaS = 0x00003C94u; ctx->a[3] = eaS; } hle_insn(ctx, 8);  /* 003858 lea %pc@(3c94),%a3 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x003862); return;  /* 00385C bras 3862 */
}

HLE_RANGE_PURE(0x003858, Auto_003858, 200, 0x003862) { pc_run_003858(ctx, 0x003858); }
static void pc_run_00385E(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00385E: goto L_00385E;
	default: __builtin_trap();
	}
L_00385E:
	{ uint32_t eaS = 0x00003C76u; ctx->a[3] = eaS; } hle_insn(ctx, 8); hle_jump(ctx, 0x003862); return;  /* 00385E lea %pc@(3c76),%a3 */
}

HLE_RANGE_PURE(0x00385E, Auto_00385E, 200, 0x003862) { pc_run_00385E(ctx, 0x00385E); }
static void pc_run_003862(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x003866: goto L_003866;
	case 0x003874: goto L_003874;
	case 0x003886: goto L_003886;
	default: __builtin_trap();
	}
L_003862:
	hle_jump(ctx, 0x003862); return;  /* 003862 bsrw 3b4a */
L_003866:
	{ uint32_t eaS = 0xFFFF8000u; ctx->a[4] = eaS; } hle_insn(ctx, 12);  /* 003866 lea ffff8000,%a4 */
	{ uint32_t v = 0xE586u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00386C movew #-6778,%d3 */
	hle_jump(ctx, 0x003870); return;  /* 003870 bsrw 391a */
L_003874:
	{ uint32_t eaS = 0xFFFF8050u; ctx->a[4] = eaS; } hle_insn(ctx, 12);  /* 003874 lea ffff8050,%a4 */
	{ uint32_t v = 0xE5AEu; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00387A movew #-6738,%d3 */
	hle_jump(ctx, 0x00387E); return;  /* 00387E bsrw 391a */
L_003882:
	hle_jump(ctx, 0x003882); return;  /* 003882 bsrw 3b4a */
L_003886:
	{ uint32_t v = 0x1388u; ctx->d[5] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12); hle_jump(ctx, 0x00388C); return;  /* 003886 movel #5000,%d5 */
}

HLE_RANGE_PURE(0x003866, Auto_003866, 200, 0x003870) { pc_run_003862(ctx, 0x003866); }
HLE_RANGE_PURE(0x003874, Auto_003874, 200, 0x00387E) { pc_run_003862(ctx, 0x003874); }
HLE_RANGE_PURE(0x003886, Auto_003886, 200, 0x00388C) { pc_run_003862(ctx, 0x003886); }
static void pc_run_00388C(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00388C: goto L_00388C;
	case 0x003894: goto L_003894;
	case 0x00389C: goto L_00389C;
	case 0x0038AC: goto L_0038AC;
	case 0x0038BC: goto L_0038BC;
	case 0x0038D0: goto L_0038D0;
	default: __builtin_trap();
	}
L_00388C:
	{ uint32_t v = 0x2u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00388C movew #2,%d1 */
	hle_jump(ctx, 0x003890); return;  /* 003890 jsr %pc@(86d8) */
L_003894:
	{ ctx->d[0] = 0x00000078u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 003894 moveq #120,%d0 */
	hle_jump(ctx, 0x003896); return;  /* 003896 jsr 13d04 */
L_00389C:
	{ ctx->d[7] = 0x00000000u; cc_logic32(ctx, ctx->d[7]); } hle_insn(ctx, 4);  /* 00389C moveq #0,%d7 */
	{ uint32_t eaS = 0xFFFF8000u; ctx->a[4] = eaS; } hle_insn(ctx, 12);  /* 00389E lea ffff8000,%a4 */
	{ uint32_t v = 0xEC0Cu; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0038A4 movew #-5108,%d3 */
	hle_jump(ctx, 0x0038A8); return;  /* 0038A8 bsrw 38da */
L_0038AC:
	{ ctx->d[7] = (ctx->d[7] << 16) | (ctx->d[7] >> 16); cc_logic32(ctx, ctx->d[7]); } hle_insn(ctx, 4);  /* 0038AC swap %d7 */
	{ uint32_t eaS = 0xFFFF8050u; ctx->a[4] = eaS; } hle_insn(ctx, 12);  /* 0038AE lea ffff8050,%a4 */
	{ uint32_t v = 0xEC34u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0038B4 movew #-5068,%d3 */
	hle_jump(ctx, 0x0038B8); return;  /* 0038B8 bsrw 38da */
L_0038BC:
	{ alu(ctx, OP_SUB, (ctx->d[7] & 0xFFFFFFFFu), 0xFF00FFu, 32, 0); } hle_insn(ctx, 14);  /* 0038BC cmpil #16711935,%d7 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_00388C; } hle_insn(ctx, 8);  /* 0038C2 bnes 388c */
	{ uint32_t v = 0x64u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0038C4 movew #100,%d1 */
	hle_jump(ctx, 0x0038C8); return;  /* 0038C8 jsr %pc@(86d8) */
L_0038CC:
	hle_jump(ctx, 0x0038CC); return;  /* 0038CC jsr %pc@(3c06) */
L_0038D0:
	{ uint32_t v = 0x14u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0038D0 movew #20,%d1 */
	hle_jump(ctx, 0x0038D4); return;  /* 0038D4 jsr %pc@(86d8) */
L_0038D8:
	hle_jump(ctx, 0x0038D8); return;  /* 0038D8 rts */
}

HLE_RANGE_PURE(0x00388C, Auto_00388C, 200, 0x003890) { pc_run_00388C(ctx, 0x00388C); }
HLE_RANGE_PURE(0x003894, Auto_003894, 200, 0x003896) { pc_run_00388C(ctx, 0x003894); }
HLE_RANGE_PURE(0x00389C, Auto_00389C, 200, 0x0038A8) { pc_run_00388C(ctx, 0x00389C); }
HLE_RANGE_PURE(0x0038AC, Auto_0038AC, 200, 0x0038B8) { pc_run_00388C(ctx, 0x0038AC); }
HLE_RANGE_PURE(0x0038BC, Auto_0038BC, 200, 0x003890, 0x0038C8) { pc_run_00388C(ctx, 0x0038BC); }
HLE_RANGE_PURE(0x0038D0, Auto_0038D0, 200, 0x0038D4) { pc_run_00388C(ctx, 0x0038D0); }
static void pc_run_0038DA(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x0038DA: goto L_0038DA;
	case 0x003902: goto L_003902;
	case 0x003912: goto L_003912;
	default: __builtin_trap();
	}
L_0038DA:
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(36); cc_logic16(ctx, ld16(eaD)); } hle_insn(ctx, 12);  /* 0038DA tstw %a4@(36) */
	if (COND_MI) { hle_insn(ctx, 10); goto L_003916; } hle_insn(ctx, 8);  /* 0038DE bmis 3916 */
	{ ctx->a[7] -= 4; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->d[5] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 0038E0 movel %d5,%sp@- */
	{ ctx->a[7] -= 2; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->d[3] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0038E2 movew %d3,%sp@- */
	{ uint32_t eaS = ctx->a[4] + (uint32_t)(int32_t)(70); uint32_t v = ld32(eaS); ctx->d[4] = v; cc_logic32(ctx, v); } hle_insn(ctx, 16);  /* 0038E4 movel %a4@(70),%d4 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[4] & 0xFFFFFFFFu), (ctx->d[5] & 0xFFFFFFFFu), 32, 1); ctx->d[4] = v; } hle_insn(ctx, 8);  /* 0038E8 addl %d5,%d4 */
	{ uint32_t eaS = ctx->a[4] + (uint32_t)(int32_t)(74); alu(ctx, OP_SUB, (ctx->d[4] & 0xFFFFFFFFu), ld32(eaS), 32, 0); } hle_insn(ctx, 18);  /* 0038EA cmpl %a4@(74),%d4 */
	if (COND_LT) { hle_insn(ctx, 10); goto L_0038F6; } hle_insn(ctx, 8);  /* 0038EE blts 38f6 */
	{ uint32_t eaS = ctx->a[4] + (uint32_t)(int32_t)(74); uint32_t v = ld32(eaS); ctx->d[4] = v; cc_logic32(ctx, v); } hle_insn(ctx, 16);  /* 0038F0 movel %a4@(74),%d4 */
	set_b(&ctx->d[7], 0xFF); hle_insn(ctx, 6);  /* 0038F4 st %d7 */
L_0038F6:
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(70); uint32_t v = (ctx->d[4] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 16);  /* 0038F6 movel %d4,%a4@(70) */
	{ ctx->d[3] = 0x00000007u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 0038FA moveq #7,%d3 */
	hle_jump(ctx, 0x0038FC); return;  /* 0038FC jsr 7a92 */
L_003902:
	{ uint32_t eaS = 0xFFFFA0BAu; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 003902 lea ffffa0ba,%a0 */
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 003908 movew %sp@+,%d3 */
	{ uint32_t v = 0xC000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00390A movew #-16384,%d6 */
	hle_jump(ctx, 0x00390E); return;  /* 00390E jsr %pc@(7e08) */
L_003912:
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 4; uint32_t v = ld32(eaS); ctx->d[5] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 003912 movel %sp@+,%d5 */
	hle_jump(ctx, 0x003914); return;  /* 003914 rts */
L_003916:
	set_b(&ctx->d[7], 0xFF); hle_insn(ctx, 6);  /* 003916 st %d7 */
	hle_jump(ctx, 0x003918); return;  /* 003918 rts */
}

HLE_RANGE_PURE(0x0038DA, Auto_0038DA, 200, HLE_EXIT_SP(0x0038FC, -6), 0x003918) { pc_run_0038DA(ctx, 0x0038DA); }
HLE_RANGE_PURE(0x003902, Auto_003902, 200, HLE_EXIT_SP(0x00390E, 2)) { pc_run_0038DA(ctx, 0x003902); }
HLE_RANGE_PURE(0x003912, Auto_003912, 200, HLE_EXIT_SP(0x003914, 4)) { pc_run_0038DA(ctx, 0x003912); }
static void pc_run_003AE4(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x003AE4: goto L_003AE4;
	case 0x003B14: goto L_003B14;
	default: __builtin_trap();
	}
L_003AE4:
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(36); cc_logic16(ctx, ld16(eaD)); } hle_insn(ctx, 12);  /* 003AE4 tstw %a4@(36) */
	if (COND_MI) { hle_insn(ctx, 10); goto L_003B18; } hle_insn(ctx, 12);  /* 003AE8 bmiw 3b18 */
	{ uint32_t eaD = 0xFFFFA238u; uint32_t v = (ctx->d[3] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 003AEC movew %d3,ffffa238 */
	{ ctx->d[4] = 0x00000000u; cc_logic32(ctx, ctx->d[4]); } hle_insn(ctx, 4);  /* 003AF2 moveq #0,%d4 */
	{ uint32_t eaS = 0x00003DFFu; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 003AF4 lea %pc@(3dff),%a0 */
	{ uint32_t eaD = 0xFFFF9F68u; alu(ctx, OP_SUB, ld16(eaD), 0xDu, 16, 0); } hle_insn(ctx, 20);  /* 003AF8 cmpiw #13,ffff9f68 */
	if (COND_GT) { hle_insn(ctx, 10); goto L_003B0C; } hle_insn(ctx, 8);  /* 003B00 bgts 3b0c */
	{ uint32_t eaS = 0x00003E0Fu; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 003B02 lea %pc@(3e0f),%a0 */
	{ uint32_t v = 0x7A120u; ctx->d[4] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 003B06 movel #500000,%d4 */
L_003B0C:
	{ uint32_t v = 0xC000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 003B0C movew #-16384,%d6 */
	hle_jump(ctx, 0x003B10); return;  /* 003B10 jsr %pc@(7e02) */
L_003B14:
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(74); uint32_t v = alu(ctx, OP_ADD, ld32(eaD), (ctx->d[4] & 0xFFFFFFFFu), 32, 1); st32(eaD, v); } hle_insn(ctx, 24);  /* 003B14 addl %d4,%a4@(74) */
L_003B18:
	hle_jump(ctx, 0x003B18); return;  /* 003B18 rts */
}

HLE_RANGE_PURE(0x003AE4, Auto_003AE4, 200, 0x003B10, 0x003B18) { pc_run_003AE4(ctx, 0x003AE4); }
HLE_RANGE_PURE(0x003B14, Auto_003B14, 200, 0x003B18) { pc_run_003AE4(ctx, 0x003B14); }
static void pc_run_003B1A(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x003B1A: goto L_003B1A;
	default: __builtin_trap();
	}
L_003B1A:
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(36); cc_logic16(ctx, ld16(eaD)); } hle_insn(ctx, 12);  /* 003B1A tstw %a4@(36) */
	if (COND_MI) { hle_insn(ctx, 10); goto L_003B48; } hle_insn(ctx, 12);  /* 003B1E bmiw 3b48 */
	{ uint32_t eaD = 0xFFFFA238u; uint32_t v = (ctx->d[3] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 003B22 movew %d3,ffffa238 */
	{ uint32_t eaS = 0x00003E62u; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 003B28 lea %pc@(3e62),%a0 */
	{ uint32_t eaD = 0xFFFFA0A8u; cc_logic16(ctx, ld16(eaD)); } hle_insn(ctx, 16);  /* 003B2C tstw ffffa0a8 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_003B40; } hle_insn(ctx, 8);  /* 003B32 bnes 3b40 */
	{ uint32_t eaS = 0x00003E94u; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 003B34 lea %pc@(3e94),%a0 */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(74); uint32_t v = alu(ctx, OP_ADD, ld32(eaD), 0xF4240u, 32, 1); st32(eaD, v); } hle_insn(ctx, 32);  /* 003B38 addil #1000000,%a4@(74) */
L_003B40:
	{ uint32_t v = 0xC000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 003B40 movew #-16384,%d6 */
	hle_jump(ctx, 0x003B44); return;  /* 003B44 jsr %pc@(7e02) */
L_003B48:
	hle_jump(ctx, 0x003B48); return;  /* 003B48 rts */
}

HLE_RANGE_PURE(0x003B1A, Auto_003B1A, 200, 0x003B44, 0x003B48) { pc_run_003B1A(ctx, 0x003B1A); }
static void pc_run_003B4A(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x003B4A: goto L_003B4A;
	case 0x003B54: goto L_003B54;
	case 0x003B7A: goto L_003B7A;
	case 0x003B88: goto L_003B88;
	case 0x003BA0: goto L_003BA0;
	default: __builtin_trap();
	}
L_003B4A:
	{ ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[5]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[4]); } hle_movem_l(ctx, 24, 2);  /* 003B4A moveml %d4-%d5,%sp@- */
	hle_jump(ctx, 0x003B4E); return;  /* 003B4E jsr 11d12 */
L_003B54:
	{ uint32_t eaS = ctx->a[3] + (uint32_t)(int32_t)(4); uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 003B54 movew %a3@(4),%d0 */
	{ uint32_t eaS = 0x0003869Eu; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 003B58 lea 3869e,%a0 */
	{ uint32_t eaS = ctx->a[0] + (uint32_t)(int32_t)(0) + (uint32_t)(int32_t)(int16_t)ctx->d[0]; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 003B5E lea %a0@(0,%d0:w),%a0 */
	{ uint32_t v = 0xE91Eu; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 003B62 movew #-5858,%d1 */
	{ ctx->d[2] = 0x0000000Au; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 003B66 moveq #10,%d2 */
	{ ctx->d[3] = 0x00000006u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 003B68 moveq #6,%d3 */
	{ uint32_t v = 0xE000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 003B6A movew #-8192,%d6 */
	{ uint32_t eaS = 0xFFFF9E38u; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 003B6E movew ffff9e38,%d7 */
	hle_jump(ctx, 0x003B74); return;  /* 003B74 jsr 12234 */
L_003B7A:
	{ uint32_t eaS = ctx->a[3] + (uint32_t)(int32_t)(6); ctx->a[0] = ld32(eaS); } hle_insn(ctx, 16);  /* 003B7A moveal %a3@(6),%a0 */
	{ uint32_t eaS = 0xFFFFA194u; ctx->a[1] = eaS; } hle_insn(ctx, 12);  /* 003B7E lea ffffa194,%a1 */
	hle_jump(ctx, 0x003B84); return;  /* 003B84 jsr %pc@(8a7c) */
L_003B88:
	{ uint32_t eaS = 0xFFFFA134u; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 003B88 lea ffffa134,%a0 */
	hle_jump(ctx, 0x003B8E); return;  /* 003B8E jsr 80fe */
L_003B94:
	hle_jump(ctx, 0x003B94); return;  /* 003B94 jsr 11d28 */
L_003B9A:
	hle_jump(ctx, 0x003B9A); return;  /* 003B9A jsr 11d32 */
L_003BA0:
	{ ctx->d[4] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->d[5] = ld32(ctx->a[7]); ctx->a[7] += 4; } hle_movem_l(ctx, 28, 2);  /* 003BA0 moveml %sp@+,%d4-%d5 */
	hle_jump(ctx, 0x003BA4); return;  /* 003BA4 rts */
}

HLE_RANGE_PURE(0x003B4A, Auto_003B4A, 200, HLE_EXIT_SP(0x003B4E, -8)) { pc_run_003B4A(ctx, 0x003B4A); }
HLE_RANGE_PURE(0x003B54, Auto_003B54, 200, 0x003B74) { pc_run_003B4A(ctx, 0x003B54); }
HLE_RANGE_PURE(0x003B7A, Auto_003B7A, 200, 0x003B84) { pc_run_003B4A(ctx, 0x003B7A); }
HLE_RANGE_PURE(0x003B88, Auto_003B88, 200, 0x003B8E) { pc_run_003B4A(ctx, 0x003B88); }
HLE_RANGE_PURE(0x003BA0, Auto_003BA0, 200, HLE_EXIT_SP(0x003BA4, 8)) { pc_run_003B4A(ctx, 0x003BA0); }
static void pc_run_003BDA(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x003BDA: goto L_003BDA;
	case 0x003BE8: goto L_003BE8;
	default: __builtin_trap();
	}
L_003BDA:
	{ uint32_t eaS = 0xFFFF8000u; ctx->a[4] = eaS; } hle_insn(ctx, 12);  /* 003BDA lea ffff8000,%a4 */
	{ uint32_t v = 0xEA86u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 003BE0 movew #-5498,%d3 */
	hle_jump(ctx, 0x003BE4); return;  /* 003BE4 bsrw 3bf2 */
L_003BE8:
	{ uint32_t eaS = 0xFFFF8050u; ctx->a[4] = eaS; } hle_insn(ctx, 12);  /* 003BE8 lea ffff8050,%a4 */
	{ uint32_t v = 0xEAAEu; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); hle_jump(ctx, 0x003BF2); return;  /* 003BEE movew #-5458,%d3 */
}

HLE_RANGE_PURE(0x003BDA, Auto_003BDA, 200, 0x003BE4) { pc_run_003BDA(ctx, 0x003BDA); }
HLE_RANGE_PURE(0x003BE8, Auto_003BE8, 200, 0x003BF2) { pc_run_003BDA(ctx, 0x003BE8); }
static void pc_run_003BF2(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x003BF2: goto L_003BF2;
	default: __builtin_trap();
	}
L_003BF2:
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(36); cc_logic16(ctx, ld16(eaD)); } hle_insn(ctx, 12);  /* 003BF2 tstw %a4@(36) */
	if (COND_MI) { hle_insn(ctx, 10); goto L_003C04; } hle_insn(ctx, 8);  /* 003BF6 bmis 3c04 */
	{ uint32_t eaS = 0x00003DEBu; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 003BF8 lea %pc@(3deb),%a0 */
	{ uint32_t v = 0xC000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 003BFC movew #-16384,%d6 */
	hle_jump(ctx, 0x003C00); return;  /* 003C00 jsr %pc@(7e02) */
L_003C04:
	hle_jump(ctx, 0x003C04); return;  /* 003C04 rts */
}

HLE_RANGE_PURE(0x003BF2, Auto_003BF2, 200, 0x003C00, 0x003C04) { pc_run_003BF2(ctx, 0x003BF2); }
static void pc_run_003C06(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x003C0C: goto L_003C0C;
	case 0x003C1A: goto L_003C1A;
	default: __builtin_trap();
	}
L_003C06:
	hle_jump(ctx, 0x003C06); return;  /* 003C06 jsr 11d12 */
L_003C0C:
	{ uint32_t v = 0xE000u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 003C0C movew #-8192,%d1 */
	{ ctx->d[2] = 0x00000028u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 003C10 moveq #40,%d2 */
	{ ctx->d[3] = 0x00000014u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 003C12 moveq #20,%d3 */
	hle_jump(ctx, 0x003C14); return;  /* 003C14 jsr 12212 */
L_003C1A:
	{ uint32_t v = 0xE91Eu; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 003C1A movew #-5858,%d1 */
	{ ctx->d[2] = 0x0000000Au; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 003C1E moveq #10,%d2 */
	{ ctx->d[3] = 0x00000006u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 003C20 moveq #6,%d3 */
	hle_jump(ctx, 0x003C22); return;  /* 003C22 jsr 12212 */
L_003C28:
	hle_jump(ctx, 0x003C28); return;  /* 003C28 jsr 11d28 */
L_003C2E:
	hle_jump(ctx, 0x003C2E); return;  /* 003C2E jsr 11d32 */
L_003C34:
	hle_jump(ctx, 0x003C34); return;  /* 003C34 rts */
}

HLE_RANGE_PURE(0x003C0C, Auto_003C0C, 200, 0x003C14) { pc_run_003C06(ctx, 0x003C0C); }
HLE_RANGE_PURE(0x003C1A, Auto_003C1A, 200, 0x003C22) { pc_run_003C06(ctx, 0x003C1A); }
static void pc_run_0047CE(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x0047CE: goto L_0047CE;
	case 0x004802: goto L_004802;
	default: __builtin_trap();
	}
L_0047CE:
	{ uint32_t eaS = 0xFFFF9E98u; ctx->a[0] = ld32(eaS); } hle_insn(ctx, 20);  /* 0047CE moveal ffff9e98,%a0 */
	{ uint32_t eaS = 0xFFFFAFD2u; ctx->a[1] = eaS; } hle_insn(ctx, 12);  /* 0047D4 lea ffffafd2,%a1 */
	{ uint32_t eaS = 0x00005440u; ctx->a[2] = eaS; } hle_insn(ctx, 8);  /* 0047DA lea %pc@(5440),%a2 */
	{ uint32_t eaS = 0x000054D0u; ctx->a[4] = eaS; } hle_insn(ctx, 8);  /* 0047DE lea %pc@(54d0),%a4 */
	{ uint32_t eaS = 0x0000553Cu; ctx->a[5] = eaS; } hle_insn(ctx, 8);  /* 0047E2 lea %pc@(553c),%a5 */
	{ uint32_t eaS = 0xFFFFAE5Cu; ctx->a[6] = eaS; } hle_insn(ctx, 12);  /* 0047E6 lea ffffae5c,%fp */
	{ uint32_t v = 0xE000u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0047EC movew #-8192,%d0 */
	{ uint32_t v = 0x40u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0047F0 movew #64,%d1 */
	{ ctx->d[2] = 0x00000008u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 0047F4 moveq #8,%d2 */
	{ uint32_t eaD = 0xFFFFA0AAu; uint32_t v = 0x8000u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 20);  /* 0047F6 movew #-32768,ffffa0aa */
	hle_jump(ctx, 0x0047FE); return;  /* 0047FE bsrw 498a */
L_004802:
	{ uint32_t eaS = 0x0000548Au; ctx->a[2] = eaS; } hle_insn(ctx, 8);  /* 004802 lea %pc@(548a),%a2 */
	{ uint32_t eaS = 0xFFFFB00Au; ctx->a[1] = eaS; } hle_insn(ctx, 12);  /* 004806 lea ffffb00a,%a1 */
	{ uint32_t v = 0xC000u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00480C movew #-16384,%d0 */
	{ uint32_t v = 0x40u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 004810 movew #64,%d1 */
	{ ctx->d[2] = 0x00000008u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 004814 moveq #8,%d2 */
	{ uint32_t eaD = 0xFFFFA0AAu; st16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 20);  /* 004816 clrw ffffa0aa */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00498A); return;  /* 00481C braw 498a */
}

HLE_RANGE_PURE(0x0047CE, Auto_0047CE, 200, 0x0047FE) { pc_run_0047CE(ctx, 0x0047CE); }
HLE_RANGE_PURE(0x004802, Auto_004802, 200, 0x00498A) { pc_run_0047CE(ctx, 0x004802); }
static void pc_run_0056FA(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x0056FE: goto L_0056FE;
	default: __builtin_trap();
	}
L_0056FA:
	hle_jump(ctx, 0x0056FA); return;  /* 0056FA jsr %pc@(5708) */
L_0056FE:
	{ uint32_t eaD = 0xFFFFA246u;  st8(eaD, 0xFF); } hle_insn(ctx, 20);  /* 0056FE st ffffa246 */
	hle_jump(ctx, 0x005704); return;  /* 005704 jmp %pc@(8a4c) */
}

HLE_RANGE_PURE(0x0056FE, Auto_0056FE, 200, 0x005704) { pc_run_0056FA(ctx, 0x0056FE); }
static void pc_run_005708(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x005708: goto L_005708;
	default: __builtin_trap();
	}
L_005708:
	{ uint32_t v = 0x3Fu; set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 005708 movew #63,%d7 */
	{ uint32_t eaS = 0xFFFFA134u; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 00570C lea ffffa134,%a0 */
L_005712:
	{ uint32_t eaD = ctx->a[0]; ctx->a[0] += 2; uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 005712 movew %d0,%a0@+ */
	{ uint16_t v = (uint16_t)ctx->d[7] - 1; set_w(&ctx->d[7], v); if (v != 0xFFFF) { hle_insn(ctx, 10); goto L_005712; } hle_insn(ctx, 14); }  /* 005714 dbf %d7,5712 */
	hle_jump(ctx, 0x005718); return;  /* 005718 rts */
}

HLE_RANGE_PURE(0x005708, Auto_005708, 200, 0x005718) { pc_run_005708(ctx, 0x005708); }
static void pc_run_005A24(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x005A24: goto L_005A24;
	default: __builtin_trap();
	}
L_005A24:
	{ uint32_t eaD = 0xFFFF9EC0u; cc_logic8(ctx, ld8(eaD)); } hle_insn(ctx, 16);  /* 005A24 tstb ffff9ec0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_005A3A; } hle_insn(ctx, 8);  /* 005A2A beqs 5a3a */
	{ uint32_t eaS = 0xFFFF9ED6u; ctx->a[1] = ld32(eaS); } hle_insn(ctx, 20);  /* 005A2C moveal ffff9ed6,%a1 */
	{ uint32_t eaD = ctx->a[1]; st16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 12);  /* 005A32 clrw %a1@ */
	{ uint32_t eaD = 0xFFFF80DAu; uint32_t v = alu(ctx, OP_SUB, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 20);  /* 005A34 subqw #1,ffff80da */
L_005A3A:
	{ uint32_t eaD = 0xFFFF9EC0u;  st8(eaD, 0xFF); } hle_insn(ctx, 20);  /* 005A3A st ffff9ec0 */
	{ uint32_t eaD = 0xFFFF9ED6u; uint32_t v = (ctx->a[6] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 20);  /* 005A40 movel %fp,ffff9ed6 */
	hle_jump(ctx, 0x005A46); return;  /* 005A46 rts */
}

HLE_RANGE_PURE(0x005A24, Auto_005A24, 200, 0x005A46) { pc_run_005A24(ctx, 0x005A24); }
static void pc_run_005A80(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x005A80: goto L_005A80;
	default: __builtin_trap();
	}
L_005A80:
	{ uint32_t eaS = 0x00005A8Cu; ctx->a[2] = eaS; } hle_insn(ctx, 8);  /* 005A80 lea %pc@(5a8c),%a2 */
	{ uint32_t eaS = 0x00005AB4u; ctx->a[3] = eaS; } hle_insn(ctx, 8);  /* 005A84 lea %pc@(5ab4),%a3 */
	hle_jump(ctx, 0x005A88); return;  /* 005A88 jmp %pc@(5940) */
}

HLE_RANGE_PURE(0x005A80, Auto_005A80, 200, 0x005A88) { pc_run_005A80(ctx, 0x005A80); }
static void pc_run_005A8C(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x005A8C: goto L_005A8C;
	default: __builtin_trap();
	}
L_005A8C:
	{ uint32_t eaS = 0x00005A92u; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 005A8C lea %pc@(5a92),%a0 */
	hle_jump(ctx, 0x005A90); return;  /* 005A90 rts */
}

HLE_RANGE_PURE(0x005A8C, Auto_005A8C, 200, 0x005A90) { pc_run_005A8C(ctx, 0x005A8C); }
static void pc_run_005A92(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x005A96: goto L_005A96;
	default: __builtin_trap();
	}
L_005A92:
	hle_jump(ctx, 0x005A92); return;  /* 005A92 jsr %pc@(72f8) */
L_005A96:
	{ unsigned b = 15; uint32_t m = 1u << b; cc_set(ctx, CC_Z, (ctx->d[5] & m) ? 0 : CC_Z); ctx->d[5] &= ~m; hle_insn(ctx, 12 + ((b > 15) ? 2 : 0)); }  /* 005A96 bclr #15,%d5 */
	{ uint32_t eaD = ctx->a[6]; uint32_t v = (ctx->d[5] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 005A9A movew %d5,%fp@ */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(20); uint32_t v = 0x20u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 005A9C movew #32,%fp@(20) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0xB78Cu; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 005AA2 movel #46988,%fp@(32) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(50); uint32_t v = alu(ctx, OP_ADD, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 16);  /* 005AAA addqw #1,%a4@(50) */
	hle_jump(ctx, 0x005AAE); return;  /* 005AAE jsr %pc@(5a24) */
L_005AB2:
	hle_jump(ctx, 0x005AB2); return;  /* 005AB2 rts */
}

HLE_RANGE_PURE(0x005A96, Auto_005A96, 200, 0x005AAE) { pc_run_005A92(ctx, 0x005A96); }
static void pc_run_005DEA(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x005DEA: goto L_005DEA;
	default: __builtin_trap();
	}
L_005DEA:
	{ uint32_t eaS = 0x00005DF6u; ctx->a[2] = eaS; } hle_insn(ctx, 8);  /* 005DEA lea %pc@(5df6),%a2 */
	{ uint32_t eaS = 0x00005E14u; ctx->a[3] = eaS; } hle_insn(ctx, 8);  /* 005DEE lea %pc@(5e14),%a3 */
	hle_jump(ctx, 0x005DF2); return;  /* 005DF2 jmp %pc@(5940) */
}

HLE_RANGE_PURE(0x005DEA, Auto_005DEA, 200, 0x005DF2) { pc_run_005DEA(ctx, 0x005DEA); }
static void pc_run_005DF6(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x005DF6: goto L_005DF6;
	default: __builtin_trap();
	}
L_005DF6:
	{ uint32_t eaS = 0x00005DFCu; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 005DF6 lea %pc@(5dfc),%a0 */
	hle_jump(ctx, 0x005DFA); return;  /* 005DFA rts */
}

HLE_RANGE_PURE(0x005DF6, Auto_005DF6, 200, 0x005DFA) { pc_run_005DF6(ctx, 0x005DF6); }
static void pc_run_005E56(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x005E56: goto L_005E56;
	default: __builtin_trap();
	}
L_005E56:
	{ uint32_t eaS = 0x00005E88u; ctx->a[3] = eaS; } hle_insn(ctx, 8);  /* 005E56 lea %pc@(5e88),%a3 */
	{ uint32_t eaS = 0x00005E62u; ctx->a[2] = eaS; } hle_insn(ctx, 8);  /* 005E5A lea %pc@(5e62),%a2 */
	hle_jump(ctx, 0x005E5E); return;  /* 005E5E jmp %pc@(5940) */
}

HLE_RANGE_PURE(0x005E56, Auto_005E56, 200, 0x005E5E) { pc_run_005E56(ctx, 0x005E56); }
static void pc_run_005E90(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x005E90: goto L_005E90;
	default: __builtin_trap();
	}
L_005E90:
	{ uint32_t eaS = 0x00005EC2u; ctx->a[3] = eaS; } hle_insn(ctx, 8);  /* 005E90 lea %pc@(5ec2),%a3 */
	{ uint32_t eaS = 0x00005E9Cu; ctx->a[2] = eaS; } hle_insn(ctx, 8);  /* 005E94 lea %pc@(5e9c),%a2 */
	hle_jump(ctx, 0x005E98); return;  /* 005E98 jmp %pc@(5940) */
}

HLE_RANGE_PURE(0x005E90, Auto_005E90, 200, 0x005E98) { pc_run_005E90(ctx, 0x005E90); }
static void pc_run_005E9C(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x005E9C: goto L_005E9C;
	default: __builtin_trap();
	}
L_005E9C:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0xBA8Eu; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 005E9C movel #47758,%fp@(32) */
	{ uint32_t eaS = 0x00005EAAu; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 005EA4 lea %pc@(5eaa),%a0 */
	hle_jump(ctx, 0x005EA8); return;  /* 005EA8 rts */
}

HLE_RANGE_PURE(0x005E9C, Auto_005E9C, 200, 0x005EA8) { pc_run_005E9C(ctx, 0x005E9C); }
static void pc_run_005ECA(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x005ECA: goto L_005ECA;
	default: __builtin_trap();
	}
L_005ECA:
	{ uint32_t eaS = 0x00005EFCu; ctx->a[3] = eaS; } hle_insn(ctx, 8);  /* 005ECA lea %pc@(5efc),%a3 */
	{ uint32_t eaS = 0x00005ED6u; ctx->a[2] = eaS; } hle_insn(ctx, 8);  /* 005ECE lea %pc@(5ed6),%a2 */
	hle_jump(ctx, 0x005ED2); return;  /* 005ED2 jmp %pc@(5940) */
}

HLE_RANGE_PURE(0x005ECA, Auto_005ECA, 200, 0x005ED2) { pc_run_005ECA(ctx, 0x005ECA); }
static void pc_run_005ED6(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x005ED6: goto L_005ED6;
	default: __builtin_trap();
	}
L_005ED6:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0xBBAEu; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 005ED6 movel #48046,%fp@(32) */
	{ uint32_t eaS = 0x00005EE4u; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 005EDE lea %pc@(5ee4),%a0 */
	hle_jump(ctx, 0x005EE2); return;  /* 005EE2 rts */
}

HLE_RANGE_PURE(0x005ED6, Auto_005ED6, 200, 0x005EE2) { pc_run_005ED6(ctx, 0x005ED6); }
static void pc_run_0060F4(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x0060F4: goto L_0060F4;
	default: __builtin_trap();
	}
L_0060F4:
	{ uint32_t eaS = 0x0000611Eu; ctx->a[3] = eaS; } hle_insn(ctx, 8);  /* 0060F4 lea %pc@(611e),%a3 */
	{ uint32_t eaS = 0x00006100u; ctx->a[2] = eaS; } hle_insn(ctx, 8);  /* 0060F8 lea %pc@(6100),%a2 */
	hle_jump(ctx, 0x0060FC); return;  /* 0060FC jmp %pc@(5940) */
}

HLE_RANGE_PURE(0x0060F4, Auto_0060F4, 200, 0x0060FC) { pc_run_0060F4(ctx, 0x0060F4); }
static void pc_run_006100(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x006100: goto L_006100;
	default: __builtin_trap();
	}
L_006100:
	{ uint32_t eaS = 0x00006106u; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 006100 lea %pc@(6106),%a0 */
	hle_jump(ctx, 0x006104); return;  /* 006104 rts */
}

HLE_RANGE_PURE(0x006100, Auto_006100, 200, 0x006104) { pc_run_006100(ctx, 0x006100); }
static void pc_run_006304(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x006304: goto L_006304;
	default: __builtin_trap();
	}
L_006304:
	{ uint32_t eaS = 0x0000630Au; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 006304 lea %pc@(630a),%a0 */
	hle_jump(ctx, 0x006308); return;  /* 006308 rts */
}

HLE_RANGE_PURE(0x006304, Auto_006304, 200, 0x006308) { pc_run_006304(ctx, 0x006304); }
static void pc_run_00635C(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00635C: goto L_00635C;
	default: __builtin_trap();
	}
L_00635C:
	{ uint32_t eaS = 0x00006386u; ctx->a[3] = eaS; } hle_insn(ctx, 8);  /* 00635C lea %pc@(6386),%a3 */
	{ uint32_t eaS = 0x00006368u; ctx->a[2] = eaS; } hle_insn(ctx, 8);  /* 006360 lea %pc@(6368),%a2 */
	hle_jump(ctx, 0x006364); return;  /* 006364 jmp %pc@(5940) */
}

HLE_RANGE_PURE(0x00635C, Auto_00635C, 200, 0x006364) { pc_run_00635C(ctx, 0x00635C); }
static void pc_run_006368(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x006368: goto L_006368;
	default: __builtin_trap();
	}
L_006368:
	{ uint32_t eaS = 0x0000636Eu; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 006368 lea %pc@(636e),%a0 */
	hle_jump(ctx, 0x00636C); return;  /* 00636C rts */
}

HLE_RANGE_PURE(0x006368, Auto_006368, 200, 0x00636C) { pc_run_006368(ctx, 0x006368); }
static void pc_run_00636E(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x006372: goto L_006372;
	default: __builtin_trap();
	}
L_00636E:
	hle_jump(ctx, 0x00636E); return;  /* 00636E jsr %pc@(72ce) */
L_006372:
	{ unsigned b = 15; uint32_t m = 1u << b; cc_set(ctx, CC_Z, (ctx->d[5] & m) ? 0 : CC_Z); ctx->d[5] &= ~m; hle_insn(ctx, 12 + ((b > 15) ? 2 : 0)); }  /* 006372 bclr #15,%d5 */
	{ uint32_t eaD = ctx->a[6]; uint32_t v = (ctx->d[5] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 006376 movew %d5,%fp@ */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0xDB5Au; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 006378 movel #56154,%fp@(32) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(58); uint32_t v = alu(ctx, OP_ADD, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 16);  /* 006380 addqw #1,%a4@(58) */
	hle_jump(ctx, 0x006384); return;  /* 006384 rts */
}

HLE_RANGE_PURE(0x006372, Auto_006372, 200, 0x006384) { pc_run_00636E(ctx, 0x006372); }
static void pc_run_0063F2(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x0063F2: goto L_0063F2;
	default: __builtin_trap();
	}
L_0063F2:
	{ uint32_t eaS = 0x0000641Cu; ctx->a[3] = eaS; } hle_insn(ctx, 8);  /* 0063F2 lea %pc@(641c),%a3 */
	{ uint32_t eaS = 0x000063FEu; ctx->a[2] = eaS; } hle_insn(ctx, 8);  /* 0063F6 lea %pc@(63fe),%a2 */
	hle_jump(ctx, 0x0063FA); return;  /* 0063FA jmp %pc@(5940) */
}

HLE_RANGE_PURE(0x0063F2, Auto_0063F2, 200, 0x0063FA) { pc_run_0063F2(ctx, 0x0063F2); }
static void pc_run_0064E0(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x0064E0: goto L_0064E0;
	default: __builtin_trap();
	}
L_0064E0:
	{ uint32_t eaS = 0x0000650Au; ctx->a[3] = eaS; } hle_insn(ctx, 8);  /* 0064E0 lea %pc@(650a),%a3 */
	{ uint32_t eaS = 0x000064ECu; ctx->a[2] = eaS; } hle_insn(ctx, 8);  /* 0064E4 lea %pc@(64ec),%a2 */
	hle_jump(ctx, 0x0064E8); return;  /* 0064E8 jmp %pc@(5940) */
}

HLE_RANGE_PURE(0x0064E0, Auto_0064E0, 200, 0x0064E8) { pc_run_0064E0(ctx, 0x0064E0); }
static void pc_run_0064EC(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x0064EC: goto L_0064EC;
	default: __builtin_trap();
	}
L_0064EC:
	{ uint32_t eaS = 0x000064F2u; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 0064EC lea %pc@(64f2),%a0 */
	hle_jump(ctx, 0x0064F0); return;  /* 0064F0 rts */
}

HLE_RANGE_PURE(0x0064EC, Auto_0064EC, 200, 0x0064F0) { pc_run_0064EC(ctx, 0x0064EC); }
static void pc_run_0069BC(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x0069BC: goto L_0069BC;
	default: __builtin_trap();
	}
L_0069BC:
	{ uint32_t eaS = 0x000069E6u; ctx->a[3] = eaS; } hle_insn(ctx, 8);  /* 0069BC lea %pc@(69e6),%a3 */
	{ uint32_t eaS = 0x000069C8u; ctx->a[2] = eaS; } hle_insn(ctx, 8);  /* 0069C0 lea %pc@(69c8),%a2 */
	hle_jump(ctx, 0x0069C4); return;  /* 0069C4 jmp %pc@(5940) */
}

HLE_RANGE_PURE(0x0069BC, Auto_0069BC, 200, 0x0069C4) { pc_run_0069BC(ctx, 0x0069BC); }
static void pc_run_0069C8(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x0069C8: goto L_0069C8;
	default: __builtin_trap();
	}
L_0069C8:
	{ uint32_t eaS = 0x000069CEu; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 0069C8 lea %pc@(69ce),%a0 */
	hle_jump(ctx, 0x0069CC); return;  /* 0069CC rts */
}

HLE_RANGE_PURE(0x0069C8, Auto_0069C8, 200, 0x0069CC) { pc_run_0069C8(ctx, 0x0069C8); }
static void pc_run_006C8E(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x006C8E: goto L_006C8E;
	default: __builtin_trap();
	}
L_006C8E:
	{ uint32_t eaD = ctx->a[6];  unsigned b = 7; uint32_t m = 1u << b; uint8_t v = ld8(eaD); cc_set(ctx, CC_Z, (v & m) ? 0 : CC_Z);  } hle_insn(ctx, 12);  /* 006C8E btst #15,%fp@ */
	if (COND_NE) { hle_insn(ctx, 10); goto L_006C96; } hle_insn(ctx, 8);  /* 006C92 bnes 6c96 */
	hle_jump(ctx, 0x006C94); return;  /* 006C94 rts */
L_006C96:
	{ uint32_t eaS = 0x00006D52u; ctx->a[3] = eaS; } hle_insn(ctx, 8);  /* 006C96 lea %pc@(6d52),%a3 */
	{ uint32_t eaD = 0xFFFFA0CAu; alu(ctx, OP_SUB, ld16(eaD), 0x1Eu, 16, 0); } hle_insn(ctx, 20);  /* 006C9A cmpiw #30,ffffa0ca */
	if (COND_LT) { hle_insn(ctx, 10); goto L_006CA8; } hle_insn(ctx, 8);  /* 006CA2 blts 6ca8 */
	{ uint32_t eaS = 0x00006D5Au; ctx->a[3] = eaS; } hle_insn(ctx, 8);  /* 006CA4 lea %pc@(6d5a),%a3 */
L_006CA8:
	{ uint32_t eaS = 0x00006CB0u; ctx->a[2] = eaS; } hle_insn(ctx, 8);  /* 006CA8 lea %pc@(6cb0),%a2 */
	hle_jump(ctx, 0x006CAC); return;  /* 006CAC jmp %pc@(5940) */
}

HLE_RANGE_PURE(0x006C8E, Auto_006C8E, 200, 0x006C94, 0x006CAC) { pc_run_006C8E(ctx, 0x006C8E); }
static void pc_run_006CB0(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x006CB0: goto L_006CB0;
	case 0x006CCE: goto L_006CCE;
	default: __builtin_trap();
	}
L_006CB0:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(4); alu(ctx, OP_SUB, ld16(eaD), 0xAAu, 16, 0); } hle_insn(ctx, 16);  /* 006CB0 cmpiw #170,%fp@(4) */
	if (COND_NE) { hle_insn(ctx, 10); goto L_006CCA; } hle_insn(ctx, 8);  /* 006CB6 bnes 6cca */
	{ uint32_t eaD = ctx->a[6];  unsigned b = 7; uint32_t m = 1u << b; uint8_t v = ld8(eaD); cc_set(ctx, CC_Z, (v & m) ? 0 : CC_Z); st8(eaD, v & ~m); } hle_insn(ctx, 16);  /* 006CB8 bclr #15,%fp@ */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0x14D80u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 006CBC movel #85376,%fp@(32) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = 0xCu; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 006CC4 movew #12,%fp@(36) */
L_006CCA:
	hle_jump(ctx, 0x006CCA); return;  /* 006CCA bsrw 6e32 */
L_006CCE:
	{ uint32_t eaS = 0x00006CD4u; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 006CCE lea %pc@(6cd4),%a0 */
	hle_jump(ctx, 0x006CD2); return;  /* 006CD2 rts */
}

HLE_RANGE_PURE(0x006CB0, Auto_006CB0, 200, 0x006CCA) { pc_run_006CB0(ctx, 0x006CB0); }
HLE_RANGE_PURE(0x006CCE, Auto_006CCE, 200, 0x006CD2) { pc_run_006CB0(ctx, 0x006CCE); }
static void pc_run_006E90(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x006E90: goto L_006E90;
	case 0x006E9C: goto L_006E9C;
	default: __builtin_trap();
	}
L_006E90:
	{ uint32_t eaS = 0x00006EE8u; ctx->a[3] = eaS; } hle_insn(ctx, 8);  /* 006E90 lea %pc@(6ee8),%a3 */
	{ uint32_t eaS = 0x00006EA8u; ctx->a[2] = eaS; } hle_insn(ctx, 8);  /* 006E94 lea %pc@(6ea8),%a2 */
	hle_jump(ctx, 0x006E98); return;  /* 006E98 jsr %pc@(5940) */
L_006E9C:
	{ uint32_t eaS = 0x00006EF0u; ctx->a[3] = eaS; } hle_insn(ctx, 8);  /* 006E9C lea %pc@(6ef0),%a3 */
	{ uint32_t eaS = 0x00006EA8u; ctx->a[2] = eaS; } hle_insn(ctx, 8);  /* 006EA0 lea %pc@(6ea8),%a2 */
	hle_jump(ctx, 0x006EA4); return;  /* 006EA4 jmp %pc@(5940) */
}

HLE_RANGE_PURE(0x006E90, Auto_006E90, 200, 0x006E98) { pc_run_006E90(ctx, 0x006E90); }
HLE_RANGE_PURE(0x006E9C, Auto_006E9C, 200, 0x006EA4) { pc_run_006E90(ctx, 0x006E9C); }
static void pc_run_00700A(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	default: __builtin_trap();
	}
L_00700A:
	hle_jump(ctx, 0x00700A); return;  /* 00700A rts */
}

static void pc_run_0070CA(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x0070CA: goto L_0070CA;
	case 0x0070D4: goto L_0070D4;
	default: __builtin_trap();
	}
L_0070CA:
	{ uint32_t eaS = 0x000070E6u; ctx->a[3] = eaS; } hle_insn(ctx, 8);  /* 0070CA lea %pc@(70e6),%a3 */
	{ ctx->d[6] = 0x00000001u; cc_logic32(ctx, ctx->d[6]); } hle_insn(ctx, 4);  /* 0070CE moveq #1,%d6 */
	hle_jump(ctx, 0x0070D0); return;  /* 0070D0 jsr %pc@(7086) */
L_0070D4:
	{ cc_logic16(ctx, (ctx->d[6] & 0xFFFFu)); } hle_insn(ctx, 4);  /* 0070D4 tstw %d6 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0070E4; } hle_insn(ctx, 8);  /* 0070D6 beqs 70e4 */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(42); uint32_t v = 0xFC18u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 0070D8 movew #-1000,%a4@(42) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(28); uint32_t v = 0xFFFFu; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 0070DE movew #-1,%a4@(28) */
L_0070E4:
	hle_jump(ctx, 0x0070E4); return;  /* 0070E4 rts */
}

HLE_RANGE_PURE(0x0070CA, Auto_0070CA, 200, 0x0070D0) { pc_run_0070CA(ctx, 0x0070CA); }
HLE_RANGE_PURE(0x0070D4, Auto_0070D4, 200, 0x0070E4) { pc_run_0070CA(ctx, 0x0070D4); }
static void pc_run_0070EE(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x0070EE: goto L_0070EE;
	case 0x0070F8: goto L_0070F8;
	default: __builtin_trap();
	}
L_0070EE:
	{ uint32_t eaS = 0x00007104u; ctx->a[3] = eaS; } hle_insn(ctx, 8);  /* 0070EE lea %pc@(7104),%a3 */
	{ ctx->d[6] = 0x00000002u; cc_logic32(ctx, ctx->d[6]); } hle_insn(ctx, 4);  /* 0070F2 moveq #2,%d6 */
	hle_jump(ctx, 0x0070F4); return;  /* 0070F4 jsr %pc@(7086) */
L_0070F8:
	{ cc_logic16(ctx, (ctx->d[6] & 0xFFFFu)); } hle_insn(ctx, 4);  /* 0070F8 tstw %d6 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_007102; } hle_insn(ctx, 8);  /* 0070FA beqs 7102 */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(28); uint32_t v = 0xFFFFu; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 0070FC movew #-1,%a4@(28) */
L_007102:
	hle_jump(ctx, 0x007102); return;  /* 007102 rts */
}

HLE_RANGE_PURE(0x0070EE, Auto_0070EE, 200, 0x0070F4) { pc_run_0070EE(ctx, 0x0070EE); }
HLE_RANGE_PURE(0x0070F8, Auto_0070F8, 200, 0x007102) { pc_run_0070EE(ctx, 0x0070F8); }
static void pc_run_00710C(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00710C: goto L_00710C;
	case 0x007116: goto L_007116;
	default: __builtin_trap();
	}
L_00710C:
	{ uint32_t eaS = 0x00007122u; ctx->a[3] = eaS; } hle_insn(ctx, 8);  /* 00710C lea %pc@(7122),%a3 */
	{ ctx->d[6] = 0x00000003u; cc_logic32(ctx, ctx->d[6]); } hle_insn(ctx, 4);  /* 007110 moveq #3,%d6 */
	hle_jump(ctx, 0x007112); return;  /* 007112 jsr %pc@(7086) */
L_007116:
	{ cc_logic16(ctx, (ctx->d[6] & 0xFFFFu)); } hle_insn(ctx, 4);  /* 007116 tstw %d6 */
	if (COND_EQ) { hle_insn(ctx, 10); hle_jump(ctx, 0x007120); return; } hle_insn(ctx, 8); hle_jump(ctx, 0x00711A); return;  /* 007118 beqs 7120 */
}

HLE_RANGE_PURE(0x00710C, Auto_00710C, 200, 0x007112) { pc_run_00710C(ctx, 0x00710C); }
HLE_RANGE_PURE(0x007116, Auto_007116, 200, 0x00711A, 0x007120) { pc_run_00710C(ctx, 0x007116); }
static void pc_run_007120(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	default: __builtin_trap();
	}
L_007120:
	hle_jump(ctx, 0x007120); return;  /* 007120 rts */
}

static void pc_run_00712A(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00712A: goto L_00712A;
	case 0x007134: goto L_007134;
	default: __builtin_trap();
	}
L_00712A:
	{ uint32_t eaS = 0x00007166u; ctx->a[3] = eaS; } hle_insn(ctx, 8);  /* 00712A lea %pc@(7166),%a3 */
	{ ctx->d[6] = 0x00000000u; cc_logic32(ctx, ctx->d[6]); } hle_insn(ctx, 4);  /* 00712E moveq #0,%d6 */
	hle_jump(ctx, 0x007130); return;  /* 007130 jsr %pc@(7086) */
L_007134:
	{ cc_logic16(ctx, (ctx->d[6] & 0xFFFFu)); } hle_insn(ctx, 4);  /* 007134 tstw %d6 */
	if (COND_EQ) { hle_insn(ctx, 10); hle_jump(ctx, 0x007164); return; } hle_insn(ctx, 8); hle_jump(ctx, 0x007138); return;  /* 007136 beqs 7164 */
}

HLE_RANGE_PURE(0x00712A, Auto_00712A, 200, 0x007130) { pc_run_00712A(ctx, 0x00712A); }
HLE_RANGE_PURE(0x007134, Auto_007134, 200, 0x007138, 0x007164) { pc_run_00712A(ctx, 0x007134); }
static void pc_run_007164(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	default: __builtin_trap();
	}
L_007164:
	hle_jump(ctx, 0x007164); return;  /* 007164 rts */
}

static void pc_run_00719A(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00719A: goto L_00719A;
	default: __builtin_trap();
	}
L_00719A:
	{ uint32_t eaS = ctx->a[4] + (uint32_t)(int32_t)(30); uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00719A movew %a4@(30),%d0 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), 0x4000u, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 8);  /* 00719E addiw #16384,%d0 */
	if (COND_CC) { hle_insn(ctx, 10); goto L_0071A6; } hle_insn(ctx, 8);  /* 0071A2 bccs 71a6 */
	{ ctx->d[0] = 0xFFFFFFFFu; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 0071A4 moveq #-1,%d0 */
L_0071A6:
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(30); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12); hle_jump(ctx, 0x0071AA); return;  /* 0071A6 movew %d0,%a4@(30) */
}

HLE_RANGE_PURE(0x00719A, Auto_00719A, 200, 0x0071AA) { pc_run_00719A(ctx, 0x00719A); }
static void pc_run_0071AA(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	default: __builtin_trap();
	}
L_0071AA:
	hle_jump(ctx, 0x0071AA); return;  /* 0071AA rts */
}

static void pc_run_00720C(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00720C: goto L_00720C;
	case 0x007216: goto L_007216;
	default: __builtin_trap();
	}
L_00720C:
	{ uint32_t eaS = 0x00007228u; ctx->a[3] = eaS; } hle_insn(ctx, 8);  /* 00720C lea %pc@(7228),%a3 */
	{ ctx->d[6] = 0x00000006u; cc_logic32(ctx, ctx->d[6]); } hle_insn(ctx, 4);  /* 007210 moveq #6,%d6 */
	hle_jump(ctx, 0x007212); return;  /* 007212 jsr %pc@(7086) */
L_007216:
	{ cc_logic16(ctx, (ctx->d[6] & 0xFFFFu)); } hle_insn(ctx, 4);  /* 007216 tstw %d6 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_007226; } hle_insn(ctx, 8);  /* 007218 beqs 7226 */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(22); alu(ctx, OP_SUB, ld16(eaD), 0x6u, 16, 0); } hle_insn(ctx, 16);  /* 00721A cmpiw #6,%a4@(22) */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_007226; } hle_insn(ctx, 8);  /* 007220 beqs 7226 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00711A); return;  /* 007222 braw 711a */
L_007226:
	hle_jump(ctx, 0x007226); return;  /* 007226 rts */
}

HLE_RANGE_PURE(0x00720C, Auto_00720C, 200, 0x007212) { pc_run_00720C(ctx, 0x00720C); }
HLE_RANGE_PURE(0x007216, Auto_007216, 200, 0x00711A, 0x007226) { pc_run_00720C(ctx, 0x007216); }
static void pc_run_007230(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x007230: goto L_007230;
	case 0x00723A: goto L_00723A;
	default: __builtin_trap();
	}
L_007230:
	{ uint32_t eaS = 0x00007258u; ctx->a[3] = eaS; } hle_insn(ctx, 8);  /* 007230 lea %pc@(7258),%a3 */
	{ ctx->d[6] = 0x00000009u; cc_logic32(ctx, ctx->d[6]); } hle_insn(ctx, 4);  /* 007234 moveq #9,%d6 */
	hle_jump(ctx, 0x007236); return;  /* 007236 jsr %pc@(7086) */
L_00723A:
	{ cc_logic16(ctx, (ctx->d[6] & 0xFFFFu)); } hle_insn(ctx, 4);  /* 00723A tstw %d6 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_007256; } hle_insn(ctx, 8);  /* 00723C beqs 7256 */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(20); uint32_t v = 0x3u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00723E movew #3,%a4@(20) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(28); uint32_t v = 0xFFFFu; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 007244 movew #-1,%a4@(28) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(44); uint32_t v = 0xFC18u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00724A movew #-1000,%a4@(44) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(42); uint32_t v = 0xFC18u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 007250 movew #-1000,%a4@(42) */
L_007256:
	hle_jump(ctx, 0x007256); return;  /* 007256 rts */
}

HLE_RANGE_PURE(0x007230, Auto_007230, 200, 0x007236) { pc_run_007230(ctx, 0x007230); }
HLE_RANGE_PURE(0x00723A, Auto_00723A, 200, 0x007256) { pc_run_007230(ctx, 0x00723A); }
static void pc_run_0072B2(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x0072B2: goto L_0072B2;
	default: __builtin_trap();
	}
L_0072B2:
	{ ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[1]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[0]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[0]); } hle_movem_l(ctx, 32, 3);  /* 0072B2 moveml %d0/%a0-%a1,%sp@- */
	{ uint32_t v = 0x50u; ctx->d[0] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 0072B6 movel #80,%d0 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x007376); return;  /* 0072BC braw 7376 */
}

HLE_RANGE_PURE(0x0072B2, Auto_0072B2, 200, HLE_EXIT_SP(0x007376, -12)) { pc_run_0072B2(ctx, 0x0072B2); }
static void pc_run_0072C0(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x0072C0: goto L_0072C0;
	default: __builtin_trap();
	}
L_0072C0:
	{ ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[1]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[0]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[0]); } hle_movem_l(ctx, 32, 3);  /* 0072C0 moveml %d0/%a0-%a1,%sp@- */
	{ uint32_t v = 0x50u; ctx->d[0] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 0072C4 movel #80,%d0 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x007376); return;  /* 0072CA braw 7376 */
}

HLE_RANGE_PURE(0x0072C0, Auto_0072C0, 200, HLE_EXIT_SP(0x007376, -12)) { pc_run_0072C0(ctx, 0x0072C0); }
static void pc_run_0072CE(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x0072CE: goto L_0072CE;
	default: __builtin_trap();
	}
L_0072CE:
	{ ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[1]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[0]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[0]); } hle_movem_l(ctx, 32, 3);  /* 0072CE moveml %d0/%a0-%a1,%sp@- */
	{ uint32_t v = 0x100u; ctx->d[0] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 0072D2 movel #256,%d0 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x007376); return;  /* 0072D8 braw 7376 */
}

HLE_RANGE_PURE(0x0072CE, Auto_0072CE, 200, HLE_EXIT_SP(0x007376, -12)) { pc_run_0072CE(ctx, 0x0072CE); }
static void pc_run_0072DC(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x0072DC: goto L_0072DC;
	default: __builtin_trap();
	}
L_0072DC:
	{ ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[1]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[0]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[0]); } hle_movem_l(ctx, 32, 3);  /* 0072DC moveml %d0/%a0-%a1,%sp@- */
	{ uint32_t v = 0x150u; ctx->d[0] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 0072E0 movel #336,%d0 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x007376); return;  /* 0072E6 braw 7376 */
}

HLE_RANGE_PURE(0x0072DC, Auto_0072DC, 200, HLE_EXIT_SP(0x007376, -12)) { pc_run_0072DC(ctx, 0x0072DC); }
static void pc_run_0072EA(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x0072EA: goto L_0072EA;
	default: __builtin_trap();
	}
L_0072EA:
	{ ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[1]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[0]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[0]); } hle_movem_l(ctx, 32, 3);  /* 0072EA moveml %d0/%a0-%a1,%sp@- */
	{ uint32_t v = 0x200u; ctx->d[0] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 0072EE movel #512,%d0 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x007376); return;  /* 0072F4 braw 7376 */
}

HLE_RANGE_PURE(0x0072EA, Auto_0072EA, 200, HLE_EXIT_SP(0x007376, -12)) { pc_run_0072EA(ctx, 0x0072EA); }
static void pc_run_007AD4(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x007AD4: goto L_007AD4;
	default: __builtin_trap();
	}
L_007AD4:
	{ ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[2]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[1]); } hle_movem_l(ctx, 24, 2);  /* 007AD4 moveml %d1-%d2,%sp@- */
	{ ctx->d[1] = 0x00000000u; cc_logic32(ctx, ctx->d[1]); } hle_insn(ctx, 4);  /* 007AD8 moveq #0,%d1 */
	{ ctx->d[2] = 0x00000009u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 007ADA moveq #9,%d2 */
L_007ADC:
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFFFFFu), (ctx->d[0] & 0xFFFFFFFFu), 32, 1); ctx->d[1] = v; } hle_insn(ctx, 8);  /* 007ADC addl %d0,%d1 */
	{ uint16_t v = (uint16_t)ctx->d[2] - 1; set_w(&ctx->d[2], v); if (v != 0xFFFF) { hle_insn(ctx, 10); goto L_007ADC; } hle_insn(ctx, 14); }  /* 007ADE dbf %d2,7adc */
	{ uint32_t v = (ctx->d[1] & 0xFFFFFFFFu); ctx->d[0] = v; cc_logic32(ctx, v); } hle_insn(ctx, 4);  /* 007AE2 movel %d1,%d0 */
	{ ctx->d[1] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->d[2] = ld32(ctx->a[7]); ctx->a[7] += 4; } hle_movem_l(ctx, 28, 2);  /* 007AE4 moveml %sp@+,%d1-%d2 */
	hle_jump(ctx, 0x007AE8); return;  /* 007AE8 rts */
}

HLE_RANGE_PURE(0x007AD4, Auto_007AD4, 200, 0x007AE8) { pc_run_007AD4(ctx, 0x007AD4); }
static void pc_run_007AEA(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x007AEE: goto L_007AEE;
	default: __builtin_trap();
	}
L_007AEA:
	hle_jump(ctx, 0x007AEA); return;  /* 007AEA jsr %pc@(7bb8) */
L_007AEE:
	{ uint32_t eaS = 0xFFFF80C0u; uint32_t v = ld16(eaS); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 007AEE movew ffff80c0,%d3 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[3] & 0xFFFFu), 0x6u, 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 4);  /* 007AF4 addqw #6,%d3 */
	{ uint32_t eaS = 0x0000328Cu; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 007AF6 lea %pc@(328c),%a0 */
	hle_jump(ctx, 0x007AFA); return;  /* 007AFA jmp %pc@(7e08) */
}

HLE_RANGE_PURE(0x007AEE, Auto_007AEE, 200, 0x007AFA) { pc_run_007AEA(ctx, 0x007AEE); }
static void pc_run_007AFE(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x007B02: goto L_007B02;
	default: __builtin_trap();
	}
L_007AFE:
	hle_jump(ctx, 0x007AFE); return;  /* 007AFE jsr %pc@(7bb8) */
L_007B02:
	{ uint32_t eaS = 0xFFFF80C0u; uint32_t v = ld16(eaS); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 007B02 movew ffff80c0,%d3 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[3] & 0xFFFFu), 0x10u, 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 8);  /* 007B08 addiw #16,%d3 */
	{ uint32_t eaS = 0x00002CBEu; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 007B0C lea %pc@(2cbe),%a0 */
	hle_jump(ctx, 0x007B10); return;  /* 007B10 jmp %pc@(7e08) */
}

HLE_RANGE_PURE(0x007B02, Auto_007B02, 200, 0x007B10) { pc_run_007AFE(ctx, 0x007B02); }
static void pc_run_007B14(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x007B18: goto L_007B18;
	default: __builtin_trap();
	}
L_007B14:
	hle_jump(ctx, 0x007B14); return;  /* 007B14 jsr %pc@(7bb8) */
L_007B18:
	{ uint32_t eaS = 0xFFFF80C0u; uint32_t v = ld16(eaS); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 007B18 movew ffff80c0,%d3 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[3] & 0xFFFFu), 0x2u, 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 4);  /* 007B1E addqw #2,%d3 */
	{ uint32_t eaS = 0x00002CAFu; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 007B20 lea %pc@(2caf),%a0 */
	hle_jump(ctx, 0x007B24); return;  /* 007B24 jmp %pc@(7e08) */
}

HLE_RANGE_PURE(0x007B18, Auto_007B18, 200, 0x007B24) { pc_run_007B14(ctx, 0x007B18); }
static void pc_run_007B28(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x007B2C: goto L_007B2C;
	default: __builtin_trap();
	}
L_007B28:
	hle_jump(ctx, 0x007B28); return;  /* 007B28 jsr %pc@(7bb8) */
L_007B2C:
	{ uint32_t eaS = 0xFFFF80C0u; uint32_t v = ld16(eaS); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 007B2C movew ffff80c0,%d3 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[3] & 0xFFFFu), 0x6u, 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 4);  /* 007B32 addqw #6,%d3 */
	{ uint32_t eaS = 0x00002CA4u; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 007B34 lea %pc@(2ca4),%a0 */
	hle_jump(ctx, 0x007B38); return;  /* 007B38 jmp %pc@(7e08) */
}

HLE_RANGE_PURE(0x007B2C, Auto_007B2C, 200, 0x007B38) { pc_run_007B28(ctx, 0x007B2C); }
static void pc_run_007B3C(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x007B40: goto L_007B40;
	default: __builtin_trap();
	}
L_007B3C:
	hle_jump(ctx, 0x007B3C); return;  /* 007B3C jsr %pc@(7bb8) */
L_007B40:
	{ uint32_t eaS = 0xFFFF80C0u; uint32_t v = ld16(eaS); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 007B40 movew ffff80c0,%d3 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[3] & 0xFFFFu), 0x6u, 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 4);  /* 007B46 addqw #6,%d3 */
	{ uint32_t eaS = 0x00002C99u; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 007B48 lea %pc@(2c99),%a0 */
	hle_jump(ctx, 0x007B4C); return;  /* 007B4C jmp %pc@(7e08) */
}

HLE_RANGE_PURE(0x007B40, Auto_007B40, 200, 0x007B4C) { pc_run_007B3C(ctx, 0x007B40); }
static void pc_run_007B50(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x007B54: goto L_007B54;
	default: __builtin_trap();
	}
L_007B50:
	hle_jump(ctx, 0x007B50); return;  /* 007B50 jsr %pc@(7bb8) */
L_007B54:
	{ uint32_t eaS = 0xFFFF80C0u; uint32_t v = ld16(eaS); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 007B54 movew ffff80c0,%d3 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[3] & 0xFFFFu), 0x6u, 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 4);  /* 007B5A addqw #6,%d3 */
	{ uint32_t eaS = 0x0000329Fu; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 007B5C lea %pc@(329f),%a0 */
	hle_jump(ctx, 0x007B60); return;  /* 007B60 jmp %pc@(7e08) */
}

HLE_RANGE_PURE(0x007B54, Auto_007B54, 200, 0x007B60) { pc_run_007B50(ctx, 0x007B54); }
static void pc_run_007BB8(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x007BB8: goto L_007BB8;
	case 0x007BDA: goto L_007BDA;
	default: __builtin_trap();
	}
L_007BB8:
	{ uint32_t v = 0xCD00u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 007BB8 movew #-13056,%d3 */
	{ alu(ctx, OP_SUB, ctx->a[4], 0xFFFF8000u, 32, 0); } hle_insn(ctx, 14);  /* 007BBC cmpal #-32768,%a4 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_007BC8; } hle_insn(ctx, 8);  /* 007BC2 beqs 7bc8 */
	{ uint32_t v = 0xCD2Au; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 007BC4 movew #-13014,%d3 */
L_007BC8:
	{ uint32_t eaD = 0xFFFF80C0u; uint32_t v = (ctx->d[3] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 007BC8 movew %d3,ffff80c0 */
	{ uint32_t eaS = 0x00002C6Eu; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 007BCE lea %pc@(2c6e),%a0 */
	{ uint32_t v = 0xC000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 007BD2 movew #-16384,%d6 */
	hle_jump(ctx, 0x007BD6); return;  /* 007BD6 jsr %pc@(7eb2) */
L_007BDA:
	{ uint32_t eaS = 0xFFFF80C0u; uint32_t v = ld16(eaS); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 007BDA movew ffff80c0,%d3 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[3] & 0xFFFFu), 0x80u, 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 8);  /* 007BE0 addiw #128,%d3 */
	{ uint32_t eaS = 0x00002C6Eu; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 007BE4 lea %pc@(2c6e),%a0 */
	hle_jump(ctx, 0x007BE8); return;  /* 007BE8 jmp %pc@(7eb2) */
}

HLE_RANGE_PURE(0x007BB8, Auto_007BB8, 200, 0x007BD6) { pc_run_007BB8(ctx, 0x007BB8); }
HLE_RANGE_PURE(0x007BDA, Auto_007BDA, 200, 0x007BE8) { pc_run_007BB8(ctx, 0x007BDA); }
static void pc_run_007BF8(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x007BF8: goto L_007BF8;
	default: __builtin_trap();
	}
L_007BF8:
	{ uint32_t eaS = 0xFFFF9EACu; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 007BF8 lea ffff9eac,%a0 */
	{ ctx->a[1] = (ctx->a[0] & 0xFFFFFFFFu); } hle_insn(ctx, 4);  /* 007BFE moveal %a0,%a1 */
	{ uint32_t eaD = ctx->a[1]; ctx->a[1] += 4; uint32_t v = (ctx->d[0] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 007C00 movel %d0,%a1@+ */
	hle_insn(ctx, 10); hle_jump(ctx, 0x007C1E); return;  /* 007C02 bras 7c1e */
}

HLE_RANGE_PURE(0x007BF8, Auto_007BF8, 200, 0x007C1E) { pc_run_007BF8(ctx, 0x007BF8); }
static void pc_run_007C04(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x007C04: goto L_007C04;
	default: __builtin_trap();
	}
L_007C04:
	{ uint32_t eaS = 0xFFFF9EB4u; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 007C04 lea ffff9eb4,%a0 */
	{ ctx->a[1] = (ctx->a[0] & 0xFFFFFFFFu); } hle_insn(ctx, 4);  /* 007C0A moveal %a0,%a1 */
	{ uint32_t eaD = ctx->a[1]; ctx->a[1] += 4; uint32_t v = (ctx->d[0] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 007C0C movel %d0,%a1@+ */
	hle_insn(ctx, 10); hle_jump(ctx, 0x007C1E); return;  /* 007C0E bras 7c1e */
}

HLE_RANGE_PURE(0x007C04, Auto_007C04, 200, 0x007C1E) { pc_run_007C04(ctx, 0x007C04); }
static void pc_run_007C54(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x007C54: goto L_007C54;
	default: __builtin_trap();
	}
L_007C54:
	{ uint32_t v = 0xC0A6u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 007C54 movew #-16218,%d3 */
	{ uint32_t eaS = 0xFFFF80CAu; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 007C58 lea ffff80ca,%a0 */
	{ uint32_t v = 0xC000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 007C5E movew #-16384,%d6 */
	{ ctx->d[7] = 0x00000000u; cc_logic32(ctx, ctx->d[7]); } hle_insn(ctx, 4);  /* 007C62 moveq #0,%d7 */
	hle_jump(ctx, 0x007C64); return;  /* 007C64 jmp %pc@(7d0e) */
}

HLE_RANGE_PURE(0x007C54, Auto_007C54, 200, 0x007C64) { pc_run_007C54(ctx, 0x007C54); }
static void pc_run_007C7C(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x007C7C: goto L_007C7C;
	default: __builtin_trap();
	}
L_007C7C:
	{ uint32_t eaS = ctx->a[4] + (uint32_t)(int32_t)(26); ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 007C7C lea %a4@(26),%a0 */
	{ ctx->a[1] = (ctx->a[0] & 0xFFFFFFFFu); } hle_insn(ctx, 4);  /* 007C80 moveal %a0,%a1 */
	{ uint32_t eaD = ctx->a[1]; ctx->a[1] += 2; uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 007C82 movew %d0,%a1@+ */
	hle_insn(ctx, 10); hle_jump(ctx, 0x007C90); return;  /* 007C84 bras 7c90 */
}

HLE_RANGE_PURE(0x007C7C, Auto_007C7C, 200, 0x007C90) { pc_run_007C7C(ctx, 0x007C7C); }
static void pc_run_007D5A(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x007D5A: goto L_007D5A;
	default: __builtin_trap();
	}
L_007D5A:
	{ ctx->d[7] = 0x00000000u; cc_logic32(ctx, ctx->d[7]); } hle_insn(ctx, 4);  /* 007D5A moveq #0,%d7 */
	{ uint32_t eaS = 0xFFFF9EB4u; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 007D5C lea ffff9eb4,%a0 */
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFu), 0xAu, 8, 0); } hle_insn(ctx, 8);  /* 007D62 cmpib #10,%d0 */
	if (COND_LT) { hle_insn(ctx, 10); goto L_007D6A; } hle_insn(ctx, 8);  /* 007D66 blts 7d6a */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFu), 0x6u, 8, 1); set_b(&ctx->d[0], v); } hle_insn(ctx, 4);  /* 007D68 addqb #6,%d0 */
L_007D6A:
	{ uint32_t eaD = ctx->a[0]; uint32_t v = (ctx->d[0] & 0xFFu); st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 8);  /* 007D6A moveb %d0,%a0@ */
	hle_insn(ctx, 10); hle_jump(ctx, 0x007D78); return;  /* 007D6C bras 7d78 */
}

HLE_RANGE_PURE(0x007D5A, Auto_007D5A, 200, 0x007D78) { pc_run_007D5A(ctx, 0x007D5A); }
static void pc_run_007D6E(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x007D6E: goto L_007D6E;
	default: __builtin_trap();
	}
L_007D6E:
	{ ctx->d[7] = 0x00000003u; cc_logic32(ctx, ctx->d[7]); } hle_insn(ctx, 4);  /* 007D6E moveq #3,%d7 */
	{ uint32_t eaS = 0xFFFF9EB4u; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 007D70 lea ffff9eb4,%a0 */
	{ uint32_t eaD = ctx->a[0]; uint32_t v = (ctx->d[0] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 12); hle_jump(ctx, 0x007D78); return;  /* 007D76 movel %d0,%a0@ */
}

HLE_RANGE_PURE(0x007D6E, Auto_007D6E, 200, 0x007D78) { pc_run_007D6E(ctx, 0x007D6E); }
static void pc_run_007D78(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x007D78: goto L_007D78;
	case 0x007DB2: goto L_007DB2;
	case 0x007DE8: goto L_007DE8;
	default: __builtin_trap();
	}
L_007D78:
	{ ctx->d[4] = 0x00000000u; cc_logic32(ctx, ctx->d[4]); } hle_insn(ctx, 4);  /* 007D78 moveq #0,%d4 */
L_007D7A:
	{ uint32_t eaS = ctx->a[0]; uint32_t v = ld8(eaS); set_b(&ctx->d[0], v); cc_logic8(ctx, v); } hle_insn(ctx, 8);  /* 007D7A moveb %a0@,%d0 */
	{ uint32_t v = shf(ctx, 'r', (ctx->d[0] & 0xFFFFu), 4, 16); set_w(&ctx->d[0], v); } hle_insn(ctx, 14);  /* 007D7C lsrw #4,%d0 */
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0xFu) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 007D7E andiw #15,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_007D8E; } hle_insn(ctx, 8);  /* 007D82 bnes 7d8e */
	{ cc_logic16(ctx, (ctx->d[4] & 0xFFFFu)); } hle_insn(ctx, 4);  /* 007D84 tstw %d4 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_007D8E; } hle_insn(ctx, 8);  /* 007D86 bnes 7d8e */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[3] & 0xFFFFu), 0x4u, 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 8);  /* 007D88 addiw #4,%d3 */
	hle_insn(ctx, 10); goto L_007DB2;  /* 007D8C bras 7db2 */
L_007D8E:
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[4] & 0xFFFFu), 0x1u, 16, 1); set_w(&ctx->d[4], v); } hle_insn(ctx, 4);  /* 007D8E addqw #1,%d4 */
	{ uint32_t v = 0x400u; set_w(&ctx->d[5], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 007D90 movew #1024,%d5 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), 0x1Au, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 8);  /* 007D94 addiw #26,%d0 */
	hle_jump(ctx, 0x007D98); return;  /* 007D98 jsr 11d12 */
L_007D9E:
	hle_jump(ctx, 0x007D9E); return;  /* 007D9E jsr %pc@(7fa4) */
L_007DA2:
	hle_jump(ctx, 0x007DA2); return;  /* 007DA2 jsr 11d28 */
L_007DA8:
	hle_jump(ctx, 0x007DA8); return;  /* 007DA8 jsr %pc@(86e2) */
L_007DAC:
	hle_jump(ctx, 0x007DAC); return;  /* 007DAC jsr 11d32 */
L_007DB2:
	{ uint32_t eaS = ctx->a[0]; ctx->a[0] += 1; uint32_t v = ld8(eaS); set_b(&ctx->d[0], v); cc_logic8(ctx, v); } hle_insn(ctx, 8);  /* 007DB2 moveb %a0@+,%d0 */
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0xFu) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 007DB4 andiw #15,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_007DC4; } hle_insn(ctx, 8);  /* 007DB8 bnes 7dc4 */
	{ cc_logic16(ctx, (ctx->d[4] & 0xFFFFu)); } hle_insn(ctx, 4);  /* 007DBA tstw %d4 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_007DC4; } hle_insn(ctx, 8);  /* 007DBC bnes 7dc4 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[3] & 0xFFFFu), 0x4u, 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 8);  /* 007DBE addiw #4,%d3 */
	hle_insn(ctx, 10); goto L_007DE8;  /* 007DC2 bras 7de8 */
L_007DC4:
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[4] & 0xFFFFu), 0x1u, 16, 1); set_w(&ctx->d[4], v); } hle_insn(ctx, 4);  /* 007DC4 addqw #1,%d4 */
	{ uint32_t v = 0x400u; set_w(&ctx->d[5], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 007DC6 movew #1024,%d5 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), 0x1Au, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 8);  /* 007DCA addiw #26,%d0 */
	hle_jump(ctx, 0x007DCE); return;  /* 007DCE jsr 11d12 */
L_007DD4:
	hle_jump(ctx, 0x007DD4); return;  /* 007DD4 jsr %pc@(7fa4) */
L_007DD8:
	hle_jump(ctx, 0x007DD8); return;  /* 007DD8 jsr 11d28 */
L_007DDE:
	hle_jump(ctx, 0x007DDE); return;  /* 007DDE jsr %pc@(86e2) */
L_007DE2:
	hle_jump(ctx, 0x007DE2); return;  /* 007DE2 jsr 11d32 */
L_007DE8:
	{ uint16_t v = (uint16_t)ctx->d[7] - 1; set_w(&ctx->d[7], v); if (v != 0xFFFF) { hle_insn(ctx, 10); goto L_007D7A; } hle_insn(ctx, 14); }  /* 007DE8 dbf %d7,7d7a */
	hle_jump(ctx, 0x007DEC); return;  /* 007DEC rts */
}

HLE_RANGE_PURE(0x007D78, Auto_007D78, 200, 0x007D98, 0x007DCE, 0x007DEC) { pc_run_007D78(ctx, 0x007D78); }
HLE_RANGE_PURE(0x007DB2, Auto_007DB2, 200, 0x007D98, 0x007DCE, 0x007DEC) { pc_run_007D78(ctx, 0x007DB2); }
HLE_RANGE_PURE(0x007DE8, Auto_007DE8, 200, 0x007D98, 0x007DCE, 0x007DEC) { pc_run_007D78(ctx, 0x007DE8); }
static void pc_run_007DF6(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x007DF6: goto L_007DF6;
	default: __builtin_trap();
	}
L_007DF6:
	{ uint32_t v = shf(ctx, 'L', (ctx->d[1] & 0xFFFFu), 7, 16); set_w(&ctx->d[1], v); } hle_insn(ctx, 20);  /* 007DF6 aslw #7,%d1 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), (ctx->d[0] & 0xFFFFu), 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 4);  /* 007DF8 addw %d0,%d0 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), (ctx->d[0] & 0xFFFFu), 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 4);  /* 007DFA addw %d0,%d1 */
	{ uint32_t v = 0xE000u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 007DFC movew #-8192,%d3 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[3] & 0xFFFFu), (ctx->d[1] & 0xFFFFu), 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 4); hle_jump(ctx, 0x007E02); return;  /* 007E00 addw %d1,%d3 */
}

HLE_RANGE_PURE(0x007DF6, Auto_007DF6, 200, 0x007E02) { pc_run_007DF6(ctx, 0x007DF6); }
static void pc_run_007E02(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x007E02: goto L_007E02;
	default: __builtin_trap();
	}
L_007E02:
	{ uint32_t eaD = 0xFFFF80C0u; uint32_t v = (ctx->d[3] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16); hle_jump(ctx, 0x007E08); return;  /* 007E02 movew %d3,ffff80c0 */
}

HLE_RANGE_PURE(0x007E02, Auto_007E02, 200, 0x007E08) { pc_run_007E02(ctx, 0x007E02); }
static void pc_run_007ED2(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x007EDC: goto L_007EDC;
	case 0x007EFA: goto L_007EFA;
	default: __builtin_trap();
	}
L_007ED2:
	hle_jump(ctx, 0x007ED2); return;  /* 007ED2 jsr 11d12 */
L_007ED8:
	hle_jump(ctx, 0x007ED8); return;  /* 007ED8 jsr %pc@(7f00) */
L_007EDC:
	{ uint32_t v = 0x78u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 007EDC movew #120,%d0 */
	hle_jump(ctx, 0x007EE0); return;  /* 007EE0 jsr 13d04 */
L_007EE6:
	hle_jump(ctx, 0x007EE6); return;  /* 007EE6 jsr 11d28 */
L_007EEC:
	hle_jump(ctx, 0x007EEC); return;  /* 007EEC jsr %pc@(86e2) */
L_007EF0:
	hle_jump(ctx, 0x007EF0); return;  /* 007EF0 jsr %pc@(86e2) */
L_007EF4:
	hle_jump(ctx, 0x007EF4); return;  /* 007EF4 jsr 11d32 */
L_007EFA:
	{ uint32_t eaD = ctx->a[0]; cc_logic8(ctx, ld8(eaD)); } hle_insn(ctx, 8);  /* 007EFA tstb %a0@ */
	if (COND_NE) { hle_insn(ctx, 10); goto L_007ED2; } hle_insn(ctx, 8);  /* 007EFC bnes 7ed2 */
	hle_jump(ctx, 0x007EFE); return;  /* 007EFE rts */
}

HLE_RANGE_PURE(0x007EDC, Auto_007EDC, 200, 0x007EE0) { pc_run_007ED2(ctx, 0x007EDC); }
HLE_RANGE_PURE(0x007EFA, Auto_007EFA, 200, 0x007ED2, 0x007EFE) { pc_run_007ED2(ctx, 0x007EFA); }
static void pc_run_007FF4(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	default: __builtin_trap();
	}
L_007FF4:
	hle_jump(ctx, 0x007FF4); return;  /* 007FF4 rts */
}

static void pc_run_0086D8(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x0086DC: goto L_0086DC;
	default: __builtin_trap();
	}
L_0086D8:
	hle_jump(ctx, 0x0086D8); return;  /* 0086D8 jsr %pc@(86e2) */
L_0086DC:
	{ uint16_t v = (uint16_t)ctx->d[1] - 1; set_w(&ctx->d[1], v); if (v != 0xFFFF) { hle_insn(ctx, 10); goto L_0086D8; } hle_insn(ctx, 14); }  /* 0086DC dbf %d1,86d8 */
	hle_jump(ctx, 0x0086E0); return;  /* 0086E0 rts */
}

HLE_RANGE_PURE(0x0086DC, Auto_0086DC, 200, 0x0086D8, 0x0086E0) { pc_run_0086D8(ctx, 0x0086DC); }
static void pc_run_008B02(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x008B02: goto L_008B02;
	default: __builtin_trap();
	}
L_008B02:
	{ uint32_t eaD = 0xFFFFA235u;  st8(eaD, 0); } hle_insn(ctx, 20);  /* 008B02 sf ffffa235 */
	{ uint32_t eaD = 0xFFFFA0A6u;  st8(eaD, 0); } hle_insn(ctx, 20);  /* 008B08 sf ffffa0a6 */
	{ uint32_t eaD = 0xFFFFA0F0u; uint32_t v = 0x87654321u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 28);  /* 008B0E movel #-2023406815,ffffa0f0 */
	hle_jump(ctx, 0x008B18); return;  /* 008B18 rts */
}

HLE_RANGE_PURE(0x008B02, Auto_008B02, 200, 0x008B18) { pc_run_008B02(ctx, 0x008B02); }
static void pc_run_008B1A(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x008B22: goto L_008B22;
	default: __builtin_trap();
	}
L_008B1A:
	hle_jump(ctx, 0x008B1A); return;  /* 008B1A bsrw 8bbe */
L_008B1E:
	hle_jump(ctx, 0x008B1E); return;  /* 008B1E bsrw 8bb4 */
L_008B22:
	{ uint32_t eaD = 0xFFFF8074u; uint32_t v = 0x1u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 20);  /* 008B22 movew #1,ffff8074 */
	{ uint32_t eaD = 0xFFFF8076u; uint32_t v = 0xFEu; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 20);  /* 008B2A movew #254,ffff8076 */
	{ uint32_t eaD = 0xFFFF8078u; uint32_t v = 0xE0u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 20);  /* 008B32 movew #224,ffff8078 */
	hle_jump(ctx, 0x008B3A); return;  /* 008B3A rts */
}

HLE_RANGE_PURE(0x008B22, Auto_008B22, 200, 0x008B3A) { pc_run_008B1A(ctx, 0x008B22); }
static void pc_run_008BB4(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x008BB4: goto L_008BB4;
	default: __builtin_trap();
	}
L_008BB4:
	{ uint32_t eaS = 0xFFFF8050u; ctx->a[4] = eaS; } hle_insn(ctx, 12);  /* 008BB4 lea ffff8050,%a4 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x008BC4); return;  /* 008BBA braw 8bc4 */
}

HLE_RANGE_PURE(0x008BB4, Auto_008BB4, 200, 0x008BC4) { pc_run_008BB4(ctx, 0x008BB4); }
static void pc_run_008BBE(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x008BBE: goto L_008BBE;
	default: __builtin_trap();
	}
L_008BBE:
	{ uint32_t eaS = 0xFFFF8000u; ctx->a[4] = eaS; } hle_insn(ctx, 12); hle_jump(ctx, 0x008BC4); return;  /* 008BBE lea ffff8000,%a4 */
}

HLE_RANGE_PURE(0x008BBE, Auto_008BBE, 200, 0x008BC4) { pc_run_008BBE(ctx, 0x008BBE); }
static void pc_run_008BC4(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x008BC8: goto L_008BC8;
	default: __builtin_trap();
	}
L_008BC4:
	hle_jump(ctx, 0x008BC4); return;  /* 008BC4 bsrw 8c0c */
L_008BC8:
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(36); uint32_t v = 0x0u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 008BC8 movew #0,%a4@(36) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(38); uint32_t v = 0x42u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 008BCE movew #66,%a4@(38) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(40); uint32_t v = 0xE0u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 008BD4 movew #224,%a4@(40) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(50); st16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 16);  /* 008BDA clrw %a4@(50) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(52); st16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 16);  /* 008BDE clrw %a4@(52) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(68); st16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 16);  /* 008BE2 clrw %a4@(68) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(54); st16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 16);  /* 008BE6 clrw %a4@(54) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(56); st16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 16);  /* 008BEA clrw %a4@(56) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(58); st16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 16);  /* 008BEE clrw %a4@(58) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(50); st16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 16);  /* 008BF2 clrw %a4@(50) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(60); st16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 16);  /* 008BF6 clrw %a4@(60) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(62); st16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 16);  /* 008BFA clrw %a4@(62) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(64); st16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 16);  /* 008BFE clrw %a4@(64) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(34); st16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 16);  /* 008C02 clrw %a4@(34) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(32); st16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 16);  /* 008C06 clrw %a4@(32) */
	hle_jump(ctx, 0x008C0A); return;  /* 008C0A rts */
}

HLE_RANGE_PURE(0x008BC8, Auto_008BC8, 200, 0x008C0A) { pc_run_008BC4(ctx, 0x008BC8); }
static void pc_run_008C0C(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x008C0C: goto L_008C0C;
	case 0x008C3C: goto L_008C3C;
	default: __builtin_trap();
	}
L_008C0C:
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(20); uint32_t v = 0x1u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 008C0C movew #1,%a4@(20) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(22); uint32_t v = 0x4u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 008C12 movew #4,%a4@(22) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(28); uint32_t v = 0xFFFFu; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 008C18 movew #-1,%a4@(28) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(30); uint32_t v = 0xFFFFu; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 008C1E movew #-1,%a4@(30) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(42); uint32_t v = 0x0u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 008C24 movew #0,%a4@(42) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(44); uint32_t v = 0x0u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 008C2A movew #0,%a4@(44) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(46); uint32_t v = 0x0u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 008C30 movew #0,%a4@(46) */
	{ ctx->d[0] = 0x00000025u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 008C36 moveq #37,%d0 */
	hle_jump(ctx, 0x008C38); return;  /* 008C38 bsrw 7c7c */
L_008C3C:
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(48); st16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 16);  /* 008C3C clrw %a4@(48) */
	{ uint32_t eaD = 0xFFFF80CCu; alu(ctx, OP_SUB, ld16(eaD), 0x5u, 16, 0); } hle_insn(ctx, 20);  /* 008C40 cmpiw #5,ffff80cc */
	if (COND_LT) { hle_insn(ctx, 10); goto L_008C50; } hle_insn(ctx, 8);  /* 008C48 blts 8c50 */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(22); uint32_t v = 0x5u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 008C4A movew #5,%a4@(22) */
L_008C50:
	hle_jump(ctx, 0x008C50); return;  /* 008C50 rts */
}

HLE_RANGE_PURE(0x008C0C, Auto_008C0C, 200, 0x008C38) { pc_run_008C0C(ctx, 0x008C0C); }
HLE_RANGE_PURE(0x008C3C, Auto_008C3C, 200, 0x008C50) { pc_run_008C0C(ctx, 0x008C3C); }
static void pc_run_008C68(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x008C68: goto L_008C68;
	default: __builtin_trap();
	}
L_008C68:
	{ uint32_t eaS = ctx->a[4] + (uint32_t)(int32_t)(34); uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 008C68 movew %a4@(34),%d0 */
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x2u, 16, 0); } hle_insn(ctx, 8);  /* 008C6C cmpiw #2,%d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_008C78; } hle_insn(ctx, 8);  /* 008C70 beqs 8c78 */
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x7u, 16, 0); } hle_insn(ctx, 8);  /* 008C72 cmpiw #7,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_008C8C; } hle_insn(ctx, 8);  /* 008C76 bnes 8c8c */
L_008C78:
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(36); uint32_t v = 0x0u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 008C78 movew #0,%a4@(36) */
	{ alu(ctx, OP_SUB, ctx->a[4], 0xFFFF8000u, 32, 0); } hle_insn(ctx, 14);  /* 008C7E cmpal #-32768,%a4 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_008C8C; } hle_insn(ctx, 8);  /* 008C84 beqs 8c8c */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(36); uint32_t v = 0x1u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 008C86 movew #1,%a4@(36) */
L_008C8C:
	hle_jump(ctx, 0x008C8C); return;  /* 008C8C rts */
}

HLE_RANGE_PURE(0x008C68, Auto_008C68, 200, 0x008C8C) { pc_run_008C68(ctx, 0x008C68); }
static void pc_run_008F0A(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x008F0A: goto L_008F0A;
	case 0x008F2C: goto L_008F2C;
	default: __builtin_trap();
	}
L_008F0A:
	{ uint32_t eaD = 0xFFFFA246u;  st8(eaD, 0xFF); } hle_insn(ctx, 20);  /* 008F0A st ffffa246 */
	{ uint32_t eaS = 0xFFFFA134u; ctx->a[1] = eaS; } hle_insn(ctx, 12);  /* 008F10 lea ffffa134,%a1 */
	{ uint32_t eaS = 0xFFFFB1D2u; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 008F16 lea ffffb1d2,%a0 */
	hle_jump(ctx, 0x008F1C); return;  /* 008F1C jsr %pc@(8a7c) */
L_008F20:
	hle_jump(ctx, 0x008F20); return;  /* 008F20 jsr %pc@(8a7c) */
L_008F24:
	hle_jump(ctx, 0x008F24); return;  /* 008F24 jsr %pc@(8a7c) */
L_008F28:
	hle_jump(ctx, 0x008F28); return;  /* 008F28 jsr %pc@(8a7c) */
L_008F2C:
	hle_insn(ctx, 10); hle_jump(ctx, 0x008F7E); return;  /* 008F2C bras 8f7e */
}

HLE_RANGE_PURE(0x008F0A, Auto_008F0A, 200, 0x008F1C) { pc_run_008F0A(ctx, 0x008F0A); }
HLE_RANGE_PURE(0x008F2C, Auto_008F2C, 200, 0x008F7E) { pc_run_008F0A(ctx, 0x008F2C); }
static void pc_run_008F7E(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x008F7E: goto L_008F7E;
	case 0x008F94: goto L_008F94;
	case 0x008FA0: goto L_008FA0;
	default: __builtin_trap();
	}
L_008F7E:
	{ ctx->d[0] = 0x00000001u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 008F7E moveq #1,%d0 */
L_008F80:
	{ ctx->a[7] -= 2; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 008F80 movew %d0,%sp@- */
	hle_jump(ctx, 0x008F82); return;  /* 008F82 jsr 86e2 */
L_008F88:
	hle_jump(ctx, 0x008F88); return;  /* 008F88 jsr 86e2 */
L_008F8E:
	hle_jump(ctx, 0x008F8E); return;  /* 008F8E jsr 8970 */
L_008F94:
	{ uint32_t eaS = 0xFFFFA1B4u; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 008F94 lea ffffa1b4,%a0 */
	hle_jump(ctx, 0x008F9A); return;  /* 008F9A jsr 80fe */
L_008FA0:
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 008FA0 movew %sp@+,%d0 */
	{ uint16_t v = (uint16_t)ctx->d[0] - 1; set_w(&ctx->d[0], v); if (v != 0xFFFF) { hle_insn(ctx, 10); goto L_008F80; } hle_insn(ctx, 14); }  /* 008FA2 dbf %d0,8f80 */
	hle_jump(ctx, 0x008FA6); return;  /* 008FA6 rts */
}

HLE_RANGE_PURE(0x008F7E, Auto_008F7E, 200, HLE_EXIT_SP(0x008F82, -2)) { pc_run_008F7E(ctx, 0x008F7E); }
HLE_RANGE_PURE(0x008F94, Auto_008F94, 200, 0x008F9A) { pc_run_008F7E(ctx, 0x008F94); }
HLE_RANGE_PURE(0x008FA0, Auto_008FA0, 200, 0x008F82, HLE_EXIT_SP(0x008FA6, 2)) { pc_run_008F7E(ctx, 0x008FA0); }
static void pc_run_009242(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x009242: goto L_009242;
	case 0x00925A: goto L_00925A;
	default: __builtin_trap();
	}
L_009242:
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(36); cc_logic16(ctx, ld16(eaD)); } hle_insn(ctx, 12);  /* 009242 tstw %a4@(36) */
	if (COND_MI) { hle_insn(ctx, 10); goto L_009278; } hle_insn(ctx, 8);  /* 009246 bmis 9278 */
	{ uint32_t v = shf(ctx, 'r', (ctx->d[0] & 0xFFFFu), 4, 16); set_w(&ctx->d[0], v); } hle_insn(ctx, 14);  /* 009248 lsrw #4,%d0 */
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0xFu) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00924A andiw #15,%d0 */
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 00924E movew %d0,%d1 */
	{ uint32_t eaS = 0xFFFFAF88u; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 009250 lea ffffaf88,%a0 */
	hle_jump(ctx, 0x009256); return;  /* 009256 jsr %pc@(927a) */
L_00925A:
	{ uint32_t eaS = ctx->a[0] + (uint32_t)(int32_t)(8); ctx->a[1] = eaS; } hle_insn(ctx, 8);  /* 00925A lea %a0@(8),%a1 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), 0x1u, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 4);  /* 00925E addqw #1,%d0 */
	{ uint32_t v = shf(ctx, 'r', (ctx->d[0] & 0xFFFFu), 1, 16); set_w(&ctx->d[0], v); } hle_insn(ctx, 8);  /* 009260 lsrw #1,%d0 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x1u, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 4);  /* 009262 subqw #1,%d0 */
	if (COND_MI) { hle_insn(ctx, 10); goto L_00926C; } hle_insn(ctx, 8);  /* 009264 bmis 926c */
L_009266:
	{ ctx->a[1] -= 1; uint32_t eaD = ctx->a[1]; uint32_t v = (ctx->d[2] & 0xFFu); st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 8);  /* 009266 moveb %d2,%a1@- */
	{ uint16_t v = (uint16_t)ctx->d[0] - 1; set_w(&ctx->d[0], v); if (v != 0xFFFF) { hle_insn(ctx, 10); goto L_009266; } hle_insn(ctx, 14); }  /* 009268 dbf %d0,9266 */
L_00926C:
	{ unsigned b = 0; uint32_t m = 1u << b; cc_set(ctx, CC_Z, (ctx->d[1] & m) ? 0 : CC_Z);  hle_insn(ctx, 10); }  /* 00926C btst #0,%d1 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_009274; } hle_insn(ctx, 8);  /* 009270 bnes 9274 */
	{ ctx->a[1] -= 1; uint32_t eaD = ctx->a[1]; uint32_t v = (ctx->d[4] & 0xFFu); st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 8);  /* 009272 moveb %d4,%a1@- */
L_009274:
	hle_jump(ctx, 0x009274); return;  /* 009274 jsr %pc@(7eb2) */
L_009278:
	hle_jump(ctx, 0x009278); return;  /* 009278 rts */
}

HLE_RANGE_PURE(0x009242, Auto_009242, 200, 0x009256, 0x009278) { pc_run_009242(ctx, 0x009242); }
HLE_RANGE_PURE(0x00925A, Auto_00925A, 200, 0x009274) { pc_run_009242(ctx, 0x00925A); }
static void pc_run_00927A(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00927A: goto L_00927A;
	default: __builtin_trap();
	}
L_00927A:
	{ uint32_t eaD = ctx->a[0]; uint32_t v = 0x4F4F4F4Fu; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 20);  /* 00927A movel #1330597711,%a0@ */
	{ uint32_t eaD = ctx->a[0] + (uint32_t)(int32_t)(4); uint32_t v = 0x4F4F4F4Fu; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 009280 movel #1330597711,%a0@(4) */
	{ uint32_t eaD = ctx->a[0] + (uint32_t)(int32_t)(8); uint32_t v = 0x0u; st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 16);  /* 009288 moveb #0,%a0@(8) */
	hle_jump(ctx, 0x00928E); return;  /* 00928E rts */
}

HLE_RANGE_PURE(0x00927A, Auto_00927A, 200, 0x00928E) { pc_run_00927A(ctx, 0x00927A); }
static void pc_run_0092A8(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x0092A8: goto L_0092A8;
	case 0x0092B8: goto L_0092B8;
	case 0x0092D6: goto L_0092D6;
	default: __builtin_trap();
	}
L_0092A8:
	{ ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[6]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[7]); } hle_movem_l(ctx, 24, 2);  /* 0092A8 moveml %d7/%fp,%sp@- */
	{ ctx->d[4] = 0x00000000u; cc_logic32(ctx, ctx->d[4]); } hle_insn(ctx, 4);  /* 0092AC moveq #0,%d4 */
	{ uint32_t eaS = 0xFFFFA0A8u; uint32_t v = ld16(eaS); set_w(&ctx->d[4], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 0092AE movew ffffa0a8,%d4 */
	{ ctx->d[3] = 0x00000002u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 0092B2 moveq #2,%d3 */
	hle_jump(ctx, 0x0092B4); return;  /* 0092B4 jsr %pc@(7a92) */
L_0092B8:
	{ uint32_t eaD = 0xFFFFA0B8u; uint32_t v = 0x1Cu; st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 20);  /* 0092B8 moveb #28,ffffa0b8 */
	{ uint32_t eaD = 0xFFFFA0B9u; uint32_t v = 0x2u; st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 20);  /* 0092C0 moveb #2,ffffa0b9 */
	{ uint32_t eaS = 0xFFFFA0B8u; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 0092C8 lea ffffa0b8,%a0 */
	{ uint32_t v = 0xC000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0092CE movew #-16384,%d6 */
	hle_jump(ctx, 0x0092D2); return;  /* 0092D2 jsr %pc@(7dee) */
L_0092D6:
	{ ctx->d[7] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->a[6] = ld32(ctx->a[7]); ctx->a[7] += 4; } hle_movem_l(ctx, 28, 2);  /* 0092D6 moveml %sp@+,%d7/%fp */
	hle_jump(ctx, 0x0092DA); return;  /* 0092DA rts */
}

HLE_RANGE_PURE(0x0092A8, Auto_0092A8, 200, HLE_EXIT_SP(0x0092B4, -8)) { pc_run_0092A8(ctx, 0x0092A8); }
HLE_RANGE_PURE(0x0092B8, Auto_0092B8, 200, 0x0092D2) { pc_run_0092A8(ctx, 0x0092B8); }
HLE_RANGE_PURE(0x0092D6, Auto_0092D6, 200, HLE_EXIT_SP(0x0092DA, 8)) { pc_run_0092A8(ctx, 0x0092D6); }
static void pc_run_009866(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x009866: goto L_009866;
	default: __builtin_trap();
	}
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
	hle_jump(ctx, 0x00988C); return;  /* 00988C rts */
}

HLE_RANGE_PURE(0x009866, Auto_009866, 200, 0x00988C) { pc_run_009866(ctx, 0x009866); }
static void pc_run_0098FC(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x0098FC: goto L_0098FC;
	default: __builtin_trap();
	}
L_0098FC:
	{ uint32_t eaS = 0xFFFFAB9Eu; ctx->a[4] = eaS; } hle_insn(ctx, 12);  /* 0098FC lea ffffab9e,%a4 */
	{ ctx->d[0] = 0x00000000u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 009902 moveq #0,%d0 */
	{ uint32_t eaD = ctx->a[4]; uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 009904 movew %d0,%a4@ */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(32); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 009906 movew %d0,%a4@(32) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(64); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00990A movew %d0,%a4@(64) */
	hle_jump(ctx, 0x00990E); return;  /* 00990E rts */
}

HLE_RANGE_PURE(0x0098FC, Auto_0098FC, 200, 0x00990E) { pc_run_0098FC(ctx, 0x0098FC); }
static void pc_run_00995A(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00995A: goto L_00995A;
	default: __builtin_trap();
	}
L_00995A:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(4); uint32_t v = 0x48u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00995A movew #72,%fp@(4) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(8); uint32_t v = 0x28u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 009960 movew #40,%fp@(8) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = 0xF0u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 009966 movew #240,%fp@(36) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(6); uint32_t v = 0xEu; st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 16);  /* 00996C moveb #14,%fp@(6) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(7); uint32_t v = 0x8Cu; st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 16);  /* 009972 moveb #-116,%fp@(7) */
	{ uint32_t eaD = 0xFFFF80DAu; uint32_t v = alu(ctx, OP_ADD, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 20);  /* 009978 addqw #1,ffff80da */
	{ uint32_t eaD = 0xFFFF9E08u; uint32_t v = alu(ctx, OP_ADD, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 20);  /* 00997E addqw #1,ffff9e08 */
	hle_jump(ctx, 0x009984); return;  /* 009984 jsr %pc@(998a) */
L_009988:
	hle_jump(ctx, 0x009988); return;  /* 009988 rts */
}

HLE_RANGE_PURE(0x00995A, Auto_00995A, 200, 0x009984) { pc_run_00995A(ctx, 0x00995A); }
static void pc_run_00998A(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00998A: goto L_00998A;
	default: __builtin_trap();
	}
L_00998A:
	{ uint32_t eaS = 0xFFFFAABEu; ctx->a[4] = eaS; } hle_insn(ctx, 12);  /* 00998A lea ffffaabe,%a4 */
	{ ctx->d[0] = 0x00000000u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 009990 moveq #0,%d0 */
	{ uint32_t eaD = ctx->a[4]; uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 009992 movew %d0,%a4@ */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(32); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 009994 movew %d0,%a4@(32) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(64); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 009998 movew %d0,%a4@(64) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(96); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00999C movew %d0,%a4@(96) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(128); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 0099A0 movew %d0,%a4@(128) */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(160); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 0099A4 movew %d0,%a4@(160) */
	hle_jump(ctx, 0x0099A8); return;  /* 0099A8 rts */
}

HLE_RANGE_PURE(0x00998A, Auto_00998A, 200, 0x0099A8) { pc_run_00998A(ctx, 0x00998A); }
static void pc_run_0099C8(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x0099CC: goto L_0099CC;
	default: __builtin_trap();
	}
L_0099C8:
	hle_jump(ctx, 0x0099C8); return;  /* 0099C8 bsrw 99d4 */
L_0099CC:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(38); uint32_t v = 0xFFFFu; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 0099CC movew #-1,%fp@(38) */
	hle_jump(ctx, 0x0099D2); return;  /* 0099D2 rts */
}

HLE_RANGE_PURE(0x0099CC, Auto_0099CC, 200, 0x0099D2) { pc_run_0099C8(ctx, 0x0099CC); }
static void pc_run_009A5E(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x009A62: goto L_009A62;
	case 0x009A70: goto L_009A70;
	default: __builtin_trap();
	}
L_009A5E:
	hle_jump(ctx, 0x009A5E); return;  /* 009A5E jsr %pc@(881c) */
L_009A62:
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0x3Fu) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 009A62 andiw #63,%d0 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), 0x38u, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 8);  /* 009A66 addiw #56,%d0 */
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[4], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 009A6A movew %d0,%d4 */
	hle_jump(ctx, 0x009A6C); return;  /* 009A6C jsr %pc@(881c) */
L_009A70:
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0x7Fu) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 009A70 andiw #127,%d0 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), 0x60u, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 8);  /* 009A74 addiw #96,%d0 */
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 4); hle_jump(ctx, 0x009A7A); return;  /* 009A78 movew %d0,%d3 */
}

HLE_RANGE_PURE(0x009A62, Auto_009A62, 200, 0x009A6C) { pc_run_009A5E(ctx, 0x009A62); }
HLE_RANGE_PURE(0x009A70, Auto_009A70, 200, 0x009A7A) { pc_run_009A5E(ctx, 0x009A70); }
static void pc_run_009A7A(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x009A7A: goto L_009A7A;
	default: __builtin_trap();
	}
L_009A7A:
	{ uint32_t v = (ctx->d[5] & 0xFFFFu); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 009A7A movew %d5,%d0 */
	{ uint32_t v = 0x1u; set_b(&ctx->d[5], v); cc_logic8(ctx, v); } hle_insn(ctx, 8);  /* 009A7C moveb #1,%d5 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[3] & 0xFFFFu), (ctx->d[0] & 0xFFFFu), 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 4);  /* 009A80 subw %d0,%d3 */
	if (COND_PL) { hle_insn(ctx, 10); goto L_009A88; } hle_insn(ctx, 8);  /* 009A82 bpls 9a88 */
	{ uint32_t v = alu(ctx, OP_SUB, 0, (ctx->d[5] & 0xFFu), 8, 1); set_b(&ctx->d[5], v); } hle_insn(ctx, 4);  /* 009A84 negb %d5 */
	{ uint32_t v = alu(ctx, OP_SUB, 0, (ctx->d[3] & 0xFFFFu), 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 4);  /* 009A86 negw %d3 */
L_009A88:
	{ alu(ctx, OP_SUB, (ctx->d[3] & 0xFFFFu), 0xFFu, 16, 0); } hle_insn(ctx, 8);  /* 009A88 cmpiw #255,%d3 */
	if (COND_LE) { hle_insn(ctx, 10); goto L_009A92; } hle_insn(ctx, 8);  /* 009A8C bles 9a92 */
	{ uint32_t v = 0xFFu; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 009A8E movew #255,%d3 */
L_009A92:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(14); uint32_t v = (ctx->d[5] & 0xFFu); st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 12);  /* 009A92 moveb %d5,%fp@(14) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 009A96 movew %d0,%fp@(2) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(10); uint32_t v = (ctx->d[3] & 0xFFu); st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 12);  /* 009A9A moveb %d3,%fp@(10) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(11); st8(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 16);  /* 009A9E clrb %fp@(11) */
	{ uint32_t v = 0x1u; set_b(&ctx->d[5], v); cc_logic8(ctx, v); } hle_insn(ctx, 8);  /* 009AA2 moveb #1,%d5 */
	{ uint32_t v = (ctx->d[4] & 0xFFFFu); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 009AA6 movew %d4,%d3 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[4] & 0xFFFFu), (ctx->d[1] & 0xFFFFu), 16, 1); set_w(&ctx->d[4], v); } hle_insn(ctx, 4);  /* 009AA8 subw %d1,%d4 */
	if (COND_PL) { hle_insn(ctx, 10); goto L_009AB0; } hle_insn(ctx, 8);  /* 009AAA bpls 9ab0 */
	{ uint32_t v = alu(ctx, OP_SUB, 0, (ctx->d[5] & 0xFFu), 8, 1); set_b(&ctx->d[5], v); } hle_insn(ctx, 4);  /* 009AAC negb %d5 */
	{ uint32_t v = alu(ctx, OP_SUB, 0, (ctx->d[4] & 0xFFFFu), 16, 1); set_w(&ctx->d[4], v); } hle_insn(ctx, 4);  /* 009AAE negw %d4 */
L_009AB0:
	{ alu(ctx, OP_SUB, (ctx->d[4] & 0xFFFFu), 0xFFu, 16, 0); } hle_insn(ctx, 8);  /* 009AB0 cmpiw #255,%d4 */
	if (COND_LE) { hle_insn(ctx, 10); goto L_009ABA; } hle_insn(ctx, 8);  /* 009AB4 bles 9aba */
	{ uint32_t v = 0xFFu; set_w(&ctx->d[4], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 009AB6 movew #255,%d4 */
L_009ABA:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(15); uint32_t v = (ctx->d[5] & 0xFFu); st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 12);  /* 009ABA moveb %d5,%fp@(15) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(4); uint32_t v = (ctx->d[1] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 009ABE movew %d1,%fp@(4) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(12); uint32_t v = (ctx->d[4] & 0xFFu); st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 12);  /* 009AC2 moveb %d4,%fp@(12) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(13); st8(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 16);  /* 009AC6 clrb %fp@(13) */
	hle_jump(ctx, 0x009ACA); return;  /* 009ACA rts */
}

HLE_RANGE_PURE(0x009A7A, Auto_009A7A, 200, 0x009ACA) { pc_run_009A7A(ctx, 0x009A7A); }
static void pc_run_009B14(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x009B14: goto L_009B14;
	default: __builtin_trap();
	}
L_009B14:
	{ uint32_t v = shf(ctx, 'L', (ctx->d[1] & 0xFFFFu), 5, 16); set_w(&ctx->d[1], v); } hle_insn(ctx, 16); hle_jump(ctx, 0x009B16); return;  /* 009B14 aslw #5,%d1 */
}

HLE_RANGE_PURE(0x009B14, Auto_009B14, 200, 0x009B16) { pc_run_009B14(ctx, 0x009B14); }
static void pc_run_009E86(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x009E86: goto L_009E86;
	case 0x009E98: goto L_009E98;
	default: __builtin_trap();
	}
L_009E86:
	{ uint32_t v = 0xA000u; ctx->d[6] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 009E86 movel #40960,%d6 */
	{ uint32_t eaS = 0xFFFF9E46u; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 009E8C movew ffff9e46,%d7 */
	hle_jump(ctx, 0x009E92); return;  /* 009E92 jsr 12234 */
L_009E98:
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 4; ctx->a[0] = ld32(eaS); } hle_insn(ctx, 12);  /* 009E98 moveal %sp@+,%a0 */
	{ uint32_t eaD = 0xFFFF9E0Cu; uint32_t v = alu(ctx, OP_ADD, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 20);  /* 009E9A addqw #1,ffff9e0c */
	hle_jump(ctx, 0x009EA0); return;  /* 009EA0 rts */
}

HLE_RANGE_PURE(0x009E86, Auto_009E86, 200, 0x009E92) { pc_run_009E86(ctx, 0x009E86); }
HLE_RANGE_PURE(0x009E98, Auto_009E98, 200, HLE_EXIT_SP(0x009EA0, 4)) { pc_run_009E86(ctx, 0x009E98); }
static void pc_run_009EF8(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x009EF8: goto L_009EF8;
	default: __builtin_trap();
	}
L_009EF8:
	{ uint32_t eaS = 0xFFFFAA5Cu; ctx->a[4] = eaS; } hle_insn(ctx, 12);  /* 009EF8 lea ffffaa5c,%a4 */
	{ ctx->d[1] = 0x0000001Fu; cc_logic32(ctx, ctx->d[1]); } hle_insn(ctx, 4);  /* 009EFE moveq #31,%d1 */
L_009F00:
	{ uint32_t eaD = ctx->a[4]; uint32_t v = (ctx->d[2] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 009F00 movew %d2,%a4@ */
	{ uint32_t eaS = ctx->a[4] + (uint32_t)(int32_t)(32); ctx->a[4] = eaS; } hle_insn(ctx, 8);  /* 009F02 lea %a4@(32),%a4 */
	{ uint16_t v = (uint16_t)ctx->d[1] - 1; set_w(&ctx->d[1], v); if (v != 0xFFFF) { hle_insn(ctx, 10); goto L_009F00; } hle_insn(ctx, 14); }  /* 009F06 dbf %d1,9f00 */
	hle_jump(ctx, 0x009F0A); return;  /* 009F0A rts */
}

HLE_RANGE_PURE(0x009EF8, Auto_009EF8, 200, 0x009F0A) { pc_run_009EF8(ctx, 0x009EF8); }
static void pc_run_00A52C(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00A52C: goto L_00A52C;
	case 0x00A548: goto L_00A548;
	case 0x00A580: goto L_00A580;
	case 0x00A59A: goto L_00A59A;
	default: __builtin_trap();
	}
L_00A52C:
	{ uint32_t v = 0xC332u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00A52C movew #-15566,%d3 */
	{ ctx->d[4] = 0x00000020u; cc_logic32(ctx, ctx->d[4]); } hle_insn(ctx, 4);  /* 00A530 moveq #32,%d4 */
	{ uint32_t v = 0x58u; set_w(&ctx->d[5], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00A532 movew #88,%d5 */
	{ uint32_t v = 0x282u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00A536 movew #642,%d1 */
	{ uint32_t eaS = 0xFFFFAF04u; ctx->a[1] = eaS; } hle_insn(ctx, 12);  /* 00A53A lea ffffaf04,%a1 */
	{ ctx->d[0] = 0x00000001u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 00A540 moveq #1,%d0 */
L_00A542:
	{ ctx->d[6] = 0x00000004u; cc_logic32(ctx, ctx->d[6]); } hle_insn(ctx, 4);  /* 00A542 moveq #4,%d6 */
L_00A544:
	hle_jump(ctx, 0x00A544); return;  /* 00A544 jsr %pc@(a5b2) */
L_00A548:
	{ uint32_t eaD = ctx->a[1]; ctx->a[1] += 4; uint32_t v = (ctx->a[6] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 00A548 movel %fp,%a1@+ */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[1] & 0xFFFFu), 0x40u, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 00A54A subiw #64,%d1 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[3] & 0xFFFFu), 0x10u, 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 8);  /* 00A54E addiw #16,%d3 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[4] & 0xFFFFu), 0x40u, 16, 1); set_w(&ctx->d[4], v); } hle_insn(ctx, 8);  /* 00A552 addiw #64,%d4 */
	{ uint16_t v = (uint16_t)ctx->d[6] - 1; set_w(&ctx->d[6], v); if (v != 0xFFFF) { hle_insn(ctx, 10); goto L_00A544; } hle_insn(ctx, 14); }  /* 00A556 dbf %d6,a544 */
	{ uint32_t eaD = ctx->a[1]; ctx->a[1] += 2; uint32_t v = 0x5u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00A55A movew #5,%a1@+ */
	{ uint32_t eaD = ctx->a[1]; ctx->a[1] += 2; uint32_t v = 0x5u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00A55E movew #5,%a1@+ */
	{ uint32_t eaD = ctx->a[1]; ctx->a[1] += 4; uint32_t v = 0x0u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 20);  /* 00A562 movel #0,%a1@+ */
	{ uint32_t eaD = ctx->a[1]; ctx->a[1] += 4; uint32_t v = 0x0u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 20);  /* 00A568 movel #0,%a1@+ */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[5] & 0xFFFFu), 0x30u, 16, 1); set_w(&ctx->d[5], v); } hle_insn(ctx, 8);  /* 00A56E addiw #48,%d5 */
	{ uint32_t v = 0xC632u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00A572 movew #-14798,%d3 */
	{ ctx->d[4] = 0x00000020u; cc_logic32(ctx, ctx->d[4]); } hle_insn(ctx, 4);  /* 00A576 moveq #32,%d4 */
	{ uint16_t v = (uint16_t)ctx->d[0] - 1; set_w(&ctx->d[0], v); if (v != 0xFFFF) { hle_insn(ctx, 10); goto L_00A542; } hle_insn(ctx, 14); }  /* 00A578 dbf %d0,a542 */
	hle_jump(ctx, 0x00A57C); return;  /* 00A57C jsr %pc@(a5b2) */
L_00A580:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0xDCE4u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 00A580 movel #56548,%fp@(32) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(28); uint32_t v = 0x597Au; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 00A588 movel #22906,%fp@(28) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = 0x14u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00A590 movew #20,%fp@(36) */
	hle_jump(ctx, 0x00A596); return;  /* 00A596 jsr %pc@(a5b2) */
L_00A59A:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0xDCDCu; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 00A59A movel #56540,%fp@(32) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(28); uint32_t v = 0x597Au; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 00A5A2 movel #22906,%fp@(28) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = 0x78u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00A5AA movew #120,%fp@(36) */
	hle_jump(ctx, 0x00A5B0); return;  /* 00A5B0 rts */
}

HLE_RANGE_PURE(0x00A52C, Auto_00A52C, 200, 0x00A544) { pc_run_00A52C(ctx, 0x00A52C); }
HLE_RANGE_PURE(0x00A548, Auto_00A548, 200, 0x00A544, 0x00A57C) { pc_run_00A52C(ctx, 0x00A548); }
HLE_RANGE_PURE(0x00A580, Auto_00A580, 200, 0x00A596) { pc_run_00A52C(ctx, 0x00A580); }
HLE_RANGE_PURE(0x00A59A, Auto_00A59A, 200, 0x00A5B0) { pc_run_00A52C(ctx, 0x00A59A); }
static void pc_run_00A5B2(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00A5B6: goto L_00A5B6;
	default: __builtin_trap();
	}
L_00A5B2:
	hle_jump(ctx, 0x00A5B2); return;  /* 00A5B2 jsr %pc@(930e) */
L_00A5B6:
	{ uint32_t v = 0x22u; set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00A5B6 movew #34,%d2 */
	{ unsigned b = 15; uint32_t m = 1u << b; cc_set(ctx, CC_Z, (ctx->d[2] & m) ? 0 : CC_Z); ctx->d[2] |= m; hle_insn(ctx, 10 + ((b > 15) ? 2 : 0)); }  /* 00A5BA bset #15,%d2 */
	{ uint32_t eaD = ctx->a[6]; uint32_t v = (ctx->d[2] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00A5BE movew %d2,%fp@ */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = (ctx->d[4] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00A5C0 movew %d4,%fp@(2) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(4); uint32_t v = (ctx->d[5] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00A5C4 movew %d5,%fp@(4) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(6); uint32_t v = 0x16u; st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 16);  /* 00A5C8 moveb #22,%fp@(6) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(7); uint32_t v = 0xAu; st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 16);  /* 00A5CE moveb #10,%fp@(7) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(28); uint32_t v = 0x6424u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 00A5D4 movel #25636,%fp@(28) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0xDBF6u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 00A5DC movel #56310,%fp@(32) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(8); uint32_t v = 0x78u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00A5E4 movew #120,%fp@(8) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(10); uint32_t v = (ctx->d[3] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00A5EA movew %d3,%fp@(10) */
	{ uint32_t eaD = 0xFFFF80DAu; uint32_t v = alu(ctx, OP_ADD, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 20);  /* 00A5EE addqw #1,ffff80da */
	hle_jump(ctx, 0x00A5F4); return;  /* 00A5F4 rts */
}

HLE_RANGE_PURE(0x00A5B6, Auto_00A5B6, 200, 0x00A5F4) { pc_run_00A5B2(ctx, 0x00A5B6); }
static void pc_run_00A764(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00A764: goto L_00A764;
	case 0x00A774: goto L_00A774;
	case 0x00A788: goto L_00A788;
	case 0x00A7A2: goto L_00A7A2;
	case 0x00A7BC: goto L_00A7BC;
	default: __builtin_trap();
	}
L_00A764:
	{ uint32_t eaD = 0xFFFFAF84u; uint32_t v = 0x4u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 20);  /* 00A764 movew #4,ffffaf84 */
	{ uint32_t eaS = 0x0000A7D6u; ctx->a[1] = eaS; } hle_insn(ctx, 8);  /* 00A76C lea %pc@(a7d6),%a1 */
	hle_jump(ctx, 0x00A770); return;  /* 00A770 jsr %pc@(92dc) */
L_00A774:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(10); uint32_t v = 0xC374u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00A774 movew #-15500,%fp@(10) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = 0x140u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00A77A movew #320,%fp@(36) */
	{ uint32_t eaS = 0x0000A7D6u; ctx->a[1] = eaS; } hle_insn(ctx, 8);  /* 00A780 lea %pc@(a7d6),%a1 */
	hle_jump(ctx, 0x00A784); return;  /* 00A784 jsr %pc@(92dc) */
L_00A788:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(10); uint32_t v = 0xC774u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00A788 movew #-14476,%fp@(10) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = 0xF0u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00A78E movew #240,%fp@(36) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(4); uint32_t v = 0x80u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00A794 movew #128,%fp@(4) */
	{ uint32_t eaS = 0x0000A7D6u; ctx->a[1] = eaS; } hle_insn(ctx, 8);  /* 00A79A lea %pc@(a7d6),%a1 */
	hle_jump(ctx, 0x00A79E); return;  /* 00A79E jsr %pc@(92dc) */
L_00A7A2:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(10); uint32_t v = 0xC334u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00A7A2 movew #-15564,%fp@(10) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = 0xA0u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00A7A8 movew #160,%fp@(36) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = 0x120u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00A7AE movew #288,%fp@(2) */
	{ uint32_t eaS = 0x0000A7D6u; ctx->a[1] = eaS; } hle_insn(ctx, 8);  /* 00A7B4 lea %pc@(a7d6),%a1 */
	hle_jump(ctx, 0x00A7B8); return;  /* 00A7B8 jsr %pc@(92dc) */
L_00A7BC:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(10); uint32_t v = 0xC734u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00A7BC movew #-14540,%fp@(10) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = 0x50u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00A7C2 movew #80,%fp@(36) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = 0x120u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00A7C8 movew #288,%fp@(2) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(4); uint32_t v = 0x80u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00A7CE movew #128,%fp@(4) */
	hle_jump(ctx, 0x00A7D4); return;  /* 00A7D4 rts */
}

HLE_RANGE_PURE(0x00A764, Auto_00A764, 200, 0x00A770) { pc_run_00A764(ctx, 0x00A764); }
HLE_RANGE_PURE(0x00A774, Auto_00A774, 200, 0x00A784) { pc_run_00A764(ctx, 0x00A774); }
HLE_RANGE_PURE(0x00A788, Auto_00A788, 200, 0x00A79E) { pc_run_00A764(ctx, 0x00A788); }
HLE_RANGE_PURE(0x00A7A2, Auto_00A7A2, 200, 0x00A7B8) { pc_run_00A764(ctx, 0x00A7A2); }
HLE_RANGE_PURE(0x00A7BC, Auto_00A7BC, 200, 0x00A7D4) { pc_run_00A764(ctx, 0x00A7BC); }
static void pc_run_00AC06(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00AC06: goto L_00AC06;
	case 0x00AC0E: goto L_00AC0E;
	default: __builtin_trap();
	}
L_00AC06:
	{ uint32_t eaS = 0x0000AC58u; ctx->a[1] = eaS; } hle_insn(ctx, 8);  /* 00AC06 lea %pc@(ac58),%a1 */
	hle_jump(ctx, 0x00AC0A); return;  /* 00AC0A jsr %pc@(92dc) */
L_00AC0E:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00AC0E movew %d0,%fp@(2) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(11); st8(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 16);  /* 00AC12 clrb %fp@(11) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(10); uint32_t v = 0x90u; st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 16);  /* 00AC16 moveb #-112,%fp@(10) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(16); uint32_t v = 0xCu; st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 16);  /* 00AC1C moveb #12,%fp@(16) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(14); uint32_t v = 0x1u; st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 16);  /* 00AC22 moveb #1,%fp@(14) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(15); uint32_t v = 0x1u; st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 16);  /* 00AC28 moveb #1,%fp@(15) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(12); uint32_t v = 0x0u; st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 16);  /* 00AC2E moveb #0,%fp@(12) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(13); st8(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 16);  /* 00AC34 clrb %fp@(13) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(18); uint32_t v = 0xFFu; st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 16);  /* 00AC38 moveb #-1,%fp@(18) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(20); st16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 16);  /* 00AC3E clrw %fp@(20) */
	{ uint32_t eaS = 0xFFFF9EE0u; ctx->a[1] = eaS; } hle_insn(ctx, 12);  /* 00AC42 lea ffff9ee0,%a1 */
	{ ctx->d[0] = 0x0000001Fu; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 00AC48 moveq #31,%d0 */
L_00AC4A:
	{ uint32_t eaD = ctx->a[1]; ctx->a[1] += 4; st32(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 20);  /* 00AC4A clrl %a1@+ */
	{ uint16_t v = (uint16_t)ctx->d[0] - 1; set_w(&ctx->d[0], v); if (v != 0xFFFF) { hle_insn(ctx, 10); goto L_00AC4A; } hle_insn(ctx, 14); }  /* 00AC4C dbf %d0,ac4a */
	{ uint32_t eaD = 0xFFFF9E14u; uint32_t v = alu(ctx, OP_ADD, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 20);  /* 00AC50 addqw #1,ffff9e14 */
	hle_jump(ctx, 0x00AC56); return;  /* 00AC56 rts */
}

HLE_RANGE_PURE(0x00AC06, Auto_00AC06, 200, 0x00AC0A) { pc_run_00AC06(ctx, 0x00AC06); }
HLE_RANGE_PURE(0x00AC0E, Auto_00AC0E, 200, 0x00AC56) { pc_run_00AC06(ctx, 0x00AC0E); }
static void pc_run_00AD6E(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00AD72: goto L_00AD72;
	default: __builtin_trap();
	}
L_00AD6E:
	hle_jump(ctx, 0x00AD6E); return;  /* 00AD6E jsr %pc@(ad9c) */
L_00AD72:
	{ uint32_t eaD = ctx->a[6];  unsigned b = 6; uint32_t m = 1u << b; uint8_t v = ld8(eaD); cc_set(ctx, CC_Z, (v & m) ? 0 : CC_Z); st8(eaD, v | m); } hle_insn(ctx, 16);  /* 00AD72 bset #14,%fp@ */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0x1085Cu; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 00AD76 movel #67676,%fp@(32) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(10); uint32_t v = 0xCu; st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 16);  /* 00AD7E moveb #12,%fp@(10) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(4); uint32_t v = 0x84u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00AD84 movew #132,%fp@(4) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(6); uint32_t v = 0x0u; st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 16);  /* 00AD8A moveb #0,%fp@(6) */
	{ uint32_t eaD = ctx->a[6];  unsigned b = 7; uint32_t m = 1u << b; uint8_t v = ld8(eaD); cc_set(ctx, CC_Z, (v & m) ? 0 : CC_Z); st8(eaD, v & ~m); } hle_insn(ctx, 16);  /* 00AD90 bclr #15,%fp@ */
	{ uint32_t eaD = 0xFFFF9ED2u; uint32_t v = (ctx->a[6] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 20);  /* 00AD94 movel %fp,ffff9ed2 */
	hle_jump(ctx, 0x00AD9A); return;  /* 00AD9A rts */
}

HLE_RANGE_PURE(0x00AD72, Auto_00AD72, 200, 0x00AD9A) { pc_run_00AD6E(ctx, 0x00AD72); }
static void pc_run_00AF72(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00AF72: goto L_00AF72;
	case 0x00AF94: goto L_00AF94;
	case 0x00AF9E: goto L_00AF9E;
	case 0x00AFA6: goto L_00AFA6;
	default: __builtin_trap();
	}
L_00AF72:
	{ ctx->a[7] -= 4; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->a[6] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 00AF72 movel %fp,%sp@- */
	{ uint32_t v = 0xE7D0u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00AF74 movew #-6192,%d1 */
	{ uint32_t eaS = 0x000A10CAu; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 00AF78 lea a10ca,%a0 */
	{ ctx->d[2] = 0x00000018u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 00AF7E moveq #24,%d2 */
	{ ctx->d[3] = 0x00000011u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 00AF80 moveq #17,%d3 */
	{ uint32_t v = 0xA000u; ctx->d[6] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 00AF82 movel #40960,%d6 */
	{ ctx->a[7] -= 2; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->d[7] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00AF88 movew %d7,%sp@- */
	{ uint32_t eaS = 0xFFFF9E74u; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00AF8A movew ffff9e74,%d7 */
	hle_jump(ctx, 0x00AF90); return;  /* 00AF90 jsr %pc@(12234) */
L_00AF94:
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00AF94 movew %sp@+,%d7 */
	{ uint32_t v = 0x0u; set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00AF96 movew #0,%d2 */
	hle_jump(ctx, 0x00AF9A); return;  /* 00AF9A jsr %pc@(9ef8) */
L_00AF9E:
	{ uint32_t eaS = 0x0000AFC2u; ctx->a[1] = eaS; } hle_insn(ctx, 8);  /* 00AF9E lea %pc@(afc2),%a1 */
	hle_jump(ctx, 0x00AFA2); return;  /* 00AFA2 jsr %pc@(92dc) */
L_00AFA6:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = 0xC8u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00AFA6 movew #200,%fp@(36) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(20); uint32_t v = 0x1u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00AFAC movew #1,%fp@(20) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(26); uint32_t v = 0x1u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00AFB2 movew #1,%fp@(26) */
	{ uint32_t eaD = 0xFFFF9ECEu; uint32_t v = (ctx->a[6] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 20);  /* 00AFB8 movel %fp,ffff9ece */
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 4; ctx->a[6] = ld32(eaS); } hle_insn(ctx, 12);  /* 00AFBE moveal %sp@+,%fp */
	hle_jump(ctx, 0x00AFC0); return;  /* 00AFC0 rts */
}

HLE_RANGE_PURE(0x00AF72, Auto_00AF72, 200, HLE_EXIT_SP(0x00AF90, -6)) { pc_run_00AF72(ctx, 0x00AF72); }
HLE_RANGE_PURE(0x00AF94, Auto_00AF94, 200, HLE_EXIT_SP(0x00AF9A, 2)) { pc_run_00AF72(ctx, 0x00AF94); }
HLE_RANGE_PURE(0x00AF9E, Auto_00AF9E, 200, 0x00AFA2) { pc_run_00AF72(ctx, 0x00AF9E); }
HLE_RANGE_PURE(0x00AFA6, Auto_00AFA6, 200, HLE_EXIT_SP(0x00AFC0, 4)) { pc_run_00AF72(ctx, 0x00AFA6); }
static void pc_run_00B08E(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00B08E: goto L_00B08E;
	default: __builtin_trap();
	}
L_00B08E:
	{ ctx->a[7] -= 4; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->a[0] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 00B08E movel %a0,%sp@- */
	{ uint32_t eaS = 0xFFFF9ECEu; ctx->a[6] = ld32(eaS); } hle_insn(ctx, 20);  /* 00B090 moveal ffff9ece,%fp */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = 0x0u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00B096 movew #0,%fp@(36) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0x145A0u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 00B09C movel #83360,%fp@(32) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = 0x280u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00B0A4 movew #640,%fp@(2) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(4); st16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 16);  /* 00B0AA clrw %fp@(4) */
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 4; ctx->a[0] = ld32(eaS); } hle_insn(ctx, 12);  /* 00B0AE moveal %sp@+,%a0 */
	hle_jump(ctx, 0x00B0B0); return;  /* 00B0B0 rts */
}

HLE_RANGE_PURE(0x00B08E, Auto_00B08E, 200, 0x00B0B0) { pc_run_00B08E(ctx, 0x00B08E); }
static void pc_run_00B128(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00B128: goto L_00B128;
	case 0x00B130: goto L_00B130;
	default: __builtin_trap();
	}
L_00B128:
	{ uint32_t eaS = 0x0000B15Eu; ctx->a[1] = eaS; } hle_insn(ctx, 8);  /* 00B128 lea %pc@(b15e),%a1 */
	hle_jump(ctx, 0x00B12C); return;  /* 00B12C jsr %pc@(92dc) */
L_00B130:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(10); uint32_t v = 0x20u; st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 16);  /* 00B130 moveb #32,%fp@(10) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(14); uint32_t v = 0x1u; st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 16);  /* 00B136 moveb #1,%fp@(14) */
	{ uint32_t eaD = ctx->a[6];  unsigned b = 6; uint32_t m = 1u << b; uint8_t v = ld8(eaD); cc_set(ctx, CC_Z, (v & m) ? 0 : CC_Z); st8(eaD, v | m); } hle_insn(ctx, 16);  /* 00B13C bset #14,%fp@ */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(11); st8(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 16);  /* 00B140 clrb %fp@(11) */
	{ uint32_t eaD = 0xFFFF9ED6u; uint32_t v = (ctx->a[6] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 20);  /* 00B144 movel %fp,ffff9ed6 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(40); uint32_t v = 0xAAu; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00B14A movew #170,%fp@(40) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = 0x8Cu; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00B150 movew #140,%fp@(2) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(26); uint32_t v = 0x5u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00B156 movew #5,%fp@(26) */
	hle_jump(ctx, 0x00B15C); return;  /* 00B15C rts */
}

HLE_RANGE_PURE(0x00B128, Auto_00B128, 200, 0x00B12C) { pc_run_00B128(ctx, 0x00B128); }
HLE_RANGE_PURE(0x00B130, Auto_00B130, 200, 0x00B15C) { pc_run_00B128(ctx, 0x00B130); }
static void pc_run_00B192(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00B192: goto L_00B192;
	case 0x00B19A: goto L_00B19A;
	default: __builtin_trap();
	}
L_00B192:
	{ uint32_t eaS = 0x0000B1BCu; ctx->a[1] = eaS; } hle_insn(ctx, 8);  /* 00B192 lea %pc@(b1bc),%a1 */
	hle_jump(ctx, 0x00B196); return;  /* 00B196 jsr %pc@(92dc) */
L_00B19A:
	{ uint32_t eaS = 0xFFFF9E94u; uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = ld32(eaS); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 32);  /* 00B19A movel ffff9e94,%fp@(2) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(4); alu(ctx, OP_SUB, ld16(eaD), 0x90u, 16, 0); } hle_insn(ctx, 16);  /* 00B1A2 cmpiw #144,%fp@(4) */
	if (COND_LT) { hle_insn(ctx, 10); goto L_00B1B0; } hle_insn(ctx, 8);  /* 00B1A8 blts b1b0 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(4); uint32_t v = 0x90u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00B1AA movew #144,%fp@(4) */
L_00B1B0:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(38); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00B1B0 movew %d0,%fp@(38) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = 0x80u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00B1B4 movew #128,%fp@(36) */
	hle_jump(ctx, 0x00B1BA); return;  /* 00B1BA rts */
}

HLE_RANGE_PURE(0x00B192, Auto_00B192, 200, 0x00B196) { pc_run_00B192(ctx, 0x00B192); }
HLE_RANGE_PURE(0x00B19A, Auto_00B19A, 200, 0x00B1BA) { pc_run_00B192(ctx, 0x00B19A); }
static void pc_run_00B3AE(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00B3AE: goto L_00B3AE;
	default: __builtin_trap();
	}
L_00B3AE:
	{ uint32_t eaS = ctx->a[6]; uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00B3AE movew %fp@,%d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_00B3B8; } hle_insn(ctx, 8);  /* 00B3B0 beqs b3b8 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(32); ctx->a[0] = ld32(eaS); } hle_insn(ctx, 16);  /* 00B3B2 moveal %fp@(32),%a0 */
	hle_jump(ctx, 0x00B3B6); return;  /* 00B3B6 jmp %a0@ */
L_00B3B8:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(44); ctx->a[6] = eaS; } hle_insn(ctx, 8);  /* 00B3B8 lea %fp@(44),%fp */
	hle_insn(ctx, 10); goto L_00B3AE;  /* 00B3BC bras b3ae */
}

HLE_RANGE_PURE(0x00B3AE, Auto_00B3AE, 200, 0x00B3B6) { pc_run_00B3AE(ctx, 0x00B3AE); }
static void pc_run_00B3C8(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00B3D0: goto L_00B3D0;
	case 0x00B3F0: goto L_00B3F0;
	case 0x00B3FC: goto L_00B3FC;
	case 0x00B408: goto L_00B408;
	case 0x00B412: goto L_00B412;
	case 0x00B41A: goto L_00B41A;
	case 0x00B420: goto L_00B420;
	default: __builtin_trap();
	}
L_00B3C8:
	hle_jump(ctx, 0x00B3C8); return;  /* 00B3C8 jsr %pc@(b4a6) */
L_00B3CC:
	hle_jump(ctx, 0x00B3CC); return;  /* 00B3CC jsr %pc@(12316) */
L_00B3D0:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = ld16(eaS); set_w(&ctx->d[5], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00B3D0 movew %fp@(36),%d5 */
	{ uint32_t v = ((ctx->d[5] & 0xFFFFu) & 0x38u) & 0xFFFFu; set_w(&ctx->d[5], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00B3D4 andiw #56,%d5 */
	{ uint32_t v = shf(ctx, 'r', (ctx->d[5] & 0xFFFFu), 3, 16); set_w(&ctx->d[5], v); } hle_insn(ctx, 12);  /* 00B3D8 lsrw #3,%d5 */
	{ uint32_t v = ((ctx->d[5] & 0xFFFFu) ^ 0x7u) & 0xFFFFu; set_w(&ctx->d[5], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00B3DA eoriw #7,%d5 */
	{ uint32_t eaD = ctx->a[6];  unsigned b = 6; uint32_t m = 1u << b; uint8_t v = ld8(eaD); cc_set(ctx, CC_Z, (v & m) ? 0 : CC_Z);  } hle_insn(ctx, 12);  /* 00B3DE btst #6,%fp@ */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_00B3FE; } hle_insn(ctx, 12);  /* 00B3E2 beqw b3fe */
	{ uint32_t v = (ctx->d[5] & 0xFFFFu); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 00B3E6 movew %d5,%d1 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0x6u, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 4);  /* 00B3E8 addqw #6,%d1 */
	hle_jump(ctx, 0x00B3EA); return;  /* 00B3EA jsr 13228 */
L_00B3F0:
	{ uint32_t v = (ctx->d[5] & 0xFFFFu); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 00B3F0 movew %d5,%d1 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0x64u, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 00B3F2 addiw #100,%d1 */
	hle_jump(ctx, 0x00B3F6); return;  /* 00B3F6 jsr 13228 */
L_00B3FC:
	hle_insn(ctx, 10); goto L_00B412;  /* 00B3FC bras b412 */
L_00B3FE:
	{ uint32_t v = (ctx->d[5] & 0xFFFFu); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 00B3FE movew %d5,%d1 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0xEu, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 00B400 addiw #14,%d1 */
	hle_jump(ctx, 0x00B404); return;  /* 00B404 bsrw 13228 */
L_00B408:
	{ uint32_t v = (ctx->d[5] & 0xFFFFu); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 00B408 movew %d5,%d1 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0x74u, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 00B40A addiw #116,%d1 */
	hle_jump(ctx, 0x00B40E); return;  /* 00B40E bsrw 13228 */
L_00B412:
	{ uint32_t v = 0x168u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00B412 movew #360,%d0 */
	hle_jump(ctx, 0x00B416); return;  /* 00B416 jsr %pc@(12342) */
L_00B41A:
	{ ctx->d[0] = 0xFFFFFFD8u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 00B41A moveq #-40,%d0 */
	hle_jump(ctx, 0x00B41C); return;  /* 00B41C jsr %pc@(12332) */
L_00B420:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = alu(ctx, OP_SUB, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 16);  /* 00B420 subqw #1,%fp@(36) */
	if (COND_NE) { hle_insn(ctx, 10); goto L_00B438; } hle_insn(ctx, 8);  /* 00B424 bnes b438 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0xB43Cu; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 00B426 movel #46140,%fp@(32) */
	{ uint32_t eaD = ctx->a[6];  unsigned b = 5; uint32_t m = 1u << b; uint8_t v = ld8(eaD); cc_set(ctx, CC_Z, (v & m) ? 0 : CC_Z); st8(eaD, v | m); } hle_insn(ctx, 16);  /* 00B42E bset #13,%fp@ */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = 0x64u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00B432 movew #100,%fp@(36) */
L_00B438:
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 00B438 braw b3be */
}

HLE_RANGE_PURE(0x00B3D0, Auto_00B3D0, 200, 0x00B3EA, 0x00B404) { pc_run_00B3C8(ctx, 0x00B3D0); }
HLE_RANGE_PURE(0x00B3F0, Auto_00B3F0, 200, 0x00B3F6) { pc_run_00B3C8(ctx, 0x00B3F0); }
HLE_RANGE_PURE(0x00B3FC, Auto_00B3FC, 200, 0x00B416) { pc_run_00B3C8(ctx, 0x00B3FC); }
HLE_RANGE_PURE(0x00B408, Auto_00B408, 200, 0x00B40E) { pc_run_00B3C8(ctx, 0x00B408); }
HLE_RANGE_PURE(0x00B412, Auto_00B412, 200, 0x00B416) { pc_run_00B3C8(ctx, 0x00B412); }
HLE_RANGE_PURE(0x00B41A, Auto_00B41A, 200, 0x00B41C) { pc_run_00B3C8(ctx, 0x00B41A); }
HLE_RANGE_PURE(0x00B420, Auto_00B420, 200, 0x00B3BE) { pc_run_00B3C8(ctx, 0x00B420); }
static void pc_run_00B43C(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00B440: goto L_00B440;
	case 0x00B446: goto L_00B446;
	case 0x00B450: goto L_00B450;
	case 0x00B474: goto L_00B474;
	case 0x00B47C: goto L_00B47C;
	case 0x00B484: goto L_00B484;
	default: __builtin_trap();
	}
L_00B43C:
	hle_jump(ctx, 0x00B43C); return;  /* 00B43C jsr %pc@(12316) */
L_00B440:
	{ ctx->d[0] = 0xFFFFFFD8u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 00B440 moveq #-40,%d0 */
	hle_jump(ctx, 0x00B442); return;  /* 00B442 jsr %pc@(12332) */
L_00B446:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = alu(ctx, OP_SUB, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 16);  /* 00B446 subqw #1,%fp@(36) */
	if (COND_NE) { hle_insn(ctx, 10); goto L_00B450; } hle_insn(ctx, 8);  /* 00B44A bnes b450 */
	hle_jump(ctx, 0x00B44C); return;  /* 00B44C jsr %pc@(b488) */
L_00B450:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00B450 movew %fp@(36),%d0 */
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x50u, 16, 0); } hle_insn(ctx, 8);  /* 00B454 cmpiw #80,%d0 */
	if (COND_GT) { hle_insn(ctx, 10); goto L_00B47C; } hle_insn(ctx, 8);  /* 00B458 bgts b47c */
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x14u, 16, 0); } hle_insn(ctx, 8);  /* 00B45A cmpiw #20,%d0 */
	if (COND_LT) { hle_insn(ctx, 10); goto L_00B47C; } hle_insn(ctx, 8);  /* 00B45E blts b47c */
	{ unsigned b = 2; uint32_t m = 1u << b; cc_set(ctx, CC_Z, (ctx->d[0] & m) ? 0 : CC_Z);  hle_insn(ctx, 10); }  /* 00B460 btst #2,%d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_00B47C; } hle_insn(ctx, 8);  /* 00B464 beqs b47c */
	{ uint32_t v = 0x60u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00B466 movew #96,%d0 */
	hle_jump(ctx, 0x00B46A); return;  /* 00B46A jsr 13d04 */
L_00B470:
	hle_jump(ctx, 0x00B470); return;  /* 00B470 bsrw 9054 */
L_00B474:
	{ uint32_t v = 0x169u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00B474 movew #361,%d1 */
	hle_jump(ctx, 0x00B478); return;  /* 00B478 jsr %pc@(13228) */
L_00B47C:
	{ uint32_t v = 0x168u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00B47C movew #360,%d1 */
	hle_jump(ctx, 0x00B480); return;  /* 00B480 jsr %pc@(13228) */
L_00B484:
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 00B484 braw b3be */
}

HLE_RANGE_PURE(0x00B440, Auto_00B440, 200, 0x00B442) { pc_run_00B43C(ctx, 0x00B440); }
HLE_RANGE_PURE(0x00B446, Auto_00B446, 200, 0x00B44C, 0x00B46A, 0x00B480) { pc_run_00B43C(ctx, 0x00B446); }
HLE_RANGE_PURE(0x00B450, Auto_00B450, 200, 0x00B46A, 0x00B480) { pc_run_00B43C(ctx, 0x00B450); }
HLE_RANGE_PURE(0x00B474, Auto_00B474, 200, 0x00B478) { pc_run_00B43C(ctx, 0x00B474); }
HLE_RANGE_PURE(0x00B47C, Auto_00B47C, 200, 0x00B480) { pc_run_00B43C(ctx, 0x00B47C); }
HLE_RANGE_PURE(0x00B484, Auto_00B484, 200, 0x00B3BE) { pc_run_00B43C(ctx, 0x00B484); }
static void pc_run_00B488(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00B488: goto L_00B488;
	case 0x00B498: goto L_00B498;
	default: __builtin_trap();
	}
L_00B488:
	{ uint32_t eaD = ctx->a[6];  unsigned b = 5; uint32_t m = 1u << b; uint8_t v = ld8(eaD); cc_set(ctx, CC_Z, (v & m) ? 0 : CC_Z); st8(eaD, v & ~m); } hle_insn(ctx, 16);  /* 00B488 bclr #13,%fp@ */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0xB3C8u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 00B48C movel #46024,%fp@(32) */
	hle_jump(ctx, 0x00B494); return;  /* 00B494 jsr %pc@(881c) */
L_00B498:
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0x3Fu) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00B498 andiw #63,%d0 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), 0x40u, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 8);  /* 00B49C addiw #64,%d0 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00B4A0 movew %d0,%fp@(36) */
	hle_jump(ctx, 0x00B4A4); return;  /* 00B4A4 rts */
}

HLE_RANGE_PURE(0x00B488, Auto_00B488, 200, 0x00B494) { pc_run_00B488(ctx, 0x00B488); }
HLE_RANGE_PURE(0x00B498, Auto_00B498, 200, 0x00B4A4) { pc_run_00B488(ctx, 0x00B498); }
static void pc_run_00B5C2(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00B5C2: goto L_00B5C2;
	default: __builtin_trap();
	}
L_00B5C2:
	{ uint32_t eaD = ctx->a[6];  unsigned b = 4; uint32_t m = 1u << b; uint8_t v = ld8(eaD); cc_set(ctx, CC_Z, (v & m) ? 0 : CC_Z);  } hle_insn(ctx, 12);  /* 00B5C2 btst #4,%fp@ */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_00B5DA; } hle_insn(ctx, 12);  /* 00B5C6 beqw b5da */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0xB65Au; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 00B5CA movel #46682,%fp@(32) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = 0x20u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00B5D2 movew #32,%fp@(36) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B5E8); return;  /* 00B5D8 bras b5e8 */
L_00B5DA:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0xB5ECu; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 00B5DA movel #46572,%fp@(32) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = 0x64u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16); hle_jump(ctx, 0x00B5E8); return;  /* 00B5E2 movew #100,%fp@(36) */
}

HLE_RANGE_PURE(0x00B5C2, Auto_00B5C2, 200, 0x00B5E8) { pc_run_00B5C2(ctx, 0x00B5C2); }
static void pc_run_00B772(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00B772: goto L_00B772;
	case 0x00B77E: goto L_00B77E;
	default: __builtin_trap();
	}
L_00B772:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0xB6D0u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 00B772 movel #46800,%fp@(32) */
	hle_jump(ctx, 0x00B77A); return;  /* 00B77A jsr %pc@(881c) */
L_00B77E:
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0x3Fu) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00B77E andiw #63,%d0 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), 0x96u, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 8);  /* 00B782 addiw #150,%d0 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00B786 movew %d0,%fp@(36) */
	hle_jump(ctx, 0x00B78A); return;  /* 00B78A rts */
}

HLE_RANGE_PURE(0x00B772, Auto_00B772, 200, 0x00B77A) { pc_run_00B772(ctx, 0x00B772); }
HLE_RANGE_PURE(0x00B77E, Auto_00B77E, 200, 0x00B78A) { pc_run_00B772(ctx, 0x00B77E); }
static void pc_run_00B846(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00B846: goto L_00B846;
	case 0x00B852: goto L_00B852;
	default: __builtin_trap();
	}
L_00B846:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0xB796u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 00B846 movel #46998,%fp@(32) */
	hle_jump(ctx, 0x00B84E); return;  /* 00B84E jsr %pc@(881c) */
L_00B852:
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0x3Fu) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00B852 andiw #63,%d0 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), 0x80u, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 8);  /* 00B856 addiw #128,%d0 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00B85A movew %d0,%fp@(36) */
	hle_jump(ctx, 0x00B85E); return;  /* 00B85E rts */
}

HLE_RANGE_PURE(0x00B846, Auto_00B846, 200, 0x00B84E) { pc_run_00B846(ctx, 0x00B846); }
HLE_RANGE_PURE(0x00B852, Auto_00B852, 200, 0x00B85E) { pc_run_00B846(ctx, 0x00B852); }
static void pc_run_00B962(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00B962: goto L_00B962;
	case 0x00B980: goto L_00B980;
	default: __builtin_trap();
	}
L_00B962:
	{ ctx->a[7] -= 2; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->d[7] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00B962 movew %d7,%sp@- */
	{ uint32_t v = 0xC570u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00B964 movew #-14992,%d1 */
	{ uint32_t eaS = 0x000A0FC2u; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 00B968 lea a0fc2,%a0 */
	{ ctx->d[2] = 0x00000008u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 00B96E moveq #8,%d2 */
	{ ctx->d[3] = 0x00000003u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 00B970 moveq #3,%d3 */
	{ uint32_t v = 0x0u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00B972 movew #0,%d6 */
	{ uint32_t eaS = 0xFFFF9E34u; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00B976 movew ffff9e34,%d7 */
	hle_jump(ctx, 0x00B97C); return;  /* 00B97C jsr %pc@(12234) */
L_00B980:
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00B980 movew %sp@+,%d7 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0xB9BAu; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 00B982 movel #47546,%fp@(32) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 00B98A braw b3be */
}

HLE_RANGE_PURE(0x00B962, Auto_00B962, 200, HLE_EXIT_SP(0x00B97C, -2)) { pc_run_00B962(ctx, 0x00B962); }
HLE_RANGE_PURE(0x00B980, Auto_00B980, 200, HLE_EXIT_SP(0x00B3BE, 2)) { pc_run_00B962(ctx, 0x00B980); }
static void pc_run_00B9BA(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00B9BE: goto L_00B9BE;
	case 0x00B9EC: goto L_00B9EC;
	case 0x00B9F6: goto L_00B9F6;
	default: __builtin_trap();
	}
L_00B9BA:
	hle_jump(ctx, 0x00B9BA); return;  /* 00B9BA bsrw 9078 */
L_00B9BE:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00B9BE movew %fp@(36),%d0 */
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0x8u) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00B9C2 andiw #8,%d0 */
	{ uint32_t v = shf(ctx, 'r', (ctx->d[0] & 0xFFFFu), 1, 16); set_w(&ctx->d[0], v); } hle_insn(ctx, 8);  /* 00B9C6 lsrw #1,%d0 */
	{ uint32_t eaS = 0x0000BA5Au; ctx->a[1] = eaS; } hle_insn(ctx, 8);  /* 00B9C8 lea %pc@(ba5a),%a1 */
	{ uint32_t eaS = ctx->a[1] + (uint32_t)(int32_t)(0) + (uint32_t)(int32_t)(int16_t)ctx->d[0]; ctx->a[0] = ld32(eaS); } hle_insn(ctx, 18);  /* 00B9CC moveal %a1@(0,%d0:w),%a0 */
	{ uint32_t v = 0x4u; set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00B9D0 movew #4,%d2 */
	{ uint32_t v = 0x3u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00B9D4 movew #3,%d3 */
	{ uint32_t v = 0xC570u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00B9D8 movew #-14992,%d1 */
	{ uint32_t v = 0x0u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00B9DC movew #0,%d6 */
	{ ctx->a[7] -= 2; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->d[7] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00B9E0 movew %d7,%sp@- */
	{ uint32_t eaS = 0xFFFF9E34u; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00B9E2 movew ffff9e34,%d7 */
	hle_jump(ctx, 0x00B9E8); return;  /* 00B9E8 jsr %pc@(12234) */
L_00B9EC:
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00B9EC movew %sp@+,%d7 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = alu(ctx, OP_SUB, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 16);  /* 00B9EE subqw #1,%fp@(36) */
	hle_jump(ctx, 0x00B9F2); return;  /* 00B9F2 jsr %pc@(12316) */
L_00B9F6:
	{ uint32_t eaS = 0xFFFFAB9Eu; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 00B9F6 lea ffffab9e,%a0 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00B9FC movew %fp@(2),%d0 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), 0x1u, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 4);  /* 00BA00 addqw #1,%d0 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00BA02 movew %d0,%fp@(2) */
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x160u, 16, 0); } hle_insn(ctx, 8);  /* 00BA06 cmpiw #352,%d0 */
	if (COND_LT) { hle_insn(ctx, 10); goto L_00BA14; } hle_insn(ctx, 8);  /* 00BA0A blts ba14 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0xBA2Au; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 00BA0C movel #47658,%fp@(32) */
L_00BA14:
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), 0x20u, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 8);  /* 00BA14 addiw #32,%d0 */
	{ uint32_t eaD = ctx->a[0]; uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BA18 movew %d0,%a0@ */
	{ uint32_t eaD = ctx->a[0] + (uint32_t)(int32_t)(32); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00BA1A movew %d0,%a0@(32) */
	{ uint32_t eaD = ctx->a[0] + (uint32_t)(int32_t)(64); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00BA1E movew %d0,%a0@(64) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 00BA22 braw b3be */
}

HLE_RANGE_PURE(0x00B9BE, Auto_00B9BE, 200, HLE_EXIT_SP(0x00B9E8, -2)) { pc_run_00B9BA(ctx, 0x00B9BE); }
HLE_RANGE_PURE(0x00B9EC, Auto_00B9EC, 200, HLE_EXIT_SP(0x00B9F2, 2)) { pc_run_00B9BA(ctx, 0x00B9EC); }
HLE_RANGE_PURE(0x00B9F6, Auto_00B9F6, 200, 0x00B3BE) { pc_run_00B9BA(ctx, 0x00B9F6); }
static void pc_run_00BA62(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00BA62: goto L_00BA62;
	case 0x00BA80: goto L_00BA80;
	default: __builtin_trap();
	}
L_00BA62:
	{ ctx->a[7] -= 2; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->d[7] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BA62 movew %d7,%sp@- */
	{ uint32_t v = 0xC1D0u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BA64 movew #-15920,%d1 */
	{ uint32_t eaS = 0x000A0EB2u; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 00BA68 lea a0eb2,%a0 */
	{ ctx->d[2] = 0x00000010u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 00BA6E moveq #16,%d2 */
	{ ctx->d[3] = 0x00000006u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 00BA70 moveq #6,%d3 */
	{ uint32_t v = 0x0u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BA72 movew #0,%d6 */
	{ uint32_t eaS = 0xFFFF9E34u; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00BA76 movew ffff9e34,%d7 */
	hle_jump(ctx, 0x00BA7C); return;  /* 00BA7C jsr %pc@(1227a) */
L_00BA80:
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BA80 movew %sp@+,%d7 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0xBAD6u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 00BA82 movel #47830,%fp@(32) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 00BA8A braw b3be */
}

HLE_RANGE_PURE(0x00BA62, Auto_00BA62, 200, HLE_EXIT_SP(0x00BA7C, -2)) { pc_run_00BA62(ctx, 0x00BA62); }
HLE_RANGE_PURE(0x00BA80, Auto_00BA80, 200, HLE_EXIT_SP(0x00B3BE, 2)) { pc_run_00BA62(ctx, 0x00BA80); }
static void pc_run_00BA8E(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00BA8E: goto L_00BA8E;
	case 0x00BA94: goto L_00BA94;
	case 0x00BAA4: goto L_00BAA4;
	case 0x00BAAA: goto L_00BAAA;
	case 0x00BABA: goto L_00BABA;
	default: __builtin_trap();
	}
L_00BA8E:
	{ ctx->d[1] = 0x00000000u; cc_logic32(ctx, ctx->d[1]); } hle_insn(ctx, 4);  /* 00BA8E moveq #0,%d1 */
	hle_jump(ctx, 0x00BA90); return;  /* 00BA90 jsr %pc@(babc) */
L_00BA94:
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0x11Bu, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 00BA94 addiw #283,%d1 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[2] & 0xFFFFu), 0x8u, 16, 1); set_w(&ctx->d[2], v); } hle_insn(ctx, 8);  /* 00BA98 subiw #8,%d2 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[3] & 0xFFFFu), 0x10u, 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 8);  /* 00BA9C subiw #16,%d3 */
	hle_jump(ctx, 0x00BAA0); return;  /* 00BAA0 jsr %pc@(13240) */
L_00BAA4:
	{ ctx->d[1] = 0x00000003u; cc_logic32(ctx, ctx->d[1]); } hle_insn(ctx, 4);  /* 00BAA4 moveq #3,%d1 */
	hle_jump(ctx, 0x00BAA6); return;  /* 00BAA6 jsr %pc@(babc) */
L_00BAAA:
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0x11Bu, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 00BAAA addiw #283,%d1 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[2] & 0xFFFFu), 0x34u, 16, 1); set_w(&ctx->d[2], v); } hle_insn(ctx, 8);  /* 00BAAE addiw #52,%d2 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[3] & 0xFFFFu), 0x12u, 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 8);  /* 00BAB2 subiw #18,%d3 */
	hle_jump(ctx, 0x00BAB6); return;  /* 00BAB6 jsr %pc@(13240) */
L_00BABA:
	hle_insn(ctx, 10); hle_jump(ctx, 0x00BAD6); return;  /* 00BABA bras bad6 */
}

HLE_RANGE_PURE(0x00BA8E, Auto_00BA8E, 200, 0x00BA90) { pc_run_00BA8E(ctx, 0x00BA8E); }
HLE_RANGE_PURE(0x00BA94, Auto_00BA94, 200, 0x00BAA0) { pc_run_00BA8E(ctx, 0x00BA94); }
HLE_RANGE_PURE(0x00BAA4, Auto_00BAA4, 200, 0x00BAA6) { pc_run_00BA8E(ctx, 0x00BAA4); }
HLE_RANGE_PURE(0x00BAAA, Auto_00BAAA, 200, 0x00BAB6) { pc_run_00BA8E(ctx, 0x00BAAA); }
HLE_RANGE_PURE(0x00BABA, Auto_00BABA, 200, 0x00BAD6) { pc_run_00BA8E(ctx, 0x00BABA); }
static void pc_run_00BAD6(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00BADA: goto L_00BADA;
	case 0x00BB08: goto L_00BB08;
	case 0x00BB0E: goto L_00BB0E;
	default: __builtin_trap();
	}
L_00BAD6:
	hle_jump(ctx, 0x00BAD6); return;  /* 00BAD6 bsrw 9078 */
L_00BADA:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00BADA movew %fp@(36),%d0 */
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0x78u) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BADE andiw #120,%d0 */
	{ uint32_t v = shf(ctx, 'r', (ctx->d[0] & 0xFFFFu), 1, 16); set_w(&ctx->d[0], v); } hle_insn(ctx, 8);  /* 00BAE2 lsrw #1,%d0 */
	{ uint32_t eaS = 0x0000BC86u; ctx->a[1] = eaS; } hle_insn(ctx, 8);  /* 00BAE4 lea %pc@(bc86),%a1 */
	{ uint32_t eaS = ctx->a[1] + (uint32_t)(int32_t)(0) + (uint32_t)(int32_t)(int16_t)ctx->d[0]; ctx->a[0] = ld32(eaS); } hle_insn(ctx, 18);  /* 00BAE8 moveal %a1@(0,%d0:w),%a0 */
	{ uint32_t v = 0x3u; set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BAEC movew #3,%d2 */
	{ uint32_t v = 0x4u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BAF0 movew #4,%d3 */
	{ uint32_t v = 0xC2DEu; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BAF4 movew #-15650,%d1 */
	{ uint32_t v = 0x0u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BAF8 movew #0,%d6 */
	{ ctx->a[7] -= 2; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->d[7] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BAFC movew %d7,%sp@- */
	{ uint32_t eaS = 0xFFFF9E34u; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00BAFE movew ffff9e34,%d7 */
	hle_jump(ctx, 0x00BB04); return;  /* 00BB04 jsr %pc@(1227a) */
L_00BB08:
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BB08 movew %sp@+,%d7 */
	hle_jump(ctx, 0x00BB0A); return;  /* 00BB0A jsr %pc@(12316) */
L_00BB0E:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = alu(ctx, OP_SUB, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 16);  /* 00BB0E subqw #1,%fp@(36) */
	{ uint32_t eaS = 0xFFFFAABEu; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 00BB12 lea ffffaabe,%a0 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00BB18 movew %fp@(2),%d0 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x2u, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 4);  /* 00BB1C subqw #2,%d0 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00BB1E movew %d0,%fp@(2) */
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0xFFC0u, 16, 0); } hle_insn(ctx, 8);  /* 00BB22 cmpiw #-64,%d0 */
	if (COND_GT) { hle_insn(ctx, 10); goto L_00BB30; } hle_insn(ctx, 8);  /* 00BB26 bgts bb30 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0xBB52u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 00BB28 movel #47954,%fp@(32) */
L_00BB30:
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x180u, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 8);  /* 00BB30 subiw #384,%d0 */
	{ uint32_t eaD = ctx->a[0]; uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BB34 movew %d0,%a0@ */
	{ uint32_t eaD = ctx->a[0] + (uint32_t)(int32_t)(32); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00BB36 movew %d0,%a0@(32) */
	{ uint32_t eaD = ctx->a[0] + (uint32_t)(int32_t)(64); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00BB3A movew %d0,%a0@(64) */
	{ uint32_t eaD = ctx->a[0] + (uint32_t)(int32_t)(96); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00BB3E movew %d0,%a0@(96) */
	{ uint32_t eaD = ctx->a[0] + (uint32_t)(int32_t)(128); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00BB42 movew %d0,%a0@(128) */
	{ uint32_t eaD = ctx->a[0] + (uint32_t)(int32_t)(160); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00BB46 movew %d0,%a0@(160) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 00BB4A braw b3be */
}

HLE_RANGE_PURE(0x00BADA, Auto_00BADA, 200, HLE_EXIT_SP(0x00BB04, -2)) { pc_run_00BAD6(ctx, 0x00BADA); }
HLE_RANGE_PURE(0x00BB08, Auto_00BB08, 200, HLE_EXIT_SP(0x00BB0A, 2)) { pc_run_00BAD6(ctx, 0x00BB08); }
HLE_RANGE_PURE(0x00BB0E, Auto_00BB0E, 200, 0x00B3BE) { pc_run_00BAD6(ctx, 0x00BB0E); }
static void pc_run_00BB52(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00BB52: goto L_00BB52;
	case 0x00BB58: goto L_00BB58;
	case 0x00BB76: goto L_00BB76;
	default: __builtin_trap();
	}
L_00BB52:
	{ uint32_t eaD = ctx->a[6]; st16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 12);  /* 00BB52 clrw %fp@ */
	hle_jump(ctx, 0x00BB54); return;  /* 00BB54 jsr %pc@(998a) */
L_00BB58:
	{ ctx->a[7] -= 2; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->d[7] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BB58 movew %d7,%sp@- */
	{ uint32_t v = 0xC1D0u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BB5A movew #-15920,%d1 */
	{ uint32_t eaS = 0x00046C94u; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 00BB5E lea 46c94,%a0 */
	{ ctx->d[2] = 0x00000010u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 00BB64 moveq #16,%d2 */
	{ ctx->d[3] = 0x00000006u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 00BB66 moveq #6,%d3 */
	{ uint32_t v = 0x0u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BB68 movew #0,%d6 */
	{ uint32_t eaS = 0xFFFF9E38u; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00BB6C movew ffff9e38,%d7 */
	hle_jump(ctx, 0x00BB72); return;  /* 00BB72 jsr %pc@(12234) */
L_00BB76:
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BB76 movew %sp@+,%d7 */
	{ uint32_t eaD = 0xFFFF80DAu; uint32_t v = alu(ctx, OP_SUB, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 20);  /* 00BB78 subqw #1,ffff80da */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 00BB7E braw b3be */
}

HLE_RANGE_PURE(0x00BB52, Auto_00BB52, 200, 0x00BB54) { pc_run_00BB52(ctx, 0x00BB52); }
HLE_RANGE_PURE(0x00BB58, Auto_00BB58, 200, HLE_EXIT_SP(0x00BB72, -2)) { pc_run_00BB52(ctx, 0x00BB58); }
HLE_RANGE_PURE(0x00BB76, Auto_00BB76, 200, HLE_EXIT_SP(0x00B3BE, 2)) { pc_run_00BB52(ctx, 0x00BB76); }
static void pc_run_00BB82(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00BB82: goto L_00BB82;
	case 0x00BBA0: goto L_00BBA0;
	default: __builtin_trap();
	}
L_00BB82:
	{ ctx->a[7] -= 2; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->d[7] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BB82 movew %d7,%sp@- */
	{ uint32_t v = 0xC1E0u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BB84 movew #-15904,%d1 */
	{ uint32_t eaS = 0x000A0EB2u; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 00BB88 lea a0eb2,%a0 */
	{ ctx->d[2] = 0x00000010u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 00BB8E moveq #16,%d2 */
	{ ctx->d[3] = 0x00000006u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 00BB90 moveq #6,%d3 */
	{ uint32_t v = 0x0u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BB92 movew #0,%d6 */
	{ uint32_t eaS = 0xFFFF9E34u; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00BB96 movew ffff9e34,%d7 */
	hle_jump(ctx, 0x00BB9C); return;  /* 00BB9C jsr %pc@(12234) */
L_00BBA0:
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BBA0 movew %sp@+,%d7 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0xBBDAu; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 00BBA2 movel #48090,%fp@(32) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 00BBAA braw b3be */
}

HLE_RANGE_PURE(0x00BB82, Auto_00BB82, 200, HLE_EXIT_SP(0x00BB9C, -2)) { pc_run_00BB82(ctx, 0x00BB82); }
HLE_RANGE_PURE(0x00BBA0, Auto_00BBA0, 200, HLE_EXIT_SP(0x00B3BE, 2)) { pc_run_00BB82(ctx, 0x00BBA0); }
static void pc_run_00BBDA(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00BBDE: goto L_00BBDE;
	case 0x00BC0C: goto L_00BC0C;
	case 0x00BC12: goto L_00BC12;
	default: __builtin_trap();
	}
L_00BBDA:
	hle_jump(ctx, 0x00BBDA); return;  /* 00BBDA bsrw 9078 */
L_00BBDE:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00BBDE movew %fp@(36),%d0 */
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0x78u) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BBE2 andiw #120,%d0 */
	{ uint32_t v = shf(ctx, 'r', (ctx->d[0] & 0xFFFFu), 1, 16); set_w(&ctx->d[0], v); } hle_insn(ctx, 8);  /* 00BBE6 lsrw #1,%d0 */
	{ uint32_t eaS = 0x0000BC86u; ctx->a[1] = eaS; } hle_insn(ctx, 8);  /* 00BBE8 lea %pc@(bc86),%a1 */
	{ uint32_t eaS = ctx->a[1] + (uint32_t)(int32_t)(0) + (uint32_t)(int32_t)(int16_t)ctx->d[0]; ctx->a[0] = ld32(eaS); } hle_insn(ctx, 18);  /* 00BBEC moveal %a1@(0,%d0:w),%a0 */
	{ uint32_t v = 0x3u; set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BBF0 movew #3,%d2 */
	{ uint32_t v = 0x4u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BBF4 movew #4,%d3 */
	{ uint32_t v = 0xC2ECu; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BBF8 movew #-15636,%d1 */
	{ uint32_t v = 0x0u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BBFC movew #0,%d6 */
	{ ctx->a[7] -= 2; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->d[7] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BC00 movew %d7,%sp@- */
	{ uint32_t eaS = 0xFFFF9E34u; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00BC02 movew ffff9e34,%d7 */
	hle_jump(ctx, 0x00BC08); return;  /* 00BC08 jsr %pc@(12234) */
L_00BC0C:
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BC0C movew %sp@+,%d7 */
	hle_jump(ctx, 0x00BC0E); return;  /* 00BC0E jsr %pc@(12316) */
L_00BC12:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = alu(ctx, OP_SUB, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 16);  /* 00BC12 subqw #1,%fp@(36) */
	{ uint32_t eaS = 0xFFFFAABEu; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 00BC16 lea ffffaabe,%a0 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00BC1C movew %fp@(2),%d0 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), 0x2u, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 4);  /* 00BC20 addqw #2,%d0 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00BC22 movew %d0,%fp@(2) */
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x180u, 16, 0); } hle_insn(ctx, 8);  /* 00BC26 cmpiw #384,%d0 */
	if (COND_LT) { hle_insn(ctx, 10); goto L_00BC34; } hle_insn(ctx, 8);  /* 00BC2A blts bc34 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0xBC56u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 00BC2C movel #48214,%fp@(32) */
L_00BC34:
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), 0x40u, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 8);  /* 00BC34 addiw #64,%d0 */
	{ uint32_t eaD = ctx->a[0]; uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BC38 movew %d0,%a0@ */
	{ uint32_t eaD = ctx->a[0] + (uint32_t)(int32_t)(32); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00BC3A movew %d0,%a0@(32) */
	{ uint32_t eaD = ctx->a[0] + (uint32_t)(int32_t)(64); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00BC3E movew %d0,%a0@(64) */
	{ uint32_t eaD = ctx->a[0] + (uint32_t)(int32_t)(96); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00BC42 movew %d0,%a0@(96) */
	{ uint32_t eaD = ctx->a[0] + (uint32_t)(int32_t)(128); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00BC46 movew %d0,%a0@(128) */
	{ uint32_t eaD = ctx->a[0] + (uint32_t)(int32_t)(160); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00BC4A movew %d0,%a0@(160) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 00BC4E braw b3be */
}

HLE_RANGE_PURE(0x00BBDE, Auto_00BBDE, 200, HLE_EXIT_SP(0x00BC08, -2)) { pc_run_00BBDA(ctx, 0x00BBDE); }
HLE_RANGE_PURE(0x00BC0C, Auto_00BC0C, 200, HLE_EXIT_SP(0x00BC0E, 2)) { pc_run_00BBDA(ctx, 0x00BC0C); }
HLE_RANGE_PURE(0x00BC12, Auto_00BC12, 200, 0x00B3BE) { pc_run_00BBDA(ctx, 0x00BC12); }
static void pc_run_00BCC6(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00BCC6: goto L_00BCC6;
	case 0x00BCDC: goto L_00BCDC;
	default: __builtin_trap();
	}
L_00BCC6:
	{ ctx->d[5] = 0x00000005u; cc_logic32(ctx, ctx->d[5]); } hle_insn(ctx, 4);  /* 00BCC6 moveq #5,%d5 */
L_00BCC8:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00BCC8 movew %fp@(2),%d0 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(4); uint32_t v = ld16(eaS); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00BCCC movew %fp@(4),%d1 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[1] & 0xFFFFu), 0x10u, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 00BCD0 subiw #16,%d1 */
	{ uint32_t eaS = 0x00012856u; ctx->a[2] = eaS; } hle_insn(ctx, 8);  /* 00BCD4 lea %pc@(12856),%a2 */
	hle_jump(ctx, 0x00BCD8); return;  /* 00BCD8 bsrw 1277c */
L_00BCDC:
	{ uint16_t v = (uint16_t)ctx->d[5] - 1; set_w(&ctx->d[5], v); if (v != 0xFFFF) { hle_insn(ctx, 10); goto L_00BCC8; } hle_insn(ctx, 14); }  /* 00BCDC dbf %d5,bcc8 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(2); ctx->d[0] = (uint32_t)(int32_t)(int16_t)ld16(eaS + 0); ctx->d[1] = (uint32_t)(int32_t)(int16_t)ld16(eaS + 2); } hle_insn(ctx, 24);  /* 00BCE0 movemw %fp@(2),%d0-%d1 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[1] & 0xFFFFu), 0xCu, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 00BCE6 subiw #12,%d1 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(6); uint32_t v = ld16(eaS); set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00BCEA movew %fp@(6),%d2 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[2] & 0xFFu), 0x1u, 8, 1); set_b(&ctx->d[2], v); } hle_insn(ctx, 4);  /* 00BCEE subqb #1,%d2 */
	hle_jump(ctx, 0x00BCF0); return;  /* 00BCF0 bsrw 1254e */
L_00BCF4:
	hle_jump(ctx, 0x00BCF4); return;  /* 00BCF4 rts */
}

HLE_RANGE_PURE(0x00BCC6, Auto_00BCC6, 200, 0x00BCD8) { pc_run_00BCC6(ctx, 0x00BCC6); }
HLE_RANGE_PURE(0x00BCDC, Auto_00BCDC, 200, 0x00BCD8, 0x00BCF0) { pc_run_00BCC6(ctx, 0x00BCDC); }
static void pc_run_00BCF6(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00BCF6: goto L_00BCF6;
	case 0x00BD14: goto L_00BD14;
	default: __builtin_trap();
	}
L_00BCF6:
	{ ctx->a[7] -= 2; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->d[7] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BCF6 movew %d7,%sp@- */
	{ uint32_t v = 0xC150u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BCF8 movew #-16048,%d1 */
	{ uint32_t eaS = 0x000BC66Au; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 00BCFC lea bc66a,%a0 */
	{ ctx->d[2] = 0x00000018u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 00BD02 moveq #24,%d2 */
	{ ctx->d[3] = 0x00000008u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 00BD04 moveq #8,%d3 */
	{ uint32_t v = 0x0u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BD06 movew #0,%d6 */
	{ uint32_t eaS = 0xFFFF9E34u; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00BD0A movew ffff9e34,%d7 */
	hle_jump(ctx, 0x00BD10); return;  /* 00BD10 jsr %pc@(12234) */
L_00BD14:
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BD14 movew %sp@+,%d7 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0xBD54u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 00BD16 movel #48468,%fp@(32) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 00BD1E braw b3be */
}

HLE_RANGE_PURE(0x00BCF6, Auto_00BCF6, 200, HLE_EXIT_SP(0x00BD10, -2)) { pc_run_00BCF6(ctx, 0x00BCF6); }
HLE_RANGE_PURE(0x00BD14, Auto_00BD14, 200, HLE_EXIT_SP(0x00B3BE, 2)) { pc_run_00BCF6(ctx, 0x00BD14); }
static void pc_run_00BE42(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00BE42: goto L_00BE42;
	case 0x00BE6A: goto L_00BE6A;
	case 0x00BE8A: goto L_00BE8A;
	default: __builtin_trap();
	}
L_00BE42:
	{ ctx->a[7] -= 2; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->d[7] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BE42 movew %d7,%sp@- */
	{ uint32_t eaS = 0xFFFF80BCu; ctx->a[5] = ld32(eaS); } hle_insn(ctx, 20);  /* 00BE44 moveal ffff80bc,%a5 */
	{ uint32_t v = 0xC100u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BE4A movew #-16128,%d1 */
	{ uint32_t eaS = 0x00046C44u; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 00BE4E lea 46c44,%a0 */
	{ ctx->d[2] = 0x00000020u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 00BE54 moveq #32,%d2 */
	{ ctx->d[3] = 0x0000000Bu; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 00BE56 moveq #11,%d3 */
	{ uint32_t v = 0x0u; ctx->d[6] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 00BE58 movel #0,%d6 */
	{ uint32_t eaS = 0xFFFF9E38u; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00BE5E movew ffff9e38,%d7 */
	hle_jump(ctx, 0x00BE64); return;  /* 00BE64 jsr 12234 */
L_00BE6A:
	{ uint32_t v = 0xC140u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BE6A movew #-16064,%d1 */
	{ uint32_t eaS = 0x00046C44u; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 00BE6E lea 46c44,%a0 */
	{ ctx->d[2] = 0x00000020u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 00BE74 moveq #32,%d2 */
	{ ctx->d[3] = 0x0000000Bu; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 00BE76 moveq #11,%d3 */
	{ uint32_t v = 0x0u; ctx->d[6] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 00BE78 movel #0,%d6 */
	{ uint32_t eaS = 0xFFFF9E38u; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00BE7E movew ffff9e38,%d7 */
	hle_jump(ctx, 0x00BE84); return;  /* 00BE84 jsr 12234 */
L_00BE8A:
	{ uint32_t eaD = 0xFFFF80BCu; uint32_t v = (ctx->a[5] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 20);  /* 00BE8A movel %a5,ffff80bc */
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BE90 movew %sp@+,%d7 */
	hle_jump(ctx, 0x00BE92); return;  /* 00BE92 rts */
}

HLE_RANGE_PURE(0x00BE42, Auto_00BE42, 200, HLE_EXIT_SP(0x00BE64, -2)) { pc_run_00BE42(ctx, 0x00BE42); }
HLE_RANGE_PURE(0x00BE6A, Auto_00BE6A, 200, 0x00BE84) { pc_run_00BE42(ctx, 0x00BE6A); }
HLE_RANGE_PURE(0x00BE8A, Auto_00BE8A, 200, HLE_EXIT_SP(0x00BE92, 2)) { pc_run_00BE42(ctx, 0x00BE8A); }
static void pc_run_00BF60(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00BF60: goto L_00BF60;
	case 0x00BF80: goto L_00BF80;
	default: __builtin_trap();
	}
L_00BF60:
	{ ctx->a[7] -= 2; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->d[7] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BF60 movew %d7,%sp@- */
	{ uint32_t v = 0xC150u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BF62 movew #-16048,%d1 */
	{ uint32_t eaS = 0x000BC66Au; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 00BF66 lea bc66a,%a0 */
	{ ctx->d[2] = 0x00000018u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 00BF6C moveq #24,%d2 */
	{ ctx->d[3] = 0x00000006u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 00BF6E moveq #6,%d3 */
	{ uint32_t v = 0x0u; ctx->d[6] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 00BF70 movel #0,%d6 */
	{ uint32_t eaS = 0xFFFF9E34u; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00BF76 movew ffff9e34,%d7 */
	hle_jump(ctx, 0x00BF7C); return;  /* 00BF7C jsr %pc@(1227a) */
L_00BF80:
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BF80 movew %sp@+,%d7 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0xBFC0u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 00BF82 movel #49088,%fp@(32) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 00BF8A braw b3be */
}

HLE_RANGE_PURE(0x00BF60, Auto_00BF60, 200, HLE_EXIT_SP(0x00BF7C, -2)) { pc_run_00BF60(ctx, 0x00BF60); }
HLE_RANGE_PURE(0x00BF80, Auto_00BF80, 200, HLE_EXIT_SP(0x00B3BE, 2)) { pc_run_00BF60(ctx, 0x00BF80); }
static void pc_run_00BFC0(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00BFC0: goto L_00BFC0;
	default: __builtin_trap();
	}
L_00BFC0:
	{ uint32_t eaD = 0xFFFF9E92u; uint32_t v = alu(ctx, OP_ADD, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 20);  /* 00BFC0 addqw #1,ffff9e92 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00BFC6 movew %fp@(36),%d0 */
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0x78u) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BFCA andiw #120,%d0 */
	{ uint32_t v = shf(ctx, 'r', (ctx->d[0] & 0xFFFFu), 1, 16); set_w(&ctx->d[0], v); } hle_insn(ctx, 8);  /* 00BFCE lsrw #1,%d0 */
	{ uint32_t eaS = 0x0000BDEEu; ctx->a[1] = eaS; } hle_insn(ctx, 8);  /* 00BFD0 lea %pc@(bdee),%a1 */
	{ uint32_t eaS = ctx->a[1] + (uint32_t)(int32_t)(0) + (uint32_t)(int32_t)(int16_t)ctx->d[0]; ctx->a[0] = ld32(eaS); } hle_insn(ctx, 18); hle_jump(ctx, 0x00BFD8); return;  /* 00BFD4 moveal %a1@(0,%d0:w),%a0 */
}

HLE_RANGE_PURE(0x00BFC0, Auto_00BFC0, 200, 0x00BFD8) { pc_run_00BFC0(ctx, 0x00BFC0); }
static void pc_run_00BFD8(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00BFD8: goto L_00BFD8;
	case 0x00BFF4: goto L_00BFF4;
	case 0x00C012: goto L_00C012;
	case 0x00C022: goto L_00C022;
	default: __builtin_trap();
	}
L_00BFD8:
	{ uint32_t v = 0x5u; set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BFD8 movew #5,%d2 */
	{ uint32_t v = 0x2u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BFDC movew #2,%d3 */
	{ uint32_t v = 0xC364u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BFE0 movew #-15516,%d1 */
	{ uint32_t v = 0x0u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BFE4 movew #0,%d6 */
	{ ctx->a[7] -= 2; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->d[7] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BFE8 movew %d7,%sp@- */
	{ uint32_t eaS = 0xFFFF9E34u; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00BFEA movew ffff9e34,%d7 */
	hle_jump(ctx, 0x00BFF0); return;  /* 00BFF0 jsr %pc@(1227a) */
L_00BFF4:
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BFF4 movew %sp@+,%d7 */
	{ uint32_t v = 0x6u; set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BFF6 movew #6,%d2 */
	{ uint32_t v = 0x2u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BFFA movew #2,%d3 */
	{ uint32_t v = 0xC462u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00BFFE movew #-15262,%d1 */
	{ uint32_t v = 0x0u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00C002 movew #0,%d6 */
	{ ctx->a[7] -= 2; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->d[7] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00C006 movew %d7,%sp@- */
	{ uint32_t eaS = 0xFFFF9E34u; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00C008 movew ffff9e34,%d7 */
	hle_jump(ctx, 0x00C00E); return;  /* 00C00E jsr %pc@(1227a) */
L_00C012:
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00C012 movew %sp@+,%d7 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = alu(ctx, OP_SUB, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 16);  /* 00C014 subqw #1,%fp@(36) */
	{ uint32_t eaS = 0xFFFFAA9Eu; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 00C018 lea ffffaa9e,%a0 */
	hle_jump(ctx, 0x00C01E); return;  /* 00C01E jsr %pc@(b4a6) */
L_00C022:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00C022 movew %fp@(2),%d0 */
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0xFFA0u, 16, 0); } hle_insn(ctx, 8);  /* 00C026 cmpiw #-96,%d0 */
	if (COND_GT) { hle_insn(ctx, 10); goto L_00C034; } hle_insn(ctx, 8);  /* 00C02A bgts c034 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0xBE2Eu; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 00C02C movel #48686,%fp@(32) */
L_00C034:
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x1A0u, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 8);  /* 00C034 subiw #416,%d0 */
	{ uint32_t eaD = ctx->a[0]; uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00C038 movew %d0,%a0@ */
	{ uint32_t eaD = ctx->a[0] + (uint32_t)(int32_t)(32); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00C03A movew %d0,%a0@(32) */
	{ uint32_t eaD = ctx->a[0] + (uint32_t)(int32_t)(64); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00C03E movew %d0,%a0@(64) */
	{ uint32_t eaD = ctx->a[0] + (uint32_t)(int32_t)(96); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00C042 movew %d0,%a0@(96) */
	{ uint32_t eaD = ctx->a[0] + (uint32_t)(int32_t)(128); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00C046 movew %d0,%a0@(128) */
	{ uint32_t eaD = ctx->a[0] + (uint32_t)(int32_t)(160); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00C04A movew %d0,%a0@(160) */
	{ uint32_t eaD = ctx->a[0] + (uint32_t)(int32_t)(192); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00C04E movew %d0,%a0@(192) */
	{ uint32_t eaD = ctx->a[0] + (uint32_t)(int32_t)(224); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00C052 movew %d0,%a0@(224) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 00C056 braw b3be */
}

HLE_RANGE_PURE(0x00BFD8, Auto_00BFD8, 200, HLE_EXIT_SP(0x00BFF0, -2)) { pc_run_00BFD8(ctx, 0x00BFD8); }
HLE_RANGE_PURE(0x00BFF4, Auto_00BFF4, 200, 0x00C00E) { pc_run_00BFD8(ctx, 0x00BFF4); }
HLE_RANGE_PURE(0x00C012, Auto_00C012, 200, HLE_EXIT_SP(0x00C01E, 2)) { pc_run_00BFD8(ctx, 0x00C012); }
HLE_RANGE_PURE(0x00C022, Auto_00C022, 200, 0x00B3BE) { pc_run_00BFD8(ctx, 0x00C022); }
static void pc_run_00C1E6(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00C1E6: goto L_00C1E6;
	case 0x00C206: goto L_00C206;
	default: __builtin_trap();
	}
L_00C1E6:
	{ uint32_t eaS = 0x00065264u; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 00C1E6 lea 65264,%a0 */
	{ ctx->d[2] = 0x00000012u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 00C1EC moveq #18,%d2 */
	{ ctx->d[3] = 0x00000008u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 00C1EE moveq #8,%d3 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(10); uint32_t v = ld16(eaS); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00C1F0 movew %fp@(10),%d1 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0xC0EEu, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 00C1F4 addiw #-16146,%d1 */
	{ ctx->d[6] = 0x00000000u; cc_logic32(ctx, ctx->d[6]); } hle_insn(ctx, 4);  /* 00C1F8 moveq #0,%d6 */
	{ ctx->a[7] -= 2; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->d[7] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00C1FA movew %d7,%sp@- */
	{ uint32_t eaS = 0xFFFF9E38u; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00C1FC movew ffff9e38,%d7 */
	hle_jump(ctx, 0x00C202); return;  /* 00C202 jsr %pc@(12234) */
L_00C206:
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00C206 movew %sp@+,%d7 */
	hle_jump(ctx, 0x00C208); return;  /* 00C208 rts */
}

HLE_RANGE_PURE(0x00C1E6, Auto_00C1E6, 200, HLE_EXIT_SP(0x00C202, -2)) { pc_run_00C1E6(ctx, 0x00C1E6); }
HLE_RANGE_PURE(0x00C206, Auto_00C206, 200, HLE_EXIT_SP(0x00C208, 2)) { pc_run_00C1E6(ctx, 0x00C206); }
static void pc_run_00C278(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00C278: goto L_00C278;
	case 0x00C286: goto L_00C286;
	case 0x00C290: goto L_00C290;
	case 0x00C2AE: goto L_00C2AE;
	default: __builtin_trap();
	}
L_00C278:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00C278 movew %d0,%fp@(36) */
	{ unsigned b = 0; uint32_t m = 1u << b; cc_set(ctx, CC_Z, (ctx->d[0] & m) ? 0 : CC_Z);  hle_insn(ctx, 10); }  /* 00C27C btst #0,%d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_00C2AE; } hle_insn(ctx, 8);  /* 00C280 beqs c2ae */
	hle_jump(ctx, 0x00C282); return;  /* 00C282 jsr %pc@(881c) */
L_00C286:
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0x1Fu) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00C286 andiw #31,%d0 */
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 00C28A movew %d0,%d1 */
	hle_jump(ctx, 0x00C28C); return;  /* 00C28C jsr %pc@(881c) */
L_00C290:
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0x1Fu) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00C290 andiw #31,%d0 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), ld16(eaS), 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 12);  /* 00C294 addw %fp@(2),%d0 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x10u, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 8);  /* 00C298 subiw #16,%d0 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(4); uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), ld16(eaS), 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 12);  /* 00C29C addw %fp@(4),%d1 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[1] & 0xFFFFu), 0x20u, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 00C2A0 subiw #32,%d1 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(6); uint32_t v = ld16(eaS); set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00C2A4 movew %fp@(6),%d2 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[2] & 0xFFu), 0x1u, 8, 1); set_b(&ctx->d[2], v); } hle_insn(ctx, 4);  /* 00C2A8 subqb #1,%d2 */
	hle_jump(ctx, 0x00C2AA); return;  /* 00C2AA jsr %pc@(1253c) */
L_00C2AE:
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 00C2AE braw b3be */
}

HLE_RANGE_PURE(0x00C278, Auto_00C278, 200, 0x00B3BE, 0x00C282) { pc_run_00C278(ctx, 0x00C278); }
HLE_RANGE_PURE(0x00C286, Auto_00C286, 200, 0x00C28C) { pc_run_00C278(ctx, 0x00C286); }
HLE_RANGE_PURE(0x00C290, Auto_00C290, 200, 0x00C2AA) { pc_run_00C278(ctx, 0x00C290); }
HLE_RANGE_PURE(0x00C2AE, Auto_00C2AE, 200, 0x00B3BE) { pc_run_00C278(ctx, 0x00C2AE); }
static void pc_run_00C4F4(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00C4F4: goto L_00C4F4;
	case 0x00C502: goto L_00C502;
	case 0x00C510: goto L_00C510;
	case 0x00C51A: goto L_00C51A;
	case 0x00C538: goto L_00C538;
	default: __builtin_trap();
	}
L_00C4F4:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00C4F4 movew %fp@(36),%d0 */
	{ unsigned b = 2; uint32_t m = 1u << b; cc_set(ctx, CC_Z, (ctx->d[0] & m) ? 0 : CC_Z);  hle_insn(ctx, 10); }  /* 00C4F8 btst #2,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_00C502; } hle_insn(ctx, 8);  /* 00C4FC bnes c502 */
	hle_jump(ctx, 0x00C4FE); return;  /* 00C4FE jsr %pc@(13228) */
L_00C502:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00C502 movew %fp@(36),%d0 */
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0x7u) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00C506 andiw #7,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_00C538; } hle_insn(ctx, 8);  /* 00C50A bnes c538 */
	hle_jump(ctx, 0x00C50C); return;  /* 00C50C jsr %pc@(881c) */
L_00C510:
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0x3Fu) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00C510 andiw #63,%d0 */
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 00C514 movew %d0,%d1 */
	hle_jump(ctx, 0x00C516); return;  /* 00C516 jsr %pc@(881c) */
L_00C51A:
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0x3Fu) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00C51A andiw #63,%d0 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), ld16(eaS), 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 12);  /* 00C51E addw %fp@(2),%d0 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x20u, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 8);  /* 00C522 subiw #32,%d0 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(4); uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), ld16(eaS), 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 12);  /* 00C526 addw %fp@(4),%d1 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[1] & 0xFFFFu), 0x20u, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 00C52A subiw #32,%d1 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(6); uint32_t v = ld16(eaS); set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00C52E movew %fp@(6),%d2 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[2] & 0xFFu), 0x1u, 8, 1); set_b(&ctx->d[2], v); } hle_insn(ctx, 4);  /* 00C532 subqb #1,%d2 */
	hle_jump(ctx, 0x00C534); return;  /* 00C534 jsr %pc@(12560) */
L_00C538:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = alu(ctx, OP_SUB, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 16);  /* 00C538 subqw #1,%fp@(36) */
	if (COND_NE) { hle_insn(ctx, 10); goto L_00C546; } hle_insn(ctx, 8);  /* 00C53C bnes c546 */
	{ uint32_t eaD = ctx->a[6]; st16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 12);  /* 00C53E clrw %fp@ */
	{ uint32_t eaD = 0xFFFF80DAu; uint32_t v = alu(ctx, OP_SUB, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 20);  /* 00C540 subqw #1,ffff80da */
L_00C546:
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 00C546 braw b3be */
}

HLE_RANGE_PURE(0x00C4F4, Auto_00C4F4, 200, 0x00B3BE, 0x00C4FE, 0x00C50C) { pc_run_00C4F4(ctx, 0x00C4F4); }
HLE_RANGE_PURE(0x00C502, Auto_00C502, 200, 0x00B3BE, 0x00C50C) { pc_run_00C4F4(ctx, 0x00C502); }
HLE_RANGE_PURE(0x00C510, Auto_00C510, 200, 0x00C516) { pc_run_00C4F4(ctx, 0x00C510); }
HLE_RANGE_PURE(0x00C51A, Auto_00C51A, 200, 0x00C534) { pc_run_00C4F4(ctx, 0x00C51A); }
HLE_RANGE_PURE(0x00C538, Auto_00C538, 200, 0x00B3BE) { pc_run_00C4F4(ctx, 0x00C538); }
static void pc_run_00C652(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00C652: goto L_00C652;
	case 0x00C65E: goto L_00C65E;
	default: __builtin_trap();
	}
L_00C652:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0xB586u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 00C652 movel #46470,%fp@(32) */
	hle_jump(ctx, 0x00C65A); return;  /* 00C65A jsr %pc@(881c) */
L_00C65E:
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0x3Fu) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00C65E andiw #63,%d0 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), 0x40u, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 8);  /* 00C662 addiw #64,%d0 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00C666 movew %d0,%fp@(36) */
	hle_jump(ctx, 0x00C66A); return;  /* 00C66A rts */
}

HLE_RANGE_PURE(0x00C652, Auto_00C652, 200, 0x00C65A) { pc_run_00C652(ctx, 0x00C652); }
HLE_RANGE_PURE(0x00C65E, Auto_00C65E, 200, 0x00C66A) { pc_run_00C652(ctx, 0x00C65E); }
static void pc_run_00C8A8(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00C8AC: goto L_00C8AC;
	case 0x00C8B6: goto L_00C8B6;
	default: __builtin_trap();
	}
L_00C8A8:
	hle_jump(ctx, 0x00C8A8); return;  /* 00C8A8 jsr %pc@(12316) */
L_00C8AC:
	{ ctx->d[0] = 0xFFFFFFF4u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 00C8AC moveq #-12,%d0 */
	hle_jump(ctx, 0x00C8AE); return;  /* 00C8AE jsr %pc@(12332) */
L_00C8B2:
	hle_jump(ctx, 0x00C8B2); return;  /* 00C8B2 bsrw 13228 */
L_00C8B6:
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 00C8B6 braw b3be */
}

HLE_RANGE_PURE(0x00C8AC, Auto_00C8AC, 200, 0x00C8AE) { pc_run_00C8A8(ctx, 0x00C8AC); }
HLE_RANGE_PURE(0x00C8B6, Auto_00C8B6, 200, 0x00B3BE) { pc_run_00C8A8(ctx, 0x00C8B6); }
static void pc_run_00C9C2(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00C9C2: goto L_00C9C2;
	case 0x00C9E4: goto L_00C9E4;
	default: __builtin_trap();
	}
L_00C9C2:
	{ ctx->a[7] -= 2; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->d[7] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00C9C2 movew %d7,%sp@- */
	{ uint32_t eaS = 0x0004A7E8u; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 00C9C4 lea 4a7e8,%a0 */
	{ uint32_t eaS = ctx->a[0] + (uint32_t)(int32_t)(0) + (uint32_t)(int32_t)(int16_t)ctx->d[1]; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 00C9CA lea %a0@(0,%d1:w),%a0 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(10); uint32_t v = ld16(eaS); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00C9CE movew %fp@(10),%d1 */
	{ ctx->d[2] = 0x00000006u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 00C9D2 moveq #6,%d2 */
	{ ctx->d[3] = 0x00000007u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 00C9D4 moveq #7,%d3 */
	{ uint32_t v = 0xA000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00C9D6 movew #-24576,%d6 */
	{ uint32_t eaS = 0xFFFF9E46u; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00C9DA movew ffff9e46,%d7 */
	hle_jump(ctx, 0x00C9E0); return;  /* 00C9E0 jsr %pc@(12234) */
L_00C9E4:
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); hle_jump(ctx, 0x00C9E6); return;  /* 00C9E4 movew %sp@+,%d7 */
}

HLE_RANGE_PURE(0x00C9C2, Auto_00C9C2, 200, HLE_EXIT_SP(0x00C9E0, -2)) { pc_run_00C9C2(ctx, 0x00C9C2); }
HLE_RANGE_PURE(0x00C9E4, Auto_00C9E4, 200, HLE_EXIT_SP(0x00C9E6, 2)) { pc_run_00C9C2(ctx, 0x00C9E4); }
static void pc_run_00CABA(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00CABE: goto L_00CABE;
	case 0x00CAC4: goto L_00CAC4;
	case 0x00CAD8: goto L_00CAD8;
	case 0x00CAF6: goto L_00CAF6;
	default: __builtin_trap();
	}
L_00CABA:
	hle_jump(ctx, 0x00CABA); return;  /* 00CABA jsr %pc@(12316) */
L_00CABE:
	{ ctx->d[0] = 0xFFFFFFE0u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 00CABE moveq #-32,%d0 */
	hle_jump(ctx, 0x00CAC0); return;  /* 00CAC0 jsr %pc@(12332) */
L_00CAC4:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(37);  unsigned b = 0; uint32_t m = 1u << b; uint8_t v = ld8(eaD); cc_set(ctx, CC_Z, (v & m) ? 0 : CC_Z);  } hle_insn(ctx, 16);  /* 00CAC4 btst #0,%fp@(37) */
	if (COND_NE) { hle_insn(ctx, 10); goto L_00CAD0; } hle_insn(ctx, 8);  /* 00CACA bnes cad0 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = alu(ctx, OP_SUB, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 16);  /* 00CACC subqw #1,%fp@(2) */
L_00CAD0:
	{ uint32_t v = 0xCDu; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00CAD0 movew #205,%d1 */
	hle_jump(ctx, 0x00CAD4); return;  /* 00CAD4 jsr %pc@(13228) */
L_00CAD8:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = alu(ctx, OP_SUB, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 16);  /* 00CAD8 subqw #1,%fp@(36) */
	if (COND_NE) { hle_insn(ctx, 10); goto L_00CAF8; } hle_insn(ctx, 8);  /* 00CADC bnes caf8 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = 0x46u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00CADE movew #70,%fp@(36) */
	{ ctx->a[7] -= 4; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->a[6] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 00CAE4 movel %fp,%sp@- */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(4); uint32_t v = ld16(eaS); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00CAE6 movew %fp@(4),%d1 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[1] & 0xFFFFu), 0x24u, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 00CAEA subiw #36,%d1 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = ld16(eaS); set_w(&ctx->d[5], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00CAEE movew %fp@(2),%d5 */
	hle_jump(ctx, 0x00CAF2); return;  /* 00CAF2 jsr %pc@(9a02) */
L_00CAF6:
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 4; ctx->a[6] = ld32(eaS); } hle_insn(ctx, 12);  /* 00CAF6 moveal %sp@+,%fp */
L_00CAF8:
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 00CAF8 braw b3be */
}

HLE_RANGE_PURE(0x00CABE, Auto_00CABE, 200, 0x00CAC0) { pc_run_00CABA(ctx, 0x00CABE); }
HLE_RANGE_PURE(0x00CAC4, Auto_00CAC4, 200, 0x00CAD4) { pc_run_00CABA(ctx, 0x00CAC4); }
HLE_RANGE_PURE(0x00CAD8, Auto_00CAD8, 200, 0x00B3BE, HLE_EXIT_SP(0x00CAF2, -4)) { pc_run_00CABA(ctx, 0x00CAD8); }
HLE_RANGE_PURE(0x00CAF6, Auto_00CAF6, 200, HLE_EXIT_SP(0x00B3BE, 4)) { pc_run_00CABA(ctx, 0x00CAF6); }
static void pc_run_00CBE8(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00CBE8: goto L_00CBE8;
	case 0x00CC1E: goto L_00CC1E;
	case 0x00CC26: goto L_00CC26;
	case 0x00CC2E: goto L_00CC2E;
	case 0x00CC36: goto L_00CC36;
	default: __builtin_trap();
	}
L_00CBE8:
	{ uint32_t eaS = 0xFFFFAADCu; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 00CBE8 lea ffffaadc,%a0 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x68u, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 8);  /* 00CBEE subiw #104,%d0 */
	{ ctx->d[1] = 0x0000000Fu; cc_logic32(ctx, ctx->d[1]); } hle_insn(ctx, 4);  /* 00CBF2 moveq #15,%d1 */
L_00CBF4:
	{ uint32_t eaD = ctx->a[0]; uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00CBF4 movew %d0,%a0@ */
	{ uint32_t eaS = ctx->a[0] + (uint32_t)(int32_t)(32); ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 00CBF6 lea %a0@(32),%a0 */
	{ uint16_t v = (uint16_t)ctx->d[1] - 1; set_w(&ctx->d[1], v); if (v != 0xFFFF) { hle_insn(ctx, 10); goto L_00CBF4; } hle_insn(ctx, 14); }  /* 00CBFA dbf %d1,cbf4 */
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[4], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 00CBFE movew %d0,%d4 */
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0x3u) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00CC00 andiw #3,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_00CC38; } hle_insn(ctx, 8);  /* 00CC04 bnes cc38 */
	{ uint32_t v = ((ctx->d[4] & 0xFFFFu) & 0xCu) & 0xFFFFu; set_w(&ctx->d[4], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00CC06 andiw #12,%d4 */
	{ uint32_t v = shf(ctx, 'r', (ctx->d[4] & 0xFFFFu), 1, 16); set_w(&ctx->d[4], v); } hle_insn(ctx, 8);  /* 00CC0A lsrw #1,%d4 */
	{ ctx->a[7] -= 2; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->d[7] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00CC0C movew %d7,%sp@- */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(2); alu(ctx, OP_SUB, ld16(eaD), 0x140u, 16, 0); } hle_insn(ctx, 16);  /* 00CC0E cmpiw #320,%fp@(2) */
	if (COND_GT) { hle_insn(ctx, 10); goto L_00CC1E; } hle_insn(ctx, 8);  /* 00CC14 bgts cc1e */
	{ uint32_t v = 0xE8ACu; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00CC16 movew #-5972,%d1 */
	hle_jump(ctx, 0x00CC1A); return;  /* 00CC1A jsr %pc@(cc3c) */
L_00CC1E:
	{ uint32_t v = 0xE892u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00CC1E movew #-5998,%d1 */
	hle_jump(ctx, 0x00CC22); return;  /* 00CC22 jsr %pc@(cc3c) */
L_00CC26:
	{ uint32_t v = 0xE89Au; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00CC26 movew #-5990,%d1 */
	hle_jump(ctx, 0x00CC2A); return;  /* 00CC2A jsr %pc@(cc6a) */
L_00CC2E:
	{ uint32_t v = 0xE880u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00CC2E movew #-6016,%d1 */
	hle_jump(ctx, 0x00CC32); return;  /* 00CC32 jsr %pc@(cc6a) */
L_00CC36:
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00CC36 movew %sp@+,%d7 */
L_00CC38:
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 00CC38 braw b3be */
}

HLE_RANGE_PURE(0x00CBE8, Auto_00CBE8, 200, 0x00B3BE, HLE_EXIT_SP(0x00CC1A, -2), HLE_EXIT_SP(0x00CC22, -2)) { pc_run_00CBE8(ctx, 0x00CBE8); }
HLE_RANGE_PURE(0x00CC1E, Auto_00CC1E, 200, 0x00CC22) { pc_run_00CBE8(ctx, 0x00CC1E); }
HLE_RANGE_PURE(0x00CC26, Auto_00CC26, 200, 0x00CC2A) { pc_run_00CBE8(ctx, 0x00CC26); }
HLE_RANGE_PURE(0x00CC2E, Auto_00CC2E, 200, 0x00CC32) { pc_run_00CBE8(ctx, 0x00CC2E); }
HLE_RANGE_PURE(0x00CC36, Auto_00CC36, 200, HLE_EXIT_SP(0x00B3BE, 2)) { pc_run_00CBE8(ctx, 0x00CC36); }
static void pc_run_00CCE6(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00CCE6: goto L_00CCE6;
	case 0x00CCF0: goto L_00CCF0;
	case 0x00CCFA: goto L_00CCFA;
	case 0x00CD0E: goto L_00CD0E;
	default: __builtin_trap();
	}
L_00CCE6:
	{ ctx->d[4] = 0x00000005u; cc_logic32(ctx, ctx->d[4]); } hle_insn(ctx, 4);  /* 00CCE6 moveq #5,%d4 */
L_00CCE8:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(4); uint32_t v = ld16(eaS); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00CCE8 movew %fp@(4),%d1 */
	hle_jump(ctx, 0x00CCEC); return;  /* 00CCEC jsr %pc@(881c) */
L_00CCF0:
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0x10u) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00CCF0 andiw #16,%d0 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), (ctx->d[0] & 0xFFFFu), 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 4);  /* 00CCF4 addw %d0,%d1 */
	hle_jump(ctx, 0x00CCF6); return;  /* 00CCF6 jsr %pc@(881c) */
L_00CCFA:
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0x10u) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00CCFA andiw #16,%d0 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), ld16(eaS), 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 12);  /* 00CCFE addw %fp@(2),%d0 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(6); uint32_t v = ld16(eaS); set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00CD02 movew %fp@(6),%d2 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[1] & 0xFFFFu), 0x10u, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 00CD06 subiw #16,%d1 */
	hle_jump(ctx, 0x00CD0A); return;  /* 00CD0A jsr %pc@(12494) */
L_00CD0E:
	{ uint16_t v = (uint16_t)ctx->d[4] - 1; set_w(&ctx->d[4], v); if (v != 0xFFFF) { hle_insn(ctx, 10); goto L_00CCE8; } hle_insn(ctx, 14); }  /* 00CD0E dbf %d4,cce8 */
	{ uint32_t eaD = 0xFFFF9EBCu;  st8(eaD, 0xFF); } hle_insn(ctx, 20);  /* 00CD12 st ffff9ebc */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 00CD18 braw b3be */
}

HLE_RANGE_PURE(0x00CCE6, Auto_00CCE6, 200, 0x00CCEC) { pc_run_00CCE6(ctx, 0x00CCE6); }
HLE_RANGE_PURE(0x00CCF0, Auto_00CCF0, 200, 0x00CCF6) { pc_run_00CCE6(ctx, 0x00CCF0); }
HLE_RANGE_PURE(0x00CCFA, Auto_00CCFA, 200, 0x00CD0A) { pc_run_00CCE6(ctx, 0x00CCFA); }
HLE_RANGE_PURE(0x00CD0E, Auto_00CD0E, 200, 0x00B3BE, 0x00CCEC) { pc_run_00CCE6(ctx, 0x00CD0E); }
static void pc_run_00CD88(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00CD88: goto L_00CD88;
	case 0x00CDAA: goto L_00CDAA;
	default: __builtin_trap();
	}
L_00CD88:
	{ ctx->a[7] -= 2; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->d[7] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00CD88 movew %d7,%sp@- */
	{ uint32_t eaS = 0x0004E26Au; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 00CD8A lea 4e26a,%a0 */
	{ uint32_t eaS = ctx->a[0] + (uint32_t)(int32_t)(0) + (uint32_t)(int32_t)(int16_t)ctx->d[1]; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 00CD90 lea %a0@(0,%d1:w),%a0 */
	{ uint32_t v = 0xE698u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00CD94 movew #-6504,%d1 */
	{ ctx->d[2] = 0x00000002u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 00CD98 moveq #2,%d2 */
	{ ctx->d[3] = 0x00000002u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 00CD9A moveq #2,%d3 */
	{ uint32_t v = 0x0u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00CD9C movew #0,%d6 */
	{ uint32_t eaS = 0xFFFF9E48u; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00CDA0 movew ffff9e48,%d7 */
	hle_jump(ctx, 0x00CDA6); return;  /* 00CDA6 jsr %pc@(12234) */
L_00CDAA:
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00CDAA movew %sp@+,%d7 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00CDFC); return;  /* 00CDAC braw cdfc */
}

HLE_RANGE_PURE(0x00CD88, Auto_00CD88, 200, HLE_EXIT_SP(0x00CDA6, -2)) { pc_run_00CD88(ctx, 0x00CD88); }
HLE_RANGE_PURE(0x00CDAA, Auto_00CDAA, 200, HLE_EXIT_SP(0x00CDFC, 2)) { pc_run_00CD88(ctx, 0x00CDAA); }
static void pc_run_00CDFC(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	default: __builtin_trap();
	}
L_00CDFC:
	hle_jump(ctx, 0x00CDFC); return;  /* 00CDFC rts */
}

static void pc_run_00CF58(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00CF58: goto L_00CF58;
	case 0x00CF8E: goto L_00CF8E;
	case 0x00CF9A: goto L_00CF9A;
	default: __builtin_trap();
	}
L_00CF58:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(42); uint32_t v = alu(ctx, OP_SUB, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 16);  /* 00CF58 subqw #1,%fp@(42) */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(42); uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00CF5C movew %fp@(42),%d0 */
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x30u, 16, 0); } hle_insn(ctx, 8);  /* 00CF60 cmpiw #48,%d0 */
	if (COND_GE) { hle_insn(ctx, 10); goto L_00CF9C; } hle_insn(ctx, 12);  /* 00CF64 bgew cf9c */
	{ cc_logic16(ctx, (ctx->d[0] & 0xFFFFu)); } hle_insn(ctx, 4);  /* 00CF68 tstw %d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_00CF72; } hle_insn(ctx, 8);  /* 00CF6A bnes cf72 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(42); uint32_t v = 0x100u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00CF6C movew #256,%fp@(42) */
L_00CF72:
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0xFu) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00CF72 andiw #15,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_00CF9C; } hle_insn(ctx, 8);  /* 00CF76 bnes cf9c */
	{ ctx->a[7] -= 4; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->a[6] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 00CF78 movel %fp,%sp@- */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(4); uint32_t v = ld16(eaS); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00CF7A movew %fp@(4),%d1 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[1] & 0xFFFFu), 0x10u, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 00CF7E subiw #16,%d1 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = ld16(eaS); set_w(&ctx->d[5], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00CF82 movew %fp@(2),%d5 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[5] & 0xFFFFu), 0x18u, 16, 1); set_w(&ctx->d[5], v); } hle_insn(ctx, 8);  /* 00CF86 addiw #24,%d5 */
	hle_jump(ctx, 0x00CF8A); return;  /* 00CF8A jsr %pc@(881c) */
L_00CF8E:
	{ cc_logic16(ctx, (ctx->d[0] & 0xFFFFu)); } hle_insn(ctx, 4);  /* 00CF8E tstw %d0 */
	if (COND_MI) { hle_insn(ctx, 10); goto L_00CF96; } hle_insn(ctx, 8);  /* 00CF90 bmis cf96 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[5] & 0xFFFFu), 0x30u, 16, 1); set_w(&ctx->d[5], v); } hle_insn(ctx, 8);  /* 00CF92 subiw #48,%d5 */
L_00CF96:
	hle_jump(ctx, 0x00CF96); return;  /* 00CF96 jsr %pc@(99d4) */
L_00CF9A:
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 4; ctx->a[6] = ld32(eaS); } hle_insn(ctx, 12);  /* 00CF9A moveal %sp@+,%fp */
L_00CF9C:
	hle_jump(ctx, 0x00CF9C); return;  /* 00CF9C rts */
}

HLE_RANGE_PURE(0x00CF58, Auto_00CF58, 200, HLE_EXIT_SP(0x00CF8A, -4), 0x00CF9C) { pc_run_00CF58(ctx, 0x00CF58); }
HLE_RANGE_PURE(0x00CF8E, Auto_00CF8E, 200, 0x00CF96) { pc_run_00CF58(ctx, 0x00CF8E); }
HLE_RANGE_PURE(0x00CF9A, Auto_00CF9A, 200, HLE_EXIT_SP(0x00CF9C, 4)) { pc_run_00CF58(ctx, 0x00CF9A); }
static void pc_run_00D1D2(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00D1D6: goto L_00D1D6;
	case 0x00D1E8: goto L_00D1E8;
	case 0x00D214: goto L_00D214;
	case 0x00D226: goto L_00D226;
	default: __builtin_trap();
	}
L_00D1D2:
	hle_jump(ctx, 0x00D1D2); return;  /* 00D1D2 jsr %pc@(d236) */
L_00D1D6:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(4); uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00D1D6 movew %fp@(4),%d0 */
	{ uint32_t v = 0xB0u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00D1DA movew #176,%d0 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(4); uint32_t v = alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), ld16(eaS), 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 12);  /* 00D1DE subw %fp@(4),%d0 */
	{ ctx->d[1] = 0x00000000u; cc_logic32(ctx, ctx->d[1]); } hle_insn(ctx, 4);  /* 00D1E2 moveq #0,%d1 */
	hle_jump(ctx, 0x00D1E4); return;  /* 00D1E4 jsr %pc@(11f58) */
L_00D1E8:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00D1E8 movew %fp@(36),%d0 */
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 00D1EC movew %d0,%d1 */
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0x180u) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00D1EE andiw #384,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_00D214; } hle_insn(ctx, 8);  /* 00D1F2 bnes d214 */
	{ unsigned b = 2; uint32_t m = 1u << b; cc_set(ctx, CC_Z, (ctx->d[1] & m) ? 0 : CC_Z);  hle_insn(ctx, 10); }  /* 00D1F4 btst #2,%d1 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_00D214; } hle_insn(ctx, 8);  /* 00D1F8 beqs d214 */
	{ uint32_t v = 0xE1u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00D1FA movew #225,%d1 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(2); ctx->d[2] = (uint32_t)(int32_t)(int16_t)ld16(eaS + 0); ctx->d[3] = (uint32_t)(int32_t)(int16_t)ld16(eaS + 2); } hle_insn(ctx, 24);  /* 00D1FE movemw %fp@(2),%d2-%d3 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[2] & 0xFFFFu), 0x68u, 16, 1); set_w(&ctx->d[2], v); } hle_insn(ctx, 8);  /* 00D204 addiw #104,%d2 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[3] & 0xFFFFu), 0x37u, 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 8);  /* 00D208 subiw #55,%d3 */
	{ uint32_t v = 0x63u; set_w(&ctx->d[5], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00D20C movew #99,%d5 */
	hle_jump(ctx, 0x00D210); return;  /* 00D210 jsr %pc@(13240) */
L_00D214:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(4); uint32_t v = ld16(eaS); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00D214 movew %fp@(4),%d3 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[3] & 0xFFFFu), 0x4u, 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 8);  /* 00D218 subiw #4,%d3 */
	{ alu(ctx, OP_SUB, (ctx->d[3] & 0xFFFFu), 0xA6u, 16, 0); } hle_insn(ctx, 8);  /* 00D21C cmpiw #166,%d3 */
	if (COND_LT) { hle_insn(ctx, 10); goto L_00D230; } hle_insn(ctx, 8);  /* 00D220 blts d230 */
	hle_jump(ctx, 0x00D222); return;  /* 00D222 jsr %pc@(881c) */
L_00D226:
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0x7u) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00D226 andiw #7,%d0 */
	{ uint32_t v = 0xABu; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00D22A movew #171,%d3 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[3] & 0xFFFFu), (ctx->d[0] & 0xFFFFu), 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 4);  /* 00D22E subw %d0,%d3 */
L_00D230:
	hle_jump(ctx, 0x00D230); return;  /* 00D230 rts */
}

HLE_RANGE_PURE(0x00D1D6, Auto_00D1D6, 200, 0x00D1E4) { pc_run_00D1D2(ctx, 0x00D1D6); }
HLE_RANGE_PURE(0x00D1E8, Auto_00D1E8, 200, 0x00D210, 0x00D222, 0x00D230) { pc_run_00D1D2(ctx, 0x00D1E8); }
HLE_RANGE_PURE(0x00D214, Auto_00D214, 200, 0x00D222, 0x00D230) { pc_run_00D1D2(ctx, 0x00D214); }
HLE_RANGE_PURE(0x00D226, Auto_00D226, 200, 0x00D230) { pc_run_00D1D2(ctx, 0x00D226); }
static void pc_run_00D232(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00D232: goto L_00D232;
	default: __builtin_trap();
	}
L_00D232:
	{ ctx->d[4] = 0x00000000u; cc_logic32(ctx, ctx->d[4]); } hle_insn(ctx, 4);  /* 00D232 moveq #0,%d4 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00D238); return;  /* 00D234 bras d238 */
}

HLE_RANGE_PURE(0x00D232, Auto_00D232, 200, 0x00D238) { pc_run_00D232(ctx, 0x00D232); }
static void pc_run_00D236(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00D236: goto L_00D236;
	default: __builtin_trap();
	}
L_00D236:
	{ ctx->d[4] = 0x00000003u; cc_logic32(ctx, ctx->d[4]); } hle_insn(ctx, 4); hle_jump(ctx, 0x00D238); return;  /* 00D236 moveq #3,%d4 */
}

HLE_RANGE_PURE(0x00D236, Auto_00D236, 200, 0x00D238) { pc_run_00D236(ctx, 0x00D236); }
static void pc_run_00D2B8(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00D2B8: goto L_00D2B8;
	case 0x00D2DE: goto L_00D2DE;
	case 0x00D2F8: goto L_00D2F8;
	case 0x00D306: goto L_00D306;
	default: __builtin_trap();
	}
L_00D2B8:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = ld16(eaS); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00D2B8 movew %fp@(36),%d1 */
	{ uint32_t v = ((ctx->d[1] & 0xFFFFu) & 0x6u) & 0xFFFFu; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00D2BC andiw #6,%d1 */
	{ uint32_t v = shf(ctx, 'a', (ctx->d[1] & 0xFFFFu), 1, 16); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 00D2C0 asrw #1,%d1 */
	{ uint32_t v = ((ctx->d[1] & 0xFFFFu) ^ 0x3u) & 0xFFFFu; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00D2C2 eoriw #3,%d1 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0xDDu, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 00D2C6 addiw #221,%d1 */
	{ ctx->a[7] -= 2; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->d[1] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00D2CA movew %d1,%sp@- */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = ld16(eaS); set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00D2CC movew %fp@(2),%d2 */
	if (COND_MI) { hle_insn(ctx, 10); goto L_00D2E0; } hle_insn(ctx, 8);  /* 00D2D0 bmis d2e0 */
	{ ctx->d[5] = 0x00000063u; cc_logic32(ctx, ctx->d[5]); } hle_insn(ctx, 4);  /* 00D2D2 moveq #99,%d5 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[2] & 0xFFFFu), 0x3Cu, 16, 1); set_w(&ctx->d[2], v); } hle_insn(ctx, 8);  /* 00D2D4 subiw #60,%d2 */
	{ ctx->a[7] -= 2; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->d[3] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00D2D8 movew %d3,%sp@- */
	hle_jump(ctx, 0x00D2DA); return;  /* 00D2DA jsr %pc@(13240) */
L_00D2DE:
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00D2DE movew %sp@+,%d3 */
L_00D2E0:
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00D2E0 movew %sp@+,%d1 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = ld16(eaS); set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00D2E2 movew %fp@(2),%d2 */
	{ alu(ctx, OP_SUB, (ctx->d[2] & 0xFFFFu), 0x12Cu, 16, 0); } hle_insn(ctx, 8);  /* 00D2E6 cmpiw #300,%d2 */
	if (COND_GT) { hle_insn(ctx, 10); goto L_00D2F8; } hle_insn(ctx, 12);  /* 00D2EA bgtw d2f8 */
	{ ctx->d[5] = 0x00000063u; cc_logic32(ctx, ctx->d[5]); } hle_insn(ctx, 4);  /* 00D2EE moveq #99,%d5 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[2] & 0xFFFFu), 0x5Cu, 16, 1); set_w(&ctx->d[2], v); } hle_insn(ctx, 8);  /* 00D2F0 addiw #92,%d2 */
	hle_jump(ctx, 0x00D2F4); return;  /* 00D2F4 jsr %pc@(13240) */
L_00D2F8:
	{ uint32_t eaD = 0xFFFF9EA6u; cc_logic16(ctx, ld16(eaD)); } hle_insn(ctx, 16);  /* 00D2F8 tstw ffff9ea6 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_00D306; } hle_insn(ctx, 8);  /* 00D2FE beqs d306 */
	{ ctx->d[1] = 0x00000005u; cc_logic32(ctx, ctx->d[1]); } hle_insn(ctx, 4);  /* 00D300 moveq #5,%d1 */
	hle_jump(ctx, 0x00D302); return;  /* 00D302 jsr %pc@(13228) */
L_00D306:
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 00D306 braw b3be */
}

HLE_RANGE_PURE(0x00D2B8, Auto_00D2B8, 200, 0x00B3BE, HLE_EXIT_SP(0x00D2DA, -4), 0x00D2F4, 0x00D302) { pc_run_00D2B8(ctx, 0x00D2B8); }
HLE_RANGE_PURE(0x00D2DE, Auto_00D2DE, 200, HLE_EXIT_SP(0x00B3BE, 4), HLE_EXIT_SP(0x00D2F4, 4), HLE_EXIT_SP(0x00D302, 4)) { pc_run_00D2B8(ctx, 0x00D2DE); }
HLE_RANGE_PURE(0x00D2F8, Auto_00D2F8, 200, 0x00B3BE, 0x00D302) { pc_run_00D2B8(ctx, 0x00D2F8); }
HLE_RANGE_PURE(0x00D306, Auto_00D306, 200, 0x00B3BE) { pc_run_00D2B8(ctx, 0x00D306); }
static void pc_run_00D9FC(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00D9FC: goto L_00D9FC;
	default: __builtin_trap();
	}
L_00D9FC:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = alu(ctx, OP_SUB, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 16);  /* 00D9FC subqw #1,%fp@(36) */
	if (COND_PL) { hle_insn(ctx, 10); goto L_00DA14; } hle_insn(ctx, 8);  /* 00DA00 bpls da14 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0xDA18u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 00DA02 movel #55832,%fp@(32) */
	{ uint32_t eaD = ctx->a[6];  unsigned b = 7; uint32_t m = 1u << b; uint8_t v = ld8(eaD); cc_set(ctx, CC_Z, (v & m) ? 0 : CC_Z); st8(eaD, v & ~m); } hle_insn(ctx, 16);  /* 00DA0A bclr #7,%fp@ */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = 0x10u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00DA0E movew #16,%fp@(36) */
L_00DA14:
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 00DA14 braw b3be */
}

HLE_RANGE_PURE(0x00D9FC, Auto_00D9FC, 200, 0x00B3BE) { pc_run_00D9FC(ctx, 0x00D9FC); }
static void pc_run_00DB5A(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00DB5A: goto L_00DB5A;
	case 0x00DB6E: goto L_00DB6E;
	default: __builtin_trap();
	}
L_00DB5A:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(2); ctx->d[0] = (uint32_t)(int32_t)(int16_t)ld16(eaS + 0); ctx->d[1] = (uint32_t)(int32_t)(int16_t)ld16(eaS + 2); } hle_insn(ctx, 24);  /* 00DB5A movemw %fp@(2),%d0-%d1 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(6); uint32_t v = ld16(eaS); set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00DB60 movew %fp@(6),%d2 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[2] & 0xFFFFu), 0x1u, 16, 1); set_w(&ctx->d[2], v); } hle_insn(ctx, 4);  /* 00DB64 subqw #1,%d2 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0x20u, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 00DB66 addiw #32,%d1 */
	hle_jump(ctx, 0x00DB6A); return;  /* 00DB6A jsr %pc@(1253c) */
L_00DB6E:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0xDB76u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24); hle_jump(ctx, 0x00DB76); return;  /* 00DB6E movel #56182,%fp@(32) */
}

HLE_RANGE_PURE(0x00DB5A, Auto_00DB5A, 200, 0x00DB6A) { pc_run_00DB5A(ctx, 0x00DB5A); }
HLE_RANGE_PURE(0x00DB6E, Auto_00DB6E, 200, 0x00DB76) { pc_run_00DB5A(ctx, 0x00DB6E); }
static void pc_run_00DB76(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00DB7A: goto L_00DB7A;
	case 0x00DB80: goto L_00DB80;
	case 0x00DB88: goto L_00DB88;
	default: __builtin_trap();
	}
L_00DB76:
	hle_jump(ctx, 0x00DB76); return;  /* 00DB76 jsr %pc@(12316) */
L_00DB7A:
	{ ctx->d[0] = 0xFFFFFFD8u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 00DB7A moveq #-40,%d0 */
	hle_jump(ctx, 0x00DB7C); return;  /* 00DB7C jsr %pc@(12332) */
L_00DB80:
	{ uint32_t v = 0x174u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00DB80 movew #372,%d1 */
	hle_jump(ctx, 0x00DB84); return;  /* 00DB84 jsr %pc@(13228) */
L_00DB88:
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 00DB88 braw b3be */
}

HLE_RANGE_PURE(0x00DB7A, Auto_00DB7A, 200, 0x00DB7C) { pc_run_00DB76(ctx, 0x00DB7A); }
HLE_RANGE_PURE(0x00DB80, Auto_00DB80, 200, 0x00DB84) { pc_run_00DB76(ctx, 0x00DB80); }
HLE_RANGE_PURE(0x00DB88, Auto_00DB88, 200, 0x00B3BE) { pc_run_00DB76(ctx, 0x00DB88); }
static void pc_run_00DC9E(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00DC9E: goto L_00DC9E;
	default: __builtin_trap();
	}
L_00DC9E:
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0xCu) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00DC9E andiw #12,%d0 */
	{ uint32_t eaS = 0x0005FA92u; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 00DCA2 lea 5fa92,%a0 */
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 00DCA8 movew %d0,%d2 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), (ctx->d[0] & 0xFFFFu), 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 4);  /* 00DCAA addw %d0,%d0 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), (ctx->d[2] & 0xFFFFu), 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 4);  /* 00DCAC addw %d2,%d0 */
	{ uint32_t eaS = ctx->a[0] + (uint32_t)(int32_t)(0) + (uint32_t)(int32_t)(int16_t)ctx->d[0]; ctx->a[0] = eaS; } hle_insn(ctx, 12); hle_jump(ctx, 0x00DCB2); return;  /* 00DCAE lea %a0@(0,%d0:w),%a0 */
}

HLE_RANGE_PURE(0x00DC9E, Auto_00DC9E, 200, 0x00DCB2) { pc_run_00DC9E(ctx, 0x00DC9E); }
static void pc_run_00DCEA(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00DCEA: goto L_00DCEA;
	case 0x00DCF6: goto L_00DCF6;
	case 0x00DD3E: goto L_00DD3E;
	default: __builtin_trap();
	}
L_00DCEA:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = alu(ctx, OP_SUB, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 16);  /* 00DCEA subqw #1,%fp@(36) */
	if (COND_NE) { hle_insn(ctx, 10); goto L_00DD2C; } hle_insn(ctx, 8);  /* 00DCEE bnes dd2c */
	{ ctx->d[1] = 0x00000012u; cc_logic32(ctx, ctx->d[1]); } hle_insn(ctx, 4);  /* 00DCF0 moveq #18,%d1 */
L_00DCF2:
	hle_jump(ctx, 0x00DCF2); return;  /* 00DCF2 jsr %pc@(881c) */
L_00DCF6:
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0x7u) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00DCF6 andiw #7,%d0 */
	{ uint32_t v = shf(ctx, 'L', (ctx->d[0] & 0xFFFFu), 2, 16); set_w(&ctx->d[0], v); } hle_insn(ctx, 10);  /* 00DCFA aslw #2,%d0 */
	{ uint32_t eaS = ctx->a[1] + (uint32_t)(int32_t)(0) + (uint32_t)(int32_t)(int16_t)ctx->d[0]; uint32_t v = ld32(eaS); ctx->d[2] = v; cc_logic32(ctx, v); } hle_insn(ctx, 18);  /* 00DCFC movel %a1@(0,%d0:w),%d2 */
	if (COND_PL) { hle_insn(ctx, 10); goto L_00DD0C; } hle_insn(ctx, 8);  /* 00DD00 bpls dd0c */
	{ ctx->a[2] = (ctx->d[2] & 0xFFFFFFFFu); } hle_insn(ctx, 4);  /* 00DD02 moveal %d2,%a2 */
	{ uint32_t eaS = ctx->a[2]; uint32_t v = ld16(eaS); set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00DD04 movew %a2@,%d2 */
	{ alu(ctx, OP_SUB, (ctx->d[2] & 0xFFu), 0x22u, 8, 0); } hle_insn(ctx, 8);  /* 00DD06 cmpib #34,%d2 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_00DD14; } hle_insn(ctx, 8);  /* 00DD0A beqs dd14 */
L_00DD0C:
	{ uint16_t v = (uint16_t)ctx->d[1] - 1; set_w(&ctx->d[1], v); if (v != 0xFFFF) { hle_insn(ctx, 10); goto L_00DCF2; } hle_insn(ctx, 14); }  /* 00DD0C dbf %d1,dcf2 */
	hle_insn(ctx, 10); goto L_00DD26;  /* 00DD10 braw dd26 */
L_00DD14:
	{ uint32_t eaD = ctx->a[2] + (uint32_t)(int32_t)(36); uint32_t v = 0x10u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00DD14 movew #16,%a2@(36) */
	{ uint32_t eaD = ctx->a[2];  unsigned b = 7; uint32_t m = 1u << b; uint8_t v = ld8(eaD); cc_set(ctx, CC_Z, (v & m) ? 0 : CC_Z); st8(eaD, v & ~m); } hle_insn(ctx, 16);  /* 00DD1A bclr #7,%a2@ */
	{ uint32_t eaD = ctx->a[2] + (uint32_t)(int32_t)(32); uint32_t v = 0xDC7Eu; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 00DD1E movel #56446,%a2@(32) */
L_00DD26:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = 0x186u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00DD26 movew #390,%fp@(36) */
L_00DD2C:
	{ uint32_t eaD = 0xFFFFAF18u; cc_logic16(ctx, ld16(eaD)); } hle_insn(ctx, 16);  /* 00DD2C tstw ffffaf18 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_00DD60; } hle_insn(ctx, 8);  /* 00DD32 bnes dd60 */
	{ ctx->a[0] = 0x1FCBAu; } hle_insn(ctx, 12);  /* 00DD34 moveal #130234,%a0 */
	hle_jump(ctx, 0x00DD3A); return;  /* 00DD3A jsr %pc@(933e) */
L_00DD3E:
	{ uint32_t eaS = 0xFFFF87B2u; ctx->a[1] = eaS; } hle_insn(ctx, 12);  /* 00DD3E lea ffff87b2,%a1 */
	{ ctx->d[3] = 0x0000004Fu; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 00DD44 moveq #79,%d3 */
L_00DD46:
	{ uint32_t eaS = ctx->a[1]; uint32_t v = ld16(eaS); set_w(&ctx->d[4], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00DD46 movew %a1@,%d4 */
	{ alu(ctx, OP_SUB, (ctx->d[4] & 0xFFu), 0x22u, 8, 0); } hle_insn(ctx, 8);  /* 00DD48 cmpib #34,%d4 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_00DD56; } hle_insn(ctx, 8);  /* 00DD4C bnes dd56 */
	{ uint32_t eaD = ctx->a[1]; st16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 12);  /* 00DD4E clrw %a1@ */
	{ uint32_t eaD = 0xFFFF80DAu; uint32_t v = alu(ctx, OP_SUB, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 20);  /* 00DD50 subqw #1,ffff80da */
L_00DD56:
	{ uint32_t eaS = ctx->a[1] + (uint32_t)(int32_t)(44); ctx->a[1] = eaS; } hle_insn(ctx, 8);  /* 00DD56 lea %a1@(44),%a1 */
	{ uint16_t v = (uint16_t)ctx->d[3] - 1; set_w(&ctx->d[3], v); if (v != 0xFFFF) { hle_insn(ctx, 10); goto L_00DD46; } hle_insn(ctx, 14); }  /* 00DD5A dbf %d3,dd46 */
	{ set_w(&ctx->d[7], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 4);  /* 00DD5E clrw %d7 */
L_00DD60:
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 00DD60 braw b3be */
}

HLE_RANGE_PURE(0x00DCEA, Auto_00DCEA, 200, 0x00B3BE, 0x00DCF2, 0x00DD3A) { pc_run_00DCEA(ctx, 0x00DCEA); }
HLE_RANGE_PURE(0x00DCF6, Auto_00DCF6, 200, 0x00B3BE, 0x00DCF2, 0x00DD3A) { pc_run_00DCEA(ctx, 0x00DCF6); }
HLE_RANGE_PURE(0x00DD3E, Auto_00DD3E, 200, 0x00B3BE) { pc_run_00DCEA(ctx, 0x00DD3E); }
static void pc_run_00E0E8(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00E0E8: goto L_00E0E8;
	default: __builtin_trap();
	}
L_00E0E8:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(38); cc_logic16(ctx, ld16(eaD)); } hle_insn(ctx, 12);  /* 00E0E8 tstw %fp@(38) */
	if (COND_PL) { hle_insn(ctx, 10); goto L_00E0F2; } hle_insn(ctx, 8);  /* 00E0EC bpls e0f2 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0xBFu, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 00E0EE addiw #191,%d1 */
L_00E0F2:
	hle_jump(ctx, 0x00E0F2); return;  /* 00E0F2 jsr %pc@(13228) */
L_00E0F6:
	hle_jump(ctx, 0x00E0F6); return;  /* 00E0F6 rts */
}

HLE_RANGE_PURE(0x00E0E8, Auto_00E0E8, 200, 0x00E0F2) { pc_run_00E0E8(ctx, 0x00E0E8); }
static void pc_run_00E0F8(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00E0F8: goto L_00E0F8;
	default: __builtin_trap();
	}
L_00E0F8:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(38); cc_logic16(ctx, ld16(eaD)); } hle_insn(ctx, 12);  /* 00E0F8 tstw %fp@(38) */
	if (COND_PL) { hle_insn(ctx, 10); goto L_00E102; } hle_insn(ctx, 8);  /* 00E0FC bpls e102 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0xBFu, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 00E0FE addiw #191,%d1 */
L_00E102:
	hle_jump(ctx, 0x00E102); return;  /* 00E102 jsr %pc@(13240) */
L_00E106:
	hle_jump(ctx, 0x00E106); return;  /* 00E106 rts */
}

HLE_RANGE_PURE(0x00E0F8, Auto_00E0F8, 200, 0x00E102) { pc_run_00E0F8(ctx, 0x00E0F8); }
static void pc_run_00E108(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00E108: goto L_00E108;
	default: __builtin_trap();
	}
L_00E108:
	{ ctx->d[4] = 0x00000000u; cc_logic32(ctx, ctx->d[4]); } hle_insn(ctx, 4);  /* 00E108 moveq #0,%d4 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00E10E); return;  /* 00E10A bras e10e */
}

HLE_RANGE_PURE(0x00E108, Auto_00E108, 200, 0x00E10E) { pc_run_00E108(ctx, 0x00E108); }
static void pc_run_00E10C(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00E10C: goto L_00E10C;
	default: __builtin_trap();
	}
L_00E10C:
	{ ctx->d[4] = 0x00000003u; cc_logic32(ctx, ctx->d[4]); } hle_insn(ctx, 4); hle_jump(ctx, 0x00E10E); return;  /* 00E10C moveq #3,%d4 */
}

HLE_RANGE_PURE(0x00E10C, Auto_00E10C, 200, 0x00E10E) { pc_run_00E10C(ctx, 0x00E10C); }
static void pc_run_00E10E(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00E10E: goto L_00E10E;
	default: __builtin_trap();
	}
L_00E10E:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(14); uint32_t v = ld8(eaS); set_b(&ctx->d[2], v); cc_logic8(ctx, v); } hle_insn(ctx, 12);  /* 00E10E moveb %fp@(14),%d2 */
	{ set_w(&ctx->d[2], (uint32_t)(int32_t)(int8_t)ctx->d[2]); cc_logic16(ctx, ctx->d[2] & 0xFFFFu); } hle_insn(ctx, 4);  /* 00E112 extw %d2 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(10); uint32_t v = ld8(eaS); set_b(&ctx->d[3], v); cc_logic8(ctx, v); } hle_insn(ctx, 12);  /* 00E114 moveb %fp@(10),%d3 */
L_00E118:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(11); uint32_t v = alu(ctx, OP_ADD, ld8(eaD), (ctx->d[3] & 0xFFu), 8, 1); st8(eaD, v); } hle_insn(ctx, 16);  /* 00E118 addb %d3,%fp@(11) */
	if (COND_CC) { hle_insn(ctx, 10); goto L_00E122; } hle_insn(ctx, 8);  /* 00E11C bccs e122 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = alu(ctx, OP_ADD, ld16(eaD), (ctx->d[2] & 0xFFFFu), 16, 1); st16(eaD, v); } hle_insn(ctx, 16);  /* 00E11E addw %d2,%fp@(2) */
L_00E122:
	{ uint16_t v = (uint16_t)ctx->d[4] - 1; set_w(&ctx->d[4], v); if (v != 0xFFFF) { hle_insn(ctx, 10); goto L_00E118; } hle_insn(ctx, 14); }  /* 00E122 dbf %d4,e118 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(16); uint32_t v = ld8(eaS); set_b(&ctx->d[0], v); cc_logic8(ctx, v); } hle_insn(ctx, 12);  /* 00E126 moveb %fp@(16),%d0 */
	if (COND_MI) { hle_insn(ctx, 10); goto L_00E140; } hle_insn(ctx, 8);  /* 00E12A bmis e140 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(10); uint32_t v = alu(ctx, OP_SUB, ld8(eaD), (ctx->d[0] & 0xFFu), 8, 1); st8(eaD, v); } hle_insn(ctx, 16);  /* 00E12C subb %d0,%fp@(10) */
	if (COND_CC) { hle_insn(ctx, 10); goto L_00E144; } hle_insn(ctx, 8);  /* 00E130 bccs e144 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(14); uint32_t v = alu(ctx, OP_SUB, 0, ld8(eaD), 8, 1); st8(eaD, v); } hle_insn(ctx, 16);  /* 00E132 negb %fp@(14) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(16); uint32_t v = alu(ctx, OP_SUB, 0, ld8(eaD), 8, 1); st8(eaD, v); } hle_insn(ctx, 16);  /* 00E136 negb %fp@(16) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(10); st8(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 16);  /* 00E13A clrb %fp@(10) */
	hle_insn(ctx, 10); goto L_00E144;  /* 00E13E bras e144 */
L_00E140:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(10); uint32_t v = alu(ctx, OP_SUB, ld8(eaD), (ctx->d[0] & 0xFFu), 8, 1); st8(eaD, v); } hle_insn(ctx, 16);  /* 00E140 subb %d0,%fp@(10) */
L_00E144:
	hle_jump(ctx, 0x00E144); return;  /* 00E144 rts */
}

HLE_RANGE_PURE(0x00E10E, Auto_00E10E, 200, 0x00E144) { pc_run_00E10E(ctx, 0x00E10E); }
static void pc_run_00E146(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00E146: goto L_00E146;
	default: __builtin_trap();
	}
L_00E146:
	{ uint32_t eaS = 0xFFFFA0EEu; uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00E146 movew ffffa0ee,%d0 */
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0x3u) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00E14C andiw #3,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_00E162; } hle_insn(ctx, 8);  /* 00E150 bnes e162 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(8); uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00E152 movew %fp@(8),%d0 */
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0xCu, 16, 0); } hle_insn(ctx, 8);  /* 00E156 cmpiw #12,%d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_00E162; } hle_insn(ctx, 8);  /* 00E15A beqs e162 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), 0x1u, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 4);  /* 00E15C addqw #1,%d0 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(8); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00E15E movew %d0,%fp@(8) */
L_00E162:
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 00E162 braw b3be */
}

HLE_RANGE_PURE(0x00E146, Auto_00E146, 200, 0x00B3BE) { pc_run_00E146(ctx, 0x00E146); }
static void pc_run_00E254(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00E258: goto L_00E258;
	case 0x00E25E: goto L_00E25E;
	case 0x00E266: goto L_00E266;
	default: __builtin_trap();
	}
L_00E254:
	hle_jump(ctx, 0x00E254); return;  /* 00E254 jsr %pc@(12316) */
L_00E258:
	{ ctx->d[0] = 0xFFFFFFD8u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 00E258 moveq #-40,%d0 */
	hle_jump(ctx, 0x00E25A); return;  /* 00E25A jsr %pc@(12332) */
L_00E25E:
	{ uint32_t v = 0x190u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00E25E movew #400,%d1 */
	hle_jump(ctx, 0x00E262); return;  /* 00E262 jsr %pc@(13228) */
L_00E266:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = alu(ctx, OP_SUB, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 16);  /* 00E266 subqw #1,%fp@(36) */
	if (COND_NE) { hle_insn(ctx, 10); goto L_00E27A; } hle_insn(ctx, 8);  /* 00E26A bnes e27a */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0xE27Eu; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 00E26C movel #57982,%fp@(32) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = 0x5Au; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00E274 movew #90,%fp@(36) */
L_00E27A:
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 00E27A braw b3be */
}

HLE_RANGE_PURE(0x00E258, Auto_00E258, 200, 0x00E25A) { pc_run_00E254(ctx, 0x00E258); }
HLE_RANGE_PURE(0x00E25E, Auto_00E25E, 200, 0x00E262) { pc_run_00E254(ctx, 0x00E25E); }
HLE_RANGE_PURE(0x00E266, Auto_00E266, 200, 0x00B3BE) { pc_run_00E254(ctx, 0x00E266); }
static void pc_run_00E27E(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00E282: goto L_00E282;
	case 0x00E28C: goto L_00E28C;
	case 0x00E294: goto L_00E294;
	default: __builtin_trap();
	}
L_00E27E:
	hle_jump(ctx, 0x00E27E); return;  /* 00E27E jsr %pc@(12316) */
L_00E282:
	{ ctx->d[0] = 0xFFFFFFD8u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 00E282 moveq #-40,%d0 */
	hle_jump(ctx, 0x00E284); return;  /* 00E284 jsr %pc@(12332) */
L_00E288:
	hle_jump(ctx, 0x00E288); return;  /* 00E288 jsr %pc@(d236) */
L_00E28C:
	{ uint32_t v = 0x190u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00E28C movew #400,%d1 */
	hle_jump(ctx, 0x00E290); return;  /* 00E290 jsr %pc@(13228) */
L_00E294:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(15); cc_logic8(ctx, ld8(eaD)); } hle_insn(ctx, 12);  /* 00E294 tstb %fp@(15) */
	if (COND_MI) { hle_insn(ctx, 10); goto L_00E2A8; } hle_insn(ctx, 8);  /* 00E298 bmis e2a8 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0xE2ACu; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 00E29A movel #58028,%fp@(32) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = 0x5Au; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00E2A2 movew #90,%fp@(36) */
L_00E2A8:
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 00E2A8 braw b3be */
}

HLE_RANGE_PURE(0x00E282, Auto_00E282, 200, 0x00E284) { pc_run_00E27E(ctx, 0x00E282); }
HLE_RANGE_PURE(0x00E28C, Auto_00E28C, 200, 0x00E290) { pc_run_00E27E(ctx, 0x00E28C); }
HLE_RANGE_PURE(0x00E294, Auto_00E294, 200, 0x00B3BE) { pc_run_00E27E(ctx, 0x00E294); }
static void pc_run_00E2AC(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00E2B0: goto L_00E2B0;
	case 0x00E2BA: goto L_00E2BA;
	case 0x00E2C2: goto L_00E2C2;
	default: __builtin_trap();
	}
L_00E2AC:
	hle_jump(ctx, 0x00E2AC); return;  /* 00E2AC jsr %pc@(12316) */
L_00E2B0:
	{ ctx->d[0] = 0xFFFFFFD8u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 00E2B0 moveq #-40,%d0 */
	hle_jump(ctx, 0x00E2B2); return;  /* 00E2B2 jsr %pc@(12332) */
L_00E2B6:
	hle_jump(ctx, 0x00E2B6); return;  /* 00E2B6 jsr %pc@(b4a6) */
L_00E2BA:
	{ uint32_t v = 0x190u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00E2BA movew #400,%d1 */
	hle_jump(ctx, 0x00E2BE); return;  /* 00E2BE jsr %pc@(13228) */
L_00E2C2:
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 00E2C2 braw b3be */
}

HLE_RANGE_PURE(0x00E2B0, Auto_00E2B0, 200, 0x00E2B2) { pc_run_00E2AC(ctx, 0x00E2B0); }
HLE_RANGE_PURE(0x00E2BA, Auto_00E2BA, 200, 0x00E2BE) { pc_run_00E2AC(ctx, 0x00E2BA); }
HLE_RANGE_PURE(0x00E2C2, Auto_00E2C2, 200, 0x00B3BE) { pc_run_00E2AC(ctx, 0x00E2C2); }
static void pc_run_00ED04(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00ED04: goto L_00ED04;
	case 0x00ED14: goto L_00ED14;
	case 0x00ED24: goto L_00ED24;
	default: __builtin_trap();
	}
L_00ED04:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(8); alu(ctx, OP_SUB, ld16(eaD), 0xFu, 16, 0); } hle_insn(ctx, 16);  /* 00ED04 cmpiw #15,%fp@(8) */
	if (COND_GT) { hle_insn(ctx, 10); goto L_00ED14; } hle_insn(ctx, 8);  /* 00ED0A bgts ed14 */
	{ uint32_t v = 0x1B3u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00ED0C movew #435,%d1 */
	hle_jump(ctx, 0x00ED10); return;  /* 00ED10 bsrw 13228 */
L_00ED14:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(8); alu(ctx, OP_SUB, ld16(eaD), 0xAu, 16, 0); } hle_insn(ctx, 16);  /* 00ED14 cmpiw #10,%fp@(8) */
	if (COND_GT) { hle_insn(ctx, 10); goto L_00ED24; } hle_insn(ctx, 8);  /* 00ED1A bgts ed24 */
	{ uint32_t v = 0x1B4u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00ED1C movew #436,%d1 */
	hle_jump(ctx, 0x00ED20); return;  /* 00ED20 bsrw 13228 */
L_00ED24:
	{ uint32_t v = 0x1B2u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00ED24 movew #434,%d1 */
	hle_jump(ctx, 0x00ED28); return;  /* 00ED28 bsrw 13228 */
L_00ED2C:
	hle_jump(ctx, 0x00ED2C); return;  /* 00ED2C rts */
}

HLE_RANGE_PURE(0x00ED04, Auto_00ED04, 200, 0x00ED10, 0x00ED20, 0x00ED28) { pc_run_00ED04(ctx, 0x00ED04); }
HLE_RANGE_PURE(0x00ED14, Auto_00ED14, 200, 0x00ED20, 0x00ED28) { pc_run_00ED04(ctx, 0x00ED14); }
HLE_RANGE_PURE(0x00ED24, Auto_00ED24, 200, 0x00ED28) { pc_run_00ED04(ctx, 0x00ED24); }
static void pc_run_00F370(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00F378: goto L_00F378;
	case 0x00F396: goto L_00F396;
	default: __builtin_trap();
	}
L_00F370:
	hle_jump(ctx, 0x00F370); return;  /* 00F370 jsr %pc@(d232) */
L_00F374:
	hle_jump(ctx, 0x00F374); return;  /* 00F374 jsr %pc@(e10c) */
L_00F378:
	{ ctx->d[0] = 0x00000000u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 00F378 moveq #0,%d0 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(10); uint32_t v = ld8(eaS); set_b(&ctx->d[0], v); cc_logic8(ctx, v); } hle_insn(ctx, 12);  /* 00F37A moveb %fp@(10),%d0 */
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0xF0u, 16, 0); } hle_insn(ctx, 8);  /* 00F37E cmpiw #240,%d0 */
	if (COND_LE) { hle_insn(ctx, 10); goto L_00F38E; } hle_insn(ctx, 8);  /* 00F382 bles f38e */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(16); uint32_t v = alu(ctx, OP_SUB, 0, ld8(eaD), 8, 1); st8(eaD, v); } hle_insn(ctx, 16);  /* 00F384 negb %fp@(16) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(10); uint32_t v = 0xF0u; st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 16);  /* 00F388 moveb #-16,%fp@(10) */
L_00F38E:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(7); uint32_t v = alu(ctx, OP_SUB, ld8(eaD), 0x1u, 8, 1); st8(eaD, v); } hle_insn(ctx, 16);  /* 00F38E subqb #1,%fp@(7) */
	hle_jump(ctx, 0x00F392); return;  /* 00F392 jsr %pc@(f452) */
L_00F396:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(4); alu(ctx, OP_SUB, ld16(eaD), 0xC8u, 16, 0); } hle_insn(ctx, 16);  /* 00F396 cmpiw #200,%fp@(4) */
	if (COND_GT) { hle_insn(ctx, 10); goto L_00F3A8; } hle_insn(ctx, 12);  /* 00F39C bgtw f3a8 */
L_00F3A0:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = alu(ctx, OP_SUB, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 16);  /* 00F3A0 subqw #1,%fp@(36) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 00F3A4 braw b3be */
L_00F3A8:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = 0x1Fu; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00F3A8 movew #31,%fp@(36) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0xF3C0u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 00F3AE movel #62400,%fp@(32) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(20); uint32_t v = 0xFFFFu; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00F3B6 movew #-1,%fp@(20) */
	hle_insn(ctx, 10); goto L_00F3A0;  /* 00F3BC braw f3a0 */
}

HLE_RANGE_PURE(0x00F378, Auto_00F378, 200, 0x00F392) { pc_run_00F370(ctx, 0x00F378); }
HLE_RANGE_PURE(0x00F396, Auto_00F396, 200, 0x00B3BE) { pc_run_00F370(ctx, 0x00F396); }
static void pc_run_00F3C0(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00F3C0: goto L_00F3C0;
	case 0x00F3DE: goto L_00F3DE;
	default: __builtin_trap();
	}
L_00F3C0:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = alu(ctx, OP_SUB, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 16);  /* 00F3C0 subqw #1,%fp@(36) */
	if (COND_NE) { hle_insn(ctx, 10); goto L_00F3D4; } hle_insn(ctx, 8);  /* 00F3C4 bnes f3d4 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0xF3E2u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 00F3C6 movel #62434,%fp@(32) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = 0x20u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00F3CE movew #32,%fp@(36) */
L_00F3D4:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(4); uint32_t v = alu(ctx, OP_SUB, ld16(eaD), 0x5u, 16, 1); st16(eaD, v); } hle_insn(ctx, 20);  /* 00F3D4 subiw #5,%fp@(4) */
	hle_jump(ctx, 0x00F3DA); return;  /* 00F3DA jsr %pc@(f452) */
L_00F3DE:
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 00F3DE braw b3be */
}

HLE_RANGE_PURE(0x00F3C0, Auto_00F3C0, 200, 0x00F3DA) { pc_run_00F3C0(ctx, 0x00F3C0); }
HLE_RANGE_PURE(0x00F3DE, Auto_00F3DE, 200, 0x00B3BE) { pc_run_00F3C0(ctx, 0x00F3DE); }
static void pc_run_00F3E2(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00F3E2: goto L_00F3E2;
	case 0x00F3FC: goto L_00F3FC;
	case 0x00F41E: goto L_00F41E;
	case 0x00F42A: goto L_00F42A;
	default: __builtin_trap();
	}
L_00F3E2:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00F3E2 movew %fp@(36),%d0 */
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0x7u) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00F3E6 andiw #7,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_00F3FC; } hle_insn(ctx, 8);  /* 00F3EA bnes f3fc */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(2); ctx->d[0] = (uint32_t)(int32_t)(int16_t)ld16(eaS + 0); ctx->d[1] = (uint32_t)(int32_t)(int16_t)ld16(eaS + 2); } hle_insn(ctx, 24);  /* 00F3EC movemw %fp@(2),%d0-%d1 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(6); uint32_t v = ld16(eaS); set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00F3F2 movew %fp@(6),%d2 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[2] & 0xFFFFu), 0x1u, 16, 1); set_w(&ctx->d[2], v); } hle_insn(ctx, 4);  /* 00F3F6 subqw #1,%d2 */
	hle_jump(ctx, 0x00F3F8); return;  /* 00F3F8 jsr %pc@(124d2) */
L_00F3FC:
	{ uint32_t eaS = 0xFFFF9EDEu; uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00F3FC movew ffff9ede,%d0 */
	{ uint32_t eaS = 0xFFFF9EE0u; ctx->a[1] = eaS; } hle_insn(ctx, 12);  /* 00F402 lea ffff9ee0,%a1 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t eaD = ctx->a[1] + (uint32_t)(int32_t)(0) + (uint32_t)(int32_t)(int16_t)ctx->d[0]; uint32_t v = ld32(eaS); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 30);  /* 00F408 movel %fp@(2),%a1@(0,%d0:w) */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), 0x4u, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 4);  /* 00F40E addqw #4,%d0 */
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0x7Fu) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00F410 andiw #127,%d0 */
	{ uint32_t eaD = 0xFFFF9EDEu; uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00F414 movew %d0,ffff9ede */
	hle_jump(ctx, 0x00F41A); return;  /* 00F41A jsr %pc@(f4b0) */
L_00F41E:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = alu(ctx, OP_SUB, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 16);  /* 00F41E subqw #1,%fp@(36) */
	if (COND_NE) { hle_insn(ctx, 10); goto L_00F432; } hle_insn(ctx, 8);  /* 00F422 bnes f432 */
	hle_jump(ctx, 0x00F424); return;  /* 00F424 jsr 908a */
L_00F42A:
	{ uint32_t eaD = ctx->a[6]; st16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 12);  /* 00F42A clrw %fp@ */
	{ uint32_t eaD = 0xFFFF80DAu; uint32_t v = alu(ctx, OP_SUB, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 20);  /* 00F42C subqw #1,ffff80da */
L_00F432:
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 00F432 braw b3be */
}

HLE_RANGE_PURE(0x00F3E2, Auto_00F3E2, 200, 0x00F3F8, 0x00F41A) { pc_run_00F3E2(ctx, 0x00F3E2); }
HLE_RANGE_PURE(0x00F3FC, Auto_00F3FC, 200, 0x00F41A) { pc_run_00F3E2(ctx, 0x00F3FC); }
HLE_RANGE_PURE(0x00F41E, Auto_00F41E, 200, 0x00B3BE, 0x00F424) { pc_run_00F3E2(ctx, 0x00F41E); }
HLE_RANGE_PURE(0x00F42A, Auto_00F42A, 200, 0x00B3BE) { pc_run_00F3E2(ctx, 0x00F42A); }
static void pc_run_00F5C0(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00F5C0: goto L_00F5C0;
	default: __builtin_trap();
	}
L_00F5C0:
	{ uint32_t v = 0x215u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00F5C0 movew #533,%d1 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(38); cc_logic16(ctx, ld16(eaD)); } hle_insn(ctx, 12);  /* 00F5C4 tstw %fp@(38) */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_00F5CE; } hle_insn(ctx, 8);  /* 00F5C8 beqs f5ce */
	{ uint32_t v = 0x249u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00F5CA movew #585,%d1 */
L_00F5CE:
	hle_insn(ctx, 10); hle_jump(ctx, 0x00F648); return;  /* 00F5CE braw f648 */
}

HLE_RANGE_PURE(0x00F5C0, Auto_00F5C0, 200, 0x00F648) { pc_run_00F5C0(ctx, 0x00F5C0); }
static void pc_run_00F648(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00F64C: goto L_00F64C;
	case 0x00F656: goto L_00F656;
	default: __builtin_trap();
	}
L_00F648:
	hle_jump(ctx, 0x00F648); return;  /* 00F648 jsr %pc@(12316) */
L_00F64C:
	{ ctx->d[0] = 0xFFFFFFF4u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 00F64C moveq #-12,%d0 */
	hle_jump(ctx, 0x00F64E); return;  /* 00F64E jsr %pc@(12332) */
L_00F652:
	hle_jump(ctx, 0x00F652); return;  /* 00F652 bsrw 13228 */
L_00F656:
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 00F656 braw b3be */
}

HLE_RANGE_PURE(0x00F64C, Auto_00F64C, 200, 0x00F64E) { pc_run_00F648(ctx, 0x00F64C); }
HLE_RANGE_PURE(0x00F656, Auto_00F656, 200, 0x00B3BE) { pc_run_00F648(ctx, 0x00F656); }
static void pc_run_00F83E(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00F83E: goto L_00F83E;
	case 0x00F868: goto L_00F868;
	default: __builtin_trap();
	}
L_00F83E:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(4); uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00F83E movew %fp@(4),%d0 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), 0x8u, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 4);  /* 00F842 addqw #8,%d0 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(40); alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), ld16(eaS), 16, 0); } hle_insn(ctx, 12);  /* 00F844 cmpw %fp@(40),%d0 */
	if (COND_LT) { hle_insn(ctx, 10); goto L_00F85C; } hle_insn(ctx, 8);  /* 00F848 blts f85c */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0xF86Cu; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 00F84A movel #63596,%fp@(32) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = 0x17u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00F852 movew #23,%fp@(36) */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(40); uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00F858 movew %fp@(40),%d0 */
L_00F85C:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(4); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00F85C movew %d0,%fp@(4) */
	{ uint32_t v = 0x232u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00F860 movew #562,%d1 */
	hle_jump(ctx, 0x00F864); return;  /* 00F864 bsrw 13228 */
L_00F868:
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 00F868 braw b3be */
}

HLE_RANGE_PURE(0x00F83E, Auto_00F83E, 200, 0x00F864) { pc_run_00F83E(ctx, 0x00F83E); }
HLE_RANGE_PURE(0x00F868, Auto_00F868, 200, 0x00B3BE) { pc_run_00F83E(ctx, 0x00F868); }
static void pc_run_00FB64(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00FB68: goto L_00FB68;
	case 0x00FB72: goto L_00FB72;
	default: __builtin_trap();
	}
L_00FB64:
	hle_jump(ctx, 0x00FB64); return;  /* 00FB64 jsr %pc@(12316) */
L_00FB68:
	{ ctx->d[0] = 0xFFFFFFE0u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 00FB68 moveq #-32,%d0 */
	hle_jump(ctx, 0x00FB6A); return;  /* 00FB6A jsr %pc@(12332) */
L_00FB6E:
	hle_jump(ctx, 0x00FB6E); return;  /* 00FB6E bsrw 13228 */
L_00FB72:
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 00FB72 braw b3be */
}

HLE_RANGE_PURE(0x00FB68, Auto_00FB68, 200, 0x00FB6A) { pc_run_00FB64(ctx, 0x00FB68); }
HLE_RANGE_PURE(0x00FB72, Auto_00FB72, 200, 0x00B3BE) { pc_run_00FB64(ctx, 0x00FB72); }
static void pc_run_00FB8E(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00FB8E: goto L_00FB8E;
	default: __builtin_trap();
	}
L_00FB8E:
	{ uint32_t v = 0x287u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00FB8E movew #647,%d1 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00FB98); return;  /* 00FB92 bras fb98 */
}

HLE_RANGE_PURE(0x00FB8E, Auto_00FB8E, 200, 0x00FB98) { pc_run_00FB8E(ctx, 0x00FB8E); }
static void pc_run_00FB94(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00FB94: goto L_00FB94;
	default: __builtin_trap();
	}
L_00FB94:
	{ uint32_t v = 0x286u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); hle_jump(ctx, 0x00FB98); return;  /* 00FB94 movew #646,%d1 */
}

HLE_RANGE_PURE(0x00FB94, Auto_00FB94, 200, 0x00FB98) { pc_run_00FB94(ctx, 0x00FB94); }
static void pc_run_00FB98(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00FB9C: goto L_00FB9C;
	case 0x00FBA6: goto L_00FBA6;
	default: __builtin_trap();
	}
L_00FB98:
	hle_jump(ctx, 0x00FB98); return;  /* 00FB98 jsr %pc@(12316) */
L_00FB9C:
	{ ctx->d[0] = 0xFFFFFFE0u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 00FB9C moveq #-32,%d0 */
	hle_jump(ctx, 0x00FB9E); return;  /* 00FB9E jsr %pc@(12332) */
L_00FBA2:
	hle_jump(ctx, 0x00FBA2); return;  /* 00FBA2 bsrw 13228 */
L_00FBA6:
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 00FBA6 braw b3be */
}

HLE_RANGE_PURE(0x00FB9C, Auto_00FB9C, 200, 0x00FB9E) { pc_run_00FB98(ctx, 0x00FB9C); }
HLE_RANGE_PURE(0x00FBA6, Auto_00FBA6, 200, 0x00B3BE) { pc_run_00FB98(ctx, 0x00FBA6); }
static void pc_run_00FC3A(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00FC3A: goto L_00FC3A;
	case 0x00FC4C: goto L_00FC4C;
	case 0x00FC58: goto L_00FC58;
	case 0x00FC5E: goto L_00FC5E;
	case 0x00FC66: goto L_00FC66;
	default: __builtin_trap();
	}
L_00FC3A:
	{ uint32_t eaD = ctx->a[6];  unsigned b = 6; uint32_t m = 1u << b; uint8_t v = ld8(eaD); cc_set(ctx, CC_Z, (v & m) ? 0 : CC_Z);  } hle_insn(ctx, 12);  /* 00FC3A btst #6,%fp@ */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_00FC4E; } hle_insn(ctx, 12);  /* 00FC3E beqw fc4e */
	{ uint32_t v = (ctx->d[5] & 0xFFFFu); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 00FC42 movew %d5,%d1 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0x33Au, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 00FC44 addiw #826,%d1 */
	hle_jump(ctx, 0x00FC48); return;  /* 00FC48 bsrw 13228 */
L_00FC4C:
	hle_insn(ctx, 10); goto L_00FC58;  /* 00FC4C bras fc58 */
L_00FC4E:
	{ uint32_t v = (ctx->d[5] & 0xFFFFu); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 00FC4E movew %d5,%d1 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0x332u, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 00FC50 addiw #818,%d1 */
	hle_jump(ctx, 0x00FC54); return;  /* 00FC54 bsrw 13228 */
L_00FC58:
	{ ctx->d[0] = 0xFFFFFFD8u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 00FC58 moveq #-40,%d0 */
	hle_jump(ctx, 0x00FC5A); return;  /* 00FC5A jsr %pc@(12332) */
L_00FC5E:
	{ uint32_t v = 0x168u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00FC5E movew #360,%d0 */
	hle_jump(ctx, 0x00FC62); return;  /* 00FC62 jsr %pc@(12342) */
L_00FC66:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = alu(ctx, OP_SUB, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 16);  /* 00FC66 subqw #1,%fp@(36) */
	if (COND_NE) { hle_insn(ctx, 10); goto L_00FC74; } hle_insn(ctx, 8);  /* 00FC6A bnes fc74 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0xFE8Eu; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 00FC6C movel #65166,%fp@(32) */
L_00FC74:
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 00FC74 braw b3be */
}

HLE_RANGE_PURE(0x00FC3A, Auto_00FC3A, 200, 0x00FC48, 0x00FC54) { pc_run_00FC3A(ctx, 0x00FC3A); }
HLE_RANGE_PURE(0x00FC4C, Auto_00FC4C, 200, 0x00FC5A) { pc_run_00FC3A(ctx, 0x00FC4C); }
HLE_RANGE_PURE(0x00FC58, Auto_00FC58, 200, 0x00FC5A) { pc_run_00FC3A(ctx, 0x00FC58); }
HLE_RANGE_PURE(0x00FC5E, Auto_00FC5E, 200, 0x00FC62) { pc_run_00FC3A(ctx, 0x00FC5E); }
HLE_RANGE_PURE(0x00FC66, Auto_00FC66, 200, 0x00B3BE) { pc_run_00FC3A(ctx, 0x00FC66); }
static void pc_run_00FD0C(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00FD0C: goto L_00FD0C;
	case 0x00FD18: goto L_00FD18;
	default: __builtin_trap();
	}
L_00FD0C:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0xFBAAu; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 00FD0C movel #64426,%fp@(32) */
	hle_jump(ctx, 0x00FD14); return;  /* 00FD14 jsr %pc@(881c) */
L_00FD18:
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0x3Fu) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00FD18 andiw #63,%d0 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), 0x50u, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 8);  /* 00FD1C addiw #80,%d0 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00FD20 movew %d0,%fp@(36) */
	hle_jump(ctx, 0x00FD24); return;  /* 00FD24 rts */
}

HLE_RANGE_PURE(0x00FD0C, Auto_00FD0C, 200, 0x00FD14) { pc_run_00FD0C(ctx, 0x00FD0C); }
HLE_RANGE_PURE(0x00FD18, Auto_00FD18, 200, 0x00FD24) { pc_run_00FD0C(ctx, 0x00FD18); }
static void pc_run_00FFC0(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x00FFC0: goto L_00FFC0;
	case 0x00FFCC: goto L_00FFCC;
	default: __builtin_trap();
	}
L_00FFC0:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0xFF12u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 00FFC0 movel #65298,%fp@(32) */
	hle_jump(ctx, 0x00FFC8); return;  /* 00FFC8 jsr %pc@(881c) */
L_00FFCC:
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0x3Fu) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00FFCC andiw #63,%d0 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), 0x50u, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 8);  /* 00FFD0 addiw #80,%d0 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00FFD4 movew %d0,%fp@(36) */
	hle_jump(ctx, 0x00FFD8); return;  /* 00FFD8 rts */
}

HLE_RANGE_PURE(0x00FFC0, Auto_00FFC0, 200, 0x00FFC8) { pc_run_00FFC0(ctx, 0x00FFC0); }
HLE_RANGE_PURE(0x00FFCC, Auto_00FFCC, 200, 0x00FFD8) { pc_run_00FFC0(ctx, 0x00FFCC); }
static void pc_run_010126(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x01012E: goto L_01012E;
	default: __builtin_trap();
	}
L_010126:
	hle_jump(ctx, 0x010126); return;  /* 010126 jsr %pc@(13228) */
L_01012A:
	hle_jump(ctx, 0x01012A); return;  /* 01012A jsr %pc@(12316) */
L_01012E:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 01012E movew %fp@(2),%d0 */
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0xFFECu, 16, 0); } hle_insn(ctx, 8);  /* 010132 cmpiw #-20,%d0 */
	if (COND_GT) { hle_insn(ctx, 10); goto L_010146; } hle_insn(ctx, 8);  /* 010136 bgts 10146 */
	{ uint32_t eaD = ctx->a[6]; st16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 12);  /* 010138 clrw %fp@ */
	{ uint32_t eaD = 0xFFFF80DAu; uint32_t v = alu(ctx, OP_SUB, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 20);  /* 01013A subqw #1,ffff80da */
	{ uint32_t eaD = 0xFFFF9EDAu; st32(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 28);  /* 010140 clrl ffff9eda */
L_010146:
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 010146 braw b3be */
}

HLE_RANGE_PURE(0x01012E, Auto_01012E, 200, 0x00B3BE) { pc_run_010126(ctx, 0x01012E); }
static void pc_run_0102A6(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x0102A6: goto L_0102A6;
	case 0x0102C2: goto L_0102C2;
	default: __builtin_trap();
	}
L_0102A6:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(2); alu(ctx, OP_SUB, ld16(eaD), 0xFFE2u, 16, 0); } hle_insn(ctx, 16);  /* 0102A6 cmpiw #-30,%fp@(2) */
	if (COND_LT) { hle_insn(ctx, 10); goto L_0102B6; } hle_insn(ctx, 8);  /* 0102AC blts 102b6 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(2); alu(ctx, OP_SUB, ld16(eaD), 0x15Eu, 16, 0); } hle_insn(ctx, 16);  /* 0102AE cmpiw #350,%fp@(2) */
	if (COND_LE) { hle_insn(ctx, 10); goto L_0102CE; } hle_insn(ctx, 8);  /* 0102B4 bles 102ce */
L_0102B6:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0x1036Eu; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 0102B6 movel #66414,%fp@(32) */
	hle_jump(ctx, 0x0102BE); return;  /* 0102BE jsr %pc@(881c) */
L_0102C2:
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0x3Fu) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0102C2 andiw #63,%d0 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), 0x64u, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 8);  /* 0102C6 addiw #100,%d0 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 0102CA movew %d0,%fp@(36) */
L_0102CE:
	hle_jump(ctx, 0x0102CE); return;  /* 0102CE rts */
}

HLE_RANGE_PURE(0x0102A6, Auto_0102A6, 200, 0x0102BE, 0x0102CE) { pc_run_0102A6(ctx, 0x0102A6); }
HLE_RANGE_PURE(0x0102C2, Auto_0102C2, 200, 0x0102CE) { pc_run_0102A6(ctx, 0x0102C2); }
static void pc_run_010408(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x010408: goto L_010408;
	case 0x010414: goto L_010414;
	default: __builtin_trap();
	}
L_010408:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0x10238u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 010408 movel #66104,%fp@(32) */
	hle_jump(ctx, 0x010410); return;  /* 010410 jsr %pc@(881c) */
L_010414:
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0x3Fu) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 010414 andiw #63,%d0 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), 0x50u, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 8);  /* 010418 addiw #80,%d0 */
	{ uint32_t eaD = ctx->a[6];  unsigned b = 6; uint32_t m = 1u << b; uint8_t v = ld8(eaD); cc_set(ctx, CC_Z, (v & m) ? 0 : CC_Z);  } hle_insn(ctx, 12);  /* 01041C btst #14,%fp@ */
	if (COND_NE) { hle_insn(ctx, 10); goto L_010426; } hle_insn(ctx, 8);  /* 010420 bnes 10426 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x32u, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 8);  /* 010422 subiw #50,%d0 */
L_010426:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 010426 movew %d0,%fp@(36) */
	hle_jump(ctx, 0x01042A); return;  /* 01042A rts */
}

HLE_RANGE_PURE(0x010408, Auto_010408, 200, 0x010410) { pc_run_010408(ctx, 0x010408); }
HLE_RANGE_PURE(0x010414, Auto_010414, 200, 0x01042A) { pc_run_010408(ctx, 0x010414); }
static void pc_run_0104EE(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x0104EE: goto L_0104EE;
	case 0x01050A: goto L_01050A;
	default: __builtin_trap();
	}
L_0104EE:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(2); alu(ctx, OP_SUB, ld16(eaD), 0xFFE2u, 16, 0); } hle_insn(ctx, 16);  /* 0104EE cmpiw #-30,%fp@(2) */
	if (COND_LT) { hle_insn(ctx, 10); goto L_0104FE; } hle_insn(ctx, 8);  /* 0104F4 blts 104fe */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(2); alu(ctx, OP_SUB, ld16(eaD), 0x15Eu, 16, 0); } hle_insn(ctx, 16);  /* 0104F6 cmpiw #350,%fp@(2) */
	if (COND_LE) { hle_insn(ctx, 10); goto L_010516; } hle_insn(ctx, 8);  /* 0104FC bles 10516 */
L_0104FE:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0x10582u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 0104FE movel #66946,%fp@(32) */
	hle_jump(ctx, 0x010506); return;  /* 010506 jsr %pc@(881c) */
L_01050A:
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0x3Fu) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 01050A andiw #63,%d0 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), 0x64u, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 8);  /* 01050E addiw #100,%d0 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 010512 movew %d0,%fp@(36) */
L_010516:
	hle_jump(ctx, 0x010516); return;  /* 010516 rts */
}

HLE_RANGE_PURE(0x0104EE, Auto_0104EE, 200, 0x010506, 0x010516) { pc_run_0104EE(ctx, 0x0104EE); }
HLE_RANGE_PURE(0x01050A, Auto_01050A, 200, 0x010516) { pc_run_0104EE(ctx, 0x01050A); }
static void pc_run_0107AC(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x0107B0: goto L_0107B0;
	default: __builtin_trap();
	}
L_0107AC:
	hle_jump(ctx, 0x0107AC); return;  /* 0107AC jsr %pc@(10812) */
L_0107B0:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(2); alu(ctx, OP_SUB, ld16(eaD), 0xA0u, 16, 0); } hle_insn(ctx, 16);  /* 0107B0 cmpiw #160,%fp@(2) */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0107C6; } hle_insn(ctx, 8);  /* 0107B6 bnes 107c6 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = 0x96u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 0107B8 movew #150,%fp@(36) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0x107DAu; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 0107BE movel #67546,%fp@(32) */
L_0107C6:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(2); alu(ctx, OP_SUB, ld16(eaD), 0x154u, 16, 0); } hle_insn(ctx, 16);  /* 0107C6 cmpiw #340,%fp@(2) */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0107D6; } hle_insn(ctx, 8);  /* 0107CC bnes 107d6 */
	{ uint32_t eaD = ctx->a[6]; st16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 12);  /* 0107CE clrw %fp@ */
	{ uint32_t eaD = 0xFFFF80DAu; uint32_t v = alu(ctx, OP_SUB, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 20);  /* 0107D0 subqw #1,ffff80da */
L_0107D6:
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 0107D6 braw b3be */
}

HLE_RANGE_PURE(0x0107B0, Auto_0107B0, 200, 0x00B3BE) { pc_run_0107AC(ctx, 0x0107B0); }
static void pc_run_010812(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x010812: goto L_010812;
	case 0x01081E: goto L_01081E;
	default: __builtin_trap();
	}
L_010812:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = alu(ctx, OP_SUB, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 16);  /* 010812 subqw #1,%fp@(36) */
	hle_jump(ctx, 0x010816); return;  /* 010816 jsr %pc@(b4a6) */
L_01081A:
	hle_jump(ctx, 0x01081A); return;  /* 01081A jsr %pc@(12316) */
L_01081E:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = ld16(eaS); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 01081E movew %fp@(36),%d1 */
	{ uint32_t v = ((ctx->d[1] & 0xFFFFu) & 0x38u) & 0xFFFFu; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 010822 andiw #56,%d1 */
	{ uint32_t v = shf(ctx, 'r', (ctx->d[1] & 0xFFFFu), 3, 16); set_w(&ctx->d[1], v); } hle_insn(ctx, 12);  /* 010826 lsrw #3,%d1 */
	{ uint32_t v = ((ctx->d[1] & 0xFFFFu) ^ 0x7u) & 0xFFFFu; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 010828 eoriw #7,%d1 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x010666); return;  /* 01082C braw 10666 */
}

HLE_RANGE_PURE(0x010812, Auto_010812, 200, 0x010816) { pc_run_010812(ctx, 0x010812); }
HLE_RANGE_PURE(0x01081E, Auto_01081E, 200, 0x010666) { pc_run_010812(ctx, 0x01081E); }
static void pc_run_01085C(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x01085C: goto L_01085C;
	case 0x010864: goto L_010864;
	default: __builtin_trap();
	}
L_01085C:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = alu(ctx, OP_SUB, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 16);  /* 01085C subqw #1,%fp@(36) */
	hle_jump(ctx, 0x010860); return;  /* 010860 jsr %pc@(10812) */
L_010864:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(2); alu(ctx, OP_SUB, ld16(eaD), 0xFFECu, 16, 0); } hle_insn(ctx, 16);  /* 010864 cmpiw #-20,%fp@(2) */
	if (COND_NE) { hle_insn(ctx, 10); goto L_01087A; } hle_insn(ctx, 8);  /* 01086A bnes 1087a */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = 0xC8u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 01086C movew #200,%fp@(36) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0x1087Eu; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 010872 movel #67710,%fp@(32) */
L_01087A:
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 01087A braw b3be */
}

HLE_RANGE_PURE(0x01085C, Auto_01085C, 200, 0x010860) { pc_run_01085C(ctx, 0x01085C); }
HLE_RANGE_PURE(0x010864, Auto_010864, 200, 0x00B3BE) { pc_run_01085C(ctx, 0x010864); }
static void pc_run_01087E(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x01087E: goto L_01087E;
	default: __builtin_trap();
	}
L_01087E:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = alu(ctx, OP_SUB, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 16);  /* 01087E subqw #1,%fp@(36) */
	if (COND_NE) { hle_insn(ctx, 10); goto L_01089C; } hle_insn(ctx, 8);  /* 010882 bnes 1089c */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(14); uint32_t v = alu(ctx, OP_SUB, 0, ld8(eaD), 8, 1); st8(eaD, v); } hle_insn(ctx, 16);  /* 010884 negb %fp@(14) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(10); uint32_t v = 0x3Cu; st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 16);  /* 010888 moveb #60,%fp@(10) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(4); uint32_t v = 0xD0u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 01088E movew #208,%fp@(4) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0x108A0u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 010894 movel #67744,%fp@(32) */
L_01089C:
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 01089C braw b3be */
}

HLE_RANGE_PURE(0x01087E, Auto_01087E, 200, 0x00B3BE) { pc_run_01087E(ctx, 0x01087E); }
static void pc_run_0108A0(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x0108A4: goto L_0108A4;
	default: __builtin_trap();
	}
L_0108A0:
	hle_jump(ctx, 0x0108A0); return;  /* 0108A0 jsr %pc@(10812) */
L_0108A4:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(2); alu(ctx, OP_SUB, ld16(eaD), 0xE9u, 16, 0); } hle_insn(ctx, 16);  /* 0108A4 cmpiw #233,%fp@(2) */
	if (COND_LT) { hle_insn(ctx, 10); goto L_0108B8; } hle_insn(ctx, 8);  /* 0108AA blts 108b8 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0x108CAu; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 0108AC movel #67786,%fp@(32) */
	{ uint32_t eaD = ctx->a[6];  unsigned b = 6; uint32_t m = 1u << b; uint8_t v = ld8(eaD); cc_set(ctx, CC_Z, (v & m) ? 0 : CC_Z); st8(eaD, v ^ m); } hle_insn(ctx, 16);  /* 0108B4 bchg #14,%fp@ */
L_0108B8:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 0108B8 movew %fp@(36),%d0 */
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0x3u) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0108BC andiw #3,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0108C6; } hle_insn(ctx, 8);  /* 0108C0 bnes 108c6 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(4); uint32_t v = alu(ctx, OP_SUB, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 16);  /* 0108C2 subqw #1,%fp@(4) */
L_0108C6:
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 0108C6 braw b3be */
}

HLE_RANGE_PURE(0x0108A4, Auto_0108A4, 200, 0x00B3BE) { pc_run_0108A0(ctx, 0x0108A4); }
static void pc_run_0108CA(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x0108CA: goto L_0108CA;
	case 0x0108D0: goto L_0108D0;
	default: __builtin_trap();
	}
L_0108CA:
	{ ctx->d[1] = 0x00000008u; cc_logic32(ctx, ctx->d[1]); } hle_insn(ctx, 4);  /* 0108CA moveq #8,%d1 */
	hle_jump(ctx, 0x0108CC); return;  /* 0108CC bsrw 10666 */
L_0108D0:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0x108DCu; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 0108D0 movel #67804,%fp@(32) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 0108D8 braw b3be */
}

HLE_RANGE_PURE(0x0108CA, Auto_0108CA, 200, 0x0108CC) { pc_run_0108CA(ctx, 0x0108CA); }
HLE_RANGE_PURE(0x0108D0, Auto_0108D0, 200, 0x00B3BE) { pc_run_0108CA(ctx, 0x0108D0); }
static void pc_run_0108DC(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x0108DC: goto L_0108DC;
	case 0x0108E4: goto L_0108E4;
	default: __builtin_trap();
	}
L_0108DC:
	{ uint32_t v = 0x2C9u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0108DC movew #713,%d1 */
	hle_jump(ctx, 0x0108E0); return;  /* 0108E0 jsr %pc@(13228) */
L_0108E4:
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 0108E4 braw b3be */
}

HLE_RANGE_PURE(0x0108DC, Auto_0108DC, 200, 0x0108E0) { pc_run_0108DC(ctx, 0x0108DC); }
HLE_RANGE_PURE(0x0108E4, Auto_0108E4, 200, 0x00B3BE) { pc_run_0108DC(ctx, 0x0108E4); }
static void pc_run_010908(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x010908: goto L_010908;
	case 0x010910: goto L_010910;
	default: __builtin_trap();
	}
L_010908:
	{ uint32_t v = 0x26Bu; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 010908 movew #619,%d1 */
	hle_jump(ctx, 0x01090C); return;  /* 01090C jsr %pc@(13228) */
L_010910:
	hle_insn(ctx, 10); hle_jump(ctx, 0x01091A); return;  /* 010910 bras 1091a */
}

HLE_RANGE_PURE(0x010908, Auto_010908, 200, 0x01090C) { pc_run_010908(ctx, 0x010908); }
HLE_RANGE_PURE(0x010910, Auto_010910, 200, 0x01091A) { pc_run_010908(ctx, 0x010910); }
static void pc_run_01091A(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x01091A: goto L_01091A;
	case 0x01094E: goto L_01094E;
	case 0x010968: goto L_010968;
	case 0x010970: goto L_010970;
	case 0x010978: goto L_010978;
	case 0x01098A: goto L_01098A;
	case 0x010996: goto L_010996;
	case 0x0109A8: goto L_0109A8;
	default: __builtin_trap();
	}
L_01091A:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = alu(ctx, OP_SUB, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 16);  /* 01091A subqw #1,%fp@(36) */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = ld16(eaS); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 01091E movew %fp@(36),%d1 */
	{ uint32_t v = ((ctx->d[1] & 0xFFFFu) & 0x6u) & 0xFFFFu; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 010922 andiw #6,%d1 */
	{ uint32_t v = shf(ctx, 'a', (ctx->d[1] & 0xFFFFu), 1, 16); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 010926 asrw #1,%d1 */
	{ uint32_t v = ((ctx->d[1] & 0xFFFFu) ^ 0x3u) & 0xFFFFu; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 010928 eoriw #3,%d1 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0xDDu, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 01092C addiw #221,%d1 */
	{ ctx->a[7] -= 2; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->d[1] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 010930 movew %d1,%sp@- */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = ld16(eaS); set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 010932 movew %fp@(2),%d2 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(4); uint32_t v = ld16(eaS); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 010936 movew %fp@(4),%d3 */
	{ ctx->d[5] = 0x00000000u; cc_logic32(ctx, ctx->d[5]); } hle_insn(ctx, 4);  /* 01093A moveq #0,%d5 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(7); uint32_t v = ld8(eaS); set_b(&ctx->d[5], v); cc_logic8(ctx, v); } hle_insn(ctx, 12);  /* 01093C moveb %fp@(7),%d5 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[2] & 0xFFFFu), 0x42u, 16, 1); set_w(&ctx->d[2], v); } hle_insn(ctx, 8);  /* 010940 subiw #66,%d2 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[3] & 0xFFFFu), 0x2u, 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 8);  /* 010944 subiw #2,%d3 */
	{ ctx->a[7] -= 2; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->d[3] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 010948 movew %d3,%sp@- */
	hle_jump(ctx, 0x01094A); return;  /* 01094A jsr %pc@(13240) */
L_01094E:
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 01094E movew %sp@+,%d3 */
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 010950 movew %sp@+,%d1 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = ld16(eaS); set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 010952 movew %fp@(2),%d2 */
	{ alu(ctx, OP_SUB, (ctx->d[2] & 0xFFFFu), 0x12Cu, 16, 0); } hle_insn(ctx, 8);  /* 010956 cmpiw #300,%d2 */
	{ ctx->d[5] = 0x00000000u; cc_logic32(ctx, ctx->d[5]); } hle_insn(ctx, 4);  /* 01095A moveq #0,%d5 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(7); uint32_t v = ld8(eaS); set_b(&ctx->d[5], v); cc_logic8(ctx, v); } hle_insn(ctx, 12);  /* 01095C moveb %fp@(7),%d5 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[2] & 0xFFFFu), 0x2Du, 16, 1); set_w(&ctx->d[2], v); } hle_insn(ctx, 8);  /* 010960 addiw #45,%d2 */
	hle_jump(ctx, 0x010964); return;  /* 010964 jsr %pc@(13240) */
L_010968:
	{ uint32_t v = 0x269u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 010968 movew #617,%d1 */
	hle_jump(ctx, 0x01096C); return;  /* 01096C jsr %pc@(13228) */
L_010970:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = ld16(eaS); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 010970 movew %fp@(36),%d1 */
	hle_jump(ctx, 0x010974); return;  /* 010974 bsrw 109ac */
L_010978:
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[2] & 0xFFFFu), 0x50u, 16, 1); set_w(&ctx->d[2], v); } hle_insn(ctx, 8);  /* 010978 subiw #80,%d2 */
	{ uint32_t eaD = ctx->a[6];  unsigned b = 0; uint32_t m = 1u << b; uint8_t v = ld8(eaD); cc_set(ctx, CC_Z, (v & m) ? 0 : CC_Z);  } hle_insn(ctx, 12);  /* 01097C btst #8,%fp@ */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_010986; } hle_insn(ctx, 8);  /* 010980 beqs 10986 */
	{ uint32_t v = 0x37Fu; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 010982 movew #895,%d1 */
L_010986:
	hle_jump(ctx, 0x010986); return;  /* 010986 jsr %pc@(13240) */
L_01098A:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = ld16(eaS); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 01098A movew %fp@(36),%d1 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0xEu, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 01098E addiw #14,%d1 */
	hle_jump(ctx, 0x010992); return;  /* 010992 bsrw 109ac */
L_010996:
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[2] & 0xFFFFu), 0x28u, 16, 1); set_w(&ctx->d[2], v); } hle_insn(ctx, 8);  /* 010996 addiw #40,%d2 */
	{ uint32_t eaD = ctx->a[6];  unsigned b = 1; uint32_t m = 1u << b; uint8_t v = ld8(eaD); cc_set(ctx, CC_Z, (v & m) ? 0 : CC_Z);  } hle_insn(ctx, 12);  /* 01099A btst #9,%fp@ */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0109A4; } hle_insn(ctx, 8);  /* 01099E beqs 109a4 */
	{ uint32_t v = 0x37Fu; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0109A0 movew #895,%d1 */
L_0109A4:
	hle_jump(ctx, 0x0109A4); return;  /* 0109A4 jsr %pc@(13240) */
L_0109A8:
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 0109A8 braw b3be */
}

HLE_RANGE_PURE(0x01091A, Auto_01091A, 200, HLE_EXIT_SP(0x01094A, -4)) { pc_run_01091A(ctx, 0x01091A); }
HLE_RANGE_PURE(0x01094E, Auto_01094E, 200, HLE_EXIT_SP(0x010964, 4)) { pc_run_01091A(ctx, 0x01094E); }
HLE_RANGE_PURE(0x010968, Auto_010968, 200, 0x01096C) { pc_run_01091A(ctx, 0x010968); }
HLE_RANGE_PURE(0x010970, Auto_010970, 200, 0x010974) { pc_run_01091A(ctx, 0x010970); }
HLE_RANGE_PURE(0x010978, Auto_010978, 200, 0x010986) { pc_run_01091A(ctx, 0x010978); }
HLE_RANGE_PURE(0x01098A, Auto_01098A, 200, 0x010992) { pc_run_01091A(ctx, 0x01098A); }
HLE_RANGE_PURE(0x010996, Auto_010996, 200, 0x0109A4) { pc_run_01091A(ctx, 0x010996); }
HLE_RANGE_PURE(0x0109A8, Auto_0109A8, 200, 0x00B3BE) { pc_run_01091A(ctx, 0x0109A8); }
static void pc_run_0109AC(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x0109AC: goto L_0109AC;
	default: __builtin_trap();
	}
L_0109AC:
	{ uint32_t v = ((ctx->d[1] & 0xFFFFu) & 0x18u) & 0xFFFFu; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0109AC andiw #24,%d1 */
	{ uint32_t v = shf(ctx, 'r', (ctx->d[1] & 0xFFFFu), 3, 16); set_w(&ctx->d[1], v); } hle_insn(ctx, 12);  /* 0109B0 lsrw #3,%d1 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0x270u, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 0109B2 addiw #624,%d1 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(2); ctx->d[2] = (uint32_t)(int32_t)(int16_t)ld16(eaS + 0); ctx->d[3] = (uint32_t)(int32_t)(int16_t)ld16(eaS + 2); } hle_insn(ctx, 24);  /* 0109B6 movemw %fp@(2),%d2-%d3 */
	{ ctx->d[5] = 0x00000000u; cc_logic32(ctx, ctx->d[5]); } hle_insn(ctx, 4);  /* 0109BC moveq #0,%d5 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(7); uint32_t v = ld8(eaS); set_b(&ctx->d[5], v); cc_logic8(ctx, v); } hle_insn(ctx, 12);  /* 0109BE moveb %fp@(7),%d5 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[3] & 0xFFFFu), 0x78u, 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 8);  /* 0109C2 subiw #120,%d3 */
	hle_jump(ctx, 0x0109C6); return;  /* 0109C6 rts */
}

HLE_RANGE_PURE(0x0109AC, Auto_0109AC, 200, 0x0109C6) { pc_run_0109AC(ctx, 0x0109AC); }
static void pc_run_010D0C(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x010D10: goto L_010D10;
	case 0x010D16: goto L_010D16;
	default: __builtin_trap();
	}
L_010D0C:
	hle_jump(ctx, 0x010D0C); return;  /* 010D0C jsr %pc@(d0f8) */
L_010D10:
	{ ctx->d[2] = 0x00000000u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 010D10 moveq #0,%d2 */
	hle_jump(ctx, 0x010D12); return;  /* 010D12 jsr %pc@(9ef8) */
L_010D16:
	{ ctx->d[0] = 0x00000000u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 010D16 moveq #0,%d0 */
	{ ctx->d[1] = 0x00000000u; cc_logic32(ctx, ctx->d[1]); } hle_insn(ctx, 4);  /* 010D18 moveq #0,%d1 */
	hle_jump(ctx, 0x010D1A); return;  /* 010D1A jmp 81b6 */
}

HLE_RANGE_PURE(0x010D10, Auto_010D10, 200, 0x010D12) { pc_run_010D0C(ctx, 0x010D10); }
HLE_RANGE_PURE(0x010D16, Auto_010D16, 200, 0x010D1A) { pc_run_010D0C(ctx, 0x010D16); }
static void pc_run_010D20(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x010D20: goto L_010D20;
	case 0x010D30: goto L_010D30;
	case 0x010D42: goto L_010D42;
	default: __builtin_trap();
	}
L_010D20:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = ld16(eaS); set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 010D20 movew %fp@(2),%d2 */
	if (COND_PL) { hle_insn(ctx, 10); goto L_010D2C; } hle_insn(ctx, 8);  /* 010D24 bpls 10d2c */
	{ set_w(&ctx->d[2], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 4);  /* 010D26 clrw %d2 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = (ctx->d[2] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 010D28 movew %d2,%fp@(2) */
L_010D2C:
	hle_jump(ctx, 0x010D2C); return;  /* 010D2C jsr %pc@(9ef8) */
L_010D30:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(4); uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 010D30 movew %fp@(4),%d0 */
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0xFFu) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 010D34 andiw #255,%d0 */
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) ^ 0xFFu) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 010D38 eoriw #255,%d0 */
	{ ctx->d[1] = 0x00000000u; cc_logic32(ctx, ctx->d[1]); } hle_insn(ctx, 4);  /* 010D3C moveq #0,%d1 */
	hle_jump(ctx, 0x010D3E); return;  /* 010D3E jsr %pc@(11f58) */
L_010D42:
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 010D42 braw b3be */
}

HLE_RANGE_PURE(0x010D20, Auto_010D20, 200, 0x010D2C) { pc_run_010D20(ctx, 0x010D20); }
HLE_RANGE_PURE(0x010D30, Auto_010D30, 200, 0x010D3E) { pc_run_010D20(ctx, 0x010D30); }
HLE_RANGE_PURE(0x010D42, Auto_010D42, 200, 0x00B3BE) { pc_run_010D20(ctx, 0x010D42); }
static void pc_run_011072(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x011072: goto L_011072;
	default: __builtin_trap();
	}
L_011072:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = ld16(eaS); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 011072 movew %fp@(36),%d1 */
	{ uint32_t v = ((ctx->d[1] & 0xFFFFu) & 0x6u) & 0xFFFFu; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 011076 andiw #6,%d1 */
	{ uint32_t v = shf(ctx, 'a', (ctx->d[1] & 0xFFFFu), 1, 16); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 01107A asrw #1,%d1 */
	{ uint32_t v = ((ctx->d[1] & 0xFFFFu) ^ 0x3u) & 0xFFFFu; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 01107C eoriw #3,%d1 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0xDDu, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 011080 addiw #221,%d1 */
	{ ctx->d[5] = 0x0000000Fu; cc_logic32(ctx, ctx->d[5]); } hle_insn(ctx, 4);  /* 011084 moveq #15,%d5 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(4); uint32_t v = ld16(eaS); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 011086 movew %fp@(4),%d3 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[3] & 0xFFFFu), 0x1u, 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 4);  /* 01108A subqw #1,%d3 */
	hle_jump(ctx, 0x01108C); return;  /* 01108C jsr %pc@(13240) */
L_011090:
	hle_jump(ctx, 0x011090); return;  /* 011090 rts */
}

HLE_RANGE_PURE(0x011072, Auto_011072, 200, 0x01108C) { pc_run_011072(ctx, 0x011072); }
static void pc_run_011092(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x011092: goto L_011092;
	case 0x0110A8: goto L_0110A8;
	case 0x0110DC: goto L_0110DC;
	case 0x0110F4: goto L_0110F4;
	case 0x011128: goto L_011128;
	case 0x011142: goto L_011142;
	default: __builtin_trap();
	}
L_011092:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = alu(ctx, OP_SUB, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 16);  /* 011092 subqw #1,%fp@(36) */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(20); uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 011096 movew %fp@(20),%d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0110DE; } hle_insn(ctx, 8);  /* 01109A beqs 110de */
	if (COND_PL) { hle_insn(ctx, 10); goto L_0110AE; } hle_insn(ctx, 8);  /* 01109C bpls 110ae */
	{ uint32_t eaS = 0x000A1152u; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 01109E lea a1152,%a0 */
	hle_jump(ctx, 0x0110A4); return;  /* 0110A4 bsrw 1114c */
L_0110A8:
	{ uint32_t v = 0x7u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0110A8 movew #7,%d0 */
	hle_insn(ctx, 10); goto L_0110DE;  /* 0110AC bras 110de */
L_0110AE:
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x1u, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 4);  /* 0110AE subqw #1,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0110DE; } hle_insn(ctx, 8);  /* 0110B0 bnes 110de */
	{ uint32_t eaS = 0x000A12E2u; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 0110B2 lea a12e2,%a0 */
	{ uint32_t eaD = 0xFFFFA242u; alu(ctx, OP_SUB, ld16(eaD), 0x28u, 16, 0); } hle_insn(ctx, 20);  /* 0110B8 cmpiw #40,ffffa242 */
	if (COND_LT) { hle_insn(ctx, 10); goto L_0110D8; } hle_insn(ctx, 8);  /* 0110C0 blts 110d8 */
	{ uint32_t eaS = 0x000A1472u; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 0110C2 lea a1472,%a0 */
	{ uint32_t eaD = 0xFFFFA242u; alu(ctx, OP_SUB, ld16(eaD), 0x64u, 16, 0); } hle_insn(ctx, 20);  /* 0110C8 cmpiw #100,ffffa242 */
	if (COND_LT) { hle_insn(ctx, 10); goto L_0110D8; } hle_insn(ctx, 8);  /* 0110D0 blts 110d8 */
	{ uint32_t eaS = 0x000A1364u; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 0110D2 lea a1364,%a0 */
L_0110D8:
	hle_jump(ctx, 0x0110D8); return;  /* 0110D8 bsrw 1114c */
L_0110DC:
	{ ctx->d[0] = 0x00000000u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 0110DC moveq #0,%d0 */
L_0110DE:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(20); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 0110DE movew %d0,%fp@(20) */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(26); uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 0110E2 movew %fp@(26),%d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_01112A; } hle_insn(ctx, 8);  /* 0110E6 beqs 1112a */
	if (COND_PL) { hle_insn(ctx, 10); goto L_0110FA; } hle_insn(ctx, 8);  /* 0110E8 bpls 110fa */
	{ uint32_t eaS = 0x000A115Cu; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 0110EA lea a115c,%a0 */
	hle_jump(ctx, 0x0110F0); return;  /* 0110F0 bsrw 11146 */
L_0110F4:
	{ uint32_t v = 0x7u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0110F4 movew #7,%d0 */
	hle_insn(ctx, 10); goto L_01112A;  /* 0110F8 bras 1112a */
L_0110FA:
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x1u, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 4);  /* 0110FA subqw #1,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_01112A; } hle_insn(ctx, 8);  /* 0110FC bnes 1112a */
	{ uint32_t eaS = 0x000A12ECu; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 0110FE lea a12ec,%a0 */
	{ uint32_t eaD = 0xFFFFA242u; alu(ctx, OP_SUB, ld16(eaD), 0x28u, 16, 0); } hle_insn(ctx, 20);  /* 011104 cmpiw #40,ffffa242 */
	if (COND_LT) { hle_insn(ctx, 10); goto L_011124; } hle_insn(ctx, 8);  /* 01110C blts 11124 */
	{ uint32_t eaS = 0x000A147Cu; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 01110E lea a147c,%a0 */
	{ uint32_t eaD = 0xFFFFA242u; alu(ctx, OP_SUB, ld16(eaD), 0x64u, 16, 0); } hle_insn(ctx, 20);  /* 011114 cmpiw #100,ffffa242 */
	if (COND_LT) { hle_insn(ctx, 10); goto L_011124; } hle_insn(ctx, 8);  /* 01111C blts 11124 */
	{ uint32_t eaS = 0x000A136Eu; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 01111E lea a136e,%a0 */
L_011124:
	hle_jump(ctx, 0x011124); return;  /* 011124 bsrw 11146 */
L_011128:
	{ ctx->d[0] = 0x00000000u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 011128 moveq #0,%d0 */
L_01112A:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(26); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 01112A movew %d0,%fp@(26) */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = ld16(eaS); set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 01112E movew %fp@(2),%d2 */
	{ alu(ctx, OP_SUB, (ctx->d[2] & 0xFFFFu), 0x168u, 16, 0); } hle_insn(ctx, 8);  /* 011132 cmpiw #360,%d2 */
	if (COND_GT) { hle_insn(ctx, 10); goto L_011142; } hle_insn(ctx, 12);  /* 011136 bgtw 11142 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[2] & 0xFFFFu), 0x22u, 16, 1); set_w(&ctx->d[2], v); } hle_insn(ctx, 8);  /* 01113A subiw #34,%d2 */
	hle_jump(ctx, 0x01113E); return;  /* 01113E bsrw 11072 */
L_011142:
	hle_insn(ctx, 10); hle_jump(ctx, 0x010D20); return;  /* 011142 braw 10d20 */
}

HLE_RANGE_PURE(0x011092, Auto_011092, 200, 0x010D20, 0x0110A4, 0x0110D8, 0x0110F0, 0x011124, 0x01113E) { pc_run_011092(ctx, 0x011092); }
HLE_RANGE_PURE(0x0110A8, Auto_0110A8, 200, 0x010D20, 0x0110F0, 0x011124, 0x01113E) { pc_run_011092(ctx, 0x0110A8); }
HLE_RANGE_PURE(0x0110DC, Auto_0110DC, 200, 0x010D20, 0x0110F0, 0x011124, 0x01113E) { pc_run_011092(ctx, 0x0110DC); }
HLE_RANGE_PURE(0x0110F4, Auto_0110F4, 200, 0x010D20, 0x01113E) { pc_run_011092(ctx, 0x0110F4); }
HLE_RANGE_PURE(0x011128, Auto_011128, 200, 0x010D20, 0x01113E) { pc_run_011092(ctx, 0x011128); }
HLE_RANGE_PURE(0x011142, Auto_011142, 200, 0x010D20) { pc_run_011092(ctx, 0x011142); }
static void pc_run_0112A8(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x0112A8: goto L_0112A8;
	case 0x0112BA: goto L_0112BA;
	default: __builtin_trap();
	}
L_0112A8:
	{ uint32_t v = 0xA000u; ctx->d[6] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 0112A8 movel #40960,%d6 */
	{ ctx->a[7] -= 2; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->d[7] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0112AE movew %d7,%sp@- */
	{ uint32_t eaS = 0xFFFF9E74u; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 0112B0 movew ffff9e74,%d7 */
	hle_jump(ctx, 0x0112B6); return;  /* 0112B6 jsr %pc@(12234) */
L_0112BA:
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0112BA movew %sp@+,%d7 */
	hle_jump(ctx, 0x0112BC); return;  /* 0112BC rts */
}

HLE_RANGE_PURE(0x0112A8, Auto_0112A8, 200, HLE_EXIT_SP(0x0112B6, -2)) { pc_run_0112A8(ctx, 0x0112A8); }
HLE_RANGE_PURE(0x0112BA, Auto_0112BA, 200, HLE_EXIT_SP(0x0112BC, 2)) { pc_run_0112A8(ctx, 0x0112BA); }
static void pc_run_0112BE(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x0112BE: goto L_0112BE;
	default: __builtin_trap();
	}
L_0112BE:
	{ uint32_t eaS = 0x0009C5BAu; ctx->a[0] = eaS; } hle_insn(ctx, 12); hle_jump(ctx, 0x0112C4); return;  /* 0112BE lea 9c5ba,%a0 */
}

HLE_RANGE_PURE(0x0112BE, Auto_0112BE, 200, 0x0112C4) { pc_run_0112BE(ctx, 0x0112BE); }
static void pc_run_0112C4(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x0112C4: goto L_0112C4;
	case 0x0112DA: goto L_0112DA;
	default: __builtin_trap();
	}
L_0112C4:
	{ ctx->d[2] = 0x00000006u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 0112C4 moveq #6,%d2 */
	{ ctx->d[3] = 0x00000005u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 0112C6 moveq #5,%d3 */
	{ uint32_t v = 0xA000u; ctx->d[6] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 0112C8 movel #40960,%d6 */
	{ ctx->a[7] -= 2; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->d[7] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0112CE movew %d7,%sp@- */
	{ uint32_t eaS = 0xFFFF9E48u; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 0112D0 movew ffff9e48,%d7 */
	hle_jump(ctx, 0x0112D6); return;  /* 0112D6 jsr %pc@(122ca) */
L_0112DA:
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0112DA movew %sp@+,%d7 */
	hle_jump(ctx, 0x0112DC); return;  /* 0112DC rts */
}

HLE_RANGE_PURE(0x0112C4, Auto_0112C4, 200, HLE_EXIT_SP(0x0112D6, -2)) { pc_run_0112C4(ctx, 0x0112C4); }
HLE_RANGE_PURE(0x0112DA, Auto_0112DA, 200, HLE_EXIT_SP(0x0112DC, 2)) { pc_run_0112C4(ctx, 0x0112DA); }
static void pc_run_0112DE(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x0112DE: goto L_0112DE;
	case 0x0112FA: goto L_0112FA;
	default: __builtin_trap();
	}
L_0112DE:
	{ uint32_t eaS = 0x000A63E6u; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 0112DE lea a63e6,%a0 */
	{ ctx->d[2] = 0x0000000Cu; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 0112E4 moveq #12,%d2 */
	{ ctx->d[3] = 0x00000001u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 0112E6 moveq #1,%d3 */
	{ uint32_t v = 0x2000u; ctx->d[6] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 0112E8 movel #8192,%d6 */
	{ ctx->a[7] -= 2; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->d[7] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0112EE movew %d7,%sp@- */
	{ uint32_t eaS = 0xFFFF9E38u; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 0112F0 movew ffff9e38,%d7 */
	hle_jump(ctx, 0x0112F6); return;  /* 0112F6 jsr %pc@(12234) */
L_0112FA:
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0112FA movew %sp@+,%d7 */
	hle_jump(ctx, 0x0112FC); return;  /* 0112FC rts */
}

HLE_RANGE_PURE(0x0112DE, Auto_0112DE, 200, HLE_EXIT_SP(0x0112F6, -2)) { pc_run_0112DE(ctx, 0x0112DE); }
HLE_RANGE_PURE(0x0112FA, Auto_0112FA, 200, HLE_EXIT_SP(0x0112FC, 2)) { pc_run_0112DE(ctx, 0x0112FA); }
static void pc_run_011334(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x011334: goto L_011334;
	case 0x011352: goto L_011352;
	default: __builtin_trap();
	}
L_011334:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(2); alu(ctx, OP_SUB, ld16(eaD), 0xFFE2u, 16, 0); } hle_insn(ctx, 16);  /* 011334 cmpiw #-30,%fp@(2) */
	if (COND_LT) { hle_insn(ctx, 10); goto L_011344; } hle_insn(ctx, 8);  /* 01133A blts 11344 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(2); alu(ctx, OP_SUB, ld16(eaD), 0x15Eu, 16, 0); } hle_insn(ctx, 16);  /* 01133C cmpiw #350,%fp@(2) */
	if (COND_LE) { hle_insn(ctx, 10); goto L_01135E; } hle_insn(ctx, 8);  /* 011342 bles 1135e */
L_011344:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0x114C0u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 011344 movel #70848,%fp@(32) */
	hle_jump(ctx, 0x01134C); return;  /* 01134C jsr 881c */
L_011352:
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0xFu) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 011352 andiw #15,%d0 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), 0x1Eu, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 8);  /* 011356 addiw #30,%d0 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 01135A movew %d0,%fp@(36) */
L_01135E:
	hle_jump(ctx, 0x01135E); return;  /* 01135E rts */
}

HLE_RANGE_PURE(0x011334, Auto_011334, 200, 0x01134C, 0x01135E) { pc_run_011334(ctx, 0x011334); }
HLE_RANGE_PURE(0x011352, Auto_011352, 200, 0x01135E) { pc_run_011334(ctx, 0x011352); }
static void pc_run_0115FA(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x0115FA: goto L_0115FA;
	case 0x011608: goto L_011608;
	default: __builtin_trap();
	}
L_0115FA:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0x112FEu; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 0115FA movel #70398,%fp@(32) */
	hle_jump(ctx, 0x011602); return;  /* 011602 jsr 881c */
L_011608:
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0x1Fu) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 011608 andiw #31,%d0 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), 0x1Eu, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 8);  /* 01160C addiw #30,%d0 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 011610 movew %d0,%fp@(36) */
	hle_jump(ctx, 0x011614); return;  /* 011614 rts */
}

HLE_RANGE_PURE(0x0115FA, Auto_0115FA, 200, 0x011602) { pc_run_0115FA(ctx, 0x0115FA); }
HLE_RANGE_PURE(0x011608, Auto_011608, 200, 0x011614) { pc_run_0115FA(ctx, 0x011608); }
static void pc_run_011616(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x011616: goto L_011616;
	case 0x011628: goto L_011628;
	case 0x011630: goto L_011630;
	case 0x011650: goto L_011650;
	case 0x011658: goto L_011658;
	case 0x011660: goto L_011660;
	case 0x011674: goto L_011674;
	case 0x01168C: goto L_01168C;
	case 0x011694: goto L_011694;
	case 0x0116AE: goto L_0116AE;
	case 0x0116C8: goto L_0116C8;
	case 0x0116E0: goto L_0116E0;
	case 0x0116F6: goto L_0116F6;
	case 0x0116FE: goto L_0116FE;
	case 0x011706: goto L_011706;
	case 0x01171A: goto L_01171A;
	case 0x011722: goto L_011722;
	case 0x01172A: goto L_01172A;
	case 0x01174E: goto L_01174E;
	case 0x011756: goto L_011756;
	case 0x01176E: goto L_01176E;
	case 0x011786: goto L_011786;
	case 0x011796: goto L_011796;
	case 0x0117B0: goto L_0117B0;
	case 0x0117C0: goto L_0117C0;
	case 0x0117E2: goto L_0117E2;
	case 0x0117F6: goto L_0117F6;
	case 0x011810: goto L_011810;
	case 0x01182A: goto L_01182A;
	case 0x011844: goto L_011844;
	case 0x01184C: goto L_01184C;
	case 0x011866: goto L_011866;
	case 0x01187E: goto L_01187E;
	case 0x01188E: goto L_01188E;
	case 0x011896: goto L_011896;
	case 0x0118B0: goto L_0118B0;
	case 0x0118B8: goto L_0118B8;
	default: __builtin_trap();
	}
L_011616:
	{ uint32_t v = (ctx->d[1] & 0xFFFFu); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 011616 movew %d1,%d0 */
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0xFu, 16, 0); } hle_insn(ctx, 8);  /* 011618 cmpiw #15,%d0 */
	if (COND_GT) { hle_insn(ctx, 10); goto L_011644; } hle_insn(ctx, 8);  /* 01161C bgts 11644 */
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 01161E movew %d0,%d3 */
	{ uint32_t v = ((ctx->d[3] & 0xFFFFu) & 0x7u) & 0xFFFFu; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 011620 andiw #7,%d3 */
	hle_jump(ctx, 0x011624); return;  /* 011624 bsrw 118ea */
L_011628:
	{ uint32_t v = 0x0u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 011628 movew #0,%d3 */
	hle_jump(ctx, 0x01162C); return;  /* 01162C bsrw 118dc */
L_011630:
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 011630 movew %d0,%d1 */
	{ uint32_t eaD = ctx->a[6];  unsigned b = 6; uint32_t m = 1u << b; uint8_t v = ld8(eaD); cc_set(ctx, CC_Z, (v & m) ? 0 : CC_Z);  } hle_insn(ctx, 12);  /* 011632 btst #14,%fp@ */
	if (COND_NE) { hle_insn(ctx, 10); goto L_01163C; } hle_insn(ctx, 8);  /* 011636 bnes 1163c */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0x8u, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 011638 addiw #8,%d1 */
L_01163C:
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0x274u, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 01163C addiw #628,%d1 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x013228); return;  /* 011640 braw 13228 */
L_011644:
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x10u, 16, 0); } hle_insn(ctx, 8);  /* 011644 cmpiw #16,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_011668; } hle_insn(ctx, 8);  /* 011648 bnes 11668 */
	{ ctx->d[3] = 0x00000008u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 01164A moveq #8,%d3 */
	hle_jump(ctx, 0x01164C); return;  /* 01164C bsrw 118ea */
L_011650:
	{ uint32_t v = 0x1u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 011650 movew #1,%d3 */
	hle_jump(ctx, 0x011654); return;  /* 011654 bsrw 118dc */
L_011658:
	{ uint32_t v = 0x303u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 011658 movew #771,%d1 */
	hle_jump(ctx, 0x01165C); return;  /* 01165C bsrw 13228 */
L_011660:
	{ uint32_t v = 0x302u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 011660 movew #770,%d1 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x013228); return;  /* 011664 braw 13228 */
L_011668:
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x11u, 16, 0); } hle_insn(ctx, 8);  /* 011668 cmpiw #17,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_01167C; } hle_insn(ctx, 8);  /* 01166C bnes 1167c */
	{ ctx->d[3] = 0x00000009u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 01166E moveq #9,%d3 */
	hle_jump(ctx, 0x011670); return;  /* 011670 bsrw 118ea */
L_011674:
	{ uint32_t v = 0x30Cu; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 011674 movew #780,%d1 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x013228); return;  /* 011678 braw 13228 */
L_01167C:
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x13u, 16, 0); } hle_insn(ctx, 8);  /* 01167C cmpiw #19,%d0 */
	if (COND_GT) { hle_insn(ctx, 10); goto L_01169C; } hle_insn(ctx, 8);  /* 011680 bgts 1169c */
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 011682 movew %d0,%d3 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[3] & 0xFFFFu), 0x8u, 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 8);  /* 011684 subiw #8,%d3 */
	hle_jump(ctx, 0x011688); return;  /* 011688 bsrw 118ea */
L_01168C:
	{ uint32_t v = 0x4u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 01168C movew #4,%d3 */
	hle_jump(ctx, 0x011690); return;  /* 011690 bsrw 118dc */
L_011694:
	{ uint32_t v = 0x30Du; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 011694 movew #781,%d1 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x013228); return;  /* 011698 braw 13228 */
L_01169C:
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x1Cu, 16, 0); } hle_insn(ctx, 8);  /* 01169C cmpiw #28,%d0 */
	if (COND_GT) { hle_insn(ctx, 10); goto L_0116D0; } hle_insn(ctx, 8);  /* 0116A0 bgts 116d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0116C2; } hle_insn(ctx, 8);  /* 0116A2 beqs 116c2 */
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 0116A4 movew %d0,%d3 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[3] & 0xFFFFu), 0x7u, 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 8);  /* 0116A6 subiw #7,%d3 */
	hle_jump(ctx, 0x0116AA); return;  /* 0116AA bsrw 118ea */
L_0116AE:
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 0116AE movew %d0,%d1 */
	{ uint32_t eaD = ctx->a[6];  unsigned b = 6; uint32_t m = 1u << b; uint8_t v = ld8(eaD); cc_set(ctx, CC_Z, (v & m) ? 0 : CC_Z);  } hle_insn(ctx, 12);  /* 0116B0 btst #14,%fp@ */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0116BA; } hle_insn(ctx, 8);  /* 0116B4 beqs 116ba */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0xDu, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 0116B6 addiw #13,%d1 */
L_0116BA:
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0x2FBu, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 0116BA addiw #763,%d1 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x013228); return;  /* 0116BE braw 13228 */
L_0116C2:
	{ ctx->d[3] = 0x0000000Cu; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 0116C2 moveq #12,%d3 */
	hle_jump(ctx, 0x0116C4); return;  /* 0116C4 bsrw 118ea */
L_0116C8:
	{ uint32_t v = 0x30Eu; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0116C8 movew #782,%d1 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x013228); return;  /* 0116CC braw 13228 */
L_0116D0:
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x21u, 16, 0); } hle_insn(ctx, 8);  /* 0116D0 cmpiw #33,%d0 */
	if (COND_GT) { hle_insn(ctx, 10); goto L_0116EA; } hle_insn(ctx, 8);  /* 0116D4 bgts 116ea */
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 0116D6 movew %d0,%d3 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[3] & 0xFFFFu), 0x8u, 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 8);  /* 0116D8 subiw #8,%d3 */
	hle_jump(ctx, 0x0116DC); return;  /* 0116DC bsrw 118ea */
L_0116E0:
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 0116E0 movew %d0,%d1 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0x2FAu, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 0116E2 addiw #762,%d1 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x013228); return;  /* 0116E6 braw 13228 */
L_0116EA:
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x22u, 16, 0); } hle_insn(ctx, 8);  /* 0116EA cmpiw #34,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_01170E; } hle_insn(ctx, 8);  /* 0116EE bnes 1170e */
L_0116F0:
	{ ctx->d[3] = 0x00000008u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 0116F0 moveq #8,%d3 */
	hle_jump(ctx, 0x0116F2); return;  /* 0116F2 bsrw 118ea */
L_0116F6:
	{ uint32_t v = 0x3u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0116F6 movew #3,%d3 */
	hle_jump(ctx, 0x0116FA); return;  /* 0116FA bsrw 118dc */
L_0116FE:
	{ uint32_t v = 0x305u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0116FE movew #773,%d1 */
	hle_jump(ctx, 0x011702); return;  /* 011702 bsrw 13228 */
L_011706:
	{ uint32_t v = 0x302u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 011706 movew #770,%d1 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x013228); return;  /* 01170A braw 13228 */
L_01170E:
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x23u, 16, 0); } hle_insn(ctx, 8);  /* 01170E cmpiw #35,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_011732; } hle_insn(ctx, 8);  /* 011712 bnes 11732 */
	{ ctx->d[3] = 0x00000008u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 011714 moveq #8,%d3 */
	hle_jump(ctx, 0x011716); return;  /* 011716 bsrw 118ea */
L_01171A:
	{ uint32_t v = 0x2u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 01171A movew #2,%d3 */
	hle_jump(ctx, 0x01171E); return;  /* 01171E bsrw 118dc */
L_011722:
	{ uint32_t v = 0x304u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 011722 movew #772,%d1 */
	hle_jump(ctx, 0x011726); return;  /* 011726 bsrw 13228 */
L_01172A:
	{ uint32_t v = 0x302u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 01172A movew #770,%d1 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x013228); return;  /* 01172E braw 13228 */
L_011732:
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x29u, 16, 0); } hle_insn(ctx, 8);  /* 011732 cmpiw #41,%d0 */
	if (COND_GT) { hle_insn(ctx, 10); goto L_0117E6; } hle_insn(ctx, 12);  /* 011736 bgtw 117e6 */
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x24u, 16, 0); } hle_insn(ctx, 8);  /* 01173A cmpiw #36,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_011758; } hle_insn(ctx, 8);  /* 01173E bnes 11758 */
	{ uint32_t eaD = 0xFFFF9E58u; uint32_t v = 0x424u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 20);  /* 011740 movew #1060,ffff9e58 */
	{ ctx->d[3] = 0x00000000u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 011748 moveq #0,%d3 */
	hle_jump(ctx, 0x01174A); return;  /* 01174A bsrw 118c0 */
L_01174E:
	{ uint32_t v = 0x306u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 01174E movew #774,%d1 */
	hle_jump(ctx, 0x011752); return;  /* 011752 bsrw 13228 */
L_011756:
	hle_insn(ctx, 10); goto L_0116F0;  /* 011756 bras 116f0 */
L_011758:
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x25u, 16, 0); } hle_insn(ctx, 8);  /* 011758 cmpiw #37,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_011770; } hle_insn(ctx, 8);  /* 01175C bnes 11770 */
	{ uint32_t eaD = 0xFFFF9E58u; uint32_t v = 0x430u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 20);  /* 01175E movew #1072,ffff9e58 */
	{ uint32_t v = 0x309u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 011766 movew #777,%d1 */
	hle_jump(ctx, 0x01176A); return;  /* 01176A bsrw 13228 */
L_01176E:
	hle_insn(ctx, 10); goto L_0116F0;  /* 01176E bras 116f0 */
L_011770:
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x26u, 16, 0); } hle_insn(ctx, 8);  /* 011770 cmpiw #38,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_01179A; } hle_insn(ctx, 8);  /* 011774 bnes 1179a */
	{ uint32_t eaD = 0xFFFF9E1Au; uint32_t v = 0x3E8u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 20);  /* 011776 movew #1000,ffff9e1a */
	{ uint32_t v = 0x307u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 01177E movew #775,%d1 */
	hle_jump(ctx, 0x011782); return;  /* 011782 bsrw 13228 */
L_011786:
	{ uint32_t eaD = 0xFFFF9E58u; uint32_t v = 0x430u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 20);  /* 011786 movew #1072,ffff9e58 */
	{ uint32_t v = 0x309u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 01178E movew #777,%d1 */
	hle_jump(ctx, 0x011792); return;  /* 011792 bsrw 13228 */
L_011796:
	hle_insn(ctx, 10); goto L_0116F0;  /* 011796 braw 116f0 */
L_01179A:
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x27u, 16, 0); } hle_insn(ctx, 8);  /* 01179A cmpiw #39,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0117C4; } hle_insn(ctx, 8);  /* 01179E bnes 117c4 */
	{ uint32_t eaD = 0xFFFF9E1Au; uint32_t v = 0x3FCu; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 20);  /* 0117A0 movew #1020,ffff9e1a */
	{ uint32_t v = 0x308u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0117A8 movew #776,%d1 */
	hle_jump(ctx, 0x0117AC); return;  /* 0117AC bsrw 13228 */
L_0117B0:
	{ uint32_t eaD = 0xFFFF9E58u; uint32_t v = 0x430u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 20);  /* 0117B0 movew #1072,ffff9e58 */
	{ uint32_t v = 0x309u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0117B8 movew #777,%d1 */
	hle_jump(ctx, 0x0117BC); return;  /* 0117BC bsrw 13228 */
L_0117C0:
	hle_insn(ctx, 10); goto L_0116F0;  /* 0117C0 braw 116f0 */
L_0117C4:
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x28u, 16, 0); } hle_insn(ctx, 8);  /* 0117C4 cmpiw #40,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0117E6; } hle_insn(ctx, 8);  /* 0117C8 bnes 117e6 */
	{ uint32_t eaD = 0xFFFF9E1Au; uint32_t v = 0x3FCu; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 20);  /* 0117CA movew #1020,ffff9e1a */
	{ uint32_t eaD = 0xFFFF9E58u; uint32_t v = 0x430u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 20);  /* 0117D2 movew #1072,ffff9e58 */
	{ uint32_t v = 0x30Au; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0117DA movew #778,%d1 */
	hle_jump(ctx, 0x0117DE); return;  /* 0117DE bsrw 13228 */
L_0117E2:
	hle_insn(ctx, 10); goto L_0116F0;  /* 0117E2 braw 116f0 */
L_0117E6:
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x32u, 16, 0); } hle_insn(ctx, 8);  /* 0117E6 cmpiw #50,%d0 */
	if (COND_GE) { hle_insn(ctx, 10); goto L_011800; } hle_insn(ctx, 8);  /* 0117EA bges 11800 */
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 0117EC movew %d0,%d3 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[3] & 0xFFFFu), 0x1Du, 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 8);  /* 0117EE subiw #29,%d3 */
	hle_jump(ctx, 0x0117F2); return;  /* 0117F2 bsrw 118ea */
L_0117F6:
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 0117F6 movew %d0,%d1 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0x2F2u, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 0117F8 addiw #754,%d1 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x013228); return;  /* 0117FC braw 13228 */
L_011800:
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x37u, 16, 0); } hle_insn(ctx, 8);  /* 011800 cmpiw #55,%d0 */
	if (COND_GT) { hle_insn(ctx, 10); goto L_01181A; } hle_insn(ctx, 8);  /* 011804 bgts 1181a */
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 011806 movew %d0,%d3 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[3] & 0xFFFFu), 0x18u, 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 8);  /* 011808 subiw #24,%d3 */
	hle_jump(ctx, 0x01180C); return;  /* 01180C bsrw 118ea */
L_011810:
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 011810 movew %d0,%d1 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0x31Bu, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 011812 addiw #795,%d1 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x013228); return;  /* 011816 braw 13228 */
L_01181A:
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x3Du, 16, 0); } hle_insn(ctx, 8);  /* 01181A cmpiw #61,%d0 */
	if (COND_GT) { hle_insn(ctx, 10); goto L_011834; } hle_insn(ctx, 8);  /* 01181E bgts 11834 */
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 011820 movew %d0,%d3 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[3] & 0xFFFFu), 0x18u, 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 8);  /* 011822 subiw #24,%d3 */
	hle_jump(ctx, 0x011826); return;  /* 011826 bsrw 118ea */
L_01182A:
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 01182A movew %d0,%d1 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0x327u, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 01182C addiw #807,%d1 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x013228); return;  /* 011830 braw 13228 */
L_011834:
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x43u, 16, 0); } hle_insn(ctx, 8);  /* 011834 cmpiw #67,%d0 */
	if (COND_GT) { hle_insn(ctx, 10); goto L_011856; } hle_insn(ctx, 8);  /* 011838 bgts 11856 */
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 01183A movew %d0,%d3 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[3] & 0xFFFFu), 0x18u, 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 8);  /* 01183C subiw #24,%d3 */
	hle_jump(ctx, 0x011840); return;  /* 011840 bsrw 118ea */
L_011844:
	{ uint32_t v = 0x5u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 011844 movew #5,%d3 */
	hle_jump(ctx, 0x011848); return;  /* 011848 bsrw 118dc */
L_01184C:
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 01184C movew %d0,%d1 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0x315u, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 01184E addiw #789,%d1 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x013228); return;  /* 011852 braw 13228 */
L_011856:
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x49u, 16, 0); } hle_insn(ctx, 8);  /* 011856 cmpiw #73,%d0 */
	if (COND_GT) { hle_insn(ctx, 10); goto L_01186E; } hle_insn(ctx, 8);  /* 01185A bgts 1186e */
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 01185C movew %d0,%d3 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[3] & 0xFFFFu), 0x18u, 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 8);  /* 01185E subiw #24,%d3 */
	hle_jump(ctx, 0x011862); return;  /* 011862 bsrw 118ea */
L_011866:
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0x315u, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 011866 addiw #789,%d1 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x013228); return;  /* 01186A braw 13228 */
L_01186E:
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x4Bu, 16, 0); } hle_insn(ctx, 8);  /* 01186E cmpiw #75,%d0 */
	if (COND_GT) { hle_insn(ctx, 10); goto L_01189E; } hle_insn(ctx, 8);  /* 011872 bgts 1189e */
	if (COND_NE) { hle_insn(ctx, 10); goto L_01187E; } hle_insn(ctx, 8);  /* 011874 bnes 1187e */
	{ uint32_t v = 0x36Eu; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 011876 movew #878,%d1 */
	hle_jump(ctx, 0x01187A); return;  /* 01187A bsrw 13228 */
L_01187E:
	{ uint32_t eaD = 0xFFFF9E76u; uint32_t v = 0x1F4u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 20);  /* 01187E movew #500,ffff9e76 */
	{ uint32_t v = 0x6u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 011886 movew #6,%d3 */
	hle_jump(ctx, 0x01188A); return;  /* 01188A bsrw 118dc */
L_01188E:
	{ uint32_t v = 0x365u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 01188E movew #869,%d1 */
	hle_jump(ctx, 0x011892); return;  /* 011892 bsrw 13228 */
L_011896:
	{ uint32_t v = 0x366u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 011896 movew #870,%d1 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x013228); return;  /* 01189A braw 13228 */
L_01189E:
	{ uint32_t eaD = 0xFFFF9E76u; uint32_t v = 0x220u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 20);  /* 01189E movew #544,ffff9e76 */
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 0118A6 movew %d0,%d3 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[3] & 0xFFFFu), 0x45u, 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 8);  /* 0118A8 subiw #69,%d3 */
	hle_jump(ctx, 0x0118AC); return;  /* 0118AC bsrw 118dc */
L_0118B0:
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0x31Bu, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 0118B0 addiw #795,%d1 */
	hle_jump(ctx, 0x0118B4); return;  /* 0118B4 bsrw 13228 */
L_0118B8:
	{ uint32_t v = 0x365u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0118B8 movew #869,%d1 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x013228); return;  /* 0118BC braw 13228 */
}

HLE_RANGE_PURE(0x011616, Auto_011616, 200, 0x011624, 0x01164C, 0x011670, 0x011688, 0x0116AA, 0x0116C4, 0x0116DC, 0x0116F2, 0x011716, 0x01174A, 0x01176A, 0x011782, 0x0117AC, 0x0117DE, 0x0117F2, 0x01180C, 0x011826, 0x011840, 0x011862, 0x01187A, 0x01188A, 0x0118AC) { pc_run_011616(ctx, 0x011616); }
HLE_RANGE_PURE(0x011628, Auto_011628, 200, 0x01162C) { pc_run_011616(ctx, 0x011628); }
HLE_RANGE_PURE(0x011630, Auto_011630, 200, 0x013228) { pc_run_011616(ctx, 0x011630); }
HLE_RANGE_PURE(0x011650, Auto_011650, 200, 0x011654) { pc_run_011616(ctx, 0x011650); }
HLE_RANGE_PURE(0x011658, Auto_011658, 200, 0x01165C) { pc_run_011616(ctx, 0x011658); }
HLE_RANGE_PURE(0x011660, Auto_011660, 200, 0x013228) { pc_run_011616(ctx, 0x011660); }
HLE_RANGE_PURE(0x011674, Auto_011674, 200, 0x013228) { pc_run_011616(ctx, 0x011674); }
HLE_RANGE_PURE(0x01168C, Auto_01168C, 200, 0x011690) { pc_run_011616(ctx, 0x01168C); }
HLE_RANGE_PURE(0x011694, Auto_011694, 200, 0x013228) { pc_run_011616(ctx, 0x011694); }
HLE_RANGE_PURE(0x0116AE, Auto_0116AE, 200, 0x013228) { pc_run_011616(ctx, 0x0116AE); }
HLE_RANGE_PURE(0x0116C8, Auto_0116C8, 200, 0x013228) { pc_run_011616(ctx, 0x0116C8); }
HLE_RANGE_PURE(0x0116E0, Auto_0116E0, 200, 0x013228) { pc_run_011616(ctx, 0x0116E0); }
HLE_RANGE_PURE(0x0116F6, Auto_0116F6, 200, 0x0116FA) { pc_run_011616(ctx, 0x0116F6); }
HLE_RANGE_PURE(0x0116FE, Auto_0116FE, 200, 0x011702) { pc_run_011616(ctx, 0x0116FE); }
HLE_RANGE_PURE(0x011706, Auto_011706, 200, 0x013228) { pc_run_011616(ctx, 0x011706); }
HLE_RANGE_PURE(0x01171A, Auto_01171A, 200, 0x01171E) { pc_run_011616(ctx, 0x01171A); }
HLE_RANGE_PURE(0x011722, Auto_011722, 200, 0x011726) { pc_run_011616(ctx, 0x011722); }
HLE_RANGE_PURE(0x01172A, Auto_01172A, 200, 0x013228) { pc_run_011616(ctx, 0x01172A); }
HLE_RANGE_PURE(0x01174E, Auto_01174E, 200, 0x011752) { pc_run_011616(ctx, 0x01174E); }
HLE_RANGE_PURE(0x011756, Auto_011756, 200, 0x0116F2) { pc_run_011616(ctx, 0x011756); }
HLE_RANGE_PURE(0x01176E, Auto_01176E, 200, 0x0116F2) { pc_run_011616(ctx, 0x01176E); }
HLE_RANGE_PURE(0x011786, Auto_011786, 200, 0x011792) { pc_run_011616(ctx, 0x011786); }
HLE_RANGE_PURE(0x011796, Auto_011796, 200, 0x0116F2) { pc_run_011616(ctx, 0x011796); }
HLE_RANGE_PURE(0x0117B0, Auto_0117B0, 200, 0x0117BC) { pc_run_011616(ctx, 0x0117B0); }
HLE_RANGE_PURE(0x0117C0, Auto_0117C0, 200, 0x0116F2) { pc_run_011616(ctx, 0x0117C0); }
HLE_RANGE_PURE(0x0117E2, Auto_0117E2, 200, 0x0116F2) { pc_run_011616(ctx, 0x0117E2); }
HLE_RANGE_PURE(0x0117F6, Auto_0117F6, 200, 0x013228) { pc_run_011616(ctx, 0x0117F6); }
HLE_RANGE_PURE(0x011810, Auto_011810, 200, 0x013228) { pc_run_011616(ctx, 0x011810); }
HLE_RANGE_PURE(0x01182A, Auto_01182A, 200, 0x013228) { pc_run_011616(ctx, 0x01182A); }
HLE_RANGE_PURE(0x011844, Auto_011844, 200, 0x011848) { pc_run_011616(ctx, 0x011844); }
HLE_RANGE_PURE(0x01184C, Auto_01184C, 200, 0x013228) { pc_run_011616(ctx, 0x01184C); }
HLE_RANGE_PURE(0x011866, Auto_011866, 200, 0x013228) { pc_run_011616(ctx, 0x011866); }
HLE_RANGE_PURE(0x01187E, Auto_01187E, 200, 0x01188A) { pc_run_011616(ctx, 0x01187E); }
HLE_RANGE_PURE(0x01188E, Auto_01188E, 200, 0x011892) { pc_run_011616(ctx, 0x01188E); }
HLE_RANGE_PURE(0x011896, Auto_011896, 200, 0x013228) { pc_run_011616(ctx, 0x011896); }
HLE_RANGE_PURE(0x0118B0, Auto_0118B0, 200, 0x0118B4) { pc_run_011616(ctx, 0x0118B0); }
HLE_RANGE_PURE(0x0118B8, Auto_0118B8, 200, 0x013228) { pc_run_011616(ctx, 0x0118B8); }
static void pc_run_0118C0(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x0118C0: goto L_0118C0;
	default: __builtin_trap();
	}
L_0118C0:
	{ uint32_t eaS = 0x00011A30u; ctx->a[1] = eaS; } hle_insn(ctx, 8);  /* 0118C0 lea %pc@(11a30),%a1 */
	{ uint32_t eaS = 0xFFFF9E58u; uint32_t v = ld16(eaS); set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 0118C4 movew ffff9e58,%d6 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x0118F4); return;  /* 0118CA braw 118f4 */
}

HLE_RANGE_PURE(0x0118C0, Auto_0118C0, 200, 0x0118F4) { pc_run_0118C0(ctx, 0x0118C0); }
static void pc_run_0118DC(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x0118DC: goto L_0118DC;
	default: __builtin_trap();
	}
L_0118DC:
	{ uint32_t eaS = 0x000119F8u; ctx->a[1] = eaS; } hle_insn(ctx, 8);  /* 0118DC lea %pc@(119f8),%a1 */
	{ uint32_t eaS = 0xFFFF9E5Cu; uint32_t v = ld16(eaS); set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 0118E0 movew ffff9e5c,%d6 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x0118F4); return;  /* 0118E6 braw 118f4 */
}

HLE_RANGE_PURE(0x0118DC, Auto_0118DC, 200, 0x0118F4) { pc_run_0118DC(ctx, 0x0118DC); }
static void pc_run_0118EA(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x0118EA: goto L_0118EA;
	default: __builtin_trap();
	}
L_0118EA:
	{ uint32_t eaS = 0x00011928u; ctx->a[1] = eaS; } hle_insn(ctx, 8);  /* 0118EA lea %pc@(11928),%a1 */
	{ uint32_t eaS = 0xFFFF9E76u; uint32_t v = ld16(eaS); set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 16); hle_jump(ctx, 0x0118F4); return;  /* 0118EE movew ffff9e76,%d6 */
}

HLE_RANGE_PURE(0x0118EA, Auto_0118EA, 200, 0x0118F4) { pc_run_0118EA(ctx, 0x0118EA); }
static void pc_run_011B10(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x011B10: goto L_011B10;
	default: __builtin_trap();
	}
L_011B10:
	{ uint32_t eaD = 0xFFFF9EC2u; cc_logic8(ctx, ld8(eaD)); } hle_insn(ctx, 16);  /* 011B10 tstb ffff9ec2 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_011B4A; } hle_insn(ctx, 12);  /* 011B16 beqw 11b4a */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0x11A90u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 011B1A movel #72336,%fp@(32) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = 0xFFu; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 011B22 movew #255,%fp@(36) */
	{ uint32_t eaS = 0xFFFFAAFCu; uint32_t eaD = 0xFFFFAB1Cu; uint32_t v = ld16(eaS); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 28);  /* 011B28 movew ffffaafc,ffffab1c */
	{ uint32_t eaS = 0xFFFF9ECAu; ctx->a[1] = ld32(eaS); } hle_insn(ctx, 20);  /* 011B32 moveal ffff9eca,%a1 */
	{ uint32_t eaD = ctx->a[1] + (uint32_t)(int32_t)(32); uint32_t v = 0x11C2Cu; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 011B38 movel #72748,%a1@(32) */
	{ uint32_t eaD = 0xFFFF9EC2u; st8(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 20);  /* 011B40 clrb ffff9ec2 */
	{ uint32_t eaD = ctx->a[6];  unsigned b = 7; uint32_t m = 1u << b; uint8_t v = ld8(eaD); cc_set(ctx, CC_Z, (v & m) ? 0 : CC_Z); st8(eaD, v & ~m); } hle_insn(ctx, 16);  /* 011B46 bclr #15,%fp@ */
L_011B4A:
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 011B4A braw b3be */
}

HLE_RANGE_PURE(0x011B10, Auto_011B10, 200, 0x00B3BE) { pc_run_011B10(ctx, 0x011B10); }
static void pc_run_011F94(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x011F94: goto L_011F94;
	case 0x011F9C: goto L_011F9C;
	default: __builtin_trap();
	}
L_011F94:
	{ ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[5]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[4]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[3]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[2]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[1]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[0]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[2]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[1]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[0]); } hle_movem_l(ctx, 80, 9);  /* 011F94 moveml %d0-%d2/%a0-%a5,%sp@- */
	hle_jump(ctx, 0x011F98); return;  /* 011F98 jsr %pc@(11fa2) */
L_011F9C:
	{ ctx->d[0] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->d[1] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->d[2] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->a[0] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->a[1] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->a[2] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->a[3] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->a[4] = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->a[5] = ld32(ctx->a[7]); ctx->a[7] += 4; } hle_movem_l(ctx, 84, 9);  /* 011F9C moveml %sp@+,%d0-%d2/%a0-%a5 */
	hle_jump(ctx, 0x011FA0); return;  /* 011FA0 rts */
}

HLE_RANGE_PURE(0x011F94, Auto_011F94, 200, HLE_EXIT_SP(0x011F98, -36)) { pc_run_011F94(ctx, 0x011F94); }
HLE_RANGE_PURE(0x011F9C, Auto_011F9C, 200, HLE_EXIT_SP(0x011FA0, 36)) { pc_run_011F94(ctx, 0x011F9C); }
static void pc_run_012410(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x012410: goto L_012410;
	default: __builtin_trap();
	}
L_012410:
	hle_insn(ctx, 10); goto L_013184;  /* 012410 braw 13184 */
L_013184:
	{ uint32_t eaS = 0x000131B2u; ctx->a[5] = eaS; } hle_insn(ctx, 8); hle_jump(ctx, 0x013188); return;  /* 013184 lea %pc@(131b2),%a5 */
}

HLE_RANGE_PURE(0x012410, Auto_012410, 200, 0x013188) { pc_run_012410(ctx, 0x012410); }
static void pc_run_01241C(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x01241C: goto L_01241C;
	default: __builtin_trap();
	}
L_01241C:
	hle_insn(ctx, 10); hle_jump(ctx, 0x012FC2); return;  /* 01241C braw 12fc2 */
}

HLE_RANGE_PURE(0x01241C, Auto_01241C, 200, 0x012FC2) { pc_run_01241C(ctx, 0x01241C); }
static void pc_run_012420(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x012420: goto L_012420;
	case 0x01312E: goto L_01312E;
	case 0x01316E: goto L_01316E;
	default: __builtin_trap();
	}
L_012420:
	hle_insn(ctx, 10); goto L_013110;  /* 012420 braw 13110 */
L_013110:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = alu(ctx, OP_SUB, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 16);  /* 013110 subqw #1,%fp@(36) */
	if (COND_MI) { hle_insn(ctx, 10); goto L_013172; } hle_insn(ctx, 8);  /* 013114 bmis 13172 */
	{ uint32_t eaS = 0xFFFF8000u; ctx->a[4] = eaS; } hle_insn(ctx, 12);  /* 013116 lea ffff8000,%a4 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(30); cc_logic16(ctx, ld16(eaD)); } hle_insn(ctx, 12);  /* 01311C tstw %fp@(30) */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_013128; } hle_insn(ctx, 8);  /* 013120 beqs 13128 */
	{ uint32_t eaS = 0xFFFF8050u; ctx->a[4] = eaS; } hle_insn(ctx, 12);  /* 013122 lea ffff8050,%a4 */
L_013128:
	hle_jump(ctx, 0x013128); return;  /* 013128 jsr 881c */
L_01312E:
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 01312E movew %d0,%d1 */
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0xFu) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 013130 andiw #15,%d0 */
	{ cc_logic8(ctx, (ctx->d[1] & 0xFFu)); } hle_insn(ctx, 4);  /* 013134 tstb %d1 */
	if (COND_PL) { hle_insn(ctx, 10); goto L_01313A; } hle_insn(ctx, 8);  /* 013136 bpls 1313a */
	{ uint32_t v = alu(ctx, OP_SUB, 0, (ctx->d[0] & 0xFFFFu), 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 4);  /* 013138 negw %d0 */
L_01313A:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(38); uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), ld16(eaS), 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 12);  /* 01313A addw %fp@(38),%d0 */
	{ uint32_t eaD = 0xFFFF9E94u; uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 01313E movew %d0,ffff9e94 */
	{ uint32_t v = shf(ctx, 'r', (ctx->d[1] & 0xFFFFu), 8, 16); set_w(&ctx->d[1], v); } hle_insn(ctx, 22);  /* 013144 lsrw #8,%d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFFu); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 013146 movew %d1,%d0 */
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0xFu) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 013148 andiw #15,%d0 */
	{ cc_logic8(ctx, (ctx->d[1] & 0xFFu)); } hle_insn(ctx, 4);  /* 01314C tstb %d1 */
	if (COND_PL) { hle_insn(ctx, 10); goto L_013152; } hle_insn(ctx, 8);  /* 01314E bpls 13152 */
	{ uint32_t v = alu(ctx, OP_SUB, 0, (ctx->d[0] & 0xFFFFu), 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 4);  /* 013150 negw %d0 */
L_013152:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(40); uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), ld16(eaS), 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 12);  /* 013152 addw %fp@(40),%d0 */
	{ uint32_t eaD = 0xFFFF9E96u; uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 013156 movew %d0,ffff9e96 */
	{ uint32_t eaD = 0xFFFFA0CAu; uint32_t v = 0x8u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 20);  /* 01315C movew #8,ffffa0ca */
	{ ctx->a[7] -= 4; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->a[6] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 013164 movel %fp,%sp@- */
	{ ctx->a[7] -= 2; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->d[7] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 013166 movew %d7,%sp@- */
	hle_jump(ctx, 0x013168); return;  /* 013168 jsr 5892 */
L_01316E:
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 01316E movew %sp@+,%d7 */
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 4; ctx->a[6] = ld32(eaS); } hle_insn(ctx, 12);  /* 013170 moveal %sp@+,%fp */
L_013172:
	{ uint32_t eaS = 0x000131E2u; ctx->a[5] = eaS; } hle_insn(ctx, 8);  /* 013172 lea %pc@(131e2),%a5 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x013188); return;  /* 013176 bras 13188 */
}

HLE_RANGE_PURE(0x012420, Auto_012420, 200, 0x013128, 0x013188) { pc_run_012420(ctx, 0x012420); }
HLE_RANGE_PURE(0x01312E, Auto_01312E, 200, HLE_EXIT_SP(0x013168, -6)) { pc_run_012420(ctx, 0x01312E); }
HLE_RANGE_PURE(0x01316E, Auto_01316E, 200, HLE_EXIT_SP(0x013188, 6)) { pc_run_012420(ctx, 0x01316E); }
static void pc_run_012428(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x012428: goto L_012428;
	case 0x0130BE: goto L_0130BE;
	case 0x0130FA: goto L_0130FA;
	default: __builtin_trap();
	}
L_012428:
	hle_insn(ctx, 10); goto L_0130A2;  /* 012428 braw 130a2 */
L_0130A2:
	{ ctx->a[7] -= 2; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->d[7] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0130A2 movew %d7,%sp@- */
	{ ctx->a[7] -= 4; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->a[6] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 0130A4 movel %fp,%sp@- */
	{ uint32_t eaS = 0xFFFF8000u; ctx->a[4] = eaS; } hle_insn(ctx, 12);  /* 0130A6 lea ffff8000,%a4 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(30); cc_logic16(ctx, ld16(eaD)); } hle_insn(ctx, 12);  /* 0130AC tstw %fp@(30) */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0130B8; } hle_insn(ctx, 8);  /* 0130B0 beqs 130b8 */
	{ uint32_t eaS = 0xFFFF8050u; ctx->a[4] = eaS; } hle_insn(ctx, 12);  /* 0130B2 lea ffff8050,%a4 */
L_0130B8:
	hle_jump(ctx, 0x0130B8); return;  /* 0130B8 jsr 881c */
L_0130BE:
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 0130BE movew %d0,%d1 */
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0x1Fu) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0130C0 andiw #31,%d0 */
	{ cc_logic8(ctx, (ctx->d[1] & 0xFFu)); } hle_insn(ctx, 4);  /* 0130C4 tstb %d1 */
	if (COND_PL) { hle_insn(ctx, 10); goto L_0130CA; } hle_insn(ctx, 8);  /* 0130C6 bpls 130ca */
	{ uint32_t v = alu(ctx, OP_SUB, 0, (ctx->d[0] & 0xFFFFu), 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 4);  /* 0130C8 negw %d0 */
L_0130CA:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), ld16(eaS), 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 12);  /* 0130CA addw %fp@(2),%d0 */
	{ uint32_t eaD = 0xFFFF9E94u; uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 0130CE movew %d0,ffff9e94 */
	{ uint32_t v = shf(ctx, 'r', (ctx->d[1] & 0xFFFFu), 8, 16); set_w(&ctx->d[1], v); } hle_insn(ctx, 22);  /* 0130D4 lsrw #8,%d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFFu); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 0130D6 movew %d1,%d0 */
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0x1Fu) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0130D8 andiw #31,%d0 */
	{ cc_logic8(ctx, (ctx->d[1] & 0xFFu)); } hle_insn(ctx, 4);  /* 0130DC tstb %d1 */
	if (COND_PL) { hle_insn(ctx, 10); goto L_0130E2; } hle_insn(ctx, 8);  /* 0130DE bpls 130e2 */
	{ uint32_t v = alu(ctx, OP_SUB, 0, (ctx->d[0] & 0xFFFFu), 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 4);  /* 0130E0 negw %d0 */
L_0130E2:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(4); uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), ld16(eaS), 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 12);  /* 0130E2 addw %fp@(4),%d0 */
	{ uint32_t eaD = 0xFFFF9E96u; uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 0130E6 movew %d0,ffff9e96 */
	{ uint32_t eaD = 0xFFFFA0CAu; uint32_t v = 0x3u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 20);  /* 0130EC movew #3,ffffa0ca */
	hle_jump(ctx, 0x0130F4); return;  /* 0130F4 jsr 5892 */
L_0130FA:
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 4; ctx->a[6] = ld32(eaS); } hle_insn(ctx, 12);  /* 0130FA moveal %sp@+,%fp */
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0130FC movew %sp@+,%d7 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(20); uint32_t v = alu(ctx, OP_SUB, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 16);  /* 0130FE subqw #1,%fp@(20) */
	if (COND_NE) { hle_insn(ctx, 10); goto L_01310C; } hle_insn(ctx, 8);  /* 013102 bnes 1310c */
	{ uint32_t eaD = ctx->a[6]; st16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 12);  /* 013104 clrw %fp@ */
	{ uint32_t eaD = 0xFFFF80D6u; uint32_t v = alu(ctx, OP_SUB, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 20);  /* 013106 subqw #1,ffff80d6 */
L_01310C:
	hle_insn(ctx, 10); hle_jump(ctx, 0x012432); return;  /* 01310C braw 12432 */
}

HLE_RANGE_PURE(0x012428, Auto_012428, 200, HLE_EXIT_SP(0x0130B8, -6)) { pc_run_012428(ctx, 0x012428); }
HLE_RANGE_PURE(0x0130BE, Auto_0130BE, 200, 0x0130F4) { pc_run_012428(ctx, 0x0130BE); }
HLE_RANGE_PURE(0x0130FA, Auto_0130FA, 200, HLE_EXIT_SP(0x012432, 6)) { pc_run_012428(ctx, 0x0130FA); }
static void pc_run_012432(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x012432: goto L_012432;
	default: __builtin_trap();
	}
L_012432:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(42); ctx->a[6] = eaS; } hle_insn(ctx, 8);  /* 012432 lea %fp@(42),%fp */
	{ uint16_t v = (uint16_t)ctx->d[7] - 1; set_w(&ctx->d[7], v); if (v != 0xFFFF) { hle_insn(ctx, 10); hle_jump(ctx, 0x012402); return; } hle_insn(ctx, 14); } hle_jump(ctx, 0x01243A); return;  /* 012436 dbf %d7,12402 */
}

HLE_RANGE_PURE(0x012432, Auto_012432, 200, 0x012402, 0x01243A) { pc_run_012432(ctx, 0x012432); }
static void pc_run_01243A(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	default: __builtin_trap();
	}
L_01243A:
	hle_jump(ctx, 0x01243A); return;  /* 01243A rts */
}

static void pc_run_012A16(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x012A16: goto L_012A16;
	case 0x012A2A: goto L_012A2A;
	case 0x012A32: goto L_012A32;
	case 0x012A3A: goto L_012A3A;
	case 0x012A42: goto L_012A42;
	default: __builtin_trap();
	}
L_012A16:
	{ ctx->a[7] -= 4; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->a[6] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 012A16 movel %fp,%sp@- */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(6); uint32_t v = ld16(eaS); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 012A18 movew %fp@(6),%d3 */
	{ alu(ctx, OP_SUB, (ctx->d[2] & 0xFFu), 0x3u, 8, 0); } hle_insn(ctx, 8);  /* 012A1C cmpib #3,%d2 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_012A3E; } hle_insn(ctx, 8);  /* 012A20 bnes 12a3e */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x5u, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 4);  /* 012A22 subqw #5,%d0 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[1] & 0xFFFFu), 0x2u, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 4);  /* 012A24 subqw #2,%d1 */
	hle_jump(ctx, 0x012A26); return;  /* 012A26 jsr %pc@(12a46) */
L_012A2A:
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[1] & 0xFFFFu), 0xAu, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 012A2A subiw #10,%d1 */
	hle_jump(ctx, 0x012A2E); return;  /* 012A2E jsr %pc@(12a46) */
L_012A32:
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), 0xAu, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 8);  /* 012A32 addiw #10,%d0 */
	hle_jump(ctx, 0x012A36); return;  /* 012A36 jsr %pc@(12a46) */
L_012A3A:
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0xAu, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 012A3A addiw #10,%d1 */
L_012A3E:
	hle_jump(ctx, 0x012A3E); return;  /* 012A3E jsr %pc@(12a46) */
L_012A42:
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 4; ctx->a[6] = ld32(eaS); } hle_insn(ctx, 12);  /* 012A42 moveal %sp@+,%fp */
	hle_jump(ctx, 0x012A44); return;  /* 012A44 rts */
}

HLE_RANGE_PURE(0x012A16, Auto_012A16, 200, HLE_EXIT_SP(0x012A26, -4), HLE_EXIT_SP(0x012A3E, -4)) { pc_run_012A16(ctx, 0x012A16); }
HLE_RANGE_PURE(0x012A2A, Auto_012A2A, 200, 0x012A2E) { pc_run_012A16(ctx, 0x012A2A); }
HLE_RANGE_PURE(0x012A32, Auto_012A32, 200, 0x012A36) { pc_run_012A16(ctx, 0x012A32); }
HLE_RANGE_PURE(0x012A3A, Auto_012A3A, 200, 0x012A3E) { pc_run_012A16(ctx, 0x012A3A); }
HLE_RANGE_PURE(0x012A42, Auto_012A42, 200, HLE_EXIT_SP(0x012A44, 4)) { pc_run_012A16(ctx, 0x012A42); }
static void pc_run_012A46(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x012A4A: goto L_012A4A;
	default: __builtin_trap();
	}
L_012A46:
	hle_jump(ctx, 0x012A46); return;  /* 012A46 jsr %pc@(1296e) */
L_012A4A:
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
	hle_jump(ctx, 0x012A76); return;  /* 012A76 rts */
}

HLE_RANGE_PURE(0x012A4A, Auto_012A4A, 200, 0x012A76) { pc_run_012A46(ctx, 0x012A4A); }
static void pc_run_012A78(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x012A78: goto L_012A78;
	case 0x012A8E: goto L_012A8E;
	case 0x012A98: goto L_012A98;
	default: __builtin_trap();
	}
L_012A78:
	{ ctx->a[7] -= 4; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->a[6] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 012A78 movel %fp,%sp@- */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(2); ctx->d[0] = (uint32_t)(int32_t)(int16_t)ld16(eaS + 0); ctx->d[1] = (uint32_t)(int32_t)(int16_t)ld16(eaS + 2); } hle_insn(ctx, 24);  /* 012A7A movemw %fp@(2),%d0-%d1 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0x24u, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 012A80 addiw #36,%d1 */
	{ ctx->d[2] = 0x00000002u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 012A84 moveq #2,%d2 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(6); uint32_t v = ld16(eaS); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 012A86 movew %fp@(6),%d3 */
	hle_jump(ctx, 0x012A8A); return;  /* 012A8A jsr %pc@(12a46) */
L_012A8E:
	{ uint32_t v = 0xB0u; set_w(&ctx->d[4], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 012A8E movew #176,%d4 */
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 012A92 movew %d0,%d3 */
	hle_jump(ctx, 0x012A94); return;  /* 012A94 jsr %pc@(12abe) */
L_012A98:
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 4; ctx->a[6] = ld32(eaS); } hle_insn(ctx, 12);  /* 012A98 moveal %sp@+,%fp */
	hle_jump(ctx, 0x012A9A); return;  /* 012A9A rts */
}

HLE_RANGE_PURE(0x012A78, Auto_012A78, 200, HLE_EXIT_SP(0x012A8A, -4)) { pc_run_012A78(ctx, 0x012A78); }
HLE_RANGE_PURE(0x012A8E, Auto_012A8E, 200, 0x012A94) { pc_run_012A78(ctx, 0x012A8E); }
HLE_RANGE_PURE(0x012A98, Auto_012A98, 200, HLE_EXIT_SP(0x012A9A, 4)) { pc_run_012A78(ctx, 0x012A98); }
static void pc_run_012A9C(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x012A9C: goto L_012A9C;
	case 0x012AB0: goto L_012AB0;
	case 0x012ABA: goto L_012ABA;
	default: __builtin_trap();
	}
L_012A9C:
	{ ctx->a[7] -= 4; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->a[6] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 012A9C movel %fp,%sp@- */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(2); ctx->d[0] = (uint32_t)(int32_t)(int16_t)ld16(eaS + 0); ctx->d[1] = (uint32_t)(int32_t)(int16_t)ld16(eaS + 2); } hle_insn(ctx, 24);  /* 012A9E movemw %fp@(2),%d0-%d1 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0x14u, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 012AA4 addiw #20,%d1 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(6); uint32_t v = ld16(eaS); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 012AA8 movew %fp@(6),%d3 */
	hle_jump(ctx, 0x012AAC); return;  /* 012AAC jsr %pc@(12a46) */
L_012AB0:
	{ uint32_t v = 0xB8u; set_w(&ctx->d[4], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 012AB0 movew #184,%d4 */
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 012AB4 movew %d0,%d3 */
	hle_jump(ctx, 0x012AB6); return;  /* 012AB6 jsr %pc@(12abe) */
L_012ABA:
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 4; ctx->a[6] = ld32(eaS); } hle_insn(ctx, 12);  /* 012ABA moveal %sp@+,%fp */
	hle_jump(ctx, 0x012ABC); return;  /* 012ABC rts */
}

HLE_RANGE_PURE(0x012A9C, Auto_012A9C, 200, HLE_EXIT_SP(0x012AAC, -4)) { pc_run_012A9C(ctx, 0x012A9C); }
HLE_RANGE_PURE(0x012AB0, Auto_012AB0, 200, 0x012AB6) { pc_run_012A9C(ctx, 0x012AB0); }
HLE_RANGE_PURE(0x012ABA, Auto_012ABA, 200, HLE_EXIT_SP(0x012ABC, 4)) { pc_run_012A9C(ctx, 0x012ABA); }
static void pc_run_012ABE(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x012ABE: goto L_012ABE;
	default: __builtin_trap();
	}
L_012ABE:
	{ uint32_t eaD = ctx->a[6];  unsigned b = 0; uint32_t m = 1u << b; uint8_t v = ld8(eaD); cc_set(ctx, CC_Z, (v & m) ? 0 : CC_Z); st8(eaD, v | m); } hle_insn(ctx, 16);  /* 012ABE bset #0,%fp@ */
	{ uint32_t v = 0x1u; set_b(&ctx->d[5], v); cc_logic8(ctx, v); } hle_insn(ctx, 8);  /* 012AC2 moveb #1,%d5 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[3] & 0xFFFFu), (ctx->d[0] & 0xFFFFu), 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 4);  /* 012AC6 subw %d0,%d3 */
	if (COND_PL) { hle_insn(ctx, 10); goto L_012ACE; } hle_insn(ctx, 8);  /* 012AC8 bpls 12ace */
	{ uint32_t v = alu(ctx, OP_SUB, 0, (ctx->d[5] & 0xFFu), 8, 1); set_b(&ctx->d[5], v); } hle_insn(ctx, 4);  /* 012ACA negb %d5 */
	{ uint32_t v = alu(ctx, OP_SUB, 0, (ctx->d[3] & 0xFFFFu), 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 4);  /* 012ACC negw %d3 */
L_012ACE:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(14); uint32_t v = (ctx->d[5] & 0xFFu); st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 12);  /* 012ACE moveb %d5,%fp@(14) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(10); uint32_t v = (ctx->d[3] & 0xFFu); st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 12);  /* 012AD2 moveb %d3,%fp@(10) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(11); st8(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 16);  /* 012AD6 clrb %fp@(11) */
	{ uint32_t v = 0x1u; set_b(&ctx->d[5], v); cc_logic8(ctx, v); } hle_insn(ctx, 8);  /* 012ADA moveb #1,%d5 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[4] & 0xFFFFu), (ctx->d[1] & 0xFFFFu), 16, 1); set_w(&ctx->d[4], v); } hle_insn(ctx, 4);  /* 012ADE subw %d1,%d4 */
	if (COND_PL) { hle_insn(ctx, 10); goto L_012AE6; } hle_insn(ctx, 8);  /* 012AE0 bpls 12ae6 */
	{ uint32_t v = alu(ctx, OP_SUB, 0, (ctx->d[5] & 0xFFu), 8, 1); set_b(&ctx->d[5], v); } hle_insn(ctx, 4);  /* 012AE2 negb %d5 */
	{ uint32_t v = alu(ctx, OP_SUB, 0, (ctx->d[4] & 0xFFFFu), 16, 1); set_w(&ctx->d[4], v); } hle_insn(ctx, 4);  /* 012AE4 negw %d4 */
L_012AE6:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(15); uint32_t v = (ctx->d[5] & 0xFFu); st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 12);  /* 012AE6 moveb %d5,%fp@(15) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(12); uint32_t v = (ctx->d[4] & 0xFFu); st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 12);  /* 012AEA moveb %d4,%fp@(12) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(13); st8(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 16);  /* 012AEE clrb %fp@(13) */
	hle_jump(ctx, 0x012AF2); return;  /* 012AF2 rts */
}

HLE_RANGE_PURE(0x012ABE, Auto_012ABE, 200, 0x012AF2) { pc_run_012ABE(ctx, 0x012ABE); }
static void pc_run_012AF4(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x012AF4: goto L_012AF4;
	default: __builtin_trap();
	}
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
	hle_jump(ctx, 0x012B5A); return;  /* 012B5A rts */
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
	hle_jump(ctx, 0x012BAE); return;  /* 012BAE rts */
}

HLE_RANGE_PURE(0x012AF4, Auto_012AF4, 200, 0x012B5A, 0x012BAE) { pc_run_012AF4(ctx, 0x012AF4); }
static void pc_run_012D00(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x012D00: goto L_012D00;
	case 0x012D14: goto L_012D14;
	case 0x012D9A: goto L_012D9A;
	case 0x012DD4: goto L_012DD4;
	default: __builtin_trap();
	}
L_012D00:
	{ uint32_t eaS = ctx->a[4] + (uint32_t)(int32_t)(38); uint32_t v = ld16(eaS); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 012D00 movew %a4@(38),%d3 */
	{ uint32_t eaS = ctx->a[4] + (uint32_t)(int32_t)(40); uint32_t v = ld16(eaS); set_w(&ctx->d[4], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 012D04 movew %a4@(40),%d4 */
	{ uint32_t eaS = ctx->a[4] + (uint32_t)(int32_t)(36); uint32_t v = ld16(eaS); set_w(&ctx->d[5], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 012D08 movew %a4@(36),%d5 */
	{ uint32_t t_ = ctx->d[0]; ctx->d[0] = ctx->d[3]; ctx->d[3] = t_; } hle_insn(ctx, 6);  /* 012D0C exg %d0,%d3 */
	{ uint32_t t_ = ctx->d[1]; ctx->d[1] = ctx->d[4]; ctx->d[4] = t_; } hle_insn(ctx, 6);  /* 012D0E exg %d1,%d4 */
	hle_jump(ctx, 0x012D10); return;  /* 012D10 jsr %pc@(1290e) */
L_012D14:
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
	hle_jump(ctx, 0x012D94); return;  /* 012D94 jsr 881c */
L_012D9A:
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
	hle_jump(ctx, 0x012DCE); return;  /* 012DCE jsr 73a0 */
L_012DD4:
	{ uint32_t eaD = 0xFFFF80D6u; uint32_t v = alu(ctx, OP_ADD, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 20);  /* 012DD4 addqw #1,ffff80d6 */
L_012DDA:
	hle_jump(ctx, 0x012DDA); return;  /* 012DDA rts */
}

HLE_RANGE_PURE(0x012D00, Auto_012D00, 200, 0x012D10) { pc_run_012D00(ctx, 0x012D00); }
HLE_RANGE_PURE(0x012D14, Auto_012D14, 200, 0x012D94, 0x012DCE, 0x012DDA) { pc_run_012D00(ctx, 0x012D14); }
HLE_RANGE_PURE(0x012D9A, Auto_012D9A, 200, 0x012DCE, 0x012DDA) { pc_run_012D00(ctx, 0x012D9A); }
HLE_RANGE_PURE(0x012DD4, Auto_012DD4, 200, 0x012DDA) { pc_run_012D00(ctx, 0x012DD4); }
static void pc_run_013052(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x013052: goto L_013052;
	case 0x01307E: goto L_01307E;
	default: __builtin_trap();
	}
L_013052:
	{ ctx->a[7] -= 2; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->d[7] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 013052 movew %d7,%sp@- */
	{ ctx->a[7] -= 4; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->a[6] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 013054 movel %fp,%sp@- */
	{ uint32_t eaS = 0xFFFF8000u; ctx->a[4] = eaS; } hle_insn(ctx, 12);  /* 013056 lea ffff8000,%a4 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(30); cc_logic16(ctx, ld16(eaD)); } hle_insn(ctx, 12);  /* 01305C tstw %fp@(30) */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_013068; } hle_insn(ctx, 8);  /* 013060 beqs 13068 */
	{ uint32_t eaS = 0xFFFF8050u; ctx->a[4] = eaS; } hle_insn(ctx, 12);  /* 013062 lea ffff8050,%a4 */
L_013068:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t eaD = 0xFFFF9E94u; uint32_t v = ld32(eaS); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 32);  /* 013068 movel %fp@(2),ffff9e94 */
	{ uint32_t eaD = 0xFFFFA0CAu; uint32_t v = 0x29u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 20);  /* 013070 movew #41,ffffa0ca */
	hle_jump(ctx, 0x013078); return;  /* 013078 jsr 5892 */
L_01307E:
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 4; ctx->a[6] = ld32(eaS); } hle_insn(ctx, 12);  /* 01307E moveal %sp@+,%fp */
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 013080 movew %sp@+,%d7 */
	{ uint32_t eaD = 0xFFFF80CCu; alu(ctx, OP_SUB, ld16(eaD), 0x2u, 16, 0); } hle_insn(ctx, 20);  /* 013082 cmpiw #2,ffff80cc */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_013096; } hle_insn(ctx, 8);  /* 01308A beqs 13096 */
	{ uint32_t eaD = ctx->a[6]; st16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 12);  /* 01308C clrw %fp@ */
	{ uint32_t eaD = 0xFFFF80D6u; uint32_t v = alu(ctx, OP_SUB, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 20);  /* 01308E subqw #1,ffff80d6 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x01303E); return;  /* 013094 bras 1303e */
L_013096:
	{ uint32_t eaD = ctx->a[6]; uint32_t v = 0x7u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 013096 movew #7,%fp@ */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(20); uint32_t v = 0xCu; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 01309A movew #12,%fp@(20) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x01303E); return;  /* 0130A0 bras 1303e */
}

HLE_RANGE_PURE(0x013052, Auto_013052, 200, HLE_EXIT_SP(0x013078, -6)) { pc_run_013052(ctx, 0x013052); }
HLE_RANGE_PURE(0x01307E, Auto_01307E, 200, HLE_EXIT_SP(0x01303E, 6)) { pc_run_013052(ctx, 0x01307E); }
static void pc_run_0131F2(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x0131F2: goto L_0131F2;
	default: __builtin_trap();
	}
L_0131F2:
	{ uint32_t eaD = 0xFFFF9E98u; uint32_t v = 0x4A88u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 28);  /* 0131F2 movel #19080,ffff9e98 */
	{ uint32_t eaD = 0xFFFF8030u; st16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 20);  /* 0131FC clrw ffff8030 */
	{ uint32_t eaD = 0xFFFF8080u; st16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 20);  /* 013202 clrw ffff8080 */
	hle_jump(ctx, 0x013208); return;  /* 013208 rts */
}

HLE_RANGE_PURE(0x0131F2, Auto_0131F2, 200, 0x013208) { pc_run_0131F2(ctx, 0x0131F2); }
static void pc_run_01320A(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x01320A: goto L_01320A;
	default: __builtin_trap();
	}
L_01320A:
	{ uint32_t eaS = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); hle_jump(ctx, 0x01320C); return;  /* 01320A movew %a0@+,%d0 */
}

HLE_RANGE_PURE(0x01320A, Auto_01320A, 200, 0x01320C) { pc_run_01320A(ctx, 0x01320A); }
static void pc_run_01320C(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x01320C: goto L_01320C;
	default: __builtin_trap();
	}
L_01320C:
	{ uint32_t v = shf(ctx, 'a', (ctx->d[0] & 0xFFFFu), 1, 16); set_w(&ctx->d[0], v); } hle_insn(ctx, 8);  /* 01320C asrw #1,%d0 */
	{ uint32_t eaD = 0xFFFFA0E0u; uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 01320E movew %d0,ffffa0e0 */
	{ uint32_t eaD = 0xFFFFA0E2u; uint32_t v = (ctx->a[0] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 20);  /* 013214 movel %a0,ffffa0e2 */
	{ uint32_t v = shf(ctx, 'L', (ctx->d[1] & 0xFFFFu), 5, 16); set_w(&ctx->d[1], v); } hle_insn(ctx, 16);  /* 01321A aslw #5,%d1 */
	{ uint32_t eaD = 0xFFFFA0E6u; uint32_t v = (ctx->d[1] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 01321C movew %d1,ffffa0e6 */
	hle_jump(ctx, 0x013222); return;  /* 013222 jsr %pc@(11fa2) */
L_013226:
	hle_jump(ctx, 0x013226); return;  /* 013226 rts */
}

HLE_RANGE_PURE(0x01320C, Auto_01320C, 200, 0x013222) { pc_run_01320C(ctx, 0x01320C); }
static void pc_run_013470(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x013470: goto L_013470;
	default: __builtin_trap();
	}
L_013470:
	{ ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[6]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[5]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[4]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[3]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[2]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[1]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[0]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[7]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[6]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[5]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[4]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[3]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[2]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[1]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[0]); } hle_movem_l(ctx, 128, 15);  /* 013470 moveml %d0-%fp,%sp@- */
	{ uint32_t eaS = 0x00013480u; ctx->a[4] = eaS; } hle_insn(ctx, 8);  /* 013474 lea %pc@(13480),%a4 */
	{ uint32_t eaS = ctx->a[4] + (uint32_t)(int32_t)(0) + (uint32_t)(int32_t)(int16_t)ctx->d[5]; ctx->a[4] = ld32(eaS); } hle_insn(ctx, 18);  /* 013478 moveal %a4@(0,%d5:w),%a4 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x013866); return;  /* 01347C braw 13866 */
}

HLE_RANGE_PURE(0x013470, Auto_013470, 200, HLE_EXIT_SP(0x013866, -60)) { pc_run_013470(ctx, 0x013470); }
static void pc_run_01385A(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x01385A: goto L_01385A;
	default: __builtin_trap();
	}
L_01385A:
	{ ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[6]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[5]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[4]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[3]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[2]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[1]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->a[0]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[7]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[6]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[5]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[4]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[3]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[2]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[1]); ctx->a[7] -= 4; st32(ctx->a[7], ctx->d[0]); } hle_movem_l(ctx, 128, 15);  /* 01385A moveml %d0-%fp,%sp@- */
	{ uint32_t eaS = 0x0001390Eu; ctx->a[4] = eaS; } hle_insn(ctx, 8);  /* 01385E lea %pc@(1390e),%a4 */
	{ uint32_t eaS = ctx->a[4] + (uint32_t)(int32_t)(0) + (uint32_t)(int32_t)(int16_t)ctx->d[5]; ctx->a[4] = ld32(eaS); } hle_insn(ctx, 18); hle_jump(ctx, 0x013866); return;  /* 013862 moveal %a4@(0,%d5:w),%a4 */
}

HLE_RANGE_PURE(0x01385A, Auto_01385A, 200, HLE_EXIT_SP(0x013866, -60)) { pc_run_01385A(ctx, 0x01385A); }
static void pc_run_013866(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x013866: goto L_013866;
	case 0x013888: goto L_013888;
	default: __builtin_trap();
	}
L_013866:
	{ uint32_t eaS = 0xFFFFA238u; uint32_t v = ld16(eaS); set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 013866 movew ffffa238,%d6 */
	{ uint32_t v = alu(ctx, OP_SUB, 0, (ctx->d[6] & 0xFFFFu), 16, 1); set_w(&ctx->d[6], v); } hle_insn(ctx, 4);  /* 01386C negw %d6 */
	{ uint32_t v = ((ctx->d[6] & 0xFFFFu) & 0x1FFu) & 0xFFFFu; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 01386E andiw #511,%d6 */
	{ uint32_t eaD = 0xFFFFA238u; uint32_t v = (ctx->d[6] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 013872 movew %d6,ffffa238 */
	{ uint32_t eaS = ctx->a[4]; ctx->a[4] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 013878 movew %a4@+,%d3 */
L_01387A:
	{ uint32_t eaD = ctx->a[4]; alu(ctx, OP_SUB, ld16(eaD), 0xFFFFu, 16, 0); } hle_insn(ctx, 12);  /* 01387A cmpiw #-1,%a4@ */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_013908; } hle_insn(ctx, 12);  /* 01387E beqw 13908 */
	hle_jump(ctx, 0x013882); return;  /* 013882 jsr 930e */
L_013888:
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
	hle_jump(ctx, 0x01390C); return;  /* 01390C rts */
}

HLE_RANGE_PURE(0x013866, Auto_013866, 200, 0x013882, HLE_EXIT_SP(0x01390C, 60)) { pc_run_013866(ctx, 0x013866); }
HLE_RANGE_PURE(0x013888, Auto_013888, 200, 0x013882, HLE_EXIT_SP(0x01390C, 60)) { pc_run_013866(ctx, 0x013888); }
static void pc_run_013C74(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x013C74: goto L_013C74;
	case 0x013C86: goto L_013C86;
	case 0x013C96: goto L_013C96;
	case 0x013CBA: goto L_013CBA;
	default: __builtin_trap();
	}
L_013C74:
	{ ctx->a[7] -= 4; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->a[0] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 013C74 movel %a0,%sp@- */
	{ uint32_t v = 0xF8u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 013C76 movew #248,%d0 */
L_013C7A:
	{ ctx->a[7] -= 2; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 013C7A movew %d0,%sp@- */
	hle_jump(ctx, 0x013C7C); return;  /* 013C7C jsr %pc@(11d12) */
L_013C80:
	hle_jump(ctx, 0x013C80); return;  /* 013C80 jsr 4752 */
L_013C86:
	{ uint32_t eaD = 0xFFFF80CCu; alu(ctx, OP_SUB, ld16(eaD), 0x5u, 16, 0); } hle_insn(ctx, 20);  /* 013C86 cmpiw #5,ffff80cc */
	if (COND_NE) { hle_insn(ctx, 10); goto L_013C96; } hle_insn(ctx, 8);  /* 013C8E bnes 13c96 */
	hle_jump(ctx, 0x013C90); return;  /* 013C90 jsr 4820 */
L_013C96:
	{ uint32_t eaD = 0xFFFF80CCu; alu(ctx, OP_SUB, ld16(eaD), 0x6u, 16, 0); } hle_insn(ctx, 20);  /* 013C96 cmpiw #6,ffff80cc */
	if (COND_NE) { hle_insn(ctx, 10); goto L_013CA6; } hle_insn(ctx, 8);  /* 013C9E bnes 13ca6 */
	hle_jump(ctx, 0x013CA0); return;  /* 013CA0 jsr 47ce */
L_013CA6:
	hle_jump(ctx, 0x013CA6); return;  /* 013CA6 jsr 13376 */
L_013CAC:
	hle_jump(ctx, 0x013CAC); return;  /* 013CAC jsr b398 */
L_013CB2:
	hle_jump(ctx, 0x013CB2); return;  /* 013CB2 jsr %pc@(11d28) */
L_013CB6:
	hle_jump(ctx, 0x013CB6); return;  /* 013CB6 jsr %pc@(11d32) */
L_013CBA:
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 013CBA movew %sp@+,%d0 */
	{ uint16_t v = (uint16_t)ctx->d[0] - 1; set_w(&ctx->d[0], v); if (v != 0xFFFF) { hle_insn(ctx, 10); goto L_013C7A; } hle_insn(ctx, 14); }  /* 013CBC dbf %d0,13c7a */
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 4; ctx->a[0] = ld32(eaS); } hle_insn(ctx, 12);  /* 013CC0 moveal %sp@+,%a0 */
	hle_jump(ctx, 0x013CC2); return;  /* 013CC2 rts */
}

HLE_RANGE_PURE(0x013C74, Auto_013C74, 200, HLE_EXIT_SP(0x013C7C, -6)) { pc_run_013C74(ctx, 0x013C74); }
HLE_RANGE_PURE(0x013C86, Auto_013C86, 200, 0x013C90, 0x013CA0, 0x013CA6) { pc_run_013C74(ctx, 0x013C86); }
HLE_RANGE_PURE(0x013C96, Auto_013C96, 200, 0x013CA0, 0x013CA6) { pc_run_013C74(ctx, 0x013C96); }
HLE_RANGE_PURE(0x013CBA, Auto_013CBA, 200, 0x013C7C, HLE_EXIT_SP(0x013CC2, 6)) { pc_run_013C74(ctx, 0x013CBA); }
static void pc_run_013D90(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	default: __builtin_trap();
	}
L_013D90:
	hle_jump(ctx, 0x013D90); return;  /* 013D90 rts */
}

static void pc_run_013D92(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x013D92: goto L_013D92;
	default: __builtin_trap();
	}
L_013D92:
	{ uint32_t v = 0xE0000u; ctx->d[0] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 013D92 movel #917504,%d0 */
	{ uint32_t v = 0x20C2u; ctx->d[2] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 013D98 movel #8386,%d2 */
	{ uint32_t v = 0xCAu; set_b(&ctx->d[1], v); cc_logic8(ctx, v); } hle_insn(ctx, 8);  /* 013D9E moveb #-54,%d1 */
	{ ctx->d[3] = 0x00000002u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 013DA2 moveq #2,%d3 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x013D56); return;  /* 013DA4 braw 13d56 */
}

HLE_RANGE_PURE(0x013D92, Auto_013D92, 200, 0x013D56) { pc_run_013D92(ctx, 0x013D92); }
static void pc_run_013DA8(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x013DA8: goto L_013DA8;
	default: __builtin_trap();
	}
L_013DA8:
	{ uint32_t v = 0xE20C2u; ctx->d[0] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 013DA8 movel #925890,%d0 */
	{ uint32_t v = 0x11E2u; ctx->d[2] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 013DAE movel #4578,%d2 */
	{ uint32_t v = 0xCAu; set_b(&ctx->d[1], v); cc_logic8(ctx, v); } hle_insn(ctx, 8);  /* 013DB4 moveb #-54,%d1 */
	{ ctx->d[3] = 0x00000003u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 013DB8 moveq #3,%d3 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x013D56); return;  /* 013DBA braw 13d56 */
}

HLE_RANGE_PURE(0x013DA8, Auto_013DA8, 200, 0x013D56) { pc_run_013DA8(ctx, 0x013DA8); }
static void pc_run_013DBE(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x013DBE: goto L_013DBE;
	default: __builtin_trap();
	}
L_013DBE:
	{ uint32_t v = 0xE32A4u; ctx->d[0] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 013DBE movel #930468,%d0 */
	{ uint32_t v = 0x27C2u; ctx->d[2] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 013DC4 movel #10178,%d2 */
	{ uint32_t v = 0xCAu; set_b(&ctx->d[1], v); cc_logic8(ctx, v); } hle_insn(ctx, 8);  /* 013DCA moveb #-54,%d1 */
	{ ctx->d[3] = 0x00000004u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 013DCE moveq #4,%d3 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x013D56); return;  /* 013DD0 braw 13d56 */
}

HLE_RANGE_PURE(0x013DBE, Auto_013DBE, 200, 0x013D56) { pc_run_013DBE(ctx, 0x013DBE); }
static void pc_run_013DD4(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	default: __builtin_trap();
	}
L_013DD4:
	hle_jump(ctx, 0x013DD4); return;  /* 013DD4 rts */
}

static void pc_run_013DD6(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x013DD6: goto L_013DD6;
	default: __builtin_trap();
	}
L_013DD6:
	{ uint32_t v = 0xE5A66u; ctx->d[0] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 013DD6 movel #940646,%d0 */
	{ uint32_t v = 0x2342u; ctx->d[2] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 013DDC movel #9026,%d2 */
	{ uint32_t v = 0xCAu; set_b(&ctx->d[1], v); cc_logic8(ctx, v); } hle_insn(ctx, 8);  /* 013DE2 moveb #-54,%d1 */
	{ ctx->d[3] = 0x00000006u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 013DE6 moveq #6,%d3 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x013D56); return;  /* 013DE8 braw 13d56 */
}

HLE_RANGE_PURE(0x013DD6, Auto_013DD6, 200, 0x013D56) { pc_run_013DD6(ctx, 0x013DD6); }
static void pc_run_013DEC(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x013DEC: goto L_013DEC;
	default: __builtin_trap();
	}
L_013DEC:
	{ uint32_t v = 0xE8000u; ctx->d[0] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 013DEC movel #950272,%d0 */
	{ uint32_t v = 0x3082u; ctx->d[2] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 013DF2 movel #12418,%d2 */
	{ uint32_t v = 0xCAu; set_b(&ctx->d[1], v); cc_logic8(ctx, v); } hle_insn(ctx, 8);  /* 013DF8 moveb #-54,%d1 */
	{ ctx->d[3] = 0x00000007u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 013DFC moveq #7,%d3 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x013D56); return;  /* 013DFE braw 13d56 */
}

HLE_RANGE_PURE(0x013DEC, Auto_013DEC, 200, 0x013D56) { pc_run_013DEC(ctx, 0x013DEC); }
static void pc_run_013E02(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	default: __builtin_trap();
	}
L_013E02:
	hle_jump(ctx, 0x013E02); return;  /* 013E02 rts */
}

static void pc_run_013E1A(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x013E1A: goto L_013E1A;
	default: __builtin_trap();
	}
L_013E1A:
	{ uint32_t v = 0xECDA4u; ctx->d[0] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 013E1A movel #970148,%d0 */
	{ uint32_t v = 0x1D42u; ctx->d[2] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 013E20 movel #7490,%d2 */
	{ uint32_t v = 0xCAu; set_b(&ctx->d[1], v); cc_logic8(ctx, v); } hle_insn(ctx, 8);  /* 013E26 moveb #-54,%d1 */
	{ ctx->d[3] = 0x0000000Au; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 013E2A moveq #10,%d3 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x013D56); return;  /* 013E2C braw 13d56 */
}

HLE_RANGE_PURE(0x013E1A, Auto_013E1A, 200, 0x013D56) { pc_run_013E1A(ctx, 0x013E1A); }
static void pc_run_014510(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x014510: goto L_014510;
	default: __builtin_trap();
	}
L_014510:
	{ uint32_t eaS = 0xFFFF8000u; ctx->a[4] = eaS; } hle_insn(ctx, 12);  /* 014510 lea ffff8000,%a4 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(6); cc_logic16(ctx, ld16(eaD)); } hle_insn(ctx, 12);  /* 014516 tstw %fp@(6) */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_014522; } hle_insn(ctx, 8);  /* 01451A beqs 14522 */
	{ uint32_t eaS = 0xFFFF8050u; ctx->a[4] = eaS; } hle_insn(ctx, 12);  /* 01451C lea ffff8050,%a4 */
L_014522:
	{ uint32_t v = shf(ctx, 'L', (ctx->d[0] & 0xFFFFu), 2, 16); set_w(&ctx->d[0], v); } hle_insn(ctx, 10);  /* 014522 aslw #2,%d0 */
	hle_jump(ctx, 0x014524); return;  /* 014524 jmp %pc@(14528,%d0:w) */
}

HLE_RANGE_PURE(0x014510, Auto_014510, 200, 0x014524) { pc_run_014510(ctx, 0x014510); }
static void pc_run_01452C(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x01452C: goto L_01452C;
	default: __builtin_trap();
	}
L_01452C:
	hle_insn(ctx, 10); goto L_01458A;  /* 01452C braw 1458a */
L_01458A:
	{ uint32_t v = 0x7u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 01458A movew #7,%d0 */
	hle_jump(ctx, 0x01458E); return;  /* 01458E jsr 13e30 */
L_014594:
	hle_jump(ctx, 0x014594); return;  /* 014594 jmp 719a */
}

HLE_RANGE_PURE(0x01452C, Auto_01452C, 200, 0x01458E) { pc_run_01452C(ctx, 0x01452C); }
static void pc_run_014530(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x014530: goto L_014530;
	default: __builtin_trap();
	}
L_014530:
	hle_insn(ctx, 10); goto L_014566;  /* 014530 braw 14566 */
L_014566:
	{ ctx->d[0] = 0x00000010u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 014566 moveq #16,%d0 */
	hle_jump(ctx, 0x014568); return;  /* 014568 jsr 7c7c */
L_01456E:
	hle_jump(ctx, 0x01456E); return;  /* 01456E rts */
}

HLE_RANGE_PURE(0x014530, Auto_014530, 200, 0x014568) { pc_run_014530(ctx, 0x014530); }
static void pc_run_01453C(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x01453C: goto L_01453C;
	case 0x01457A: goto L_01457A;
	default: __builtin_trap();
	}
L_01453C:
	hle_insn(ctx, 10); goto L_014570;  /* 01453C braw 14570 */
L_014570:
	{ uint32_t v = 0x4u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 014570 movew #4,%d0 */
	hle_jump(ctx, 0x014574); return;  /* 014574 jsr 13e30 */
L_01457A:
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(46); uint32_t v = 0xFA24u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 01457A movew #-1500,%a4@(46) */
	hle_jump(ctx, 0x014580); return;  /* 014580 rts */
}

HLE_RANGE_PURE(0x01453C, Auto_01453C, 200, 0x014574) { pc_run_01453C(ctx, 0x01453C); }
HLE_RANGE_PURE(0x01457A, Auto_01457A, 200, 0x014580) { pc_run_01453C(ctx, 0x01457A); }
static void pc_run_014540(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x014540: goto L_014540;
	default: __builtin_trap();
	}
L_014540:
	hle_insn(ctx, 10); goto L_014582;  /* 014540 braw 14582 */
L_014582:
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(28); uint32_t v = 0xFFFFu; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 014582 movew #-1,%a4@(28) */
	hle_jump(ctx, 0x014588); return;  /* 014588 rts */
}

HLE_RANGE_PURE(0x014540, Auto_014540, 200, 0x014588) { pc_run_014540(ctx, 0x014540); }
static void pc_run_0145A0(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x0145A0: goto L_0145A0;
	case 0x0145C8: goto L_0145C8;
	case 0x0145DC: goto L_0145DC;
	case 0x0145E6: goto L_0145E6;
	default: __builtin_trap();
	}
L_0145A0:
	{ ctx->a[7] -= 2; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->d[7] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0145A0 movew %d7,%sp@- */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 0145A2 movew %fp@(36),%d0 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x1u, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 4);  /* 0145A6 subqw #1,%d0 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 0145A8 movew %d0,%fp@(36) */
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0xFFE8u, 16, 0); } hle_insn(ctx, 8);  /* 0145AC cmpiw #-24,%d0 */
	if (COND_LE) { hle_insn(ctx, 10); goto L_0145CC; } hle_insn(ctx, 8);  /* 0145B0 bles 145cc */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(4); uint32_t v = alu(ctx, OP_ADD, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 16);  /* 0145B2 addqw #1,%fp@(4) */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(4); uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 0145B6 movew %fp@(4),%d0 */
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0xFFu) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0145BA andiw #255,%d0 */
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) ^ 0xFFu) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0145BE eoriw #255,%d0 */
	{ ctx->d[1] = 0x00000000u; cc_logic32(ctx, ctx->d[1]); } hle_insn(ctx, 4);  /* 0145C2 moveq #0,%d1 */
	hle_jump(ctx, 0x0145C4); return;  /* 0145C4 jsr %pc@(11f58) */
L_0145C8:
	hle_insn(ctx, 10); goto L_0145F4;  /* 0145C8 braw 145f4 */
L_0145CC:
	if (COND_NE) { hle_insn(ctx, 10); goto L_0145E0; } hle_insn(ctx, 8);  /* 0145CC bnes 145e0 */
	{ uint32_t v = 0x0u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0145CE movew #0,%d0 */
	{ ctx->d[1] = 0x00000000u; cc_logic32(ctx, ctx->d[1]); } hle_insn(ctx, 4);  /* 0145D2 moveq #0,%d1 */
	hle_jump(ctx, 0x0145D4); return;  /* 0145D4 jsr %pc@(11f58) */
L_0145D8:
	hle_jump(ctx, 0x0145D8); return;  /* 0145D8 bsrw d0f8 */
L_0145DC:
	hle_insn(ctx, 10); goto L_0145F4;  /* 0145DC braw 145f4 */
L_0145E0:
	hle_jump(ctx, 0x0145E0); return;  /* 0145E0 jsr a4b2 */
L_0145E6:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0x145FCu; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 0145E6 movel #83452,%fp@(32) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = 0x0u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 0145EE movew #0,%fp@(36) */
L_0145F4:
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0145F4 movew %sp@+,%d7 */
	hle_jump(ctx, 0x0145F6); return;  /* 0145F6 jmp b3be */
}

HLE_RANGE_PURE(0x0145A0, Auto_0145A0, 200, HLE_EXIT_SP(0x0145C4, -2), HLE_EXIT_SP(0x0145D4, -2), HLE_EXIT_SP(0x0145E0, -2)) { pc_run_0145A0(ctx, 0x0145A0); }
HLE_RANGE_PURE(0x0145C8, Auto_0145C8, 200, HLE_EXIT_SP(0x0145F6, 2)) { pc_run_0145A0(ctx, 0x0145C8); }
HLE_RANGE_PURE(0x0145DC, Auto_0145DC, 200, HLE_EXIT_SP(0x0145F6, 2)) { pc_run_0145A0(ctx, 0x0145DC); }
HLE_RANGE_PURE(0x0145E6, Auto_0145E6, 200, HLE_EXIT_SP(0x0145F6, 2)) { pc_run_0145A0(ctx, 0x0145E6); }
static void pc_run_0145FC(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x0145FC: goto L_0145FC;
	case 0x01462C: goto L_01462C;
	case 0x014644: goto L_014644;
	case 0x014652: goto L_014652;
	case 0x01466A: goto L_01466A;
	case 0x014678: goto L_014678;
	case 0x014690: goto L_014690;
	case 0x0146B8: goto L_0146B8;
	case 0x0146C6: goto L_0146C6;
	case 0x0146D4: goto L_0146D4;
	case 0x0146EC: goto L_0146EC;
	case 0x0146FA: goto L_0146FA;
	case 0x014712: goto L_014712;
	default: __builtin_trap();
	}
L_0145FC:
	{ ctx->a[7] -= 2; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->d[7] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0145FC movew %d7,%sp@- */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 0145FE movew %fp@(36),%d0 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x1u, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 4);  /* 014602 subqw #1,%d0 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 014604 movew %d0,%fp@(36) */
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0xFFFFu, 16, 0); } hle_insn(ctx, 8);  /* 014608 cmpiw #-1,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_014630; } hle_insn(ctx, 8);  /* 01460C bnes 14630 */
	{ uint32_t v = 0xE150u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 01460E movew #-7856,%d1 */
	{ uint32_t eaS = 0x0003B20Au; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 014612 lea 3b20a,%a0 */
	{ ctx->d[2] = 0x00000018u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 014618 moveq #24,%d2 */
	{ ctx->d[3] = 0x00000018u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 01461A moveq #24,%d3 */
	{ uint32_t v = 0xE000u; ctx->d[6] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 01461C movel #57344,%d6 */
	{ uint32_t eaS = 0xFFFF9E34u; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 014622 movew ffff9e34,%d7 */
	hle_jump(ctx, 0x014628); return;  /* 014628 jsr %pc@(12234) */
L_01462C:
	hle_insn(ctx, 10); goto L_014732;  /* 01462C braw 14732 */
L_014630:
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0xFF40u, 16, 0); } hle_insn(ctx, 8);  /* 014630 cmpiw #-192,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_014656; } hle_insn(ctx, 8);  /* 014634 bnes 14656 */
	{ uint32_t v = 0xE100u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 014636 movew #-7936,%d1 */
	{ uint32_t eaS = 0x0003B22Au; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 01463A lea 3b22a,%a0 */
	hle_jump(ctx, 0x014640); return;  /* 014640 bsrw 14750 */
L_014644:
	{ uint32_t v = 0xE120u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 014644 movew #-7904,%d1 */
	{ uint32_t eaS = 0x0003B22Au; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 014648 lea 3b22a,%a0 */
	hle_jump(ctx, 0x01464E); return;  /* 01464E bsrw 1477c */
L_014652:
	hle_insn(ctx, 10); goto L_014732;  /* 014652 braw 14732 */
L_014656:
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0xFE80u, 16, 0); } hle_insn(ctx, 8);  /* 014656 cmpiw #-384,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_01467C; } hle_insn(ctx, 8);  /* 01465A bnes 1467c */
	{ uint32_t v = 0xE130u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 01465C movew #-7888,%d1 */
	{ uint32_t eaS = 0x0003B22Au; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 014660 lea 3b22a,%a0 */
	hle_jump(ctx, 0x014666); return;  /* 014666 bsrw 14750 */
L_01466A:
	{ uint32_t v = 0xE150u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 01466A movew #-7856,%d1 */
	{ uint32_t eaS = 0x0003B22Au; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 01466E lea 3b22a,%a0 */
	hle_jump(ctx, 0x014674); return;  /* 014674 bsrw 1477c */
L_014678:
	hle_insn(ctx, 10); goto L_014732;  /* 014678 braw 14732 */
L_01467C:
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0xFDC0u, 16, 0); } hle_insn(ctx, 8);  /* 01467C cmpiw #-576,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_014694; } hle_insn(ctx, 8);  /* 014680 bnes 14694 */
	{ uint32_t v = 0xE160u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 014682 movew #-7840,%d1 */
	{ uint32_t eaS = 0x0003B22Au; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 014686 lea 3b22a,%a0 */
	hle_jump(ctx, 0x01468C); return;  /* 01468C bsrw 1477c */
L_014690:
	hle_insn(ctx, 10); goto L_014732;  /* 014690 braw 14732 */
L_014694:
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0xFD00u, 16, 0); } hle_insn(ctx, 8);  /* 014694 cmpiw #-768,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0146D8; } hle_insn(ctx, 8);  /* 014698 bnes 146d8 */
	{ uint32_t v = 0xE110u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 01469A movew #-7920,%d1 */
	{ uint32_t eaS = 0x0003B21Au; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 01469E lea 3b21a,%a0 */
	{ ctx->d[2] = 0x00000008u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 0146A4 moveq #8,%d2 */
	{ ctx->d[3] = 0x00000018u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 0146A6 moveq #24,%d3 */
	{ uint32_t v = 0xE000u; ctx->d[6] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 0146A8 movel #57344,%d6 */
	{ uint32_t eaS = 0xFFFF9E34u; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 0146AE movew ffff9e34,%d7 */
	hle_jump(ctx, 0x0146B4); return;  /* 0146B4 jsr %pc@(1227a) */
L_0146B8:
	{ uint32_t v = 0xE120u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0146B8 movew #-7904,%d1 */
	{ uint32_t eaS = 0x0003B24Au; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 0146BC lea 3b24a,%a0 */
	hle_jump(ctx, 0x0146C2); return;  /* 0146C2 bsrw 1477c */
L_0146C6:
	{ uint32_t v = 0xE130u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0146C6 movew #-7888,%d1 */
	{ uint32_t eaS = 0x000A6ED6u; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 0146CA lea a6ed6,%a0 */
	hle_jump(ctx, 0x0146D0); return;  /* 0146D0 bsrw 14766 */
L_0146D4:
	hle_insn(ctx, 10); goto L_014732;  /* 0146D4 braw 14732 */
L_0146D8:
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0xFC40u, 16, 0); } hle_insn(ctx, 8);  /* 0146D8 cmpiw #-960,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0146FE; } hle_insn(ctx, 8);  /* 0146DC bnes 146fe */
	{ uint32_t v = 0xE140u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0146DE movew #-7872,%d1 */
	{ uint32_t eaS = 0x000A6EE6u; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 0146E2 lea a6ee6,%a0 */
	hle_jump(ctx, 0x0146E8); return;  /* 0146E8 bsrw 1473a */
L_0146EC:
	{ uint32_t v = 0xE160u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0146EC movew #-7840,%d1 */
	{ uint32_t eaS = 0x000A6F06u; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 0146F0 lea a6f06,%a0 */
	hle_jump(ctx, 0x0146F6); return;  /* 0146F6 bsrw 14766 */
L_0146FA:
	hle_insn(ctx, 10); goto L_014732;  /* 0146FA braw 14732 */
L_0146FE:
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0xFB80u, 16, 0); } hle_insn(ctx, 8);  /* 0146FE cmpiw #-1152,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_014716; } hle_insn(ctx, 8);  /* 014702 bnes 14716 */
	{ uint32_t v = 0xE170u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 014704 movew #-7824,%d1 */
	{ uint32_t eaS = 0x000A6F16u; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 014708 lea a6f16,%a0 */
	hle_jump(ctx, 0x01470E); return;  /* 01470E bsrw 14766 */
L_014712:
	hle_insn(ctx, 10); goto L_014732;  /* 014712 braw 14732 */
L_014716:
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0xFB40u, 16, 0); } hle_insn(ctx, 8);  /* 014716 cmpiw #-1216,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_014732; } hle_insn(ctx, 8);  /* 01471A bnes 14732 */
	{ uint32_t eaD = 0xFFFF9E98u; uint32_t v = 0x4A88u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 28);  /* 01471C movel #19080,ffff9e98 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0x14792u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 014726 movel #83858,%fp@(32) */
	hle_insn(ctx, 10); goto L_014732;  /* 01472E braw 14732 */
L_014732:
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 014732 movew %sp@+,%d7 */
	hle_jump(ctx, 0x014734); return;  /* 014734 jmp b3be */
}

HLE_RANGE_PURE(0x0145FC, Auto_0145FC, 200, HLE_EXIT_SP(0x014628, -2), HLE_EXIT_SP(0x014640, -2), HLE_EXIT_SP(0x014666, -2), HLE_EXIT_SP(0x01468C, -2), HLE_EXIT_SP(0x0146B4, -2), HLE_EXIT_SP(0x0146E8, -2), HLE_EXIT_SP(0x01470E, -2), 0x014734) { pc_run_0145FC(ctx, 0x0145FC); }
HLE_RANGE_PURE(0x01462C, Auto_01462C, 200, HLE_EXIT_SP(0x014734, 2)) { pc_run_0145FC(ctx, 0x01462C); }
HLE_RANGE_PURE(0x014644, Auto_014644, 200, 0x01464E) { pc_run_0145FC(ctx, 0x014644); }
HLE_RANGE_PURE(0x014652, Auto_014652, 200, HLE_EXIT_SP(0x014734, 2)) { pc_run_0145FC(ctx, 0x014652); }
HLE_RANGE_PURE(0x01466A, Auto_01466A, 200, 0x014674) { pc_run_0145FC(ctx, 0x01466A); }
HLE_RANGE_PURE(0x014678, Auto_014678, 200, HLE_EXIT_SP(0x014734, 2)) { pc_run_0145FC(ctx, 0x014678); }
HLE_RANGE_PURE(0x014690, Auto_014690, 200, HLE_EXIT_SP(0x014734, 2)) { pc_run_0145FC(ctx, 0x014690); }
HLE_RANGE_PURE(0x0146B8, Auto_0146B8, 200, 0x0146C2) { pc_run_0145FC(ctx, 0x0146B8); }
HLE_RANGE_PURE(0x0146C6, Auto_0146C6, 200, 0x0146D0) { pc_run_0145FC(ctx, 0x0146C6); }
HLE_RANGE_PURE(0x0146D4, Auto_0146D4, 200, HLE_EXIT_SP(0x014734, 2)) { pc_run_0145FC(ctx, 0x0146D4); }
HLE_RANGE_PURE(0x0146EC, Auto_0146EC, 200, 0x0146F6) { pc_run_0145FC(ctx, 0x0146EC); }
HLE_RANGE_PURE(0x0146FA, Auto_0146FA, 200, HLE_EXIT_SP(0x014734, 2)) { pc_run_0145FC(ctx, 0x0146FA); }
HLE_RANGE_PURE(0x014712, Auto_014712, 200, HLE_EXIT_SP(0x014734, 2)) { pc_run_0145FC(ctx, 0x014712); }
static void pc_run_01473A(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x01473A: goto L_01473A;
	default: __builtin_trap();
	}
L_01473A:
	{ uint32_t eaS = 0xFFFF9E38u; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 01473A movew ffff9e38,%d7 */
	{ ctx->d[2] = 0x00000010u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 014740 moveq #16,%d2 */
	{ ctx->d[3] = 0x00000018u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 014742 moveq #24,%d3 */
	{ uint32_t v = 0x8000u; ctx->d[6] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 014744 movel #32768,%d6 */
	hle_jump(ctx, 0x01474A); return;  /* 01474A jsr %pc@(12234) */
L_01474E:
	hle_jump(ctx, 0x01474E); return;  /* 01474E rts */
}

HLE_RANGE_PURE(0x01473A, Auto_01473A, 200, 0x01474A) { pc_run_01473A(ctx, 0x01473A); }
static void pc_run_014750(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x014750: goto L_014750;
	default: __builtin_trap();
	}
L_014750:
	{ uint32_t eaS = 0xFFFF9E34u; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 014750 movew ffff9e34,%d7 */
	{ ctx->d[2] = 0x00000010u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 014756 moveq #16,%d2 */
	{ ctx->d[3] = 0x00000018u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 014758 moveq #24,%d3 */
	{ uint32_t v = 0xE000u; ctx->d[6] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 01475A movel #57344,%d6 */
	hle_jump(ctx, 0x014760); return;  /* 014760 jsr %pc@(12234) */
L_014764:
	hle_jump(ctx, 0x014764); return;  /* 014764 rts */
}

HLE_RANGE_PURE(0x014750, Auto_014750, 200, 0x014760) { pc_run_014750(ctx, 0x014750); }
static void pc_run_014766(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x014766: goto L_014766;
	default: __builtin_trap();
	}
L_014766:
	{ uint32_t eaS = 0xFFFF9E38u; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 014766 movew ffff9e38,%d7 */
	{ ctx->d[2] = 0x00000008u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 01476C moveq #8,%d2 */
	{ ctx->d[3] = 0x00000018u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 01476E moveq #24,%d3 */
	{ uint32_t v = 0x8000u; ctx->d[6] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 014770 movel #32768,%d6 */
	hle_jump(ctx, 0x014776); return;  /* 014776 jsr %pc@(12234) */
L_01477A:
	hle_jump(ctx, 0x01477A); return;  /* 01477A rts */
}

HLE_RANGE_PURE(0x014766, Auto_014766, 200, 0x014776) { pc_run_014766(ctx, 0x014766); }
static void pc_run_01477C(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x01477C: goto L_01477C;
	default: __builtin_trap();
	}
L_01477C:
	{ uint32_t eaS = 0xFFFF9E34u; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 01477C movew ffff9e34,%d7 */
	{ ctx->d[2] = 0x00000008u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 014782 moveq #8,%d2 */
	{ ctx->d[3] = 0x00000018u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 014784 moveq #24,%d3 */
	{ uint32_t v = 0xE000u; ctx->d[6] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 014786 movel #57344,%d6 */
	hle_jump(ctx, 0x01478C); return;  /* 01478C jsr %pc@(12234) */
L_014790:
	hle_jump(ctx, 0x014790); return;  /* 014790 rts */
}

HLE_RANGE_PURE(0x01477C, Auto_01477C, 200, 0x01478C) { pc_run_01477C(ctx, 0x01477C); }
static void pc_run_014866(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x01486C: goto L_01486C;
	case 0x014880: goto L_014880;
	default: __builtin_trap();
	}
L_014866:
	hle_jump(ctx, 0x014866); return;  /* 014866 jsr b4a6 */
L_01486C:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = ld16(eaS); set_w(&ctx->d[5], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 01486C movew %fp@(36),%d5 */
	{ uint32_t v = ((ctx->d[5] & 0xFFFFu) & 0x38u) & 0xFFFFu; set_w(&ctx->d[5], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 014870 andiw #56,%d5 */
	{ uint32_t v = shf(ctx, 'r', (ctx->d[5] & 0xFFFFu), 3, 16); set_w(&ctx->d[5], v); } hle_insn(ctx, 12);  /* 014874 lsrw #3,%d5 */
	{ uint32_t v = ((ctx->d[5] & 0xFFFFu) ^ 0x7u) & 0xFFFFu; set_w(&ctx->d[5], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 014876 eoriw #7,%d5 */
	{ uint32_t v = (ctx->d[5] & 0xFFFFu); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 01487A movew %d5,%d1 */
	hle_jump(ctx, 0x01487C); return;  /* 01487C jsr %pc@(11616) */
L_014880:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = alu(ctx, OP_SUB, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 16);  /* 014880 subqw #1,%fp@(36) */
	if (COND_NE) { hle_insn(ctx, 10); goto L_01488E; } hle_insn(ctx, 8);  /* 014884 bnes 1488e */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0x14956u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 014886 movel #84310,%fp@(32) */
L_01488E:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(2); alu(ctx, OP_SUB, ld16(eaD), 0xD6u, 16, 0); } hle_insn(ctx, 16);  /* 01488E cmpiw #214,%fp@(2) */
	if (COND_LT) { hle_insn(ctx, 10); goto L_0148A8; } hle_insn(ctx, 8);  /* 014894 blts 148a8 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0x14A34u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 014896 movel #84532,%fp@(32) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = 0x7Fu; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 01489E movew #127,%fp@(36) */
	{ uint32_t eaD = ctx->a[6];  unsigned b = 7; uint32_t m = 1u << b; uint8_t v = ld8(eaD); cc_set(ctx, CC_Z, (v & m) ? 0 : CC_Z); st8(eaD, v & ~m); } hle_insn(ctx, 16);  /* 0148A4 bclr #15,%fp@ */
L_0148A8:
	hle_jump(ctx, 0x0148A8); return;  /* 0148A8 jmp b3be */
}

HLE_RANGE_PURE(0x01486C, Auto_01486C, 200, 0x01487C) { pc_run_014866(ctx, 0x01486C); }
HLE_RANGE_PURE(0x014880, Auto_014880, 200, 0x0148A8) { pc_run_014866(ctx, 0x014880); }
static void pc_run_014956(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x014956: goto L_014956;
	case 0x014960: goto L_014960;
	case 0x0149A2: goto L_0149A2;
	default: __builtin_trap();
	}
L_014956:
	{ uint32_t eaD = ctx->a[6];  unsigned b = 7; uint32_t m = 1u << b; uint8_t v = ld8(eaD); cc_set(ctx, CC_Z, (v & m) ? 0 : CC_Z); st8(eaD, v | m); } hle_insn(ctx, 16);  /* 014956 bset #15,%fp@ */
	hle_jump(ctx, 0x01495A); return;  /* 01495A jsr 881c */
L_014960:
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0x3u) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 014960 andiw #3,%d0 */
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x1u, 16, 0); } hle_insn(ctx, 8);  /* 014964 cmpiw #1,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_01497C; } hle_insn(ctx, 8);  /* 014968 bnes 1497c */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0x148F0u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 01496A movel #84208,%fp@(32) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = 0x20u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 014972 movew #32,%fp@(36) */
	hle_insn(ctx, 10); goto L_0149AE;  /* 014978 braw 149ae */
L_01497C:
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x2u, 16, 0); } hle_insn(ctx, 8);  /* 01497C cmpiw #2,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_014994; } hle_insn(ctx, 8);  /* 014980 bnes 14994 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0x148AEu; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 014982 movel #84142,%fp@(32) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = 0x40u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 01498A movew #64,%fp@(36) */
	hle_insn(ctx, 10); goto L_0149AE;  /* 014990 braw 149ae */
L_014994:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0x14866u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 014994 movel #84070,%fp@(32) */
	hle_jump(ctx, 0x01499C); return;  /* 01499C jsr 881c */
L_0149A2:
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0xFu) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0149A2 andiw #15,%d0 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), 0xAu, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 8);  /* 0149A6 addiw #10,%d0 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 0149AA movew %d0,%fp@(36) */
L_0149AE:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(32); ctx->a[1] = ld32(eaS); } hle_insn(ctx, 16);  /* 0149AE moveal %fp@(32),%a1 */
	hle_jump(ctx, 0x0149B2); return;  /* 0149B2 jmp %a1@ */
}

HLE_RANGE_PURE(0x014956, Auto_014956, 200, 0x01495A) { pc_run_014956(ctx, 0x014956); }
HLE_RANGE_PURE(0x014960, Auto_014960, 200, 0x01499C, 0x0149B2) { pc_run_014956(ctx, 0x014960); }
HLE_RANGE_PURE(0x0149A2, Auto_0149A2, 200, 0x0149B2) { pc_run_014956(ctx, 0x0149A2); }
static void pc_run_014AA6(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x014AA6: goto L_014AA6;
	default: __builtin_trap();
	}
L_014AA6:
	{ ctx->d[1] = 0x0000003Eu; cc_logic32(ctx, ctx->d[1]); } hle_insn(ctx, 4);  /* 014AA6 moveq #62,%d1 */
	hle_jump(ctx, 0x014AA8); return;  /* 014AA8 jsr %pc@(11616) */
L_014AAC:
	hle_jump(ctx, 0x014AAC); return;  /* 014AAC jmp b3be */
}

HLE_RANGE_PURE(0x014AA6, Auto_014AA6, 200, 0x014AA8) { pc_run_014AA6(ctx, 0x014AA6); }
static void pc_run_014AB2(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x014AB2: goto L_014AB2;
	default: __builtin_trap();
	}
L_014AB2:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(4); uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 014AB2 movew %fp@(4),%d0 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), 0x8u, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 4);  /* 014AB6 addqw #8,%d0 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(40); alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), ld16(eaS), 16, 0); } hle_insn(ctx, 12);  /* 014AB8 cmpw %fp@(40),%d0 */
	if (COND_LT) { hle_insn(ctx, 10); goto L_014AD0; } hle_insn(ctx, 8);  /* 014ABC blts 14ad0 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0x14AE2u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 014ABE movel #84706,%fp@(32) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = 0xFu; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 014AC6 movew #15,%fp@(36) */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(40); uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 014ACC movew %fp@(40),%d0 */
L_014AD0:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(4); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 014AD0 movew %d0,%fp@(4) */
	{ uint32_t v = 0x1Cu; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 014AD4 movew #28,%d1 */
	hle_jump(ctx, 0x014AD8); return;  /* 014AD8 bsrw 11616 */
L_014ADC:
	hle_jump(ctx, 0x014ADC); return;  /* 014ADC jmp b3be */
}

HLE_RANGE_PURE(0x014AB2, Auto_014AB2, 200, 0x014AD8) { pc_run_014AB2(ctx, 0x014AB2); }
static void pc_run_014AE2(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x014AE2: goto L_014AE2;
	case 0x014AEA: goto L_014AEA;
	default: __builtin_trap();
	}
L_014AE2:
	{ uint32_t v = 0x1Du; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 014AE2 movew #29,%d1 */
	hle_jump(ctx, 0x014AE6); return;  /* 014AE6 bsrw 11616 */
L_014AEA:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = alu(ctx, OP_SUB, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 16);  /* 014AEA subqw #1,%fp@(36) */
	if (COND_NE) { hle_insn(ctx, 10); goto L_014AF8; } hle_insn(ctx, 8);  /* 014AEE bnes 14af8 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0x14956u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 014AF0 movel #84310,%fp@(32) */
L_014AF8:
	hle_jump(ctx, 0x014AF8); return;  /* 014AF8 jmp b3be */
}

HLE_RANGE_PURE(0x014AE2, Auto_014AE2, 200, 0x014AE6) { pc_run_014AE2(ctx, 0x014AE2); }
HLE_RANGE_PURE(0x014AEA, Auto_014AEA, 200, 0x014AF8) { pc_run_014AE2(ctx, 0x014AEA); }
static void pc_run_014D80(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x014D80: goto L_014D80;
	case 0x014D90: goto L_014D90;
	default: __builtin_trap();
	}
L_014D80:
	{ ctx->d[1] = 0x00000011u; cc_logic32(ctx, ctx->d[1]); } hle_insn(ctx, 4);  /* 014D80 moveq #17,%d1 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); alu(ctx, OP_SUB, ld16(eaD), 0x3u, 16, 0); } hle_insn(ctx, 16);  /* 014D82 cmpiw #3,%fp@(36) */
	if (COND_GT) { hle_insn(ctx, 10); goto L_014D8C; } hle_insn(ctx, 8);  /* 014D88 bgts 14d8c */
	{ ctx->d[1] = 0x00000010u; cc_logic32(ctx, ctx->d[1]); } hle_insn(ctx, 4);  /* 014D8A moveq #16,%d1 */
L_014D8C:
	hle_jump(ctx, 0x014D8C); return;  /* 014D8C bsrw 11616 */
L_014D90:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = alu(ctx, OP_SUB, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 16);  /* 014D90 subqw #1,%fp@(36) */
	if (COND_NE) { hle_insn(ctx, 10); goto L_014D9E; } hle_insn(ctx, 8);  /* 014D94 bnes 14d9e */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0x14956u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 014D96 movel #84310,%fp@(32) */
L_014D9E:
	hle_jump(ctx, 0x014D9E); return;  /* 014D9E jmp b3be */
}

HLE_RANGE_PURE(0x014D80, Auto_014D80, 200, 0x014D8C) { pc_run_014D80(ctx, 0x014D80); }
HLE_RANGE_PURE(0x014D90, Auto_014D90, 200, 0x014D9E) { pc_run_014D80(ctx, 0x014D90); }
static void pc_run_014E4C(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x014E4C: goto L_014E4C;
	case 0x014E70: goto L_014E70;
	default: __builtin_trap();
	}
L_014E4C:
	{ ctx->d[1] = 0x0000000Cu; cc_logic32(ctx, ctx->d[1]); } hle_insn(ctx, 4);  /* 014E4C moveq #12,%d1 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); alu(ctx, OP_SUB, ld16(eaD), 0x46u, 16, 0); } hle_insn(ctx, 16);  /* 014E4E cmpiw #70,%fp@(36) */
	if (COND_LT) { hle_insn(ctx, 10); goto L_014E6C; } hle_insn(ctx, 8);  /* 014E54 blts 14e6c */
	{ ctx->d[1] = 0x0000000Bu; cc_logic32(ctx, ctx->d[1]); } hle_insn(ctx, 4);  /* 014E56 moveq #11,%d1 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); alu(ctx, OP_SUB, ld16(eaD), 0x50u, 16, 0); } hle_insn(ctx, 16);  /* 014E58 cmpiw #80,%fp@(36) */
	if (COND_LT) { hle_insn(ctx, 10); goto L_014E6C; } hle_insn(ctx, 8);  /* 014E5E blts 14e6c */
	{ ctx->d[1] = 0x0000000Au; cc_logic32(ctx, ctx->d[1]); } hle_insn(ctx, 4);  /* 014E60 moveq #10,%d1 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); alu(ctx, OP_SUB, ld16(eaD), 0x5Au, 16, 0); } hle_insn(ctx, 16);  /* 014E62 cmpiw #90,%fp@(36) */
	if (COND_LT) { hle_insn(ctx, 10); goto L_014E6C; } hle_insn(ctx, 8);  /* 014E68 blts 14e6c */
	{ ctx->d[1] = 0x00000009u; cc_logic32(ctx, ctx->d[1]); } hle_insn(ctx, 4);  /* 014E6A moveq #9,%d1 */
L_014E6C:
	hle_jump(ctx, 0x014E6C); return;  /* 014E6C bsrw 10666 */
L_014E70:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = alu(ctx, OP_SUB, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 16);  /* 014E70 subqw #1,%fp@(36) */
	if (COND_NE) { hle_insn(ctx, 10); goto L_014E7E; } hle_insn(ctx, 8);  /* 014E74 bnes 14e7e */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0x14E84u; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 014E76 movel #85636,%fp@(32) */
L_014E7E:
	hle_jump(ctx, 0x014E7E); return;  /* 014E7E jmp b3be */
}

HLE_RANGE_PURE(0x014E4C, Auto_014E4C, 200, 0x014E6C) { pc_run_014E4C(ctx, 0x014E4C); }
HLE_RANGE_PURE(0x014E70, Auto_014E70, 200, 0x014E7E) { pc_run_014E4C(ctx, 0x014E70); }
static void pc_run_014E84(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x014E84: goto L_014E84;
	default: __builtin_trap();
	}
L_014E84:
	{ ctx->d[1] = 0x0000000Cu; cc_logic32(ctx, ctx->d[1]); } hle_insn(ctx, 4);  /* 014E84 moveq #12,%d1 */
	hle_jump(ctx, 0x014E86); return;  /* 014E86 bsrw 10666 */
L_014E8A:
	hle_jump(ctx, 0x014E8A); return;  /* 014E8A jmp b3be */
}

HLE_RANGE_PURE(0x014E84, Auto_014E84, 200, 0x014E86) { pc_run_014E84(ctx, 0x014E84); }
static void pc_run_0151F4(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x0151F8: goto L_0151F8;
	default: __builtin_trap();
	}
L_0151F4:
	hle_jump(ctx, 0x0151F4); return;  /* 0151F4 jsr %pc@(12316) */
L_0151F8:
	{ ctx->d[0] = 0xFFFFFFE0u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 0151F8 moveq #-32,%d0 */
	hle_jump(ctx, 0x0151FA); return;  /* 0151FA jsr %pc@(12332) */
L_0151FE:
	hle_jump(ctx, 0x0151FE); return;  /* 0151FE bsrw 13228 */
L_015202:
	hle_jump(ctx, 0x015202); return;  /* 015202 jmp b3be */
}

HLE_RANGE_PURE(0x0151F8, Auto_0151F8, 200, 0x0151FA) { pc_run_0151F4(ctx, 0x0151F8); }
static void pc_run_015224(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	default: __builtin_trap();
	}
L_015224:
	hle_jump(ctx, 0x015224); return;  /* 015224 jmp b3be */
}

