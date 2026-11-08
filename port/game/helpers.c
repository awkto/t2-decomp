/* Small helpers that were still 68k (docs/remaining.md): the per-layer background stream wrappers
 * BgStreamL5/L2/L4/L1 ($004820..$00498A, they set the layer tables and `bsr` into BgStreamLayers), the HUD bar
 * helper $00920C, the animation helper $00C442, the truck helper $00D238 and the object helper $00DB36.
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
	case 0x004820: goto L_004820;
	case 0x004854: goto L_004854;
	case 0x004872: goto L_004872;
	case 0x0048A6: goto L_0048A6;
	case 0x0048C4: goto L_0048C4;
	case 0x0048F8: goto L_0048F8;
	case 0x004916: goto L_004916;
	case 0x004960: goto L_004960;
	case 0x00920C: goto L_00920C;
	case 0x009224: goto L_009224;
	case 0x00C446: goto L_00C446;
	case 0x00C46A: goto L_00C46A;
	case 0x00C48A: goto L_00C48A;
	case 0x00D238: goto L_00D238;
	case 0x00DB3A: goto L_00DB3A;
	case 0x00DB40: goto L_00DB40;
	case 0x00DB56: goto L_00DB56;
	default: __builtin_trap();
	}
L_004820:
	{ uint32_t eaS = 0xFFFF9E98u; ctx->a[0] = ld32(eaS); } hle_insn(ctx, 20);  /* 004820 moveal ffff9e98,%a0 */
	{ uint32_t eaS = 0xFFFFAFD2u; ctx->a[1] = eaS; } hle_insn(ctx, 12);  /* 004826 lea ffffafd2,%a1 */
	{ uint32_t eaS = 0x00005302u; ctx->a[2] = eaS; } hle_insn(ctx, 8);  /* 00482C lea %pc@(5302),%a2 */
	{ uint32_t eaS = 0x000053FEu; ctx->a[4] = eaS; } hle_insn(ctx, 8);  /* 004830 lea %pc@(53fe),%a4 */
	{ uint32_t eaS = 0x0000542Au; ctx->a[5] = eaS; } hle_insn(ctx, 8);  /* 004834 lea %pc@(542a),%a5 */
	{ uint32_t eaS = 0xFFFFAE5Cu; ctx->a[6] = eaS; } hle_insn(ctx, 12);  /* 004838 lea ffffae5c,%fp */
	{ uint32_t v = 0xE000u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00483E movew #-8192,%d0 */
	{ uint32_t v = 0x40u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 004842 movew #64,%d1 */
	{ ctx->d[2] = 0x00000008u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 004846 moveq #8,%d2 */
	{ uint32_t eaD = 0xFFFFA0AAu; uint32_t v = 0xE000u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 20);  /* 004848 movew #-8192,ffffa0aa */
	hle_jump(ctx, 0x004850); return;  /* 004850 bsrw 498a */
L_004854:
	{ uint32_t eaS = 0x000053A6u; ctx->a[2] = eaS; } hle_insn(ctx, 8);  /* 004854 lea %pc@(53a6),%a2 */
	{ uint32_t eaS = 0xFFFFB00Au; ctx->a[1] = eaS; } hle_insn(ctx, 12);  /* 004858 lea ffffb00a,%a1 */
	{ uint32_t v = 0xC000u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00485E movew #-16384,%d0 */
	{ uint32_t v = 0x80u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 004862 movew #128,%d1 */
	{ ctx->d[2] = 0x00000010u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 004866 moveq #16,%d2 */
	{ uint32_t eaD = 0xFFFFA0AAu; st16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 20);  /* 004868 clrw ffffa0aa */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00498A); return;  /* 00486E braw 498a */
L_004872:
	{ uint32_t eaS = 0xFFFF9E98u; ctx->a[0] = ld32(eaS); } hle_insn(ctx, 20);  /* 004872 moveal ffff9e98,%a0 */
	{ uint32_t eaS = 0xFFFFAFD2u; ctx->a[1] = eaS; } hle_insn(ctx, 12);  /* 004878 lea ffffafd2,%a1 */
	{ uint32_t eaS = 0x0000522Eu; ctx->a[2] = eaS; } hle_insn(ctx, 8);  /* 00487E lea %pc@(522e),%a2 */
	{ uint32_t eaS = 0x000052C0u; ctx->a[4] = eaS; } hle_insn(ctx, 8);  /* 004882 lea %pc@(52c0),%a4 */
	{ uint32_t eaS = 0x000052ECu; ctx->a[5] = eaS; } hle_insn(ctx, 8);  /* 004886 lea %pc@(52ec),%a5 */
	{ uint32_t eaS = 0xFFFFAE5Cu; ctx->a[6] = eaS; } hle_insn(ctx, 12);  /* 00488A lea ffffae5c,%fp */
	{ uint32_t v = 0xE000u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 004890 movew #-8192,%d0 */
	{ uint32_t v = 0x40u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 004894 movew #64,%d1 */
	{ ctx->d[2] = 0x00000008u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 004898 moveq #8,%d2 */
	{ uint32_t eaD = 0xFFFFA0AAu; uint32_t v = 0x8000u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 20);  /* 00489A movew #-32768,ffffa0aa */
	hle_jump(ctx, 0x0048A2); return;  /* 0048A2 bsrw 498a */
L_0048A6:
	{ uint32_t eaS = 0x00005256u; ctx->a[2] = eaS; } hle_insn(ctx, 8);  /* 0048A6 lea %pc@(5256),%a2 */
	{ uint32_t eaS = 0xFFFFB00Au; ctx->a[1] = eaS; } hle_insn(ctx, 12);  /* 0048AA lea ffffb00a,%a1 */
	{ uint32_t v = 0xC000u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0048B0 movew #-16384,%d0 */
	{ uint32_t v = 0x40u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0048B4 movew #64,%d1 */
	{ ctx->d[2] = 0x00000008u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 0048B8 moveq #8,%d2 */
	{ uint32_t eaD = 0xFFFFA0AAu; st16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 20);  /* 0048BA clrw ffffa0aa */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00498A); return;  /* 0048C0 braw 498a */
L_0048C4:
	{ uint32_t eaS = 0xFFFF9E98u; ctx->a[0] = ld32(eaS); } hle_insn(ctx, 20);  /* 0048C4 moveal ffff9e98,%a0 */
	{ uint32_t eaS = 0xFFFFAFD2u; ctx->a[1] = eaS; } hle_insn(ctx, 12);  /* 0048CA lea ffffafd2,%a1 */
	{ uint32_t eaS = 0x00005026u; ctx->a[2] = eaS; } hle_insn(ctx, 8);  /* 0048D0 lea %pc@(5026),%a2 */
	{ uint32_t eaS = 0x000051A4u; ctx->a[4] = eaS; } hle_insn(ctx, 8);  /* 0048D4 lea %pc@(51a4),%a4 */
	{ uint32_t eaS = 0x00005200u; ctx->a[5] = eaS; } hle_insn(ctx, 8);  /* 0048D8 lea %pc@(5200),%a5 */
	{ uint32_t eaS = 0xFFFFAE5Cu; ctx->a[6] = eaS; } hle_insn(ctx, 12);  /* 0048DC lea ffffae5c,%fp */
	{ uint32_t v = 0xE000u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0048E2 movew #-8192,%d0 */
	{ uint32_t v = 0x40u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 0048E6 movew #64,%d1 */
	{ ctx->d[2] = 0x00000008u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 0048EA moveq #8,%d2 */
	{ uint32_t eaD = 0xFFFFA0AAu; uint32_t v = 0x8000u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 20);  /* 0048EC movew #-32768,ffffa0aa */
	hle_jump(ctx, 0x0048F4); return;  /* 0048F4 bsrw 498a */
L_0048F8:
	{ uint32_t eaS = 0x000050C8u; ctx->a[2] = eaS; } hle_insn(ctx, 8);  /* 0048F8 lea %pc@(50c8),%a2 */
	{ uint32_t eaS = 0xFFFFB00Au; ctx->a[1] = eaS; } hle_insn(ctx, 12);  /* 0048FC lea ffffb00a,%a1 */
	{ uint32_t v = 0xC000u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 004902 movew #-16384,%d0 */
	{ uint32_t v = 0x40u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 004906 movew #64,%d1 */
	{ ctx->d[2] = 0x00000008u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 00490A moveq #8,%d2 */
	{ uint32_t eaD = 0xFFFFA0AAu; st16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 20);  /* 00490C clrw ffffa0aa */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00498A); return;  /* 004912 braw 498a */
L_004916:
	{ uint32_t eaS = 0xFFFF9E98u; ctx->a[0] = ld32(eaS); } hle_insn(ctx, 20);  /* 004916 moveal ffff9e98,%a0 */
	{ uint32_t eaS = 0x00004F48u; ctx->a[4] = eaS; } hle_insn(ctx, 8);  /* 00491C lea %pc@(4f48),%a4 */
	{ uint32_t eaS = 0x00004FDCu; ctx->a[5] = eaS; } hle_insn(ctx, 8);  /* 004920 lea %pc@(4fdc),%a5 */
	{ uint32_t eaD = 0xFFFFA236u; cc_logic16(ctx, ld16(eaD)); } hle_insn(ctx, 16);  /* 004924 tstw ffffa236 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_004962; } hle_insn(ctx, 8);  /* 00492A beqs 4962 */
	{ uint32_t eaS = 0xFFFFAE5Cu; ctx->a[6] = eaS; } hle_insn(ctx, 12);  /* 00492C lea ffffae5c,%fp */
	{ uint32_t eaS = 0xFFFFAFD2u; ctx->a[1] = eaS; } hle_insn(ctx, 12);  /* 004932 lea ffffafd2,%a1 */
	{ uint32_t eaS = 0x00004F08u; ctx->a[2] = eaS; } hle_insn(ctx, 8);  /* 004938 lea %pc@(4f08),%a2 */
	{ uint32_t eaD = 0xFFFFA236u; alu(ctx, OP_SUB, ld16(eaD), 0x1u, 16, 0); } hle_insn(ctx, 20);  /* 00493C cmpiw #1,ffffa236 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_00494A; } hle_insn(ctx, 8);  /* 004944 bnes 494a */
	{ uint32_t eaS = 0x00004EE8u; ctx->a[2] = eaS; } hle_insn(ctx, 8);  /* 004946 lea %pc@(4ee8),%a2 */
L_00494A:
	{ uint32_t v = 0xE000u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00494A movew #-8192,%d0 */
	{ uint32_t v = 0x40u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00494E movew #64,%d1 */
	{ ctx->d[2] = 0x00000008u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 004952 moveq #8,%d2 */
	{ uint32_t eaD = 0xFFFFA0AAu; uint32_t v = 0x8000u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 20);  /* 004954 movew #-32768,ffffa0aa */
	hle_jump(ctx, 0x00495C); return;  /* 00495C bsrw 498a */
L_004960:
	hle_insn(ctx, 10); goto L_00496A;  /* 004960 bras 496a */
L_004962:
	{ uint32_t eaD = ctx->a[0]; ctx->a[0] += 2; cc_logic16(ctx, ld16(eaD)); } hle_insn(ctx, 8);  /* 004962 tstw %a0@+ */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_00496A; } hle_insn(ctx, 8);  /* 004964 beqs 496a */
	{ ctx->a[0] += 2; } hle_insn(ctx, 8);  /* 004966 addql #2,%a0 */
	hle_insn(ctx, 10); goto L_004962;  /* 004968 bras 4962 */
L_00496A:
	{ uint32_t eaS = 0xFFFFAE66u; ctx->a[6] = eaS; } hle_insn(ctx, 12);  /* 00496A lea ffffae66,%fp */
	{ uint32_t eaS = 0xFFFFB00Au; ctx->a[1] = eaS; } hle_insn(ctx, 12);  /* 004970 lea ffffb00a,%a1 */
	{ uint32_t eaS = 0x00004DA4u; ctx->a[2] = eaS; } hle_insn(ctx, 8);  /* 004976 lea %pc@(4da4),%a2 */
	{ uint32_t v = 0xC000u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00497A movew #-16384,%d0 */
	{ uint32_t v = 0x80u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00497E movew #128,%d1 */
	{ ctx->d[2] = 0x00000010u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 004982 moveq #16,%d2 */
	{ uint32_t eaD = 0xFFFFA0AAu; st16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 20); hle_jump(ctx, 0x00498A); return;  /* 004984 clrw ffffa0aa */
L_00920C:
	{ uint32_t eaD = ctx->a[4] + (uint32_t)(int32_t)(36); cc_logic16(ctx, ld16(eaD)); } hle_insn(ctx, 12);  /* 00920C tstw %a4@(36) */
	if (COND_MI) { hle_insn(ctx, 10); goto L_009240; } hle_insn(ctx, 8);  /* 009210 bmis 9240 */
	{ uint32_t v = shf(ctx, 'r', (ctx->d[0] & 0xFFFFu), 4, 16); set_w(&ctx->d[0], v); } hle_insn(ctx, 14);  /* 009212 lsrw #4,%d0 */
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0xFu) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 009214 andiw #15,%d0 */
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 009218 movew %d0,%d1 */
	{ uint32_t eaS = 0xFFFFAF88u; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 00921A lea ffffaf88,%a0 */
	hle_jump(ctx, 0x009220); return;  /* 009220 jsr %pc@(927a) */
L_009224:
	{ ctx->a[1] = (ctx->a[0] & 0xFFFFFFFFu); } hle_insn(ctx, 4);  /* 009224 moveal %a0,%a1 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), 0x1u, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 4);  /* 009226 addqw #1,%d0 */
	{ uint32_t v = shf(ctx, 'r', (ctx->d[0] & 0xFFFFu), 1, 16); set_w(&ctx->d[0], v); } hle_insn(ctx, 8);  /* 009228 lsrw #1,%d0 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x1u, 16, 1); set_w(&ctx->d[0], v); } hle_insn(ctx, 4);  /* 00922A subqw #1,%d0 */
	if (COND_MI) { hle_insn(ctx, 10); goto L_009234; } hle_insn(ctx, 8);  /* 00922C bmis 9234 */
L_00922E:
	{ uint32_t eaD = ctx->a[1]; ctx->a[1] += 1; uint32_t v = (ctx->d[2] & 0xFFu); st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 8);  /* 00922E moveb %d2,%a1@+ */
	{ uint16_t v = (uint16_t)ctx->d[0] - 1; set_w(&ctx->d[0], v); if (v != 0xFFFF) { hle_insn(ctx, 10); goto L_00922E; } hle_insn(ctx, 14); }  /* 009230 dbf %d0,922e */
L_009234:
	{ unsigned b = 0; uint32_t m = 1u << b; cc_set(ctx, CC_Z, (ctx->d[1] & m) ? 0 : CC_Z);  hle_insn(ctx, 10); }  /* 009234 btst #0,%d1 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_00923C; } hle_insn(ctx, 8);  /* 009238 bnes 923c */
	{ uint32_t eaD = ctx->a[1]; uint32_t v = (ctx->d[4] & 0xFFu); st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 8);  /* 00923A moveb %d4,%a1@ */
L_00923C:
	hle_jump(ctx, 0x00923C); return;  /* 00923C jsr %pc@(7eb2) */
L_009240:
	hle_jump(ctx, 0x009240); return;  /* 009240 rts */
L_00C442:
	hle_jump(ctx, 0x00C442); return;  /* 00C442 jsr %pc@(c48e) */
L_00C446:
	{ cc_logic8(ctx, (ctx->d[4] & 0xFFu)); } hle_insn(ctx, 4);  /* 00C446 tstb %d4 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_00C46E; } hle_insn(ctx, 12);  /* 00C448 beqw c46e */
	if (COND_PL) { hle_insn(ctx, 10); goto L_00C46A; } hle_insn(ctx, 12);  /* 00C44C bplw c46a */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(20); uint32_t v = ld16(eaS); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00C450 movew %fp@(20),%d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFFu); set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 4);  /* 00C454 movew %d1,%d2 */
	{ uint32_t v = shf(ctx, 'a', (ctx->d[2] & 0xFFFFu), 4, 16); set_w(&ctx->d[2], v); } hle_insn(ctx, 14);  /* 00C456 asrw #4,%d2 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[2] & 0xFFFFu), 0xDu, 16, 1); set_w(&ctx->d[2], v); } hle_insn(ctx, 8);  /* 00C458 addiw #13,%d2 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(7); uint32_t v = (ctx->d[2] & 0xFFu); st8(eaD, v); cc_logic8(ctx, v); } hle_insn(ctx, 12);  /* 00C45C moveb %d2,%fp@(7) */
	{ uint32_t v = shf(ctx, 'a', (ctx->d[1] & 0xFFFFu), 4, 16); set_w(&ctx->d[1], v); } hle_insn(ctx, 14);  /* 00C460 asrw #4,%d1 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0xB8u, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 00C462 addiw #184,%d1 */
	hle_jump(ctx, 0x00C466); return;  /* 00C466 jsr %pc@(13228) */
L_00C46A:
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 00C46A braw b3be */
L_00C46E:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = 0x14u; st16(eaD, v); cc_logic16(ctx, v); } hle_insn(ctx, 16);  /* 00C46E movew #20,%fp@(36) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(32); uint32_t v = 0xC4EAu; st32(eaD, v); cc_logic32(ctx, v); } hle_insn(ctx, 24);  /* 00C474 movel #50410,%fp@(32) */
	{ uint32_t v = 0x6Au; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00C47C movew #106,%d0 */
	hle_jump(ctx, 0x00C480); return;  /* 00C480 jsr 13cc4 */
L_00C486:
	hle_jump(ctx, 0x00C486); return;  /* 00C486 bsrw 908a */
L_00C48A:
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 00C48A braw b3be */
L_00D238:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(15); uint32_t v = ld8(eaS); set_b(&ctx->d[2], v); cc_logic8(ctx, v); } hle_insn(ctx, 12);  /* 00D238 moveb %fp@(15),%d2 */
	{ set_w(&ctx->d[2], (uint32_t)(int32_t)(int8_t)ctx->d[2]); cc_logic16(ctx, ctx->d[2] & 0xFFFFu); } hle_insn(ctx, 4);  /* 00D23C extw %d2 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(12); uint32_t v = ld8(eaS); set_b(&ctx->d[3], v); cc_logic8(ctx, v); } hle_insn(ctx, 12);  /* 00D23E moveb %fp@(12),%d3 */
L_00D242:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(13); uint32_t v = alu(ctx, OP_ADD, ld8(eaD), (ctx->d[3] & 0xFFu), 8, 1); st8(eaD, v); } hle_insn(ctx, 16);  /* 00D242 addb %d3,%fp@(13) */
	if (COND_CC) { hle_insn(ctx, 10); goto L_00D24C; } hle_insn(ctx, 8);  /* 00D246 bccs d24c */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(4); uint32_t v = alu(ctx, OP_ADD, ld16(eaD), (ctx->d[2] & 0xFFFFu), 16, 1); st16(eaD, v); } hle_insn(ctx, 16);  /* 00D248 addw %d2,%fp@(4) */
L_00D24C:
	{ uint16_t v = (uint16_t)ctx->d[4] - 1; set_w(&ctx->d[4], v); if (v != 0xFFFF) { hle_insn(ctx, 10); goto L_00D242; } hle_insn(ctx, 14); }  /* 00D24C dbf %d4,d242 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(18); uint32_t v = ld8(eaS); set_b(&ctx->d[0], v); cc_logic8(ctx, v); } hle_insn(ctx, 12);  /* 00D250 moveb %fp@(18),%d0 */
	if (COND_MI) { hle_insn(ctx, 10); goto L_00D26A; } hle_insn(ctx, 8);  /* 00D254 bmis d26a */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(12); uint32_t v = alu(ctx, OP_SUB, ld8(eaD), (ctx->d[0] & 0xFFu), 8, 1); st8(eaD, v); } hle_insn(ctx, 16);  /* 00D256 subb %d0,%fp@(12) */
	if (COND_CC) { hle_insn(ctx, 10); goto L_00D278; } hle_insn(ctx, 8);  /* 00D25A bccs d278 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(15); uint32_t v = alu(ctx, OP_SUB, 0, ld8(eaD), 8, 1); st8(eaD, v); } hle_insn(ctx, 16);  /* 00D25C negb %fp@(15) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(18); uint32_t v = alu(ctx, OP_SUB, 0, ld8(eaD), 8, 1); st8(eaD, v); } hle_insn(ctx, 16);  /* 00D260 negb %fp@(18) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(12); st8(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 16);  /* 00D264 clrb %fp@(12) */
	hle_insn(ctx, 10); goto L_00D278;  /* 00D268 bras d278 */
L_00D26A:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(12); uint32_t v = alu(ctx, OP_SUB, ld8(eaD), (ctx->d[0] & 0xFFu), 8, 1); st8(eaD, v); } hle_insn(ctx, 16);  /* 00D26A subb %d0,%fp@(12) */
	if (COND_CS) { hle_insn(ctx, 10); goto L_00D278; } hle_insn(ctx, 8);  /* 00D26E bcss d278 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(12); uint32_t v = alu(ctx, OP_ADD, ld8(eaD), (ctx->d[0] & 0xFFu), 8, 1); st8(eaD, v); } hle_insn(ctx, 16);  /* 00D270 addb %d0,%fp@(12) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(18); st8(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 16);  /* 00D274 clrb %fp@(18) */
L_00D278:
	hle_jump(ctx, 0x00D278); return;  /* 00D278 rts */
L_00DB36:
	hle_jump(ctx, 0x00DB36); return;  /* 00DB36 jsr %pc@(12316) */
L_00DB3A:
	{ ctx->d[0] = 0xFFFFFFD8u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 00DB3A moveq #-40,%d0 */
	hle_jump(ctx, 0x00DB3C); return;  /* 00DB3C jsr %pc@(12332) */
L_00DB40:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = alu(ctx, OP_SUB, ld16(eaD), 0x1u, 16, 1); st16(eaD, v); } hle_insn(ctx, 16);  /* 00DB40 subqw #1,%fp@(36) */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(36); uint32_t v = ld16(eaS); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 12);  /* 00DB44 movew %fp@(36),%d1 */
	{ uint32_t v = shf(ctx, 'r', (ctx->d[1] & 0xFFFFu), 2, 16); set_w(&ctx->d[1], v); } hle_insn(ctx, 10);  /* 00DB48 lsrw #2,%d1 */
	{ uint32_t v = ((ctx->d[1] & 0xFFFFu) & 0x3u) & 0xFFFFu; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8);  /* 00DB4A andiw #3,%d1 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0x170u, 16, 1); set_w(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 00DB4E addiw #368,%d1 */
	hle_jump(ctx, 0x00DB52); return;  /* 00DB52 jsr %pc@(13228) */
L_00DB56:
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return;  /* 00DB56 braw b3be */
}

HLE_RANGE_PURE(0x004820, BgStreamL5, 200, 0x004850) { pc_run(ctx, 0x004820); }
HLE_RANGE_PURE(0x004854, Helper_004854, 200, 0x00498A) { pc_run(ctx, 0x004854); }
HLE_RANGE_PURE(0x004872, BgStreamL2, 200, 0x0048A2) { pc_run(ctx, 0x004872); }
HLE_RANGE_PURE(0x0048A6, Helper_0048A6, 200, 0x00498A) { pc_run(ctx, 0x0048A6); }
HLE_RANGE_PURE(0x0048C4, BgStreamL4, 200, 0x0048F4) { pc_run(ctx, 0x0048C4); }
HLE_RANGE_PURE(0x0048F8, Helper_0048F8, 200, 0x00498A) { pc_run(ctx, 0x0048F8); }
HLE_RANGE_PURE(0x004916, BgStreamL1, 200, 0x00495C, 0x00498A) { pc_run(ctx, 0x004916); }
HLE_RANGE_PURE(0x004960, Helper_004960, 200, 0x00498A) { pc_run(ctx, 0x004960); }
HLE_RANGE_PURE(0x00920C, HudBarHelper, 200, 0x009220, 0x009240) { pc_run(ctx, 0x00920C); }
HLE_RANGE_PURE(0x009224, Helper_009224, 200, 0x00923C) { pc_run(ctx, 0x009224); }
HLE_RANGE_PURE(0x00C446, Helper_00C446, 200, 0x00B3BE, 0x00C466, 0x00C480) { pc_run(ctx, 0x00C446); }
HLE_RANGE_PURE(0x00C46A, Helper_00C46A, 200, 0x00B3BE) { pc_run(ctx, 0x00C46A); }
HLE_RANGE_PURE(0x00C48A, Helper_00C48A, 200, 0x00B3BE) { pc_run(ctx, 0x00C48A); }
HLE_RANGE_PURE(0x00D238, TruckHelper, 200, 0x00D278) { pc_run(ctx, 0x00D238); }
HLE_RANGE_PURE(0x00DB3A, Helper_00DB3A, 200, 0x00DB3C) { pc_run(ctx, 0x00DB3A); }
HLE_RANGE_PURE(0x00DB40, Helper_00DB40, 200, 0x00DB52) { pc_run(ctx, 0x00DB40); }
HLE_RANGE_PURE(0x00DB56, Helper_00DB56, 200, 0x00B3BE) { pc_run(ctx, 0x00DB56); }
