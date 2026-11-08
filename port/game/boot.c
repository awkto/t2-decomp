/* The cold-boot path and the hardware helpers it needs, as C blocks (t2-decomp#11, phase P5; docs/native.md "Cold boot").
 *
 * 260 instructions: Reset, Boot, the Z80 loader, VdpSetRegs, the VRAM/CRAM/VSRAM clear helpers, the port-2 id read
 *
 * Generated instruction by instruction from the disassembly by tools/py/bootgen.py (objdump with relocations and the
 * GPGX opcode cycle table; `costcheck.py` checks every charge).  The blocks are pieces as in piecegen.py (an entry, up
 * to the next jsr/bsr/jmp/rts, which the glue blocks of glue.c execute), with the instruction timeline of the sound
 * driver (game/sound.c): hle_insn for the instructions that touch work RAM and ROM only, hle_pre .. hle_post around the
 * ones that reach the I/O area, the Z80 or the VDP, so the access sees the clock the 68k core would give it.  A
 * backward branch leaves the piece (one pass of a loop is one block) and a write to SR ends it.  The VDP status word
 * has its bits 10-15 from the instruction prefetch (the word behind the reading instruction, as in game/vdpfill.c).
 */
#include "hle.h"
#include "gx.h"

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#pragma GCC diagnostic ignored "-Wunused-label"

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

/* asl ('L') / lsl ('l') / lsr ('r') / asr ('a') / roxl ('x') by an immediate count (1..8) on a register */
static uint32_t shf(hle_ctx *c, char kind, uint32_t v, int n, int bits)
{
	uint32_t m = (bits == 32) ? 0xFFFFFFFFu : ((1u << bits) - 1), sg = 1u << (bits - 1);
	uint32_t res = v & m;
	unsigned cb = 0, ov = 0, x = (c->sr & CC_X) ? 1 : 0;
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
	(void)x;
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

/* abcd (sub = 0) / sbcd (sub = 1) of one byte: GPGX's m68k_op_abcd_8_mm / sbcd_8_mm (undefined N, V included) */
static uint32_t __attribute__((unused)) bcd(hle_ctx *ctx, int sub, uint32_t src, uint32_t dst)
{
	uint32_t x = (ctx->sr & CC_X) ? 1 : 0, res, vraw, carry, bits = 0;
	if (!sub) {
		uint32_t lo = (src & 0x0F) + (dst & 0x0F) + x;
		uint32_t corf = (lo > 9) ? 6 : 0;
		uint32_t r2 = lo + (src & 0xF0) + (dst & 0xF0);
		vraw = ~r2;
		uint32_t r3 = r2 + corf;
		carry = (r3 > 0x9F) ? 1 : 0;
		if (carry)
			r3 -= 0xA0;
		res = r3 & 0xFF;
		vraw &= r3;
	} else {
		res = (dst & 0x0F) - (src & 0x0F) - x;
		uint32_t corf = (res > 0xF) ? 6 : 0;
		res += (dst & 0xF0) - (src & 0xF0);
		vraw = res;
		if (res > 0xFF) {
			res += 0xA0;
			carry = 1;
		} else {
			carry = res < corf;
		}
		res = (res - corf) & 0xFF;
		vraw &= ~res;
	}
	if (carry)
		bits |= CC_X | CC_C;
	if (res & 0x80)
		bits |= CC_N;
	if (vraw & 0x80)
		bits |= CC_V;
	cc_set(ctx, CC_X | CC_N | CC_V | CC_C, bits);
	if (res)
		cc_set(ctx, CC_Z, 0);
	return res;
}

/* work RAM and ROM are plain memory; anything else (Z80 area, I/O, VDP) is an I/O access made at the instruction's clock */
static inline int plain(uint32_t a)
{
	a &= 0xFFFFFFu;
	return a >= 0xE00000u || a < 0x400000u;
}
static uint32_t g_next_pc;   /* the word behind the instruction that makes the I/O access (the prefetch queue) */
static int g_own_sess;       /* this block started a VDP session (below) */
static void io_pre(hle_ctx *ctx, unsigned op, uint32_t next_pc)
{
	hle_pre(ctx);
	gx_m68k_set_ir(op);
	g_next_pc = next_pc;
}
/* The VDP is clocked by the CPU: its raster line follows the 68k clock, and a data port write stalls the CPU while the
 * FIFO is full.  Both need the "exact session" of game/vdpfill.c, which the first VDP access of a block opens (when no
 * C function above us has one) and block_end() closes: the line base and V counter are rewound to the session start
 * and the time spent is charged. */
static inline int is_vdp(uint32_t p) { return (p & 0xFFFFE0u) == 0xC00000u; }
static void vdp_enter(void)
{
	if (!g_own_sess && hle_nest_depth() == 0) {
		hle_dma_begin();
		hle_exact_session();
		g_own_sess = 1;
	}
	gx_vdp_follow_cpu_lines();
}
static inline void block_end(void)
{
	if (g_own_sess) {
		g_own_sess = 0;
		hle_vdp_end();
	}
}
static inline uint8_t ld8(uint32_t a)
{
	uint32_t p = a & 0xFFFFFFu;
	if (p >= 0xE00000u) return RAM8(p);
	if (p < 0x400000u) return ROM8(p);
	if (is_vdp(p)) vdp_enter();
	return bus_r8(p);
}
static inline uint16_t ld16(uint32_t a)
{
	uint32_t p = a & 0xFFFFFFu;
	if (p >= 0xE00000u) return RAM16(p);
	if (p < 0x400000u) return ROM16(p);
	if (is_vdp(p)) vdp_enter();
	if ((p & 0xFFFFE4u) == 0xC00004u)                    /* VDP status: bits 10-15 are the prefetched word */
		return (uint16_t)((bus_r16(p) & 0x3FF) | (ROM16(g_next_pc) & 0xFC00));
	return bus_r16(p);
}
static inline uint32_t ld32(uint32_t a)
{
	uint32_t p = a & 0xFFFFFFu;
	if (p >= 0xE00000u) return RAM32(p);
	if (p < 0x400000u) return ROM32(p);
	if (is_vdp(p)) vdp_enter();
	return bus_r32(p);
}
static inline void st8(uint32_t a, uint32_t v)
{
	uint32_t p = a & 0xFFFFFFu;
	if (p >= 0xE00000u) { RAM8(p) = (uint8_t)v; return; }
	if (is_vdp(p)) { vdp_enter(); hle_vdp_timed(1); bus_w8(p, (uint8_t)v); hle_vdp_timed(0); return; }
	bus_w8(p, (uint8_t)v);
}
static inline void st16(uint32_t a, uint32_t v)
{
	uint32_t p = a & 0xFFFFFFu;
	if (p >= 0xE00000u) { RAM16(p) = (uint16_t)v; return; }
	if (is_vdp(p)) { vdp_enter(); if (getenv("T2_BTRACE")) fprintf(stderr, "st16 %06X <- %04X clk %d\n", p, v & 0xFFFF, gx_m68k_cycles()); hle_vdp_timed(1); bus_w16(p, (uint16_t)v); hle_vdp_timed(0); return; }
	bus_w16(p, (uint16_t)v);
}
static inline void st32(uint32_t a, uint32_t v)
{
	uint32_t p = a & 0xFFFFFFu;
	if (p >= 0xE00000u) { RAM32_W(p, v); return; }
	if (is_vdp(p)) { vdp_enter(); hle_vdp_timed(1); bus_w32(p, v); hle_vdp_timed(0); return; }
	bus_w32(p, v);
}

static void boot_run(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x000200: goto L_000200;
	case 0x000238: goto L_000238;
	case 0x00024A: goto L_00024A;
	case 0x000250: goto L_000250;
	case 0x00025C: goto L_00025C;
	case 0x000268: goto L_000268;
	case 0x000272: goto L_000272;
	case 0x00027A: goto L_00027A;
	case 0x00028C: goto L_00028C;
	case 0x001BE8: goto L_001BE8;
	case 0x007406: goto L_007406;
	case 0x007422: goto L_007422;
	case 0x007432: goto L_007432;
	case 0x007442: goto L_007442;
	case 0x008162: goto L_008162;
	case 0x00817A: goto L_00817A;
	case 0x00818C: goto L_00818C;
	case 0x0081A2: goto L_0081A2;
	case 0x0081B6: goto L_0081B6;
	case 0x0081CA: goto L_0081CA;
	case 0x00867A: goto L_00867A;
	case 0x008692: goto L_008692;
	case 0x0086F6: goto L_0086F6;
	case 0x00871A: goto L_00871A;
	case 0x00884C: goto L_00884C;
	case 0x0FBB00: goto L_0FBB00;
	case 0x0FBB06: goto L_0FBB06;
	case 0x0FBB16: goto L_0FBB16;
	case 0x0FBB22: goto L_0FBB22;
	case 0x0FBB26: goto L_0FBB26;
	case 0x0FBB2E: goto L_0FBB2E;
	case 0x0FBB68: goto L_0FBB68;
	case 0x0FC32E: goto L_0FC32E;
	case 0x0FC340: goto L_0FC340;
	case 0x0FC352: goto L_0FC352;
	case 0x0FC35E: goto L_0FC35E;
	case 0x0FC370: goto L_0FC370;
	case 0x0FC37A: goto L_0FC37A;
	case 0x0FC37C: goto L_0FC37C;
	case 0x0FC388: goto L_0FC388;
	case 0x0FC3A8: goto L_0FC3A8;
	case 0x0FC3B6: goto L_0FC3B6;
	case 0x0FC3DE: goto L_0FC3DE;
	case 0x0FC3E6: goto L_0FC3E6;
	case 0x0FC3F8: goto L_0FC3F8;
	case 0x0FC438: goto L_0FC438;
	case 0x0FC440: goto L_0FC440;
	case 0x0FC44A: goto L_0FC44A;
	case 0x0FC462: goto L_0FC462;
	default: __builtin_trap();
	}
L_000200:
	{ uint32_t eaD = 0x00A10008u; int io = !plain(eaD); if (io) io_pre(ctx, 0x4AB9, 0x000206); cc_logic32(ctx, ld32(eaD)); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 000200 tstl a10008 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_00020E; } hle_insn(ctx, 8);  /* 000206 bnes 20e */
	{ uint32_t eaD = 0x00A1000Cu; int io = !plain(eaD); if (io) io_pre(ctx, 0x4A79, 0x00020E); cc_logic16(ctx, ld16(eaD)); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 000208 tstw a1000c */
L_00020E:
	if (COND_NE) { hle_insn(ctx, 10); goto L_00028C; } hle_insn(ctx, 8);  /* 00020E bnes 28c */
	{ uint32_t eaS = 0x0000028Eu; ctx->a[5] = eaS; } hle_insn(ctx, 8);  /* 000210 lea %pc@(28e),%a5 */
	{ uint32_t eaS = ctx->a[5]; int io = !plain(eaS); if (io) io_pre(ctx, 0x4C9D, 0x000218); ctx->d[5] = (uint32_t)(int32_t)(int16_t)ld16(eaS + 0); ctx->d[6] = (uint32_t)(int32_t)(int16_t)ld16(eaS + 2); ctx->d[7] = (uint32_t)(int32_t)(int16_t)ld16(eaS + 4); ctx->a[5] += 6; if (io) hle_post(ctx, 24); else hle_insn(ctx, 24); }  /* 000214 movemw %a5@+,%d5-%d7 */
	{ uint32_t eaS = ctx->a[5]; int io = !plain(eaS); if (io) io_pre(ctx, 0x4CDD, 0x00021C); ctx->a[0] = ld32(eaS + 0); ctx->a[1] = ld32(eaS + 4); ctx->a[2] = ld32(eaS + 8); ctx->a[3] = ld32(eaS + 12); ctx->a[4] = ld32(eaS + 16); ctx->a[5] += 20; if (io) hle_post(ctx, 52); else hle_movem_l(ctx, 52, 5); }  /* 000218 moveml %a5@+,%a0-%a4 */
	{ uint32_t eaS = ctx->a[1] + (uint32_t)(int32_t)(-4351); int io = !plain(eaS); if (io) io_pre(ctx, 0x1029, 0x000220); uint32_t v = ld8(eaS); set_b(&ctx->d[0], v); cc_logic8(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 00021C moveb %a1@(-4351),%d0 */
	{ uint32_t v = ((ctx->d[0] & 0xFFu) & 0xFu) & 0xFFu; set_b(&ctx->d[0], v); cc_logic8(ctx, v); } hle_insn(ctx, 8);  /* 000220 andib #15,%d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_00022E; } hle_insn(ctx, 8);  /* 000224 beqs 22e */
	{ uint32_t eaD = ctx->a[1] + (uint32_t)(int32_t)(12032); int io = !plain(eaD); if (io) io_pre(ctx, 0x237C, 0x00022E); uint32_t v = 0x53454741u; st32(eaD, v); cc_logic32(ctx, v); if (io) hle_post(ctx, 24); else hle_insn(ctx, 24); }  /* 000226 movel #1397049153,%a1@(12032) */
L_00022E:
	{ uint32_t eaS = ctx->a[4]; int io = !plain(eaS); if (io) io_pre(ctx, 0x3014, 0x000230); uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); if (io) hle_post(ctx, 8); else hle_insn(ctx, 8); }  /* 00022E movew %a4@,%d0 */
	{ ctx->d[0] = 0x00000000u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 000230 moveq #0,%d0 */
	{ ctx->a[6] = (ctx->d[0] & 0xFFFFFFFFu); } hle_insn(ctx, 4);  /* 000232 moveal %d0,%fp */
	hle_insn(ctx, 4); hle_set_usp(ctx->a[6]);  /* 000234 movel %fp,%usp */
	{ ctx->d[1] = 0x00000017u; cc_logic32(ctx, ctx->d[1]); } hle_insn(ctx, 4);  /* 000236 moveq #23,%d1 */
L_000238:
	{ uint32_t eaS = ctx->a[5]; ctx->a[5] += 1; int io = !plain(eaS); if (io) io_pre(ctx, 0x1A1D, 0x00023A); uint32_t v = ld8(eaS); set_b(&ctx->d[5], v); cc_logic8(ctx, v); if (io) hle_post(ctx, 8); else hle_insn(ctx, 8); }  /* 000238 moveb %a5@+,%d5 */
	{ uint32_t eaD = ctx->a[4]; int io = !plain(eaD); if (io) io_pre(ctx, 0x3885, 0x00023C); uint32_t v = (ctx->d[5] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 8); else hle_insn(ctx, 8); }  /* 00023A movew %d5,%a4@ */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[5] & 0xFFFFu), (ctx->d[7] & 0xFFFFu), 16, 1); set_w(&ctx->d[5], v); } hle_insn(ctx, 4);  /* 00023C addw %d7,%d5 */
	{ uint16_t v = (uint16_t)ctx->d[1] - 1; set_w(&ctx->d[1], v); if (v != 0xFFFF) { hle_insn(ctx, 10); hle_jump(ctx, 0x000238); return; } hle_insn(ctx, 14); }  /* 00023E dbf %d1,238 */
	{ uint32_t eaS = ctx->a[5]; ctx->a[5] += 4; uint32_t eaD = ctx->a[4]; int io = !plain(eaS) || !plain(eaD); if (io) io_pre(ctx, 0x289D, 0x000244); uint32_t v = ld32(eaS); st32(eaD, v); cc_logic32(ctx, v); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 000242 movel %a5@+,%a4@ */
	{ uint32_t eaD = ctx->a[3]; int io = !plain(eaD); if (io) io_pre(ctx, 0x3680, 0x000246); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 8); else hle_insn(ctx, 8); }  /* 000244 movew %d0,%a3@ */
	{ uint32_t eaD = ctx->a[1]; int io = !plain(eaD); if (io) io_pre(ctx, 0x3287, 0x000248); uint32_t v = (ctx->d[7] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 8); else hle_insn(ctx, 8); }  /* 000246 movew %d7,%a1@ */
	{ uint32_t eaD = ctx->a[2]; int io = !plain(eaD); if (io) io_pre(ctx, 0x3487, 0x00024A); uint32_t v = (ctx->d[7] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 8); else hle_insn(ctx, 8); }  /* 000248 movew %d7,%a2@ */
L_00024A:
	{ uint32_t eaD = ctx->a[1];  int io = !plain(eaD); if (io) io_pre(ctx, 0x0111, 0x00024C); unsigned b = (ctx->d[0] & 7); uint32_t m = 1u << b; uint8_t v = ld8(eaD); cc_set(ctx, CC_Z, (v & m) ? 0 : CC_Z);  if (io) hle_post(ctx, 8); else hle_insn(ctx, 8); }  /* 00024A btst %d0,%a1@ */
	if (COND_NE) { hle_insn(ctx, 10); hle_jump(ctx, 0x00024A); return; } hle_insn(ctx, 8);  /* 00024C bnes 24a */
	{ ctx->d[2] = 0x00000025u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 00024E moveq #37,%d2 */
L_000250:
	{ uint32_t eaS = ctx->a[5]; ctx->a[5] += 1; uint32_t eaD = ctx->a[0]; ctx->a[0] += 1; int io = !plain(eaS) || !plain(eaD); if (io) io_pre(ctx, 0x10DD, 0x000252); uint32_t v = ld8(eaS); st8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 000250 moveb %a5@+,%a0@+ */
	{ uint16_t v = (uint16_t)ctx->d[2] - 1; set_w(&ctx->d[2], v); if (v != 0xFFFF) { hle_insn(ctx, 10); hle_jump(ctx, 0x000250); return; } hle_insn(ctx, 14); }  /* 000252 dbf %d2,250 */
	{ uint32_t eaD = ctx->a[2]; int io = !plain(eaD); if (io) io_pre(ctx, 0x3480, 0x000258); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 8); else hle_insn(ctx, 8); }  /* 000256 movew %d0,%a2@ */
	{ uint32_t eaD = ctx->a[1]; int io = !plain(eaD); if (io) io_pre(ctx, 0x3280, 0x00025A); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 8); else hle_insn(ctx, 8); }  /* 000258 movew %d0,%a1@ */
	{ uint32_t eaD = ctx->a[2]; int io = !plain(eaD); if (io) io_pre(ctx, 0x3487, 0x00025C); uint32_t v = (ctx->d[7] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 8); else hle_insn(ctx, 8); }  /* 00025A movew %d7,%a2@ */
L_00025C:
	{ ctx->a[6] -= 4; uint32_t eaD = ctx->a[6]; int io = !plain(eaD); if (io) io_pre(ctx, 0x2D00, 0x00025E); uint32_t v = (ctx->d[0] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 00025C movel %d0,%fp@- */
	{ uint16_t v = (uint16_t)ctx->d[6] - 1; set_w(&ctx->d[6], v); if (v != 0xFFFF) { hle_insn(ctx, 10); hle_jump(ctx, 0x00025C); return; } hle_insn(ctx, 14); }  /* 00025E dbf %d6,25c */
	{ uint32_t eaS = ctx->a[5]; ctx->a[5] += 4; uint32_t eaD = ctx->a[4]; int io = !plain(eaS) || !plain(eaD); if (io) io_pre(ctx, 0x289D, 0x000264); uint32_t v = ld32(eaS); st32(eaD, v); cc_logic32(ctx, v); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 000262 movel %a5@+,%a4@ */
	{ uint32_t eaS = ctx->a[5]; ctx->a[5] += 4; uint32_t eaD = ctx->a[4]; int io = !plain(eaS) || !plain(eaD); if (io) io_pre(ctx, 0x289D, 0x000266); uint32_t v = ld32(eaS); st32(eaD, v); cc_logic32(ctx, v); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 000264 movel %a5@+,%a4@ */
	{ ctx->d[3] = 0x0000001Fu; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 000266 moveq #31,%d3 */
L_000268:
	{ uint32_t eaD = ctx->a[3]; int io = !plain(eaD); if (io) io_pre(ctx, 0x2680, 0x00026A); uint32_t v = (ctx->d[0] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 000268 movel %d0,%a3@ */
	{ uint16_t v = (uint16_t)ctx->d[3] - 1; set_w(&ctx->d[3], v); if (v != 0xFFFF) { hle_insn(ctx, 10); hle_jump(ctx, 0x000268); return; } hle_insn(ctx, 14); }  /* 00026A dbf %d3,268 */
	{ uint32_t eaS = ctx->a[5]; ctx->a[5] += 4; uint32_t eaD = ctx->a[4]; int io = !plain(eaS) || !plain(eaD); if (io) io_pre(ctx, 0x289D, 0x000270); uint32_t v = ld32(eaS); st32(eaD, v); cc_logic32(ctx, v); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 00026E movel %a5@+,%a4@ */
	{ ctx->d[4] = 0x00000013u; cc_logic32(ctx, ctx->d[4]); } hle_insn(ctx, 4);  /* 000270 moveq #19,%d4 */
L_000272:
	{ uint32_t eaD = ctx->a[3]; int io = !plain(eaD); if (io) io_pre(ctx, 0x2680, 0x000274); uint32_t v = (ctx->d[0] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 000272 movel %d0,%a3@ */
	{ uint16_t v = (uint16_t)ctx->d[4] - 1; set_w(&ctx->d[4], v); if (v != 0xFFFF) { hle_insn(ctx, 10); hle_jump(ctx, 0x000272); return; } hle_insn(ctx, 14); }  /* 000274 dbf %d4,272 */
	{ ctx->d[5] = 0x00000003u; cc_logic32(ctx, ctx->d[5]); } hle_insn(ctx, 4);  /* 000278 moveq #3,%d5 */
L_00027A:
	{ uint32_t eaS = ctx->a[5]; ctx->a[5] += 1; uint32_t eaD = ctx->a[3] + (uint32_t)(int32_t)(17); int io = !plain(eaS) || !plain(eaD); if (io) io_pre(ctx, 0x175D, 0x00027E); uint32_t v = ld8(eaS); st8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 00027A moveb %a5@+,%a3@(17) */
	{ uint16_t v = (uint16_t)ctx->d[5] - 1; set_w(&ctx->d[5], v); if (v != 0xFFFF) { hle_insn(ctx, 10); hle_jump(ctx, 0x00027A); return; } hle_insn(ctx, 14); }  /* 00027E dbf %d5,27a */
	{ uint32_t eaD = ctx->a[2]; int io = !plain(eaD); if (io) io_pre(ctx, 0x3480, 0x000284); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 8); else hle_insn(ctx, 8); }  /* 000282 movew %d0,%a2@ */
	{ uint32_t eaS = ctx->a[6]; int io = !plain(eaS); if (io) io_pre(ctx, 0x4CD6, 0x000288); ctx->d[0] = ld32(eaS + 0); ctx->d[1] = ld32(eaS + 4); ctx->d[2] = ld32(eaS + 8); ctx->d[3] = ld32(eaS + 12); ctx->d[4] = ld32(eaS + 16); ctx->d[5] = ld32(eaS + 20); ctx->d[6] = ld32(eaS + 24); ctx->d[7] = ld32(eaS + 28); ctx->a[0] = ld32(eaS + 32); ctx->a[1] = ld32(eaS + 36); ctx->a[2] = ld32(eaS + 40); ctx->a[3] = ld32(eaS + 44); ctx->a[4] = ld32(eaS + 48); ctx->a[5] = ld32(eaS + 52); ctx->a[6] = ld32(eaS + 56); if (io) hle_post(ctx, 132); else hle_movem_l(ctx, 132, 15); }  /* 000284 moveml %fp@,%d0-%fp */
	hle_insn(ctx, 16); ctx->sr = 0x2700u; hle_jump(ctx, 0x00028C); return;  /* 000288 movew #9984,%sr */
L_00028C:
	hle_insn(ctx, 10); goto L_0002FA;  /* 00028C bras 2fa */
L_0002FA:
	{ uint32_t eaD = 0x00C00004u; int io = !plain(eaD); if (io) io_pre(ctx, 0x4A79, 0x000300); cc_logic16(ctx, ld16(eaD)); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0002FA tstw c00004 */
	hle_jump(ctx, 0x000300); return;  /* 000300 jmp 7406 */
L_001BE8:
	{ uint32_t eaS = 0x00A10005u; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 001BE8 lea a10005,%a0 */
	{ uint32_t eaS = 0x00A1000Bu; ctx->a[1] = eaS; } hle_insn(ctx, 12);  /* 001BEE lea a1000b,%a1 */
	{ uint32_t eaS = 0x00A10019u; ctx->a[2] = eaS; } hle_insn(ctx, 12);  /* 001BF4 lea a10019,%a2 */
	{ uint32_t eaD = ctx->a[0]; int io = !plain(eaD); if (io) io_pre(ctx, 0x10BC, 0x001BFE); uint32_t v = 0x0u; st8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 001BFA moveb #0,%a0@ */
	{ uint32_t eaD = ctx->a[1]; int io = !plain(eaD); if (io) io_pre(ctx, 0x12BC, 0x001C02); uint32_t v = 0x40u; st8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 001BFE moveb #64,%a1@ */
	{ uint32_t eaD = ctx->a[2]; int io = !plain(eaD); if (io) io_pre(ctx, 0x14BC, 0x001C06); uint32_t v = 0x80u; st8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 001C02 moveb #-128,%a2@ */
	hle_insn(ctx, 4);  /* 001C06 nop */
	hle_insn(ctx, 4);  /* 001C08 nop */
	{ uint32_t eaS = ctx->a[0]; int io = !plain(eaS); if (io) io_pre(ctx, 0x1210, 0x001C0C); uint32_t v = ld8(eaS); set_b(&ctx->d[1], v); cc_logic8(ctx, v); if (io) hle_post(ctx, 8); else hle_insn(ctx, 8); }  /* 001C0A moveb %a0@,%d1 */
	{ uint32_t eaD = ctx->a[0]; int io = !plain(eaD); if (io) io_pre(ctx, 0x10BC, 0x001C10); uint32_t v = 0x40u; st8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 001C0C moveb #64,%a0@ */
	{ uint32_t v = (ctx->d[1] & 0xFFu); set_b(&ctx->d[6], v); cc_logic8(ctx, v); } hle_insn(ctx, 4);  /* 001C10 moveb %d1,%d6 */
	{ uint32_t v = (ctx->d[1] & 0xFFu); set_b(&ctx->d[2], v); cc_logic8(ctx, v); } hle_insn(ctx, 4);  /* 001C12 moveb %d1,%d2 */
	{ uint32_t v = ((ctx->d[1] & 0xFFu) & 0x5u) & 0xFFu; set_b(&ctx->d[1], v); cc_logic8(ctx, v); } hle_insn(ctx, 8);  /* 001C14 andib #5,%d1 */
	{ uint32_t v = ((ctx->d[2] & 0xFFu) & 0xAu) & 0xFFu; set_b(&ctx->d[2], v); cc_logic8(ctx, v); } hle_insn(ctx, 8);  /* 001C18 andib #10,%d2 */
	{ uint32_t v = shf(ctx, 'r', (ctx->d[2] & 0xFFu), 1, 8); set_b(&ctx->d[2], v); } hle_insn(ctx, 8);  /* 001C1C lsrb #1,%d2 */
	{ uint32_t v = ((ctx->d[1] & 0xFFu) | (ctx->d[2] & 0xFFu)) & 0xFFu; set_b(&ctx->d[1], v); cc_logic8(ctx, v); } hle_insn(ctx, 4);  /* 001C1E orb %d2,%d1 */
	{ uint32_t eaS = ctx->a[0]; int io = !plain(eaS); if (io) io_pre(ctx, 0x1410, 0x001C22); uint32_t v = ld8(eaS); set_b(&ctx->d[2], v); cc_logic8(ctx, v); if (io) hle_post(ctx, 8); else hle_insn(ctx, 8); }  /* 001C20 moveb %a0@,%d2 */
	{ uint32_t eaD = ctx->a[0]; int io = !plain(eaD); if (io) io_pre(ctx, 0x10BC, 0x001C26); uint32_t v = 0x0u; st8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 001C22 moveb #0,%a0@ */
	{ uint32_t v = (ctx->d[2] & 0xFFu); set_b(&ctx->d[7], v); cc_logic8(ctx, v); } hle_insn(ctx, 4);  /* 001C26 moveb %d2,%d7 */
	{ uint32_t v = (ctx->d[2] & 0xFFu); set_b(&ctx->d[3], v); cc_logic8(ctx, v); } hle_insn(ctx, 4);  /* 001C28 moveb %d2,%d3 */
	{ uint32_t v = ((ctx->d[2] & 0xFFu) & 0x5u) & 0xFFu; set_b(&ctx->d[2], v); cc_logic8(ctx, v); } hle_insn(ctx, 8);  /* 001C2A andib #5,%d2 */
	{ uint32_t v = ((ctx->d[3] & 0xFFu) & 0xAu) & 0xFFu; set_b(&ctx->d[3], v); cc_logic8(ctx, v); } hle_insn(ctx, 8);  /* 001C2E andib #10,%d3 */
	{ uint32_t v = shf(ctx, 'r', (ctx->d[3] & 0xFFu), 1, 8); set_b(&ctx->d[3], v); } hle_insn(ctx, 8);  /* 001C32 lsrb #1,%d3 */
	{ uint32_t v = ((ctx->d[2] & 0xFFu) | (ctx->d[3] & 0xFFu)) & 0xFFu; set_b(&ctx->d[2], v); cc_logic8(ctx, v); } hle_insn(ctx, 4);  /* 001C34 orb %d3,%d2 */
	{ ctx->d[0] = 0x00000000u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 001C36 moveq #0,%d0 */
	{ uint32_t v = shf(ctx, 'l', (ctx->d[2] & 0xFFu), 6, 8); set_b(&ctx->d[2], v); } hle_insn(ctx, 18);  /* 001C38 lslb #6,%d2 */
	{ unsigned sr = ctx->sr; uint32_t v = ctx->d[0] & 0xFFFFu; { unsigned x = (sr & CC_X) ? 1 : 0, c = (v & 0x8000u) ? 1 : 0; v = ((v << 1) | x) & 0xFFFFu; sr = c ? (sr | CC_X | CC_C) : (sr & ~(CC_X | CC_C)); } set_w(&ctx->d[0], v); cc_set(ctx, CC_X | CC_N | CC_Z | CC_V | CC_C, (sr & (CC_X | CC_C)) | ((v & 0x8000u) ? CC_N : 0) | (v ? 0 : CC_Z)); } hle_insn(ctx, 8);  /* 001C3A roxlw #1,%d0 */
	{ uint32_t v = shf(ctx, 'l', (ctx->d[2] & 0xFFu), 2, 8); set_b(&ctx->d[2], v); } hle_insn(ctx, 10);  /* 001C3C lslb #2,%d2 */
	{ unsigned sr = ctx->sr; uint32_t v = ctx->d[0] & 0xFFFFu; { unsigned x = (sr & CC_X) ? 1 : 0, c = (v & 0x8000u) ? 1 : 0; v = ((v << 1) | x) & 0xFFFFu; sr = c ? (sr | CC_X | CC_C) : (sr & ~(CC_X | CC_C)); } set_w(&ctx->d[0], v); cc_set(ctx, CC_X | CC_N | CC_Z | CC_V | CC_C, (sr & (CC_X | CC_C)) | ((v & 0x8000u) ? CC_N : 0) | (v ? 0 : CC_Z)); } hle_insn(ctx, 8);  /* 001C3E roxlw #1,%d0 */
	{ uint32_t v = shf(ctx, 'l', (ctx->d[1] & 0xFFu), 6, 8); set_b(&ctx->d[1], v); } hle_insn(ctx, 18);  /* 001C40 lslb #6,%d1 */
	{ unsigned sr = ctx->sr; uint32_t v = ctx->d[0] & 0xFFFFu; { unsigned x = (sr & CC_X) ? 1 : 0, c = (v & 0x8000u) ? 1 : 0; v = ((v << 1) | x) & 0xFFFFu; sr = c ? (sr | CC_X | CC_C) : (sr & ~(CC_X | CC_C)); } set_w(&ctx->d[0], v); cc_set(ctx, CC_X | CC_N | CC_Z | CC_V | CC_C, (sr & (CC_X | CC_C)) | ((v & 0x8000u) ? CC_N : 0) | (v ? 0 : CC_Z)); } hle_insn(ctx, 8);  /* 001C42 roxlw #1,%d0 */
	{ uint32_t v = shf(ctx, 'l', (ctx->d[1] & 0xFFu), 2, 8); set_b(&ctx->d[1], v); } hle_insn(ctx, 10);  /* 001C44 lslb #2,%d1 */
	{ unsigned sr = ctx->sr; uint32_t v = ctx->d[0] & 0xFFFFu; { unsigned x = (sr & CC_X) ? 1 : 0, c = (v & 0x8000u) ? 1 : 0; v = ((v << 1) | x) & 0xFFFFu; sr = c ? (sr | CC_X | CC_C) : (sr & ~(CC_X | CC_C)); } set_w(&ctx->d[0], v); cc_set(ctx, CC_X | CC_N | CC_Z | CC_V | CC_C, (sr & (CC_X | CC_C)) | ((v & 0x8000u) ? CC_N : 0) | (v ? 0 : CC_Z)); } hle_insn(ctx, 8);  /* 001C46 roxlw #1,%d0 */
	hle_jump(ctx, 0x001C48); return;  /* 001C48 rts */
L_007406:
	{ uint32_t eaS = 0xFFFFFF9Cu; ctx->a[7] = eaS; } hle_insn(ctx, 12);  /* 007406 lea ffffff9c,%sp */
	hle_jump(ctx, 0x00740C); return;  /* 00740C jsr %pc@(8162) */
L_007410:
	hle_jump(ctx, 0x007410); return;  /* 007410 jsr %pc@(86c4) */
L_007414:
	hle_jump(ctx, 0x007414); return;  /* 007414 jsr %pc@(8b02) */
L_007418:
	hle_jump(ctx, 0x007418); return;  /* 007418 jsr fbb00 */
L_00741E:
	hle_jump(ctx, 0x00741E); return;  /* 00741E jsr %pc@(884c) */
L_007422:
	{ uint32_t eaS = 0x0000869Cu; ctx->a[1] = eaS; } hle_insn(ctx, 8);  /* 007422 lea %pc@(869c),%a1 */
	hle_jump(ctx, 0x007426); return;  /* 007426 jsr %pc@(867a) */
L_00742A:
	hle_jump(ctx, 0x00742A); return;  /* 00742A jsr %pc@(818c) */
L_00742E:
	hle_jump(ctx, 0x00742E); return;  /* 00742E jsr %pc@(800c) */
L_007432:
	{ uint32_t eaD = 0xFFFF9DF5u; int io = !plain(eaD); if (io) io_pre(ctx, 0x13FC, 0x00743A); uint32_t v = 0x5u; st8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 007432 moveb #5,ffff9df5 */
	hle_insn(ctx, 16); ctx->sr = 0x2500u; hle_jump(ctx, 0x00743E); return;  /* 00743A movew #9472,%sr */
L_00743E:
	hle_jump(ctx, 0x00743E); return;  /* 00743E jsr %pc@(86e2) */
L_007442:
	{ uint32_t eaS = 0x00C00004u; int io = !plain(eaS); if (io) io_pre(ctx, 0x3039, 0x007448); uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 007442 movew c00004,%d0 */
	{ unsigned b = 0; uint32_t m = 1u << b; cc_set(ctx, CC_Z, (ctx->d[0] & m) ? 0 : CC_Z);  hle_insn(ctx, 10); }  /* 007448 btst #0,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_007454; } hle_insn(ctx, 8);  /* 00744C bnes 7454 */
	{ uint32_t eaD = 0xFFFFA247u;  int io = !plain(eaD); if (io) io_pre(ctx, 0x50F9, 0x007454); st8(eaD, 0xFF); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 00744E st ffffa247 */
L_007454:
	hle_jump(ctx, 0x007454); return;  /* 007454 jsr %pc@(3ec8) */
L_007458:
	hle_jump(ctx, 0x007458); return;  /* 007458 jmp %pc@(900) */
L_008162:
	{ uint32_t eaS = 0x00C00000u; ctx->a[5] = eaS; } hle_insn(ctx, 12);  /* 008162 lea c00000,%a5 */
	{ uint32_t eaD = ctx->a[5] + (uint32_t)(int32_t)(4); int io = !plain(eaD); if (io) io_pre(ctx, 0x3B7C, 0x00816E); uint32_t v = 0x8F02u; st16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 008168 movew #-28926,%a5@(4) */
	{ uint32_t eaD = ctx->a[5] + (uint32_t)(int32_t)(4); int io = !plain(eaD); if (io) io_pre(ctx, 0x2B7C, 0x008176); uint32_t v = 0xC0000000u; st32(eaD, v); cc_logic32(ctx, v); if (io) hle_post(ctx, 24); else hle_insn(ctx, 24); }  /* 00816E movel #-1073741824,%a5@(4) */
	{ ctx->d[2] = 0x00000000u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 008176 moveq #0,%d2 */
	{ ctx->d[0] = 0x0000003Fu; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 008178 moveq #63,%d0 */
L_00817A:
	{ uint32_t eaS = ctx->a[5] + (uint32_t)(int32_t)(4); int io = !plain(eaS); if (io) io_pre(ctx, 0x322D, 0x00817E); uint32_t v = ld16(eaS); set_w(&ctx->d[1], v); cc_logic16(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 00817A movew %a5@(4),%d1 */
	{ unsigned b = 1; uint32_t m = 1u << b; cc_set(ctx, CC_Z, (ctx->d[1] & m) ? 0 : CC_Z);  hle_insn(ctx, 10); }  /* 00817E btst #1,%d1 */
	if (COND_NE) { hle_insn(ctx, 10); hle_jump(ctx, 0x00817A); return; } hle_insn(ctx, 8);  /* 008182 bnes 817a */
	{ uint32_t eaD = ctx->a[5]; int io = !plain(eaD); if (io) io_pre(ctx, 0x3A82, 0x008186); uint32_t v = (ctx->d[2] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 8); else hle_insn(ctx, 8); }  /* 008184 movew %d2,%a5@ */
	{ uint16_t v = (uint16_t)ctx->d[0] - 1; set_w(&ctx->d[0], v); if (v != 0xFFFF) { hle_insn(ctx, 10); hle_jump(ctx, 0x00817A); return; } hle_insn(ctx, 14); }  /* 008186 dbf %d0,817a */
	hle_jump(ctx, 0x00818A); return;  /* 00818A rts */
L_00818C:
	{ uint32_t eaS = 0x00C00000u; ctx->a[5] = eaS; } hle_insn(ctx, 12);  /* 00818C lea c00000,%a5 */
	{ uint32_t eaD = ctx->a[5] + (uint32_t)(int32_t)(4); int io = !plain(eaD); if (io) io_pre(ctx, 0x3B7C, 0x008198); uint32_t v = 0x8F02u; st16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 008192 movew #-28926,%a5@(4) */
	{ uint32_t eaD = ctx->a[5] + (uint32_t)(int32_t)(4); int io = !plain(eaD); if (io) io_pre(ctx, 0x2B7C, 0x0081A0); uint32_t v = 0x40000010u; st32(eaD, v); cc_logic32(ctx, v); if (io) hle_post(ctx, 24); else hle_insn(ctx, 24); }  /* 008198 movel #1073741840,%a5@(4) */
	{ ctx->d[0] = 0x0000004Fu; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 0081A0 moveq #79,%d0 */
L_0081A2:
	{ uint32_t eaS = ctx->a[5] + (uint32_t)(int32_t)(4); int io = !plain(eaS); if (io) io_pre(ctx, 0x322D, 0x0081A6); uint32_t v = ld16(eaS); set_w(&ctx->d[1], v); cc_logic16(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0081A2 movew %a5@(4),%d1 */
	{ unsigned b = 1; uint32_t m = 1u << b; cc_set(ctx, CC_Z, (ctx->d[1] & m) ? 0 : CC_Z);  hle_insn(ctx, 10); }  /* 0081A6 btst #1,%d1 */
	if (COND_NE) { hle_insn(ctx, 10); hle_jump(ctx, 0x0081A2); return; } hle_insn(ctx, 8);  /* 0081AA bnes 81a2 */
	{ uint32_t eaD = ctx->a[5]; int io = !plain(eaD); if (io) io_pre(ctx, 0x3ABC, 0x0081B0); uint32_t v = 0x0u; st16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0081AC movew #0,%a5@ */
	{ uint16_t v = (uint16_t)ctx->d[0] - 1; set_w(&ctx->d[0], v); if (v != 0xFFFF) { hle_insn(ctx, 10); hle_jump(ctx, 0x0081A2); return; } hle_insn(ctx, 14); }  /* 0081B0 dbf %d0,81a2 */
	hle_jump(ctx, 0x0081B4); return;  /* 0081B4 rts */
L_0081B6:
	{ uint32_t eaS = 0x00C00000u; ctx->a[5] = eaS; } hle_insn(ctx, 12);  /* 0081B6 lea c00000,%a5 */
	{ uint32_t eaD = ctx->a[5] + (uint32_t)(int32_t)(4); int io = !plain(eaD); if (io) io_pre(ctx, 0x3B7C, 0x0081C2); uint32_t v = 0x8F02u; st16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0081BC movew #-28926,%a5@(4) */
	{ uint32_t eaD = ctx->a[5] + (uint32_t)(int32_t)(4); int io = !plain(eaD); if (io) io_pre(ctx, 0x2B7C, 0x0081CA); uint32_t v = 0x40000010u; st32(eaD, v); cc_logic32(ctx, v); if (io) hle_post(ctx, 24); else hle_insn(ctx, 24); }  /* 0081C2 movel #1073741840,%a5@(4) */
L_0081CA:
	{ uint32_t eaS = ctx->a[5] + (uint32_t)(int32_t)(4); int io = !plain(eaS); if (io) io_pre(ctx, 0x342D, 0x0081CE); uint32_t v = ld16(eaS); set_w(&ctx->d[2], v); cc_logic16(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0081CA movew %a5@(4),%d2 */
	{ unsigned b = 1; uint32_t m = 1u << b; cc_set(ctx, CC_Z, (ctx->d[2] & m) ? 0 : CC_Z);  hle_insn(ctx, 10); }  /* 0081CE btst #1,%d2 */
	if (COND_NE) { hle_insn(ctx, 10); hle_jump(ctx, 0x0081CA); return; } hle_insn(ctx, 8);  /* 0081D2 bnes 81ca */
	{ uint32_t eaD = ctx->a[5]; int io = !plain(eaD); if (io) io_pre(ctx, 0x3A80, 0x0081D6); uint32_t v = (ctx->d[0] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 8); else hle_insn(ctx, 8); }  /* 0081D4 movew %d0,%a5@ */
	{ uint32_t eaD = ctx->a[5]; int io = !plain(eaD); if (io) io_pre(ctx, 0x3A81, 0x0081D8); uint32_t v = (ctx->d[1] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 8); else hle_insn(ctx, 8); }  /* 0081D6 movew %d1,%a5@ */
	hle_jump(ctx, 0x0081D8); return;  /* 0081D8 rts */
L_00867A:
	{ uint32_t eaS = 0x00C00004u; ctx->a[5] = eaS; } hle_insn(ctx, 12);  /* 00867A lea c00004,%a5 */
	{ uint32_t eaS = 0xFFFFA0DAu; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 008680 lea ffffa0da,%a0 */
	{ uint32_t eaS = ctx->a[1]; ctx->a[1] += 2; int io = !plain(eaS); if (io) io_pre(ctx, 0x3219, 0x008688); uint32_t v = ld16(eaS); set_w(&ctx->d[1], v); cc_logic16(ctx, v); if (io) hle_post(ctx, 8); else hle_insn(ctx, 8); }  /* 008686 movew %a1@+,%d1 */
	{ uint32_t eaD = ctx->a[0]; ctx->a[0] += 2; int io = !plain(eaD); if (io) io_pre(ctx, 0x30C1, 0x00868A); uint32_t v = (ctx->d[1] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 8); else hle_insn(ctx, 8); }  /* 008688 movew %d1,%a0@+ */
	{ uint32_t eaD = ctx->a[5]; int io = !plain(eaD); if (io) io_pre(ctx, 0x3A81, 0x00868C); uint32_t v = (ctx->d[1] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 8); else hle_insn(ctx, 8); }  /* 00868A movew %d1,%a5@ */
	{ uint32_t eaS = ctx->a[1]; ctx->a[1] += 2; int io = !plain(eaS); if (io) io_pre(ctx, 0x3219, 0x00868E); uint32_t v = ld16(eaS); set_w(&ctx->d[1], v); cc_logic16(ctx, v); if (io) hle_post(ctx, 8); else hle_insn(ctx, 8); }  /* 00868C movew %a1@+,%d1 */
	{ uint32_t eaD = ctx->a[0]; ctx->a[0] += 2; int io = !plain(eaD); if (io) io_pre(ctx, 0x30C1, 0x008690); uint32_t v = (ctx->d[1] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 8); else hle_insn(ctx, 8); }  /* 00868E movew %d1,%a0@+ */
	{ uint32_t eaD = ctx->a[5]; int io = !plain(eaD); if (io) io_pre(ctx, 0x3A81, 0x008692); uint32_t v = (ctx->d[1] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 8); else hle_insn(ctx, 8); }  /* 008690 movew %d1,%a5@ */
L_008692:
	{ uint32_t eaS = ctx->a[1]; ctx->a[1] += 2; int io = !plain(eaS); if (io) io_pre(ctx, 0x3219, 0x008694); uint32_t v = ld16(eaS); set_w(&ctx->d[1], v); cc_logic16(ctx, v); if (io) hle_post(ctx, 8); else hle_insn(ctx, 8); }  /* 008692 movew %a1@+,%d1 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_00869A; } hle_insn(ctx, 8);  /* 008694 beqs 869a */
	{ uint32_t eaD = ctx->a[5]; int io = !plain(eaD); if (io) io_pre(ctx, 0x3A81, 0x008698); uint32_t v = (ctx->d[1] & 0xFFFFu); st16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 8); else hle_insn(ctx, 8); }  /* 008696 movew %d1,%a5@ */
	hle_insn(ctx, 10); hle_jump(ctx, 0x008692); return;  /* 008698 bras 8692 */
L_00869A:
	hle_jump(ctx, 0x00869A); return;  /* 00869A rts */
L_0086F6:
	{ uint32_t eaS = 0x00C00000u; ctx->a[5] = eaS; } hle_insn(ctx, 12);  /* 0086F6 lea c00000,%a5 */
	{ uint32_t eaS = ctx->a[5] + (uint32_t)(int32_t)(4); int io = !plain(eaS); if (io) io_pre(ctx, 0x302D, 0x008700); uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0086FC movew %a5@(4),%d0 */
	{ uint32_t v = 0x67Fu; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 008700 movew #1663,%d1 */
	{ uint32_t eaD = ctx->a[5] + (uint32_t)(int32_t)(4); int io = !plain(eaD); if (io) io_pre(ctx, 0x3B7C, 0x00870A); uint32_t v = 0x8F02u; st16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 008704 movew #-28926,%a5@(4) */
	{ uint32_t eaD = ctx->a[5] + (uint32_t)(int32_t)(4); int io = !plain(eaD); if (io) io_pre(ctx, 0x3B7C, 0x008710); uint32_t v = 0x4100u; st16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 00870A movew #16640,%a5@(4) */
	{ uint32_t eaD = ctx->a[5] + (uint32_t)(int32_t)(4); int io = !plain(eaD); if (io) io_pre(ctx, 0x3B7C, 0x008716); uint32_t v = 0x3u; st16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 008710 movew #3,%a5@(4) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00873A); return;  /* 008716 braw 873a */
L_00871A:
	{ uint32_t eaS = 0x00C00000u; ctx->a[5] = eaS; } hle_insn(ctx, 12);  /* 00871A lea c00000,%a5 */
	{ uint32_t eaS = ctx->a[5] + (uint32_t)(int32_t)(4); int io = !plain(eaS); if (io) io_pre(ctx, 0x302D, 0x008724); uint32_t v = ld16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 008720 movew %a5@(4),%d0 */
	{ uint32_t v = 0x6FFu; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 008724 movew #1791,%d1 */
	{ uint32_t eaD = ctx->a[5] + (uint32_t)(int32_t)(4); int io = !plain(eaD); if (io) io_pre(ctx, 0x3B7C, 0x00872E); uint32_t v = 0x8F02u; st16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 008728 movew #-28926,%a5@(4) */
	{ uint32_t eaD = ctx->a[5] + (uint32_t)(int32_t)(4); int io = !plain(eaD); if (io) io_pre(ctx, 0x3B7C, 0x008734); uint32_t v = 0x4000u; st16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 00872E movew #16384,%a5@(4) */
	{ uint32_t eaD = ctx->a[5] + (uint32_t)(int32_t)(4); int io = !plain(eaD); if (io) io_pre(ctx, 0x3B7C, 0x00873A); uint32_t v = 0x3u; st16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); } hle_jump(ctx, 0x00873A); return;  /* 008734 movew #3,%a5@(4) */
L_00884C:
	{ ctx->d[0] = 0x00000000u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 00884C moveq #0,%d0 */
	{ uint32_t eaD = 0x00A10003u; int io = !plain(eaD); if (io) io_pre(ctx, 0x13C0, 0x008854); uint32_t v = (ctx->d[0] & 0xFFu); st8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 00884E moveb %d0,a10003 */
	{ uint32_t eaD = 0x00A10009u; int io = !plain(eaD); if (io) io_pre(ctx, 0x13FC, 0x00885C); uint32_t v = 0x40u; st8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 008854 moveb #64,a10009 */
	{ uint32_t eaD = 0x00A10013u; int io = !plain(eaD); if (io) io_pre(ctx, 0x13C0, 0x008862); uint32_t v = (ctx->d[0] & 0xFFu); st8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 00885C moveb %d0,a10013 */
	{ uint32_t eaD = 0xFFFF8050u; int io = !plain(eaD); if (io) io_pre(ctx, 0x33FC, 0x00886A); uint32_t v = 0x0u; st16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 008862 movew #0,ffff8050 */
	{ uint32_t eaS = 0xFFFF8050u; uint32_t eaD = 0xFFFF8052u; int io = !plain(eaS) || !plain(eaD); if (io) io_pre(ctx, 0x33F9, 0x008874); uint32_t v = ld16(eaS); st16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 28); else hle_insn(ctx, 28); }  /* 00886A movew ffff8050,ffff8052 */
	hle_jump(ctx, 0x008874); return;  /* 008874 rts */
L_0FBB00:
	hle_insn(ctx, 14); push16(ctx, ctx->sr);  /* 0FBB00 movew %sr,%sp@- */
	hle_insn(ctx, 16); ctx->sr = 0x2700u; hle_jump(ctx, 0x0FBB06); return;  /* 0FBB02 movew #9984,%sr */
L_0FBB06:
	{ uint32_t eaS = 0xFFFF0000u; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 0FBB06 lea ffff0000,%a0 */
	{ uint32_t eaS = 0xFFFF00A6u; ctx->a[1] = eaS; } hle_insn(ctx, 12);  /* 0FBB0C lea ffff00a6,%a1 */
	{ uint32_t v = 0xA5u; set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0FBB12 movew #165,%d7 */
L_0FBB16:
	{ uint32_t eaD = ctx->a[0]; ctx->a[0] += 1; int io = !plain(eaD); if (io) io_pre(ctx, 0x4218, 0x0FBB18); st8(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FBB16 clrb %a0@+ */
	{ uint16_t v = (uint16_t)ctx->d[7] - 1; set_w(&ctx->d[7], v); if (v != 0xFFFF) { hle_insn(ctx, 10); hle_jump(ctx, 0x0FBB16); return; } hle_insn(ctx, 14); }  /* 0FBB18 dbf %d7,fbb16 */
	hle_insn(ctx, 4);  /* 0FBB1C nop */
	hle_jump(ctx, 0x0FBB1E); return;  /* 0FBB1E bsrw fc350 */
L_0FBB22:
	{ uint32_t v = 0x400u; set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0FBB22 movew #1024,%d7 */
L_0FBB26:
	{ uint16_t v = (uint16_t)ctx->d[7] - 1; set_w(&ctx->d[7], v); if (v != 0xFFFF) { hle_insn(ctx, 10); hle_jump(ctx, 0x0FBB26); return; } hle_insn(ctx, 14); }  /* 0FBB26 dbf %d7,fbb26 */
	hle_jump(ctx, 0x0FBB2A); return;  /* 0FBB2A bsrw fc408 */
L_0FBB2E:
	{ uint32_t eaD = 0xFFFF0006u; int io = !plain(eaD); if (io) io_pre(ctx, 0x4279, 0x0FBB34); st16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBB2E clrw ffff0006 */
	{ uint32_t eaD = 0x00A10001u;  int io = !plain(eaD); if (io) io_pre(ctx, 0x0839, 0x0FBB3C); unsigned b = 6; uint32_t m = 1u << b; uint8_t v = ld8(eaD); cc_set(ctx, CC_Z, (v & m) ? 0 : CC_Z);  if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBB34 btst #6,a10001 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0FBB44; } hle_insn(ctx, 8);  /* 0FBB3C bnes fbb44 */
	{ uint32_t eaD = 0xFFFF0006u; int io = !plain(eaD); if (io) io_pre(ctx, 0x5279, 0x0FBB44); uint32_t v = alu(ctx, OP_ADD, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBB3E addqw #1,ffff0006 */
L_0FBB44:
	{ ctx->d[0] = 0x00000000u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 0FBB44 moveq #0,%d0 */
	{ uint32_t eaS = 0x00A00005u; int io = !plain(eaS); if (io) io_pre(ctx, 0x1039, 0x0FBB4C); uint32_t v = ld8(eaS); set_b(&ctx->d[0], v); cc_logic8(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FBB46 moveb a00005,%d0 */
	{ uint32_t v = ctx->d[0] & 0xFFFFFFFFu; unsigned c = 0; for (int k = 0; k < 8; k++) { c = (v & 0x80000000u) ? 1 : 0; v = ((v << 1) | c) & 0xFFFFFFFFu; } ctx->d[0] = v; cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, (c ? CC_C : 0) | ((v & 0x80000000u) ? CC_N : 0) | (v ? 0 : CC_Z)); } hle_insn(ctx, 24);  /* 0FBB4C roll #8,%d0 */
	{ uint32_t eaS = 0x00A00004u; int io = !plain(eaS); if (io) io_pre(ctx, 0x1039, 0x0FBB54); uint32_t v = ld8(eaS); set_b(&ctx->d[0], v); cc_logic8(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FBB4E moveb a00004,%d0 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFFFFFu), 0xA00000u, 32, 1); ctx->d[0] = v; } hle_insn(ctx, 16);  /* 0FBB54 addil #10485760,%d0 */
	{ uint32_t eaD = 0xFFFF0000u; int io = !plain(eaD); if (io) io_pre(ctx, 0x23C0, 0x0FBB60); uint32_t v = (ctx->d[0] & 0xFFFFFFFFu); st32(eaD, v); cc_logic32(ctx, v); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBB5A movel %d0,ffff0000 */
	hle_jump(ctx, 0x0FBB60); return;  /* 0FBB60 bsrw fc428 */
L_0FBB64:
	hle_jump(ctx, 0x0FBB64); return;  /* 0FBB64 bsrw fc438 */
L_0FBB68:
	{ uint16_t v = pop16(ctx); hle_insn(ctx, 16); ctx->sr = v; hle_jump(ctx, 0x0FBB6A); return; }  /* 0FBB68 movew %sp@+,%sr */
L_0FBB6A:
	hle_jump(ctx, 0x0FBB6A); return;  /* 0FBB6A rts */
L_0FC32E:
	{ uint32_t eaD = 0x00A11100u; int io = !plain(eaD); if (io) io_pre(ctx, 0x33FC, 0x0FC336); uint32_t v = 0x100u; st16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC32E movew #256,a11100 */
	{ uint32_t eaD = 0x00A11200u; int io = !plain(eaD); if (io) io_pre(ctx, 0x33FC, 0x0FC33E); uint32_t v = 0x0u; st16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC336 movew #0,a11200 */
	{ ctx->d[0] = 0x00000009u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 0FC33E moveq #9,%d0 */
L_0FC340:
	hle_insn(ctx, 4);  /* 0FC340 nop */
	{ uint16_t v = (uint16_t)ctx->d[0] - 1; set_w(&ctx->d[0], v); if (v != 0xFFFF) { hle_insn(ctx, 10); hle_jump(ctx, 0x0FC340); return; } hle_insn(ctx, 14); }  /* 0FC342 dbf %d0,fc340 */
	{ uint32_t eaD = 0x00A11200u; int io = !plain(eaD); if (io) io_pre(ctx, 0x33FC, 0x0FC34E); uint32_t v = 0x100u; st16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC346 movew #256,a11200 */
	hle_jump(ctx, 0x0FC34E); return;  /* 0FC34E rts */
L_0FC350:
	hle_jump(ctx, 0x0FC350); return;  /* 0FC350 bsrs fc32e */
L_0FC352:
	{ uint32_t eaS = 0x00A00000u; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 0FC352 lea a00000,%a0 */
	{ uint32_t eaS = 0x000FC312u; ctx->a[1] = eaS; } hle_insn(ctx, 8);  /* 0FC358 lea %pc@(fc312),%a1 */
	{ ctx->d[7] = 0x00000005u; cc_logic32(ctx, ctx->d[7]); } hle_insn(ctx, 4);  /* 0FC35C moveq #5,%d7 */
L_0FC35E:
	{ uint32_t eaS = ctx->a[1]; ctx->a[1] += 1; uint32_t eaD = ctx->a[0]; ctx->a[0] += 1; int io = !plain(eaS) || !plain(eaD); if (io) io_pre(ctx, 0x10D9, 0x0FC360); uint32_t v = ld8(eaS); st8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FC35E moveb %a1@+,%a0@+ */
	{ uint16_t v = (uint16_t)ctx->d[7] - 1; set_w(&ctx->d[7], v); if (v != 0xFFFF) { hle_insn(ctx, 10); hle_jump(ctx, 0x0FC35E); return; } hle_insn(ctx, 14); }  /* 0FC360 dbf %d7,fc35e */
	{ uint32_t v = 0x0u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0FC364 movew #0,%d0 */
	{ uint32_t eaD = ctx->a[0]; ctx->a[0] += 1; int io = !plain(eaD); if (io) io_pre(ctx, 0x10C0, 0x0FC36A); uint32_t v = (ctx->d[0] & 0xFFu); st8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 8); else hle_insn(ctx, 8); }  /* 0FC368 moveb %d0,%a0@+ */
	{ uint32_t v = shf(ctx, 'r', (ctx->d[0] & 0xFFFFu), 8, 16); set_w(&ctx->d[0], v); } hle_insn(ctx, 22);  /* 0FC36A lsrw #8,%d0 */
	{ uint32_t eaD = ctx->a[0]; ctx->a[0] += 1; int io = !plain(eaD); if (io) io_pre(ctx, 0x10C0, 0x0FC36E); uint32_t v = (ctx->d[0] & 0xFFu); st8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 8); else hle_insn(ctx, 8); }  /* 0FC36C moveb %d0,%a0@+ */
	{ ctx->d[7] = 0x00000014u; cc_logic32(ctx, ctx->d[7]); } hle_insn(ctx, 4);  /* 0FC36E moveq #20,%d7 */
L_0FC370:
	{ uint32_t eaS = ctx->a[1]; ctx->a[1] += 1; uint32_t eaD = ctx->a[0]; ctx->a[0] += 1; int io = !plain(eaS) || !plain(eaD); if (io) io_pre(ctx, 0x10D9, 0x0FC372); uint32_t v = ld8(eaS); st8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FC370 moveb %a1@+,%a0@+ */
	{ uint16_t v = (uint16_t)ctx->d[7] - 1; set_w(&ctx->d[7], v); if (v != 0xFFFF) { hle_insn(ctx, 10); hle_jump(ctx, 0x0FC370); return; } hle_insn(ctx, 14); }  /* 0FC372 dbf %d7,fc370 */
	hle_jump(ctx, 0x0FC376); return;  /* 0FC376 bsrw fc3e6 */
L_0FC37A:
	{ ctx->d[7] = 0x00000063u; cc_logic32(ctx, ctx->d[7]); } hle_insn(ctx, 4);  /* 0FC37A moveq #99,%d7 */
L_0FC37C:
	{ uint16_t v = (uint16_t)ctx->d[7] - 1; set_w(&ctx->d[7], v); if (v != 0xFFFF) { hle_insn(ctx, 10); hle_jump(ctx, 0x0FC37C); return; } hle_insn(ctx, 14); }  /* 0FC37C dbf %d7,fc37c */
	{ uint32_t eaD = 0x00A11100u; int io = !plain(eaD); if (io) io_pre(ctx, 0x33FC, 0x0FC388); uint32_t v = 0x100u; st16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC380 movew #256,a11100 */
L_0FC388:
	{ uint32_t eaD = 0x00A11100u;  int io = !plain(eaD); if (io) io_pre(ctx, 0x0839, 0x0FC390); unsigned b = 0; uint32_t m = 1u << b; uint8_t v = ld8(eaD); cc_set(ctx, CC_Z, (v & m) ? 0 : CC_Z);  if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC388 btst #0,a11100 */
	if (COND_NE) { hle_insn(ctx, 10); hle_jump(ctx, 0x0FC388); return; } hle_insn(ctx, 8);  /* 0FC390 bnes fc388 */
	{ uint32_t eaS = 0x00A01000u; int io = !plain(eaS); if (io) io_pre(ctx, 0x1039, 0x0FC398); uint32_t v = ld8(eaS); set_b(&ctx->d[0], v); cc_logic8(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC392 moveb a01000,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0FC3A4; } hle_insn(ctx, 8);  /* 0FC398 bnes fc3a4 */
	{ uint32_t eaD = 0x00A11100u; int io = !plain(eaD); if (io) io_pre(ctx, 0x33FC, 0x0FC3A2); uint32_t v = 0x0u; st16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC39A movew #0,a11100 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x0FC37A); return;  /* 0FC3A2 bras fc37a */
L_0FC3A4:
	hle_jump(ctx, 0x0FC3A4); return;  /* 0FC3A4 bsrw fc32e */
L_0FC3A8:
	{ uint32_t eaS = 0x000FC5CCu; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 0FC3A8 lea %pc@(fc5cc),%a0 */
	{ uint32_t eaS = 0x00A00C00u; ctx->a[1] = eaS; } hle_insn(ctx, 12);  /* 0FC3AC lea a00c00,%a1 */
	{ uint32_t v = 0x13FFu; set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0FC3B2 movew #5119,%d7 */
L_0FC3B6:
	{ uint32_t eaS = ctx->a[0]; ctx->a[0] += 1; uint32_t eaD = ctx->a[1]; ctx->a[1] += 1; int io = !plain(eaS) || !plain(eaD); if (io) io_pre(ctx, 0x12D8, 0x0FC3B8); uint32_t v = ld8(eaS); st8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FC3B6 moveb %a0@+,%a1@+ */
	{ uint16_t v = (uint16_t)ctx->d[7] - 1; set_w(&ctx->d[7], v); if (v != 0xFFFF) { hle_insn(ctx, 10); hle_jump(ctx, 0x0FC3B6); return; } hle_insn(ctx, 14); }  /* 0FC3B8 dbf %d7,fc3b6 */
	{ uint32_t eaD = 0x00A00000u; int io = !plain(eaD); if (io) io_pre(ctx, 0x13FC, 0x0FC3C4); uint32_t v = 0xF3u; st8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC3BC moveb #-13,a00000 */
	{ uint32_t eaD = 0x00A00001u; int io = !plain(eaD); if (io) io_pre(ctx, 0x13FC, 0x0FC3CC); uint32_t v = 0xC3u; st8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC3C4 moveb #-61,a00001 */
	{ uint32_t eaD = 0x00A00002u; int io = !plain(eaD); if (io) io_pre(ctx, 0x13FC, 0x0FC3D4); uint32_t v = 0x0u; st8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC3CC moveb #0,a00002 */
	{ uint32_t eaD = 0x00A00003u; int io = !plain(eaD); if (io) io_pre(ctx, 0x13FC, 0x0FC3DC); uint32_t v = 0xCu; st8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC3D4 moveb #12,a00003 */
	hle_jump(ctx, 0x0FC3DC); return;  /* 0FC3DC bsrs fc3e6 */
L_0FC3DE:
	{ uint32_t eaD = 0xFFFF009Eu;  int io = !plain(eaD); if (io) io_pre(ctx, 0x51F9, 0x0FC3E4); st8(eaD, 0); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC3DE sf ffff009e */
	hle_jump(ctx, 0x0FC3E4); return;  /* 0FC3E4 rts */
L_0FC3E6:
	{ uint32_t eaD = 0x00A11200u; int io = !plain(eaD); if (io) io_pre(ctx, 0x33FC, 0x0FC3EE); uint32_t v = 0x0u; st16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC3E6 movew #0,a11200 */
	{ uint32_t eaD = 0x00A11100u; int io = !plain(eaD); if (io) io_pre(ctx, 0x33FC, 0x0FC3F6); uint32_t v = 0x0u; st16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC3EE movew #0,a11100 */
	{ ctx->d[0] = 0x00000009u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 0FC3F6 moveq #9,%d0 */
L_0FC3F8:
	hle_insn(ctx, 4);  /* 0FC3F8 nop */
	{ uint16_t v = (uint16_t)ctx->d[0] - 1; set_w(&ctx->d[0], v); if (v != 0xFFFF) { hle_insn(ctx, 10); hle_jump(ctx, 0x0FC3F8); return; } hle_insn(ctx, 14); }  /* 0FC3FA dbf %d0,fc3f8 */
	{ uint32_t eaD = 0x00A11200u; int io = !plain(eaD); if (io) io_pre(ctx, 0x33FC, 0x0FC406); uint32_t v = 0x100u; st16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC3FE movew #256,a11200 */
	hle_jump(ctx, 0x0FC406); return;  /* 0FC406 rts */
L_0FC438:
	{ uint32_t eaS = 0x000FB2E2u; ctx->a[1] = eaS; } hle_insn(ctx, 8);  /* 0FC438 lea %pc@(fb2e2),%a1 */
	hle_jump(ctx, 0x0FC43C); return;  /* 0FC43C bsrw fc59c */
L_0FC440:
	{ uint32_t eaS = 0x00A00400u; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 0FC440 lea a00400,%a0 */
	{ uint32_t v = 0x7FFu; set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0FC446 movew #2047,%d7 */
L_0FC44A:
	{ uint32_t eaS = ctx->a[1]; ctx->a[1] += 1; uint32_t eaD = ctx->a[0]; ctx->a[0] += 1; int io = !plain(eaS) || !plain(eaD); if (io) io_pre(ctx, 0x10D9, 0x0FC44C); uint32_t v = ld8(eaS); st8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FC44A moveb %a1@+,%a0@+ */
	{ uint16_t v = (uint16_t)ctx->d[7] - 1; set_w(&ctx->d[7], v); if (v != 0xFFFF) { hle_insn(ctx, 10); hle_jump(ctx, 0x0FC44A); return; } hle_insn(ctx, 14); }  /* 0FC44C dbf %d7,fc44a */
	{ uint32_t eaD = ctx->a[6]; int io = !plain(eaD); if (io) io_pre(ctx, 0x1CBC, 0x0FC454); uint32_t v = 0x2u; st8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FC450 moveb #2,%fp@ */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(24); ctx->a[6] = eaS; } hle_insn(ctx, 8);  /* 0FC454 lea %fp@(24),%fp */
	{ uint32_t eaS = 0x000F8000u; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 0FC458 lea f8000,%a0 */
	{ uint32_t v = 0x1Fu; set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0FC45E movew #31,%d7 */
L_0FC462:
	{ uint32_t eaS = ctx->a[0]; ctx->a[0] += 1; uint32_t eaD = ctx->a[6]; ctx->a[6] += 1; int io = !plain(eaS) || !plain(eaD); if (io) io_pre(ctx, 0x1CD8, 0x0FC464); uint32_t v = ld8(eaS); st8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FC462 moveb %a0@+,%fp@+ */
	{ uint16_t v = (uint16_t)ctx->d[7] - 1; set_w(&ctx->d[7], v); if (v != 0xFFFF) { hle_insn(ctx, 10); hle_jump(ctx, 0x0FC462); return; } hle_insn(ctx, 14); }  /* 0FC464 dbf %d7,fc462 */
	{ uint32_t eaS = 0xFFFF0000u; int io = !plain(eaS); if (io) io_pre(ctx, 0x2079, 0x0FC46E); ctx->a[0] = ld32(eaS); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC468 moveal ffff0000,%a0 */
	{ uint32_t eaD = ctx->a[0] + (uint32_t)(int32_t)(68); int io = !plain(eaD); if (io) io_pre(ctx, 0x117C, 0x0FC474); uint32_t v = 0x1Eu; st8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC46E moveb #30,%a0@(68) */
	hle_jump(ctx, 0x0FC474); return;  /* 0FC474 bsrw fc428 */
L_0FC478:
	hle_jump(ctx, 0x0FC478); return;  /* 0FC478 rts */
}

HLE_RANGE(0x000200, bt_Reset, 1114, 0x000238, 0x00024A, 0x000250, 0x00025C, 0x000268, 0x000272, 0x00027A, 0x00028C, 0x000300) { boot_run(ctx, 0x000200); block_end(); }
HLE_RANGE(0x000238, bt_Boot_000238, 744, 0x000238, 0x00024A, 0x000250, 0x00025C, 0x000268, 0x000272, 0x00027A, 0x00028C) { boot_run(ctx, 0x000238); block_end(); }
HLE_RANGE(0x00024A, bt_Boot_00024A, 618, 0x00024A, 0x000250, 0x00025C, 0x000268, 0x000272, 0x00027A, 0x00028C) { boot_run(ctx, 0x00024A); block_end(); }
HLE_RANGE(0x000250, bt_Boot_000250, 578, 0x000250, 0x00025C, 0x000268, 0x000272, 0x00027A, 0x00028C) { boot_run(ctx, 0x000250); block_end(); }
HLE_RANGE(0x00025C, bt_Boot_00025C, 498, 0x00025C, 0x000268, 0x000272, 0x00027A, 0x00028C) { boot_run(ctx, 0x00025C); block_end(); }
HLE_RANGE(0x000268, bt_Boot_000268, 398, 0x000268, 0x000272, 0x00027A, 0x00028C) { boot_run(ctx, 0x000268); block_end(); }
HLE_RANGE(0x000272, bt_Boot_000272, 324, 0x000272, 0x00027A, 0x00028C) { boot_run(ctx, 0x000272); block_end(); }
HLE_RANGE(0x00027A, bt_Boot_00027A, 276, 0x00027A, 0x00028C) { boot_run(ctx, 0x00027A); block_end(); }
HLE_RANGE(0x00028C, bt_Boot_00028C, 116, 0x000300) { boot_run(ctx, 0x00028C); block_end(); }
HLE_RANGE(0x001BE8, bt_ReadPort2Id, 564, 0x001C48) { boot_run(ctx, 0x001BE8); block_end(); }
HLE_RANGE(0x007406, bt_Boot, 102, 0x00740C) { boot_run(ctx, 0x007406); block_end(); }
HLE_RANGE(0x007422, bt_Boot_007422, 98, 0x007426) { boot_run(ctx, 0x007422); block_end(); }
HLE_RANGE(0x007432, bt_Boot_007432, 108, 0x00743E) { boot_run(ctx, 0x007432); block_end(); }
HLE_RANGE(0x007442, bt_Boot_007442, 164, 0x007454) { boot_run(ctx, 0x007442); block_end(); }
HLE_RANGE(0x008162, bt_CramClear, 256, 0x00817A, 0x00818A) { boot_run(ctx, 0x008162); block_end(); }
HLE_RANGE(0x00817A, bt_Boot_00817A, 166, 0x00817A, 0x00818A) { boot_run(ctx, 0x00817A); block_end(); }
HLE_RANGE(0x00818C, bt_VsramClear, 250, 0x0081A2, 0x0081B4) { boot_run(ctx, 0x00818C); block_end(); }
HLE_RANGE(0x0081A2, bt_Boot_0081A2, 170, 0x0081A2, 0x0081B4) { boot_run(ctx, 0x0081A2); block_end(); }
HLE_RANGE(0x0081B6, bt_VdpSetScroll2, 230, 0x0081CA, 0x0081D8) { boot_run(ctx, 0x0081B6); block_end(); }
HLE_RANGE(0x0081CA, bt_Boot_0081CA, 160, 0x0081CA, 0x0081D8) { boot_run(ctx, 0x0081CA); block_end(); }
HLE_RANGE(0x00867A, bt_VdpSetRegs, 262, 0x008692, 0x00869A) { boot_run(ctx, 0x00867A); block_end(); }
HLE_RANGE(0x008692, bt_Boot_008692, 142, 0x008692, 0x00869A) { boot_run(ctx, 0x008692); block_end(); }
HLE_RANGE(0x0086F6, bt_VramClearAlt, 192, 0x00873A) { boot_run(ctx, 0x0086F6); block_end(); }
HLE_RANGE(0x00871A, bt_VramClear, 176, 0x00873A) { boot_run(ctx, 0x00871A); block_end(); }
HLE_RANGE(0x00884C, bt_InitPort1, 222, 0x008874) { boot_run(ctx, 0x00884C); block_end(); }
HLE_RANGE(0x0FBB00, bt_SndBoot, 102, HLE_EXIT_SP(0x0FBB06, -2)) { boot_run(ctx, 0x0FBB00); block_end(); }
HLE_RANGE(0x0FBB06, bt_Boot_0FBB06, 182, 0x0FBB16, 0x0FBB1E) { boot_run(ctx, 0x0FBB06); block_end(); }
HLE_RANGE(0x0FBB16, bt_Boot_0FBB16, 132, 0x0FBB16, 0x0FBB1E) { boot_run(ctx, 0x0FBB16); block_end(); }
HLE_RANGE(0x0FBB22, bt_Boot_0FBB22, 118, 0x0FBB26, 0x0FBB2A) { boot_run(ctx, 0x0FBB22); block_end(); }
HLE_RANGE(0x0FBB26, bt_Boot_0FBB26, 104, 0x0FBB26, 0x0FBB2A) { boot_run(ctx, 0x0FBB26); block_end(); }
HLE_RANGE(0x0FBB2E, bt_Boot_0FBB2E, 310, 0x0FBB60) { boot_run(ctx, 0x0FBB2E); block_end(); }
HLE_RANGE(0x0FBB68, bt_Boot_0FBB68, 82, HLE_EXIT_SP(0x0FBB6A, 2)) { boot_run(ctx, 0x0FBB68); block_end(); }
HLE_RANGE(0x0FC32E, bt_Z80Reset, 200, 0x0FC340, 0x0FC34E) { boot_run(ctx, 0x0FC32E); block_end(); }
HLE_RANGE(0x0FC340, bt_Boot_0FC340, 138, 0x0FC340, 0x0FC34E) { boot_run(ctx, 0x0FC340); block_end(); }
HLE_RANGE(0x0FC352, bt_Boot_0FC352, 282, 0x0FC35E, 0x0FC370, 0x0FC376) { boot_run(ctx, 0x0FC352); block_end(); }
HLE_RANGE(0x0FC35E, bt_Boot_0FC35E, 240, 0x0FC35E, 0x0FC370, 0x0FC376) { boot_run(ctx, 0x0FC35E); block_end(); }
HLE_RANGE(0x0FC370, bt_Boot_0FC370, 122, 0x0FC370, 0x0FC376) { boot_run(ctx, 0x0FC370); block_end(); }
HLE_RANGE(0x0FC37A, bt_Boot_0FC37A, 262, 0x0FC37A, 0x0FC37C, 0x0FC388, 0x0FC3A4) { boot_run(ctx, 0x0FC37A); block_end(); }
HLE_RANGE(0x0FC37C, bt_Boot_0FC37C, 252, 0x0FC37A, 0x0FC37C, 0x0FC388, 0x0FC3A4) { boot_run(ctx, 0x0FC37C); block_end(); }
HLE_RANGE(0x0FC388, bt_Boot_0FC388, 206, 0x0FC37A, 0x0FC388, 0x0FC3A4) { boot_run(ctx, 0x0FC388); block_end(); }
HLE_RANGE(0x0FC3A8, bt_Boot_0FC3A8, 272, 0x0FC3B6, 0x0FC3DC) { boot_run(ctx, 0x0FC3A8); block_end(); }
HLE_RANGE(0x0FC3B6, bt_Boot_0FC3B6, 226, 0x0FC3B6, 0x0FC3DC) { boot_run(ctx, 0x0FC3B6); block_end(); }
HLE_RANGE(0x0FC3DE, bt_Boot_0FC3DE, 108, 0x0FC3E4) { boot_run(ctx, 0x0FC3DE); block_end(); }
HLE_RANGE(0x0FC3E6, bt_Z80Reset2, 200, 0x0FC3F8, 0x0FC406) { boot_run(ctx, 0x0FC3E6); block_end(); }
HLE_RANGE(0x0FC3F8, bt_Boot_0FC3F8, 138, 0x0FC3F8, 0x0FC406) { boot_run(ctx, 0x0FC3F8); block_end(); }
HLE_RANGE(0x0FC438, bt_SndBoot2, 98, 0x0FC43C) { boot_run(ctx, 0x0FC438); block_end(); }
HLE_RANGE(0x0FC440, bt_Boot_0FC440, 304, 0x0FC44A, 0x0FC462, 0x0FC474) { boot_run(ctx, 0x0FC440); block_end(); }
HLE_RANGE(0x0FC44A, bt_Boot_0FC44A, 272, 0x0FC44A, 0x0FC462, 0x0FC474) { boot_run(ctx, 0x0FC44A); block_end(); }
HLE_RANGE(0x0FC462, bt_Boot_0FC462, 170, 0x0FC462, 0x0FC474) { boot_run(ctx, 0x0FC462); block_end(); }
