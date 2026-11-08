/* Level-script opcode handlers (ScriptOps $00938A; docs/subsystems/levels.md, "Opcodes").
 *
 * The interpreter (game/levelscript.c) ends its ranges in front of `jsr (a1)`; the real 68k
 * performs the jsr and the CPU hook then replaces the handler at its entry address. On entry
 * a0 = the handler's arguments; a handler leaves a0 after them (or at the jump target) and rts's
 * back to `bra.w ScriptNext`.
 *
 * Replaced here: goto $0099AA, scroll_script setters $0099B0/$0099BC, poke_word
 * $00A130, poke_long $00A138, end_level $009C36, gosub $00B2E6, return $00B2F2, and the thin jsr
 * wrappers text $009C18 (PrintText) and text_clear $009C30, which call the 68k
 * callee through hle_call68k. Skipped: nop $0099AE and draw_17 $009C2A (no script uses
 * them), native $00A140 (jmp into arbitrary 68k) and the spawns.
 *
 * Every poke target in the 8 scripts is work RAM ($FF9Exx..$FFAFxx); the pokes go to RAM only.
 * Costs are per instruction (the rts included), as a HLE_SEQ timeline. */
#include "hle.h"

#include <stdint.h>

#define SCROLL_SCRIPT 0xFF9E98u
#define LEVEL_EXIT    0xFFA234u
#define SCRIPT_RET    0xFF9F62u

static inline uint16_t rd16(uint32_t a)
{
	return (a & 0xFF0000u) == 0xFF0000u ? RAM16(a) : ROM16(a);
}
static inline uint32_t rd32(uint32_t a) { return ((uint32_t)rd16(a) << 16) | rd16(a + 2); }

/* $0099AA: movea.l (a0)+,a0 ; rts   (movea: no flags) */
HLE_FUNC_PURE(0x0099AA, ScrGoto, 28)
{
	HLE_SEQ(ctx, 12, 16);
	ctx->a[0] = rd32(ctx->a[0]);
}

/* move.l #imm,(abs).l ; rts: N=Z=V=C=0 (imm positive, nonzero), X kept */
static void set_scroll_script(hle_ctx *ctx, uint32_t v)
{
	HLE_SEQ(ctx, 28, 16);
	RAM32_W(SCROLL_SCRIPT, v);
	cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, 0);
}

HLE_FUNC_PURE(0x0099B0, ScrScrollTab4A88, 44) { set_scroll_script(ctx, 0x4A88); }
HLE_FUNC_PURE(0x0099BC, ScrScrollTab4A94, 44) { set_scroll_script(ctx, 0x4A94); }

/* $00A130: move.w (a0)+,d0 ; movea.l (a0)+,a1 ; move.w d0,(a1) ; rts */
HLE_FUNC_PURE(0x00A130, ScrPokeWord, 44)
{
	HLE_SEQ(ctx, 8, 12, 8, 16);
	uint16_t v = rd16(ctx->a[0]);
	uint32_t a1 = rd32(ctx->a[0] + 2);
	ctx->a[0] += 6;
	set_w(&ctx->d[0], v);
	ctx->a[1] = a1;
	RAM16(a1) = v;
	cc_logic16(ctx, v);
}

/* $00A138: move.l (a0)+,d0 ; movea.l (a0)+,a1 ; move.l d0,(a1) ; rts */
HLE_FUNC_PURE(0x00A138, ScrPokeLong, 52)
{
	HLE_SEQ(ctx, 12, 12, 12, 16);
	uint32_t v = rd32(ctx->a[0]);
	uint32_t a1 = rd32(ctx->a[0] + 4);
	ctx->a[0] += 8;
	ctx->d[0] = v;
	ctx->a[1] = a1;
	RAM32_W(a1, v);
	cc_logic32(ctx, v);
}

/* $009C36: move.b #$ff,(level_exit).l ; rts */
HLE_FUNC_PURE(0x009C36, ScrEndLevel, 36)
{
	HLE_SEQ(ctx, 20, 16);
	RAM8(LEVEL_EXIT) = 0xFF;
	cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_N);
}

/* $00B2E6: movea.l (a0)+,a1 ; move.l a0,(script_ret).l ; movea.l a1,a0 ; rts
 * (the stored return address is a0 after the argument, flags: move.l a0 -> N/Z, V=C=0) */
HLE_FUNC_PURE(0x00B2E6, ScrGosub, 52)
{
	HLE_SEQ(ctx, 12, 20, 4, 16);
	ctx->a[1] = rd32(ctx->a[0]);
	uint32_t ret = ctx->a[0] + 4;
	RAM32_W(SCRIPT_RET, ret);
	ctx->a[0] = ctx->a[1];
	cc_logic32(ctx, ret);
}

/* $00B2F2: movea.l (script_ret).l,a0 ; rts */
HLE_FUNC_PURE(0x00B2F2, ScrReturn, 36)
{
	HLE_SEQ(ctx, 20, 16);
	ctx->a[0] = RAM32(SCRIPT_RET);
}

/* $009C18: movea.l (a0)+,a1 ; move.l a0,-(sp) ; movea.l a1,a0 ; move.w #$c000,d6 ;
 *          jsr (PrintText,pc) ; movea.l (sp)+,a0 ; rts.   PrintText runs as 68k. */
HLE_FUNC_YIELD(0x009C18, ScrText, 60000)
{
	HLE_SEQ(ctx, 12, 12, 4, 8, 18);
	ctx->a[1] = rd32(ctx->a[0]);
	ctx->a[0] += 4;
	push32(ctx, ctx->a[0]);
	ctx->a[0] = ctx->a[1];
	set_w(&ctx->d[6], 0xC000);
	cc_logic16(ctx, 0xC000);
	hle_sync(ctx);
	hle_callc(ctx, 0x007DEE);
	hle_insn(ctx, 12);
	ctx->a[0] = pop32(ctx);
	hle_insn(ctx, 16);
}

/* $009C30: jsr (callee,pc) ; rts */
HLE_FUNC_YIELD(0x009C30, ScrTextClear, 60000)
{
	hle_insn(ctx, 18);
	hle_sync(ctx);
	hle_callc(ctx, 0x00BF46);
	hle_insn(ctx, 16);
}
