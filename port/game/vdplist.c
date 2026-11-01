/* VDP command list interpreter: VdpListRun $011D32 and its 9 command handlers
 * (VdpListJump $011D40: 0 end, 1 copy words, 2 fill 0, 3 write word, 4 CRAM line,
 *  5 fill value, 6 tile rect, 7 hscroll, 8 vscroll). docs/subsystems/vdp.md.
 *
 * The list lives in RAM at vdp_list $FFB252 (write pointer vdp_list_ptr $FF80BC).
 * VdpListRun sets a6 to the list and loops: `move.w (a6)+,d0; asl.w #2,d0;
 * jmp VdpListJump(pc,d0.w)`. Every handler reads its arguments from (a6)+ and either
 * falls back into VdpListNext $011D38 (tail `bra.w`, no flags) or, for command 0, rewinds
 * vdp_list_ptr and `rts`. So VdpListRun is an ordinary subroutine whose return happens at
 * command 0; the other handlers are code ranges that leave through $011D38.
 *
 * Registration: VdpListRun is an HLE_FUNC that runs the whole list, calling the shared
 * command bodies directly. Each handler is separately registered so it can be proved by
 * difftest: command 0 as an HLE_FUNC (it ends in the rts), 1..8 as HLE_RANGEs whose single
 * exit is VdpListNext $011D38 (SP unchanged: the handlers never touch the stack). Enabling
 * a handler alone still works: the 68k VdpListRun jumps to it, the hook runs the C range and
 * it hle_jump()s back to VdpListNext.
 *
 * Callees. VramFillWords $008782 (commands 2 and 5) and VramWriteWord $0087C4 (command 3)
 * are not ported, so the bodies call them through hle_call68k: the nested 68k runs exactly as
 * the original `jsr` (registers, flags, RAM and the ordered VDP writes are identical) and the
 * layer adds its cycles to the charge. The other commands' VDP writes are inline in the
 * handler and are reproduced here through vdp_ctrl_w / vdp_data_w (same order and width:
 * status poll first, then bset/bclr of the DMA-enable shadow bit $FFA0DD bit 4 around the
 * copy where the original does it, autoincrement $8F02/$8F20, the 14-bit address + CD bits,
 * and for command 4 the 32-bit CRAM command). Reading the status is done with vdp_status_r()
 * because the original's `move.w (4,a5),d0` has side effects (clears `pending` and the
 * SOVR/SCOL bits) and its value ends up in d0/d1/d2.
 *
 * Registers/flags. Each body mirrors every register write with its exact width (move.w =
 * set_w, moveq/movea/lea = full) and the final CCR of the last flag-setting instruction.
 * Only the final state is observable, but the register *widths* matter because the high
 * words survive. Command 6's final flags are the `addi.w #$80,d3` of the last row; command 4
 * and 8's are the closing `move.w shadow,($C00004)` / the second vscroll data write.
 *
 * Cycles. Charged per path (docs/porting.md, like BuildSpriteTable): each translated
 * instruction adds its GPGX 68000 cost as it executes, so the per-command cost and the whole
 * list cost track the original. Commands 2/3/5 charge only their own arg reads; the nested
 * VramFillWords/VramWriteWord time is added by the layer. T2_VDP_STATS=1 prints the per-
 * command call counts of a run (used to show every command is exercised by the captures).
 */
#include "hle.h"
#include "gx.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define VDP_LIST        0xFFFFB252u   /* vdp_list: command words */
#define VDP_LIST_PTR    0xFF80BCu
#define VDP_REG01       0xFFA0DCu     /* shadow of VDP regs 0/1 (word) */
#define VDP_REG01_DMA   0xFFA0DDu     /* its low byte; bit 4 = DMA enable */
#define VDP_LIST_NEXT   0x011D38u     /* VdpListNext */
#define VRAM_FILL_WORDS 0x008782u
#define VRAM_WRITE_WORD 0x0087C4u

/* ---------------------------------------------------------------- path stats (T2_VDP_STATS) */
static unsigned long g_cmd[9];
static int g_stats = -1;

static void vdp_stats_print(void)
{
	if (g_stats != 1)
		return;
	fprintf(stderr,
	        "VdpListRun commands: 0 end=%lu 1 copy=%lu 2 fill0=%lu 3 word=%lu 4 cram=%lu "
	        "5 fillv=%lu 6 rect=%lu 7 hscroll=%lu 8 vscroll=%lu\n",
	        g_cmd[0], g_cmd[1], g_cmd[2], g_cmd[3], g_cmd[4],
	        g_cmd[5], g_cmd[6], g_cmd[7], g_cmd[8]);
}

static int vdp_stats_on(void)
{
	if (g_stats < 0) {
		g_stats = getenv("T2_VDP_STATS") != NULL;
		if (g_stats)
			atexit(vdp_stats_print);
	}
	return g_stats == 1;
}

static void count_cmd(int c)
{
	if (vdp_stats_on() && c >= 0 && c < 9)
		g_cmd[c]++;
}

/* ---------------------------------------------------------------- flag helpers */
/* ASL.W #n,Dn (1<=n<=8): X=C=last bit out, V=MSB changed, N/Z from result. */
static uint16_t asl_w(hle_ctx *ctx, uint16_t v, int n)
{
	uint32_t w = (uint32_t)v << n;
	uint16_t r = (uint16_t)w;
	unsigned c = (v >> (16 - n)) & 1;
	unsigned ov = (((v ^ r) & 0x8000) != 0);
	cc_set(ctx, CC_X | CC_N | CC_Z | CC_V | CC_C,
	       (c ? (CC_X | CC_C) : 0) | ((r & 0x8000) ? CC_N : 0) | (r ? 0 : CC_Z) |
	       (ov ? CC_V : 0));
	return r;
}

/* ROL.W #n,Dn: C=last bit rotated out, N/Z from result, V=0, X unchanged. */
static uint16_t rol_w(hle_ctx *ctx, uint16_t v, int n)
{
	uint16_t r = (uint16_t)((v << n) | (v >> (16 - n)));
	unsigned c = (v >> (16 - n)) & 1;
	cc_set(ctx, CC_N | CC_Z | CC_V | CC_C,
	       (c ? CC_C : 0) | ((r & 0x8000) ? CC_N : 0) | (r ? 0 : CC_Z));
	return r;
}

/* ADD.W / ADDI.W / ADDQ.W: N/Z/V/C from the result, X=C (carry out). */
static uint16_t add_w(hle_ctx *ctx, uint16_t a, uint16_t b)
{
	uint32_t r = (uint32_t)a + b;
	uint16_t rr = (uint16_t)r;
	unsigned carry = (r >> 16) & 1;
	unsigned ov = (((a ^ rr) & (b ^ rr)) >> 15) & 1;
	cc_set(ctx, CC_X | CC_N | CC_Z | CC_V | CC_C,
	       (carry ? (CC_X | CC_C) : 0) | ((rr & 0x8000) ? CC_N : 0) | (rr ? 0 : CC_Z) |
	       (ov ? CC_V : 0));
	return rr;
}

/* A 68k control-port word read returns the VDP status (low 10 bits) OR'd with bits 10-15 of
 * the prefetched instruction word (mem68k.c vdp_read_word). While the 68k executes a 4-byte
 * `move.w (d16,An),Dn` at `read_addr`, m68k.pc is already read_addr+4, so that is the word
 * OR'd in. From C m68k.pc is the read instruction, so rebuild the value. */
static uint16_t vdp_status_word(uint32_t read_addr)
{
	return (uint16_t)((vdp_status_r() & 0x3FF) | (ROM16(read_addr + 4) & 0xFC00));
}

/* poll the VDP status until DMA is not busy, as `move.w (4,a5),d0; btst.b #1,d0; bne` */
static void poll_status(hle_ctx *ctx, int reg, uint32_t read_addr)
{
	uint16_t st;
	do {
		st = vdp_status_word(read_addr);
		set_w(&ctx->d[reg], st);
	} while (st & 2);
}

/* The original `jsr (target).l` costs 20 cycles before the callee runs; hle_call68k pushes a
 * sentinel and starts the callee without charging them. Advance the CPU clock so a status
 * poll inside the callee sees the same raster time as the original. Inside a C function this
 * is part of the session, so the layer counts it as nested time. */
static void jsr_tick(void) { gx_m68k_set_cycles(gx_m68k_cycles() + 20 * 7); }

/* Advance the CPU clock by `c` 68k cycles of C-executed instructions, so a later status poll
 * sees the real raster time. Inside a C function the layer counts it as nested time. */
static void tick(int c) { gx_m68k_set_cycles(gx_m68k_cycles() + c * 7); }

/* Run the original 68k from `start` up to `stop` (a timing-sensitive VDP status read that
 * must happen at the real raster time), then leave ctx at `stop` for C. */
static void run_to(hle_ctx *ctx, uint32_t start, uint32_t stop)
{
	uint32_t exits[2];
	exits[0] = stop;
	exits[1] = 0;
	ctx->pc = start;
	hle_run68k_until(ctx, exits);
	ctx->jumped = 0;
	ctx->pc = stop;
}

/* set up the VRAM/CRAM address: d2 = (addr & $3fff) | $4000, d1 = (addr rol 2) & 3 */
static void vram_addr(hle_ctx *ctx, uint16_t addr, int d1reg, int d2reg)
{
	set_w(&ctx->d[d1reg], addr);
	uint16_t d1 = rol_w(ctx, addr, 2) & 3;      /* move.w d2,d1 / rol.w #2,d1 / andi.w #3,d1 */
	set_w(&ctx->d[d1reg], d1);
	set_w(&ctx->d[d2reg], (uint16_t)((addr & 0x3FFF) | 0x4000));
}

/* ---------------------------------------------------------------- command bodies */
/* cmd 0 ($011D64): rewind the list; the HLE_FUNC layer performs the rts. */
static void cmd_end(hle_ctx *ctx)
{
	count_cmd(0);
	RAM32_W(VDP_LIST_PTR, VDP_LIST);            /* move.l #vdp_list,(vdp_list_ptr).l */
	ctx->cycles += 20 + 16;                     /* move.l Dn,(abs).l + rts */
	/* final flags from the move.l: N from $FFFFB252 bit 31 (=1), Z=0, V=C=0, X unchanged */
	cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_N);
}

/* cmd 1 ($012068): copy `count` words from src to VRAM, autoincrement 2. */
static void cmd_copy_words(hle_ctx *ctx)
{
	count_cmd(1);
	run_to(ctx, 0x012068, 0x012074);            /* prologue (arg reads) as 68k: the status poll
	                                             * below must see the real raster time */
	uint16_t count = ctx->d[4] & 0xFFFF;
	uint32_t src = ctx->a[1];
	uint16_t addr = ctx->d[2] & 0xFFFF;

	poll_status(ctx, 0, 0x012074);              /* loc_012074: move.w (4,a5),d0 / btst / bne */
	ctx->cycles += 12 + 6 + 10;                 /* (one poll iteration) */
	RAM8(VDP_REG01_DMA) |= 0x10;                /* bset.b #4,(vdp_reg01+1).l */
	ctx->cycles += 20;
	vdp_ctrl_w(RAM16(VDP_REG01));               /* move.w (vdp_reg01).l,(4,a5) */
	ctx->cycles += 28;
	vdp_ctrl_w(0x8F02);                         /* move.w #$8f02,(4,a5) */
	ctx->cycles += 12;
	vram_addr(ctx, addr, 1, 2);                 /* move.w d2,d1 / andi / ori / rol / andi */
	ctx->cycles += 4 + 8 + 8 + 10 + 8;
	vdp_ctrl_w(ctx->d[2] & 0xFFFF);             /* move.w d2,(4,a5) */
	vdp_ctrl_w(ctx->d[1] & 0xFFFF);             /* move.w d1,(4,a5) */
	ctx->cycles += 12 + 12;

	uint16_t d4 = (uint16_t)(count - 1);        /* subq.w #1,d4 */
	ctx->cycles += 4;
	uint16_t last = 0;
	for (;;) {                                  /* loc_0120B0: move.w (a1)+,(a5) / dbra d4 */
		last = bus_r16(ctx->a[1]);
		ctx->a[1] += 2;
		vdp_data_w(last);
		d4 = (uint16_t)(d4 - 1);
		if (d4 == 0xFFFF) {
			ctx->cycles += 8 + 14;      /* move.w + dbra expire */
			break;
		}
		ctx->cycles += 8 + 10;              /* move.w + dbra continue */
	}
	set_w(&ctx->d[4], 0xFFFF);
	cc_logic16(ctx, last);                      /* final flags: the last move.w */
}

/* cmd 2 ($011D70): fill `bytes` bytes at VRAM addr with 0 (VramFillWords). */
static void cmd_fill_zero(hle_ctx *ctx)
{
	count_cmd(2);
	run_to(ctx, 0x011D70, 0x011D76);            /* prologue (arg reads) as 68k: the callee's
	                                             * status poll must see the real raster time */
	jsr_tick();                                 /* the jsr itself */
	hle_call68k(ctx, VRAM_FILL_WORDS);          /* jsr (VramFillWords).l (not ported) */
}

/* cmd 3 ($011D90): write one word to VRAM (VramWriteWord). */
static void cmd_write_word(hle_ctx *ctx)
{
	count_cmd(3);
	run_to(ctx, 0x011D90, 0x011D94);            /* prologue (arg reads) as 68k */
	jsr_tick();                                 /* the jsr itself */
	hle_call68k(ctx, VRAM_WRITE_WORD);          /* jsr (VramWriteWord).l (not ported) */
}

/* cmd 4 ($011D9E): 16 CRAM words from src to the CRAM line at `addr`. */
static void cmd_cram_line(hle_ctx *ctx)
{
	count_cmd(4);
	run_to(ctx, 0x011D9E, 0x011DB0);            /* prologue (CRAM address setup) as 68k: the
	                                             * status poll below must see the real raster time */
	poll_status(ctx, 0, 0x011DB0);              /* loc_011DB0 */
	ctx->cycles += 12 + 6 + 10;
	RAM8(VDP_REG01_DMA) |= 0x10;                /* bset.b #4,(vdp_reg01+1).l */
	ctx->cycles += 20;
	vdp_ctrl_w(RAM16(VDP_REG01));               /* move.w (vdp_reg01).l,(4,a5) */
	ctx->cycles += 28;
	vdp_ctrl_w(0x8F02);                         /* move.w #$8f02,(4,a5) */
	ctx->cycles += 12;
	vdp_ctrl_w32(ctx->d[1]);                    /* move.l d1,(4,a5) */
	ctx->cycles += 16;
	ctx->d[0] = 15;                             /* moveq #$f,d0 */
	ctx->cycles += 4;

	uint16_t d0 = 15;
	for (;;) {                                  /* loc_011DD8: poll, move.w (a0)+,(a5), dbra d0 */
		run_to(ctx, 0x011DD8, 0x011DE2);    /* the per-iteration status poll as 68k */
		uint16_t w = bus_r16(ctx->a[0]);
		ctx->a[0] += 2;
		vdp_data_w(w);
		tick(12);                           /* move.w (a0)+,(a5) */
		d0 = (uint16_t)(d0 - 1);
		if (d0 == 0xFFFF) {
			tick(14);
			break;
		}
		tick(10);
	}
	set_w(&ctx->d[0], 0xFFFF);

	RAM8(VDP_REG01_DMA) &= (uint8_t)~0x10;      /* bclr.b #4,(vdp_reg01+1).l */
	ctx->cycles += 20;
	vdp_ctrl_w(RAM16(VDP_REG01));               /* move.w (vdp_reg01).l,(4,a5) */
	ctx->cycles += 28;
	/* final flags from the closing move.w (N/Z from the shadow word, V=C=0) */
	cc_logic16(ctx, RAM16(VDP_REG01));
}

/* cmd 5 ($011D80): fill `bytes` bytes at VRAM addr with the word `value`. */
static void cmd_fill_value(hle_ctx *ctx)
{
	count_cmd(5);
	run_to(ctx, 0x011D80, 0x011D86);            /* prologue (arg reads) as 68k: the callee's
	                                             * status poll must see the real raster time */
	jsr_tick();                                 /* the jsr itself */
	hle_call68k(ctx, VRAM_FILL_WORDS);          /* jsr (VramFillWords).l (not ported) */
}

/* cmd 6 ($0120BA): tilemap rectangle: `cols` words per row, `rows-1` rows, rows $80 bytes
 * apart in VRAM, data from src. */
static void cmd_tile_rect(hle_ctx *ctx)
{
	count_cmd(6);
	run_to(ctx, 0x0120BA, 0x0120C8);            /* prologue (arg reads) as 68k: the poll below
	                                             * must see the real raster time */
	uint16_t cols = ctx->d[4] & 0xFFFF;
	uint16_t addr = ctx->d[3] & 0xFFFF;
	uint16_t rows1 = ctx->d[6] & 0xFFFF;

	poll_status(ctx, 0, 0x0120C8);              /* loc_0120C8 */
	ctx->cycles += 12 + 6 + 10;
	vdp_ctrl_w(0x8F02);                         /* move.w #$8f02,(4,a5) */
	ctx->cycles += 12;
	uint16_t d4 = (uint16_t)(cols - 1);         /* subq.w #1,d4 */
	set_w(&ctx->d[4], d4);
	ctx->cycles += 4;

	uint16_t d3 = ctx->d[3] & 0xFFFF;
	uint16_t d6 = rows1;
	for (;;) {                                  /* loc_0120DA: one row */
		vram_addr(ctx, d3, 1, 2);           /* move.w d3,d2 / move.w d2,d1 / andi / ori / rol / andi */
		ctx->cycles += 4 + 4 + 8 + 8 + 10 + 8;
		vdp_ctrl_w(ctx->d[2] & 0xFFFF);
		vdp_ctrl_w(ctx->d[1] & 0xFFFF);
		ctx->cycles += 12 + 12;
		set_w(&ctx->d[5], d4);              /* move.w d4,d5 */
		ctx->cycles += 4;
		uint16_t d5 = d4;
		for (;;) {                          /* loc_0120F6: cols words */
			uint16_t w = bus_r16(ctx->a[1]);
			ctx->a[1] += 2;
			vdp_data_w(w);
			d5 = (uint16_t)(d5 - 1);
			if (d5 == 0xFFFF) {
				ctx->cycles += 8 + 14;
				break;
			}
			ctx->cycles += 8 + 10;
		}
		set_w(&ctx->d[5], 0xFFFF);
		d3 = add_w(ctx, d3, 0x0080);        /* addi.w #$80,d3 (sets the final flags) */
		set_w(&ctx->d[3], d3);
		ctx->cycles += 8;
		d6 = (uint16_t)(d6 - 1);
		if (d6 == 0xFFFF) {
			ctx->cycles += 14;
			break;
		}
		ctx->cycles += 10;
	}
	set_w(&ctx->d[6], 0xFFFF);
}

/* cmd 7 ($011E48): hscroll: 28 words with source stride $20 and autoincrement $20 for plane A,
 * then the same at +2 for plane B. */
static void cmd_hscroll(hle_ctx *ctx)
{
	count_cmd(7);
	run_to(ctx, 0x011E48, 0x011E54);            /* prologue (arg reads) as 68k: the status read
	                                             * below must see the real raster time */
	uint32_t src = ctx->a[1];
	uint16_t addr = ctx->d[5] & 0xFFFF;

	uint16_t st = vdp_status_word(0x011E54);   /* move.w (4,a5),d0 (single read, no poll) */
	set_w(&ctx->d[0], st);
	ctx->cycles += 12;
	set_w(&ctx->d[2], addr);                    /* move.w d5,d2 (d2 is NOT modified) */
	ctx->cycles += 4;
	vdp_ctrl_w(0x8F20);                         /* move.w #$8f20,(4,a5) */
	ctx->cycles += 12;
	/* move.w d2,d3 / andi.w #$3fff / ori.w #$4000 / move.w d3,(4,a5) */
	vdp_ctrl_w((uint16_t)((addr & 0x3FFF) | 0x4000));
	/* move.w d2,d3 / rol.w #2,d3 / andi.w #3 / move.w d3,(4,a5) */
	set_w(&ctx->d[3], rol_w(ctx, addr, 2) & 3);
	vdp_ctrl_w(ctx->d[3] & 0xFFFF);
	ctx->cycles += 4 + 8 + 8 + 12 + 4 + 10 + 8 + 12;

	/* plane A: 28 words from src, stride $20 */
	uint16_t last = 0;
	for (int i = 0; i < 28; i++) {              /* loc_011E7E */
		last = bus_r16(src + (uint32_t)i * 0x20);
		set_w(&ctx->d[2], last);
		vdp_data_w(last);
		ctx->cycles += 8 + 8 + 8;
	}
	ctx->cycles += 28 * 10 + 14;                /* the dbra of the loop (27 continue + 1 expire) */

	/* plane B: addr + 2, from src + 2 */
	uint16_t addr2 = (uint16_t)(addr + 2);
	set_w(&ctx->d[2], addr2);                   /* move.w d5,d2 / addq.w #2,d2 */
	ctx->cycles += 4 + 4;
	vdp_ctrl_w(0x8F20);
	ctx->cycles += 12;
	vdp_ctrl_w((uint16_t)((addr2 & 0x3FFF) | 0x4000));
	set_w(&ctx->d[3], rol_w(ctx, addr2, 2) & 3);
	vdp_ctrl_w(ctx->d[3] & 0xFFFF);
	ctx->cycles += 4 + 8 + 8 + 12 + 4 + 10 + 8 + 12;
	for (int i = 0; i < 28; i++) {              /* loc_011EB4 */
		last = bus_r16(src + 2 + (uint32_t)i * 0x20);
		set_w(&ctx->d[2], last);
		vdp_data_w(last);
		ctx->cycles += 8 + 8 + 8;
	}
	ctx->cycles += 28 * 10 + 14;
	ctx->d[1] = 0x0000FFFF;                     /* moveq #$1b,d1 (clears the high word) / dbra */
	ctx->a[2] = src + 2 + 28 * 0x20;            /* lea (a1),a2 / lea (a1+2),a2 / 28x lea $20(a2) */
	cc_logic16(ctx, last);                      /* final flags: the last move.w d2,(a5) */
}

/* cmd 8 ($011F6E): VSRAM 0/1 = vscroll A, vscroll B. */
static void cmd_vscroll(hle_ctx *ctx)
{
	count_cmd(8);
	run_to(ctx, 0x011F6E, 0x011F82);            /* prologue (lea + ctrl writes) as 68k: the poll
	                                             * below must see the real raster time */
	poll_status(ctx, 2, 0x011F82);              /* loc_011F82: move.w (4,a5),d2 / btst / bne */
	ctx->cycles += 12 + 6 + 10;
	uint16_t a = RAM16(ctx->a[6]); ctx->a[6] += 2;   /* move.w (a6)+,(a5) */
	vdp_data_w(a);
	ctx->cycles += 8;
	uint16_t b = RAM16(ctx->a[6]); ctx->a[6] += 2;   /* move.w (a6)+,(a5) */
	vdp_data_w(b);
	ctx->cycles += 8;
	cc_logic16(ctx, b);                         /* final flags: the second move.w */
}

/* ---------------------------------------------------------------- registrations */
HLE_FUNC(0x011D64, VdpCmd0End, 0)
{
	cmd_end(ctx);
}

HLE_RANGE(0x012068, VdpCmd1CopyWords, 0, VDP_LIST_NEXT)
{
	cmd_copy_words(ctx);
	hle_jump(ctx, VDP_LIST_NEXT);
}

HLE_RANGE(0x011D70, VdpCmd2FillZero, 0, VDP_LIST_NEXT)
{
	cmd_fill_zero(ctx);
	hle_jump(ctx, VDP_LIST_NEXT);
}

HLE_RANGE(0x011D90, VdpCmd3WriteWord, 0, VDP_LIST_NEXT)
{
	cmd_write_word(ctx);
	hle_jump(ctx, VDP_LIST_NEXT);
}

HLE_RANGE(0x011D9E, VdpCmd4CramLine, 0, VDP_LIST_NEXT)
{
	cmd_cram_line(ctx);
	hle_jump(ctx, VDP_LIST_NEXT);
}

HLE_RANGE(0x011D80, VdpCmd5FillValue, 0, VDP_LIST_NEXT)
{
	cmd_fill_value(ctx);
	hle_jump(ctx, VDP_LIST_NEXT);
}

HLE_RANGE(0x0120BA, VdpCmd6TileRect, 0, VDP_LIST_NEXT)
{
	cmd_tile_rect(ctx);
	hle_jump(ctx, VDP_LIST_NEXT);
}

HLE_RANGE(0x011E48, VdpCmd7HScroll, 0, VDP_LIST_NEXT)
{
	cmd_hscroll(ctx);
	hle_jump(ctx, VDP_LIST_NEXT);
}

HLE_RANGE(0x011F6E, VdpCmd8VScroll, 0, VDP_LIST_NEXT)
{
	cmd_vscroll(ctx);
	hle_jump(ctx, VDP_LIST_NEXT);
}

HLE_FUNC(0x011D32, VdpListRun, 0)
{
	ctx->a[6] = VDP_LIST;                       /* lea.l (vdp_list).l,a6 */
	ctx->cycles += 12;
	for (;;) {
		uint16_t cmd = RAM16(ctx->a[6]);        /* move.w (a6)+,d0 */
		ctx->a[6] += 2;
		set_w(&ctx->d[0], cmd);
		ctx->cycles += 8;
		set_w(&ctx->d[0], asl_w(ctx, cmd, 2));  /* asl.w #2,d0 */
		ctx->cycles += 8 + 12;                  /* asl.w + jmp (d8,pc,d0.w) */
		switch (cmd) {
		case 0: cmd_end(ctx); return;
		case 1: cmd_copy_words(ctx); break;
		case 2: cmd_fill_zero(ctx); break;
		case 3: cmd_write_word(ctx); break;
		case 4: cmd_cram_line(ctx); break;
		case 5: cmd_fill_value(ctx); break;
		case 6: cmd_tile_rect(ctx); break;
		case 7: cmd_hscroll(ctx); break;
		case 8: cmd_vscroll(ctx); break;
		default: count_cmd(-1); return;
		}
	}
}
