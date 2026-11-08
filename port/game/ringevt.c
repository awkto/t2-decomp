/* The event ring consumer $013F7E (docs/remaining.md "Other") and its helpers: the bonus/powerup
 * message popup that is queued as byte pairs in the ring at $FF80A0 (head $FF80B0, tail $FF80B2), the
 * producer sub_013E30 ($013E30), the text-row printers sub_0141B8 / sub_0141E6.
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


static void pc_run(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x013E30: goto L_013E30;
	case 0x013F2E: goto L_013F2E;
	case 0x013F7E: goto L_013F7E;
	case 0x013F8E: goto L_013F8E;
	case 0x01400E: goto L_01400E;
	case 0x014046: goto L_014046;
	case 0x01404C: goto L_01404C;
	case 0x0140A2: goto L_0140A2;
	case 0x0140CE: goto L_0140CE;
	case 0x01413A: goto L_01413A;
	case 0x014154: goto L_014154;
	case 0x014162: goto L_014162;
	case 0x01417E: goto L_01417E;
	case 0x014198: goto L_014198;
	case 0x0141A0: goto L_0141A0;
	case 0x0141AE: goto L_0141AE;
	case 0x0141B8: goto L_0141B8;
	case 0x0141E6: goto L_0141E6;
	case 0x0141F4: goto L_0141F4;
	case 0x01423E: goto L_01423E;
	case 0x01426E: goto L_01426E;
	default: __builtin_trap();
	}
L_013E30:
	{ uint32_t eaD = 0xFFFF80B6u; cc_logic16(ctx, ld16(eaD)); } hle_insn(ctx, 16);  /* 013E30 tstw ffff80b6 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_013E5E; } hle_insn(ctx, 8);  /* 013E36 bnes 13e5e */
	{ uint32_t eaS = 0xFFFF80B2u; uint32_t v = ld16(eaS); set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 013E38 movew ffff80b2,%d2 */
	{ uint32_t eaS = ctx->a[4] + (uint32_t)(int32_t)(36); uint32_t v = ld16(eaS); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 013E3E movew %a4@(36),%d3 */
	{ uint32_t eaS = 0xFFFF80A0u; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 013E42 lea ffff80a0,%a0 */
	{ uint32_t eaD = ctx->a[0] + (uint32_t)(int32_t)(0) + (uint32_t)(int32_t)(int16_t)ctx->d[2]; uint32_t v = (ctx->d[0] & 0xFFu); st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 14);  /* 013E48 moveb %d0,%a0@(0,%d2:w) */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_013E5E; } hle_insn(ctx, 8);  /* 013E4C beqs 13e5e */
	{ uint32_t eaD = ctx->a[0] + (uint32_t)(int32_t)(1) + (uint32_t)(int32_t)(int16_t)ctx->d[2]; uint32_t v = (ctx->d[3] & 0xFFu); st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 14);  /* 013E4E moveb %d3,%a0@(1,%d2:w) */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[2] & 0xFFFFu), 0x2u, 16, 1); set_w(&ctx->d[2], v); } hle_insn(ctx, 4);  /* 013E52 addqw #2,%d2 */
	{ uint32_t v = ((ctx->d[2] & 0xFFFFu) & 0xFu) & 0xFFFFu; set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 013E54 andiw #15,%d2 */
	{ uint32_t eaD = 0xFFFF80B2u; uint32_t v = (ctx->d[2] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 013E58 movew %d2,ffff80b2 */
L_013E5E:
	hle_jump(ctx, 0x013E5E); return;  /* 013E5E rts */
L_013E60:
	{ ctx->a[7] -= 4; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->a[6] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 013E60 movel %fp,%sp@- */
	{ cc_logic8(ctx, (ctx->d[2] & 0xFFu)); } hle_insn(ctx, 4);  /* 013E62 tstb %d2 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_013E90; } hle_insn(ctx, 8);  /* 013E64 bnes 13e90 */
	{ uint32_t eaS = 0xFFFF8030u; uint32_t v = ld16(eaS); set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 013E66 movew ffff8030,%d2 */
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x5u, 16, 0); } hle_insn(ctx, 8);  /* 013E6C cmpiw #5,%d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_013E78; } hle_insn(ctx, 8);  /* 013E70 beqs 13e78 */
	{ unsigned b = (ctx->d[0] & 31); uint32_t m = 1u << b; cc_set(ctx, CC_Z, (ctx->d[2] & m) ? 0 : CC_Z); ctx->d[2] |= m; hle_insn(ctx, 6 + ((b > 15) ? 2 : 0)); }  /* 013E72 bset %d0,%d2 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_013F7A; } hle_insn(ctx, 12);  /* 013E74 bnew 13f7a */
L_013E78:
	{ uint32_t eaD = 0xFFFF8030u; uint32_t v = (ctx->d[2] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 013E78 movew %d2,ffff8030 */
	{ uint32_t eaS = 0xFFFF80E2u; ctx->a[6] = eaS; } hle_insn(ctx, 12);  /* 013E7E lea ffff80e2,%fp */
	{ uint32_t eaS = 0xFFFF8000u; ctx->a[4] = eaS; } hle_insn(ctx, 12);  /* 013E84 lea ffff8000,%a4 */
	{ uint32_t v = 0xE188u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 013E8A movew #-7800,%d3 */
	hle_insn(ctx, 10); goto L_013EB8;  /* 013E8E bras 13eb8 */
L_013E90:
	{ uint32_t eaS = 0xFFFF8080u; uint32_t v = ld16(eaS); set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 013E90 movew ffff8080,%d2 */
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x5u, 16, 0); } hle_insn(ctx, 8);  /* 013E96 cmpiw #5,%d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_013EA2; } hle_insn(ctx, 8);  /* 013E9A beqs 13ea2 */
	{ unsigned b = (ctx->d[0] & 31); uint32_t m = 1u << b; cc_set(ctx, CC_Z, (ctx->d[2] & m) ? 0 : CC_Z); ctx->d[2] |= m; hle_insn(ctx, 6 + ((b > 15) ? 2 : 0)); }  /* 013E9C bset %d0,%d2 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_013F7A; } hle_insn(ctx, 12);  /* 013E9E bnew 13f7a */
L_013EA2:
	{ uint32_t eaD = 0xFFFF8080u; uint32_t v = (ctx->d[2] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 013EA2 movew %d2,ffff8080 */
	{ uint32_t eaS = 0xFFFF80E2u; ctx->a[6] = eaS; } hle_insn(ctx, 12);  /* 013EA8 lea ffff80e2,%fp */
	{ uint32_t eaS = 0xFFFF8050u; ctx->a[4] = eaS; } hle_insn(ctx, 12);  /* 013EAE lea ffff8050,%a4 */
	{ uint32_t v = 0xE1A8u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 013EB4 movew #-7768,%d3 */
L_013EB8:
	{ uint32_t eaD = 0xFFFF80CCu; alu(ctx, OP_SUB, ld16(eaD), 0x1u, 16, 0); } hle_insn(ctx, 20);  /* 013EB8 cmpiw #1,ffff80cc */
	if (COND_NE) { hle_insn(ctx, 10); goto L_013EC6; } hle_insn(ctx, 8);  /* 013EC0 bnes 13ec6 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[3] & 0xFFFFu), 0x80u, 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 8);  /* 013EC2 subiw #128,%d3 */
L_013EC6:
	{ uint32_t eaD = 0xFFFF80B4u;  st8(eaD, 0xFF); } hle_insn(ctx, 20);  /* 013EC6 st ffff80b4 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(0); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 013ECC movew %d0,%fp@(0) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = (ctx->d[3] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 013ED0 movew %d3,%fp@(2) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(10); st16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 16);  /* 013ED4 clrw %fp@(10) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(8); uint32_t v = 0xC8u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 013ED8 movew #200,%fp@(8) */
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 013EDE movew %d0,%d2 */
	{ uint32_t v = shf(ctx, 'L', (ctx->d[2] & 0xFFFFu), 3, 16); set_w(&ctx->d[2], v); } hle_insn(ctx, 12);  /* 013EE0 aslw #3,%d2 */
	{ uint32_t eaS = 0x00014272u; ctx->a[2] = eaS; } hle_insn(ctx, 12);  /* 013EE2 lea 14272,%a2 */
	{ uint32_t eaS = ctx->a[2] + (uint32_t)(int32_t)(0) + (uint32_t)(int32_t)(int16_t)ctx->d[2]; ctx->a[2] = eaS; } hle_insn(ctx, 12);  /* 013EE8 lea %a2@(0,%d2:w),%a2 */
	{ uint32_t eaS = ctx->a[2]; ctx->a[2] += 4; uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(4); uint32_t v = ld32(eaS); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 013EEC movel %a2@+,%fp@(4) */
	{ uint32_t eaS = ctx->a[2]; ctx->a[2] += 4; ctx->a[2] = ld32(eaS); } hle_insn(ctx, 12);  /* 013EF0 moveal %a2@+,%a2 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(12); uint32_t v = 0xBu; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 013EF2 movew #11,%fp@(12) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(14); uint32_t v = 0x8u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 013EF8 movew #8,%fp@(14) */
	{ uint32_t eaD = 0xFFFF80CCu; alu(ctx, OP_SUB, ld16(eaD), 0x8u, 16, 0); } hle_insn(ctx, 20);  /* 013EFE cmpiw #8,ffff80cc */
	if (COND_GE) { hle_insn(ctx, 10); goto L_013F7A; } hle_insn(ctx, 12);  /* 013F06 bgew 13f7a */
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x4u, 16, 0); } hle_insn(ctx, 8);  /* 013F0A cmpiw #4,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_013F22; } hle_insn(ctx, 8);  /* 013F0E bnes 13f22 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(12); uint32_t v = 0x6u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 013F10 movew #6,%fp@(12) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(14); uint32_t v = 0x9u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 013F16 movew #9,%fp@(14) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = alu(ctx, OP_ADD, ld16(eaD), 0x6u, 16, 1); st16(eaD, v); } hle_insn(ctx, 20);  /* 013F1C addiw #6,%fp@(2) */
L_013F22:
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x5u, 16, 0); } hle_insn(ctx, 8);  /* 013F22 cmpiw #5,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_013F42; } hle_insn(ctx, 8);  /* 013F26 bnes 13f42 */
L_013F28:
	hle_jump(ctx, 0x013F28); return;  /* 013F28 jsr 881c */
L_013F2E:
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0x7u) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 013F2E andiw #7,%d0 */
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x7u, 16, 0); } hle_insn(ctx, 8);  /* 013F32 cmpiw #7,%d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_013F28; } hle_insn(ctx, 8);  /* 013F36 beqs 13f28 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(4); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 013F38 movew %d0,%fp@(4) */
	{ uint32_t eaS = ctx->a[4] + (uint32_t)(int32_t)(36); uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(6); uint32_t v = ld16(eaS); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 20);  /* 013F3C movew %a4@(36),%fp@(6) */
L_013F42:
	{ uint32_t eaS = 0xFFFF80BCu; ctx->a[5] = ld32(eaS); } hle_insn(ctx, 20);  /* 013F42 moveal ffff80bc,%a5 */
	{ uint32_t v = 0xBu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 013F48 movew #11,%d0 */
L_013F4C:
	{ uint32_t eaD = ctx->a[5]; ctx->a[5] += 2; uint32_t v = 0x2u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 013F4C movew #2,%a5@+ */
	{ uint32_t eaD = ctx->a[5]; ctx->a[5] += 2; uint32_t v = 0x1Au; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 013F50 movew #26,%a5@+ */
	{ uint32_t eaD = ctx->a[5]; ctx->a[5] += 2; uint32_t v = (ctx->d[3] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 013F54 movew %d3,%a5@+ */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[3] & 0xFFFFu), 0x80u, 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 8);  /* 013F56 addiw #128,%d3 */
	{ uint16_t v = (uint16_t)ctx->d[0] - 1; set_w(&ctx->d[0], v); if (v != 0xFFFF) { hle_insn(ctx, 10); goto L_013F4C; } hle_insn(ctx, 14); }  /* 013F5A dbf %d0,13f4c */
	{ uint32_t eaD = ctx->a[5]; ctx->a[5] += 2; uint32_t v = 0x1u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 013F5E movew #1,%a5@+ */
	{ uint32_t eaS = ctx->a[2]; ctx->a[2] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 013F62 movew %a2@+,%d0 */
	{ uint32_t v = shf(ctx, 'a', (ctx->d[0] & 0xFFFFu), 1, 16); set_w(&ctx->d[0], v); } hle_insn(ctx, 8);  /* 013F64 asrw #1,%d0 */
	{ uint32_t eaD = ctx->a[5]; ctx->a[5] += 2; uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 013F66 movew %d0,%a5@+ */
	{ uint32_t eaD = ctx->a[5]; ctx->a[5] += 4; uint32_t v = (ctx->a[2] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 12);  /* 013F68 movel %a2,%a5@+ */
	{ uint32_t eaS = 0xFFFF9E42u; uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 013F6A movew ffff9e42,%d0 */
	{ uint32_t v = shf(ctx, 'L', (ctx->d[0] & 0xFFFFu), 5, 16); set_w(&ctx->d[0], v); } hle_insn(ctx, 16);  /* 013F70 aslw #5,%d0 */
	{ uint32_t eaD = ctx->a[5]; ctx->a[5] += 2; uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 013F72 movew %d0,%a5@+ */
	{ uint32_t eaD = 0xFFFF80BCu; uint32_t v = (ctx->a[5] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 20);  /* 013F74 movel %a5,ffff80bc */
L_013F7A:
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 4; ctx->a[6] = ld32(eaS); } hle_insn(ctx, 12);  /* 013F7A moveal %sp@+,%fp */
	hle_jump(ctx, 0x013F7C); return;  /* 013F7C rts */
L_013F7E:
	{ uint32_t eaD = 0xFFFF80B4u; cc_logic8(ctx, ld8(eaD)); } hle_insn(ctx, 16);  /* 013F7E tstb ffff80b4 */
	if (COND_MI) { hle_insn(ctx, 10); goto L_013FC4; } hle_insn(ctx, 8);  /* 013F84 bmis 13fc4 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_013F94; } hle_insn(ctx, 8);  /* 013F86 beqs 13f94 */
	hle_jump(ctx, 0x013F88); return;  /* 013F88 jsr 9290 */
L_013F8E:
	{ uint32_t eaD = 0xFFFF80B4u; st8(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 20);  /* 013F8E clrb ffff80b4 */
L_013F94:
	{ uint32_t eaS = 0xFFFF80B0u; uint32_t v = ld16(eaS); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 013F94 movew ffff80b0,%d1 */
	{ uint32_t eaS = 0xFFFF80B2u; alu(ctx, OP_SUB, (ctx->d[1] & 0xFFFFu), ld16(eaS), 16, 0); } hle_insn(ctx, 16);  /* 013F9A cmpw ffff80b2,%d1 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_014120; } hle_insn(ctx, 12);  /* 013FA0 beqw 14120 */
	{ uint32_t eaS = 0xFFFF80A0u; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 013FA4 lea ffff80a0,%a0 */
	{ ctx->d[0] = 0x00000000u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 013FAA moveq #0,%d0 */
	{ uint32_t eaS = ctx->a[0] + (uint32_t)(int32_t)(0) + (uint32_t)(int32_t)(int16_t)ctx->d[1]; uint32_t v = ld8(eaS); set_b(&ctx->d[0], v); cc_logic8(ctx, v); } hle_insn(ctx, 14);  /* 013FAC moveb %a0@(0,%d1:w),%d0 */
	{ uint32_t eaS = ctx->a[0] + (uint32_t)(int32_t)(1) + (uint32_t)(int32_t)(int16_t)ctx->d[1]; uint32_t v = ld8(eaS); set_b(&ctx->d[2], v); cc_logic8(ctx, v); } hle_insn(ctx, 14);  /* 013FB0 moveb %a0@(1,%d1:w),%d2 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0x2u, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 4);  /* 013FB4 addqw #2,%d1 */
	{ uint32_t v = ((ctx->d[1] & 0xFFFFu) & 0xFu) & 0xFFFFu; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 013FB6 andiw #15,%d1 */
	{ uint32_t eaD = 0xFFFF80B0u; uint32_t v = (ctx->d[1] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 013FBA movew %d1,ffff80b0 */
	hle_insn(ctx, 10); goto L_013E60;  /* 013FC0 braw 13e60 */
L_013FC4:
	{ uint32_t eaS = 0xFFFF80E2u; ctx->a[6] = eaS; } hle_insn(ctx, 12);  /* 013FC4 lea ffff80e2,%fp */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(0); uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 013FCA movew %fp@(0),%d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_014120; } hle_insn(ctx, 12);  /* 013FCE beqw 14120 */
	{ uint32_t eaD = 0xFFFF80CCu; alu(ctx, OP_SUB, ld16(eaD), 0x8u, 16, 0); } hle_insn(ctx, 20);  /* 013FD2 cmpiw #8,ffff80cc */
	if (COND_GE) { hle_insn(ctx, 10); goto L_014242; } hle_insn(ctx, 12);  /* 013FDA bgew 14242 */
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x5u, 16, 0); } hle_insn(ctx, 8);  /* 013FDE cmpiw #5,%d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_014122; } hle_insn(ctx, 12);  /* 013FE2 beqw 14122 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(10); uint32_t v = ld16(eaS); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 013FE6 movew %fp@(10),%d1 */
	{ alu(ctx, OP_SUB, (ctx->d[1] & 0xFFFFu), 0xCu, 16, 0); } hle_insn(ctx, 8);  /* 013FEA cmpiw #12,%d1 */
	if (COND_GE) { hle_insn(ctx, 10); goto L_014054; } hle_insn(ctx, 12);  /* 013FEE bgew 14054 */
	{ alu(ctx, OP_SUB, (ctx->d[1] & 0xFFFFu), 0x2u, 16, 0); } hle_insn(ctx, 8);  /* 013FF2 cmpiw #2,%d1 */
	if (COND_GT) { hle_insn(ctx, 10); goto L_01401A; } hle_insn(ctx, 8);  /* 013FF6 bgts 1401a */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(4); ctx->a[0] = ld32(eaS); } hle_insn(ctx, 16);  /* 013FF8 moveal %fp@(4),%a0 */
	{ uint32_t v = shf(ctx, 'L', (ctx->d[1] & 0xFFFFu), 7, 16); set_w(&ctx->d[1], v); } hle_insn(ctx, 20);  /* 013FFC aslw #7,%d1 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), ld16(eaS), 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 12);  /* 013FFE addw %fp@(2),%d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFFu); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 014002 movew %d1,%d3 */
	{ uint32_t v = 0xC000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 014004 movew #-16384,%d6 */
	hle_jump(ctx, 0x014008); return;  /* 014008 jsr 7e08 */
L_01400E:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(10); uint32_t v = alu(ctx, OP_ADD, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 16);  /* 01400E addqw #1,%fp@(10) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(4); uint32_t v = (ctx->a[0] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 16);  /* 014012 movel %a0,%fp@(4) */
	hle_insn(ctx, 10); goto L_0140A2;  /* 014016 braw 140a2 */
L_01401A:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(4); ctx->a[4] = ld32(eaS); } hle_insn(ctx, 16);  /* 01401A moveal %fp@(4),%a4 */
	{ uint32_t eaS = ctx->a[4]; ctx->a[0] = ld32(eaS); } hle_insn(ctx, 12);  /* 01401E moveal %a4@,%a0 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = ld16(eaS); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 014020 movew %fp@(2),%d1 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0x180u, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 014024 addiw #384,%d1 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(12); uint32_t v = ld16(eaS); set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 014028 movew %fp@(12),%d2 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(10); uint32_t v = ld16(eaS); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 01402C movew %fp@(10),%d3 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[3] & 0xFFFFu), 0x2u, 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 4);  /* 014030 subqw #2,%d3 */
	{ uint32_t v = 0xC000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 014032 movew #-16384,%d6 */
	{ uint32_t eaS = 0xFFFF9E42u; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 014036 movew ffff9e42,%d7 */
	{ uint32_t eaD = ctx->a[6]; alu(ctx, OP_SUB, ld16(eaD), 0xBu, 16, 0); } hle_insn(ctx, 12);  /* 01403C cmpiw #11,%fp@ */
	if (COND_NE) { hle_insn(ctx, 10); goto L_014048; } hle_insn(ctx, 8);  /* 014040 bnes 14048 */
	hle_jump(ctx, 0x014042); return;  /* 014042 jsr %pc@(12234) */
L_014046:
	hle_insn(ctx, 10); goto L_01404C;  /* 014046 bras 1404c */
L_014048:
	hle_jump(ctx, 0x014048); return;  /* 014048 jsr %pc@(122ca) */
L_01404C:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(10); uint32_t v = alu(ctx, OP_ADD, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 16);  /* 01404C addqw #1,%fp@(10) */
	hle_insn(ctx, 10); goto L_0140A2;  /* 014050 braw 140a2 */
L_014054:
	{ uint32_t eaD = ctx->a[6]; alu(ctx, OP_SUB, ld16(eaD), 0x6u, 16, 0); } hle_insn(ctx, 12);  /* 014054 cmpiw #6,%fp@ */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0140A2; } hle_insn(ctx, 12);  /* 014058 beqw 140a2 */
	{ uint32_t eaD = ctx->a[6]; alu(ctx, OP_SUB, ld16(eaD), 0xBu, 16, 0); } hle_insn(ctx, 12);  /* 01405C cmpiw #11,%fp@ */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0141F8; } hle_insn(ctx, 12);  /* 014060 beqw 141f8 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(8); uint32_t v = ld16(eaS); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 014064 movew %fp@(8),%d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFFu); set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 014068 movew %d1,%d2 */
	{ uint32_t v = ((ctx->d[2] & 0xFFFFu) & 0xFu) & 0xFFFFu; set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 01406A andiw #15,%d2 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0140A2; } hle_insn(ctx, 8);  /* 01406E bnes 140a2 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(4); ctx->a[0] = ld32(eaS); } hle_insn(ctx, 16);  /* 014070 moveal %fp@(4),%a0 */
	{ uint32_t eaS = ctx->a[0]; ctx->a[0] = ld32(eaS); } hle_insn(ctx, 12);  /* 014074 moveal %a0@,%a0 */
	{ unsigned b = 4; uint32_t m = 1u << b; cc_set(ctx, CC_Z, (ctx->d[1] & m) ? 0 : CC_Z);  hle_insn(ctx, 10); }  /* 014076 btst #4,%d1 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_014084; } hle_insn(ctx, 8);  /* 01407A beqs 14084 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(4); ctx->a[0] = ld32(eaS); } hle_insn(ctx, 16);  /* 01407C moveal %fp@(4),%a0 */
	{ uint32_t eaS = ctx->a[0] + (uint32_t)(int32_t)(4); ctx->a[0] = ld32(eaS); } hle_insn(ctx, 16);  /* 014080 moveal %a0@(4),%a0 */
L_014084:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = ld16(eaS); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 014084 movew %fp@(2),%d1 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0x180u, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 014088 addiw #384,%d1 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(12); uint32_t v = ld16(eaS); set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 01408C movew %fp@(12),%d2 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(14); uint32_t v = ld16(eaS); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 014090 movew %fp@(14),%d3 */
	{ uint32_t v = 0xC000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 014094 movew #-16384,%d6 */
	{ uint32_t eaS = 0xFFFF9E42u; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 014098 movew ffff9e42,%d7 */
	hle_jump(ctx, 0x01409E); return;  /* 01409E jsr %pc@(122ca) */
L_0140A2:
	{ uint32_t eaS = 0xFFFF80B0u; uint32_t v = ld16(eaS); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 0140A2 movew ffff80b0,%d1 */
	{ uint32_t eaS = 0xFFFF80B2u; uint32_t v = alu(ctx, OP_SUB, (ctx->d[1] & 0xFFFFu), ld16(eaS), 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 16);  /* 0140A8 subw ffff80b2,%d1 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0140B4; } hle_insn(ctx, 8);  /* 0140AE beqs 140b4 */
	{ uint32_t v = 0x6Eu; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0140B0 movew #110,%d1 */
L_0140B4:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(8); uint32_t v = alu(ctx, OP_SUB, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 16);  /* 0140B4 subqw #1,%fp@(8) */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(8); alu(ctx, OP_SUB, (ctx->d[1] & 0xFFFFu), ld16(eaS), 16, 0); } hle_insn(ctx, 12);  /* 0140B8 cmpw %fp@(8),%d1 */
	if (COND_LE) { hle_insn(ctx, 10); goto L_014120; } hle_insn(ctx, 8);  /* 0140BC bles 14120 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(0); alu(ctx, OP_SUB, ld16(eaD), 0x5u, 16, 0); } hle_insn(ctx, 16);  /* 0140BE cmpiw #5,%fp@(0) */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0140CE; } hle_insn(ctx, 8);  /* 0140C4 bnes 140ce */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(4); uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 0140C6 movew %fp@(4),%d0 */
	hle_jump(ctx, 0x0140CA); return;  /* 0140CA jsr %pc@(14510) */
L_0140CE:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(0); st16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 16);  /* 0140CE clrw %fp@(0) */
	{ uint32_t eaD = 0xFFFF80B4u; st8(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 20);  /* 0140D2 clrb ffff80b4 */
	{ uint32_t eaD = 0xFFFF80CCu; alu(ctx, OP_SUB, ld16(eaD), 0x8u, 16, 0); } hle_insn(ctx, 20);  /* 0140D8 cmpiw #8,ffff80cc */
	if (COND_GE) { hle_insn(ctx, 10); goto L_014120; } hle_insn(ctx, 8);  /* 0140E0 bges 14120 */
	{ uint32_t eaS = 0xFFFF80BCu; ctx->a[5] = ld32(eaS); } hle_insn(ctx, 20);  /* 0140E2 moveal ffff80bc,%a5 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = ld16(eaS); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 0140E8 movew %fp@(2),%d1 */
	{ uint32_t v = 0x9u; set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0140EC movew #9,%d2 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[2] & 0xFFFFu), 0x3u, 16, 1); set_w(&ctx->d[2], v); } hle_insn(ctx, 4);  /* 0140F0 addqw #3,%d2 */
	{ uint32_t v = 0xFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0140F2 movew #15,%d0 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), (ctx->d[0] & 0xFFFFu), 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 4);  /* 0140F6 addw %d0,%d0 */
L_0140F8:
	{ uint32_t eaD = ctx->a[5]; ctx->a[5] += 2; uint32_t v = 0x2u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 0140F8 movew #2,%a5@+ */
	{ uint32_t eaD = ctx->a[5]; ctx->a[5] += 2; uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0140FC movew %d0,%a5@+ */
	{ uint32_t eaD = ctx->a[5]; ctx->a[5] += 2; uint32_t v = (ctx->d[1] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0140FE movew %d1,%a5@+ */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0x80u, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 014100 addiw #128,%d1 */
	{ uint16_t v = (uint16_t)ctx->d[2] - 1; set_w(&ctx->d[2], v); if (v != 0xFFFF) { hle_insn(ctx, 10); goto L_0140F8; } hle_insn(ctx, 14); }  /* 014104 dbf %d2,140f8 */
	{ uint32_t eaD = 0xFFFF80BCu; uint32_t v = (ctx->a[5] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 20);  /* 014108 movel %a5,ffff80bc */
	{ uint32_t eaD = 0xFFFF80CCu; alu(ctx, OP_SUB, ld16(eaD), 0x5u, 16, 0); } hle_insn(ctx, 20);  /* 01410E cmpiw #5,ffff80cc */
	if (COND_NE) { hle_insn(ctx, 10); goto L_014120; } hle_insn(ctx, 8);  /* 014116 bnes 14120 */
	{ uint32_t eaD = 0xFFFF80B4u; uint32_t v = 0x1u; st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 20);  /* 014118 moveb #1,ffff80b4 */
L_014120:
	hle_jump(ctx, 0x014120); return;  /* 014120 rts */
L_014122:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(10); uint32_t v = ld16(eaS); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 014122 movew %fp@(10),%d1 */
	{ alu(ctx, OP_SUB, (ctx->d[1] & 0xFFFFu), 0x8u, 16, 0); } hle_insn(ctx, 8);  /* 014126 cmpiw #8,%d1 */
	if (COND_GE) { hle_insn(ctx, 10); goto L_014142; } hle_insn(ctx, 12);  /* 01412A bgew 14142 */
	{ uint32_t eaD = 0xFFFF9EA2u; st16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 20);  /* 01412E clrw ffff9ea2 */
	{ uint32_t v = (ctx->d[1] & 0xFFFFu); set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 014134 movew %d1,%d2 */
	hle_jump(ctx, 0x014136); return;  /* 014136 jsr %pc@(141b8) */
L_01413A:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(10); uint32_t v = alu(ctx, OP_ADD, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 16);  /* 01413A addqw #1,%fp@(10) */
	hle_insn(ctx, 10); goto L_0140A2;  /* 01413E braw 140a2 */
L_014142:
	{ alu(ctx, OP_SUB, (ctx->d[1] & 0xFFFFu), 0x18u, 16, 0); } hle_insn(ctx, 8);  /* 014142 cmpiw #24,%d1 */
	if (COND_GE) { hle_insn(ctx, 10); goto L_01416A; } hle_insn(ctx, 12);  /* 014146 bgew 1416a */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[1] & 0xFFFFu), 0x8u, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 01414A subiw #8,%d1 */
	{ uint32_t v = shf(ctx, 'a', (ctx->d[1] & 0xFFFFu), 1, 16); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 01414E asrw #1,%d1 */
	hle_jump(ctx, 0x014150); return;  /* 014150 jsr %pc@(141e6) */
L_014154:
	{ uint32_t eaD = 0xFFFF9EA2u; uint32_t v = (ctx->d[1] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 014154 movew %d1,ffff9ea2 */
	{ ctx->d[2] = 0xFFFFFFFFu; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 01415A moveq #-1,%d2 */
	hle_jump(ctx, 0x01415C); return;  /* 01415C jsr 141b8 */
L_014162:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(10); uint32_t v = alu(ctx, OP_ADD, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 16);  /* 014162 addqw #1,%fp@(10) */
	hle_insn(ctx, 10); goto L_0140A2;  /* 014166 braw 140a2 */
L_01416A:
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[1] & 0xFFFFu), 0x18u, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 01416A subiw #24,%d1 */
	{ uint32_t v = shf(ctx, 'a', (ctx->d[1] & 0xFFFFu), 1, 16); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 01416E asrw #1,%d1 */
	{ uint32_t v = ((ctx->d[1] & 0xFFFFu) ^ 0x7u) & 0xFFFFu; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 014170 eoriw #7,%d1 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(4); alu(ctx, OP_SUB, (ctx->d[1] & 0xFFFFu), ld16(eaS), 16, 0); } hle_insn(ctx, 12);  /* 014174 cmpw %fp@(4),%d1 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_01419C; } hle_insn(ctx, 8);  /* 014178 bnes 1419c */
	hle_jump(ctx, 0x01417A); return;  /* 01417A jsr %pc@(141e6) */
L_01417E:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(8); uint32_t v = ld16(eaS); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 01417E movew %fp@(8),%d3 */
	{ uint32_t v = (ctx->d[3] & 0xFFFFu); set_w(&ctx->d[4], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 014182 movew %d3,%d4 */
	{ uint32_t v = ((ctx->d[3] & 0xFFFFu) & 0xFu) & 0xFFFFu; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 014184 andiw #15,%d3 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_014198; } hle_insn(ctx, 8);  /* 014188 bnes 14198 */
	{ uint32_t v = (ctx->d[1] & 0xFFFFu); set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 01418A movew %d1,%d2 */
	{ unsigned b = 4; uint32_t m = 1u << b; cc_set(ctx, CC_Z, (ctx->d[4] & m) ? 0 : CC_Z);  hle_insn(ctx, 10); }  /* 01418C btst #4,%d4 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_014194; } hle_insn(ctx, 8);  /* 014190 bnes 14194 */
	{ ctx->d[2] = 0xFFFFFFFFu; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 014192 moveq #-1,%d2 */
L_014194:
	hle_jump(ctx, 0x014194); return;  /* 014194 jsr %pc@(141b8) */
L_014198:
	hle_insn(ctx, 10); goto L_0140A2;  /* 014198 braw 140a2 */
L_01419C:
	hle_jump(ctx, 0x01419C); return;  /* 01419C jsr %pc@(141e6) */
L_0141A0:
	{ uint32_t eaD = 0xFFFF9EA2u; uint32_t v = (ctx->d[1] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 0141A0 movew %d1,ffff9ea2 */
	{ ctx->d[2] = 0xFFFFFFFFu; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 0141A6 moveq #-1,%d2 */
	hle_jump(ctx, 0x0141A8); return;  /* 0141A8 jsr 141b8 */
L_0141AE:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(10); uint32_t v = alu(ctx, OP_ADD, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 16);  /* 0141AE addqw #1,%fp@(10) */
	{ ctx->d[1] = 0x00000000u; cc_logic32(ctx, ctx->d[1]); } hle_insn(ctx, 4);  /* 0141B2 moveq #0,%d1 */
	hle_insn(ctx, 10); goto L_0140B4;  /* 0141B4 braw 140b4 */
L_0141B8:
	{ uint32_t eaS = 0x00014400u; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 0141B8 lea %pc@(14400),%a0 */
	{ uint32_t eaD = 0xFFFF80CCu; alu(ctx, OP_SUB, ld16(eaD), 0x5u, 16, 0); } hle_insn(ctx, 20);  /* 0141BC cmpiw #5,ffff80cc */
	if (COND_LT) { hle_insn(ctx, 10); goto L_0141CA; } hle_insn(ctx, 8);  /* 0141C4 blts 141ca */
	{ uint32_t eaS = 0x00014490u; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 0141C6 lea %pc@(14490),%a0 */
L_0141CA:
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0x1u, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 4);  /* 0141CA addqw #1,%d1 */
	{ uint32_t v = shf(ctx, 'L', (ctx->d[2] & 0xFFFFu), 4, 16); set_w(&ctx->d[2], v); } hle_insn(ctx, 14);  /* 0141CC aslw #4,%d2 */
	{ uint32_t eaS = ctx->a[0] + (uint32_t)(int32_t)(0) + (uint32_t)(int32_t)(int16_t)ctx->d[2]; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 0141CE lea %a0@(0,%d2:w),%a0 */
	{ uint32_t v = shf(ctx, 'L', (ctx->d[1] & 0xFFFFu), 7, 16); set_w(&ctx->d[1], v); } hle_insn(ctx, 20);  /* 0141D2 aslw #7,%d1 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), ld16(eaS), 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 12);  /* 0141D4 addw %fp@(2),%d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFFu); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 0141D8 movew %d1,%d3 */
	{ uint32_t v = 0xC000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0141DA movew #-16384,%d6 */
	hle_jump(ctx, 0x0141DE); return;  /* 0141DE jsr 7e08 */
L_0141E4:
	hle_jump(ctx, 0x0141E4); return;  /* 0141E4 rts */
L_0141E6:
	{ ctx->a[7] -= 2; uint32_t eaD = ctx->a[7]; uint32_t v = (ctx->d[1] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0141E6 movew %d1,%sp@- */
	{ uint32_t eaS = 0xFFFF9EA2u; uint32_t v = ld16(eaS); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 0141E8 movew ffff9ea2,%d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFFu); set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 0141EE movew %d1,%d2 */
	hle_jump(ctx, 0x0141F0); return;  /* 0141F0 jsr %pc@(141b8) */
L_0141F4:
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 2; uint32_t v = ld16(eaS); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0141F4 movew %sp@+,%d1 */
	hle_jump(ctx, 0x0141F6); return;  /* 0141F6 rts */
L_0141F8:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(8); uint32_t v = ld16(eaS); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 0141F8 movew %fp@(8),%d1 */
	{ alu(ctx, OP_SUB, (ctx->d[1] & 0xFFFFu), 0x8Cu, 16, 0); } hle_insn(ctx, 8);  /* 0141FC cmpiw #140,%d1 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_01420C; } hle_insn(ctx, 8);  /* 014200 bnes 1420c */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(4); ctx->a[0] = ld32(eaS); } hle_insn(ctx, 16);  /* 014202 moveal %fp@(4),%a0 */
	{ uint32_t eaS = ctx->a[0] + (uint32_t)(int32_t)(4); ctx->a[0] = ld32(eaS); } hle_insn(ctx, 16);  /* 014206 moveal %a0@(4),%a0 */
	hle_insn(ctx, 10); goto L_014220;  /* 01420A bras 14220 */
L_01420C:
	{ alu(ctx, OP_SUB, (ctx->d[1] & 0xFFFFu), 0x50u, 16, 0); } hle_insn(ctx, 8);  /* 01420C cmpiw #80,%d1 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_01421C; } hle_insn(ctx, 8);  /* 014210 bnes 1421c */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(4); ctx->a[0] = ld32(eaS); } hle_insn(ctx, 16);  /* 014212 moveal %fp@(4),%a0 */
	{ uint32_t eaS = ctx->a[0] + (uint32_t)(int32_t)(8); ctx->a[0] = ld32(eaS); } hle_insn(ctx, 16);  /* 014216 moveal %a0@(8),%a0 */
	hle_insn(ctx, 10); goto L_014220;  /* 01421A bras 14220 */
L_01421C:
	hle_insn(ctx, 10); goto L_0140A2;  /* 01421C braw 140a2 */
L_014220:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = ld16(eaS); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 014220 movew %fp@(2),%d1 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0x180u, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 014224 addiw #384,%d1 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(12); uint32_t v = ld16(eaS); set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 014228 movew %fp@(12),%d2 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(14); uint32_t v = ld16(eaS); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 01422C movew %fp@(14),%d3 */
	{ uint32_t v = 0xC000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 014230 movew #-16384,%d6 */
	{ uint32_t eaS = 0xFFFF9E42u; uint32_t v = ld16(eaS); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 014234 movew ffff9e42,%d7 */
	hle_jump(ctx, 0x01423A); return;  /* 01423A jsr %pc@(12234) */
L_01423E:
	hle_insn(ctx, 10); goto L_0140A2;  /* 01423E braw 140a2 */
L_014242:
	{ uint32_t v = 0x2A7u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 014242 movew #679,%d1 */
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0xBu, 16, 0); } hle_insn(ctx, 8);  /* 014246 cmpiw #11,%d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_014250; } hle_insn(ctx, 8);  /* 01424A beqs 14250 */
	{ uint32_t v = 0x2A6u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 01424C movew #678,%d1 */
L_014250:
	{ uint32_t v = 0x32u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 014250 movew #50,%d3 */
	{ uint32_t v = 0x24u; set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 014254 movew #36,%d2 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(2); uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 014258 movew %fp@(2),%d0 */
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0xE188u, 16, 0); } hle_insn(ctx, 8);  /* 01425C cmpiw #-7800,%d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_014266; } hle_insn(ctx, 8);  /* 014260 beqs 14266 */
	{ uint32_t v = 0xC4u; set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 014262 movew #196,%d2 */
L_014266:
	{ ctx->d[5] = 0x00000001u; cc_logic32(ctx, ctx->d[5]); } hle_insn(ctx, 4);  /* 014266 moveq #1,%d5 */
	hle_jump(ctx, 0x014268); return;  /* 014268 jsr 13240 */
L_01426E:
	hle_insn(ctx, 10); goto L_0140A2;  /* 01426E braw 140a2 */
}

HLE_RANGE_PURE(0x013E30, EventPush, 200, 0x013E5E) { pc_run(ctx, 0x013E30); }
HLE_RANGE_PURE(0x013F2E, EventRing_013F2E, 200, 0x013F28, HLE_EXIT_SP(0x013F7C, 4)) { pc_run(ctx, 0x013F2E); }
HLE_RANGE_PURE(0x013F7E, EventRing, 200, HLE_EXIT_SP(0x013F28, -4), 0x013F7C, 0x013F88, 0x014008, 0x014042, 0x014048, 0x01409E, 0x0140CA, 0x014120, 0x014136, 0x014150, 0x01417A, 0x01419C, 0x01423A, 0x014268) { pc_run(ctx, 0x013F7E); }
HLE_RANGE_PURE(0x013F8E, EventRing_013F8E, 200, HLE_EXIT_SP(0x013F28, -4), 0x013F7C, 0x014120) { pc_run(ctx, 0x013F8E); }
HLE_RANGE_PURE(0x01400E, EventRing_01400E, 200, 0x0140CA, 0x014120) { pc_run(ctx, 0x01400E); }
HLE_RANGE_PURE(0x014046, EventRing_014046, 200, 0x0140CA, 0x014120) { pc_run(ctx, 0x014046); }
HLE_RANGE_PURE(0x01404C, EventRing_01404C, 200, 0x0140CA, 0x014120) { pc_run(ctx, 0x01404C); }
HLE_RANGE_PURE(0x0140A2, EventRing_0140A2, 200, 0x0140CA, 0x014120) { pc_run(ctx, 0x0140A2); }
HLE_RANGE_PURE(0x0140CE, EventRing_0140CE, 200, 0x014120) { pc_run(ctx, 0x0140CE); }
HLE_RANGE_PURE(0x01413A, EventRing_01413A, 200, 0x0140CA, 0x014120) { pc_run(ctx, 0x01413A); }
HLE_RANGE_PURE(0x014154, EventRing_014154, 200, 0x01415C) { pc_run(ctx, 0x014154); }
HLE_RANGE_PURE(0x014162, EventRing_014162, 200, 0x0140CA, 0x014120) { pc_run(ctx, 0x014162); }
HLE_RANGE_PURE(0x01417E, EventRing_01417E, 200, 0x0140CA, 0x014120, 0x014194) { pc_run(ctx, 0x01417E); }
HLE_RANGE_PURE(0x014198, EventRing_014198, 200, 0x0140CA, 0x014120) { pc_run(ctx, 0x014198); }
HLE_RANGE_PURE(0x0141A0, EventRing_0141A0, 200, 0x0141A8) { pc_run(ctx, 0x0141A0); }
HLE_RANGE_PURE(0x0141AE, EventRing_0141AE, 200, 0x0140CA, 0x014120) { pc_run(ctx, 0x0141AE); }
HLE_RANGE_PURE(0x0141B8, EventText, 200, 0x0141DE) { pc_run(ctx, 0x0141B8); }
HLE_RANGE_PURE(0x0141E6, EventText2, 200, HLE_EXIT_SP(0x0141F0, -2)) { pc_run(ctx, 0x0141E6); }
HLE_RANGE_PURE(0x0141F4, EventRing_0141F4, 200, HLE_EXIT_SP(0x0141F6, 2)) { pc_run(ctx, 0x0141F4); }
HLE_RANGE_PURE(0x01423E, EventRing_01423E, 200, 0x0140CA, 0x014120) { pc_run(ctx, 0x01423E); }
HLE_RANGE_PURE(0x01426E, EventRing_01426E, 200, 0x0140CA, 0x014120) { pc_run(ctx, 0x01426E); }
