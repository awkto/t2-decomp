/* The 68k side of the sound driver: the per-frame tick $0FBBD0 (called from VBlankInt) and everything it
 * reaches: the tick body $0FBBFE, the channel update $0FBE18 / $0FBDDC / $0FBDF0, the note data fetch
 * $0FBF26 / $0FBF08, the channel event reader $0FBF58, the sfx tick $0FC10C / $0FC194 / $0FC184, the
 * command slots $0FC2AA / $0FC2D6 / $0FC2DA, the Z80 bus request / release $0FC408 / $0FC428 (with the
 * BUSACK spin), the Z80 command helpers $0FC47A / $0FC488 / $0FC496 / $0FC4FA and the wait for the
 * Z80's command byte $0FC59C.
 *
 * Generated instruction by instruction from the disassembly (tools/py/sndgen.py reading the ROM
 * through objdump and the GPGX opcode cycle table; tools/py/costcheck.py checks every charge). All the
 * routines are one function, snd_run(), entered through a switch on the entry address; a `bsr` is a
 * recursive call (the return address is pushed and popped like the original so the stack bytes are
 * the same), `rts` returns, and the branches are gotos, so a fall-through or tail `bra` into another
 * routine is just code order.
 *
 * Every instruction goes through the timeline (hle_insn / hle_pre + hle_post). The Z80 area, the I/O
 * area ($A10001 version, $A11100 BUSREQ) and the VDP are touched through bus_r8..bus_w32 between
 * hle_pre() and hle_post(), once per original instruction and in the original order: the Z80 bus
 * request makes GPGX run the Z80 up to the 68k clock, a Z80 read of the 68k bank stalls the 68k, and
 * the spin loops (BUSACK in $0FC408, the command byte in $0FC59C) are iterated one pass at a time
 * with their own charges. Work RAM and ROM are read and written directly. Whether an access is I/O
 * is decided from the effective address at run time (a6 is the Z80 RAM base read from $FF0000).
 *
 * The functions are not pure (Z80 bus), so the layer judges them by their declared bound; they are
 * always entered with the interrupt mask at 6 (inside VBlankInt), so only the frame-end check uses
 * it. */
#include "hle.h"

#include <stdint.h>

/* longest span of a call (68k cycles); only the frame-end check uses it, see above */
#define SND_BOUND 6000   /* observed max 5356 (SndTick in the difftest sandbox, no Z80 stalls) */

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

/* lsr / lsl / roxr by an immediate count (1..8); returns the result, sets X N Z V C */
static uint32_t shf(hle_ctx *c, int kind, uint32_t v, int n, int bits)
{
	uint32_t m = (bits == 32) ? 0xFFFFFFFFu : ((1u << bits) - 1), sg = 1u << (bits - 1);
	uint32_t res = v & m;
	unsigned cb = 0, x = (c->sr & CC_X) ? 1 : 0;
	for (int k = 0; k < n; k++) {
		if (kind == 0) {            /* lsr */
			cb = res & 1;
			res >>= 1;
			x = cb;
		} else if (kind == 1) {     /* lsl */
			cb = (res & sg) ? 1 : 0;
			res = (res << 1) & m;
			x = cb;
		} else {                    /* roxr */
			cb = res & 1;
			res = (res >> 1) | (x ? sg : 0);
			x = cb;
		}
	}
	unsigned f = (cb ? CC_C : 0) | ((res & sg) ? CC_N : 0) | (res ? 0 : CC_Z);
	if (kind == 2 || n > 0)
		f |= x ? CC_X : 0;
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

/* work RAM and ROM are plain memory; anything else (Z80 area, I/O, VDP) is an I/O access */
static inline int plain(uint32_t a)
{
	a &= 0xFFFFFFu;
	return a >= 0xE00000u || a < 0x400000u;
}
static inline uint8_t r8(uint32_t a)
{
	uint32_t p = a & 0xFFFFFFu;
	if (p >= 0xE00000u) return RAM8(p);
	if (p < 0x400000u) return ROM8(p);
	return bus_r8(p);
}
static inline uint16_t r16(uint32_t a)
{
	uint32_t p = a & 0xFFFFFFu;
	if (p >= 0xE00000u) return RAM16(p);
	if (p < 0x400000u) return ROM16(p);
	return bus_r16(p);
}
static inline uint32_t r32(uint32_t a)
{
	uint32_t p = a & 0xFFFFFFu;
	if (p >= 0xE00000u) return RAM32(p);
	if (p < 0x400000u) return ROM32(p);
	return bus_r32(p);
}
static inline void w8(uint32_t a, uint32_t v)
{
	uint32_t p = a & 0xFFFFFFu;
	if (p >= 0xE00000u) RAM8(p) = (uint8_t)v; else bus_w8(p, (uint8_t)v);
}
static inline void w16(uint32_t a, uint32_t v)
{
	uint32_t p = a & 0xFFFFFFu;
	if (p >= 0xE00000u) RAM16(p) = (uint16_t)v; else bus_w16(p, (uint16_t)v);
}
static inline void w32(uint32_t a, uint32_t v)
{
	uint32_t p = a & 0xFFFFFFu;
	if (p >= 0xE00000u) RAM32_W(p, v); else bus_w32(p, v);
}

/* `nested` is 1 for a routine reached by a bsr of this code (its rts pops the return address the
 * bsr pushed); the top-level entry leaves its own return address to the layer. */
static void snd_run(hle_ctx *ctx, uint32_t entry, int nested)
{
	switch (entry) {
	case 0x0FBBD0: goto L_0FBBD0;
	case 0x0FBBE4: goto L_0FBBE4;
	case 0x0FBBF2: goto L_0FBBF2;
	case 0x0FBBFC: goto L_0FBBFC;
	case 0x0FBBFE: goto L_0FBBFE;
	case 0x0FBC1E: goto L_0FBC1E;
	case 0x0FBC28: goto L_0FBC28;
	case 0x0FBC6A: goto L_0FBC6A;
	case 0x0FBCA0: goto L_0FBCA0;
	case 0x0FBCBC: goto L_0FBCBC;
	case 0x0FBD02: goto L_0FBD02;
	case 0x0FBD60: goto L_0FBD60;
	case 0x0FBD6C: goto L_0FBD6C;
	case 0x0FBD78: goto L_0FBD78;
	case 0x0FBDB6: goto L_0FBDB6;
	case 0x0FBDC0: goto L_0FBDC0;
	case 0x0FBDCC: goto L_0FBDCC;
	case 0x0FBDD0: goto L_0FBDD0;
	case 0x0FBDDC: goto L_0FBDDC;
	case 0x0FBDE2: goto L_0FBDE2;
	case 0x0FBDF0: goto L_0FBDF0;
	case 0x0FBE16: goto L_0FBE16;
	case 0x0FBE18: goto L_0FBE18;
	case 0x0FBE4C: goto L_0FBE4C;
	case 0x0FBE7A: goto L_0FBE7A;
	case 0x0FBEC8: goto L_0FBEC8;
	case 0x0FBEF4: goto L_0FBEF4;
	case 0x0FBEFE: goto L_0FBEFE;
	case 0x0FBF06: goto L_0FBF06;
	case 0x0FBF08: goto L_0FBF08;
	case 0x0FBF26: goto L_0FBF26;
	case 0x0FBF30: goto L_0FBF30;
	case 0x0FBF3C: goto L_0FBF3C;
	case 0x0FBF4A: goto L_0FBF4A;
	case 0x0FBF4C: goto L_0FBF4C;
	case 0x0FBF58: goto L_0FBF58;
	case 0x0FBF68: goto L_0FBF68;
	case 0x0FBF96: goto L_0FBF96;
	case 0x0FBF9A: goto L_0FBF9A;
	case 0x0FBFCE: goto L_0FBFCE;
	case 0x0FBFD2: goto L_0FBFD2;
	case 0x0FBFDA: goto L_0FBFDA;
	case 0x0FBFF2: goto L_0FBFF2;
	case 0x0FBFF4: goto L_0FBFF4;
	case 0x0FC01A: goto L_0FC01A;
	case 0x0FC024: goto L_0FC024;
	case 0x0FC03E: goto L_0FC03E;
	case 0x0FC04E: goto L_0FC04E;
	case 0x0FC06E: goto L_0FC06E;
	case 0x0FC084: goto L_0FC084;
	case 0x0FC096: goto L_0FC096;
	case 0x0FC0A4: goto L_0FC0A4;
	case 0x0FC0A6: goto L_0FC0A6;
	case 0x0FC0F0: goto L_0FC0F0;
	case 0x0FC0FE: goto L_0FC0FE;
	case 0x0FC10C: goto L_0FC10C;
	case 0x0FC128: goto L_0FC128;
	case 0x0FC180: goto L_0FC180;
	case 0x0FC182: goto L_0FC182;
	case 0x0FC184: goto L_0FC184;
	case 0x0FC194: goto L_0FC194;
	case 0x0FC19E: goto L_0FC19E;
	case 0x0FC1B2: goto L_0FC1B2;
	case 0x0FC1B4: goto L_0FC1B4;
	case 0x0FC1C4: goto L_0FC1C4;
	case 0x0FC1F2: goto L_0FC1F2;
	case 0x0FC1F6: goto L_0FC1F6;
	case 0x0FC22C: goto L_0FC22C;
	case 0x0FC22E: goto L_0FC22E;
	case 0x0FC24C: goto L_0FC24C;
	case 0x0FC25E: goto L_0FC25E;
	case 0x0FC27A: goto L_0FC27A;
	case 0x0FC290: goto L_0FC290;
	case 0x0FC2AA: goto L_0FC2AA;
	case 0x0FC2D6: goto L_0FC2D6;
	case 0x0FC2DA: goto L_0FC2DA;
	case 0x0FC310: goto L_0FC310;
	case 0x0FC408: goto L_0FC408;
	case 0x0FC412: goto L_0FC412;
	case 0x0FC428: goto L_0FC428;
	case 0x0FC47A: goto L_0FC47A;
	case 0x0FC488: goto L_0FC488;
	case 0x0FC496: goto L_0FC496;
	case 0x0FC4BA: goto L_0FC4BA;
	case 0x0FC4CA: goto L_0FC4CA;
	case 0x0FC4E0: goto L_0FC4E0;
	case 0x0FC4F4: goto L_0FC4F4;
	case 0x0FC4FA: goto L_0FC4FA;
	case 0x0FC512: goto L_0FC512;
	case 0x0FC538: goto L_0FC538;
	case 0x0FC552: goto L_0FC552;
	case 0x0FC59C: goto L_0FC59C;
	case 0x0FC5A2: goto L_0FC5A2;
	case 0x0FC5CA: goto L_0FC5CA;
	default: __builtin_trap();
	}
L_0FBBD0:
	{ uint32_t eaD = 0x00A11100u;  int io = !plain(eaD); if (io) hle_pre(ctx); uint8_t v = r8(eaD); cc_set(ctx, CC_Z, ((v >> 0) & 1) ? 0 : CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBBD0 btst #0,a11100 */
	{ ctx->a[7] -= 2; uint32_t eaD = ctx->a[7]; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, ctx->sr); if (io) hle_post(ctx, 14); else hle_insn(ctx, 14); }  /* 0FBBD8 movew %sr,%sp@- */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0FBBE4; } hle_insn(ctx, 8);  /* 0FBBDA bnes fbbe4 */
	{ uint32_t eaD = 0x00A11100u; int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = 0x0u; w16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBBDC movew #0,a11100 */
L_0FBBE4:
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FBBE6u); } hle_insn(ctx, 18); snd_run(ctx, 0x0FBBFEu, 1);  /* 0FBBE4 bsrs fbbfe */
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 2; int io = !plain(eaS); if (io) hle_pre(ctx); { ctx->sr = (uint16_t)(r16(eaS) & 0xA71Fu); } if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FBBE6 movew %sp@+,%sr */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0FBBFC; } hle_insn(ctx, 8);  /* 0FBBE8 bnes fbbfc */
	{ uint32_t eaD = 0x00A11100u; int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = 0x100u; w16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBBEA movew #256,a11100 */
L_0FBBF2:
	{ uint32_t eaD = 0x00A11100u;  int io = !plain(eaD); if (io) hle_pre(ctx); uint8_t v = r8(eaD); cc_set(ctx, CC_Z, ((v >> 0) & 1) ? 0 : CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBBF2 btst #0,a11100 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0FBBF2; } hle_insn(ctx, 8);  /* 0FBBFA bnes fbbf2 */
L_0FBBFC:
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FBBFC rts */
L_0FBBFE:
	{ uint32_t eaD = 0xFFFF0006u; int io = !plain(eaD); if (io) hle_pre(ctx); cc_logic16(ctx, r16(eaD)); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FBBFE tstw ffff0006 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0FBC1E; } hle_insn(ctx, 8);  /* 0FBC04 beqs fbc1e */
	{ uint32_t eaD = 0xFFFF0008u; int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = alu(ctx, OP_ADD, r16(eaD), 0x1u, 16, 1); w16(eaD, v); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBC06 addqw #1,ffff0008 */
	{ uint32_t eaD = 0xFFFF0008u; int io = !plain(eaD); if (io) hle_pre(ctx); alu(ctx, OP_SUB, r16(eaD), 0x6u, 16, 0); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBC0C cmpiw #6,ffff0008 */
	if (COND_CS) { hle_insn(ctx, 10); goto L_0FBC1E; } hle_insn(ctx, 8);  /* 0FBC14 bcss fbc1e */
	{ uint32_t eaD = 0xFFFF0008u; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBC16 clrw ffff0008 */
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FBC1C rts */
L_0FBC1E:
	{ uint32_t eaD = 0xFFFF002Eu; int io = !plain(eaD); if (io) hle_pre(ctx); cc_logic8(ctx, r8(eaD)); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FBC1E tstb ffff002e */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0FBC28; } hle_insn(ctx, 8);  /* 0FBC24 beqs fbc28 */
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FBC26 rts */
L_0FBC28:
	{ uint32_t eaD = 0xFFFF000Au; int io = !plain(eaD); if (io) hle_pre(ctx); cc_logic16(ctx, r16(eaD)); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FBC28 tstw ffff000a */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0FBCA0; } hle_insn(ctx, 12);  /* 0FBC2E beqw fbca0 */
	hle_movem_l(ctx, 128, 15); { ctx->a[7] -= 4; w32(ctx->a[7], ctx->a[6]); ctx->a[7] -= 4; w32(ctx->a[7], ctx->a[5]); ctx->a[7] -= 4; w32(ctx->a[7], ctx->a[4]); ctx->a[7] -= 4; w32(ctx->a[7], ctx->a[3]); ctx->a[7] -= 4; w32(ctx->a[7], ctx->a[2]); ctx->a[7] -= 4; w32(ctx->a[7], ctx->a[1]); ctx->a[7] -= 4; w32(ctx->a[7], ctx->a[0]); ctx->a[7] -= 4; w32(ctx->a[7], ctx->d[7]); ctx->a[7] -= 4; w32(ctx->a[7], ctx->d[6]); ctx->a[7] -= 4; w32(ctx->a[7], ctx->d[5]); ctx->a[7] -= 4; w32(ctx->a[7], ctx->d[4]); ctx->a[7] -= 4; w32(ctx->a[7], ctx->d[3]); ctx->a[7] -= 4; w32(ctx->a[7], ctx->d[2]); ctx->a[7] -= 4; w32(ctx->a[7], ctx->d[1]); ctx->a[7] -= 4; w32(ctx->a[7], ctx->d[0]); }  /* 0FBC32 moveml %d0-%fp,%sp@- */
	{ uint32_t eaS = 0xFFFF000Au; int io = !plain(eaS); if (io) hle_pre(ctx); uint32_t v = r16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FBC36 movew ffff000a,%d0 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x1u, 16, 1); set_w(&ctx->d[0], v); hle_insn(ctx, 4); }  /* 0FBC3C subqw #1,%d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0FBC6A; } hle_insn(ctx, 8);  /* 0FBC3E beqs fbc6a */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FBC44u); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC59Cu, 1);  /* 0FBC40 bsrw fc59c */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(0); int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = 0x7u; w8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FBC44 moveb #7,%fp@(0) */
	{ uint32_t eaS = 0xFFFF00A2u; int io = !plain(eaS); if (io) hle_pre(ctx); uint32_t v = r16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FBC4A movew ffff00a2,%d0 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(1); int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = (ctx->d[0] & 0xFFu); w8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FBC50 moveb %d0,%fp@(1) */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FBC58u); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC428u, 1);  /* 0FBC54 bsrw fc428 */
	{ uint32_t eaD = 0xFFFF002Eu;  int io = !plain(eaD); if (io) hle_pre(ctx); w8(eaD, 0); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBC58 sf ffff002e */
	{ uint32_t eaD = 0xFFFF000Au; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBC5E clrw ffff000a */
	hle_movem_l(ctx, 132, 15); { ctx->d[0] = r32(ctx->a[7]); ctx->a[7] += 4; ctx->d[1] = r32(ctx->a[7]); ctx->a[7] += 4; ctx->d[2] = r32(ctx->a[7]); ctx->a[7] += 4; ctx->d[3] = r32(ctx->a[7]); ctx->a[7] += 4; ctx->d[4] = r32(ctx->a[7]); ctx->a[7] += 4; ctx->d[5] = r32(ctx->a[7]); ctx->a[7] += 4; ctx->d[6] = r32(ctx->a[7]); ctx->a[7] += 4; ctx->d[7] = r32(ctx->a[7]); ctx->a[7] += 4; ctx->a[0] = r32(ctx->a[7]); ctx->a[7] += 4; ctx->a[1] = r32(ctx->a[7]); ctx->a[7] += 4; ctx->a[2] = r32(ctx->a[7]); ctx->a[7] += 4; ctx->a[3] = r32(ctx->a[7]); ctx->a[7] += 4; ctx->a[4] = r32(ctx->a[7]); ctx->a[7] += 4; ctx->a[5] = r32(ctx->a[7]); ctx->a[7] += 4; ctx->a[6] = r32(ctx->a[7]); ctx->a[7] += 4; }  /* 0FBC64 moveml %sp@+,%d0-%fp */
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FBC68 rts */
L_0FBC6A:
	{ uint32_t eaD = 0xFFFF002Eu;  int io = !plain(eaD); if (io) hle_pre(ctx); w8(eaD, 0xFF); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBC6A st ffff002e */
	{ uint32_t eaD = 0xFFFF000Au; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBC70 clrw ffff000a */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FBC7Au); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC59Cu, 1);  /* 0FBC76 bsrw fc59c */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(1); int io = !plain(eaS); if (io) hle_pre(ctx); uint32_t v = r8(eaS); set_b(&ctx->d[0], v); cc_logic8(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FBC7A moveb %fp@(1),%d0 */
	{ uint32_t eaD = 0xFFFF00A2u; int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = (ctx->d[0] & 0xFFFFu); w16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FBC7E movew %d0,ffff00a2 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(1); int io = !plain(eaD); if (io) hle_pre(ctx); w8(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FBC84 clrb %fp@(1) */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FBC8Cu); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC488u, 1);  /* 0FBC88 bsrw fc488 */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FBC90u); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC428u, 1);  /* 0FBC8C bsrw fc428 */
	{ uint32_t eaD = 0xFFFF0016u; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBC90 clrw ffff0016 */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FBC9Au); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC428u, 1);  /* 0FBC96 bsrw fc428 */
	hle_movem_l(ctx, 132, 15); { ctx->d[0] = r32(ctx->a[7]); ctx->a[7] += 4; ctx->d[1] = r32(ctx->a[7]); ctx->a[7] += 4; ctx->d[2] = r32(ctx->a[7]); ctx->a[7] += 4; ctx->d[3] = r32(ctx->a[7]); ctx->a[7] += 4; ctx->d[4] = r32(ctx->a[7]); ctx->a[7] += 4; ctx->d[5] = r32(ctx->a[7]); ctx->a[7] += 4; ctx->d[6] = r32(ctx->a[7]); ctx->a[7] += 4; ctx->d[7] = r32(ctx->a[7]); ctx->a[7] += 4; ctx->a[0] = r32(ctx->a[7]); ctx->a[7] += 4; ctx->a[1] = r32(ctx->a[7]); ctx->a[7] += 4; ctx->a[2] = r32(ctx->a[7]); ctx->a[7] += 4; ctx->a[3] = r32(ctx->a[7]); ctx->a[7] += 4; ctx->a[4] = r32(ctx->a[7]); ctx->a[7] += 4; ctx->a[5] = r32(ctx->a[7]); ctx->a[7] += 4; ctx->a[6] = r32(ctx->a[7]); ctx->a[7] += 4; }  /* 0FBC9A moveml %sp@+,%d0-%fp */
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FBC9E rts */
L_0FBCA0:
	hle_movem_l(ctx, 128, 15); { ctx->a[7] -= 4; w32(ctx->a[7], ctx->a[6]); ctx->a[7] -= 4; w32(ctx->a[7], ctx->a[5]); ctx->a[7] -= 4; w32(ctx->a[7], ctx->a[4]); ctx->a[7] -= 4; w32(ctx->a[7], ctx->a[3]); ctx->a[7] -= 4; w32(ctx->a[7], ctx->a[2]); ctx->a[7] -= 4; w32(ctx->a[7], ctx->a[1]); ctx->a[7] -= 4; w32(ctx->a[7], ctx->a[0]); ctx->a[7] -= 4; w32(ctx->a[7], ctx->d[7]); ctx->a[7] -= 4; w32(ctx->a[7], ctx->d[6]); ctx->a[7] -= 4; w32(ctx->a[7], ctx->d[5]); ctx->a[7] -= 4; w32(ctx->a[7], ctx->d[4]); ctx->a[7] -= 4; w32(ctx->a[7], ctx->d[3]); ctx->a[7] -= 4; w32(ctx->a[7], ctx->d[2]); ctx->a[7] -= 4; w32(ctx->a[7], ctx->d[1]); ctx->a[7] -= 4; w32(ctx->a[7], ctx->d[0]); }  /* 0FBCA0 moveml %d0-%fp,%sp@- */
	{ uint32_t eaD = 0xFFFF002Eu;  int io = !plain(eaD); if (io) hle_pre(ctx); w8(eaD, 0xFF); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBCA4 st ffff002e */
	{ uint32_t eaS = 0xFFFF0038u; int io = !plain(eaS); if (io) hle_pre(ctx); uint32_t v = r16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FBCAA movew ffff0038,%d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0FBD02; } hle_insn(ctx, 8);  /* 0FBCB0 beqs fbd02 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x1u, 16, 1); set_w(&ctx->d[0], v); hle_insn(ctx, 4); }  /* 0FBCB2 subqw #1,%d0 */
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0xFFu, 16, 0); hle_insn(ctx, 8); }  /* 0FBCB4 cmpiw #255,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0FBCBC; } hle_insn(ctx, 8);  /* 0FBCB8 bnes fbcbc */
	{ ctx->d[0] = 0x00000000u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 0FBCBA moveq #0,%d0 */
L_0FBCBC:
	{ uint32_t eaD = 0xFFFF0026u; int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = (ctx->d[0] & 0xFFFFu); w16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FBCBC movew %d0,ffff0026 */
	{ uint32_t eaD = 0xFFFF0038u; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBCC2 clrw ffff0038 */
	{ uint32_t eaD = 0xFFFF0036u; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBCC8 clrw ffff0036 */
	{ uint32_t eaD = 0xFFFF0028u; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBCCE clrw ffff0028 */
	{ uint32_t eaD = 0xFFFF0034u; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBCD4 clrw ffff0034 */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FBCDEu); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC0F0u, 1);  /* 0FBCDA bsrw fc0f0 */
	{ uint32_t eaD = 0xFFFF0032u;  int io = !plain(eaD); if (io) hle_pre(ctx); w8(eaD, 0xFF); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBCDE st ffff0032 */
	{ uint32_t eaS = 0xFFFF0000u; int io = !plain(eaS); if (io) hle_pre(ctx); ctx->a[6] = r32(eaS); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBCE4 moveal ffff0000,%fp */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FBCEEu); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC408u, 1);  /* 0FBCEA bsrw fc408 */
	{ uint32_t eaS = 0xFFFF009Cu; uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(60); int io = !plain(eaS) || !plain(eaD); if (io) hle_pre(ctx); uint32_t v = r8(eaS); w8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 24); else hle_insn(ctx, 24); }  /* 0FBCEE moveb ffff009c,%fp@(60) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(1);  int io = !plain(eaD); if (io) hle_pre(ctx); w8(eaD, 0xFF); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FBCF6 st %fp@(1) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(15);  int io = !plain(eaD); if (io) hle_pre(ctx); w8(eaD, 0); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FBCFA sf %fp@(15) */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FBD02u); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC428u, 1);  /* 0FBCFE bsrw fc428 */
L_0FBD02:
	{ uint32_t eaS = 0xFFFF000Eu; int io = !plain(eaS); if (io) hle_pre(ctx); uint32_t v = r16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FBD02 movew ffff000e,%d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0FBD60; } hle_insn(ctx, 8);  /* 0FBD08 beqs fbd60 */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x1u, 16, 1); set_w(&ctx->d[0], v); hle_insn(ctx, 4); }  /* 0FBD0A subqw #1,%d0 */
	{ uint32_t eaD = 0xFFFF0012u; int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = (ctx->d[0] & 0xFFFFu); w16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FBD0C movew %d0,ffff0012 */
	{ uint32_t eaD = 0xFFFF0014u; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBD12 clrw ffff0014 */
	{ uint32_t eaD = 0xFFFF002Cu; int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = 0x6u; w16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBD18 movew #6,ffff002c */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FBD24u); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC0A6u, 1);  /* 0FBD20 bsrw fc0a6 */
	{ uint32_t eaS = 0xFFFF0000u; int io = !plain(eaS); if (io) hle_pre(ctx); ctx->a[6] = r32(eaS); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBD24 moveal ffff0000,%fp */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FBD2Eu); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC408u, 1);  /* 0FBD2A bsrw fc408 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(1);  int io = !plain(eaD); if (io) hle_pre(ctx); w8(eaD, 0); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FBD2E sf %fp@(1) */
	{ uint32_t eaD = 0xFFFF0072u;  int io = !plain(eaD); if (io) hle_pre(ctx); w8(eaD, 0); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBD32 sf ffff0072 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(58);  int io = !plain(eaD); if (io) hle_pre(ctx); w8(eaD, 0); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FBD38 sf %fp@(58) */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FBD40u); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC428u, 1);  /* 0FBD3C bsrw fc428 */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FBD44u); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC47Au, 1);  /* 0FBD40 bsrw fc47a */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FBD48u); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC488u, 1);  /* 0FBD44 bsrw fc488 */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FBD4Cu); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC0A6u, 1);  /* 0FBD48 bsrw fc0a6 */
	{ uint32_t eaD = 0xFFFF000Eu; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBD4C clrw ffff000e */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FBD56u); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC408u, 1);  /* 0FBD52 bsrw fc408 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(14);  int io = !plain(eaD); if (io) hle_pre(ctx); w8(eaD, 0); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FBD56 sf %fp@(14) */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FBD5Eu); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC428u, 1);  /* 0FBD5A bsrw fc428 */
	hle_insn(ctx, 10); goto L_0FBDD0;  /* 0FBD5E bras fbdd0 */
L_0FBD60:
	{ uint32_t eaS = 0xFFFF0016u; int io = !plain(eaS); if (io) hle_pre(ctx); uint32_t v = r16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FBD60 movew ffff0016,%d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0FBD6C; } hle_insn(ctx, 8);  /* 0FBD66 beqs fbd6c */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FBD6Cu); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC10Cu, 1);  /* 0FBD68 bsrw fc10c */
L_0FBD6C:
	{ uint32_t eaD = 0xFFFF001Eu; int io = !plain(eaD); if (io) hle_pre(ctx); cc_logic16(ctx, r16(eaD)); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FBD6C tstw ffff001e */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0FBD78; } hle_insn(ctx, 8);  /* 0FBD72 beqs fbd78 */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FBD78u); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC2AAu, 1);  /* 0FBD74 bsrw fc2aa */
L_0FBD78:
	{ uint32_t eaD = 0xFFFF0010u; int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = alu(ctx, OP_ADD, r16(eaD), 0x1u, 16, 1); w16(eaD, v); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBD78 addqw #1,ffff0010 */
	{ uint32_t eaS = 0xFFFF002Cu; int io = !plain(eaS); if (io) hle_pre(ctx); uint32_t v = r16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FBD7E movew ffff002c,%d0 */
	{ uint32_t eaS = 0xFFFF0010u; int io = !plain(eaS); if (io) hle_pre(ctx); alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), r16(eaS), 16, 0); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FBD84 cmpw ffff0010,%d0 */
	if (COND_CC) { hle_insn(ctx, 10); goto L_0FBDC0; } hle_insn(ctx, 8);  /* 0FBD8A bccs fbdc0 */
	{ uint32_t eaD = 0xFFFF0010u; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBD8C clrw ffff0010 */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FBD96u); } hle_insn(ctx, 18); snd_run(ctx, 0x0FBF08u, 1);  /* 0FBD92 bsrw fbf08 */
	{ uint32_t eaD = 0xFFFF0014u; int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = alu(ctx, OP_ADD, r16(eaD), 0x1u, 16, 1); w16(eaD, v); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBD96 addqw #1,ffff0014 */
	{ uint32_t eaD = 0xFFFF0014u; int io = !plain(eaD); if (io) hle_pre(ctx); alu(ctx, OP_SUB, r16(eaD), 0x40u, 16, 0); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBD9C cmpiw #64,ffff0014 */
	if (COND_CS) { hle_insn(ctx, 10); goto L_0FBDB6; } hle_insn(ctx, 8);  /* 0FBDA4 bcss fbdb6 */
	{ uint32_t eaD = 0xFFFF0014u; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBDA6 clrw ffff0014 */
	{ uint32_t eaD = 0xFFFF0012u; int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = alu(ctx, OP_ADD, r16(eaD), 0x1u, 16, 1); w16(eaD, v); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBDAC addqw #1,ffff0012 */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FBDB6u); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC0A6u, 1);  /* 0FBDB2 bsrw fc0a6 */
L_0FBDB6:
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FBDBAu); } hle_insn(ctx, 18); snd_run(ctx, 0x0FBDDCu, 1);  /* 0FBDB6 bsrw fbddc */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FBDBEu); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC4FAu, 1);  /* 0FBDBA bsrw fc4fa */
	hle_insn(ctx, 10); goto L_0FBDD0;  /* 0FBDBE bras fbdd0 */
L_0FBDC0:
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FBDC4u); } hle_insn(ctx, 18); snd_run(ctx, 0x0FBDF0u, 1);  /* 0FBDC0 bsrw fbdf0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0FBDCC; } hle_insn(ctx, 8);  /* 0FBDC4 beqs fbdcc */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FBDCAu); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC4FAu, 1);  /* 0FBDC6 bsrw fc4fa */
	hle_insn(ctx, 10); goto L_0FBDD0;  /* 0FBDCA bras fbdd0 */
L_0FBDCC:
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FBDD0u); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC496u, 1);  /* 0FBDCC bsrw fc496 */
L_0FBDD0:
	{ uint32_t eaD = 0xFFFF002Eu;  int io = !plain(eaD); if (io) hle_pre(ctx); w8(eaD, 0); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBDD0 sf ffff002e */
	hle_movem_l(ctx, 132, 15); { ctx->d[0] = r32(ctx->a[7]); ctx->a[7] += 4; ctx->d[1] = r32(ctx->a[7]); ctx->a[7] += 4; ctx->d[2] = r32(ctx->a[7]); ctx->a[7] += 4; ctx->d[3] = r32(ctx->a[7]); ctx->a[7] += 4; ctx->d[4] = r32(ctx->a[7]); ctx->a[7] += 4; ctx->d[5] = r32(ctx->a[7]); ctx->a[7] += 4; ctx->d[6] = r32(ctx->a[7]); ctx->a[7] += 4; ctx->d[7] = r32(ctx->a[7]); ctx->a[7] += 4; ctx->a[0] = r32(ctx->a[7]); ctx->a[7] += 4; ctx->a[1] = r32(ctx->a[7]); ctx->a[7] += 4; ctx->a[2] = r32(ctx->a[7]); ctx->a[7] += 4; ctx->a[3] = r32(ctx->a[7]); ctx->a[7] += 4; ctx->a[4] = r32(ctx->a[7]); ctx->a[7] += 4; ctx->a[5] = r32(ctx->a[7]); ctx->a[7] += 4; ctx->a[6] = r32(ctx->a[7]); ctx->a[7] += 4; }  /* 0FBDD6 moveml %sp@+,%d0-%fp */
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FBDDA rts */
L_0FBDDC:
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FBDDEu); } hle_insn(ctx, 18); snd_run(ctx, 0x0FBE18u, 1);  /* 0FBDDC bsrs fbe18 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0FBDE2; } hle_insn(ctx, 8);  /* 0FBDDE bnes fbde2 */
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FBDE0 rts */
L_0FBDE2:
	{ uint32_t eaS = 0xFFFF0086u; uint32_t eaD = 0xFFFF006Eu; int io = !plain(eaS) || !plain(eaD); if (io) hle_pre(ctx); uint32_t v = r16(eaS); w16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 28); else hle_insn(ctx, 28); }  /* 0FBDE2 movew ffff0086,ffff006e */
	{ ctx->d[0] = 0x00000001u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 0FBDEC moveq #1,%d0 */
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FBDEE rts */
L_0FBDF0:
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FBDF2u); } hle_insn(ctx, 18); snd_run(ctx, 0x0FBE18u, 1);  /* 0FBDF0 bsrs fbe18 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0FBE16; } hle_insn(ctx, 8);  /* 0FBDF2 beqs fbe16 */
	{ uint32_t eaD = 0xFFFF003Eu; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBDF4 clrw ffff003e */
	{ uint32_t eaD = 0xFFFF004Au; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBDFA clrw ffff004a */
	{ uint32_t eaD = 0xFFFF0056u; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBE00 clrw ffff0056 */
	{ uint32_t eaD = 0xFFFF0062u; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBE06 clrw ffff0062 */
	{ uint32_t eaD = 0xFFFF007Au; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBE0C clrw ffff007a */
	hle_insn(ctx, 10); goto L_0FBDE2;  /* 0FBE12 braw fbde2 */
L_0FBE16:
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FBE16 rts */
L_0FBE18:
	{ uint32_t eaS = 0xFFFF0082u; ctx->a[5] = eaS; } hle_insn(ctx, 12);  /* 0FBE18 lea ffff0082,%a5 */
	{ uint32_t eaS = 0xFFFF006Au; ctx->a[3] = eaS; } hle_insn(ctx, 12);  /* 0FBE1E lea ffff006a,%a3 */
	{ uint32_t eaD = 0xFFFF00A0u; int io = !plain(eaD); if (io) hle_pre(ctx); cc_logic16(ctx, r16(eaD)); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FBE24 tstw ffff00a0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0FBEC8; } hle_insn(ctx, 12);  /* 0FBE2A bnew fbec8 */
	{ uint32_t eaD = 0xFFFF0032u; int io = !plain(eaD); if (io) hle_pre(ctx); cc_logic16(ctx, r16(eaD)); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FBE2E tstw ffff0032 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0FBF06; } hle_insn(ctx, 12);  /* 0FBE34 beqw fbf06 */
	{ uint32_t eaD = ctx->a[5] + (uint32_t)(int32_t)(4); int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FBE38 clrw %a5@(4) */
	{ uint32_t eaS = ctx->a[3] + (uint32_t)(int32_t)(5); int io = !plain(eaS); if (io) hle_pre(ctx); uint32_t v = r8(eaS); set_b(&ctx->d[1], v); cc_logic8(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FBE3C moveb %a3@(5),%d1 */
	{ uint32_t v = shf(ctx, 0, (ctx->d[1] & 0xFFu), 1, 8); set_b(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 0FBE40 lsrb #1,%d1 */
	{ alu(ctx, OP_SUB, (ctx->d[1] & 0xFFu), 0x7Cu, 8, 0); hle_insn(ctx, 8); }  /* 0FBE42 cmpib #124,%d1 */
	if (COND_CC) { hle_insn(ctx, 10); goto L_0FBE4C; } hle_insn(ctx, 8);  /* 0FBE46 bccs fbe4c */
	{ uint32_t eaD = ctx->a[3] + (uint32_t)(int32_t)(4); int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FBE48 clrw %a3@(4) */
L_0FBE4C:
	{ uint32_t eaD = 0xFFFF0030u;  int io = !plain(eaD); if (io) hle_pre(ctx); w8(eaD, 0xFF); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBE4C st ffff0030 */
	{ uint32_t eaD = ctx->a[3] + (uint32_t)(int32_t)(10);  int io = !plain(eaD); if (io) hle_pre(ctx); w8(eaD, 0xFF); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FBE52 st %a3@(10) */
	{ uint32_t eaS = 0xFFFF0026u; int io = !plain(eaS); if (io) hle_pre(ctx); uint32_t v = r16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FBE56 movew ffff0026,%d0 */
	{ uint32_t eaS = 0xFFFF0028u; uint32_t eaD = 0xFFFF002Au; int io = !plain(eaS) || !plain(eaD); if (io) hle_pre(ctx); uint32_t v = r16(eaS); w16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 28); else hle_insn(ctx, 28); }  /* 0FBE5C movew ffff0028,ffff002a */
	{ uint32_t eaS = 0x000F8426u; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 0FBE66 lea f8426,%a0 */
	{ uint32_t eaD = 0x000F8020u; int io = !plain(eaD); if (io) hle_pre(ctx); cc_logic16(ctx, r16(eaD)); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FBE6C tstw f8020 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0FBE7A; } hle_insn(ctx, 8);  /* 0FBE72 beqs fbe7a */
	{ uint32_t eaS = 0x000F842Au; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 0FBE74 lea f842a,%a0 */
L_0FBE7A:
	{ uint32_t v = 0x0u; set_w(&ctx->d[7], v); cc_logic16(ctx, v); hle_insn(ctx, 8); }  /* 0FBE7A movew #0,%d7 */
	{ uint32_t eaD = 0xFFFF0034u; int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = alu(ctx, OP_ADD, r16(eaD), 0x1u, 16, 1); w16(eaD, v); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBE7E addqw #1,ffff0034 */
	{ uint32_t eaS = 0xFFFF0036u; int io = !plain(eaS); if (io) hle_pre(ctx); uint32_t v = r16(eaS); set_w(&ctx->d[4], v); cc_logic16(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FBE84 movew ffff0036,%d4 */
	{ uint32_t eaS = 0xFFFF0034u; int io = !plain(eaS); if (io) hle_pre(ctx); alu(ctx, OP_SUB, (ctx->d[4] & 0xFFFFu), r16(eaS), 16, 0); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FBE8A cmpw ffff0034,%d4 */
	if (COND_CC) { hle_insn(ctx, 10); goto L_0FBEFE; } hle_insn(ctx, 8);  /* 0FBE90 bccs fbefe */
	{ uint32_t eaD = 0xFFFF0034u; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBE92 clrw ffff0034 */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FBE9Cu); } hle_insn(ctx, 18); snd_run(ctx, 0x0FBF26u, 1);  /* 0FBE98 bsrw fbf26 */
	{ uint32_t eaD = 0xFFFF0028u; int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = alu(ctx, OP_ADD, r16(eaD), 0x1u, 16, 1); w16(eaD, v); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBE9C addqw #1,ffff0028 */
	{ uint32_t eaD = 0xFFFF0028u; int io = !plain(eaD); if (io) hle_pre(ctx); alu(ctx, OP_SUB, r16(eaD), 0x40u, 16, 0); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBEA2 cmpiw #64,ffff0028 */
	if (COND_CS) { hle_insn(ctx, 10); goto L_0FBEF4; } hle_insn(ctx, 8);  /* 0FBEAA bcss fbef4 */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FBEB0u); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC0F0u, 1);  /* 0FBEAC bsrw fc0f0 */
	{ uint32_t eaD = 0xFFFF0028u; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBEB0 clrw ffff0028 */
	{ uint32_t eaD = 0xFFFF0026u; int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = alu(ctx, OP_ADD, r16(eaD), 0x1u, 16, 1); w16(eaD, v); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBEB6 addqw #1,ffff0026 */
	{ uint32_t eaD = 0xFFFF0026u; int io = !plain(eaD); if (io) hle_pre(ctx); cc_logic16(ctx, r16(eaD)); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FBEBC tstw ffff0026 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0FBEF4; } hle_insn(ctx, 8);  /* 0FBEC2 bnes fbef4 */
	{ uint32_t eaD = ctx->a[3] + (uint32_t)(int32_t)(8);  int io = !plain(eaD); if (io) hle_pre(ctx); w8(eaD, 0xFF); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FBEC4 st %a3@(8) */
L_0FBEC8:
	{ uint32_t eaD = 0xFFFF00A0u;  int io = !plain(eaD); if (io) hle_pre(ctx); w8(eaD, 0); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBEC8 sf ffff00a0 */
	{ uint32_t eaD = ctx->a[3] + (uint32_t)(int32_t)(10);  int io = !plain(eaD); if (io) hle_pre(ctx); w8(eaD, 0); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FBECE sf %a3@(10) */
	{ uint32_t eaD = 0xFFFF0032u; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBED2 clrw ffff0032 */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FBEDCu); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC408u, 1);  /* 0FBED8 bsrw fc408 */
	{ uint32_t eaS = 0xFFFF0000u; int io = !plain(eaS); if (io) hle_pre(ctx); ctx->a[6] = r32(eaS); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBEDC moveal ffff0000,%fp */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(1);  int io = !plain(eaD); if (io) hle_pre(ctx); w8(eaD, 0); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FBEE2 sf %fp@(1) */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FBEEAu); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC428u, 1);  /* 0FBEE6 bsrw fc428 */
	{ ctx->d[0] = 0x00000000u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 0FBEEA moveq #0,%d0 */
	{ uint32_t eaD = 0xFFFF0030u;  int io = !plain(eaD); if (io) hle_pre(ctx); w8(eaD, 0); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBEEC sf ffff0030 */
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FBEF2 rts */
L_0FBEF4:
	{ uint32_t eaD = 0xFFFF0030u;  int io = !plain(eaD); if (io) hle_pre(ctx); w8(eaD, 0); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBEF4 sf ffff0030 */
	{ ctx->d[0] = 0x00000001u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 0FBEFA moveq #1,%d0 */
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FBEFC rts */
L_0FBEFE:
	{ uint32_t eaD = 0xFFFF0030u;  int io = !plain(eaD); if (io) hle_pre(ctx); w8(eaD, 0); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FBEFE sf ffff0030 */
	{ ctx->d[0] = 0x00000000u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 0FBF04 moveq #0,%d0 */
L_0FBF06:
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FBF06 rts */
L_0FBF08:
	{ uint32_t eaS = 0xFFFF0012u; int io = !plain(eaS); if (io) hle_pre(ctx); uint32_t v = r16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FBF08 movew ffff0012,%d0 */
	{ uint32_t eaS = 0xFFFF0014u; uint32_t eaD = 0xFFFF002Au; int io = !plain(eaS) || !plain(eaD); if (io) hle_pre(ctx); uint32_t v = r16(eaS); w16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 28); else hle_insn(ctx, 28); }  /* 0FBF0E movew ffff0014,ffff002a */
	{ uint32_t eaS = 0xFFFF003Au; ctx->a[5] = eaS; } hle_insn(ctx, 12);  /* 0FBF18 lea ffff003a,%a5 */
	{ uint32_t v = 0x5u; set_w(&ctx->d[7], v); cc_logic16(ctx, v); hle_insn(ctx, 8); }  /* 0FBF1E movew #5,%d7 */
	{ uint32_t eaS = 0x000F8422u; ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 0FBF22 lea %pc@(f8422),%a0 */
L_0FBF26:
	{ uint32_t eaD = 0x000F8020u; int io = !plain(eaD); if (io) hle_pre(ctx); cc_logic16(ctx, r16(eaD)); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FBF26 tstw f8020 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0FBF30; } hle_insn(ctx, 8);  /* 0FBF2C beqs fbf30 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), (ctx->d[0] & 0xFFFFu), 16, 1); set_w(&ctx->d[0], v); hle_insn(ctx, 4); }  /* 0FBF2E addw %d0,%d0 */
L_0FBF30:
	{ uint32_t v = ((ctx->d[0] & 0xFFFFFFFFu) & 0xFFFFu) & 0xFFFFFFFFu; ctx->d[0] = v; cc_logic32(ctx, v); hle_insn(ctx, 16); }  /* 0FBF30 andil #65535,%d0 */
	{ uint32_t res = (ctx->d[0] & 0xFFFFu) * 0x6u; ctx->d[0] = res; cc_logic32(ctx, res); } hle_insn(ctx, 46);  /* 0FBF36 muluw #6,%d0 */
	{ ctx->a[0] += (uint32_t)(int32_t)(int16_t)(ctx->d[0] & 0xFFFFu); hle_insn(ctx, 8); }  /* 0FBF3A addaw %d0,%a0 */
L_0FBF3C:
	{ ctx->d[0] = 0x00000000u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 0FBF3C moveq #0,%d0 */
	{ uint32_t eaD = 0x000F8020u; int io = !plain(eaD); if (io) hle_pre(ctx); cc_logic16(ctx, r16(eaD)); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FBF3E tstw f8020 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0FBF4A; } hle_insn(ctx, 8);  /* 0FBF44 beqs fbf4a */
	{ uint32_t eaS = ctx->a[0]; ctx->a[0] += 2; int io = !plain(eaS); if (io) hle_pre(ctx); uint32_t v = r16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); if (io) hle_post(ctx, 8); else hle_insn(ctx, 8); }  /* 0FBF46 movew %a0@+,%d0 */
	hle_insn(ctx, 10); goto L_0FBF4C;  /* 0FBF48 bras fbf4c */
L_0FBF4A:
	{ uint32_t eaS = ctx->a[0]; ctx->a[0] += 1; int io = !plain(eaS); if (io) hle_pre(ctx); uint32_t v = r8(eaS); set_b(&ctx->d[0], v); cc_logic8(ctx, v); if (io) hle_post(ctx, 8); else hle_insn(ctx, 8); }  /* 0FBF4A moveb %a0@+,%d0 */
L_0FBF4C:
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FBF4Eu); } hle_insn(ctx, 18); snd_run(ctx, 0x0FBF58u, 1);  /* 0FBF4C bsrs fbf58 */
	{ uint32_t eaS = ctx->a[5] + (uint32_t)(int32_t)(12); ctx->a[5] = eaS; } hle_insn(ctx, 8);  /* 0FBF4E lea %a5@(12),%a5 */
	{ uint16_t v = (uint16_t)ctx->d[7] - 1; set_w(&ctx->d[7], v); if (v != 0xFFFF) { hle_insn(ctx, 10); goto L_0FBF3C; } hle_insn(ctx, 14); }  /* 0FBF52 dbf %d7,fbf3c */
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FBF56 rts */
L_0FBF58:
	{ uint32_t eaD = ctx->a[5] + (uint32_t)(int32_t)(2); int io = !plain(eaD); if (io) hle_pre(ctx); cc_logic16(ctx, r16(eaD)); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FBF58 tstw %a5@(2) */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0FBF68; } hle_insn(ctx, 8);  /* 0FBF5C beqs fbf68 */
	{ uint32_t eaD = ctx->a[5] + (uint32_t)(int32_t)(2); int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = alu(ctx, OP_SUB, r16(eaD), 0x1u, 16, 1); w16(eaD, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FBF5E subqw #1,%a5@(2) */
	{ uint32_t eaD = ctx->a[5] + (uint32_t)(int32_t)(0); int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = alu(ctx, OP_ADD, r16(eaD), 0x1u, 16, 1); w16(eaD, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FBF62 addqw #1,%a5@(0) */
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FBF66 rts */
L_0FBF68:
	{ uint32_t v = shf(ctx, 1, (ctx->d[0] & 0xFFFFu), 2, 16); set_w(&ctx->d[0], v); } hle_insn(ctx, 10);  /* 0FBF68 lslw #2,%d0 */
	{ uint32_t eaS = 0x000F8022u; ctx->a[1] = eaS; } hle_insn(ctx, 8);  /* 0FBF6A lea %pc@(f8022),%a1 */
	{ ctx->a[1] += (uint32_t)(int32_t)(int16_t)(ctx->d[0] & 0xFFFFu); hle_insn(ctx, 8); }  /* 0FBF6E addaw %d0,%a1 */
	{ uint32_t eaS = ctx->a[1]; int io = !plain(eaS); if (io) hle_pre(ctx); ctx->a[1] = r32(eaS); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FBF70 moveal %a1@,%a1 */
	{ uint32_t eaS = 0xFFFF002Au; int io = !plain(eaS); if (io) hle_pre(ctx); uint32_t v = r16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FBF72 movew ffff002a,%d0 */
	{ uint32_t eaS = ctx->a[5] + (uint32_t)(int32_t)(0); int io = !plain(eaS); if (io) hle_pre(ctx); uint32_t v = alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), r16(eaS), 16, 1); set_w(&ctx->d[0], v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FBF78 subw %a5@(0),%d0 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), (ctx->d[0] & 0xFFFFu), 16, 1); set_w(&ctx->d[0], v); hle_insn(ctx, 4); }  /* 0FBF7C addw %d0,%d0 */
	{ ctx->a[1] += (uint32_t)(int32_t)(int16_t)(ctx->d[0] & 0xFFFFu); hle_insn(ctx, 8); }  /* 0FBF7E addaw %d0,%a1 */
	{ ctx->d[0] = 0x00000000u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 0FBF80 moveq #0,%d0 */
	{ ctx->d[1] = 0x00000000u; cc_logic32(ctx, ctx->d[1]); } hle_insn(ctx, 4);  /* 0FBF82 moveq #0,%d1 */
	{ uint32_t eaS = ctx->a[1]; ctx->a[1] += 1; int io = !plain(eaS); if (io) hle_pre(ctx); uint32_t v = r8(eaS); set_b(&ctx->d[0], v); cc_logic8(ctx, v); if (io) hle_post(ctx, 8); else hle_insn(ctx, 8); }  /* 0FBF84 moveb %a1@+,%d0 */
	{ uint32_t eaS = ctx->a[1]; ctx->a[1] += 1; int io = !plain(eaS); if (io) hle_pre(ctx); uint32_t v = r8(eaS); set_b(&ctx->d[1], v); cc_logic8(ctx, v); if (io) hle_post(ctx, 8); else hle_insn(ctx, 8); }  /* 0FBF86 moveb %a1@+,%d1 */
	{ alu(ctx, OP_SUB, (ctx->d[1] & 0xFFu), 0xFFu, 8, 0); hle_insn(ctx, 8); }  /* 0FBF88 cmpib #-1,%d1 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0FBF9A; } hle_insn(ctx, 8);  /* 0FBF8C bnes fbf9a */
	{ cc_logic8(ctx, (ctx->d[0] & 0xFFu)); hle_insn(ctx, 4); }  /* 0FBF8E tstb %d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0FBF96; } hle_insn(ctx, 8);  /* 0FBF90 beqs fbf96 */
	{ uint32_t eaD = ctx->a[5] + (uint32_t)(int32_t)(2); int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = (ctx->d[0] & 0xFFFFu); w16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FBF92 movew %d0,%a5@(2) */
L_0FBF96:
	{ ctx->d[0] = 0x00000000u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 0FBF96 moveq #0,%d0 */
	{ ctx->d[1] = 0x00000000u; cc_logic32(ctx, ctx->d[1]); } hle_insn(ctx, 4);  /* 0FBF98 moveq #0,%d1 */
L_0FBF9A:
	{ uint32_t eaD = ctx->a[5] + (uint32_t)(int32_t)(10); int io = !plain(eaD); if (io) hle_pre(ctx); cc_logic16(ctx, r16(eaD)); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FBF9A tstw %a5@(10) */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0FBFDA; } hle_insn(ctx, 8);  /* 0FBF9E bnes fbfda */
	{ uint32_t eaD = ctx->a[5] + (uint32_t)(int32_t)(8); int io = !plain(eaD); if (io) hle_pre(ctx); cc_logic16(ctx, r16(eaD)); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FBFA0 tstw %a5@(8) */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0FBFD2; } hle_insn(ctx, 8);  /* 0FBFA4 beqs fbfd2 */
	{ uint32_t v = (ctx->d[1] & 0xFFu); set_b(&ctx->d[2], v); cc_logic8(ctx, v); hle_insn(ctx, 4); }  /* 0FBFA6 moveb %d1,%d2 */
	{ uint32_t v = shf(ctx, 0, (ctx->d[2] & 0xFFu), 1, 8); set_b(&ctx->d[2], v); } hle_insn(ctx, 8);  /* 0FBFA8 lsrb #1,%d2 */
	{ alu(ctx, OP_SUB, (ctx->d[2] & 0xFFu), 0x7Cu, 8, 0); hle_insn(ctx, 8); }  /* 0FBFAA cmpib #124,%d2 */
	if (COND_CC) { hle_insn(ctx, 10); goto L_0FBFD2; } hle_insn(ctx, 8);  /* 0FBFAE bccs fbfd2 */
	{ ctx->d[3] = 0x00000000u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4);  /* 0FBFB0 moveq #0,%d3 */
	{ uint32_t v = (ctx->d[0] & 0xFFu); set_b(&ctx->d[2], v); cc_logic8(ctx, v); hle_insn(ctx, 4); }  /* 0FBFB2 moveb %d0,%d2 */
	{ uint32_t v = (ctx->d[1] & 0xFFu); set_b(&ctx->d[3], v); cc_logic8(ctx, v); hle_insn(ctx, 4); }  /* 0FBFB4 moveb %d1,%d3 */
	{ uint32_t v = shf(ctx, 1, (ctx->d[3] & 0xFFFFu), 8, 16); set_w(&ctx->d[3], v); } hle_insn(ctx, 22);  /* 0FBFB6 lslw #8,%d3 */
	{ uint32_t v = ((ctx->d[3] & 0xFFu) | (ctx->d[2] & 0xFFu)) & 0xFFu; set_b(&ctx->d[3], v); cc_logic8(ctx, v); hle_insn(ctx, 4); }  /* 0FBFB8 orb %d2,%d3 */
	{ uint32_t v = ((ctx->d[3] & 0xFFFFu) & 0x1F0u) & 0xFFFFu; set_w(&ctx->d[3], v); cc_logic16(ctx, v); hle_insn(ctx, 8); }  /* 0FBFBA andiw #496,%d3 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0FBFCE; } hle_insn(ctx, 8);  /* 0FBFBE bnes fbfce */
	{ ctx->d[2] = 0x00000000u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4);  /* 0FBFC0 moveq #0,%d2 */
	{ uint32_t eaS = ctx->a[5] + (uint32_t)(int32_t)(6); int io = !plain(eaS); if (io) hle_pre(ctx); uint32_t v = r8(eaS); set_b(&ctx->d[2], v); cc_logic8(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FBFC2 moveb %a5@(6),%d2 */
	{ uint32_t v = shf(ctx, 1, (ctx->d[2] & 0xFFFFu), 1, 16); set_w(&ctx->d[2], v); } hle_insn(ctx, 8);  /* 0FBFC6 lslw #1,%d2 */
	{ uint32_t v = ((ctx->d[0] & 0xFFu) | (ctx->d[2] & 0xFFu)) & 0xFFu; set_b(&ctx->d[0], v); cc_logic8(ctx, v); hle_insn(ctx, 4); }  /* 0FBFC8 orb %d2,%d0 */
	{ uint32_t v = shf(ctx, 0, (ctx->d[2] & 0xFFFFu), 8, 16); set_w(&ctx->d[2], v); } hle_insn(ctx, 22);  /* 0FBFCA lsrw #8,%d2 */
	{ uint32_t v = ((ctx->d[1] & 0xFFu) | (ctx->d[2] & 0xFFu)) & 0xFFu; set_b(&ctx->d[1], v); cc_logic8(ctx, v); hle_insn(ctx, 4); }  /* 0FBFCC orb %d2,%d1 */
L_0FBFCE:
	{ uint32_t eaD = ctx->a[5] + (uint32_t)(int32_t)(8); int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FBFCE clrw %a5@(8) */
L_0FBFD2:
	{ uint32_t eaD = ctx->a[5] + (uint32_t)(int32_t)(4); int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = (ctx->d[0] & 0xFFu); w8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FBFD2 moveb %d0,%a5@(4) */
	{ uint32_t eaD = ctx->a[5] + (uint32_t)(int32_t)(5); int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = (ctx->d[1] & 0xFFu); w8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FBFD6 moveb %d1,%a5@(5) */
L_0FBFDA:
	{ uint32_t v = (ctx->d[1] & 0xFFu); set_b(&ctx->d[2], v); cc_logic8(ctx, v); hle_insn(ctx, 4); }  /* 0FBFDA moveb %d1,%d2 */
	{ uint32_t v = shf(ctx, 0, (ctx->d[1] & 0xFFu), 1, 8); set_b(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 0FBFDC lsrb #1,%d1 */
	{ alu(ctx, OP_SUB, (ctx->d[1] & 0xFFu), 0x7Cu, 8, 0); hle_insn(ctx, 8); }  /* 0FBFDE cmpib #124,%d1 */
	if (COND_CC) { hle_insn(ctx, 10); goto L_0FBFF4; } hle_insn(ctx, 8);  /* 0FBFE2 bccs fbff4 */
	{ uint32_t v = shf(ctx, 2, (ctx->d[2] & 0xFFu), 1, 8); set_b(&ctx->d[2], v); } hle_insn(ctx, 8);  /* 0FBFE4 roxrb #1,%d2 */
	{ uint32_t v = shf(ctx, 2, (ctx->d[0] & 0xFFu), 1, 8); set_b(&ctx->d[0], v); } hle_insn(ctx, 8);  /* 0FBFE6 roxrb #1,%d0 */
	{ uint32_t v = ((ctx->d[0] & 0xFFu) & 0xF8u) & 0xFFu; set_b(&ctx->d[0], v); cc_logic8(ctx, v); hle_insn(ctx, 8); }  /* 0FBFE8 andib #-8,%d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0FBFF2; } hle_insn(ctx, 8);  /* 0FBFEC beqs fbff2 */
	{ uint32_t eaD = ctx->a[5] + (uint32_t)(int32_t)(6); int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = (ctx->d[0] & 0xFFu); w8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FBFEE moveb %d0,%a5@(6) */
L_0FBFF2:
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FBFF2 rts */
L_0FBFF4:
	{ alu(ctx, OP_SUB, (ctx->d[1] & 0xFFu), 0x7Cu, 8, 0); hle_insn(ctx, 8); }  /* 0FBFF4 cmpib #124,%d1 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0FC0A4; } hle_insn(ctx, 12);  /* 0FBFF8 beqw fc0a4 */
	{ alu(ctx, OP_SUB, (ctx->d[1] & 0xFFu), 0x7Du, 8, 0); hle_insn(ctx, 8); }  /* 0FBFFC cmpib #125,%d1 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0FC04E; } hle_insn(ctx, 8);  /* 0FC000 beqs fc04e */
	{ alu(ctx, OP_SUB, (ctx->d[1] & 0xFFu), 0x7Eu, 8, 0); hle_insn(ctx, 8); }  /* 0FC002 cmpib #126,%d1 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0FC024; } hle_insn(ctx, 8);  /* 0FC006 beqs fc024 */
	{ uint32_t eaD = 0xFFFF0030u; int io = !plain(eaD); if (io) hle_pre(ctx); cc_logic16(ctx, r16(eaD)); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC008 tstw ffff0030 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0FC01A; } hle_insn(ctx, 8);  /* 0FC00E beqs fc01a */
	{ uint32_t eaD = 0xFFFF0028u; int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = 0x3Fu; w16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC010 movew #63,ffff0028 */
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FC018 rts */
L_0FC01A:
	{ uint32_t eaD = 0xFFFF0014u; int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = 0x3Fu; w16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC01A movew #63,ffff0014 */
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FC022 rts */
L_0FC024:
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x1u, 16, 1); set_w(&ctx->d[0], v); hle_insn(ctx, 4); }  /* 0FC024 subqw #1,%d0 */
	{ uint32_t eaD = 0xFFFF0030u; int io = !plain(eaD); if (io) hle_pre(ctx); cc_logic16(ctx, r16(eaD)); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC026 tstw ffff0030 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0FC03E; } hle_insn(ctx, 8);  /* 0FC02C beqs fc03e */
	{ uint32_t eaD = 0xFFFF0028u; int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = 0x3Fu; w16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC02E movew #63,ffff0028 */
	{ uint32_t eaD = 0xFFFF0026u; int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = (ctx->d[0] & 0xFFFFu); w16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC036 movew %d0,ffff0026 */
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FC03C rts */
L_0FC03E:
	{ uint32_t eaD = 0xFFFF0012u; int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = (ctx->d[0] & 0xFFFFu); w16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC03E movew %d0,ffff0012 */
	{ uint32_t eaD = 0xFFFF0014u; int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = 0x3Fu; w16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC044 movew #63,ffff0014 */
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FC04C rts */
L_0FC04E:
	{ uint32_t eaS = 0xFFFF0000u; int io = !plain(eaS); if (io) hle_pre(ctx); ctx->a[6] = r32(eaS); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC04E moveal ffff0000,%fp */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FC058u); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC408u, 1);  /* 0FC054 bsrw fc408 */
	{ uint32_t eaD = 0xFFFF0030u; int io = !plain(eaD); if (io) hle_pre(ctx); cc_logic16(ctx, r16(eaD)); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC058 tstw ffff0030 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0FC084; } hle_insn(ctx, 8);  /* 0FC05E beqs fc084 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(15); int io = !plain(eaD); if (io) hle_pre(ctx); w8(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC060 clrb %fp@(15) */
	{ cc_set(ctx, CC_Z, ((ctx->d[0] >> 7) & 1) ? 0 : CC_Z); } hle_insn(ctx, 10);  /* 0FC064 btst #7,%d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0FC06E; } hle_insn(ctx, 8);  /* 0FC068 beqs fc06e */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(15);  int io = !plain(eaD); if (io) hle_pre(ctx); w8(eaD, 0xFF); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC06A st %fp@(15) */
L_0FC06E:
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FC072u); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC428u, 1);  /* 0FC06E bsrw fc428 */
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0x7Fu) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); hle_insn(ctx, 8); }  /* 0FC072 andiw #127,%d0 */
	{ uint32_t eaD = 0xFFFF0036u; int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = (ctx->d[0] & 0xFFFFu); w16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC076 movew %d0,ffff0036 */
	{ uint32_t eaD = 0xFFFF0034u; int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = (ctx->d[0] & 0xFFFFu); w16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC07C movew %d0,ffff0034 */
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FC082 rts */
L_0FC084:
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(14); int io = !plain(eaD); if (io) hle_pre(ctx); w8(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC084 clrb %fp@(14) */
	{ cc_set(ctx, CC_Z, ((ctx->d[0] >> 7) & 1) ? 0 : CC_Z); } hle_insn(ctx, 10);  /* 0FC088 btst #7,%d0 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(14);  int io = !plain(eaD); if (io) hle_pre(ctx); w8(eaD, 0); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC08C sf %fp@(14) */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0FC096; } hle_insn(ctx, 8);  /* 0FC090 beqs fc096 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(14);  int io = !plain(eaD); if (io) hle_pre(ctx); w8(eaD, 0xFF); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC092 st %fp@(14) */
L_0FC096:
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FC09Au); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC428u, 1);  /* 0FC096 bsrw fc428 */
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0x7Fu) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); hle_insn(ctx, 8); }  /* 0FC09A andiw #127,%d0 */
	{ uint32_t eaD = 0xFFFF002Cu; int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = (ctx->d[0] & 0xFFFFu); w16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC09E movew %d0,ffff002c */
L_0FC0A4:
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FC0A4 rts */
L_0FC0A6:
	{ uint32_t eaD = 0xFFFF003Cu; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC0A6 clrw ffff003c */
	{ uint32_t eaD = 0xFFFF003Au; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC0AC clrw ffff003a */
	{ uint32_t eaD = 0xFFFF0048u; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC0B2 clrw ffff0048 */
	{ uint32_t eaD = 0xFFFF0046u; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC0B8 clrw ffff0046 */
	{ uint32_t eaD = 0xFFFF0054u; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC0BE clrw ffff0054 */
	{ uint32_t eaD = 0xFFFF0052u; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC0C4 clrw ffff0052 */
	{ uint32_t eaD = 0xFFFF0060u; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC0CA clrw ffff0060 */
	{ uint32_t eaD = 0xFFFF005Eu; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC0D0 clrw ffff005e */
	{ uint32_t eaD = 0xFFFF006Cu; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC0D6 clrw ffff006c */
	{ uint32_t eaD = 0xFFFF006Au; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC0DC clrw ffff006a */
	{ uint32_t eaD = 0xFFFF0078u; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC0E2 clrw ffff0078 */
	{ uint32_t eaD = 0xFFFF0076u; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC0E8 clrw ffff0076 */
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FC0EE rts */
L_0FC0F0:
	{ uint32_t eaD = 0xFFFF0084u; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC0F0 clrw ffff0084 */
	{ uint32_t eaD = 0xFFFF0082u; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC0F6 clrw ffff0082 */
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FC0FC rts */
L_0FC0FE:
	{ uint32_t eaD = 0xFFFF0090u; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC0FE clrw ffff0090 */
	{ uint32_t eaD = 0xFFFF008Eu; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC104 clrw ffff008e */
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FC10A rts */
L_0FC10C:
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x1u, 16, 1); set_w(&ctx->d[0], v); hle_insn(ctx, 4); }  /* 0FC10C subqw #1,%d0 */
	{ uint32_t eaS = 0xFFFF008Eu; ctx->a[5] = eaS; } hle_insn(ctx, 12);  /* 0FC10E lea ffff008e,%a5 */
	{ uint32_t eaS = 0x000F8427u; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 0FC114 lea f8427,%a0 */
	{ uint32_t eaD = 0x000F8020u; int io = !plain(eaD); if (io) hle_pre(ctx); cc_logic16(ctx, r16(eaD)); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC11A tstw f8020 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0FC128; } hle_insn(ctx, 8);  /* 0FC120 beqs fc128 */
	{ uint32_t eaS = 0x000F842Cu; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 0FC122 lea f842c,%a0 */
L_0FC128:
	{ uint32_t eaD = 0xFFFF001Au; int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = alu(ctx, OP_ADD, r16(eaD), 0x1u, 16, 1); w16(eaD, v); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC128 addqw #1,ffff001a */
	{ uint32_t eaS = 0xFFFF001Cu; int io = !plain(eaS); if (io) hle_pre(ctx); uint32_t v = r16(eaS); set_w(&ctx->d[4], v); cc_logic16(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC12E movew ffff001c,%d4 */
	{ uint32_t eaS = 0xFFFF001Au; int io = !plain(eaS); if (io) hle_pre(ctx); alu(ctx, OP_SUB, (ctx->d[4] & 0xFFFFu), r16(eaS), 16, 0); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC134 cmpw ffff001a,%d4 */
	if (COND_CC) { hle_insn(ctx, 10); goto L_0FC182; } hle_insn(ctx, 8);  /* 0FC13A bccs fc182 */
	{ uint32_t eaD = 0xFFFF001Au; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC13C clrw ffff001a */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FC146u); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC194u, 1);  /* 0FC142 bsrw fc194 */
	{ uint32_t eaD = 0xFFFF0018u; int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = alu(ctx, OP_ADD, r16(eaD), 0x1u, 16, 1); w16(eaD, v); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC146 addqw #1,ffff0018 */
	{ uint32_t eaD = 0xFFFF0018u; int io = !plain(eaD); if (io) hle_pre(ctx); alu(ctx, OP_SUB, r16(eaD), 0x40u, 16, 0); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC14C cmpiw #64,ffff0018 */
	if (COND_CS) { hle_insn(ctx, 10); goto L_0FC180; } hle_insn(ctx, 8);  /* 0FC154 bcss fc180 */
	{ uint32_t eaD = 0xFFFF0090u; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC156 clrw ffff0090 */
	{ uint32_t eaD = 0xFFFF008Eu; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC15C clrw ffff008e */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FC166u); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC0FEu, 1);  /* 0FC162 bsrw fc0fe */
	{ uint32_t eaD = 0xFFFF0018u; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC166 clrw ffff0018 */
	{ uint32_t eaD = 0xFFFF0016u; int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = alu(ctx, OP_ADD, r16(eaD), 0x1u, 16, 1); w16(eaD, v); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC16C addqw #1,ffff0016 */
	{ uint32_t eaD = 0xFFFF0016u; int io = !plain(eaD); if (io) hle_pre(ctx); cc_logic16(ctx, r16(eaD)); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC172 tstw ffff0016 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0FC180; } hle_insn(ctx, 8);  /* 0FC178 bnes fc180 */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FC17Eu); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC184u, 1);  /* 0FC17A bsrw fc184 */
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FC17E rts */
L_0FC180:
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FC180 rts */
L_0FC182:
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FC182 rts */
L_0FC184:
	{ uint32_t v = 0x1u; set_w(&ctx->d[2], v); cc_logic16(ctx, v); hle_insn(ctx, 8); }  /* 0FC184 movew #1,%d2 */
	{ uint32_t v = 0x0u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); hle_insn(ctx, 8); }  /* 0FC188 movew #0,%d0 */
	{ uint32_t v = 0xFFu; set_w(&ctx->d[1], v); cc_logic16(ctx, v); hle_insn(ctx, 8); }  /* 0FC18C movew #255,%d1 */
	hle_insn(ctx, 10); goto L_0FC2D6;  /* 0FC190 braw fc2d6 */
L_0FC194:
	{ uint32_t eaD = 0x000F8020u; int io = !plain(eaD); if (io) hle_pre(ctx); cc_logic16(ctx, r16(eaD)); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC194 tstw f8020 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0FC19E; } hle_insn(ctx, 8);  /* 0FC19A beqs fc19e */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), (ctx->d[0] & 0xFFFFu), 16, 1); set_w(&ctx->d[0], v); hle_insn(ctx, 4); }  /* 0FC19C addw %d0,%d0 */
L_0FC19E:
	{ uint32_t res = (ctx->d[0] & 0xFFFFu) * 0x6u; ctx->d[0] = res; cc_logic32(ctx, res); } hle_insn(ctx, 46);  /* 0FC19E muluw #6,%d0 */
	{ ctx->a[0] += (uint32_t)(int32_t)(int16_t)(ctx->d[0] & 0xFFFFu); hle_insn(ctx, 8); }  /* 0FC1A2 addaw %d0,%a0 */
	{ ctx->d[0] = 0x00000000u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 0FC1A4 moveq #0,%d0 */
	{ uint32_t eaD = 0x000F8020u; int io = !plain(eaD); if (io) hle_pre(ctx); cc_logic16(ctx, r16(eaD)); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC1A6 tstw f8020 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0FC1B2; } hle_insn(ctx, 8);  /* 0FC1AC beqs fc1b2 */
	{ uint32_t eaS = ctx->a[0]; ctx->a[0] += 2; int io = !plain(eaS); if (io) hle_pre(ctx); uint32_t v = r16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); if (io) hle_post(ctx, 8); else hle_insn(ctx, 8); }  /* 0FC1AE movew %a0@+,%d0 */
	hle_insn(ctx, 10); goto L_0FC1B4;  /* 0FC1B0 bras fc1b4 */
L_0FC1B2:
	{ uint32_t eaS = ctx->a[0]; ctx->a[0] += 1; int io = !plain(eaS); if (io) hle_pre(ctx); uint32_t v = r8(eaS); set_b(&ctx->d[0], v); cc_logic8(ctx, v); if (io) hle_post(ctx, 8); else hle_insn(ctx, 8); }  /* 0FC1B2 moveb %a0@+,%d0 */
L_0FC1B4:
	{ uint32_t eaD = ctx->a[5] + (uint32_t)(int32_t)(2); int io = !plain(eaD); if (io) hle_pre(ctx); cc_logic16(ctx, r16(eaD)); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FC1B4 tstw %a5@(2) */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0FC1C4; } hle_insn(ctx, 8);  /* 0FC1B8 beqs fc1c4 */
	{ uint32_t eaD = ctx->a[5] + (uint32_t)(int32_t)(2); int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = alu(ctx, OP_SUB, r16(eaD), 0x1u, 16, 1); w16(eaD, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC1BA subqw #1,%a5@(2) */
	{ uint32_t eaD = ctx->a[5] + (uint32_t)(int32_t)(0); int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = alu(ctx, OP_ADD, r16(eaD), 0x1u, 16, 1); w16(eaD, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC1BE addqw #1,%a5@(0) */
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FC1C2 rts */
L_0FC1C4:
	{ uint32_t v = shf(ctx, 1, (ctx->d[0] & 0xFFFFu), 2, 16); set_w(&ctx->d[0], v); } hle_insn(ctx, 10);  /* 0FC1C4 lslw #2,%d0 */
	{ uint32_t eaS = 0x000F8022u; ctx->a[1] = eaS; } hle_insn(ctx, 8);  /* 0FC1C6 lea %pc@(f8022),%a1 */
	{ ctx->a[1] += (uint32_t)(int32_t)(int16_t)(ctx->d[0] & 0xFFFFu); hle_insn(ctx, 8); }  /* 0FC1CA addaw %d0,%a1 */
	{ uint32_t eaS = ctx->a[1]; int io = !plain(eaS); if (io) hle_pre(ctx); ctx->a[1] = r32(eaS); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FC1CC moveal %a1@,%a1 */
	{ uint32_t eaS = 0xFFFF0018u; int io = !plain(eaS); if (io) hle_pre(ctx); uint32_t v = r16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC1CE movew ffff0018,%d0 */
	{ uint32_t eaS = ctx->a[5] + (uint32_t)(int32_t)(0); int io = !plain(eaS); if (io) hle_pre(ctx); uint32_t v = alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), r16(eaS), 16, 1); set_w(&ctx->d[0], v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FC1D4 subw %a5@(0),%d0 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), (ctx->d[0] & 0xFFFFu), 16, 1); set_w(&ctx->d[0], v); hle_insn(ctx, 4); }  /* 0FC1D8 addw %d0,%d0 */
	{ ctx->a[1] += (uint32_t)(int32_t)(int16_t)(ctx->d[0] & 0xFFFFu); hle_insn(ctx, 8); }  /* 0FC1DA addaw %d0,%a1 */
	{ ctx->d[0] = 0x00000000u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 0FC1DC moveq #0,%d0 */
	{ ctx->d[1] = 0x00000000u; cc_logic32(ctx, ctx->d[1]); } hle_insn(ctx, 4);  /* 0FC1DE moveq #0,%d1 */
	{ uint32_t eaS = ctx->a[1]; ctx->a[1] += 1; int io = !plain(eaS); if (io) hle_pre(ctx); uint32_t v = r8(eaS); set_b(&ctx->d[0], v); cc_logic8(ctx, v); if (io) hle_post(ctx, 8); else hle_insn(ctx, 8); }  /* 0FC1E0 moveb %a1@+,%d0 */
	{ uint32_t eaS = ctx->a[1]; ctx->a[1] += 1; int io = !plain(eaS); if (io) hle_pre(ctx); uint32_t v = r8(eaS); set_b(&ctx->d[1], v); cc_logic8(ctx, v); if (io) hle_post(ctx, 8); else hle_insn(ctx, 8); }  /* 0FC1E2 moveb %a1@+,%d1 */
	{ alu(ctx, OP_SUB, (ctx->d[1] & 0xFFu), 0xFFu, 8, 0); hle_insn(ctx, 8); }  /* 0FC1E4 cmpib #-1,%d1 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0FC1F6; } hle_insn(ctx, 8);  /* 0FC1E8 bnes fc1f6 */
	{ cc_logic8(ctx, (ctx->d[0] & 0xFFu)); hle_insn(ctx, 4); }  /* 0FC1EA tstb %d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0FC1F2; } hle_insn(ctx, 8);  /* 0FC1EC beqs fc1f2 */
	{ uint32_t eaD = ctx->a[5] + (uint32_t)(int32_t)(2); int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = (ctx->d[0] & 0xFFFFu); w16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FC1EE movew %d0,%a5@(2) */
L_0FC1F2:
	{ ctx->d[0] = 0x00000000u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 0FC1F2 moveq #0,%d0 */
	{ ctx->d[1] = 0x00000000u; cc_logic32(ctx, ctx->d[1]); } hle_insn(ctx, 4);  /* 0FC1F4 moveq #0,%d1 */
L_0FC1F6:
	{ uint32_t v = (ctx->d[1] & 0xFFu); set_b(&ctx->d[2], v); cc_logic8(ctx, v); hle_insn(ctx, 4); }  /* 0FC1F6 moveb %d1,%d2 */
	{ uint32_t v = shf(ctx, 0, (ctx->d[1] & 0xFFu), 1, 8); set_b(&ctx->d[1], v); } hle_insn(ctx, 8);  /* 0FC1F8 lsrb #1,%d1 */
	{ alu(ctx, OP_SUB, (ctx->d[1] & 0xFFu), 0x7Cu, 8, 0); hle_insn(ctx, 8); }  /* 0FC1FA cmpib #124,%d1 */
	if (COND_CC) { hle_insn(ctx, 10); goto L_0FC22E; } hle_insn(ctx, 8);  /* 0FC1FE bccs fc22e */
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[1] & 0xFFu), 0x6Du, 8, 1); set_b(&ctx->d[1], v); hle_insn(ctx, 8); }  /* 0FC200 subib #109,%d1 */
	if (COND_CS) { hle_insn(ctx, 10); goto L_0FC22C; } hle_insn(ctx, 8);  /* 0FC204 bcss fc22c */
	{ alu(ctx, OP_SUB, (ctx->d[1] & 0xFFu), 0x8u, 8, 0); hle_insn(ctx, 8); }  /* 0FC206 cmpib #8,%d1 */
	if (COND_CC) { hle_insn(ctx, 10); goto L_0FC22C; } hle_insn(ctx, 8);  /* 0FC20A bccs fc22c */
	{ uint32_t eaS = 0x000F8000u; ctx->a[1] = eaS; } hle_insn(ctx, 12);  /* 0FC20C lea f8000,%a1 */
	{ uint32_t v = ((ctx->d[1] & 0xFFFFu) & 0xFFu) & 0xFFFFu; set_w(&ctx->d[1], v); cc_logic16(ctx, v); hle_insn(ctx, 8); }  /* 0FC212 andiw #255,%d1 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), (ctx->d[1] & 0xFFFFu), 16, 1); set_w(&ctx->d[1], v); hle_insn(ctx, 4); }  /* 0FC216 addw %d1,%d1 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), (ctx->d[1] & 0xFFFFu), 16, 1); set_w(&ctx->d[1], v); hle_insn(ctx, 4); }  /* 0FC218 addw %d1,%d1 */
	{ ctx->a[1] += (uint32_t)(int32_t)(int16_t)(ctx->d[1] & 0xFFFFu); hle_insn(ctx, 8); }  /* 0FC21A addaw %d1,%a1 */
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[1], v); cc_logic16(ctx, v); hle_insn(ctx, 4); }  /* 0FC21C movew %d0,%d1 */
	{ uint32_t eaS = ctx->a[1]; int io = !plain(eaS); if (io) hle_pre(ctx); uint32_t v = r16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); if (io) hle_post(ctx, 8); else hle_insn(ctx, 8); }  /* 0FC21E movew %a1@,%d0 */
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) | 0x8000u) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); hle_insn(ctx, 8); }  /* 0FC220 oriw #-32768,%d0 */
	{ uint32_t eaS = ctx->a[1] + (uint32_t)(int32_t)(2); int io = !plain(eaS); if (io) hle_pre(ctx); uint32_t v = r16(eaS); set_w(&ctx->d[2], v); cc_logic16(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FC224 movew %a1@(2),%d2 */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FC22Cu); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC2D6u, 1);  /* 0FC228 bsrw fc2d6 */
L_0FC22C:
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FC22C rts */
L_0FC22E:
	{ alu(ctx, OP_SUB, (ctx->d[1] & 0xFFu), 0x7Cu, 8, 0); hle_insn(ctx, 8); }  /* 0FC22E cmpib #124,%d1 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0FC27A; } hle_insn(ctx, 12);  /* 0FC232 beqw fc27a */
	{ alu(ctx, OP_SUB, (ctx->d[1] & 0xFFu), 0x7Du, 8, 0); hle_insn(ctx, 8); }  /* 0FC236 cmpib #125,%d1 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0FC25E; } hle_insn(ctx, 8);  /* 0FC23A beqs fc25e */
	{ alu(ctx, OP_SUB, (ctx->d[1] & 0xFFu), 0x7Eu, 8, 0); hle_insn(ctx, 8); }  /* 0FC23C cmpib #126,%d1 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0FC24C; } hle_insn(ctx, 8);  /* 0FC240 beqs fc24c */
	{ uint32_t eaD = 0xFFFF0018u; int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = 0x3Fu; w16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC242 movew #63,ffff0018 */
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FC24A rts */
L_0FC24C:
	{ uint32_t v = alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x1u, 16, 1); set_w(&ctx->d[0], v); hle_insn(ctx, 4); }  /* 0FC24C subqw #1,%d0 */
	{ uint32_t eaD = 0xFFFF0018u; int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = 0x3Fu; w16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC24E movew #63,ffff0018 */
	{ uint32_t eaD = 0xFFFF0016u; int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = (ctx->d[0] & 0xFFFFu); w16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC256 movew %d0,ffff0016 */
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FC25C rts */
L_0FC25E:
	{ uint32_t eaS = 0xFFFF0000u; int io = !plain(eaS); if (io) hle_pre(ctx); ctx->a[6] = r32(eaS); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC25E moveal ffff0000,%fp */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FC268u); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC408u, 1);  /* 0FC264 bsrw fc408 */
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0x7Fu) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); hle_insn(ctx, 8); }  /* 0FC268 andiw #127,%d0 */
	{ uint32_t eaD = 0xFFFF001Cu; int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = (ctx->d[0] & 0xFFFFu); w16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC26C movew %d0,ffff001c */
	{ uint32_t eaD = 0xFFFF001Au; int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = (ctx->d[0] & 0xFFFFu); w16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC272 movew %d0,ffff001a */
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FC278 rts */
L_0FC27A:
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FC27A rts */
	{ uint32_t v = (ctx->d[0] & 0xFFFFFFFFu); ctx->d[3] = v; cc_logic32(ctx, v); hle_insn(ctx, 4); }  /* 0FC27C movel %d0,%d3 */
	{ uint32_t v = shf(ctx, 0, (ctx->d[3] & 0xFFFFFFFFu), 8, 32); ctx->d[3] = v; } hle_insn(ctx, 24);  /* 0FC27E lsrl #8,%d3 */
	{ uint32_t v = shf(ctx, 0, (ctx->d[3] & 0xFFFFFFFFu), 7, 32); ctx->d[3] = v; } hle_insn(ctx, 22);  /* 0FC280 lsrl #7,%d3 */
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) & 0x7FFFu) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); hle_insn(ctx, 8); }  /* 0FC282 andiw #32767,%d0 */
	{ uint32_t v = ((ctx->d[0] & 0xFFFFu) | 0x8000u) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); hle_insn(ctx, 8); }  /* 0FC286 oriw #-32768,%d0 */
	hle_insn(ctx, 10); goto L_0FC290;  /* 0FC28A bras fc290 */
	{ uint32_t v = 0x1Eu; set_w(&ctx->d[3], v); cc_logic16(ctx, v); hle_insn(ctx, 8); }  /* 0FC28C movew #30,%d3 */
L_0FC290:
	{ uint32_t eaD = 0xFFFF001Eu; int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = (ctx->d[1] & 0xFFFFu); w16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC290 movew %d1,ffff001e */
	{ uint32_t eaD = 0xFFFF0022u; int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = (ctx->d[0] & 0xFFFFu); w16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC296 movew %d0,ffff0022 */
	{ uint32_t eaD = 0xFFFF0020u; int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = (ctx->d[2] & 0xFFFFu); w16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC29C movew %d2,ffff0020 */
	{ uint32_t eaD = 0xFFFF0024u; int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = (ctx->d[3] & 0xFFFFu); w16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC2A2 movew %d3,ffff0024 */
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FC2A8 rts */
L_0FC2AA:
	{ uint32_t eaD = 0xFFFF0016u; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC2AA clrw ffff0016 */
	{ uint32_t eaD = 0xFFFF0018u; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC2B0 clrw ffff0018 */
	{ uint32_t eaS = 0xFFFF001Eu; int io = !plain(eaS); if (io) hle_pre(ctx); uint32_t v = r16(eaS); set_w(&ctx->d[1], v); cc_logic16(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC2B6 movew ffff001e,%d1 */
	{ uint32_t eaS = 0xFFFF0022u; int io = !plain(eaS); if (io) hle_pre(ctx); uint32_t v = r16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC2BC movew ffff0022,%d0 */
	{ uint32_t eaS = 0xFFFF0020u; int io = !plain(eaS); if (io) hle_pre(ctx); uint32_t v = r16(eaS); set_w(&ctx->d[2], v); cc_logic16(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC2C2 movew ffff0020,%d2 */
	{ uint32_t eaS = 0xFFFF0024u; int io = !plain(eaS); if (io) hle_pre(ctx); uint32_t v = r16(eaS); set_w(&ctx->d[3], v); cc_logic16(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC2C8 movew ffff0024,%d3 */
	{ uint32_t eaD = 0xFFFF001Eu; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC2CE clrw ffff001e */
	hle_insn(ctx, 10); goto L_0FC2DA;  /* 0FC2D4 bras fc2da */
L_0FC2D6:
	{ uint32_t v = 0x1Eu; set_b(&ctx->d[3], v); cc_logic8(ctx, v); hle_insn(ctx, 8); }  /* 0FC2D6 moveb #30,%d3 */
L_0FC2DA:
	{ uint32_t eaD = 0xFFFF000Cu; int io = !plain(eaD); if (io) hle_pre(ctx); cc_logic16(ctx, r16(eaD)); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC2DA tstw ffff000c */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0FC310; } hle_insn(ctx, 8);  /* 0FC2E0 bnes fc310 */
	{ uint32_t eaS = 0xFFFF0000u; int io = !plain(eaS); if (io) hle_pre(ctx); ctx->a[6] = r32(eaS); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC2E2 moveal ffff0000,%fp */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FC2ECu); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC408u, 1);  /* 0FC2E8 bsrw fc408 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(16); int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = (ctx->d[0] & 0xFFu); w8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FC2EC moveb %d0,%fp@(16) */
	{ uint32_t v = shf(ctx, 0, (ctx->d[0] & 0xFFFFu), 8, 16); set_w(&ctx->d[0], v); } hle_insn(ctx, 22);  /* 0FC2F0 lsrw #8,%d0 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(17); int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = (ctx->d[0] & 0xFFu); w8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FC2F2 moveb %d0,%fp@(17) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(18); int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = (ctx->d[1] & 0xFFu); w8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FC2F6 moveb %d1,%fp@(18) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(20); int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = (ctx->d[2] & 0xFFu); w8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FC2FA moveb %d2,%fp@(20) */
	{ uint32_t v = shf(ctx, 0, (ctx->d[2] & 0xFFFFu), 8, 16); set_w(&ctx->d[2], v); } hle_insn(ctx, 22);  /* 0FC2FE lsrw #8,%d2 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(21); int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = (ctx->d[2] & 0xFFu); w8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FC300 moveb %d2,%fp@(21) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(22);  int io = !plain(eaD); if (io) hle_pre(ctx); w8(eaD, 0xFF); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC304 st %fp@(22) */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(66); int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = (ctx->d[3] & 0xFFu); w8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FC308 moveb %d3,%fp@(66) */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FC310u); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC428u, 1);  /* 0FC30C bsrw fc428 */
L_0FC310:
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FC310 rts */
	/* ---- gap ---- */
L_0FC408:
	{ uint32_t eaD = 0x00A11100u; int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = 0x1u; w8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC408 moveb #1,a11100 */
	{ ctx->a[7] -= 4; uint32_t eaD = ctx->a[7]; int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = (ctx->d[0] & 0xFFFFFFFFu); w32(eaD, v); cc_logic32(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FC410 movel %d0,%sp@- */
L_0FC412:
	{ uint32_t eaS = 0x00A11100u; int io = !plain(eaS); if (io) hle_pre(ctx); uint32_t v = r8(eaS); set_b(&ctx->d[0], v); cc_logic8(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC412 moveb a11100,%d0 */
	{ uint32_t v = ((ctx->d[0] & 0xFFu) & 0x1u) & 0xFFu; set_b(&ctx->d[0], v); cc_logic8(ctx, v); hle_insn(ctx, 8); }  /* 0FC418 andib #1,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0FC412; } hle_insn(ctx, 8);  /* 0FC41C bnes fc412 */
	{ uint32_t eaD = 0xFFFF009Eu;  int io = !plain(eaD); if (io) hle_pre(ctx); w8(eaD, 0xFF); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC41E st ffff009e */
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 4; int io = !plain(eaS); if (io) hle_pre(ctx); uint32_t v = r32(eaS); ctx->d[0] = v; cc_logic32(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FC424 movel %sp@+,%d0 */
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FC426 rts */
L_0FC428:
	{ uint32_t eaD = 0x00A11100u; int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = 0x0u; w8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC428 moveb #0,a11100 */
	{ uint32_t eaD = 0xFFFF009Eu;  int io = !plain(eaD); if (io) hle_pre(ctx); w8(eaD, 0); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC430 sf ffff009e */
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FC436 rts */
	/* ---- gap ---- */
L_0FC47A:
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FC47Eu); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC59Cu, 1);  /* 0FC47A bsrw fc59c */
	{ uint32_t eaD = ctx->a[6]; int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = 0x2u; w8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FC47E moveb #2,%fp@ */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FC486u); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC428u, 1);  /* 0FC482 bsrw fc428 */
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FC486 rts */
L_0FC488:
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FC48Cu); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC59Cu, 1);  /* 0FC488 bsrw fc59c */
	{ uint32_t eaD = ctx->a[6]; int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = 0x4u; w8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FC48C moveb #4,%fp@ */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FC494u); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC428u, 1);  /* 0FC490 bsrw fc428 */
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FC494 rts */
L_0FC496:
	{ uint32_t eaS = 0xFFFF0012u; int io = !plain(eaS); if (io) hle_pre(ctx); uint32_t v = r16(eaS); set_w(&ctx->d[0], v); cc_logic16(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC496 movew ffff0012,%d0 */
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x2u, 16, 0); hle_insn(ctx, 8); }  /* 0FC49C cmpiw #2,%d0 */
	if (COND_CC) { hle_insn(ctx, 10); goto L_0FC4BA; } hle_insn(ctx, 12);  /* 0FC4A0 bccw fc4ba */
	{ uint32_t eaD = 0xFFFF0032u; int io = !plain(eaD); if (io) hle_pre(ctx); cc_logic8(ctx, r8(eaD)); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC4A4 tstb ffff0032 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0FC4BA; } hle_insn(ctx, 12);  /* 0FC4AA bnew fc4ba */
	{ uint32_t eaS = 0xFFFF0000u; int io = !plain(eaS); if (io) hle_pre(ctx); ctx->a[6] = r32(eaS); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC4AE moveal ffff0000,%fp */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FC4B8u); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC408u, 1);  /* 0FC4B4 bsrw fc408 */
	hle_insn(ctx, 10); goto L_0FC4CA;  /* 0FC4B8 bras fc4ca */
L_0FC4BA:
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FC4BEu); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC59Cu, 1);  /* 0FC4BA bsrw fc59c */
	{ uint32_t eaS = 0xFFFF009Au; uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(58); int io = !plain(eaS) || !plain(eaD); if (io) hle_pre(ctx); uint32_t v = r8(eaS); w8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 24); else hle_insn(ctx, 24); }  /* 0FC4BE moveb ffff009a,%fp@(58) */
	{ uint32_t eaD = ctx->a[6]; int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = 0x1u; w8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FC4C6 moveb #1,%fp@ */
L_0FC4CA:
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(56); uint32_t eaD = 0xFFFF00A4u; int io = !plain(eaS) || !plain(eaD); if (io) hle_pre(ctx); uint32_t v = r8(eaS); w8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 24); else hle_insn(ctx, 24); }  /* 0FC4CA moveb %fp@(56),ffff00a4 */
	{ ctx->d[0] = 0x00000000u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 0FC4D2 moveq #0,%d0 */
	{ uint32_t eaD = 0x00A10001u;  int io = !plain(eaD); if (io) hle_pre(ctx); uint8_t v = r8(eaD); cc_set(ctx, CC_Z, ((v >> 6) & 1) ? 0 : CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC4D4 btst #6,a10001 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0FC4E0; } hle_insn(ctx, 8);  /* 0FC4DC bnes fc4e0 */
	{ uint32_t v = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), 0x1u, 16, 1); set_w(&ctx->d[0], v); hle_insn(ctx, 4); }  /* 0FC4DE addqw #1,%d0 */
L_0FC4E0:
	{ uint32_t eaS = 0xFFFF0006u; int io = !plain(eaS); if (io) hle_pre(ctx); alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), r16(eaS), 16, 0); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC4E0 cmpw ffff0006,%d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0FC4F4; } hle_insn(ctx, 8);  /* 0FC4E6 beqs fc4f4 */
	{ uint32_t eaD = 0xFFFF0006u; int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = (ctx->d[0] & 0xFFFFu); w16(eaD, v); cc_logic16(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC4E8 movew %d0,ffff0006 */
	{ uint32_t eaD = 0xFFFF0008u; int io = !plain(eaD); if (io) hle_pre(ctx); w16(eaD, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC4EE clrw ffff0008 */
L_0FC4F4:
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FC4F8u); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC428u, 1);  /* 0FC4F4 bsrw fc428 */
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FC4F8 rts */
L_0FC4FA:
	{ uint32_t eaD = 0xFFFF0032u; int io = !plain(eaD); if (io) hle_pre(ctx); cc_logic8(ctx, r8(eaD)); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC4FA tstb ffff0032 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0FC538; } hle_insn(ctx, 12);  /* 0FC500 bnew fc538 */
	{ uint32_t eaD = 0xFFFF0012u; int io = !plain(eaD); if (io) hle_pre(ctx); alu(ctx, OP_SUB, r16(eaD), 0x1u, 16, 0); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC504 cmpiw #1,ffff0012 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0FC512; } hle_insn(ctx, 12);  /* 0FC50C bnew fc512 */
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FC510 rts */
L_0FC512:
	{ uint32_t eaS = 0xFFFF003Au; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 0FC512 lea ffff003a,%a0 */
	{ ctx->d[0] = 0x00000000u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4);  /* 0FC518 moveq #0,%d0 */
	{ uint32_t eaS = ctx->a[0] + (uint32_t)(int32_t)(4); int io = !plain(eaS); if (io) hle_pre(ctx); uint32_t v = ((ctx->d[0] & 0xFFFFu) | r16(eaS)) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FC51A orw %a0@(4),%d0 */
	{ uint32_t eaS = ctx->a[0] + (uint32_t)(int32_t)(16); int io = !plain(eaS); if (io) hle_pre(ctx); uint32_t v = ((ctx->d[0] & 0xFFFFu) | r16(eaS)) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FC51E orw %a0@(16),%d0 */
	{ uint32_t eaS = ctx->a[0] + (uint32_t)(int32_t)(28); int io = !plain(eaS); if (io) hle_pre(ctx); uint32_t v = ((ctx->d[0] & 0xFFFFu) | r16(eaS)) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FC522 orw %a0@(28),%d0 */
	{ uint32_t eaS = ctx->a[0] + (uint32_t)(int32_t)(40); int io = !plain(eaS); if (io) hle_pre(ctx); uint32_t v = ((ctx->d[0] & 0xFFFFu) | r16(eaS)) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FC526 orw %a0@(40),%d0 */
	{ uint32_t eaS = ctx->a[0] + (uint32_t)(int32_t)(52); int io = !plain(eaS); if (io) hle_pre(ctx); uint32_t v = ((ctx->d[0] & 0xFFFFu) | r16(eaS)) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FC52A orw %a0@(52),%d0 */
	{ uint32_t eaS = ctx->a[0] + (uint32_t)(int32_t)(64); int io = !plain(eaS); if (io) hle_pre(ctx); uint32_t v = ((ctx->d[0] & 0xFFFFu) | r16(eaS)) & 0xFFFFu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FC52E orw %a0@(64),%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0FC538; } hle_insn(ctx, 12);  /* 0FC532 bnew fc538 */
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FC536 rts */
L_0FC538:
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FC53Cu); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC59Cu, 1);  /* 0FC538 bsrw fc59c */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(2); ctx->a[5] = eaS; } hle_insn(ctx, 8);  /* 0FC53C lea %fp@(2),%a5 */
	{ uint32_t eaS = ctx->a[6] + (uint32_t)(int32_t)(56); uint32_t eaD = 0xFFFF00A4u; int io = !plain(eaS) || !plain(eaD); if (io) hle_pre(ctx); uint32_t v = r8(eaS); w8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 24); else hle_insn(ctx, 24); }  /* 0FC540 moveb %fp@(56),ffff00a4 */
	{ uint32_t v = 0x5u; set_w(&ctx->d[7], v); cc_logic16(ctx, v); hle_insn(ctx, 8); }  /* 0FC548 movew #5,%d7 */
	{ uint32_t eaS = 0xFFFF003Au; ctx->a[0] = eaS; } hle_insn(ctx, 12);  /* 0FC54C lea ffff003a,%a0 */
L_0FC552:
	{ uint32_t eaS = ctx->a[0] + (uint32_t)(int32_t)(4); uint32_t eaD = ctx->a[5]; ctx->a[5] += 1; int io = !plain(eaS) || !plain(eaD); if (io) hle_pre(ctx); uint32_t v = r8(eaS); w8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC552 moveb %a0@(4),%a5@+ */
	{ uint32_t eaS = ctx->a[0] + (uint32_t)(int32_t)(5); uint32_t eaD = ctx->a[5]; ctx->a[5] += 1; int io = !plain(eaS) || !plain(eaD); if (io) hle_pre(ctx); uint32_t v = r8(eaS); w8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 16); else hle_insn(ctx, 16); }  /* 0FC556 moveb %a0@(5),%a5@+ */
	{ uint32_t eaS = ctx->a[0] + (uint32_t)(int32_t)(12); ctx->a[0] = eaS; } hle_insn(ctx, 8);  /* 0FC55A lea %a0@(12),%a0 */
	{ uint16_t v = (uint16_t)ctx->d[7] - 1; set_w(&ctx->d[7], v); if (v != 0xFFFF) { hle_insn(ctx, 10); goto L_0FC552; } hle_insn(ctx, 14); }  /* 0FC55E dbf %d7,fc552 */
	{ uint32_t eaD = ctx->a[6]; int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = 0xAu; w8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FC562 moveb #10,%fp@ */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FC56Au); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC428u, 1);  /* 0FC566 bsrw fc428 */
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FC56A rts */
	{ ctx->a[7] -= 4; uint32_t eaD = ctx->a[7]; int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = (ctx->a[6] & 0xFFFFFFFFu); w32(eaD, v); cc_logic32(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FC56C movel %fp,%sp@- */
	{ uint32_t eaS = 0xFFFF0000u; int io = !plain(eaS); if (io) hle_pre(ctx); ctx->a[6] = r32(eaS); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC56E moveal ffff0000,%fp */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FC578u); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC408u, 1);  /* 0FC574 bsrw fc408 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(64); int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = (ctx->d[0] & 0xFFu); w8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FC578 moveb %d0,%fp@(64) */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FC580u); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC428u, 1);  /* 0FC57C bsrw fc428 */
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 4; int io = !plain(eaS); if (io) hle_pre(ctx); ctx->a[6] = r32(eaS); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FC580 moveal %sp@+,%fp */
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FC582 rts */
	{ ctx->a[7] -= 4; uint32_t eaD = ctx->a[7]; int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = (ctx->a[6] & 0xFFFFFFFFu); w32(eaD, v); cc_logic32(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FC584 movel %fp,%sp@- */
	{ uint32_t eaS = 0xFFFF0000u; int io = !plain(eaS); if (io) hle_pre(ctx); ctx->a[6] = r32(eaS); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC586 moveal ffff0000,%fp */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FC590u); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC408u, 1);  /* 0FC58C bsrw fc408 */
	{ uint32_t eaD = ctx->a[6] + (uint32_t)(int32_t)(62); int io = !plain(eaD); if (io) hle_pre(ctx); uint32_t v = (ctx->d[0] & 0xFFu); w8(eaD, v); cc_logic8(ctx, v); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FC590 moveb %d0,%fp@(62) */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FC598u); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC428u, 1);  /* 0FC594 bsrw fc428 */
	{ uint32_t eaS = ctx->a[7]; ctx->a[7] += 4; int io = !plain(eaS); if (io) hle_pre(ctx); ctx->a[6] = r32(eaS); if (io) hle_post(ctx, 12); else hle_insn(ctx, 12); }  /* 0FC598 moveal %sp@+,%fp */
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FC59A rts */
L_0FC59C:
	{ uint32_t eaS = 0xFFFF0000u; int io = !plain(eaS); if (io) hle_pre(ctx); ctx->a[6] = r32(eaS); if (io) hle_post(ctx, 20); else hle_insn(ctx, 20); }  /* 0FC59C moveal ffff0000,%fp */
L_0FC5A2:
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FC5A6u); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC408u, 1);  /* 0FC5A2 bsrw fc408 */
	{ uint32_t eaS = ctx->a[6]; int io = !plain(eaS); if (io) hle_pre(ctx); uint32_t v = r8(eaS); set_b(&ctx->d[0], v); cc_logic8(ctx, v); if (io) hle_post(ctx, 8); else hle_insn(ctx, 8); }  /* 0FC5A6 moveb %fp@,%d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0FC5CA; } hle_insn(ctx, 8);  /* 0FC5A8 beqs fc5ca */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FC5AEu); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC428u, 1);  /* 0FC5AA bsrw fc428 */
	hle_insn(ctx, 4);  /* 0FC5AE nop */
	hle_insn(ctx, 4);  /* 0FC5B0 nop */
	hle_insn(ctx, 4);  /* 0FC5B2 nop */
	hle_insn(ctx, 4);  /* 0FC5B4 nop */
	hle_insn(ctx, 4);  /* 0FC5B6 nop */
	hle_insn(ctx, 4);  /* 0FC5B8 nop */
	hle_insn(ctx, 4);  /* 0FC5BA nop */
	hle_insn(ctx, 4);  /* 0FC5BC nop */
	hle_insn(ctx, 4);  /* 0FC5BE nop */
	hle_insn(ctx, 4);  /* 0FC5C0 nop */
	hle_insn(ctx, 4);  /* 0FC5C2 nop */
	hle_insn(ctx, 10); goto L_0FC5A2;  /* 0FC5C4 bras fc5a2 */
	{ ctx->a[7] -= 4; RAM32_W(ctx->a[7], 0x0FC5CAu); } hle_insn(ctx, 18); snd_run(ctx, 0x0FC408u, 1);  /* 0FC5C6 bsrw fc408 */
L_0FC5CA:
	hle_insn(ctx, 16); if (nested) ctx->a[7] += 4; return;  /* 0FC5CA rts */
}

/* the bound is the longest span of a call (the layer declines a call that could straddle the frame end) */
HLE_FUNC(0x0FBBD0, SndTick, SND_BOUND) { snd_run(ctx, 0x0FBBD0u, 0); }  /* the tick wrapper: takes the Z80 bus, runs the tick body, releases */
HLE_FUNC(0x0FBBFE, SndTickBody, SND_BOUND) { snd_run(ctx, 0x0FBBFEu, 0); }  /* the tick body */
HLE_FUNC(0x0FBDDC, SndChanUpdate2, SND_BOUND) { snd_run(ctx, 0x0FBDDCu, 0); }  /* channel update, then set the 0FBDE2 result */
HLE_FUNC(0x0FBDE2, SndChanDone, SND_BOUND) { snd_run(ctx, 0x0FBDE2u, 0); }  /* copy $FF0086 to $FF006E, d0 = 1 */
HLE_FUNC(0x0FBDF0, SndChanUpdate3, SND_BOUND) { snd_run(ctx, 0x0FBDF0u, 0); }  /* channel update, then clear the channel words */
HLE_FUNC(0x0FBE18, SndChanUpdate, SND_BOUND) { snd_run(ctx, 0x0FBE18u, 0); }  /* channel update */
HLE_FUNC(0x0FBF08, SndNoteFetch0, SND_BOUND) { snd_run(ctx, 0x0FBF08u, 0); }  /* note data fetch for the music channel set */
HLE_FUNC(0x0FBF26, SndNoteFetch, SND_BOUND) { snd_run(ctx, 0x0FBF26u, 0); }  /* note data fetch (6-byte entries per channel) */
HLE_FUNC(0x0FBF58, SndEvent, SND_BOUND) { snd_run(ctx, 0x0FBF58u, 0); }  /* channel event reader */
HLE_FUNC(0x0FC0A6, SndClearChan, SND_BOUND) { snd_run(ctx, 0x0FC0A6u, 0); }  /* clear the six channel word pairs */
HLE_FUNC(0x0FC0F0, SndClear0F0, SND_BOUND) { snd_run(ctx, 0x0FC0F0u, 0); }  /* clear $FF0084/$FF0082 */
HLE_FUNC(0x0FC10C, SndSfxTick, SND_BOUND) { snd_run(ctx, 0x0FC10Cu, 0); }  /* sound-effect channel tick */
HLE_FUNC(0x0FC184, SndSfxStop, SND_BOUND) { snd_run(ctx, 0x0FC184u, 0); }  /* sfx stop request */
HLE_FUNC(0x0FC194, SndSfxFetch, SND_BOUND) { snd_run(ctx, 0x0FC194u, 0); }  /* sfx note data fetch */
HLE_FUNC(0x0FC2AA, SndSlotFlush, SND_BOUND) { snd_run(ctx, 0x0FC2AAu, 0); }  /* send the queued command slots to the Z80 */
HLE_FUNC(0x0FC2D6, SndZ80Cmd1E, SND_BOUND) { snd_run(ctx, 0x0FC2D6u, 0); }  /* write a command block to the Z80 (d3 = $1E) */
HLE_FUNC(0x0FC2DA, SndZ80Cmd, SND_BOUND) { snd_run(ctx, 0x0FC2DAu, 0); }  /* write a command block to the Z80 */
HLE_FUNC(0x0FC408, SndBusReq, SND_BOUND) { snd_run(ctx, 0x0FC408u, 0); }  /* Z80 BUSREQ acquire + spin */
HLE_FUNC(0x0FC428, SndBusRel, SND_BOUND) { snd_run(ctx, 0x0FC428u, 0); }  /* Z80 BUSREQ release */
HLE_FUNC(0x0FC47A, SndZ80Cmd2, SND_BOUND) { snd_run(ctx, 0x0FC47Au, 0); }  /* Z80 command byte 2 */
HLE_FUNC(0x0FC488, SndZ80Cmd4, SND_BOUND) { snd_run(ctx, 0x0FC488u, 0); }  /* Z80 command byte 4 */
HLE_FUNC(0x0FC496, SndUploadCheck, SND_BOUND) { snd_run(ctx, 0x0FC496u, 0); }  /* driver-side upload / region flag check */
HLE_FUNC(0x0FC4FA, SndFlagCheck, SND_BOUND) { snd_run(ctx, 0x0FC4FAu, 0); }  /* channel flag check, upload to the Z80 */
HLE_FUNC(0x0FC59C, SndWaitZ80, SND_BOUND) { snd_run(ctx, 0x0FC59Cu, 0); }  /* wait until the Z80 command byte is 0 */
