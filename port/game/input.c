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
HLE_FUNC(0x00071C, QueueCrosshair, 164)
{
	uint32_t a4 = ctx->a[4];
	uint16_t x = RAM16(a4 + 4);
	set_w(&ctx->d[2], x);                    /* move.w (0x4,a4),d2 */
	f_nz(ctx, x, 16);
	uint16_t y = RAM16(a4 + 6);
	set_w(&ctx->d[3], y);                    /* move.w (0x6,a4),d3 */
	f_nz(ctx, y, 16);
	uint16_t idx = RAM16(a4 + 0x24);
	set_w(&ctx->d[1], idx);                  /* move.w (0x24,a4),d1 */
	f_nz(ctx, idx, 16);
	if (idx & 0x8000) {                      /* bmi.w loc_000736: not playing, no sprite */
		if (input_stats_on())
			g_qc_skip++;
		return;
	}
	if (input_stats_on())
		g_qc_queue++;
	uint16_t d1 = add_w(ctx, idx, 1);        /* addq.w #1,d1 */
	set_w(&ctx->d[1], d1);
	set_w(&ctx->d[5], d1);                   /* move.w d1,d5 */
	f_nz(ctx, d1, 16);
	hle_call68k(ctx, QUEUE_SPRITE);          /* jsr (QueueSprite).l: 68k, not ported yet */
}

/* ------------------------------------------------------------------ PadMoveCrosshair $0005FC */
/* One held direction: advance its accel index at `accel`, look the step up in the table and
 * return it (d3). $FF8008/0A/0C/0E are shared by both players, as in the original. */
static uint16_t pad_step(hle_ctx *ctx, uint16_t prev, uint16_t bit, uint32_t accel)
{
	uint16_t d2 = RAM16(accel);
	set_w(&ctx->d[2], d2);                   /* move.w (accel).l,d2 */
	f_nz(ctx, d2, 16);
	if (!(prev & bit))
		d2 = 0xFFFE;                     /* move.w #$fffe,d2 (first frame of the hold) */
	d2 = add_w(ctx, d2, 2);                  /* addq.w #2,d2 */
	set_w(&ctx->d[2], d2);
	if (d2 == 0x14) {                        /* cmpi.w #$14 / bne / subq.w #2 */
		d2 = sub_w(ctx, d2, 2);
		set_w(&ctx->d[2], d2);
	}
	RAM16(accel) = d2;                       /* move.w d2,(accel).l */
	f_nz(ctx, d2, 16);
	uint16_t d3 = ROM16(PAD_ACCEL + (int16_t)d2);   /* move.w (0,a3,d2.w),d3 */
	set_w(&ctx->d[3], d3);
	f_nz(ctx, d3, 16);
	return d3;
}

HLE_FUNC(0x0005FC, PadMoveCrosshair, 397)
{
	uint32_t a4 = ctx->a[4];
	uint16_t buttons = RAM16(a4);
	set_w(&ctx->d[0], buttons);              /* move.w $0(a4),d0 */
	f_nz(ctx, buttons, 16);
	uint16_t prev = RAM16(a4 + 2);
	set_w(&ctx->d[1], prev);                 /* move.w (0x2,a4),d1 */
	f_nz(ctx, prev, 16);

	bool up = buttons & 1, down = buttons & 2, left = buttons & 4, right = buttons & 8;
	if (input_stats_on()) {
		if (up) g_pad_up++; else if (down) g_pad_down++;
		if (left) g_pad_left++; else if (right) g_pad_right++;
		if (!up && !down && !left && !right) g_pad_none++;
	}

	if (up) {
		/* loc_0005FC up: Y -= table[d2], clamp Y >= $18 */
		uint16_t d3 = pad_step(ctx, prev, 1, 0xFF8008);
		uint16_t d2 = RAM16(a4 + 6);
		set_w(&ctx->d[2], d2);           /* move.w (0x6,a4),d2 */
		f_nz(ctx, d2, 16);
		d2 = sub_w(ctx, d2, d3);         /* sub.w d3,d2 */
		set_w(&ctx->d[2], d2);
		if ((int16_t)d2 < 0x18) d2 = 0x18;   /* cmpi.w #$18 / bge / move.w #$18 */
		RAM16(a4 + 6) = d2;              /* move.w d2,(0x6,a4) */
		f_nz(ctx, d2, 16);
	} else if (down) {
		/* loc_00064C down: Y += table[d2], clamp Y <= $c8 */
		uint16_t d3 = pad_step(ctx, prev, 2, 0xFF800A);
		uint16_t d2 = RAM16(a4 + 6);
		set_w(&ctx->d[2], d2);
		f_nz(ctx, d2, 16);
		d2 = add_w(ctx, d2, d3);         /* add.w d3,d2 */
		set_w(&ctx->d[2], d2);
		if ((int16_t)d2 > 0xC8) d2 = 0xC8;   /* cmpi.w #$c8 / ble / move.w #$c8 */
		RAM16(a4 + 6) = d2;
		f_nz(ctx, d2, 16);
	}

	if (left) {
		/* loc_000690 left: X -= table[d2], clamp X >= $8, then QueueCrosshair */
		uint16_t d3 = pad_step(ctx, prev, 4, 0xFF800C);
		uint16_t d2 = RAM16(a4 + 4);
		set_w(&ctx->d[2], d2);
		f_nz(ctx, d2, 16);
		d2 = sub_w(ctx, d2, d3);
		set_w(&ctx->d[2], d2);
		if ((int16_t)d2 < 0x8) d2 = 0x8;
		RAM16(a4 + 4) = d2;
		f_nz(ctx, d2, 16);
		QueueCrosshair(ctx);
		return;
	}
	if (right) {
		/* loc_0006D8 right: X += table[d2], clamp X <= $138 */
		uint16_t d3 = pad_step(ctx, prev, 8, 0xFF800E);
		uint16_t d2 = RAM16(a4 + 4);
		set_w(&ctx->d[2], d2);
		f_nz(ctx, d2, 16);
		d2 = add_w(ctx, d2, d3);
		set_w(&ctx->d[2], d2);
		if ((int16_t)d2 > 0x138) d2 = 0x138;
		RAM16(a4 + 4) = d2;
		f_nz(ctx, d2, 16);
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
	ReadPad2(ctx);
	ReadPad1(ctx);
	uint32_t e = RAM32(P1_BASE);             /* move.l (p1).l,d0 */
	ctx->d[0] = e;
	f_nz(ctx, e, 32);
	RAM32_W(P1_BASE, RAM32(P2_BASE));        /* move.l (p2).l,(p1).l */
	RAM32_W(P2_BASE, e);                     /* move.l d0,(p2).l */
	f_nz(ctx, e, 32);
}

HLE_FUNC(0x0003DE, ReadInput, 1713)
{
	uint8_t mode = RAM8(CONTROL_MODE);
	set_b(&ctx->d[0], mode);                 /* move.b (control_mode).l,d0 */
	f_nz(ctx, mode, 8);
	if (mode == 0x02) {                      /* loc_00042A: Menacer only */
		if (input_stats_on()) g_ri_mode2++;
		MenacerReadXY(ctx);
		ctx->a[4] = P1_BASE;             /* lea.l (0xffff8000).l,a4 */
		uint16_t cur = RAM16(P1_BASE);
		RAM16(P1_BASE + 2) = cur;        /* move.w $0(a4),$2(a4) */
		f_nz(ctx, cur, 16);
		menacer_btn_map(ctx);
		RAM16(P1_BASE) = ctx->d[0] & 0xFFFF;   /* move.w d0,$0(a4) */
		f_nz(ctx, ctx->d[0] & 0xFFFF, 16);
		return;
	}
	if (mode & 0x01) {                       /* modes 1/5: two pads */
		if (input_stats_on()) g_ri_twopad++;
		read_input_two_pad(ctx);
		return;
	}
	if (input_stats_on()) g_ri_mode04++;
	/* loc_000464: ReadPad1 -> P2, Menacer -> P1, keep only the top nibble of the buttons */
	ReadPad1(ctx);
	ctx->a[4] = P2_BASE;
	MenacerReadXY(ctx);
	ctx->a[4] = P1_BASE;                     /* lea.l (0xffff8000).l,a4 */
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
}

/* ------------------------------------------------------------------ ReadInputAim $0004B6 */
HLE_FUNC(0x0004B6, ReadInputAim, 2070)
{
	uint8_t mode = RAM8(CONTROL_MODE);
	set_b(&ctx->d[0], mode);                 /* move.b (control_mode).l,d0 */
	f_nz(ctx, mode, 8);
	if (mode == 0x02) {                      /* loc_00051E: Menacer only, no Y clamp */
		if (input_stats_on()) g_ria_mode2++;
		MenacerReadXY(ctx);
		ctx->a[4] = P1_BASE;
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
		QueueCrosshair(ctx);             /* bra.w QueueCrosshair */
		return;
	}
	if (mode & 0x01) {                       /* modes 1/5: two pads + both crosshairs */
		if (input_stats_on()) g_ria_twopad++;
		read_input_two_pad(ctx);
		ctx->a[3] = PAD_ACCEL;           /* lea.l (PadAccelTable).l,a3 */
		ctx->a[4] = P1_BASE;
		PadMoveCrosshair(ctx);           /* bsr.w PadMoveCrosshair */
		ctx->a[4] = P2_BASE;
		PadMoveCrosshair(ctx);           /* bsr.w PadMoveCrosshair */
		return;
	}
	if (input_stats_on()) g_ria_mode04++;
	/* loc_00056E: pad crosshair on P2, Menacer on P1, Y clamped 24..200 */
	ReadPad1(ctx);
	ctx->a[3] = PAD_ACCEL;
	ctx->a[4] = P2_BASE;
	PadMoveCrosshair(ctx);                   /* bsr.w PadMoveCrosshair */
	MenacerReadXY(ctx);
	ctx->a[4] = P1_BASE;
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
	if (ctx->d[0] & 0x80) {                  /* bpl.s loc_0005CC (skip if positive) */
		set_w(&ctx->d[0], ctx->d[0] & 0x80);     /* andi.b #$80,d0 */
		f_nz(ctx, ctx->d[0] & 0xFF, 8);
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
	int16_t s = (int16_t)gy;
	if (s > 0xC8) s = 0xC8;                  /* cmpi.w #$c8 / ble / move.w #$c8 */
	if (s < 0x18) s = 0x18;                  /* cmpi.w #$18 / bge / move.w #$18 */
	gy = (uint16_t)s;
	set_w(&ctx->d[2], gy);
	RAM16(P1_BASE + 6) = gy;                 /* move.w d2,(0x6,a4) */
	f_nz(ctx, gy, 16);
	QueueCrosshair(ctx);                     /* bra.w QueueCrosshair */
}
