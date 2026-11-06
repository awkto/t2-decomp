/* Level-script spawn opcode handlers (ScriptOps $00938A; docs/subsystems/levels.md, "Opcodes"): the
 * spawn_tNN wrappers ($01 $02 $07-$0E $12 $13 $15 $19 $1B $1D-$20 $24 $26-$2F $30-$3C), queue_gfx
 * $0F-$11, draw_tiles $1A/$1C, load_tiles $25, clear_tiles $28/$29 and copy_tiles $2A.
 *
 * Same scheme as scriptops.c: a handler is entered by the interpreter's `jsr (a1)` with a0 = its
 * arguments, and leaves a0 after them; the layer performs the closing `rts` (charged here). Every
 * handler only touches work RAM (object table, counters, the VDP list in RAM, the sound-driver
 * variables) and reads ROM (script arguments, templates, tile data), so all are HLE_FUNC_PURE. The
 * called subroutines are static C functions generated from the same disassembly (ObjAlloc, Random
 * and ObjSpawnTemplate are the existing C functions, called after their jsr is charged). Every
 * instruction is on the timeline with the GPGX cost of its opcode word (a comment on every line
 * names the 68k instruction and its address); the bodies were produced instruction by instruction
 * from the disassembly. Flags and registers are mirrored exactly (the interpreter's ScriptNext
 * carries the handler's d0/CCR on).
 */
#include "hle.h"

#include <stdint.h>

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

/* asl/asr/lsr.w #n,Dr (1..8): kind 'l' asl, 'a' asr, 'r' lsr; the caller charges 6 + 2n */
static void sh_w(hle_ctx *c, int r, char kind, int n)
{
	uint32_t v = c->d[r] & 0xFFFF, res;
	unsigned f = 0;
	if (kind == 'l') {
		res = (v << n) & 0xFFFF;
		if ((v >> (16 - n)) & 1) f |= CC_C | CC_X;
		uint32_t top = (v >> (15 - n)) & ((1u << (n + 1)) - 1);
		if (top != 0 && top != ((1u << (n + 1)) - 1)) f |= CC_V;
	} else if (kind == 'r') {
		res = v >> n;
		if ((v >> (n - 1)) & 1) f |= CC_C | CC_X;
	} else {
		res = (uint32_t)((int32_t)(int16_t)v >> n) & 0xFFFF;
		if ((v >> (n - 1)) & 1) f |= CC_C | CC_X;
	}
	if (res & 0x8000) f |= CC_N;
	if (!res) f |= CC_Z;
	cc_set(c, CC_X | CC_N | CC_Z | CC_V | CC_C, f);
	set_w(&c->d[r], res);
}
void ScrSpawnT01a(hle_ctx *ctx);
void ScrSpawnT01b(hle_ctx *ctx);
void ScrSpawnT01c(hle_ctx *ctx);
void ScrSpawnT11(hle_ctx *ctx);
void ScrSpawnT03(hle_ctx *ctx);
void ScrSpawnT1d(hle_ctx *ctx);
void ScrSpawnT01d(hle_ctx *ctx);
void ScrSpawnT07b(hle_ctx *ctx);
void ScrSpawnT07a(hle_ctx *ctx);
static void u_009866(hle_ctx *ctx);
void ScrSpawnT0d(hle_ctx *ctx);
void ScrSpawnT0c(hle_ctx *ctx);
static void u_0098FC(hle_ctx *ctx);
void ScrSpawnT0ab(hle_ctx *ctx);
void ScrSpawnT0aa(hle_ctx *ctx);
static void u_00998A(hle_ctx *ctx);
void ScrSpawnT1a(hle_ctx *ctx);
void ScrSpawnT09(hle_ctx *ctx);
static void u_009A02(hle_ctx *ctx);
static void u_009A5E(hle_ctx *ctx);
void ScrQueueGfx0F(hle_ctx *ctx);
void ScrQueueGfx10(hle_ctx *ctx);
void ScrQueueGfx11(hle_ctx *ctx);
void ScrSpawnT0e(hle_ctx *ctx);
void ScrSpawnT0fa(hle_ctx *ctx);
void ScrSpawnT0fb(hle_ctx *ctx);
void ScrCopyTiles048CA6(hle_ctx *ctx);
void ScrSpawnT15b(hle_ctx *ctx);
void ScrLoadTiles(hle_ctx *ctx);
void ScrSpawnT13a(hle_ctx *ctx);
void ScrSpawnT13b(hle_ctx *ctx);
void ScrClearTilesE780(hle_ctx *ctx);
void ScrClearTilesE7B0(hle_ctx *ctx);
static void u_009EF8(hle_ctx *ctx);
void ScrSpawnT17(hle_ctx *ctx);
void ScrDrawTilesE39A(hle_ctx *ctx);
void ScrSpawnT1b(hle_ctx *ctx);
void ScrDrawTilesE51E(hle_ctx *ctx);
void ScrSpawnT23(hle_ctx *ctx);
void ScrSpawnT0ea(hle_ctx *ctx);
void ScrSpawnT24a(hle_ctx *ctx);
void ScrSpawnT24b(hle_ctx *ctx);
void ScrSpawnT0fc(hle_ctx *ctx);
void ScrSpawnT20(hle_ctx *ctx);
void ScrSpawnT0fd(hle_ctx *ctx);
void ScrSpawnT0fe(hle_ctx *ctx);
void ScrSpawnT29(hle_ctx *ctx);
void ScrSpawnT26a(hle_ctx *ctx);
void ScrSpawnT26b(hle_ctx *ctx);
void ScrSpawnT26c(hle_ctx *ctx);
void ScrSpawnT23a(hle_ctx *ctx);
void ScrSpawnT0ff(hle_ctx *ctx);
void ScrSpawnT28b(hle_ctx *ctx);
void ScrSpawnT27a(hle_ctx *ctx);
void ScrSpawnT27b(hle_ctx *ctx);
static void u_00B488(hle_ctx *ctx);
static void u_00B772(hle_ctx *ctx);
static void u_00B846(hle_ctx *ctx);
static void u_00C652(hle_ctx *ctx);
static void u_00D30A(hle_ctx *ctx);
static void u_00EE88(hle_ctx *ctx);
static void u_00F170(hle_ctx *ctx);
static void u_00F356(hle_ctx *ctx);
static void u_00FD0C(hle_ctx *ctx);
static void u_00FFC0(hle_ctx *ctx);
static void u_010408(hle_ctx *ctx);
static void u_012234(hle_ctx *ctx);
static void u_012986(hle_ctx *ctx);
static void u_013D04(hle_ctx *ctx);
static void u_0FBB6C(hle_ctx *ctx);
static void u_0FBB96(hle_ctx *ctx);
/* sub_009486 */
HLE_FUNC_PURE(0x009486, ScrSpawnT01a, 4000)
{
	hle_insn(ctx, 18); ObjAlloc(ctx); /* 009486 jsr (ObjAlloc,pc) */
	{ { uint32_t v = 0x1u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00948A move.w #0x1,d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x8000u) ? 0 : CC_Z); ctx->d[0] |= 0x8000u; } hle_insn(ctx, 10); /* 00948E bset.b #0xf,d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x4000u) ? 0 : CC_Z); ctx->d[0] |= 0x4000u; } hle_insn(ctx, 10); /* 009492 bset.b #0xe,d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x2000u) ? 0 : CC_Z); ctx->d[0] &= ~0x2000u; } hle_insn(ctx, 12); /* 009496 bclr.b #0xd,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00949A move.w d0,(a6) */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00949C move.w (a0)+,(0x2,a6) */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); uint32_t da = ctx->a[6] + (uint32_t)4; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 0094A0 move.w (a0)+,(0x4,a6) */
	{ { uint32_t v = 0x16u; uint32_t da = ctx->a[6] + (uint32_t)6; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 0094A4 move.b #0x16,(0x6,a6) */
	{ { uint32_t v = 0xEu; uint32_t da = ctx->a[6] + (uint32_t)7; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 0094AA move.b #0xe,(0x7,a6) */
	{ { uint32_t v = 0x59ACu; uint32_t da = ctx->a[6] + (uint32_t)28; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 0094B0 move.l #0x59ac,(0x1c,a6) */
	{ { uint32_t v = 0x4u; uint32_t da = ctx->a[6] + (uint32_t)8; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 0094B8 move.w #0x4,(0x8,a6) */
	{ { uint32_t v = 0x40u; uint32_t da = ctx->a[6] + (uint32_t)10; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 0094BE move.b #0x40,(0xa,a6) */
	{ { uint32_t v = 0x1u; uint32_t da = ctx->a[6] + (uint32_t)14; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 0094C4 move.b #0x1,(0xe,a6) */
	{ { uint32_t da = ctx->a[6] + (uint32_t)11; st8(da, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } } hle_insn(ctx, 16); /* 0094CA clr.b (0xb,a6) */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF80DAu; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 0094CE addq.w #0x1,(0xffff80da).l */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF9E06u; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 0094D4 addq.w #0x1,(0xffff9e06).l */
	{ { uint32_t sv = 0x12Cu; uint32_t da = ctx->a[6] + (uint32_t)2; (void)alu(ctx, OP_SUB, ld16(da), sv, 16, 0); } } hle_insn(ctx, 16); /* 0094DA cmpi.w #0x12c,(0x2,a6) */
	if (!c_ge(ctx)) { hle_insn(ctx, 10); goto L_0094EC; } hle_insn(ctx, 8); /* 0094E0 blt.s loc_0094EC */
	{ { uint32_t v = 0xFFu; uint32_t da = ctx->a[6] + (uint32_t)14; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 0094E2 move.b #0xff,(0xe,a6) */
	{ { uint32_t da = ctx->a[6]; uint8_t v = ld8(da); cc_set(ctx, CC_Z, (v & 0x40) ? 0 : CC_Z); st8(da, v & 0xBF); } } hle_insn(ctx, 16); /* 0094E8 bclr.b #0x6,(a6) */
L_0094EC:
	hle_insn(ctx, 18); u_00B488(ctx); /* 0094EC jsr (sub_00B488,pc) */
	hle_insn(ctx, 16); return; /* 0094F0 rts */
}
/* sub_0094F2 */
HLE_FUNC_PURE(0x0094F2, ScrSpawnT01b, 4000)
{
	hle_insn(ctx, 18); ObjAlloc(ctx); /* 0094F2 jsr (ObjAlloc,pc) */
	{ { uint32_t v = 0x1u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 0094F6 move.w #0x1,d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x8000u) ? 0 : CC_Z); ctx->d[0] |= 0x8000u; } hle_insn(ctx, 10); /* 0094FA bset.b #0xf,d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x4000u) ? 0 : CC_Z); ctx->d[0] |= 0x4000u; } hle_insn(ctx, 10); /* 0094FE bset.b #0xe,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009502 move.w d0,(a6) */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009504 move.w (a0)+,(0x2,a6) */
	{ { uint32_t v = 0x13u; uint32_t da = ctx->a[6] + (uint32_t)6; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 009508 move.b #0x13,(0x6,a6) */
	{ { uint32_t v = 0x4Eu; uint32_t da = ctx->a[6] + (uint32_t)7; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00950E move.b #0x4e,(0x7,a6) */
	{ { uint32_t v = 0x5A48u; uint32_t da = ctx->a[6] + (uint32_t)28; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 009514 move.l #0x5a48,(0x1c,a6) */
	{ { uint32_t v = 0x3u; uint32_t da = ctx->a[6] + (uint32_t)8; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00951C move.w #0x3,(0x8,a6) */
	{ { uint32_t v = 0x28u; uint32_t da = ctx->a[6] + (uint32_t)10; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 009522 move.b #0x28,(0xa,a6) */
	{ { uint32_t v = 0x1u; uint32_t da = ctx->a[6] + (uint32_t)14; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 009528 move.b #0x1,(0xe,a6) */
	{ { uint32_t da = ctx->a[6] + (uint32_t)11; st8(da, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } } hle_insn(ctx, 16); /* 00952E clr.b (0xb,a6) */
	{ { uint32_t sv = 0x12Cu; uint32_t da = ctx->a[6] + (uint32_t)2; (void)alu(ctx, OP_SUB, ld16(da), sv, 16, 0); } } hle_insn(ctx, 16); /* 009532 cmpi.w #0x12c,(0x2,a6) */
	if (!c_ge(ctx)) { hle_insn(ctx, 10); goto L_009544; } hle_insn(ctx, 8); /* 009538 blt.s loc_009544 */
	{ { uint32_t v = 0xFFu; uint32_t da = ctx->a[6] + (uint32_t)14; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00953A move.b #0xff,(0xe,a6) */
	{ { uint32_t da = ctx->a[6]; uint8_t v = ld8(da); cc_set(ctx, CC_Z, (v & 0x40) ? 0 : CC_Z); st8(da, v & 0xBF); } } hle_insn(ctx, 16); /* 009540 bclr.b #0x6,(a6) */
L_009544:
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF80DAu; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 009544 addq.w #0x1,(0xffff80da).l */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF9E06u; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 00954A addq.w #0x1,(0xffff9e06).l */
	hle_insn(ctx, 18); u_00C652(ctx); /* 009550 jsr (sub_00C652,pc) */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009554 move.w (a0)+,d0 */
	if (!c_z(ctx)) { hle_insn(ctx, 10); goto L_009568; } hle_insn(ctx, 8); /* 009556 bne.s loc_009568 */
	{ { uint32_t v = 0xA0u; uint32_t da = ctx->a[6] + (uint32_t)40; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009558 move.w #0xa0,(0x28,a6) */
	{ { uint32_t v = 0xF83Eu; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 00955E move.l #0xf83e,(0x20,a6) */
	hle_insn(ctx, 10); goto L_009570; /* 009566 bra.s loc_009570 */
L_009568:
	if (!c_n(ctx)) { hle_insn(ctx, 10); goto L_009570; } hle_insn(ctx, 8); /* 009568 bpl.s loc_009570 */
	{ { uint32_t r = alu(ctx, OP_SUB, 0, (ctx->d[0] & 0xFFFF), 16, 1); set_w(&ctx->d[0], r); } } hle_insn(ctx, 4); /* 00956A neg.w d0 */
	{ { uint32_t da = ctx->a[6]; uint8_t v = ld8(da); cc_set(ctx, CC_Z, (v & 0x10) ? 0 : CC_Z); st8(da, v | 0x10); } } hle_insn(ctx, 16); /* 00956C bset.b #0x4,(a6) */
L_009570:
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6] + (uint32_t)4; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 009570 move.w d0,(0x4,a6) */
	hle_insn(ctx, 16); return; /* 009574 rts */
}
/* sub_009576 */
HLE_FUNC_PURE(0x009576, ScrSpawnT01c, 4000)
{
	hle_insn(ctx, 18); ObjAlloc(ctx); /* 009576 jsr (ObjAlloc,pc) */
	{ { uint32_t v = 0x1u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00957A move.w #0x1,d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x8000u) ? 0 : CC_Z); ctx->d[0] |= 0x8000u; } hle_insn(ctx, 10); /* 00957E bset.b #0xf,d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x4000u) ? 0 : CC_Z); ctx->d[0] |= 0x4000u; } hle_insn(ctx, 10); /* 009582 bset.b #0xe,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009586 move.w d0,(a6) */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009588 move.w (a0)+,(0x2,a6) */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); uint32_t da = ctx->a[6] + (uint32_t)4; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00958C move.w (a0)+,(0x4,a6) */
	{ { uint32_t v = 0x11u; uint32_t da = ctx->a[6] + (uint32_t)6; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 009590 move.b #0x11,(0x6,a6) */
	{ { uint32_t v = 0x79u; uint32_t da = ctx->a[6] + (uint32_t)7; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 009596 move.b #0x79,(0x7,a6) */
	{ { uint32_t v = 0x5A80u; uint32_t da = ctx->a[6] + (uint32_t)28; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 00959C move.l #0x5a80,(0x1c,a6) */
	{ { uint32_t v = 0x2u; uint32_t da = ctx->a[6] + (uint32_t)8; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 0095A4 move.w #0x2,(0x8,a6) */
	{ { uint32_t v = 0x20u; uint32_t da = ctx->a[6] + (uint32_t)10; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 0095AA move.b #0x20,(0xa,a6) */
	{ { uint32_t v = 0x1u; uint32_t da = ctx->a[6] + (uint32_t)14; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 0095B0 move.b #0x1,(0xe,a6) */
	{ { uint32_t da = ctx->a[6] + (uint32_t)11; st8(da, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } } hle_insn(ctx, 16); /* 0095B6 clr.b (0xb,a6) */
	{ { uint32_t sv = 0x12Cu; uint32_t da = ctx->a[6] + (uint32_t)2; (void)alu(ctx, OP_SUB, ld16(da), sv, 16, 0); } } hle_insn(ctx, 16); /* 0095BA cmpi.w #0x12c,(0x2,a6) */
	if (!c_ge(ctx)) { hle_insn(ctx, 10); goto L_0095CC; } hle_insn(ctx, 8); /* 0095C0 blt.s loc_0095CC */
	{ { uint32_t v = 0xFFu; uint32_t da = ctx->a[6] + (uint32_t)14; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 0095C2 move.b #0xff,(0xe,a6) */
	{ { uint32_t da = ctx->a[6]; uint8_t v = ld8(da); cc_set(ctx, CC_Z, (v & 0x40) ? 0 : CC_Z); st8(da, v & 0xBF); } } hle_insn(ctx, 16); /* 0095C8 bclr.b #0x6,(a6) */
L_0095CC:
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF80DAu; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 0095CC addq.w #0x1,(0xffff80da).l */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF9E06u; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 0095D2 addq.w #0x1,(0xffff9e06).l */
	hle_insn(ctx, 18); u_00B772(ctx); /* 0095D8 jsr (sub_00B772,pc) */
	hle_insn(ctx, 16); return; /* 0095DC rts */
}
/* sub_0095DE */
HLE_FUNC_PURE(0x0095DE, ScrSpawnT11, 4000)
{
	hle_insn(ctx, 18); ObjAlloc(ctx); /* 0095DE jsr (ObjAlloc,pc) */
	{ { uint32_t v = 0x11u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 0095E2 move.w #0x11,d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x8000u) ? 0 : CC_Z); ctx->d[0] |= 0x8000u; } hle_insn(ctx, 10); /* 0095E6 bset.b #0xf,d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x4000u) ? 0 : CC_Z); ctx->d[0] |= 0x4000u; } hle_insn(ctx, 10); /* 0095EA bset.b #0xe,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 0095EE move.w d0,(a6) */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 0095F0 move.w (a0)+,(0x2,a6) */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); uint32_t da = ctx->a[6] + (uint32_t)4; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 0095F4 move.w (a0)+,(0x4,a6) */
	{ { uint32_t v = 0x10u; uint32_t da = ctx->a[6] + (uint32_t)6; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 0095F8 move.b #0x10,(0x6,a6) */
	{ { uint32_t v = 0x8Eu; uint32_t da = ctx->a[6] + (uint32_t)7; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 0095FE move.b #0x8e,(0x7,a6) */
	{ { uint32_t v = 0x5ABCu; uint32_t da = ctx->a[6] + (uint32_t)28; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 009604 move.l #0x5abc,(0x1c,a6) */
	{ { uint32_t v = 0x2u; uint32_t da = ctx->a[6] + (uint32_t)8; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00960C move.w #0x2,(0x8,a6) */
	{ { uint32_t v = 0x10u; uint32_t da = ctx->a[6] + (uint32_t)10; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 009612 move.b #0x10,(0xa,a6) */
	{ { uint32_t v = 0x1u; uint32_t da = ctx->a[6] + (uint32_t)14; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 009618 move.b #0x1,(0xe,a6) */
	{ { uint32_t da = ctx->a[6] + (uint32_t)11; st8(da, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } } hle_insn(ctx, 16); /* 00961E clr.b (0xb,a6) */
	{ { uint32_t sv = 0x12Cu; uint32_t da = ctx->a[6] + (uint32_t)2; (void)alu(ctx, OP_SUB, ld16(da), sv, 16, 0); } } hle_insn(ctx, 16); /* 009622 cmpi.w #0x12c,(0x2,a6) */
	if (!c_ge(ctx)) { hle_insn(ctx, 10); goto L_009634; } hle_insn(ctx, 8); /* 009628 blt.s loc_009634 */
	{ { uint32_t v = 0xFFu; uint32_t da = ctx->a[6] + (uint32_t)14; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00962A move.b #0xff,(0xe,a6) */
	{ { uint32_t da = ctx->a[6]; uint8_t v = ld8(da); cc_set(ctx, CC_Z, (v & 0x40) ? 0 : CC_Z); st8(da, v & 0xBF); } } hle_insn(ctx, 16); /* 009630 bclr.b #0x6,(a6) */
L_009634:
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF80DAu; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 009634 addq.w #0x1,(0xffff80da).l */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF9E06u; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 00963A addq.w #0x1,(0xffff9e06).l */
	hle_insn(ctx, 18); u_00B846(ctx); /* 009640 jsr (sub_00B846,pc) */
	hle_insn(ctx, 16); return; /* 009644 rts */
}
/* spawn_t03 */
HLE_FUNC_PURE(0x009646, ScrSpawnT03, 4000)
{
	hle_insn(ctx, 18); ObjAlloc(ctx); /* 009646 jsr (ObjAlloc,pc) */
	{ { uint32_t v = 0x3u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00964A move.w #0x3,d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x8000u) ? 0 : CC_Z); ctx->d[0] &= ~0x8000u; } hle_insn(ctx, 12); /* 00964E bclr.b #0xf,d0 */
	hle_insn(ctx, 18); Random(ctx); /* 009652 jsr (Random,pc) */
	{ { uint32_t sv = 0x3u; uint32_t r = ((ctx->d[0] & 0xFFFF) & sv) & 0xFFFFu; set_w(&ctx->d[0], r); cc_logic16(ctx, r); } } hle_insn(ctx, 8); /* 009656 andi.w #0x3,d0 */
	if (!c_z(ctx)) { hle_insn(ctx, 10); goto L_009660; } hle_insn(ctx, 8); /* 00965A bne.s loc_009660 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x2000u) ? 0 : CC_Z); ctx->d[0] |= 0x2000u; } hle_insn(ctx, 10); /* 00965C bset.b #0xd,d0 */
L_009660:
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009660 move.w d0,(a6) */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009662 move.w (a0)+,d0 */
	if (!c_n(ctx)) { hle_insn(ctx, 10); goto L_00966C; } hle_insn(ctx, 8); /* 009664 bpl.s loc_00966C */
	{ { uint32_t da = ctx->a[6]; uint8_t v = ld8(da); cc_set(ctx, CC_Z, (v & 0x4) ? 0 : CC_Z); st8(da, v | 0x4); } } hle_insn(ctx, 16); /* 009666 bset.b #0xa,(a6) */
	{ { uint32_t r = alu(ctx, OP_SUB, 0, (ctx->d[0] & 0xFFFF), 16, 1); set_w(&ctx->d[0], r); } } hle_insn(ctx, 4); /* 00966A neg.w d0 */
L_00966C:
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 00966C move.w d0,(0x2,a6) */
	{ { uint32_t v = 0x150u; uint32_t da = ctx->a[6] + (uint32_t)4; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009670 move.w #0x150,(0x4,a6) */
	{ { uint32_t v = 0x17u; uint32_t da = ctx->a[6] + (uint32_t)6; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 009676 move.b #0x17,(0x6,a6) */
	{ { uint32_t v = 0x8u; uint32_t da = ctx->a[6] + (uint32_t)7; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00967C move.b #0x8,(0x7,a6) */
	{ { uint32_t v = 0x10u; uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009682 move.w #0x10,(0x24,a6) */
	{ { uint32_t v = 0x5AF8u; uint32_t da = ctx->a[6] + (uint32_t)28; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 009688 move.l #0x5af8,(0x1c,a6) */
	{ { uint32_t v = 0xD978u; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 009690 move.l #0xd978,(0x20,a6) */
	{ { uint32_t v = 0xFu; uint32_t da = ctx->a[6] + (uint32_t)8; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009698 move.w #0xf,(0x8,a6) */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF80DAu; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 00969E addq.w #0x1,(0xffff80da).l */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF9E06u; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 0096A4 addq.w #0x1,(0xffff9e06).l */
	{ { uint32_t v = 0x64u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 0096AA move.w #0x64,d0 */
	hle_insn(ctx, 20); u_013D04(ctx); /* 0096AE jsr (sub_013D04).l */
	hle_insn(ctx, 16); return; /* 0096B4 rts */
}
/* sub_0096B6 */
HLE_FUNC_PURE(0x0096B6, ScrSpawnT1d, 4000)
{
	hle_insn(ctx, 18); ObjAlloc(ctx); /* 0096B6 jsr (ObjAlloc,pc) */
	{ { uint32_t v = 0x1Du; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 0096BA move.w #0x1d,d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x8000u) ? 0 : CC_Z); ctx->d[0] |= 0x8000u; } hle_insn(ctx, 10); /* 0096BE bset.b #0xf,d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x4000u) ? 0 : CC_Z); ctx->d[0] |= 0x4000u; } hle_insn(ctx, 10); /* 0096C2 bset.b #0xe,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 0096C6 move.w d0,(a6) */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 0096C8 move.w (a0)+,(0x2,a6) */
	{ { uint32_t v = 0xB8u; uint32_t da = ctx->a[6] + (uint32_t)4; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 0096CC move.w #0xb8,(0x4,a6) */
	{ { uint32_t v = 0x17u; uint32_t da = ctx->a[6] + (uint32_t)6; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 0096D2 move.b #0x17,(0x6,a6) */
	{ { uint32_t v = 0xEu; uint32_t da = ctx->a[6] + (uint32_t)7; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 0096D8 move.b #0xe,(0x7,a6) */
	{ { uint32_t v = 0x64u; uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 0096DE move.w #0x64,(0x24,a6) */
	{ { uint32_t v = 0x59ACu; uint32_t da = ctx->a[6] + (uint32_t)28; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 0096E4 move.l #0x59ac,(0x1c,a6) */
	{ { uint32_t v = 0xD674u; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 0096EC move.l #0xd674,(0x20,a6) */
	{ { uint32_t v = 0x4u; uint32_t da = ctx->a[6] + (uint32_t)8; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 0096F4 move.w #0x4,(0x8,a6) */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 0096FA move.w (a0)+,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFF); uint32_t da = ctx->a[6] + (uint32_t)10; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 12); /* 0096FC move.b d0,(0xa,a6) */
	{ { uint32_t v = 0x1u; uint32_t da = ctx->a[6] + (uint32_t)14; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 009700 move.b #0x1,(0xe,a6) */
	{ { uint32_t da = ctx->a[6] + (uint32_t)11; st8(da, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } } hle_insn(ctx, 16); /* 009706 clr.b (0xb,a6) */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF80DAu; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 00970A addq.w #0x1,(0xffff80da).l */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF9E06u; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 009710 addq.w #0x1,(0xffff9e06).l */
	hle_insn(ctx, 16); return; /* 009716 rts */
}
/* spawn_t01d */
HLE_FUNC_PURE(0x009718, ScrSpawnT01d, 4000)
{
	hle_insn(ctx, 18); ObjAlloc(ctx); /* 009718 jsr (ObjAlloc,pc) */
	{ { uint32_t v = 0x1u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00971C move.w #0x1,d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x8000u) ? 0 : CC_Z); ctx->d[0] |= 0x8000u; } hle_insn(ctx, 10); /* 009720 bset.b #0xf,d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x4000u) ? 0 : CC_Z); ctx->d[0] |= 0x4000u; } hle_insn(ctx, 10); /* 009724 bset.b #0xe,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009728 move.w d0,(a6) */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00972A move.w (a0)+,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 00972C move.w d0,(0x2,a6) */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009730 move.w (a0)+,d1 */
	if (!c_z(ctx)) { hle_insn(ctx, 10); goto L_00973E; } hle_insn(ctx, 8); /* 009732 bne.s loc_00973E */
	{ { uint32_t da = ctx->a[6]; uint8_t v = ld8(da); cc_set(ctx, CC_Z, (v & 0x8) ? 0 : CC_Z); st8(da, v | 0x8); } } hle_insn(ctx, 16); /* 009734 bset.b #0x3,(a6) */
	{ { uint32_t v = 0x88u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009738 move.w #0x88,d1 */
	hle_insn(ctx, 10); goto L_009746; /* 00973C bra.s loc_009746 */
L_00973E:
	if (!c_n(ctx)) { hle_insn(ctx, 10); goto L_009746; } hle_insn(ctx, 8); /* 00973E bpl.s loc_009746 */
	{ { uint32_t r = alu(ctx, OP_SUB, 0, (ctx->d[1] & 0xFFFF), 16, 1); set_w(&ctx->d[1], r); } } hle_insn(ctx, 4); /* 009740 neg.w d1 */
	{ { uint32_t da = ctx->a[6]; uint8_t v = ld8(da); cc_set(ctx, CC_Z, (v & 0x10) ? 0 : CC_Z); st8(da, v | 0x10); } } hle_insn(ctx, 16); /* 009742 bset.b #0x4,(a6) */
L_009746:
	{ { uint32_t v = (ctx->d[1] & 0xFFFF); uint32_t da = ctx->a[6] + (uint32_t)4; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 009746 move.w d1,(0x4,a6) */
	{ { uint32_t v = 0x12u; uint32_t da = ctx->a[6] + (uint32_t)6; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00974A move.b #0x12,(0x6,a6) */
	{ { uint32_t v = 0x9Cu; uint32_t da = ctx->a[6] + (uint32_t)7; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 009750 move.b #0x9c,(0x7,a6) */
	{ { uint32_t v = 0x15Eu; uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009756 move.w #0x15e,(0x24,a6) */
	{ { uint32_t v = 0x5BECu; uint32_t da = ctx->a[6] + (uint32_t)28; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 00975C move.l #0x5bec,(0x1c,a6) */
	{ { uint32_t v = 0xC564u; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 009764 move.l #0xc564,(0x20,a6) */
	{ { uint32_t v = 0x4u; uint32_t da = ctx->a[6] + (uint32_t)8; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00976C move.w #0x4,(0x8,a6) */
	{ { uint32_t da = ctx->a[6] + (uint32_t)20; st16(da, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } } hle_insn(ctx, 16); /* 009772 clr.w (0x14,a6) */
	{ { uint32_t da = ctx->a[6] + (uint32_t)11; st8(da, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } } hle_insn(ctx, 16); /* 009776 clr.b (0xb,a6) */
	{ { uint32_t da = ctx->a[6] + (uint32_t)10; st8(da, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } } hle_insn(ctx, 16); /* 00977A clr.b (0xa,a6) */
	{ ctx->d[1] = 0x1u; cc_logic32(ctx, 0x1u); } hle_insn(ctx, 4); /* 00977E moveq #0x1,d1 */
	{ { uint32_t sv = 0xA0u; uint32_t r = alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[0], r); } } hle_insn(ctx, 8); /* 009780 subi.w #0xa0,d0 */
	if (!c_n(ctx)) { hle_insn(ctx, 10); goto L_00978A; } hle_insn(ctx, 8); /* 009784 bpl.s loc_00978A */
	{ ctx->d[1] = 0xFFFFFFFFu; cc_logic32(ctx, 0xFFFFFFFFu); } hle_insn(ctx, 4); /* 009786 moveq #-0x1,d1 */
	{ { uint32_t r = alu(ctx, OP_SUB, 0, (ctx->d[0] & 0xFFFF), 16, 1); set_w(&ctx->d[0], r); } } hle_insn(ctx, 4); /* 009788 neg.w d0 */
L_00978A:
	{ sh_w(ctx, 0, 'a', 1); hle_insn(ctx, 8); } /* 00978A asr.w #0x1,d0 */
	{ { uint32_t sv = 0x28u; (void)alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFF), sv, 16, 0); } } hle_insn(ctx, 8); /* 00978C cmpi.w #0x28,d0 */
	if (c_le(ctx)) { hle_insn(ctx, 10); goto L_009796; } hle_insn(ctx, 8); /* 009790 ble.s loc_009796 */
	{ { uint32_t v = 0x28u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009792 move.w #0x28,d0 */
L_009796:
	{ { uint32_t v = (ctx->d[1] & 0xFF); uint32_t da = ctx->a[6] + (uint32_t)14; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 12); /* 009796 move.b d1,(0xe,a6) */
	{ { uint32_t v = (ctx->d[0] & 0xFF); uint32_t da = ctx->a[6] + (uint32_t)10; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 12); /* 00979A move.b d0,(0xa,a6) */
	{ { uint32_t v = 0x1u; uint32_t da = ctx->a[6] + (uint32_t)15; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00979E move.b #0x1,(0xf,a6) */
	{ { uint32_t v = 0x10u; uint32_t da = ctx->a[6] + (uint32_t)12; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 0097A4 move.b #0x10,(0xc,a6) */
	{ { uint32_t da = ctx->a[6] + (uint32_t)13; st8(da, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } } hle_insn(ctx, 16); /* 0097AA clr.b (0xd,a6) */
	{ { uint32_t v = 0x12EEEu; ctx->a[5] = v; } } hle_insn(ctx, 12); /* 0097AE movea.l #0x12eee,a5 */
	{ { uint32_t sa = ctx->a[5]; ctx->a[5] += 2; uint32_t v = ld16(sa); uint32_t da = ctx->a[6] + (uint32_t)26; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 0097B4 move.w (a5)+,(0x1a,a6) */
	{ { uint32_t v = ctx->a[5]; uint32_t da = ctx->a[6] + (uint32_t)22; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 16); /* 0097B8 move.l a5,(0x16,a6) */
	{ { uint32_t v = 0x200u; uint32_t da = ctx->a[6] + (uint32_t)20; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 0097BC move.w #0x200,(0x14,a6) */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF80DAu; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 0097C2 addq.w #0x1,(0xffff80da).l */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF9E06u; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 0097C8 addq.w #0x1,(0xffff9e06).l */
	hle_insn(ctx, 16); return; /* 0097CE rts */
}
/* sub_0097D0 */
HLE_FUNC_PURE(0x0097D0, ScrSpawnT07b, 4000)
{
	hle_insn(ctx, 18); ObjAlloc(ctx); /* 0097D0 jsr (ObjAlloc,pc) */
	{ { uint32_t v = 0x7u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 0097D4 move.w #0x7,d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x8000u) ? 0 : CC_Z); ctx->d[0] |= 0x8000u; } hle_insn(ctx, 10); /* 0097D8 bset.b #0xf,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 0097DC move.w d0,(a6) */
	{ { uint32_t v = 0x1A0u; uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 0097DE move.w #0x1a0,(0x2,a6) */
	{ { uint32_t v = 0xBF60u; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 0097E4 move.l #0xbf60,(0x20,a6) */
	{ { uint32_t v = 0x5DEAu; uint32_t da = ctx->a[6] + (uint32_t)28; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 0097EC move.l #0x5dea,(0x1c,a6) */
	{ { uint32_t v = 0xA0u; uint32_t da = ctx->a[6] + (uint32_t)10; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 0097F4 move.b #0xa0,(0xa,a6) */
	{ { uint32_t v = 0xFFu; uint32_t da = ctx->a[6] + (uint32_t)14; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 0097FA move.b #0xff,(0xe,a6) */
	hle_insn(ctx, 10); goto L_009832; /* 009800 bra.s sub_009832 */
L_009832:
	{ { uint32_t da = ctx->a[6] + (uint32_t)11; st8(da, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } } hle_insn(ctx, 16); /* 009832 clr.b (0xb,a6) */
	{ { uint32_t v = 0x50u; uint32_t da = ctx->a[6] + (uint32_t)4; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009836 move.w #0x50,(0x4,a6) */
	{ { uint32_t v = 0x28u; uint32_t da = ctx->a[6] + (uint32_t)8; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00983C move.w #0x28,(0x8,a6) */
	{ { uint32_t v = 0xEu; uint32_t da = ctx->a[6] + (uint32_t)6; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 009842 move.b #0xe,(0x6,a6) */
	{ { uint32_t v = 0x8Cu; uint32_t da = ctx->a[6] + (uint32_t)7; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 009848 move.b #0x8c,(0x7,a6) */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF80DAu; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 00984E addq.w #0x1,(0xffff80da).l */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF9E08u; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 009854 addq.w #0x1,(0xffff9e08).l */
	{ { uint32_t v = ctx->a[6]; uint32_t da = 0xFFFF9ECEu; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 20); /* 00985A move.l a6,(0xffff9ece).l */
	hle_insn(ctx, 18); u_009866(ctx); /* 009860 jsr (sub_009866,pc) */
	hle_insn(ctx, 16); return; /* 009864 rts */
}
/* sub_009802 */
HLE_FUNC_PURE(0x009802, ScrSpawnT07a, 4000)
{
	hle_insn(ctx, 18); ObjAlloc(ctx); /* 009802 jsr (ObjAlloc,pc) */
	{ { uint32_t v = 0x7u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009806 move.w #0x7,d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x8000u) ? 0 : CC_Z); ctx->d[0] |= 0x8000u; } hle_insn(ctx, 10); /* 00980A bset.b #0xf,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00980E move.w d0,(a6) */
	{ { uint32_t v = 0xFFA0u; uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009810 move.w #0xffa0,(0x2,a6) */
	{ { uint32_t v = 0xBCF6u; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 009816 move.l #0xbcf6,(0x20,a6) */
	{ { uint32_t v = 0x5DC0u; uint32_t da = ctx->a[6] + (uint32_t)28; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 00981E move.l #0x5dc0,(0x1c,a6) */
	{ { uint32_t v = 0x80u; uint32_t da = ctx->a[6] + (uint32_t)10; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 009826 move.b #0x80,(0xa,a6) */
	{ { uint32_t v = 0x1u; uint32_t da = ctx->a[6] + (uint32_t)14; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00982C move.b #0x1,(0xe,a6) */
	{ { uint32_t da = ctx->a[6] + (uint32_t)11; st8(da, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } } hle_insn(ctx, 16); /* 009832 clr.b (0xb,a6) */
	{ { uint32_t v = 0x50u; uint32_t da = ctx->a[6] + (uint32_t)4; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009836 move.w #0x50,(0x4,a6) */
	{ { uint32_t v = 0x28u; uint32_t da = ctx->a[6] + (uint32_t)8; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00983C move.w #0x28,(0x8,a6) */
	{ { uint32_t v = 0xEu; uint32_t da = ctx->a[6] + (uint32_t)6; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 009842 move.b #0xe,(0x6,a6) */
	{ { uint32_t v = 0x8Cu; uint32_t da = ctx->a[6] + (uint32_t)7; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 009848 move.b #0x8c,(0x7,a6) */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF80DAu; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 00984E addq.w #0x1,(0xffff80da).l */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF9E08u; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 009854 addq.w #0x1,(0xffff9e08).l */
	{ { uint32_t v = ctx->a[6]; uint32_t da = 0xFFFF9ECEu; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 20); /* 00985A move.l a6,(0xffff9ece).l */
	hle_insn(ctx, 18); u_009866(ctx); /* 009860 jsr (sub_009866,pc) */
	hle_insn(ctx, 16); return; /* 009864 rts */
}
/* sub_009866 */
static void u_009866(hle_ctx *ctx)
{
	{ ctx->a[4] = 0xFFFFAA9Eu; } hle_insn(ctx, 12); /* 009866 lea.l (0xffffaa9e).l,a4 */
	{ ctx->d[0] = 0x0u; cc_logic32(ctx, 0x0u); } hle_insn(ctx, 4); /* 00986C moveq #0x0,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[4]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00986E move.w d0,(a4) */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[4] + (uint32_t)32; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 009870 move.w d0,(0x20,a4) */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[4] + (uint32_t)64; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 009874 move.w d0,(0x40,a4) */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[4] + (uint32_t)96; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 009878 move.w d0,(0x60,a4) */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[4] + (uint32_t)128; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 00987C move.w d0,(0x80,a4) */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[4] + (uint32_t)160; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 009880 move.w d0,(0xa0,a4) */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[4] + (uint32_t)192; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 009884 move.w d0,(0xc0,a4) */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[4] + (uint32_t)224; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 009888 move.w d0,(0xe0,a4) */
	hle_insn(ctx, 16); return; /* 00988C rts */
}
/* sub_00988E */
HLE_FUNC_PURE(0x00988E, ScrSpawnT0d, 4000)
{
	hle_insn(ctx, 18); ObjAlloc(ctx); /* 00988E jsr (ObjAlloc,pc) */
	{ { uint32_t v = 0x800Du; uint32_t da = ctx->a[6]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 009892 move.w #0x800d,(a6) */
	{ { uint32_t v = 0x160u; uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009896 move.w #0x160,(0x2,a6) */
	{ { uint32_t v = 0xB86Au; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 00989C move.l #0xb86a,(0x20,a6) */
	{ { uint32_t v = 0x5E1Cu; uint32_t da = ctx->a[6] + (uint32_t)28; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 0098A4 move.l #0x5e1c,(0x1c,a6) */
	hle_insn(ctx, 10); goto L_0098CC; /* 0098AC bra.s sub_0098CC */
L_0098CC:
	{ { uint32_t v = 0x68u; uint32_t da = ctx->a[6] + (uint32_t)4; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 0098CC move.w #0x68,(0x4,a6) */
	{ { uint32_t v = 0x28u; uint32_t da = ctx->a[6] + (uint32_t)8; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 0098D2 move.w #0x28,(0x8,a6) */
	{ { uint32_t v = 0xF0u; uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 0098D8 move.w #0xf0,(0x24,a6) */
	{ { uint32_t v = 0xEu; uint32_t da = ctx->a[6] + (uint32_t)6; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 0098DE move.b #0xe,(0x6,a6) */
	{ { uint32_t v = 0x8Cu; uint32_t da = ctx->a[6] + (uint32_t)7; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 0098E4 move.b #0x8c,(0x7,a6) */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF80DAu; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 0098EA addq.w #0x1,(0xffff80da).l */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF9E08u; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 0098F0 addq.w #0x1,(0xffff9e08).l */
	hle_insn(ctx, 18); u_0098FC(ctx); /* 0098F6 jsr (sub_0098FC,pc) */
	hle_insn(ctx, 16); return; /* 0098FA rts */
}
/* sub_0098AE */
HLE_FUNC_PURE(0x0098AE, ScrSpawnT0c, 4000)
{
	hle_insn(ctx, 18); ObjAlloc(ctx); /* 0098AE jsr (ObjAlloc,pc) */
	{ { uint32_t v = 0x800Cu; uint32_t da = ctx->a[6]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 0098B2 move.w #0x800c,(a6) */
	{ { uint32_t v = 0xFFE0u; uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 0098B6 move.w #0xffe0,(0x2,a6) */
	{ { uint32_t v = 0xB962u; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 0098BC move.l #0xb962,(0x20,a6) */
	{ { uint32_t v = 0x5E56u; uint32_t da = ctx->a[6] + (uint32_t)28; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 0098C4 move.l #0x5e56,(0x1c,a6) */
	{ { uint32_t v = 0x68u; uint32_t da = ctx->a[6] + (uint32_t)4; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 0098CC move.w #0x68,(0x4,a6) */
	{ { uint32_t v = 0x28u; uint32_t da = ctx->a[6] + (uint32_t)8; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 0098D2 move.w #0x28,(0x8,a6) */
	{ { uint32_t v = 0xF0u; uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 0098D8 move.w #0xf0,(0x24,a6) */
	{ { uint32_t v = 0xEu; uint32_t da = ctx->a[6] + (uint32_t)6; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 0098DE move.b #0xe,(0x6,a6) */
	{ { uint32_t v = 0x8Cu; uint32_t da = ctx->a[6] + (uint32_t)7; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 0098E4 move.b #0x8c,(0x7,a6) */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF80DAu; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 0098EA addq.w #0x1,(0xffff80da).l */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF9E08u; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 0098F0 addq.w #0x1,(0xffff9e08).l */
	hle_insn(ctx, 18); u_0098FC(ctx); /* 0098F6 jsr (sub_0098FC,pc) */
	hle_insn(ctx, 16); return; /* 0098FA rts */
}
/* sub_0098FC */
static void u_0098FC(hle_ctx *ctx)
{
	{ ctx->a[4] = 0xFFFFAB9Eu; } hle_insn(ctx, 12); /* 0098FC lea.l (0xffffab9e).l,a4 */
	{ ctx->d[0] = 0x0u; cc_logic32(ctx, 0x0u); } hle_insn(ctx, 4); /* 009902 moveq #0x0,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[4]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009904 move.w d0,(a4) */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[4] + (uint32_t)32; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 009906 move.w d0,(0x20,a4) */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[4] + (uint32_t)64; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 00990A move.w d0,(0x40,a4) */
	hle_insn(ctx, 16); return; /* 00990E rts */
}
/* sub_009910 */
HLE_FUNC_PURE(0x009910, ScrSpawnT0ab, 4000)
{
	hle_insn(ctx, 18); ObjAlloc(ctx); /* 009910 jsr (ObjAlloc,pc) */
	{ { uint32_t v = 0xBu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009914 move.w #0xb,d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x8000u) ? 0 : CC_Z); ctx->d[0] |= 0x8000u; } hle_insn(ctx, 10); /* 009918 bset.b #0xf,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00991C move.w d0,(a6) */
	{ { uint32_t v = 0x180u; uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00991E move.w #0x180,(0x2,a6) */
	{ { uint32_t v = 0xBA62u; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 009924 move.l #0xba62,(0x20,a6) */
	{ { uint32_t v = 0x5E90u; uint32_t da = ctx->a[6] + (uint32_t)28; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 00992C move.l #0x5e90,(0x1c,a6) */
	hle_insn(ctx, 10); goto L_00995A; /* 009934 bra.s sub_00995A */
L_00995A:
	{ { uint32_t v = 0x48u; uint32_t da = ctx->a[6] + (uint32_t)4; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00995A move.w #0x48,(0x4,a6) */
	{ { uint32_t v = 0x28u; uint32_t da = ctx->a[6] + (uint32_t)8; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009960 move.w #0x28,(0x8,a6) */
	{ { uint32_t v = 0xF0u; uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009966 move.w #0xf0,(0x24,a6) */
	{ { uint32_t v = 0xEu; uint32_t da = ctx->a[6] + (uint32_t)6; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00996C move.b #0xe,(0x6,a6) */
	{ { uint32_t v = 0x8Cu; uint32_t da = ctx->a[6] + (uint32_t)7; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 009972 move.b #0x8c,(0x7,a6) */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF80DAu; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 009978 addq.w #0x1,(0xffff80da).l */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF9E08u; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 00997E addq.w #0x1,(0xffff9e08).l */
	hle_insn(ctx, 18); u_00998A(ctx); /* 009984 jsr (sub_00998A,pc) */
	hle_insn(ctx, 16); return; /* 009988 rts */
}
/* sub_009936 */
HLE_FUNC_PURE(0x009936, ScrSpawnT0aa, 4000)
{
	hle_insn(ctx, 18); ObjAlloc(ctx); /* 009936 jsr (ObjAlloc,pc) */
	{ { uint32_t v = 0xAu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00993A move.w #0xa,d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x8000u) ? 0 : CC_Z); ctx->d[0] |= 0x8000u; } hle_insn(ctx, 10); /* 00993E bset.b #0xf,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009942 move.w d0,(a6) */
	{ { uint32_t v = 0xFFC0u; uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009944 move.w #0xffc0,(0x2,a6) */
	{ { uint32_t v = 0xBB82u; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 00994A move.l #0xbb82,(0x20,a6) */
	{ { uint32_t v = 0x5ECAu; uint32_t da = ctx->a[6] + (uint32_t)28; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 009952 move.l #0x5eca,(0x1c,a6) */
	{ { uint32_t v = 0x48u; uint32_t da = ctx->a[6] + (uint32_t)4; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00995A move.w #0x48,(0x4,a6) */
	{ { uint32_t v = 0x28u; uint32_t da = ctx->a[6] + (uint32_t)8; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009960 move.w #0x28,(0x8,a6) */
	{ { uint32_t v = 0xF0u; uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009966 move.w #0xf0,(0x24,a6) */
	{ { uint32_t v = 0xEu; uint32_t da = ctx->a[6] + (uint32_t)6; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00996C move.b #0xe,(0x6,a6) */
	{ { uint32_t v = 0x8Cu; uint32_t da = ctx->a[6] + (uint32_t)7; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 009972 move.b #0x8c,(0x7,a6) */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF80DAu; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 009978 addq.w #0x1,(0xffff80da).l */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF9E08u; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 00997E addq.w #0x1,(0xffff9e08).l */
	hle_insn(ctx, 18); u_00998A(ctx); /* 009984 jsr (sub_00998A,pc) */
	hle_insn(ctx, 16); return; /* 009988 rts */
}
/* sub_00998A */
static void u_00998A(hle_ctx *ctx)
{
	{ ctx->a[4] = 0xFFFFAABEu; } hle_insn(ctx, 12); /* 00998A lea.l (0xffffaabe).l,a4 */
	{ ctx->d[0] = 0x0u; cc_logic32(ctx, 0x0u); } hle_insn(ctx, 4); /* 009990 moveq #0x0,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[4]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009992 move.w d0,(a4) */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[4] + (uint32_t)32; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 009994 move.w d0,(0x20,a4) */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[4] + (uint32_t)64; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 009998 move.w d0,(0x40,a4) */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[4] + (uint32_t)96; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 00999C move.w d0,(0x60,a4) */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[4] + (uint32_t)128; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 0099A0 move.w d0,(0x80,a4) */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[4] + (uint32_t)160; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 0099A4 move.w d0,(0xa0,a4) */
	hle_insn(ctx, 16); return; /* 0099A8 rts */
}
/* sub_0099D4 */
HLE_FUNC_PURE(0x0099D4, ScrSpawnT1a, 4000)
{
	hle_insn(ctx, 18); u_009A02(ctx); /* 0099D4 bsr.w sub_009A02 */
	{ { uint32_t v = 0x1Au; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 0099D8 move.w #0x1a,d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x8000u) ? 0 : CC_Z); ctx->d[0] |= 0x8000u; } hle_insn(ctx, 10); /* 0099DC bset.b #0xf,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 0099E0 move.w d0,(a6) */
	{ { uint32_t v = 0x12Cu; uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 0099E2 move.w #0x12c,(0x24,a6) */
	{ { uint32_t v = 0xD028u; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 0099E8 move.l #0xd028,(0x20,a6) */
	{ { uint32_t v = 0x6004u; uint32_t da = ctx->a[6] + (uint32_t)28; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 0099F0 move.l #0x6004,(0x1c,a6) */
	{ { uint32_t da = ctx->a[6] + (uint32_t)38; st16(da, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } } hle_insn(ctx, 16); /* 0099F8 clr.w (0x26,a6) */
	hle_insn(ctx, 16); return; /* 0099FC rts */
}
/* sub_0099FE */
HLE_FUNC_PURE(0x0099FE, ScrSpawnT09, 4000)
{
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); set_w(&ctx->d[5], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 0099FE move.w (a0)+,d5 */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009A00 move.w (a0)+,d1 */
	hle_insn(ctx, 18); ObjAlloc(ctx); /* 009A02 jsr (ObjAlloc,pc) */
	{ { cc_logic16(ctx, (ctx->d[2] & 0xFFFF)); } } hle_insn(ctx, 4); /* 009A06 tst.w d2 */
	if (c_n(ctx)) { hle_insn(ctx, 10); goto L_009A5C; } hle_insn(ctx, 12); /* 009A08 bmi.w loc_009A5C */
	hle_insn(ctx, 18); u_009A5E(ctx); /* 009A0C jsr (sub_009A5E,pc) */
	{ { uint32_t v = 0x12E86u; ctx->a[5] = v; } } hle_insn(ctx, 12); /* 009A10 movea.l #0x12e86,a5 */
	{ { uint32_t sa = ctx->a[5]; ctx->a[5] += 2; uint32_t v = ld16(sa); uint32_t da = ctx->a[6] + (uint32_t)26; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009A16 move.w (a5)+,(0x1a,a6) */
	{ { uint32_t v = ctx->a[5]; uint32_t da = ctx->a[6] + (uint32_t)22; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 16); /* 009A1A move.l a5,(0x16,a6) */
	{ { uint32_t v = 0x100u; uint32_t da = ctx->a[6] + (uint32_t)20; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009A1E move.w #0x100,(0x14,a6) */
	{ { uint32_t v = 0x9u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009A24 move.w #0x9,d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x8000u) ? 0 : CC_Z); ctx->d[0] |= 0x8000u; } hle_insn(ctx, 10); /* 009A28 bset.b #0xf,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009A2C move.w d0,(a6) */
	{ { uint32_t v = 0x12Cu; uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009A2E move.w #0x12c,(0x24,a6) */
	{ { uint32_t v = 0xC442u; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 009A34 move.l #0xc442,(0x20,a6) */
	{ { uint32_t v = 0x6042u; uint32_t da = ctx->a[6] + (uint32_t)28; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 009A3C move.l #0x6042,(0x1c,a6) */
	{ { uint32_t v = 0x10u; uint32_t da = ctx->a[6] + (uint32_t)6; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 009A44 move.b #0x10,(0x6,a6) */
	{ { uint32_t v = 0x96u; uint32_t da = ctx->a[6] + (uint32_t)7; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 009A4A move.b #0x96,(0x7,a6) */
	{ { uint32_t v = 0x0u; uint32_t da = ctx->a[6] + (uint32_t)8; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009A50 move.w #0x0,(0x8,a6) */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF80DAu; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 009A56 addq.w #0x1,(0xffff80da).l */
L_009A5C:
	hle_insn(ctx, 16); return; /* 009A5C rts */
}
/* sub_009A02 */
static void u_009A02(hle_ctx *ctx)
{
	hle_insn(ctx, 18); ObjAlloc(ctx); /* 009A02 jsr (ObjAlloc,pc) */
	{ { cc_logic16(ctx, (ctx->d[2] & 0xFFFF)); } } hle_insn(ctx, 4); /* 009A06 tst.w d2 */
	if (c_n(ctx)) { hle_insn(ctx, 10); goto L_009A5C; } hle_insn(ctx, 12); /* 009A08 bmi.w loc_009A5C */
	hle_insn(ctx, 18); u_009A5E(ctx); /* 009A0C jsr (sub_009A5E,pc) */
	{ { uint32_t v = 0x12E86u; ctx->a[5] = v; } } hle_insn(ctx, 12); /* 009A10 movea.l #0x12e86,a5 */
	{ { uint32_t sa = ctx->a[5]; ctx->a[5] += 2; uint32_t v = ld16(sa); uint32_t da = ctx->a[6] + (uint32_t)26; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009A16 move.w (a5)+,(0x1a,a6) */
	{ { uint32_t v = ctx->a[5]; uint32_t da = ctx->a[6] + (uint32_t)22; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 16); /* 009A1A move.l a5,(0x16,a6) */
	{ { uint32_t v = 0x100u; uint32_t da = ctx->a[6] + (uint32_t)20; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009A1E move.w #0x100,(0x14,a6) */
	{ { uint32_t v = 0x9u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009A24 move.w #0x9,d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x8000u) ? 0 : CC_Z); ctx->d[0] |= 0x8000u; } hle_insn(ctx, 10); /* 009A28 bset.b #0xf,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009A2C move.w d0,(a6) */
	{ { uint32_t v = 0x12Cu; uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009A2E move.w #0x12c,(0x24,a6) */
	{ { uint32_t v = 0xC442u; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 009A34 move.l #0xc442,(0x20,a6) */
	{ { uint32_t v = 0x6042u; uint32_t da = ctx->a[6] + (uint32_t)28; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 009A3C move.l #0x6042,(0x1c,a6) */
	{ { uint32_t v = 0x10u; uint32_t da = ctx->a[6] + (uint32_t)6; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 009A44 move.b #0x10,(0x6,a6) */
	{ { uint32_t v = 0x96u; uint32_t da = ctx->a[6] + (uint32_t)7; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 009A4A move.b #0x96,(0x7,a6) */
	{ { uint32_t v = 0x0u; uint32_t da = ctx->a[6] + (uint32_t)8; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009A50 move.w #0x0,(0x8,a6) */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF80DAu; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 009A56 addq.w #0x1,(0xffff80da).l */
L_009A5C:
	hle_insn(ctx, 16); return; /* 009A5C rts */
}
/* sub_009A5E */
static void u_009A5E(hle_ctx *ctx)
{
	hle_insn(ctx, 18); Random(ctx); /* 009A5E jsr (Random,pc) */
	{ { uint32_t sv = 0x3Fu; uint32_t r = ((ctx->d[0] & 0xFFFF) & sv) & 0xFFFFu; set_w(&ctx->d[0], r); cc_logic16(ctx, r); } } hle_insn(ctx, 8); /* 009A62 andi.w #0x3f,d0 */
	{ { uint32_t sv = 0x38u; uint32_t r = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[0], r); } } hle_insn(ctx, 8); /* 009A66 addi.w #0x38,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); set_w(&ctx->d[4], v); cc_logic16(ctx, v); } } hle_insn(ctx, 4); /* 009A6A move.w d0,d4 */
	hle_insn(ctx, 18); Random(ctx); /* 009A6C jsr (Random,pc) */
	{ { uint32_t sv = 0x7Fu; uint32_t r = ((ctx->d[0] & 0xFFFF) & sv) & 0xFFFFu; set_w(&ctx->d[0], r); cc_logic16(ctx, r); } } hle_insn(ctx, 8); /* 009A70 andi.w #0x7f,d0 */
	{ { uint32_t sv = 0x60u; uint32_t r = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[0], r); } } hle_insn(ctx, 8); /* 009A74 addi.w #0x60,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } } hle_insn(ctx, 4); /* 009A78 move.w d0,d3 */
	{ { uint32_t v = (ctx->d[5] & 0xFFFF); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 4); /* 009A7A move.w d5,d0 */
	{ { uint32_t v = 0x1u; set_b(&ctx->d[5], v); cc_logic8(ctx, v); } } hle_insn(ctx, 8); /* 009A7C move.b #0x1,d5 */
	{ { uint32_t sv = (ctx->d[0] & 0xFFFF); uint32_t r = alu(ctx, OP_SUB, (ctx->d[3] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[3], r); } } hle_insn(ctx, 4); /* 009A80 sub.w d0,d3 */
	if (!c_n(ctx)) { hle_insn(ctx, 10); goto L_009A88; } hle_insn(ctx, 8); /* 009A82 bpl.s loc_009A88 */
	{ { uint32_t r = alu(ctx, OP_SUB, 0, (ctx->d[5] & 0xFF), 8, 1); set_b(&ctx->d[5], r); } } hle_insn(ctx, 4); /* 009A84 neg.b d5 */
	{ { uint32_t r = alu(ctx, OP_SUB, 0, (ctx->d[3] & 0xFFFF), 16, 1); set_w(&ctx->d[3], r); } } hle_insn(ctx, 4); /* 009A86 neg.w d3 */
L_009A88:
	{ { uint32_t sv = 0xFFu; (void)alu(ctx, OP_SUB, (ctx->d[3] & 0xFFFF), sv, 16, 0); } } hle_insn(ctx, 8); /* 009A88 cmpi.w #0xff,d3 */
	if (c_le(ctx)) { hle_insn(ctx, 10); goto L_009A92; } hle_insn(ctx, 8); /* 009A8C ble.s loc_009A92 */
	{ { uint32_t v = 0xFFu; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009A8E move.w #0xff,d3 */
L_009A92:
	{ { uint32_t v = (ctx->d[5] & 0xFF); uint32_t da = ctx->a[6] + (uint32_t)14; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 12); /* 009A92 move.b d5,(0xe,a6) */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 009A96 move.w d0,(0x2,a6) */
	{ { uint32_t v = (ctx->d[3] & 0xFF); uint32_t da = ctx->a[6] + (uint32_t)10; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 12); /* 009A9A move.b d3,(0xa,a6) */
	{ { uint32_t da = ctx->a[6] + (uint32_t)11; st8(da, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } } hle_insn(ctx, 16); /* 009A9E clr.b (0xb,a6) */
	{ { uint32_t v = 0x1u; set_b(&ctx->d[5], v); cc_logic8(ctx, v); } } hle_insn(ctx, 8); /* 009AA2 move.b #0x1,d5 */
	{ { uint32_t v = (ctx->d[4] & 0xFFFF); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } } hle_insn(ctx, 4); /* 009AA6 move.w d4,d3 */
	{ { uint32_t sv = (ctx->d[1] & 0xFFFF); uint32_t r = alu(ctx, OP_SUB, (ctx->d[4] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[4], r); } } hle_insn(ctx, 4); /* 009AA8 sub.w d1,d4 */
	if (!c_n(ctx)) { hle_insn(ctx, 10); goto L_009AB0; } hle_insn(ctx, 8); /* 009AAA bpl.s loc_009AB0 */
	{ { uint32_t r = alu(ctx, OP_SUB, 0, (ctx->d[5] & 0xFF), 8, 1); set_b(&ctx->d[5], r); } } hle_insn(ctx, 4); /* 009AAC neg.b d5 */
	{ { uint32_t r = alu(ctx, OP_SUB, 0, (ctx->d[4] & 0xFFFF), 16, 1); set_w(&ctx->d[4], r); } } hle_insn(ctx, 4); /* 009AAE neg.w d4 */
L_009AB0:
	{ { uint32_t sv = 0xFFu; (void)alu(ctx, OP_SUB, (ctx->d[4] & 0xFFFF), sv, 16, 0); } } hle_insn(ctx, 8); /* 009AB0 cmpi.w #0xff,d4 */
	if (c_le(ctx)) { hle_insn(ctx, 10); goto L_009ABA; } hle_insn(ctx, 8); /* 009AB4 ble.s loc_009ABA */
	{ { uint32_t v = 0xFFu; set_w(&ctx->d[4], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009AB6 move.w #0xff,d4 */
L_009ABA:
	{ { uint32_t v = (ctx->d[5] & 0xFF); uint32_t da = ctx->a[6] + (uint32_t)15; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 12); /* 009ABA move.b d5,(0xf,a6) */
	{ { uint32_t v = (ctx->d[1] & 0xFFFF); uint32_t da = ctx->a[6] + (uint32_t)4; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 009ABE move.w d1,(0x4,a6) */
	{ { uint32_t v = (ctx->d[4] & 0xFF); uint32_t da = ctx->a[6] + (uint32_t)12; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 12); /* 009AC2 move.b d4,(0xc,a6) */
	{ { uint32_t da = ctx->a[6] + (uint32_t)13; st8(da, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } } hle_insn(ctx, 16); /* 009AC6 clr.b (0xd,a6) */
	hle_insn(ctx, 16); return; /* 009ACA rts */
}
/* sub_009ACC */
HLE_FUNC_PURE(0x009ACC, ScrQueueGfx0F, 4000)
{
	hle_insn(ctx, 18); u_009866(ctx); /* 009ACC jsr (sub_009866,pc) */
	hle_insn(ctx, 18); u_00998A(ctx); /* 009AD0 jsr (sub_00998A,pc) */
	{ ctx->a[1] = 0x000BCBBCu; } hle_insn(ctx, 12); /* 009AD4 lea.l (dat_0BCBBC).l,a1 */
	{ { uint32_t sa = 0xFFFF9E36u; uint32_t v = ld16(sa); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009ADA move.w (0xffff9e36).l,d1 */
	{ sh_w(ctx, 1, 'l', 5); hle_insn(ctx, 16); } /* 009AE0 asl.w #0x5,d1 */
	{ { uint32_t v = 0xDC0u; set_w(&ctx->d[2], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009AE2 move.w #0xdc0,d2 */
	hle_insn(ctx, 10); goto L_009B16; /* 009AE6 bra.s sub_009B16 */
L_009B16:
	{ sh_w(ctx, 2, 'a', 1); hle_insn(ctx, 8); } /* 009B16 asr.w #0x1,d2 */
	{ { uint32_t sa = 0xFFFF80BCu; uint32_t v = ld32(sa); ctx->a[5] = v; } } hle_insn(ctx, 20); /* 009B18 movea.l (0xffff80bc).l,a5 */
	{ { uint32_t v = 0x1u; uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 009B1E move.w #0x1,(a5)+ */
	{ { uint32_t v = (ctx->d[2] & 0xFFFF); uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009B22 move.w d2,(a5)+ */
	{ { uint32_t v = ctx->a[1]; uint32_t da = ctx->a[5]; ctx->a[5] += 4; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 12); /* 009B24 move.l a1,(a5)+ */
	{ { uint32_t v = (ctx->d[1] & 0xFFFF); uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009B26 move.w d1,(a5)+ */
	{ { uint32_t v = ctx->a[5]; uint32_t da = 0xFFFF80BCu; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 20); /* 009B28 move.l a5,(0xffff80bc).l */
	hle_insn(ctx, 16); return; /* 009B2E rts */
}
/* sub_009AE8 */
HLE_FUNC_PURE(0x009AE8, ScrQueueGfx10, 4000)
{
	{ ctx->a[1] = 0x000BD97Cu; } hle_insn(ctx, 12); /* 009AE8 lea.l (dat_0BD97C).l,a1 */
	{ { uint32_t sa = 0xFFFF9E36u; uint32_t v = ld16(sa); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009AEE move.w (0xffff9e36).l,d1 */
	{ { uint32_t sv = 0x6Eu; uint32_t r = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[1], r); } } hle_insn(ctx, 8); /* 009AF4 addi.w #0x6e,d1 */
	{ sh_w(ctx, 1, 'l', 5); hle_insn(ctx, 16); } /* 009AF8 asl.w #0x5,d1 */
	{ { uint32_t v = 0xDC0u; set_w(&ctx->d[2], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009AFA move.w #0xdc0,d2 */
	hle_insn(ctx, 10); goto L_009B16; /* 009AFE bra.s sub_009B16 */
L_009B16:
	{ sh_w(ctx, 2, 'a', 1); hle_insn(ctx, 8); } /* 009B16 asr.w #0x1,d2 */
	{ { uint32_t sa = 0xFFFF80BCu; uint32_t v = ld32(sa); ctx->a[5] = v; } } hle_insn(ctx, 20); /* 009B18 movea.l (0xffff80bc).l,a5 */
	{ { uint32_t v = 0x1u; uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 009B1E move.w #0x1,(a5)+ */
	{ { uint32_t v = (ctx->d[2] & 0xFFFF); uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009B22 move.w d2,(a5)+ */
	{ { uint32_t v = ctx->a[1]; uint32_t da = ctx->a[5]; ctx->a[5] += 4; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 12); /* 009B24 move.l a1,(a5)+ */
	{ { uint32_t v = (ctx->d[1] & 0xFFFF); uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009B26 move.w d1,(a5)+ */
	{ { uint32_t v = ctx->a[5]; uint32_t da = 0xFFFF80BCu; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 20); /* 009B28 move.l a5,(0xffff80bc).l */
	hle_insn(ctx, 16); return; /* 009B2E rts */
}
/* sub_009B00 */
HLE_FUNC_PURE(0x009B00, ScrQueueGfx11, 4000)
{
	{ ctx->a[1] = 0x000BE73Cu; } hle_insn(ctx, 12); /* 009B00 lea.l (dat_0BE73C).l,a1 */
	{ { uint32_t sa = 0xFFFF9E36u; uint32_t v = ld16(sa); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009B06 move.w (0xffff9e36).l,d1 */
	{ { uint32_t sv = 0xDCu; uint32_t r = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[1], r); } } hle_insn(ctx, 8); /* 009B0C addi.w #0xdc,d1 */
	{ { uint32_t v = 0x1260u; set_w(&ctx->d[2], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009B10 move.w #0x1260,d2 */
	{ sh_w(ctx, 1, 'l', 5); hle_insn(ctx, 16); } /* 009B14 asl.w #0x5,d1 */
	{ sh_w(ctx, 2, 'a', 1); hle_insn(ctx, 8); } /* 009B16 asr.w #0x1,d2 */
	{ { uint32_t sa = 0xFFFF80BCu; uint32_t v = ld32(sa); ctx->a[5] = v; } } hle_insn(ctx, 20); /* 009B18 movea.l (0xffff80bc).l,a5 */
	{ { uint32_t v = 0x1u; uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 009B1E move.w #0x1,(a5)+ */
	{ { uint32_t v = (ctx->d[2] & 0xFFFF); uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009B22 move.w d2,(a5)+ */
	{ { uint32_t v = ctx->a[1]; uint32_t da = ctx->a[5]; ctx->a[5] += 4; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 12); /* 009B24 move.l a1,(a5)+ */
	{ { uint32_t v = (ctx->d[1] & 0xFFFF); uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009B26 move.w d1,(a5)+ */
	{ { uint32_t v = ctx->a[5]; uint32_t da = 0xFFFF80BCu; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 20); /* 009B28 move.l a5,(0xffff80bc).l */
	hle_insn(ctx, 16); return; /* 009B2E rts */
}
/* sub_009B30 */
HLE_FUNC_PURE(0x009B30, ScrSpawnT0e, 4000)
{
	hle_insn(ctx, 18); ObjAlloc(ctx); /* 009B30 jsr (ObjAlloc,pc) */
	{ { uint32_t v = 0xEu; uint32_t da = ctx->a[6]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 009B34 move.w #0xe,(a6) */
	{ { uint32_t da = ctx->a[6]; uint8_t v = ld8(da); cc_set(ctx, CC_Z, (v & 0x80) ? 0 : CC_Z); st8(da, v & 0x7F); } } hle_insn(ctx, 16); /* 009B38 bclr.b #0x7,(a6) */
	{ { uint32_t v = 0x10u; uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009B3C move.w #0x10,(0x24,a6) */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009B42 move.w (a0)+,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6] + (uint32_t)10; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 009B44 move.w d0,(0xa,a6) */
	{ sh_w(ctx, 0, 'l', 2); hle_insn(ctx, 10); } /* 009B48 asl.w #0x2,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 009B4A move.w d0,(0x2,a6) */
	{ { uint32_t v = 0x58u; uint32_t da = ctx->a[6] + (uint32_t)4; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009B4E move.w #0x58,(0x4,a6) */
	{ { uint32_t v = 0xC0AAu; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 009B54 move.l #0xc0aa,(0x20,a6) */
	{ { uint32_t v = 0x5CEEu; uint32_t da = ctx->a[6] + (uint32_t)28; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 009B5C move.l #0x5cee,(0x1c,a6) */
	{ { uint32_t v = 0x5u; uint32_t da = ctx->a[6] + (uint32_t)8; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009B64 move.w #0x5,(0x8,a6) */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF80DAu; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 009B6A addq.w #0x1,(0xffff80da).l */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF9E08u; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 009B70 addq.w #0x1,(0xffff9e08).l */
	hle_insn(ctx, 16); return; /* 009B76 rts */
}
/* sub_009B78 */
HLE_FUNC_PURE(0x009B78, ScrSpawnT0fa, 4000)
{
	hle_insn(ctx, 18); ObjAlloc(ctx); /* 009B78 jsr (ObjAlloc,pc) */
	{ { uint32_t v = 0xFu; uint32_t da = ctx->a[6]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 009B7C move.w #0xf,(a6) */
	{ { uint32_t v = 0x1u; uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009B80 move.w #0x1,(0x24,a6) */
	{ { uint32_t v = 0x0u; uint32_t da = ctx->a[6] + (uint32_t)4; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009B86 move.w #0x0,(0x4,a6) */
	{ { uint32_t v = 0x19u; uint32_t da = ctx->a[6] + (uint32_t)6; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 009B8C move.b #0x19,(0x6,a6) */
	{ { uint32_t v = 0xCu; uint32_t da = ctx->a[6] + (uint32_t)7; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 009B92 move.b #0xc,(0x7,a6) */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009B98 move.w (a0)+,(0x2,a6) */
	{ { uint32_t v = 0xC802u; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 009B9C move.l #0xc802,(0x20,a6) */
	{ { uint32_t v = 0x597Cu; uint32_t da = ctx->a[6] + (uint32_t)28; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 009BA4 move.l #0x597c,(0x1c,a6) */
	{ { uint32_t v = 0x0u; uint32_t da = ctx->a[6] + (uint32_t)8; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009BAC move.w #0x0,(0x8,a6) */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); uint32_t da = ctx->a[6] + (uint32_t)42; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009BB2 move.w (a0)+,(0x2a,a6) */
	if (c_z(ctx)) { hle_insn(ctx, 10); goto L_009BC0; } hle_insn(ctx, 8); /* 009BB6 beq.s loc_009BC0 */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF80DAu; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 009BB8 addq.w #0x1,(0xffff80da).l */
	hle_insn(ctx, 16); return; /* 009BBE rts */
L_009BC0:
	{ { uint32_t v = 0x14u; uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009BC0 move.w #0x14,(0x24,a6) */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); uint32_t da = ctx->a[6] + (uint32_t)42; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009BC6 move.w (a0)+,(0x2a,a6) */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF80DAu; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 009BCA addq.w #0x1,(0xffff80da).l */
	hle_insn(ctx, 16); return; /* 009BD0 rts */
}
/* sub_009BD2 */
HLE_FUNC_PURE(0x009BD2, ScrSpawnT0fb, 4000)
{
	hle_insn(ctx, 18); ObjAlloc(ctx); /* 009BD2 jsr (ObjAlloc,pc) */
	{ { uint32_t v = 0xFu; uint32_t da = ctx->a[6]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 009BD6 move.w #0xf,(a6) */
	{ { uint32_t v = 0x1u; uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009BDA move.w #0x1,(0x24,a6) */
	{ { uint32_t v = 0xCFu; uint32_t da = ctx->a[6] + (uint32_t)4; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009BE0 move.w #0xcf,(0x4,a6) */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009BE6 move.w (a0)+,(0x2,a6) */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); uint32_t da = ctx->a[6] + (uint32_t)42; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009BEA move.w (a0)+,(0x2a,a6) */
	{ { uint32_t v = 0x19u; uint32_t da = ctx->a[6] + (uint32_t)6; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 009BEE move.b #0x19,(0x6,a6) */
	{ { uint32_t v = 0xCu; uint32_t da = ctx->a[6] + (uint32_t)7; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 009BF4 move.b #0xc,(0x7,a6) */
	{ { uint32_t v = 0xC846u; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 009BFA move.l #0xc846,(0x20,a6) */
	{ { uint32_t v = 0x597Cu; uint32_t da = ctx->a[6] + (uint32_t)28; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 009C02 move.l #0x597c,(0x1c,a6) */
	{ { uint32_t v = 0x0u; uint32_t da = ctx->a[6] + (uint32_t)8; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009C0A move.w #0x0,(0x8,a6) */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF80DAu; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 009C10 addq.w #0x1,(0xffff80da).l */
	hle_insn(ctx, 16); return; /* 009C16 rts */
}
/* copy_tiles_048ca6 */
HLE_FUNC_PURE(0x009C4A, ScrCopyTiles048CA6, 4000)
{
	goto L_009C4A;
L_009B14:
	{ sh_w(ctx, 1, 'l', 5); hle_insn(ctx, 16); } /* 009B14 asl.w #0x5,d1 */
	{ sh_w(ctx, 2, 'a', 1); hle_insn(ctx, 8); } /* 009B16 asr.w #0x1,d2 */
	{ { uint32_t sa = 0xFFFF80BCu; uint32_t v = ld32(sa); ctx->a[5] = v; } } hle_insn(ctx, 20); /* 009B18 movea.l (0xffff80bc).l,a5 */
	{ { uint32_t v = 0x1u; uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 009B1E move.w #0x1,(a5)+ */
	{ { uint32_t v = (ctx->d[2] & 0xFFFF); uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009B22 move.w d2,(a5)+ */
	{ { uint32_t v = ctx->a[1]; uint32_t da = ctx->a[5]; ctx->a[5] += 4; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 12); /* 009B24 move.l a1,(a5)+ */
	{ { uint32_t v = (ctx->d[1] & 0xFFFF); uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009B26 move.w d1,(a5)+ */
	{ { uint32_t v = ctx->a[5]; uint32_t da = 0xFFFF80BCu; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 20); /* 009B28 move.l a5,(0xffff80bc).l */
	hle_insn(ctx, 16); return; /* 009B2E rts */
L_009C4A:
	{ ctx->a[1] = 0x00048CA6u; } hle_insn(ctx, 12); /* 009C4A lea.l (dat_048CA6).l,a1 */
	{ { uint32_t sa = 0xFFFF9E44u; uint32_t v = ld16(sa); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009C50 move.w (0xffff9e44).l,d1 */
	{ { uint32_t sv = 0xC7u; uint32_t r = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[1], r); } } hle_insn(ctx, 8); /* 009C56 addi.w #0xc7,d1 */
	{ { uint32_t v = 0x300u; set_w(&ctx->d[2], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009C5A move.w #0x300,d2 */
	hle_insn(ctx, 10); goto L_009B14; /* 009C5E bra.w sub_009B14 */
}
/* sub_009C62 */
HLE_FUNC_PURE(0x009C62, ScrSpawnT15b, 4000)
{
	hle_insn(ctx, 18); ObjAlloc(ctx); /* 009C62 jsr (ObjAlloc,pc) */
	{ { uint32_t v = 0x15u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009C66 move.w #0x15,d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x8000u) ? 0 : CC_Z); ctx->d[0] |= 0x8000u; } hle_insn(ctx, 10); /* 009C6A bset.b #0xf,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009C6E move.w d0,(a6) */
	{ { uint32_t v = 0xA0u; uint32_t da = ctx->a[6] + (uint32_t)4; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009C70 move.w #0xa0,(0x4,a6) */
	{ { uint32_t v = 0x12u; uint32_t da = ctx->a[6] + (uint32_t)6; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 009C76 move.b #0x12,(0x6,a6) */
	{ { uint32_t v = 0x50u; uint32_t da = ctx->a[6] + (uint32_t)7; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 009C7C move.b #0x50,(0x7,a6) */
	{ { uint32_t v = 0x168u; uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009C82 move.w #0x168,(0x2,a6) */
	{ { uint32_t v = 0xCABAu; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 009C88 move.l #0xcaba,(0x20,a6) */
	{ { uint32_t v = 0x60F4u; uint32_t da = ctx->a[6] + (uint32_t)28; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 009C90 move.l #0x60f4,(0x1c,a6) */
	{ { uint32_t v = 0x28u; uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009C98 move.w #0x28,(0x24,a6) */
	{ { uint32_t v = 0x28u; uint32_t da = ctx->a[6] + (uint32_t)8; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009C9E move.w #0x28,(0x8,a6) */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF80DAu; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 009CA4 addq.w #0x1,(0xffff80da).l */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF9E0Au; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 009CAA addq.w #0x1,(0xffff9e0a).l */
	hle_insn(ctx, 16); return; /* 009CB0 rts */
}
/* sub_009CB2 */
HLE_FUNC_PURE(0x009CB2, ScrLoadTiles, 4000)
{
	goto L_009CB2;
L_009B14:
	{ sh_w(ctx, 1, 'l', 5); hle_insn(ctx, 16); } /* 009B14 asl.w #0x5,d1 */
L_009B16:
	{ sh_w(ctx, 2, 'a', 1); hle_insn(ctx, 8); } /* 009B16 asr.w #0x1,d2 */
	{ { uint32_t sa = 0xFFFF80BCu; uint32_t v = ld32(sa); ctx->a[5] = v; } } hle_insn(ctx, 20); /* 009B18 movea.l (0xffff80bc).l,a5 */
	{ { uint32_t v = 0x1u; uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 009B1E move.w #0x1,(a5)+ */
	{ { uint32_t v = (ctx->d[2] & 0xFFFF); uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009B22 move.w d2,(a5)+ */
	{ { uint32_t v = ctx->a[1]; uint32_t da = ctx->a[5]; ctx->a[5] += 4; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 12); /* 009B24 move.l a1,(a5)+ */
	{ { uint32_t v = (ctx->d[1] & 0xFFFF); uint32_t da = ctx->a[5]; ctx->a[5] += 2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009B26 move.w d1,(a5)+ */
	{ { uint32_t v = ctx->a[5]; uint32_t da = 0xFFFF80BCu; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 20); /* 009B28 move.l a5,(0xffff80bc).l */
	hle_insn(ctx, 16); return; /* 009B2E rts */
L_009CB2:
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009CB2 move.w (a0)+,d0 */
	if (c_z(ctx)) { hle_insn(ctx, 10); goto L_009CE2; } hle_insn(ctx, 8); /* 009CB4 beq.s loc_009CE2 */
	if (c_n(ctx)) { hle_insn(ctx, 10); goto L_009CC6; } hle_insn(ctx, 8); /* 009CB6 bmi.s loc_009CC6 */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 4; uint32_t v = ld32(sa); ctx->a[1] = v; } } hle_insn(ctx, 12); /* 009CB8 movea.l (a0)+,a1 */
	{ { uint32_t sa = ctx->a[1]; uint32_t v = ld16(sa); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009CBA move.w (a1),d1 */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 4; uint32_t v = ld32(sa); ctx->a[1] = v; } } hle_insn(ctx, 12); /* 009CBC movea.l (a0)+,a1 */
	{ { uint32_t sa = ctx->a[1]; ctx->a[1] += 2; uint32_t v = ld16(sa); set_w(&ctx->d[2], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009CBE move.w (a1)+,d2 */
	{ sh_w(ctx, 2, 'a', 1); hle_insn(ctx, 8); } /* 009CC0 asr.w #0x1,d2 */
	hle_insn(ctx, 10); goto L_009B14; /* 009CC2 bra.w sub_009B14 */
L_009CC6:
	{ { uint32_t sv = 0xFFFFu; (void)alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFF), sv, 16, 0); } } hle_insn(ctx, 8); /* 009CC6 cmpi.w #0xffff,d0 */
	if (!c_z(ctx)) { hle_insn(ctx, 10); goto L_009CEE; } hle_insn(ctx, 8); /* 009CCA bne.s loc_009CEE */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 4; uint32_t v = ld32(sa); ctx->a[1] = v; } } hle_insn(ctx, 12); /* 009CCC movea.l (a0)+,a1 */
	{ { uint32_t sa = ctx->a[1]; uint32_t v = ld16(sa); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009CCE move.w (a1),d1 */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 4; uint32_t v = ld32(sa); ctx->a[1] = v; } } hle_insn(ctx, 12); /* 009CD0 movea.l (a0)+,a1 */
	{ { uint32_t sa = ctx->a[1]; ctx->a[1] += 2; uint32_t v = ld16(sa); set_w(&ctx->d[2], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009CD2 move.w (a1)+,d2 */
	{ sh_w(ctx, 1, 'l', 5); hle_insn(ctx, 16); } /* 009CD4 asl.w #0x5,d1 */
	{ sh_w(ctx, 2, 'a', 1); hle_insn(ctx, 8); } /* 009CD6 asr.w #0x1,d2 */
	{ { uint32_t sv = (ctx->d[2] & 0xFFFF); uint32_t r = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[1], r); } } hle_insn(ctx, 4); /* 009CD8 add.w d2,d1 */
	{ { uint32_t sa = ctx->a[1] + (uint32_t)0 + (uint32_t)(int32_t)(int16_t)ctx->d[2]; ctx->a[1] = sa; } } hle_insn(ctx, 12); /* 009CDA lea.l (0x0,a1,d2.w),a1 */
	hle_insn(ctx, 10); goto L_009B16; /* 009CDE bra.w sub_009B16 */
L_009CE2:
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 4; uint32_t v = ld32(sa); ctx->a[1] = v; } } hle_insn(ctx, 12); /* 009CE2 movea.l (a0)+,a1 */
	{ { uint32_t sa = ctx->a[1]; uint32_t v = ld16(sa); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009CE4 move.w (a1),d1 */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 4; uint32_t v = ld32(sa); ctx->a[1] = v; } } hle_insn(ctx, 12); /* 009CE6 movea.l (a0)+,a1 */
	{ { uint32_t sa = ctx->a[1]; ctx->a[1] += 2; uint32_t v = ld16(sa); set_w(&ctx->d[2], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009CE8 move.w (a1)+,d2 */
	hle_insn(ctx, 10); goto L_009B14; /* 009CEA bra.w sub_009B14 */
L_009CEE:
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 4; uint32_t v = ld32(sa); ctx->a[1] = v; } } hle_insn(ctx, 12); /* 009CEE movea.l (a0)+,a1 */
	{ { uint32_t sa = ctx->a[1]; uint32_t v = ld16(sa); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009CF0 move.w (a1),d1 */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 4; uint32_t v = ld32(sa); ctx->a[1] = v; } } hle_insn(ctx, 12); /* 009CF2 movea.l (a0)+,a1 */
	{ { uint32_t sa = ctx->a[1]; ctx->a[1] += 2; uint32_t v = ld16(sa); set_w(&ctx->d[2], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009CF4 move.w (a1)+,d2 */
	{ sh_w(ctx, 2, 'a', 2); hle_insn(ctx, 10); } /* 009CF6 asr.w #0x2,d2 */
	{ sh_w(ctx, 1, 'l', 5); hle_insn(ctx, 16); } /* 009CF8 asl.w #0x5,d1 */
	{ { uint32_t sv = 0xFFFEu; (void)alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFF), sv, 16, 0); } } hle_insn(ctx, 8); /* 009CFA cmpi.w #0xfffe,d0 */
	if (c_z(ctx)) { hle_insn(ctx, 10); goto L_009D1E; } hle_insn(ctx, 8); /* 009CFE beq.s loc_009D1E */
	{ { uint32_t sv = (ctx->d[2] & 0xFFFF); uint32_t r = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[1], r); } } hle_insn(ctx, 4); /* 009D00 add.w d2,d1 */
	{ { uint32_t sa = ctx->a[1] + (uint32_t)0 + (uint32_t)(int32_t)(int16_t)ctx->d[2]; ctx->a[1] = sa; } } hle_insn(ctx, 12); /* 009D02 lea.l (0x0,a1,d2.w),a1 */
	{ { uint32_t sv = 0xFFFDu; (void)alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFF), sv, 16, 0); } } hle_insn(ctx, 8); /* 009D06 cmpi.w #0xfffd,d0 */
	if (c_z(ctx)) { hle_insn(ctx, 10); goto L_009D1E; } hle_insn(ctx, 8); /* 009D0A beq.s loc_009D1E */
	{ { uint32_t sv = (ctx->d[2] & 0xFFFF); uint32_t r = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[1], r); } } hle_insn(ctx, 4); /* 009D0C add.w d2,d1 */
	{ { uint32_t sa = ctx->a[1] + (uint32_t)0 + (uint32_t)(int32_t)(int16_t)ctx->d[2]; ctx->a[1] = sa; } } hle_insn(ctx, 12); /* 009D0E lea.l (0x0,a1,d2.w),a1 */
	{ { uint32_t sv = 0xFFFCu; (void)alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFF), sv, 16, 0); } } hle_insn(ctx, 8); /* 009D12 cmpi.w #0xfffc,d0 */
	if (c_z(ctx)) { hle_insn(ctx, 10); goto L_009D1E; } hle_insn(ctx, 8); /* 009D16 beq.s loc_009D1E */
	{ { uint32_t sa = ctx->a[1] + (uint32_t)0 + (uint32_t)(int32_t)(int16_t)ctx->d[2]; ctx->a[1] = sa; } } hle_insn(ctx, 12); /* 009D18 lea.l (0x0,a1,d2.w),a1 */
	{ { uint32_t sv = (ctx->d[2] & 0xFFFF); uint32_t r = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[1], r); } } hle_insn(ctx, 4); /* 009D1C add.w d2,d1 */
L_009D1E:
	hle_insn(ctx, 10); goto L_009B16; /* 009D1E bra.w sub_009B16 */
}
/* sub_009D22 */
HLE_FUNC_PURE(0x009D22, ScrSpawnT13a, 4000)
{
	{ { uint32_t v = 0x4B7Cu; uint32_t da = 0xFFFF9E98u; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 28); /* 009D22 move.l #0x4b7c,(0xffff9e98).l */
	{ ctx->a[4] = 0xFFFFAC3Cu; } hle_insn(ctx, 12); /* 009D2C lea.l (0xffffac3c).l,a4 */
	{ { uint32_t v = 0x140u; set_w(&ctx->d[2], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009D32 move.w #0x140,d2 */
	{ ctx->d[1] = 0x7u; cc_logic32(ctx, 0x7u); } hle_insn(ctx, 4); /* 009D36 moveq #0x7,d1 */
L_009D38:
	{ { uint32_t v = (ctx->d[2] & 0xFFFF); uint32_t da = ctx->a[4]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009D38 move.w d2,(a4) */
	{ { uint32_t sa = ctx->a[4] + (uint32_t)32; ctx->a[4] = sa; } } hle_insn(ctx, 8); /* 009D3A lea.l (0x20,a4),a4 */
	{ uint16_t c = (uint16_t)(ctx->d[1] - 1); set_w(&ctx->d[1], c); if (c != 0xFFFF) { hle_insn(ctx, 10); goto L_009D38; } hle_insn(ctx, 14); } /* 009D3E dbra d1,loc_009D38 */
	{ ctx->d[4] = 0x7u; cc_logic32(ctx, 0x7u); } hle_insn(ctx, 4); /* 009D42 moveq #0x7,d4 */
	{ ctx->a[4] = 0x009E02u; } hle_insn(ctx, 8); /* 009D44 lea.l (dat_009E02,pc),a4 */
L_009D48:
	hle_insn(ctx, 18); ObjAlloc(ctx); /* 009D48 jsr (ObjAlloc,pc) */
	{ { cc_logic16(ctx, (ctx->d[2] & 0xFFFF)); } } hle_insn(ctx, 4); /* 009D4C tst.w d2 */
	if (c_n(ctx)) { hle_insn(ctx, 10); goto L_009D9A; } hle_insn(ctx, 12); /* 009D4E bmi.w loc_009D9A */
	{ { uint32_t v = 0x14u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009D52 move.w #0x14,d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x8000u) ? 0 : CC_Z); ctx->d[0] |= 0x8000u; } hle_insn(ctx, 10); /* 009D56 bset.b #0xf,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009D5A move.w d0,(a6) */
	{ { uint32_t sa = ctx->a[4]; ctx->a[4] += 2; uint32_t v = ld16(sa); uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009D5C move.w (a4)+,(0x2,a6) */
	{ { uint32_t sa = ctx->a[4]; ctx->a[4] += 2; uint32_t v = ld16(sa); uint32_t da = ctx->a[6] + (uint32_t)4; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009D60 move.w (a4)+,(0x4,a6) */
	{ { uint32_t v = 0x14u; uint32_t da = ctx->a[6] + (uint32_t)6; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 009D64 move.b #0x14,(0x6,a6) */
	{ { uint32_t v = 0x10u; uint32_t da = ctx->a[6] + (uint32_t)7; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 009D6A move.b #0x10,(0x7,a6) */
	{ { uint32_t v = 0x5F56u; uint32_t da = ctx->a[6] + (uint32_t)28; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 009D70 move.l #0x5f56,(0x1c,a6) */
	{ { uint32_t v = 0xCA0Au; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 009D78 move.l #0xca0a,(0x20,a6) */
	{ { uint32_t v = 0xAu; uint32_t da = ctx->a[6] + (uint32_t)8; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009D80 move.w #0xa,(0x8,a6) */
	{ { uint32_t v = (ctx->d[4] & 0xFFFF); uint32_t da = ctx->a[6] + (uint32_t)42; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 009D86 move.w d4,(0x2a,a6) */
	{ { uint32_t v = 0xAu; uint32_t da = ctx->a[6] + (uint32_t)8; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009D8A move.w #0xa,(0x8,a6) */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF80DAu; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 009D90 addq.w #0x1,(0xffff80da).l */
	{ uint16_t c = (uint16_t)(ctx->d[4] - 1); set_w(&ctx->d[4], c); if (c != 0xFFFF) { hle_insn(ctx, 10); goto L_009D48; } hle_insn(ctx, 14); } /* 009D96 dbra d4,loc_009D48 */
L_009D9A:
	hle_insn(ctx, 18); ObjAlloc(ctx); /* 009D9A jsr (ObjAlloc,pc) */
	{ { cc_logic16(ctx, (ctx->d[2] & 0xFFFF)); } } hle_insn(ctx, 4); /* 009D9E tst.w d2 */
	if (c_n(ctx)) { hle_insn(ctx, 10); goto L_009DEE; } hle_insn(ctx, 12); /* 009DA0 bmi.w loc_009DEE */
	{ { uint32_t v = 0x13u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009DA4 move.w #0x13,d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x8000u) ? 0 : CC_Z); ctx->d[0] |= 0x8000u; } hle_insn(ctx, 10); /* 009DA8 bset.b #0xf,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009DAC move.w d0,(a6) */
	{ { uint32_t v = 0x198u; uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009DAE move.w #0x198,(0x2,a6) */
	{ { uint32_t v = 0xB0u; uint32_t da = ctx->a[6] + (uint32_t)4; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009DB4 move.w #0xb0,(0x4,a6) */
	{ { uint32_t v = 0x14u; uint32_t da = ctx->a[6] + (uint32_t)6; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 009DBA move.b #0x14,(0x6,a6) */
	{ { uint32_t v = 0x10u; uint32_t da = ctx->a[6] + (uint32_t)7; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 009DC0 move.b #0x10,(0x7,a6) */
	{ { uint32_t v = 0x96u; uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009DC6 move.w #0x96,(0x24,a6) */
	{ { uint32_t v = 0x5F04u; uint32_t da = ctx->a[6] + (uint32_t)28; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 009DCC move.l #0x5f04,(0x1c,a6) */
	{ { uint32_t v = 0xC9A2u; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 009DD4 move.l #0xc9a2,(0x20,a6) */
	{ { uint32_t v = 0x14u; uint32_t da = ctx->a[6] + (uint32_t)8; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009DDC move.w #0x14,(0x8,a6) */
	{ { uint32_t v = 0xE810u; uint32_t da = ctx->a[6] + (uint32_t)10; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009DE2 move.w #0xe810,(0xa,a6) */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF80DAu; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 009DE8 addq.w #0x1,(0xffff80da).l */
L_009DEE:
	{ { uint32_t v = ctx->a[0]; ctx->a[7] -= 4; uint32_t da = ctx->a[7]; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 12); /* 009DEE move.l a0,-(sp) */
	{ { uint32_t v = 0xE780u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009DF0 move.w #0xe780,d1 */
	{ ctx->a[0] = 0x0004A7E8u; } hle_insn(ctx, 12); /* 009DF4 lea.l (dat_04A7E8).l,a0 */
	{ ctx->d[2] = 0x18u; cc_logic32(ctx, 0x18u); } hle_insn(ctx, 4); /* 009DFA moveq #0x18,d2 */
	{ ctx->d[3] = 0x8u; cc_logic32(ctx, 0x8u); } hle_insn(ctx, 4); /* 009DFC moveq #0x8,d3 */
	hle_insn(ctx, 10); goto L_009E86; /* 009DFE bra.w sub_009E86 */
L_009E86:
	{ { uint32_t v = 0xA000u; ctx->d[6] = v; cc_logic32(ctx, v); } } hle_insn(ctx, 12); /* 009E86 move.l #0xa000,d6 */
	{ { uint32_t sa = 0xFFFF9E46u; uint32_t v = ld16(sa); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009E8C move.w (0xffff9e46).l,d7 */
	hle_insn(ctx, 20); u_012234(ctx); /* 009E92 jsr (VdpQueueTileRect).l */
	{ { uint32_t sa = ctx->a[7]; ctx->a[7] += 4; uint32_t v = ld32(sa); ctx->a[0] = v; } } hle_insn(ctx, 12); /* 009E98 movea.l (sp)+,a0 */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF9E0Cu; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 009E9A addq.w #0x1,(0xffff9e0c).l */
	hle_insn(ctx, 16); return; /* 009EA0 rts */
}
/* sub_009E22 */
HLE_FUNC_PURE(0x009E22, ScrSpawnT13b, 4000)
{
	hle_insn(ctx, 18); ObjAlloc(ctx); /* 009E22 jsr (ObjAlloc,pc) */
	{ { cc_logic16(ctx, (ctx->d[2] & 0xFFFF)); } } hle_insn(ctx, 4); /* 009E26 tst.w d2 */
	if (c_n(ctx)) { hle_insn(ctx, 10); goto L_009E76; } hle_insn(ctx, 12); /* 009E28 bmi.w loc_009E76 */
	{ { uint32_t v = 0x13u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009E2C move.w #0x13,d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x8000u) ? 0 : CC_Z); ctx->d[0] |= 0x8000u; } hle_insn(ctx, 10); /* 009E30 bset.b #0xf,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009E34 move.w d0,(a6) */
	{ { uint32_t v = 0x1A0u; uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009E36 move.w #0x1a0,(0x2,a6) */
	{ { uint32_t v = 0xB0u; uint32_t da = ctx->a[6] + (uint32_t)4; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009E3C move.w #0xb0,(0x4,a6) */
	{ { uint32_t v = 0x14u; uint32_t da = ctx->a[6] + (uint32_t)6; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 009E42 move.b #0x14,(0x6,a6) */
	{ { uint32_t v = 0x10u; uint32_t da = ctx->a[6] + (uint32_t)7; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 009E48 move.b #0x10,(0x7,a6) */
	{ { uint32_t v = 0x96u; uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009E4E move.w #0x96,(0x24,a6) */
	{ { uint32_t v = 0x5F04u; uint32_t da = ctx->a[6] + (uint32_t)28; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 009E54 move.l #0x5f04,(0x1c,a6) */
	{ { uint32_t v = 0xC9A2u; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 009E5C move.l #0xc9a2,(0x20,a6) */
	{ { uint32_t v = 0x14u; uint32_t da = ctx->a[6] + (uint32_t)8; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009E64 move.w #0x14,(0x8,a6) */
	{ { uint32_t v = 0xE832u; uint32_t da = ctx->a[6] + (uint32_t)10; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009E6A move.w #0xe832,(0xa,a6) */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF80DAu; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 009E70 addq.w #0x1,(0xffff80da).l */
L_009E76:
	{ { uint32_t v = ctx->a[0]; ctx->a[7] -= 4; uint32_t da = ctx->a[7]; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 12); /* 009E76 move.l a0,-(sp) */
	{ { uint32_t v = 0xE7B0u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009E78 move.w #0xe7b0,d1 */
	{ ctx->a[0] = 0x0004A818u; } hle_insn(ctx, 12); /* 009E7C lea.l (dat_04A818).l,a0 */
	{ ctx->d[2] = 0x10u; cc_logic32(ctx, 0x10u); } hle_insn(ctx, 4); /* 009E82 moveq #0x10,d2 */
	{ ctx->d[3] = 0x8u; cc_logic32(ctx, 0x8u); } hle_insn(ctx, 4); /* 009E84 moveq #0x8,d3 */
	{ { uint32_t v = 0xA000u; ctx->d[6] = v; cc_logic32(ctx, v); } } hle_insn(ctx, 12); /* 009E86 move.l #0xa000,d6 */
	{ { uint32_t sa = 0xFFFF9E46u; uint32_t v = ld16(sa); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009E8C move.w (0xffff9e46).l,d7 */
	hle_insn(ctx, 20); u_012234(ctx); /* 009E92 jsr (VdpQueueTileRect).l */
	{ { uint32_t sa = ctx->a[7]; ctx->a[7] += 4; uint32_t v = ld32(sa); ctx->a[0] = v; } } hle_insn(ctx, 12); /* 009E98 movea.l (sp)+,a0 */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF9E0Cu; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 009E9A addq.w #0x1,(0xffff9e0c).l */
	hle_insn(ctx, 16); return; /* 009EA0 rts */
}
/* clear_tiles_e780 */
HLE_FUNC_PURE(0x009EA2, ScrClearTilesE780, 4000)
{
	{ { uint32_t v = ctx->a[0]; ctx->a[7] -= 4; uint32_t da = ctx->a[7]; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 12); /* 009EA2 move.l a0,-(sp) */
	hle_insn(ctx, 20); u_012986(ctx); /* 009EA4 jsr (sub_012986).l */
	{ { uint32_t v = 0xE780u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009EAA move.w #0xe780,d1 */
	{ ctx->d[2] = 0x30u; cc_logic32(ctx, 0x30u); } hle_insn(ctx, 4); /* 009EAE moveq #0x30,d2 */
	hle_insn(ctx, 10); goto L_009EBA; /* 009EB0 bra.s sub_009EBA */
L_009EBA:
	{ { uint32_t v = 0x7u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009EBA move.w #0x7,d0 */
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
}
/* clear_tiles_e7b0 */
HLE_FUNC_PURE(0x009EB2, ScrClearTilesE7B0, 4000)
{
	{ { uint32_t v = ctx->a[0]; ctx->a[7] -= 4; uint32_t da = ctx->a[7]; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 12); /* 009EB2 move.l a0,-(sp) */
	{ { uint32_t v = 0xE7B0u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009EB4 move.w #0xe7b0,d1 */
	{ ctx->d[2] = 0x20u; cc_logic32(ctx, 0x20u); } hle_insn(ctx, 4); /* 009EB8 moveq #0x20,d2 */
	{ { uint32_t v = 0x7u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009EBA move.w #0x7,d0 */
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
/* sub_009F24 */
HLE_FUNC_PURE(0x009F24, ScrSpawnT17, 4000)
{
	{ { uint32_t da = 0xFFFF9EBCu; st8(da, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } } hle_insn(ctx, 20); /* 009F24 clr.b (0xffff9ebc).l */
	{ { uint32_t v = 0x140u; set_w(&ctx->d[2], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009F2A move.w #0x140,d2 */
	hle_insn(ctx, 18); u_009EF8(ctx); /* 009F2E jsr (sub_009EF8,pc) */
	hle_insn(ctx, 18); ObjAlloc(ctx); /* 009F32 jsr (ObjAlloc,pc) */
	{ { cc_logic16(ctx, (ctx->d[2] & 0xFFFF)); } } hle_insn(ctx, 4); /* 009F36 tst.w d2 */
	if (c_n(ctx)) { hle_insn(ctx, 10); goto L_00A0DA; } hle_insn(ctx, 12); /* 009F38 bmi.w loc_00A0DA */
	{ { uint32_t v = 0x1u; uint32_t da = 0xFFFF9E18u; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 20); /* 009F3C move.w #0x1,(0xffff9e18).l */
	{ { uint32_t v = 0x16u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009F44 move.w #0x16,d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x8000u) ? 0 : CC_Z); ctx->d[0] &= ~0x8000u; } hle_insn(ctx, 12); /* 009F48 bclr.b #0xf,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009F4C move.w d0,(a6) */
	{ { uint32_t v = 0x1A8u; uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009F4E move.w #0x1a8,(0x2,a6) */
	{ { uint32_t v = 0xA0u; uint32_t da = ctx->a[6] + (uint32_t)4; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009F54 move.w #0xa0,(0x4,a6) */
	{ { uint32_t v = 0x14u; uint32_t da = ctx->a[6] + (uint32_t)6; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 009F5A move.b #0x14,(0x6,a6) */
	{ { uint32_t v = 0x50u; uint32_t da = ctx->a[6] + (uint32_t)7; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 009F60 move.b #0x50,(0x7,a6) */
	{ { uint32_t v = 0x15Eu; uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009F66 move.w #0x15e,(0x24,a6) */
	{ { uint32_t v = 0x6126u; uint32_t da = ctx->a[6] + (uint32_t)28; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 009F6C move.l #0x6126,(0x1c,a6) */
	{ { uint32_t v = 0xCB34u; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 009F74 move.l #0xcb34,(0x20,a6) */
	{ { uint32_t v = 0x460u; uint32_t da = ctx->a[6] + (uint32_t)8; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009F7C move.w #0x460,(0x8,a6) */
	{ { uint32_t v = 0x28u; uint32_t da = ctx->a[6] + (uint32_t)42; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009F82 move.w #0x28,(0x2a,a6) */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF80DAu; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 009F88 addq.w #0x1,(0xffff80da).l */
	{ { uint32_t v = ctx->a[6]; uint32_t da = 0xFFFF9ECAu; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 20); /* 009F8E move.l a6,(0xffff9eca).l */
	hle_insn(ctx, 18); ObjAlloc(ctx); /* 009F94 jsr (ObjAlloc,pc) */
	{ { cc_logic16(ctx, (ctx->d[2] & 0xFFFF)); } } hle_insn(ctx, 4); /* 009F98 tst.w d2 */
	if (c_n(ctx)) { hle_insn(ctx, 10); goto L_00A0DA; } hle_insn(ctx, 12); /* 009F9A bmi.w loc_00A0DA */
	{ { uint32_t v = 0x17u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009F9E move.w #0x17,d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x8000u) ? 0 : CC_Z); ctx->d[0] &= ~0x8000u; } hle_insn(ctx, 12); /* 009FA2 bclr.b #0xf,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009FA6 move.w d0,(a6) */
	{ { uint32_t v = 0x190u; uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009FA8 move.w #0x190,(0x2,a6) */
	{ { uint32_t v = 0x60u; uint32_t da = ctx->a[6] + (uint32_t)4; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009FAE move.w #0x60,(0x4,a6) */
	{ { uint32_t v = 0x14u; uint32_t da = ctx->a[6] + (uint32_t)6; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 009FB4 move.b #0x14,(0x6,a6) */
	{ { uint32_t v = 0x50u; uint32_t da = ctx->a[6] + (uint32_t)7; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 009FBA move.b #0x50,(0x7,a6) */
	{ { uint32_t v = 0xC8u; uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009FC0 move.w #0xc8,(0x24,a6) */
	{ { uint32_t v = 0x6174u; uint32_t da = ctx->a[6] + (uint32_t)28; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 009FC6 move.l #0x6174,(0x1c,a6) */
	{ { uint32_t v = 0xCDFEu; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 009FCE move.l #0xcdfe,(0x20,a6) */
	{ { uint32_t v = 0x384u; uint32_t da = ctx->a[6] + (uint32_t)8; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009FD6 move.w #0x384,(0x8,a6) */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF80DAu; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 009FDC addq.w #0x1,(0xffff80da).l */
	hle_insn(ctx, 18); ObjAlloc(ctx); /* 009FE2 jsr (ObjAlloc,pc) */
	{ { cc_logic16(ctx, (ctx->d[2] & 0xFFFF)); } } hle_insn(ctx, 4); /* 009FE6 tst.w d2 */
	if (c_n(ctx)) { hle_insn(ctx, 10); goto L_00A0DA; } hle_insn(ctx, 12); /* 009FE8 bmi.w loc_00A0DA */
	{ { uint32_t v = 0x18u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009FEC move.w #0x18,d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x8000u) ? 0 : CC_Z); ctx->d[0] |= 0x8000u; } hle_insn(ctx, 10); /* 009FF0 bset.b #0xf,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 009FF4 move.w d0,(a6) */
	{ { uint32_t v = 0x190u; uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009FF6 move.w #0x190,(0x2,a6) */
	{ { uint32_t v = 0x40u; uint32_t da = ctx->a[6] + (uint32_t)4; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 009FFC move.w #0x40,(0x4,a6) */
	{ { uint32_t v = 0x14u; uint32_t da = ctx->a[6] + (uint32_t)6; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00A002 move.b #0x14,(0x6,a6) */
	{ { uint32_t v = 0x96u; uint32_t da = ctx->a[6] + (uint32_t)7; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00A008 move.b #0x96,(0x7,a6) */
	{ { uint32_t v = 0xC8u; uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A00E move.w #0xc8,(0x24,a6) */
	{ { uint32_t v = 0x61CAu; uint32_t da = ctx->a[6] + (uint32_t)28; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 00A014 move.l #0x61ca,(0x1c,a6) */
	{ { uint32_t v = 0xCE46u; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 00A01C move.l #0xce46,(0x20,a6) */
	{ { uint32_t v = 0x2EEu; uint32_t da = ctx->a[6] + (uint32_t)8; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A024 move.w #0x2ee,(0x8,a6) */
	{ { uint32_t v = 0x22Cu; uint32_t da = ctx->a[6] + (uint32_t)42; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A02A move.w #0x22c,(0x2a,a6) */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF80DAu; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 00A030 addq.w #0x1,(0xffff80da).l */
	hle_insn(ctx, 18); ObjAlloc(ctx); /* 00A036 jsr (ObjAlloc,pc) */
	{ { cc_logic16(ctx, (ctx->d[2] & 0xFFFF)); } } hle_insn(ctx, 4); /* 00A03A tst.w d2 */
	if (c_n(ctx)) { hle_insn(ctx, 10); goto L_00A0DA; } hle_insn(ctx, 12); /* 00A03C bmi.w loc_00A0DA */
	{ { uint32_t v = 0x19u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00A040 move.w #0x19,d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x8000u) ? 0 : CC_Z); ctx->d[0] |= 0x8000u; } hle_insn(ctx, 10); /* 00A044 bset.b #0xf,d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x4000u) ? 0 : CC_Z); ctx->d[0] &= ~0x4000u; } hle_insn(ctx, 12); /* 00A048 bclr.b #0xe,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00A04C move.w d0,(a6) */
	{ { uint32_t v = 0x152u; uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A04E move.w #0x152,(0x2,a6) */
	{ { uint32_t v = 0x48u; uint32_t da = ctx->a[6] + (uint32_t)4; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A054 move.w #0x48,(0x4,a6) */
	{ { uint32_t v = 0x14u; uint32_t da = ctx->a[6] + (uint32_t)6; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00A05A move.b #0x14,(0x6,a6) */
	{ { uint32_t v = 0x50u; uint32_t da = ctx->a[6] + (uint32_t)7; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00A060 move.b #0x50,(0x7,a6) */
	{ { uint32_t v = 0xC8u; uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A066 move.w #0xc8,(0x24,a6) */
	{ { uint32_t v = 0x6208u; uint32_t da = ctx->a[6] + (uint32_t)28; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 00A06C move.l #0x6208,(0x1c,a6) */
	{ { uint32_t v = 0xCF9Eu; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 00A074 move.l #0xcf9e,(0x20,a6) */
	{ { uint32_t v = 0x2EEu; uint32_t da = ctx->a[6] + (uint32_t)8; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A07C move.w #0x2ee,(0x8,a6) */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF80DAu; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 00A082 addq.w #0x1,(0xffff80da).l */
	hle_insn(ctx, 18); ObjAlloc(ctx); /* 00A088 jsr (ObjAlloc,pc) */
	{ { cc_logic16(ctx, (ctx->d[2] & 0xFFFF)); } } hle_insn(ctx, 4); /* 00A08C tst.w d2 */
	if (c_n(ctx)) { hle_insn(ctx, 10); goto L_00A0DA; } hle_insn(ctx, 12); /* 00A08E bmi.w loc_00A0DA */
	{ { uint32_t v = 0x19u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00A092 move.w #0x19,d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x8000u) ? 0 : CC_Z); ctx->d[0] |= 0x8000u; } hle_insn(ctx, 10); /* 00A096 bset.b #0xf,d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x4000u) ? 0 : CC_Z); ctx->d[0] |= 0x4000u; } hle_insn(ctx, 10); /* 00A09A bset.b #0xe,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00A09E move.w d0,(a6) */
	{ { uint32_t v = 0x1CEu; uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A0A0 move.w #0x1ce,(0x2,a6) */
	{ { uint32_t v = 0x48u; uint32_t da = ctx->a[6] + (uint32_t)4; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A0A6 move.w #0x48,(0x4,a6) */
	{ { uint32_t v = 0x14u; uint32_t da = ctx->a[6] + (uint32_t)6; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00A0AC move.b #0x14,(0x6,a6) */
	{ { uint32_t v = 0x50u; uint32_t da = ctx->a[6] + (uint32_t)7; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00A0B2 move.b #0x50,(0x7,a6) */
	{ { uint32_t v = 0x6Du; uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A0B8 move.w #0x6d,(0x24,a6) */
	{ { uint32_t v = 0x6208u; uint32_t da = ctx->a[6] + (uint32_t)28; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 00A0BE move.l #0x6208,(0x1c,a6) */
	{ { uint32_t v = 0xCF9Eu; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 00A0C6 move.l #0xcf9e,(0x20,a6) */
	{ { uint32_t v = 0x2EEu; uint32_t da = ctx->a[6] + (uint32_t)8; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A0CE move.w #0x2ee,(0x8,a6) */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF80DAu; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 00A0D4 addq.w #0x1,(0xffff80da).l */
L_00A0DA:
	{ { uint32_t v = ctx->a[0]; ctx->a[7] -= 4; uint32_t da = ctx->a[7]; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 12); /* 00A0DA move.l a0,-(sp) */
	{ { uint32_t v = 0xE380u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00A0DC move.w #0xe380,d1 */
	{ ctx->a[0] = 0x0004E26Au; } hle_insn(ctx, 12); /* 00A0E0 lea.l (dat_04E26A).l,a0 */
	{ ctx->d[2] = 0xDu; cc_logic32(ctx, 0xDu); } hle_insn(ctx, 4); /* 00A0E6 moveq #0xd,d2 */
	{ ctx->d[3] = 0xDu; cc_logic32(ctx, 0xDu); } hle_insn(ctx, 4); /* 00A0E8 moveq #0xd,d3 */
	{ { uint32_t v = 0x8000u; ctx->d[6] = v; cc_logic32(ctx, v); } } hle_insn(ctx, 12); /* 00A0EA move.l #0x8000,d6 */
	{ { uint32_t sa = 0xFFFF9E48u; uint32_t v = ld16(sa); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A0F0 move.w (0xffff9e48).l,d7 */
	hle_insn(ctx, 20); u_012234(ctx); /* 00A0F6 jsr (VdpQueueTileRect).l */
	{ { uint32_t sa = ctx->a[7]; ctx->a[7] += 4; uint32_t v = ld32(sa); ctx->a[0] = v; } } hle_insn(ctx, 12); /* 00A0FC movea.l (sp)+,a0 */
	{ { uint32_t v = 0x10u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00A0FE move.w #0x10,d0 */
	hle_insn(ctx, 20); u_0FBB6C(ctx); /* 00A102 jsr (sub_0FBB6C).l */
	hle_insn(ctx, 16); return; /* 00A108 rts */
}
/* draw_tiles_e39a */
HLE_FUNC_PURE(0x00A10A, ScrDrawTilesE39A, 4000)
{
	{ { uint32_t v = ctx->a[0]; ctx->a[7] -= 4; uint32_t da = ctx->a[7]; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 12); /* 00A10A move.l a0,-(sp) */
	{ { uint32_t v = 0xE39Au; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00A10C move.w #0xe39a,d1 */
	{ ctx->a[0] = 0x0004E284u; } hle_insn(ctx, 12); /* 00A110 lea.l (dat_04E284).l,a0 */
	{ ctx->d[2] = 0xDu; cc_logic32(ctx, 0xDu); } hle_insn(ctx, 4); /* 00A116 moveq #0xd,d2 */
	{ ctx->d[3] = 0xDu; cc_logic32(ctx, 0xDu); } hle_insn(ctx, 4); /* 00A118 moveq #0xd,d3 */
	{ { uint32_t v = 0x8000u; ctx->d[6] = v; cc_logic32(ctx, v); } } hle_insn(ctx, 12); /* 00A11A move.l #0x8000,d6 */
	{ { uint32_t sa = 0xFFFF9E48u; uint32_t v = ld16(sa); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A120 move.w (0xffff9e48).l,d7 */
	hle_insn(ctx, 20); u_012234(ctx); /* 00A126 jsr (VdpQueueTileRect).l */
	{ { uint32_t sa = ctx->a[7]; ctx->a[7] += 4; uint32_t v = ld32(sa); ctx->a[0] = v; } } hle_insn(ctx, 12); /* 00A12C movea.l (sp)+,a0 */
	hle_insn(ctx, 16); return; /* 00A12E rts */
}
/* sub_00A144 */
HLE_FUNC_PURE(0x00A144, ScrSpawnT1b, 4000)
{
	{ { uint32_t v = 0x140u; set_w(&ctx->d[2], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00A144 move.w #0x140,d2 */
	hle_insn(ctx, 18); u_009EF8(ctx); /* 00A148 jsr (sub_009EF8,pc) */
	hle_insn(ctx, 18); ObjAlloc(ctx); /* 00A14C jsr (ObjAlloc,pc) */
	{ { cc_logic16(ctx, (ctx->d[2] & 0xFFFF)); } } hle_insn(ctx, 4); /* 00A150 tst.w d2 */
	if (c_n(ctx)) { hle_insn(ctx, 10); goto L_00A1FA; } hle_insn(ctx, 12); /* 00A152 bmi.w loc_00A1FA */
	{ { uint32_t v = ctx->a[6]; uint32_t da = 0xFFFF9ECAu; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 20); /* 00A156 move.l a6,(0xffff9eca).l */
	{ { uint32_t v = 0x1Bu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00A15C move.w #0x1b,d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x8000u) ? 0 : CC_Z); ctx->d[0] |= 0x8000u; } hle_insn(ctx, 10); /* 00A160 bset.b #0xf,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00A164 move.w d0,(a6) */
	{ { uint32_t v = 0x1B8u; uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A166 move.w #0x1b8,(0x2,a6) */
	{ { uint32_t v = 0xB0u; uint32_t da = ctx->a[6] + (uint32_t)4; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A16C move.w #0xb0,(0x4,a6) */
	{ { uint32_t v = 0x14u; uint32_t da = ctx->a[6] + (uint32_t)6; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00A172 move.b #0x14,(0x6,a6) */
	{ { uint32_t v = 0x65u; uint32_t da = ctx->a[6] + (uint32_t)7; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00A178 move.b #0x65,(0x7,a6) */
	{ { uint32_t v = 0x15Eu; uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A17E move.w #0x15e,(0x24,a6) */
	{ { uint32_t v = 0x627Au; uint32_t da = ctx->a[6] + (uint32_t)28; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 00A184 move.l #0x627a,(0x1c,a6) */
	{ { uint32_t v = 0xD2ACu; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 00A18C move.l #0xd2ac,(0x20,a6) */
	{ { uint32_t v = 0x64u; uint32_t da = ctx->a[6] + (uint32_t)8; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A194 move.w #0x64,(0x8,a6) */
	{ { uint32_t v = 0x0u; uint32_t da = ctx->a[6] + (uint32_t)12; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00A19A move.b #0x0,(0xc,a6) */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF80DAu; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 00A1A0 addq.w #0x1,(0xffff80da).l */
	hle_insn(ctx, 18); u_00D30A(ctx); /* 00A1A6 jsr (TruckDamageInit,pc) */
	hle_insn(ctx, 18); ObjAlloc(ctx); /* 00A1AA jsr (ObjAlloc,pc) */
	{ { cc_logic16(ctx, (ctx->d[2] & 0xFFFF)); } } hle_insn(ctx, 4); /* 00A1AE tst.w d2 */
	if (c_n(ctx)) { hle_insn(ctx, 10); goto L_00A1FA; } hle_insn(ctx, 12); /* 00A1B0 bmi.w loc_00A1FA */
	{ { uint32_t v = 0x1Bu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00A1B4 move.w #0x1b,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00A1B8 move.w d0,(a6) */
	{ { uint32_t v = 0x40u; uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A1BA move.w #0x40,(0x2,a6) */
	{ { uint32_t v = 0xB0u; uint32_t da = ctx->a[6] + (uint32_t)4; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A1C0 move.w #0xb0,(0x4,a6) */
	{ { uint32_t v = 0x14u; uint32_t da = ctx->a[6] + (uint32_t)6; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00A1C6 move.b #0x14,(0x6,a6) */
	{ { uint32_t v = 0x50u; uint32_t da = ctx->a[6] + (uint32_t)7; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00A1CC move.b #0x50,(0x7,a6) */
	{ { uint32_t v = 0x15Eu; uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A1D2 move.w #0x15e,(0x24,a6) */
	{ { uint32_t v = 0x597Au; uint32_t da = ctx->a[6] + (uint32_t)28; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 00A1D8 move.l #0x597a,(0x1c,a6) */
	{ { uint32_t v = 0xD56Eu; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 00A1E0 move.l #0xd56e,(0x20,a6) */
	{ { uint32_t v = 0x64u; uint32_t da = ctx->a[6] + (uint32_t)8; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A1E8 move.w #0x64,(0x8,a6) */
	{ { uint32_t v = 0x0u; uint32_t da = ctx->a[6] + (uint32_t)12; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00A1EE move.b #0x0,(0xc,a6) */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF80DAu; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 00A1F4 addq.w #0x1,(0xffff80da).l */
L_00A1FA:
	{ { uint32_t v = ctx->a[0]; ctx->a[7] -= 4; uint32_t da = ctx->a[7]; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 12); /* 00A1FA move.l a0,-(sp) */
	{ { uint32_t v = 0xE500u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00A1FC move.w #0xe500,d1 */
	{ ctx->a[0] = 0x000539D0u; } hle_insn(ctx, 12); /* 00A200 lea.l (dat_0539D0).l,a0 */
	{ ctx->d[2] = 0xFu; cc_logic32(ctx, 0xFu); } hle_insn(ctx, 4); /* 00A206 moveq #0xf,d2 */
	{ ctx->d[3] = 0xBu; cc_logic32(ctx, 0xBu); } hle_insn(ctx, 4); /* 00A208 moveq #0xb,d3 */
	{ { uint32_t v = 0xE000u; ctx->d[6] = v; cc_logic32(ctx, v); } } hle_insn(ctx, 12); /* 00A20A move.l #0xe000,d6 */
	{ { uint32_t sa = 0xFFFF9E4Au; uint32_t v = ld16(sa); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A210 move.w (0xffff9e4a).l,d7 */
	hle_insn(ctx, 20); u_012234(ctx); /* 00A216 jsr (VdpQueueTileRect).l */
	{ { uint32_t sa = ctx->a[7]; ctx->a[7] += 4; uint32_t v = ld32(sa); ctx->a[0] = v; } } hle_insn(ctx, 12); /* 00A21C movea.l (sp)+,a0 */
	{ { uint32_t v = 0xFFFFu; uint32_t da = 0xFFFF9EA6u; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 20); /* 00A21E move.w #0xffff,(0xffff9ea6).l */
	hle_insn(ctx, 16); return; /* 00A226 rts */
}
/* draw_tiles_e51e */
HLE_FUNC_PURE(0x00A228, ScrDrawTilesE51E, 4000)
{
	{ { uint32_t v = ctx->a[0]; ctx->a[7] -= 4; uint32_t da = ctx->a[7]; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 12); /* 00A228 move.l a0,-(sp) */
	{ { uint32_t v = 0xE51Eu; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00A22A move.w #0xe51e,d1 */
	{ ctx->a[0] = 0x000539EEu; } hle_insn(ctx, 12); /* 00A22E lea.l (dat_0539EE).l,a0 */
	{ ctx->d[2] = 0x10u; cc_logic32(ctx, 0x10u); } hle_insn(ctx, 4); /* 00A234 moveq #0x10,d2 */
	{ ctx->d[3] = 0xBu; cc_logic32(ctx, 0xBu); } hle_insn(ctx, 4); /* 00A236 moveq #0xb,d3 */
	{ { uint32_t v = 0xE000u; ctx->d[6] = v; cc_logic32(ctx, v); } } hle_insn(ctx, 12); /* 00A238 move.l #0xe000,d6 */
	{ { uint32_t sa = 0xFFFF9E4Au; uint32_t v = ld16(sa); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A23E move.w (0xffff9e4a).l,d7 */
	hle_insn(ctx, 20); u_012234(ctx); /* 00A244 jsr (VdpQueueTileRect).l */
	{ { uint32_t sa = ctx->a[7]; ctx->a[7] += 4; uint32_t v = ld32(sa); ctx->a[0] = v; } } hle_insn(ctx, 12); /* 00A24A movea.l (sp)+,a0 */
	hle_insn(ctx, 16); return; /* 00A24C rts */
}
/* sub_00A5F6 */
HLE_FUNC_PURE(0x00A5F6, ScrSpawnT23, 4000)
{
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); set_w(&ctx->d[5], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00A5F6 move.w (a0)+,d5 */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00A5F8 move.w (a0)+,d1 */
	hle_insn(ctx, 18); u_009A02(ctx); /* 00A5FA bsr.w sub_009A02 */
	{ { uint32_t v = 0x23u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00A5FE move.w #0x23,d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x8000u) ? 0 : CC_Z); ctx->d[0] |= 0x8000u; } hle_insn(ctx, 10); /* 00A602 bset.b #0xf,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00A606 move.w d0,(a6) */
	{ { uint32_t v = 0xDDBAu; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 00A608 move.l #0xddba,(0x20,a6) */
	{ { uint32_t v = 0x6900u; uint32_t da = ctx->a[6] + (uint32_t)28; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 00A610 move.l #0x6900,(0x1c,a6) */
	{ { uint32_t v = 0xAu; uint32_t da = ctx->a[6] + (uint32_t)8; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A618 move.w #0xa,(0x8,a6) */
	{ { uint32_t v = 0x12EB2u; ctx->a[5] = v; } } hle_insn(ctx, 12); /* 00A61E movea.l #0x12eb2,a5 */
	{ { uint32_t sa = ctx->a[5]; ctx->a[5] += 2; uint32_t v = ld16(sa); uint32_t da = ctx->a[6] + (uint32_t)26; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A624 move.w (a5)+,(0x1a,a6) */
	{ { uint32_t v = ctx->a[5]; uint32_t da = ctx->a[6] + (uint32_t)22; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 16); /* 00A628 move.l a5,(0x16,a6) */
	hle_insn(ctx, 20); Random(ctx); /* 00A62C jsr (Random).l */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6] + (uint32_t)38; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 00A632 move.w d0,(0x26,a6) */
	{ { uint32_t v = 0x6u; uint32_t da = ctx->a[6] + (uint32_t)40; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A636 move.w #0x6,(0x28,a6) */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF9E10u; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 00A63C addq.w #0x1,(0xffff9e10).l */
	hle_insn(ctx, 16); return; /* 00A642 rts */
}
/* spawn_t0ea */
HLE_FUNC_PURE(0x00A644, ScrSpawnT0ea, 4000)
{
	{ ctx->a[1] = 0x00A654u; } hle_insn(ctx, 8); /* 00A644 lea.l (dat_00A654,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00A648 jsr (ObjSpawnTemplate,pc) */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF9E08u; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 00A64C addq.w #0x1,(0xffff9e08).l */
	hle_insn(ctx, 16); return; /* 00A652 rts */
}
/* spawn_t24a */
HLE_FUNC_PURE(0x00A666, ScrSpawnT24a, 4000)
{
	{ ctx->a[1] = 0x00A680u; } hle_insn(ctx, 8); /* 00A666 lea.l (dat_00A680,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00A66A jsr (ObjSpawnTemplate,pc) */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00A66E move.w (a0)+,d0 */
	{ { uint32_t sv = 0xE300u; uint32_t r = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[0], r); } } hle_insn(ctx, 8); /* 00A670 addi.w #0xe300,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6] + (uint32_t)10; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 00A674 move.w d0,(0xa,a6) */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF9E0Eu; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 00A678 addq.w #0x1,(0xffff9e0e).l */
	hle_insn(ctx, 16); return; /* 00A67E rts */
}
/* spawn_t24b */
HLE_FUNC_PURE(0x00A692, ScrSpawnT24b, 4000)
{
	{ ctx->a[1] = 0x00A700u; } hle_insn(ctx, 8); /* 00A692 lea.l (dat_00A700,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00A696 jsr (ObjSpawnTemplate,pc) */
	{ { uint32_t sa = ctx->a[0]; uint32_t v = ld16(sa); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00A69A move.w (a0),d0 */
	{ { uint32_t sv = 0xE400u; uint32_t r = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[0], r); } } hle_insn(ctx, 8); /* 00A69C addi.w #0xe400,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6] + (uint32_t)10; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 00A6A0 move.w d0,(0xa,a6) */
	{ { uint32_t v = 0xFFFFu; uint32_t da = ctx->a[6] + (uint32_t)42; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A6A4 move.w #0xffff,(0x2a,a6) */
	{ ctx->a[1] = 0x00A700u; } hle_insn(ctx, 8); /* 00A6AA lea.l (dat_00A700,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00A6AE jsr (ObjSpawnTemplate,pc) */
	{ { uint32_t sv = 0x40u; uint32_t da = ctx->a[6] + (uint32_t)2; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 00A6B2 addi.w #0x40,(0x2,a6) */
	{ { uint32_t sa = ctx->a[0]; uint32_t v = ld16(sa); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00A6B8 move.w (a0),d0 */
	{ { uint32_t sv = 0xE410u; uint32_t r = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[0], r); } } hle_insn(ctx, 8); /* 00A6BA addi.w #0xe410,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6] + (uint32_t)10; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 00A6BE move.w d0,(0xa,a6) */
	{ { uint32_t v = 0x0u; uint32_t da = ctx->a[6] + (uint32_t)42; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A6C2 move.w #0x0,(0x2a,a6) */
	{ ctx->a[1] = 0x00A700u; } hle_insn(ctx, 8); /* 00A6C8 lea.l (dat_00A700,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00A6CC jsr (ObjSpawnTemplate,pc) */
	{ { uint32_t sv = 0x20u; uint32_t da = ctx->a[6] + (uint32_t)2; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 00A6D0 addi.w #0x20,(0x2,a6) */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00A6D6 move.w (a0)+,d0 */
	{ { uint32_t sv = 0xE400u; uint32_t r = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[0], r); } } hle_insn(ctx, 8); /* 00A6D8 addi.w #0xe400,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6] + (uint32_t)10; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 00A6DC move.w d0,(0xa,a6) */
	{ { uint32_t v = 0x1u; uint32_t da = ctx->a[6] + (uint32_t)42; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A6E0 move.w #0x1,(0x2a,a6) */
	{ { uint32_t da = ctx->a[6]; uint8_t v = ld8(da); cc_set(ctx, CC_Z, (v & 0x80) ? 0 : CC_Z); st8(da, v & 0x7F); } } hle_insn(ctx, 16); /* 00A6E6 bclr.b #0xf,(a6) */
	{ { uint32_t v = ctx->a[6]; uint32_t da = 0xFFFF9EDAu; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 20); /* 00A6EA move.l a6,(0xffff9eda).l */
	{ { uint32_t v = 0x2u; uint32_t da = 0xFFFFA23Cu; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 20); /* 00A6F0 move.w #0x2,(0xffffa23c).l */
	{ { uint32_t sv = 0x3u; uint32_t da = 0xFFFF9E0Eu; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 00A6F8 addq.w #0x3,(0xffff9e0e).l */
	hle_insn(ctx, 16); return; /* 00A6FE rts */
}
/* spawn_t0fc */
HLE_FUNC_PURE(0x00A7E8, ScrSpawnT0fc, 4000)
{
	{ ctx->a[1] = 0x00A7FAu; } hle_insn(ctx, 8); /* 00A7E8 lea.l (dat_00A7FA,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00A7EC jsr (ObjSpawnTemplate,pc) */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A7F0 move.w (a0)+,(0x2,a6) */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); uint32_t da = ctx->a[6] + (uint32_t)10; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A7F4 move.w (a0)+,(0xa,a6) */
	hle_insn(ctx, 16); return; /* 00A7F8 rts */
}
/* spawn_t20 */
HLE_FUNC_PURE(0x00A80C, ScrSpawnT20, 4000)
{
	{ ctx->a[1] = 0x00A81Au; } hle_insn(ctx, 8); /* 00A80C lea.l (dat_00A81A,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00A810 jsr (ObjSpawnTemplate,pc) */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A814 move.w (a0)+,(0x2,a6) */
	hle_insn(ctx, 16); return; /* 00A818 rts */
}
/* spawn_t0fd */
HLE_FUNC_PURE(0x00A8CC, ScrSpawnT0fd, 4000)
{
	{ ctx->a[1] = 0x00A8DEu; } hle_insn(ctx, 8); /* 00A8CC lea.l (dat_00A8DE,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00A8D0 jsr (ObjSpawnTemplate,pc) */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A8D4 move.w (a0)+,(0x2,a6) */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); uint32_t da = ctx->a[6] + (uint32_t)4; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A8D8 move.w (a0)+,(0x4,a6) */
	hle_insn(ctx, 16); return; /* 00A8DC rts */
}
/* spawn_t0fe */
HLE_FUNC_PURE(0x00A8F0, ScrSpawnT0fe, 4000)
{
	{ ctx->a[1] = 0x00A902u; } hle_insn(ctx, 8); /* 00A8F0 lea.l (dat_00A902,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00A8F4 jsr (ObjSpawnTemplate,pc) */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A8F8 move.w (a0)+,(0x2,a6) */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); uint32_t da = ctx->a[6] + (uint32_t)4; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00A8FC move.w (a0)+,(0x4,a6) */
	hle_insn(ctx, 16); return; /* 00A900 rts */
}
/* spawn_t29 */
HLE_FUNC_PURE(0x00AA3E, ScrSpawnT29, 4000)
{
	{ ctx->a[1] = 0x00AA58u; } hle_insn(ctx, 8); /* 00AA3E lea.l (dat_00AA58,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00AA42 jsr (ObjSpawnTemplate,pc) */
	{ { uint32_t v = 0x10u; uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00AA46 move.w #0x10,(0x24,a6) */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00AA4C move.w (a0)+,(0x2,a6) */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF9E12u; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 00AA50 addq.w #0x1,(0xffff9e12).l */
	hle_insn(ctx, 16); return; /* 00AA56 rts */
}
/* spawn_t26a */
HLE_FUNC_PURE(0x00AA6A, ScrSpawnT26a, 4000)
{
	{ ctx->a[1] = 0x00AAC0u; } hle_insn(ctx, 8); /* 00AA6A lea.l (dat_00AAC0,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00AA6E jsr (ObjSpawnTemplate,pc) */
	{ { uint32_t v = 0x10u; uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00AA72 move.w #0x10,(0x24,a6) */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00AA78 move.w (a0)+,(0x2,a6) */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); uint32_t da = ctx->a[6] + (uint32_t)4; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00AA7C move.w (a0)+,(0x4,a6) */
	{ { uint32_t v = 0xBBu; uint32_t da = ctx->a[6] + (uint32_t)40; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00AA80 move.w #0xbb,(0x28,a6) */
	{ { uint32_t sa = ctx->a[6]; uint32_t v = ld16(sa); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00AA86 move.w (a6),d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x8000u) ? 0 : CC_Z); ctx->d[0] |= 0x8000u; } hle_insn(ctx, 10); /* 00AA88 bset.b #0xf,d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x4000u) ? 0 : CC_Z); ctx->d[0] |= 0x4000u; } hle_insn(ctx, 10); /* 00AA8C bset.b #0xe,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00AA90 move.w d0,(a6) */
	{ { uint32_t v = 0x20u; uint32_t da = ctx->a[6] + (uint32_t)10; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00AA92 move.b #0x20,(0xa,a6) */
	{ { uint32_t v = 0x1u; uint32_t da = ctx->a[6] + (uint32_t)14; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00AA98 move.b #0x1,(0xe,a6) */
	{ { uint32_t da = ctx->a[6] + (uint32_t)11; st8(da, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } } hle_insn(ctx, 16); /* 00AA9E clr.b (0xb,a6) */
	hle_insn(ctx, 18); u_00EE88(ctx); /* 00AAA2 jsr (sub_00EE88,pc) */
	{ { uint32_t sv = 0x12Cu; uint32_t da = ctx->a[6] + (uint32_t)2; (void)alu(ctx, OP_SUB, ld16(da), sv, 16, 0); } } hle_insn(ctx, 16); /* 00AAA6 cmpi.w #0x12c,(0x2,a6) */
	if (!c_ge(ctx)) { hle_insn(ctx, 10); goto L_00AAB8; } hle_insn(ctx, 8); /* 00AAAC blt.s loc_00AAB8 */
	{ { uint32_t v = 0xFFu; uint32_t da = ctx->a[6] + (uint32_t)14; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00AAAE move.b #0xff,(0xe,a6) */
	{ { uint32_t da = ctx->a[6]; uint8_t v = ld8(da); cc_set(ctx, CC_Z, (v & 0x40) ? 0 : CC_Z); st8(da, v & 0xBF); } } hle_insn(ctx, 16); /* 00AAB4 bclr.b #0x6,(a6) */
L_00AAB8:
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF9E12u; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 00AAB8 addq.w #0x1,(0xffff9e12).l */
	hle_insn(ctx, 16); return; /* 00AABE rts */
}
/* spawn_t26b */
HLE_FUNC_PURE(0x00AAD2, ScrSpawnT26b, 4000)
{
	{ ctx->a[1] = 0x00AB3Cu; } hle_insn(ctx, 8); /* 00AAD2 lea.l (dat_00AB3C,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00AAD6 jsr (ObjSpawnTemplate,pc) */
	{ { uint32_t v = 0x10u; uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00AADA move.w #0x10,(0x24,a6) */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00AAE0 move.w (a0)+,(0x2,a6) */
	{ { uint32_t v = 0xB0u; uint32_t da = ctx->a[6] + (uint32_t)40; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00AAE4 move.w #0xb0,(0x28,a6) */
	{ { uint32_t sa = ctx->a[6]; uint32_t v = ld16(sa); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00AAEA move.w (a6),d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x8000u) ? 0 : CC_Z); ctx->d[0] |= 0x8000u; } hle_insn(ctx, 10); /* 00AAEC bset.b #0xf,d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x4000u) ? 0 : CC_Z); ctx->d[0] |= 0x4000u; } hle_insn(ctx, 10); /* 00AAF0 bset.b #0xe,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00AAF4 move.w d0,(a6) */
	{ { uint32_t v = 0x18u; uint32_t da = ctx->a[6] + (uint32_t)10; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00AAF6 move.b #0x18,(0xa,a6) */
	{ { uint32_t v = 0x1u; uint32_t da = ctx->a[6] + (uint32_t)14; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00AAFC move.b #0x1,(0xe,a6) */
	{ { uint32_t da = ctx->a[6] + (uint32_t)11; st8(da, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } } hle_insn(ctx, 16); /* 00AB02 clr.b (0xb,a6) */
	hle_insn(ctx, 18); u_00F170(ctx); /* 00AB06 jsr (sub_00F170,pc) */
	{ { uint32_t sv = 0x12Cu; uint32_t da = ctx->a[6] + (uint32_t)2; (void)alu(ctx, OP_SUB, ld16(da), sv, 16, 0); } } hle_insn(ctx, 16); /* 00AB0A cmpi.w #0x12c,(0x2,a6) */
	if (!c_ge(ctx)) { hle_insn(ctx, 10); goto L_00AB1C; } hle_insn(ctx, 8); /* 00AB10 blt.s loc_00AB1C */
	{ { uint32_t v = 0xFFu; uint32_t da = ctx->a[6] + (uint32_t)14; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00AB12 move.b #0xff,(0xe,a6) */
	{ { uint32_t da = ctx->a[6]; uint8_t v = ld8(da); cc_set(ctx, CC_Z, (v & 0x40) ? 0 : CC_Z); st8(da, v & 0xBF); } } hle_insn(ctx, 16); /* 00AB18 bclr.b #0x6,(a6) */
L_00AB1C:
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00AB1C move.w (a0)+,d0 */
	if (!c_n(ctx)) { hle_insn(ctx, 10); goto L_00AB30; } hle_insn(ctx, 8); /* 00AB1E bpl.s loc_00AB30 */
	{ { uint32_t v = 0xF69Cu; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 00AB20 move.l #0xf69c,(0x20,a6) */
	{ { uint32_t r = alu(ctx, OP_SUB, 0, (ctx->d[0] & 0xFFFF), 16, 1); set_w(&ctx->d[0], r); } } hle_insn(ctx, 4); /* 00AB28 neg.w d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6] + (uint32_t)40; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 00AB2A move.w d0,(0x28,a6) */
	{ ctx->d[0] = 0x0u; cc_logic32(ctx, 0x0u); } hle_insn(ctx, 4); /* 00AB2E moveq #0x0,d0 */
L_00AB30:
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6] + (uint32_t)4; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 00AB30 move.w d0,(0x4,a6) */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF9E12u; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 00AB34 addq.w #0x1,(0xffff9e12).l */
	hle_insn(ctx, 16); return; /* 00AB3A rts */
}
/* spawn_t26c */
HLE_FUNC_PURE(0x00AB8A, ScrSpawnT26c, 4000)
{
	{ ctx->a[1] = 0x00ABF4u; } hle_insn(ctx, 8); /* 00AB8A lea.l (dat_00ABF4,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00AB8E jsr (ObjSpawnTemplate,pc) */
	{ { uint32_t v = 0x10u; uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00AB92 move.w #0x10,(0x24,a6) */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00AB98 move.w (a0)+,(0x2,a6) */
	{ { uint32_t v = 0xA8u; uint32_t da = ctx->a[6] + (uint32_t)40; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00AB9C move.w #0xa8,(0x28,a6) */
	{ { uint32_t sa = ctx->a[6]; uint32_t v = ld16(sa); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00ABA2 move.w (a6),d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x8000u) ? 0 : CC_Z); ctx->d[0] |= 0x8000u; } hle_insn(ctx, 10); /* 00ABA4 bset.b #0xf,d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x4000u) ? 0 : CC_Z); ctx->d[0] |= 0x4000u; } hle_insn(ctx, 10); /* 00ABA8 bset.b #0xe,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00ABAC move.w d0,(a6) */
	{ { uint32_t v = 0x10u; uint32_t da = ctx->a[6] + (uint32_t)10; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00ABAE move.b #0x10,(0xa,a6) */
	{ { uint32_t v = 0x1u; uint32_t da = ctx->a[6] + (uint32_t)14; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00ABB4 move.b #0x1,(0xe,a6) */
	{ { uint32_t da = ctx->a[6] + (uint32_t)11; st8(da, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } } hle_insn(ctx, 16); /* 00ABBA clr.b (0xb,a6) */
	hle_insn(ctx, 18); u_00F356(ctx); /* 00ABBE jsr (sub_00F356,pc) */
	{ { uint32_t sv = 0x12Cu; uint32_t da = ctx->a[6] + (uint32_t)2; (void)alu(ctx, OP_SUB, ld16(da), sv, 16, 0); } } hle_insn(ctx, 16); /* 00ABC2 cmpi.w #0x12c,(0x2,a6) */
	if (!c_ge(ctx)) { hle_insn(ctx, 10); goto L_00ABD4; } hle_insn(ctx, 8); /* 00ABC8 blt.s loc_00ABD4 */
	{ { uint32_t v = 0xFFu; uint32_t da = ctx->a[6] + (uint32_t)14; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00ABCA move.b #0xff,(0xe,a6) */
	{ { uint32_t da = ctx->a[6]; uint8_t v = ld8(da); cc_set(ctx, CC_Z, (v & 0x40) ? 0 : CC_Z); st8(da, v & 0xBF); } } hle_insn(ctx, 16); /* 00ABD0 bclr.b #0x6,(a6) */
L_00ABD4:
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00ABD4 move.w (a0)+,d0 */
	if (!c_n(ctx)) { hle_insn(ctx, 10); goto L_00ABE8; } hle_insn(ctx, 8); /* 00ABD6 bpl.s loc_00ABE8 */
	{ { uint32_t v = 0xF6F4u; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 00ABD8 move.l #0xf6f4,(0x20,a6) */
	{ { uint32_t r = alu(ctx, OP_SUB, 0, (ctx->d[0] & 0xFFFF), 16, 1); set_w(&ctx->d[0], r); } } hle_insn(ctx, 4); /* 00ABE0 neg.w d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6] + (uint32_t)40; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 00ABE2 move.w d0,(0x28,a6) */
	{ ctx->d[0] = 0x0u; cc_logic32(ctx, 0x0u); } hle_insn(ctx, 4); /* 00ABE6 moveq #0x0,d0 */
L_00ABE8:
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6] + (uint32_t)4; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 00ABE8 move.w d0,(0x4,a6) */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF9E12u; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 00ABEC addq.w #0x1,(0xffff9e12).l */
	hle_insn(ctx, 16); return; /* 00ABF2 rts */
}
/* spawn_t23a */
HLE_FUNC_PURE(0x00AC6A, ScrSpawnT23a, 4000)
{
	{ ctx->a[1] = 0x00AC9Eu; } hle_insn(ctx, 8); /* 00AC6A lea.l (dat_00AC9E,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00AC6E jsr (ObjSpawnTemplate,pc) */
	{ { uint32_t da = ctx->a[6] + (uint32_t)38; st16(da, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } } hle_insn(ctx, 16); /* 00AC72 clr.w (0x26,a6) */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00AC76 move.w (a0)+,(0x2,a6) */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00AC7A move.w (a0)+,d0 */
	if (!c_n(ctx)) { hle_insn(ctx, 10); goto L_00AC86; } hle_insn(ctx, 8); /* 00AC7C bpl.s loc_00AC86 */
	{ { uint32_t r = alu(ctx, OP_SUB, 0, (ctx->d[0] & 0xFFFF), 16, 1); set_w(&ctx->d[0], r); } } hle_insn(ctx, 4); /* 00AC7E neg.w d0 */
	{ { uint32_t v = 0xFFFFu; uint32_t da = ctx->a[6] + (uint32_t)38; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00AC80 move.w #0xffff,(0x26,a6) */
L_00AC86:
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6] + (uint32_t)40; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 00AC86 move.w d0,(0x28,a6) */
	{ { uint32_t v = 0xCu; uint32_t da = ctx->a[6] + (uint32_t)18; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00AC8A move.b #0xc,(0x12,a6) */
	{ { uint32_t v = 0x4u; uint32_t da = ctx->a[6] + (uint32_t)16; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00AC90 move.b #0x4,(0x10,a6) */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF9E10u; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 00AC96 addq.w #0x1,(0xffff9e10).l */
	hle_insn(ctx, 16); return; /* 00AC9C rts */
}
/* spawn_t0ff */
HLE_FUNC_PURE(0x00ACF2, ScrSpawnT0ff, 4000)
{
	{ ctx->a[1] = 0x00AD02u; } hle_insn(ctx, 8); /* 00ACF2 lea.l (dat_00AD02,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00ACF6 jsr (ObjSpawnTemplate,pc) */
	{ { uint32_t v = ctx->a[6]; uint32_t da = 0xFFFF9EDAu; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 20); /* 00ACFA move.l a6,(0xffff9eda).l */
	hle_insn(ctx, 16); return; /* 00AD00 rts */
}
/* spawn_t28b */
HLE_FUNC_PURE(0x00AD14, ScrSpawnT28b, 4000)
{
	{ ctx->a[1] = 0x00AD36u; } hle_insn(ctx, 8); /* 00AD14 lea.l (dat_00AD36,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00AD18 jsr (ObjSpawnTemplate,pc) */
	{ { uint32_t da = ctx->a[6]; uint8_t v = ld8(da); cc_set(ctx, CC_Z, (v & 0x40) ? 0 : CC_Z); st8(da, v | 0x40); } } hle_insn(ctx, 16); /* 00AD1C bset.b #0xe,(a6) */
	{ { uint32_t v = 0x40u; uint32_t da = ctx->a[6] + (uint32_t)10; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00AD20 move.b #0x40,(0xa,a6) */
	{ { uint32_t v = 0x1u; uint32_t da = ctx->a[6] + (uint32_t)14; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00AD26 move.b #0x1,(0xe,a6) */
	{ { uint32_t da = ctx->a[6] + (uint32_t)11; st8(da, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } } hle_insn(ctx, 16); /* 00AD2C clr.b (0xb,a6) */
	hle_insn(ctx, 18); u_010408(ctx); /* 00AD30 jsr (sub_010408,pc) */
	hle_insn(ctx, 16); return; /* 00AD34 rts */
}
/* spawn_t27a */
HLE_FUNC_PURE(0x00ADD0, ScrSpawnT27a, 4000)
{
	{ ctx->a[1] = 0x00AE60u; } hle_insn(ctx, 8); /* 00ADD0 lea.l (dat_00AE60,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00ADD4 jsr (ObjSpawnTemplate,pc) */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00ADD8 move.w (a0)+,(0x2,a6) */
	{ { uint32_t sa = ctx->a[6]; uint32_t v = ld16(sa); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00ADDC move.w (a6),d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x8000u) ? 0 : CC_Z); ctx->d[0] |= 0x8000u; } hle_insn(ctx, 10); /* 00ADDE bset.b #0xf,d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x4000u) ? 0 : CC_Z); ctx->d[0] |= 0x4000u; } hle_insn(ctx, 10); /* 00ADE2 bset.b #0xe,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00ADE6 move.w d0,(a6) */
	{ { uint32_t v = 0x40u; uint32_t da = ctx->a[6] + (uint32_t)10; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00ADE8 move.b #0x40,(0xa,a6) */
	{ { uint32_t v = 0x1u; uint32_t da = ctx->a[6] + (uint32_t)14; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00ADEE move.b #0x1,(0xe,a6) */
	{ { uint32_t da = ctx->a[6] + (uint32_t)11; st8(da, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } } hle_insn(ctx, 16); /* 00ADF4 clr.b (0xb,a6) */
	hle_insn(ctx, 18); u_00FD0C(ctx); /* 00ADF8 jsr (sub_00FD0C,pc) */
	{ { uint32_t sa = ctx->a[6] + (uint32_t)2; uint32_t v = ld16(sa); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 00ADFC move.w (0x2,a6),d0 */
	{ { uint32_t sv = 0x12Cu; (void)alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFF), sv, 16, 0); } } hle_insn(ctx, 8); /* 00AE00 cmpi.w #0x12c,d0 */
	if (!c_ge(ctx)) { hle_insn(ctx, 10); goto L_00AE10; } hle_insn(ctx, 8); /* 00AE04 blt.s loc_00AE10 */
	{ { uint32_t v = 0xFFu; uint32_t da = ctx->a[6] + (uint32_t)14; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00AE06 move.b #0xff,(0xe,a6) */
	{ { uint32_t da = ctx->a[6]; uint8_t v = ld8(da); cc_set(ctx, CC_Z, (v & 0x40) ? 0 : CC_Z); st8(da, v & 0xBF); } } hle_insn(ctx, 16); /* 00AE0C bclr.b #0x6,(a6) */
L_00AE10:
	{ { cc_logic16(ctx, (ctx->d[0] & 0xFFFF)); } } hle_insn(ctx, 4); /* 00AE10 tst.w d0 */
	if (!c_z(ctx)) { hle_insn(ctx, 10); goto L_00AE1C; } hle_insn(ctx, 8); /* 00AE12 bne.s loc_00AE1C */
	{ { uint32_t da = ctx->a[6]; uint8_t v = ld8(da); cc_set(ctx, CC_Z, (v & 0x2) ? 0 : CC_Z); st8(da, v | 0x2); } } hle_insn(ctx, 16); /* 00AE14 bset.b #0x9,(a6) */
	{ { uint32_t v = 0xFFECu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00AE18 move.w #0xffec,d0 */
L_00AE1C:
	{ { uint32_t sv = 0x140u; (void)alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFF), sv, 16, 0); } } hle_insn(ctx, 8); /* 00AE1C cmpi.w #0x140,d0 */
	if (!c_z(ctx)) { hle_insn(ctx, 10); goto L_00AE2A; } hle_insn(ctx, 8); /* 00AE20 bne.s loc_00AE2A */
	{ { uint32_t da = ctx->a[6]; uint8_t v = ld8(da); cc_set(ctx, CC_Z, (v & 0x2) ? 0 : CC_Z); st8(da, v | 0x2); } } hle_insn(ctx, 16); /* 00AE22 bset.b #0x9,(a6) */
	{ { uint32_t v = 0x154u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00AE26 move.w #0x154,d0 */
L_00AE2A:
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00AE2A move.w (a0)+,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6] + (uint32_t)40; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 00AE2C move.w d0,(0x28,a6) */
	if (!c_n(ctx)) { hle_insn(ctx, 10); goto L_00AE42; } hle_insn(ctx, 8); /* 00AE30 bpl.s loc_00AE42 */
	{ { uint32_t r = alu(ctx, OP_SUB, 0, (ctx->d[0] & 0xFFFF), 16, 1); set_w(&ctx->d[0], r); } } hle_insn(ctx, 4); /* 00AE32 neg.w d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6] + (uint32_t)40; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 00AE34 move.w d0,(0x28,a6) */
	{ { uint32_t v = 0xFE3Cu; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 00AE38 move.l #0xfe3c,(0x20,a6) */
	{ ctx->d[0] = 0x0u; cc_logic32(ctx, 0x0u); } hle_insn(ctx, 4); /* 00AE40 moveq #0x0,d0 */
L_00AE42:
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6] + (uint32_t)4; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 00AE42 move.w d0,(0x4,a6) */
	{ { uint32_t sv = 0xB0u; uint32_t da = ctx->a[6] + (uint32_t)40; (void)alu(ctx, OP_SUB, ld16(da), sv, 16, 0); } } hle_insn(ctx, 16); /* 00AE46 cmpi.w #0xb0,(0x28,a6) */
	if (c_gt(ctx)) { hle_insn(ctx, 10); goto L_00AE54; } hle_insn(ctx, 8); /* 00AE4C bgt.s loc_00AE54 */
	if (c_z(ctx)) { hle_insn(ctx, 10); goto L_00AE58; } hle_insn(ctx, 8); /* 00AE4E beq.s loc_00AE58 */
	{ { uint32_t sv = 0x4u; uint32_t da = ctx->a[6] + (uint32_t)7; uint32_t r = alu(ctx, OP_ADD, ld8(da), sv, 8, 1); st8(da, r); } } hle_insn(ctx, 16); /* 00AE50 addq.b #0x4,(0x7,a6) */
L_00AE54:
	{ { uint32_t sv = 0x2u; uint32_t da = ctx->a[6] + (uint32_t)7; uint32_t r = alu(ctx, OP_SUB, ld8(da), sv, 8, 1); st8(da, r); } } hle_insn(ctx, 16); /* 00AE54 subq.b #0x2,(0x7,a6) */
L_00AE58:
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF9E0Au; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 00AE58 addq.w #0x1,(0xffff9e0a).l */
	hle_insn(ctx, 16); return; /* 00AE5E rts */
}
/* spawn_t27b */
HLE_FUNC_PURE(0x00AE72, ScrSpawnT27b, 4000)
{
	{ ctx->a[1] = 0x00AEE8u; } hle_insn(ctx, 8); /* 00AE72 lea.l (dat_00AEE8,pc),a1 */
	hle_insn(ctx, 18); ObjSpawnTemplate(ctx); /* 00AE76 jsr (ObjSpawnTemplate,pc) */
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); uint32_t da = ctx->a[6] + (uint32_t)2; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00AE7A move.w (a0)+,(0x2,a6) */
	{ { uint32_t v = 0xB0u; uint32_t da = ctx->a[6] + (uint32_t)40; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 00AE7E move.w #0xb0,(0x28,a6) */
	{ { uint32_t sa = ctx->a[6]; uint32_t v = ld16(sa); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00AE84 move.w (a6),d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x8000u) ? 0 : CC_Z); ctx->d[0] |= 0x8000u; } hle_insn(ctx, 10); /* 00AE86 bset.b #0xf,d0 */
	{ cc_set(ctx, CC_Z, (ctx->d[0] & 0x4000u) ? 0 : CC_Z); ctx->d[0] |= 0x4000u; } hle_insn(ctx, 10); /* 00AE8A bset.b #0xe,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6]; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00AE8E move.w d0,(a6) */
	{ { uint32_t v = 0x40u; uint32_t da = ctx->a[6] + (uint32_t)10; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00AE90 move.b #0x40,(0xa,a6) */
	{ { uint32_t v = 0x1u; uint32_t da = ctx->a[6] + (uint32_t)14; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00AE96 move.b #0x1,(0xe,a6) */
	{ { uint32_t da = ctx->a[6] + (uint32_t)11; st8(da, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } } hle_insn(ctx, 16); /* 00AE9C clr.b (0xb,a6) */
	hle_insn(ctx, 18); u_00FFC0(ctx); /* 00AEA0 jsr (sub_00FFC0,pc) */
	{ { uint32_t sv = 0x12Cu; uint32_t da = ctx->a[6] + (uint32_t)2; (void)alu(ctx, OP_SUB, ld16(da), sv, 16, 0); } } hle_insn(ctx, 16); /* 00AEA4 cmpi.w #0x12c,(0x2,a6) */
	if (!c_ge(ctx)) { hle_insn(ctx, 10); goto L_00AEB6; } hle_insn(ctx, 8); /* 00AEAA blt.s loc_00AEB6 */
	{ { uint32_t v = 0xFFu; uint32_t da = ctx->a[6] + (uint32_t)14; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 16); /* 00AEAC move.b #0xff,(0xe,a6) */
	{ { uint32_t da = ctx->a[6]; uint8_t v = ld8(da); cc_set(ctx, CC_Z, (v & 0x40) ? 0 : CC_Z); st8(da, v & 0xBF); } } hle_insn(ctx, 16); /* 00AEB2 bclr.b #0xe,(a6) */
L_00AEB6:
	{ { uint32_t sa = ctx->a[0]; ctx->a[0] += 2; uint32_t v = ld16(sa); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 00AEB6 move.w (a0)+,d0 */
	if (!c_n(ctx)) { hle_insn(ctx, 10); goto L_00AECA; } hle_insn(ctx, 8); /* 00AEB8 bpl.s loc_00AECA */
	{ { uint32_t r = alu(ctx, OP_SUB, 0, (ctx->d[0] & 0xFFFF), 16, 1); set_w(&ctx->d[0], r); } } hle_insn(ctx, 4); /* 00AEBA neg.w d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6] + (uint32_t)40; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 00AEBC move.w d0,(0x28,a6) */
	{ { uint32_t v = 0x100D0u; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 00AEC0 move.l #0x100d0,(0x20,a6) */
	{ ctx->d[0] = 0x0u; cc_logic32(ctx, 0x0u); } hle_insn(ctx, 4); /* 00AEC8 moveq #0x0,d0 */
L_00AECA:
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6] + (uint32_t)4; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 00AECA move.w d0,(0x4,a6) */
	{ { uint32_t sv = 0xB0u; uint32_t da = ctx->a[6] + (uint32_t)40; (void)alu(ctx, OP_SUB, ld16(da), sv, 16, 0); } } hle_insn(ctx, 16); /* 00AECE cmpi.w #0xb0,(0x28,a6) */
	if (c_gt(ctx)) { hle_insn(ctx, 10); goto L_00AEDC; } hle_insn(ctx, 8); /* 00AED4 bgt.s loc_00AEDC */
	if (c_z(ctx)) { hle_insn(ctx, 10); goto L_00AEE0; } hle_insn(ctx, 8); /* 00AED6 beq.s loc_00AEE0 */
	{ { uint32_t sv = 0x4u; uint32_t da = ctx->a[6] + (uint32_t)7; uint32_t r = alu(ctx, OP_ADD, ld8(da), sv, 8, 1); st8(da, r); } } hle_insn(ctx, 16); /* 00AED8 addq.b #0x4,(0x7,a6) */
L_00AEDC:
	{ { uint32_t sv = 0x2u; uint32_t da = ctx->a[6] + (uint32_t)7; uint32_t r = alu(ctx, OP_SUB, ld8(da), sv, 8, 1); st8(da, r); } } hle_insn(ctx, 16); /* 00AEDC subq.b #0x2,(0x7,a6) */
L_00AEE0:
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF9E0Au; uint32_t r = alu(ctx, OP_ADD, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 00AEE0 addq.w #0x1,(0xffff9e0a).l */
	hle_insn(ctx, 16); return; /* 00AEE6 rts */
}
/* sub_00B488 */
static void u_00B488(hle_ctx *ctx)
{
	{ { uint32_t da = ctx->a[6]; uint8_t v = ld8(da); cc_set(ctx, CC_Z, (v & 0x20) ? 0 : CC_Z); st8(da, v & 0xDF); } } hle_insn(ctx, 16); /* 00B488 bclr.b #0xd,(a6) */
	{ { uint32_t v = 0xB3C8u; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 00B48C move.l #0xb3c8,(0x20,a6) */
	hle_insn(ctx, 18); Random(ctx); /* 00B494 jsr (Random,pc) */
	{ { uint32_t sv = 0x3Fu; uint32_t r = ((ctx->d[0] & 0xFFFF) & sv) & 0xFFFFu; set_w(&ctx->d[0], r); cc_logic16(ctx, r); } } hle_insn(ctx, 8); /* 00B498 andi.w #0x3f,d0 */
	{ { uint32_t sv = 0x40u; uint32_t r = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[0], r); } } hle_insn(ctx, 8); /* 00B49C addi.w #0x40,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 00B4A0 move.w d0,(0x24,a6) */
	hle_insn(ctx, 16); return; /* 00B4A4 rts */
}
/* sub_00B772 */
static void u_00B772(hle_ctx *ctx)
{
	{ { uint32_t v = 0xB6D0u; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 00B772 move.l #0xb6d0,(0x20,a6) */
	hle_insn(ctx, 18); Random(ctx); /* 00B77A jsr (Random,pc) */
	{ { uint32_t sv = 0x3Fu; uint32_t r = ((ctx->d[0] & 0xFFFF) & sv) & 0xFFFFu; set_w(&ctx->d[0], r); cc_logic16(ctx, r); } } hle_insn(ctx, 8); /* 00B77E andi.w #0x3f,d0 */
	{ { uint32_t sv = 0x96u; uint32_t r = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[0], r); } } hle_insn(ctx, 8); /* 00B782 addi.w #0x96,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 00B786 move.w d0,(0x24,a6) */
	hle_insn(ctx, 16); return; /* 00B78A rts */
}
/* sub_00B846 */
static void u_00B846(hle_ctx *ctx)
{
	{ { uint32_t v = 0xB796u; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 00B846 move.l #0xb796,(0x20,a6) */
	hle_insn(ctx, 18); Random(ctx); /* 00B84E jsr (Random,pc) */
	{ { uint32_t sv = 0x3Fu; uint32_t r = ((ctx->d[0] & 0xFFFF) & sv) & 0xFFFFu; set_w(&ctx->d[0], r); cc_logic16(ctx, r); } } hle_insn(ctx, 8); /* 00B852 andi.w #0x3f,d0 */
	{ { uint32_t sv = 0x80u; uint32_t r = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[0], r); } } hle_insn(ctx, 8); /* 00B856 addi.w #0x80,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 00B85A move.w d0,(0x24,a6) */
	hle_insn(ctx, 16); return; /* 00B85E rts */
}
/* sub_00C652 */
static void u_00C652(hle_ctx *ctx)
{
	{ { uint32_t v = 0xB586u; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 00C652 move.l #0xb586,(0x20,a6) */
	hle_insn(ctx, 18); Random(ctx); /* 00C65A jsr (Random,pc) */
	{ { uint32_t sv = 0x3Fu; uint32_t r = ((ctx->d[0] & 0xFFFF) & sv) & 0xFFFFu; set_w(&ctx->d[0], r); cc_logic16(ctx, r); } } hle_insn(ctx, 8); /* 00C65E andi.w #0x3f,d0 */
	{ { uint32_t sv = 0x40u; uint32_t r = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[0], r); } } hle_insn(ctx, 8); /* 00C662 addi.w #0x40,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 00C666 move.w d0,(0x24,a6) */
	hle_insn(ctx, 16); return; /* 00C66A rts */
}
/* TruckDamageInit */
static void u_00D30A(hle_ctx *ctx)
{
	{ { uint32_t da = 0xFFFF9E92u; st16(da, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } } hle_insn(ctx, 20); /* 00D30A clr.w (0xffff9e92).l */
	{ ctx->a[1] = 0xFFFFAE84u; } hle_insn(ctx, 12); /* 00D310 lea.l (0xffffae84).l,a1 */
	{ ctx->d[0] = 0xFu; cc_logic32(ctx, 0xFu); } hle_insn(ctx, 4); /* 00D316 moveq #0xf,d0 */
	{ ctx->d[1] = 0x1u; cc_logic32(ctx, 0x1u); } hle_insn(ctx, 4); /* 00D318 moveq #0x1,d1 */
L_00D31A:
	{ { uint32_t v = (ctx->d[1] & 0xFF); uint32_t da = ctx->a[1]; ctx->a[1] += 1; st8(da, v); cc_logic8(ctx, v); } } hle_insn(ctx, 8); /* 00D31A move.b d1,(a1)+ */
	{ uint16_t c = (uint16_t)(ctx->d[0] - 1); set_w(&ctx->d[0], c); if (c != 0xFFFF) { hle_insn(ctx, 10); goto L_00D31A; } hle_insn(ctx, 14); } /* 00D31C dbra d0,loc_00D31A */
	hle_insn(ctx, 16); return; /* 00D320 rts */
}
/* sub_00EE88 */
static void u_00EE88(hle_ctx *ctx)
{
	{ { uint32_t v = 0xEDD4u; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 00EE88 move.l #0xedd4,(0x20,a6) */
	hle_insn(ctx, 18); Random(ctx); /* 00EE90 jsr (Random,pc) */
	{ { uint32_t sv = 0x3Fu; uint32_t r = ((ctx->d[0] & 0xFFFF) & sv) & 0xFFFFu; set_w(&ctx->d[0], r); cc_logic16(ctx, r); } } hle_insn(ctx, 8); /* 00EE94 andi.w #0x3f,d0 */
	{ { uint32_t sv = 0x50u; uint32_t r = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[0], r); } } hle_insn(ctx, 8); /* 00EE98 addi.w #0x50,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 00EE9C move.w d0,(0x24,a6) */
	hle_insn(ctx, 16); return; /* 00EEA0 rts */
}
/* sub_00F170 */
static void u_00F170(hle_ctx *ctx)
{
	{ { uint32_t v = 0xEFA4u; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 00F170 move.l #0xefa4,(0x20,a6) */
	hle_insn(ctx, 18); Random(ctx); /* 00F178 jsr (Random,pc) */
	{ { uint32_t sv = 0x3Fu; uint32_t r = ((ctx->d[0] & 0xFFFF) & sv) & 0xFFFFu; set_w(&ctx->d[0], r); cc_logic16(ctx, r); } } hle_insn(ctx, 8); /* 00F17C andi.w #0x3f,d0 */
	{ { uint32_t sv = 0x50u; uint32_t r = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[0], r); } } hle_insn(ctx, 8); /* 00F180 addi.w #0x50,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 00F184 move.w d0,(0x24,a6) */
	hle_insn(ctx, 16); return; /* 00F188 rts */
}
/* sub_00F356 */
static void u_00F356(hle_ctx *ctx)
{
	{ { uint32_t v = 0xF18Au; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 00F356 move.l #0xf18a,(0x20,a6) */
	hle_insn(ctx, 18); Random(ctx); /* 00F35E jsr (Random,pc) */
	{ { uint32_t sv = 0x3Fu; uint32_t r = ((ctx->d[0] & 0xFFFF) & sv) & 0xFFFFu; set_w(&ctx->d[0], r); cc_logic16(ctx, r); } } hle_insn(ctx, 8); /* 00F362 andi.w #0x3f,d0 */
	{ { uint32_t sv = 0x50u; uint32_t r = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[0], r); } } hle_insn(ctx, 8); /* 00F366 addi.w #0x50,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 00F36A move.w d0,(0x24,a6) */
	hle_insn(ctx, 16); return; /* 00F36E rts */
}
/* sub_00FD0C */
static void u_00FD0C(hle_ctx *ctx)
{
	{ { uint32_t v = 0xFBAAu; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 00FD0C move.l #0xfbaa,(0x20,a6) */
	hle_insn(ctx, 18); Random(ctx); /* 00FD14 jsr (Random,pc) */
	{ { uint32_t sv = 0x3Fu; uint32_t r = ((ctx->d[0] & 0xFFFF) & sv) & 0xFFFFu; set_w(&ctx->d[0], r); cc_logic16(ctx, r); } } hle_insn(ctx, 8); /* 00FD18 andi.w #0x3f,d0 */
	{ { uint32_t sv = 0x50u; uint32_t r = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[0], r); } } hle_insn(ctx, 8); /* 00FD1C addi.w #0x50,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 00FD20 move.w d0,(0x24,a6) */
	hle_insn(ctx, 16); return; /* 00FD24 rts */
}
/* sub_00FFC0 */
static void u_00FFC0(hle_ctx *ctx)
{
	{ { uint32_t v = 0xFF12u; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 00FFC0 move.l #0xff12,(0x20,a6) */
	hle_insn(ctx, 18); Random(ctx); /* 00FFC8 jsr (Random,pc) */
	{ { uint32_t sv = 0x3Fu; uint32_t r = ((ctx->d[0] & 0xFFFF) & sv) & 0xFFFFu; set_w(&ctx->d[0], r); cc_logic16(ctx, r); } } hle_insn(ctx, 8); /* 00FFCC andi.w #0x3f,d0 */
	{ { uint32_t sv = 0x50u; uint32_t r = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[0], r); } } hle_insn(ctx, 8); /* 00FFD0 addi.w #0x50,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 00FFD4 move.w d0,(0x24,a6) */
	hle_insn(ctx, 16); return; /* 00FFD8 rts */
}
/* sub_010408 */
static void u_010408(hle_ctx *ctx)
{
	{ { uint32_t v = 0x10238u; uint32_t da = ctx->a[6] + (uint32_t)32; st32(da, v); cc_logic32(ctx, v); } } hle_insn(ctx, 24); /* 010408 move.l #0x10238,(0x20,a6) */
	hle_insn(ctx, 18); Random(ctx); /* 010410 jsr (Random,pc) */
	{ { uint32_t sv = 0x3Fu; uint32_t r = ((ctx->d[0] & 0xFFFF) & sv) & 0xFFFFu; set_w(&ctx->d[0], r); cc_logic16(ctx, r); } } hle_insn(ctx, 8); /* 010414 andi.w #0x3f,d0 */
	{ { uint32_t sv = 0x50u; uint32_t r = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[0], r); } } hle_insn(ctx, 8); /* 010418 addi.w #0x50,d0 */
	{ { uint32_t da = ctx->a[6]; uint8_t v = ld8(da); cc_set(ctx, CC_Z, (v & 0x40) ? 0 : CC_Z); } } hle_insn(ctx, 12); /* 01041C btst.b #0xe,(a6) */
	if (!c_z(ctx)) { hle_insn(ctx, 10); goto L_010426; } hle_insn(ctx, 8); /* 010420 bne.s loc_010426 */
	{ { uint32_t sv = 0x32u; uint32_t r = alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[0], r); } } hle_insn(ctx, 8); /* 010422 subi.w #0x32,d0 */
L_010426:
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = ctx->a[6] + (uint32_t)36; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 12); /* 010426 move.w d0,(0x24,a6) */
	hle_insn(ctx, 16); return; /* 01042A rts */
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
/* sub_012986 */
static void u_012986(hle_ctx *ctx)
{
	{ ctx->a[6] = 0xFFFF87B2u; } hle_insn(ctx, 12); /* 012986 lea.l (0xffff87b2).l,a6 */
	{ { uint32_t v = 0x4Eu; set_w(&ctx->d[2], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 01298C move.w #0x4e,d2 */
L_012990:
	{ { uint32_t sa = ctx->a[6]; uint32_t v = ld16(sa); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } } hle_insn(ctx, 8); /* 012990 move.w (a6),d0 */
	{ { uint32_t sv = 0x14u; (void)alu(ctx, OP_SUB, (ctx->d[0] & 0xFF), sv, 8, 0); } } hle_insn(ctx, 8); /* 012992 cmpi.b #0x14,d0 */
	if (c_z(ctx)) { hle_insn(ctx, 10); goto L_01299E; } hle_insn(ctx, 8); /* 012996 beq.s loc_01299E */
	{ { uint32_t sv = 0x13u; (void)alu(ctx, OP_SUB, (ctx->d[0] & 0xFF), sv, 8, 0); } } hle_insn(ctx, 8); /* 012998 cmpi.b #0x13,d0 */
	if (!c_z(ctx)) { hle_insn(ctx, 10); goto L_0129A6; } hle_insn(ctx, 8); /* 01299C bne.s loc_0129A6 */
L_01299E:
	{ { uint32_t da = ctx->a[6]; st16(da, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } } hle_insn(ctx, 12); /* 01299E clr.w (a6) */
	{ { uint32_t sv = 0x1u; uint32_t da = 0xFFFF80DAu; uint32_t r = alu(ctx, OP_SUB, ld16(da), sv, 16, 1); st16(da, r); } } hle_insn(ctx, 20); /* 0129A0 subq.w #0x1,(0xffff80da).l */
L_0129A6:
	{ { uint32_t sa = ctx->a[6] + (uint32_t)44; ctx->a[6] = sa; } } hle_insn(ctx, 8); /* 0129A6 lea.l (0x2c,a6),a6 */
	{ uint16_t c = (uint16_t)(ctx->d[2] - 1); set_w(&ctx->d[2], c); if (c != 0xFFFF) { hle_insn(ctx, 10); goto L_012990; } hle_insn(ctx, 14); } /* 0129AA dbra d2,loc_012990 */
	hle_insn(ctx, 16); return; /* 0129AE rts */
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
/* sub_0FBB6C */
static void u_0FBB6C(hle_ctx *ctx)
{
	{ { uint32_t da = 0xFFFF000Cu; cc_logic16(ctx, ld16(da)); } } hle_insn(ctx, 16); /* 0FBB6C tst.w (0xffff000c).l */
	if (!c_z(ctx)) { hle_insn(ctx, 10); goto L_0FBB94; } hle_insn(ctx, 8); /* 0FBB72 bne.s loc_0FBB94 */
	{ { uint32_t sv = 0x1u; uint32_t r = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFF), sv, 16, 1); set_w(&ctx->d[0], r); } } hle_insn(ctx, 4); /* 0FBB74 addq.w #0x1,d0 */
	{ { uint32_t v = (ctx->d[0] & 0xFFFF); uint32_t da = 0xFFFF000Eu; st16(da, v); cc_logic16(ctx, v); } } hle_insn(ctx, 16); /* 0FBB76 move.w d0,(0xffff000e).l */
	{ { uint32_t da = 0xFFFF00A0u; st8(da, 0xFF); } } hle_insn(ctx, 20); /* 0FBB7C st.b (0xffff00a0).l */
	{ { uint32_t da = 0xFFFF0072u; st8(da, 0); } } hle_insn(ctx, 20); /* 0FBB82 sf.b (0xffff0072).l */
	{ { uint32_t da = 0xFFFF002Eu; st8(da, 0); } } hle_insn(ctx, 20); /* 0FBB88 sf.b (0xffff002e).l */
	{ { uint32_t da = 0xFFFF009Au; st8(da, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } } hle_insn(ctx, 20); /* 0FBB8E clr.b (0xffff009a).l */
L_0FBB94:
	hle_insn(ctx, 16); return; /* 0FBB94 rts */
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
