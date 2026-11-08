/* VDP upload helpers: CramUpload $0080FE (64 CRAM words from (a0)), VramFillWords $008782 (d1 bytes at
 * VRAM d2 = word d6), the two VRAM fill loops sub_00873A / sub_00874E (a word per status poll) and
 * RamClear $0086C4 (boot work RAM clear). docs/porting.md, "VDP upload helpers".
 *
 * The VDP functions use the exact clock model of game/vdplist.c and game/textprint.c: every original
 * instruction advances the real CPU clock in the core's order (bus refresh check, the access, the
 * cost), the data port writes are timed (FIFO stalls), the status polls read the VDP at their own
 * clock, and the function runs inside an exact session. CramUpload is short and guarded by
 * `safe_run` (a loose upper bound; the 68k runs the call when a frame-loop event could fall in it).
 *
 * The fill loops are long (VramFillWords up to a whole plane, the 873A/874E loops ~100000 cycles, i.e.
 * a frame): they cross the frame-loop events. At top level they use the bail mechanism of vdplist.c:
 * the loop runs instruction by instruction, `due()` is checked before each one, and at the first
 * instruction boundary at or after the next event's clock every register and the CCR are mirrored into
 * ctx and the call jumps into the original code at that instruction (`hle_ctx.bail`). Called nested
 * (hle_call68k from the VDP list commands 2 and 5, which have their own guard) the loop must fit whole
 * (`safe_run`), else the 68k runs it.
 *
 * The status poll is `move.w (4,a5),d0 / btst.b #n,d0 / bne.s`: bits 10-15 of the value read are the
 * prefetched instruction word (see vdp_status_word in vdplist.c), the btst only sets Z. DmaToVram and
 * VramClearDma stay 68k (docs/remaining.md).
 */
#include "hle.h"
#include "native.h"
#include "gx.h"

#include <stdint.h>
#include <string.h>

static uint16_t vdp_status_word(uint32_t read_addr)
{
	return (uint16_t)((vdp_status_r() & 0x3FF) | (ROM16(read_addr + 4) & 0xFC00));
}

/* `move.w (4,a5),dN` at `at` (4 bytes): the core's status read adds the cost of the executing opcode
 * (m68k_cycles() = CYC_INSTRUCTION[REG_IR]), so REG_IR must hold the reading instruction. */
static uint16_t status_rd(uint32_t at, unsigned op);
static void post(int c);
/* the VDP raster follows the CPU clock; the difftest sandbox's reference run only moves the line base
 * (not the V counter, see sandbox.c), so the C side does the same there, else a status read's VINT bit
 * differs for a call that spans a line. The lockstep gate is the proof for the V counter. */
static void follow(void)
{
	if (hle_yielding())
		hle_yield_point();            /* the frame loop's line work happens while we are suspended */
	else if (hle_post_access)
		gx_vdp_follow_cpu();
	else
		gx_vdp_follow_cpu_lines();
}
static void pre(void)
{
	follow();
	int c = gx_m68k_cycles(), r = gx_m68k_refresh_cycles();
	if (c >= r && !hle_irqd_nested()) {
		gx_m68k_set_refresh_cycles(c + 128 * 7);
		gx_m68k_set_cycles(c + 2 * 7);
	}
}
static uint16_t status_rd(uint32_t at, unsigned op)
{
	pre();
	gx_m68k_set_ir(op);
	uint16_t st = vdp_status_word(at);
	post(12);
	return st;
}
static void post(int c) { hle_irqd_note(c * 7); gx_m68k_set_cycles(gx_m68k_cycles() + c * 7); }
static void tick(int c) { pre(); post(c); }
static void wr_ctrl(uint16_t v, int c) { pre(); vdp_ctrl_w(v); post(c); }
static void wr_data(uint16_t v, int c)
{
	pre();
	hle_vdp_timed(1);
	vdp_data_w(v);
	hle_vdp_timed(0);
	post(c);
}
static void sess_begin(void)
{
	hle_dma_begin();
	hle_exact_session();
	hle_irqd_enable();
}
static void sess_finish(void) { hle_dma_end(); }

static int safe_run(int bound_cycles)
{
	if (native_active)                  /* no 68k to hand the call to: the native runner takes interrupts between blocks */
		return 1;
	if (hle_chk_on)
		hle_chk_note(bound_cycles);
	if (hle_post_access || hle_yielding())
		return 1;
	if (gx_dma_length())
		return 0;
	long left = (long)gx_vdp_next_toggle_clock() - gx_m68k_cycles();
	return left > (long)bound_cycles * 7 + 3420;
}

/* margin-free guard for the fixed-length head of a bail-capable call (cycles) */
static int head_ok(int head_cycles)
{
	if (native_active)                  /* no 68k to hand the call to: the native runner takes interrupts between blocks */
		return 1;
	if (hle_chk_on)
		hle_chk_note(head_cycles);
	if (hle_post_access || hle_yielding())
		return 1;
	if (gx_dma_length())
		return 0;
	long left = (long)gx_vdp_next_toggle_clock() - gx_m68k_cycles();
	return left > (long)head_cycles * 7;
}

static int g_armed;
static long g_tog;
static int due(int slack)
{
	if (native_active)                  /* no 68k to hand the call to: the native runner takes interrupts between blocks */
		return 0;
	if (hle_yielding())
		return 0;
	if (g_armed)
		follow();
	return g_armed && (long)gx_m68k_cycles() + (long)slack * 7 >= g_tog;
}

/* ---------------------------------------------------------------- CramUpload $0080FE */
HLE_FUNC_YIELD(0x0080FE, CramUpload, 0)
{
	uint32_t a0 = ctx->a[0] & 0xFFFFFF;
	if (a0 + 128 > 0x1000000u || (a0 >= 0x400000u && a0 < 0xE00000u) ||
	    !safe_run(400 + 64 * 110)) {
		hle_decline();
		return;
	}
	sess_begin();
	ctx->a[5] = 0xC00000u;                                        /* 0080FE lea (0xc00000).l,a5 */
	tick(12);
	uint16_t st = status_rd(0x008104, 0x302D);                    /* 008104 move.w (4,a5),d0 */
	set_w(&ctx->d[0], st);
	wr_ctrl(0x8F02, 16);                                          /* 008108 move.w #$8f02,(4,a5) */
	pre();                                                        /* 00810E move.l #$c0000000,(4,a5) */
	vdp_ctrl_w32(0xC0000000u);
	post(24);
	ctx->d[0] = 0x3F;                                             /* 008116 moveq #$3f,d0 */
	tick(4);
	uint16_t last = 0;
	uint16_t d0 = 0x3F;
	for (;;) {
		for (;;) {                                            /* 008118 poll: move.w/btst.b #1/bne.s */
			st = status_rd(0x008118, 0x322D);
			set_w(&ctx->d[1], st);
			tick(10);
			if (!(st & 2)) {
				tick(8);
				break;
			}
			tick(10);
		}
		last = bus_r16(ctx->a[0]);                            /* 008122 move.w (a0)+,(a5) */
		ctx->a[0] += 2;
		wr_data(last, 12);
		d0 = (uint16_t)(d0 - 1);                              /* 008124 dbra d0 */
		if (d0 == 0xFFFF) {
			tick(14);
			break;
		}
		tick(10);
	}
	set_w(&ctx->d[0], 0xFFFF);
	cc_logic16(ctx, last);
	tick(16);                                                     /* 008128 rts */
	sess_finish();
}

/* ---------------------------------------------------------------- fill loops */
/* The common loop: `move.w d2,(a5)` at `loop`, then the poll move.w (4,a5),d0 (loop+2) / btst.b #8,d0
 * (loop+6) / bne.s (loop+10), dbra d1 (loop+12), falling out at loop+16. Returns 1 when bailing (ctx
 * is complete, ctx->pc = the next instruction). */
static int fill_loop(hle_ctx *ctx, uint32_t loop)
{
	uint16_t v = (uint16_t)ctx->d[2];
	for (;;) {
		if (due(0)) {
			ctx->pc = loop;
			return 1;
		}
		wr_data(v, 8);                                        /* move.w d2,(a5) */
		cc_logic16(ctx, v);
		for (;;) {
			if (due(0)) {
				ctx->pc = loop + 2;
				return 1;
			}
			uint16_t st = status_rd(loop + 2, 0x302D);    /* move.w (4,a5),d0 */
			set_w(&ctx->d[0], st);
			cc_logic16(ctx, st);
			if (due(0)) {
				ctx->pc = loop + 6;
				return 1;
			}
			tick(10);                                     /* btst.b #8,d0 */
			cc_set(ctx, CC_Z, (st & 0x100) ? 0 : CC_Z);
			if (due(0)) {
				ctx->pc = loop + 10;
				return 1;
			}
			if (st & 0x100) {                             /* bne.s */
				tick(10);
				continue;
			}
			tick(8);
			break;
		}
		if (due(0)) {
			ctx->pc = loop + 12;
			return 1;
		}
		uint16_t d1 = (uint16_t)ctx->d[1] - 1;                /* dbra d1 */
		set_w(&ctx->d[1], d1);
		if (d1 == 0xFFFF) {
			tick(14);
			return 0;
		}
		tick(10);
	}
}

/* shared entry: arm the bail points (top level) or require the whole call to fit (nested) */
static int fill_enter(int head_cycles, int bound)
{
	int nested = hle_nest_depth() > 0;
	if (nested ? !safe_run(bound) : !head_ok(head_cycles))
		return 0;
	sess_begin();
	g_armed = !nested && !hle_post_access;
	g_tog = g_armed ? (long)gx_vdp_next_toggle_clock() : (long)0x7FFFFFFF;
	return 1;
}
static void fill_leave(hle_ctx *ctx, int bail)
{
	g_armed = 0;
	sess_finish();
	if (bail) {
		ctx->bail = 1;
		hle_jump(ctx, ctx->pc);
	}
}

/* sub_00874E: lea / move.w #$6ff,d1 / three control words / d2 = 0 / loop at $00876E / rts */
static int tail_874e(hle_ctx *ctx)
{
	ctx->a[5] = 0xC00000u;                                        /* 00874E lea (0xc00000).l,a5 */
	tick(12);
	set_w(&ctx->d[1], 0x6FF);                                     /* 008754 move.w #$6ff,d1 */
	tick(8);
	wr_ctrl(0x8F02, 16);                                          /* 008758 */
	wr_ctrl(0x6000, 16);                                          /* 00875E */
	wr_ctrl(0x0003, 16);                                          /* 008764 */
	set_w(&ctx->d[2], 0);                                         /* 00876A move.w #0,d2 */
	cc_logic16(ctx, 0);
	tick(8);
	if (fill_loop(ctx, 0x00876E))
		return 1;
	if (due(0)) {                                                 /* before the rts */
		ctx->pc = 0x00877E;
		return 1;
	}
	tick(16);                                                     /* 00877E rts */
	return 0;
}

#define HEAD_874E 450
#define LOOP_COST 110

HLE_FUNC_YIELD(0x00874E, VramFillTail, 0)
{
	if (!fill_enter(HEAD_874E, 400 + 0x700 * LOOP_COST)) {
		hle_decline();
		return;
	}
	int bail = tail_874e(ctx);
	fill_leave(ctx, bail);
}

/* sub_00873A: d2 = 0, loop at $00873E over d1+1 words, then falls into sub_00874E */
HLE_FUNC_YIELD(0x00873A, VramFillBody, 0)
{
	if (ctx->a[5] != 0xC00000u ||
	    !fill_enter(100, 400 + (int)((ctx->d[1] & 0xFFFF) + 1 + 0x700) * LOOP_COST)) {
		hle_decline();
		return;
	}
	set_w(&ctx->d[2], 0);                                         /* 00873A move.w #0,d2 */
	cc_logic16(ctx, 0);
	tick(8);
	int bail = fill_loop(ctx, 0x00873E);
	if (!bail && due(HEAD_874E)) {                                /* the 874E head must fit before the event */
		ctx->pc = 0x00874E;
		bail = 1;
	}
	if (!bail)
		bail = tail_874e(ctx);
	fill_leave(ctx, bail);
}

/* ---------------------------------------------------------------- VramFillWords $008782 */
HLE_FUNC_YIELD(0x008782, VramFillWords, 0)
{
	uint32_t words = ((ctx->d[1] & 0xFFFF) >> 1) + 1;
	if (!fill_enter(500, 500 + (int)words * LOOP_COST)) {
		hle_decline();
		return;
	}
	ctx->a[5] = 0xC00000u;                                        /* 008782 lea (0xc00000).l,a5 */
	tick(12);
	uint16_t st = status_rd(0x008788, 0x302D);                    /* 008788 move.w (4,a5),d0 */
	set_w(&ctx->d[0], st);
	uint16_t d1w = (uint16_t)ctx->d[1];
	uint16_t half = d1w >> 1;
	set_w(&ctx->d[1], half);                                      /* 00878C lsr.w #1,d1 */
	tick(8);
	set_w(&ctx->d[1], (uint16_t)(half - 1));                      /* 00878E subq.w #1,d1 */
	tick(4);
	wr_ctrl(0x8F02, 16);                                          /* 008790 move.w #$8f02,(4,a5) */
	uint16_t d2 = (uint16_t)ctx->d[2];
	set_w(&ctx->d[3], d2);                                        /* 008796 move.w d2,d3 */
	tick(4);
	uint16_t d3 = d2 & 0x3FFF;
	set_w(&ctx->d[3], d3);                                        /* 008798 andi.w #$3fff,d3 */
	tick(8);
	d3 |= 0x4000;
	set_w(&ctx->d[3], d3);                                        /* 00879C ori.w #$4000,d3 */
	tick(8);
	wr_ctrl(d3, 12);                                              /* 0087A0 move.w d3,(4,a5) */
	set_w(&ctx->d[3], d2);                                        /* 0087A4 move.w d2,d3 */
	tick(4);
	d3 = (uint16_t)((d2 << 2) | (d2 >> 14));
	set_w(&ctx->d[3], d3);                                        /* 0087A6 rol.w #2,d3 */
	tick(10);
	d3 &= 3;
	set_w(&ctx->d[3], d3);                                        /* 0087A8 andi.w #3,d3 */
	tick(8);
	wr_ctrl(d3, 12);                                              /* 0087AC move.w d3,(4,a5) */
	uint16_t d6 = (uint16_t)ctx->d[6];
	set_w(&ctx->d[2], d6);                                        /* 0087B0 move.w d6,d2 */
	cc_logic16(ctx, d6);
	cc_set(ctx, CC_X, half == 0 ? CC_X : 0);                      /* X from the subq (borrow) */
	tick(4);
	int bail = fill_loop(ctx, 0x0087B2);
	if (!bail && due(0)) {
		ctx->pc = 0x0087C2;
		bail = 1;
	}
	if (!bail)
		tick(16);                                             /* 0087C2 rts */
	fill_leave(ctx, bail);
}

/* ---------------------------------------------------------------- RamClear $0086C4 */
/* clears $FF0000..$FFFF8B (the hi-scores/options/magic from $FFFF8C stay): 32710 iterations of
 * move.w d1,(a0)+ (8) / cmpa.l #imm,a0 (14) / bne.s (10 taken, 8 on the last) */
HLE_FUNC_PURE(0x0086C4, RamClear, 1050000)
{
	hle_insn(ctx, 12);                                            /* 0086C4 lea.l (0xffff0000).l,a0 */
	hle_insn(ctx, 4);                                             /* 0086CA moveq #0,d1 */
	memset(work_ram, 0, 0xFF8C);
	for (uint32_t p = 0; p < 0xFF8C; p += 2) {
		hle_insn(ctx, 8);                                     /* 0086CC move.w d1,(a0)+ */
		hle_insn(ctx, 14);                                    /* 0086CE cmpa.l #$ffffff8c,a0 */
		hle_insn(ctx, p + 2 != 0xFF8C ? 10 : 8);              /* 0086D4 bne.s */
	}
	ctx->a[0] = 0xFFFFFF8Cu;
	ctx->d[1] = 0;
	cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);                 /* cmpa.l: equal */
	hle_insn(ctx, 16);                                            /* 0086D6 rts */
}

/* ---------------------------------------------------------------- the DMA routines
 * VramClearDma $00800C / sub_00807C (VRAM fill by DMA) and DmaToVram $011FA2 / DmaToVramSave $011F94 (68k to
 * VRAM DMA). They start a transfer and poll the DMA-busy flag, which lasts for lines or frames: the polls
 * are instruction-exact like the other VDP code here and the function is a coroutine, so at every slice end
 * it hands the machine to the frame loop, which advances the DMA line by line (GPGX stalls the CPU clock
 * to the end of the chunk, vdp_dma_update) exactly as it does for the 68k in its poll loop. The session
 * ends with hle_exact_end(), not hle_dma_end(): the transfer must not be completed at once. */
static void dma_end(void) { hle_exact_end(); }
/* movem.l: GPGX checks the bus refresh threshold again after the per-register part (n x 8 cycles) and then
 * skips one refresh (hle_insn_x in hle.c) */
static void movem_skip(int n)
{
	if (gx_m68k_cycles() + n * 8 * 7 >= gx_m68k_refresh_cycles())
		gx_m68k_set_refresh_cycles(gx_m68k_refresh_cycles() + 128 * 7);
}

/* a RAM-only instruction: the yield point and refresh come first, then the effect, then the cost */
#define RAMOP(cost, ...) do { pre(); __VA_ARGS__; post(cost); } while (0)

/* poll the DMA-busy flag: move.w (4,a5),dN / btst.b #1,dN / bne.s */
static void dma_poll(hle_ctx *ctx, int reg, uint32_t at, unsigned op)
{
	for (;;) {
		uint16_t st = status_rd(at, op);
		set_w(&ctx->d[reg], st);
		cc_logic16(ctx, st);
		tick(10);                                             /* btst.b #1,dN */
		cc_set(ctx, CC_Z, (st & 2) ? 0 : CC_Z);
		if (st & 2) {
			tick(10);                                     /* bne.s taken */
			continue;
		}
		tick(8);
		break;
	}
}

/* bset.b #4,(vdp_reg01+1) / move.w (vdp_reg01),(ctrl): DMA enable on / the shadow register written back */
static void dma_enable(hle_ctx *ctx, int on)
{
	RAMOP(24, uint8_t o = RAM8(0xFFA0DD); cc_set(ctx, CC_Z, (o & 0x10) ? 0 : CC_Z); RAM8(0xFFA0DD) = on ? (o | 0x10) : (uint8_t)(o & ~0x10));
	uint16_t v = RAM16(0xFFA0DC);
	pre();
	vdp_ctrl_w(v);
	cc_logic16(ctx, v);
	post(28);
}

/* VramClearDma / DmaFillAt: exact since the delayed IRQ of the register 1 write is modelled (hle.c irqd_hook) */
#if 1
HLE_FUNC_YIELD(0x00800C, VramClearDma, 0)
{
	sess_begin();
	ctx->a[5] = 0xC00000u;                                        /* 00800C lea.l (0xc00000).l,a5 */
	tick(12);
	set_w(&ctx->d[0], status_rd(0x008012, 0x302D));               /* 008012 move.w (4,a5),d0 */
	dma_enable(ctx, 1);                                           /* 008016 bset / 00801E move.w */
	wr_ctrl(0x8F01, 16);                                          /* 008028 */
	wr_ctrl(0x93F0, 16);                                          /* 00802E */
	wr_ctrl(0x94FF, 16);                                          /* 008034 */
	wr_ctrl(0x9780, 16);                                          /* 00803A */
	RAMOP(28, RAM32_W(0xFFA0E8, 0x40000080u); cc_logic32(ctx, 0x40000080u));   /* 008040 move.l #$40000080,(abs).l */
	{ uint16_t v; pre(); v = RAM16(0xFFA0E8); vdp_ctrl_w(v); cc_logic16(ctx, v); post(24); }   /* 00804A */
	{ uint16_t v; pre(); v = RAM16(0xFFA0EA); vdp_ctrl_w(v); cc_logic16(ctx, v); post(24); }   /* 008052 */
	wr_data(0, 12);                                               /* 00805A move.w #0,(a5): starts the fill */
	cc_logic16(ctx, 0);
	dma_poll(ctx, 0, 0x00805E, 0x302D);                           /* 00805E */
	dma_enable(ctx, 0);                                           /* 008068 bclr / 008070 move.w */
	tick(16);                                                     /* 00807A rts */
	dma_end();
}

HLE_FUNC_YIELD(0x00807C, DmaFillAt, 0)
{
	sess_begin();
	ctx->a[5] = 0xC00000u;                                        /* 00807C lea.l (0xc00000).l,a5 */
	tick(12);
	dma_enable(ctx, 1);                                           /* 008082 bset / 00808A move.w */
	wr_ctrl(0x8F01, 16);                                          /* 008094 */
	wr_ctrl(0x9300, 16);                                          /* 00809A */
	wr_ctrl(0x9410, 16);                                          /* 0080A0 */
	wr_ctrl(0x9780, 16);                                          /* 0080A6 */
	uint16_t d1 = (uint16_t)ctx->d[1];
	set_w(&ctx->d[0], d1); cc_logic16(ctx, d1); tick(4);          /* 0080AC move.w d1,d0 */
	uint16_t d0 = d1 & 0x3FFF;
	set_w(&ctx->d[0], d0); cc_logic16(ctx, d0); tick(8);          /* 0080AE andi.w #$3fff,d0 */
	d0 |= 0x4000;
	set_w(&ctx->d[0], d0); cc_logic16(ctx, d0); tick(8);          /* 0080B2 ori.w #$4000,d0 */
	RAMOP(16, RAM16(0xFFA0E8) = d0; cc_logic16(ctx, d0));         /* 0080B6 move.w d0,(abs).l */
	d1 = (uint16_t)((d1 << 2) | (d1 >> 14));
	set_w(&ctx->d[1], d1); tick(10);                              /* 0080BC rol.w #2,d1 */
	cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, ((d1 & 0x8000) ? CC_N : 0) | (d1 ? 0 : CC_Z) | ((d1 & 1) ? CC_C : 0));
	d1 &= 3;
	set_w(&ctx->d[1], d1); cc_logic16(ctx, d1); tick(8);          /* 0080BE andi.w #3,d1 */
	d1 |= 0x80;
	set_w(&ctx->d[1], d1); cc_logic16(ctx, d1); tick(8);          /* 0080C2 ori.w #$80,d1 */
	RAMOP(16, RAM16(0xFFA0EA) = d1; cc_logic16(ctx, d1));         /* 0080C6 move.w d1,(abs).l */
	{ uint16_t v; pre(); v = RAM16(0xFFA0E8); vdp_ctrl_w(v); cc_logic16(ctx, v); post(24); }   /* 0080CC */
	{ uint16_t v; pre(); v = RAM16(0xFFA0EA); vdp_ctrl_w(v); cc_logic16(ctx, v); post(24); }   /* 0080D4 */
	wr_data(0, 12);                                               /* 0080DC move.w #0,(a5): starts the fill */
	cc_logic16(ctx, 0);
	dma_poll(ctx, 0, 0x0080E0, 0x302D);                           /* 0080E0 */
	dma_enable(ctx, 0);                                           /* 0080EA bclr / 0080F2 move.w */
	tick(16);                                                     /* 0080FC rts */
	dma_end();
}

#endif

/* DmaToVram: the 68k to VRAM transfer described by the record at $FFA0E0 (length, source, destination) */
static void dma_to_vram(hle_ctx *ctx)
{
	ctx->a[5] = 0xC00000u;                                        /* 011FA2 lea.l (0xc00000).l,a5 */
	tick(12);
	ctx->a[0] = 0xFFFFA0E0u;                                      /* 011FA8 lea.l (0xffffa0e0).l,a0 */
	tick(12);
	dma_poll(ctx, 0, 0x011FAE, 0x302D);                           /* 011FAE */
	dma_enable(ctx, 1);                                           /* 011FB8 bset / 011FC0 move.w */
	wr_ctrl(0x8F02, 16);                                          /* 011FCA */
	uint32_t a0 = ctx->a[0];
	uint16_t d0 = 0x9300;                                         /* 011FD0 move.w #$9300,d0 */
	set_w(&ctx->d[0], d0); cc_logic16(ctx, d0); tick(8);
	d0 = (uint16_t)((d0 & 0xFF00) | RAM8(a0 + 1));                /* 011FD4 move.b (1,a0),d0 */
	set_w(&ctx->d[0], d0); cc_logic8(ctx, d0 & 0xFF); tick(12);
	wr_ctrl(d0, 12);                                              /* 011FD8 */
	d0 = 0x9400;                                                  /* 011FDC */
	set_w(&ctx->d[0], d0); cc_logic16(ctx, d0); tick(8);
	d0 = (uint16_t)((d0 & 0xFF00) | RAM8(a0));                    /* 011FE0 move.b (a0),d0 */
	set_w(&ctx->d[0], d0); cc_logic8(ctx, d0 & 0xFF); tick(8);
	wr_ctrl(d0, 12);                                              /* 011FE2 */
	uint32_t d1 = RAM32(a0 + 2);                                  /* 011FE6 move.l (2,a0),d1 */
	ctx->d[1] = d1; cc_logic32(ctx, d1); tick(16);
	d1 &= 0xFFFFFF;                                               /* 011FEA andi.l #$ffffff,d1 */
	ctx->d[1] = d1; cc_logic32(ctx, d1); tick(16);
	{ unsigned c = d1 & 1; d1 >>= 1; ctx->d[1] = d1;              /* 011FF0 lsr.l #1,d1 */
	  cc_set(ctx, CC_X | CC_N | CC_Z | CC_V | CC_C, (c ? (CC_X | CC_C) : 0) | (d1 ? 0 : CC_Z)); tick(10); }
	d0 = 0x9500;                                                  /* 011FF2 */
	set_w(&ctx->d[0], d0); cc_logic16(ctx, d0); tick(8);
	d0 = (uint16_t)(d0 | (d1 & 0xFF));                            /* 011FF6 or.b d1,d0 */
	set_w(&ctx->d[0], d0); cc_logic8(ctx, d0 & 0xFF); tick(4);
	wr_ctrl(d0, 12);                                              /* 011FF8 */
	d0 = 0x9600;                                                  /* 011FFC */
	set_w(&ctx->d[0], d0); cc_logic16(ctx, d0); tick(8);
	{ unsigned c = (d1 >> 7) & 1; uint16_t w = (uint16_t)(((d1 & 0xFFFF) >> 8)); set_w(&ctx->d[1], w); d1 = ctx->d[1];   /* 012000 lsr.w #8,d1 */
	  cc_set(ctx, CC_X | CC_N | CC_Z | CC_V | CC_C, (c ? (CC_X | CC_C) : 0) | (w ? 0 : CC_Z)); tick(22); }
	d0 = (uint16_t)(d0 | (d1 & 0xFF));                            /* 012002 or.b d1,d0 */
	set_w(&ctx->d[0], d0); cc_logic8(ctx, d0 & 0xFF); tick(4);
	wr_ctrl(d0, 12);                                              /* 012004 */
	d0 = 0x9700;                                                  /* 012008 */
	set_w(&ctx->d[0], d0); cc_logic16(ctx, d0); tick(8);
	d1 = (d1 << 16) | (d1 >> 16); ctx->d[1] = d1; cc_logic32(ctx, d1); tick(4);   /* 01200C swap d1 */
	d0 = (uint16_t)(d0 | (d1 & 0xFF));                            /* 01200E or.b d1,d0 */
	set_w(&ctx->d[0], d0); cc_logic8(ctx, d0 & 0xFF); tick(4);
	wr_ctrl(d0, 12);                                              /* 012010 */
	uint16_t w1 = RAM16(a0 + 6);                                  /* 012014 move.w (6,a0),d1 */
	set_w(&ctx->d[1], w1); cc_logic16(ctx, w1); tick(12);
	uint16_t w2 = w1;                                             /* 012018 move.w d1,d2 */
	set_w(&ctx->d[2], w2); cc_logic16(ctx, w2); tick(4);
	w1 &= 0x3FFF; set_w(&ctx->d[1], w1); cc_logic16(ctx, w1); tick(8);       /* 01201A andi.w #$3fff,d1 */
	w1 |= 0x4000; set_w(&ctx->d[1], w1); cc_logic16(ctx, w1); tick(8);       /* 01201E ori.w #$4000,d1 */
	wr_ctrl(w1, 12);                                              /* 012022 */
	w2 &= 0xC000; set_w(&ctx->d[2], w2); cc_logic16(ctx, w2); tick(8);       /* 012026 andi.w #$c000,d2 */
	{ uint16_t r = (uint16_t)((w2 << 2) | (w2 >> 14)); w2 = r; set_w(&ctx->d[2], w2); tick(10);   /* 01202A rol.w #2,d2 */
	  cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, ((w2 & 0x8000) ? CC_N : 0) | (w2 ? 0 : CC_Z) | ((w2 & 1) ? CC_C : 0)); }
	w2 |= 0x80; set_w(&ctx->d[2], w2); cc_logic16(ctx, w2); tick(8);         /* 01202C ori.w #$80,d2 */
	RAMOP(12, RAM16(a0 + 8) = w2; cc_logic16(ctx, w2));           /* 012030 move.w d2,(8,a0) */
	{ uint16_t v; pre(); v = RAM16(a0 + 8); vdp_ctrl_w(v); cc_logic16(ctx, v); post(20); }   /* 012034: starts the DMA */
	dma_poll(ctx, 0, 0x01203A, 0x302D);                           /* 01203A */
	dma_enable(ctx, 0);                                           /* 012044 bclr / 01204C move.w */
	ctx->a[1] = RAM32(a0 + 2);                                    /* 012056 movea.l (2,a0),a1 */
	tick(16);
	wr_ctrl(w1, 12);                                              /* 01205A move.w d1,(4,a5) */
	{ uint16_t v; pre(); v = RAM16(a0 + 8); vdp_ctrl_w(v); cc_logic16(ctx, v); post(20); }   /* 01205E */
	{ uint16_t v; pre(); v = bus_r16(ctx->a[1]); hle_vdp_timed(1); vdp_data_w(v); hle_vdp_timed(0); cc_logic16(ctx, v); post(12); }   /* 012064 move.w (a1),(a5) */
	tick(16);                                                     /* 012066 rts */
}

HLE_FUNC_YIELD(0x011FA2, DmaToVram, 0)
{
	sess_begin();
	dma_to_vram(ctx);
	dma_end();
}

/* DmaToVramSave: movem.l d0-d2/a0-a5,-(sp) / jsr DmaToVram / movem.l (sp)+,... / rts */
HLE_FUNC_YIELD(0x011F94, DmaToVramSave, 0)
{
	sess_begin();
	{
		uint32_t save[9] = { ctx->d[0], ctx->d[1], ctx->d[2], ctx->a[0], ctx->a[1], ctx->a[2], ctx->a[3], ctx->a[4], ctx->a[5] };
		pre();
		for (int i = 8; i >= 0; i--) {
			ctx->a[7] -= 4;
			bus_w32(ctx->a[7], save[i]);
		}
		movem_skip(9);                                        /* GPGX: the refresh check inside movem */
		post(80);                                             /* 011F94 movem.l d0-d2/a0-a5,-(sp): 8 + 8*9 */
		tick(18);                                             /* 011F98 jsr (DmaToVram,pc) */
		dma_to_vram(ctx);
		pre();
		for (int i = 0; i < 9; i++) {
			uint32_t v = bus_r32(ctx->a[7]);
			ctx->a[7] += 4;
			if (i < 3) ctx->d[i] = v; else ctx->a[i - 3] = v;
		}
		movem_skip(9);
		post(84);                                             /* 011F9C movem.l (sp)+,d0-d2/a0-a5 */
		tick(16);                                             /* 011FA0 rts */
	}
	dma_end();
}

/* sub_0081B6: VSRAM address 0, then two words d0, d1 (the vertical scroll pair) once the FIFO has room */
HLE_FUNC_YIELD(0x0081B6, VdpVScrollWrite, 0)
{
	sess_begin();
	ctx->a[5] = 0xC00000u;                                        /* 0081B6 lea.l (0xc00000).l,a5 */
	tick(12);
	wr_ctrl(0x8F02, 16);                                          /* 0081BC */
	pre();                                                        /* 0081C2 move.l #$40000010,(4,a5) */
	vdp_ctrl_w32(0x40000010u);
	cc_logic32(ctx, 0x40000010u);
	post(24);
	dma_poll(ctx, 2, 0x0081CA, 0x342D);                           /* 0081CA move.w (4,a5),d2 / btst / bne */
	wr_data((uint16_t)ctx->d[0], 8);                              /* 0081D4 move.w d0,(a5) */
	wr_data((uint16_t)ctx->d[1], 8);                              /* 0081D6 move.w d1,(a5) */
	cc_logic16(ctx, (uint16_t)ctx->d[1]);
	tick(16);                                                     /* 0081D8 rts */
	dma_end();
}
