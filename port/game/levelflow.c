/* Level flow: NextLevel $0074D8, LevelEnd $0078D0, the per-level loader dispatcher $001D3A and
 * LevelLoad1..8 ($002980 $00241E $00281E $002632 $00229E $002154 $001FB2 $001DB4)
 * (docs/subsystems/levels.md, mainloop.md).
 *
 * These routines are long and almost every statement between their calls is a `jsr`/`bsr` to code that
 * does VDP work (tile queues and DMA through the VDP list, plane fills, palette loads), blocking waits
 * (WaitVBlank, WaitFrames, PalFade), sound requests or the hi-score screen. All of that stays the real
 * 68k: the routines are registered as HLE_RANGE_PURE pieces that cover only the routines' OWN
 * instructions. A piece runs from an entry (the routine's start, or the return address of a `jsr`/`bsr`)
 * up to the next `jsr`/`bsr`/`rts` and leaves with hle_jump to that instruction, which the 68k executes
 * (so every callee, registered or not, is entered exactly as in the original and a callee that is
 * itself replaced is dispatched on its own). The pieces touch only work RAM and registers, so the layer
 * measures each one with a dry run and declines it when a VBlank would fall inside: no hand-made
 * bound is needed. Nothing here touches the VDP ports; the few instructions the loaders have besides
 * their calls are `move.w #imm,(abs)` tile-base words, `lea` of ROM data and argument loads.
 *
 * Generated instruction by instruction from the disassembly (tools: a throw-away generator reading
 * the ROM with objdump and the GPGX opcode cycle table); every charge is the GPGX cost of the opcode
 * word and tools/py/costcheck.py checks them. Registers and flags are mirrored. All the pieces are in
 * one function, lf_run(), entered through a switch on the entry address; branches inside the routines
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

#define F_N (((ctx->sr) & CC_N) != 0)
#define F_Z (((ctx->sr) & CC_Z) != 0)
#define F_V (((ctx->sr) & CC_V) != 0)
#define F_C (((ctx->sr) & CC_C) != 0)
#define COND_EQ F_Z
#define COND_NE (!F_Z)
#define COND_LT (F_N != F_V)
#define COND_GE (F_N == F_V)
#define COND_GT (!F_Z && F_N == F_V)
#define COND_LE (F_Z || F_N != F_V)
#define COND_CC (!F_C)
#define COND_CS F_C
#define COND_PL (!F_N)
#define COND_MI F_N


#define LF_EXITS \
	0x001D46, 0x001D54, 0x001D62, 0x001D70, 0x001D7E, 0x001D8C, \
	0x001D9A, 0x001DA8, 0x001DB2, 0x001DB4, 0x001DE2, 0x001DF8, \
	0x001E0E, 0x001E24, 0x001E6E, 0x001E84, 0x001E9A, 0x001EB0, \
	0x001EC6, 0x001ECC, 0x001EDC, 0x001EFC, 0x001F1C, 0x001F22, \
	0x001F28, 0x001F2E, 0x001F4E, 0x001F6E, 0x001F74, 0x001F7A, \
	0x001F8A, 0x001F92, 0x001F9A, 0x001FA2, 0x001FAA, 0x001FB0, \
	0x001FB2, 0x001FD4, 0x001FEA, 0x002000, 0x002016, 0x002034, \
	0x00204A, 0x002050, 0x002070, 0x002090, 0x0020B0, 0x0020D0, \
	0x0020F0, 0x002110, 0x002116, 0x00211C, 0x00212C, 0x002134, \
	0x00213C, 0x002144, 0x00214C, 0x002152, 0x002154, 0x00216E, \
	0x0021A4, 0x0021BA, 0x0021D4, 0x0021FA, 0x00220A, 0x00221A, \
	0x00223A, 0x00225A, 0x002260, 0x002266, 0x002276, 0x00227E, \
	0x002286, 0x00228E, 0x002296, 0x00229C, 0x00229E, 0x0022A8, \
	0x0022B2, 0x0022DA, 0x0022F0, 0x002326, 0x00233C, 0x00236A, \
	0x00237A, 0x00238A, 0x0023AA, 0x0023CA, 0x0023D0, 0x0023D6, \
	0x0023E6, 0x0023EE, 0x0023F6, 0x0023FE, 0x002406, 0x00240C, \
	0x00241E, 0x00243A, 0x002450, 0x002466, 0x0024EC, 0x0024FC, \
	0x00251C, 0x00253C, 0x00255C, 0x002562, 0x002568, 0x00256E, \
	0x00258E, 0x0025AE, 0x0025CE, 0x0025EE, 0x0025F4, 0x0025FA, \
	0x00260A, 0x002612, 0x00261A, 0x002622, 0x00262A, 0x002630, \
	0x002632, 0x00264E, 0x002664, 0x00267A, 0x002690, 0x0026A6, \
	0x0026BC, 0x00278A, 0x00279A, 0x0027BA, 0x0027DA, 0x0027E0, \
	0x0027E6, 0x0027F6, 0x0027FE, 0x002806, 0x00280E, 0x002816, \
	0x00281C, 0x00281E, 0x002832, 0x002848, 0x00285E, 0x002874, \
	0x00288A, 0x0028A0, 0x0028E6, 0x0028F6, 0x002916, 0x002936, \
	0x00293C, 0x002942, 0x002948, 0x002958, 0x002960, 0x002968, \
	0x002970, 0x002978, 0x00297E, 0x002980, 0x00299A, 0x0029B0, \
	0x002A4E, 0x002A5E, 0x002A6E, 0x002A7E, 0x002A9E, 0x002ABE, \
	0x002AC4, 0x002ACA, 0x002AD0, 0x002AE0, 0x002AE8, 0x002AF0, \
	0x002AF8, 0x002B00, 0x002B06, 0x00750E, 0x007550, 0x007554, \
	0x00759E, 0x0075BC, 0x0075DA, 0x0075E0, 0x0075E4, 0x0075E8, \
	0x0075EC, 0x0075F0, 0x0075F4, 0x0075FA, 0x0075FE, 0x007604, \
	0x007608, 0x00760E, 0x007614, 0x007620, 0x00762C, 0x00764A, \
	0x00765E, 0x0078F6, 0x0078FC, 0x007902, 0x007908, 0x00790E, \
	0x007912, 0x00791A, 0x00792A, 0x007934, 0x00793C, 0x007946, \
	0x007952, 0x007982, 0x007996, 0x00799C, 0x0079CE, 0x0079EC, \
	0x0079F0, 0x007A0E, 0x007A42, 0x007A4E, 0x007A52

static void lf_run(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x001D3A: goto L_001D3A;
	case 0x001D4C: goto L_001D4C;
	case 0x001D5A: goto L_001D5A;
	case 0x001D68: goto L_001D68;
	case 0x001D76: goto L_001D76;
	case 0x001D84: goto L_001D84;
	case 0x001D92: goto L_001D92;
	case 0x001DA0: goto L_001DA0;
	case 0x001DAE: goto L_001DAE;
	case 0x001DB8: goto L_001DB8;
	case 0x001DE8: goto L_001DE8;
	case 0x001DFE: goto L_001DFE;
	case 0x001E14: goto L_001E14;
	case 0x001E2A: goto L_001E2A;
	case 0x001E74: goto L_001E74;
	case 0x001E8A: goto L_001E8A;
	case 0x001EA0: goto L_001EA0;
	case 0x001EB6: goto L_001EB6;
	case 0x001ED2: goto L_001ED2;
	case 0x001EE2: goto L_001EE2;
	case 0x001F02: goto L_001F02;
	case 0x001F34: goto L_001F34;
	case 0x001F54: goto L_001F54;
	case 0x001F80: goto L_001F80;
	case 0x001F8E: goto L_001F8E;
	case 0x001F96: goto L_001F96;
	case 0x001F9E: goto L_001F9E;
	case 0x001FA6: goto L_001FA6;
	case 0x001FB6: goto L_001FB6;
	case 0x001FDA: goto L_001FDA;
	case 0x001FF0: goto L_001FF0;
	case 0x002006: goto L_002006;
	case 0x00201C: goto L_00201C;
	case 0x00203A: goto L_00203A;
	case 0x002056: goto L_002056;
	case 0x002076: goto L_002076;
	case 0x002096: goto L_002096;
	case 0x0020B6: goto L_0020B6;
	case 0x0020D6: goto L_0020D6;
	case 0x0020F6: goto L_0020F6;
	case 0x002122: goto L_002122;
	case 0x002130: goto L_002130;
	case 0x002138: goto L_002138;
	case 0x002140: goto L_002140;
	case 0x002148: goto L_002148;
	case 0x002158: goto L_002158;
	case 0x002174: goto L_002174;
	case 0x0021AA: goto L_0021AA;
	case 0x0021C0: goto L_0021C0;
	case 0x0021DA: goto L_0021DA;
	case 0x002200: goto L_002200;
	case 0x002210: goto L_002210;
	case 0x002220: goto L_002220;
	case 0x002240: goto L_002240;
	case 0x00226C: goto L_00226C;
	case 0x00227A: goto L_00227A;
	case 0x002282: goto L_002282;
	case 0x00228A: goto L_00228A;
	case 0x002292: goto L_002292;
	case 0x0022A2: goto L_0022A2;
	case 0x0022AC: goto L_0022AC;
	case 0x0022B6: goto L_0022B6;
	case 0x0022E0: goto L_0022E0;
	case 0x0022F6: goto L_0022F6;
	case 0x00232C: goto L_00232C;
	case 0x002342: goto L_002342;
	case 0x002370: goto L_002370;
	case 0x002380: goto L_002380;
	case 0x002390: goto L_002390;
	case 0x0023B0: goto L_0023B0;
	case 0x0023DC: goto L_0023DC;
	case 0x0023EA: goto L_0023EA;
	case 0x0023F2: goto L_0023F2;
	case 0x0023FA: goto L_0023FA;
	case 0x002402: goto L_002402;
	case 0x002422: goto L_002422;
	case 0x002440: goto L_002440;
	case 0x002456: goto L_002456;
	case 0x00246C: goto L_00246C;
	case 0x0024F2: goto L_0024F2;
	case 0x002502: goto L_002502;
	case 0x002522: goto L_002522;
	case 0x002542: goto L_002542;
	case 0x002574: goto L_002574;
	case 0x002594: goto L_002594;
	case 0x0025B4: goto L_0025B4;
	case 0x0025D4: goto L_0025D4;
	case 0x002600: goto L_002600;
	case 0x00260E: goto L_00260E;
	case 0x002616: goto L_002616;
	case 0x00261E: goto L_00261E;
	case 0x002626: goto L_002626;
	case 0x002636: goto L_002636;
	case 0x002654: goto L_002654;
	case 0x00266A: goto L_00266A;
	case 0x002680: goto L_002680;
	case 0x002696: goto L_002696;
	case 0x0026AC: goto L_0026AC;
	case 0x0026C2: goto L_0026C2;
	case 0x002790: goto L_002790;
	case 0x0027A0: goto L_0027A0;
	case 0x0027C0: goto L_0027C0;
	case 0x0027EC: goto L_0027EC;
	case 0x0027FA: goto L_0027FA;
	case 0x002802: goto L_002802;
	case 0x00280A: goto L_00280A;
	case 0x002812: goto L_002812;
	case 0x002822: goto L_002822;
	case 0x002838: goto L_002838;
	case 0x00284E: goto L_00284E;
	case 0x002864: goto L_002864;
	case 0x00287A: goto L_00287A;
	case 0x002890: goto L_002890;
	case 0x0028A6: goto L_0028A6;
	case 0x0028EC: goto L_0028EC;
	case 0x0028FC: goto L_0028FC;
	case 0x00291C: goto L_00291C;
	case 0x00294E: goto L_00294E;
	case 0x00295C: goto L_00295C;
	case 0x002964: goto L_002964;
	case 0x00296C: goto L_00296C;
	case 0x002974: goto L_002974;
	case 0x002984: goto L_002984;
	case 0x0029A0: goto L_0029A0;
	case 0x0029B6: goto L_0029B6;
	case 0x002A54: goto L_002A54;
	case 0x002A64: goto L_002A64;
	case 0x002A74: goto L_002A74;
	case 0x002A84: goto L_002A84;
	case 0x002AA4: goto L_002AA4;
	case 0x002AD6: goto L_002AD6;
	case 0x002AE4: goto L_002AE4;
	case 0x002AEC: goto L_002AEC;
	case 0x002AF4: goto L_002AF4;
	case 0x002AFC: goto L_002AFC;
	case 0x0074D8: goto L_0074D8;
	case 0x007512: goto L_007512;
	case 0x007558: goto L_007558;
	case 0x0075A4: goto L_0075A4;
	case 0x0075C2: goto L_0075C2;
	case 0x00761A: goto L_00761A;
	case 0x007624: goto L_007624;
	case 0x007632: goto L_007632;
	case 0x00764E: goto L_00764E;
	case 0x0078D0: goto L_0078D0;
	case 0x007918: goto L_007918;
	case 0x007920: goto L_007920;
	case 0x00792E: goto L_00792E;
	case 0x007938: goto L_007938;
	case 0x007942: goto L_007942;
	case 0x00794A: goto L_00794A;
	case 0x007958: goto L_007958;
	case 0x007988: goto L_007988;
	case 0x0079A2: goto L_0079A2;
	case 0x0079D2: goto L_0079D2;
	case 0x007A46: goto L_007A46;
	case 0x007A56: goto L_007A56;
	default: return;
	}
L_001D3A:
	{ uint32_t v = RAM16(0xFFFF80CC); set_w(&ctx->d[0], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 001D3A move.w 0xffff80cc,d0 */
	alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFF), 0x1, 16, 0);
	hle_insn(ctx, 8); /* 001D40 cmpi.w #1,d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_001D4E; }  /* 001D44 bne.s 0x1d4e */
	hle_insn(ctx, 8);
	hle_jump(ctx, 0x001D46); return;  /* 001D46 jsr 0x2980 */
L_001D4C:
	hle_insn(ctx, 10); /* 001D4C bra.s 0x1db2 */
	goto L_001DB2;
L_001D4E:
	alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFF), 0x2, 16, 0);
	hle_insn(ctx, 8); /* 001D4E cmpi.w #2,d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_001D5C; }  /* 001D52 bne.s 0x1d5c */
	hle_insn(ctx, 8);
	hle_jump(ctx, 0x001D54); return;  /* 001D54 jsr 0x241e */
L_001D5A:
	hle_insn(ctx, 10); /* 001D5A bra.s 0x1db2 */
	goto L_001DB2;
L_001D5C:
	alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFF), 0x3, 16, 0);
	hle_insn(ctx, 8); /* 001D5C cmpi.w #3,d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_001D6A; }  /* 001D60 bne.s 0x1d6a */
	hle_insn(ctx, 8);
	hle_jump(ctx, 0x001D62); return;  /* 001D62 jsr 0x281e */
L_001D68:
	hle_insn(ctx, 10); /* 001D68 bra.s 0x1db2 */
	goto L_001DB2;
L_001D6A:
	alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFF), 0x4, 16, 0);
	hle_insn(ctx, 8); /* 001D6A cmpi.w #4,d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_001D78; }  /* 001D6E bne.s 0x1d78 */
	hle_insn(ctx, 8);
	hle_jump(ctx, 0x001D70); return;  /* 001D70 jsr 0x2632 */
L_001D76:
	hle_insn(ctx, 10); /* 001D76 bra.s 0x1db2 */
	goto L_001DB2;
L_001D78:
	alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFF), 0x5, 16, 0);
	hle_insn(ctx, 8); /* 001D78 cmpi.w #5,d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_001D86; }  /* 001D7C bne.s 0x1d86 */
	hle_insn(ctx, 8);
	hle_jump(ctx, 0x001D7E); return;  /* 001D7E jsr 0x229e */
L_001D84:
	hle_insn(ctx, 10); /* 001D84 bra.s 0x1db2 */
	goto L_001DB2;
L_001D86:
	alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFF), 0x6, 16, 0);
	hle_insn(ctx, 8); /* 001D86 cmpi.w #6,d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_001D94; }  /* 001D8A bne.s 0x1d94 */
	hle_insn(ctx, 8);
	hle_jump(ctx, 0x001D8C); return;  /* 001D8C jsr 0x2154 */
L_001D92:
	hle_insn(ctx, 10); /* 001D92 bra.s 0x1db2 */
	goto L_001DB2;
L_001D94:
	alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFF), 0x7, 16, 0);
	hle_insn(ctx, 8); /* 001D94 cmpi.w #7,d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_001DA2; }  /* 001D98 bne.s 0x1da2 */
	hle_insn(ctx, 8);
	hle_jump(ctx, 0x001D9A); return;  /* 001D9A jsr 0x1fb2 */
L_001DA0:
	hle_insn(ctx, 10); /* 001DA0 bra.s 0x1db2 */
	goto L_001DB2;
L_001DA2:
	alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFF), 0x8, 16, 0);
	hle_insn(ctx, 8); /* 001DA2 cmpi.w #8,d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_001DB0; }  /* 001DA6 bne.s 0x1db0 */
	hle_insn(ctx, 8);
	hle_jump(ctx, 0x001DA8); return;  /* 001DA8 jsr 0x1db4 */
L_001DAE:
	hle_insn(ctx, 10); /* 001DAE bra.s 0x1db2 */
	goto L_001DB2;
L_001DB0:
	hle_insn(ctx, 4); /* 001DB0 nop */
L_001DB2:
	hle_jump(ctx, 0x001DB2); return;  /* 001DB2 rts */
	hle_jump(ctx, 0x001DB4); return;  /* 001DB4 jsr pc@(0x32ac) */
L_001DB8:
	RAM8(0xFFFF9EC2) = 0;
	hle_insn(ctx, 20); /* 001DB8 sf 0xffff9ec2 */
	RAM8(0xFFFF9EC3) = 0;
	hle_insn(ctx, 20); /* 001DBE sf 0xffff9ec3 */
	{ uint32_t v = 0xFFE0; RAM16(0xFFFF9F60) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 001DC4 move.w #-32,0xffff9f60 */
	RAM16(0xFFFF9EC6) = 0; cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	hle_insn(ctx, 20); /* 001DCC clr.w 0xffff9ec6 */
	ctx->a[0] = 0x000A20F4;
	hle_insn(ctx, 12); /* 001DD2 lea 0xa20f4,a0 */
	{ uint32_t v = 0x1; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 001DD8 move.w #1,d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFF); RAM16(0xFFFF9E38) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 001DDC move.w d1,0xffff9e38 */
	hle_jump(ctx, 0x001DE2); return;  /* 001DE2 jsr 0x1320a */
L_001DE8:
	ctx->a[0] = 0x0009C0D8;
	hle_insn(ctx, 12); /* 001DE8 lea 0x9c0d8,a0 */
	{ uint32_t v = 0x64; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 001DEE move.w #100,d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFF); RAM16(0xFFFF9E48) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 001DF2 move.w d1,0xffff9e48 */
	hle_jump(ctx, 0x001DF8); return;  /* 001DF8 jsr 0x1320a */
L_001DFE:
	ctx->a[0] = 0x0009C632;
	hle_insn(ctx, 12); /* 001DFE lea 0x9c632,a0 */
	{ uint32_t v = 0x8C; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 001E04 move.w #140,d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFF); RAM16(0xFFFF9E74) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 001E08 move.w d1,0xffff9e74 */
	hle_jump(ctx, 0x001E0E); return;  /* 001E0E jsr 0x1320a */
L_001E14:
	ctx->a[0] = 0x000A78D6;
	hle_insn(ctx, 12); /* 001E14 lea 0xa78d6,a0 */
	{ uint32_t v = 0x250; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 001E1A move.w #592,d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFF); RAM16(0xFFFF9E5A) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 001E1E move.w d1,0xffff9e5a */
	hle_jump(ctx, 0x001E24); return;  /* 001E24 jsr 0x1320a */
L_001E2A:
	{ uint32_t v = 0x1F2; RAM16(0xFFFF9E34) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 001E2A move.w #498,0xffff9e34 */
	{ uint32_t v = 0x1DA; RAM16(0xFFFF9E5E) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 001E32 move.w #474,0xffff9e5e */
	{ uint32_t v = 0x322; RAM16(0xFFFF9E76) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 001E3A move.w #802,0xffff9e76 */
	{ uint32_t v = 0x34E; RAM16(0xFFFF9E5C) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 001E42 move.w #846,0xffff9e5c */
	{ uint32_t v = 0x374; RAM16(0xFFFF9E58) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 001E4A move.w #884,0xffff9e58 */
	{ uint32_t v = 0x38F; RAM16(0xFFFF9E1A) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 001E52 move.w #911,0xffff9e1a */
	ctx->a[0] = 0x0008CE10;
	hle_insn(ctx, 12); /* 001E5A lea 0x8ce10,a0 */
	{ uint32_t v = 0x51E; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 001E60 move.w #1310,d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFF); RAM16(0xFFFF9E78) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 001E64 move.w d1,0xffff9e78 */
	{ uint32_t v = 0x40; set_w(&ctx->d[0], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 001E6A move.w #64,d0 */
	hle_jump(ctx, 0x001E6E); return;  /* 001E6E jsr 0x1320c */
L_001E74:
	ctx->a[0] = 0x000DB9D8;
	hle_insn(ctx, 12); /* 001E74 lea 0xdb9d8,a0 */
	{ uint32_t v = 0x522; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 001E7A move.w #1314,d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFF); RAM16(0xFFFF9E44) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 001E7E move.w d1,0xffff9e44 */
	hle_jump(ctx, 0x001E84); return;  /* 001E84 jsr 0x1320a */
L_001E8A:
	ctx->a[0] = 0x00027202;
	hle_insn(ctx, 12); /* 001E8A lea 0x27202,a0 */
	{ uint32_t v = 0x59A; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 001E90 move.w #1434,d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFF); RAM16(0xFFFF9E3C) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 001E94 move.w d1,0xffff9e3c */
	hle_jump(ctx, 0x001E9A); return;  /* 001E9A jsr 0x1320a */
L_001EA0:
	ctx->a[0] = 0x000A1AB2;
	hle_insn(ctx, 12); /* 001EA0 lea 0xa1ab2,a0 */
	{ uint32_t v = 0x527; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 001EA6 move.w #1319,d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFF); RAM16(0xFFFF9E42) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 001EAA move.w d1,0xffff9e42 */
	hle_jump(ctx, 0x001EB0); return;  /* 001EB0 jsr 0x1320a */
L_001EB6:
	ctx->a[0] = 0x000269E0;
	hle_insn(ctx, 12); /* 001EB6 lea 0x269e0,a0 */
	{ uint32_t v = 0x559; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 001EBC move.w #1369,d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFF); RAM16(0xFFFF9E3A) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 001EC0 move.w d1,0xffff9e3a */
	hle_jump(ctx, 0x001EC6); return;  /* 001EC6 jsr 0x1320a */
	hle_jump(ctx, 0x001ECC); return;  /* 001ECC jsr 0x11d12 */
L_001ED2:
	ctx->a[1] = 0x0001346D;
	hle_insn(ctx, 12); /* 001ED2 lea 0x1346d,a1 */
	{ uint32_t v = 0x4FB; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 001ED8 move.w #1275,d1 */
	hle_jump(ctx, 0x001EDC); return;  /* 001EDC jsr 0x13394 */
L_001EE2:
	{ uint32_t v = 0xC140; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 001EE2 move.w #-16064,d1 */
	ctx->a[0] = 0x000A5C16;
	hle_insn(ctx, 12); /* 001EE6 lea 0xa5c16,a0 */
	ctx->d[2] = 0x00000020; cc_logic32(ctx, ctx->d[2]);
	hle_insn(ctx, 4); /* 001EEC moveq #32,d2 */
	ctx->d[3] = 0x00000018; cc_logic32(ctx, ctx->d[3]);
	hle_insn(ctx, 4); /* 001EEE moveq #24,d3 */
	{ uint32_t v = 0x0; ctx->d[6] = v; cc_logic32(ctx, v); }
	hle_insn(ctx, 12); /* 001EF0 move.l #0,d6 */
	{ uint32_t v = RAM16(0xFFFF9E38); set_w(&ctx->d[7], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 001EF6 move.w 0xffff9e38,d7 */
	hle_jump(ctx, 0x001EFC); return;  /* 001EFC jsr 0x12234 */
L_001F02:
	{ uint32_t v = 0xC100; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 001F02 move.w #-16128,d1 */
	ctx->a[0] = 0x000A5C16;
	hle_insn(ctx, 12); /* 001F06 lea 0xa5c16,a0 */
	ctx->d[2] = 0x00000020; cc_logic32(ctx, ctx->d[2]);
	hle_insn(ctx, 4); /* 001F0C moveq #32,d2 */
	ctx->d[3] = 0x00000018; cc_logic32(ctx, ctx->d[3]);
	hle_insn(ctx, 4); /* 001F0E moveq #24,d3 */
	{ uint32_t v = 0x0; ctx->d[6] = v; cc_logic32(ctx, v); }
	hle_insn(ctx, 12); /* 001F10 move.l #0,d6 */
	{ uint32_t v = RAM16(0xFFFF9E38); set_w(&ctx->d[7], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 001F16 move.w 0xffff9e38,d7 */
	hle_jump(ctx, 0x001F1C); return;  /* 001F1C jsr 0x12234 */
	hle_jump(ctx, 0x001F22); return;  /* 001F22 jsr 0x11d28 */
	hle_jump(ctx, 0x001F28); return;  /* 001F28 jsr 0x11d32 */
	hle_jump(ctx, 0x001F2E); return;  /* 001F2E jsr 0x11d12 */
L_001F34:
	{ uint32_t v = 0xEBC0; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 001F34 move.w #-5184,d1 */
	ctx->a[0] = 0x000A63E6;
	hle_insn(ctx, 12); /* 001F38 lea 0xa63e6,a0 */
	ctx->d[2] = 0x00000020; cc_logic32(ctx, ctx->d[2]);
	hle_insn(ctx, 4); /* 001F3E moveq #32,d2 */
	ctx->d[3] = 0x00000003; cc_logic32(ctx, ctx->d[3]);
	hle_insn(ctx, 4); /* 001F40 moveq #3,d3 */
	{ uint32_t v = 0x2000; ctx->d[6] = v; cc_logic32(ctx, v); }
	hle_insn(ctx, 12); /* 001F42 move.l #8192,d6 */
	{ uint32_t v = RAM16(0xFFFF9E38); set_w(&ctx->d[7], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 001F48 move.w 0xffff9e38,d7 */
	hle_jump(ctx, 0x001F4E); return;  /* 001F4E jsr 0x12234 */
L_001F54:
	{ uint32_t v = 0xEB80; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 001F54 move.w #-5248,d1 */
	ctx->a[0] = 0x000A63E6;
	hle_insn(ctx, 12); /* 001F58 lea 0xa63e6,a0 */
	ctx->d[2] = 0x00000020; cc_logic32(ctx, ctx->d[2]);
	hle_insn(ctx, 4); /* 001F5E moveq #32,d2 */
	ctx->d[3] = 0x00000003; cc_logic32(ctx, ctx->d[3]);
	hle_insn(ctx, 4); /* 001F60 moveq #3,d3 */
	{ uint32_t v = 0x2000; ctx->d[6] = v; cc_logic32(ctx, v); }
	hle_insn(ctx, 12); /* 001F62 move.l #8192,d6 */
	{ uint32_t v = RAM16(0xFFFF9E38); set_w(&ctx->d[7], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 001F68 move.w 0xffff9e38,d7 */
	hle_jump(ctx, 0x001F6E); return;  /* 001F6E jsr 0x12234 */
	hle_jump(ctx, 0x001F74); return;  /* 001F74 jsr 0x11d28 */
	hle_jump(ctx, 0x001F7A); return;  /* 001F7A jsr 0x11d32 */
L_001F80:
	ctx->a[0] = 0x0000861A;
	hle_insn(ctx, 8); /* 001F80 lea pc@(0x861a),a0 */
	ctx->a[1] = 0xFFFFA134;
	hle_insn(ctx, 12); /* 001F84 lea 0xffffa134,a1 */
	hle_jump(ctx, 0x001F8A); return;  /* 001F8A jsr pc@(0x8a7c) */
L_001F8E:
	ctx->a[0] = 0x000085DA;
	hle_insn(ctx, 8); /* 001F8E lea pc@(0x85da),a0 */
	hle_jump(ctx, 0x001F92); return;  /* 001F92 jsr pc@(0x8a7c) */
L_001F96:
	ctx->a[0] = 0x000083FA;
	hle_insn(ctx, 8); /* 001F96 lea pc@(0x83fa),a0 */
	hle_jump(ctx, 0x001F9A); return;  /* 001F9A jsr pc@(0x8a7c) */
L_001F9E:
	ctx->a[0] = 0x000085FA;
	hle_insn(ctx, 8); /* 001F9E lea pc@(0x85fa),a0 */
	hle_jump(ctx, 0x001FA2); return;  /* 001FA2 jsr pc@(0x8a7c) */
L_001FA6:
	{ uint32_t v = 0x4A; set_w(&ctx->d[0], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 001FA6 move.w #74,d0 */
	hle_jump(ctx, 0x001FAA); return;  /* 001FAA jsr 0xfbb6c */
	hle_jump(ctx, 0x001FB0); return;  /* 001FB0 rts */
	hle_jump(ctx, 0x001FB2); return;  /* 001FB2 jsr pc@(0x32ac) */
L_001FB6:
	{ uint32_t v = 0xFFFF; RAM16(0xFFFF80B6) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 001FB6 move.w #-1,0xffff80b6 */
	RAM16(0xFFFFA23C) = 0; cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	hle_insn(ctx, 20); /* 001FBE clr.w 0xffffa23c */
	ctx->a[0] = 0x0009A296;
	hle_insn(ctx, 12); /* 001FC4 lea 0x9a296,a0 */
	{ uint32_t v = 0x1; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 001FCA move.w #1,d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFF); RAM16(0xFFFF9E38) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 001FCE move.w d1,0xffff9e38 */
	hle_jump(ctx, 0x001FD4); return;  /* 001FD4 jsr 0x1320a */
L_001FDA:
	ctx->a[0] = 0x00097654;
	hle_insn(ctx, 12); /* 001FDA lea 0x97654,a0 */
	{ uint32_t v = 0xB7; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 001FE0 move.w #183,d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFF); RAM16(0xFFFF9E72) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 001FE4 move.w d1,0xffff9e72 */
	hle_jump(ctx, 0x001FEA); return;  /* 001FEA jsr 0x1320a */
L_001FF0:
	ctx->a[0] = 0x000540B0;
	hle_insn(ctx, 12); /* 001FF0 lea 0x540b0,a0 */
	{ uint32_t v = 0x225; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 001FF6 move.w #549,d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFF); RAM16(0xFFFF9E4C) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 001FFA move.w d1,0xffff9e4c */
	hle_jump(ctx, 0x002000); return;  /* 002000 jsr 0x1320a */
L_002006:
	ctx->a[0] = 0x000C89A2;
	hle_insn(ctx, 12); /* 002006 lea 0xc89a2,a0 */
	{ uint32_t v = 0x289; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 00200C move.w #649,d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFF); RAM16(0xFFFF9E34) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 002010 move.w d1,0xffff9e34 */
	hle_jump(ctx, 0x002016); return;  /* 002016 jsr 0x1320a */
L_00201C:
	{ uint32_t v = 0x289; RAM16(0xFFFF9E74) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 00201C move.w #649,0xffff9e74 */
	ctx->a[0] = 0x00027202;
	hle_insn(ctx, 12); /* 002024 lea 0x27202,a0 */
	{ uint32_t v = 0x59A; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 00202A move.w #1434,d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFF); RAM16(0xFFFF9E3C) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 00202E move.w d1,0xffff9e3c */
	hle_jump(ctx, 0x002034); return;  /* 002034 jsr 0x1320a */
L_00203A:
	ctx->a[0] = 0x000269E0;
	hle_insn(ctx, 12); /* 00203A lea 0x269e0,a0 */
	{ uint32_t v = 0x559; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 002040 move.w #1369,d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFF); RAM16(0xFFFF9E3A) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 002044 move.w d1,0xffff9e3a */
	hle_jump(ctx, 0x00204A); return;  /* 00204A jsr 0x1320a */
	hle_jump(ctx, 0x002050); return;  /* 002050 jsr 0x11d12 */
L_002056:
	{ uint32_t v = 0xC140; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 002056 move.w #-16064,d1 */
	ctx->a[0] = 0x0009B958;
	hle_insn(ctx, 12); /* 00205A lea 0x9b958,a0 */
	ctx->d[2] = 0x00000020; cc_logic32(ctx, ctx->d[2]);
	hle_insn(ctx, 4); /* 002060 moveq #32,d2 */
	ctx->d[3] = 0x0000000E; cc_logic32(ctx, ctx->d[3]);
	hle_insn(ctx, 4); /* 002062 moveq #14,d3 */
	{ uint32_t v = 0x0; ctx->d[6] = v; cc_logic32(ctx, v); }
	hle_insn(ctx, 12); /* 002064 move.l #0,d6 */
	{ uint32_t v = RAM16(0xFFFF9E38); set_w(&ctx->d[7], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 00206A move.w 0xffff9e38,d7 */
	hle_jump(ctx, 0x002070); return;  /* 002070 jsr 0x12234 */
L_002076:
	{ uint32_t v = 0xC100; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 002076 move.w #-16128,d1 */
	ctx->a[0] = 0x0009B958;
	hle_insn(ctx, 12); /* 00207A lea 0x9b958,a0 */
	ctx->d[2] = 0x00000020; cc_logic32(ctx, ctx->d[2]);
	hle_insn(ctx, 4); /* 002080 moveq #32,d2 */
	ctx->d[3] = 0x0000000E; cc_logic32(ctx, ctx->d[3]);
	hle_insn(ctx, 4); /* 002082 moveq #14,d3 */
	{ uint32_t v = 0x0; ctx->d[6] = v; cc_logic32(ctx, v); }
	hle_insn(ctx, 12); /* 002084 move.l #0,d6 */
	{ uint32_t v = RAM16(0xFFFF9E38); set_w(&ctx->d[7], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 00208A move.w 0xffff9e38,d7 */
	hle_jump(ctx, 0x002090); return;  /* 002090 jsr 0x12234 */
L_002096:
	{ uint32_t v = 0xC840; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 002096 move.w #-14272,d1 */
	ctx->a[0] = 0x0009BDB8;
	hle_insn(ctx, 12); /* 00209A lea 0x9bdb8,a0 */
	ctx->d[2] = 0x00000020; cc_logic32(ctx, ctx->d[2]);
	hle_insn(ctx, 4); /* 0020A0 moveq #32,d2 */
	ctx->d[3] = 0x00000004; cc_logic32(ctx, ctx->d[3]);
	hle_insn(ctx, 4); /* 0020A2 moveq #4,d3 */
	{ uint32_t v = 0x2000; ctx->d[6] = v; cc_logic32(ctx, v); }
	hle_insn(ctx, 12); /* 0020A4 move.l #8192,d6 */
	{ uint32_t v = RAM16(0xFFFF9E38); set_w(&ctx->d[7], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 0020AA move.w 0xffff9e38,d7 */
	hle_jump(ctx, 0x0020B0); return;  /* 0020B0 jsr 0x12234 */
L_0020B6:
	{ uint32_t v = 0xC800; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 0020B6 move.w #-14336,d1 */
	ctx->a[0] = 0x0009BDB8;
	hle_insn(ctx, 12); /* 0020BA lea 0x9bdb8,a0 */
	ctx->d[2] = 0x00000020; cc_logic32(ctx, ctx->d[2]);
	hle_insn(ctx, 4); /* 0020C0 moveq #32,d2 */
	ctx->d[3] = 0x00000004; cc_logic32(ctx, ctx->d[3]);
	hle_insn(ctx, 4); /* 0020C2 moveq #4,d3 */
	{ uint32_t v = 0x2000; ctx->d[6] = v; cc_logic32(ctx, v); }
	hle_insn(ctx, 12); /* 0020C4 move.l #8192,d6 */
	{ uint32_t v = RAM16(0xFFFF9E38); set_w(&ctx->d[7], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 0020CA move.w 0xffff9e38,d7 */
	hle_jump(ctx, 0x0020D0); return;  /* 0020D0 jsr 0x12234 */
L_0020D6:
	{ uint32_t v = 0xCA40; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 0020D6 move.w #-13760,d1 */
	ctx->a[0] = 0x0009BEF8;
	hle_insn(ctx, 12); /* 0020DA lea 0x9bef8,a0 */
	ctx->d[2] = 0x00000020; cc_logic32(ctx, ctx->d[2]);
	hle_insn(ctx, 4); /* 0020E0 moveq #32,d2 */
	ctx->d[3] = 0x00000006; cc_logic32(ctx, ctx->d[3]);
	hle_insn(ctx, 4); /* 0020E2 moveq #6,d3 */
	{ uint32_t v = 0x6000; ctx->d[6] = v; cc_logic32(ctx, v); }
	hle_insn(ctx, 12); /* 0020E4 move.l #24576,d6 */
	{ uint32_t v = RAM16(0xFFFF9E38); set_w(&ctx->d[7], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 0020EA move.w 0xffff9e38,d7 */
	hle_jump(ctx, 0x0020F0); return;  /* 0020F0 jsr 0x12234 */
L_0020F6:
	{ uint32_t v = 0xCA00; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 0020F6 move.w #-13824,d1 */
	ctx->a[0] = 0x0009BEF8;
	hle_insn(ctx, 12); /* 0020FA lea 0x9bef8,a0 */
	ctx->d[2] = 0x00000020; cc_logic32(ctx, ctx->d[2]);
	hle_insn(ctx, 4); /* 002100 moveq #32,d2 */
	ctx->d[3] = 0x00000006; cc_logic32(ctx, ctx->d[3]);
	hle_insn(ctx, 4); /* 002102 moveq #6,d3 */
	{ uint32_t v = 0x6000; ctx->d[6] = v; cc_logic32(ctx, v); }
	hle_insn(ctx, 12); /* 002104 move.l #24576,d6 */
	{ uint32_t v = RAM16(0xFFFF9E38); set_w(&ctx->d[7], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 00210A move.w 0xffff9e38,d7 */
	hle_jump(ctx, 0x002110); return;  /* 002110 jsr 0x12234 */
	hle_jump(ctx, 0x002116); return;  /* 002116 jsr 0x11d28 */
	hle_jump(ctx, 0x00211C); return;  /* 00211C jsr 0x11d32 */
L_002122:
	ctx->a[0] = 0x000085BA;
	hle_insn(ctx, 8); /* 002122 lea pc@(0x85ba),a0 */
	ctx->a[1] = 0xFFFFA134;
	hle_insn(ctx, 12); /* 002126 lea 0xffffa134,a1 */
	hle_jump(ctx, 0x00212C); return;  /* 00212C jsr pc@(0x8a7c) */
L_002130:
	ctx->a[0] = 0x000085DA;
	hle_insn(ctx, 8); /* 002130 lea pc@(0x85da),a0 */
	hle_jump(ctx, 0x002134); return;  /* 002134 jsr pc@(0x8a7c) */
L_002138:
	ctx->a[0] = 0x000083FA;
	hle_insn(ctx, 8); /* 002138 lea pc@(0x83fa),a0 */
	hle_jump(ctx, 0x00213C); return;  /* 00213C jsr pc@(0x8a7c) */
L_002140:
	ctx->a[0] = 0x0000843A;
	hle_insn(ctx, 8); /* 002140 lea pc@(0x843a),a0 */
	hle_jump(ctx, 0x002144); return;  /* 002144 jsr pc@(0x8a7c) */
L_002148:
	{ uint32_t v = 0x40; set_w(&ctx->d[0], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 002148 move.w #64,d0 */
	hle_jump(ctx, 0x00214C); return;  /* 00214C jsr 0xfbb6c */
	hle_jump(ctx, 0x002152); return;  /* 002152 rts */
	hle_jump(ctx, 0x002154); return;  /* 002154 jsr pc@(0x32ac) */
L_002158:
	RAM16(0xFFFF9EC6) = 0; cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	hle_insn(ctx, 20); /* 002158 clr.w 0xffff9ec6 */
	ctx->a[0] = 0x0008CE50;
	hle_insn(ctx, 12); /* 00215E lea 0x8ce50,a0 */
	{ uint32_t v = 0x1; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 002164 move.w #1,d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFF); RAM16(0xFFFF9E38) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 002168 move.w d1,0xffff9e38 */
	hle_jump(ctx, 0x00216E); return;  /* 00216E jsr 0x1320a */
L_002174:
	{ uint32_t v = 0x5BB; RAM16(0xFFFF9E42) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 002174 move.w #1467,0xffff9e42 */
	{ uint32_t v = 0x295; RAM16(0xFFFF9E5E) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 00217C move.w #661,0xffff9e5e */
	{ uint32_t v = 0x295; RAM16(0xFFFF9E5C) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 002184 move.w #661,0xffff9e5c */
	{ uint32_t v = 0x29C; RAM16(0xFFFF9E76) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 00218C move.w #668,0xffff9e76 */
	ctx->a[0] = 0x000AC5DA;
	hle_insn(ctx, 12); /* 002194 lea 0xac5da,a0 */
	{ uint32_t v = 0x2AD; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 00219A move.w #685,d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFF); RAM16(0xFFFF9E68) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 00219E move.w d1,0xffff9e68 */
	hle_jump(ctx, 0x0021A4); return;  /* 0021A4 jsr 0x1320a */
L_0021AA:
	ctx->a[0] = 0x000DBA3A;
	hle_insn(ctx, 12); /* 0021AA lea 0xdba3a,a0 */
	{ uint32_t v = 0x3F4; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 0021B0 move.w #1012,d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFF); RAM16(0xFFFF9E56) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 0021B4 move.w d1,0xffff9e56 */
	hle_jump(ctx, 0x0021BA); return;  /* 0021BA jsr 0x1320a */
L_0021C0:
	ctx->a[0] = 0x0008CE10;
	hle_insn(ctx, 12); /* 0021C0 lea 0x8ce10,a0 */
	{ uint32_t v = 0x3F2; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 0021C6 move.w #1010,d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFF); RAM16(0xFFFF9E78) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 0021CA move.w d1,0xffff9e78 */
	{ uint32_t v = 0x40; set_w(&ctx->d[0], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 0021D0 move.w #64,d0 */
	hle_jump(ctx, 0x0021D4); return;  /* 0021D4 jsr 0x1320c */
L_0021DA:
	{ uint32_t v = 0x555; RAM16(0xFFFF9E3C) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 0021DA move.w #1365,0xffff9e3c */
	{ uint32_t v = 0x514; RAM16(0xFFFF9E3A) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 0021E2 move.w #1300,0xffff9e3a */
	{ uint32_t v = 0x4EC; RAM16(0xFFFF9E6C) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 0021EA move.w #1260,0xffff9e6c */
	{ uint32_t v = 0x4C2; RAM16(0xFFFF9E3E) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 0021F2 move.w #1218,0xffff9e3e */
	hle_jump(ctx, 0x0021FA); return;  /* 0021FA jsr 0x11d12 */
L_002200:
	ctx->a[1] = 0x0001345F;
	hle_insn(ctx, 12); /* 002200 lea 0x1345f,a1 */
	{ uint32_t v = 0x6C7; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 002206 move.w #1735,d1 */
	hle_jump(ctx, 0x00220A); return;  /* 00220A jsr 0x13394 */
L_002210:
	ctx->a[1] = 0x00013463;
	hle_insn(ctx, 12); /* 002210 lea 0x13463,a1 */
	{ uint32_t v = 0x7BE; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 002216 move.w #1982,d1 */
	hle_jump(ctx, 0x00221A); return;  /* 00221A jsr 0x13394 */
L_002220:
	{ uint32_t v = 0xCA40; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 002220 move.w #-13760,d1 */
	ctx->a[0] = 0x00089C0E;
	hle_insn(ctx, 12); /* 002224 lea 0x89c0e,a0 */
	ctx->d[2] = 0x00000020; cc_logic32(ctx, ctx->d[2]);
	hle_insn(ctx, 4); /* 00222A moveq #32,d2 */
	ctx->d[3] = 0x00000006; cc_logic32(ctx, ctx->d[3]);
	hle_insn(ctx, 4); /* 00222C moveq #6,d3 */
	{ uint32_t v = 0x0; ctx->d[6] = v; cc_logic32(ctx, v); }
	hle_insn(ctx, 12); /* 00222E move.l #0,d6 */
	{ uint32_t v = RAM16(0xFFFF9E38); set_w(&ctx->d[7], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 002234 move.w 0xffff9e38,d7 */
	hle_jump(ctx, 0x00223A); return;  /* 00223A jsr 0x12234 */
L_002240:
	{ uint32_t v = 0xCA00; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 002240 move.w #-13824,d1 */
	ctx->a[0] = 0x00089C0E;
	hle_insn(ctx, 12); /* 002244 lea 0x89c0e,a0 */
	ctx->d[2] = 0x00000020; cc_logic32(ctx, ctx->d[2]);
	hle_insn(ctx, 4); /* 00224A moveq #32,d2 */
	ctx->d[3] = 0x00000006; cc_logic32(ctx, ctx->d[3]);
	hle_insn(ctx, 4); /* 00224C moveq #6,d3 */
	{ uint32_t v = 0x0; ctx->d[6] = v; cc_logic32(ctx, v); }
	hle_insn(ctx, 12); /* 00224E move.l #0,d6 */
	{ uint32_t v = RAM16(0xFFFF9E38); set_w(&ctx->d[7], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 002254 move.w 0xffff9e38,d7 */
	hle_jump(ctx, 0x00225A); return;  /* 00225A jsr 0x12234 */
	hle_jump(ctx, 0x002260); return;  /* 002260 jsr 0x11d28 */
	hle_jump(ctx, 0x002266); return;  /* 002266 jsr 0x11d32 */
L_00226C:
	ctx->a[0] = 0x0000859A;
	hle_insn(ctx, 8); /* 00226C lea pc@(0x859a),a0 */
	ctx->a[1] = 0xFFFFA134;
	hle_insn(ctx, 12); /* 002270 lea 0xffffa134,a1 */
	hle_jump(ctx, 0x002276); return;  /* 002276 jsr pc@(0x8a7c) */
L_00227A:
	ctx->a[0] = 0x0000863A;
	hle_insn(ctx, 8); /* 00227A lea pc@(0x863a),a0 */
	hle_jump(ctx, 0x00227E); return;  /* 00227E jsr pc@(0x8a7c) */
L_002282:
	ctx->a[0] = 0x000083FA;
	hle_insn(ctx, 8); /* 002282 lea pc@(0x83fa),a0 */
	hle_jump(ctx, 0x002286); return;  /* 002286 jsr pc@(0x8a7c) */
L_00228A:
	ctx->a[0] = 0x0000843A;
	hle_insn(ctx, 8); /* 00228A lea pc@(0x843a),a0 */
	hle_jump(ctx, 0x00228E); return;  /* 00228E jsr pc@(0x8a7c) */
L_002292:
	{ uint32_t v = 0x40; set_w(&ctx->d[0], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 002292 move.w #64,d0 */
	hle_jump(ctx, 0x002296); return;  /* 002296 jsr 0xfbb6c */
	hle_jump(ctx, 0x00229C); return;  /* 00229C rts */
	hle_jump(ctx, 0x00229E); return;  /* 00229E jsr pc@(0x32ac) */
L_0022A2:
	ctx->a[4] = 0xFFFF8000;
	hle_insn(ctx, 12); /* 0022A2 lea 0xffff8000,a4 */
	hle_jump(ctx, 0x0022A8); return;  /* 0022A8 bsr.w 0x240e */
L_0022AC:
	ctx->a[4] = 0xFFFF8050;
	hle_insn(ctx, 12); /* 0022AC lea 0xffff8050,a4 */
	hle_jump(ctx, 0x0022B2); return;  /* 0022B2 bsr.w 0x240e */
L_0022B6:
	{ uint32_t v = 0x11F; RAM16(0xFFFFA0A8) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 0022B6 move.w #287,0xffffa0a8 */
	RAM32_W(0xFFFF9EDA, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	hle_insn(ctx, 28); /* 0022BE clr.l 0xffff9eda */
	RAM16(0xFFFF9EC6) = 0; cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	hle_insn(ctx, 20); /* 0022C4 clr.w 0xffff9ec6 */
	ctx->a[0] = 0x00085D8C;
	hle_insn(ctx, 12); /* 0022CA lea 0x85d8c,a0 */
	{ uint32_t v = 0x1; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 0022D0 move.w #1,d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFF); RAM16(0xFFFF9E38) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 0022D4 move.w d1,0xffff9e38 */
	hle_jump(ctx, 0x0022DA); return;  /* 0022DA jsr 0x1320a */
L_0022E0:
	ctx->a[0] = 0x0008C72E;
	hle_insn(ctx, 12); /* 0022E0 lea 0x8c72e,a0 */
	{ uint32_t v = 0x204; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 0022E6 move.w #516,d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFF); RAM16(0xFFFF9E6A) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 0022EA move.w d1,0xffff9e6a */
	hle_jump(ctx, 0x0022F0); return;  /* 0022F0 jsr 0x1320a */
L_0022F6:
	{ uint32_t v = 0x23B; RAM16(0xFFFF9E78) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 0022F6 move.w #571,0xffff9e78 */
	{ uint32_t v = 0x5BB; RAM16(0xFFFF9E42) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 0022FE move.w #1467,0xffff9e42 */
	{ uint32_t v = 0x23D; RAM16(0xFFFF9E70) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 002306 move.w #573,0xffff9e70 */
	{ uint32_t v = 0x23D; RAM16(0xFFFF9E5E) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 00230E move.w #573,0xffff9e5e */
	ctx->a[0] = 0x000D9556;
	hle_insn(ctx, 12); /* 002316 lea 0xd9556,a0 */
	{ uint32_t v = 0x344; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 00231C move.w #836,d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFF); RAM16(0xFFFF9E1A) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 002320 move.w d1,0xffff9e1a */
	hle_jump(ctx, 0x002326); return;  /* 002326 jsr 0x1320a */
L_00232C:
	ctx->a[0] = 0x000DCCBC;
	hle_insn(ctx, 12); /* 00232C lea 0xdccbc,a0 */
	{ uint32_t v = 0x468; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 002332 move.w #1128,d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFF); RAM16(0xFFFF9E5C) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 002336 move.w d1,0xffff9e5c */
	hle_jump(ctx, 0x00233C); return;  /* 00233C jsr 0x1320a */
L_002342:
	{ uint32_t v = 0x46C; RAM16(0xFFFF9E5A) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 002342 move.w #1132,0xffff9e5a */
	{ uint32_t v = 0x555; RAM16(0xFFFF9E3C) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 00234A move.w #1365,0xffff9e3c */
	{ uint32_t v = 0x514; RAM16(0xFFFF9E3A) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 002352 move.w #1300,0xffff9e3a */
	{ uint32_t v = 0x4CB; RAM16(0xFFFF9E3E) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 00235A move.w #1227,0xffff9e3e */
	{ uint32_t v = 0x4F5; RAM16(0xFFFF9E58) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 002362 move.w #1269,0xffff9e58 */
	hle_jump(ctx, 0x00236A); return;  /* 00236A jsr 0x11d12 */
L_002370:
	ctx->a[1] = 0x0001345F;
	hle_insn(ctx, 12); /* 002370 lea 0x1345f,a1 */
	{ uint32_t v = 0x6C7; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 002376 move.w #1735,d1 */
	hle_jump(ctx, 0x00237A); return;  /* 00237A jsr 0x13394 */
L_002380:
	ctx->a[1] = 0x00013463;
	hle_insn(ctx, 12); /* 002380 lea 0x13463,a1 */
	{ uint32_t v = 0x7BE; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 002386 move.w #1982,d1 */
	hle_jump(ctx, 0x00238A); return;  /* 00238A jsr 0x13394 */
L_002390:
	{ uint32_t v = 0xCA40; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 002390 move.w #-13760,d1 */
	ctx->a[0] = 0x00089C0E;
	hle_insn(ctx, 12); /* 002394 lea 0x89c0e,a0 */
	ctx->d[2] = 0x00000020; cc_logic32(ctx, ctx->d[2]);
	hle_insn(ctx, 4); /* 00239A moveq #32,d2 */
	ctx->d[3] = 0x00000006; cc_logic32(ctx, ctx->d[3]);
	hle_insn(ctx, 4); /* 00239C moveq #6,d3 */
	{ uint32_t v = 0x0; ctx->d[6] = v; cc_logic32(ctx, v); }
	hle_insn(ctx, 12); /* 00239E move.l #0,d6 */
	{ uint32_t v = RAM16(0xFFFF9E38); set_w(&ctx->d[7], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 0023A4 move.w 0xffff9e38,d7 */
	hle_jump(ctx, 0x0023AA); return;  /* 0023AA jsr 0x12234 */
L_0023B0:
	{ uint32_t v = 0xCA00; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 0023B0 move.w #-13824,d1 */
	ctx->a[0] = 0x00089C0E;
	hle_insn(ctx, 12); /* 0023B4 lea 0x89c0e,a0 */
	ctx->d[2] = 0x00000020; cc_logic32(ctx, ctx->d[2]);
	hle_insn(ctx, 4); /* 0023BA moveq #32,d2 */
	ctx->d[3] = 0x00000006; cc_logic32(ctx, ctx->d[3]);
	hle_insn(ctx, 4); /* 0023BC moveq #6,d3 */
	{ uint32_t v = 0x0; ctx->d[6] = v; cc_logic32(ctx, v); }
	hle_insn(ctx, 12); /* 0023BE move.l #0,d6 */
	{ uint32_t v = RAM16(0xFFFF9E38); set_w(&ctx->d[7], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 0023C4 move.w 0xffff9e38,d7 */
	hle_jump(ctx, 0x0023CA); return;  /* 0023CA jsr 0x12234 */
	hle_jump(ctx, 0x0023D0); return;  /* 0023D0 jsr 0x11d28 */
	hle_jump(ctx, 0x0023D6); return;  /* 0023D6 jsr 0x11d32 */
L_0023DC:
	ctx->a[0] = 0x0000857A;
	hle_insn(ctx, 8); /* 0023DC lea pc@(0x857a),a0 */
	ctx->a[1] = 0xFFFFA134;
	hle_insn(ctx, 12); /* 0023E0 lea 0xffffa134,a1 */
	hle_jump(ctx, 0x0023E6); return;  /* 0023E6 jsr pc@(0x8a7c) */
L_0023EA:
	ctx->a[0] = 0x0000863A;
	hle_insn(ctx, 8); /* 0023EA lea pc@(0x863a),a0 */
	hle_jump(ctx, 0x0023EE); return;  /* 0023EE jsr pc@(0x8a7c) */
L_0023F2:
	ctx->a[0] = 0x000083FA;
	hle_insn(ctx, 8); /* 0023F2 lea pc@(0x83fa),a0 */
	hle_jump(ctx, 0x0023F6); return;  /* 0023F6 jsr pc@(0x8a7c) */
L_0023FA:
	ctx->a[0] = 0x0000843A;
	hle_insn(ctx, 8); /* 0023FA lea pc@(0x843a),a0 */
	hle_jump(ctx, 0x0023FE); return;  /* 0023FE jsr pc@(0x8a7c) */
L_002402:
	{ uint32_t v = 0x40; set_w(&ctx->d[0], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 002402 move.w #64,d0 */
	hle_jump(ctx, 0x002406); return;  /* 002406 jsr 0xfbb6c */
	hle_jump(ctx, 0x00240C); return;  /* 00240C rts */
	hle_jump(ctx, 0x00241E); return;  /* 00241E jsr pc@(0x32ac) */
L_002422:
	{ uint32_t v = 0xFFFF; RAM16(0xFFFF80B6) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 002422 move.w #-1,0xffff80b6 */
	ctx->a[0] = 0x0006F4D0;
	hle_insn(ctx, 12); /* 00242A lea 0x6f4d0,a0 */
	{ uint32_t v = 0x1; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 002430 move.w #1,d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFF); RAM16(0xFFFF9E38) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 002434 move.w d1,0xffff9e38 */
	hle_jump(ctx, 0x00243A); return;  /* 00243A jsr 0x1320a */
L_002440:
	ctx->a[0] = 0x00073902;
	hle_insn(ctx, 12); /* 002440 lea 0x73902,a0 */
	{ uint32_t v = 0x16C; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 002446 move.w #364,d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFF); RAM16(0xFFFF9E4E) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 00244A move.w d1,0xffff9e4e */
	hle_jump(ctx, 0x002450); return;  /* 002450 jsr 0x1320a */
L_002456:
	ctx->a[0] = 0x0007E2B0;
	hle_insn(ctx, 12); /* 002456 lea 0x7e2b0,a0 */
	{ uint32_t v = 0x22A; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 00245C move.w #554,d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFF); RAM16(0xFFFF9E78) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 002460 move.w d1,0xffff9e78 */
	hle_jump(ctx, 0x002466); return;  /* 002466 jsr 0x1320a */
L_00246C:
	{ uint32_t v = 0x3E9; RAM16(0xFFFF9E4C) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 00246C move.w #1001,0xffff9e4c */
	{ uint32_t v = 0x1CD; RAM16(0xFFFF9E5A) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 002474 move.w #461,0xffff9e5a */
	{ uint32_t v = 0x4AA; RAM16(0xFFFF9E5C) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 00247C move.w #1194,0xffff9e5c */
	{ uint32_t v = 0x365; RAM16(0xFFFF9E5E) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 002484 move.w #869,0xffff9e5e */
	{ uint32_t v = 0x3E9; RAM16(0xFFFF9E62) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 00248C move.w #1001,0xffff9e62 */
	{ uint32_t v = 0x41B; RAM16(0xFFFF9E64) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 002494 move.w #1051,0xffff9e64 */
	{ uint32_t v = 0x1CD; RAM16(0xFFFF9E68) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 00249C move.w #461,0xffff9e68 */
	{ uint32_t v = 0x24F; RAM16(0xFFFF9E58) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 0024A4 move.w #591,0xffff9e58 */
	{ uint32_t v = 0x28B; RAM16(0xFFFF9E6C) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 0024AC move.w #651,0xffff9e6c */
	{ uint32_t v = 0x30D; RAM16(0xFFFF9E6E) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 0024B4 move.w #781,0xffff9e6e */
	{ uint32_t v = 0x28B; RAM16(0xFFFF9E46) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 0024BC move.w #651,0xffff9e46 */
	{ uint32_t v = 0x569; RAM16(0xFFFF9E3A) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 0024C4 move.w #1385,0xffff9e3a */
	{ uint32_t v = 0x5AA; RAM16(0xFFFF9E60) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 0024CC move.w #1450,0xffff9e60 */
	{ uint32_t v = 0x503; RAM16(0xFFFF9E3C) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 0024D4 move.w #1283,0xffff9e3c */
	{ uint32_t v = 0x422; RAM16(0xFFFF9E56) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 0024DC move.w #1058,0xffff9e56 */
	{ uint32_t v = 0x372; RAM16(0xFFFF9E1C) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 0024E4 move.w #882,0xffff9e1c */
	hle_jump(ctx, 0x0024EC); return;  /* 0024EC jsr 0x11d12 */
L_0024F2:
	ctx->a[1] = 0x00013454;
	hle_insn(ctx, 12); /* 0024F2 lea 0x13454,a1 */
	{ uint32_t v = 0x5D2; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 0024F8 move.w #1490,d1 */
	hle_jump(ctx, 0x0024FC); return;  /* 0024FC jsr 0x13394 */
L_002502:
	{ uint32_t v = 0xC140; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 002502 move.w #-16064,d1 */
	ctx->a[0] = 0x00072232;
	hle_insn(ctx, 12); /* 002506 lea 0x72232,a0 */
	ctx->d[2] = 0x00000020; cc_logic32(ctx, ctx->d[2]);
	hle_insn(ctx, 4); /* 00250C moveq #32,d2 */
	ctx->d[3] = 0x00000018; cc_logic32(ctx, ctx->d[3]);
	hle_insn(ctx, 4); /* 00250E moveq #24,d3 */
	{ uint32_t v = 0x0; ctx->d[6] = v; cc_logic32(ctx, v); }
	hle_insn(ctx, 12); /* 002510 move.l #0,d6 */
	{ uint32_t v = RAM16(0xFFFF9E38); set_w(&ctx->d[7], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 002516 move.w 0xffff9e38,d7 */
	hle_jump(ctx, 0x00251C); return;  /* 00251C jsr 0x12234 */
L_002522:
	{ uint32_t v = 0xC100; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 002522 move.w #-16128,d1 */
	ctx->a[0] = 0x00072232;
	hle_insn(ctx, 12); /* 002526 lea 0x72232,a0 */
	ctx->d[2] = 0x00000020; cc_logic32(ctx, ctx->d[2]);
	hle_insn(ctx, 4); /* 00252C moveq #32,d2 */
	ctx->d[3] = 0x00000018; cc_logic32(ctx, ctx->d[3]);
	hle_insn(ctx, 4); /* 00252E moveq #24,d3 */
	{ uint32_t v = 0x0; ctx->d[6] = v; cc_logic32(ctx, v); }
	hle_insn(ctx, 12); /* 002530 move.l #0,d6 */
	{ uint32_t v = RAM16(0xFFFF9E38); set_w(&ctx->d[7], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 002536 move.w 0xffff9e38,d7 */
	hle_jump(ctx, 0x00253C); return;  /* 00253C jsr 0x12234 */
L_002542:
	{ uint32_t v = 0xC540; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 002542 move.w #-15040,d1 */
	ctx->a[0] = 0x000729B2;
	hle_insn(ctx, 12); /* 002546 lea 0x729b2,a0 */
	ctx->d[2] = 0x00000020; cc_logic32(ctx, ctx->d[2]);
	hle_insn(ctx, 4); /* 00254C moveq #32,d2 */
	ctx->d[3] = 0x0000000B; cc_logic32(ctx, ctx->d[3]);
	hle_insn(ctx, 4); /* 00254E moveq #11,d3 */
	{ uint32_t v = 0x0; ctx->d[6] = v; cc_logic32(ctx, v); }
	hle_insn(ctx, 12); /* 002550 move.l #0,d6 */
	{ uint32_t v = RAM16(0xFFFF9E38); set_w(&ctx->d[7], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 002556 move.w 0xffff9e38,d7 */
	hle_jump(ctx, 0x00255C); return;  /* 00255C jsr 0x12234 */
	hle_jump(ctx, 0x002562); return;  /* 002562 jsr 0x11d28 */
	hle_jump(ctx, 0x002568); return;  /* 002568 jsr 0x11d32 */
	hle_jump(ctx, 0x00256E); return;  /* 00256E jsr 0x11d12 */
L_002574:
	{ uint32_t v = 0xE100; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 002574 move.w #-7936,d1 */
	ctx->a[0] = 0x00073132;
	hle_insn(ctx, 12); /* 002578 lea 0x73132,a0 */
	ctx->d[2] = 0x00000018; cc_logic32(ctx, ctx->d[2]);
	hle_insn(ctx, 4); /* 00257E moveq #24,d2 */
	ctx->d[3] = 0x00000018; cc_logic32(ctx, ctx->d[3]);
	hle_insn(ctx, 4); /* 002580 moveq #24,d3 */
	{ uint32_t v = 0x8000; ctx->d[6] = v; cc_logic32(ctx, v); }
	hle_insn(ctx, 12); /* 002582 move.l #32768,d6 */
	{ uint32_t v = RAM16(0xFFFF9E38); set_w(&ctx->d[7], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 002588 move.w 0xffff9e38,d7 */
	hle_jump(ctx, 0x00258E); return;  /* 00258E jsr 0x12234 */
L_002594:
	{ uint32_t v = 0xE130; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 002594 move.w #-7888,d1 */
	ctx->a[0] = 0x00073142;
	hle_insn(ctx, 12); /* 002598 lea 0x73142,a0 */
	ctx->d[2] = 0x00000010; cc_logic32(ctx, ctx->d[2]);
	hle_insn(ctx, 4); /* 00259E moveq #16,d2 */
	ctx->d[3] = 0x00000018; cc_logic32(ctx, ctx->d[3]);
	hle_insn(ctx, 4); /* 0025A0 moveq #24,d3 */
	{ uint32_t v = 0x8000; ctx->d[6] = v; cc_logic32(ctx, v); }
	hle_insn(ctx, 12); /* 0025A2 move.l #32768,d6 */
	{ uint32_t v = RAM16(0xFFFF9E38); set_w(&ctx->d[7], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 0025A8 move.w 0xffff9e38,d7 */
	hle_jump(ctx, 0x0025AE); return;  /* 0025AE jsr 0x12234 */
L_0025B4:
	{ uint32_t v = 0xE150; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 0025B4 move.w #-7856,d1 */
	ctx->a[0] = 0x00073142;
	hle_insn(ctx, 12); /* 0025B8 lea 0x73142,a0 */
	ctx->d[2] = 0x00000008; cc_logic32(ctx, ctx->d[2]);
	hle_insn(ctx, 4); /* 0025BE moveq #8,d2 */
	ctx->d[3] = 0x00000018; cc_logic32(ctx, ctx->d[3]);
	hle_insn(ctx, 4); /* 0025C0 moveq #24,d3 */
	{ uint32_t v = 0x8000; ctx->d[6] = v; cc_logic32(ctx, v); }
	hle_insn(ctx, 12); /* 0025C2 move.l #32768,d6 */
	{ uint32_t v = RAM16(0xFFFF9E38); set_w(&ctx->d[7], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 0025C8 move.w 0xffff9e38,d7 */
	hle_jump(ctx, 0x0025CE); return;  /* 0025CE jsr 0x12234 */
L_0025D4:
	{ uint32_t v = 0xE160; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 0025D4 move.w #-7840,d1 */
	ctx->a[0] = 0x00073172;
	hle_insn(ctx, 12); /* 0025D8 lea 0x73172,a0 */
	ctx->d[2] = 0x00000008; cc_logic32(ctx, ctx->d[2]);
	hle_insn(ctx, 4); /* 0025DE moveq #8,d2 */
	ctx->d[3] = 0x00000018; cc_logic32(ctx, ctx->d[3]);
	hle_insn(ctx, 4); /* 0025E0 moveq #24,d3 */
	{ uint32_t v = 0x8000; ctx->d[6] = v; cc_logic32(ctx, v); }
	hle_insn(ctx, 12); /* 0025E2 move.l #32768,d6 */
	{ uint32_t v = RAM16(0xFFFF9E38); set_w(&ctx->d[7], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 0025E8 move.w 0xffff9e38,d7 */
	hle_jump(ctx, 0x0025EE); return;  /* 0025EE jsr 0x12234 */
	hle_jump(ctx, 0x0025F4); return;  /* 0025F4 jsr 0x11d28 */
	hle_jump(ctx, 0x0025FA); return;  /* 0025FA jsr 0x11d32 */
L_002600:
	ctx->a[0] = 0x0000851A;
	hle_insn(ctx, 8); /* 002600 lea pc@(0x851a),a0 */
	ctx->a[1] = 0xFFFFA134;
	hle_insn(ctx, 12); /* 002604 lea 0xffffa134,a1 */
	hle_jump(ctx, 0x00260A); return;  /* 00260A jsr pc@(0x8a7c) */
L_00260E:
	ctx->a[0] = 0x0000855A;
	hle_insn(ctx, 8); /* 00260E lea pc@(0x855a),a0 */
	hle_jump(ctx, 0x002612); return;  /* 002612 jsr pc@(0x8a7c) */
L_002616:
	ctx->a[0] = 0x000083FA;
	hle_insn(ctx, 8); /* 002616 lea pc@(0x83fa),a0 */
	hle_jump(ctx, 0x00261A); return;  /* 00261A jsr pc@(0x8a7c) */
L_00261E:
	ctx->a[0] = 0x0000843A;
	hle_insn(ctx, 8); /* 00261E lea pc@(0x843a),a0 */
	hle_jump(ctx, 0x002622); return;  /* 002622 jsr pc@(0x8a7c) */
L_002626:
	{ uint32_t v = 0x19; set_w(&ctx->d[0], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 002626 move.w #25,d0 */
	hle_jump(ctx, 0x00262A); return;  /* 00262A jsr 0xfbb6c */
	hle_jump(ctx, 0x002630); return;  /* 002630 rts */
	hle_jump(ctx, 0x002632); return;  /* 002632 jsr pc@(0x32ac) */
L_002636:
	{ uint32_t v = 0xFFFF; RAM16(0xFFFF80B6) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 002636 move.w #-1,0xffff80b6 */
	ctx->a[0] = 0x00025DFC;
	hle_insn(ctx, 12); /* 00263E lea 0x25dfc,a0 */
	{ uint32_t v = 0x670; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 002644 move.w #1648,d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFF); RAM16(0xFFFF9E2C) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 002648 move.w d1,0xffff9e2c */
	hle_jump(ctx, 0x00264E); return;  /* 00264E jsr 0x1320a */
L_002654:
	ctx->a[0] = 0x00061C22;
	hle_insn(ctx, 12); /* 002654 lea 0x61c22,a0 */
	{ uint32_t v = 0x1; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 00265A move.w #1,d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFF); RAM16(0xFFFF9E38) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 00265E move.w d1,0xffff9e38 */
	hle_jump(ctx, 0x002664); return;  /* 002664 jsr 0x1320a */
L_00266A:
	ctx->a[0] = 0x0002585A;
	hle_insn(ctx, 12); /* 00266A lea 0x2585a,a0 */
	{ uint32_t v = 0x5B7; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 002670 move.w #1463,d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFF); RAM16(0xFFFF9E28) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 002674 move.w d1,0xffff9e28 */
	hle_jump(ctx, 0x00267A); return;  /* 00267A jsr 0x1320a */
L_002680:
	ctx->a[0] = 0x00068648;
	hle_insn(ctx, 12); /* 002680 lea 0x68648,a0 */
	{ uint32_t v = 0x1C3; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 002686 move.w #451,d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFF); RAM16(0xFFFF9E54) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 00268A move.w d1,0xffff9e54 */
	hle_jump(ctx, 0x002690); return;  /* 002690 jsr 0x1320a */
L_002696:
	ctx->a[0] = 0x000BCBBA;
	hle_insn(ctx, 12); /* 002696 lea 0xbcbba,a0 */
	{ uint32_t v = 0x257; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 00269C move.w #599,d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFF); RAM16(0xFFFF9E36) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 0026A0 move.w d1,0xffff9e36 */
	hle_jump(ctx, 0x0026A6); return;  /* 0026A6 jsr 0x1320a */
L_0026AC:
	ctx->a[0] = 0x000473C4;
	hle_insn(ctx, 12); /* 0026AC lea 0x473c4,a0 */
	{ uint32_t v = 0x3B2; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 0026B2 move.w #946,d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFF); RAM16(0xFFFF9E44) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 0026B6 move.w d1,0xffff9e44 */
	hle_jump(ctx, 0x0026BC); return;  /* 0026BC jsr 0x1320a */
L_0026C2:
	{ uint32_t v = 0x1C4; RAM16(0xFFFF9E46) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 0026C2 move.w #452,0xffff9e46 */
	{ uint32_t v = 0x174; RAM16(0xFFFF9E50) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 0026CA move.w #372,0xffff9e50 */
	{ uint32_t v = 0x10E; RAM16(0xFFFF9E4E) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 0026D2 move.w #270,0xffff9e4e */
	{ uint32_t v = 0x1C3; RAM16(0xFFFF9E48) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 0026DA move.w #451,0xffff9e48 */
	{ uint32_t v = 0x2DE; RAM16(0xFFFF9E4C) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 0026E2 move.w #734,0xffff9e4c */
	{ uint32_t v = 0x378; RAM16(0xFFFF9E5A) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 0026EA move.w #888,0xffff9e5a */
	{ uint32_t v = 0x3B2; RAM16(0xFFFF9E5C) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 0026F2 move.w #946,0xffff9e5c */
	{ uint32_t v = 0x1C3; RAM16(0xFFFF9E5E) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 0026FA move.w #451,0xffff9e5e */
	{ uint32_t v = 0x1C3; RAM16(0xFFFF9E66) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 002702 move.w #451,0xffff9e66 */
	{ uint32_t v = 0x257; RAM16(0xFFFF9E30) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 00270A move.w #599,0xffff9e30 */
	{ uint32_t v = 0x1C3; RAM16(0xFFFF9E1E) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 002712 move.w #451,0xffff9e1e */
	{ uint32_t v = 0x27A; RAM16(0xFFFF9E1C) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 00271A move.w #634,0xffff9e1c */
	{ uint32_t v = 0x32A; RAM16(0xFFFF9E56) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 002722 move.w #810,0xffff9e56 */
	{ uint32_t v = 0x4B0; RAM16(0xFFFF9E34) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 00272A move.w #1200,0xffff9e34 */
	{ uint32_t v = 0x6ED; RAM16(0xFFFF9E76) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 002732 move.w #1773,0xffff9e76 */
	{ uint32_t v = 0x4BE; RAM16(0xFFFF9E3E) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 00273A move.w #1214,0xffff9e3e */
	{ uint32_t v = 0x4E8; RAM16(0xFFFF9E3C) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 002742 move.w #1256,0xffff9e3c */
	{ uint32_t v = 0x54E; RAM16(0xFFFF9E3A) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 00274A move.w #1358,0xffff9e3a */
	{ uint32_t v = 0x58F; RAM16(0xFFFF9E24) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 002752 move.w #1423,0xffff9e24 */
	{ uint32_t v = 0x58F; RAM16(0xFFFF9E60) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 00275A move.w #1423,0xffff9e60 */
	{ uint32_t v = 0x121; RAM16(0xFFFF9E4A) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 002762 move.w #289,0xffff9e4a */
	{ uint32_t v = 0x29D; RAM16(0xFFFF9E58) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 00276A move.w #669,0xffff9e58 */
	{ uint32_t v = 0x2D9; RAM16(0xFFFF9E6E) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 002772 move.w #729,0xffff9e6e */
	{ uint32_t v = 0x44A; RAM16(0xFFFF9E68) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 00277A move.w #1098,0xffff9e68 */
	{ uint32_t v = 0x3D6; RAM16(0xFFFF9E6C) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 002782 move.w #982,0xffff9e6c */
	hle_jump(ctx, 0x00278A); return;  /* 00278A jsr 0x11d12 */
L_002790:
	ctx->a[1] = 0x0001345A;
	hle_insn(ctx, 12); /* 002790 lea 0x1345a,a1 */
	{ uint32_t v = 0x5E4; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 002796 move.w #1508,d1 */
	hle_jump(ctx, 0x00279A); return;  /* 00279A jsr 0x13394 */
L_0027A0:
	{ uint32_t v = 0xC140; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 0027A0 move.w #-16064,d1 */
	ctx->a[0] = 0x00065264;
	hle_insn(ctx, 12); /* 0027A4 lea 0x65264,a0 */
	ctx->d[2] = 0x00000020; cc_logic32(ctx, ctx->d[2]);
	hle_insn(ctx, 4); /* 0027AA moveq #32,d2 */
	ctx->d[3] = 0x00000018; cc_logic32(ctx, ctx->d[3]);
	hle_insn(ctx, 4); /* 0027AC moveq #24,d3 */
	{ uint32_t v = 0x0; ctx->d[6] = v; cc_logic32(ctx, v); }
	hle_insn(ctx, 12); /* 0027AE move.l #0,d6 */
	{ uint32_t v = RAM16(0xFFFF9E38); set_w(&ctx->d[7], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 0027B4 move.w 0xffff9e38,d7 */
	hle_jump(ctx, 0x0027BA); return;  /* 0027BA jsr 0x12234 */
L_0027C0:
	{ uint32_t v = 0xC100; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 0027C0 move.w #-16128,d1 */
	ctx->a[0] = 0x00065264;
	hle_insn(ctx, 12); /* 0027C4 lea 0x65264,a0 */
	ctx->d[2] = 0x00000020; cc_logic32(ctx, ctx->d[2]);
	hle_insn(ctx, 4); /* 0027CA moveq #32,d2 */
	ctx->d[3] = 0x00000018; cc_logic32(ctx, ctx->d[3]);
	hle_insn(ctx, 4); /* 0027CC moveq #24,d3 */
	{ uint32_t v = 0x0; ctx->d[6] = v; cc_logic32(ctx, v); }
	hle_insn(ctx, 12); /* 0027CE move.l #0,d6 */
	{ uint32_t v = RAM16(0xFFFF9E38); set_w(&ctx->d[7], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 0027D4 move.w 0xffff9e38,d7 */
	hle_jump(ctx, 0x0027DA); return;  /* 0027DA jsr 0x12234 */
	hle_jump(ctx, 0x0027E0); return;  /* 0027E0 jsr 0x11d28 */
	hle_jump(ctx, 0x0027E6); return;  /* 0027E6 jsr 0x11d32 */
L_0027EC:
	ctx->a[0] = 0x000084BA;
	hle_insn(ctx, 8); /* 0027EC lea pc@(0x84ba),a0 */
	ctx->a[1] = 0xFFFFA134;
	hle_insn(ctx, 12); /* 0027F0 lea 0xffffa134,a1 */
	hle_jump(ctx, 0x0027F6); return;  /* 0027F6 jsr pc@(0x8a7c) */
L_0027FA:
	ctx->a[0] = 0x0000849A;
	hle_insn(ctx, 8); /* 0027FA lea pc@(0x849a),a0 */
	hle_jump(ctx, 0x0027FE); return;  /* 0027FE jsr pc@(0x8a7c) */
L_002802:
	ctx->a[0] = 0x000083FA;
	hle_insn(ctx, 8); /* 002802 lea pc@(0x83fa),a0 */
	hle_jump(ctx, 0x002806); return;  /* 002806 jsr pc@(0x8a7c) */
L_00280A:
	ctx->a[0] = 0x0000843A;
	hle_insn(ctx, 8); /* 00280A lea pc@(0x843a),a0 */
	hle_jump(ctx, 0x00280E); return;  /* 00280E jsr pc@(0x8a7c) */
L_002812:
	{ uint32_t v = 0x28; set_w(&ctx->d[0], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 002812 move.w #40,d0 */
	hle_jump(ctx, 0x002816); return;  /* 002816 jsr 0xfbb6c */
	hle_jump(ctx, 0x00281C); return;  /* 00281C rts */
	hle_jump(ctx, 0x00281E); return;  /* 00281E jsr pc@(0x32ac) */
L_002822:
	ctx->a[0] = 0x000433E2;
	hle_insn(ctx, 12); /* 002822 lea 0x433e2,a0 */
	{ uint32_t v = 0x1; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 002828 move.w #1,d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFF); RAM16(0xFFFF9E38) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 00282C move.w d1,0xffff9e38 */
	hle_jump(ctx, 0x002832); return;  /* 002832 jsr 0x1320a */
L_002838:
	ctx->a[0] = 0x0002585A;
	hle_insn(ctx, 12); /* 002838 lea 0x2585a,a0 */
	{ uint32_t v = 0x599; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 00283E move.w #1433,d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFF); RAM16(0xFFFF9E28) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 002842 move.w d1,0xffff9e28 */
	hle_jump(ctx, 0x002848); return;  /* 002848 jsr 0x1320a */
L_00284E:
	ctx->a[0] = 0x00050BEE;
	hle_insn(ctx, 12); /* 00284E lea 0x50bee,a0 */
	{ uint32_t v = 0xB2; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 002854 move.w #178,d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFF); RAM16(0xFFFF9E4A) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 002858 move.w d1,0xffff9e4a */
	hle_jump(ctx, 0x00285E); return;  /* 00285E jsr 0x1320a */
L_002864:
	ctx->a[0] = 0x000540B0;
	hle_insn(ctx, 12); /* 002864 lea 0x540b0,a0 */
	{ uint32_t v = 0x221; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 00286A move.w #545,d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFF); RAM16(0xFFFF9E4C) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 00286E move.w d1,0xffff9e4c */
	hle_jump(ctx, 0x002874); return;  /* 002874 jsr 0x1320a */
L_00287A:
	ctx->a[0] = 0x000269E0;
	hle_insn(ctx, 12); /* 00287A lea 0x269e0,a0 */
	{ uint32_t v = 0x53F; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 002880 move.w #1343,d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFF); RAM16(0xFFFF9E3A) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 002884 move.w d1,0xffff9e3a */
	hle_jump(ctx, 0x00288A); return;  /* 00288A jsr 0x1320a */
L_002890:
	ctx->a[0] = 0x0005C8CC;
	hle_insn(ctx, 12); /* 002890 lea 0x5c8cc,a0 */
	{ uint32_t v = 0x28F; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 002896 move.w #655,d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFF); RAM16(0xFFFF9E54) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 00289A move.w d1,0xffff9e54 */
	hle_jump(ctx, 0x0028A0); return;  /* 0028A0 jsr 0x1320a */
L_0028A6:
	{ uint32_t v = 0x3C3; RAM16(0xFFFF9E1A) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 0028A6 move.w #963,0xffff9e1a */
	{ uint32_t v = 0x4EB; RAM16(0xFFFF9E42) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 0028AE move.w #1259,0xffff9e42 */
	{ uint32_t v = 0x333; RAM16(0xFFFF9E4E) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 0028B6 move.w #819,0xffff9e4e */
	{ uint32_t v = 0x292; RAM16(0xFFFF9E34) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 0028BE move.w #658,0xffff9e34 */
	{ uint32_t v = 0x4EB; RAM16(0xFFFF9E24) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 0028C6 move.w #1259,0xffff9e24 */
	{ uint32_t v = 0xB2; RAM16(0xFFFF9E36) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 0028CE move.w #178,0xffff9e36 */
	{ uint32_t v = 0x28F; RAM16(0xFFFF9E44) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 0028D6 move.w #655,0xffff9e44 */
	{ uint32_t v = 0x357; RAM16(0xFFFF9E3C) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 0028DE move.w #855,0xffff9e3c */
	hle_jump(ctx, 0x0028E6); return;  /* 0028E6 jsr 0x11d12 */
L_0028EC:
	ctx->a[1] = 0x00013469;
	hle_insn(ctx, 12); /* 0028EC lea 0x13469,a1 */
	{ uint32_t v = 0x5C6; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 0028F2 move.w #1478,d1 */
	hle_jump(ctx, 0x0028F6); return;  /* 0028F6 jsr 0x13394 */
L_0028FC:
	{ uint32_t v = 0xC140; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 0028FC move.w #-16064,d1 */
	ctx->a[0] = 0x00046C44;
	hle_insn(ctx, 12); /* 002900 lea 0x46c44,a0 */
	ctx->d[2] = 0x00000020; cc_logic32(ctx, ctx->d[2]);
	hle_insn(ctx, 4); /* 002906 moveq #32,d2 */
	ctx->d[3] = 0x00000018; cc_logic32(ctx, ctx->d[3]);
	hle_insn(ctx, 4); /* 002908 moveq #24,d3 */
	{ uint32_t v = 0x0; ctx->d[6] = v; cc_logic32(ctx, v); }
	hle_insn(ctx, 12); /* 00290A move.l #0,d6 */
	{ uint32_t v = RAM16(0xFFFF9E38); set_w(&ctx->d[7], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 002910 move.w 0xffff9e38,d7 */
	hle_jump(ctx, 0x002916); return;  /* 002916 jsr 0x12234 */
L_00291C:
	{ uint32_t v = 0xC100; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 00291C move.w #-16128,d1 */
	ctx->a[0] = 0x00046C44;
	hle_insn(ctx, 12); /* 002920 lea 0x46c44,a0 */
	ctx->d[2] = 0x00000020; cc_logic32(ctx, ctx->d[2]);
	hle_insn(ctx, 4); /* 002926 moveq #32,d2 */
	ctx->d[3] = 0x00000018; cc_logic32(ctx, ctx->d[3]);
	hle_insn(ctx, 4); /* 002928 moveq #24,d3 */
	{ uint32_t v = 0x0; ctx->d[6] = v; cc_logic32(ctx, v); }
	hle_insn(ctx, 12); /* 00292A move.l #0,d6 */
	{ uint32_t v = RAM16(0xFFFF9E38); set_w(&ctx->d[7], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 002930 move.w 0xffff9e38,d7 */
	hle_jump(ctx, 0x002936); return;  /* 002936 jsr 0x12234 */
	hle_jump(ctx, 0x00293C); return;  /* 00293C jsr 0xbe42 */
	hle_jump(ctx, 0x002942); return;  /* 002942 jsr 0x11d28 */
	hle_jump(ctx, 0x002948); return;  /* 002948 jsr 0x11d32 */
L_00294E:
	ctx->a[0] = 0x000084BA;
	hle_insn(ctx, 8); /* 00294E lea pc@(0x84ba),a0 */
	ctx->a[1] = 0xFFFFA134;
	hle_insn(ctx, 12); /* 002952 lea 0xffffa134,a1 */
	hle_jump(ctx, 0x002958); return;  /* 002958 jsr pc@(0x8a7c) */
L_00295C:
	ctx->a[0] = 0x0000849A;
	hle_insn(ctx, 8); /* 00295C lea pc@(0x849a),a0 */
	hle_jump(ctx, 0x002960); return;  /* 002960 jsr pc@(0x8a7c) */
L_002964:
	ctx->a[0] = 0x000083FA;
	hle_insn(ctx, 8); /* 002964 lea pc@(0x83fa),a0 */
	hle_jump(ctx, 0x002968); return;  /* 002968 jsr pc@(0x8a7c) */
L_00296C:
	ctx->a[0] = 0x0000843A;
	hle_insn(ctx, 8); /* 00296C lea pc@(0x843a),a0 */
	hle_jump(ctx, 0x002970); return;  /* 002970 jsr pc@(0x8a7c) */
L_002974:
	{ uint32_t v = 0x2; set_w(&ctx->d[0], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 002974 move.w #2,d0 */
	hle_jump(ctx, 0x002978); return;  /* 002978 jsr 0xfbb6c */
	hle_jump(ctx, 0x00297E); return;  /* 00297E rts */
	hle_jump(ctx, 0x002980); return;  /* 002980 jsr pc@(0x32ac) */
L_002984:
	RAM16(0xFFFFA236) = 0; cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	hle_insn(ctx, 20); /* 002984 clr.w 0xffffa236 */
	ctx->a[0] = 0x0002585A;
	hle_insn(ctx, 12); /* 00298A lea 0x2585a,a0 */
	{ uint32_t v = 0x595; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 002990 move.w #1429,d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFF); RAM16(0xFFFF9E28) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 002994 move.w d1,0xffff9e28 */
	hle_jump(ctx, 0x00299A); return;  /* 00299A jsr 0x1320a */
L_0029A0:
	ctx->a[0] = 0x000433E2;
	hle_insn(ctx, 12); /* 0029A0 lea 0x433e2,a0 */
	{ uint32_t v = 0x1; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 0029A6 move.w #1,d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFF); RAM16(0xFFFF9E38) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 0029AA move.w d1,0xffff9e38 */
	hle_jump(ctx, 0x0029B0); return;  /* 0029B0 jsr 0x1320a */
L_0029B6:
	{ uint32_t v = 0x183; RAM16(0xFFFF9E46) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 0029B6 move.w #387,0xffff9e46 */
	{ uint32_t v = 0x245; RAM16(0xFFFF9E1C) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 0029BE move.w #581,0xffff9e1c */
	{ uint32_t v = 0x2F5; RAM16(0xFFFF9E56) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 0029C6 move.w #757,0xffff9e56 */
	{ uint32_t v = 0x3D6; RAM16(0xFFFF9E22) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 0029CE move.w #982,0xffff9e22 */
	{ uint32_t v = 0x4C3; RAM16(0xFFFF9E3A) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 0029D6 move.w #1219,0xffff9e3a */
	{ uint32_t v = 0x4C3; RAM16(0xFFFF9E3C) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 0029DE move.w #1219,0xffff9e3c */
	{ uint32_t v = 0x29F; RAM16(0xFFFF9E4A) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 0029E6 move.w #671,0xffff9e4a */
	{ uint32_t v = 0x529; RAM16(0xFFFF9E24) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 0029EE move.w #1321,0xffff9e24 */
	{ uint32_t v = 0x549; RAM16(0xFFFF9E42) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 0029F6 move.w #1353,0xffff9e42 */
	{ uint32_t v = 0x245; RAM16(0xFFFF9E1A) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 0029FE move.w #581,0xffff9e1a */
	{ uint32_t v = 0x361; RAM16(0xFFFF9E52) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 002A06 move.w #865,0xffff9e52 */
	{ uint32_t v = 0x361; RAM16(0xFFFF9E1E) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 002A0E move.w #865,0xffff9e1e */
	{ uint32_t v = 0x3E6; RAM16(0xFFFF9E20) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 002A16 move.w #998,0xffff9e20 */
	{ uint32_t v = 0x2F5; RAM16(0xFFFF9E34) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 002A1E move.w #757,0xffff9e34 */
	{ uint32_t v = 0x369; RAM16(0xFFFF9E4E) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 002A26 move.w #873,0xffff9e4e */
	{ uint32_t v = 0x183; RAM16(0xFFFF9E36) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 002A2E move.w #387,0xffff9e36 */
	{ uint32_t v = 0x401; RAM16(0xFFFF9E30) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 002A36 move.w #1025,0xffff9e30 */
	{ uint32_t v = 0x183; RAM16(0xFFFF9E44) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 002A3E move.w #387,0xffff9e44 */
	{ uint32_t v = 0x183; RAM16(0xFFFF9E48) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 002A46 move.w #387,0xffff9e48 */
	hle_jump(ctx, 0x002A4E); return;  /* 002A4E jsr 0x11d12 */
L_002A54:
	ctx->a[1] = 0x00013448;
	hle_insn(ctx, 12); /* 002A54 lea 0x13448,a1 */
	{ uint32_t v = 0x5C2; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 002A5A move.w #1474,d1 */
	hle_jump(ctx, 0x002A5E); return;  /* 002A5E jsr 0x13394 */
L_002A64:
	ctx->a[1] = 0x0001344F;
	hle_insn(ctx, 12); /* 002A64 lea 0x1344f,a1 */
	{ uint32_t v = 0x6ED; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 002A6A move.w #1773,d1 */
	hle_jump(ctx, 0x002A6E); return;  /* 002A6E jsr 0x13394 */
L_002A74:
	ctx->a[1] = 0x00013452;
	hle_insn(ctx, 12); /* 002A74 lea 0x13452,a1 */
	{ uint32_t v = 0x5FC; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 002A7A move.w #1532,d1 */
	hle_jump(ctx, 0x002A7E); return;  /* 002A7E jsr 0x13394 */
L_002A84:
	{ uint32_t v = 0xC140; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 002A84 move.w #-16064,d1 */
	ctx->a[0] = 0x00046C44;
	hle_insn(ctx, 12); /* 002A88 lea 0x46c44,a0 */
	ctx->d[2] = 0x00000020; cc_logic32(ctx, ctx->d[2]);
	hle_insn(ctx, 4); /* 002A8E moveq #32,d2 */
	ctx->d[3] = 0x00000018; cc_logic32(ctx, ctx->d[3]);
	hle_insn(ctx, 4); /* 002A90 moveq #24,d3 */
	{ uint32_t v = 0x0; ctx->d[6] = v; cc_logic32(ctx, v); }
	hle_insn(ctx, 12); /* 002A92 move.l #0,d6 */
	{ uint32_t v = RAM16(0xFFFF9E38); set_w(&ctx->d[7], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 002A98 move.w 0xffff9e38,d7 */
	hle_jump(ctx, 0x002A9E); return;  /* 002A9E jsr 0x12234 */
L_002AA4:
	{ uint32_t v = 0xC100; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 002AA4 move.w #-16128,d1 */
	ctx->a[0] = 0x00046C44;
	hle_insn(ctx, 12); /* 002AA8 lea 0x46c44,a0 */
	ctx->d[2] = 0x00000020; cc_logic32(ctx, ctx->d[2]);
	hle_insn(ctx, 4); /* 002AAE moveq #32,d2 */
	ctx->d[3] = 0x00000018; cc_logic32(ctx, ctx->d[3]);
	hle_insn(ctx, 4); /* 002AB0 moveq #24,d3 */
	{ uint32_t v = 0x0; ctx->d[6] = v; cc_logic32(ctx, v); }
	hle_insn(ctx, 12); /* 002AB2 move.l #0,d6 */
	{ uint32_t v = RAM16(0xFFFF9E38); set_w(&ctx->d[7], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 002AB8 move.w 0xffff9e38,d7 */
	hle_jump(ctx, 0x002ABE); return;  /* 002ABE jsr 0x12234 */
	hle_jump(ctx, 0x002AC4); return;  /* 002AC4 jsr 0xbe42 */
	hle_jump(ctx, 0x002ACA); return;  /* 002ACA jsr 0x11d28 */
	hle_jump(ctx, 0x002AD0); return;  /* 002AD0 jsr 0x11d32 */
L_002AD6:
	ctx->a[0] = 0x000084BA;
	hle_insn(ctx, 8); /* 002AD6 lea pc@(0x84ba),a0 */
	ctx->a[1] = 0xFFFFA134;
	hle_insn(ctx, 12); /* 002ADA lea 0xffffa134,a1 */
	hle_jump(ctx, 0x002AE0); return;  /* 002AE0 jsr pc@(0x8a7c) */
L_002AE4:
	ctx->a[0] = 0x0000847A;
	hle_insn(ctx, 8); /* 002AE4 lea pc@(0x847a),a0 */
	hle_jump(ctx, 0x002AE8); return;  /* 002AE8 jsr pc@(0x8a7c) */
L_002AEC:
	ctx->a[0] = 0x000083FA;
	hle_insn(ctx, 8); /* 002AEC lea pc@(0x83fa),a0 */
	hle_jump(ctx, 0x002AF0); return;  /* 002AF0 jsr pc@(0x8a7c) */
L_002AF4:
	ctx->a[0] = 0x0000843A;
	hle_insn(ctx, 8); /* 002AF4 lea pc@(0x843a),a0 */
	hle_jump(ctx, 0x002AF8); return;  /* 002AF8 jsr pc@(0x8a7c) */
L_002AFC:
	{ uint32_t v = 0x2; set_w(&ctx->d[0], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 002AFC move.w #2,d0 */
	hle_jump(ctx, 0x002B00); return;  /* 002B00 jsr 0xfbb6c */
	hle_jump(ctx, 0x002B06); return;  /* 002B06 rts */
L_0074D8:
	{ uint32_t v = RAM16(0xFFFF80CC); set_w(&ctx->d[0], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 0074D8 move.w 0xffff80cc,d0 */
	alu(ctx, OP_SUB, RAM8(0xFFFF9DF5), 0x7, 8, 0);
	hle_insn(ctx, 20); /* 0074DE cmpi.b #7,0xffff9df5 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0074F2; }  /* 0074E6 bne.s 0x74f2 */
	hle_insn(ctx, 8);
	alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFF), 0x8, 16, 0);
	hle_insn(ctx, 8); /* 0074E8 cmpi.w #8,d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0074F2; }  /* 0074EC bne.s 0x74f2 */
	hle_insn(ctx, 8);
	{ uint32_t v = 0x0; set_w(&ctx->d[0], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 0074EE move.w #0,d0 */
L_0074F2:
	{ uint32_t r = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFF), 1, 16, 1); set_w(&ctx->d[0], r); }
	hle_insn(ctx, 4); /* 0074F2 addq.w #1,d0 */
	alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFF), 0x9, 16, 0);
	hle_insn(ctx, 8); /* 0074F4 cmpi.w #9,d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0074FE; }  /* 0074F8 bne.s 0x74fe */
	hle_insn(ctx, 8);
	{ uint32_t v = 0x8; set_w(&ctx->d[0], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 0074FA move.w #8,d0 */
L_0074FE:
	{ uint32_t v = (ctx->d[0] & 0xFFFF); RAM16(0xFFFF80CC) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 0074FE move.w d0,0xffff80cc */
	alu(ctx, OP_SUB, RAM16(0xFFFF80CC), 0x6, 16, 0);
	hle_insn(ctx, 20); /* 007504 cmpi.w #6,0xffff80cc */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_007548; }  /* 00750C beq.s 0x7548 */
	hle_insn(ctx, 8);
	hle_jump(ctx, 0x00750E); return;  /* 00750E jsr pc@(0x8b1a) */
L_007512:
	RAM16(0xFFFF9E06) = 0; cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	hle_insn(ctx, 20); /* 007512 clr.w 0xffff9e06 */
	RAM16(0xFFFF9E08) = 0; cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	hle_insn(ctx, 20); /* 007518 clr.w 0xffff9e08 */
	RAM16(0xFFFF9E0A) = 0; cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	hle_insn(ctx, 20); /* 00751E clr.w 0xffff9e0a */
	RAM16(0xFFFF9E0C) = 0; cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	hle_insn(ctx, 20); /* 007524 clr.w 0xffff9e0c */
	RAM16(0xFFFF9E0E) = 0; cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	hle_insn(ctx, 20); /* 00752A clr.w 0xffff9e0e */
	RAM16(0xFFFF9E14) = 0; cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	hle_insn(ctx, 20); /* 007530 clr.w 0xffff9e14 */
	RAM16(0xFFFF9E18) = 0; cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	hle_insn(ctx, 20); /* 007536 clr.w 0xffff9e18 */
	RAM16(0xFFFF9E10) = 0; cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	hle_insn(ctx, 20); /* 00753C clr.w 0xffff9e10 */
	RAM16(0xFFFF9E12) = 0; cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	hle_insn(ctx, 20); /* 007542 clr.w 0xffff9e12 */
L_007548:
	{ uint32_t v = 0x0; RAM8(0xFFFFA235) = v; cc_logic8(ctx, v); }
	hle_insn(ctx, 20); /* 007548 move.b #0,0xffffa235 */
	hle_jump(ctx, 0x007550); return;  /* 007550 jsr pc@(0x1d3a) */
	hle_jump(ctx, 0x007554); return;  /* 007554 bsr.w 0x8b3c */
L_007558:
	{ uint32_t v = 0x48; RAM16(0xFFFF9F6A) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 007558 move.w #72,0xffff9f6a */
	{ uint32_t v = 0xC0; RAM8(0xFFFF9F6C) = v; cc_logic8(ctx, v); }
	hle_insn(ctx, 20); /* 007560 move.b #-64,0xffff9f6c */
	alu(ctx, OP_SUB, RAM8(0xFFFF9DF5), 0x3, 8, 0);
	hle_insn(ctx, 20); /* 007568 cmpi.b #3,0xffff9df5 */
	if (COND_LT) { hle_insn(ctx, 10); goto L_0075C2; }  /* 007570 blt.s 0x75c2 */
	hle_insn(ctx, 8);
	{ uint32_t v = 0x4E; RAM16(0xFFFF9F6A) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 007572 move.w #78,0xffff9f6a */
	{ uint32_t v = 0xA0; RAM8(0xFFFF9F6C) = v; cc_logic8(ctx, v); }
	hle_insn(ctx, 20); /* 00757A move.b #-96,0xffff9f6c */
	ctx->a[4] = 0xFFFF8050;
	hle_insn(ctx, 12); /* 007582 lea 0xffff8050,a4 */
	cc_logic8(ctx, RAM8(0xFFFFA0A7));
	hle_insn(ctx, 16); /* 007588 tst.b 0xffffa0a7 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_007596; }  /* 00758E beq.s 0x7596 */
	hle_insn(ctx, 8);
	ctx->a[4] = 0xFFFF8000;
	hle_insn(ctx, 12); /* 007590 lea 0xffff8000,a4 */
L_007596:
	{ uint32_t v = 0xFFFF; RAM16(ctx->a[4] + 36) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 007596 move.w #-1,a4@(36) */
	ctx->d[0] = 0x00000004; cc_logic32(ctx, ctx->d[0]);
	hle_insn(ctx, 4); /* 00759C moveq #4,d0 */
	hle_jump(ctx, 0x00759E); return;  /* 00759E jsr 0x8fa8 */
L_0075A4:
	alu(ctx, OP_SUB, RAM8(0xFFFF9DF5), 0x7, 8, 0);
	hle_insn(ctx, 20); /* 0075A4 cmpi.b #7,0xffff9df5 */
	if (COND_LT) { hle_insn(ctx, 10); goto L_0075C2; }  /* 0075AC blt.s 0x75c2 */
	hle_insn(ctx, 8);
	ctx->a[4] = 0xFFFF8000;
	hle_insn(ctx, 12); /* 0075AE lea 0xffff8000,a4 */
	{ uint32_t v = 0xFFFF; RAM16(ctx->a[4] + 36) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 0075B4 move.w #-1,a4@(36) */
	ctx->d[0] = 0x00000005; cc_logic32(ctx, ctx->d[0]);
	hle_insn(ctx, 4); /* 0075BA moveq #5,d0 */
	hle_jump(ctx, 0x0075BC); return;  /* 0075BC jsr 0x8fa8 */
L_0075C2:
	ctx->a[4] = 0xFFFF8000;
	hle_insn(ctx, 12); /* 0075C2 lea 0xffff8000,a4 */
	{ uint32_t v = 0xFFFF; RAM16(ctx->a[4] + 36) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 0075C8 move.w #-1,a4@(36) */
	ctx->a[4] = 0xFFFF8050;
	hle_insn(ctx, 12); /* 0075CE lea 0xffff8050,a4 */
	{ uint32_t v = 0xFFFF; RAM16(ctx->a[4] + 36) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 16); /* 0075D4 move.w #-1,a4@(36) */
	hle_jump(ctx, 0x0075DA); return;  /* 0075DA jsr 0x131f2 */
	hle_jump(ctx, 0x0075E0); return;  /* 0075E0 jsr pc@(0x7c9a) */
	hle_jump(ctx, 0x0075E4); return;  /* 0075E4 jsr pc@(0x7cd8) */
	hle_jump(ctx, 0x0075E8); return;  /* 0075E8 jsr pc@(0x8c52) */
	hle_jump(ctx, 0x0075EC); return;  /* 0075EC jsr pc@(0x9186) */
	hle_jump(ctx, 0x0075F0); return;  /* 0075F0 jsr pc@(0x9104) */
	hle_jump(ctx, 0x0075F4); return;  /* 0075F4 jsr 0x11d12 */
	hle_jump(ctx, 0x0075FA); return;  /* 0075FA jsr pc@(0x9326) */
	hle_jump(ctx, 0x0075FE); return;  /* 0075FE jsr 0x13376 */
	hle_jump(ctx, 0x007604); return;  /* 007604 jsr pc@(0xb398) */
	hle_jump(ctx, 0x007608); return;  /* 007608 jsr 0x13272 */
	hle_jump(ctx, 0x00760E); return;  /* 00760E jsr 0x11d28 */
	hle_jump(ctx, 0x007614); return;  /* 007614 jsr 0x11d32 */
L_00761A:
	RAM8(0xFFFFA246) = 0xFF;
	hle_insn(ctx, 20); /* 00761A st 0xffffa246 */
	hle_jump(ctx, 0x007620); return;  /* 007620 jsr pc@(0x8a4c) */
L_007624:
	{ uint32_t v = 0xFF; RAM8(0xFFFFA235) = v; cc_logic8(ctx, v); }
	hle_insn(ctx, 20); /* 007624 move.b #-1,0xffffa235 */
	hle_jump(ctx, 0x00762C); return;  /* 00762C jsr 0x13e1a */
L_007632:
	RAM16(0xFFFF8000) = 0; cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	hle_insn(ctx, 20); /* 007632 clr.w 0xffff8000 */
	RAM16(0xFFFF8050) = 0; cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	hle_insn(ctx, 20); /* 007638 clr.w 0xffff8050 */
L_00763E:
	cc_logic8(ctx, RAM8(0xFFFF9EC0));
	hle_insn(ctx, 16); /* 00763E tst.b 0xffff9ec0 */
	RAM8(0xFFFF9EBE) = 0; cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	hle_insn(ctx, 20); /* 007644 clr.b 0xffff9ebe */
	hle_jump(ctx, 0x00764A); return;  /* 00764A jsr pc@(0x86e2) */
L_00764E:
	{ uint32_t v = 0x8000; RAM16(0xFFFFA0EE) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 00764E move.w #-32768,0xffffa0ee */
	{ uint32_t v = 0x8001; RAM16(0xFFFFA0AE) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 007656 move.w #-32767,0xffffa0ae */
	{ hle_jump(ctx, 0x00765E); return; }
L_0078D0:
	RAM16(0xFFFF9EA4) = 0; cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	hle_insn(ctx, 20); /* 0078D0 clr.w 0xffff9ea4 */
	alu(ctx, OP_SUB, RAM16(0xFFFF80CC), 0xA, 16, 0);
	hle_insn(ctx, 20); /* 0078D6 cmpi.w #10,0xffff80cc */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0079A6; }  /* 0078DE beq.w 0x79a6 */
	hle_insn(ctx, 12);
	alu(ctx, OP_SUB, RAM8(0xFFFF9DF5), 0x8, 8, 0);
	hle_insn(ctx, 20); /* 0078E2 cmpi.b #8,0xffff9df5 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0078F6; }  /* 0078EA beq.s 0x78f6 */
	hle_insn(ctx, 8);
	alu(ctx, OP_SUB, RAM8(0xFFFF9DF5), 0x7, 8, 0);
	hle_insn(ctx, 20); /* 0078EC cmpi.b #7,0xffff9df5 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_007924; }  /* 0078F4 bne.s 0x7924 */
	hle_insn(ctx, 8);
L_0078F6:
	hle_jump(ctx, 0x0078F6); return;  /* 0078F6 jsr 0x11d12 */
	hle_jump(ctx, 0x0078FC); return;  /* 0078FC jsr 0x13376 */
	hle_jump(ctx, 0x007902); return;  /* 007902 jsr 0x13272 */
	hle_jump(ctx, 0x007908); return;  /* 007908 jsr 0x11d28 */
	hle_jump(ctx, 0x00790E); return;  /* 00790E jsr pc@(0x86e2) */
	hle_jump(ctx, 0x007912); return;  /* 007912 jsr 0x11d32 */
L_007918:
	ctx->d[0] = 0x00000000; cc_logic32(ctx, ctx->d[0]);
	hle_insn(ctx, 4); /* 007918 moveq #0,d0 */
	hle_jump(ctx, 0x00791A); return;  /* 00791A jsr 0xfbb6c */
L_007920:
	hle_insn(ctx, 10); /* 007920 bra.w 0x79a6 */
	goto L_0079A6;
L_007924:
	ctx->a[4] = 0xFFFF8000;
	hle_insn(ctx, 12); /* 007924 lea 0xffff8000,a4 */
	hle_jump(ctx, 0x00792A); return;  /* 00792A bsr.w 0x8c68 */
L_00792E:
	ctx->a[4] = 0xFFFF8050;
	hle_insn(ctx, 12); /* 00792E lea 0xffff8050,a4 */
	hle_jump(ctx, 0x007934); return;  /* 007934 bsr.w 0x8c68 */
L_007938:
	{ uint32_t v = 0x39; set_w(&ctx->d[0], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 007938 move.w #57,d0 */
	hle_jump(ctx, 0x00793C); return;  /* 00793C jsr 0xfbb6c */
L_007942:
	{ uint32_t v = 0x96; set_w(&ctx->d[1], v); cc_logic16(ctx, v); }
	hle_insn(ctx, 8); /* 007942 move.w #150,d1 */
	hle_jump(ctx, 0x007946); return;  /* 007946 jsr pc@(0x86d8) */
L_00794A:
	cc_logic8(ctx, RAM8(0xFFFF9F6D));
	hle_insn(ctx, 16); /* 00794A tst.b 0xffff9f6d */
	if (COND_NE) { hle_insn(ctx, 10); goto L_00795A; }  /* 007950 bne.s 0x795a */
	hle_insn(ctx, 8);
	hle_jump(ctx, 0x007952); return;  /* 007952 jsr 0x15a2 */
L_007958:
	hle_insn(ctx, 10); /* 007958 bra.s 0x79a6 */
	goto L_0079A6;
L_00795A:
	alu(ctx, OP_SUB, RAM16(0xFFFF80CC), 0x5, 16, 0);
	hle_insn(ctx, 20); /* 00795A cmpi.w #5,0xffff80cc */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0079A2; }  /* 007962 beq.s 0x79a2 */
	hle_insn(ctx, 8);
	alu(ctx, OP_SUB, RAM16(0xFFFF80CC), 0x7, 16, 0);
	hle_insn(ctx, 20); /* 007964 cmpi.w #7,0xffff80cc */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0079A2; }  /* 00796C beq.s 0x79a2 */
	hle_insn(ctx, 8);
	alu(ctx, OP_SUB, RAM16(0xFFFF80CC), 0x8, 16, 0);
	hle_insn(ctx, 20); /* 00796E cmpi.w #8,0xffff80cc */
	if (COND_NE) { hle_insn(ctx, 10); goto L_007994; }  /* 007976 bne.s 0x7994 */
	hle_insn(ctx, 8);
	alu(ctx, OP_SUB, RAM8(0xFFFFA234), 0x1, 8, 0);
	hle_insn(ctx, 20); /* 007978 cmpi.b #1,0xffffa234 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_007988; }  /* 007980 bne.s 0x7988 */
	hle_insn(ctx, 8);
	hle_jump(ctx, 0x007982); return;  /* 007982 jsr 0x15ec */
L_007988:
	alu(ctx, OP_SUB, RAM8(0xFFFFA234), 0x2, 8, 0);
	hle_insn(ctx, 20); /* 007988 cmpi.b #2,0xffffa234 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0079A2; }  /* 007990 beq.s 0x79a2 */
	hle_insn(ctx, 8);
	hle_insn(ctx, 10); /* 007992 bra.s 0x79a6 */
	goto L_0079A6;
L_007994:
	ctx->d[0] = 0x0000003A; cc_logic32(ctx, ctx->d[0]);
	hle_insn(ctx, 4); /* 007994 moveq #58,d0 */
	hle_jump(ctx, 0x007996); return;  /* 007996 jsr 0xfbb6c */
	hle_jump(ctx, 0x00799C); return;  /* 00799C jsr 0x3360 */
L_0079A2:
	hle_insn(ctx, 10); /* 0079A2 bra.w 0x74d8 */
	goto L_0074D8;
L_0079A6:
	alu(ctx, OP_SUB, RAM8(0xFFFF9DF5), 0x7, 8, 0);
	hle_insn(ctx, 20); /* 0079A6 cmpi.b #7,0xffff9df5 */
	if (COND_GE) { hle_insn(ctx, 10); goto L_0079F0; }  /* 0079AE bge.s 0x79f0 */
	hle_insn(ctx, 8);
	alu(ctx, OP_SUB, RAM16(0xFFFF80CC), 0xA, 16, 0);
	hle_insn(ctx, 20); /* 0079B0 cmpi.w #10,0xffff80cc */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0079F2; }  /* 0079B8 bne.s 0x79f2 */
	hle_insn(ctx, 8);
	{ uint32_t v = RAM32(0xFFFF8032); RAM32_W(0xFFFFFFEE, v); cc_logic32(ctx, v); }
	hle_insn(ctx, 36); /* 0079BA move.l 0xffff8032,0xffffffee */
	{ uint32_t v = RAM32(0xFFFF9EA8); RAM32_W(0xFFFFFFF2, v); cc_logic32(ctx, v); }
	hle_insn(ctx, 36); /* 0079C4 move.l 0xffff9ea8,0xfffffff2 */
	hle_jump(ctx, 0x0079CE); return;  /* 0079CE bsr.w 0x3efe */
L_0079D2:
	RAM16(0xFFFF80CC) = 0; cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	hle_insn(ctx, 20); /* 0079D2 clr.w 0xffff80cc */
	{ uint32_t v = RAM32(0xFFFF8082); RAM32_W(0xFFFFFFEE, v); cc_logic32(ctx, v); }
	hle_insn(ctx, 36); /* 0079D8 move.l 0xffff8082,0xffffffee */
	{ uint32_t v = RAM32(0xFFFF9EB0); RAM32_W(0xFFFFFFF2, v); cc_logic32(ctx, v); }
	hle_insn(ctx, 36); /* 0079E2 move.l 0xffff9eb0,0xfffffff2 */
	hle_jump(ctx, 0x0079EC); return;  /* 0079EC bsr.w 0x3efe */
L_0079F0:
	hle_jump(ctx, 0x0079F0); return;  /* 0079F0 rts */
L_0079F2:
	{ uint32_t v = RAM32(0xFFFF9EB0); ctx->d[0] = v; cc_logic32(ctx, v); }
	hle_insn(ctx, 20); /* 0079F2 move.l 0xffff9eb0,d0 */
	alu(ctx, OP_SUB, ctx->d[0], RAM32(0xFFFFFFEA), 32, 0);
	hle_insn(ctx, 22); /* 0079F8 cmp.l 0xffffffea,d0 */
	if (COND_GT) { hle_insn(ctx, 10); goto L_007A10; }  /* 0079FE bgt.s 0x7a10 */
	hle_insn(ctx, 8);
	{ uint32_t v = RAM32(0xFFFF9EA8); ctx->d[0] = v; cc_logic32(ctx, v); }
	hle_insn(ctx, 20); /* 007A00 move.l 0xffff9ea8,d0 */
	alu(ctx, OP_SUB, ctx->d[0], RAM32(0xFFFFFFEA), 32, 0);
	hle_insn(ctx, 22); /* 007A06 cmp.l 0xffffffea,d0 */
	if (COND_GT) { hle_insn(ctx, 10); goto L_007A10; }  /* 007A0C bgt.s 0x7a10 */
	hle_insn(ctx, 8);
	hle_jump(ctx, 0x007A0E); return;  /* 007A0E rts */
L_007A10:
	ctx->d[3] = 0x00000000; cc_logic32(ctx, ctx->d[3]);
	hle_insn(ctx, 4); /* 007A10 moveq #0,d3 */
	cc_logic32(ctx, RAM32(0xFFFF9EB0));
	hle_insn(ctx, 20); /* 007A12 tst.l 0xffff9eb0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_007A1C; }  /* 007A18 beq.s 0x7a1c */
	hle_insn(ctx, 8);
	{ uint32_t r = alu(ctx, OP_ADD, (ctx->d[3] & 0xFFFF), 1, 16, 1); set_w(&ctx->d[3], r); }
	hle_insn(ctx, 4); /* 007A1A addq.w #1,d3 */
L_007A1C:
	cc_logic32(ctx, RAM32(0xFFFF9EA8));
	hle_insn(ctx, 20); /* 007A1C tst.l 0xffff9ea8 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_007A26; }  /* 007A22 beq.s 0x7a26 */
	hle_insn(ctx, 8);
	{ uint32_t r = alu(ctx, OP_ADD, (ctx->d[3] & 0xFFFF), 1, 16, 1); set_w(&ctx->d[3], r); }
	hle_insn(ctx, 4); /* 007A24 addq.w #1,d3 */
L_007A26:
	alu(ctx, OP_SUB, (ctx->d[3] & 0xFFFF), 0x2, 16, 0);
	hle_insn(ctx, 8); /* 007A26 cmpi.w #2,d3 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_007A3A; }  /* 007A2A bne.s 0x7a3a */
	hle_insn(ctx, 8);
	{ uint32_t v = RAM8(0xFFFF9DF5) & 0x3; RAM8(0xFFFF9DF5) = v; cc_logic8(ctx, v); }
	hle_insn(ctx, 24); /* 007A2C andi.b #3,0xffff9df5 */
	RAM8(0xFFFFA0A7) = 0; cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	hle_insn(ctx, 20); /* 007A34 clr.b 0xffffa0a7 */
L_007A3A:
	{ uint32_t v = 0xA; RAM16(0xFFFF80CC) = v; cc_logic16(ctx, v); }
	hle_insn(ctx, 20); /* 007A3A move.w #10,0xffff80cc */
	hle_jump(ctx, 0x007A42); return;  /* 007A42 jsr pc@(0x9326) */
L_007A46:
	{ uint32_t v = 0x0; RAM8(0xFFFFA235) = v; cc_logic8(ctx, v); }
	hle_insn(ctx, 20); /* 007A46 move.b #0,0xffffa235 */
	hle_jump(ctx, 0x007A4E); return;  /* 007A4E bsr.w 0x8b3c */
	hle_jump(ctx, 0x007A52); return;  /* 007A52 jsr pc@(0x1c4a) */
L_007A56:
	{ uint32_t v = 0xFF; RAM8(0xFFFFA235) = v; cc_logic8(ctx, v); }
	hle_insn(ctx, 20); /* 007A56 move.b #-1,0xffffa235 */
	RAM16(0xFFFF8000) = 0; cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	hle_insn(ctx, 20); /* 007A5E clr.w 0xffff8000 */
	RAM16(0xFFFF8050) = 0; cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z);
	hle_insn(ctx, 20); /* 007A64 clr.w 0xffff8050 */
	hle_insn(ctx, 10); /* 007A6A bra.w 0x763e */
	goto L_00763E;
}

HLE_RANGE_PURE(0x001D3A, LevelLoadDispatch, 200, LF_EXITS) { lf_run(ctx, 0x001D3A); }
HLE_RANGE_PURE(0x001D4C, LevelFlow_001D4C, 200, LF_EXITS) { lf_run(ctx, 0x001D4C); }
HLE_RANGE_PURE(0x001D5A, LevelFlow_001D5A, 200, LF_EXITS) { lf_run(ctx, 0x001D5A); }
HLE_RANGE_PURE(0x001D68, LevelFlow_001D68, 200, LF_EXITS) { lf_run(ctx, 0x001D68); }
HLE_RANGE_PURE(0x001D76, LevelFlow_001D76, 200, LF_EXITS) { lf_run(ctx, 0x001D76); }
HLE_RANGE_PURE(0x001D84, LevelFlow_001D84, 200, LF_EXITS) { lf_run(ctx, 0x001D84); }
HLE_RANGE_PURE(0x001D92, LevelFlow_001D92, 200, LF_EXITS) { lf_run(ctx, 0x001D92); }
HLE_RANGE_PURE(0x001DA0, LevelFlow_001DA0, 200, LF_EXITS) { lf_run(ctx, 0x001DA0); }
HLE_RANGE_PURE(0x001DAE, LevelFlow_001DAE, 200, LF_EXITS) { lf_run(ctx, 0x001DAE); }
HLE_RANGE_PURE(0x001DB8, LevelFlow_001DB8, 200, LF_EXITS) { lf_run(ctx, 0x001DB8); }
HLE_RANGE_PURE(0x001DE8, LevelFlow_001DE8, 200, LF_EXITS) { lf_run(ctx, 0x001DE8); }
HLE_RANGE_PURE(0x001DFE, LevelFlow_001DFE, 200, LF_EXITS) { lf_run(ctx, 0x001DFE); }
HLE_RANGE_PURE(0x001E14, LevelFlow_001E14, 200, LF_EXITS) { lf_run(ctx, 0x001E14); }
HLE_RANGE_PURE(0x001E2A, LevelFlow_001E2A, 200, LF_EXITS) { lf_run(ctx, 0x001E2A); }
HLE_RANGE_PURE(0x001E74, LevelFlow_001E74, 200, LF_EXITS) { lf_run(ctx, 0x001E74); }
HLE_RANGE_PURE(0x001E8A, LevelFlow_001E8A, 200, LF_EXITS) { lf_run(ctx, 0x001E8A); }
HLE_RANGE_PURE(0x001EA0, LevelFlow_001EA0, 200, LF_EXITS) { lf_run(ctx, 0x001EA0); }
HLE_RANGE_PURE(0x001EB6, LevelFlow_001EB6, 200, LF_EXITS) { lf_run(ctx, 0x001EB6); }
HLE_RANGE_PURE(0x001ED2, LevelFlow_001ED2, 200, LF_EXITS) { lf_run(ctx, 0x001ED2); }
HLE_RANGE_PURE(0x001EE2, LevelFlow_001EE2, 200, LF_EXITS) { lf_run(ctx, 0x001EE2); }
HLE_RANGE_PURE(0x001F02, LevelFlow_001F02, 200, LF_EXITS) { lf_run(ctx, 0x001F02); }
HLE_RANGE_PURE(0x001F34, LevelFlow_001F34, 200, LF_EXITS) { lf_run(ctx, 0x001F34); }
HLE_RANGE_PURE(0x001F54, LevelFlow_001F54, 200, LF_EXITS) { lf_run(ctx, 0x001F54); }
HLE_RANGE_PURE(0x001F80, LevelFlow_001F80, 200, LF_EXITS) { lf_run(ctx, 0x001F80); }
HLE_RANGE_PURE(0x001F8E, LevelFlow_001F8E, 200, LF_EXITS) { lf_run(ctx, 0x001F8E); }
HLE_RANGE_PURE(0x001F96, LevelFlow_001F96, 200, LF_EXITS) { lf_run(ctx, 0x001F96); }
HLE_RANGE_PURE(0x001F9E, LevelFlow_001F9E, 200, LF_EXITS) { lf_run(ctx, 0x001F9E); }
HLE_RANGE_PURE(0x001FA6, LevelFlow_001FA6, 200, LF_EXITS) { lf_run(ctx, 0x001FA6); }
HLE_RANGE_PURE(0x001FB6, LevelFlow_001FB6, 200, LF_EXITS) { lf_run(ctx, 0x001FB6); }
HLE_RANGE_PURE(0x001FDA, LevelFlow_001FDA, 200, LF_EXITS) { lf_run(ctx, 0x001FDA); }
HLE_RANGE_PURE(0x001FF0, LevelFlow_001FF0, 200, LF_EXITS) { lf_run(ctx, 0x001FF0); }
HLE_RANGE_PURE(0x002006, LevelFlow_002006, 200, LF_EXITS) { lf_run(ctx, 0x002006); }
HLE_RANGE_PURE(0x00201C, LevelFlow_00201C, 200, LF_EXITS) { lf_run(ctx, 0x00201C); }
HLE_RANGE_PURE(0x00203A, LevelFlow_00203A, 200, LF_EXITS) { lf_run(ctx, 0x00203A); }
HLE_RANGE_PURE(0x002056, LevelFlow_002056, 200, LF_EXITS) { lf_run(ctx, 0x002056); }
HLE_RANGE_PURE(0x002076, LevelFlow_002076, 200, LF_EXITS) { lf_run(ctx, 0x002076); }
HLE_RANGE_PURE(0x002096, LevelFlow_002096, 200, LF_EXITS) { lf_run(ctx, 0x002096); }
HLE_RANGE_PURE(0x0020B6, LevelFlow_0020B6, 200, LF_EXITS) { lf_run(ctx, 0x0020B6); }
HLE_RANGE_PURE(0x0020D6, LevelFlow_0020D6, 200, LF_EXITS) { lf_run(ctx, 0x0020D6); }
HLE_RANGE_PURE(0x0020F6, LevelFlow_0020F6, 200, LF_EXITS) { lf_run(ctx, 0x0020F6); }
HLE_RANGE_PURE(0x002122, LevelFlow_002122, 200, LF_EXITS) { lf_run(ctx, 0x002122); }
HLE_RANGE_PURE(0x002130, LevelFlow_002130, 200, LF_EXITS) { lf_run(ctx, 0x002130); }
HLE_RANGE_PURE(0x002138, LevelFlow_002138, 200, LF_EXITS) { lf_run(ctx, 0x002138); }
HLE_RANGE_PURE(0x002140, LevelFlow_002140, 200, LF_EXITS) { lf_run(ctx, 0x002140); }
HLE_RANGE_PURE(0x002148, LevelFlow_002148, 200, LF_EXITS) { lf_run(ctx, 0x002148); }
HLE_RANGE_PURE(0x002158, LevelFlow_002158, 200, LF_EXITS) { lf_run(ctx, 0x002158); }
HLE_RANGE_PURE(0x002174, LevelFlow_002174, 200, LF_EXITS) { lf_run(ctx, 0x002174); }
HLE_RANGE_PURE(0x0021AA, LevelFlow_0021AA, 200, LF_EXITS) { lf_run(ctx, 0x0021AA); }
HLE_RANGE_PURE(0x0021C0, LevelFlow_0021C0, 200, LF_EXITS) { lf_run(ctx, 0x0021C0); }
HLE_RANGE_PURE(0x0021DA, LevelFlow_0021DA, 200, LF_EXITS) { lf_run(ctx, 0x0021DA); }
HLE_RANGE_PURE(0x002200, LevelFlow_002200, 200, LF_EXITS) { lf_run(ctx, 0x002200); }
HLE_RANGE_PURE(0x002210, LevelFlow_002210, 200, LF_EXITS) { lf_run(ctx, 0x002210); }
HLE_RANGE_PURE(0x002220, LevelFlow_002220, 200, LF_EXITS) { lf_run(ctx, 0x002220); }
HLE_RANGE_PURE(0x002240, LevelFlow_002240, 200, LF_EXITS) { lf_run(ctx, 0x002240); }
HLE_RANGE_PURE(0x00226C, LevelFlow_00226C, 200, LF_EXITS) { lf_run(ctx, 0x00226C); }
HLE_RANGE_PURE(0x00227A, LevelFlow_00227A, 200, LF_EXITS) { lf_run(ctx, 0x00227A); }
HLE_RANGE_PURE(0x002282, LevelFlow_002282, 200, LF_EXITS) { lf_run(ctx, 0x002282); }
HLE_RANGE_PURE(0x00228A, LevelFlow_00228A, 200, LF_EXITS) { lf_run(ctx, 0x00228A); }
HLE_RANGE_PURE(0x002292, LevelFlow_002292, 200, LF_EXITS) { lf_run(ctx, 0x002292); }
HLE_RANGE_PURE(0x0022A2, LevelFlow_0022A2, 200, LF_EXITS) { lf_run(ctx, 0x0022A2); }
HLE_RANGE_PURE(0x0022AC, LevelFlow_0022AC, 200, LF_EXITS) { lf_run(ctx, 0x0022AC); }
HLE_RANGE_PURE(0x0022B6, LevelFlow_0022B6, 200, LF_EXITS) { lf_run(ctx, 0x0022B6); }
HLE_RANGE_PURE(0x0022E0, LevelFlow_0022E0, 200, LF_EXITS) { lf_run(ctx, 0x0022E0); }
HLE_RANGE_PURE(0x0022F6, LevelFlow_0022F6, 200, LF_EXITS) { lf_run(ctx, 0x0022F6); }
HLE_RANGE_PURE(0x00232C, LevelFlow_00232C, 200, LF_EXITS) { lf_run(ctx, 0x00232C); }
HLE_RANGE_PURE(0x002342, LevelFlow_002342, 200, LF_EXITS) { lf_run(ctx, 0x002342); }
HLE_RANGE_PURE(0x002370, LevelFlow_002370, 200, LF_EXITS) { lf_run(ctx, 0x002370); }
HLE_RANGE_PURE(0x002380, LevelFlow_002380, 200, LF_EXITS) { lf_run(ctx, 0x002380); }
HLE_RANGE_PURE(0x002390, LevelFlow_002390, 200, LF_EXITS) { lf_run(ctx, 0x002390); }
HLE_RANGE_PURE(0x0023B0, LevelFlow_0023B0, 200, LF_EXITS) { lf_run(ctx, 0x0023B0); }
HLE_RANGE_PURE(0x0023DC, LevelFlow_0023DC, 200, LF_EXITS) { lf_run(ctx, 0x0023DC); }
HLE_RANGE_PURE(0x0023EA, LevelFlow_0023EA, 200, LF_EXITS) { lf_run(ctx, 0x0023EA); }
HLE_RANGE_PURE(0x0023F2, LevelFlow_0023F2, 200, LF_EXITS) { lf_run(ctx, 0x0023F2); }
HLE_RANGE_PURE(0x0023FA, LevelFlow_0023FA, 200, LF_EXITS) { lf_run(ctx, 0x0023FA); }
HLE_RANGE_PURE(0x002402, LevelFlow_002402, 200, LF_EXITS) { lf_run(ctx, 0x002402); }
HLE_RANGE_PURE(0x002422, LevelFlow_002422, 200, LF_EXITS) { lf_run(ctx, 0x002422); }
HLE_RANGE_PURE(0x002440, LevelFlow_002440, 200, LF_EXITS) { lf_run(ctx, 0x002440); }
HLE_RANGE_PURE(0x002456, LevelFlow_002456, 200, LF_EXITS) { lf_run(ctx, 0x002456); }
HLE_RANGE_PURE(0x00246C, LevelFlow_00246C, 200, LF_EXITS) { lf_run(ctx, 0x00246C); }
HLE_RANGE_PURE(0x0024F2, LevelFlow_0024F2, 200, LF_EXITS) { lf_run(ctx, 0x0024F2); }
HLE_RANGE_PURE(0x002502, LevelFlow_002502, 200, LF_EXITS) { lf_run(ctx, 0x002502); }
HLE_RANGE_PURE(0x002522, LevelFlow_002522, 200, LF_EXITS) { lf_run(ctx, 0x002522); }
HLE_RANGE_PURE(0x002542, LevelFlow_002542, 200, LF_EXITS) { lf_run(ctx, 0x002542); }
HLE_RANGE_PURE(0x002574, LevelFlow_002574, 200, LF_EXITS) { lf_run(ctx, 0x002574); }
HLE_RANGE_PURE(0x002594, LevelFlow_002594, 200, LF_EXITS) { lf_run(ctx, 0x002594); }
HLE_RANGE_PURE(0x0025B4, LevelFlow_0025B4, 200, LF_EXITS) { lf_run(ctx, 0x0025B4); }
HLE_RANGE_PURE(0x0025D4, LevelFlow_0025D4, 200, LF_EXITS) { lf_run(ctx, 0x0025D4); }
HLE_RANGE_PURE(0x002600, LevelFlow_002600, 200, LF_EXITS) { lf_run(ctx, 0x002600); }
HLE_RANGE_PURE(0x00260E, LevelFlow_00260E, 200, LF_EXITS) { lf_run(ctx, 0x00260E); }
HLE_RANGE_PURE(0x002616, LevelFlow_002616, 200, LF_EXITS) { lf_run(ctx, 0x002616); }
HLE_RANGE_PURE(0x00261E, LevelFlow_00261E, 200, LF_EXITS) { lf_run(ctx, 0x00261E); }
HLE_RANGE_PURE(0x002626, LevelFlow_002626, 200, LF_EXITS) { lf_run(ctx, 0x002626); }
HLE_RANGE_PURE(0x002636, LevelFlow_002636, 200, LF_EXITS) { lf_run(ctx, 0x002636); }
HLE_RANGE_PURE(0x002654, LevelFlow_002654, 200, LF_EXITS) { lf_run(ctx, 0x002654); }
HLE_RANGE_PURE(0x00266A, LevelFlow_00266A, 200, LF_EXITS) { lf_run(ctx, 0x00266A); }
HLE_RANGE_PURE(0x002680, LevelFlow_002680, 200, LF_EXITS) { lf_run(ctx, 0x002680); }
HLE_RANGE_PURE(0x002696, LevelFlow_002696, 200, LF_EXITS) { lf_run(ctx, 0x002696); }
HLE_RANGE_PURE(0x0026AC, LevelFlow_0026AC, 200, LF_EXITS) { lf_run(ctx, 0x0026AC); }
HLE_RANGE_PURE(0x0026C2, LevelFlow_0026C2, 200, LF_EXITS) { lf_run(ctx, 0x0026C2); }
HLE_RANGE_PURE(0x002790, LevelFlow_002790, 200, LF_EXITS) { lf_run(ctx, 0x002790); }
HLE_RANGE_PURE(0x0027A0, LevelFlow_0027A0, 200, LF_EXITS) { lf_run(ctx, 0x0027A0); }
HLE_RANGE_PURE(0x0027C0, LevelFlow_0027C0, 200, LF_EXITS) { lf_run(ctx, 0x0027C0); }
HLE_RANGE_PURE(0x0027EC, LevelFlow_0027EC, 200, LF_EXITS) { lf_run(ctx, 0x0027EC); }
HLE_RANGE_PURE(0x0027FA, LevelFlow_0027FA, 200, LF_EXITS) { lf_run(ctx, 0x0027FA); }
HLE_RANGE_PURE(0x002802, LevelFlow_002802, 200, LF_EXITS) { lf_run(ctx, 0x002802); }
HLE_RANGE_PURE(0x00280A, LevelFlow_00280A, 200, LF_EXITS) { lf_run(ctx, 0x00280A); }
HLE_RANGE_PURE(0x002812, LevelFlow_002812, 200, LF_EXITS) { lf_run(ctx, 0x002812); }
HLE_RANGE_PURE(0x002822, LevelFlow_002822, 200, LF_EXITS) { lf_run(ctx, 0x002822); }
HLE_RANGE_PURE(0x002838, LevelFlow_002838, 200, LF_EXITS) { lf_run(ctx, 0x002838); }
HLE_RANGE_PURE(0x00284E, LevelFlow_00284E, 200, LF_EXITS) { lf_run(ctx, 0x00284E); }
HLE_RANGE_PURE(0x002864, LevelFlow_002864, 200, LF_EXITS) { lf_run(ctx, 0x002864); }
HLE_RANGE_PURE(0x00287A, LevelFlow_00287A, 200, LF_EXITS) { lf_run(ctx, 0x00287A); }
HLE_RANGE_PURE(0x002890, LevelFlow_002890, 200, LF_EXITS) { lf_run(ctx, 0x002890); }
HLE_RANGE_PURE(0x0028A6, LevelFlow_0028A6, 200, LF_EXITS) { lf_run(ctx, 0x0028A6); }
HLE_RANGE_PURE(0x0028EC, LevelFlow_0028EC, 200, LF_EXITS) { lf_run(ctx, 0x0028EC); }
HLE_RANGE_PURE(0x0028FC, LevelFlow_0028FC, 200, LF_EXITS) { lf_run(ctx, 0x0028FC); }
HLE_RANGE_PURE(0x00291C, LevelFlow_00291C, 200, LF_EXITS) { lf_run(ctx, 0x00291C); }
HLE_RANGE_PURE(0x00294E, LevelFlow_00294E, 200, LF_EXITS) { lf_run(ctx, 0x00294E); }
HLE_RANGE_PURE(0x00295C, LevelFlow_00295C, 200, LF_EXITS) { lf_run(ctx, 0x00295C); }
HLE_RANGE_PURE(0x002964, LevelFlow_002964, 200, LF_EXITS) { lf_run(ctx, 0x002964); }
HLE_RANGE_PURE(0x00296C, LevelFlow_00296C, 200, LF_EXITS) { lf_run(ctx, 0x00296C); }
HLE_RANGE_PURE(0x002974, LevelFlow_002974, 200, LF_EXITS) { lf_run(ctx, 0x002974); }
HLE_RANGE_PURE(0x002984, LevelFlow_002984, 200, LF_EXITS) { lf_run(ctx, 0x002984); }
HLE_RANGE_PURE(0x0029A0, LevelFlow_0029A0, 200, LF_EXITS) { lf_run(ctx, 0x0029A0); }
HLE_RANGE_PURE(0x0029B6, LevelFlow_0029B6, 200, LF_EXITS) { lf_run(ctx, 0x0029B6); }
HLE_RANGE_PURE(0x002A54, LevelFlow_002A54, 200, LF_EXITS) { lf_run(ctx, 0x002A54); }
HLE_RANGE_PURE(0x002A64, LevelFlow_002A64, 200, LF_EXITS) { lf_run(ctx, 0x002A64); }
HLE_RANGE_PURE(0x002A74, LevelFlow_002A74, 200, LF_EXITS) { lf_run(ctx, 0x002A74); }
HLE_RANGE_PURE(0x002A84, LevelFlow_002A84, 200, LF_EXITS) { lf_run(ctx, 0x002A84); }
HLE_RANGE_PURE(0x002AA4, LevelFlow_002AA4, 200, LF_EXITS) { lf_run(ctx, 0x002AA4); }
HLE_RANGE_PURE(0x002AD6, LevelFlow_002AD6, 200, LF_EXITS) { lf_run(ctx, 0x002AD6); }
HLE_RANGE_PURE(0x002AE4, LevelFlow_002AE4, 200, LF_EXITS) { lf_run(ctx, 0x002AE4); }
HLE_RANGE_PURE(0x002AEC, LevelFlow_002AEC, 200, LF_EXITS) { lf_run(ctx, 0x002AEC); }
HLE_RANGE_PURE(0x002AF4, LevelFlow_002AF4, 200, LF_EXITS) { lf_run(ctx, 0x002AF4); }
HLE_RANGE_PURE(0x002AFC, LevelFlow_002AFC, 200, LF_EXITS) { lf_run(ctx, 0x002AFC); }
HLE_RANGE_PURE(0x0074D8, NextLevel, 200, LF_EXITS) { lf_run(ctx, 0x0074D8); }
HLE_RANGE_PURE(0x007512, LevelFlow_007512, 200, LF_EXITS) { lf_run(ctx, 0x007512); }
HLE_RANGE_PURE(0x007558, LevelFlow_007558, 200, LF_EXITS) { lf_run(ctx, 0x007558); }
HLE_RANGE_PURE(0x0075A4, LevelFlow_0075A4, 200, LF_EXITS) { lf_run(ctx, 0x0075A4); }
HLE_RANGE_PURE(0x0075C2, LevelFlow_0075C2, 200, LF_EXITS) { lf_run(ctx, 0x0075C2); }
HLE_RANGE_PURE(0x00761A, LevelFlow_00761A, 200, LF_EXITS) { lf_run(ctx, 0x00761A); }
HLE_RANGE_PURE(0x007624, LevelFlow_007624, 200, LF_EXITS) { lf_run(ctx, 0x007624); }
HLE_RANGE_PURE(0x007632, LevelFlow_007632, 200, LF_EXITS) { lf_run(ctx, 0x007632); }
HLE_RANGE_PURE(0x00764E, LevelFlow_00764E, 200, LF_EXITS) { lf_run(ctx, 0x00764E); }
HLE_RANGE_PURE(0x0078D0, LevelEnd, 200, LF_EXITS) { lf_run(ctx, 0x0078D0); }
HLE_RANGE_PURE(0x007918, LevelFlow_007918, 200, LF_EXITS) { lf_run(ctx, 0x007918); }
HLE_RANGE_PURE(0x007920, LevelFlow_007920, 200, LF_EXITS) { lf_run(ctx, 0x007920); }
HLE_RANGE_PURE(0x00792E, LevelFlow_00792E, 200, LF_EXITS) { lf_run(ctx, 0x00792E); }
HLE_RANGE_PURE(0x007938, LevelFlow_007938, 200, LF_EXITS) { lf_run(ctx, 0x007938); }
HLE_RANGE_PURE(0x007942, LevelFlow_007942, 200, LF_EXITS) { lf_run(ctx, 0x007942); }
HLE_RANGE_PURE(0x00794A, LevelFlow_00794A, 200, LF_EXITS) { lf_run(ctx, 0x00794A); }
HLE_RANGE_PURE(0x007958, LevelFlow_007958, 200, LF_EXITS) { lf_run(ctx, 0x007958); }
HLE_RANGE_PURE(0x007988, LevelFlow_007988, 200, LF_EXITS) { lf_run(ctx, 0x007988); }
HLE_RANGE_PURE(0x0079A2, LevelFlow_0079A2, 200, LF_EXITS) { lf_run(ctx, 0x0079A2); }
HLE_RANGE_PURE(0x0079D2, LevelFlow_0079D2, 200, LF_EXITS) { lf_run(ctx, 0x0079D2); }
HLE_RANGE_PURE(0x007A46, LevelFlow_007A46, 200, LF_EXITS) { lf_run(ctx, 0x007A46); }
HLE_RANGE_PURE(0x007A56, LevelFlow_007A56, 200, LF_EXITS) { lf_run(ctx, 0x007A56); }
