/* Level-script interpreter: ScriptStart ($009326), ScriptNext ($00933E), ScriptTick ($009358),
 * ScriptExec ($009376). docs/subsystems/levels.md, "The script interpreter".
 *
 *   ScriptStart: d0 = level; a0 = LevelScripts[level]; script_clock = 0; falls into ScriptNext
 *   ScriptNext:  d0 = (a0)+ (delay); 0 -> ScriptExec; else script_due = d0 + script_clock,
 *                script_ptr = a0, rts
 *   ScriptTick:  script_clock++; if == script_due: a0 = script_ptr, ScriptExec; else rts
 *   ScriptExec:  d0 = (a0)+ (opcode); a1 = ScriptOps[op]; jsr (a1); bra ScriptNext
 *
 * Only the interpreter is C. The opcode handlers (ScriptOps $00938A) stay 68k, run by the original
 * `jsr (a1)` (see the range note below); a handler may change a0, the script pointer (goto/gosub),
 * spawn objects, queue VDP commands, call ScriptNext itself, ... and the 68k code after it simply
 * re-enters the C ScriptNext, so nothing is cached across the call.
 *
 * Registers/flags: d0.w = last word read/computed, a0 advances, a1 = handler, and the final CCR is
 * ScriptNext's (X from `add.w`, N/Z from `move.l a0,...`) or, for ScriptTick's not-due exit, the
 * `cmp.w`. RAM: script_ptr $FF80CE, script_clock $FF80D2, script_due $FF80D4, level $FF80CC. */
#include "hle.h"

#include <stdint.h>

#define SCRIPT_PTR    0xFF80CEu
#define SCRIPT_CLOCK  0xFF80D2u
#define SCRIPT_DUE    0xFF80D4u
#define LEVEL         0xFF80CCu
#define LEVEL_SCRIPTS 0x00B36Cu
#define SCRIPT_OPS    0x00938Au

/* 68000 costs */
#define C_MOVEW_PI_D   8    /* move.w (a0)+,d0 */
#define C_BEQ_W_TAKEN  10
#define C_BEQ_W_NOT    12
#define C_ADDW_AL_D    16   /* add.w (abs).l,d0 */
#define C_MOVEW_AL_D   16   /* move.w (abs).l,d0 */
#define C_MOVEW_D_AL   16   /* move.w d0,(abs).l */
#define C_MOVEL_A_AL   20   /* move.l a0,(abs).l */
#define C_MOVEAL_AL    20   /* movea.l (abs).l,a0 */
#define C_ADDQ_W        4
#define C_CMPW_AL_D    16
#define C_ASL_W2       10
#define C_LEA_AL       12
#define C_MOVEAL_IX    18   /* movea.l (0,An,Dn.w),An */
#define C_JSR_AN       16
#define C_BRA_W        10
#define C_RTS          16
#define C_CLR_W_AL     20

/* script data lives in ROM; a handler could in principle point a0 into RAM */
static inline uint16_t rd16(uint32_t a)
{
	return (a & 0xFF0000u) == 0xFF0000u ? RAM16(a) : ROM16(a);
}
static inline uint32_t rd32(uint32_t a) { return ((uint32_t)rd16(a) << 16) | rd16(a + 2); }

/* asl.w #2,d0 */
static void asl_w2(hle_ctx *ctx)
{
	uint32_t v = ctx->d[0] & 0xFFFF, r = (v << 2) & 0xFFFF;
	unsigned top = (v >> 13) & 7;   /* bits 15..13: V when they are not all equal */
	unsigned f = 0;
	if (v & 0x4000) f |= CC_C | CC_X;
	if (r & 0x8000) f |= CC_N;
	if (!r) f |= CC_Z;
	if (top != 0 && top != 7) f |= CC_V;
	cc_set(ctx, CC_X | CC_N | CC_Z | CC_V | CC_C, f);
	set_w(&ctx->d[0], r);
}

/* The interpreter is registered as HLE_RANGEs, not functions: the opcode handler is called
 * by the 68k `jsr (a1)` at $009384, never from C. Handlers can block (a fade or a native routine
 * that waits for VBlank runs for dozens of frames, 3.9M cycles on the captures), and C cannot wait
 * for an interrupt, so each range stops right before that `jsr`, the real 68k executes it, and the
 * `bra.w ScriptNext` after it ($009386) lands on the C ScriptNext again. A handler's own
 * `jsr ScriptNext` reaches the C ScriptNext the same way. The not-due / delayed exits jump to the
 * 68k `rts` at $009356 (free). So every range does only RAM/ROM work: HLE_RANGE_PURE.
 *
 * Costs are on the instruction timeline; the `jsr` and `rts` are real 68k instructions. */
#define EXIT_RTS 0x009356u
#define EXIT_JSR 0x009384u

/* ScriptNext part, then (delay 0) the ScriptExec part; ends with hle_jump */
static void script_next(hle_ctx *ctx)
{
	/* ScriptNext: move.w (a0)+,d0 ; beq.w ScriptExec */
	uint16_t w = rd16(ctx->a[0]);
	ctx->a[0] += 2;
	set_w(&ctx->d[0], w);
	cc_logic16(ctx, w);
	hle_insn(ctx, C_MOVEW_PI_D);
	if (w != 0) {
		hle_insn(ctx, C_BEQ_W_NOT);
		/* add.w (script_clock).l,d0 */
		uint32_t clk = RAM16(SCRIPT_CLOCK), s = w + clk;
		uint16_t r = (uint16_t)s;
		unsigned f = 0;
		if (s > 0xFFFF) f |= CC_C | CC_X;
		if (r & 0x8000) f |= CC_N;
		if (!r) f |= CC_Z;
		if (~(w ^ clk) & (w ^ r) & 0x8000) f |= CC_V;
		cc_set(ctx, CC_X | CC_N | CC_Z | CC_V | CC_C, f);
		set_w(&ctx->d[0], r);
		hle_insn(ctx, C_ADDW_AL_D);
		RAM16(SCRIPT_DUE) = r;                 /* move.w d0,(script_due).l */
		cc_logic16(ctx, r);
		hle_insn(ctx, C_MOVEW_D_AL);
		RAM32_W(SCRIPT_PTR, ctx->a[0]);        /* move.l a0,(script_ptr).l */
		cc_logic32(ctx, ctx->a[0]);
		hle_insn(ctx, C_MOVEL_A_AL);
		hle_jump(ctx, EXIT_RTS);
		return;
	}
	hle_insn(ctx, C_BEQ_W_TAKEN);
	/* fall into ScriptExec */
}

/* ScriptExec up to the jsr (a1) */
static void script_exec(hle_ctx *ctx)
{
	uint16_t op = rd16(ctx->a[0]);
	ctx->a[0] += 2;
	set_w(&ctx->d[0], op);
	cc_logic16(ctx, op);
	hle_insn(ctx, C_MOVEW_PI_D);
	asl_w2(ctx);
	hle_insn(ctx, C_ASL_W2);
	ctx->a[1] = SCRIPT_OPS;                        /* lea.l (ScriptOps).l,a1 */
	hle_insn(ctx, C_LEA_AL);
	ctx->a[1] = rd32(SCRIPT_OPS + (uint32_t)(int32_t)(int16_t)(ctx->d[0] & 0xFFFF));
	hle_insn(ctx, C_MOVEAL_IX);
	hle_jump(ctx, EXIT_JSR);
}

static void script_next_exec(hle_ctx *ctx)
{
	script_next(ctx);
	if (!ctx->jumped)
		script_exec(ctx);
}

HLE_RANGE_PURE(0x009358, ScriptTick, 120, EXIT_RTS, EXIT_JSR)
{
	uint32_t clk = RAM16(SCRIPT_CLOCK);
	uint16_t d0 = (uint16_t)(clk + 1);
	hle_insn(ctx, C_MOVEW_AL_D);
	/* addq.w #1,d0: its flags are overwritten by the cmp below, only X survives */
	unsigned x = (clk + 1 > 0xFFFF) ? CC_X : 0;
	set_w(&ctx->d[0], d0);
	hle_insn(ctx, C_ADDQ_W);
	RAM16(SCRIPT_CLOCK) = d0;
	hle_insn(ctx, C_MOVEW_D_AL);
	/* cmp.w (script_due).l,d0: d0 - due; X unchanged by cmp */
	uint16_t due = RAM16(SCRIPT_DUE), r = (uint16_t)(d0 - due);
	unsigned f = 0;
	if (d0 < due) f |= CC_C;
	if (r & 0x8000) f |= CC_N;
	if (!r) f |= CC_Z;
	if ((d0 ^ due) & (d0 ^ r) & 0x8000) f |= CC_V;
	cc_set(ctx, CC_X, x);
	cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, f);
	hle_insn(ctx, C_CMPW_AL_D);
	if (r != 0) {
		hle_insn(ctx, C_BEQ_W_TAKEN);               /* bne.w taken (same cost as beq.w) */
		hle_jump(ctx, EXIT_RTS);
		return;
	}
	hle_insn(ctx, C_BEQ_W_NOT);
	ctx->a[0] = RAM32(SCRIPT_PTR);
	hle_insn(ctx, C_MOVEAL_AL);
	script_exec(ctx);
}

HLE_RANGE_PURE(0x009376, ScriptExec, 120, EXIT_JSR)
{
	script_exec(ctx);
}

HLE_RANGE_PURE(0x00933E, ScriptNext, 120, EXIT_RTS, EXIT_JSR)
{
	script_next_exec(ctx);
}

HLE_RANGE_PURE(0x009326, ScriptStart, 160, EXIT_RTS, EXIT_JSR)
{
	set_w(&ctx->d[0], RAM16(LEVEL));
	hle_insn(ctx, C_MOVEW_AL_D);
	asl_w2(ctx);
	hle_insn(ctx, C_ASL_W2);
	ctx->a[0] = LEVEL_SCRIPTS;
	hle_insn(ctx, C_LEA_AL);
	ctx->a[0] = rd32(LEVEL_SCRIPTS + (uint32_t)(int32_t)(int16_t)(ctx->d[0] & 0xFFFF));
	hle_insn(ctx, C_MOVEAL_IX);
	RAM16(SCRIPT_CLOCK) = 0;
	cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	hle_insn(ctx, C_CLR_W_AL);
	script_next_exec(ctx);
}
