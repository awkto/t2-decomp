/* Level-script `native` opcode ($14, ScrNative $00A140) and its targets (docs/subsystems/levels.md,
 * "native"): the routine address is a long script argument; the interpreter's `jsr (a1)` lands in
 * ScrNative, which `jmp`s to the target, and the target's closing `rts` returns to the interpreter.
 *
 * ScrNative is a range that stops in front of its `jmp (a1)`; the real 68k jumps and the CPU hook
 * replaces the target at its entry address. The targets are one-off spawns (a handful of boss and
 * set-piece objects built with ObjAlloc/ObjSpawnTemplate), tile/palette queues, player setup
 * (ScrSetPlayers $00B2FA), the demo skip (ScrDemoSkip $00B35A) and palette loads (ScrLoadPalette
 * $00A2A8). Four read their own arguments (a0 on entry, left after them): $00A2A8 (line.w,
 * palette.l), $00B35A (offset.w), $00A82C and $00ACB0 (one word).
 *
 * Generated instruction by instruction from the disassembly (every line carries its address and
 * the GPGX cost of its opcode word), as scriptspawn.c: the called subroutines are static C copies
 * (ObjAlloc/ObjSpawnTemplate/Random are the existing C functions). Targets that touch only work RAM
 * are HLE_FUNC_PURE; ScrLoadPalette ($00A2A8, CramUpload), $00A4B2 (sub_0081B6, VDP) and $009290
 * (PrintText) call the 68k with hle_call68k, are not pure, and carry a measured decline bound.
 * Not ported: $013C74 (runs 248 frames of the object loop inside itself; it is interrupted by
 * every VBlank, so the C version could never run), see docs/porting.md.
 */
#include "hle.h"

#include <stdint.h>
#include <string.h>

void ObjAlloc(hle_ctx *ctx);
void ObjSpawnTemplate(hle_ctx *ctx);
void Random(hle_ctx *ctx);

enum { OP_ADD, OP_SUB };

/* a (op) b at `bits` width with the 68k flags (X set as well when setx) */
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

static __attribute__((unused)) int c_z(hle_ctx *c) { return (c->sr & CC_Z) != 0; }
static __attribute__((unused)) int c_n(hle_ctx *c) { return (c->sr & CC_N) != 0; }
static __attribute__((unused)) int c_c(hle_ctx *c) { return (c->sr & CC_C) != 0; }
static __attribute__((unused)) int c_v(hle_ctx *c) { return (c->sr & CC_V) != 0; }
static __attribute__((unused)) int c_ge(hle_ctx *c) { return c_n(c) == c_v(c); }
static __attribute__((unused)) int c_le(hle_ctx *c) { return c_z(c) || c_n(c) != c_v(c); }
static __attribute__((unused)) int c_hi(hle_ctx *c) { return !c_c(c) && !c_z(c); }
static __attribute__((unused)) int c_gt(hle_ctx *c) { return !c_z(c) && c_n(c) == c_v(c); }

/* work RAM or ROM by address */
static inline int is_ram(uint32_t a) { return (a & 0xFFFFFFu) >= 0xE00000u; }
static inline uint8_t ld8(uint32_t a) { return is_ram(a) ? RAM8(a) : ROM8(a); }
static inline uint16_t ld16(uint32_t a) { return is_ram(a) ? RAM16(a) : ROM16(a); }
static inline uint32_t ld32(uint32_t a) { return ((uint32_t)ld16(a) << 16) | ld16(a + 2); }
static inline void st8(uint32_t a, uint32_t v) { RAM8(a) = (uint8_t)v; }
static inline void st16(uint32_t a, uint32_t v) { RAM16(a) = (uint16_t)v; }
static inline void st32(uint32_t a, uint32_t v) { RAM32_W(a, v); }

/* asl/asr/lsl/lsr #n,Dr (1..8) at `bits` width; the caller charges base + 2n */
static void sh_n(hle_ctx *c, int r, const char *kind, int n, int bits)
{
	uint32_t m = bits == 32 ? 0xFFFFFFFFu : ((1u << bits) - 1), sg = 1u << (bits - 1);
	uint32_t v = c->d[r] & m, res;
	unsigned f = 0;
	if (!strcmp(kind, "asl")) {
		res = (uint32_t)(((uint64_t)v << n) & m);
		if ((v >> (bits - n)) & 1) f |= CC_C | CC_X;
		uint32_t top = (v >> (bits - 1 - n)) & ((1u << (n + 1)) - 1);
		if (top != 0 && top != ((1u << (n + 1)) - 1)) f |= CC_V;
	} else if (!strcmp(kind, "asr")) {
		int32_t sv = bits == 32 ? (int32_t)v : (bits == 16 ? (int32_t)(int16_t)v : (int32_t)(int8_t)v);
		res = (uint32_t)(sv >> n) & m;
		if ((v >> (n - 1)) & 1) f |= CC_C | CC_X;
	} else if (!strcmp(kind, "lsl")) {
		res = (uint32_t)(((uint64_t)v << n) & m);
		if ((v >> (bits - n)) & 1) f |= CC_C | CC_X;
	} else {
		res = v >> n;
		if ((v >> (n - 1)) & 1) f |= CC_C | CC_X;
	}
	if (res & sg) f |= CC_N;
	if (!res) f |= CC_Z;
	cc_set(c, CC_X | CC_N | CC_Z | CC_V | CC_C, f);
	if (bits == 32) c->d[r] = res;
	else if (bits == 16) set_w(&c->d[r], res);
	else set_b(&c->d[r], res);
}
static void u_007A92(hle_ctx *ctx);
static void u_007AD4(hle_ctx *ctx);
static void u_008A7C(hle_ctx *ctx);
void Nat009290(hle_ctx *ctx);
static void u_0092A8(hle_ctx *ctx);
static void u_009EF8(hle_ctx *ctx);
void Nat009F0C(hle_ctx *ctx);
void Nat00A24E(hle_ctx *ctx);
void Nat00A25A(hle_ctx *ctx);
void Nat00A266(hle_ctx *ctx);
void Nat00A270(hle_ctx *ctx);
void ScrLoadPalette(hle_ctx *ctx);
void Nat00A2D4(hle_ctx *ctx);
void Nat00A304(hle_ctx *ctx);
void Nat00A31A(hle_ctx *ctx);
void Nat00A32A(hle_ctx *ctx);
void Nat00A33A(hle_ctx *ctx);
void Nat00A3F4(hle_ctx *ctx);
void Nat00A41A(hle_ctx *ctx);
void Nat00A46E(hle_ctx *ctx);
void Nat00A4B2(hle_ctx *ctx);
void Nat00A4D2(hle_ctx *ctx);
void Nat00A4F4(hle_ctx *ctx);
void Nat00A510(hle_ctx *ctx);
void Nat00A712(hle_ctx *ctx);
void Nat00A82C(hle_ctx *ctx);
void Nat00A85E(hle_ctx *ctx);
void Nat00A880(hle_ctx *ctx);
void Nat00A8BC(hle_ctx *ctx);
void Nat00A914(hle_ctx *ctx);
void Nat00A94A(hle_ctx *ctx);
void Nat00AB4E(hle_ctx *ctx);
void Nat00AB62(hle_ctx *ctx);
void Nat00AB76(hle_ctx *ctx);
void Nat00ACB0(hle_ctx *ctx);
void Nat00AD48(hle_ctx *ctx);
void Nat00AD9C(hle_ctx *ctx);
void Nat00AEFA(hle_ctx *ctx);
void Nat00AF1C(hle_ctx *ctx);
void Nat00AFD4(hle_ctx *ctx);
void Nat00AFFC(hle_ctx *ctx);
void Nat00B02A(hle_ctx *ctx);
void Nat00B0B2(hle_ctx *ctx);
void Nat00B1CE(hle_ctx *ctx);
void Nat00B200(hle_ctx *ctx);
void ScrSetPlayers(hle_ctx *ctx);
void ScrDemoSkip(hle_ctx *ctx);
void Nat00D0F8(hle_ctx *ctx);
static void u_010642(hle_ctx *ctx);
static void u_0112A8(hle_ctx *ctx);
static void u_0112BE(hle_ctx *ctx);
static void u_0115FA(hle_ctx *ctx);
static void u_0118C0(hle_ctx *ctx);
static void u_0118CE(hle_ctx *ctx);
static void u_012234(hle_ctx *ctx);
static void u_0122CA(hle_ctx *ctx);
static void u_013D04(hle_ctx *ctx);
void Nat013E04(hle_ctx *ctx);
static void u_0FBB96(hle_ctx *ctx);
static void u_0FC27C(hle_ctx *ctx);
/* BcdAdd */
static void u_007A92(hle_ctx *ctx)
{
	{ { uint32_t base = ctx->a[7] - 36; st32(base + 0, ctx->d[0]); st32(base + 4, ctx->d[1]); st32(base + 8, ctx->d[2]); st32(base + 12, ctx->d[3]); st32(base + 16, ctx->d[4]); st32(base + 20, ctx->d[5]); st32(base + 24, ctx->d[6]); st32(base + 28, ctx->d[7]); st32(base + 32, ctx->a[1]); ctx->a[7] = base; } } hle_movem_l(ctx, 80, 9); /* 007A92 movem.l d0-d7/a1,-(sp) */
	{ ctx->d[7] = 0x0u; cc_logic32(ctx, 0x0u); } hle_insn(ctx, 4); /* 007A96 moveq #0x0,d7 */
	{ ctx->a[1] = 0xFFFFA0BAu; } hle_insn(ctx, 12); /* 007A98 lea.l (0xffffa0ba).l,a1 */
L_007A9E:
	{ ctx->d[0] = 0x1u; cc_logic32(ctx, 0x1u); } hle_insn(ctx, 4); /* 007A9E moveq #0x1,d0 */
	{ { uint32_t v = ctx->d[3]; ctx->d[5] = v; cc_logic32(ctx, v); } } hle_insn(ctx, 4); /* 007AA0 move.l d3,d5 */
	hle_insn(ctx, 10); goto L_007AA8; /* 007AA2 bra.s loc_007AA8 */
L_007AA4:
	hle_insn(ctx, 18); u_007AD4(ctx); /* 007AA4 jsr (sub_007AD4,pc) */
L_007AA8:
	{ uint16_t c = (uint16_t)(ctx->d[5] - 1); set_w(&ctx->d[5], c); if (c != 0xFFFF) { hle_insn(ctx, 10); goto L_007AA4; } hle_insn(ctx, 14); } /* 007AA8 dbra d5,loc_007AA4 */
	{ { set_b(&ctx->d[6], 0xFF); } } hle_insn(ctx, 6); /* 007AAC st.b d6 */
	{ ctx->d[1] = 0x0u; cc_logic32(ctx, 0x0u); } hle_insn(ctx, 4); /* 007AAE moveq #0x0,d1 */
L_007AB0:
	{ { uint32_t sv = 0x1u; uint32_t r = alu(ctx, OP_ADD, (ctx->d[6] & 0xFF), sv, 8, 1); set_b(&ctx->d[6], r); } } hle_insn(ctx, 4); /* 007AB0 addq.b #0x1,d6 */
	{ { uint32_t sv = ctx->d[0]; uint32_t r = alu(ctx, OP_ADD, ctx->d[1], sv, 32, 1); ctx->d[1] = r; } } hle_insn(ctx, 8); /* 007AB2 add.l d0,d1 */
	{ { uint32_t sv = ctx->d[1]; (void)alu(ctx, OP_SUB, ctx->d[4], sv, 32, 0); } } hle_insn(ctx, 6); /* 007AB4 cmp.l d1,d4 */
	if (c_ge(ctx)) { hle_insn(ctx, 10); goto L_007AB0; } hle_insn(ctx, 8); /* 007AB6 bge.s loc_007AB0 */
	{ { uint32_t sv = ctx->d[0]; uint32_t r = alu(ctx, OP_SUB, ctx->d[1], sv, 32, 1); ctx->d[1] = r; } } hle_insn(ctx, 8); /* 007AB8 sub.l d0,d1 */
	{ { uint32_t sv = ctx->d[1]; uint32_t r = alu(ctx, OP_SUB, ctx->d[4], sv, 32, 1); ctx->d[4] = r; } } hle_insn(ctx, 8); /* 007ABA sub.l d1,d4 */
	{ { uint32_t sv = 0x30u; uint32_t r = alu(ctx, OP_ADD, (ctx->d[6] & 0xFF), sv, 8, 1); set_b(&ctx->d[6], r); } } hle_insn(ctx, 8); /* 007ABC addi.b #0x30,d6 */
	{ { uint32_t v = (ctx->d[6] & 0xFF); uint32_t da = ctx->a[1] + (uint32_t)0 + (uint32_t)(int32_t)(int16_t)ctx->d[7]; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 14); /* 007AC0 move.b d6,(0x0,a1,d7.w) */
	{ { uint32_t sv = 0x1u; uint32_t r = alu(ctx, OP_ADD, (ctx->d[7] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[7], r); } } hle_insn(ctx, 4); /* 007AC4 addq.w #0x1,d7 */
	{ uint16_t c = (uint16_t)(ctx->d[3] - 1); set_w(&ctx->d[3], c); if (c != 0xFFFF) { hle_insn(ctx, 10); goto L_007A9E; } hle_insn(ctx, 14); } /* 007AC6 dbra d3,loc_007A9E */
	{ { uint32_t da = ctx->a[1] + (uint32_t)0 + (uint32_t)(int32_t)(int16_t)ctx->d[7]; st8(da, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } } hle_insn(ctx, 18); /* 007ACA clr.b (0x0,a1,d7.w) */
	{ { uint32_t base = ctx->a[7]; ctx->d[0] = ld32(base + 0); ctx->d[1] = ld32(base + 4); ctx->d[2] = ld32(base + 8); ctx->d[3] = ld32(base + 12); ctx->d[4] = ld32(base + 16); ctx->d[5] = ld32(base + 20); ctx->d[6] = ld32(base + 24); ctx->d[7] = ld32(base + 28); ctx->a[1] = ld32(base + 32); ctx->a[7] = base + 36; } } hle_movem_l(ctx, 84, 9); /* 007ACE movem.l (sp)+,d0-d7/a1 */
	hle_insn(ctx, 16); return; /* 007AD2 rts */
}
/* sub_007AD4 */
static void u_007AD4(hle_ctx *ctx)
{
	{ { uint32_t base = ctx->a[7] - 8; st32(base + 0, ctx->d[1]); st32(base + 4, ctx->d[2]); ctx->a[7] = base; } } hle_movem_l(ctx, 24, 2); /* 007AD4 movem.l d1-d2,-(sp) */
	{ ctx->d[1] = 0x0u; cc_logic32(ctx, 0x0u); } hle_insn(ctx, 4); /* 007AD8 moveq #0x0,d1 */
	{ ctx->d[2] = 0x9u; cc_logic32(ctx, 0x9u); } hle_insn(ctx, 4); /* 007ADA moveq #0x9,d2 */
L_007ADC:
	{ { uint32_t sv = ctx->d[0]; uint32_t r = alu(ctx, OP_ADD, ctx->d[1], sv, 32, 1); ctx->d[1] = r; } } hle_insn(ctx, 8); /* 007ADC add.l d0,d1 */
	{ uint16_t c = (uint16_t)(ctx->d[2] - 1); set_w(&ctx->d[2], c); if (c != 0xFFFF) { hle_insn(ctx, 10); goto L_007ADC; } hle_insn(ctx, 14); } /* 007ADE dbra d2,loc_007ADC */
	{ { uint32_t v = ctx->d[1]; ctx->d[0] = v; cc_logic32(ctx, v); } } hle_insn(ctx, 4); /* 007AE2 move.l d1,d0 */
	{ { uint32_t base = ctx->a[7]; ctx->d[1] = ld32(base + 0); ctx->d[2] = ld32(base + 4); ctx->a[7] = base + 8; } } hle_movem_l(ctx, 28, 2); /* 007AE4 movem.l (sp)+,d1-d2 */
	hle_insn(ctx, 16); return; /* 007AE8 rts */
}
/* Copy32 */
static void u_008A7C(hle_ctx *ctx)
{
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 4; uint32_t v = ld32(sa); uint32_t da = ctx->a[1]; ctx->a[1] += 4; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 20); /* 008A7C move.l (a0)+,(a1)+ */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 4; uint32_t v = ld32(sa); uint32_t da = ctx->a[1]; ctx->a[1] += 4; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 20); /* 008A7E move.l (a0)+,(a1)+ */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 4; uint32_t v = ld32(sa); uint32_t da = ctx->a[1]; ctx->a[1] += 4; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 20); /* 008A80 move.l (a0)+,(a1)+ */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 4; uint32_t v = ld32(sa); uint32_t da = ctx->a[1]; ctx->a[1] += 4; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 20); /* 008A82 move.l (a0)+,(a1)+ */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 4; uint32_t v = ld32(sa); uint32_t da = ctx->a[1]; ctx->a[1] += 4; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 20); /* 008A84 move.l (a0)+,(a1)+ */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 4; uint32_t v = ld32(sa); uint32_t da = ctx->a[1]; ctx->a[1] += 4; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 20); /* 008A86 move.l (a0)+,(a1)+ */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 4; uint32_t v = ld32(sa); uint32_t da = ctx->a[1]; ctx->a[1] += 4; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 20); /* 008A88 move.l (a0)+,(a1)+ */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 4; uint32_t v = ld32(sa); uint32_t da = ctx->a[1]; ctx->a[1] += 4; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 20); /* 008A8A move.l (a0)+,(a1)+ */
	hle_insn(ctx, 16); return; /* 008A8C rts */
}
/* sub_009290 */
HLE_FUNC(0x009290, Nat009290, 12000)
{
	{ { uint32_t v = ctx->a[0]; ctx->a[7] -= 4; uint32_t da = ctx->a[7]; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 12); /* 009290 move.l a0,-(sp) */
	{ ctx->a[0] = 0x000031B5u; } hle_insn(ctx, 12); /* 009292 lea.l (dat_0031B5).l,a0 */
	{ { uint32_t v = 0xC000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009298 move.w #0xc000,d6 */
	hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x007DEE); /* 00929C jsr (sub_007DEE,pc) */
	hle_insn(ctx, 18); u_0092A8(ctx); /* 0092A0 jsr (sub_0092A8,pc) */
	{ { uint32_t sa = ctx->a[7]; ctx->a[7] += 4; uint32_t v = ld32(sa); ctx->a[0] = v; } } hle_insn(ctx, 12); /* 0092A4 movea.l (sp)+,a0 */
	hle_insn(ctx, 16); return; /* 0092A6 rts */
}
/* sub_0092A8 */
static void u_0092A8(hle_ctx *ctx)
{
	{ { uint32_t base = ctx->a[7] - 8; st32(base + 0, ctx->d[7]); st32(base + 4, ctx->a[6]); ctx->a[7] = base; } } hle_movem_l(ctx, 24, 2); /* 0092A8 movem.l d7/a6,-(sp) */
	{ ctx->d[4] = 0x0u; cc_logic32(ctx, 0x0u); } hle_insn(ctx, 4); /* 0092AC moveq #0x0,d4 */
	{ { uint32_t sa = 0xFFFFA0A8u; uint32_t v = ld16(sa); set_w(&ctx->d[4], v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 0092AE move.w (-0x5f58).w,d4 */
	{ ctx->d[3] = 0x2u; cc_logic32(ctx, 0x2u); } hle_insn(ctx, 4); /* 0092B2 moveq #0x2,d3 */
	hle_insn(ctx, 18); u_007A92(ctx); /* 0092B4 jsr (BcdAdd,pc) */
	{ { uint32_t v = 0x1Cu; uint32_t da = 0xFFFFA0B8u; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 20); /* 0092B8 move.b #0x1c,(0xffffa0b8).l */
	{ { uint32_t v = 0x2u; uint32_t da = 0xFFFFA0B9u; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 20); /* 0092C0 move.b #0x2,(0xffffa0b9).l */
	{ ctx->a[0] = 0xFFFFA0B8u; } hle_insn(ctx, 12); /* 0092C8 lea.l (0xffffa0b8).l,a0 */
	{ { uint32_t v = 0xC000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 0092CE move.w #0xc000,d6 */
	hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x007DEE); /* 0092D2 jsr (sub_007DEE,pc) */
	{ { uint32_t base = ctx->a[7]; ctx->d[7] = ld32(base + 0); ctx->a[6] = ld32(base + 4); ctx->a[7] = base + 8; } } hle_movem_l(ctx, 28, 2); /* 0092D6 movem.l (sp)+,d7/a6 */
	hle_insn(ctx, 16); return; /* 0092DA rts */
}
/* sub_009EF8 */
static void u_009EF8(hle_ctx *ctx)
{
	{ ctx->a[4] = 0xFFFFAA5Cu; } hle_insn(ctx, 12); /* 009EF8 lea.l (0xffffaa5c).l,a4 */
	{ ctx->d[1] = 0x1Fu; cc_logic32(ctx, 0x1Fu); } hle_insn(ctx, 4); /* 009EFE moveq #0x1f,d1 */
L_009F00:
	{ { uint32_t v = (ctx->d[2] & 0xFFFF); uint32_t da = ctx->a[4]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009F00 move.w d2,(a4) */
	{ { uint32_t sa = ctx->a[4] + (uint32_t)32; ctx->a[4] = sa; } } hle_insn(ctx, 8); /* 009F02 lea.l (0x20,a4),a4 */
	{ uint16_t c = (uint16_t)(ctx->d[1] - 1); set_w(&ctx->d[1], c); if (c != 0xFFFF) { hle_insn(ctx, 10); goto L_009F00; } hle_insn(ctx, 14); } /* 009F06 dbra d1,loc_009F00 */
	hle_insn(ctx, 16); return; /* 009F0A rts */
}
/* sub_009F0C */
HLE_FUNC_PURE(0x009F0C, Nat009F0C, 4000)
{
	{ ctx->a[4] = 0xFFFFAC1Cu; } hle_insn(ctx, 12); /* 009F0C lea.l (0xffffac1c).l,a4 */
	{ { uint32_t v = 0xFFE0u; set_w(&ctx->d[2], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009F12 move.w #0xffe0,d2 */
	{ ctx->d[1] = 0xAu; cc_logic32(ctx, 0xAu); } hle_insn(ctx, 4); /* 009F16 moveq #0xa,d1 */
L_009F18:
	{ { uint32_t v = (ctx->d[2] & 0xFFFF); uint32_t da = ctx->a[4]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009F18 move.w d2,(a4) */
	{ { uint32_t sa = ctx->a[4] + (uint32_t)32; ctx->a[4] = sa; } } hle_insn(ctx, 8); /* 009F1A lea.l (0x20,a4),a4 */
	{ uint16_t c = (uint16_t)(ctx->d[1] - 1); set_w(&ctx->d[1], c); if (c != 0xFFFF) { hle_insn(ctx, 10); goto L_009F18; } hle_insn(ctx, 14); } /* 009F1E dbra d1,loc_009F18 */
	hle_insn(ctx, 16); return; /* 009F22 rts */
}
/* sub_00A24E */
HLE_FUNC_PURE(0x00A24E, Nat00A24E, 4000)
{
	{ { uint32_t v = 0x81u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00A24E move.w #0x81,d0 */
	hle_insn(ctx, 20); u_013D04(ctx); /* 00A252 jsr (sub_013D04).l */
	hle_insn(ctx, 16); return; /* 00A258 rts */
}
/* sub_00A25A */
HLE_FUNC_PURE(0x00A25A, Nat00A25A, 4000)
{
	{ { uint32_t v = 0x82u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00A25A move.w #0x82,d0 */
	hle_insn(ctx, 20); u_013D04(ctx); /* 00A25E jsr (sub_013D04).l */
	hle_insn(ctx, 16); return; /* 00A264 rts */
}
/* sub_00A266 */
HLE_FUNC_PURE(0x00A266, Nat00A266, 4000)
{
	goto L_00A266;
L_009EBE:
	{ { uint32_t sa = 0xFFFF80BCu; uint32_t v = ld32(sa); ctx->a[5] = v; } } hle_insn(ctx, 20); /* 009EBE movea.l (0xffff80bc).l,a5 */
L_009EC4:
	{ { uint32_t v = 0x2u; uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 009EC4 move.w #0x2,(a5)+ */
	{ { uint32_t v = (ctx->d[2] & 0xFFFF); uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009EC8 move.w d2,(a5)+ */
	{ { uint32_t v = (ctx->d[1] & 0xFFFF); uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009ECA move.w d1,(a5)+ */
	{ { uint32_t sv = 0x80u; uint32_t r = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[1], r); } } hle_insn(ctx, 8); /* 009ECC addi.w #0x80,d1 */
	{ uint16_t c = (uint16_t)(ctx->d[0] - 1); set_w(&ctx->d[0], c); if (c != 0xFFFF) { hle_insn(ctx, 10); goto L_009EC4; } hle_insn(ctx, 14); } /* 009ED0 dbra d0,loc_009EC4 */
	{ { uint32_t v = ctx->a[5]; uint32_t da = 0xFFFF80BCu; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 20); /* 009ED4 move.l a5,(0xffff80bc).l */
	{ { uint32_t sa = ctx->a[7]; ctx->a[7] += 4; uint32_t v = ld32(sa); ctx->a[0] = v; } } hle_insn(ctx, 12); /* 009EDA movea.l (sp)+,a0 */
	hle_insn(ctx, 16); return; /* 009EDC rts */
L_00A266:
	{ { uint32_t v = ctx->a[0]; ctx->a[7] -= 4; uint32_t da = ctx->a[7]; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 12); /* 00A266 move.l a0,-(sp) */
	{ { uint32_t v = 0xE500u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00A268 move.w #0xe500,d1 */
	{ ctx->d[2] = 0x1Eu; cc_logic32(ctx, 0x1Eu); } hle_insn(ctx, 4); /* 00A26C moveq #0x1e,d2 */
	hle_insn(ctx, 10); goto L_00A278; /* 00A26E bra.s sub_00A278 */
L_00A278:
	{ { uint32_t v = 0xAu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00A278 move.w #0xa,d0 */
	hle_insn(ctx, 10); goto L_009EBE; /* 00A27C bra.w sub_009EBE */
}
/* sub_00A270 */
HLE_FUNC_PURE(0x00A270, Nat00A270, 4000)
{
	goto L_00A270;
L_009EBE:
	{ { uint32_t sa = 0xFFFF80BCu; uint32_t v = ld32(sa); ctx->a[5] = v; } } hle_insn(ctx, 20); /* 009EBE movea.l (0xffff80bc).l,a5 */
L_009EC4:
	{ { uint32_t v = 0x2u; uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 009EC4 move.w #0x2,(a5)+ */
	{ { uint32_t v = (ctx->d[2] & 0xFFFF); uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009EC8 move.w d2,(a5)+ */
	{ { uint32_t v = (ctx->d[1] & 0xFFFF); uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009ECA move.w d1,(a5)+ */
	{ { uint32_t sv = 0x80u; uint32_t r = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[1], r); } } hle_insn(ctx, 8); /* 009ECC addi.w #0x80,d1 */
	{ uint16_t c = (uint16_t)(ctx->d[0] - 1); set_w(&ctx->d[0], c); if (c != 0xFFFF) { hle_insn(ctx, 10); goto L_009EC4; } hle_insn(ctx, 14); } /* 009ED0 dbra d0,loc_009EC4 */
	{ { uint32_t v = ctx->a[5]; uint32_t da = 0xFFFF80BCu; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 20); /* 009ED4 move.l a5,(0xffff80bc).l */
	{ { uint32_t sa = ctx->a[7]; ctx->a[7] += 4; uint32_t v = ld32(sa); ctx->a[0] = v; } } hle_insn(ctx, 12); /* 009EDA movea.l (sp)+,a0 */
	hle_insn(ctx, 16); return; /* 009EDC rts */
L_00A270:
	{ { uint32_t v = ctx->a[0]; ctx->a[7] -= 4; uint32_t da = ctx->a[7]; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 12); /* 00A270 move.l a0,-(sp) */
	{ { uint32_t v = 0xE51Eu; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00A272 move.w #0xe51e,d1 */
	{ ctx->d[2] = 0x20u; cc_logic32(ctx, 0x20u); } hle_insn(ctx, 4); /* 00A276 moveq #0x20,d2 */
	{ { uint32_t v = 0xAu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00A278 move.w #0xa,d0 */
	hle_insn(ctx, 10); goto L_009EBE; /* 00A27C bra.w sub_009EBE */
}
/* ScrLoadPalette */
HLE_FUNC(0x00A2A8, ScrLoadPalette, 5400)
{
	{ { uint32_t v = ctx->a[0]; ctx->a[7] -= 4; uint32_t da = ctx->a[7]; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 12); /* 00A2A8 move.l a0,-(sp) */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00A2AA move.w (a0)+,d0 */
	{ sh_n(ctx, 0, "asl", 5, 16); hle_insn(ctx, 16); } /* 00A2AC asl.w #0x5,d0 */
	{ ctx->a[1] = 0xFFFFA1B4u; } hle_insn(ctx, 12); /* 00A2AE lea.l (0xffffa1b4).l,a1 */
	{ { uint32_t sa = ctx->a[1] + (uint32_t)0 + (uint32_t)(int32_t)(int16_t)ctx->d[0]; ctx->a[1] = sa; } } hle_insn(ctx, 12); /* 00A2B4 lea.l (0x0,a1,d0.w),a1 */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 4; uint32_t v = ld32(sa); ctx->a[0] = v; } } hle_insn(ctx, 12); /* 00A2B8 movea.l (a0)+,a0 */
	hle_insn(ctx, 20); u_008A7C(ctx); /* 00A2BA jsr (Copy32).l */
	{ ctx->a[0] = 0xFFFFA1B4u; } hle_insn(ctx, 12); /* 00A2C0 lea.l (0xffffa1b4).l,a0 */
	hle_insn(ctx, 20); hle_sync(ctx); hle_call68k(ctx, 0x0080FE); /* 00A2C6 jsr (CramUpload).l */
	{ { uint32_t sa = ctx->a[7]; ctx->a[7] += 4; uint32_t v = ld32(sa); ctx->a[0] = v; } } hle_insn(ctx, 12); /* 00A2CC movea.l (sp)+,a0 */
	{ { uint32_t sa = ctx->a[0] + (uint32_t)6; ctx->a[0] = sa; } } hle_insn(ctx, 8); /* 00A2CE lea.l (0x6,a0),a0 */
	hle_insn(ctx, 16); return; /* 00A2D2 rts */
}
/* sub_00A2D4 */
HLE_FUNC_PURE(0x00A2D4, Nat00A2D4, 4000)
{
	{ { uint32_t sa = 0xFFFF9ECAu; uint32_t v = ld32(sa); ctx->a[1] = v; } } hle_insn(ctx, 20); /* 00A2D4 movea.l (0xffff9eca).l,a1 */
	{ { uint32_t v = 0xD112u; uint32_t da = ctx->a[1] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 00A2DA move.l #0xd112,(0x20,a1) */
	{ { uint32_t v = 0x0u; uint32_t da = ctx->a[6] + (uint32_t)6; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00A2E2 move.b #0x0,(0x6,a6) */
	{ { uint32_t v = 0x1Eu; uint32_t da = ctx->a[6] + (uint32_t)12; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00A2E8 move.b #0x1e,(0xc,a6) */
	{ { uint32_t v = 0xFFu; uint32_t da = ctx->a[6] + (uint32_t)15; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00A2EE move.b #0xff,(0xf,a6) */
	{ { uint32_t da = ctx->a[6] + (uint32_t)13; st8(da, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } } hle_insn(ctx, 16); /* 00A2F4 clr.b (0xd,a6) */
	{ { uint32_t v = 0x2u; uint32_t da = ctx->a[6] + (uint32_t)18; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00A2F8 move.b #0x2,(0x12,a6) */
	{ { uint32_t sv = 0x1u; uint32_t da = ctx->a[6] + (uint32_t)4; uint32_t r = alu(ctx, OP_SUB, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 16); /* 00A2FE subq.w #0x1,(0x4,a6) */
	hle_insn(ctx, 16); return; /* 00A302 rts */
}
/* sub_00A304 */
HLE_FUNC_PURE(0x00A304, Nat00A304, 4000)
{
	{ { uint32_t sa = 0xFFFF9ECAu; uint32_t v = ld32(sa); ctx->a[1] = v; } } hle_insn(ctx, 20); /* 00A304 movea.l (0xffff9eca).l,a1 */
	{ { uint32_t v = 0xD27Au; uint32_t da = ctx->a[1] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 00A30A move.l #0xd27a,(0x20,a1) */
	{ { uint32_t v = 0x0u; uint32_t da = ctx->a[6] + (uint32_t)6; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00A312 move.b #0x0,(0x6,a6) */
	hle_insn(ctx, 16); return; /* 00A318 rts */
}
/* sub_00A31A */
HLE_FUNC_PURE(0x00A31A, Nat00A31A, 4000)
{
	{ { uint32_t sa = 0xFFFF9ECAu; uint32_t v = ld32(sa); ctx->a[1] = v; } } hle_insn(ctx, 20); /* 00A31A movea.l (0xffff9eca).l,a1 */
	{ { uint32_t v = 0xD2ACu; uint32_t da = ctx->a[1] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 00A320 move.l #0xd2ac,(0x20,a1) */
	hle_insn(ctx, 16); return; /* 00A328 rts */
}
/* sub_00A32A */
HLE_FUNC_PURE(0x00A32A, Nat00A32A, 4000)
{
	{ { uint32_t sa = 0xFFFF9ECAu; uint32_t v = ld32(sa); ctx->a[1] = v; } } hle_insn(ctx, 20); /* 00A32A movea.l (0xffff9eca).l,a1 */
	{ { uint32_t da = ctx->a[1]; st16(da, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } } hle_insn(ctx, 12); /* 00A330 clr.w (a1) */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF80DAu; uint32_t r = alu(ctx, OP_SUB, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 00A332 subq.w #0x1,(0xffff80da).l */
	hle_insn(ctx, 16); return; /* 00A338 rts */
}
/* sub_00A33A */
HLE_FUNC_PURE(0x00A33A, Nat00A33A, 4000)
{
	goto L_00A33A;
L_009B14:
	{ sh_n(ctx, 1, "asl", 5, 16); hle_insn(ctx, 16); } /* 009B14 asl.w #0x5,d1 */
	{ sh_n(ctx, 2, "asr", 1, 16); hle_insn(ctx, 8); } /* 009B16 asr.w #0x1,d2 */
	{ { uint32_t sa = 0xFFFF80BCu; uint32_t v = ld32(sa); ctx->a[5] = v; } } hle_insn(ctx, 20); /* 009B18 movea.l (0xffff80bc).l,a5 */
	{ { uint32_t v = 0x1u; uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 009B1E move.w #0x1,(a5)+ */
	{ { uint32_t v = (ctx->d[2] & 0xFFFF); uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009B22 move.w d2,(a5)+ */
	{ { uint32_t v = ctx->a[1]; uint32_t da = ctx->a[5]; ctx->a[5] += 4; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 12); /* 009B24 move.l a1,(a5)+ */
	{ { uint32_t v = (ctx->d[1] & 0xFFFF); uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009B26 move.w d1,(a5)+ */
	{ { uint32_t v = ctx->a[5]; uint32_t da = 0xFFFF80BCu; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 20); /* 009B28 move.l a5,(0xffff80bc).l */
	hle_insn(ctx, 16); return; /* 009B2E rts */
L_00A33A:
	{ { uint32_t sa = 0xFFFF9E1Au; uint32_t v = ld16(sa); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A33A move.w (0xffff9e1a).l,d1 */
	{ { uint32_t sv = 0xF8u; uint32_t r = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[1], r); } } hle_insn(ctx, 8); /* 00A340 addi.w #0xf8,d1 */
	{ ctx->a[1] = 0x00055854u; } hle_insn(ctx, 12); /* 00A344 lea.l (dat_055854).l,a1 */
	{ { uint32_t sa = ctx->a[1]; ctx->a[1] += 2; uint32_t v = ld16(sa); set_w(&ctx->d[2], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00A34A move.w (a1)+,d2 */
	hle_insn(ctx, 10); goto L_009B14; /* 00A34C bra.w sub_009B14 */
}
/* sub_00A3F4 */
HLE_FUNC_PURE(0x00A3F4, Nat00A3F4, 4000)
{
	hle_insn(ctx, 18); Nat00A41A(ctx); /* 00A3F4 jsr (sub_00A41A,pc) */
	{ { uint32_t v = 0xD8DCu; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 00A3F8 move.l #0xd8dc,(0x20,a6) */
	{ { uint32_t v = 0x8Cu; uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A400 move.w #0x8c,(0x2,a6) */
	{ { uint32_t v = 0x40u; uint32_t da = ctx->a[6] + (uint32_t)10; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00A406 move.b #0x40,(0xa,a6) */
	{ { uint32_t v = 0x140u; uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A40C move.w #0x140,(0x24,a6) */
	{ { uint32_t da = 0xFFFF9EA6u; st16(da, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } } hle_insn(ctx, 20); /* 00A412 clr.w (0xffff9ea6).l */
	hle_insn(ctx, 16); return; /* 00A418 rts */
}
/* sub_00A41A */
HLE_FUNC_PURE(0x00A41A, Nat00A41A, 4000)
{
	hle_insn(ctx, 18); ObjAlloc(ctx); /* 00A41A jsr (ObjAlloc,pc) */
	{ { uint32_t v = 0x1Eu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00A41E move.w #0x1e,d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x4000u) ? 0 : CC_Z); ctx->d[0] |= 0x4000u; } hle_insn(ctx, 10); /* 00A422 bset.b #0xe,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00A426 move.w d0,(a6) */
	{ { uint32_t v = 0xA0u; uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A428 move.w #0xa0,(0x2,a6) */
	{ { uint32_t v = 0xB8u; uint32_t da = ctx->a[6] + (uint32_t)4; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A42E move.w #0xb8,(0x4,a6) */
	{ { uint32_t v = 0x17u; uint32_t da = ctx->a[6] + (uint32_t)6; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00A434 move.b #0x17,(0x6,a6) */
	{ { uint32_t v = 0x8Cu; uint32_t da = ctx->a[6] + (uint32_t)7; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00A43A move.b #0x8c,(0x7,a6) */
	{ { uint32_t v = 0x270u; uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A440 move.w #0x270,(0x24,a6) */
	{ { uint32_t v = 0x597Au; uint32_t da = ctx->a[6] + (uint32_t)28; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 00A446 move.l #0x597a,(0x1c,a6) */
	{ { uint32_t v = 0xD712u; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 00A44E move.l #0xd712,(0x20,a6) */
	{ { uint32_t v = 0x40u; uint32_t da = ctx->a[6] + (uint32_t)10; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00A456 move.b #0x40,(0xa,a6) */
	{ { uint32_t v = 0x1u; uint32_t da = ctx->a[6] + (uint32_t)14; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00A45C move.b #0x1,(0xe,a6) */
	{ { uint32_t da = ctx->a[6] + (uint32_t)11; st8(da, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } } hle_insn(ctx, 16); /* 00A462 clr.b (0xb,a6) */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF80DAu; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 00A466 addq.w #0x1,(0xffff80da).l */
	hle_insn(ctx, 16); return; /* 00A46C rts */
}
/* sub_00A46E */
HLE_FUNC_PURE(0x00A46E, Nat00A46E, 4000)
{
	hle_insn(ctx, 18); ObjAlloc(ctx); /* 00A46E jsr (ObjAlloc,pc) */
	{ { uint32_t v = 0x1Eu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00A472 move.w #0x1e,d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x4000u) ? 0 : CC_Z); ctx->d[0] |= 0x4000u; } hle_insn(ctx, 10); /* 00A476 bset.b #0xe,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00A47A move.w d0,(a6) */
	{ { uint32_t v = 0xA0u; uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A47C move.w #0xa0,(0x2,a6) */
	{ { uint32_t v = 0xB8u; uint32_t da = ctx->a[6] + (uint32_t)4; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A482 move.w #0xb8,(0x4,a6) */
	{ { uint32_t v = 0x17u; uint32_t da = ctx->a[6] + (uint32_t)6; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00A488 move.b #0x17,(0x6,a6) */
	{ { uint32_t v = 0xEu; uint32_t da = ctx->a[6] + (uint32_t)7; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00A48E move.b #0xe,(0x7,a6) */
	{ { uint32_t v = 0x2BCu; uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A494 move.w #0x2bc,(0x24,a6) */
	{ { uint32_t v = 0x597Au; uint32_t da = ctx->a[6] + (uint32_t)28; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 00A49A move.l #0x597a,(0x1c,a6) */
	{ { uint32_t v = 0xD90Eu; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 00A4A2 move.l #0xd90e,(0x20,a6) */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF80DAu; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 00A4AA addq.w #0x1,(0xffff80da).l */
	hle_insn(ctx, 16); return; /* 00A4B0 rts */
}
/* sub_00A4B2 */
HLE_FUNC(0x00A4B2, Nat00A4B2, 1750)
{
	{ ctx->d[0] = 0x0u; cc_logic32(ctx, 0x0u); } hle_insn(ctx, 4); /* 00A4B2 moveq #0x0,d0 */
	{ ctx->d[1] = 0x0u; cc_logic32(ctx, 0x0u); } hle_insn(ctx, 4); /* 00A4B4 moveq #0x0,d1 */
	hle_insn(ctx, 20); hle_sync(ctx); hle_call68k(ctx, 0x0081B6); /* 00A4B6 jsr (sub_0081B6).l */
	{ ctx->a[1] = 0xFFFFAA5Cu; } hle_insn(ctx, 12); /* 00A4BC lea.l (0xffffaa5c).l,a1 */
	{ { uint32_t v = 0x1Fu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00A4C2 move.w #0x1f,d0 */
L_00A4C6:
	{ { uint32_t da = ctx->a[1]; st16(da, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } } hle_insn(ctx, 12); /* 00A4C6 clr.w (a1) */
	{ { uint32_t sa = ctx->a[1] + (uint32_t)32; ctx->a[1] = sa; } } hle_insn(ctx, 8); /* 00A4C8 lea.l (0x20,a1),a1 */
	{ uint16_t c = (uint16_t)(ctx->d[0] - 1); set_w(&ctx->d[0], c); if (c != 0xFFFF) { hle_insn(ctx, 10); goto L_00A4C6; } hle_insn(ctx, 14); } /* 00A4CC dbra d0,loc_00A4C6 */
	hle_insn(ctx, 16); return; /* 00A4D0 rts */
}
/* sub_00A4D2 */
HLE_FUNC_PURE(0x00A4D2, Nat00A4D2, 4000)
{
	{ ctx->a[1] = 0x00A4E2u; } hle_insn(ctx, 8); /* 00A4D2 lea.l (dat_00A4E2,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00A4D6 jsr (ObjSpawnTemplate,pc) */
	{ { uint32_t v = 0x3Cu; uint32_t da = ctx->a[6] + (uint32_t)20; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A4DA move.w #0x3c,(0x14,a6) */
	hle_insn(ctx, 16); return; /* 00A4E0 rts */
}
/* sub_00A4F4 */
HLE_FUNC_PURE(0x00A4F4, Nat00A4F4, 4000)
{
	{ ctx->a[1] = 0x00A4FEu; } hle_insn(ctx, 8); /* 00A4F4 lea.l (dat_00A4FE,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00A4F8 jsr (ObjSpawnTemplate,pc) */
	hle_insn(ctx, 16); return; /* 00A4FC rts */
}
/* sub_00A510 */
HLE_FUNC_PURE(0x00A510, Nat00A510, 4000)
{
	{ ctx->a[1] = 0x00A51Au; } hle_insn(ctx, 8); /* 00A510 lea.l (dat_00A51A,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00A514 jsr (ObjSpawnTemplate,pc) */
	hle_insn(ctx, 16); return; /* 00A518 rts */
}
/* sub_00A712 */
HLE_FUNC_PURE(0x00A712, Nat00A712, 4000)
{
	{ ctx->a[1] = 0x00A752u; } hle_insn(ctx, 8); /* 00A712 lea.l (dat_00A752,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00A716 jsr (ObjSpawnTemplate,pc) */
	{ { uint32_t v = 0xC8u; uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A71A move.w #0xc8,(0x24,a6) */
	{ { uint32_t da = ctx->a[6] + (uint32_t)13; st8(da, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } } hle_insn(ctx, 16); /* 00A720 clr.b (0xd,a6) */
	{ { uint32_t v = 0x1u; uint32_t da = ctx->a[6] + (uint32_t)18; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00A724 move.b #0x1,(0x12,a6) */
	{ { uint32_t v = 0x5Fu; uint32_t da = ctx->a[6] + (uint32_t)12; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00A72A move.b #0x5f,(0xc,a6) */
	{ { uint32_t v = 0xFFu; uint32_t da = ctx->a[6] + (uint32_t)15; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00A730 move.b #0xff,(0xf,a6) */
	{ { uint32_t v = 0x20u; uint32_t da = ctx->a[6] + (uint32_t)10; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00A736 move.b #0x20,(0xa,a6) */
	{ { uint32_t da = ctx->a[6] + (uint32_t)11; st8(da, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } } hle_insn(ctx, 16); /* 00A73C clr.b (0xb,a6) */
	{ { uint32_t v = 0xFFu; uint32_t da = ctx->a[6] + (uint32_t)14; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00A740 move.b #0xff,(0xe,a6) */
	{ { uint32_t da = ctx->a[6]; uint8_t v = ld8(da); cc_set(ctx, CC_Z, (v & 0x40) ? 0 : CC_Z); st8(da, v & 0xBF); } } hle_insn(ctx, 16); /* 00A746 bclr.b #0x6,(a6) */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF9E08u; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 00A74A addq.w #0x1,(0xffff9e08).l */
	hle_insn(ctx, 16); return; /* 00A750 rts */
}
/* sub_00A82C */
HLE_FUNC_PURE(0x00A82C, Nat00A82C, 4000)
{
	{ ctx->a[1] = 0x00A84Cu; } hle_insn(ctx, 8); /* 00A82C lea.l (dat_00A84C,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00A830 jsr (ObjSpawnTemplate,pc) */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00A834 move.w (a0)+,d0 */
	if (!c_n(ctx)) { hle_insn(ctx, 10); goto L_00A840; } hle_insn(ctx, 8); /* 00A836 bpl.s loc_00A840 */
	{ { uint32_t r = alu(ctx, OP_SUB, 0, (ctx->d[0] & 0xFFFF), 16, 1); set_w(&ctx->d[0], r); } } hle_insn(ctx, 4); /* 00A838 neg.w d0 */
	{ { uint32_t v = 0x48u; uint32_t da = ctx->a[6] + (uint32_t)4; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A83A move.w #0x48,(0x4,a6) */
L_00A840:
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 00A840 move.w d0,(0x2,a6) */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF9E0Cu; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 00A844 addq.w #0x1,(0xffff9e0c).l */
	hle_insn(ctx, 16); return; /* 00A84A rts */
}
/* sub_00A85E */
HLE_FUNC_PURE(0x00A85E, Nat00A85E, 4000)
{
	{ ctx->a[1] = 0x00A86Eu; } hle_insn(ctx, 8); /* 00A85E lea.l (dat_00A86E,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00A862 jsr (ObjSpawnTemplate,pc) */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF9E0Eu; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 00A866 addq.w #0x1,(0xffff9e0e).l */
	hle_insn(ctx, 16); return; /* 00A86C rts */
}
/* sub_00A880 */
HLE_FUNC_PURE(0x00A880, Nat00A880, 4000)
{
	{ ctx->a[1] = 0x00A898u; } hle_insn(ctx, 8); /* 00A880 lea.l (dat_00A898,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00A884 jsr (ObjSpawnTemplate,pc) */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF9E0Cu; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 00A888 addq.w #0x1,(0xffff9e0c).l */
	{ ctx->a[1] = 0x00A8AAu; } hle_insn(ctx, 8); /* 00A88E lea.l (dat_00A8AA,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00A892 jsr (ObjSpawnTemplate,pc) */
	hle_insn(ctx, 16); return; /* 00A896 rts */
}
/* sub_00A8BC */
HLE_FUNC_PURE(0x00A8BC, Nat00A8BC, 4000)
{
	{ ctx->a[1] = 0x00A8AAu; } hle_insn(ctx, 8); /* 00A8BC lea.l (dat_00A8AA,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00A8C0 jsr (ObjSpawnTemplate,pc) */
	{ { uint32_t v = 0x154u; uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A8C4 move.w #0x154,(0x2,a6) */
	hle_insn(ctx, 16); return; /* 00A8CA rts */
}
/* sub_00A914 */
HLE_FUNC_PURE(0x00A914, Nat00A914, 4000)
{
	{ ctx->a[1] = 0x00A938u; } hle_insn(ctx, 8); /* 00A914 lea.l (dat_00A938,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00A918 jsr (ObjSpawnTemplate,pc) */
	{ { uint32_t v = 0x87u; uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A91C move.w #0x87,(0x24,a6) */
	{ { uint32_t da = ctx->a[6]; uint8_t v = ld8(da); cc_set(ctx, CC_Z, (v & 0x40) ? 0 : CC_Z); st8(da, v | 0x40); } } hle_insn(ctx, 16); /* 00A922 bset.b #0x6,(a6) */
	{ { uint32_t v = 0x20u; uint32_t da = ctx->a[6] + (uint32_t)10; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00A926 move.b #0x20,(0xa,a6) */
	{ { uint32_t v = 0x1u; uint32_t da = ctx->a[6] + (uint32_t)14; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00A92C move.b #0x1,(0xe,a6) */
	{ { uint32_t da = ctx->a[6] + (uint32_t)11; st8(da, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } } hle_insn(ctx, 16); /* 00A932 clr.b (0xb,a6) */
	hle_insn(ctx, 16); return; /* 00A936 rts */
}
/* sub_00A94A */
HLE_FUNC_PURE(0x00A94A, Nat00A94A, 4000)
{
	{ ctx->a[1] = 0x00AA08u; } hle_insn(ctx, 8); /* 00A94A lea.l (dat_00AA08,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00A94E jsr (ObjSpawnTemplate,pc) */
	{ { uint32_t v = 0x0u; uint32_t da = ctx->a[6] + (uint32_t)10; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A952 move.w #0x0,(0xa,a6) */
	{ ctx->a[1] = 0x00AA1Au; } hle_insn(ctx, 8); /* 00A958 lea.l (dat_00AA1A,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00A95C jsr (ObjSpawnTemplate,pc) */
	{ { uint32_t v = 0x4u; uint32_t da = ctx->a[6] + (uint32_t)10; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A960 move.w #0x4,(0xa,a6) */
	{ ctx->a[1] = 0x00AA2Cu; } hle_insn(ctx, 8); /* 00A966 lea.l (dat_00AA2C,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00A96A jsr (ObjSpawnTemplate,pc) */
	{ { uint32_t v = 0x8u; uint32_t da = ctx->a[6] + (uint32_t)10; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A96E move.w #0x8,(0xa,a6) */
	{ ctx->a[1] = 0x00AA08u; } hle_insn(ctx, 8); /* 00A974 lea.l (dat_00AA08,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00A978 jsr (ObjSpawnTemplate,pc) */
	{ { uint32_t v = 0x1u; uint32_t da = ctx->a[6] + (uint32_t)10; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A97C move.w #0x1,(0xa,a6) */
	{ { uint32_t sv = 0x18u; uint32_t da = ctx->a[6] + (uint32_t)4; uint32_t r = alu(ctx, OP_SUB, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 00A982 subi.w #0x18,(0x4,a6) */
	{ ctx->a[1] = 0x00AA1Au; } hle_insn(ctx, 8); /* 00A988 lea.l (dat_00AA1A,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00A98C jsr (ObjSpawnTemplate,pc) */
	{ { uint32_t v = 0x5u; uint32_t da = ctx->a[6] + (uint32_t)10; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A990 move.w #0x5,(0xa,a6) */
	{ { uint32_t sv = 0x18u; uint32_t da = ctx->a[6] + (uint32_t)4; uint32_t r = alu(ctx, OP_SUB, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 00A996 subi.w #0x18,(0x4,a6) */
	{ ctx->a[1] = 0x00AA2Cu; } hle_insn(ctx, 8); /* 00A99C lea.l (dat_00AA2C,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00A9A0 jsr (ObjSpawnTemplate,pc) */
	{ { uint32_t v = 0x9u; uint32_t da = ctx->a[6] + (uint32_t)10; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A9A4 move.w #0x9,(0xa,a6) */
	{ { uint32_t sv = 0x18u; uint32_t da = ctx->a[6] + (uint32_t)4; uint32_t r = alu(ctx, OP_SUB, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 00A9AA subi.w #0x18,(0x4,a6) */
	{ ctx->a[1] = 0x00AA08u; } hle_insn(ctx, 8); /* 00A9B0 lea.l (dat_00AA08,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00A9B4 jsr (ObjSpawnTemplate,pc) */
	{ { uint32_t v = 0x2u; uint32_t da = ctx->a[6] + (uint32_t)10; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A9B8 move.w #0x2,(0xa,a6) */
	{ { uint32_t sv = 0x30u; uint32_t da = ctx->a[6] + (uint32_t)4; uint32_t r = alu(ctx, OP_SUB, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 00A9BE subi.w #0x30,(0x4,a6) */
	{ ctx->a[1] = 0x00AA1Au; } hle_insn(ctx, 8); /* 00A9C4 lea.l (dat_00AA1A,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00A9C8 jsr (ObjSpawnTemplate,pc) */
	{ { uint32_t v = 0x6u; uint32_t da = ctx->a[6] + (uint32_t)10; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A9CC move.w #0x6,(0xa,a6) */
	{ { uint32_t sv = 0x30u; uint32_t da = ctx->a[6] + (uint32_t)4; uint32_t r = alu(ctx, OP_SUB, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 00A9D2 subi.w #0x30,(0x4,a6) */
	{ ctx->a[1] = 0x00AA2Cu; } hle_insn(ctx, 8); /* 00A9D8 lea.l (dat_00AA2C,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00A9DC jsr (ObjSpawnTemplate,pc) */
	{ { uint32_t v = 0xAu; uint32_t da = ctx->a[6] + (uint32_t)10; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A9E0 move.w #0xa,(0xa,a6) */
	{ { uint32_t sv = 0x30u; uint32_t da = ctx->a[6] + (uint32_t)4; uint32_t r = alu(ctx, OP_SUB, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 00A9E6 subi.w #0x30,(0x4,a6) */
	{ ctx->a[1] = 0xFFFFAE84u; } hle_insn(ctx, 12); /* 00A9EC lea.l (0xffffae84).l,a1 */
	{ { uint32_t v = 0xFFFFFF00u; ctx->d[0] = v; cc_logic32(ctx, v); } } hle_insn(ctx, 12); /* 00A9F2 move.l #0xffffff00,d0 */
	{ { uint32_t v = ctx->d[0]; uint32_t da = ctx->a[1]; ctx->a[1] += 4; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 12); /* 00A9F8 move.l d0,(a1)+ */
	{ { uint32_t v = ctx->d[0]; uint32_t da = ctx->a[1]; ctx->a[1] += 4; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 12); /* 00A9FA move.l d0,(a1)+ */
	{ { uint32_t v = ctx->d[0]; uint32_t da = ctx->a[1]; ctx->a[1] += 4; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 12); /* 00A9FC move.l d0,(a1)+ */
	{ { uint32_t sv = 0x9u; uint32_t da = 0xFFFF9E0Eu; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 24); /* 00A9FE addi.w #0x9,(0xffff9e0e).l */
	hle_insn(ctx, 16); return; /* 00AA06 rts */
}
/* sub_00AB4E */
HLE_FUNC_PURE(0x00AB4E, Nat00AB4E, 4000)
{
	goto L_00AB4E;
L_009B14:
	{ sh_n(ctx, 1, "asl", 5, 16); hle_insn(ctx, 16); } /* 009B14 asl.w #0x5,d1 */
	{ sh_n(ctx, 2, "asr", 1, 16); hle_insn(ctx, 8); } /* 009B16 asr.w #0x1,d2 */
	{ { uint32_t sa = 0xFFFF80BCu; uint32_t v = ld32(sa); ctx->a[5] = v; } } hle_insn(ctx, 20); /* 009B18 movea.l (0xffff80bc).l,a5 */
	{ { uint32_t v = 0x1u; uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 009B1E move.w #0x1,(a5)+ */
	{ { uint32_t v = (ctx->d[2] & 0xFFFF); uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009B22 move.w d2,(a5)+ */
	{ { uint32_t v = ctx->a[1]; uint32_t da = ctx->a[5]; ctx->a[5] += 4; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 12); /* 009B24 move.l a1,(a5)+ */
	{ { uint32_t v = (ctx->d[1] & 0xFFFF); uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009B26 move.w d1,(a5)+ */
	{ { uint32_t v = ctx->a[5]; uint32_t da = 0xFFFF80BCu; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 20); /* 009B28 move.l a5,(0xffff80bc).l */
	hle_insn(ctx, 16); return; /* 009B2E rts */
L_00AB4E:
	{ { uint32_t sa = 0xFFFF9E62u; uint32_t v = ld16(sa); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00AB4E move.w (0xffff9e62).l,d1 */
	{ ctx->a[1] = 0x00083128u; } hle_insn(ctx, 12); /* 00AB54 lea.l (dat_083128).l,a1 */
	{ { uint32_t v = 0x640u; set_w(&ctx->d[2], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00AB5A move.w #0x640,d2 */
	hle_insn(ctx, 10); goto L_009B14; /* 00AB5E bra.w sub_009B14 */
}
/* sub_00AB62 */
HLE_FUNC_PURE(0x00AB62, Nat00AB62, 4000)
{
	goto L_00AB62;
L_009B14:
	{ sh_n(ctx, 1, "asl", 5, 16); hle_insn(ctx, 16); } /* 009B14 asl.w #0x5,d1 */
	{ sh_n(ctx, 2, "asr", 1, 16); hle_insn(ctx, 8); } /* 009B16 asr.w #0x1,d2 */
	{ { uint32_t sa = 0xFFFF80BCu; uint32_t v = ld32(sa); ctx->a[5] = v; } } hle_insn(ctx, 20); /* 009B18 movea.l (0xffff80bc).l,a5 */
	{ { uint32_t v = 0x1u; uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 009B1E move.w #0x1,(a5)+ */
	{ { uint32_t v = (ctx->d[2] & 0xFFFF); uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009B22 move.w d2,(a5)+ */
	{ { uint32_t v = ctx->a[1]; uint32_t da = ctx->a[5]; ctx->a[5] += 4; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 12); /* 009B24 move.l a1,(a5)+ */
	{ { uint32_t v = (ctx->d[1] & 0xFFFF); uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009B26 move.w d1,(a5)+ */
	{ { uint32_t v = ctx->a[5]; uint32_t da = 0xFFFF80BCu; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 20); /* 009B28 move.l a5,(0xffff80bc).l */
	hle_insn(ctx, 16); return; /* 009B2E rts */
L_00AB62:
	{ { uint32_t sa = 0xFFFF9E64u; uint32_t v = ld16(sa); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00AB62 move.w (0xffff9e64).l,d1 */
	{ ctx->a[1] = 0x0008446Au; } hle_insn(ctx, 12); /* 00AB68 lea.l (dat_08446A).l,a1 */
	{ { uint32_t v = 0x460u; set_w(&ctx->d[2], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00AB6E move.w #0x460,d2 */
	hle_insn(ctx, 10); goto L_009B14; /* 00AB72 bra.w sub_009B14 */
}
/* sub_00AB76 */
HLE_FUNC_PURE(0x00AB76, Nat00AB76, 4000)
{
	goto L_00AB76;
L_009B14:
	{ sh_n(ctx, 1, "asl", 5, 16); hle_insn(ctx, 16); } /* 009B14 asl.w #0x5,d1 */
	{ sh_n(ctx, 2, "asr", 1, 16); hle_insn(ctx, 8); } /* 009B16 asr.w #0x1,d2 */
	{ { uint32_t sa = 0xFFFF80BCu; uint32_t v = ld32(sa); ctx->a[5] = v; } } hle_insn(ctx, 20); /* 009B18 movea.l (0xffff80bc).l,a5 */
	{ { uint32_t v = 0x1u; uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 009B1E move.w #0x1,(a5)+ */
	{ { uint32_t v = (ctx->d[2] & 0xFFFF); uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009B22 move.w d2,(a5)+ */
	{ { uint32_t v = ctx->a[1]; uint32_t da = ctx->a[5]; ctx->a[5] += 4; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 12); /* 009B24 move.l a1,(a5)+ */
	{ { uint32_t v = (ctx->d[1] & 0xFFFF); uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009B26 move.w d1,(a5)+ */
	{ { uint32_t v = ctx->a[5]; uint32_t da = 0xFFFF80BCu; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 20); /* 009B28 move.l a5,(0xffff80bc).l */
	hle_insn(ctx, 16); return; /* 009B2E rts */
L_00AB76:
	{ { uint32_t sa = 0xFFFF9E4Au; uint32_t v = ld16(sa); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00AB76 move.w (0xffff9e4a).l,d1 */
	{ ctx->a[1] = 0x000852CCu; } hle_insn(ctx, 12); /* 00AB7C lea.l (dat_0852CC).l,a1 */
	{ { uint32_t v = 0x500u; set_w(&ctx->d[2], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00AB82 move.w #0x500,d2 */
	hle_insn(ctx, 10); goto L_009B14; /* 00AB86 bra.w sub_009B14 */
}
/* sub_00ACB0 */
HLE_FUNC_PURE(0x00ACB0, Nat00ACB0, 4000)
{
	{ ctx->a[1] = 0x00ACBEu; } hle_insn(ctx, 8); /* 00ACB0 lea.l (dat_00ACBE,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00ACB4 jsr (ObjSpawnTemplate,pc) */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00ACB8 move.w (a0)+,(0x2,a6) */
	hle_insn(ctx, 16); return; /* 00ACBC rts */
}
/* sub_00AD48 */
HLE_FUNC_PURE(0x00AD48, Nat00AD48, 4000)
{
	hle_insn(ctx, 18); Nat00AD9C(ctx); /* 00AD48 jsr (sub_00AD9C,pc) */
	{ { uint32_t da = ctx->a[6]; uint8_t v = ld8(da); cc_set(ctx, CC_Z, (v & 0x40) ? 0 : CC_Z); st8(da, v | 0x40); } } hle_insn(ctx, 16); /* 00AD4C bset.b #0xe,(a6) */
	{ { uint32_t da = ctx->a[6] + (uint32_t)14; uint32_t r = alu(ctx, OP_SUB, 0, ld8(da), 8, 1); st8(da, r); } } hle_insn(ctx, 16); /* 00AD50 neg.b (0xe,a6) */
	{ { uint32_t v = 0xD712u; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 00AD54 move.l #0xd712,(0x20,a6) */
	{ { uint32_t v = 0xA0u; uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00AD5C move.w #0xa0,(0x2,a6) */
	{ { uint32_t da = ctx->a[6]; uint8_t v = ld8(da); cc_set(ctx, CC_Z, (v & 0x80) ? 0 : CC_Z); st8(da, v & 0x7F); } } hle_insn(ctx, 16); /* 00AD62 bclr.b #0xf,(a6) */
	{ { uint32_t v = 0x12Eu; uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00AD66 move.w #0x12e,(0x24,a6) */
	hle_insn(ctx, 16); return; /* 00AD6C rts */
}
/* sub_00AD9C */
HLE_FUNC_PURE(0x00AD9C, Nat00AD9C, 4000)
{
	{ ctx->a[1] = 0x00ADBEu; } hle_insn(ctx, 8); /* 00AD9C lea.l (dat_00ADBE,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00ADA0 jsr (ObjSpawnTemplate,pc) */
	{ { uint32_t da = ctx->a[6]; uint8_t v = ld8(da); cc_set(ctx, CC_Z, (v & 0x40) ? 0 : CC_Z); st8(da, v & 0xBF); } } hle_insn(ctx, 16); /* 00ADA4 bclr.b #0xe,(a6) */
	{ { uint32_t v = 0x40u; uint32_t da = ctx->a[6] + (uint32_t)10; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00ADA8 move.b #0x40,(0xa,a6) */
	{ { uint32_t v = 0xFFu; uint32_t da = ctx->a[6] + (uint32_t)14; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00ADAE move.b #0xff,(0xe,a6) */
	{ { uint32_t da = ctx->a[6] + (uint32_t)11; st8(da, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } } hle_insn(ctx, 16); /* 00ADB4 clr.b (0xb,a6) */
	hle_insn(ctx, 18); u_010642(ctx); /* 00ADB8 jsr (sub_010642,pc) */
	hle_insn(ctx, 16); return; /* 00ADBC rts */
}
/* sub_00AEFA */
HLE_FUNC_PURE(0x00AEFA, Nat00AEFA, 4000)
{
	{ ctx->a[1] = 0x00AF0Au; } hle_insn(ctx, 8); /* 00AEFA lea.l (dat_00AF0A,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00AEFE jsr (ObjSpawnTemplate,pc) */
	{ { uint32_t v = ctx->a[6]; uint32_t da = 0xFFFF9ECAu; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 20); /* 00AF02 move.l a6,(0xffff9eca).l */
	hle_insn(ctx, 16); return; /* 00AF08 rts */
}
/* sub_00AF1C */
HLE_FUNC_PURE(0x00AF1C, Nat00AF1C, 4000)
{
	{ { uint32_t v = ctx->a[0]; ctx->a[7] -= 4; uint32_t da = ctx->a[7]; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 12); /* 00AF1C move.l a0,-(sp) */
	hle_insn(ctx, 18); Nat00D0F8(ctx); /* 00AF1E jsr (sub_00D0F8,pc) */
	{ { uint32_t v = 0xEE50u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00AF22 move.w #0xee50,d1 */
	{ ctx->a[0] = 0x000CB254u; } hle_insn(ctx, 12); /* 00AF26 lea.l (dat_0CB254).l,a0 */
	{ ctx->d[2] = 0x18u; cc_logic32(ctx, 0x18u); } hle_insn(ctx, 4); /* 00AF2C moveq #0x18,d2 */
	{ ctx->d[3] = 0x4u; cc_logic32(ctx, 0x4u); } hle_insn(ctx, 4); /* 00AF2E moveq #0x4,d3 */
	{ { uint32_t v = 0xA000u; ctx->d[6] = v; cc_logic32(ctx, v); } } hle_insn(ctx, 12); /* 00AF30 move.l #0xa000,d6 */
	{ { uint32_t sa = 0xFFFF9E34u; uint32_t v = ld16(sa); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00AF36 move.w (0xffff9e34).l,d7 */
	hle_insn(ctx, 18); u_012234(ctx); /* 00AF3C jsr (VdpQueueTileRect,pc) */
	{ { uint32_t v = 0x0u; set_w(&ctx->d[2], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00AF40 move.w #0x0,d2 */
	hle_insn(ctx, 18); u_009EF8(ctx); /* 00AF44 jsr (sub_009EF8,pc) */
	{ { uint32_t sa = ctx->a[7]; ctx->a[7] += 4; uint32_t v = ld32(sa); ctx->a[0] = v; } } hle_insn(ctx, 12); /* 00AF48 movea.l (sp)+,a0 */
	{ ctx->a[1] = 0x00AF60u; } hle_insn(ctx, 8); /* 00AF4A lea.l (dat_00AF60,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00AF4E jsr (ObjSpawnTemplate,pc) */
	{ { uint32_t v = 0x190u; uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00AF52 move.w #0x190,(0x24,a6) */
	{ { uint32_t v = ctx->a[6]; uint32_t da = 0xFFFF9ECEu; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 20); /* 00AF58 move.l a6,(0xffff9ece).l */
	hle_insn(ctx, 16); return; /* 00AF5E rts */
}
/* sub_00AFD4 */
HLE_FUNC_PURE(0x00AFD4, Nat00AFD4, 4000)
{
	{ { uint32_t sa = 0xFFFF9EDAu; uint32_t v = ld32(sa); ctx->a[1] = v; } } hle_insn(ctx, 20); /* 00AFD4 movea.l (0xffff9eda).l,a1 */
	{ { uint32_t v = 0x64u; uint32_t da = ctx->a[1] + (uint32_t)38; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00AFDA move.w #0x64,(0x26,a1) */
	{ { uint32_t v = 0x64u; uint32_t da = ctx->a[1] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00AFE0 move.w #0x64,(0x24,a1) */
	hle_insn(ctx, 16); return; /* 00AFE6 rts */
}
/* sub_00AFFC */
HLE_FUNC_PURE(0x00AFFC, Nat00AFFC, 4000)
{
	{ ctx->a[1] = 0x00B018u; } hle_insn(ctx, 8); /* 00AFFC lea.l (dat_00B018,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00B000 jsr (ObjSpawnTemplate,pc) */
	{ { uint32_t v = 0xAu; uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00B004 move.w #0xa,(0x24,a6) */
	{ { uint32_t v = 0x28u; uint32_t da = ctx->a[6] + (uint32_t)38; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00B00A move.w #0x28,(0x26,a6) */
	{ { uint32_t v = ctx->a[6]; uint32_t da = 0xFFFF9EDAu; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 20); /* 00B010 move.l a6,(0xffff9eda).l */
	hle_insn(ctx, 16); return; /* 00B016 rts */
}
/* sub_00B02A */
HLE_FUNC_PURE(0x00B02A, Nat00B02A, 4000)
{
	{ { uint32_t v = ctx->a[0]; ctx->a[7] -= 4; uint32_t da = ctx->a[7]; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 12); /* 00B02A move.l a0,-(sp) */
	{ { uint32_t v = 0xE4D0u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00B02C move.w #0xe4d0,d1 */
	{ ctx->a[0] = 0x000A1602u; } hle_insn(ctx, 12); /* 00B030 lea.l (dat_0A1602).l,a0 */
	{ ctx->d[2] = 0x11u; cc_logic32(ctx, 0x11u); } hle_insn(ctx, 4); /* 00B036 moveq #0x11,d2 */
	{ ctx->d[3] = 0xDu; cc_logic32(ctx, 0xDu); } hle_insn(ctx, 4); /* 00B038 moveq #0xd,d3 */
	hle_insn(ctx, 18); u_0112A8(ctx); /* 00B03A bsr.w sub_0112A8 */
	{ { uint32_t v = 0xE4F2u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00B03E move.w #0xe4f2,d1 */
	{ ctx->a[0] = 0x000A161Cu; } hle_insn(ctx, 12); /* 00B042 lea.l (dat_0A161C).l,a0 */
	{ ctx->d[2] = 0x7u; cc_logic32(ctx, 0x7u); } hle_insn(ctx, 4); /* 00B048 moveq #0x7,d2 */
	{ ctx->d[3] = 0xDu; cc_logic32(ctx, 0xDu); } hle_insn(ctx, 4); /* 00B04A moveq #0xd,d3 */
	hle_insn(ctx, 18); u_0112A8(ctx); /* 00B04C bsr.w sub_0112A8 */
	{ { uint32_t v = 0xE9DCu; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00B050 move.w #0xe9dc,d1 */
	hle_insn(ctx, 18); u_0112BE(ctx); /* 00B054 bsr.w sub_0112BE */
	{ { uint32_t v = 0xE9E8u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00B058 move.w #0xe9e8,d1 */
	hle_insn(ctx, 18); u_0112BE(ctx); /* 00B05C bsr.w sub_0112BE */
	{ ctx->a[1] = 0x00B07Cu; } hle_insn(ctx, 8); /* 00B060 lea.l (dat_00B07C,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00B064 jsr (ObjSpawnTemplate,pc) */
	{ { uint32_t v = 0xC8u; uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00B068 move.w #0xc8,(0x24,a6) */
	{ { uint32_t da = ctx->a[6] + (uint32_t)20; st16(da, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } } hle_insn(ctx, 16); /* 00B06E clr.w (0x14,a6) */
	{ { uint32_t v = ctx->a[6]; uint32_t da = 0xFFFF9ECEu; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 20); /* 00B072 move.l a6,(0xffff9ece).l */
	{ { uint32_t sa = ctx->a[7]; ctx->a[7] += 4; uint32_t v = ld32(sa); ctx->a[0] = v; } } hle_insn(ctx, 12); /* 00B078 movea.l (sp)+,a0 */
	hle_insn(ctx, 16); return; /* 00B07A rts */
}
/* sub_00B0B2 */
HLE_FUNC_PURE(0x00B0B2, Nat00B0B2, 4000)
{
	{ ctx->a[1] = 0x00B116u; } hle_insn(ctx, 8); /* 00B0B2 lea.l (dat_00B116,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00B0B6 jsr (ObjSpawnTemplate,pc) */
	{ { uint32_t v = 0x20u; uint32_t da = ctx->a[6] + (uint32_t)10; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00B0BA move.b #0x20,(0xa,a6) */
	{ { uint32_t v = 0xFFu; uint32_t da = ctx->a[6] + (uint32_t)14; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00B0C0 move.b #0xff,(0xe,a6) */
	{ { uint32_t da = ctx->a[6] + (uint32_t)11; st8(da, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } } hle_insn(ctx, 16); /* 00B0C6 clr.b (0xb,a6) */
	{ { uint32_t v = ctx->a[6]; uint32_t da = 0xFFFF9ED6u; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 20); /* 00B0CA move.l a6,(0xffff9ed6).l */
	{ { uint32_t v = 0xFFE0u; uint32_t da = 0xFFFF9F60u; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 20); /* 00B0D0 move.w #0xffe0,(0xffff9f60).l */
	{ { uint32_t sa = ctx->a[6] + (uint32_t)4; uint32_t v = ld16(sa); uint32_t da = ctx->a[6] + (uint32_t)40; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 20); /* 00B0D8 move.w (0x4,a6),(0x28,a6) */
	hle_insn(ctx, 20); u_0115FA(ctx); /* 00B0DE jsr (sub_0115FA).l */
	{ { uint32_t v = 0x3E8u; uint32_t da = 0xFFFF9E1Au; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 20); /* 00B0E4 move.w #0x3e8,(0xffff9e1a).l */
	{ ctx->d[3] = 0x0u; cc_logic32(ctx, 0x0u); } hle_insn(ctx, 4); /* 00B0EC moveq #0x0,d3 */
	hle_insn(ctx, 20); u_0118CE(ctx); /* 00B0EE jsr (sub_0118CE).l */
	{ { uint32_t v = 0x3FCu; uint32_t da = 0xFFFF9E1Au; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 20); /* 00B0F4 move.w #0x3fc,(0xffff9e1a).l */
	{ ctx->d[3] = 0x1u; cc_logic32(ctx, 0x1u); } hle_insn(ctx, 4); /* 00B0FC moveq #0x1,d3 */
	hle_insn(ctx, 20); u_0118CE(ctx); /* 00B0FE jsr (sub_0118CE).l */
	{ { uint32_t v = 0x430u; uint32_t da = 0xFFFF9E58u; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 20); /* 00B104 move.w #0x430,(0xffff9e58).l */
	{ ctx->d[3] = 0x1u; cc_logic32(ctx, 0x1u); } hle_insn(ctx, 4); /* 00B10C moveq #0x1,d3 */
	hle_insn(ctx, 20); u_0118C0(ctx); /* 00B10E jsr (sub_0118C0).l */
	hle_insn(ctx, 16); return; /* 00B114 rts */
}
/* sub_00B1CE */
HLE_FUNC_PURE(0x00B1CE, Nat00B1CE, 4000)
{
	{ ctx->a[1] = 0x00B1EEu; } hle_insn(ctx, 8); /* 00B1CE lea.l (dat_00B1EE,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00B1D2 jsr (ObjSpawnTemplate,pc) */
	{ ctx->a[1] = 0xFFFFAE84u; } hle_insn(ctx, 12); /* 00B1D6 lea.l (0xffffae84).l,a1 */
	{ ctx->d[0] = 0x58u; cc_logic32(ctx, 0x58u); } hle_insn(ctx, 4); /* 00B1DC moveq #0x58,d0 */
	{ ctx->d[1] = 0x63u; cc_logic32(ctx, 0x63u); } hle_insn(ctx, 4); /* 00B1DE moveq #0x63,d1 */
L_00B1E0:
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[1]; ctx->a[1] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00B1E0 move.w d0,(a1)+ */
	{ uint16_t c = (uint16_t)(ctx->d[1] - 1); set_w(&ctx->d[1], c); if (c != 0xFFFF) { hle_insn(ctx, 10); goto L_00B1E0; } hle_insn(ctx, 14); } /* 00B1E2 dbra d1,loc_00B1E0 */
	{ { uint32_t v = ctx->a[6]; uint32_t da = 0xFFFF9ECAu; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 20); /* 00B1E6 move.l a6,(0xffff9eca).l */
	hle_insn(ctx, 16); return; /* 00B1EC rts */
}
/* sub_00B200 */
HLE_FUNC_PURE(0x00B200, Nat00B200, 4000)
{
	{ ctx->d[0] = 0x0u; cc_logic32(ctx, 0x0u); } hle_insn(ctx, 4); /* 00B200 moveq #0x0,d0 */
	{ ctx->a[3] = 0x00B272u; } hle_insn(ctx, 8); /* 00B202 lea.l (dat_00B272,pc),a3 */
L_00B206:
	{ ctx->a[1] = 0x00B260u; } hle_insn(ctx, 8); /* 00B206 lea.l (dat_00B260,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00B20A jsr (ObjSpawnTemplate,pc) */
	{ { uint32_t sa = ctx->a[3]; ctx->a[3] += 4; uint32_t v = ld32(sa); uint32_t da = ctx->a[6] + (uint32_t)2; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 00B20E move.l (a3)+,(0x2,a6) */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6] + (uint32_t)38; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 00B212 move.w d0,(0x26,a6) */
	{ { uint32_t sv = 0x1u; uint32_t r = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[0], r); } } hle_insn(ctx, 4); /* 00B216 addq.w #0x1,d0 */
	{ { uint32_t sv = 0x1Au; (void)alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFF), sv, 16, 0); } } hle_insn(ctx, 8); /* 00B218 cmpi.w #0x1a,d0 */
	if (!c_z(ctx)) { hle_insn(ctx, 10); goto L_00B206; } hle_insn(ctx, 8); /* 00B21C bne.s loc_00B206 */
L_00B21E:
	{ ctx->a[1] = 0x00B260u; } hle_insn(ctx, 8); /* 00B21E lea.l (dat_00B260,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00B222 jsr (ObjSpawnTemplate,pc) */
	{ { uint32_t v = 0x6F64u; uint32_t da = ctx->a[6] + (uint32_t)28; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 00B226 move.l #0x6f64,(0x1c,a6) */
	{ { uint32_t sa = ctx->a[3]; ctx->a[3] += 4; uint32_t v = ld32(sa); uint32_t da = ctx->a[6] + (uint32_t)2; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 00B22E move.l (a3)+,(0x2,a6) */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6] + (uint32_t)38; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 00B232 move.w d0,(0x26,a6) */
	{ { uint32_t sv = 0x1u; uint32_t r = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[0], r); } } hle_insn(ctx, 4); /* 00B236 addq.w #0x1,d0 */
	{ { uint32_t sv = 0x1Du; (void)alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFF), sv, 16, 0); } } hle_insn(ctx, 8); /* 00B238 cmpi.w #0x1d,d0 */
	if (!c_z(ctx)) { hle_insn(ctx, 10); goto L_00B21E; } hle_insn(ctx, 8); /* 00B23C bne.s loc_00B21E */
	{ { uint32_t v = 0x2E2E2E00u; uint32_t da = 0xFFFF8032u; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 28); /* 00B23E move.l #0x2e2e2e00,(0xffff8032).l */
	{ { uint32_t v = 0x2E2E2E00u; uint32_t da = 0xFFFF8082u; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 28); /* 00B248 move.l #0x2e2e2e00,(0xffff8082).l */
	{ { uint32_t da = 0xFFFF8036u; st16(da, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } } hle_insn(ctx, 20); /* 00B252 clr.w (0xffff8036).l */
	{ { uint32_t da = 0xFFFF8086u; st16(da, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } } hle_insn(ctx, 20); /* 00B258 clr.w (0xffff8086).l */
	hle_insn(ctx, 16); return; /* 00B25E rts */
}
/* ScrSetPlayers */
HLE_FUNC_PURE(0x00B2FA, ScrSetPlayers, 4000)
{
	{ { uint32_t sv = 0x7u; uint32_t da = 0xFFFF9DF5u; (void)alu(ctx, OP_SUB, ld8(da), sv, 8, 0); } } hle_insn(ctx, 20); /* 00B2FA cmpi.b #0x7,(0xffff9df5).l */
	if (c_z(ctx)) { hle_insn(ctx, 10); goto L_00B340; } hle_insn(ctx, 8); /* 00B302 beq.s loc_00B340 */
	{ ctx->a[4] = 0xFFFF8000u; } hle_insn(ctx, 12); /* 00B304 lea.l (0xffff8000).l,a4 */
	{ { uint32_t v = 0x0u; uint32_t da = ctx->a[4] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00B30A move.w #0x0,(0x24,a4) */
	{ ctx->a[4] = 0xFFFF8050u; } hle_insn(ctx, 12); /* 00B310 lea.l (0xffff8050).l,a4 */
	{ { uint32_t v = 0x1u; uint32_t da = ctx->a[4] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00B316 move.w #0x1,(0x24,a4) */
	{ { uint32_t sv = 0x3u; uint32_t da = 0xFFFF9DF5u; (void)alu(ctx, OP_SUB, ld8(da), sv, 8, 0); } } hle_insn(ctx, 20); /* 00B31C cmpi.b #0x3,(0xffff9df5).l */
	if (!c_ge(ctx)) { hle_insn(ctx, 10); goto L_00B340; } hle_insn(ctx, 8); /* 00B324 blt.s loc_00B340 */
	{ ctx->a[4] = 0xFFFF8050u; } hle_insn(ctx, 12); /* 00B326 lea.l (0xffff8050).l,a4 */
	{ { uint32_t da = 0xFFFFA0A7u; cc_logic8(ctx, ld8(da)); } } hle_insn(ctx, 16); /* 00B32C tst.b (0xffffa0a7).l */
	if (c_z(ctx)) { hle_insn(ctx, 10); goto L_00B33A; } hle_insn(ctx, 8); /* 00B332 beq.s loc_00B33A */
	{ ctx->a[4] = 0xFFFF8000u; } hle_insn(ctx, 12); /* 00B334 lea.l (0xffff8000).l,a4 */
L_00B33A:
	{ { uint32_t v = 0xFFFFu; uint32_t da = ctx->a[4] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00B33A move.w #0xffff,(0x24,a4) */
L_00B340:
	hle_insn(ctx, 16); return; /* 00B340 rts */
}
/* ScrDemoSkip */
HLE_FUNC_PURE(0x00B35A, ScrDemoSkip, 4000)
{
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00B35A move.w (a0)+,d0 */
	{ { uint32_t sv = 0x7u; uint32_t da = 0xFFFF9DF5u; (void)alu(ctx, OP_SUB, ld8(da), sv, 8, 0); } } hle_insn(ctx, 20); /* 00B35C cmpi.b #0x7,(0xffff9df5).l */
	if (!c_z(ctx)) { hle_insn(ctx, 10); goto L_00B36A; } hle_insn(ctx, 8); /* 00B364 bne.s loc_00B36A */
	{ { uint32_t sa = ctx->a[0] + (uint32_t)-2 + (uint32_t)(int32_t)(int16_t)ctx->d[0]; ctx->a[0] = sa; } } hle_insn(ctx, 12); /* 00B366 lea.l (-0x2,a0,d0.w),a0 */
L_00B36A:
	hle_insn(ctx, 16); return; /* 00B36A rts */
}
/* sub_00D0F8 */
HLE_FUNC_PURE(0x00D0F8, Nat00D0F8, 4000)
{
	{ { uint32_t sa = 0xFFFF80BCu; uint32_t v = ld32(sa); ctx->a[5] = v; } } hle_insn(ctx, 20); /* 00D0F8 movea.l (0xffff80bc).l,a5 */
	{ { uint32_t v = 0x2u; uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 00D0FE move.w #0x2,(a5)+ */
	{ { uint32_t v = 0xF80u; uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 00D102 move.w #0xf80,(a5)+ */
	{ { uint32_t v = 0xE000u; uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 00D106 move.w #0xe000,(a5)+ */
	{ { uint32_t v = ctx->a[5]; uint32_t da = 0xFFFF80BCu; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 20); /* 00D10A move.l a5,(0xffff80bc).l */
	hle_insn(ctx, 16); return; /* 00D110 rts */
}
/* sub_010642 */
static void u_010642(hle_ctx *ctx)
{
	{ { uint32_t v = 0x104B6u; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 010642 move.l #0x104b6,(0x20,a6) */
	hle_insn(ctx, 18); Random(ctx); /* 01064A jsr (Random,pc) */
	{ { uint32_t sv = 0x3Fu; uint32_t r = ((ctx->d[0] & 0xFFFF) & sv) & 0xFFFFu; set_w(&ctx->d[0], r); cc_logic16(ctx, r); } } hle_insn(ctx, 8); /* 01064E andi.w #0x3f,d0 */
	{ { uint32_t sv = 0x50u; uint32_t r = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[0], r); } } hle_insn(ctx, 8); /* 010652 addi.w #0x50,d0 */
	{ { uint32_t da = ctx->a[6]; uint8_t v = ld8(da); cc_set(ctx, CC_Z, (v & 0x40) ? 0 : CC_Z); } } hle_insn(ctx, 12); /* 010656 btst.b #0xe,(a6) */
	if (!c_z(ctx)) { hle_insn(ctx, 10); goto L_010660; } hle_insn(ctx, 8); /* 01065A bne.s loc_010660 */
	{ { uint32_t sv = 0x32u; uint32_t r = alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[0], r); } } hle_insn(ctx, 8); /* 01065C subi.w #0x32,d0 */
L_010660:
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 010660 move.w d0,(0x24,a6) */
	hle_insn(ctx, 16); return; /* 010664 rts */
}
/* sub_0112A8 */
static void u_0112A8(hle_ctx *ctx)
{
	{ { uint32_t v = 0xA000u; ctx->d[6] = v; cc_logic32(ctx, v); } } hle_insn(ctx, 12); /* 0112A8 move.l #0xa000,d6 */
	{ { uint32_t v = (ctx->d[7] & 0xFFFF); ctx->a[7] -= 2; uint32_t da = ctx->a[7]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 0112AE move.w d7,-(sp) */
	{ { uint32_t sa = 0xFFFF9E74u; uint32_t v = ld16(sa); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 0112B0 move.w (0xffff9e74).l,d7 */
	hle_insn(ctx, 18); u_012234(ctx); /* 0112B6 jsr (VdpQueueTileRect,pc) */
	{ { uint32_t sa = ctx->a[7]; ctx->a[7] += 2; uint32_t v = ld16(sa); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 0112BA move.w (sp)+,d7 */
	hle_insn(ctx, 16); return; /* 0112BC rts */
}
/* sub_0112BE */
static void u_0112BE(hle_ctx *ctx)
{
	{ ctx->a[0] = 0x0009C5BAu; } hle_insn(ctx, 12); /* 0112BE lea.l (dat_09C5BA).l,a0 */
	{ ctx->d[2] = 0x6u; cc_logic32(ctx, 0x6u); } hle_insn(ctx, 4); /* 0112C4 moveq #0x6,d2 */
	{ ctx->d[3] = 0x5u; cc_logic32(ctx, 0x5u); } hle_insn(ctx, 4); /* 0112C6 moveq #0x5,d3 */
	{ { uint32_t v = 0xA000u; ctx->d[6] = v; cc_logic32(ctx, v); } } hle_insn(ctx, 12); /* 0112C8 move.l #0xa000,d6 */
	{ { uint32_t v = (ctx->d[7] & 0xFFFF); ctx->a[7] -= 2; uint32_t da = ctx->a[7]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 0112CE move.w d7,-(sp) */
	{ { uint32_t sa = 0xFFFF9E48u; uint32_t v = ld16(sa); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 0112D0 move.w (0xffff9e48).l,d7 */
	hle_insn(ctx, 18); u_0122CA(ctx); /* 0112D6 jsr (sub_0122CA,pc) */
	{ { uint32_t sa = ctx->a[7]; ctx->a[7] += 2; uint32_t v = ld16(sa); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 0112DA move.w (sp)+,d7 */
	hle_insn(ctx, 16); return; /* 0112DC rts */
}
/* sub_0115FA */
static void u_0115FA(hle_ctx *ctx)
{
	{ { uint32_t v = 0x112FEu; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 0115FA move.l #0x112fe,(0x20,a6) */
	hle_insn(ctx, 20); Random(ctx); /* 011602 jsr (Random).l */
	{ { uint32_t sv = 0x1Fu; uint32_t r = ((ctx->d[0] & 0xFFFF) & sv) & 0xFFFFu; set_w(&ctx->d[0], r); cc_logic16(ctx, r); } } hle_insn(ctx, 8); /* 011608 andi.w #0x1f,d0 */
	{ { uint32_t sv = 0x1Eu; uint32_t r = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[0], r); } } hle_insn(ctx, 8); /* 01160C addi.w #0x1e,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 011610 move.w d0,(0x24,a6) */
	hle_insn(ctx, 16); return; /* 011614 rts */
}
/* sub_0118C0 */
static void u_0118C0(hle_ctx *ctx)
{
	{ ctx->a[1] = 0x011A30u; } hle_insn(ctx, 8); /* 0118C0 lea.l (dat_011A30,pc),a1 */
	{ { uint32_t sa = 0xFFFF9E58u; uint32_t v = ld16(sa); set_w(&ctx->d[6], v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 0118C4 move.w (0xffff9e58).l,d6 */
	hle_insn(ctx, 10); goto L_0118F4; /* 0118CA bra.w sub_0118F4 */
L_0118F4:
	{ { uint32_t sa = 0xFFFF80BCu; uint32_t v = ld32(sa); ctx->a[5] = v; } } hle_insn(ctx, 20); /* 0118F4 movea.l (0xffff80bc).l,a5 */
	{ { uint32_t v = 0x1u; uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 0118FA move.w #0x1,(a5)+ */
	{ sh_n(ctx, 3, "asl", 2, 16); hle_insn(ctx, 10); } /* 0118FE asl.w #0x2,d3 */
	{ { uint32_t sa = ctx->a[1] + (uint32_t)0 + (uint32_t)(int32_t)(int16_t)ctx->d[3]; uint32_t v = ld16(sa); set_w(&ctx->d[2], v); cc_logic16(ctx, v); } } hle_insn(ctx, 14); /* 011900 move.w (0x0,a1,d3.w),d2 */
	{ sh_n(ctx, 2, "asr", 1, 16); hle_insn(ctx, 8); } /* 011904 asr.w #0x1,d2 */
	{ { uint32_t v = (ctx->d[2] & 0xFFFF); uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 011906 move.w d2,(a5)+ */
	{ { uint32_t sa = ctx->a[1] + (uint32_t)2 + (uint32_t)(int32_t)(int16_t)ctx->d[3]; uint32_t v = ld16(sa); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } } hle_insn(ctx, 14); /* 011908 move.w (0x2,a1,d3.w),d3 */
	{ ctx->a[2] = 0x000CB6B6u; } hle_insn(ctx, 12); /* 01190C lea.l (dat_0CB6B6).l,a2 */
	{ { uint32_t sv = 0xFFFFu; uint32_t r = (ctx->d[3] & sv) & 0xFFFFFFFFu; ctx->d[3] = r; cc_logic32(ctx, r); } } hle_insn(ctx, 16); /* 011912 andi.l #0xffff,d3 */
	{ { ctx->a[2] += ctx->d[3]; } } hle_insn(ctx, 8); /* 011918 adda.l d3,a2 */
	{ { uint32_t v = ctx->a[2]; uint32_t da = ctx->a[5]; ctx->a[5] += 4; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 12); /* 01191A move.l a2,(a5)+ */
	{ sh_n(ctx, 6, "asl", 5, 16); hle_insn(ctx, 16); } /* 01191C asl.w #0x5,d6 */
	{ { uint32_t v = (ctx->d[6] & 0xFFFF); uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 01191E move.w d6,(a5)+ */
	{ { uint32_t v = ctx->a[5]; uint32_t da = 0xFFFF80BCu; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 20); /* 011920 move.l a5,(0xffff80bc).l */
	hle_insn(ctx, 16); return; /* 011926 rts */
}
/* sub_0118CE */
static void u_0118CE(hle_ctx *ctx)
{
	{ ctx->a[1] = 0x011A3Cu; } hle_insn(ctx, 8); /* 0118CE lea.l (dat_011A3C,pc),a1 */
	{ { uint32_t sa = 0xFFFF9E1Au; uint32_t v = ld16(sa); set_w(&ctx->d[6], v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 0118D2 move.w (0xffff9e1a).l,d6 */
	hle_insn(ctx, 10); goto L_0118F4; /* 0118D8 bra.w sub_0118F4 */
L_0118F4:
	{ { uint32_t sa = 0xFFFF80BCu; uint32_t v = ld32(sa); ctx->a[5] = v; } } hle_insn(ctx, 20); /* 0118F4 movea.l (0xffff80bc).l,a5 */
	{ { uint32_t v = 0x1u; uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 0118FA move.w #0x1,(a5)+ */
	{ sh_n(ctx, 3, "asl", 2, 16); hle_insn(ctx, 10); } /* 0118FE asl.w #0x2,d3 */
	{ { uint32_t sa = ctx->a[1] + (uint32_t)0 + (uint32_t)(int32_t)(int16_t)ctx->d[3]; uint32_t v = ld16(sa); set_w(&ctx->d[2], v); cc_logic16(ctx, v); } } hle_insn(ctx, 14); /* 011900 move.w (0x0,a1,d3.w),d2 */
	{ sh_n(ctx, 2, "asr", 1, 16); hle_insn(ctx, 8); } /* 011904 asr.w #0x1,d2 */
	{ { uint32_t v = (ctx->d[2] & 0xFFFF); uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 011906 move.w d2,(a5)+ */
	{ { uint32_t sa = ctx->a[1] + (uint32_t)2 + (uint32_t)(int32_t)(int16_t)ctx->d[3]; uint32_t v = ld16(sa); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } } hle_insn(ctx, 14); /* 011908 move.w (0x2,a1,d3.w),d3 */
	{ ctx->a[2] = 0x000CB6B6u; } hle_insn(ctx, 12); /* 01190C lea.l (dat_0CB6B6).l,a2 */
	{ { uint32_t sv = 0xFFFFu; uint32_t r = (ctx->d[3] & sv) & 0xFFFFFFFFu; ctx->d[3] = r; cc_logic32(ctx, r); } } hle_insn(ctx, 16); /* 011912 andi.l #0xffff,d3 */
	{ { ctx->a[2] += ctx->d[3]; } } hle_insn(ctx, 8); /* 011918 adda.l d3,a2 */
	{ { uint32_t v = ctx->a[2]; uint32_t da = ctx->a[5]; ctx->a[5] += 4; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 12); /* 01191A move.l a2,(a5)+ */
	{ sh_n(ctx, 6, "asl", 5, 16); hle_insn(ctx, 16); } /* 01191C asl.w #0x5,d6 */
	{ { uint32_t v = (ctx->d[6] & 0xFFFF); uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 01191E move.w d6,(a5)+ */
	{ { uint32_t v = ctx->a[5]; uint32_t da = 0xFFFF80BCu; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 20); /* 011920 move.l a5,(0xffff80bc).l */
	hle_insn(ctx, 16); return; /* 011926 rts */
}
/* VdpQueueTileRect */
static void u_012234(hle_ctx *ctx)
{
	{ { uint32_t sv = 0x1u; uint32_t r = alu(ctx, OP_SUB, (ctx->d[3] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[3], r); } } hle_insn(ctx, 4); /* 012234 subq.w #0x1,d3 */
	{ { uint32_t sa = 0xFFFF80B8u; uint32_t v = ld32(sa); ctx->a[1] = v; } } hle_insn(ctx, 20); /* 012236 movea.l (0xffff80b8).l,a1 */
	{ { uint32_t sa = 0xFFFF80BCu; uint32_t v = ld32(sa); ctx->a[5] = v; } } hle_insn(ctx, 20); /* 01223C movea.l (0xffff80bc).l,a5 */
	{ { uint32_t v = 0x6u; uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 012242 move.w #0x6,(a5)+ */
	{ { uint32_t v = (ctx->d[2] & 0xFFFF); uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 012246 move.w d2,(a5)+ */
	{ { uint32_t v = ctx->a[1]; uint32_t da = ctx->a[5]; ctx->a[5] += 4; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 12); /* 012248 move.l a1,(a5)+ */
	{ { uint32_t v = (ctx->d[1] & 0xFFFF); uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 01224A move.w d1,(a5)+ */
	{ { uint32_t v = (ctx->d[3] & 0xFFFF); uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 01224C move.w d3,(a5)+ */
L_01224E:
	{ { uint32_t v = (ctx->d[2] & 0xFFFF); set_w(&ctx->d[4], v); cc_logic16(ctx, v); } } hle_insn(ctx, 4); /* 01224E move.w d2,d4 */
	{ { uint32_t sv = 0x1u; uint32_t r = alu(ctx, OP_SUB, (ctx->d[4] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[4], r); } } hle_insn(ctx, 4); /* 012250 subq.w #0x1,d4 */
	{ { uint32_t v = ctx->a[0]; ctx->a[4] = v; } } hle_insn(ctx, 4); /* 012252 movea.l a0,a4 */
L_012254:
	{ { uint32_t sa = ctx->a[4]; ctx->a[4] += 2; uint32_t v = ld16(sa); set_w(&ctx->d[5], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 012254 move.w (a4)+,d5 */
	{ { uint32_t sv = (ctx->d[7] & 0xFFFF); uint32_t r = alu(ctx, OP_ADD, (ctx->d[5] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[5], r); } } hle_insn(ctx, 4); /* 012256 add.w d7,d5 */
	{ { uint32_t sv = (ctx->d[6] & 0xFFFF); uint32_t r = ((ctx->d[5] & 0xFFFF) | sv) & 0xFFFFu; set_w(&ctx->d[5], r); cc_logic16(ctx, r); } } hle_insn(ctx, 4); /* 012258 or.w d6,d5 */
	{ { uint32_t v = (ctx->d[5] & 0xFFFF); uint32_t da = ctx->a[1]; ctx->a[1] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 01225A move.w d5,(a1)+ */
	{ uint16_t c = (uint16_t)(ctx->d[4] - 1); set_w(&ctx->d[4], c); if (c != 0xFFFF) { hle_insn(ctx, 10); goto L_012254; } hle_insn(ctx, 14); } /* 01225C dbra d4,loc_012254 */
	{ { uint32_t sa = ctx->a[0] + (uint32_t)80; ctx->a[0] = sa; } } hle_insn(ctx, 8); /* 012260 lea.l (0x50,a0),a0 */
	{ { uint32_t sv = 0x80u; uint32_t r = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[1], r); } } hle_insn(ctx, 8); /* 012264 addi.w #0x80,d1 */
	{ uint16_t c = (uint16_t)(ctx->d[3] - 1); set_w(&ctx->d[3], c); if (c != 0xFFFF) { hle_insn(ctx, 10); goto L_01224E; } hle_insn(ctx, 14); } /* 012268 dbra d3,loc_01224E */
	{ { uint32_t v = ctx->a[5]; uint32_t da = 0xFFFF80BCu; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 20); /* 01226C move.l a5,(0xffff80bc).l */
	{ { uint32_t v = ctx->a[1]; uint32_t da = 0xFFFF80B8u; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 20); /* 012272 move.l a1,(0xffff80b8).l */
	hle_insn(ctx, 16); return; /* 012278 rts */
}
/* sub_0122CA */
static void u_0122CA(hle_ctx *ctx)
{
	{ { uint32_t sv = 0x1u; uint32_t r = alu(ctx, OP_SUB, (ctx->d[3] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[3], r); } } hle_insn(ctx, 4); /* 0122CA subq.w #0x1,d3 */
	{ { uint32_t sa = 0xFFFF80B8u; uint32_t v = ld32(sa); ctx->a[1] = v; } } hle_insn(ctx, 20); /* 0122CC movea.l (0xffff80b8).l,a1 */
	{ { uint32_t sa = 0xFFFF80BCu; uint32_t v = ld32(sa); ctx->a[5] = v; } } hle_insn(ctx, 20); /* 0122D2 movea.l (0xffff80bc).l,a5 */
	{ { uint32_t v = 0x6u; uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 0122D8 move.w #0x6,(a5)+ */
	{ { uint32_t v = (ctx->d[2] & 0xFFFF); uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 0122DC move.w d2,(a5)+ */
	{ { uint32_t v = ctx->a[1]; uint32_t da = ctx->a[5]; ctx->a[5] += 4; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 12); /* 0122DE move.l a1,(a5)+ */
	{ { uint32_t v = (ctx->d[1] & 0xFFFF); uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 0122E0 move.w d1,(a5)+ */
	{ { uint32_t v = (ctx->d[3] & 0xFFFF); uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 0122E2 move.w d3,(a5)+ */
L_0122E4:
	{ { uint32_t v = (ctx->d[2] & 0xFFFF); set_w(&ctx->d[4], v); cc_logic16(ctx, v); } } hle_insn(ctx, 4); /* 0122E4 move.w d2,d4 */
	{ { uint32_t sv = 0x1u; uint32_t r = alu(ctx, OP_SUB, (ctx->d[4] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[4], r); } } hle_insn(ctx, 4); /* 0122E6 subq.w #0x1,d4 */
	{ { uint32_t v = ctx->a[0]; ctx->a[4] = v; } } hle_insn(ctx, 4); /* 0122E8 movea.l a0,a4 */
L_0122EA:
	{ { uint32_t sa = ctx->a[4]; ctx->a[4] += 2; uint32_t v = ld16(sa); set_w(&ctx->d[5], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 0122EA move.w (a4)+,d5 */
	{ { uint32_t sv = (ctx->d[7] & 0xFFFF); uint32_t r = alu(ctx, OP_ADD, (ctx->d[5] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[5], r); } } hle_insn(ctx, 4); /* 0122EC add.w d7,d5 */
	{ { uint32_t sv = (ctx->d[6] & 0xFFFF); uint32_t r = ((ctx->d[5] & 0xFFFF) | sv) & 0xFFFFu; set_w(&ctx->d[5], r); cc_logic16(ctx, r); } } hle_insn(ctx, 4); /* 0122EE or.w d6,d5 */
	{ { uint32_t v = (ctx->d[5] & 0xFFFF); uint32_t da = ctx->a[1]; ctx->a[1] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 0122F0 move.w d5,(a1)+ */
	{ uint16_t c = (uint16_t)(ctx->d[4] - 1); set_w(&ctx->d[4], c); if (c != 0xFFFF) { hle_insn(ctx, 10); goto L_0122EA; } hle_insn(ctx, 14); } /* 0122F2 dbra d4,loc_0122EA */
	{ cc_set(ctx, CC_Z, (ctx->d[2] & 0x1u) ? 0 : CC_Z); } hle_insn(ctx, 10); /* 0122F6 btst.b #0x0,d2 */
	if (c_z(ctx)) { hle_insn(ctx, 10); goto L_0122FE; } hle_insn(ctx, 8); /* 0122FA beq.s loc_0122FE */
	{ { uint32_t sv = 0x2u; ctx->a[4] += sv; } } hle_insn(ctx, 8); /* 0122FC addq.l #0x2,a4 */
L_0122FE:
	{ { uint32_t v = ctx->a[4]; ctx->a[0] = v; } } hle_insn(ctx, 4); /* 0122FE movea.l a4,a0 */
	{ { uint32_t sv = 0x80u; uint32_t r = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[1], r); } } hle_insn(ctx, 8); /* 012300 addi.w #0x80,d1 */
	{ uint16_t c = (uint16_t)(ctx->d[3] - 1); set_w(&ctx->d[3], c); if (c != 0xFFFF) { hle_insn(ctx, 10); goto L_0122E4; } hle_insn(ctx, 14); } /* 012304 dbra d3,loc_0122E4 */
	{ { uint32_t v = ctx->a[5]; uint32_t da = 0xFFFF80BCu; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 20); /* 012308 move.l a5,(0xffff80bc).l */
	{ { uint32_t v = ctx->a[1]; uint32_t da = 0xFFFF80B8u; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 20); /* 01230E move.l a1,(0xffff80b8).l */
	hle_insn(ctx, 16); return; /* 012314 rts */
}
/* sub_013D04 */
static void u_013D04(hle_ctx *ctx)
{
	{ { uint32_t base = ctx->a[7] - 4; st16(base + 0, ctx->d[1]); st16(base + 2, ctx->d[2]); ctx->a[7] = base; } } hle_insn(ctx, 16); /* 013D04 movem.w d1-d2,-(sp) */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } } hle_insn(ctx, 4); /* 013D08 move.w d0,d1 */
	{ { uint32_t sv = 0x5Eu; uint32_t r = alu(ctx, OP_SUB, (ctx->d[1] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[1], r); } } hle_insn(ctx, 8); /* 013D0A subi.w #0x5e,d1 */
	{ { uint32_t sa = 0x013D30u + (uint32_t)(int32_t)(int16_t)ctx->d[1]; uint32_t v = ld8(sa); set_b(&ctx->d[2], v); cc_logic8(ctx, v); } } hle_insn(ctx, 14); /* 013D0E move.b (dat_013D30,pc,d1.w),d2 */
	{ { uint32_t sa = 0xFFFF0026u; uint32_t v = ld16(sa); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 013D12 move.w (0xffff0026).l,d1 */
	if (c_z(ctx)) { hle_insn(ctx, 10); goto L_013D24; } hle_insn(ctx, 8); /* 013D18 beq.s loc_013D24 */
	{ { uint32_t sv = 0x5Eu; uint32_t r = alu(ctx, OP_SUB, (ctx->d[1] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[1], r); } } hle_insn(ctx, 8); /* 013D1A subi.w #0x5e,d1 */
	{ { uint32_t sa = 0x013D30u + (uint32_t)(int32_t)(int16_t)ctx->d[1]; uint32_t sv = ld8(sa); (void)alu(ctx, OP_SUB, (ctx->d[2] & 0xFF), sv, 8, 0); } } hle_insn(ctx, 14); /* 013D1E cmp.b (dat_013D30,pc,d1.w),d2 */
	if (!c_ge(ctx)) { hle_insn(ctx, 10); goto L_013D2A; } hle_insn(ctx, 8); /* 013D22 blt.s loc_013D2A */
L_013D24:
	hle_insn(ctx, 20); u_0FBB96(ctx); /* 013D24 jsr (sub_0FBB96).l */
L_013D2A:
	{ { uint32_t base = ctx->a[7]; ctx->d[1] = (uint32_t)(int32_t)(int16_t)ld16(base + 0); ctx->d[2] = (uint32_t)(int32_t)(int16_t)ld16(base + 2); ctx->a[7] = base + 4; } } hle_insn(ctx, 20); /* 013D2A movem.w (sp)+,d1-d2 */
	hle_insn(ctx, 16); return; /* 013D2E rts */
}
/* sub_013E04 */
HLE_FUNC_PURE(0x013E04, Nat013E04, 4000)
{
	goto L_013E04;
L_013D56:
	{ { uint32_t da = 0xFFFF00A4u; cc_logic8(ctx, ld8(da)); } } hle_insn(ctx, 16); /* 013D56 tst.b (0xffff00a4).l */
	if (c_z(ctx)) { hle_insn(ctx, 10); goto L_013D72; } hle_insn(ctx, 8); /* 013D5C beq.s loc_013D72 */
	{ { uint32_t sa = 0xFFFF9EC4u; uint32_t v = ld16(sa); set_w(&ctx->d[4], v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 013D5E move.w (0xffff9ec4).l,d4 */
	if (c_z(ctx)) { hle_insn(ctx, 10); goto L_013D72; } hle_insn(ctx, 8); /* 013D64 beq.s loc_013D72 */
	if (!c_ge(ctx)) { hle_insn(ctx, 10); goto L_013D72; } hle_insn(ctx, 8); /* 013D66 blt.s loc_013D72 */
	{ { uint32_t sa = 0x013D84u + (uint32_t)(int32_t)(int16_t)ctx->d[4]; uint32_t v = ld8(sa); set_b(&ctx->d[4], v); cc_logic8(ctx, v); } } hle_insn(ctx, 14); /* 013D68 move.b (dat_013D84,pc,d4.w),d4 */
	{ { uint32_t sa = 0x013D84u + (uint32_t)(int32_t)(int16_t)ctx->d[3]; uint32_t sv = ld8(sa); (void)alu(ctx, OP_SUB, (ctx->d[4] & 0xFF), sv, 8, 0); } } hle_insn(ctx, 14); /* 013D6C cmp.b (dat_013D84,pc,d3.w),d4 */
	if (c_le(ctx)) { hle_insn(ctx, 10); goto L_013D82; } hle_insn(ctx, 8); /* 013D70 ble.s loc_013D82 */
L_013D72:
	{ { uint32_t v = (ctx->d[3] & 0xFFFF); uint32_t da = 0xFFFF9EC4u; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 013D72 move.w d3,(0xffff9ec4).l */
	{ { uint32_t v = (ctx->d[7] & 0xFFFF); ctx->a[7] -= 2; uint32_t da = ctx->a[7]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 013D78 move.w d7,-(sp) */
	hle_insn(ctx, 20); u_0FC27C(ctx); /* 013D7A jsr (sub_0FC27C).l */
	{ { uint32_t sa = ctx->a[7]; ctx->a[7] += 2; uint32_t v = ld16(sa); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 013D80 move.w (sp)+,d7 */
L_013D82:
	hle_insn(ctx, 16); return; /* 013D82 rts */
L_013E04:
	{ { uint32_t v = 0xEB082u; ctx->d[0] = v; cc_logic32(ctx, v); } } hle_insn(ctx, 12); /* 013E04 move.l #0xeb082,d0 */
	{ { uint32_t v = 0x1D22u; ctx->d[2] = v; cc_logic32(ctx, v); } } hle_insn(ctx, 12); /* 013E0A move.l #0x1d22,d2 */
	{ { uint32_t v = 0xCAu; set_b(&ctx->d[1], v); cc_logic8(ctx, v); } } hle_insn(ctx, 8); /* 013E10 move.b #0xca,d1 */
	{ ctx->d[3] = 0x9u; cc_logic32(ctx, 0x9u); } hle_insn(ctx, 4); /* 013E14 moveq #0x9,d3 */
	hle_insn(ctx, 10); goto L_013D56; /* 013E16 bra.w sub_013D56 */
}
/* sub_0FBB96 */
static void u_0FBB96(hle_ctx *ctx)
{
	{ { uint32_t da = 0xFFFF000Cu; cc_logic16(ctx, ld16(da)); } } hle_insn(ctx, 16); /* 0FBB96 tst.w (0xffff000c).l */
	if (!c_z(ctx)) { hle_insn(ctx, 10); goto L_0FBBAC; } hle_insn(ctx, 8); /* 0FBB9C bne.s loc_0FBBAC */
	{ { uint32_t sv = 0x1u; uint32_t r = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[0], r); } } hle_insn(ctx, 4); /* 0FBB9E addq.w #0x1,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = 0xFFFF0038u; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 0FBBA0 move.w d0,(0xffff0038).l */
	{ { uint32_t da = 0xFFFF002Eu; st8(da, 0); } } hle_insn(ctx, 20); /* 0FBBA6 sf.b (0xffff002e).l */
L_0FBBAC:
	hle_insn(ctx, 16); return; /* 0FBBAC rts */
}
/* sub_0FC27C */
static void u_0FC27C(hle_ctx *ctx)
{
	{ { uint32_t v = ctx->d[0]; ctx->d[3] = v; cc_logic32(ctx, v); } } hle_insn(ctx, 4); /* 0FC27C move.l d0,d3 */
	{ sh_n(ctx, 3, "lsr", 8, 32); hle_insn(ctx, 24); } /* 0FC27E lsr.l #0x8,d3 */
	{ sh_n(ctx, 3, "lsr", 7, 32); hle_insn(ctx, 22); } /* 0FC280 lsr.l #0x7,d3 */
	{ { uint32_t sv = 0x7FFFu; uint32_t r = ((ctx->d[0] & 0xFFFF) & sv) & 0xFFFFu; set_w(&ctx->d[0], r); cc_logic16(ctx, r); } } hle_insn(ctx, 8); /* 0FC282 andi.w #0x7fff,d0 */
	{ { uint32_t sv = 0x8000u; uint32_t r = ((ctx->d[0] & 0xFFFF) | sv) & 0xFFFFu; set_w(&ctx->d[0], r); cc_logic16(ctx, r); } } hle_insn(ctx, 8); /* 0FC286 ori.w #0x8000,d0 */
	hle_insn(ctx, 10); goto L_0FC290; /* 0FC28A bra.s sub_0FC290 */
L_0FC290:
	{ { uint32_t v = (ctx->d[1] & 0xFFFF); uint32_t da = 0xFFFF001Eu; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 0FC290 move.w d1,(0xffff001e).l */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = 0xFFFF0022u; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 0FC296 move.w d0,(0xffff0022).l */
	{ { uint32_t v = (ctx->d[2] & 0xFFFF); uint32_t da = 0xFFFF0020u; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 0FC29C move.w d2,(0xffff0020).l */
	{ { uint32_t v = (ctx->d[3] & 0xFFFF); uint32_t da = 0xFFFF0024u; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 0FC2A2 move.w d3,(0xffff0024).l */
	hle_insn(ctx, 16); return; /* 0FC2A8 rts */
}

/* ------------------------------------------------------------------ ScrNative and the tail-jump target */

/* $00A140 ScrNative (op $14): movea.l (a0)+,a1 ; jmp (a1).  The range stops in front of the
 * `jmp (a1)` ($00A142); the real 68k performs the jump and reaches the target's entry address,
 * where the CPU hook replaces the target if it is registered above. */
HLE_RANGE_PURE(0x00A140, ScrNative, 60, 0x00A142)
{
	HLE_SEQ(ctx, 12);
	ctx->a[1] = ld32(ctx->a[0]);
	ctx->a[0] += 4;
	hle_jump(ctx, 0x00A142);
}

/* $00B342: move.w #$e214,d1 ; moveq #$1e,d2 ; moveq #6,d3 ; jmp (VdpQueueFillRows).l.  Stops in
 * front of the jmp ($00B34A), which the real 68k executes (the callee's rts returns to the script). */
HLE_RANGE_PURE(0x00B342, ScrQueueFillRowsE214, 80, 0x00B34A)
{
	HLE_SEQ(ctx, 8, 4, 4);
	set_w(&ctx->d[1], 0xE214);
	ctx->d[2] = 0x1E;
	ctx->d[3] = 6;
	cc_logic32(ctx, 6);
	hle_jump(ctx, 0x00B34A);
}
