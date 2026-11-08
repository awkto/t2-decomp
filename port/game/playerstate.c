/* The per-player state handler $008C8E (PlayerState) and its caller $008C52 (PlayersState): timers,
 * the state-1/5/7 sounds and bars, the state-2 death / continue paths up to the PlayerDied label
 * ($008D68, which game/screens.c already covers).
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
	case 0x008C52: goto L_008C52;
	case 0x008C5C: goto L_008C5C;
	case 0x008C8E: goto L_008C8E;
	case 0x008CA4: goto L_008CA4;
	case 0x008CB2: goto L_008CB2;
	case 0x008CD0: goto L_008CD0;
	case 0x008CD6: goto L_008CD6;
	case 0x008D0A: goto L_008D0A;
	case 0x008D56: goto L_008D56;
	case 0x008D5E: goto L_008D5E;
	default: __builtin_trap();
	}
L_008C52:
	{ uint32_t eaS = 0xFFFF8000u; ctx->a[4] = eaS; } hle_insn(ctx, 12);  /* 008C52 lea ffff8000,%a4 */
	hle_jump(ctx, 0x008C58); return;  /* 008C58 bsrw 8c8e */
L_008C5C:
	{ uint32_t eaS = 0xFFFF8050u; ctx->a[4] = eaS; } hle_insn(ctx, 12);  /* 008C5C lea ffff8050,%a4 */
	hle_jump(ctx, 0x008C62); return;  /* 008C62 bsrw 8c8e */
L_008C66:
	hle_jump(ctx, 0x008C66); return;  /* 008C66 rts */
L_008C8E:
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(34); alu(ctx, OP_SUB, ld16(eaD), 0x1u, 16, 0); } hle_insn(ctx, 16);  /* 008C8E cmpiw #1,%a4@(34) */
	if (COND_NE) { hle_insn(ctx, 10); goto L_008CB2; } hle_insn(ctx, 8);  /* 008C94 bnes 8cb2 */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(32); alu(ctx, OP_SUB, ld16(eaD), 0x78u, 16, 0); } hle_insn(ctx, 16);  /* 008C96 cmpiw #120,%a4@(32) */
	if (COND_NE) { hle_insn(ctx, 10); goto L_008CA4; } hle_insn(ctx, 8);  /* 008C9C bnes 8ca4 */
	hle_jump(ctx, 0x008C9E); return;  /* 008C9E jsr 13e02 */
L_008CA4:
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(32); alu(ctx, OP_SUB, ld16(eaD), 0x32u, 16, 0); } hle_insn(ctx, 16);  /* 008CA4 cmpiw #50,%a4@(32) */
	if (COND_NE) { hle_insn(ctx, 10); goto L_008CB2; } hle_insn(ctx, 8);  /* 008CAA bnes 8cb2 */
	hle_jump(ctx, 0x008CAC); return;  /* 008CAC jsr 13d90 */
L_008CB2:
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(34); alu(ctx, OP_SUB, ld16(eaD), 0x5u, 16, 0); } hle_insn(ctx, 16);  /* 008CB2 cmpiw #5,%a4@(34) */
	if (COND_NE) { hle_insn(ctx, 10); goto L_008CD6; } hle_insn(ctx, 8);  /* 008CB8 bnes 8cd6 */
	{ uint32_t eaS = ctx->a[4] + (uint32_t)(int32_t)(32); uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 008CBA movew %a4@(32),%d0 */
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 008CBE movew %d0,%d1 */
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0xFu) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 008CC0 andiw #15,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_008CD6; } hle_insn(ctx, 8);  /* 008CC4 bnes 8cd6 */
	{ uint32_t v = ((ctx->d[1] & 0xFFFFu) & 0x10u) & 0xFFFFu; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 008CC6 andiw #16,%d1 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_008CD2; } hle_insn(ctx, 8);  /* 008CCA beqs 8cd2 */
	hle_jump(ctx, 0x008CCC); return;  /* 008CCC jsr %pc@(7b50) */
L_008CD0:
	hle_insn(ctx, 10); goto L_008CD6;  /* 008CD0 bras 8cd6 */
L_008CD2:
	hle_jump(ctx, 0x008CD2); return;  /* 008CD2 bsrw 7bb8 */
L_008CD6:
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(34); alu(ctx, OP_SUB, ld16(eaD), 0x7u, 16, 0); } hle_insn(ctx, 16);  /* 008CD6 cmpiw #7,%a4@(34) */
	if (COND_NE) { hle_insn(ctx, 10); goto L_008D22; } hle_insn(ctx, 8);  /* 008CDC bnes 8d22 */
	{ uint32_t eaS = ctx->a[4] + (uint32_t)(int32_t)(32); uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 008CDE movew %a4@(32),%d0 */
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x1u, 16, 0); } hle_insn(ctx, 8);  /* 008CE2 cmpiw #1,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_008CEC; } hle_insn(ctx, 8);  /* 008CE6 bnes 8cec */
	hle_insn(ctx, 10); hle_jump(ctx, 0x008E68); return;  /* 008CE8 braw 8e68 */
L_008CEC:
	{ uint32_t eaS = ctx->a[4] + (uint32_t)(int32_t)(78); uint32_t v = ld16(eaS); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 008CEC movew %a4@(78),%d3 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[3] & 0xFFFFu), 0x1Au, 16, 1); set_w(&ctx->d[3], v); } hle_insn(ctx, 8);  /* 008CF0 addiw #26,%d3 */
	{ uint32_t v = shf(ctx, 'a', (ctx->d[0] & 0xFFFFu), 3, 16); set_w(&ctx->d[0], v); } hle_insn(ctx, 12);  /* 008CF4 asrw #3,%d0 */
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0xFFF8u) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 008CF6 andiw #-8,%d0 */
	{ uint32_t eaS = 0x00008EDAu; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 008CFA lea %pc@(8eda),%a0 */
	{ uint32_t eaS = ctx->a[0] + (uint32_t)(int32_t)(0) + (uint32_t)(int32_t)(int16_t)ctx->d[0]; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 008CFE lea %a0@(0,%d0:w),%a0 */
	{ uint32_t v = 0xC000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 008D02 movew #-16384,%d6 */
	hle_jump(ctx, 0x008D06); return;  /* 008D06 jsr %pc@(7e08) */
L_008D0A:
	{ uint32_t eaS = ctx->a[4] + (uint32_t)(int32_t)(0); uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 008D0A movew %a4@(0),%d0 */
	{ unsigned b = 7; uint32_t m = 1u << b; cc_set(ctx, CC_Z, (ctx->d[0] & m) ? 0 : CC_Z);  hle_insn(ctx, 10); }  /* 008D0E btst #7,%d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_008D22; } hle_insn(ctx, 8);  /* 008D12 beqs 8d22 */
	{ uint32_t eaD = 0xFFFF80CAu; cc_logic8(ctx, ld8(eaD)); } hle_insn(ctx, 16);  /* 008D14 tstb ffff80ca */
	if (COND_GT) { hle_insn(ctx, 10); hle_jump(ctx, 0x008DA2); return; } hle_insn(ctx, 12);  /* 008D1A bgtw 8da2 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x008E68); return;  /* 008D1E braw 8e68 */
L_008D22:
	{ uint32_t eaS = ctx->a[4] + (uint32_t)(int32_t)(32); uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 008D22 movew %a4@(32),%d0 */
	if (COND_EQ) { hle_insn(ctx, 10); hle_jump(ctx, 0x008DFC); return; } hle_insn(ctx, 12);  /* 008D26 beqw 8dfc */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x1u, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 4);  /* 008D2A subqw #1,%d0 */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(32); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 008D2C movew %d0,%a4@(32) */
	if (COND_NE) { hle_insn(ctx, 10); hle_jump(ctx, 0x008DFC); return; } hle_insn(ctx, 12);  /* 008D30 bnew 8dfc */
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(34); alu(ctx, OP_SUB, ld16(eaD), 0x2u, 16, 0); } hle_insn(ctx, 16);  /* 008D34 cmpiw #2,%a4@(34) */
	if (COND_NE) { hle_insn(ctx, 10); hle_jump(ctx, 0x008DC0); return; } hle_insn(ctx, 12);  /* 008D3A bnew 8dc0 */
	{ uint32_t eaD = 0xFFFF9F6Du; alu(ctx, OP_SUB, ld8(eaD), 0x1u, 8, 0); } hle_insn(ctx, 20);  /* 008D3E cmpib #1,ffff9f6d */
	if (COND_NE) { hle_insn(ctx, 10); hle_jump(ctx, 0x008D68); return; } hle_insn(ctx, 8);  /* 008D46 bnes 8d68 */
	{ uint32_t eaS = 0xFFFFA0EEu; uint32_t eaD = 0xFFFFA238u; uint32_t v = ld16(eaS); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 28);  /* 008D48 movew ffffa0ee,ffffa238 */
	hle_jump(ctx, 0x008D52); return;  /* 008D52 bsrw 8f2e */
L_008D56:
	{ uint32_t v = 0x78u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 008D56 movew #120,%d1 */
	hle_jump(ctx, 0x008D5A); return;  /* 008D5A jsr %pc@(86d8) */
L_008D5E:
	{ uint32_t eaS = 0xFFFFA238u; uint32_t eaD = 0xFFFFA0EEu; uint32_t v = ld16(eaS); st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 28); hle_jump(ctx, 0x008D68); return;  /* 008D5E movew ffffa238,ffffa0ee */
}

HLE_RANGE_PURE(0x008C52, PlayersState, 200, 0x008C58) { pc_run(ctx, 0x008C52); }
HLE_RANGE_PURE(0x008C5C, PlayerState_008C5C, 200, 0x008C62) { pc_run(ctx, 0x008C5C); }
HLE_RANGE_PURE(0x008C8E, PlayerState, 200, 0x008C9E, 0x008CAC, 0x008CCC, 0x008CD2, 0x008D06, 0x008D52, 0x008D68, 0x008DC0, 0x008DFC, 0x008E68) { pc_run(ctx, 0x008C8E); }
HLE_RANGE_PURE(0x008CA4, PlayerState_008CA4, 200, 0x008CAC, 0x008CCC, 0x008CD2, 0x008D06, 0x008D52, 0x008D68, 0x008DC0, 0x008DFC, 0x008E68) { pc_run(ctx, 0x008CA4); }
HLE_RANGE_PURE(0x008CB2, PlayerState_008CB2, 200, 0x008CCC, 0x008CD2, 0x008D06, 0x008D52, 0x008D68, 0x008DC0, 0x008DFC, 0x008E68) { pc_run(ctx, 0x008CB2); }
HLE_RANGE_PURE(0x008CD0, PlayerState_008CD0, 200, 0x008D06, 0x008D52, 0x008D68, 0x008DC0, 0x008DFC, 0x008E68) { pc_run(ctx, 0x008CD0); }
HLE_RANGE_PURE(0x008CD6, PlayerState_008CD6, 200, 0x008D06, 0x008D52, 0x008D68, 0x008DC0, 0x008DFC, 0x008E68) { pc_run(ctx, 0x008CD6); }
HLE_RANGE_PURE(0x008D0A, PlayerState_008D0A, 200, 0x008D52, 0x008D68, 0x008DA2, 0x008DC0, 0x008DFC, 0x008E68) { pc_run(ctx, 0x008D0A); }
HLE_RANGE_PURE(0x008D56, PlayerState_008D56, 200, 0x008D5A) { pc_run(ctx, 0x008D56); }
HLE_RANGE_PURE(0x008D5E, PlayerState_008D5E, 200, 0x008D68) { pc_run(ctx, 0x008D5E); }
