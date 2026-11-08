/* Player input, shooting, damage and HUD routines (docs/gun.md, docs/subsystems/mainloop.md): the per-frame
 * PlayersButtons $00074C (PlayerButtons + PlayerShoot for P1 then P2), PlayerButtons $00076A (trigger edge,
 * autofire reload, gunpower drain, missile button, the Menacer's 2-frame debounce through $FFA0A7/$FF9F66),
 * PlayerShoot $00086C (autofire reload, shot sound and hit test, pickup shot test, impact sprite),
 * PickupShotTest $00700C (the pickups pool through PickupHitTable), SetPlayerState $008FA8,
 * PlayerDamageNearest $00909C and PlayerDamage $0090B2, the HUD EnergyHud $009104 and GunpowerHud $009186
 * (with their helpers sub_00920C/sub_009242 and GunpowerFirePeriod $0091C4) and the text printer
 * PrintText $007DEE (sub_007DEE; the entries sub_007DF6/E02/E08 stay 68k).
 *
 * Called from 68k and from C callers (hle_call68k, the nested dispatch then lands here). The bodies were
 * written instruction by instruction from src/t2.s (a comment on every line names the 68k instruction);
 * every instruction is on the timeline with the GPGX cost of its opcode word. ShotHitTest, PlayerButtons,
 * PlayerShoot, PickupShotTest, SetPlayerState and PlayerDamage are called as C after their jsr/bsr is
 * charged; every other callee (sounds, sub_012D00 impact, sub_01248C, sub_007C68, the sub_007B* state
 * sprites, sub_013D92, VramWriteWord for the text, sub_007EB2 for the bars, the PickupHitTable routines) runs
 * as 68k through hle_call68k. All VDP/Z80 access happens inside those 68k callees, so the timing is theirs.
 * Not pure (nested 68k): the decline bound of each is measured with T2_HLE_BOUNDS.
 */
#include "hle.h"

#include <stdint.h>

void ShotHitTest(hle_ctx *ctx);

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

/* conditional branches: charge, return whether taken */
static int bs(hle_ctx *c, int taken) { hle_insn(c, taken ? 10 : 8); return taken; }    /* Bcc.s */
static __attribute__((unused)) int bw(hle_ctx *c, int taken) { hle_insn(c, taken ? 10 : 12); return taken; }   /* Bcc.w */

/* work RAM or ROM by address (the code reads both: slot fields and the ROM tables) */
static inline int is_ram(uint32_t a) { return (a & 0xFFFFFFu) >= 0xE00000u; }
static inline uint8_t ld8(uint32_t a) { return is_ram(a) ? RAM8(a) : ROM8(a); }
static inline uint16_t ld16(uint32_t a) { return is_ram(a) ? RAM16(a) : ROM16(a); }
static inline uint32_t ld32(uint32_t a) { return is_ram(a) ? RAM32(a) : ROM32(a); }
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
	} else if (kind == 'o') {
		res = ((v << n) | (v >> (16 - n))) & 0xFFFF;
		if (res & 1) f |= CC_C;
		f |= (c->sr & CC_X);
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

/* mulu.w/muls.w #imm,Dn: 38 + 2n cycles (n = ones in the source / bit transitions) + 4 for the immediate */
static __attribute__((unused)) void mul_w(hle_ctx *c, int r, uint16_t src, int sign)
{
	uint32_t res, n = 0;
	if (sign) {
		res = (uint32_t)((int32_t)(int16_t)c->d[r] * (int32_t)(int16_t)src);
		uint32_t t = ((uint32_t)src << 1) & 0x1FFFF;
		for (int k = 0; k < 16; k++)
			if (((t >> k) & 1) != ((t >> (k + 1)) & 1)) n++;
	} else {
		res = (c->d[r] & 0xFFFFu) * (uint32_t)src;
		for (int k = 0; k < 16; k++) n += (src >> k) & 1;
	}
	c->d[r] = res;
	cc_set(c, CC_N | CC_Z | CC_V | CC_C, (res & 0x80000000u ? CC_N : 0) | (res ? 0 : CC_Z));
	hle_insn(c, 38 + 2 * n + 4);
}


void PlayersButtons(hle_ctx *ctx);
void PlayerButtons(hle_ctx *ctx);
void PlayerShoot(hle_ctx *ctx);
void PickupShotTest(hle_ctx *ctx);
void SetPlayerState(hle_ctx *ctx);
void PlayerDamageNearest(hle_ctx *ctx);
void PlayerDamage(hle_ctx *ctx);
void EnergyHud(hle_ctx *ctx);
void GunpowerHud(hle_ctx *ctx);
void PrintText(hle_ctx *ctx);
static void sub_00927A(hle_ctx *ctx);
static void sub_0091C4(hle_ctx *ctx);
static void sub_00920C(hle_ctx *ctx);
static void sub_009242(hle_ctx *ctx);

/* sub_00927A (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_00927A(hle_ctx *ctx)
{
	{ uint32_t _v = 0x4F4F4F4Fu; st32(ctx->a[0], _v); cc_logic32(ctx, _v); hle_insn(ctx, 20); } /* 00927A movel #1330597711,%a0@ */
	{ uint32_t _v = 0x4F4F4F4Fu; st32((ctx->a[0] + (uint32_t)(int32_t)4), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 009280 movel #1330597711,%a0@(4) */
	{ uint32_t _v = 0x0u; st8((ctx->a[0] + (uint32_t)(int32_t)8), _v); cc_logic8(ctx, _v); hle_insn(ctx, 16); } /* 009288 moveb #0,%a0@(8) */
	hle_insn(ctx, 16); return; /* 00928E rts */
}

/* sub_0091C4 (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_0091C4(hle_ctx *ctx)
{
	{ uint32_t _v = ld16((ctx->a[4] + (uint32_t)(int32_t)42)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0091C4 movew %a4@(42),%d0 */
	if (bs(ctx, c_z(ctx))) goto L_0091D0; /* 0091C8 beqs 0x91d0 */
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 0091CA addqw #1,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[4] + (uint32_t)(int32_t)42), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0091CC movew %d0,%a4@(42) */
L_0091D0:;
	{ uint32_t _v = ld16((ctx->a[4] + (uint32_t)(int32_t)44)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0091D0 movew %a4@(44),%d0 */
	if (bs(ctx, c_z(ctx))) goto L_0091E4; /* 0091D4 beqs 0x91e4 */
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 0091D6 addqw #1,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_0091E0; /* 0091D8 bnes 0x91e0 */
	{ uint32_t _v = 0x1u; st16((ctx->a[4] + (uint32_t)(int32_t)20), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 0091DA movew #1,%a4@(20) */
L_0091E0:;
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[4] + (uint32_t)(int32_t)44), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0091E0 movew %d0,%a4@(44) */
L_0091E4:;
	{ uint32_t _v = 0xC000u; set_w(&ctx->d[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0091E4 movew #-16384,%d6 */
	{ uint32_t _v = ld16((ctx->a[4] + (uint32_t)(int32_t)28)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0091E8 movew %a4@(28),%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 0091EC movew %d0,%d1 */
	{ sh_w(ctx, 1, 'o', 3); hle_insn(ctx, 12); } /* 0091EE rolw #3,%d1 */
	{ uint32_t _s = 0x7u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 0091F0 andiw #7,%d1 */
	{ uint32_t _s = 0x7u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = (_d ^ _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 0091F4 eoriw #7,%d1 */
	{ uint32_t _s = 0x2u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 4); } /* 0091F8 addqw #2,%d1 */
	{ uint32_t _s = 0x3u; uint32_t _da = (ctx->a[4] + (uint32_t)(int32_t)20); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 0091FA cmpiw #3,%a4@(20) */
	if (bs(ctx, !c_z(ctx))) goto L_009204; /* 009200 bnes 0x9204 */
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 4); } /* 009202 addqw #1,%d1 */
L_009204:;
	{ uint32_t _v = (ctx->d[1] & 0xFFFFu); st16((ctx->a[4] + (uint32_t)(int32_t)18), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 009204 movew %d1,%a4@(18) */
	{ sh_w(ctx, 0, 'r', 8); hle_insn(ctx, 22); } /* 009208 lsrw #8,%d0 */
	hle_insn(ctx, 16); return; /* 00920A rts */
}

/* sub_00920C (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_00920C(hle_ctx *ctx)
{
	{ cc_logic16(ctx, ld16((ctx->a[4] + (uint32_t)(int32_t)36))); hle_insn(ctx, 12); } /* 00920C tstw %a4@(36) */
	if (bs(ctx, c_n(ctx))) goto L_009240; /* 009210 bmis 0x9240 */
	{ sh_w(ctx, 0, 'r', 4); hle_insn(ctx, 14); } /* 009212 lsrw #4,%d0 */
	{ uint32_t _s = 0xFu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 009214 andiw #15,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 009218 movew %d0,%d1 */
	{ ctx->a[0] = 0xFFFFAF88u; hle_insn(ctx, 12); } /* 00921A lea 0xffffaf88,%a0 */
	{ hle_insn(ctx, 18); sub_00927A(ctx); } /* 009220 jsr %pc@(0x927a) */
	{ uint32_t _v = ctx->a[0]; ctx->a[1] = _v; hle_insn(ctx, 4); } /* 009224 moveal %a0,%a1 */
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 009226 addqw #1,%d0 */
	{ sh_w(ctx, 0, 'r', 1); hle_insn(ctx, 8); } /* 009228 lsrw #1,%d0 */
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 00922A subqw #1,%d0 */
	if (bs(ctx, c_n(ctx))) goto L_009234; /* 00922C bmis 0x9234 */
L_00922E:;
	{ uint32_t _v = (ctx->d[2] & 0xFFu); st8(ctx->a[1], _v); ctx->a[1] += 1; cc_logic8(ctx, _v); hle_insn(ctx, 8); } /* 00922E moveb %d2,%a1@+ */
	{ uint16_t _c = (uint16_t)ctx->d[0] - 1; set_w(&ctx->d[0], _c); if (_c != 0xFFFF) { hle_insn(ctx, 10); goto L_00922E; } hle_insn(ctx, 14); } /* 009230 dbf %d0,0x922e */
L_009234:;
	{ cc_set(ctx, CC_Z, ((ctx->d[1] >> 0) & 1) ? 0 : CC_Z); hle_insn(ctx, 10); } /* 009234 btst #0,%d1 */
	if (bs(ctx, !c_z(ctx))) goto L_00923C; /* 009238 bnes 0x923c */
	{ uint32_t _v = (ctx->d[4] & 0xFFu); st8(ctx->a[1], _v); cc_logic8(ctx, _v); hle_insn(ctx, 8); } /* 00923A moveb %d4,%a1@ */
L_00923C:;
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x007EB2); } /* 00923C jsr %pc@(0x7eb2) */
L_009240:;
	hle_insn(ctx, 16); return; /* 009240 rts */
}

/* sub_009242 (called after its jsr/bsr was charged; the rts is charged here) */
static void sub_009242(hle_ctx *ctx)
{
	{ cc_logic16(ctx, ld16((ctx->a[4] + (uint32_t)(int32_t)36))); hle_insn(ctx, 12); } /* 009242 tstw %a4@(36) */
	if (bs(ctx, c_n(ctx))) goto L_009278; /* 009246 bmis 0x9278 */
	{ sh_w(ctx, 0, 'r', 4); hle_insn(ctx, 14); } /* 009248 lsrw #4,%d0 */
	{ uint32_t _s = 0xFu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 00924A andiw #15,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 00924E movew %d0,%d1 */
	{ ctx->a[0] = 0xFFFFAF88u; hle_insn(ctx, 12); } /* 009250 lea 0xffffaf88,%a0 */
	{ hle_insn(ctx, 18); sub_00927A(ctx); } /* 009256 jsr %pc@(0x927a) */
	{ ctx->a[1] = (ctx->a[0] + (uint32_t)(int32_t)8); hle_insn(ctx, 8); } /* 00925A lea %a0@(8),%a1 */
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 00925E addqw #1,%d0 */
	{ sh_w(ctx, 0, 'r', 1); hle_insn(ctx, 8); } /* 009260 lsrw #1,%d0 */
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 009262 subqw #1,%d0 */
	if (bs(ctx, c_n(ctx))) goto L_00926C; /* 009264 bmis 0x926c */
L_009266:;
	{ uint32_t _v = (ctx->d[2] & 0xFFu); ctx->a[1] -= 1; st8(ctx->a[1], _v); cc_logic8(ctx, _v); hle_insn(ctx, 8); } /* 009266 moveb %d2,%a1@- */
	{ uint16_t _c = (uint16_t)ctx->d[0] - 1; set_w(&ctx->d[0], _c); if (_c != 0xFFFF) { hle_insn(ctx, 10); goto L_009266; } hle_insn(ctx, 14); } /* 009268 dbf %d0,0x9266 */
L_00926C:;
	{ cc_set(ctx, CC_Z, ((ctx->d[1] >> 0) & 1) ? 0 : CC_Z); hle_insn(ctx, 10); } /* 00926C btst #0,%d1 */
	if (bs(ctx, !c_z(ctx))) goto L_009274; /* 009270 bnes 0x9274 */
	{ uint32_t _v = (ctx->d[4] & 0xFFu); ctx->a[1] -= 1; st8(ctx->a[1], _v); cc_logic8(ctx, _v); hle_insn(ctx, 8); } /* 009272 moveb %d4,%a1@- */
L_009274:;
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x007EB2); } /* 009274 jsr %pc@(0x7eb2) */
L_009278:;
	hle_insn(ctx, 16); return; /* 009278 rts */
}

/* Early bail (docs/porting.md): these run in C up to the last point that is certain to come before the first
 * interrupt-raising slice end or the frame end; the 68k continues from the `jsr`/`bsr` that is next (every
 * register and flag is in ctx there; the callee runs as C again under its own hook when it fits).
 * Only the function the hook entered may bail: the C-to-C calls inside it (PlayersButtons -> PlayerButtons)
 * share the ctx and must not. Nothing charged yet = nothing changed: decline. */
#define PL_BAIL(self, chunk, pc) do { \
	if (ctx->def && ctx->def->addr == (self) && hle_bail_due(ctx, (chunk))) { \
		if (!ctx->tl_used) hle_decline_why(2); else hle_bail_to(ctx, (pc)); \
		return; } } while (0)

/* $00074C PlayerButtons + PlayerShoot for P1 then P2 (decline bound 46800 = observed max 31134 x 1.5, T2_HLE_BOUNDS + NODECLINE over 92 states x 3000 frames) */
HLE_FUNC_BAIL_YIELD(0x00074C, PlayersButtons, 46800)
{
	PL_BAIL(0x00074C, 600, 0x00074C);
	{ ctx->a[4] = 0xFFFF8000u; hle_insn(ctx, 12); } /* 00074C lea 0xffff8000,%a4 */
	PL_BAIL(0x00074C, 46800, 0x000752);
	{ hle_insn(ctx, 18); PlayerButtons(ctx); } /* 000752 bsrw 0x76a */
	PL_BAIL(0x00074C, 46800, 0x000756);
	{ hle_insn(ctx, 18); PlayerShoot(ctx); } /* 000756 jsr %pc@(0x86c) */
	{ ctx->a[4] = 0xFFFF8050u; hle_insn(ctx, 12); } /* 00075A lea 0xffff8050,%a4 */
	PL_BAIL(0x00074C, 46800, 0x000760);
	{ hle_insn(ctx, 18); PlayerButtons(ctx); } /* 000760 bsrw 0x76a */
	PL_BAIL(0x00074C, 46800, 0x000764);
	{ hle_insn(ctx, 18); PlayerShoot(ctx); } /* 000764 jsr %pc@(0x86c) */
	hle_insn(ctx, 16); return; /* 000768 rts */
}

/* $00076A trigger/autofire/gunpower drain/missile, Menacer debounce (decline bound 12100 = observed max 8040 x 1.5, T2_HLE_BOUNDS + NODECLINE over 92 states x 3000 frames) */
HLE_FUNC_BAIL_YIELD(0x00076A, PlayerButtons, 12100)
{
	PL_BAIL(0x00076A, 1200, 0x00076A);
	{ cc_logic16(ctx, ld16((ctx->a[4] + (uint32_t)(int32_t)36))); hle_insn(ctx, 12); } /* 00076A tstw %a4@(36) */
	if (bw(ctx, c_n(ctx))) goto L_00086A; /* 00076E bmiw 0x86a */
	{ uint32_t _v = ld16((ctx->a[4] + (uint32_t)(int32_t)0)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 000772 movew %a4@(0),%d0 */
	{ uint32_t _v = ld16((ctx->a[4] + (uint32_t)(int32_t)2)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 000776 movew %a4@(2),%d1 */
	{ cc_set(ctx, CC_Z, ((ctx->d[0] >> 6) & 1) ? 0 : CC_Z); hle_insn(ctx, 10); } /* 00077A btst #6,%d0 */
	if (bw(ctx, c_z(ctx))) goto L_0007B2; /* 00077E beqw 0x7b2 */
	{ cc_logic16(ctx, ld16((ctx->a[4] + (uint32_t)(int32_t)42))); hle_insn(ctx, 12); } /* 000782 tstw %a4@(42) */
	if (bs(ctx, c_n(ctx))) goto L_000794; /* 000786 bmis 0x794 */
	{ uint32_t _s = 0xC0u; uint32_t _da = (ctx->a[4] + (uint32_t)(int32_t)28); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 000788 subiw #192,%a4@(28) */
	if (bs(ctx, !c_c(ctx))) goto L_000794; /* 00078E bccs 0x794 */
	{ st16((ctx->a[4] + (uint32_t)(int32_t)28), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 000790 clrw %a4@(28) */
L_000794:;
	{ cc_set(ctx, CC_Z, ((ctx->d[1] >> 6) & 1) ? 0 : CC_Z); hle_insn(ctx, 10); } /* 000794 btst #6,%d1 */
	if (bw(ctx, c_z(ctx))) goto L_0007A4; /* 000798 beqw 0x7a4 */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[4] + (uint32_t)(int32_t)16); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00079C subqw #1,%a4@(16) */
	if (bw(ctx, !c_z(ctx))) goto L_0007B2; /* 0007A0 bnew 0x7b2 */
L_0007A4:;
	{ st8(0xFFFFA0A2u, 0xFF); hle_insn(ctx, 20); } /* 0007A4 st 0xffffa0a2 */
	{ uint32_t _v = ld16((ctx->a[4] + (uint32_t)(int32_t)0)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0007AA movew %a4@(0),%d0 */
	{ uint32_t _v = ld16((ctx->a[4] + (uint32_t)(int32_t)2)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0007AE movew %a4@(2),%d1 */
L_0007B2:;
	{ uint32_t _s = 0x4u; uint32_t _da = 0xFFFF9DF5u; uint32_t _d = ld8(_da); alu(ctx, OP_SUB, _d, _s, 8, 0); hle_insn(ctx, 20); } /* 0007B2 cmpib #4,0xffff9df5 */
	if (bw(ctx, c_z(ctx))) goto L_0007CE; /* 0007BA beqw 0x7ce */
	{ uint32_t _s = 0x0u; uint32_t _da = 0xFFFF9DF5u; uint32_t _d = ld8(_da); alu(ctx, OP_SUB, _d, _s, 8, 0); hle_insn(ctx, 20); } /* 0007BE cmpib #0,0xffff9df5 */
	if (bs(ctx, !c_z(ctx))) goto L_0007DE; /* 0007C6 bnes 0x7de */
	{ cc_logic16(ctx, ld16((ctx->a[4] + (uint32_t)(int32_t)36))); hle_insn(ctx, 12); } /* 0007C8 tstw %a4@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_0007DE; /* 0007CC bnes 0x7de */
L_0007CE:;
	{ cc_logic8(ctx, ld8(0xFFFFA0A7u)); hle_insn(ctx, 16); } /* 0007CE tstb 0xffffa0a7 */
	if (bs(ctx, !c_z(ctx))) goto L_0007DE; /* 0007D4 bnes 0x7de */
	{ uint32_t _s = (ctx->d[1] & 0xFFFFu); uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 0007D6 andw %d1,%d0 */
	{ uint32_t _v = ld16(0xFFFF9F66u); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 0007D8 movew 0xffff9f66,%d1 */
L_0007DE:;
	{ cc_set(ctx, CC_Z, ((ctx->d[0] >> 4) & 1) ? 0 : CC_Z); hle_insn(ctx, 10); } /* 0007DE btst #4,%d0 */
	if (bw(ctx, c_z(ctx))) goto L_00086A; /* 0007E2 beqw 0x86a */
	{ cc_set(ctx, CC_Z, ((ctx->d[1] >> 4) & 1) ? 0 : CC_Z); hle_insn(ctx, 10); } /* 0007E6 btst #4,%d1 */
	if (bw(ctx, !c_z(ctx))) goto L_00086A; /* 0007EA bnew 0x86a */
	{ cc_logic16(ctx, ld16((ctx->a[4] + (uint32_t)(int32_t)24))); hle_insn(ctx, 12); } /* 0007EE tstw %a4@(24) */
	if (bs(ctx, !c_z(ctx))) goto L_000802; /* 0007F2 bnes 0x802 */
	{ uint32_t _v = 0x77u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0007F4 movew #119,%d0 */
	PL_BAIL(0x00076A, 12100, 0x0007F8);
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_callc(ctx, 0x013D04); } /* 0007F8 jsr 0x13d04 */
	hle_insn(ctx, 10); goto L_00086A; /* 0007FE braw 0x86a */
L_000802:;
	{ uint32_t _v = ld16((ctx->a[4] + (uint32_t)(int32_t)22)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 000802 movew %a4@(22),%d2 */
	if (bs(ctx, c_z(ctx))) goto L_00086A; /* 000806 beqs 0x86a */
	{ uint32_t _s = 0x5u; uint32_t _d = (ctx->d[2] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 000808 cmpiw #5,%d2 */
	if (bs(ctx, !c_z(ctx))) goto L_000832; /* 00080C bnes 0x832 */
	{ ctx->d[0] = 0x67u; cc_logic32(ctx, ctx->d[0]); hle_insn(ctx, 4); } /* 00080E moveq #103,%d0 */
	{ uint32_t _s = 0x7u; uint32_t _da = 0xFFFF80CCu; uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 20); } /* 000810 cmpiw #7,0xffff80cc */
	if (bs(ctx, !c_ge(ctx))) goto L_000834; /* 000818 blts 0x834 */
	{ uint32_t _v = ld32((ctx->a[4] + (uint32_t)(int32_t)4)); st32(0xFFFF9E94u, _v); cc_logic32(ctx, _v); hle_insn(ctx, 32); } /* 00081A movel %a4@(4),0xffff9e94 */
	{ uint32_t _v = 0x1Eu; st16(0xFFFFA0CAu, _v); cc_logic16(ctx, _v); hle_insn(ctx, 20); } /* 000822 movew #30,0xffffa0ca */
	PL_BAIL(0x00076A, 12100, 0x00082A);
	{ hle_insn(ctx, 18); ShotHitTest(ctx); } /* 00082A jsr %pc@(0x5892) */
	{ ctx->d[0] = 0x67u; cc_logic32(ctx, ctx->d[0]); hle_insn(ctx, 4); } /* 00082E moveq #103,%d0 */
	hle_insn(ctx, 10); goto L_000834; /* 000830 bras 0x834 */
L_000832:;
	{ ctx->d[0] = 0x6Eu; cc_logic32(ctx, ctx->d[0]); hle_insn(ctx, 4); } /* 000832 moveq #110,%d0 */
L_000834:;
	PL_BAIL(0x00076A, 12100, 0x000834);
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_callc(ctx, 0x013CC4); } /* 000834 jsr 0x13cc4 */
	PL_BAIL(0x00076A, 12100, 0x00083A);
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x007C68); } /* 00083A jsr %pc@(0x7c68) */
	{ uint32_t _v = ld16((ctx->a[4] + (uint32_t)(int32_t)22)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00083E movew %a4@(22),%d2 */
	{ uint32_t _v = ld16((ctx->a[4] + (uint32_t)(int32_t)4)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 000842 movew %a4@(4),%d0 */
	{ uint32_t _v = ld16((ctx->a[4] + (uint32_t)(int32_t)6)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 000846 movew %a4@(6),%d1 */
	PL_BAIL(0x00076A, 12100, 0x00084A);
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_callc(ctx, 0x012D00); } /* 00084A jsr 0x12d00 */
	{ uint32_t _s = 0x6u; uint32_t _da = (ctx->a[4] + (uint32_t)(int32_t)22); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 000850 cmpiw #6,%a4@(22) */
	if (bs(ctx, !c_z(ctx))) goto L_00086A; /* 000856 bnes 0x86a */
	{ cc_logic16(ctx, ld16((ctx->a[4] + (uint32_t)(int32_t)24))); hle_insn(ctx, 12); } /* 000858 tstw %a4@(24) */
	if (bs(ctx, !c_z(ctx))) goto L_00086A; /* 00085C bnes 0x86a */
	{ uint32_t _v = ld16((ctx->a[4] + (uint32_t)(int32_t)52)); st16((ctx->a[4] + (uint32_t)(int32_t)24), _v); cc_logic16(ctx, _v); hle_insn(ctx, 20); } /* 00085E movew %a4@(52),%a4@(24) */
	{ uint32_t _v = 0x5u; st16((ctx->a[4] + (uint32_t)(int32_t)22), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 000864 movew #5,%a4@(22) */
L_00086A:;
	hle_insn(ctx, 16); return; /* 00086A rts */
}

/* $00086C autofire reload, shot sound, ShotHitTest, impact sprite (decline bound 29800 = observed max 19844 x 1.5, T2_HLE_BOUNDS + NODECLINE over 92 states x 3000 frames) */
HLE_FUNC_BAIL_YIELD(0x00086C, PlayerShoot, 29800)
{
	PL_BAIL(0x00086C, 1200, 0x00086C);
	{ cc_logic8(ctx, ld8(0xFFFFA0A2u)); hle_insn(ctx, 16); } /* 00086C tstb 0xffffa0a2 */
	if (bw(ctx, c_z(ctx))) goto L_0008FE; /* 000872 beqw 0x8fe */
	{ st8(0xFFFFA0A2u, 0); hle_insn(ctx, 20); } /* 000876 sf 0xffffa0a2 */
	{ uint32_t _v = ld16((ctx->a[4] + (uint32_t)(int32_t)18)); st16((ctx->a[4] + (uint32_t)(int32_t)16), _v); cc_logic16(ctx, _v); hle_insn(ctx, 20); } /* 00087C movew %a4@(18),%a4@(16) */
	{ uint32_t _v = ld16((ctx->a[4] + (uint32_t)(int32_t)20)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 000882 movew %a4@(20),%d2 */
	if (bw(ctx, c_z(ctx))) goto L_0008D2; /* 000886 beqw 0x8d2 */
	{ uint32_t _v = ld16((ctx->a[4] + (uint32_t)(int32_t)4)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00088A movew %a4@(4),%d0 */
	{ uint32_t _v = ld16((ctx->a[4] + (uint32_t)(int32_t)6)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00088E movew %a4@(6),%d1 */
	PL_BAIL(0x00086C, 29800, 0x000892);
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_callc(ctx, 0x012D00); } /* 000892 jsr 0x12d00 */
	{ uint32_t _v = 0x5Eu; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 000898 movew #94,%d0 */
	PL_BAIL(0x00086C, 29800, 0x00089C);
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_callc(ctx, 0x013D04); } /* 00089C jsr 0x13d04 */
	{ uint32_t _v = ld32((ctx->a[4] + (uint32_t)(int32_t)4)); st32(0xFFFF9E94u, _v); cc_logic32(ctx, _v); hle_insn(ctx, 32); } /* 0008A2 movel %a4@(4),0xffff9e94 */
	PL_BAIL(0x00086C, 29800, 0x0008AA);
	{ hle_insn(ctx, 18); PickupShotTest(ctx); } /* 0008AA jsr %pc@(0x700c) */
	{ uint32_t _v = ld16((ctx->a[4] + (uint32_t)(int32_t)20)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0008AE movew %a4@(20),%d2 */
	{ uint32_t _s = (ctx->d[2] & 0xFFFFu); uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 4); } /* 0008B2 addw %d2,%d2 */
	{ uint32_t _v = (ctx->d[2] & 0xFFFFu); st16(0xFFFFA0CAu, _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 0008B4 movew %d2,0xffffa0ca */
	PL_BAIL(0x00086C, 29800, 0x0008BA);
	{ hle_insn(ctx, 18); ShotHitTest(ctx); } /* 0008BA jsr %pc@(0x5892) */
	{ cc_logic16(ctx, (ctx->d[5] & 0xFFFFu)); hle_insn(ctx, 4); } /* 0008BE tstw %d5 */
	if (bw(ctx, c_z(ctx))) goto L_0008D2; /* 0008C0 beqw 0x8d2 */
	{ uint32_t _a = (ctx->a[4] + (uint32_t)(int32_t)4); ctx->d[0] = (uint32_t)(int32_t)(int16_t)ld16(_a + 0); ctx->d[1] = (uint32_t)(int32_t)(int16_t)ld16(_a + 2); hle_insn(ctx, 24); } /* 0008C4 movemw %a4@(4),%d0-%d1 */
	{ ctx->d[2] = 0x7u; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 0008CA moveq #7,%d2 */
	PL_BAIL(0x00086C, 29800, 0x0008CC);
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_callc(ctx, 0x01248C); } /* 0008CC jsr 0x1248c */
L_0008D2:;
	{ uint32_t _s = 0xAu; uint32_t _da = 0xFFFF80CCu; uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 20); } /* 0008D2 cmpiw #10,0xffff80cc */
	if (bs(ctx, !c_z(ctx))) goto L_0008FE; /* 0008DA bnes 0x8fe */
	{ uint32_t _v = ld32((ctx->a[4] + (uint32_t)(int32_t)4)); st32(0xFFFF9E94u, _v); cc_logic32(ctx, _v); hle_insn(ctx, 32); } /* 0008DC movel %a4@(4),0xffff9e94 */
	PL_BAIL(0x00086C, 29800, 0x0008E4);
	{ hle_insn(ctx, 18); ShotHitTest(ctx); } /* 0008E4 jsr %pc@(0x5892) */
	{ cc_logic16(ctx, (ctx->d[5] & 0xFFFFu)); hle_insn(ctx, 4); } /* 0008E8 tstw %d5 */
	if (bw(ctx, c_z(ctx))) goto L_0008FE; /* 0008EA beqw 0x8fe */
	{ uint32_t _v = 0x32u; st16((ctx->a[4] + (uint32_t)(int32_t)16), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 0008EE movew #50,%a4@(16) */
	{ uint32_t _v = 0x5Eu; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0008F4 movew #94,%d0 */
	PL_BAIL(0x00086C, 29800, 0x0008F8);
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_callc(ctx, 0x013D04); } /* 0008F8 jsr 0x13d04 */
L_0008FE:;
	hle_insn(ctx, 16); return; /* 0008FE rts */
}

/* $00700C pickups pool shot test through PickupHitTable (decline bound 3300 = observed max 2176 x 1.5, T2_HLE_BOUNDS + NODECLINE over 92 states x 3000 frames) */
HLE_FUNC_YIELD(0x00700C, PickupShotTest, 3300)
{
	{ ctx->a[6] = 0xFFFF9AB2u; hle_insn(ctx, 12); } /* 00700C lea 0xffff9ab2,%fp */
	{ uint32_t _v = ld16(0xFFFF80E0u); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 007012 movew 0xffff80e0,%d7 */
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[7] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[7], _r); hle_insn(ctx, 4); } /* 007018 subqw #1,%d7 */
	if (bw(ctx, c_n(ctx))) goto L_007046; /* 00701A bmiw 0x7046 */
L_00701E:;
	{ uint32_t _v = ld16(ctx->a[6]); set_w(&ctx->d[5], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00701E movew %fp@,%d5 */
	if (bs(ctx, c_z(ctx))) goto L_007080; /* 007020 beqs 0x7080 */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 3) & 1) ? 0 : CC_Z); hle_insn(ctx, 12); } /* 007022 btst #3,%fp@ */
	if (bs(ctx, c_z(ctx))) goto L_00703E; /* 007026 beqs 0x703e */
	{ uint32_t _s = 0x19u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)22); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 007028 cmpiw #25,%fp@(22) */
	if (bs(ctx, !c_ge(ctx))) goto L_00703E; /* 00702E blts 0x703e */
	{ uint32_t _v = (ctx->d[7] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 007030 movew %d7,%sp@- */
	{ set_w(&ctx->d[5], (uint32_t)(int32_t)(int8_t)ctx->d[5]); cc_logic16(ctx, ctx->d[5]); hle_insn(ctx, 4); } /* 007032 extw %d5 */
	{ sh_w(ctx, 5, 'l', 2); hle_insn(ctx, 10); } /* 007034 aslw #2,%d5 */
	{ uint32_t _v = ld32((0x007048u + (uint32_t)((int32_t)(int16_t)ctx->d[5]))); ctx->a[1] = _v; hle_insn(ctx, 18); } /* 007036 moveal %pc@(0x7048,%d5:w),%a1 */
	{ hle_insn(ctx, 16); hle_sync(ctx); hle_callc(ctx, ctx->a[1]); } /* 00703A jsr %a1@ */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00703C movew %sp@+,%d7 */
L_00703E:;
	{ ctx->a[6] = (ctx->a[6] + (uint32_t)(int32_t)26); hle_insn(ctx, 8); } /* 00703E lea %fp@(26),%fp */
	{ uint16_t _c = (uint16_t)ctx->d[7] - 1; set_w(&ctx->d[7], _c); if (_c != 0xFFFF) { hle_insn(ctx, 10); goto L_00701E; } hle_insn(ctx, 14); } /* 007042 dbf %d7,0x701e */
L_007046:;
	hle_insn(ctx, 16); return; /* 007046 rts */
L_007080:;
	{ ctx->a[6] = (ctx->a[6] + (uint32_t)(int32_t)26); hle_insn(ctx, 8); } /* 007080 lea %fp@(26),%fp */
	hle_insn(ctx, 10); goto L_00701E; /* 007084 bras 0x701e */
}

/* $008FA8 a4 = player, d0 = state (decline bound 10200 = observed max 6784 x 1.5, T2_HLE_BOUNDS + NODECLINE over 92 states x 3000 frames) */
HLE_FUNC_YIELD(0x008FA8, SetPlayerState, 10200)
{
	{ uint32_t _v = ld16((ctx->a[4] + (uint32_t)(int32_t)34)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 008FA8 movew %a4@(34),%d1 */
	{ uint32_t _s = 0x2u; uint32_t _d = (ctx->d[1] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 008FAC cmpiw #2,%d1 */
	if (bs(ctx, c_z(ctx))) goto L_00902C; /* 008FB0 beqs 0x902c */
	{ uint32_t _v = 0xC8u; st16((ctx->a[4] + (uint32_t)(int32_t)32), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 008FB2 movew #200,%a4@(32) */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[4] + (uint32_t)(int32_t)34), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 008FB8 movew %d0,%a4@(34) */
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 008FBC cmpiw #1,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_008FC8; /* 008FC0 bnes 0x8fc8 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x007B64); } /* 008FC2 jsr %pc@(0x7b64) */
	hle_insn(ctx, 10); goto L_00902C; /* 008FC6 bras 0x902c */
L_008FC8:;
	{ uint32_t _s = 0x2u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 008FC8 cmpiw #2,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_008FEC; /* 008FCC bnes 0x8fec */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x007B3C); } /* 008FCE jsr %pc@(0x7b3c) */
	{ st16((ctx->a[4] + (uint32_t)(int32_t)20), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 008FD2 clrw %a4@(20) */
	{ st16((ctx->a[4] + (uint32_t)(int32_t)22), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 16); } /* 008FD6 clrw %a4@(22) */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF9F6Du; uint32_t _d = ld8(_da); alu(ctx, OP_SUB, _d, _s, 8, 0); hle_insn(ctx, 20); } /* 008FDA cmpib #1,0xffff9f6d */
	if (bs(ctx, !c_z(ctx))) goto L_00902C; /* 008FE2 bnes 0x902c */
	{ uint32_t _v = 0x1u; st16((ctx->a[4] + (uint32_t)(int32_t)32), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 008FE4 movew #1,%a4@(32) */
	hle_insn(ctx, 10); goto L_00902C; /* 008FEA bras 0x902c */
L_008FEC:;
	{ uint32_t _s = 0x3u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 008FEC cmpiw #3,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_008FFC; /* 008FF0 bnes 0x8ffc */
	{ uint32_t _v = 0x46u; st16((ctx->a[4] + (uint32_t)(int32_t)32), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 008FF2 movew #70,%a4@(32) */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x007B28); } /* 008FF8 jsr %pc@(0x7b28) */
L_008FFC:;
	{ uint32_t _s = 0x4u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 008FFC cmpiw #4,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_00900C; /* 009000 bnes 0x900c */
	{ uint32_t _v = 0xFFFFu; st16((ctx->a[4] + (uint32_t)(int32_t)32), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 009002 movew #-1,%a4@(32) */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x007BB8); } /* 009008 jsr %pc@(0x7bb8) */
L_00900C:;
	{ uint32_t _s = 0x5u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 00900C cmpiw #5,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_00901C; /* 009010 bnes 0x901c */
	{ uint32_t _v = 0x708u; st16((ctx->a[4] + (uint32_t)(int32_t)32), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 009012 movew #1800,%a4@(32) */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x007B50); } /* 009018 jsr %pc@(0x7b50) */
L_00901C:;
	{ uint32_t _s = 0x6u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 00901C cmpiw #6,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_00902C; /* 009020 bnes 0x902c */
	{ uint32_t _v = 0xB4u; st16((ctx->a[4] + (uint32_t)(int32_t)32), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 009022 movew #180,%a4@(32) */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_callc(ctx, 0x007B14); } /* 009028 jsr %pc@(0x7b14) */
L_00902C:;
	hle_insn(ctx, 16); return; /* 00902C rts */
}

/* $00909C d2 = energy loss for the player on the object side (decline bound 14800 = observed max 9828 x 1.5, T2_HLE_BOUNDS + NODECLINE over 92 states x 3000 frames) */
HLE_FUNC_YIELD(0x00909C, PlayerDamageNearest, 14800)
{
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00909C movew %fp@(2),%d0 */
	{ ctx->a[4] = 0xFFFF8000u; hle_insn(ctx, 12); } /* 0090A0 lea 0xffff8000,%a4 */
	{ uint32_t _s = 0xA0u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 0090A6 cmpiw #160,%d0 */
	if (bs(ctx, c_le(ctx))) { PlayerDamage(ctx); return; } /* 0090AA bles 0x90b2 */
	{ ctx->a[4] = 0xFFFF8050u; hle_insn(ctx, 12); } /* 0090AC lea 0xffff8050,%a4 */
	PlayerDamage(ctx); return; /* falls into PlayerDamage */
}

/* $0090B2 a4 = player, d2 = energy loss (decline bound 14700 = observed max 9780 x 1.5, T2_HLE_BOUNDS + NODECLINE over 92 states x 3000 frames) */
HLE_FUNC_YIELD(0x0090B2, PlayerDamage, 14700)
{
	{ cc_logic16(ctx, ld16((ctx->a[4] + (uint32_t)(int32_t)46))); hle_insn(ctx, 12); } /* 0090B2 tstw %a4@(46) */
	if (bs(ctx, c_n(ctx))) goto L_009102; /* 0090B6 bmis 0x9102 */
	{ cc_logic16(ctx, ld16((ctx->a[4] + (uint32_t)(int32_t)36))); hle_insn(ctx, 12); } /* 0090B8 tstw %a4@(36) */
	if (bs(ctx, c_n(ctx))) goto L_009102; /* 0090BC bmis 0x9102 */
	{ uint32_t _v = ld16((ctx->a[4] + (uint32_t)(int32_t)30)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0090BE movew %a4@(30),%d0 */
	{ uint32_t _s = (ctx->d[2] & 0xFFFFu); uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 0090C2 subw %d2,%d0 */
	if (bs(ctx, c_c(ctx))) goto L_0090EC; /* 0090C4 bcss 0x90ec */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[4] + (uint32_t)(int32_t)30), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0090C6 movew %d0,%a4@(30) */
	{ uint32_t _v = ld16((ctx->a[4] + (uint32_t)(int32_t)48)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0090CA movew %a4@(48),%d1 */
	{ cc_set(ctx, CC_Z, ((ctx->d[1] >> 15) & 1) ? 0 : CC_Z); hle_insn(ctx, 10); } /* 0090CE btst #15,%d1 */
	if (bs(ctx, !c_z(ctx))) goto L_009102; /* 0090D2 bnes 0x9102 */
	{ uint32_t _s = 0x2000u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 0090D4 cmpiw #8192,%d0 */
	if (bs(ctx, !c_c(ctx))) goto L_009102; /* 0090D8 bccs 0x9102 */
	{ cc_set(ctx, CC_Z, ((ctx->d[1] >> 15) & 1) ? 0 : CC_Z); ctx->d[1] |= 0x8000u; hle_insn(ctx, 10); } /* 0090DA bset #15,%d1 */
	{ uint32_t _v = (ctx->d[1] & 0xFFFFu); st16((ctx->a[4] + (uint32_t)(int32_t)48), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0090DE movew %d1,%a4@(48) */
	{ uint32_t _v = 0x3u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0090E2 movew #3,%d0 */
	{ hle_insn(ctx, 18); SetPlayerState(ctx); } /* 0090E6 jsr %pc@(0x8fa8) */
	hle_insn(ctx, 10); goto L_009102; /* 0090EA bras 0x9102 */
L_0090EC:;
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[4] + (uint32_t)(int32_t)30), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 0090EC movew %d0,%a4@(30) */
	{ uint32_t _v = 0x2u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0090F0 movew #2,%d0 */
	{ hle_insn(ctx, 18); SetPlayerState(ctx); } /* 0090F4 jsr %pc@(0x8fa8) */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_callc(ctx, 0x013D92); } /* 0090F8 jsr 0x13d92 */
	{ uint32_t _da = (ctx->a[4] + (uint32_t)(int32_t)36); uint32_t _r = ~ld16(_da) & 0xFFFFu; cc_logic16(ctx, _r); st16(_da, _r); hle_insn(ctx, 16); } /* 0090FE notw %a4@(36) */
L_009102:;
	hle_insn(ctx, 16); return; /* 009102 rts */
}

/* $009104 energy bars, ENERGY LOW (decline bound 5400 = observed max 3565 x 1.5, T2_HLE_BOUNDS + NODECLINE over 92 states x 3000 frames) */
HLE_FUNC_BAIL_YIELD(0x009104, EnergyHud, 5400)
{
	PL_BAIL(0x009104, 700, 0x009104);
	{ ctx->a[4] = 0xFFFF8000u; hle_insn(ctx, 12); } /* 009104 lea 0xffff8000,%a4 */
	{ uint32_t _v = ld16((ctx->a[4] + (uint32_t)(int32_t)46)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00910A movew %a4@(46),%d0 */
	if (bs(ctx, c_z(ctx))) goto L_009116; /* 00910E beqs 0x9116 */
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 009110 addqw #1,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[4] + (uint32_t)(int32_t)46), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 009112 movew %d0,%a4@(46) */
L_009116:;
	{ cc_logic16(ctx, ld16((ctx->a[4] + (uint32_t)(int32_t)34))); hle_insn(ctx, 12); } /* 009116 tstw %a4@(34) */
	if (bw(ctx, !c_z(ctx))) goto L_009134; /* 00911A bnew 0x9134 */
	{ uint32_t _v = ld16((ctx->a[4] + (uint32_t)(int32_t)30)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00911E movew %a4@(30),%d0 */
	{ sh_w(ctx, 0, 'r', 8); hle_insn(ctx, 22); } /* 009122 lsrw #8,%d0 */
	{ uint32_t _v = 0xC000u; set_w(&ctx->d[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 009124 movew #-16384,%d6 */
	{ ctx->d[2] = 0x44u; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 009128 moveq #68,%d2 */
	{ ctx->d[4] = 0x43u; cc_logic32(ctx, ctx->d[4]); hle_insn(ctx, 4); } /* 00912A moveq #67,%d4 */
	{ uint32_t _v = 0xCD02u; set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00912C movew #-13054,%d3 */
	PL_BAIL(0x009104, 5400, 0x009130);
	{ hle_insn(ctx, 18); sub_00920C(ctx); } /* 009130 jsr %pc@(0x920c) */
L_009134:;
	{ uint32_t _v = ld16((ctx->a[4] + (uint32_t)(int32_t)30)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 009134 movew %a4@(30),%d0 */
	{ uint32_t _s = 0x2000u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 009138 cmpiw #8192,%d0 */
	if (bs(ctx, !c_c(ctx))) goto L_009146; /* 00913C bccs 0x9146 */
	{ uint32_t _v = 0x65u; st16(0xFFFF9EA4u, _v); cc_logic16(ctx, _v); hle_insn(ctx, 20); } /* 00913E movew #101,0xffff9ea4 */
L_009146:;
	{ ctx->a[4] = 0xFFFF8050u; hle_insn(ctx, 12); } /* 009146 lea 0xffff8050,%a4 */
	{ uint32_t _v = ld16((ctx->a[4] + (uint32_t)(int32_t)46)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00914C movew %a4@(46),%d0 */
	if (bs(ctx, c_z(ctx))) goto L_009158; /* 009150 beqs 0x9158 */
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 009152 addqw #1,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[4] + (uint32_t)(int32_t)46), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 009154 movew %d0,%a4@(46) */
L_009158:;
	{ cc_logic16(ctx, ld16((ctx->a[4] + (uint32_t)(int32_t)34))); hle_insn(ctx, 12); } /* 009158 tstw %a4@(34) */
	if (bw(ctx, !c_z(ctx))) goto L_009172; /* 00915C bnew 0x9172 */
	{ uint32_t _v = ld16((ctx->a[4] + (uint32_t)(int32_t)30)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 009160 movew %a4@(30),%d0 */
	{ sh_w(ctx, 0, 'r', 8); hle_insn(ctx, 22); } /* 009164 lsrw #8,%d0 */
	{ ctx->d[2] = 0x44u; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 009166 moveq #68,%d2 */
	{ ctx->d[4] = 0x45u; cc_logic32(ctx, ctx->d[4]); hle_insn(ctx, 4); } /* 009168 moveq #69,%d4 */
	{ uint32_t _v = 0xCD3Eu; set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00916A movew #-12994,%d3 */
	PL_BAIL(0x009104, 5400, 0x00916E);
	{ hle_insn(ctx, 18); sub_009242(ctx); } /* 00916E jsr %pc@(0x9242) */
L_009172:;
	{ uint32_t _v = ld16((ctx->a[4] + (uint32_t)(int32_t)30)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 009172 movew %a4@(30),%d0 */
	{ uint32_t _s = 0x2000u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 009176 cmpiw #8192,%d0 */
	if (bs(ctx, !c_c(ctx))) goto L_009184; /* 00917A bccs 0x9184 */
	{ uint32_t _v = 0x65u; st16(0xFFFF9EA4u, _v); cc_logic16(ctx, _v); hle_insn(ctx, 20); } /* 00917C movew #101,0xffff9ea4 */
L_009184:;
	hle_insn(ctx, 16); return; /* 009184 rts */
}

/* $009186 gunpower bars (decline bound 5500 = observed max 3635 x 1.5, T2_HLE_BOUNDS + NODECLINE over 92 states x 3000 frames) */
HLE_FUNC_BAIL_YIELD(0x009186, GunpowerHud, 5500)
{
	PL_BAIL(0x009186, 700, 0x009186);
	{ ctx->a[4] = 0xFFFF8000u; hle_insn(ctx, 12); } /* 009186 lea 0xffff8000,%a4 */
	{ cc_logic16(ctx, ld16((ctx->a[4] + (uint32_t)(int32_t)34))); hle_insn(ctx, 12); } /* 00918C tstw %a4@(34) */
	if (bw(ctx, !c_z(ctx))) goto L_0091A4; /* 009190 bnew 0x91a4 */
	PL_BAIL(0x009186, 5500, 0x009194);
	{ hle_insn(ctx, 18); sub_0091C4(ctx); } /* 009194 bsrw 0x91c4 */
	{ ctx->d[2] = 0x40u; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 009198 moveq #64,%d2 */
	{ ctx->d[4] = 0x3Fu; cc_logic32(ctx, ctx->d[4]); hle_insn(ctx, 4); } /* 00919A moveq #63,%d4 */
	{ uint32_t _v = 0xCD14u; set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00919C movew #-13036,%d3 */
	PL_BAIL(0x009186, 5500, 0x0091A0);
	{ hle_insn(ctx, 18); sub_00920C(ctx); } /* 0091A0 jsr %pc@(0x920c) */
L_0091A4:;
	{ ctx->a[4] = 0xFFFF8050u; hle_insn(ctx, 12); } /* 0091A4 lea 0xffff8050,%a4 */
	{ cc_logic16(ctx, ld16((ctx->a[4] + (uint32_t)(int32_t)34))); hle_insn(ctx, 12); } /* 0091AA tstw %a4@(34) */
	if (bw(ctx, !c_z(ctx))) goto L_0091C2; /* 0091AE bnew 0x91c2 */
	PL_BAIL(0x009186, 5500, 0x0091B2);
	{ hle_insn(ctx, 18); sub_0091C4(ctx); } /* 0091B2 bsrw 0x91c4 */
	{ ctx->d[2] = 0x42u; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 0091B6 moveq #66,%d2 */
	{ ctx->d[4] = 0x41u; cc_logic32(ctx, ctx->d[4]); hle_insn(ctx, 4); } /* 0091B8 moveq #65,%d4 */
	{ uint32_t _v = 0xCD2Cu; set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 0091BA movew #-13012,%d3 */
	PL_BAIL(0x009186, 5500, 0x0091BE);
	{ hle_insn(ctx, 18); sub_009242(ctx); } /* 0091BE jsr %pc@(0x9242) */
L_0091C2:;
	hle_insn(ctx, 16); return; /* 0091C2 rts */
}

/* $007DEE text record (a0) to the name table through VramWriteWord (decline bound 222500 = observed max 148324 x 1.5, T2_HLE_BOUNDS + NODECLINE over 92 states x 3000 frames) */
HLE_FUNC_YIELD(0x007DEE, PrintText, 222500)
{
	{ ctx->d[0] = 0x0u; cc_logic32(ctx, ctx->d[0]); hle_insn(ctx, 4); } /* 007DEE moveq #0,%d0 */
	{ uint32_t _p = ld8(ctx->a[0]); ctx->a[0] += 1; uint32_t _v = _p; set_b(&ctx->d[0], _v); cc_logic8(ctx, _v); hle_insn(ctx, 8); } /* 007DF0 moveb %a0@+,%d0 */
	{ ctx->d[1] = 0x0u; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 007DF2 moveq #0,%d1 */
	{ uint32_t _p = ld8(ctx->a[0]); ctx->a[0] += 1; uint32_t _v = _p; set_b(&ctx->d[1], _v); cc_logic8(ctx, _v); hle_insn(ctx, 8); } /* 007DF4 moveb %a0@+,%d1 */
	{ sh_w(ctx, 1, 'l', 7); hle_insn(ctx, 20); } /* 007DF6 aslw #7,%d1 */
	{ uint32_t _s = (ctx->d[0] & 0xFFFFu); uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 007DF8 addw %d0,%d0 */
	{ uint32_t _s = (ctx->d[0] & 0xFFFFu); uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 4); } /* 007DFA addw %d0,%d1 */
	{ uint32_t _v = 0xE000u; set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 007DFC movew #-8192,%d3 */
	{ uint32_t _s = (ctx->d[1] & 0xFFFFu); uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[3], _r); hle_insn(ctx, 4); } /* 007E00 addw %d1,%d3 */
	{ uint32_t _v = (ctx->d[3] & 0xFFFFu); st16(0xFFFF80C0u, _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 007E02 movew %d3,0xffff80c0 */
	{ uint32_t _v = ld16(0xFFFF9E2Cu); set_w(&ctx->d[5], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 007E08 movew 0xffff9e2c,%d5 */
L_007E0E:;
	{ uint32_t _p = ld8(ctx->a[0]); ctx->a[0] += 1; uint32_t _v = _p; set_b(&ctx->d[0], _v); cc_logic8(ctx, _v); hle_insn(ctx, 8); } /* 007E0E moveb %a0@+,%d0 */
	if (bs(ctx, !c_n(ctx))) goto L_007E24; /* 007E10 bpls 0x7e24 */
	{ uint32_t _v = ld16(0xFFFF80C0u); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 007E12 movew 0xffff80c0,%d3 */
	{ uint32_t _s = 0x80u; uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[3], _r); hle_insn(ctx, 8); } /* 007E18 addiw #128,%d3 */
	{ uint32_t _v = (ctx->d[3] & 0xFFFFu); st16(0xFFFF80C0u, _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 007E1C movew %d3,0xffff80c0 */
	hle_insn(ctx, 10); goto L_007E0E; /* 007E22 bras 0x7e0e */
L_007E24:;
	{ uint32_t _s = 0xFFu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 007E24 andiw #255,%d0 */
	if (bw(ctx, c_z(ctx))) goto L_007EB0; /* 007E28 beqw 0x7eb0 */
	{ uint32_t _s = 0x41u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 007E2C cmpiw #65,%d0 */
	if (bs(ctx, !c_ge(ctx))) goto L_007E3A; /* 007E30 blts 0x7e3a */
	{ uint32_t _s = 0x5Au; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 007E32 cmpiw #90,%d0 */
	if (bw(ctx, c_le(ctx))) goto L_007E9A; /* 007E36 blew 0x7e9a */
L_007E3A:;
	{ uint32_t _s = 0xDu; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 007E3A cmpiw #13,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_007E4A; /* 007E3E bnes 0x7e4a */
	{ uint32_t _s = 0xFF80u; uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[3], _r); hle_insn(ctx, 8); } /* 007E40 andiw #-128,%d3 */
	{ uint32_t _s = 0x80u; uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[3], _r); hle_insn(ctx, 8); } /* 007E44 addiw #128,%d3 */
	hle_insn(ctx, 10); goto L_007E0E; /* 007E48 bras 0x7e0e */
L_007E4A:;
	{ uint32_t _s = 0x39u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 007E4A cmpiw #57,%d0 */
	if (bs(ctx, c_gt(ctx))) goto L_007E5C; /* 007E4E bgts 0x7e5c */
	{ uint32_t _s = 0x30u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 007E50 cmpiw #48,%d0 */
	if (bs(ctx, !c_ge(ctx))) goto L_007E5C; /* 007E54 blts 0x7e5c */
	{ uint32_t _s = 0x16u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 007E56 subiw #22,%d0 */
	hle_insn(ctx, 10); goto L_007E9E; /* 007E5A bras 0x7e9e */
L_007E5C:;
	{ uint32_t _s = 0x20u; uint32_t _d = (ctx->d[0] & 0xFFu); alu(ctx, OP_SUB, _d, _s, 8, 0); hle_insn(ctx, 8); } /* 007E5C cmpib #32,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_007E68; /* 007E60 bnes 0x7e68 */
	{ uint32_t _v = 0x27u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 007E62 movew #39,%d0 */
	hle_insn(ctx, 10); goto L_007E9E; /* 007E66 bras 0x7e9e */
L_007E68:;
	{ ctx->d[1] = 0xFu; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 007E68 moveq #15,%d1 */
L_007E6A:;
	{ uint32_t _s = ld8((0x007E7Au + (uint32_t)((int32_t)(int16_t)ctx->d[1]))); uint32_t _d = (ctx->d[0] & 0xFFu); alu(ctx, OP_SUB, _d, _s, 8, 0); hle_insn(ctx, 14); } /* 007E6A cmpb %pc@(0x7e7a,%d1:w),%d0 */
	if (bs(ctx, c_z(ctx))) goto L_007E74; /* 007E6E beqs 0x7e74 */
	{ uint16_t _c = (uint16_t)ctx->d[1] - 1; set_w(&ctx->d[1], _c); if (_c != 0xFFFF) { hle_insn(ctx, 10); goto L_007E6A; } hle_insn(ctx, 14); } /* 007E70 dbf %d1,0x7e6a */
L_007E74:;
	{ uint32_t _v = ld8((0x007E8Au + (uint32_t)((int32_t)(int16_t)ctx->d[1]))); set_b(&ctx->d[0], _v); cc_logic8(ctx, _v); hle_insn(ctx, 14); } /* 007E74 moveb %pc@(0x7e8a,%d1:w),%d0 */
	hle_insn(ctx, 10); goto L_007E9E; /* 007E78 bras 0x7e9e */
L_007E9A:;
	{ uint32_t _s = 0x41u; uint32_t _d = (ctx->d[0] & 0xFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 8, 1); set_b(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 007E9A subib #65,%d0 */
L_007E9E:;
	{ uint32_t _s = (ctx->d[5] & 0xFFFFu); uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 007E9E addw %d5,%d0 */
	{ uint32_t _s = (ctx->d[6] & 0xFFFFu); uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d | _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 007EA0 orw %d6,%d0 */
	{ uint32_t _v = (ctx->d[3] & 0xFFFFu); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 4); } /* 007EA2 movew %d3,%d2 */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_callc(ctx, 0x0087C4); } /* 007EA4 jsr 0x87c4 */
	{ uint32_t _s = 0x2u; uint32_t _d = (ctx->d[3] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[3], _r); hle_insn(ctx, 4); } /* 007EAA addqw #2,%d3 */
	hle_insn(ctx, 10); goto L_007E0E; /* 007EAC braw 0x7e0e */
L_007EB0:;
	hle_insn(ctx, 16); return; /* 007EB0 rts */
}

