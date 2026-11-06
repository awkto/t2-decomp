/* Level-4 boss (Skynet CPU, object type $25) and the objects of its approach: the wall gun ($00E2E6,
 * hit test $006724) whose death runs the intro ($00E314 tile pop, $00E3E2 flash/tile frames, $00E546
 * sprite sweep, $00E5A6 spawner of the 16 CPU parts), the controller $00E72E, the CPU parts ($00E746
 * idle, $00E74A firing part, $00E7C0 death, hit test $00675E), and the finish ($00E864 explosions,
 * $00E8C8 tiles, $00E91E countdown and fade, $00E9C4 palette fade). docs/objects_types.md
 * ("Level 4"), src/t2.s.
 *
 * Same scheme as ai_l7.c / ai_l8.c: every AI state is a range from its entry (reached by the
 * `jmp (a0)` of ObjRunAll) to `bra.w ObjNext` ($00B3BE, reached with hle_jump). C callees (Random,
 * ObjAlloc, Copy32, PalFadeStep, HitBox) are called directly after their jsr is charged; every other
 * callee (VDP tile queues, CramUpload, sound, shot-hit helpers) runs as 68k through hle_call68k, so
 * those states are plain HLE_RANGEs and the others HLE_RANGE_PURE. Every instruction is on the
 * instruction timeline (GPGX costs); the bodies were produced instruction by instruction from the
 * disassembly (a comment on every line names the 68k instruction). The on-hit callbacks the hit tests
 * hand to ShotHitTest stay 68k. */
#include "hle.h"

#include <stdint.h>

void ObjQueueSprite(hle_ctx *ctx);
void QueueSprite(hle_ctx *ctx);
void Random(hle_ctx *ctx);
void HitBox(hle_ctx *ctx);
void ObjMoveX(hle_ctx *ctx);
void ObjScrollX(hle_ctx *ctx);
void ObjAlloc(hle_ctx *ctx);
void ObjKillIfLeftOf(hle_ctx *ctx);
void ObjKillIfRightOf(hle_ctx *ctx);
void Copy32(hle_ctx *ctx);
void PalFadeStep(hle_ctx *ctx);

#define EXIT_NEXT 0x00B3BEu

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

static int c_z(hle_ctx *c) { return (c->sr & CC_Z) != 0; }
static int c_n(hle_ctx *c) { return (c->sr & CC_N) != 0; }
static int c_c(hle_ctx *c) { return (c->sr & CC_C) != 0; }
static int c_v(hle_ctx *c) { return (c->sr & CC_V) != 0; }
static int c_ge(hle_ctx *c) { return c_n(c) == c_v(c); }
static int c_le(hle_ctx *c) { return c_z(c) || c_n(c) != c_v(c); }
static int c_hi(hle_ctx *c) { return !c_c(c) && !c_z(c); }
static int c_gt(hle_ctx *c) { return !c_z(c) && c_n(c) == c_v(c); }

/* conditional branches: charge, return whether taken */
static int bs(hle_ctx *c, int taken) { hle_insn(c, taken ? 10 : 8); return taken; }    /* Bcc.s */
static int bw(hle_ctx *c, int taken) { hle_insn(c, taken ? 10 : 12); return taken; }   /* Bcc.w */

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
static void mul_w(hle_ctx *c, int r, uint16_t src, int sign)
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


/* $00E2E6 wall gun before the boss: countdown, fires through sub_009A02 */
HLE_RANGE(0x00E2E6, SkyGun, 2600, EXIT_NEXT)
{
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00E2E6 subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_00E310; /* 00E2EA bnes 0xe310 */
	{ uint32_t _v = ctx->a[6]; push32(ctx, _v); cc_logic32(ctx, _v); hle_insn(ctx, 12); } /* 00E2EC movel %fp,%sp@- */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00E2EE movew %fp@(4),%d1 */
	{ uint32_t _s = 0x8u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00E2F2 subiw #8,%d1 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[5], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00E2F6 movew %fp@(2),%d5 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x009A02); } /* 00E2FA jsr %pc@(0x9a02) */
	{ uint32_t _v = pop32(ctx); ctx->a[6] = _v; hle_insn(ctx, 12); } /* 00E2FE moveal %sp@+,%fp */
	{ hle_insn(ctx, 18); Random(ctx); } /* 00E300 jsr %pc@(0x881c) */
	{ uint32_t _s = 0x3Fu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 00E304 andiw #63,%d0 */
	{ uint32_t _s = 0x50u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 00E308 addiw #80,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00E30C movew %d0,%fp@(36) */
L_00E310:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00E310 braw 0xb3be */
}

/* $00E314 wall gun killed: tile pop; the last one starts the boss intro */
HLE_RANGE(0x00E314, SkyGunDie, 14916, EXIT_NEXT)
{
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00E314 movew %fp@(2),%d0 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00E318 movew %fp@(4),%d1 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x01253C); } /* 00E31C jsr %pc@(0x1253c) */
	{ ctx->a[0] = 0x0006D582u; hle_insn(ctx, 12); } /* 00E320 lea 0x6d582,%a0 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)10)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00E326 movew %fp@(10),%d1 */
	{ uint32_t _v = 0x4u; set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E32A movew #4,%d2 */
	{ uint32_t _v = 0x2u; set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E32E movew #2,%d3 */
	{ uint32_t _v = 0x0u; set_w(&ctx->d[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E332 movew #0,%d6 */
	{ uint32_t _v = (ctx->d[7] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E336 movew %d7,%sp@- */
	{ uint32_t _v = ld16(0xFFFF9E46u); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00E338 movew 0xffff9e46,%d7 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x012234); } /* 00E33E jsr %pc@(0x12234) */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E342 movew %sp@+,%d7 */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFFAF84u; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 00E344 subqw #1,0xffffaf84 */
	if (bs(ctx, c_z(ctx))) goto L_00E358; /* 00E34A beqs 0xe358 */
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 00E34C clrw %fp@ */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DAu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 00E34E subqw #1,0xffff80da */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00E354 braw 0xb3be */
L_00E358:;
	{ ctx->a[4] = 0xFFFFAA9Cu; hle_insn(ctx, 12); } /* 00E358 lea 0xffffaa9c,%a4 */
	{ ctx->d[1] = 0xFu; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 00E35E moveq #15,%d1 */
	{ uint32_t _v = 0x40u; ctx->d[0] = _v; cc_logic32(ctx, _v); hle_insn(ctx, 12); } /* 00E360 movel #64,%d0 */
L_00E366:;
	{ uint32_t _v = ctx->d[0]; st32(ctx->a[4], _v); cc_logic32(ctx, _v); hle_insn(ctx, 12); } /* 00E366 movel %d0,%a4@ */
	{ ctx->a[4] = (ctx->a[4] + (uint32_t)(int32_t)32); hle_insn(ctx, 8); } /* 00E368 lea %a4@(32),%a4 */
	{ uint16_t _c = (uint16_t)ctx->d[1] - 1; set_w(&ctx->d[1], _c); if (_c != 0xFFFF) { hle_insn(ctx, 10); goto L_00E366; } hle_insn(ctx, 14); } /* 00E36C dbf %d1,0xe366 */
	{ uint32_t _v = 0x20u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E370 movew #32,%d0 */
	{ uint32_t _v = 0x40u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E374 movew #64,%d1 */
	{ uint32_t _v = 0x14u; set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E378 movew #20,%d2 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x01254E); } /* 00E37C jsr %pc@(0x1254e) */
	{ uint32_t _v = 0x20u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E380 movew #32,%d0 */
	{ uint32_t _v = 0x80u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E384 movew #128,%d1 */
	{ uint32_t _v = 0x14u; set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E388 movew #20,%d2 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x01254E); } /* 00E38C jsr %pc@(0x1254e) */
	{ uint32_t _v = 0x120u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E390 movew #288,%d0 */
	{ uint32_t _v = 0x40u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E394 movew #64,%d1 */
	{ uint32_t _v = 0x14u; set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E398 movew #20,%d2 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x01254E); } /* 00E39C jsr %pc@(0x1254e) */
	{ uint32_t _v = 0x120u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E3A0 movew #288,%d0 */
	{ uint32_t _v = 0x80u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E3A4 movew #128,%d1 */
	{ uint32_t _v = 0x14u; set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E3A8 movew #20,%d2 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x01254E); } /* 00E3AC jsr %pc@(0x1254e) */
	{ uint32_t _v = 0x20u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E3B0 movew #32,%d0 */
	{ uint32_t _v = 0x25u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E3B4 movew #37,%d1 */
	{ uint32_t _v = 0x14u; set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E3B8 movew #20,%d2 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x01254E); } /* 00E3BC jsr %pc@(0x1254e) */
	{ uint32_t _v = 0x120u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E3C0 movew #288,%d0 */
	{ uint32_t _v = 0x25u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E3C4 movew #37,%d1 */
	{ uint32_t _v = 0x14u; set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E3C8 movew #20,%d2 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x01254E); } /* 00E3CC jsr %pc@(0x1254e) */
	{ uint32_t _v = 0xE3E2u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00E3D0 movel #58338,%fp@(32) */
	{ uint32_t _v = 0x46u; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00E3D8 movew #70,%fp@(36) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00E3DE braw 0xb3be */
}

/* $00E3E2 boss intro: tile frames by countdown, then palette and the sprite sweep */
HLE_RANGE(0x00E3E2, SkyIntroTiles, 30703, EXIT_NEXT)
{
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00E3E2 movew %fp@(36),%d0 */
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 00E3E6 subqw #1,%d0 */
	if (bw(ctx, c_z(ctx))) goto L_00E4A4; /* 00E3E8 beqw 0xe4a4 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00E3EC movew %d0,%fp@(36) */
	{ uint32_t _s = 0x45u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 00E3F0 cmpiw #69,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_00E41E; /* 00E3F4 bnes 0xe41e */
	{ uint32_t _v = (ctx->d[7] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E3F6 movew %d7,%sp@- */
	{ ctx->a[0] = 0x0006D43Eu; hle_insn(ctx, 12); } /* 00E3F8 lea 0x6d43e,%a0 */
	{ uint32_t _v = 0xC170u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E3FE movew #-16016,%d1 */
	{ uint32_t _v = 0x7u; set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E402 movew #7,%d2 */
	{ uint32_t _v = 0x4u; set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E406 movew #4,%d3 */
	{ uint32_t _v = 0x0u; set_w(&ctx->d[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E40A movew #0,%d6 */
	{ uint32_t _v = ld16(0xFFFF9E46u); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00E40E movew 0xffff9e46,%d7 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x012234); } /* 00E414 jsr %pc@(0x12234) */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E418 movew %sp@+,%d7 */
	hle_insn(ctx, 10); goto L_00E4A0; /* 00E41A braw 0xe4a0 */
L_00E41E:;
	{ uint32_t _s = 0x44u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 00E41E cmpiw #68,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_00E44A; /* 00E422 bnes 0xe44a */
	{ uint32_t _v = (ctx->d[7] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E424 movew %d7,%sp@- */
	{ ctx->a[0] = 0x0006D43Eu; hle_insn(ctx, 12); } /* 00E426 lea 0x6d43e,%a0 */
	{ uint32_t _v = 0xC132u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E42C movew #-16078,%d1 */
	{ uint32_t _v = 0x7u; set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E430 movew #7,%d2 */
	{ uint32_t _v = 0x4u; set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E434 movew #4,%d3 */
	{ uint32_t _v = 0x0u; set_w(&ctx->d[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E438 movew #0,%d6 */
	{ uint32_t _v = ld16(0xFFFF9E46u); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00E43C movew 0xffff9e46,%d7 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x01227A); } /* 00E442 jsr %pc@(0x1227a) */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E446 movew %sp@+,%d7 */
	hle_insn(ctx, 10); goto L_00E4A0; /* 00E448 bras 0xe4a0 */
L_00E44A:;
	{ uint32_t _s = 0x2u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 00E44A cmpiw #2,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_00E476; /* 00E44E bnes 0xe476 */
	{ uint32_t _v = (ctx->d[7] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E450 movew %d7,%sp@- */
	{ ctx->a[0] = 0x0006D43Eu; hle_insn(ctx, 12); } /* 00E452 lea 0x6d43e,%a0 */
	{ uint32_t _v = 0xE100u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E458 movew #-7936,%d1 */
	{ uint32_t _v = 0x9u; set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E45C movew #9,%d2 */
	{ uint32_t _v = 0xFu; set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E460 movew #15,%d3 */
	{ uint32_t _v = 0x8000u; set_w(&ctx->d[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E464 movew #-32768,%d6 */
	{ uint32_t _v = ld16(0xFFFF9E46u); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00E468 movew 0xffff9e46,%d7 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x012234); } /* 00E46E jsr %pc@(0x12234) */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E472 movew %sp@+,%d7 */
	hle_insn(ctx, 10); goto L_00E4A0; /* 00E474 bras 0xe4a0 */
L_00E476:;
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 00E476 cmpiw #1,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_00E4A0; /* 00E47A bnes 0xe4a0 */
	{ uint32_t _v = (ctx->d[7] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E47C movew %d7,%sp@- */
	{ ctx->a[0] = 0x0006D43Eu; hle_insn(ctx, 12); } /* 00E47E lea 0x6d43e,%a0 */
	{ uint32_t _v = 0xE13Eu; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E484 movew #-7874,%d1 */
	{ uint32_t _v = 0x9u; set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E488 movew #9,%d2 */
	{ uint32_t _v = 0xFu; set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E48C movew #15,%d3 */
	{ uint32_t _v = 0x8000u; set_w(&ctx->d[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E490 movew #-32768,%d6 */
	{ uint32_t _v = ld16(0xFFFF9E46u); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00E494 movew 0xffff9e46,%d7 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x01227A); } /* 00E49A jsr %pc@(0x1227a) */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E49E movew %sp@+,%d7 */
L_00E4A0:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00E4A0 braw 0xb3be */
L_00E4A4:;
	{ ctx->a[1] = 0xFFFFA1B4u; hle_insn(ctx, 12); } /* 00E4A4 lea 0xffffa1b4,%a1 */
	{ ctx->a[0] = 0x000084DAu; hle_insn(ctx, 12); } /* 00E4AA lea 0x84da,%a0 */
	{ hle_insn(ctx, 20); Copy32(ctx); } /* 00E4B0 jsr 0x8a7c */
	{ ctx->a[0] = 0xFFFFA1B4u; hle_insn(ctx, 12); } /* 00E4B6 lea 0xffffa1b4,%a0 */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_call68k(ctx, 0x0080FE); } /* 00E4BC jsr 0x80fe */
	{ uint32_t _v = 0xE546u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00E4C2 movel #58694,%fp@(32) */
	{ uint32_t _v = 0x60u; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00E4CA movew #96,%fp@(36) */
	{ uint32_t _v = 0x71u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E4D0 movew #113,%d0 */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_call68k(ctx, 0x013D04); } /* 00E4D4 jsr 0x13d04 */
	{ uint32_t _v = (ctx->d[7] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E4DA movew %d7,%sp@- */
	{ ctx->a[0] = 0x0006D90Eu; hle_insn(ctx, 12); } /* 00E4DC lea 0x6d90e,%a0 */
	{ uint32_t _v = 0xC180u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E4E2 movew #-16000,%d1 */
	{ uint32_t _v = 0xCu; set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E4E6 movew #12,%d2 */
	{ uint32_t _v = 0xEu; set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E4EA movew #14,%d3 */
	{ uint32_t _v = 0x0u; set_w(&ctx->d[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E4EE movew #0,%d6 */
	{ uint32_t _v = ld16(0xFFFF9E46u); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00E4F2 movew 0xffff9e46,%d7 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x012234); } /* 00E4F8 jsr %pc@(0x12234) */
	{ ctx->a[0] = 0x0006D90Eu; hle_insn(ctx, 12); } /* 00E4FC lea 0x6d90e,%a0 */
	{ uint32_t _v = 0xC198u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E502 movew #-15976,%d1 */
	{ uint32_t _v = 0xCu; set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E506 movew #12,%d2 */
	{ uint32_t _v = 0xEu; set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E50A movew #14,%d3 */
	{ uint32_t _v = 0x0u; set_w(&ctx->d[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E50E movew #0,%d6 */
	{ uint32_t _v = ld16(0xFFFF9E46u); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00E512 movew 0xffff9e46,%d7 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x01227A); } /* 00E518 jsr %pc@(0x1227a) */
	{ ctx->a[0] = 0x0006DCDCu; hle_insn(ctx, 12); } /* 00E51C lea 0x6dcdc,%a0 */
	{ uint32_t _v = 0xC79Cu; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E522 movew #-14436,%d1 */
	{ uint32_t _v = 0x3u; set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E526 movew #3,%d2 */
	{ uint32_t _v = 0x2u; set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E52A movew #2,%d3 */
	{ uint32_t _v = 0x0u; set_w(&ctx->d[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E52E movew #0,%d6 */
	{ uint32_t _v = ld16(0xFFFF9E46u); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00E532 movew 0xffff9e46,%d7 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x012234); } /* 00E538 jsr %pc@(0x12234) */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E53C movew %sp@+,%d7 */
	{ uint32_t _v = 0xFFu; st8(0xFFFFA0A4u, _v); cc_logic8(ctx, _v); hle_insn(ctx, 20); } /* 00E53E moveb #-1,0xffffa0a4 */
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00E546 subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_00E578; /* 00E54A bnes 0xe578 */
	{ uint32_t _v = 0xE5A6u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00E54C movel #58790,%fp@(32) */
	{ uint32_t _v = ld32(0xFFFF80BCu); ctx->a[5] = _v; hle_insn(ctx, 20); } /* 00E554 moveal 0xffff80bc,%a5 */
	{ uint32_t _v = 0x2u; st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00E55A movew #2,%a5@+ */
	{ uint32_t _v = 0x780u; st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00E55E movew #1920,%a5@+ */
	{ uint32_t _v = 0xE100u; st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00E562 movew #-7936,%a5@+ */
	{ uint32_t _v = ctx->a[5]; st32(0xFFFF80BCu, _v); cc_logic32(ctx, _v); hle_insn(ctx, 20); } /* 00E566 movel %a5,0xffff80bc */
	{ uint32_t _v = 0x80u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E56C movew #128,%d0 */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_call68k(ctx, 0x013D04); } /* 00E570 jsr 0x13d04 */
	hle_insn(ctx, 10); goto L_00E5A2; /* 00E576 bras 0xe5a2 */
L_00E578:;
	{ ctx->d[2] = 0xFFFFFFE8u; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 00E578 moveq #-24,%d2 */
	{ uint32_t _s = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 12); } /* 00E57A addw %fp@(36),%d2 */
	{ ctx->d[3] = 0x20u; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 00E57E moveq #32,%d3 */
	{ uint32_t _v = 0xC8u; set_w(&ctx->d[5], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E580 movew #200,%d5 */
	{ uint32_t _v = 0x195u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E584 movew #405,%d1 */
	{ hle_insn(ctx, 18); QueueSprite(ctx); } /* 00E588 jsr %pc@(0x13240) */
	{ uint32_t _v = 0x138u; set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E58C movew #312,%d2 */
	{ uint32_t _s = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 12); } /* 00E590 subw %fp@(36),%d2 */
	{ ctx->d[3] = 0x20u; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 00E594 moveq #32,%d3 */
	{ uint32_t _v = 0xC8u; set_w(&ctx->d[5], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E596 movew #200,%d5 */
	{ uint32_t _v = 0x196u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E59A movew #406,%d1 */
	{ hle_insn(ctx, 18); QueueSprite(ctx); } /* 00E59E jsr %pc@(0x13240) */
L_00E5A2:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00E5A2 braw 0xb3be */
}

/* $00E546 boss intro: two sprites slide in, then the spawner */
HLE_RANGE(0x00E546, SkyIntroSweep, 762, EXIT_NEXT)
{
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00E546 subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_00E578; /* 00E54A bnes 0xe578 */
	{ uint32_t _v = 0xE5A6u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00E54C movel #58790,%fp@(32) */
	{ uint32_t _v = ld32(0xFFFF80BCu); ctx->a[5] = _v; hle_insn(ctx, 20); } /* 00E554 moveal 0xffff80bc,%a5 */
	{ uint32_t _v = 0x2u; st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00E55A movew #2,%a5@+ */
	{ uint32_t _v = 0x780u; st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00E55E movew #1920,%a5@+ */
	{ uint32_t _v = 0xE100u; st16(ctx->a[5], _v); ctx->a[5] += 2; cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00E562 movew #-7936,%a5@+ */
	{ uint32_t _v = ctx->a[5]; st32(0xFFFF80BCu, _v); cc_logic32(ctx, _v); hle_insn(ctx, 20); } /* 00E566 movel %a5,0xffff80bc */
	{ uint32_t _v = 0x80u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E56C movew #128,%d0 */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_call68k(ctx, 0x013D04); } /* 00E570 jsr 0x13d04 */
	hle_insn(ctx, 10); goto L_00E5A2; /* 00E576 bras 0xe5a2 */
L_00E578:;
	{ ctx->d[2] = 0xFFFFFFE8u; cc_logic32(ctx, ctx->d[2]); hle_insn(ctx, 4); } /* 00E578 moveq #-24,%d2 */
	{ uint32_t _s = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 12); } /* 00E57A addw %fp@(36),%d2 */
	{ ctx->d[3] = 0x20u; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 00E57E moveq #32,%d3 */
	{ uint32_t _v = 0xC8u; set_w(&ctx->d[5], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E580 movew #200,%d5 */
	{ uint32_t _v = 0x195u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E584 movew #405,%d1 */
	{ hle_insn(ctx, 18); QueueSprite(ctx); } /* 00E588 jsr %pc@(0x13240) */
	{ uint32_t _v = 0x138u; set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E58C movew #312,%d2 */
	{ uint32_t _s = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); uint32_t _d = (ctx->d[2] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[2], _r); hle_insn(ctx, 12); } /* 00E590 subw %fp@(36),%d2 */
	{ ctx->d[3] = 0x20u; cc_logic32(ctx, ctx->d[3]); hle_insn(ctx, 4); } /* 00E594 moveq #32,%d3 */
	{ uint32_t _v = 0xC8u; set_w(&ctx->d[5], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E596 movew #200,%d5 */
	{ uint32_t _v = 0x196u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E59A movew #406,%d1 */
	{ hle_insn(ctx, 18); QueueSprite(ctx); } /* 00E59E jsr %pc@(0x13240) */
L_00E5A2:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00E5A2 braw 0xb3be */
}

/* $00E5A6 boss spawner: allocate the 16 CPU parts from the table at $E62E */
HLE_RANGE_PURE(0x00E5A6, SkySpawn, 1000, EXIT_NEXT)
{
	{ uint32_t _v = ctx->a[6]; push32(ctx, _v); cc_logic32(ctx, _v); hle_insn(ctx, 12); } /* 00E5A6 movel %fp,%sp@- */
	{ uint32_t _v = 0xE72Eu; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00E5A8 movel #59182,%fp@(32) */
	{ uint32_t _v = ctx->a[6]; st32(0xFFFF9ECEu, _v); cc_logic32(ctx, _v); hle_insn(ctx, 20); } /* 00E5B0 movel %fp,0xffff9ece */
	{ ctx->a[1] = 0x00E62Eu; hle_insn(ctx, 8); } /* 00E5B6 lea %pc@(0xe62e),%a1 */
	{ ctx->d[1] = 0xFu; cc_logic32(ctx, ctx->d[1]); hle_insn(ctx, 4); } /* 00E5BA moveq #15,%d1 */
L_00E5BC:;
	{ hle_insn(ctx, 18); ObjAlloc(ctx); } /* 00E5BC jsr %pc@(0x930e) */
	{ uint32_t _v = 0x25u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E5C0 movew #37,%d0 */
	{ cc_set(ctx, CC_Z, ((ctx->d[0] >> 15) & 1) ? 0 : CC_Z); ctx->d[0] |= 0x8000u; hle_insn(ctx, 10); } /* 00E5C4 bset #15,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16(ctx->a[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E5C8 movew %d0,%fp@ */
	{ uint32_t _p = ld32(ctx->a[1]); ctx->a[1] += 4; uint32_t _v = _p; st32((ctx->a[6] + (uint32_t)(int32_t)2), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00E5CA movel %a1@+,%fp@(2) */
	{ uint32_t _p = ld16(ctx->a[1]); ctx->a[1] += 2; uint32_t _v = _p; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E5CE movew %a1@+,%d0 */
	{ uint32_t _s = 0xC180u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 00E5D0 addiw #-16000,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)10), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00E5D4 movew %d0,%fp@(10) */
	{ uint32_t _p = ld16(ctx->a[1]); ctx->a[1] += 2; uint32_t _v = _p; st16((ctx->a[6] + (uint32_t)(int32_t)42), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00E5D8 movew %a1@+,%fp@(42) */
	{ uint32_t _p = ld32(ctx->a[1]); ctx->a[1] += 4; uint32_t _v = _p; st32((ctx->a[6] + (uint32_t)(int32_t)38), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00E5DC movel %a1@+,%fp@(38) */
	{ uint32_t _p = ld16(ctx->a[1]); ctx->a[1] += 2; uint32_t _v = _p; st16((ctx->a[6] + (uint32_t)(int32_t)26), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00E5E0 movew %a1@+,%fp@(26) */
	{ uint32_t _v = 0xE746u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00E5E4 movel #59206,%fp@(32) */
	{ uint32_t _v = 0x675Eu; st32((ctx->a[6] + (uint32_t)(int32_t)28), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00E5EC movel #26462,%fp@(28) */
	{ uint32_t _p = ld16(ctx->a[1]); ctx->a[1] += 2; uint32_t _v = _p; st16((ctx->a[6] + (uint32_t)(int32_t)8), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00E5F4 movew %a1@+,%fp@(8) */
	{ uint32_t _s = 0x2u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)26); uint32_t _d = ld16(_da); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 16); } /* 00E5F8 cmpiw #2,%fp@(26) */
	if (bs(ctx, !c_z(ctx))) goto L_00E60E; /* 00E5FE bnes 0xe60e */
	{ uint32_t _v = 0xE74Au; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00E600 movel #59210,%fp@(32) */
	{ uint32_t _v = 0x32u; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00E608 movew #50,%fp@(36) */
L_00E60E:;
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DAu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 00E60E addqw #1,0xffff80da */
	{ uint16_t _c = (uint16_t)ctx->d[1] - 1; set_w(&ctx->d[1], _c); if (_c != 0xFFFF) { hle_insn(ctx, 10); goto L_00E5BC; } hle_insn(ctx, 14); } /* 00E614 dbf %d1,0xe5bc */
	{ uint32_t _v = pop32(ctx); ctx->a[6] = _v; hle_insn(ctx, 12); } /* 00E618 moveal %sp@+,%fp */
	{ uint32_t _v = 0x8u; st16(0xFFFFAF84u, _v); cc_logic16(ctx, _v); hle_insn(ctx, 20); } /* 00E61A movew #8,0xffffaf84 */
	{ uint32_t _v = 0x8u; st16(0xFFFFAF86u, _v); cc_logic16(ctx, _v); hle_insn(ctx, 20); } /* 00E622 movew #8,0xffffaf86 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00E62A braw 0xb3be */
}

/* $00E72E boss controller: periodic sound */
HLE_RANGE(0x00E72E, SkyCtl, 132, EXIT_NEXT)
{
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00E72E movew %fp@(36),%d0 */
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 00E732 subqw #1,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00E734 movew %d0,%fp@(36) */
	{ uint32_t _s = 0x3Fu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 00E738 andiw #63,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_00E742; /* 00E73C bnes 0xe742 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x013DD4); } /* 00E73E jsr %pc@(0x13dd4) */
L_00E742:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00E742 braw 0xb3be */
}

/* $00E746 CPU part: idle */
HLE_RANGE_PURE(0x00E746, SkyPartIdle, 1000, EXIT_NEXT)
{
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00E746 braw 0xb3be */
}

/* $00E74A CPU part: firing, countdown with a random reload */
HLE_RANGE(0x00E74A, SkyPartFire, 3053, EXIT_NEXT)
{
	{ cc_logic16(ctx, ld16((ctx->a[6] + (uint32_t)(int32_t)40))); hle_insn(ctx, 12); } /* 00E74A tstw %fp@(40) */
	if (bs(ctx, !c_n(ctx))) goto L_00E758; /* 00E74E bpls 0xe758 */
	{ uint32_t _v = ld16(0xFFFFAF86u); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00E750 movew 0xffffaf86,%d0 */
	hle_insn(ctx, 10); goto L_00E75E; /* 00E756 bras 0xe75e */
L_00E758:;
	{ uint32_t _v = ld16(0xFFFFAF84u); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00E758 movew 0xffffaf84,%d0 */
L_00E75E:;
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 00E75E cmpiw #1,%d0 */
	if (bw(ctx, c_z(ctx))) goto L_00E76C; /* 00E762 beqw 0xe76c */
	{ uint32_t _v = 0xC8u; st16((ctx->a[6] + (uint32_t)(int32_t)8), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00E766 movew #200,%fp@(8) */
L_00E76C:;
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00E76C subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_00E79A; /* 00E770 bnes 0xe79a */
	{ uint32_t _v = ctx->a[6]; push32(ctx, _v); cc_logic32(ctx, _v); hle_insn(ctx, 12); } /* 00E772 movel %fp,%sp@- */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00E774 movew %fp@(4),%d1 */
	{ uint32_t _s = 0x8u; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00E778 addiw #8,%d1 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[5], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00E77C movew %fp@(2),%d5 */
	{ uint32_t _s = 0x10u; uint32_t _d = (ctx->d[5] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[5], _r); hle_insn(ctx, 8); } /* 00E780 addiw #16,%d5 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x009A02); } /* 00E784 jsr %pc@(0x9a02) */
	{ uint32_t _v = pop32(ctx); ctx->a[6] = _v; hle_insn(ctx, 12); } /* 00E788 moveal %sp@+,%fp */
	{ hle_insn(ctx, 18); Random(ctx); } /* 00E78A jsr %pc@(0x881c) */
	{ uint32_t _s = 0x1Fu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 00E78E andiw #31,%d0 */
	{ uint32_t _s = 0x46u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 00E792 addiw #70,%d0 */
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00E796 movew %d0,%fp@(36) */
L_00E79A:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00E79A braw 0xb3be */
}

/* $00E7C0 CPU part destroyed: tiles, counters, the last one starts the finish */
HLE_RANGE(0x00E7C0, SkyPartDie, 3612, EXIT_NEXT)
{
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)2)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00E7C0 movew %fp@(2),%d0 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)4)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00E7C4 movew %fp@(4),%d1 */
	{ uint32_t _s = 0xEu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 00E7C8 addiw #14,%d0 */
	{ uint32_t _s = 0xEu; uint32_t _d = (ctx->d[1] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[1], _r); hle_insn(ctx, 8); } /* 00E7CC addiw #14,%d1 */
	{ uint32_t _v = 0x28u; set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E7D0 movew #40,%d2 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x01253C); } /* 00E7D4 jsr %pc@(0x1253c) */
	{ uint32_t _v = (ctx->d[7] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E7D8 movew %d7,%sp@- */
	{ ctx->a[0] = 0x0006D926u; hle_insn(ctx, 12); } /* 00E7DA lea 0x6d926,%a0 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)42)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00E7E0 movew %fp@(42),%d1 */
	{ ctx->a[0] = (ctx->a[0] + (uint32_t)(int32_t)0 + (uint32_t)((int32_t)(int16_t)ctx->d[1])); hle_insn(ctx, 12); } /* 00E7E4 lea %a0@(0,%d1:w),%a0 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)10)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00E7E8 movew %fp@(10),%d1 */
	{ uint32_t _v = 0x0u; set_w(&ctx->d[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E7EC movew #0,%d6 */
	{ uint32_t _v = ld16(0xFFFF9E46u); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00E7F0 movew 0xffff9e46,%d7 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)38)); set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00E7F6 movew %fp@(38),%d2 */
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)40)); set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00E7FA movew %fp@(40),%d3 */
	if (bs(ctx, !c_n(ctx))) goto L_00E808; /* 00E7FE bpls 0xe808 */
	{ uint32_t _r = alu(ctx, OP_SUB, 0, ctx->d[3], 16, 1); set_w(&ctx->d[3], _r); hle_insn(ctx, 4); } /* 00E800 negw %d3 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x01227A); } /* 00E802 jsr %pc@(0x1227a) */
	hle_insn(ctx, 10); goto L_00E80C; /* 00E806 bras 0xe80c */
L_00E808:;
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x012234); } /* 00E808 jsr %pc@(0x12234) */
L_00E80C:;
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E80C movew %sp@+,%d7 */
	{ cc_logic16(ctx, ld16((ctx->a[6] + (uint32_t)(int32_t)40))); hle_insn(ctx, 12); } /* 00E80E tstw %fp@(40) */
	if (bs(ctx, !c_n(ctx))) goto L_00E81C; /* 00E812 bpls 0xe81c */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFFAF86u; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 00E814 subqw #1,0xffffaf86 */
	hle_insn(ctx, 10); goto L_00E822; /* 00E81A bras 0xe822 */
L_00E81C:;
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFFAF84u; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 00E81C subqw #1,0xffffaf84 */
L_00E822:;
	{ uint32_t _v = ld16(0xFFFFAF84u); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00E822 movew 0xffffaf84,%d0 */
	{ uint32_t _s = ld16(0xFFFFAF86u); uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_ADD, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 16); } /* 00E828 addw 0xffffaf86,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_00E858; /* 00E82E bnes 0xe858 */
	{ uint32_t _v = ld32(0xFFFF9ECEu); ctx->a[1] = _v; hle_insn(ctx, 20); } /* 00E830 moveal 0xffff9ece,%a1 */
	{ uint32_t _v = 0x79u; st16(0xFFFF9EA4u, _v); cc_logic16(ctx, _v); hle_insn(ctx, 20); } /* 00E836 movew #121,0xffff9ea4 */
	{ uint32_t _v = 0xE864u; st32((ctx->a[1] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00E83E movel #59492,%a1@(32) */
	{ uint32_t _v = 0xFAu; st16((ctx->a[1] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00E846 movew #250,%a1@(36) */
	{ uint32_t _a = ctx->a[1]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o & 0x7F); hle_insn(ctx, 16); } /* 00E84C bclr #7,%a1@ */
	{ uint32_t _v = 0x597Au; st32((ctx->a[1] + (uint32_t)(int32_t)28), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00E850 movel #22906,%a1@(28) */
L_00E858:;
	{ st16(ctx->a[6], 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 12); } /* 00E858 clrw %fp@ */
	{ uint32_t _s = 0x1u; uint32_t _da = 0xFFFF80DAu; uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 20); } /* 00E85A subqw #1,0xffff80da */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00E860 braw 0xb3be */
}

/* $00E864 boss finish: countdown, explosion sprites */
HLE_RANGE(0x00E864, SkyFinish1, 29097, EXIT_NEXT)
{
	{ uint32_t _s = 0x1u; uint32_t _da = (ctx->a[6] + (uint32_t)(int32_t)36); uint32_t _d = ld16(_da); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); st16(_da, _r); hle_insn(ctx, 16); } /* 00E864 subqw #1,%fp@(36) */
	if (bs(ctx, !c_z(ctx))) goto L_00E890; /* 00E868 bnes 0xe890 */
	{ uint32_t _v = 0xE8C8u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00E86A movel #59592,%fp@(32) */
	{ ctx->a[0] = 0x0000E62Eu; hle_insn(ctx, 12); } /* 00E872 lea 0xe62e,%a0 */
	{ ctx->d[5] = 0xFu; cc_logic32(ctx, ctx->d[5]); hle_insn(ctx, 4); } /* 00E878 moveq #15,%d5 */
L_00E87A:;
	{ uint32_t _v = ld16(ctx->a[0]); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E87A movew %a0@,%d0 */
	{ uint32_t _v = ld16((ctx->a[0] + (uint32_t)(int32_t)2)); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00E87C movew %a0@(2),%d1 */
	{ uint32_t _v = 0x14u; set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E880 movew #20,%d2 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x01254E); } /* 00E884 jsr %pc@(0x1254e) */
	{ ctx->a[0] = (ctx->a[0] + (uint32_t)(int32_t)16); hle_insn(ctx, 8); } /* 00E888 lea %a0@(16),%a0 */
	{ uint16_t _c = (uint16_t)ctx->d[5] - 1; set_w(&ctx->d[5], _c); if (_c != 0xFFFF) { hle_insn(ctx, 10); goto L_00E87A; } hle_insn(ctx, 14); } /* 00E88C dbf %d5,0xe87a */
L_00E890:;
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00E890 movew %fp@(36),%d0 */
	{ uint32_t _s = 0xFu; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 00E894 andiw #15,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_00E8AE; /* 00E898 bnes 0xe8ae */
	{ uint32_t _v = 0xA0u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E89A movew #160,%d0 */
	{ uint32_t _v = 0x48u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E89E movew #72,%d1 */
	{ uint32_t _v = 0x14u; set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E8A2 movew #20,%d2 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x0124D2); } /* 00E8A6 jsr %pc@(0x124d2) */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00E8AA braw 0xb3be */
L_00E8AE:;
	{ uint32_t _s = 0x8u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 00E8AE cmpiw #8,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_00E8C4; /* 00E8B2 bnes 0xe8c4 */
	{ uint32_t _v = 0xA0u; set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E8B4 movew #160,%d0 */
	{ uint32_t _v = 0x48u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E8B8 movew #72,%d1 */
	{ uint32_t _v = 0x14u; set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E8BC movew #20,%d2 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x0124B8); } /* 00E8C0 jsr %pc@(0x124b8) */
L_00E8C4:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00E8C4 braw 0xb3be */
}

/* $00E8C8 boss finish: tiles */
HLE_RANGE(0x00E8C8, SkyFinish2, 23315, EXIT_NEXT)
{
	{ uint32_t _v = (ctx->d[7] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E8C8 movew %d7,%sp@- */
	{ ctx->a[0] = 0x0006D93Eu; hle_insn(ctx, 12); } /* 00E8CA lea 0x6d93e,%a0 */
	{ uint32_t _v = 0xC180u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E8D0 movew #-16000,%d1 */
	{ uint32_t _v = 0xCu; set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E8D4 movew #12,%d2 */
	{ uint32_t _v = 0xEu; set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E8D8 movew #14,%d3 */
	{ uint32_t _v = 0x0u; set_w(&ctx->d[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E8DC movew #0,%d6 */
	{ uint32_t _v = ld16(0xFFFF9E46u); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00E8E0 movew 0xffff9e46,%d7 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x012234); } /* 00E8E6 jsr %pc@(0x12234) */
	{ ctx->a[0] = 0x0006D93Eu; hle_insn(ctx, 12); } /* 00E8EA lea 0x6d93e,%a0 */
	{ uint32_t _v = 0xC198u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E8F0 movew #-15976,%d1 */
	{ uint32_t _v = 0xCu; set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E8F4 movew #12,%d2 */
	{ uint32_t _v = 0xEu; set_w(&ctx->d[3], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E8F8 movew #14,%d3 */
	{ uint32_t _v = 0x0u; set_w(&ctx->d[6], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E8FC movew #0,%d6 */
	{ uint32_t _v = ld16(0xFFFF9E46u); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00E900 movew 0xffff9e46,%d7 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x01227A); } /* 00E906 jsr %pc@(0x1227a) */
	{ uint32_t _v = 0xE91Eu; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00E90A movel #59678,%fp@(32) */
	{ uint32_t _v = 0x1F4u; st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00E912 movew #500,%fp@(36) */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E918 movew %sp@+,%d7 */
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00E91A braw 0xb3be */
}

/* $00E91E boss finish: countdown, explosions, palette */
HLE_RANGE(0x00E91E, SkyFinish3, 2532, EXIT_NEXT)
{
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)36)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00E91E movew %fp@(36),%d0 */
	{ uint32_t _s = 0x1u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = alu(ctx, OP_SUB, _d, _s, 16, 1); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 00E922 subqw #1,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_00E968; /* 00E924 bnes 0xe968 */
	{ uint32_t _v = 0xE9C4u; st32((ctx->a[6] + (uint32_t)(int32_t)32), _v); cc_logic32(ctx, _v); hle_insn(ctx, 24); } /* 00E926 movel #59844,%fp@(32) */
	{ st8(0xFFFFA246u, 0xFF); hle_insn(ctx, 20); } /* 00E92E st 0xffffa246 */
	{ ctx->a[1] = 0xFFFFA134u; hle_insn(ctx, 12); } /* 00E934 lea 0xffffa134,%a1 */
	{ ctx->a[0] = 0x000082BAu; hle_insn(ctx, 12); } /* 00E93A lea 0x82ba,%a0 */
	{ hle_insn(ctx, 18); Copy32(ctx); } /* 00E940 jsr %pc@(0x8a7c) */
	{ ctx->a[0] = 0x000081DAu; hle_insn(ctx, 12); } /* 00E944 lea 0x81da,%a0 */
	{ hle_insn(ctx, 18); Copy32(ctx); } /* 00E94A jsr %pc@(0x8a7c) */
	{ ctx->a[0] = 0x000081DAu; hle_insn(ctx, 12); } /* 00E94E lea 0x81da,%a0 */
	{ hle_insn(ctx, 18); Copy32(ctx); } /* 00E954 jsr %pc@(0x8a7c) */
	{ ctx->a[0] = 0x000081DAu; hle_insn(ctx, 12); } /* 00E958 lea 0x81da,%a0 */
	{ hle_insn(ctx, 18); Copy32(ctx); } /* 00E95E jsr %pc@(0x8a7c) */
	{ st8(0xFFFFA0A4u, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); hle_insn(ctx, 20); } /* 00E962 clrb 0xffffa0a4 */
L_00E968:;
	{ uint32_t _v = (ctx->d[0] & 0xFFFFu); st16((ctx->a[6] + (uint32_t)(int32_t)36), _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00E968 movew %d0,%fp@(36) */
	{ uint32_t _s = 0x190u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 00E96C cmpiw #400,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_00E984; /* 00E970 bnes 0xe984 */
	{ uint32_t _v = ld16(0xFFFF9E50u); set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00E972 movew 0xffff9e50,%d1 */
	{ ctx->a[1] = 0x00054ED2u; hle_insn(ctx, 12); } /* 00E978 lea 0x54ed2,%a1 */
	{ uint32_t _p = ld16(ctx->a[1]); ctx->a[1] += 2; uint32_t _v = _p; set_w(&ctx->d[2], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E97E movew %a1@+,%d2 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x009B14); } /* 00E980 bsrw 0x9b14 */
L_00E984:;
	if (bs(ctx, c_gt(ctx))) goto L_00E9C0; /* 00E984 bgts 0xe9c0 */
	{ uint32_t _v = 0x3Fu; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E986 movew #63,%d1 */
	{ uint32_t _s = 0x12Cu; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 00E98A cmpiw #300,%d0 */
	if (bs(ctx, c_gt(ctx))) goto L_00E9A8; /* 00E98E bgts 0xe9a8 */
	{ uint32_t _v = 0x1Fu; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E990 movew #31,%d1 */
	{ uint32_t _s = 0xC8u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 00E994 cmpiw #200,%d0 */
	if (bs(ctx, c_gt(ctx))) goto L_00E9A8; /* 00E998 bgts 0xe9a8 */
	{ uint32_t _v = 0xFu; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E99A movew #15,%d1 */
	{ uint32_t _s = 0x64u; uint32_t _d = (ctx->d[0] & 0xFFFFu); alu(ctx, OP_SUB, _d, _s, 16, 0); hle_insn(ctx, 8); } /* 00E99E cmpiw #100,%d0 */
	if (bs(ctx, c_gt(ctx))) goto L_00E9A8; /* 00E9A2 bgts 0xe9a8 */
	{ uint32_t _v = 0x7u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E9A4 movew #7,%d1 */
L_00E9A8:;
	{ uint32_t _s = (ctx->d[1] & 0xFFFFu); uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 4); } /* 00E9A8 andw %d1,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_00E9C0; /* 00E9AA bnes 0xe9c0 */
	{ uint32_t _v = ctx->a[6]; push32(ctx, _v); cc_logic32(ctx, _v); hle_insn(ctx, 12); } /* 00E9AC movel %fp,%sp@- */
	{ uint32_t _v = 0x53u; set_w(&ctx->d[1], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E9AE movew #83,%d1 */
	{ uint32_t _v = 0xA0u; set_w(&ctx->d[5], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E9B2 movew #160,%d5 */
	{ hle_insn(ctx, 18); hle_sync(ctx); hle_call68k(ctx, 0x0099C8); } /* 00E9B6 jsr %pc@(0x99c8) */
	{ uint32_t _a = ctx->a[6]; uint8_t _o = ld8(_a); cc_set(ctx, CC_Z, ((_o >> 7) & 1) ? 0 : CC_Z); st8(_a, _o & 0x7F); hle_insn(ctx, 16); } /* 00E9BA bclr #15,%fp@ */
	{ uint32_t _v = pop32(ctx); ctx->a[6] = _v; hle_insn(ctx, 12); } /* 00E9BE moveal %sp@+,%fp */
L_00E9C0:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00E9C0 braw 0xb3be */
}

/* $00E9C4 boss finish: palette fade out */
HLE_RANGE(0x00E9C4, SkyFadeEnd, 27192, EXIT_NEXT)
{
	{ uint32_t _v = ld16(0xFFFFA0EEu); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 16); } /* 00E9C4 movew 0xffffa0ee,%d0 */
	{ uint32_t _s = 0x7u; uint32_t _d = (ctx->d[0] & 0xFFFFu); uint32_t _r = (_d & _s) & 0xFFFFu; cc_logic16(ctx, _r); set_w(&ctx->d[0], _r); hle_insn(ctx, 8); } /* 00E9CA andiw #7,%d0 */
	if (bs(ctx, !c_z(ctx))) goto L_00E9F6; /* 00E9CE bnes 0xe9f6 */
	{ uint32_t _v = (ctx->d[7] & 0xFFFFu); push16(ctx, _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E9D0 movew %d7,%sp@- */
	{ hle_insn(ctx, 20); PalFadeStep(ctx); } /* 00E9D2 jsr 0x8970 */
	{ ctx->a[0] = 0xFFFFA1B4u; hle_insn(ctx, 12); } /* 00E9D8 lea 0xffffa1b4,%a0 */
	{ hle_insn(ctx, 20); hle_sync(ctx); hle_call68k(ctx, 0x0080FE); } /* 00E9DE jsr 0x80fe */
	{ uint32_t _v = pop16(ctx); set_w(&ctx->d[7], _v); cc_logic16(ctx, _v); hle_insn(ctx, 8); } /* 00E9E4 movew %sp@+,%d7 */
	{ cc_logic8(ctx, ld8(0xFFFFA246u)); hle_insn(ctx, 16); } /* 00E9E6 tstb 0xffffa246 */
	if (bs(ctx, !c_z(ctx))) goto L_00E9F6; /* 00E9EC bnes 0xe9f6 */
	{ uint32_t _v = 0x1u; st8(0xFFFFA234u, _v); cc_logic8(ctx, _v); hle_insn(ctx, 20); } /* 00E9EE moveb #1,0xffffa234 */
L_00E9F6:;
	hle_insn(ctx, 10); hle_jump(ctx, 0x00B3BE); return; /* 00E9F6 braw 0xb3be */
}

/* $006724 hit test of wall gun (one box) */
HLE_FUNC_PURE(0x006724, HitSkyGun, 300)
{
	{ ctx->a[3] = 0x006756u; hle_insn(ctx, 8); } /* 006724 lea %pc@(0x6756),%a3 */
	{ ctx->a[2] = 0x006730u; hle_insn(ctx, 8); } /* 006728 lea %pc@(0x6730),%a2 */
	hle_insn(ctx, 10); HitBox(ctx); return; /* 00672C jmp %pc@(0x5940) */
}

/* $00675E hit test of CPU part (box by +$1A) */
HLE_FUNC_PURE(0x00675E, HitSkyPart, 300)
{
	{ uint32_t _v = ld16((ctx->a[6] + (uint32_t)(int32_t)26)); set_w(&ctx->d[0], _v); cc_logic16(ctx, _v); hle_insn(ctx, 12); } /* 00675E movew %fp@(26),%d0 */
	{ sh_w(ctx, 0, 'l', 3); hle_insn(ctx, 12); } /* 006762 aslw #3,%d0 */
	{ ctx->a[3] = (0x006796u + (uint32_t)((int32_t)(int16_t)ctx->d[0])); hle_insn(ctx, 12); } /* 006764 lea %pc@(0x6796,%d0:w),%a3 */
	{ ctx->a[2] = 0x006770u; hle_insn(ctx, 8); } /* 006768 lea %pc@(0x6770),%a2 */
	hle_insn(ctx, 10); HitBox(ctx); return; /* 00676C jmp %pc@(0x5940) */
}

