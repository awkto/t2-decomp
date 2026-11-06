/* Background scroll path: ScrollRows ($004752) and BgStreamLayers ($00498A).
 * docs/subsystems/vdp.md "Rows" and "Map columns", docs/formats.md, src/t2.s.
 *
 * ScrollRows (every frame): walks the ROM group list at scroll_script $FF9E98
 * (rows.w speed.w | 0 = switch to plane B (hscroll column +2) | -1 = end). Per group the 8.8
 * accumulator at (a1) $FFAF92 advances by speed; the whole-pixel step d1 is subtracted from each
 * row's hscroll entry (a2, $20 apart), added to row_pos (a3) and stored in row_delta (a4). The
 * -1 end queues list command 7 (hscroll table -> VRAM $F000) at vdp_list_ptr. Touches RAM only
 * (HLE_FUNC_PURE); the end flags are `move.l a5,(vdp_list_ptr).l` (N/Z from a5, V=C=0) with X from
 * the last X-setting instruction (sub.w/add.w/subq.w/asr.w) on the path taken.
 *
 * BgStreamLayers (called from BgStreamL1/2/4/5/6, which stay 68k and bsr/fall into it): a loop
 * over the layer list at (a0) (rows.w, spare.w; rows <= 0 ends). For a layer whose row position
 * (a1) has reached the next trigger (a6) it advances the trigger by d1, computes the plane column
 * and the map byte index, saves d0-d3/d7/a0-a6, and queues one of: tile-rect (cmd 6, optionally
 * through sub_013470/sub_01385A on levels 6/5), mirrored rect or fill rows (cmd 2) by calling the
 * unported VdpQueue* routines through hle_call68k. A zero map byte also sets level_exit.
 * The recursion `bra BgStreamLayers` is a loop here. Not pure (nested 68k).
 *
 * Every instruction is mirrored including all flags (they reach the callees); costs are the GPGX
 * 68000 table, charged on the instruction timeline (hle_insn) like hittest.c/sprite.c.
 */
#include "hle.h"

#include <stdint.h>

#define SCROLL_SCRIPT 0xFF9E98u
#define VDP_LIST_PTR  0xFF80BCu
#define BG_SCRATCH    0xFFA238u   /* move.w d1,(abs).l: the column step */
#define BG_PLANEVAL   0xFFA0AAu
#define LEVEL_EXIT    0xFFA234u
#define LEVEL_NO      0xFF80CCu

#define ADDR_FILLROWS   0x012212u
#define ADDR_TILERECT   0x012234u
#define ADDR_TILEMIRROR 0x01227Au
#define ADDR_013470     0x013470u
#define ADDR_01385A     0x01385Au

enum { OP_ADD, OP_SUB, OP_CMP };

/* add/sub/cmp of `bits` wide values: a (op) b with 68k flags (CMP leaves X) */
static uint32_t alu(hle_ctx *c, int op, uint32_t a, uint32_t b, int bits)
{
	uint32_t m = bits == 32 ? 0xFFFFFFFFu : ((1u << bits) - 1);
	uint32_t sg = 1u << (bits - 1);
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
	unsigned f = (cy ? CC_C : 0) | (ov ? CC_V : 0) | (r & sg ? CC_N : 0) | (r ? 0 : CC_Z);
	if (op != OP_CMP && cy)
		f |= CC_X;
	cc_set(c, (op == OP_CMP ? 0 : CC_X) | CC_N | CC_Z | CC_V | CC_C, f);
	return r;
}

/* NEG.W */
static uint32_t neg16(hle_ctx *c, uint32_t a)
{
	a &= 0xFFFF;
	uint32_t r = (0u - a) & 0xFFFF;
	cc_set(c, CC_X | CC_N | CC_Z | CC_V | CC_C,
	       (a ? (CC_X | CC_C) : 0) | (a == 0x8000 ? CC_V : 0) | (r & 0x8000 ? CC_N : 0) |
	       (r ? 0 : CC_Z));
	return r;
}

/* ASR.W / ASL.W #n (1 <= n <= 8) */
static uint32_t asr16(hle_ctx *c, uint32_t v, int n)
{
	v &= 0xFFFF;
	uint32_t r = (uint32_t)((int32_t)(int16_t)v >> n) & 0xFFFF;
	unsigned xb = (v >> (n - 1)) & 1;
	cc_set(c, CC_X | CC_N | CC_Z | CC_V | CC_C,
	       (xb ? (CC_X | CC_C) : 0) | (r & 0x8000 ? CC_N : 0) | (r ? 0 : CC_Z));
	return r;
}
static uint32_t asl16(hle_ctx *c, uint32_t v, int n)
{
	v &= 0xFFFF;
	uint32_t r = (v << n) & 0xFFFF;
	unsigned xb = (v >> (16 - n)) & 1;
	uint32_t mask = (0xFFFFu << (15 - n)) & 0xFFFF;
	unsigned ov = (v & mask) != 0 && (v & mask) != mask;
	cc_set(c, CC_X | CC_N | CC_Z | CC_V | CC_C,
	       (xb ? (CC_X | CC_C) : 0) | (ov ? CC_V : 0) | (r & 0x8000 ? CC_N : 0) | (r ? 0 : CC_Z));
	return r;
}

static int f_z(hle_ctx *c)  { return (c->sr & CC_Z) != 0; }
static int f_n(hle_ctx *c)  { return (c->sr & CC_N) != 0; }
static int f_v(hle_ctx *c)  { return (c->sr & CC_V) != 0; }
static int f_lt(hle_ctx *c) { return f_n(c) != f_v(c); }
static int f_le(hle_ctx *c) { return f_z(c) || f_lt(c); }

/* ------------------------------------------------------------------ ScrollRows $004752 */
HLE_FUNC_PURE(0x004752, ScrollRows, 8000)
{
	ctx->a[0] = RAM32(SCROLL_SCRIPT);
	ctx->a[1] = 0xFFFFAF92u;
	ctx->a[2] = 0xFFFFAA5Cu;
	ctx->a[3] = 0xFFFFAFD2u;
	ctx->a[4] = 0xFFFFB0D2u;
	HLE_SEQ(ctx, 20, 12, 12, 12, 12);
	for (;;) {
		set_w(&ctx->d[2], bus_r16(ctx->a[0])); ctx->a[0] += 2;     /* move.w (a0)+,d2 */
		cc_logic16(ctx, ctx->d[2]);
		hle_insn(ctx, 8);
		if (f_n(ctx)) {                                           /* bmi.w loc_0047AE */
			hle_insn(ctx, 10);
			break;
		}
		hle_insn(ctx, 12);
		if (f_z(ctx)) {                                           /* beq.w loc_0047A4 */
			hle_insn(ctx, 10);
			ctx->a[2] = 0xFFFFAA5Eu;                          /* lea.l (hscroll_ram+2),a2 */
			HLE_SEQ(ctx, 12, 10);                             /* lea / bra.w */
			continue;
		}
		hle_insn(ctx, 12);
		set_w(&ctx->d[0], bus_r16(ctx->a[0])); ctx->a[0] += 2;     /* move.w (a0)+,d0 */
		cc_logic16(ctx, ctx->d[0]);
		set_w(&ctx->d[3], RAM16(ctx->a[1]));                       /* move.w (a1),d3 */
		cc_logic16(ctx, ctx->d[3]);
		HLE_SEQ(ctx, 8, 8);
		set_w(&ctx->d[0], alu(ctx, OP_ADD, ctx->d[0], ctx->d[3], 16));   /* add.w d3,d0 */
		set_w(&ctx->d[1], ctx->d[0]);                              /* move.w d0,d1 */
		cc_logic16(ctx, ctx->d[1]);
		HLE_SEQ(ctx, 4, 4);
		set_w(&ctx->d[3], ctx->d[3] & 0xFF00); cc_logic16(ctx, ctx->d[3]);   /* andi.w #$ff00,d3 */
		set_w(&ctx->d[1], ctx->d[1] & 0xFF00); cc_logic16(ctx, ctx->d[1]);   /* andi.w #$ff00,d1 */
		HLE_SEQ(ctx, 8, 8);
		set_w(&ctx->d[1], alu(ctx, OP_SUB, ctx->d[1], ctx->d[3], 16));       /* sub.w d3,d1 */
		set_w(&ctx->d[1], asr16(ctx, ctx->d[1], 8));                         /* asr.w #8,d1 */
		HLE_SEQ(ctx, 4, 22);
		RAM16(ctx->a[1]) = (uint16_t)ctx->d[0]; ctx->a[1] += 2;             /* move.w d0,(a1)+ */
		cc_logic16(ctx, ctx->d[0]);
		hle_insn(ctx, 8);
		set_w(&ctx->d[2], alu(ctx, OP_SUB, ctx->d[2], 1, 16));              /* subq.w #1,d2 */
		hle_insn(ctx, 4);
		for (;;) {
			uint16_t d1 = (uint16_t)ctx->d[1];
			RAM16(ctx->a[2]) = (uint16_t)alu(ctx, OP_SUB, RAM16(ctx->a[2]), d1, 16);   /* sub.w d1,(a2) */
			ctx->a[2] += 0x20;                                            /* lea (0x20,a2),a2 */
			RAM16(ctx->a[3]) = (uint16_t)alu(ctx, OP_ADD, RAM16(ctx->a[3]), d1, 16);   /* add.w d1,(a3)+ */
			ctx->a[3] += 2;
			RAM16(ctx->a[4]) = d1; ctx->a[4] += 2;                        /* move.w d1,(a4)+ */
			cc_logic16(ctx, d1);
			HLE_SEQ(ctx, 12, 8, 12, 8);
			uint16_t d2 = (uint16_t)ctx->d[2] - 1;                        /* dbra d2 */
			set_w(&ctx->d[2], d2);
			if (d2 == 0xFFFF) {
				hle_insn(ctx, 14);
				break;
			}
			hle_insn(ctx, 10);
		}
		hle_insn(ctx, 10);                                                /* bra.w loc_004770 */
	}
	/* loc_0047AE */
	uint32_t a5 = RAM32(VDP_LIST_PTR);
	RAM16(a5) = 7; a5 += 2;
	RAM16(a5) = 0x400; a5 += 2;
	RAM32_W(a5, 0xFFFFAA5Cu); a5 += 4;
	RAM16(a5) = 0xF000; a5 += 2;
	RAM32_W(VDP_LIST_PTR, a5);
	ctx->a[5] = a5;
	HLE_SEQ(ctx, 20, 12, 12, 20, 12, 20, 16);
	cc_logic32(ctx, a5);
}

/* ------------------------------------------------------------------ BgStreamLayers $00498A */
static void bg_call(hle_ctx *c, uint32_t addr)
{
	hle_insn(c, 20);                    /* jsr (abs).l */
	hle_sync(c);
	hle_call68k(c, addr);
}

/* the nested VdpQueue routines and sub_013470/sub_01385A make one call last up to ~50000 cycles (level 5
 * pad_05: 49287), so the decline window must cover that, not 6000 */
HLE_FUNC(0x00498A, BgStreamLayers, 60000)
{
	for (;;) {
		ctx->a[3] = bus_r32(ctx->a[2]); ctx->a[2] += 4;            /* movea.l (a2)+,a3 */
		hle_insn(ctx, 12);
		set_w(&ctx->d[7], bus_r16(ctx->a[0])); ctx->a[0] += 2;     /* move.w (a0)+,d7 */
		cc_logic16(ctx, ctx->d[7]);
		hle_insn(ctx, 8);
		if (f_le(ctx)) {                                           /* ble.w loc_004A62 */
			HLE_SEQ(ctx, 10, 16);
			return;
		}
		hle_insn(ctx, 12);
		cc_logic16(ctx, bus_r16(ctx->a[0])); ctx->a[0] += 2;       /* tst.w (a0)+ */
		hle_insn(ctx, 8);
		if (f_z(ctx)) { hle_insn(ctx, 10); goto next; }            /* beq.w loc_004A52 */
		hle_insn(ctx, 12);
		ctx->d[6] = ctx->a[3]; cc_logic32(ctx, ctx->d[6]);         /* move.l a3,d6 */
		hle_insn(ctx, 4);
		if (f_z(ctx)) { hle_insn(ctx, 10); goto next; }            /* beq.w */
		hle_insn(ctx, 12);
		set_w(&ctx->d[6], RAM16(ctx->a[1])); cc_logic16(ctx, ctx->d[6]);   /* move.w (a1),d6 */
		set_w(&ctx->d[5], ctx->d[6]); cc_logic16(ctx, ctx->d[5]);          /* move.w d6,d5 */
		hle_insn(ctx, 8);
		hle_insn(ctx, 4);
		alu(ctx, OP_CMP, ctx->d[6], RAM16(ctx->a[6]), 16);         /* cmp.w (a6),d6 */
		hle_insn(ctx, 8);
		if (f_lt(ctx)) { hle_insn(ctx, 10); goto next; }           /* blt.w */
		hle_insn(ctx, 12);
		set_w(&ctx->d[6], RAM16(ctx->a[6])); cc_logic16(ctx, ctx->d[6]);   /* move.w (a6),d6 */
		RAM16(ctx->a[6]) = (uint16_t)alu(ctx, OP_ADD, RAM16(ctx->a[6]), ctx->d[1], 16);  /* add.w d1,(a6) */
		RAM16(BG_SCRATCH) = (uint16_t)ctx->d[1]; cc_logic16(ctx, ctx->d[1]);             /* move.w d1,(abs).l */
		HLE_SEQ(ctx, 8, 12, 16);
		set_w(&ctx->d[6], ctx->d[5]); cc_logic16(ctx, ctx->d[6]);  /* move.w d5,d6 */
		set_w(&ctx->d[6], alu(ctx, OP_SUB, ctx->d[6], ctx->d[1], 16));   /* sub.w d1,d6 */
		set_w(&ctx->d[6], asr16(ctx, ctx->d[6], 2));               /* asr.w #2,d6 */
		set_w(&ctx->d[6], ctx->d[6] & 0x7F); cc_logic16(ctx, ctx->d[6]); /* andi.w #$7f,d6 */
		set_w(&ctx->d[6], alu(ctx, OP_ADD, ctx->d[6], ctx->d[0], 16));   /* add.w d0,d6 */
		HLE_SEQ(ctx, 4, 4, 10, 8, 4);
		alu(ctx, OP_CMP, ctx->d[2], 8, 16);                        /* cmpi.w #8,d2 */
		hle_insn(ctx, 8);
		if (!f_z(ctx)) {                                           /* bne.s loc_0049C8 */
			hle_insn(ctx, 10);
			set_w(&ctx->d[5], asr16(ctx, ctx->d[5], 7));       /* asr.w #7,d5 */
			set_w(&ctx->d[5], alu(ctx, OP_ADD, ctx->d[5], 3, 16));   /* addq.w #3,d5 */
			HLE_SEQ(ctx, 20, 4);
		} else {
			hle_insn(ctx, 8);
			set_w(&ctx->d[5], asr16(ctx, ctx->d[5], 6));       /* asr.w #6,d5 */
			HLE_SEQ(ctx, 18, 10);                              /* asr.w / bra.s */
		}
		{
			/* movem.l d0-d3/d7/a0-a6,-(sp) */
			uint32_t regs[12] = { ctx->d[0], ctx->d[1], ctx->d[2], ctx->d[3], ctx->d[7],
			                      ctx->a[0], ctx->a[1], ctx->a[2], ctx->a[3], ctx->a[4],
			                      ctx->a[5], ctx->a[6] };
			ctx->a[7] -= 48;
			for (int i = 0; i < 12; i++)
				bus_w32(ctx->a[7] + 4 * i, regs[i]);
			hle_movem_l(ctx, 104, 12);
		}
		set_w(&ctx->d[3], ctx->d[7]); cc_logic16(ctx, ctx->d[3]);  /* move.w d7,d3 */
		set_w(&ctx->d[1], ctx->d[6]); cc_logic16(ctx, ctx->d[1]);  /* move.w d6,d1 */
		set_w(&ctx->d[6], RAM16(BG_PLANEVAL)); cc_logic16(ctx, ctx->d[6]);   /* move.w (abs).l,d6 */
		HLE_SEQ(ctx, 4, 4, 16);
		{
			uint32_t b = bus_r8(ctx->a[3] + (uint32_t)(int32_t)(int16_t)ctx->d[5]);
			set_b(&ctx->d[5], b); cc_logic8(ctx, b);               /* move.b (0,a3,d5.w),d5 */
			set_w(&ctx->d[5], (uint32_t)(int32_t)(int8_t)b); cc_logic16(ctx, ctx->d[5]);   /* ext.w d5 */
			HLE_SEQ(ctx, 14, 4);
		}
		if (!f_z(ctx)) {                                           /* bne.s loc_0049EA */
			hle_insn(ctx, 10);
		} else {
			hle_insn(ctx, 8);
			bus_w8(LEVEL_EXIT, 0xFF); cc_logic8(ctx, 0xFF);        /* move.b #$ff,(abs).l */
			hle_insn(ctx, 20);
		}
		if (f_n(ctx)) {                                            /* bpl.s loc_004A16 not taken */
			hle_insn(ctx, 8);
			alu(ctx, OP_CMP, ctx->d[5], 0x80, 8);                  /* cmpi.b #$80,d5 */
			hle_insn(ctx, 8);
			if (f_z(ctx)) {                                    /* bne.s loc_0049FA not taken */
				hle_insn(ctx, 8);
				bg_call(ctx, ADDR_FILLROWS);               /* jsr (VdpQueueFillRows).l */
				hle_insn(ctx, 10);                         /* bra.s loc_004A4E */
			} else {
				hle_insn(ctx, 10);
				set_w(&ctx->d[5], neg16(ctx, ctx->d[5]));  /* neg.w d5 */
				set_w(&ctx->d[5], alu(ctx, OP_ADD, ctx->d[5], ctx->d[5], 16));   /* add.w d5,d5 */
				ctx->d[7] = 0xFFFFFFFFu; cc_logic32(ctx, ctx->d[7]);   /* moveq #-1,d7 */
				set_w(&ctx->d[7], bus_r16(ctx->a[5] + (uint32_t)(int32_t)(int16_t)ctx->d[5]));
				cc_logic16(ctx, ctx->d[7]);                /* move.w (0,a5,d5.w),d7 */
				ctx->a[3] = ctx->d[7];                     /* movea.l d7,a3 */
				set_w(&ctx->d[7], bus_r16(ctx->a[3])); cc_logic16(ctx, ctx->d[7]);   /* move.w (a3),d7 */
				set_w(&ctx->d[5], alu(ctx, OP_ADD, ctx->d[5], ctx->d[5], 16));   /* add.w d5,d5 */
				ctx->a[0] = bus_r32(ctx->a[4] + (uint32_t)(int32_t)(int16_t)ctx->d[5]);   /* movea.l (0,a4,d5.w),a0 */
				HLE_SEQ(ctx, 4, 4, 4, 14, 4, 8, 4, 18);
				bg_call(ctx, ADDR_TILEMIRROR);
				hle_insn(ctx, 10);                         /* bra.s */
			}
		} else {
			hle_insn(ctx, 10);                                 /* bpl.s taken */
			set_w(&ctx->d[5], alu(ctx, OP_ADD, ctx->d[5], ctx->d[5], 16));   /* add.w d5,d5 */
			ctx->d[7] = 0xFFFFFFFFu; cc_logic32(ctx, ctx->d[7]);   /* moveq #-1,d7 */
			set_w(&ctx->d[7], bus_r16(ctx->a[5] + (uint32_t)(int32_t)(int16_t)ctx->d[5]));
			cc_logic16(ctx, ctx->d[7]);
			ctx->a[3] = ctx->d[7];
			set_w(&ctx->d[7], bus_r16(ctx->a[3])); cc_logic16(ctx, ctx->d[7]);
			set_w(&ctx->d[5], alu(ctx, OP_ADD, ctx->d[5], ctx->d[5], 16));
			ctx->a[0] = bus_r32(ctx->a[4] + (uint32_t)(int32_t)(int16_t)ctx->d[5]);
			HLE_SEQ(ctx, 4, 4, 14, 4, 8, 4, 18);
			alu(ctx, OP_CMP, RAM16(LEVEL_NO), 6, 16);              /* cmpi.w #6,(abs).l */
			hle_insn(ctx, 20);
			if (f_z(ctx)) {                                    /* bne.s not taken */
				hle_insn(ctx, 8);
				bg_call(ctx, ADDR_013470);
			} else {
				hle_insn(ctx, 10);
			}
			alu(ctx, OP_CMP, RAM16(LEVEL_NO), 5, 16);              /* cmpi.w #5,(abs).l */
			hle_insn(ctx, 20);
			if (f_z(ctx)) {
				hle_insn(ctx, 8);
				bg_call(ctx, ADDR_01385A);
			} else {
				hle_insn(ctx, 10);
			}
			bg_call(ctx, ADDR_TILERECT);
		}
		{
			/* movem.l (sp)+,d0-d3/d7/a0-a6 */
			uint32_t *r[12] = { &ctx->d[0], &ctx->d[1], &ctx->d[2], &ctx->d[3], &ctx->d[7],
			                    &ctx->a[0], &ctx->a[1], &ctx->a[2], &ctx->a[3], &ctx->a[4],
			                    &ctx->a[5], &ctx->a[6] };
			for (int i = 0; i < 12; i++)
				*r[i] = bus_r32(ctx->a[7] + 4 * i);
			ctx->a[7] += 48;
			hle_movem_l(ctx, 108, 12);
		}
	next:
		set_w(&ctx->d[7], alu(ctx, OP_ADD, ctx->d[7], ctx->d[7], 16));      /* add.w d7,d7 */
		ctx->a[1] += (uint32_t)(int32_t)(int16_t)ctx->d[7];                 /* lea (0,a1,d7.w),a1 */
		set_w(&ctx->d[7], asl16(ctx, ctx->d[7], 6));                        /* asl.w #6,d7 */
		set_w(&ctx->d[0], alu(ctx, OP_ADD, ctx->d[0], ctx->d[7], 16));      /* add.w d7,d0 */
		ctx->a[6] += 2;                                                     /* addq.l #2,a6 */
		HLE_SEQ(ctx, 4, 12, 18, 4, 8, 10);
	}
}
