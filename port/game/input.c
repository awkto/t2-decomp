/* ReadInput $0003DE, ReadInputAim $0004B6, PadMoveCrosshair $0005FC, QueueCrosshair $00071C:
 * the per-frame input path. docs/gun.md sections 1 and 5; RAM in data/ram.txt.
 *
 * control_mode $FF9DF5 selects the devices (bit0 = two pads, bit2 = one player; value 2 =
 * Menacer only, no menu entry):
 *   mode 2 : Menacer only, no pad, no Y clamp            ($00042A / $00051E)
 *   bit0=1 : two pads (modes 1, 5): swap P1/P2 structs, ReadPad2 then ReadPad1, swap back
 *   bit0=0, not 2 (modes 0, 4): ReadPad1 into P2's struct, Menacer into P1's struct
 * ReadInput reads buttons only; ReadInputAim also moves the crosshair (pad acceleration table
 * $000738 or the Menacer X/Y) and queues the crosshair sprite.
 *
 * The Menacer buttons are remapped into the pad layout by a bit shuffle: raw D0->B (b4),
 * D1 trigger->A (b6), D2->C (b5), D3 Start->b7 (docs/gun.md). In modes 0/4 only the top nibble
 * survives and Start alone drops the rest ($0005C4).
 *
 * Calls: ReadPad1/ReadPad2 (game/pads.c) and MenacerReadXY (game/menacer.c) have C versions,
 * so they are called directly (both are proven equivalent, so the register/flag/RAM result is
 * identical to the jsr). QueueSprite ($013240) is not ported yet: it is reached through
 * hle_call68k so the 68k code runs as the original jsr would (it is a leaf that only touches the
 * sprite queue RAM at $FF9DF6/$FF9DFA).
 *
 * PadMoveCrosshair falls through into QueueCrosshair, so its C version ends by calling the C
 * QueueCrosshair. QueueCrosshair tail-jumps to QueueSprite; the C version ends with the same
 * hle_call68k and the layer performs the caller's rts. */
#include "hle.h"

#include <stdio.h>
#include <stdlib.h>

/* C replacements in other game/ files (pads.c, menacer.c) */
void ReadPad1(hle_ctx *ctx);
void ReadPad2(hle_ctx *ctx);
void MenacerReadXY(hle_ctx *ctx);
void QueueSprite(hle_ctx *ctx);

#define CONTROL_MODE 0xFF9DF5
#define BTN_PREV2    0xFF9F66
#define GUN_X        0xFF9F6E
#define GUN_Y        0xFF9F70
#define GUN_BTN      0xFF9F72
#define P1_BASE      0xFFFF8000
#define P2_BASE      0xFFFF8050
#define PAD_ACCEL    0x000738       /* data/symbols.txt: 10 words 1,1,1,2,2,3,4,5,6,8 px/frame */
#define QUEUE_SPRITE 0x013240

/* ------------------------------------------------------------------ 68k flag helpers */
/* MOVE / NOT / AND / OR / TST: N,Z from the result, V=C=0, X unchanged. */
static void f_nz(hle_ctx *ctx, uint32_t v, int size)
{
	uint32_t sign = size == 8 ? 0x80u : size == 16 ? 0x8000u : 0x80000000u;
	uint32_t mask = size == 8 ? 0xFFu : size == 16 ? 0xFFFFu : 0xFFFFFFFFu;
	cc_set(ctx, CC_N | CC_Z | CC_V | CC_C,
	       ((v & sign) ? CC_N : 0) | ((v & mask) ? 0 : CC_Z));
}

/* LSL.W #n,Dx (1<=n<=8): X=C=last bit out, N/Z from result, V=0 */
static uint32_t lsl_w(hle_ctx *ctx, uint32_t v, int n)
{
	uint32_t w = (v & 0xFFFF) << n;
	uint32_t r = w & 0xFFFF;
	unsigned cbit = ((w >> 16) & 1) ? (CC_C | CC_X) : 0;
	cc_set(ctx, CC_X | CC_N | CC_Z | CC_V | CC_C,
	       cbit | ((r & 0x8000) ? CC_N : 0) | (r ? 0 : CC_Z));
	return r;
}

/* ADD.W / SUB.W: N/Z/V/C from the result, X=C (carry / borrow) */
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

static uint16_t sub_w(hle_ctx *ctx, uint16_t a, uint16_t b)
{
	uint32_t r = (uint32_t)a - b;
	uint16_t rr = (uint16_t)r;
	unsigned borrow = (r >> 16) & 1;
	unsigned ov = (((a ^ b) & (a ^ rr)) >> 15) & 1;
	cc_set(ctx, CC_X | CC_N | CC_Z | CC_V | CC_C,
	       (borrow ? (CC_X | CC_C) : 0) | ((rr & 0x8000) ? CC_N : 0) | (rr ? 0 : CC_Z) |
	       (ov ? CC_V : 0));
	return rr;
}

/* ------------------------------------------------------------------ Menacer button remap */
/* d0 = ~gun_btn, then the bit shuffle; leaves d1/d2 set and the lsl.w #4 X flag. Shared by all
 * four Menacer mappings ($00042A, $00051E, $000464, $00056E). */
static void menacer_btn_map(hle_ctx *ctx)
{
	/* move.b (abs).l,d0 16, not.b 4, move.w d0,d1/d2 4+4, andi.w #2/#4 8+8, lsr.w #1 8, lsl.w #1 8,
	 * andi.w #9 8, or.w 4+4, lsl.w #4 14 */
	HLE_SEQ(ctx, 16, 4, 4, 4, 8, 8, 8, 8, 8, 4, 4, 14);
	set_b(&ctx->d[0], (uint8_t)~RAM8(GUN_BTN));      /* move.b (gun_btn).l,d0 / not.b d0 */
	f_nz(ctx, ctx->d[0] & 0xFF, 8);
	set_w(&ctx->d[1], ctx->d[0]);                    /* move.w d0,d1 */
	f_nz(ctx, ctx->d[0] & 0xFFFF, 16);
	set_w(&ctx->d[2], ctx->d[0]);                    /* move.w d0,d2 */
	f_nz(ctx, ctx->d[0] & 0xFFFF, 16);
	set_w(&ctx->d[1], ctx->d[1] & 0x2);              /* andi.w #$2,d1 */
	f_nz(ctx, ctx->d[1] & 0xFFFF, 16);
	set_w(&ctx->d[2], ctx->d[2] & 0x4);              /* andi.w #$4,d2 */
	f_nz(ctx, ctx->d[2] & 0xFFFF, 16);
	set_w(&ctx->d[2], lsr16(ctx, ctx->d[2] & 0xFFFF, 1));   /* lsr.w #1,d2 */
	set_w(&ctx->d[1], lsl_w(ctx, ctx->d[1] & 0xFFFF, 1));   /* lsl.w #1,d1 */
	set_w(&ctx->d[0], ctx->d[0] & 0x9);              /* andi.w #$9,d0 */
	f_nz(ctx, ctx->d[0] & 0xFFFF, 16);
	set_w(&ctx->d[0], ctx->d[0] | ctx->d[1]);        /* or.w d1,d0 */
	f_nz(ctx, ctx->d[0] & 0xFFFF, 16);
	set_w(&ctx->d[0], ctx->d[0] | ctx->d[2]);        /* or.w d2,d0 */
	f_nz(ctx, ctx->d[0] & 0xFFFF, 16);
	set_w(&ctx->d[0], lsl_w(ctx, ctx->d[0] & 0xFFFF, 4));   /* lsl.w #4,d0 */
}

/* ------------------------------------------------------------------ stats (T2_INPUT_STATS) */
static unsigned long g_ri_mode2, g_ri_twopad, g_ri_mode04;
static unsigned long g_ria_mode2, g_ria_twopad, g_ria_mode04;
static unsigned long g_pad_up, g_pad_down, g_pad_left, g_pad_right, g_pad_none;
static unsigned long g_qc_skip, g_qc_queue;
static int g_stats = -1;

static void input_stats_print(void)
{
	if (g_stats != 1)
		return;
	fprintf(stderr,
	        "ReadInput paths: mode2=%lu twopad=%lu mode04=%lu\n"
	        "ReadInputAim paths: mode2=%lu twopad=%lu mode04=%lu\n"
	        "PadMoveCrosshair dirs: up=%lu down=%lu left=%lu right=%lu none=%lu\n"
	        "QueueCrosshair: queued=%lu skipped(index<0)=%lu\n",
	        g_ri_mode2, g_ri_twopad, g_ri_mode04,
	        g_ria_mode2, g_ria_twopad, g_ria_mode04,
	        g_pad_up, g_pad_down, g_pad_left, g_pad_right, g_pad_none,
	        g_qc_queue, g_qc_skip);
}

static int input_stats_on(void)
{
	if (g_stats < 0) {
		g_stats = getenv("T2_INPUT_STATS") != NULL;
		if (g_stats)
			atexit(input_stats_print);
	}
	return g_stats == 1;
}

/* ------------------------------------------------------------------ QueueCrosshair $00071C */
HLE_FUNC_PURE(0x00071C, QueueCrosshair, 320)
{
	/* interrupt-exact (docs/porting.md, "Yielding"): each instruction reads and writes first, then is charged */
	uint32_t a4 = ctx->a[4];
	uint16_t x = RAM16(a4 + 4);
	set_w(&ctx->d[2], x);                    /* move.w (0x4,a4),d2 */
	f_nz(ctx, x, 16);
	hle_insn(ctx, 12);
	uint16_t y = RAM16(a4 + 6);
	set_w(&ctx->d[3], y);                    /* move.w (0x6,a4),d3 */
	f_nz(ctx, y, 16);
	hle_insn(ctx, 12);
	uint16_t idx = RAM16(a4 + 0x24);
	set_w(&ctx->d[1], idx);                  /* move.w (0x24,a4),d1 */
	f_nz(ctx, idx, 16);
	hle_insn(ctx, 12);
	if (idx & 0x8000) {                      /* bmi.w loc_000736: not playing, no sprite */
		if (input_stats_on())
			g_qc_skip++;
		HLE_SEQ(ctx, 10, 16);            /* bmi.w taken, rts */
		return;
	}
	hle_insn(ctx, 12);                       /* bmi.w not taken */
	if (input_stats_on())
		g_qc_queue++;
	uint16_t d1 = add_w(ctx, idx, 1);        /* addq.w #1,d1 */
	set_w(&ctx->d[1], d1);
	set_w(&ctx->d[5], d1);                   /* move.w d1,d5 */
	f_nz(ctx, d1, 16);
	HLE_SEQ(ctx, 4, 4, 20);                  /* addq.w #1,d1 / move.w d1,d5 / jsr (QueueSprite).l */
	QueueSprite(ctx);                        /* jsr (QueueSprite).l: the C version (proven equal), pure */
	hle_insn(ctx, 16);                       /* loc_000736: rts */
}

/* ------------------------------------------------------------------ PadMoveCrosshair $0005FC */
/* One held direction: advance its accel index at `accel`, look the step up in the table and
 * return it (d3). $FF8008/0A/0C/0E are shared by both players, as in the original. */
static uint16_t pad_step(hle_ctx *ctx, uint16_t prev, uint16_t bit, uint32_t accel)
{
	uint16_t d2 = RAM16(accel);
	set_w(&ctx->d[2], d2);                   /* move.w (accel).l,d2 */
	f_nz(ctx, d2, 16);
	HLE_SEQ(ctx, 16, 10);                    /* move.w (abs).l,d2 / btst.b #n,d1 */
	if (!(prev & bit)) {
		HLE_SEQ(ctx, 12, 8);             /* bne.w not taken / move.w #$fffe,d2 */
		d2 = 0xFFFE;                     /* move.w #$fffe,d2 (first frame of the hold) */
	} else {
		hle_insn(ctx, 10);               /* bne.w taken */
	}
	HLE_SEQ(ctx, 4, 8);                      /* addq.w #2,d2 / cmpi.w #$14,d2 */
	d2 = add_w(ctx, d2, 2);                  /* addq.w #2,d2 */
	set_w(&ctx->d[2], d2);
	if (d2 == 0x14) {                        /* cmpi.w #$14 / bne / subq.w #2 */
		HLE_SEQ(ctx, 12, 4);             /* bne.w not taken / subq.w #2,d2 */
		d2 = sub_w(ctx, d2, 2);
		set_w(&ctx->d[2], d2);
	} else {
		hle_insn(ctx, 10);               /* bne.w taken */
	}
	RAM16(accel) = d2;                       /* move.w d2,(accel).l */
	f_nz(ctx, d2, 16);
	hle_insn(ctx, 16);
	uint16_t d3 = ROM16(PAD_ACCEL + (int16_t)d2);   /* move.w (0,a3,d2.w),d3 */
	set_w(&ctx->d[3], d3);
	f_nz(ctx, d3, 16);
	hle_insn(ctx, 14);
	return d3;
}

/* One axis move after pad_step: d2 = coordinate +/- d3, clamped. `lim_hi`: the clamp is an upper
 * one (cmpi.w #lim,d2 / ble.s: 8 + 10 taken / 8 not taken + move.w #lim,d2 8); otherwise a lower
 * one (cmpi / bge.s). Charges move.w (d16,a4),d2 12, sub/add.w 4, cmpi 8, the branch, the
 * optional move.w #imm,d2 8 and move.w d2,(d16,a4) 12. */
static void pad_axis(hle_ctx *ctx, uint32_t addr, int d3, bool add, int lim, bool lim_hi)
{
	uint16_t d2 = RAM16(addr);
	set_w(&ctx->d[2], d2);                   /* move.w (d,a4),d2 */
	f_nz(ctx, d2, 16);
	d2 = add ? add_w(ctx, d2, d3) : sub_w(ctx, d2, d3);
	set_w(&ctx->d[2], d2);
	HLE_SEQ(ctx, 12, 4, 8);                  /* move.w (d,a4),d2 / sub.w|add.w d3,d2 / cmpi.w #lim,d2 */
	bool clamp = lim_hi ? (int16_t)d2 > lim : (int16_t)d2 < lim;
	if (clamp) {
		HLE_SEQ(ctx, 8, 8);              /* ble.s|bge.s not taken / move.w #lim,d2 */
		d2 = (uint16_t)lim;
	} else {
		hle_insn(ctx, 10);               /* taken */
	}
	RAM16(addr) = d2;                        /* move.w d2,(d,a4) */
	hle_insn(ctx, 12);
	f_nz(ctx, d2, 16);
}

HLE_FUNC_PURE(0x0005FC, PadMoveCrosshair, 640)
{
	uint32_t a4 = ctx->a[4];
	uint16_t buttons = RAM16(a4);
	set_w(&ctx->d[0], buttons);              /* move.w $0(a4),d0 */
	f_nz(ctx, buttons, 16);
	hle_insn(ctx, 12);
	uint16_t prev = RAM16(a4 + 2);
	set_w(&ctx->d[1], prev);                 /* move.w (0x2,a4),d1 */
	f_nz(ctx, prev, 16);
	HLE_SEQ(ctx, 12, 10);                    /* btst.b #0,d0 */

	bool up = buttons & 1, down = buttons & 2, left = buttons & 4, right = buttons & 8;
	if (input_stats_on()) {
		if (up) g_pad_up++; else if (down) g_pad_down++;
		if (left) g_pad_left++; else if (right) g_pad_right++;
		if (!up && !down && !left && !right) g_pad_none++;
	}

	if (up) {
		/* loc_0005FC up: Y -= table[d2], clamp Y >= $18 */
		hle_insn(ctx, 12);               /* beq.w not taken */
		uint16_t d3 = pad_step(ctx, prev, 1, 0xFF8008);
		pad_axis(ctx, a4 + 6, d3, false, 0x18, false);
		hle_insn(ctx, 10);               /* bra.w loc_000690 */
	} else {
		hle_insn(ctx, 10);               /* beq.w loc_00064C taken */
		hle_insn(ctx, 10);               /* btst.b #1,d0 */
		if (down) {
			/* loc_00064C down: Y += table[d2], clamp Y <= $c8 */
			hle_insn(ctx, 12);       /* beq.w not taken */
			uint16_t d3 = pad_step(ctx, prev, 2, 0xFF800A);
			pad_axis(ctx, a4 + 6, d3, true, 0xC8, true);
		} else {
			hle_insn(ctx, 10);       /* beq.w loc_000690 taken */
		}
	}

	hle_insn(ctx, 10);                       /* loc_000690: btst.b #2,d0 */
	if (left) {
		/* loc_000690 left: X -= table[d2], clamp X >= $8, then QueueCrosshair */
		hle_insn(ctx, 12);               /* beq.w not taken */
		uint16_t d3 = pad_step(ctx, prev, 4, 0xFF800C);
		pad_axis(ctx, a4 + 4, d3, false, 0x8, false);
		hle_insn(ctx, 10);               /* bra.w QueueCrosshair */
		QueueCrosshair(ctx);
		return;
	}
	hle_insn(ctx, 10);                       /* beq.w loc_0006D8 taken */
	hle_insn(ctx, 10);                       /* loc_0006D8: btst.b #3,d0 */
	if (right) {
		/* loc_0006D8 right: X += table[d2], clamp X <= $138 */
		hle_insn(ctx, 12);               /* beq.w not taken */
		uint16_t d3 = pad_step(ctx, prev, 8, 0xFF800E);
		pad_axis(ctx, a4 + 4, d3, true, 0x138, true);
	} else {
		hle_insn(ctx, 10);               /* beq.w QueueCrosshair taken */
	}
	QueueCrosshair(ctx);
}

/* ------------------------------------------------------------------ ReadInput $0003DE */
/* The two-pad swap: save P1's long, P1 = P2, P2 = saved, ReadPad2 -> P1, ReadPad1 -> P2, then
 * swap back so pad 1 lands in P1 ($FF8000) and pad 2 in P2 ($FF8050). */
static void read_input_two_pad(hle_ctx *ctx)
{
	uint32_t d0 = RAM32(P1_BASE);            /* move.l (p1).l,d0 */
	RAM32_W(P1_BASE, RAM32(P2_BASE));        /* move.l (p2).l,(p1).l */
	RAM32_W(P2_BASE, d0);                    /* move.l d0,(p2).l */
	f_nz(ctx, d0, 32);
	HLE_SEQ(ctx, 20, 36, 20, 20);            /* move.l (abs).l,d0 / (abs).l,(abs).l / d0,(abs).l / jsr ReadPad2 */
	ReadPad2(ctx);
	hle_insn(ctx, 20);                       /* jsr (ReadPad1).l */
	ReadPad1(ctx);
	uint32_t e = RAM32(P1_BASE);             /* move.l (p1).l,d0 */
	ctx->d[0] = e;
	f_nz(ctx, e, 32);
	RAM32_W(P1_BASE, RAM32(P2_BASE));        /* move.l (p2).l,(p1).l */
	RAM32_W(P2_BASE, e);                     /* move.l d0,(p2).l */
	f_nz(ctx, e, 32);
	HLE_SEQ(ctx, 20, 36, 20);                /* the swap back (the caller adds the rts) */
}

HLE_FUNC_LOWMASK(0x0003DE, ReadInput, 1713)
{
	uint8_t mode = RAM8(CONTROL_MODE);
	set_b(&ctx->d[0], mode);                 /* move.b (control_mode).l,d0 */
	f_nz(ctx, mode, 8);
	HLE_SEQ(ctx, 16, 8);                     /* move.b (abs).l,d0 / cmpi.b #2,d0 */
	if (mode == 0x02) {                      /* loc_00042A: Menacer only */
		if (input_stats_on()) g_ri_mode2++;
		HLE_SEQ(ctx, 10, 20);            /* beq.s taken / jsr (MenacerReadXY).l */
		MenacerReadXY(ctx);
		ctx->a[4] = P1_BASE;             /* lea.l (0xffff8000).l,a4 */
		HLE_SEQ(ctx, 12, 20);            /* lea.l (abs).l,a4 / move.w $0(a4),$2(a4) */
		uint16_t cur = RAM16(P1_BASE);
		RAM16(P1_BASE + 2) = cur;        /* move.w $0(a4),$2(a4) */
		f_nz(ctx, cur, 16);
		menacer_btn_map(ctx);
		RAM16(P1_BASE) = ctx->d[0] & 0xFFFF;   /* move.w d0,$0(a4) */
		f_nz(ctx, ctx->d[0] & 0xFFFF, 16);
		HLE_SEQ(ctx, 12, 16);            /* move.w d0,$0(a4) / rts */
		return;
	}
	HLE_SEQ(ctx, 8, 10);                     /* beq.s not taken / btst.b #0,d0 */
	if (mode & 0x01) {                       /* modes 1/5: two pads */
		if (input_stats_on()) g_ri_twopad++;
		hle_insn(ctx, 8);                /* beq.s not taken */
		read_input_two_pad(ctx);
		hle_insn(ctx, 16);               /* rts */
		return;
	}
	if (input_stats_on()) g_ri_mode04++;
	/* loc_000464: ReadPad1 -> P2, Menacer -> P1, keep only the top nibble of the buttons */
	hle_insn(ctx, 10);                       /* beq.s taken */
	hle_insn(ctx, 20);                       /* jsr (ReadPad1).l */
	ReadPad1(ctx);
	ctx->a[4] = P2_BASE;
	HLE_SEQ(ctx, 12, 20);                    /* lea.l (abs).l,a4 / jsr (MenacerReadXY).l */
	MenacerReadXY(ctx);
	ctx->a[4] = P1_BASE;                     /* lea.l (0xffff8000).l,a4 */
	HLE_SEQ(ctx, 12, 24, 20);                /* lea / move.w (2,a4),(abs).l / move.w $0(a4),$2(a4) */
	uint16_t prev = RAM16(P1_BASE + 2);
	RAM16(BTN_PREV2) = prev;                 /* move.w (0x2,a4),(btn_prev2).l */
	f_nz(ctx, prev, 16);
	uint16_t cur = RAM16(P1_BASE);
	RAM16(P1_BASE + 2) = cur;                /* move.w $0(a4),$2(a4) */
	f_nz(ctx, cur, 16);
	menacer_btn_map(ctx);
	set_w(&ctx->d[0], ctx->d[0] & 0xF0);     /* andi.w #$f0,d0 */
	f_nz(ctx, ctx->d[0] & 0xFFFF, 16);
	RAM16(P1_BASE) = ctx->d[0] & 0xFFFF;     /* move.w d0,$0(a4) */
	f_nz(ctx, ctx->d[0] & 0xFFFF, 16);
	HLE_SEQ(ctx, 8, 12, 16);                 /* andi.w #$f0,d0 / move.w d0,$0(a4) / rts */
}

/* ------------------------------------------------------------------ ReadInputAim $0004B6 */
HLE_FUNC_LOWMASK(0x0004B6, ReadInputAim, 2070)
{
	uint8_t mode = RAM8(CONTROL_MODE);
	set_b(&ctx->d[0], mode);                 /* move.b (control_mode).l,d0 */
	f_nz(ctx, mode, 8);
	HLE_SEQ(ctx, 16, 8);                     /* move.b (abs).l,d0 / cmpi.b #2,d0 */
	if (mode == 0x02) {                      /* loc_00051E: Menacer only, no Y clamp */
		if (input_stats_on()) g_ria_mode2++;
		HLE_SEQ(ctx, 10, 20);            /* beq.s taken / jsr (MenacerReadXY).l */
		MenacerReadXY(ctx);
		ctx->a[4] = P1_BASE;
		HLE_SEQ(ctx, 12, 20);            /* lea.l (abs).l,a4 / move.w $0(a4),$2(a4) */
		uint16_t cur = RAM16(P1_BASE);
		RAM16(P1_BASE + 2) = cur;        /* move.w $0(a4),$2(a4) */
		f_nz(ctx, cur, 16);
		menacer_btn_map(ctx);
		RAM16(P1_BASE) = ctx->d[0] & 0xFFFF;
		f_nz(ctx, ctx->d[0] & 0xFFFF, 16);
		uint16_t gx = RAM16(GUN_X);      /* move.w (gun_x).l,d0 */
		set_w(&ctx->d[0], gx);
		f_nz(ctx, gx, 16);
		RAM16(P1_BASE + 4) = gx;         /* move.w d0,(0x4,a4) */
		f_nz(ctx, gx, 16);
		uint16_t gy = RAM16(GUN_Y);      /* move.w (gun_y).l,d0 */
		set_w(&ctx->d[0], gy);
		f_nz(ctx, gy, 16);
		RAM16(P1_BASE + 6) = gy;         /* move.w d0,(0x6,a4) */
		f_nz(ctx, gy, 16);
		/* move.w d0,$0(a4) 12, move.w (abs).l,d0 16, move.w d0,(4,a4) 12, move.w (abs).l,d0 16,
		 * move.w d0,(6,a4) 12, bra.w QueueCrosshair 10 */
		HLE_SEQ(ctx, 12, 16, 12, 16, 12, 10);
		QueueCrosshair(ctx);             /* bra.w QueueCrosshair */
		return;
	}
	HLE_SEQ(ctx, 8, 10);                     /* beq.s not taken / btst.b #0,d0 */
	if (mode & 0x01) {                       /* modes 1/5: two pads + both crosshairs */
		if (input_stats_on()) g_ria_twopad++;
		hle_insn(ctx, 12);               /* beq.w not taken */
		read_input_two_pad(ctx);
		ctx->a[3] = PAD_ACCEL;           /* lea.l (PadAccelTable).l,a3 */
		ctx->a[4] = P1_BASE;
		HLE_SEQ(ctx, 12, 12, 18);        /* lea a3 / lea a4 / bsr.w PadMoveCrosshair */
		PadMoveCrosshair(ctx);           /* bsr.w PadMoveCrosshair */
		ctx->a[4] = P2_BASE;
		HLE_SEQ(ctx, 12, 18);            /* lea a4 / bsr.w PadMoveCrosshair */
		PadMoveCrosshair(ctx);           /* bsr.w PadMoveCrosshair */
		hle_insn(ctx, 16);               /* rts */
		return;
	}
	if (input_stats_on()) g_ria_mode04++;
	/* loc_00056E: pad crosshair on P2, Menacer on P1, Y clamped 24..200 */
	HLE_SEQ(ctx, 10, 20);                    /* beq.w taken / jsr (ReadPad1).l */
	ReadPad1(ctx);
	ctx->a[3] = PAD_ACCEL;
	ctx->a[4] = P2_BASE;
	HLE_SEQ(ctx, 12, 12, 18);                /* lea a3 / lea a4 / bsr.w PadMoveCrosshair */
	PadMoveCrosshair(ctx);                   /* bsr.w PadMoveCrosshair */
	hle_insn(ctx, 20);                       /* jsr (MenacerReadXY).l */
	MenacerReadXY(ctx);
	ctx->a[4] = P1_BASE;
	HLE_SEQ(ctx, 12, 24, 20);                /* lea / move.w (2,a4),(abs).l / move.w $0(a4),$2(a4) */
	uint16_t prev = RAM16(P1_BASE + 2);
	RAM16(BTN_PREV2) = prev;
	f_nz(ctx, prev, 16);
	uint16_t cur = RAM16(P1_BASE);
	RAM16(P1_BASE + 2) = cur;
	f_nz(ctx, cur, 16);
	menacer_btn_map(ctx);
	set_w(&ctx->d[0], ctx->d[0] & 0xF0);     /* andi.w #$f0,d0 */
	f_nz(ctx, ctx->d[0] & 0xFFFF, 16);
	f_nz(ctx, ctx->d[0] & 0xFF, 8);          /* tst.b d0 */
	HLE_SEQ(ctx, 8, 4);                      /* andi.w #$f0,d0 / tst.b d0 */
	if (ctx->d[0] & 0x80) {                  /* bpl.s loc_0005CC (skip if positive) */
		HLE_SEQ(ctx, 8, 8);              /* bpl.s not taken / andi.b #$80,d0 */
		set_w(&ctx->d[0], ctx->d[0] & 0x80);     /* andi.b #$80,d0 */
		f_nz(ctx, ctx->d[0] & 0xFF, 8);
	} else {
		hle_insn(ctx, 10);               /* bpl.s taken */
	}
	RAM16(P1_BASE) = ctx->d[0] & 0xFFFF;     /* move.w d0,$0(a4) */
	f_nz(ctx, ctx->d[0] & 0xFFFF, 16);
	uint16_t gx = RAM16(GUN_X);
	set_w(&ctx->d[0], gx);                   /* move.w (gun_x).l,d0 */
	f_nz(ctx, gx, 16);
	RAM16(P1_BASE + 4) = gx;                 /* move.w d0,(0x4,a4) */
	f_nz(ctx, gx, 16);
	uint16_t gy = RAM16(GUN_Y);
	set_w(&ctx->d[2], gy);                   /* move.w (gun_y).l,d2 */
	f_nz(ctx, gy, 16);
	HLE_SEQ(ctx, 12, 16, 12, 16, 8);         /* move.w d0,$0(a4) / (gun_x),d0 / d0,(4,a4) / (gun_y),d2 / cmpi.w #$c8,d2 */
	int16_t sy = (int16_t)gy;
	if (sy > 0xC8) {
		HLE_SEQ(ctx, 8, 8);              /* ble.s not taken / move.w #$c8,d2 */
		sy = 0xC8;                       /* cmpi.w #$c8 / ble / move.w #$c8 */
	} else {
		hle_insn(ctx, 10);               /* ble.s taken */
	}
	hle_insn(ctx, 8);                        /* cmpi.w #$18,d2 */
	if (sy < 0x18) {
		HLE_SEQ(ctx, 8, 8);              /* bge.s not taken / move.w #$18,d2 */
		sy = 0x18;                       /* cmpi.w #$18 / bge / move.w #$18 */
	} else {
		hle_insn(ctx, 10);               /* bge.s taken */
	}
	gy = (uint16_t)sy;
	set_w(&ctx->d[2], gy);
	RAM16(P1_BASE + 6) = gy;                 /* move.w d2,(0x6,a4) */
	f_nz(ctx, gy, 16);
	HLE_SEQ(ctx, 12, 10);                    /* move.w d2,(6,a4) / bra.w QueueCrosshair */
	QueueCrosshair(ctx);                     /* bra.w QueueCrosshair */
}
