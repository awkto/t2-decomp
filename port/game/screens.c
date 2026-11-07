/* Front-end screens: boot/intro logo sequence (ColdStart $000900, $00099E, $000CC6, $000D76), the
 * logo "press start" window (WaitStartWindow $000E22), main menu (MainMenuLoop $000F84,
 * MainMenuInput $001164, the menu drawing $00102A..$001162), OptionsInput $0013A2, Ending $0015EC,
 * TitleScreen $001A30 / PressStartLoop $001B72, the hi-score table (HiScoreInitOnce $003EC8,
 * SortHiScores $003EFE) and its screen $003F98, the story text screens (StoryIntro $0042CE,
 * $0041E6/$004212/$00424E), the continue countdown (PlayerDied $008D68, ContinueCountdown $008DFE)
 * and the hi-score initials entry (docs/subsystems/levels.md: level $0A runs the script $020E76,
 * whose `native` $00B200 spawns the 26 letter cells and the name display: AI $01524A, hit tests
 * $006F64/$006F78, on-hit $006F8C).
 *
 * All of these are loops around blocking calls (WaitVBlank, WaitFrames, PalFade, PrintText, the
 * VDP list and tile loaders). They are registered as HLE_RANGE_PURE pieces that cover only the
 * routines' OWN instructions, exactly as levelflow.c does: a piece runs from an entry (a routine's
 * start or the return address of a `jsr`/`bsr`) up to the next `jsr`/`bsr`/`jmp`/`rts` and leaves
 * with hle_jump to that instruction, which the 68k executes, so every callee (registered or not),
 * every wait and all VDP/DMA work stay real, and the callers of WaitVBlank ($0086E2) the host's menu
 * detection (port/src/mouse.c) looks for are still reached by the original `jsr`s at the same
 * addresses. Instructions the C does not model, or that touch hardware ($4320: the story screen
 * writes the VDP control port through a5), are barriers like the jsr's: the 68k executes them.
 *
 * The pieces touch only work RAM (and read ROM data) and registers, so the layer measures each one
 * with a dry run and declines it when a VBlank would fall inside.
 *
 * Generated instruction by instruction from the disassembly (a throw-away generator reading the ROM
 * through objdump and the GPGX opcode cycle table); every charge is the GPGX cost of the opcode word
 * and tools/py/costcheck.py checks them. Registers and flags are mirrored. All the pieces are in one
 * function, sc_run(), entered through a switch on the entry address; branches are gotos. A piece
 * that pushes before its closing jsr declares the stack delta of that exit (HLE_EXIT_SP). */
#include "hle.h"

#include <stdint.h>
#include <string.h>

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
#define COND_VC (!F_V)
#define COND_VS F_V
#define COND_HI (!F_C && !F_Z)
#define COND_LS (F_C || F_Z)

/* work RAM or ROM by address; anything else (a hardware port) would make the piece impure */
static inline int is_ram(uint32_t a) { return (a & 0xFFFFFFu) >= 0xE00000u; }
static inline void impure(void) { __builtin_trap(); }
static inline uint8_t ld8(uint32_t a)
{
	if (is_ram(a)) return RAM8(a);
	if ((a & 0xFFFFFFu) < 0x400000u) return ROM8(a);
	impure();
	return 0;
}
static inline uint16_t ld16(uint32_t a)
{
	if (is_ram(a)) return RAM16(a);
	if ((a & 0xFFFFFFu) < 0x400000u) return ROM16(a);
	impure();
	return 0;
}
static inline uint32_t ld32(uint32_t a) { return ((uint32_t)ld16(a) << 16) | ld16(a + 2); }
static inline void st8(uint32_t a, uint32_t v) { if (!is_ram(a)) impure(); RAM8(a) = (uint8_t)v; }
static inline void st16(uint32_t a, uint32_t v) { if (!is_ram(a)) impure(); RAM16(a) = (uint16_t)v; }
static inline void st32(uint32_t a, uint32_t v) { if (!is_ram(a)) impure(); RAM32_W(a, v); }

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

static void sc_run(hle_ctx *ctx, uint32_t pc)
{
	switch (pc) {
	case 0x000912: goto L_000912;
	case 0x00091A: goto L_00091A;
	case 0x000922: goto L_000922;
	case 0x00093E: goto L_00093E;
	case 0x00094C: goto L_00094C;
	case 0x00095C: goto L_00095C;
	case 0x000974: goto L_000974;
	case 0x000982: goto L_000982;
	case 0x000990: goto L_000990;
	case 0x00099A: goto L_00099A;
	case 0x00099E: goto L_00099E;
	case 0x0009B0: goto L_0009B0;
	case 0x0009BA: goto L_0009BA;
	case 0x0009CA: goto L_0009CA;
	case 0x0009E0: goto L_0009E0;
	case 0x0009F0: goto L_0009F0;
	case 0x0009FA: goto L_0009FA;
	case 0x000A04: goto L_000A04;
	case 0x000A12: goto L_000A12;
	case 0x000A24: goto L_000A24;
	case 0x000A2E: goto L_000A2E;
	case 0x000C78: goto L_000C78;
	case 0x000CB2: goto L_000CB2;
	case 0x000CC6: goto L_000CC6;
	case 0x000CD8: goto L_000CD8;
	case 0x000CE2: goto L_000CE2;
	case 0x000CF8: goto L_000CF8;
	case 0x000D06: goto L_000D06;
	case 0x000D0E: goto L_000D0E;
	case 0x000D16: goto L_000D16;
	case 0x000D1E: goto L_000D1E;
	case 0x000D28: goto L_000D28;
	case 0x000D32: goto L_000D32;
	case 0x000D3C: goto L_000D3C;
	case 0x000D48: goto L_000D48;
	case 0x000D76: goto L_000D76;
	case 0x000D88: goto L_000D88;
	case 0x000D92: goto L_000D92;
	case 0x000DA8: goto L_000DA8;
	case 0x000DBE: goto L_000DBE;
	case 0x000DCC: goto L_000DCC;
	case 0x000DD4: goto L_000DD4;
	case 0x000DDC: goto L_000DDC;
	case 0x000DE4: goto L_000DE4;
	case 0x000DEE: goto L_000DEE;
	case 0x000E04: goto L_000E04;
	case 0x000E0E: goto L_000E0E;
	case 0x000E1C: goto L_000E1C;
	case 0x000E2C: goto L_000E2C;
	case 0x000E66: goto L_000E66;
	case 0x000ED4: goto L_000ED4;
	case 0x000EE6: goto L_000EE6;
	case 0x000EF0: goto L_000EF0;
	case 0x000F06: goto L_000F06;
	case 0x000F1C: goto L_000F1C;
	case 0x000F32: goto L_000F32;
	case 0x000F40: goto L_000F40;
	case 0x000F48: goto L_000F48;
	case 0x000F50: goto L_000F50;
	case 0x000F5C: goto L_000F5C;
	case 0x000F6C: goto L_000F6C;
	case 0x000F7A: goto L_000F7A;
	case 0x000F8C: goto L_000F8C;
	case 0x000FB2: goto L_000FB2;
	case 0x000FCA: goto L_000FCA;
	case 0x000FE8: goto L_000FE8;
	case 0x000FFC: goto L_000FFC;
	case 0x00102E: goto L_00102E;
	case 0x001044: goto L_001044;
	case 0x00104C: goto L_00104C;
	case 0x00105C: goto L_00105C;
	case 0x00106E: goto L_00106E;
	case 0x00107E: goto L_00107E;
	case 0x00108E: goto L_00108E;
	case 0x001096: goto L_001096;
	case 0x00109E: goto L_00109E;
	case 0x0010B8: goto L_0010B8;
	case 0x0010C8: goto L_0010C8;
	case 0x0010DA: goto L_0010DA;
	case 0x0010F8: goto L_0010F8;
	case 0x00111A: goto L_00111A;
	case 0x001136: goto L_001136;
	case 0x00116E: goto L_00116E;
	case 0x001180: goto L_001180;
	case 0x00119C: goto L_00119C;
	case 0x0011BC: goto L_0011BC;
	case 0x0011CE: goto L_0011CE;
	case 0x001348: goto L_001348;
	case 0x00136A: goto L_00136A;
	case 0x001380: goto L_001380;
	case 0x001394: goto L_001394;
	case 0x0013AC: goto L_0013AC;
	case 0x0013BE: goto L_0013BE;
	case 0x0013C6: goto L_0013C6;
	case 0x0013DA: goto L_0013DA;
	case 0x0013E4: goto L_0013E4;
	case 0x0013EC: goto L_0013EC;
	case 0x001400: goto L_001400;
	case 0x00140A: goto L_00140A;
	case 0x001414: goto L_001414;
	case 0x00141E: goto L_00141E;
	case 0x001426: goto L_001426;
	case 0x001432: goto L_001432;
	case 0x00143C: goto L_00143C;
	case 0x00145E: goto L_00145E;
	case 0x001466: goto L_001466;
	case 0x00148C: goto L_00148C;
	case 0x001494: goto L_001494;
	case 0x0014A4: goto L_0014A4;
	case 0x0014B0: goto L_0014B0;
	case 0x0014C8: goto L_0014C8;
	case 0x0014D8: goto L_0014D8;
	case 0x0015F0: goto L_0015F0;
	case 0x001606: goto L_001606;
	case 0x00161A: goto L_00161A;
	case 0x001646: goto L_001646;
	case 0x001668: goto L_001668;
	case 0x00167A: goto L_00167A;
	case 0x00168C: goto L_00168C;
	case 0x001696: goto L_001696;
	case 0x0016A4: goto L_0016A4;
	case 0x0016D0: goto L_0016D0;
	case 0x0016DA: goto L_0016DA;
	case 0x0016F8: goto L_0016F8;
	case 0x00171C: goto L_00171C;
	case 0x001A1E: goto L_001A1E;
	case 0x001A34: goto L_001A34;
	case 0x001A42: goto L_001A42;
	case 0x001A4C: goto L_001A4C;
	case 0x001A5A: goto L_001A5A;
	case 0x001A62: goto L_001A62;
	case 0x001A6A: goto L_001A6A;
	case 0x001A72: goto L_001A72;
	case 0x001A7E: goto L_001A7E;
	case 0x001A9A: goto L_001A9A;
	case 0x001AB0: goto L_001AB0;
	case 0x001AC6: goto L_001AC6;
	case 0x001AD0: goto L_001AD0;
	case 0x001AD6: goto L_001AD6;
	case 0x001ADC: goto L_001ADC;
	case 0x001AE6: goto L_001AE6;
	case 0x001AF0: goto L_001AF0;
	case 0x001B06: goto L_001B06;
	case 0x001B1C: goto L_001B1C;
	case 0x001B54: goto L_001B54;
	case 0x001B66: goto L_001B66;
	case 0x001B72: goto L_001B72;
	case 0x001B9E: goto L_001B9E;
	case 0x001BB0: goto L_001BB0;
	case 0x003EC8: goto L_003EC8;
	case 0x003EFE: goto L_003EFE;
	case 0x003FA0: goto L_003FA0;
	case 0x003FB0: goto L_003FB0;
	case 0x003FC6: goto L_003FC6;
	case 0x003FE2: goto L_003FE2;
	case 0x003FE8: goto L_003FE8;
	case 0x003FF6: goto L_003FF6;
	case 0x003FFE: goto L_003FFE;
	case 0x004008: goto L_004008;
	case 0x004018: goto L_004018;
	case 0x00402E: goto L_00402E;
	case 0x004036: goto L_004036;
	case 0x00403C: goto L_00403C;
	case 0x00404A: goto L_00404A;
	case 0x004068: goto L_004068;
	case 0x0041EA: goto L_0041EA;
	case 0x0041F4: goto L_0041F4;
	case 0x00421A: goto L_00421A;
	case 0x004230: goto L_004230;
	case 0x00423E: goto L_00423E;
	case 0x004246: goto L_004246;
	case 0x004252: goto L_004252;
	case 0x00425E: goto L_00425E;
	case 0x004280: goto L_004280;
	case 0x00429E: goto L_00429E;
	case 0x0042B0: goto L_0042B0;
	case 0x0042D6: goto L_0042D6;
	case 0x0042EC: goto L_0042EC;
	case 0x004302: goto L_004302;
	case 0x004330: goto L_004330;
	case 0x00434E: goto L_00434E;
	case 0x00435C: goto L_00435C;
	case 0x004364: goto L_004364;
	case 0x00436E: goto L_00436E;
	case 0x004392: goto L_004392;
	case 0x0043AC: goto L_0043AC;
	case 0x0043BA: goto L_0043BA;
	case 0x0043E0: goto L_0043E0;
	case 0x0043F2: goto L_0043F2;
	case 0x0043FC: goto L_0043FC;
	case 0x00440E: goto L_00440E;
	case 0x004418: goto L_004418;
	case 0x00442A: goto L_00442A;
	case 0x004452: goto L_004452;
	case 0x004472: goto L_004472;
	case 0x0044D6: goto L_0044D6;
	case 0x0044E8: goto L_0044E8;
	case 0x004500: goto L_004500;
	case 0x004512: goto L_004512;
	case 0x00451C: goto L_00451C;
	case 0x004546: goto L_004546;
	case 0x004558: goto L_004558;
	case 0x00456A: goto L_00456A;
	case 0x00458C: goto L_00458C;
	case 0x0045EA: goto L_0045EA;
	case 0x004650: goto L_004650;
	case 0x004664: goto L_004664;
	case 0x004668: goto L_004668;
	case 0x006F64: goto L_006F64;
	case 0x006F78: goto L_006F78;
	case 0x006F8C: goto L_006F8C;
	case 0x008D68: goto L_008D68;
	case 0x008D96: goto L_008D96;
	case 0x008DAE: goto L_008DAE;
	case 0x008DBC: goto L_008DBC;
	case 0x008DD2: goto L_008DD2;
	case 0x008E02: goto L_008E02;
	case 0x008E34: goto L_008E34;
	case 0x008E3C: goto L_008E3C;
	case 0x008E5A: goto L_008E5A;
	case 0x008E96: goto L_008E96;
	case 0x008EA8: goto L_008EA8;
	case 0x008EC0: goto L_008EC0;
	case 0x008EC8: goto L_008EC8;
	case 0x01524A: goto L_01524A;
	case 0x01525E: goto L_01525E;
	case 0x015272: goto L_015272;
	case 0x015286: goto L_015286;
	case 0x0152A2: goto L_0152A2;
	default: return;
	}
L_000900:
	hle_jump(ctx, 0x000900); return;  /* 000900 jsr 1db36 */
	hle_jump(ctx, 0x000906); return;  /* 000906 jsr %pc@(8162) */
	hle_jump(ctx, 0x00090A); return;  /* 00090A jsr %pc@(818c) */
	hle_jump(ctx, 0x00090E); return;  /* 00090E jsr %pc@(800c) */
L_000912:
	{ ctx->a[1] = 0x0000571Au; } hle_insn(ctx, 8); /* 000912 lea %pc@(571a),%a1 */
	hle_jump(ctx, 0x000916); return;  /* 000916 jsr %pc@(867a) */
L_00091A:
	{ uint32_t v = 0x32u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 00091A movew #50,%d1 */
	hle_jump(ctx, 0x00091E); return;  /* 00091E jsr %pc@(86d8) */
L_000922:
	{ uint32_t v = 0x0u; st8(0xFFFF9DF5u, v); cc_logic8(ctx, v); } hle_insn(ctx, 20); /* 000922 moveb #0,ffff9df5 */
	{ uint32_t v = 0x400u; st16(0xFFFF80CAu, v); cc_logic16(ctx, v); } hle_insn(ctx, 20); /* 00092A movew #1024,ffff80ca */
	hle_jump(ctx, 0x000932); return;  /* 000932 bsrw 99e */
	hle_jump(ctx, 0x000936); return;  /* 000936 bsrw cc6 */
	hle_jump(ctx, 0x00093A); return;  /* 00093A bsrw d76 */
L_00093E:
	{ st8(0xFFFFA234u, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 20); /* 00093E clrb ffffa234 */
	hle_jump(ctx, 0x000944); return;  /* 000944 jsr %pc@(871a) */
	hle_jump(ctx, 0x000948); return;  /* 000948 jsr %pc@(42ce) */
L_00094C:
	{ cc_logic8(ctx, ld8(0xFFFFA234u)); } hle_insn(ctx, 16); /* 00094C tstb ffffa234 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_000958; } hle_insn(ctx, 8);  /* 000952 bnes 958 */
	hle_jump(ctx, 0x000954); return;  /* 000954 jsr %pc@(41e6) */
L_000958:
	hle_jump(ctx, 0x000958); return;  /* 000958 jsr %pc@(1a30) */
L_00095C:
	{ cc_logic8(ctx, (ctx->d[0] & 0xFFu)); } hle_insn(ctx, 4); /* 00095C tstb %d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_000986; } hle_insn(ctx, 8);  /* 00095E bnes 986 */
	{ st8(0xFFFFA234u, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 20); /* 000960 clrb ffffa234 */
	{ uint32_t v = 0x7u; st8(0xFFFF9DF5u, v); cc_logic8(ctx, v); } hle_insn(ctx, 20); /* 000966 moveb #7,ffff9df5 */
	hle_jump(ctx, 0x00096E); return;  /* 00096E jsr 745c */
L_000974:
	{ uint32_t da = 0xFFFF9DF5u; alu(ctx, OP_SUB, ld8(da), 0x8u, 8, 0); } hle_insn(ctx, 20); /* 000974 cmpib #8,ffff9df5 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_000958; } hle_insn(ctx, 8);  /* 00097C beqs 958 */
	hle_jump(ctx, 0x00097E); return;  /* 00097E jsr %pc@(3f98) */
L_000982:
	hle_insn(ctx, 10); goto L_000900;  /* 000982 braw 900 */
L_000986:
	hle_jump(ctx, 0x000986); return;  /* 000986 bsrw ed4 */
	hle_jump(ctx, 0x00098A); return;  /* 00098A jsr 745c */
L_000990:
	{ st8(0xFFFFA234u, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 20); /* 000990 clrb ffffa234 */
	hle_jump(ctx, 0x000996); return;  /* 000996 jsr %pc@(3f98) */
L_00099A:
	hle_insn(ctx, 10); goto L_000900;  /* 00099A braw 900 */
L_00099E:
	{ uint32_t v = 0x0u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 00099E movew #0,%d0 */
	hle_jump(ctx, 0x0009A2); return;  /* 0009A2 jsr %pc@(56fa) */
	hle_jump(ctx, 0x0009A6); return;  /* 0009A6 jsr %pc@(871a) */
	hle_jump(ctx, 0x0009AA); return;  /* 0009AA jsr 121d6 */
L_0009B0:
	{ ctx->a[1] = 0x0000576Au; } hle_insn(ctx, 8); /* 0009B0 lea %pc@(576a),%a1 */
	hle_jump(ctx, 0x0009B4); return;  /* 0009B4 jsr 867a */
L_0009BA:
	{ ctx->a[0] = 0x000268DEu; } hle_insn(ctx, 12); /* 0009BA lea 268de,%a0 */
	{ uint32_t v = 0x6C7u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 0009C0 movew #1735,%d1 */
	hle_jump(ctx, 0x0009C4); return;  /* 0009C4 jsr 1320a */
L_0009CA:
	{ ctx->a[0] = 0x00025DFCu; } hle_insn(ctx, 12); /* 0009CA lea 25dfc,%a0 */
	{ uint32_t v = 0x670u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 0009D0 movew #1648,%d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFFu); st16(0xFFFF9E2Cu, v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 0009D4 movew %d1,ffff9e2c */
	hle_jump(ctx, 0x0009DA); return;  /* 0009DA jsr 1320a */
L_0009E0:
	{ ctx->a[0] = 0x0000829Au; } hle_insn(ctx, 8); /* 0009E0 lea %pc@(829a),%a0 */
	{ ctx->a[1] = 0xFFFFA134u; } hle_insn(ctx, 12); /* 0009E4 lea ffffa134,%a1 */
	hle_jump(ctx, 0x0009EA); return;  /* 0009EA jsr 8a7c */
L_0009F0:
	{ ctx->a[0] = 0x000082FAu; } hle_insn(ctx, 8); /* 0009F0 lea %pc@(82fa),%a0 */
	hle_jump(ctx, 0x0009F4); return;  /* 0009F4 jsr 8a7c */
L_0009FA:
	{ ctx->a[0] = 0x000083FAu; } hle_insn(ctx, 8); /* 0009FA lea %pc@(83fa),%a0 */
	hle_jump(ctx, 0x0009FE); return;  /* 0009FE jsr 8a7c */
L_000A04:
	{ ctx->a[0] = 0x00000A34u; } hle_insn(ctx, 8); /* 000A04 lea %pc@(a34),%a0 */
	{ uint32_t v = 0x4000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 000A08 movew #16384,%d6 */
	hle_jump(ctx, 0x000A0C); return;  /* 000A0C jsr 7dee */
L_000A12:
	{ st16(0xFFFFA0EEu, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 20); /* 000A12 clrw ffffa0ee */
	{ st8(0xFFFFA246u, (1) ? 0xFF : 0); } hle_insn(ctx, 20); /* 000A18 st ffffa246 */
	hle_jump(ctx, 0x000A1E); return;  /* 000A1E jsr 8a4c */
L_000A24:
	{ st16(0xFFFFA0EEu, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 20); /* 000A24 clrw ffffa0ee */
L_000A2A:
	hle_jump(ctx, 0x000A2A); return;  /* 000A2A bsrw e22 */
L_000A2E:
	{ cc_logic8(ctx, (ctx->d[0] & 0xFFu)); } hle_insn(ctx, 4); /* 000A2E tstb %d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_000A2A; } hle_insn(ctx, 8);  /* 000A30 beqs a2a */
	hle_jump(ctx, 0x000A32); return;  /* 000A32 rts */
L_000C78:
	{ uint32_t v = ld16(0xFFFF8050u); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 000C78 movew ffff8050,%d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_000CB8; } hle_insn(ctx, 12);  /* 000C7E beqw cb8 */
	{ alu(ctx, OP_SUB, ctx->d[0] & 0xFFFFu, ld16(0xFFFF8052u), 16, 0); } hle_insn(ctx, 16); /* 000C82 cmpw ffff8052,%d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_000CB8; } hle_insn(ctx, 12);  /* 000C88 beqw cb8 */
	{ uint32_t v = ld16(0xFFFFA0ACu); set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 000C8C movew ffffa0ac,%d2 */
	{ alu(ctx, OP_SUB, ctx->d[0] & 0xFFu, ld8((0x00000CBAu + (int16_t)ctx->d[2])), 8, 0); } hle_insn(ctx, 14); /* 000C92 cmpb %pc@(cba,%d2:w),%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_000CB2; } hle_insn(ctx, 8);  /* 000C96 bnes cb2 */
	{ uint32_t r = alu(ctx, OP_ADD, (ctx->d[2] & 0xFFFFu), 1, 16, 1); set_w(&ctx->d[2], r); } hle_insn(ctx, 4); /* 000C98 addqw #1,%d2 */
	{ uint32_t v = (ctx->d[2] & 0xFFFFu); st16(0xFFFFA0ACu, v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 000C9A movew %d2,ffffa0ac */
	{ alu(ctx, OP_SUB, (ctx->d[2] & 0xFFFFu), 0x8u, 16, 0); } hle_insn(ctx, 8); /* 000CA0 cmpiw #8,%d2 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_000CB8; } hle_insn(ctx, 8);  /* 000CA4 bnes cb8 */
	{ st8(0xFFFFA0A6u, (1) ? 0xFF : 0); } hle_insn(ctx, 20); /* 000CA6 st ffffa0a6 */
	hle_jump(ctx, 0x000CAC); return;  /* 000CAC jsr 13e04 */
L_000CB2:
	{ st16(0xFFFFA0ACu, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 20); /* 000CB2 clrw ffffa0ac */
L_000CB8:
	hle_jump(ctx, 0x000CB8); return;  /* 000CB8 rts */
L_000CC6:
	{ uint32_t v = 0x0u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 000CC6 movew #0,%d0 */
	hle_jump(ctx, 0x000CCA); return;  /* 000CCA jsr %pc@(56fa) */
	hle_jump(ctx, 0x000CCE); return;  /* 000CCE jsr %pc@(871a) */
	hle_jump(ctx, 0x000CD2); return;  /* 000CD2 jsr 121d6 */
L_000CD8:
	{ ctx->a[1] = 0x0000576Au; } hle_insn(ctx, 8); /* 000CD8 lea %pc@(576a),%a1 */
	hle_jump(ctx, 0x000CDC); return;  /* 000CDC jsr 867a */
L_000CE2:
	{ ctx->a[0] = 0x000B4242u; } hle_insn(ctx, 12); /* 000CE2 lea b4242,%a0 */
	{ uint32_t v = 0x1u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 000CE8 movew #1,%d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFFu); st16(0xFFFF9E38u, v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 000CEC movew %d1,ffff9e38 */
	hle_jump(ctx, 0x000CF2); return;  /* 000CF2 jsr 1320a */
L_000CF8:
	{ ctx->a[0] = 0x0000825Au; } hle_insn(ctx, 8); /* 000CF8 lea %pc@(825a),%a0 */
	{ ctx->a[1] = 0xFFFFA134u; } hle_insn(ctx, 12); /* 000CFC lea ffffa134,%a1 */
	hle_jump(ctx, 0x000D02); return;  /* 000D02 jsr %pc@(8a7c) */
L_000D06:
	{ ctx->a[0] = 0x000082FAu; } hle_insn(ctx, 8); /* 000D06 lea %pc@(82fa),%a0 */
	hle_jump(ctx, 0x000D0A); return;  /* 000D0A jsr %pc@(8a7c) */
L_000D0E:
	{ ctx->a[0] = 0x000083FAu; } hle_insn(ctx, 8); /* 000D0E lea %pc@(83fa),%a0 */
	hle_jump(ctx, 0x000D12); return;  /* 000D12 jsr %pc@(8a7c) */
L_000D16:
	{ ctx->a[0] = 0x0000843Au; } hle_insn(ctx, 8); /* 000D16 lea %pc@(843a),%a0 */
	hle_jump(ctx, 0x000D1A); return;  /* 000D1A jsr %pc@(8a7c) */
L_000D1E:
	{ st16(0xFFFFA0EEu, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 20); /* 000D1E clrw ffffa0ee */
	hle_jump(ctx, 0x000D24); return;  /* 000D24 bsrw d42 */
L_000D28:
	{ st8(0xFFFFA246u, (1) ? 0xFF : 0); } hle_insn(ctx, 20); /* 000D28 st ffffa246 */
	hle_jump(ctx, 0x000D2E); return;  /* 000D2E jsr %pc@(8a4c) */
L_000D32:
	{ st16(0xFFFFA0EEu, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 20); /* 000D32 clrw ffffa0ee */
L_000D38:
	hle_jump(ctx, 0x000D38); return;  /* 000D38 bsrw e22 */
L_000D3C:
	{ cc_logic8(ctx, (ctx->d[0] & 0xFFu)); } hle_insn(ctx, 4); /* 000D3C tstb %d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_000D38; } hle_insn(ctx, 8);  /* 000D3E beqs d38 */
	hle_jump(ctx, 0x000D40); return;  /* 000D40 rts */
	hle_jump(ctx, 0x000D42); return;  /* 000D42 jsr 11d12 */
L_000D48:
	{ ctx->a[0] = 0x000B55E4u; } hle_insn(ctx, 12); /* 000D48 lea b55e4,%a0 */
	{ uint32_t v = 0xC318u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 000D4E movew #-15592,%d1 */
	{ ctx->d[2] = 0x10u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4); /* 000D52 moveq #16,%d2 */
	{ ctx->d[3] = 0xFu; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4); /* 000D54 moveq #15,%d3 */
	{ uint32_t v = 0x0u; ctx->d[6] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12); /* 000D56 movel #0,%d6 */
	{ uint32_t v = ld16(0xFFFF9E38u); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 000D5C movew ffff9e38,%d7 */
	hle_jump(ctx, 0x000D62); return;  /* 000D62 jsr 122ca */
	hle_jump(ctx, 0x000D68); return;  /* 000D68 jsr 11d28 */
	hle_jump(ctx, 0x000D6E); return;  /* 000D6E jsr 11d32 */
	hle_jump(ctx, 0x000D74); return;  /* 000D74 rts */
L_000D76:
	{ uint32_t v = 0x0u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 000D76 movew #0,%d0 */
	hle_jump(ctx, 0x000D7A); return;  /* 000D7A jsr %pc@(56fa) */
	hle_jump(ctx, 0x000D7E); return;  /* 000D7E jsr %pc@(871a) */
	hle_jump(ctx, 0x000D82); return;  /* 000D82 jsr 121d6 */
L_000D88:
	{ ctx->a[1] = 0x0000576Au; } hle_insn(ctx, 8); /* 000D88 lea %pc@(576a),%a1 */
	hle_jump(ctx, 0x000D8C); return;  /* 000D8C jsr 867a */
L_000D92:
	{ ctx->a[0] = 0x000B0D20u; } hle_insn(ctx, 12); /* 000D92 lea b0d20,%a0 */
	{ uint32_t v = 0x1u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 000D98 movew #1,%d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFFu); st16(0xFFFF9E38u, v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 000D9C movew %d1,ffff9e38 */
	hle_jump(ctx, 0x000DA2); return;  /* 000DA2 jsr 1320a */
L_000DA8:
	{ ctx->a[0] = 0x00025DFCu; } hle_insn(ctx, 12); /* 000DA8 lea 25dfc,%a0 */
	{ uint32_t v = 0x670u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 000DAE movew #1648,%d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFFu); st16(0xFFFF9E2Cu, v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 000DB2 movew %d1,ffff9e2c */
	hle_jump(ctx, 0x000DB8); return;  /* 000DB8 jsr 1320a */
L_000DBE:
	{ ctx->a[0] = 0x0000829Au; } hle_insn(ctx, 8); /* 000DBE lea %pc@(829a),%a0 */
	{ ctx->a[1] = 0xFFFFA134u; } hle_insn(ctx, 12); /* 000DC2 lea ffffa134,%a1 */
	hle_jump(ctx, 0x000DC8); return;  /* 000DC8 jsr %pc@(8a7c) */
L_000DCC:
	{ ctx->a[0] = 0x000082FAu; } hle_insn(ctx, 8); /* 000DCC lea %pc@(82fa),%a0 */
	hle_jump(ctx, 0x000DD0); return;  /* 000DD0 jsr %pc@(8a7c) */
L_000DD4:
	{ ctx->a[0] = 0x000083FAu; } hle_insn(ctx, 8); /* 000DD4 lea %pc@(83fa),%a0 */
	hle_jump(ctx, 0x000DD8); return;  /* 000DD8 jsr %pc@(8a7c) */
L_000DDC:
	{ ctx->a[0] = 0x0000843Au; } hle_insn(ctx, 8); /* 000DDC lea %pc@(843a),%a0 */
	hle_jump(ctx, 0x000DE0); return;  /* 000DE0 jsr %pc@(8a7c) */
L_000DE4:
	{ st16(0xFFFFA0EEu, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 20); /* 000DE4 clrw ffffa0ee */
	hle_jump(ctx, 0x000DEA); return;  /* 000DEA bsrw e60 */
L_000DEE:
	{ uint32_t v = 0x4000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 000DEE movew #16384,%d6 */
	{ ctx->a[0] = 0x000012D8u; } hle_insn(ctx, 12); /* 000DF2 lea 12d8,%a0 */
	hle_jump(ctx, 0x000DF8); return;  /* 000DF8 jsr 7dee */
	hle_jump(ctx, 0x000DFE); return;  /* 000DFE jsr 7dee */
L_000E04:
	{ st8(0xFFFFA246u, (1) ? 0xFF : 0); } hle_insn(ctx, 20); /* 000E04 st ffffa246 */
	hle_jump(ctx, 0x000E0A); return;  /* 000E0A jsr %pc@(8a4c) */
L_000E0E:
	{ st16(0xFFFFA0EEu, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 20); /* 000E0E clrw ffffa0ee */
L_000E14:
	hle_jump(ctx, 0x000E14); return;  /* 000E14 bsrw e60 */
	hle_jump(ctx, 0x000E18); return;  /* 000E18 bsrw e22 */
L_000E1C:
	{ cc_logic8(ctx, (ctx->d[0] & 0xFFu)); } hle_insn(ctx, 4); /* 000E1C tstb %d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_000E14; } hle_insn(ctx, 8);  /* 000E1E beqs e14 */
	hle_jump(ctx, 0x000E20); return;  /* 000E20 rts */
	hle_jump(ctx, 0x000E22); return;  /* 000E22 jsr 86e2 */
	hle_jump(ctx, 0x000E28); return;  /* 000E28 jsr %pc@(88a0) */
L_000E2C:
	{ uint32_t v = ld16(0xFFFF8050u); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 000E2C movew ffff8050,%d0 */
	{ uint32_t v = ld16(0xFFFF8052u); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 000E32 movew ffff8052,%d1 */
	{ uint32_t da = 0xFFFFA0EEu; alu(ctx, OP_SUB, ld16(da), 0xB4u, 16, 0); } hle_insn(ctx, 20); /* 000E38 cmpiw #180,ffffa0ee */
	if (COND_LT) { hle_insn(ctx, 10); goto L_000E5C; } hle_insn(ctx, 8);  /* 000E40 blts e5c */
	{ uint32_t da = 0xFFFFA0EEu; alu(ctx, OP_SUB, ld16(da), 0x12Cu, 16, 0); } hle_insn(ctx, 20); /* 000E42 cmpiw #300,ffffa0ee */
	if (COND_GE) { hle_insn(ctx, 10); goto L_000E58; } hle_insn(ctx, 8);  /* 000E4A bges e58 */
	{ uint32_t m = (1u << (0x7u & 31)); cc_set(ctx, CC_Z, (ctx->d[0] & m) ? 0 : CC_Z); } hle_insn(ctx, 10); /* 000E4C btst #7,%d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_000E5C; } hle_insn(ctx, 8);  /* 000E50 beqs e5c */
	{ uint32_t m = (1u << (0x7u & 31)); cc_set(ctx, CC_Z, (ctx->d[1] & m) ? 0 : CC_Z); } hle_insn(ctx, 10); /* 000E52 btst #7,%d1 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_000E5C; } hle_insn(ctx, 8);  /* 000E56 bnes e5c */
L_000E58:
	{ set_b(&ctx->d[0], 0xFF); hle_insn(ctx, 6); } /* 000E58 st %d0 */
	hle_jump(ctx, 0x000E5A); return;  /* 000E5A rts */
L_000E5C:
	{ set_b(&ctx->d[0], 0); hle_insn(ctx, 4); } /* 000E5C sf %d0 */
	hle_jump(ctx, 0x000E5E); return;  /* 000E5E rts */
	hle_jump(ctx, 0x000E60); return;  /* 000E60 jsr 11d12 */
L_000E66:
	{ uint32_t v = ld16(0xFFFFA0EEu); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 000E66 movew ffffa0ee,%d0 */
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 4); /* 000E6C movew %d0,%d1 */
	{ uint32_t r = ((ctx->d[1] & 0xFFFFu) & 0xE0u) & 0xFFFFu; cc_logic16(ctx, r); set_w(&ctx->d[1], r); } hle_insn(ctx, 8); /* 000E6E andiw #224,%d1 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_000E78; } hle_insn(ctx, 8);  /* 000E72 beqs e78 */
	{ ctx->d[0] = 0x0u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4); /* 000E74 moveq #0,%d0 */
	hle_insn(ctx, 10); goto L_000E7E;  /* 000E76 bras e7e */
L_000E78:
	{ sh_n(ctx, 0, "asr", 1, 16); hle_insn(ctx, 8); } /* 000E78 asrw #1,%d0 */
	{ uint32_t r = ((ctx->d[0] & 0xFFFFu) & 0xEu) & 0xFFFFu; cc_logic16(ctx, r); set_w(&ctx->d[0], r); } hle_insn(ctx, 8); /* 000E7A andiw #14,%d0 */
L_000E7E:
	{ uint32_t v = ld16((0x00000EB4u + (int16_t)ctx->d[0])); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 14); /* 000E7E movew %pc@(eb4,%d0:w),%d0 */
	{ ctx->a[0] = 0x000B3842u; } hle_insn(ctx, 12); /* 000E82 lea b3842,%a0 */
	{ ctx->a[0] = (ctx->a[0] + (0) + (int16_t)ctx->d[0]); } hle_insn(ctx, 12); /* 000E88 lea %a0@(0,%d0:w),%a0 */
	{ uint32_t v = 0xC514u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 000E8C movew #-15084,%d1 */
	{ ctx->d[2] = 0x14u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4); /* 000E90 moveq #20,%d2 */
	{ ctx->d[3] = 0x8u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4); /* 000E92 moveq #8,%d3 */
	{ uint32_t v = 0x0u; ctx->d[6] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12); /* 000E94 movel #0,%d6 */
	{ uint32_t v = ld16(0xFFFF9E38u); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 000E9A movew ffff9e38,%d7 */
	hle_jump(ctx, 0x000EA0); return;  /* 000EA0 jsr 12234 */
	hle_jump(ctx, 0x000EA6); return;  /* 000EA6 jsr 11d28 */
	hle_jump(ctx, 0x000EAC); return;  /* 000EAC jsr 11d32 */
	hle_jump(ctx, 0x000EB2); return;  /* 000EB2 rts */
L_000ED4:
	{ uint32_t v = 0x0u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 000ED4 movew #0,%d0 */
	hle_jump(ctx, 0x000ED8); return;  /* 000ED8 jsr %pc@(56fa) */
	hle_jump(ctx, 0x000EDC); return;  /* 000EDC jsr %pc@(871a) */
	hle_jump(ctx, 0x000EE0); return;  /* 000EE0 jsr 121d6 */
L_000EE6:
	{ ctx->a[1] = 0x0000576Au; } hle_insn(ctx, 8); /* 000EE6 lea %pc@(576a),%a1 */
	hle_jump(ctx, 0x000EEA); return;  /* 000EEA jsr 867a */
L_000EF0:
	{ ctx->a[0] = 0x000B8286u; } hle_insn(ctx, 12); /* 000EF0 lea b8286,%a0 */
	{ uint32_t v = 0x1u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 000EF6 movew #1,%d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFFu); st16(0xFFFF9E38u, v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 000EFA movew %d1,ffff9e38 */
	hle_jump(ctx, 0x000F00); return;  /* 000F00 jsr 1320a */
L_000F06:
	{ ctx->a[0] = 0x000B57C4u; } hle_insn(ctx, 12); /* 000F06 lea b57c4,%a0 */
	{ uint32_t v = 0x190u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 000F0C movew #400,%d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFFu); st16(0xFFFF9E5Au, v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 000F10 movew %d1,ffff9e5a */
	hle_jump(ctx, 0x000F16); return;  /* 000F16 jsr 1320a */
L_000F1C:
	{ ctx->a[0] = 0x00025DFCu; } hle_insn(ctx, 12); /* 000F1C lea 25dfc,%a0 */
	{ uint32_t v = 0x670u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 000F22 movew #1648,%d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFFu); st16(0xFFFF9E2Cu, v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 000F26 movew %d1,ffff9e2c */
	hle_jump(ctx, 0x000F2C); return;  /* 000F2C jsr 1320a */
L_000F32:
	{ ctx->a[0] = 0x0000823Au; } hle_insn(ctx, 8); /* 000F32 lea %pc@(823a),%a0 */
	{ ctx->a[1] = 0xFFFFA134u; } hle_insn(ctx, 12); /* 000F36 lea ffffa134,%a1 */
	hle_jump(ctx, 0x000F3C); return;  /* 000F3C jsr %pc@(8a7c) */
L_000F40:
	{ ctx->a[0] = 0x000085FAu; } hle_insn(ctx, 8); /* 000F40 lea %pc@(85fa),%a0 */
	hle_jump(ctx, 0x000F44); return;  /* 000F44 jsr %pc@(8a7c) */
L_000F48:
	{ ctx->a[0] = 0x000083FAu; } hle_insn(ctx, 8); /* 000F48 lea %pc@(83fa),%a0 */
	hle_jump(ctx, 0x000F4C); return;  /* 000F4C jsr %pc@(8a7c) */
L_000F50:
	{ ctx->a[0] = 0x0000843Au; } hle_insn(ctx, 8); /* 000F50 lea %pc@(843a),%a0 */
	hle_jump(ctx, 0x000F54); return;  /* 000F54 jsr %pc@(8a7c) */
	hle_jump(ctx, 0x000F58); return;  /* 000F58 bsrw 102a */
L_000F5C:
	{ st8(0xFFFFA234u, (0) ? 0xFF : 0); } hle_insn(ctx, 20); /* 000F5C sf ffffa234 */
	{ st16(0xFFFF9F60u, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 20); /* 000F62 clrw ffff9f60 */
	hle_jump(ctx, 0x000F68); return;  /* 000F68 bsrw 1be8 */
L_000F6C:
	{ cc_logic16(ctx, (ctx->d[0] & 0xFFFFu)); } hle_insn(ctx, 4); /* 000F6C tstw %d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_000F76; } hle_insn(ctx, 8);  /* 000F6E beqs f76 */
	{ uint32_t da = 0xFFFF9F60u; uint32_t r = alu(ctx, OP_ADD, ld16(da), 1, 16, 1); st16(da, r); } hle_insn(ctx, 20); /* 000F70 addqw #1,ffff9f60 */
L_000F76:
	hle_jump(ctx, 0x000F76); return;  /* 000F76 bsrw 107e */
L_000F7A:
	{ st8(0xFFFFA246u, (1) ? 0xFF : 0); } hle_insn(ctx, 20); /* 000F7A st ffffa246 */
	hle_jump(ctx, 0x000F80); return;  /* 000F80 jsr %pc@(8a4c) */
L_000F84:
	hle_jump(ctx, 0x000F84); return;  /* 000F84 bsrw 102a */
	hle_jump(ctx, 0x000F88); return;  /* 000F88 bsrw 1164 */
L_000F8C:
	{ cc_logic8(ctx, ld8(0xFFFFA234u)); } hle_insn(ctx, 16); /* 000F8C tstb ffffa234 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_000F84; } hle_insn(ctx, 8);  /* 000F92 beqs f84 */
	{ uint32_t v = ld16(0xFFFF9F60u); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 000F94 movew ffff9f60,%d0 */
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x4u, 16, 0); } hle_insn(ctx, 8); /* 000F9A cmpiw #4,%d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_001344; } hle_insn(ctx, 12);  /* 000F9E beqw 1344 */
	{ uint32_t v = ld8((0x00000FF0u + (int16_t)ctx->d[0])); set_b(&ctx->d[4], v); cc_logic8(ctx, v); } hle_insn(ctx, 14); /* 000FA2 moveb %pc@(ff0,%d0:w),%d4 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_000FAE; } hle_insn(ctx, 8);  /* 000FA6 beqs fae */
	{ alu(ctx, OP_SUB, (ctx->d[4] & 0xFFu), 0x4u, 8, 0); } hle_insn(ctx, 8); /* 000FA8 cmpib #4,%d4 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_000FC0; } hle_insn(ctx, 8);  /* 000FAC bnes fc0 */
L_000FAE:
	hle_jump(ctx, 0x000FAE); return;  /* 000FAE bsrw 1be8 */
L_000FB2:
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x0u, 16, 0); } hle_insn(ctx, 8); /* 000FB2 cmpiw #0,%d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_000FC0; } hle_insn(ctx, 8);  /* 000FB6 beqs fc0 */
	{ st8(0xFFFFA234u, (0) ? 0xFF : 0); } hle_insn(ctx, 20); /* 000FB8 sf ffffa234 */
	hle_insn(ctx, 10); goto L_000F84;  /* 000FBE bras f84 */
L_000FC0:
	{ alu(ctx, OP_SUB, (ctx->d[4] & 0xFFu), 0x1u, 8, 0); } hle_insn(ctx, 8); /* 000FC0 cmpib #1,%d4 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_000FD8; } hle_insn(ctx, 8);  /* 000FC4 bnes fd8 */
	hle_jump(ctx, 0x000FC6); return;  /* 000FC6 bsrw 1be8 */
L_000FCA:
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0xDu, 16, 0); } hle_insn(ctx, 8); /* 000FCA cmpiw #13,%d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_000FD8; } hle_insn(ctx, 8);  /* 000FCE beqs fd8 */
	{ st8(0xFFFFA234u, (0) ? 0xFF : 0); } hle_insn(ctx, 20); /* 000FD0 sf ffffa234 */
	hle_insn(ctx, 10); goto L_000F84;  /* 000FD6 bras f84 */
L_000FD8:
	{ uint32_t v = (ctx->d[4] & 0xFFu); st8(0xFFFF9DF5u, v); cc_logic8(ctx, v); } hle_insn(ctx, 16); /* 000FD8 moveb %d4,ffff9df5 */
	{ uint32_t v = ld16(0xFFFFFF9Cu); set_w(&ctx->d[4], v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 000FDE movew ffffff9c,%d4 */
	hle_jump(ctx, 0x000FE4); return;  /* 000FE4 bsrw 3818 */
L_000FE8:
	{ uint32_t v = (ctx->d[1] & 0xFFu); st8(0xFFFF80CAu, v); cc_logic8(ctx, v); } hle_insn(ctx, 16); /* 000FE8 moveb %d1,ffff80ca */
	hle_jump(ctx, 0x000FEE); return;  /* 000FEE rts */
	hle_jump(ctx, 0x000FF6); return;  /* 000FF6 jsr 11d12 */
L_000FFC:
	{ ctx->a[0] = 0x000BA848u; } hle_insn(ctx, 12); /* 000FFC lea ba848,%a0 */
	{ uint32_t v = 0xC000u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 001002 movew #-16384,%d1 */
	{ ctx->d[2] = 0x28u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4); /* 001006 moveq #40,%d2 */
	{ ctx->d[3] = 0x1Cu; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4); /* 001008 moveq #28,%d3 */
	{ uint32_t v = 0x0u; ctx->d[6] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12); /* 00100A movel #0,%d6 */
	{ uint32_t v = ld16(0xFFFF9E38u); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 001010 movew ffff9e38,%d7 */
	hle_jump(ctx, 0x001016); return;  /* 001016 jsr 12234 */
	hle_jump(ctx, 0x00101C); return;  /* 00101C jsr 11d28 */
	hle_jump(ctx, 0x001022); return;  /* 001022 jsr 11d32 */
	hle_jump(ctx, 0x001028); return;  /* 001028 rts */
	hle_jump(ctx, 0x00102A); return;  /* 00102A bsrw 1be8 */
L_00102E:
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 4); /* 00102E movew %d0,%d7 */
	{ ctx->a[0] = 0x000011DEu; } hle_insn(ctx, 8); /* 001030 lea %pc@(11de),%a0 */
	{ uint32_t v = 0x4000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 001034 movew #16384,%d6 */
	{ cc_logic16(ctx, (ctx->d[7] & 0xFFFFu)); } hle_insn(ctx, 4); /* 001038 tstw %d7 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_001040; } hle_insn(ctx, 8);  /* 00103A beqs 1040 */
	{ uint32_t v = 0x2000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 00103C movew #8192,%d6 */
L_001040:
	hle_jump(ctx, 0x001040); return;  /* 001040 jsr %pc@(7dee) */
L_001044:
	{ uint32_t v = 0x4000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 001044 movew #16384,%d6 */
	hle_jump(ctx, 0x001048); return;  /* 001048 jsr %pc@(7dee) */
L_00104C:
	{ uint32_t v = 0x4000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 00104C movew #16384,%d6 */
	{ cc_logic16(ctx, (ctx->d[7] & 0xFFFFu)); } hle_insn(ctx, 4); /* 001050 tstw %d7 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_001058; } hle_insn(ctx, 8);  /* 001052 beqs 1058 */
	{ uint32_t v = 0x2000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 001054 movew #8192,%d6 */
L_001058:
	hle_jump(ctx, 0x001058); return;  /* 001058 jsr %pc@(7dee) */
L_00105C:
	{ uint32_t v = 0x4000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 00105C movew #16384,%d6 */
	{ alu(ctx, OP_SUB, (ctx->d[7] & 0xFFFFu), 0xDu, 16, 0); } hle_insn(ctx, 8); /* 001060 cmpiw #13,%d7 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_00106A; } hle_insn(ctx, 8);  /* 001064 beqs 106a */
	{ uint32_t v = 0x2000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 001066 movew #8192,%d6 */
L_00106A:
	hle_jump(ctx, 0x00106A); return;  /* 00106A jsr %pc@(7dee) */
L_00106E:
	{ uint32_t v = 0x4000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 00106E movew #16384,%d6 */
	hle_jump(ctx, 0x001072); return;  /* 001072 jsr %pc@(7dee) */
	hle_jump(ctx, 0x001076); return;  /* 001076 rts */
L_00107E:
	{ uint32_t v = ld16(0xFFFF9F60u); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 00107E movew ffff9f60,%d0 */
	{ uint32_t v = ld8((0x00001078u + (int16_t)ctx->d[0])); set_b(&ctx->d[0], v); cc_logic8(ctx, v); } hle_insn(ctx, 14); /* 001084 moveb %pc@(1078,%d0:w),%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_001090; } hle_insn(ctx, 8);  /* 001088 bnes 1090 */
	hle_jump(ctx, 0x00108A); return;  /* 00108A bsrw ff6 */
L_00108E:
	hle_insn(ctx, 10); goto L_0010B8;  /* 00108E bras 10b8 */
L_001090:
	if (COND_MI) { hle_insn(ctx, 10); goto L_001098; } hle_insn(ctx, 8);  /* 001090 bmis 1098 */
	hle_jump(ctx, 0x001092); return;  /* 001092 bsrw 1130 */
L_001096:
	hle_insn(ctx, 10); goto L_0010B8;  /* 001096 bras 10b8 */
L_001098:
	hle_jump(ctx, 0x001098); return;  /* 001098 jsr 11d12 */
L_00109E:
	{ uint32_t v = 0xC000u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 00109E movew #-16384,%d1 */
	{ ctx->d[2] = 0x28u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4); /* 0010A2 moveq #40,%d2 */
	{ ctx->d[3] = 0x1Cu; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4); /* 0010A4 moveq #28,%d3 */
	hle_jump(ctx, 0x0010A6); return;  /* 0010A6 jsr 12212 */
	hle_jump(ctx, 0x0010AC); return;  /* 0010AC jsr 11d28 */
	hle_jump(ctx, 0x0010B2); return;  /* 0010B2 jsr 11d32 */
L_0010B8:
	{ uint32_t v = 0x7Au; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 0010B8 movew #122,%d0 */
	hle_jump(ctx, 0x0010BC); return;  /* 0010BC jsr 13d04 */
	hle_jump(ctx, 0x0010C2); return;  /* 0010C2 jsr 11d12 */
L_0010C8:
	{ uint32_t v = 0xE004u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 0010C8 movew #-8188,%d1 */
	{ uint32_t v = 0x2u; set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 0010CC movew #2,%d2 */
	{ uint32_t v = 0x19u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 0010D0 movew #25,%d3 */
	hle_jump(ctx, 0x0010D4); return;  /* 0010D4 jsr 12212 */
L_0010DA:
	{ uint32_t v = 0xE04Au; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 0010DA movew #-8118,%d1 */
	{ uint32_t v = 0x2u; set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 0010DE movew #2,%d2 */
	{ uint32_t v = 0x19u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 0010E2 movew #25,%d3 */
	hle_jump(ctx, 0x0010E6); return;  /* 0010E6 jsr 12212 */
	hle_jump(ctx, 0x0010EC); return;  /* 0010EC jsr 11d28 */
	hle_jump(ctx, 0x0010F2); return;  /* 0010F2 jsr 11d32 */
L_0010F8:
	{ uint32_t v = ld16(0xFFFF9F60u); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 0010F8 movew ffff9f60,%d3 */
	{ sh_n(ctx, 3, "asl", 7, 16); hle_insn(ctx, 20); } /* 0010FE aslw #7,%d3 */
	{ uint32_t v = (ctx->d[3] & 0xFFFFu); set_w(&ctx->d[4], v); cc_logic16(ctx, v); } hle_insn(ctx, 4); /* 001100 movew %d3,%d4 */
	{ uint32_t r = alu(ctx, OP_ADD, (ctx->d[3] & 0xFFFFu), (ctx->d[3] & 0xFFFFu), 16, 1); set_w(&ctx->d[3], r); } hle_insn(ctx, 4); /* 001102 addw %d3,%d3 */
	{ uint32_t r = alu(ctx, OP_ADD, (ctx->d[3] & 0xFFFFu), (ctx->d[4] & 0xFFFFu), 16, 1); set_w(&ctx->d[3], r); } hle_insn(ctx, 4); /* 001104 addw %d4,%d3 */
	{ uint32_t r = alu(ctx, OP_ADD, (ctx->d[3] & 0xFFFFu), 0xE284u, 16, 1); set_w(&ctx->d[3], r); } hle_insn(ctx, 8); /* 001106 addiw #-7548,%d3 */
	{ uint32_t v = 0x4000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 00110A movew #16384,%d6 */
	{ ctx->a[0] = 0x000012BAu; } hle_insn(ctx, 8); /* 00110E lea %pc@(12ba),%a0 */
	{ uint32_t v = (ctx->d[3] & 0xFFFFu); ctx->a[7] -= 2; st16(ctx->a[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 001112 movew %d3,%sp@- */
	hle_jump(ctx, 0x001114); return;  /* 001114 jsr 7e08 */
L_00111A:
	{ uint32_t v = ld16(ctx->a[7]); ctx->a[7] += 2; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 00111A movew %sp@+,%d3 */
	{ uint32_t r = alu(ctx, OP_ADD, (ctx->d[3] & 0xFFFFu), 0x48u, 16, 1); set_w(&ctx->d[3], r); } hle_insn(ctx, 8); /* 00111C addiw #72,%d3 */
	{ uint32_t v = 0x4800u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 001120 movew #18432,%d6 */
	{ ctx->a[0] = 0x000012BAu; } hle_insn(ctx, 8); /* 001124 lea %pc@(12ba),%a0 */
	hle_jump(ctx, 0x001128); return;  /* 001128 jsr 7e08 */
	hle_jump(ctx, 0x00112E); return;  /* 00112E rts */
	hle_jump(ctx, 0x001130); return;  /* 001130 jsr 11d12 */
L_001136:
	{ ctx->a[0] = 0x000B7886u; } hle_insn(ctx, 12); /* 001136 lea b7886,%a0 */
	{ uint32_t v = 0xC000u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 00113C movew #-16384,%d1 */
	{ ctx->d[2] = 0x28u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4); /* 001140 moveq #40,%d2 */
	{ ctx->d[3] = 0x1Cu; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4); /* 001142 moveq #28,%d3 */
	{ uint32_t v = 0x0u; ctx->d[6] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12); /* 001144 movel #0,%d6 */
	{ uint32_t v = ld16(0xFFFF9E5Au); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 00114A movew ffff9e5a,%d7 */
	hle_jump(ctx, 0x001150); return;  /* 001150 jsr 12234 */
	hle_jump(ctx, 0x001156); return;  /* 001156 jsr 11d28 */
	hle_jump(ctx, 0x00115C); return;  /* 00115C jsr 11d32 */
	hle_jump(ctx, 0x001162); return;  /* 001162 rts */
	hle_jump(ctx, 0x001164); return;  /* 001164 jsr 86e2 */
	hle_jump(ctx, 0x00116A); return;  /* 00116A jsr %pc@(88a0) */
L_00116E:
	{ uint32_t v = ld16(0xFFFF8050u); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 00116E movew ffff8050,%d0 */
	{ uint32_t v = ld16(0xFFFF8052u); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 001174 movew ffff8052,%d1 */
	{ ctx->d[2] = 0x0u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4); /* 00117A moveq #0,%d2 */
	hle_jump(ctx, 0x00117C); return;  /* 00117C bsrw 11ce */
L_001180:
	{ cc_logic8(ctx, (ctx->d[7] & 0xFFu)); } hle_insn(ctx, 4); /* 001180 tstb %d7 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_001196; } hle_insn(ctx, 8);  /* 001182 beqs 1196 */
	{ uint32_t v = ld16(0xFFFF9F60u); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 001184 movew ffff9f60,%d3 */
	{ uint32_t r = alu(ctx, OP_SUB, (ctx->d[3] & 0xFFFFu), 1, 16, 1); set_w(&ctx->d[3], r); } hle_insn(ctx, 4); /* 00118A subqw #1,%d3 */
	if (COND_CS) { hle_insn(ctx, 10); goto L_0011CC; } hle_insn(ctx, 8);  /* 00118C bcss 11cc */
	{ uint32_t v = (ctx->d[3] & 0xFFFFu); st16(0xFFFF9F60u, v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 00118E movew %d3,ffff9f60 */
	hle_insn(ctx, 10); goto L_0011C8;  /* 001194 bras 11c8 */
L_001196:
	{ ctx->d[2] = 0x1u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4); /* 001196 moveq #1,%d2 */
	hle_jump(ctx, 0x001198); return;  /* 001198 bsrw 11ce */
L_00119C:
	{ cc_logic8(ctx, (ctx->d[7] & 0xFFu)); } hle_insn(ctx, 4); /* 00119C tstb %d7 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0011B6; } hle_insn(ctx, 8);  /* 00119E beqs 11b6 */
	{ uint32_t v = ld16(0xFFFF9F60u); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 0011A0 movew ffff9f60,%d3 */
	{ uint32_t r = alu(ctx, OP_ADD, (ctx->d[3] & 0xFFFFu), 1, 16, 1); set_w(&ctx->d[3], r); } hle_insn(ctx, 4); /* 0011A6 addqw #1,%d3 */
	{ alu(ctx, OP_SUB, (ctx->d[3] & 0xFFFFu), 0x4u, 16, 0); } hle_insn(ctx, 8); /* 0011A8 cmpiw #4,%d3 */
	if (COND_GT) { hle_insn(ctx, 10); goto L_0011CC; } hle_insn(ctx, 8);  /* 0011AC bgts 11cc */
	{ uint32_t v = (ctx->d[3] & 0xFFFFu); st16(0xFFFF9F60u, v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 0011AE movew %d3,ffff9f60 */
	hle_insn(ctx, 10); goto L_0011C8;  /* 0011B4 bras 11c8 */
L_0011B6:
	{ ctx->d[2] = 0x7u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4); /* 0011B6 moveq #7,%d2 */
	hle_jump(ctx, 0x0011B8); return;  /* 0011B8 bsrw 11ce */
L_0011BC:
	{ cc_logic8(ctx, (ctx->d[7] & 0xFFu)); } hle_insn(ctx, 4); /* 0011BC tstb %d7 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0011CC; } hle_insn(ctx, 8);  /* 0011BE beqs 11cc */
	{ st8(0xFFFFA234u, (1) ? 0xFF : 0); } hle_insn(ctx, 20); /* 0011C0 st ffffa234 */
	hle_insn(ctx, 10); goto L_0011CC;  /* 0011C6 bras 11cc */
L_0011C8:
	hle_jump(ctx, 0x0011C8); return;  /* 0011C8 bsrw 107e */
L_0011CC:
	hle_jump(ctx, 0x0011CC); return;  /* 0011CC rts */
L_0011CE:
	{ uint32_t m = (1u << (ctx->d[2] & 31)); cc_set(ctx, CC_Z, (ctx->d[0] & m) ? 0 : CC_Z); } hle_insn(ctx, 6); /* 0011CE btst %d2,%d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0011DA; } hle_insn(ctx, 8);  /* 0011D0 beqs 11da */
	{ uint32_t m = (1u << (ctx->d[2] & 31)); cc_set(ctx, CC_Z, (ctx->d[1] & m) ? 0 : CC_Z); } hle_insn(ctx, 6); /* 0011D2 btst %d2,%d1 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0011DA; } hle_insn(ctx, 8);  /* 0011D4 bnes 11da */
	{ set_b(&ctx->d[7], 0xFF); hle_insn(ctx, 6); } /* 0011D6 st %d7 */
	hle_jump(ctx, 0x0011D8); return;  /* 0011D8 rts */
L_0011DA:
	{ set_b(&ctx->d[7], 0); hle_insn(ctx, 4); } /* 0011DA sf %d7 */
	hle_jump(ctx, 0x0011DC); return;  /* 0011DC rts */
L_001344:
	hle_jump(ctx, 0x001344); return;  /* 001344 jsr %pc@(874e) */
L_001348:
	{ st8(0xFFFFA234u, (0) ? 0xFF : 0); } hle_insn(ctx, 20); /* 001348 sf ffffa234 */
	{ st16(0xFFFF9F60u, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 20); /* 00134E clrw ffff9f60 */
	{ ctx->a[0] = 0x000012E9u; } hle_insn(ctx, 8); /* 001354 lea %pc@(12e9),%a0 */
	{ uint32_t v = 0x4000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 001358 movew #16384,%d6 */
	hle_jump(ctx, 0x00135C); return;  /* 00135C jsr 7dee */
	hle_jump(ctx, 0x001362); return;  /* 001362 bsrw 10b8 */
	hle_jump(ctx, 0x001366); return;  /* 001366 bsrw 14de */
L_00136A:
	{ uint32_t v = ld16(0xFFFFFF9Cu); st16(0xFFFFAE84u, v); cc_logic16(ctx, v); } hle_insn(ctx, 28); /* 00136A movew ffffff9c,ffffae84 */
	hle_jump(ctx, 0x001374); return;  /* 001374 bsrw 14c8 */
	hle_jump(ctx, 0x001378); return;  /* 001378 bsrw 14a4 */
L_00137C:
	hle_jump(ctx, 0x00137C); return;  /* 00137C bsrw 13a2 */
L_001380:
	{ cc_logic8(ctx, ld8(0xFFFFA234u)); } hle_insn(ctx, 16); /* 001380 tstb ffffa234 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_00137C; } hle_insn(ctx, 8);  /* 001386 beqs 137c */
	{ ctx->d[0] = 0x0u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4); /* 001388 moveq #0,%d0 */
	hle_jump(ctx, 0x00138A); return;  /* 00138A jsr fbb6c */
	hle_jump(ctx, 0x001390); return;  /* 001390 jsr %pc@(874e) */
L_001394:
	{ uint32_t v = ld16(0xFFFFAE84u); st16(0xFFFFFF9Cu, v); cc_logic16(ctx, v); } hle_insn(ctx, 28); /* 001394 movew ffffae84,ffffff9c */
	hle_insn(ctx, 10); goto L_000F32;  /* 00139E braw f32 */
	hle_jump(ctx, 0x0013A2); return;  /* 0013A2 jsr 86e2 */
	hle_jump(ctx, 0x0013A8); return;  /* 0013A8 jsr %pc@(88a0) */
L_0013AC:
	{ uint32_t v = ld16(0xFFFF8050u); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 0013AC movew ffff8050,%d0 */
	{ uint32_t v = ld16(0xFFFF8052u); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 0013B2 movew ffff8052,%d1 */
	{ ctx->d[2] = 0x3u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4); /* 0013B8 moveq #3,%d2 */
	hle_jump(ctx, 0x0013BA); return;  /* 0013BA bsrw 11ce */
L_0013BE:
	{ cc_logic8(ctx, (ctx->d[7] & 0xFFu)); } hle_insn(ctx, 4); /* 0013BE tstb %d7 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0013DE; } hle_insn(ctx, 8);  /* 0013C0 beqs 13de */
	hle_jump(ctx, 0x0013C2); return;  /* 0013C2 bsrw 14c8 */
L_0013C6:
	{ uint32_t v = ld16(ctx->a[1]); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 0013C6 movew %a1@,%d0 */
	{ uint32_t v = ld16((ctx->a[1] + (6))); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 12); /* 0013C8 movew %a1@(6),%d1 */
	{ uint32_t r = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), (ctx->d[1] & 0xFFFFu), 16, 1); set_w(&ctx->d[0], r); } hle_insn(ctx, 4); /* 0013CC addw %d1,%d0 */
	{ alu(ctx, OP_SUB, ctx->d[0] & 0xFFFFu, ld16((ctx->a[1] + (4))), 16, 0); } hle_insn(ctx, 12); /* 0013CE cmpw %a1@(4),%d0 */
	if (COND_GT) { hle_insn(ctx, 10); goto L_0013DA; } hle_insn(ctx, 8);  /* 0013D2 bgts 13da */
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); st16(ctx->a[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 0013D4 movew %d0,%a1@ */
	hle_jump(ctx, 0x0013D6); return;  /* 0013D6 bsrw 14a4 */
L_0013DA:
	hle_insn(ctx, 10); goto L_0014A2;  /* 0013DA braw 14a2 */
L_0013DE:
	{ ctx->d[2] = 0x2u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4); /* 0013DE moveq #2,%d2 */
	hle_jump(ctx, 0x0013E0); return;  /* 0013E0 bsrw 11ce */
L_0013E4:
	{ cc_logic8(ctx, (ctx->d[7] & 0xFFu)); } hle_insn(ctx, 4); /* 0013E4 tstb %d7 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_001404; } hle_insn(ctx, 8);  /* 0013E6 beqs 1404 */
	hle_jump(ctx, 0x0013E8); return;  /* 0013E8 bsrw 14c8 */
L_0013EC:
	{ uint32_t v = ld16(ctx->a[1]); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 0013EC movew %a1@,%d0 */
	{ uint32_t v = ld16((ctx->a[1] + (6))); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 12); /* 0013EE movew %a1@(6),%d1 */
	{ uint32_t r = alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), (ctx->d[1] & 0xFFFFu), 16, 1); set_w(&ctx->d[0], r); } hle_insn(ctx, 4); /* 0013F2 subw %d1,%d0 */
	{ alu(ctx, OP_SUB, ctx->d[0] & 0xFFFFu, ld16((ctx->a[1] + (2))), 16, 0); } hle_insn(ctx, 12); /* 0013F4 cmpw %a1@(2),%d0 */
	if (COND_LT) { hle_insn(ctx, 10); goto L_001400; } hle_insn(ctx, 8);  /* 0013F8 blts 1400 */
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); st16(ctx->a[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 0013FA movew %d0,%a1@ */
	hle_jump(ctx, 0x0013FC); return;  /* 0013FC bsrw 14a4 */
L_001400:
	hle_insn(ctx, 10); goto L_0014A2;  /* 001400 braw 14a2 */
L_001404:
	{ ctx->d[2] = 0x6u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4); /* 001404 moveq #6,%d2 */
	hle_jump(ctx, 0x001406); return;  /* 001406 bsrw 11ce */
L_00140A:
	{ cc_logic8(ctx, (ctx->d[7] & 0xFFu)); } hle_insn(ctx, 4); /* 00140A tstb %d7 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_001422; } hle_insn(ctx, 8);  /* 00140C bnes 1422 */
	{ ctx->d[2] = 0x4u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4); /* 00140E moveq #4,%d2 */
	hle_jump(ctx, 0x001410); return;  /* 001410 bsrw 11ce */
L_001414:
	{ cc_logic8(ctx, (ctx->d[7] & 0xFFu)); } hle_insn(ctx, 4); /* 001414 tstb %d7 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_001422; } hle_insn(ctx, 8);  /* 001416 bnes 1422 */
	{ ctx->d[2] = 0x5u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4); /* 001418 moveq #5,%d2 */
	hle_jump(ctx, 0x00141A); return;  /* 00141A bsrw 11ce */
L_00141E:
	{ cc_logic8(ctx, (ctx->d[7] & 0xFFu)); } hle_insn(ctx, 4); /* 00141E tstb %d7 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_001436; } hle_insn(ctx, 8);  /* 001420 beqs 1436 */
L_001422:
	hle_jump(ctx, 0x001422); return;  /* 001422 bsrw 14c8 */
L_001426:
	{ uint32_t v = ld32((ctx->a[1] + (10))); ctx->d[0] = v; cc_logic32(ctx, v); } hle_insn(ctx, 16); /* 001426 movel %a1@(10),%d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0014A2; } hle_insn(ctx, 12);  /* 00142A beqw 14a2 */
	{ ctx->a[2] = ctx->d[0]; } hle_insn(ctx, 4); /* 00142E moveal %d0,%a2 */
	hle_jump(ctx, 0x001430); return;  /* 001430 jsr %a2@ */
L_001432:
	hle_insn(ctx, 10); goto L_0014A2;  /* 001432 braw 14a2 */
L_001436:
	{ ctx->d[2] = 0x0u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4); /* 001436 moveq #0,%d2 */
	hle_jump(ctx, 0x001438); return;  /* 001438 bsrw 11ce */
L_00143C:
	{ cc_logic8(ctx, (ctx->d[7] & 0xFFu)); } hle_insn(ctx, 4); /* 00143C tstb %d7 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_001460; } hle_insn(ctx, 8);  /* 00143E beqs 1460 */
	{ uint32_t v = ld16(0xFFFF9F60u); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 001440 movew ffff9f60,%d3 */
	{ uint32_t r = alu(ctx, OP_SUB, (ctx->d[3] & 0xFFFFu), 1, 16, 1); set_w(&ctx->d[3], r); } hle_insn(ctx, 4); /* 001446 subqw #1,%d3 */
	if (COND_CS) { hle_insn(ctx, 10); goto L_0014A2; } hle_insn(ctx, 8);  /* 001448 bcss 14a2 */
	{ uint32_t v = (ctx->d[3] & 0xFFFFu); st16(0xFFFF9F60u, v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 00144A movew %d3,ffff9f60 */
	{ ctx->d[0] = 0x0u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4); /* 001450 moveq #0,%d0 */
	hle_jump(ctx, 0x001452); return;  /* 001452 jsr fbb6c */
	hle_jump(ctx, 0x001458); return;  /* 001458 jsr 86e2 */
L_00145E:
	hle_insn(ctx, 10); goto L_00149E;  /* 00145E bras 149e */
L_001460:
	{ ctx->d[2] = 0x1u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4); /* 001460 moveq #1,%d2 */
	hle_jump(ctx, 0x001462); return;  /* 001462 bsrw 11ce */
L_001466:
	{ cc_logic8(ctx, (ctx->d[7] & 0xFFu)); } hle_insn(ctx, 4); /* 001466 tstb %d7 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_00148E; } hle_insn(ctx, 8);  /* 001468 beqs 148e */
	{ uint32_t v = ld16(0xFFFF9F60u); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 00146A movew ffff9f60,%d3 */
	{ uint32_t r = alu(ctx, OP_ADD, (ctx->d[3] & 0xFFFFu), 1, 16, 1); set_w(&ctx->d[3], r); } hle_insn(ctx, 4); /* 001470 addqw #1,%d3 */
	{ alu(ctx, OP_SUB, (ctx->d[3] & 0xFFFFu), 0x4u, 16, 0); } hle_insn(ctx, 8); /* 001472 cmpiw #4,%d3 */
	if (COND_GT) { hle_insn(ctx, 10); goto L_0014A2; } hle_insn(ctx, 8);  /* 001476 bgts 14a2 */
	{ uint32_t v = (ctx->d[3] & 0xFFFFu); st16(0xFFFF9F60u, v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 001478 movew %d3,ffff9f60 */
	{ ctx->d[0] = 0x0u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4); /* 00147E moveq #0,%d0 */
	hle_jump(ctx, 0x001480); return;  /* 001480 jsr fbb6c */
	hle_jump(ctx, 0x001486); return;  /* 001486 jsr 86e2 */
L_00148C:
	hle_insn(ctx, 10); goto L_00149E;  /* 00148C bras 149e */
L_00148E:
	{ ctx->d[2] = 0x7u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4); /* 00148E moveq #7,%d2 */
	hle_jump(ctx, 0x001490); return;  /* 001490 bsrw 11ce */
L_001494:
	{ cc_logic8(ctx, (ctx->d[7] & 0xFFu)); } hle_insn(ctx, 4); /* 001494 tstb %d7 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0014A2; } hle_insn(ctx, 8);  /* 001496 beqs 14a2 */
	{ st8(0xFFFFA234u, (1) ? 0xFF : 0); } hle_insn(ctx, 20); /* 001498 st ffffa234 */
L_00149E:
	hle_jump(ctx, 0x00149E); return;  /* 00149E bsrw 10b8 */
L_0014A2:
	hle_jump(ctx, 0x0014A2); return;  /* 0014A2 rts */
L_0014A4:
	{ ctx->d[4] = 0x0u; cc_logic32(ctx, ctx->d[4]); } hle_insn(ctx, 4); /* 0014A4 moveq #0,%d4 */
	{ uint32_t v = ld16(ctx->a[1]); set_w(&ctx->d[4], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 0014A6 movew %a1@,%d4 */
	{ ctx->d[3] = 0x1u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4); /* 0014A8 moveq #1,%d3 */
	hle_jump(ctx, 0x0014AA); return;  /* 0014AA jsr 7a92 */
L_0014B0:
	{ uint32_t v = ld16((ctx->a[1] + (8))); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 12); /* 0014B0 movew %a1@(8),%d3 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0014C6; } hle_insn(ctx, 8);  /* 0014B4 beqs 14c6 */
	{ ctx->a[0] = 0xFFFFA0BAu; } hle_insn(ctx, 12); /* 0014B6 lea ffffa0ba,%a0 */
	{ uint32_t v = 0x4000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 0014BC movew #16384,%d6 */
	hle_jump(ctx, 0x0014C0); return;  /* 0014C0 jsr 7e08 */
L_0014C6:
	hle_jump(ctx, 0x0014C6); return;  /* 0014C6 rts */
L_0014C8:
	{ ctx->a[1] = 0xFFFFAE84u; } hle_insn(ctx, 12); /* 0014C8 lea ffffae84,%a1 */
	{ uint32_t v = ld16(0xFFFF9F60u); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 0014CE movew ffff9f60,%d0 */
	hle_jump(ctx, 0x0014D4); return;  /* 0014D4 muluw #14,%d0 */
L_0014D8:
	{ ctx->a[1] = (ctx->a[1] + (0) + (int16_t)ctx->d[0]); } hle_insn(ctx, 12); /* 0014D8 lea %a1@(0,%d0:w),%a1 */
	hle_jump(ctx, 0x0014DC); return;  /* 0014DC rts */
	hle_jump(ctx, 0x0015EC); return;  /* 0015EC bsrw 4212 */
L_0015F0:
	{ ctx->a[0] = 0x00038E6Eu; } hle_insn(ctx, 12); /* 0015F0 lea 38e6e,%a0 */
	{ uint32_t v = 0x1u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 0015F6 movew #1,%d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFFu); st16(0xFFFF9E38u, v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 0015FA movew %d1,ffff9e38 */
	hle_jump(ctx, 0x001600); return;  /* 001600 jsr 1320a */
L_001606:
	{ ctx->a[0] = 0x0000841Au; } hle_insn(ctx, 8); /* 001606 lea %pc@(841a),%a0 */
	{ ctx->a[1] = 0xFFFFA134u; } hle_insn(ctx, 12); /* 00160A lea ffffa134,%a1 */
	hle_jump(ctx, 0x001610); return;  /* 001610 jsr %pc@(8a7c) */
	hle_jump(ctx, 0x001614); return;  /* 001614 jsr 11d12 */
L_00161A:
	{ ctx->a[0] = 0x0003A4B0u; } hle_insn(ctx, 12); /* 00161A lea 3a4b0,%a0 */
	{ uint32_t v = 0xC392u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 001620 movew #-15470,%d1 */
	{ ctx->d[2] = 0x16u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4); /* 001624 moveq #22,%d2 */
	{ ctx->d[3] = 0xAu; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4); /* 001626 moveq #10,%d3 */
	{ uint32_t v = 0x0u; ctx->d[6] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12); /* 001628 movel #0,%d6 */
	{ uint32_t v = ld16(0xFFFF9E38u); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 00162E movew ffff9e38,%d7 */
	hle_jump(ctx, 0x001634); return;  /* 001634 jsr 122ca */
	hle_jump(ctx, 0x00163A); return;  /* 00163A jsr 11d28 */
	hle_jump(ctx, 0x001640); return;  /* 001640 jsr 11d32 */
L_001646:
	{ uint32_t v = 0xE188u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 001646 movew #-7800,%d3 */
	{ ctx->a[0] = 0x00001728u; } hle_insn(ctx, 8); /* 00164A lea %pc@(1728),%a0 */
	{ uint32_t v = 0xA000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 00164E movew #-24576,%d6 */
L_001652:
	hle_jump(ctx, 0x001652); return;  /* 001652 jsr 11d12 */
	hle_jump(ctx, 0x001658); return;  /* 001658 jsr %pc@(7f00) */
	hle_jump(ctx, 0x00165C); return;  /* 00165C jsr 11d28 */
	hle_jump(ctx, 0x001662); return;  /* 001662 jsr 11d32 */
L_001668:
	{ cc_logic8(ctx, ld8(ctx->a[0])); } hle_insn(ctx, 8); /* 001668 tstb %a0@ */
	if (COND_NE) { hle_insn(ctx, 10); goto L_001652; } hle_insn(ctx, 8);  /* 00166A bnes 1652 */
	{ ctx->a[0] = 0x00001739u; } hle_insn(ctx, 8); /* 00166C lea %pc@(1739),%a0 */
	{ uint32_t v = 0xC000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 001670 movew #-16384,%d6 */
	hle_jump(ctx, 0x001674); return;  /* 001674 jsr 7dee */
L_00167A:
	{ cc_logic16(ctx, ld16(0xFFFFA0A8u)); } hle_insn(ctx, 16); /* 00167A tstw ffffa0a8 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_001686; } hle_insn(ctx, 8);  /* 001680 bnes 1686 */
	{ ctx->a[0] = 0x000017A6u; } hle_insn(ctx, 8); /* 001682 lea %pc@(17a6),%a0 */
L_001686:
	hle_jump(ctx, 0x001686); return;  /* 001686 jsr 7dee */
L_00168C:
	{ st8(0xFFFFA246u, (1) ? 0xFF : 0); } hle_insn(ctx, 20); /* 00168C st ffffa246 */
	hle_jump(ctx, 0x001692); return;  /* 001692 jsr %pc@(8a4c) */
L_001696:
	{ uint32_t v = 0x12Cu; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 001696 movew #300,%d1 */
	hle_jump(ctx, 0x00169A); return;  /* 00169A jsr %pc@(86d8) */
	hle_jump(ctx, 0x00169E); return;  /* 00169E jsr 874e */
L_0016A4:
	{ st16(0xFFFFA0EEu, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 20); /* 0016A4 clrw ffffa0ee */
	{ ctx->a[0] = 0x000017E7u; } hle_insn(ctx, 8); /* 0016AA lea %pc@(17e7),%a0 */
	{ uint32_t v = ctx->a[0]; st32(0xFFFF9ECAu, v); cc_logic32(ctx, v); } hle_insn(ctx, 20); /* 0016AE movel %a0,ffff9eca */
	{ uint32_t v = 0x1Du; st16(0xFFFFA238u, v); cc_logic16(ctx, v); } hle_insn(ctx, 20); /* 0016B4 movew #29,ffffa238 */
L_0016BC:
	{ uint32_t v = ld16(0xFFFFA0EEu); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 0016BC movew ffffa0ee,%d0 */
	{ uint32_t r = ((ctx->d[0] & 0xFFFFu) & 0x1Fu) & 0xFFFFu; cc_logic16(ctx, r); set_w(&ctx->d[0], r); } hle_insn(ctx, 8); /* 0016C2 andiw #31,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0016F2; } hle_insn(ctx, 8);  /* 0016C6 bnes 16f2 */
	{ ctx->a[0] = 0x000019FAu; } hle_insn(ctx, 8); /* 0016C8 lea %pc@(19fa),%a0 */
	hle_jump(ctx, 0x0016CC); return;  /* 0016CC bsrw 1a1e */
L_0016D0:
	{ ctx->a[0] = ld32(0xFFFF9ECAu); } hle_insn(ctx, 20); /* 0016D0 moveal ffff9eca,%a0 */
	hle_jump(ctx, 0x0016D6); return;  /* 0016D6 bsrw 1a1e */
L_0016DA:
	{ uint32_t v = ctx->a[0]; st32(0xFFFF9ECAu, v); cc_logic32(ctx, v); } hle_insn(ctx, 20); /* 0016DA movel %a0,ffff9eca */
	{ uint32_t v = ld16(0xFFFFA238u); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 0016E0 movew ffffa238,%d1 */
	{ uint32_t r = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 2, 16, 1); set_w(&ctx->d[1], r); } hle_insn(ctx, 4); /* 0016E6 addqw #2,%d1 */
	{ uint32_t r = ((ctx->d[1] & 0xFFFFu) & 0x1Fu) & 0xFFFFu; cc_logic16(ctx, r); set_w(&ctx->d[1], r); } hle_insn(ctx, 8); /* 0016E8 andiw #31,%d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFFu); st16(0xFFFFA238u, v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 0016EC movew %d1,ffffa238 */
L_0016F2:
	hle_jump(ctx, 0x0016F2); return;  /* 0016F2 jsr 11d12 */
L_0016F8:
	{ uint32_t v = ld16(0xFFFFA0EEu); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 0016F8 movew ffffa0ee,%d0 */
	{ uint32_t r = ((ctx->d[0] & 0xFFFFu) & 0x1FFu) & 0xFFFFu; cc_logic16(ctx, r); set_w(&ctx->d[0], r); } hle_insn(ctx, 8); /* 0016FE andiw #511,%d0 */
	{ sh_n(ctx, 0, "asr", 1, 16); hle_insn(ctx, 8); } /* 001702 asrw #1,%d0 */
	{ ctx->d[1] = 0x0u; cc_logic32(ctx, ctx->d[1]); } hle_insn(ctx, 4); /* 001704 moveq #0,%d1 */
	hle_jump(ctx, 0x001706); return;  /* 001706 jsr 11f58 */
	hle_jump(ctx, 0x00170C); return;  /* 00170C jsr %pc@(86e2) */
	hle_jump(ctx, 0x001710); return;  /* 001710 jsr 11d28 */
	hle_jump(ctx, 0x001716); return;  /* 001716 jsr 11d32 */
L_00171C:
	{ uint32_t da = 0xFFFFA0EEu; alu(ctx, OP_SUB, ld16(da), 0x6A4u, 16, 0); } hle_insn(ctx, 20); /* 00171C cmpiw #1700,ffffa0ee */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0016BC; } hle_insn(ctx, 8);  /* 001724 bnes 16bc */
	hle_jump(ctx, 0x001726); return;  /* 001726 rts */
L_001A1E:
	{ ctx->d[1] = 0x0u; cc_logic32(ctx, ctx->d[1]); } hle_insn(ctx, 4); /* 001A1E moveq #0,%d1 */
	{ uint32_t v = ld16(0xFFFFA238u); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 001A20 movew ffffa238,%d1 */
	{ ctx->d[0] = 0x4u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4); /* 001A26 moveq #4,%d0 */
	{ uint32_t v = 0xC000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 001A28 movew #-16384,%d6 */
	hle_jump(ctx, 0x001A2C); return;  /* 001A2C braw 7df6 */
	hle_jump(ctx, 0x001A30); return;  /* 001A30 bsrw 1a38 */
L_001A34:
	hle_insn(ctx, 10); goto L_001B72;  /* 001A34 braw 1b72 */
	hle_jump(ctx, 0x001A38); return;  /* 001A38 jsr %pc@(871a) */
	hle_jump(ctx, 0x001A3C); return;  /* 001A3C jsr 121d6 */
L_001A42:
	{ ctx->a[1] = 0x0000576Au; } hle_insn(ctx, 8); /* 001A42 lea %pc@(576a),%a1 */
	hle_jump(ctx, 0x001A46); return;  /* 001A46 jsr 867a */
L_001A4C:
	{ ctx->a[0] = 0x0000827Au; } hle_insn(ctx, 8); /* 001A4C lea %pc@(827a),%a0 */
	{ ctx->a[1] = 0xFFFFA134u; } hle_insn(ctx, 12); /* 001A50 lea ffffa134,%a1 */
	hle_jump(ctx, 0x001A56); return;  /* 001A56 jsr %pc@(8a7c) */
L_001A5A:
	{ ctx->a[0] = 0x000082FAu; } hle_insn(ctx, 8); /* 001A5A lea %pc@(82fa),%a0 */
	hle_jump(ctx, 0x001A5E); return;  /* 001A5E jsr %pc@(8a7c) */
L_001A62:
	{ ctx->a[0] = 0x000083FAu; } hle_insn(ctx, 8); /* 001A62 lea %pc@(83fa),%a0 */
	hle_jump(ctx, 0x001A66); return;  /* 001A66 jsr %pc@(8a7c) */
L_001A6A:
	{ ctx->a[0] = 0x0000843Au; } hle_insn(ctx, 8); /* 001A6A lea %pc@(843a),%a0 */
	hle_jump(ctx, 0x001A6E); return;  /* 001A6E jsr %pc@(8a7c) */
L_001A72:
	{ ctx->a[0] = 0xFFFFA134u; } hle_insn(ctx, 12); /* 001A72 lea ffffa134,%a0 */
	hle_jump(ctx, 0x001A78); return;  /* 001A78 jsr 80fe */
L_001A7E:
	{ ctx->a[0] = 0xFFFFA134u; } hle_insn(ctx, 12); /* 001A7E lea ffffa134,%a0 */
	{ ctx->a[1] = 0xFFFFA1B4u; } hle_insn(ctx, 12); /* 001A84 lea ffffa1b4,%a1 */
	hle_jump(ctx, 0x001A8A); return;  /* 001A8A jsr %pc@(8a7c) */
	hle_jump(ctx, 0x001A8E); return;  /* 001A8E jsr %pc@(8a7c) */
	hle_jump(ctx, 0x001A92); return;  /* 001A92 jsr %pc@(8a7c) */
	hle_jump(ctx, 0x001A96); return;  /* 001A96 jsr %pc@(8a7c) */
L_001A9A:
	{ ctx->a[0] = 0x000C0000u; } hle_insn(ctx, 12); /* 001A9A lea c0000,%a0 */
	{ uint32_t v = 0x1u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 001AA0 movew #1,%d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFFu); st16(0xFFFF9E5Cu, v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 001AA4 movew %d1,ffff9e5c */
	hle_jump(ctx, 0x001AAA); return;  /* 001AAA jsr 1320a */
L_001AB0:
	{ ctx->a[0] = 0x00025DFCu; } hle_insn(ctx, 12); /* 001AB0 lea 25dfc,%a0 */
	{ uint32_t v = 0x670u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 001AB6 movew #1648,%d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFFu); st16(0xFFFF9E2Cu, v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 001ABA movew %d1,ffff9e2c */
	hle_jump(ctx, 0x001AC0); return;  /* 001AC0 jsr 1320a */
L_001AC6:
	{ ctx->a[3] = 0x000C75A2u; } hle_insn(ctx, 12); /* 001AC6 lea c75a2,%a3 */
	hle_jump(ctx, 0x001ACC); return;  /* 001ACC bsrw 1adc */
L_001AD0:
	{ ctx->d[1] = 0x64u; cc_logic32(ctx, ctx->d[1]); } hle_insn(ctx, 4); /* 001AD0 moveq #100,%d1 */
	hle_jump(ctx, 0x001AD2); return;  /* 001AD2 jsr %pc@(86d8) */
L_001AD6:
	{ ctx->a[3] = 0x000C7FA2u; } hle_insn(ctx, 12); /* 001AD6 lea c7fa2,%a3 */
L_001ADC:
	{ uint32_t v = 0x80u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 001ADC movew #128,%d0 */
	hle_jump(ctx, 0x001AE0); return;  /* 001AE0 jsr 13d04 */
L_001AE6:
	{ ctx->d[0] = 0x0u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4); /* 001AE6 moveq #0,%d0 */
L_001AE8:
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); ctx->a[7] -= 2; st16(ctx->a[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 001AE8 movew %d0,%sp@- */
	hle_jump(ctx, 0x001AEA); return;  /* 001AEA jsr 11d12 */
L_001AF0:
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 4); /* 001AF0 movew %d0,%d3 */
	{ uint32_t r = alu(ctx, OP_ADD, (ctx->d[3] & 0xFFFFu), 1, 16, 1); set_w(&ctx->d[3], r); } hle_insn(ctx, 4); /* 001AF2 addqw #1,%d3 */
	{ uint32_t r = alu(ctx, OP_ADD, (ctx->d[3] & 0xFFFFu), (ctx->d[3] & 0xFFFFu), 16, 1); set_w(&ctx->d[3], r); } hle_insn(ctx, 4); /* 001AF4 addw %d3,%d3 */
	{ uint32_t v = (ctx->d[3] & 0xFFFFu); ctx->a[7] -= 2; st16(ctx->a[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 001AF6 movew %d3,%sp@- */
	{ uint32_t v = 0xC000u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 001AF8 movew #-16384,%d1 */
	{ ctx->a[0] = (ctx->a[3] + (960)); } hle_insn(ctx, 8); /* 001AFC lea %a3@(960),%a0 */
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 4); /* 001B00 movew %d0,%d2 */
	hle_jump(ctx, 0x001B02); return;  /* 001B02 muluw #160,%d2 */
L_001B06:
	{ ctx->a[0] -= ctx->d[2]; } hle_insn(ctx, 8); /* 001B06 subal %d2,%a0 */
	{ uint32_t v = 0x28u; set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 001B08 movew #40,%d2 */
	{ uint32_t v = 0x0u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 001B0C movew #0,%d6 */
	{ uint32_t v = ld16(0xFFFF9E5Cu); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 001B10 movew ffff9e5c,%d7 */
	hle_jump(ctx, 0x001B16); return;  /* 001B16 jsr 12234 */
L_001B1C:
	{ uint32_t v = ld16(ctx->a[7]); ctx->a[7] += 2; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 001B1C movew %sp@+,%d3 */
	{ uint32_t v = ld16(ctx->a[7]); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 001B1E movew %sp@,%d0 */
	{ uint32_t v = 0xCD00u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 001B20 movew #-13056,%d1 */
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 4); /* 001B24 movew %d0,%d2 */
	{ sh_n(ctx, 2, "asl", 8, 16); hle_insn(ctx, 22); } /* 001B26 aslw #8,%d2 */
	{ uint32_t r = alu(ctx, OP_SUB, (ctx->d[1] & 0xFFFFu), (ctx->d[2] & 0xFFFFu), 16, 1); set_w(&ctx->d[1], r); } hle_insn(ctx, 4); /* 001B28 subw %d2,%d1 */
	{ ctx->a[0] = (ctx->a[3] + (1120)); } hle_insn(ctx, 8); /* 001B2A lea %a3@(1120),%a0 */
	{ uint32_t v = 0x28u; set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 001B2E movew #40,%d2 */
	{ uint32_t v = 0x0u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 001B32 movew #0,%d6 */
	{ uint32_t v = ld16(0xFFFF9E5Cu); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 001B36 movew ffff9e5c,%d7 */
	hle_jump(ctx, 0x001B3C); return;  /* 001B3C jsr 12234 */
	hle_jump(ctx, 0x001B42); return;  /* 001B42 jsr 11d28 */
	hle_jump(ctx, 0x001B48); return;  /* 001B48 jsr 86e2 */
	hle_jump(ctx, 0x001B4E); return;  /* 001B4E jsr 11d32 */
L_001B54:
	{ uint32_t v = ld16(ctx->a[7]); ctx->a[7] += 2; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 001B54 movew %sp@+,%d0 */
	{ uint32_t r = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), 1, 16, 1); set_w(&ctx->d[0], r); } hle_insn(ctx, 4); /* 001B56 addqw #1,%d0 */
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0x7u, 16, 0); } hle_insn(ctx, 8); /* 001B58 cmpiw #7,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_001AE8; } hle_insn(ctx, 12);  /* 001B5C bnew 1ae8 */
	hle_jump(ctx, 0x001B60); return;  /* 001B60 jsr 86e2 */
L_001B66:
	{ uint32_t v = 0x6Fu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 001B66 movew #111,%d0 */
	hle_jump(ctx, 0x001B6A); return;  /* 001B6A jsr 13d04 */
	hle_jump(ctx, 0x001B70); return;  /* 001B70 rts */
L_001B72:
	{ st16(0xFFFFA0ACu, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 20); /* 001B72 clrw ffffa0ac */
	{ st16(0xFFFFA0EEu, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 20); /* 001B78 clrw ffffa0ee */
L_001B7E:
	{ uint32_t v = 0x4000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 001B7E movew #16384,%d6 */
	{ ctx->a[0] = 0x000012BCu; } hle_insn(ctx, 8); /* 001B82 lea %pc@(12bc),%a0 */
	{ uint32_t da = 0xFFFFA0EFu; uint8_t v = ld8(da); uint32_t m = 1u << (0x4u & 7); cc_set(ctx, CC_Z, (v & m) ? 0 : CC_Z); } hle_insn(ctx, 20); /* 001B86 btst #4,ffffa0ef */
	if (COND_NE) { hle_insn(ctx, 10); goto L_001B94; } hle_insn(ctx, 8);  /* 001B8E bnes 1b94 */
	{ ctx->a[0] = 0x000012CAu; } hle_insn(ctx, 8); /* 001B90 lea %pc@(12ca),%a0 */
L_001B94:
	hle_jump(ctx, 0x001B94); return;  /* 001B94 jsr %pc@(7dee) */
	hle_jump(ctx, 0x001B98); return;  /* 001B98 jsr 86e2 */
L_001B9E:
	{ uint32_t da = 0xFFFFA0EEu; alu(ctx, OP_SUB, ld16(da), 0x258u, 16, 0); } hle_insn(ctx, 20); /* 001B9E cmpiw #600,ffffa0ee */
	if (COND_GE) { hle_insn(ctx, 10); goto L_001BD0; } hle_insn(ctx, 8);  /* 001BA6 bges 1bd0 */
	hle_jump(ctx, 0x001BA8); return;  /* 001BA8 jsr %pc@(88a0) */
	hle_jump(ctx, 0x001BAC); return;  /* 001BAC bsrw c78 */
L_001BB0:
	{ uint32_t v = ld16(0xFFFF8050u); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 001BB0 movew ffff8050,%d0 */
	{ uint32_t v = ld16(0xFFFF8052u); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 001BB6 movew ffff8052,%d1 */
	{ uint32_t m = (1u << (0x7u & 31)); cc_set(ctx, CC_Z, (ctx->d[0] & m) ? 0 : CC_Z); } hle_insn(ctx, 10); /* 001BBC btst #7,%d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_001BCC; } hle_insn(ctx, 8);  /* 001BC0 beqs 1bcc */
	{ uint32_t m = (1u << (0x7u & 31)); cc_set(ctx, CC_Z, (ctx->d[1] & m) ? 0 : CC_Z); } hle_insn(ctx, 10); /* 001BC2 btst #7,%d1 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_001BCC; } hle_insn(ctx, 8);  /* 001BC6 bnes 1bcc */
	{ set_b(&ctx->d[0], 0xFF); hle_insn(ctx, 6); } /* 001BC8 st %d0 */
	hle_jump(ctx, 0x001BCA); return;  /* 001BCA rts */
L_001BCC:
	hle_insn(ctx, 10); goto L_001B7E;  /* 001BCC braw 1b7e */
L_001BD0:
	{ set_b(&ctx->d[0], 0); hle_insn(ctx, 4); } /* 001BD0 sf %d0 */
	hle_jump(ctx, 0x001BD2); return;  /* 001BD2 rts */
L_003EC8:
	{ uint32_t da = 0xFFFFFFF6u; alu(ctx, OP_SUB, ld32(da), 0x5041554Cu, 32, 0); } hle_insn(ctx, 28); /* 003EC8 cmpil #1346458956,fffffff6 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_003EF8; } hle_insn(ctx, 8);  /* 003ED2 beqs 3ef8 */
	{ uint32_t v = 0x5041554Cu; st32(0xFFFFFFF6u, v); cc_logic32(ctx, v); } hle_insn(ctx, 28); /* 003ED4 movel #1346458956,fffffff6 */
	{ ctx->a[0] = 0x00003F3Cu; } hle_insn(ctx, 8); /* 003EDE lea %pc@(3f3c),%a0 */
	{ ctx->a[1] = 0xFFFFFF9Eu; } hle_insn(ctx, 12); /* 003EE2 lea ffffff9e,%a1 */
	{ ctx->d[0] = 0x15u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4); /* 003EE8 moveq #21,%d0 */
L_003EEA:
	{ uint32_t v = ld32(ctx->a[0]); ctx->a[0] += 4; st32(ctx->a[1], v); ctx->a[1] += 4; cc_logic32(ctx, v); } hle_insn(ctx, 20); /* 003EEA movel %a0@+,%a1@+ */
	{ uint16_t c = (uint16_t)(ctx->d[0] - 1); set_w(&ctx->d[0], c); if (c != 0xFFFF) { hle_insn(ctx, 10); goto L_003EEA; } hle_insn(ctx, 14); }  /* 003EEC dbf %d0,3eea */
	{ uint32_t v = 0x4u; st16(0xFFFFFF9Cu, v); cc_logic16(ctx, v); } hle_insn(ctx, 20); /* 003EF0 movew #4,ffffff9c */
L_003EF8:
	{ st32(0xFFFFFFF2u, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 28); /* 003EF8 clrl fffffff2 */
L_003EFE:
	{ ctx->a[0] = 0xFFFFFF9Eu; } hle_insn(ctx, 12); /* 003EFE lea ffffff9e,%a0 */
L_003F04:
	{ ctx->d[0] = 0x0u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4); /* 003F04 moveq #0,%d0 */
	{ ctx->d[3] = 0x0u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4); /* 003F06 moveq #0,%d3 */
	{ ctx->d[7] = 0x9u; cc_logic32(ctx, ctx->d[7]); } hle_insn(ctx, 4); /* 003F08 moveq #9,%d7 */
L_003F0A:
	{ uint32_t v = ld32((ctx->a[0] + (4) + (int16_t)ctx->d[0])); ctx->d[1] = v; cc_logic32(ctx, v); } hle_insn(ctx, 18); /* 003F0A movel %a0@(4,%d0:w),%d1 */
	{ alu(ctx, OP_SUB, ctx->d[1] & 0xFFFFFFFFu, ld32((ctx->a[0] + (12) + (int16_t)ctx->d[0])), 32, 0); } hle_insn(ctx, 20); /* 003F0E cmpl %a0@(c,%d0:w),%d1 */
	if (COND_GE) { hle_insn(ctx, 10); goto L_003F2E; } hle_insn(ctx, 8);  /* 003F12 bges 3f2e */
	{ uint32_t v = ld32((ctx->a[0] + (0) + (int16_t)ctx->d[0])); ctx->d[2] = v; cc_logic32(ctx, v); } hle_insn(ctx, 18); /* 003F14 movel %a0@(0,%d0:w),%d2 */
	{ uint32_t v = ld32((ctx->a[0] + (12) + (int16_t)ctx->d[0])); st32((ctx->a[0] + (4) + (int16_t)ctx->d[0]), v); cc_logic32(ctx, v); } hle_insn(ctx, 32); /* 003F18 movel %a0@(c,%d0:w),%a0@(4,%d0:w) */
	{ uint32_t v = ld32((ctx->a[0] + (8) + (int16_t)ctx->d[0])); st32((ctx->a[0] + (0) + (int16_t)ctx->d[0]), v); cc_logic32(ctx, v); } hle_insn(ctx, 32); /* 003F1E movel %a0@(8,%d0:w),%a0@(0,%d0:w) */
	{ uint32_t v = ctx->d[1]; st32((ctx->a[0] + (12) + (int16_t)ctx->d[0]), v); cc_logic32(ctx, v); } hle_insn(ctx, 18); /* 003F24 movel %d1,%a0@(c,%d0:w) */
	{ uint32_t v = ctx->d[2]; st32((ctx->a[0] + (8) + (int16_t)ctx->d[0]), v); cc_logic32(ctx, v); } hle_insn(ctx, 18); /* 003F28 movel %d2,%a0@(8,%d0:w) */
	{ set_b(&ctx->d[3], 0xFF); hle_insn(ctx, 6); } /* 003F2C st %d3 */
L_003F2E:
	{ uint32_t r = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), 0x8u, 16, 1); set_w(&ctx->d[0], r); } hle_insn(ctx, 8); /* 003F2E addiw #8,%d0 */
	{ uint16_t c = (uint16_t)(ctx->d[7] - 1); set_w(&ctx->d[7], c); if (c != 0xFFFF) { hle_insn(ctx, 10); goto L_003F0A; } hle_insn(ctx, 14); }  /* 003F32 dbf %d7,3f0a */
	{ cc_logic8(ctx, (ctx->d[3] & 0xFFu)); } hle_insn(ctx, 4); /* 003F36 tstb %d3 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_003F04; } hle_insn(ctx, 8);  /* 003F38 bnes 3f04 */
	hle_jump(ctx, 0x003F3A); return;  /* 003F3A rts */
	hle_jump(ctx, 0x003F98); return;  /* 003F98 jsr %pc@(32ac) */
	hle_jump(ctx, 0x003F9C); return;  /* 003F9C jsr %pc@(871a) */
L_003FA0:
	{ ctx->a[0] = 0x0002E808u; } hle_insn(ctx, 12); /* 003FA0 lea 2e808,%a0 */
	{ uint32_t v = 0x1u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 003FA6 movew #1,%d1 */
	hle_jump(ctx, 0x003FAA); return;  /* 003FAA jsr 1320a */
L_003FB0:
	{ ctx->a[0] = 0x0002D406u; } hle_insn(ctx, 12); /* 003FB0 lea 2d406,%a0 */
	{ uint32_t v = 0x400u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 003FB6 movew #1024,%d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFFu); st16(0xFFFF9E48u, v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 003FBA movew %d1,ffff9e48 */
	hle_jump(ctx, 0x003FC0); return;  /* 003FC0 jsr 1320a */
L_003FC6:
	{ uint32_t v = 0xC000u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 003FC6 movew #-16384,%d1 */
	{ ctx->a[0] = 0x00030EEAu; } hle_insn(ctx, 12); /* 003FCA lea 30eea,%a0 */
	{ ctx->d[2] = 0x28u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4); /* 003FD0 moveq #40,%d2 */
	{ ctx->d[3] = 0x18u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4); /* 003FD2 moveq #24,%d3 */
	{ uint32_t v = 0x0u; ctx->d[6] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12); /* 003FD4 movel #0,%d6 */
	{ uint32_t v = 0x1u; set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 003FDA movew #1,%d7 */
	hle_jump(ctx, 0x003FDE); return;  /* 003FDE jsr %pc@(4682) */
L_003FE2:
	{ ctx->d[0] = 0x0u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4); /* 003FE2 moveq #0,%d0 */
	hle_jump(ctx, 0x003FE4); return;  /* 003FE4 jsr %pc@(8808) */
L_003FE8:
	{ ctx->a[0] = 0x0000837Au; } hle_insn(ctx, 8); /* 003FE8 lea %pc@(837a),%a0 */
	{ ctx->a[1] = 0xFFFFA134u; } hle_insn(ctx, 12); /* 003FEC lea ffffa134,%a1 */
	hle_jump(ctx, 0x003FF2); return;  /* 003FF2 jsr %pc@(8a7c) */
L_003FF6:
	{ ctx->a[0] = 0x0000839Au; } hle_insn(ctx, 8); /* 003FF6 lea %pc@(839a),%a0 */
	hle_jump(ctx, 0x003FFA); return;  /* 003FFA jsr %pc@(8a7c) */
L_003FFE:
	{ st8(0xFFFFA246u, (1) ? 0xFF : 0); } hle_insn(ctx, 20); /* 003FFE st ffffa246 */
	hle_jump(ctx, 0x004004); return;  /* 004004 jsr %pc@(8a4c) */
L_004008:
	{ ctx->a[0] = 0x00004087u; } hle_insn(ctx, 8); /* 004008 lea %pc@(4087),%a0 */
	{ uint32_t v = 0xE094u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 00400C movew #-8044,%d3 */
	{ uint32_t v = 0x2000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 004010 movew #8192,%d6 */
	hle_jump(ctx, 0x004014); return;  /* 004014 jsr %pc@(7ed2) */
L_004018:
	{ ctx->d[0] = 0x1u; cc_logic32(ctx, ctx->d[0]); } hle_insn(ctx, 4); /* 004018 moveq #1,%d0 */
	{ ctx->a[4] = 0xFFFFFF9Eu; } hle_insn(ctx, 12); /* 00401A lea ffffff9e,%a4 */
	{ uint32_t v = 0xE206u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 004020 movew #-7674,%d3 */
	{ uint32_t v = 0x2000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 004024 movew #8192,%d6 */
L_004028:
	{ uint32_t v = ctx->d[0]; ctx->a[7] -= 4; st32(ctx->a[7], v); cc_logic32(ctx, v); } hle_insn(ctx, 12); /* 004028 movel %d0,%sp@- */
	hle_jump(ctx, 0x00402A); return;  /* 00402A jsr %pc@(7d5a) */
L_00402E:
	{ ctx->a[0] = 0x00004084u; } hle_insn(ctx, 8); /* 00402E lea %pc@(4084),%a0 */
	hle_jump(ctx, 0x004032); return;  /* 004032 jsr %pc@(7ed2) */
L_004036:
	{ ctx->a[0] = ctx->a[4]; } hle_insn(ctx, 4); /* 004036 moveal %a4,%a0 */
	hle_jump(ctx, 0x004038); return;  /* 004038 jsr %pc@(7ed2) */
L_00403C:
	{ ctx->a[4] = (ctx->a[4] + (4)); } hle_insn(ctx, 8); /* 00403C lea %a4@(4),%a4 */
	{ uint32_t r = alu(ctx, OP_ADD, (ctx->d[3] & 0xFFFFu), 0x8u, 16, 1); set_w(&ctx->d[3], r); } hle_insn(ctx, 8); /* 004040 addiw #8,%d3 */
	{ uint32_t v = ld32(ctx->a[4]); ctx->a[4] += 4; ctx->d[0] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12); /* 004044 movel %a4@+,%d0 */
	hle_jump(ctx, 0x004046); return;  /* 004046 jsr %pc@(7d6e) */
L_00404A:
	{ uint32_t r = ((ctx->d[3] & 0xFFFFu) & 0xFF80u) & 0xFFFFu; cc_logic16(ctx, r); set_w(&ctx->d[3], r); } hle_insn(ctx, 8); /* 00404A andiw #-128,%d3 */
	{ uint32_t r = alu(ctx, OP_ADD, (ctx->d[3] & 0xFFFFu), 0x106u, 16, 1); set_w(&ctx->d[3], r); } hle_insn(ctx, 8); /* 00404E addiw #262,%d3 */
	{ uint32_t v = ld32(ctx->a[7]); ctx->a[7] += 4; ctx->d[0] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12); /* 004052 movel %sp@+,%d0 */
	{ uint32_t r = alu(ctx, OP_ADD, (ctx->d[0] & 0xFFFFu), 1, 16, 1); set_w(&ctx->d[0], r); } hle_insn(ctx, 4); /* 004054 addqw #1,%d0 */
	{ alu(ctx, OP_SUB, (ctx->d[0] & 0xFFFFu), 0xBu, 16, 0); } hle_insn(ctx, 8); /* 004056 cmpiw #11,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_004028; } hle_insn(ctx, 8);  /* 00405A bnes 4028 */
	{ uint32_t v = 0x190u; set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 00405C movew #400,%d2 */
L_004060:
	hle_jump(ctx, 0x004060); return;  /* 004060 jsr %pc@(86e2) */
	hle_jump(ctx, 0x004064); return;  /* 004064 jsr %pc@(88a0) */
L_004068:
	{ uint32_t v = ld16(0xFFFF8050u); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 004068 movew ffff8050,%d0 */
	{ uint32_t m = (1u << (0x7u & 31)); cc_set(ctx, CC_Z, (ctx->d[0] & m) ? 0 : CC_Z); } hle_insn(ctx, 10); /* 00406E btst #7,%d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_00407E; } hle_insn(ctx, 8);  /* 004072 beqs 407e */
	{ uint32_t v = 0xFFu; st8(0xFFFFA234u, v); cc_logic8(ctx, v); } hle_insn(ctx, 20); /* 004074 moveb #-1,ffffa234 */
	hle_jump(ctx, 0x00407C); return;  /* 00407C rts */
L_00407E:
	{ uint16_t c = (uint16_t)(ctx->d[2] - 1); set_w(&ctx->d[2], c); if (c != 0xFFFF) { hle_insn(ctx, 10); goto L_004060; } hle_insn(ctx, 14); }  /* 00407E dbf %d2,4060 */
	hle_jump(ctx, 0x004082); return;  /* 004082 rts */
	hle_jump(ctx, 0x0041E6); return;  /* 0041E6 bsrw 4212 */
L_0041EA:
	{ st8(0xFFFFA246u, (1) ? 0xFF : 0); } hle_insn(ctx, 20); /* 0041EA st ffffa246 */
	hle_jump(ctx, 0x0041F0); return;  /* 0041F0 jsr %pc@(8a4c) */
L_0041F4:
	{ ctx->a[0] = 0x00004093u; } hle_insn(ctx, 8); /* 0041F4 lea %pc@(4093),%a0 */
	hle_jump(ctx, 0x0041F8); return;  /* 0041F8 jsr %pc@(424e) */
	hle_jump(ctx, 0x0041FC); return;  /* 0041FC jsr %pc@(424e) */
	hle_jump(ctx, 0x004200); return;  /* 004200 jsr %pc@(424e) */
	hle_jump(ctx, 0x004204); return;  /* 004204 jsr %pc@(424e) */
	hle_jump(ctx, 0x004208); return;  /* 004208 jsr %pc@(424e) */
	hle_jump(ctx, 0x00420C); return;  /* 00420C jsr %pc@(424e) */
	hle_jump(ctx, 0x004210); return;  /* 004210 rts */
	hle_jump(ctx, 0x004212); return;  /* 004212 jsr %pc@(32ac) */
	hle_jump(ctx, 0x004216); return;  /* 004216 jsr %pc@(871a) */
L_00421A:
	{ ctx->a[0] = 0x0002D406u; } hle_insn(ctx, 12); /* 00421A lea 2d406,%a0 */
	{ uint32_t v = 0x3E8u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 004220 movew #1000,%d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFFu); st16(0xFFFF9E48u, v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 004224 movew %d1,ffff9e48 */
	hle_jump(ctx, 0x00422A); return;  /* 00422A jsr 1320a */
L_004230:
	{ ctx->a[0] = 0x0000831Au; } hle_insn(ctx, 8); /* 004230 lea %pc@(831a),%a0 */
	{ ctx->a[1] = 0xFFFFA134u; } hle_insn(ctx, 12); /* 004234 lea ffffa134,%a1 */
	hle_jump(ctx, 0x00423A); return;  /* 00423A jsr %pc@(8a7c) */
L_00423E:
	{ ctx->a[0] = 0x0000839Au; } hle_insn(ctx, 8); /* 00423E lea %pc@(839a),%a0 */
	hle_jump(ctx, 0x004242); return;  /* 004242 jsr %pc@(8a7c) */
L_004246:
	{ ctx->a[0] = 0x000083FAu; } hle_insn(ctx, 8); /* 004246 lea %pc@(83fa),%a0 */
	hle_jump(ctx, 0x00424A); return;  /* 00424A jmp %pc@(8a7c) */
	hle_jump(ctx, 0x00424E); return;  /* 00424E jsr %pc@(871a) */
L_004252:
	{ uint32_t v = 0xE404u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 004252 movew #-7164,%d3 */
	{ uint32_t v = 0x2000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 004256 movew #8192,%d6 */
L_00425A:
	hle_jump(ctx, 0x00425A); return;  /* 00425A jsr %pc@(88a0) */
L_00425E:
	{ uint32_t v = ld16(0xFFFF8050u); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 00425E movew ffff8050,%d0 */
	{ uint32_t m = (1u << (0x7u & 31)); cc_set(ctx, CC_Z, (ctx->d[0] & m) ? 0 : CC_Z); } hle_insn(ctx, 10); /* 004264 btst #7,%d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_004276; } hle_insn(ctx, 8);  /* 004268 beqs 4276 */
	{ uint32_t v = 0xFFu; st8(0xFFFFA234u, v); cc_logic8(ctx, v); } hle_insn(ctx, 20); /* 00426A moveb #-1,ffffa234 */
	{ ctx->a[7] += 4; } hle_insn(ctx, 8); /* 004272 addql #4,%sp */
	hle_jump(ctx, 0x004274); return;  /* 004274 rts */
L_004276:
	hle_jump(ctx, 0x004276); return;  /* 004276 jsr 11d12 */
	hle_jump(ctx, 0x00427C); return;  /* 00427C jsr %pc@(7f00) */
L_004280:
	{ uint32_t v = 0x78u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 004280 movew #120,%d0 */
	hle_jump(ctx, 0x004284); return;  /* 004284 jsr 13d04 */
	hle_jump(ctx, 0x00428A); return;  /* 00428A jsr 11d28 */
	hle_jump(ctx, 0x004290); return;  /* 004290 jsr %pc@(86e2) */
	hle_jump(ctx, 0x004294); return;  /* 004294 jsr %pc@(86e2) */
	hle_jump(ctx, 0x004298); return;  /* 004298 jsr 11d32 */
L_00429E:
	{ cc_logic8(ctx, ld8(ctx->a[0])); } hle_insn(ctx, 8); /* 00429E tstb %a0@ */
	if (COND_NE) { hle_insn(ctx, 10); goto L_00425A; } hle_insn(ctx, 8);  /* 0042A0 bnes 425a */
	{ ctx->a[0] += 1; } hle_insn(ctx, 8); /* 0042A2 addql #1,%a0 */
	{ uint32_t v = 0x8Cu; set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 0042A4 movew #140,%d2 */
L_0042A8:
	hle_jump(ctx, 0x0042A8); return;  /* 0042A8 jsr %pc@(86e2) */
	hle_jump(ctx, 0x0042AC); return;  /* 0042AC jsr %pc@(88a0) */
L_0042B0:
	{ uint32_t v = ld16(0xFFFF8050u); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 0042B0 movew ffff8050,%d0 */
	{ uint32_t m = (1u << (0x7u & 31)); cc_set(ctx, CC_Z, (ctx->d[0] & m) ? 0 : CC_Z); } hle_insn(ctx, 10); /* 0042B6 btst #7,%d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0042C8; } hle_insn(ctx, 8);  /* 0042BA beqs 42c8 */
	{ uint32_t v = 0xFFu; st8(0xFFFFA234u, v); cc_logic8(ctx, v); } hle_insn(ctx, 20); /* 0042BC moveb #-1,ffffa234 */
	{ ctx->a[7] += 4; } hle_insn(ctx, 8); /* 0042C4 addql #4,%sp */
	hle_jump(ctx, 0x0042C6); return;  /* 0042C6 rts */
L_0042C8:
	{ uint16_t c = (uint16_t)(ctx->d[2] - 1); set_w(&ctx->d[2], c); if (c != 0xFFFF) { hle_insn(ctx, 10); goto L_0042A8; } hle_insn(ctx, 14); }  /* 0042C8 dbf %d2,42a8 */
	hle_jump(ctx, 0x0042CC); return;  /* 0042CC rts */
	hle_jump(ctx, 0x0042CE); return;  /* 0042CE bsrw 32ac */
	hle_jump(ctx, 0x0042D2); return;  /* 0042D2 jsr %pc@(871a) */
L_0042D6:
	{ ctx->a[0] = 0x00031E3Au; } hle_insn(ctx, 12); /* 0042D6 lea 31e3a,%a0 */
	{ uint32_t v = 0x1u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 0042DC movew #1,%d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFFu); st16(0xFFFF9E38u, v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 0042E0 movew %d1,ffff9e38 */
	hle_jump(ctx, 0x0042E6); return;  /* 0042E6 jsr 1320a */
L_0042EC:
	{ ctx->a[0] = 0x0002D406u; } hle_insn(ctx, 12); /* 0042EC lea 2d406,%a0 */
	{ uint32_t v = 0x400u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 0042F2 movew #1024,%d1 */
	{ uint32_t v = (ctx->d[1] & 0xFFFFu); st16(0xFFFF9E48u, v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 0042F6 movew %d1,ffff9e48 */
	hle_jump(ctx, 0x0042FC); return;  /* 0042FC jsr 1320a */
L_004302:
	{ uint32_t v = 0x460Cu; st32(0xFFFF9E98u, v); cc_logic32(ctx, v); } hle_insn(ctx, 28); /* 004302 movel #17932,ffff9e98 */
	{ uint32_t v = 0xFF28u; st16(0xFFFFAA5Eu, v); cc_logic16(ctx, v); } hle_insn(ctx, 20); /* 00430C movew #-216,ffffaa5e */
	{ st16(0xFFFFAA5Cu, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 20); /* 004314 clrw ffffaa5c */
	{ ctx->a[5] = 0x00C00000u; } hle_insn(ctx, 12); /* 00431A lea c00000,%a5 */
	hle_jump(ctx, 0x004320); return;  /* 004320 movew #-29952,%a5@(4) */
	hle_jump(ctx, 0x004326); return;  /* 004326 jsr 11d12 */
	hle_jump(ctx, 0x00432C); return;  /* 00432C jsr %pc@(4752) */
L_004330:
	{ uint32_t v = 0xC41Cu; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 004330 movew #-15332,%d1 */
	{ ctx->a[0] = 0x000316BAu; } hle_insn(ctx, 12); /* 004334 lea 316ba,%a0 */
	{ ctx->d[2] = 0xBu; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4); /* 00433A moveq #11,%d2 */
	{ ctx->d[3] = 0xCu; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4); /* 00433C moveq #12,%d3 */
	hle_jump(ctx, 0x00433E); return;  /* 00433E jsr %pc@(4650) */
	hle_jump(ctx, 0x004342); return;  /* 004342 jsr 11d28 */
	hle_jump(ctx, 0x004348); return;  /* 004348 jsr 11d32 */
L_00434E:
	{ ctx->a[0] = 0x0000831Au; } hle_insn(ctx, 8); /* 00434E lea %pc@(831a),%a0 */
	{ ctx->a[1] = 0xFFFFA134u; } hle_insn(ctx, 12); /* 004352 lea ffffa134,%a1 */
	hle_jump(ctx, 0x004358); return;  /* 004358 jsr %pc@(8a7c) */
L_00435C:
	{ ctx->a[0] = 0x0000839Au; } hle_insn(ctx, 8); /* 00435C lea %pc@(839a),%a0 */
	hle_jump(ctx, 0x004360); return;  /* 004360 jsr %pc@(8a7c) */
L_004364:
	{ st8(0xFFFFA246u, (1) ? 0xFF : 0); } hle_insn(ctx, 20); /* 004364 st ffffa246 */
	hle_jump(ctx, 0x00436A); return;  /* 00436A jsr %pc@(8a4c) */
L_00436E:
	{ uint32_t v = 0x4618u; st32(0xFFFF9E9Cu, v); cc_logic32(ctx, v); } hle_insn(ctx, 28); /* 00436E movel #17944,ffff9e9c */
	{ uint32_t v = 0xE084u; st16(0xFFFF9EA0u, v); cc_logic16(ctx, v); } hle_insn(ctx, 20); /* 004378 movew #-8060,ffff9ea0 */
	{ uint32_t v = 0x460Cu; st32(0xFFFF9E98u, v); cc_logic32(ctx, v); } hle_insn(ctx, 28); /* 004380 movel #17932,ffff9e98 */
	{ ctx->d[7] = 0x0u; cc_logic32(ctx, ctx->d[7]); } hle_insn(ctx, 4); /* 00438A moveq #0,%d7 */
L_00438C:
	hle_jump(ctx, 0x00438C); return;  /* 00438C jsr 11d12 */
L_004392:
	{ uint32_t m = (1u << (0x0u & 31)); cc_set(ctx, CC_Z, (ctx->d[7] & m) ? 0 : CC_Z); } hle_insn(ctx, 10); /* 004392 btst #0,%d7 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0043C6; } hle_insn(ctx, 8);  /* 004396 bnes 43c6 */
	{ ctx->a[0] = ld32(0xFFFF9E9Cu); } hle_insn(ctx, 20); /* 004398 moveal ffff9e9c,%a0 */
	{ uint32_t v = ld16(0xFFFF9EA0u); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 00439E movew ffff9ea0,%d3 */
	{ uint32_t v = 0x2000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 0043A4 movew #8192,%d6 */
	hle_jump(ctx, 0x0043A8); return;  /* 0043A8 jsr %pc@(7f00) */
L_0043AC:
	{ cc_logic8(ctx, ld8(ctx->a[0])); } hle_insn(ctx, 8); /* 0043AC tstb %a0@ */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0043BA; } hle_insn(ctx, 8);  /* 0043AE beqs 43ba */
	{ uint32_t v = 0x78u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 0043B0 movew #120,%d0 */
	hle_jump(ctx, 0x0043B4); return;  /* 0043B4 jsr 13d04 */
L_0043BA:
	{ uint32_t v = ctx->a[0]; st32(0xFFFF9E9Cu, v); cc_logic32(ctx, v); } hle_insn(ctx, 20); /* 0043BA movel %a0,ffff9e9c */
	{ uint32_t v = (ctx->d[3] & 0xFFFFu); st16(0xFFFF9EA0u, v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 0043C0 movew %d3,ffff9ea0 */
L_0043C6:
	{ alu(ctx, OP_SUB, (ctx->d[7] & 0xFFFFu), 0x6Cu, 16, 0); } hle_insn(ctx, 8); /* 0043C6 cmpiw #108,%d7 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0043D6; } hle_insn(ctx, 8);  /* 0043CA bnes 43d6 */
	{ uint32_t v = 0x4A88u; st32(0xFFFF9E98u, v); cc_logic32(ctx, v); } hle_insn(ctx, 28); /* 0043CC movel #19080,ffff9e98 */
L_0043D6:
	{ alu(ctx, OP_SUB, (ctx->d[7] & 0xFFFFu), 0x96u, 16, 0); } hle_insn(ctx, 8); /* 0043D6 cmpiw #150,%d7 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0043F2; } hle_insn(ctx, 8);  /* 0043DA bnes 43f2 */
	hle_jump(ctx, 0x0043DC); return;  /* 0043DC jsr %pc@(4668) */
L_0043E0:
	{ uint32_t v = 0xC420u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 0043E0 movew #-15328,%d1 */
	{ ctx->a[0] = 0x000316D0u; } hle_insn(ctx, 12); /* 0043E4 lea 316d0,%a0 */
	{ ctx->d[2] = 0xDu; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4); /* 0043EA moveq #13,%d2 */
	{ ctx->d[3] = 0xCu; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4); /* 0043EC moveq #12,%d3 */
	hle_jump(ctx, 0x0043EE); return;  /* 0043EE jsr %pc@(4650) */
L_0043F2:
	{ alu(ctx, OP_SUB, (ctx->d[7] & 0xFFFFu), 0x9Bu, 16, 0); } hle_insn(ctx, 8); /* 0043F2 cmpiw #155,%d7 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_00440E; } hle_insn(ctx, 8);  /* 0043F6 bnes 440e */
	hle_jump(ctx, 0x0043F8); return;  /* 0043F8 jsr %pc@(4668) */
L_0043FC:
	{ uint32_t v = 0xC424u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 0043FC movew #-15324,%d1 */
	{ ctx->a[0] = 0x000316EAu; } hle_insn(ctx, 12); /* 004400 lea 316ea,%a0 */
	{ ctx->d[2] = 0xEu; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4); /* 004406 moveq #14,%d2 */
	{ ctx->d[3] = 0xBu; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4); /* 004408 moveq #11,%d3 */
	hle_jump(ctx, 0x00440A); return;  /* 00440A jsr %pc@(4650) */
L_00440E:
	{ alu(ctx, OP_SUB, (ctx->d[7] & 0xFFFFu), 0xA0u, 16, 0); } hle_insn(ctx, 8); /* 00440E cmpiw #160,%d7 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_00442A; } hle_insn(ctx, 8);  /* 004412 bnes 442a */
	hle_jump(ctx, 0x004414); return;  /* 004414 jsr %pc@(4668) */
L_004418:
	{ uint32_t v = 0xC41Cu; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 004418 movew #-15332,%d1 */
	{ ctx->a[0] = 0x00031A7Au; } hle_insn(ctx, 12); /* 00441C lea 31a7a,%a0 */
	{ ctx->d[2] = 0xDu; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4); /* 004422 moveq #13,%d2 */
	{ ctx->d[3] = 0xCu; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4); /* 004424 moveq #12,%d3 */
	hle_jump(ctx, 0x004426); return;  /* 004426 jsr %pc@(4650) */
L_00442A:
	{ alu(ctx, OP_SUB, (ctx->d[7] & 0xFFFFu), 0xDCu, 16, 0); } hle_insn(ctx, 8); /* 00442A cmpiw #220,%d7 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_004486; } hle_insn(ctx, 8);  /* 00442E bnes 4486 */
	{ uint32_t v = (ctx->d[7] & 0xFFFFu); ctx->a[7] -= 2; st16(ctx->a[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 004430 movew %d7,%sp@- */
	{ uint32_t v = 0xC400u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 004432 movew #-15360,%d1 */
	{ ctx->a[0] = 0x00031AA2u; } hle_insn(ctx, 12); /* 004436 lea 31aa2,%a0 */
	{ ctx->d[2] = 0x14u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4); /* 00443C moveq #20,%d2 */
	{ ctx->d[3] = 0xCu; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4); /* 00443E moveq #12,%d3 */
	{ uint32_t v = 0x0u; ctx->d[6] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12); /* 004440 movel #0,%d6 */
	{ uint32_t v = ld16(0xFFFF9E38u); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 004446 movew ffff9e38,%d7 */
	hle_jump(ctx, 0x00444C); return;  /* 00444C jsr 12234 */
L_004452:
	{ uint32_t v = 0xC428u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 004452 movew #-15320,%d1 */
	{ ctx->a[0] = 0x00031AA2u; } hle_insn(ctx, 12); /* 004456 lea 31aa2,%a0 */
	{ ctx->d[2] = 0x14u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4); /* 00445C moveq #20,%d2 */
	{ ctx->d[3] = 0xCu; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4); /* 00445E moveq #12,%d3 */
	{ uint32_t v = 0x0u; ctx->d[6] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12); /* 004460 movel #0,%d6 */
	{ uint32_t v = ld16(0xFFFF9E38u); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 004466 movew ffff9e38,%d7 */
	hle_jump(ctx, 0x00446C); return;  /* 00446C jsr 1227a */
L_004472:
	{ uint32_t v = ld16(ctx->a[7]); ctx->a[7] += 2; set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 004472 movew %sp@+,%d7 */
	{ uint32_t da = 0xFFFF9E9Cu; uint32_t r = alu(ctx, OP_ADD, ld32(da), 0x1u, 32, 1); st32(da, r); } hle_insn(ctx, 36); /* 004474 addil #1,ffff9e9c */
	{ uint32_t v = 0xEB04u; st16(0xFFFF9EA0u, v); cc_logic16(ctx, v); } hle_insn(ctx, 20); /* 00447E movew #-5372,ffff9ea0 */
L_004486:
	{ alu(ctx, OP_SUB, (ctx->d[7] & 0xFFFFu), 0xFCu, 16, 0); } hle_insn(ctx, 8); /* 004486 cmpiw #252,%d7 */
	if (COND_LT) { hle_insn(ctx, 10); goto L_0044BE; } hle_insn(ctx, 8);  /* 00448A blts 44be */
	{ alu(ctx, OP_SUB, (ctx->d[7] & 0xFFFFu), 0x11Cu, 16, 0); } hle_insn(ctx, 8); /* 00448C cmpiw #284,%d7 */
	if (COND_GT) { hle_insn(ctx, 10); goto L_0044BE; } hle_insn(ctx, 8);  /* 004490 bgts 44be */
	{ uint32_t v = (ctx->d[7] & 0xFFFFu); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 4); /* 004492 movew %d7,%d1 */
	{ uint32_t r = ((ctx->d[1] & 0xFFFFu) & 0x3u) & 0xFFFFu; cc_logic16(ctx, r); set_w(&ctx->d[1], r); } hle_insn(ctx, 8); /* 004494 andiw #3,%d1 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0044BE; } hle_insn(ctx, 8);  /* 004498 bnes 44be */
	{ ctx->a[0] = 0x000082BAu; } hle_insn(ctx, 8); /* 00449A lea %pc@(82ba),%a0 */
	{ uint32_t m = (1u << (0x2u & 31)); cc_set(ctx, CC_Z, (ctx->d[7] & m) ? 0 : CC_Z); } hle_insn(ctx, 10); /* 00449E btst #2,%d7 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_0044A8; } hle_insn(ctx, 8);  /* 0044A2 beqs 44a8 */
	{ ctx->a[0] = 0x0000831Au; } hle_insn(ctx, 8); /* 0044A4 lea %pc@(831a),%a0 */
L_0044A8:
	{ ctx->a[5] = ld32(0xFFFF80BCu); } hle_insn(ctx, 20); /* 0044A8 moveal ffff80bc,%a5 */
	{ uint32_t v = 0x4u; st16(ctx->a[5], v); ctx->a[5] += 2; cc_logic16(ctx, v); } hle_insn(ctx, 12); /* 0044AE movew #4,%a5@+ */
	{ uint32_t v = 0x0u; st16(ctx->a[5], v); ctx->a[5] += 2; cc_logic16(ctx, v); } hle_insn(ctx, 12); /* 0044B2 movew #0,%a5@+ */
	{ uint32_t v = ctx->a[0]; st32(ctx->a[5], v); ctx->a[5] += 4; cc_logic32(ctx, v); } hle_insn(ctx, 12); /* 0044B6 movel %a0,%a5@+ */
	{ uint32_t v = ctx->a[5]; st32(0xFFFF80BCu, v); cc_logic32(ctx, v); } hle_insn(ctx, 20); /* 0044B8 movel %a5,ffff80bc */
L_0044BE:
	{ alu(ctx, OP_SUB, (ctx->d[7] & 0xFFFFu), 0x13Cu, 16, 0); } hle_insn(ctx, 8); /* 0044BE cmpiw #316,%d7 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0044E8; } hle_insn(ctx, 8);  /* 0044C2 bnes 44e8 */
	{ uint32_t v = 0xE510u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 0044C4 movew #-6896,%d1 */
	{ ctx->a[0] = 0x00031A94u; } hle_insn(ctx, 12); /* 0044C8 lea 31a94,%a0 */
	{ ctx->d[2] = 0x5u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4); /* 0044CE moveq #5,%d2 */
	{ ctx->d[3] = 0x5u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4); /* 0044D0 moveq #5,%d3 */
	hle_jump(ctx, 0x0044D2); return;  /* 0044D2 jsr %pc@(4650) */
L_0044D6:
	{ uint32_t v = 0xE536u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 0044D6 movew #-6858,%d1 */
	{ ctx->a[0] = 0x00031A94u; } hle_insn(ctx, 12); /* 0044DA lea 31a94,%a0 */
	{ ctx->d[2] = 0x5u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4); /* 0044E0 moveq #5,%d2 */
	{ ctx->d[3] = 0x5u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4); /* 0044E2 moveq #5,%d3 */
	hle_jump(ctx, 0x0044E4); return;  /* 0044E4 jsr %pc@(4650) */
L_0044E8:
	{ alu(ctx, OP_SUB, (ctx->d[7] & 0xFFFFu), 0x11Eu, 16, 0); } hle_insn(ctx, 8); /* 0044E8 cmpiw #286,%d7 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_004512; } hle_insn(ctx, 8);  /* 0044EC bnes 4512 */
	{ uint32_t v = 0xE614u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 0044EE movew #-6636,%d1 */
	{ ctx->a[0] = 0x00031C24u; } hle_insn(ctx, 12); /* 0044F2 lea 31c24,%a0 */
	{ ctx->d[2] = 0x2u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4); /* 0044F8 moveq #2,%d2 */
	{ ctx->d[3] = 0x2u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4); /* 0044FA moveq #2,%d3 */
	hle_jump(ctx, 0x0044FC); return;  /* 0044FC jsr %pc@(4650) */
L_004500:
	{ uint32_t v = 0xE63Au; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 004500 movew #-6598,%d1 */
	{ ctx->a[0] = 0x00031C24u; } hle_insn(ctx, 12); /* 004504 lea 31c24,%a0 */
	{ ctx->d[2] = 0x2u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4); /* 00450A moveq #2,%d2 */
	{ ctx->d[3] = 0x2u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4); /* 00450C moveq #2,%d3 */
	hle_jump(ctx, 0x00450E); return;  /* 00450E jsr %pc@(4650) */
L_004512:
	{ alu(ctx, OP_SUB, (ctx->d[7] & 0xFFFFu), 0x19Au, 16, 0); } hle_insn(ctx, 8); /* 004512 cmpiw #410,%d7 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_00456A; } hle_insn(ctx, 8);  /* 004516 bnes 456a */
	hle_jump(ctx, 0x004518); return;  /* 004518 jsr %pc@(4668) */
L_00451C:
	{ ctx->a[5] = ld32(0xFFFF80BCu); } hle_insn(ctx, 20); /* 00451C moveal ffff80bc,%a5 */
	{ uint32_t v = 0x2u; st16(ctx->a[5], v); ctx->a[5] += 2; cc_logic16(ctx, v); } hle_insn(ctx, 12); /* 004522 movew #2,%a5@+ */
	{ uint32_t v = 0x400u; st16(ctx->a[5], v); ctx->a[5] += 2; cc_logic16(ctx, v); } hle_insn(ctx, 12); /* 004526 movew #1024,%a5@+ */
	{ uint32_t v = 0xE500u; st16(ctx->a[5], v); ctx->a[5] += 2; cc_logic16(ctx, v); } hle_insn(ctx, 12); /* 00452A movew #-6912,%a5@+ */
	{ uint32_t v = ctx->a[5]; st32(0xFFFF80BCu, v); cc_logic32(ctx, v); } hle_insn(ctx, 20); /* 00452E movel %a5,ffff80bc */
	{ uint32_t v = 0xC41Cu; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 004534 movew #-15332,%d1 */
	{ ctx->a[0] = 0x00031A7Au; } hle_insn(ctx, 12); /* 004538 lea 31a7a,%a0 */
	{ ctx->d[2] = 0xDu; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4); /* 00453E moveq #13,%d2 */
	{ ctx->d[3] = 0xCu; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4); /* 004540 moveq #12,%d3 */
	hle_jump(ctx, 0x004542); return;  /* 004542 jsr %pc@(4650) */
L_004546:
	{ uint32_t v = 0xC622u; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 004546 movew #-14814,%d1 */
	{ ctx->a[0] = 0x00031CCCu; } hle_insn(ctx, 12); /* 00454A lea 31ccc,%a0 */
	{ ctx->d[2] = 0x2u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4); /* 004550 moveq #2,%d2 */
	{ ctx->d[3] = 0x2u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4); /* 004552 moveq #2,%d3 */
	hle_jump(ctx, 0x004554); return;  /* 004554 jsr %pc@(4650) */
L_004558:
	{ uint32_t v = 0xC62Eu; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 004558 movew #-14802,%d1 */
	{ ctx->a[0] = 0x00031CC6u; } hle_insn(ctx, 12); /* 00455C lea 31cc6,%a0 */
	{ ctx->d[2] = 0x2u; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4); /* 004562 moveq #2,%d2 */
	{ ctx->d[3] = 0x2u; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4); /* 004564 moveq #2,%d3 */
	hle_jump(ctx, 0x004566); return;  /* 004566 jsr %pc@(4650) */
L_00456A:
	{ alu(ctx, OP_SUB, (ctx->d[7] & 0xFFFFu), 0x1C2u, 16, 0); } hle_insn(ctx, 8); /* 00456A cmpiw #450,%d7 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_00458C; } hle_insn(ctx, 8);  /* 00456E bnes 458c */
	{ uint32_t v = 0x460Cu; st32(0xFFFF9E98u, v); cc_logic32(ctx, v); } hle_insn(ctx, 28); /* 004570 movel #17932,ffff9e98 */
	{ uint32_t v = 0xC45Eu; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 00457A movew #-15266,%d1 */
	{ ctx->a[0] = 0x000316BAu; } hle_insn(ctx, 12); /* 00457E lea 316ba,%a0 */
	{ ctx->d[2] = 0xBu; cc_logic32(ctx, ctx->d[2]); } hle_insn(ctx, 4); /* 004584 moveq #11,%d2 */
	{ ctx->d[3] = 0xCu; cc_logic32(ctx, ctx->d[3]); } hle_insn(ctx, 4); /* 004586 moveq #12,%d3 */
	hle_jump(ctx, 0x004588); return;  /* 004588 jsr %pc@(4650) */
L_00458C:
	{ alu(ctx, OP_SUB, (ctx->d[7] & 0xFFFFu), 0x11Au, 16, 0); } hle_insn(ctx, 8); /* 00458C cmpiw #282,%d7 */
	if (COND_LT) { hle_insn(ctx, 10); goto L_0045D0; } hle_insn(ctx, 8);  /* 004590 blts 45d0 */
	{ uint32_t v = (ctx->d[7] & 0xFFFFu); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 4); /* 004592 movew %d7,%d1 */
	{ uint32_t r = ((ctx->d[1] & 0xFFFFu) & 0x1Fu) & 0xFFFFu; cc_logic16(ctx, r); set_w(&ctx->d[1], r); } hle_insn(ctx, 8); /* 004594 andiw #31,%d1 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_0045D0; } hle_insn(ctx, 8);  /* 004598 bnes 45d0 */
	{ ctx->a[5] = ld32(0xFFFF80BCu); } hle_insn(ctx, 20); /* 00459A moveal ffff80bc,%a5 */
	{ uint32_t v = 0x2u; st16(ctx->a[5], v); ctx->a[5] += 2; cc_logic16(ctx, v); } hle_insn(ctx, 12); /* 0045A0 movew #2,%a5@+ */
	{ uint32_t v = 0x24u; st16(ctx->a[5], v); ctx->a[5] += 2; cc_logic16(ctx, v); } hle_insn(ctx, 12); /* 0045A4 movew #36,%a5@+ */
	{ uint32_t v = 0xEB22u; st16(ctx->a[5], v); ctx->a[5] += 2; cc_logic16(ctx, v); } hle_insn(ctx, 12); /* 0045A8 movew #-5342,%a5@+ */
	{ uint32_t v = 0x2u; st16(ctx->a[5], v); ctx->a[5] += 2; cc_logic16(ctx, v); } hle_insn(ctx, 12); /* 0045AC movew #2,%a5@+ */
	{ uint32_t v = 0x24u; st16(ctx->a[5], v); ctx->a[5] += 2; cc_logic16(ctx, v); } hle_insn(ctx, 12); /* 0045B0 movew #36,%a5@+ */
	{ uint32_t v = 0xEBA2u; st16(ctx->a[5], v); ctx->a[5] += 2; cc_logic16(ctx, v); } hle_insn(ctx, 12); /* 0045B4 movew #-5214,%a5@+ */
	{ uint32_t v = ctx->a[5]; st32(0xFFFF80BCu, v); cc_logic32(ctx, v); } hle_insn(ctx, 20); /* 0045B8 movel %a5,ffff80bc */
	{ uint32_t v = 0x4646u; st32(0xFFFF9E9Cu, v); cc_logic32(ctx, v); } hle_insn(ctx, 28); /* 0045BE movel #17990,ffff9e9c */
	{ uint32_t v = 0xEB22u; st16(0xFFFF9EA0u, v); cc_logic16(ctx, v); } hle_insn(ctx, 20); /* 0045C8 movew #-5342,ffff9ea0 */
L_0045D0:
	hle_jump(ctx, 0x0045D0); return;  /* 0045D0 jsr %pc@(4752) */
	hle_jump(ctx, 0x0045D4); return;  /* 0045D4 jsr 11d28 */
	hle_jump(ctx, 0x0045DA); return;  /* 0045DA jsr 86e2 */
	hle_jump(ctx, 0x0045E0); return;  /* 0045E0 jsr 11d32 */
	hle_jump(ctx, 0x0045E6); return;  /* 0045E6 jsr %pc@(88a0) */
L_0045EA:
	{ uint32_t v = ld16(0xFFFF8050u); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 0045EA movew ffff8050,%d0 */
	{ uint32_t m = (1u << (0x7u & 31)); cc_set(ctx, CC_Z, (ctx->d[0] & m) ? 0 : CC_Z); } hle_insn(ctx, 10); /* 0045F0 btst #7,%d0 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_004600; } hle_insn(ctx, 8);  /* 0045F4 beqs 4600 */
	{ uint32_t v = 0xFFu; st8(0xFFFFA234u, v); cc_logic8(ctx, v); } hle_insn(ctx, 20); /* 0045F6 moveb #-1,ffffa234 */
	hle_jump(ctx, 0x0045FE); return;  /* 0045FE rts */
L_004600:
	{ uint32_t r = alu(ctx, OP_ADD, (ctx->d[7] & 0xFFFFu), 1, 16, 1); set_w(&ctx->d[7], r); } hle_insn(ctx, 4); /* 004600 addqw #1,%d7 */
	{ alu(ctx, OP_SUB, (ctx->d[7] & 0xFFFFu), 0x226u, 16, 0); } hle_insn(ctx, 8); /* 004602 cmpiw #550,%d7 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_00438C; } hle_insn(ctx, 12);  /* 004606 bnew 438c */
	hle_jump(ctx, 0x00460A); return;  /* 00460A rts */
L_004650:
	{ uint32_t v = (ctx->d[7] & 0xFFFFu); ctx->a[7] -= 2; st16(ctx->a[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 004650 movew %d7,%sp@- */
	{ uint32_t v = 0x0u; ctx->d[6] = v; cc_logic32(ctx, v); } hle_insn(ctx, 12); /* 004652 movel #0,%d6 */
	{ uint32_t v = ld16(0xFFFF9E38u); set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 004658 movew ffff9e38,%d7 */
	hle_jump(ctx, 0x00465E); return;  /* 00465E jsr 12234 */
L_004664:
	{ uint32_t v = ld16(ctx->a[7]); ctx->a[7] += 2; set_w(&ctx->d[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 004664 movew %sp@+,%d7 */
	hle_jump(ctx, 0x004666); return;  /* 004666 rts */
L_004668:
	{ ctx->a[5] = ld32(0xFFFF80BCu); } hle_insn(ctx, 20); /* 004668 moveal ffff80bc,%a5 */
	{ uint32_t v = 0x2u; st16(ctx->a[5], v); ctx->a[5] += 2; cc_logic16(ctx, v); } hle_insn(ctx, 12); /* 00466E movew #2,%a5@+ */
	{ uint32_t v = 0x600u; st16(ctx->a[5], v); ctx->a[5] += 2; cc_logic16(ctx, v); } hle_insn(ctx, 12); /* 004672 movew #1536,%a5@+ */
	{ uint32_t v = 0xC400u; st16(ctx->a[5], v); ctx->a[5] += 2; cc_logic16(ctx, v); } hle_insn(ctx, 12); /* 004676 movew #-15360,%a5@+ */
	{ uint32_t v = ctx->a[5]; st32(0xFFFF80BCu, v); cc_logic32(ctx, v); } hle_insn(ctx, 20); /* 00467A movel %a5,ffff80bc */
	hle_jump(ctx, 0x004680); return;  /* 004680 rts */
L_006F64:
	{ ctx->a[3] = 0x00006F70u; } hle_insn(ctx, 8); /* 006F64 lea %pc@(6f70),%a3 */
	{ ctx->a[2] = 0x00006F8Cu; } hle_insn(ctx, 8); /* 006F68 lea %pc@(6f8c),%a2 */
	hle_jump(ctx, 0x006F6C); return;  /* 006F6C jmp %pc@(5940) */
L_006F78:
	{ ctx->a[3] = 0x00006F84u; } hle_insn(ctx, 8); /* 006F78 lea %pc@(6f84),%a3 */
	{ ctx->a[2] = 0x00006F8Cu; } hle_insn(ctx, 8); /* 006F7C lea %pc@(6f8c),%a2 */
	hle_jump(ctx, 0x006F80); return;  /* 006F80 jmp %pc@(5940) */
L_006F8C:
	{ uint32_t v = ld16((ctx->a[6] + (38))); set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 12); /* 006F8C movew %fp@(38),%d1 */
	{ uint32_t v = ld16((ctx->a[4] + (54))); set_w(&ctx->d[2], v); cc_logic16(ctx, v); } hle_insn(ctx, 12); /* 006F90 movew %a4@(54),%d2 */
	{ alu(ctx, OP_SUB, (ctx->d[1] & 0xFFFFu), 0x1Au, 16, 0); } hle_insn(ctx, 8); /* 006F94 cmpiw #26,%d1 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_006FA0; } hle_insn(ctx, 8);  /* 006F98 bnes 6fa0 */
	{ uint32_t v = 0xFFDFu; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 006F9A movew #-33,%d1 */
	hle_insn(ctx, 10); goto L_006FEC;  /* 006F9E bras 6fec */
L_006FA0:
	if (COND_LT) { hle_insn(ctx, 10); goto L_006FEC; } hle_insn(ctx, 8);  /* 006FA0 blts 6fec */
	{ alu(ctx, OP_SUB, (ctx->d[1] & 0xFFFFu), 0x1Cu, 16, 0); } hle_insn(ctx, 8); /* 006FA2 cmpiw #28,%d1 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_006FDC; } hle_insn(ctx, 8);  /* 006FA6 bnes 6fdc */
	{ alu(ctx, OP_SUB, ctx->a[4], 0xFFFF8000u, 32, 0); } hle_insn(ctx, 14); /* 006FA8 cmpal #-32768,%a4 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_006FBC; } hle_insn(ctx, 8);  /* 006FAE bnes 6fbc */
	{ uint32_t v = ld32(0xFFFF9EA8u); st32(0xFFFFA0B0u, v); cc_logic32(ctx, v); } hle_insn(ctx, 36); /* 006FB0 movel ffff9ea8,ffffa0b0 */
	hle_insn(ctx, 10); goto L_006FC6;  /* 006FBA bras 6fc6 */
L_006FBC:
	{ uint32_t v = ld32(0xFFFF9EB0u); st32(0xFFFFA0B4u, v); cc_logic32(ctx, v); } hle_insn(ctx, 36); /* 006FBC movel ffff9eb0,ffffa0b4 */
L_006FC6:
	{ uint32_t v = 0xFFFFu; st16((ctx->a[4] + (36)), v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 006FC6 movew #-1,%a4@(36) */
	{ uint32_t da = 0xFFFF9F6Du; uint32_t r = alu(ctx, OP_SUB, ld8(da), 1, 8, 1); st8(da, r); } hle_insn(ctx, 20); /* 006FCC subqb #1,ffff9f6d */
	if (COND_NE) { hle_insn(ctx, 10); goto L_006FDA; } hle_insn(ctx, 8);  /* 006FD2 bnes 6fda */
	{ st8(0xFFFFA234u, (1) ? 0xFF : 0); } hle_insn(ctx, 20); /* 006FD4 st ffffa234 */
L_006FDA:
	hle_insn(ctx, 10); goto L_007004;  /* 006FDA bras 7004 */
L_006FDC:
	{ uint32_t r = alu(ctx, OP_SUB, (ctx->d[2] & 0xFFFFu), 1, 16, 1); set_w(&ctx->d[2], r); } hle_insn(ctx, 4); /* 006FDC subqw #1,%d2 */
	if (COND_MI) { hle_insn(ctx, 10); goto L_007004; } hle_insn(ctx, 8);  /* 006FDE bmis 7004 */
	{ ctx->a[1] = (ctx->a[4] + (50)); } hle_insn(ctx, 8); /* 006FE0 lea %a4@(50),%a1 */
	{ uint32_t v = 0x2Eu; st8((ctx->a[1] + (0) + (int16_t)ctx->d[2]), v); cc_logic8(ctx, v); } hle_insn(ctx, 18); /* 006FE4 moveb #46,%a1@(0,%d2:w) */
	hle_insn(ctx, 10); goto L_007000;  /* 006FEA bras 7000 */
L_006FEC:
	{ alu(ctx, OP_SUB, (ctx->d[2] & 0xFFFFu), 0x3u, 16, 0); } hle_insn(ctx, 8); /* 006FEC cmpiw #3,%d2 */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_007004; } hle_insn(ctx, 8);  /* 006FF0 beqs 7004 */
	{ uint32_t r = alu(ctx, OP_ADD, (ctx->d[1] & 0xFFFFu), 0x41u, 16, 1); set_w(&ctx->d[1], r); } hle_insn(ctx, 8); /* 006FF2 addiw #65,%d1 */
	{ ctx->a[1] = (ctx->a[4] + (50)); } hle_insn(ctx, 8); /* 006FF6 lea %a4@(50),%a1 */
	{ uint32_t v = (ctx->d[1] & 0xFFu); st8((ctx->a[1] + (0) + (int16_t)ctx->d[2]), v); cc_logic8(ctx, v); } hle_insn(ctx, 14); /* 006FFA moveb %d1,%a1@(0,%d2:w) */
	{ uint32_t r = alu(ctx, OP_ADD, (ctx->d[2] & 0xFFFFu), 1, 16, 1); set_w(&ctx->d[2], r); } hle_insn(ctx, 4); /* 006FFE addqw #1,%d2 */
L_007000:
	{ uint32_t v = (ctx->d[2] & 0xFFFFu); st16((ctx->a[4] + (54)), v); cc_logic16(ctx, v); } hle_insn(ctx, 12); /* 007000 movew %d2,%a4@(54) */
L_007004:
	{ ctx->a[0] = 0x0000700Au; } hle_insn(ctx, 8); /* 007004 lea %pc@(700a),%a0 */
	hle_jump(ctx, 0x007008); return;  /* 007008 rts */
L_008D68:
	{ cc_logic8(ctx, ld8(0xFFFF80CAu)); } hle_insn(ctx, 16); /* 008D68 tstb ffff80ca */
	if (COND_GT) { hle_insn(ctx, 10); goto L_008D7C; } hle_insn(ctx, 8);  /* 008D6E bgts 8d7c */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_008E68; } hle_insn(ctx, 12);  /* 008D70 beqw 8e68 */
	{ st8(0xFFFFA234u, (1) ? 0xFF : 0); } hle_insn(ctx, 20); /* 008D74 st ffffa234 */
	hle_jump(ctx, 0x008D7A); return;  /* 008D7A rts */
L_008D7C:
	{ uint32_t da = 0xFFFF9F6Du; alu(ctx, OP_SUB, ld8(da), 0x1u, 8, 0); } hle_insn(ctx, 20); /* 008D7C cmpib #1,ffff9f6d */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_008DFE; } hle_insn(ctx, 8);  /* 008D84 beqs 8dfe */
	{ uint32_t v = 0x17Fu; st16((ctx->a[4] + (32)), v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 008D86 movew #383,%a4@(32) */
	{ uint32_t v = 0x7u; st16((ctx->a[4] + (34)), v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 008D8C movew #7,%a4@(34) */
	hle_jump(ctx, 0x008D92); return;  /* 008D92 bsrw 7aea */
L_008D96:
	{ uint32_t v = ld16(0xFFFF80C0u); st16((ctx->a[4] + (78)), v); cc_logic16(ctx, v); } hle_insn(ctx, 24); /* 008D96 movew ffff80c0,%a4@(78) */
	hle_insn(ctx, 10); goto L_008DFC;  /* 008D9E braw 8dfc */
L_008DA2:
	hle_jump(ctx, 0x008DA2); return;  /* 008DA2 jsr %pc@(7c2c) */
	hle_jump(ctx, 0x008DA6); return;  /* 008DA6 jsr %pc@(7c54) */
	hle_jump(ctx, 0x008DAA); return;  /* 008DAA bsrw 8c0c */
L_008DAE:
	{ uint32_t da = (ctx->a[4] + (36)); uint32_t r = ~ld16(da); st16(da, r); cc_logic16(ctx, r); } hle_insn(ctx, 16); /* 008DAE notw %a4@(36) */
	{ uint32_t base = ctx->a[7] - 60; st32(base + 0, ctx->d[0]); st32(base + 4, ctx->d[1]); st32(base + 8, ctx->d[2]); st32(base + 12, ctx->d[3]); st32(base + 16, ctx->d[4]); st32(base + 20, ctx->d[5]); st32(base + 24, ctx->d[6]); st32(base + 28, ctx->d[7]); st32(base + 32, ctx->a[0]); st32(base + 36, ctx->a[1]); st32(base + 40, ctx->a[2]); st32(base + 44, ctx->a[3]); st32(base + 48, ctx->a[4]); st32(base + 52, ctx->a[5]); st32(base + 56, ctx->a[6]); ctx->a[7] = base; } hle_movem_l(ctx, 128, 15); /* 008DB2 moveml %d0-%fp,%sp@- */
	hle_jump(ctx, 0x008DB6); return;  /* 008DB6 jsr 13e1a */
L_008DBC:
	{ uint32_t base = ctx->a[7]; ctx->d[0] = ld32(base + 0); ctx->d[1] = ld32(base + 4); ctx->d[2] = ld32(base + 8); ctx->d[3] = ld32(base + 12); ctx->d[4] = ld32(base + 16); ctx->d[5] = ld32(base + 20); ctx->d[6] = ld32(base + 24); ctx->d[7] = ld32(base + 28); ctx->a[0] = ld32(base + 32); ctx->a[1] = ld32(base + 36); ctx->a[2] = ld32(base + 40); ctx->a[3] = ld32(base + 44); ctx->a[4] = ld32(base + 48); ctx->a[5] = ld32(base + 52); ctx->a[6] = ld32(base + 56); ctx->a[7] = base + 60; } hle_movem_l(ctx, 132, 15); /* 008DBC moveml %sp@+,%d0-%fp */
	{ uint32_t da = (ctx->a[4] + (34)); alu(ctx, OP_SUB, ld16(da), 0x3u, 16, 0); } hle_insn(ctx, 16); /* 008DC0 cmpiw #3,%a4@(34) */
	if (COND_NE) { hle_insn(ctx, 10); goto L_008DD2; } hle_insn(ctx, 8);  /* 008DC6 bnes 8dd2 */
	{ uint32_t v = 0x80u; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 008DC8 movew #128,%d0 */
	hle_jump(ctx, 0x008DCC); return;  /* 008DCC jsr 13d04 */
L_008DD2:
	{ uint32_t da = (ctx->a[4] + (34)); alu(ctx, OP_SUB, ld16(da), 0x4u, 16, 0); } hle_insn(ctx, 16); /* 008DD2 cmpiw #4,%a4@(34) */
	if (COND_NE) { hle_insn(ctx, 10); goto L_008DE2; } hle_insn(ctx, 8);  /* 008DD8 bnes 8de2 */
	{ uint32_t v = 0xFFFFu; st16((ctx->a[4] + (32)), v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 008DDA movew #-1,%a4@(32) */
	hle_insn(ctx, 10); goto L_008DFC;  /* 008DE0 bras 8dfc */
L_008DE2:
	{ uint32_t da = (ctx->a[4] + (34)); alu(ctx, OP_SUB, ld16(da), 0x5u, 16, 0); } hle_insn(ctx, 16); /* 008DE2 cmpiw #5,%a4@(34) */
	if (COND_NE) { hle_insn(ctx, 10); goto L_008DF4; } hle_insn(ctx, 8);  /* 008DE8 bnes 8df4 */
	{ uint32_t v = 0xFFu; st8(0xFFFFA234u, v); cc_logic8(ctx, v); } hle_insn(ctx, 20); /* 008DEA moveb #-1,ffffa234 */
	hle_insn(ctx, 10); goto L_008DFC;  /* 008DF2 bras 8dfc */
L_008DF4:
	{ st16((ctx->a[4] + (34)), 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 16); /* 008DF4 clrw %a4@(34) */
	hle_jump(ctx, 0x008DF8); return;  /* 008DF8 jsr %pc@(2b42) */
L_008DFC:
	hle_jump(ctx, 0x008DFC); return;  /* 008DFC rts */
L_008DFE:
	hle_jump(ctx, 0x008DFE); return;  /* 008DFE bsrw 7aea */
L_008E02:
	{ uint32_t v = 0x17Fu; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 008E02 movew #383,%d0 */
	{ uint32_t v = ld16(0xFFFFA0EEu); st16(0xFFFFA238u, v); cc_logic16(ctx, v); } hle_insn(ctx, 28); /* 008E06 movew ffffa0ee,ffffa238 */
L_008E10:
	{ uint32_t v = (ctx->d[0] & 0xFFFFu); ctx->a[7] -= 2; st16(ctx->a[7], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 008E10 movew %d0,%sp@- */
	{ uint32_t v = ld16(0xFFFF80C0u); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 008E12 movew ffff80c0,%d3 */
	{ uint32_t r = alu(ctx, OP_ADD, (ctx->d[3] & 0xFFFFu), 0x1Au, 16, 1); set_w(&ctx->d[3], r); } hle_insn(ctx, 8); /* 008E18 addiw #26,%d3 */
	{ sh_n(ctx, 0, "asr", 3, 16); hle_insn(ctx, 12); } /* 008E1C asrw #3,%d0 */
	{ uint32_t r = ((ctx->d[0] & 0xFFFFu) & 0xFFF8u) & 0xFFFFu; cc_logic16(ctx, r); set_w(&ctx->d[0], r); } hle_insn(ctx, 8); /* 008E1E andiw #-8,%d0 */
	{ ctx->a[0] = 0x00008EDAu; } hle_insn(ctx, 8); /* 008E22 lea %pc@(8eda),%a0 */
	{ ctx->a[0] = (ctx->a[0] + (0) + (int16_t)ctx->d[0]); } hle_insn(ctx, 12); /* 008E26 lea %a0@(0,%d0:w),%a0 */
	hle_jump(ctx, 0x008E2A); return;  /* 008E2A jsr %pc@(7e08) */
	hle_jump(ctx, 0x008E2E); return;  /* 008E2E jsr 86e2 */
L_008E34:
	{ uint32_t v = ctx->a[4]; ctx->a[7] -= 4; st32(ctx->a[7], v); cc_logic32(ctx, v); } hle_insn(ctx, 12); /* 008E34 movel %a4,%sp@- */
	hle_jump(ctx, 0x008E36); return;  /* 008E36 jsr 3de */
L_008E3C:
	{ ctx->a[4] = ld32(ctx->a[7]); ctx->a[7] += 4; } hle_insn(ctx, 12); /* 008E3C moveal %sp@+,%a4 */
	{ uint32_t v = ld16((ctx->a[4] + (0))); set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 12); /* 008E3E movew %a4@(0),%d0 */
	{ uint32_t m = (1u << (0x7u & 31)); cc_set(ctx, CC_Z, (ctx->d[0] & m) ? 0 : CC_Z); } hle_insn(ctx, 10); /* 008E42 btst #7,%d0 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_008E54; } hle_insn(ctx, 12);  /* 008E46 bnew 8e54 */
	{ uint32_t v = ld16(ctx->a[7]); ctx->a[7] += 2; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 008E4A movew %sp@+,%d0 */
	{ uint16_t c = (uint16_t)(ctx->d[0] - 1); set_w(&ctx->d[0], c); if (c != 0xFFFF) { hle_insn(ctx, 10); goto L_008E10; } hle_insn(ctx, 14); }  /* 008E4C dbf %d0,8e10 */
	hle_insn(ctx, 10); goto L_008EA4;  /* 008E50 braw 8ea4 */
L_008E54:
	{ uint32_t v = ld16(ctx->a[7]); ctx->a[7] += 2; set_w(&ctx->d[0], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 008E54 movew %sp@+,%d0 */
	hle_jump(ctx, 0x008E56); return;  /* 008E56 bsrw 8f0a */
L_008E5A:
	{ uint32_t v = ld16(0xFFFFA238u); st16(0xFFFFA0EEu, v); cc_logic16(ctx, v); } hle_insn(ctx, 28); /* 008E5A movew ffffa238,ffffa0ee */
	hle_insn(ctx, 10); goto L_008DA2;  /* 008E64 braw 8da2 */
L_008E68:
	{ uint32_t da = 0xFFFF9DF5u; alu(ctx, OP_SUB, ld8(da), 0x3u, 8, 0); } hle_insn(ctx, 20); /* 008E68 cmpib #3,ffff9df5 */
	if (COND_GT) { hle_insn(ctx, 10); goto L_008E86; } hle_insn(ctx, 8);  /* 008E70 bgts 8e86 */
	{ uint32_t da = 0xFFFF9DF5u; uint32_t r = alu(ctx, OP_ADD, ld8(da), 4, 8, 1); st8(da, r); } hle_insn(ctx, 20); /* 008E72 addqb #4,ffff9df5 */
	{ alu(ctx, OP_SUB, ctx->a[4], 0xFFFF8000u, 32, 0); } hle_insn(ctx, 14); /* 008E78 cmpal #-32768,%a4 */
	if (COND_NE) { hle_insn(ctx, 10); goto L_008E86; } hle_insn(ctx, 8);  /* 008E7E bnes 8e86 */
	{ uint32_t da = 0xFFFFA0A7u; uint32_t r = alu(ctx, OP_ADD, ld8(da), 1, 8, 1); st8(da, r); } hle_insn(ctx, 20); /* 008E80 addqb #1,ffffa0a7 */
L_008E86:
	{ uint32_t v = 0xFFFFu; st16((ctx->a[4] + (32)), v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 008E86 movew #-1,%a4@(32) */
	{ uint32_t v = 0x4u; st16((ctx->a[4] + (34)), v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 008E8C movew #4,%a4@(34) */
	hle_jump(ctx, 0x008E92); return;  /* 008E92 jsr %pc@(7bb8) */
L_008E96:
	{ uint32_t da = 0xFFFF9F6Du; uint32_t r = alu(ctx, OP_SUB, ld8(da), 1, 8, 1); st8(da, r); } hle_insn(ctx, 20); /* 008E96 subqb #1,ffff9f6d */
	if (COND_EQ) { hle_insn(ctx, 10); goto L_008EA4; } hle_insn(ctx, 12);  /* 008E9C beqw 8ea4 */
	hle_insn(ctx, 10); goto L_008DFC;  /* 008EA0 braw 8dfc */
L_008EA4:
	hle_jump(ctx, 0x008EA4); return;  /* 008EA4 jsr %pc@(7bb8) */
L_008EA8:
	{ uint32_t v = ld16(0xFFFF80C0u); set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 16); /* 008EA8 movew ffff80c0,%d3 */
	{ uint32_t r = alu(ctx, OP_ADD, (ctx->d[3] & 0xFFFFu), 0xAu, 16, 1); set_w(&ctx->d[3], r); } hle_insn(ctx, 8); /* 008EAE addiw #10,%d3 */
	{ ctx->a[0] = 0x00003295u; } hle_insn(ctx, 8); /* 008EB2 lea %pc@(3295),%a0 */
	hle_jump(ctx, 0x008EB6); return;  /* 008EB6 jsr %pc@(7e08) */
	hle_jump(ctx, 0x008EBA); return;  /* 008EBA jsr 13dec */
L_008EC0:
	{ uint32_t v = 0x3Cu; set_w(&ctx->d[1], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 008EC0 movew #60,%d1 */
	hle_jump(ctx, 0x008EC4); return;  /* 008EC4 jsr %pc@(86d8) */
L_008EC8:
	{ st8(0xFFFF9F6Du, 0); cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_Z); } hle_insn(ctx, 20); /* 008EC8 clrb ffff9f6d */
	{ uint32_t v = 0xFEu; st8(0xFFFFA234u, v); cc_logic8(ctx, v); } hle_insn(ctx, 20); /* 008ECE moveb #-2,ffffa234 */
	hle_insn(ctx, 10); goto L_008DFC;  /* 008ED6 braw 8dfc */
L_01524A:
	{ cc_logic16(ctx, ld16((ctx->a[6] + (38)))); } hle_insn(ctx, 12); /* 01524A tstw %fp@(38) */
	if (COND_NE) { hle_insn(ctx, 10); goto L_01526C; } hle_insn(ctx, 8);  /* 01524E bnes 1526c */
	{ ctx->a[4] = 0xFFFF8000u; } hle_insn(ctx, 12); /* 015250 lea ffff8000,%a4 */
	{ uint32_t v = 0xE208u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 015256 movew #-7672,%d3 */
	hle_jump(ctx, 0x01525A); return;  /* 01525A bsrw 15272 */
L_01525E:
	{ ctx->a[4] = 0xFFFF8050u; } hle_insn(ctx, 12); /* 01525E lea ffff8050,%a4 */
	{ uint32_t v = 0xE238u; set_w(&ctx->d[3], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 015264 movew #-7624,%d3 */
	hle_jump(ctx, 0x015268); return;  /* 015268 bsrw 15272 */
L_01526C:
	hle_jump(ctx, 0x01526C); return;  /* 01526C jmp b3be */
L_015272:
	{ cc_logic16(ctx, ld16((ctx->a[4] + (36)))); } hle_insn(ctx, 12); /* 015272 tstw %a4@(36) */
	if (COND_MI) { hle_insn(ctx, 10); goto L_015288; } hle_insn(ctx, 8);  /* 015276 bmis 15288 */
	{ uint32_t v = ctx->a[6]; ctx->a[7] -= 4; st32(ctx->a[7], v); cc_logic32(ctx, v); } hle_insn(ctx, 12); /* 015278 movel %fp,%sp@- */
	{ ctx->a[0] = (ctx->a[4] + (50)); } hle_insn(ctx, 8); /* 01527A lea %a4@(50),%a0 */
	{ uint32_t v = 0x2000u; set_w(&ctx->d[6], v); cc_logic16(ctx, v); } hle_insn(ctx, 8); /* 01527E movew #8192,%d6 */
	hle_jump(ctx, 0x015282); return;  /* 015282 bsrw 1528a */
L_015286:
	{ ctx->a[6] = ld32(ctx->a[7]); ctx->a[7] += 4; } hle_insn(ctx, 12); /* 015286 moveal %sp@+,%fp */
L_015288:
	hle_jump(ctx, 0x015288); return;  /* 015288 rts */
L_01528A:
	hle_jump(ctx, 0x01528A); return;  /* 01528A jsr 11d12 */
	hle_jump(ctx, 0x015290); return;  /* 015290 jsr 7f00 */
	hle_jump(ctx, 0x015296); return;  /* 015296 jsr 11d28 */
	hle_jump(ctx, 0x01529C); return;  /* 01529C jsr 11d32 */
L_0152A2:
	{ cc_logic8(ctx, ld8(ctx->a[0])); } hle_insn(ctx, 8); /* 0152A2 tstb %a0@ */
	if (COND_NE) { hle_insn(ctx, 10); goto L_01528A; } hle_insn(ctx, 8);  /* 0152A4 bnes 1528a */
	hle_jump(ctx, 0x0152A6); return;  /* 0152A6 rts */
}

HLE_RANGE_PURE(0x000912, Screens_000912, 200,
	0x000916)
{ sc_run(ctx, 0x000912); }
HLE_RANGE_PURE(0x00091A, Screens_00091A, 200,
	0x00091E)
{ sc_run(ctx, 0x00091A); }
HLE_RANGE_PURE(0x000922, Screens_000922, 200,
	0x000932)
{ sc_run(ctx, 0x000922); }
HLE_RANGE_PURE(0x00093E, Screens_00093E, 200,
	0x000944)
{ sc_run(ctx, 0x00093E); }
HLE_RANGE_PURE(0x00094C, Screens_00094C, 200,
	0x000954, 0x000958)
{ sc_run(ctx, 0x00094C); }
HLE_RANGE_PURE(0x00095C, Screens_00095C, 200,
	0x00096E, 0x000986)
{ sc_run(ctx, 0x00095C); }
HLE_RANGE_PURE(0x000974, Screens_000974, 200,
	0x000958, 0x00097E)
{ sc_run(ctx, 0x000974); }
HLE_RANGE_PURE(0x000982, Screens_000982, 200,
	0x000900)
{ sc_run(ctx, 0x000982); }
HLE_RANGE_PURE(0x000990, Screens_000990, 200,
	0x000996)
{ sc_run(ctx, 0x000990); }
HLE_RANGE_PURE(0x00099A, Screens_00099A, 200,
	0x000900)
{ sc_run(ctx, 0x00099A); }
HLE_RANGE_PURE(0x00099E, Screens_00099E, 200,
	0x0009A2)
{ sc_run(ctx, 0x00099E); }
HLE_RANGE_PURE(0x0009B0, Screens_0009B0, 200,
	0x0009B4)
{ sc_run(ctx, 0x0009B0); }
HLE_RANGE_PURE(0x0009BA, Screens_0009BA, 200,
	0x0009C4)
{ sc_run(ctx, 0x0009BA); }
HLE_RANGE_PURE(0x0009CA, Screens_0009CA, 200,
	0x0009DA)
{ sc_run(ctx, 0x0009CA); }
HLE_RANGE_PURE(0x0009E0, Screens_0009E0, 200,
	0x0009EA)
{ sc_run(ctx, 0x0009E0); }
HLE_RANGE_PURE(0x0009F0, Screens_0009F0, 200,
	0x0009F4)
{ sc_run(ctx, 0x0009F0); }
HLE_RANGE_PURE(0x0009FA, Screens_0009FA, 200,
	0x0009FE)
{ sc_run(ctx, 0x0009FA); }
HLE_RANGE_PURE(0x000A04, Screens_000A04, 200,
	0x000A0C)
{ sc_run(ctx, 0x000A04); }
HLE_RANGE_PURE(0x000A12, Screens_000A12, 200,
	0x000A1E)
{ sc_run(ctx, 0x000A12); }
HLE_RANGE_PURE(0x000A24, Screens_000A24, 200,
	0x000A2A)
{ sc_run(ctx, 0x000A24); }
HLE_RANGE_PURE(0x000A2E, Screens_000A2E, 200,
	0x000A2A, 0x000A32)
{ sc_run(ctx, 0x000A2E); }
HLE_RANGE_PURE(0x000C78, CheatCodeCheck, 200,
	0x000CAC, 0x000CB8)
{ sc_run(ctx, 0x000C78); }
HLE_RANGE_PURE(0x000CB2, Screens_000CB2, 200,
	0x000CB8)
{ sc_run(ctx, 0x000CB2); }
HLE_RANGE_PURE(0x000CC6, Screens_000CC6, 200,
	0x000CCA)
{ sc_run(ctx, 0x000CC6); }
HLE_RANGE_PURE(0x000CD8, Screens_000CD8, 200,
	0x000CDC)
{ sc_run(ctx, 0x000CD8); }
HLE_RANGE_PURE(0x000CE2, Screens_000CE2, 200,
	0x000CF2)
{ sc_run(ctx, 0x000CE2); }
HLE_RANGE_PURE(0x000CF8, Screens_000CF8, 200,
	0x000D02)
{ sc_run(ctx, 0x000CF8); }
HLE_RANGE_PURE(0x000D06, Screens_000D06, 200,
	0x000D0A)
{ sc_run(ctx, 0x000D06); }
HLE_RANGE_PURE(0x000D0E, Screens_000D0E, 200,
	0x000D12)
{ sc_run(ctx, 0x000D0E); }
HLE_RANGE_PURE(0x000D16, Screens_000D16, 200,
	0x000D1A)
{ sc_run(ctx, 0x000D16); }
HLE_RANGE_PURE(0x000D1E, Screens_000D1E, 200,
	0x000D24)
{ sc_run(ctx, 0x000D1E); }
HLE_RANGE_PURE(0x000D28, Screens_000D28, 200,
	0x000D2E)
{ sc_run(ctx, 0x000D28); }
HLE_RANGE_PURE(0x000D32, Screens_000D32, 200,
	0x000D38)
{ sc_run(ctx, 0x000D32); }
HLE_RANGE_PURE(0x000D3C, Screens_000D3C, 200,
	0x000D38, 0x000D40)
{ sc_run(ctx, 0x000D3C); }
HLE_RANGE_PURE(0x000D48, Screens_000D48, 200,
	0x000D62)
{ sc_run(ctx, 0x000D48); }
HLE_RANGE_PURE(0x000D76, Screens_000D76, 200,
	0x000D7A)
{ sc_run(ctx, 0x000D76); }
HLE_RANGE_PURE(0x000D88, Screens_000D88, 200,
	0x000D8C)
{ sc_run(ctx, 0x000D88); }
HLE_RANGE_PURE(0x000D92, Screens_000D92, 200,
	0x000DA2)
{ sc_run(ctx, 0x000D92); }
HLE_RANGE_PURE(0x000DA8, Screens_000DA8, 200,
	0x000DB8)
{ sc_run(ctx, 0x000DA8); }
HLE_RANGE_PURE(0x000DBE, Screens_000DBE, 200,
	0x000DC8)
{ sc_run(ctx, 0x000DBE); }
HLE_RANGE_PURE(0x000DCC, Screens_000DCC, 200,
	0x000DD0)
{ sc_run(ctx, 0x000DCC); }
HLE_RANGE_PURE(0x000DD4, Screens_000DD4, 200,
	0x000DD8)
{ sc_run(ctx, 0x000DD4); }
HLE_RANGE_PURE(0x000DDC, Screens_000DDC, 200,
	0x000DE0)
{ sc_run(ctx, 0x000DDC); }
HLE_RANGE_PURE(0x000DE4, Screens_000DE4, 200,
	0x000DEA)
{ sc_run(ctx, 0x000DE4); }
HLE_RANGE_PURE(0x000DEE, Screens_000DEE, 200,
	0x000DF8)
{ sc_run(ctx, 0x000DEE); }
HLE_RANGE_PURE(0x000E04, Screens_000E04, 200,
	0x000E0A)
{ sc_run(ctx, 0x000E04); }
HLE_RANGE_PURE(0x000E0E, Screens_000E0E, 200,
	0x000E14)
{ sc_run(ctx, 0x000E0E); }
HLE_RANGE_PURE(0x000E1C, Screens_000E1C, 200,
	0x000E14, 0x000E20)
{ sc_run(ctx, 0x000E1C); }
HLE_RANGE_PURE(0x000E2C, Screens_000E2C, 200,
	0x000E5A, 0x000E5E)
{ sc_run(ctx, 0x000E2C); }
HLE_RANGE_PURE(0x000E66, Screens_000E66, 200,
	0x000EA0)
{ sc_run(ctx, 0x000E66); }
HLE_RANGE_PURE(0x000ED4, Screens_000ED4, 200,
	0x000ED8)
{ sc_run(ctx, 0x000ED4); }
HLE_RANGE_PURE(0x000EE6, Screens_000EE6, 200,
	0x000EEA)
{ sc_run(ctx, 0x000EE6); }
HLE_RANGE_PURE(0x000EF0, Screens_000EF0, 200,
	0x000F00)
{ sc_run(ctx, 0x000EF0); }
HLE_RANGE_PURE(0x000F06, Screens_000F06, 200,
	0x000F16)
{ sc_run(ctx, 0x000F06); }
HLE_RANGE_PURE(0x000F1C, Screens_000F1C, 200,
	0x000F2C)
{ sc_run(ctx, 0x000F1C); }
HLE_RANGE_PURE(0x000F32, Screens_000F32, 200,
	0x000F3C)
{ sc_run(ctx, 0x000F32); }
HLE_RANGE_PURE(0x000F40, Screens_000F40, 200,
	0x000F44)
{ sc_run(ctx, 0x000F40); }
HLE_RANGE_PURE(0x000F48, Screens_000F48, 200,
	0x000F4C)
{ sc_run(ctx, 0x000F48); }
HLE_RANGE_PURE(0x000F50, Screens_000F50, 200,
	0x000F54)
{ sc_run(ctx, 0x000F50); }
HLE_RANGE_PURE(0x000F5C, Screens_000F5C, 200,
	0x000F68)
{ sc_run(ctx, 0x000F5C); }
HLE_RANGE_PURE(0x000F6C, Screens_000F6C, 200,
	0x000F76)
{ sc_run(ctx, 0x000F6C); }
HLE_RANGE_PURE(0x000F7A, Screens_000F7A, 200,
	0x000F80)
{ sc_run(ctx, 0x000F7A); }
HLE_RANGE_PURE(0x000F8C, Screens_000F8C, 200,
	0x000F84, 0x000FAE, 0x000FC6, 0x000FE4, 0x001344)
{ sc_run(ctx, 0x000F8C); }
HLE_RANGE_PURE(0x000FB2, Screens_000FB2, 200,
	0x000F84, 0x000FC6, 0x000FE4)
{ sc_run(ctx, 0x000FB2); }
HLE_RANGE_PURE(0x000FCA, Screens_000FCA, 200,
	0x000F84, 0x000FE4)
{ sc_run(ctx, 0x000FCA); }
HLE_RANGE_PURE(0x000FE8, Screens_000FE8, 200,
	0x000FEE)
{ sc_run(ctx, 0x000FE8); }
HLE_RANGE_PURE(0x000FFC, Screens_000FFC, 200,
	0x001016)
{ sc_run(ctx, 0x000FFC); }
HLE_RANGE_PURE(0x00102E, Screens_00102E, 200,
	0x001040)
{ sc_run(ctx, 0x00102E); }
HLE_RANGE_PURE(0x001044, Screens_001044, 200,
	0x001048)
{ sc_run(ctx, 0x001044); }
HLE_RANGE_PURE(0x00104C, Screens_00104C, 200,
	0x001058)
{ sc_run(ctx, 0x00104C); }
HLE_RANGE_PURE(0x00105C, Screens_00105C, 200,
	0x00106A)
{ sc_run(ctx, 0x00105C); }
HLE_RANGE_PURE(0x00106E, Screens_00106E, 200,
	0x001072)
{ sc_run(ctx, 0x00106E); }
HLE_RANGE_PURE(0x00107E, Screens_00107E, 200,
	0x00108A, 0x001092, 0x001098)
{ sc_run(ctx, 0x00107E); }
HLE_RANGE_PURE(0x00108E, Screens_00108E, 200,
	0x0010BC)
{ sc_run(ctx, 0x00108E); }
HLE_RANGE_PURE(0x001096, Screens_001096, 200,
	0x0010BC)
{ sc_run(ctx, 0x001096); }
HLE_RANGE_PURE(0x00109E, Screens_00109E, 200,
	0x0010A6)
{ sc_run(ctx, 0x00109E); }
HLE_RANGE_PURE(0x0010B8, Screens_0010B8, 200,
	0x0010BC)
{ sc_run(ctx, 0x0010B8); }
HLE_RANGE_PURE(0x0010C8, Screens_0010C8, 200,
	0x0010D4)
{ sc_run(ctx, 0x0010C8); }
HLE_RANGE_PURE(0x0010DA, Screens_0010DA, 200,
	0x0010E6)
{ sc_run(ctx, 0x0010DA); }
HLE_RANGE_PURE(0x0010F8, Screens_0010F8, 200,
	HLE_EXIT_SP(0x001114, -2))
{ sc_run(ctx, 0x0010F8); }
HLE_RANGE_PURE(0x00111A, Screens_00111A, 200,
	HLE_EXIT_SP(0x001128, 2))
{ sc_run(ctx, 0x00111A); }
HLE_RANGE_PURE(0x001136, Screens_001136, 200,
	0x001150)
{ sc_run(ctx, 0x001136); }
HLE_RANGE_PURE(0x00116E, Screens_00116E, 200,
	0x00117C)
{ sc_run(ctx, 0x00116E); }
HLE_RANGE_PURE(0x001180, Screens_001180, 200,
	0x001198, 0x0011C8, 0x0011CC)
{ sc_run(ctx, 0x001180); }
HLE_RANGE_PURE(0x00119C, Screens_00119C, 200,
	0x0011B8, 0x0011C8, 0x0011CC)
{ sc_run(ctx, 0x00119C); }
HLE_RANGE_PURE(0x0011BC, Screens_0011BC, 200,
	0x0011CC)
{ sc_run(ctx, 0x0011BC); }
HLE_RANGE_PURE(0x0011CE, Screens_0011CE, 200,
	0x0011D8, 0x0011DC)
{ sc_run(ctx, 0x0011CE); }
HLE_RANGE_PURE(0x001348, Screens_001348, 200,
	0x00135C)
{ sc_run(ctx, 0x001348); }
HLE_RANGE_PURE(0x00136A, Screens_00136A, 200,
	0x001374)
{ sc_run(ctx, 0x00136A); }
HLE_RANGE_PURE(0x001380, Screens_001380, 200,
	0x00137C, 0x00138A)
{ sc_run(ctx, 0x001380); }
HLE_RANGE_PURE(0x001394, Screens_001394, 200,
	0x000F3C)
{ sc_run(ctx, 0x001394); }
HLE_RANGE_PURE(0x0013AC, Screens_0013AC, 200,
	0x0013BA)
{ sc_run(ctx, 0x0013AC); }
HLE_RANGE_PURE(0x0013BE, Screens_0013BE, 200,
	0x0013C2, 0x0013E0)
{ sc_run(ctx, 0x0013BE); }
HLE_RANGE_PURE(0x0013C6, Screens_0013C6, 200,
	0x0013D6, 0x0014A2)
{ sc_run(ctx, 0x0013C6); }
HLE_RANGE_PURE(0x0013DA, Screens_0013DA, 200,
	0x0014A2)
{ sc_run(ctx, 0x0013DA); }
HLE_RANGE_PURE(0x0013E4, Screens_0013E4, 200,
	0x0013E8, 0x001406)
{ sc_run(ctx, 0x0013E4); }
HLE_RANGE_PURE(0x0013EC, Screens_0013EC, 200,
	0x0013FC, 0x0014A2)
{ sc_run(ctx, 0x0013EC); }
HLE_RANGE_PURE(0x001400, Screens_001400, 200,
	0x0014A2)
{ sc_run(ctx, 0x001400); }
HLE_RANGE_PURE(0x00140A, Screens_00140A, 200,
	0x001410, 0x001422)
{ sc_run(ctx, 0x00140A); }
HLE_RANGE_PURE(0x001414, Screens_001414, 200,
	0x00141A, 0x001422)
{ sc_run(ctx, 0x001414); }
HLE_RANGE_PURE(0x00141E, Screens_00141E, 200,
	0x001422, 0x001438)
{ sc_run(ctx, 0x00141E); }
HLE_RANGE_PURE(0x001426, Screens_001426, 200,
	0x001430, 0x0014A2)
{ sc_run(ctx, 0x001426); }
HLE_RANGE_PURE(0x001432, Screens_001432, 200,
	0x0014A2)
{ sc_run(ctx, 0x001432); }
HLE_RANGE_PURE(0x00143C, Screens_00143C, 200,
	0x001452, 0x001462, 0x0014A2)
{ sc_run(ctx, 0x00143C); }
HLE_RANGE_PURE(0x00145E, Screens_00145E, 200,
	0x00149E)
{ sc_run(ctx, 0x00145E); }
HLE_RANGE_PURE(0x001466, Screens_001466, 200,
	0x001480, 0x001490, 0x0014A2)
{ sc_run(ctx, 0x001466); }
HLE_RANGE_PURE(0x00148C, Screens_00148C, 200,
	0x00149E)
{ sc_run(ctx, 0x00148C); }
HLE_RANGE_PURE(0x001494, Screens_001494, 200,
	0x00149E, 0x0014A2)
{ sc_run(ctx, 0x001494); }
HLE_RANGE_PURE(0x0014A4, Screens_0014A4, 200,
	0x0014AA)
{ sc_run(ctx, 0x0014A4); }
HLE_RANGE_PURE(0x0014B0, Screens_0014B0, 200,
	0x0014C0, 0x0014C6)
{ sc_run(ctx, 0x0014B0); }
HLE_RANGE_PURE(0x0014C8, Screens_0014C8, 200,
	0x0014D4)
{ sc_run(ctx, 0x0014C8); }
HLE_RANGE_PURE(0x0014D8, Screens_0014D8, 200,
	0x0014DC)
{ sc_run(ctx, 0x0014D8); }
HLE_RANGE_PURE(0x0015F0, Screens_0015F0, 200,
	0x001600)
{ sc_run(ctx, 0x0015F0); }
HLE_RANGE_PURE(0x001606, Screens_001606, 200,
	0x001610)
{ sc_run(ctx, 0x001606); }
HLE_RANGE_PURE(0x00161A, Screens_00161A, 200,
	0x001634)
{ sc_run(ctx, 0x00161A); }
HLE_RANGE_PURE(0x001646, Screens_001646, 200,
	0x001652)
{ sc_run(ctx, 0x001646); }
HLE_RANGE_PURE(0x001668, Screens_001668, 200,
	0x001652, 0x001674)
{ sc_run(ctx, 0x001668); }
HLE_RANGE_PURE(0x00167A, Screens_00167A, 200,
	0x001686)
{ sc_run(ctx, 0x00167A); }
HLE_RANGE_PURE(0x00168C, Screens_00168C, 200,
	0x001692)
{ sc_run(ctx, 0x00168C); }
HLE_RANGE_PURE(0x001696, Screens_001696, 200,
	0x00169A)
{ sc_run(ctx, 0x001696); }
HLE_RANGE_PURE(0x0016A4, Screens_0016A4, 200,
	0x0016CC, 0x0016F2)
{ sc_run(ctx, 0x0016A4); }
HLE_RANGE_PURE(0x0016D0, Screens_0016D0, 200,
	0x0016D6)
{ sc_run(ctx, 0x0016D0); }
HLE_RANGE_PURE(0x0016DA, Screens_0016DA, 200,
	0x0016F2)
{ sc_run(ctx, 0x0016DA); }
HLE_RANGE_PURE(0x0016F8, Screens_0016F8, 200,
	0x001706)
{ sc_run(ctx, 0x0016F8); }
HLE_RANGE_PURE(0x00171C, Screens_00171C, 200,
	0x0016CC, 0x0016F2, 0x001726)
{ sc_run(ctx, 0x00171C); }
HLE_RANGE_PURE(0x001A1E, Screens_001A1E, 200,
	0x001A2C)
{ sc_run(ctx, 0x001A1E); }
HLE_RANGE_PURE(0x001A34, Screens_001A34, 200,
	0x001B94)
{ sc_run(ctx, 0x001A34); }
HLE_RANGE_PURE(0x001A42, Screens_001A42, 200,
	0x001A46)
{ sc_run(ctx, 0x001A42); }
HLE_RANGE_PURE(0x001A4C, Screens_001A4C, 200,
	0x001A56)
{ sc_run(ctx, 0x001A4C); }
HLE_RANGE_PURE(0x001A5A, Screens_001A5A, 200,
	0x001A5E)
{ sc_run(ctx, 0x001A5A); }
HLE_RANGE_PURE(0x001A62, Screens_001A62, 200,
	0x001A66)
{ sc_run(ctx, 0x001A62); }
HLE_RANGE_PURE(0x001A6A, Screens_001A6A, 200,
	0x001A6E)
{ sc_run(ctx, 0x001A6A); }
HLE_RANGE_PURE(0x001A72, Screens_001A72, 200,
	0x001A78)
{ sc_run(ctx, 0x001A72); }
HLE_RANGE_PURE(0x001A7E, Screens_001A7E, 200,
	0x001A8A)
{ sc_run(ctx, 0x001A7E); }
HLE_RANGE_PURE(0x001A9A, Screens_001A9A, 200,
	0x001AAA)
{ sc_run(ctx, 0x001A9A); }
HLE_RANGE_PURE(0x001AB0, Screens_001AB0, 200,
	0x001AC0)
{ sc_run(ctx, 0x001AB0); }
HLE_RANGE_PURE(0x001AC6, Screens_001AC6, 200,
	0x001ACC)
{ sc_run(ctx, 0x001AC6); }
HLE_RANGE_PURE(0x001AD0, Screens_001AD0, 200,
	0x001AD2)
{ sc_run(ctx, 0x001AD0); }
HLE_RANGE_PURE(0x001AD6, Screens_001AD6, 200,
	0x001AE0)
{ sc_run(ctx, 0x001AD6); }
HLE_RANGE_PURE(0x001ADC, Screens_001ADC, 200,
	0x001AE0)
{ sc_run(ctx, 0x001ADC); }
HLE_RANGE_PURE(0x001AE6, Screens_001AE6, 200,
	HLE_EXIT_SP(0x001AEA, -2))
{ sc_run(ctx, 0x001AE6); }
HLE_RANGE_PURE(0x001AF0, Screens_001AF0, 200,
	HLE_EXIT_SP(0x001B02, -2))
{ sc_run(ctx, 0x001AF0); }
HLE_RANGE_PURE(0x001B06, Screens_001B06, 200,
	0x001B16)
{ sc_run(ctx, 0x001B06); }
HLE_RANGE_PURE(0x001B1C, Screens_001B1C, 200,
	HLE_EXIT_SP(0x001B3C, 2))
{ sc_run(ctx, 0x001B1C); }
HLE_RANGE_PURE(0x001B54, Screens_001B54, 200,
	0x001AEA, HLE_EXIT_SP(0x001B60, 2))
{ sc_run(ctx, 0x001B54); }
HLE_RANGE_PURE(0x001B66, Screens_001B66, 200,
	0x001B6A)
{ sc_run(ctx, 0x001B66); }
HLE_RANGE_PURE(0x001B72, PressStartLoop, 200,
	0x001B94)
{ sc_run(ctx, 0x001B72); }
HLE_RANGE_PURE(0x001B9E, Screens_001B9E, 200,
	0x001BA8, 0x001BD2)
{ sc_run(ctx, 0x001B9E); }
HLE_RANGE_PURE(0x001BB0, Screens_001BB0, 200,
	0x001B94, 0x001BCA)
{ sc_run(ctx, 0x001BB0); }
HLE_RANGE_PURE(0x003EC8, HiScoreInitOnce, 200,
	0x003F3A)
{ sc_run(ctx, 0x003EC8); }
HLE_RANGE_PURE(0x003EFE, SortHiScores, 200,
	0x003F3A)
{ sc_run(ctx, 0x003EFE); }
HLE_RANGE_PURE(0x003FA0, Screens_003FA0, 200,
	0x003FAA)
{ sc_run(ctx, 0x003FA0); }
HLE_RANGE_PURE(0x003FB0, Screens_003FB0, 200,
	0x003FC0)
{ sc_run(ctx, 0x003FB0); }
HLE_RANGE_PURE(0x003FC6, Screens_003FC6, 200,
	0x003FDE)
{ sc_run(ctx, 0x003FC6); }
HLE_RANGE_PURE(0x003FE2, Screens_003FE2, 200,
	0x003FE4)
{ sc_run(ctx, 0x003FE2); }
HLE_RANGE_PURE(0x003FE8, Screens_003FE8, 200,
	0x003FF2)
{ sc_run(ctx, 0x003FE8); }
HLE_RANGE_PURE(0x003FF6, Screens_003FF6, 200,
	0x003FFA)
{ sc_run(ctx, 0x003FF6); }
HLE_RANGE_PURE(0x003FFE, Screens_003FFE, 200,
	0x004004)
{ sc_run(ctx, 0x003FFE); }
HLE_RANGE_PURE(0x004008, Screens_004008, 200,
	0x004014)
{ sc_run(ctx, 0x004008); }
HLE_RANGE_PURE(0x004018, Screens_004018, 200,
	HLE_EXIT_SP(0x00402A, -4))
{ sc_run(ctx, 0x004018); }
HLE_RANGE_PURE(0x00402E, Screens_00402E, 200,
	0x004032)
{ sc_run(ctx, 0x00402E); }
HLE_RANGE_PURE(0x004036, Screens_004036, 200,
	0x004038)
{ sc_run(ctx, 0x004036); }
HLE_RANGE_PURE(0x00403C, Screens_00403C, 200,
	0x004046)
{ sc_run(ctx, 0x00403C); }
HLE_RANGE_PURE(0x00404A, Screens_00404A, 200,
	0x00402A, HLE_EXIT_SP(0x004060, 4))
{ sc_run(ctx, 0x00404A); }
HLE_RANGE_PURE(0x004068, Screens_004068, 200,
	0x004060, 0x00407C, 0x004082)
{ sc_run(ctx, 0x004068); }
HLE_RANGE_PURE(0x0041EA, Screens_0041EA, 200,
	0x0041F0)
{ sc_run(ctx, 0x0041EA); }
HLE_RANGE_PURE(0x0041F4, Screens_0041F4, 200,
	0x0041F8)
{ sc_run(ctx, 0x0041F4); }
HLE_RANGE_PURE(0x00421A, Screens_00421A, 200,
	0x00422A)
{ sc_run(ctx, 0x00421A); }
HLE_RANGE_PURE(0x004230, Screens_004230, 200,
	0x00423A)
{ sc_run(ctx, 0x004230); }
HLE_RANGE_PURE(0x00423E, Screens_00423E, 200,
	0x004242)
{ sc_run(ctx, 0x00423E); }
HLE_RANGE_PURE(0x004246, Screens_004246, 200,
	0x00424A)
{ sc_run(ctx, 0x004246); }
HLE_RANGE_PURE(0x004252, Screens_004252, 200,
	0x00425A)
{ sc_run(ctx, 0x004252); }
HLE_RANGE_PURE(0x00425E, Screens_00425E, 200,
	HLE_EXIT_SP(0x004274, 4), 0x004276)
{ sc_run(ctx, 0x00425E); }
HLE_RANGE_PURE(0x004280, Screens_004280, 200,
	0x004284)
{ sc_run(ctx, 0x004280); }
HLE_RANGE_PURE(0x00429E, Screens_00429E, 200,
	0x00425A, 0x0042A8)
{ sc_run(ctx, 0x00429E); }
HLE_RANGE_PURE(0x0042B0, Screens_0042B0, 200,
	0x0042A8, HLE_EXIT_SP(0x0042C6, 4), 0x0042CC)
{ sc_run(ctx, 0x0042B0); }
HLE_RANGE_PURE(0x0042D6, Screens_0042D6, 200,
	0x0042E6)
{ sc_run(ctx, 0x0042D6); }
HLE_RANGE_PURE(0x0042EC, Screens_0042EC, 200,
	0x0042FC)
{ sc_run(ctx, 0x0042EC); }
HLE_RANGE_PURE(0x004302, Screens_004302, 200,
	0x004320)
{ sc_run(ctx, 0x004302); }
HLE_RANGE_PURE(0x004330, Screens_004330, 200,
	0x00433E)
{ sc_run(ctx, 0x004330); }
HLE_RANGE_PURE(0x00434E, Screens_00434E, 200,
	0x004358)
{ sc_run(ctx, 0x00434E); }
HLE_RANGE_PURE(0x00435C, Screens_00435C, 200,
	0x004360)
{ sc_run(ctx, 0x00435C); }
HLE_RANGE_PURE(0x004364, Screens_004364, 200,
	0x00436A)
{ sc_run(ctx, 0x004364); }
HLE_RANGE_PURE(0x00436E, Screens_00436E, 200,
	0x00438C)
{ sc_run(ctx, 0x00436E); }
HLE_RANGE_PURE(0x004392, Screens_004392, 200,
	0x0043A8, 0x0043DC, 0x0043F8, 0x004414, HLE_EXIT_SP(0x00444C, -2), 0x0044D2, 0x0044FC,
	0x004518, 0x004588, 0x0045D0)
{ sc_run(ctx, 0x004392); }
HLE_RANGE_PURE(0x0043AC, Screens_0043AC, 200,
	0x0043B4, 0x0043DC, 0x0043F8, 0x004414, HLE_EXIT_SP(0x00444C, -2), 0x0044D2, 0x0044FC,
	0x004518, 0x004588, 0x0045D0)
{ sc_run(ctx, 0x0043AC); }
HLE_RANGE_PURE(0x0043BA, Screens_0043BA, 200,
	0x0043DC, 0x0043F8, 0x004414, HLE_EXIT_SP(0x00444C, -2), 0x0044D2, 0x0044FC, 0x004518,
	0x004588, 0x0045D0)
{ sc_run(ctx, 0x0043BA); }
HLE_RANGE_PURE(0x0043E0, Screens_0043E0, 200,
	0x0043EE)
{ sc_run(ctx, 0x0043E0); }
HLE_RANGE_PURE(0x0043F2, Screens_0043F2, 200,
	0x0043F8, 0x004414, HLE_EXIT_SP(0x00444C, -2), 0x0044D2, 0x0044FC, 0x004518, 0x004588,
	0x0045D0)
{ sc_run(ctx, 0x0043F2); }
HLE_RANGE_PURE(0x0043FC, Screens_0043FC, 200,
	0x00440A)
{ sc_run(ctx, 0x0043FC); }
HLE_RANGE_PURE(0x00440E, Screens_00440E, 200,
	0x004414, HLE_EXIT_SP(0x00444C, -2), 0x0044D2, 0x0044FC, 0x004518, 0x004588, 0x0045D0)
{ sc_run(ctx, 0x00440E); }
HLE_RANGE_PURE(0x004418, Screens_004418, 200,
	0x004426)
{ sc_run(ctx, 0x004418); }
HLE_RANGE_PURE(0x00442A, Screens_00442A, 200,
	HLE_EXIT_SP(0x00444C, -2), 0x0044D2, 0x0044FC, 0x004518, 0x004588, 0x0045D0)
{ sc_run(ctx, 0x00442A); }
HLE_RANGE_PURE(0x004452, Screens_004452, 200,
	0x00446C)
{ sc_run(ctx, 0x004452); }
HLE_RANGE_PURE(0x004472, Screens_004472, 200,
	HLE_EXIT_SP(0x0044D2, 2), HLE_EXIT_SP(0x0044FC, 2), HLE_EXIT_SP(0x004518, 2),
	HLE_EXIT_SP(0x004588, 2), HLE_EXIT_SP(0x0045D0, 2))
{ sc_run(ctx, 0x004472); }
HLE_RANGE_PURE(0x0044D6, Screens_0044D6, 200,
	0x0044E4)
{ sc_run(ctx, 0x0044D6); }
HLE_RANGE_PURE(0x0044E8, Screens_0044E8, 200,
	0x0044FC, 0x004518, 0x004588, 0x0045D0)
{ sc_run(ctx, 0x0044E8); }
HLE_RANGE_PURE(0x004500, Screens_004500, 200,
	0x00450E)
{ sc_run(ctx, 0x004500); }
HLE_RANGE_PURE(0x004512, Screens_004512, 200,
	0x004518, 0x004588, 0x0045D0)
{ sc_run(ctx, 0x004512); }
HLE_RANGE_PURE(0x00451C, Screens_00451C, 200,
	0x004542)
{ sc_run(ctx, 0x00451C); }
HLE_RANGE_PURE(0x004546, Screens_004546, 200,
	0x004554)
{ sc_run(ctx, 0x004546); }
HLE_RANGE_PURE(0x004558, Screens_004558, 200,
	0x004566)
{ sc_run(ctx, 0x004558); }
HLE_RANGE_PURE(0x00456A, Screens_00456A, 200,
	0x004588, 0x0045D0)
{ sc_run(ctx, 0x00456A); }
HLE_RANGE_PURE(0x00458C, Screens_00458C, 200,
	0x0045D0)
{ sc_run(ctx, 0x00458C); }
HLE_RANGE_PURE(0x0045EA, Screens_0045EA, 200,
	0x00438C, 0x0045FE, 0x00460A)
{ sc_run(ctx, 0x0045EA); }
HLE_RANGE_PURE(0x004650, Screens_004650, 200,
	HLE_EXIT_SP(0x00465E, -2))
{ sc_run(ctx, 0x004650); }
HLE_RANGE_PURE(0x004664, Screens_004664, 200,
	HLE_EXIT_SP(0x004666, 2))
{ sc_run(ctx, 0x004664); }
HLE_RANGE_PURE(0x004668, Screens_004668, 200,
	0x004680)
{ sc_run(ctx, 0x004668); }
HLE_RANGE_PURE(0x006F64, InitialsHit1, 200,
	0x006F6C)
{ sc_run(ctx, 0x006F64); }
HLE_RANGE_PURE(0x006F78, InitialsHit2, 200,
	0x006F80)
{ sc_run(ctx, 0x006F78); }
HLE_RANGE_PURE(0x006F8C, InitialsOnHit, 200,
	0x007008)
{ sc_run(ctx, 0x006F8C); }
HLE_RANGE_PURE(0x008D68, PlayerDied, 200,
	0x008D7A, 0x008D92, 0x008DFE, 0x008E92)
{ sc_run(ctx, 0x008D68); }
HLE_RANGE_PURE(0x008D96, Screens_008D96, 200,
	0x008DFC)
{ sc_run(ctx, 0x008D96); }
HLE_RANGE_PURE(0x008DAE, Screens_008DAE, 200,
	HLE_EXIT_SP(0x008DB6, -60))
{ sc_run(ctx, 0x008DAE); }
HLE_RANGE_PURE(0x008DBC, Screens_008DBC, 200,
	HLE_EXIT_SP(0x008DCC, 60), HLE_EXIT_SP(0x008DF8, 60), HLE_EXIT_SP(0x008DFC, 60))
{ sc_run(ctx, 0x008DBC); }
HLE_RANGE_PURE(0x008DD2, Screens_008DD2, 200,
	0x008DF8, 0x008DFC)
{ sc_run(ctx, 0x008DD2); }
HLE_RANGE_PURE(0x008E02, Screens_008E02, 200,
	HLE_EXIT_SP(0x008E2A, -2))
{ sc_run(ctx, 0x008E02); }
HLE_RANGE_PURE(0x008E34, Screens_008E34, 200,
	HLE_EXIT_SP(0x008E36, -4))
{ sc_run(ctx, 0x008E34); }
HLE_RANGE_PURE(0x008E3C, Screens_008E3C, 200,
	HLE_EXIT_SP(0x008E2A, 4), HLE_EXIT_SP(0x008E56, 6), HLE_EXIT_SP(0x008EA4, 6))
{ sc_run(ctx, 0x008E3C); }
HLE_RANGE_PURE(0x008E5A, Screens_008E5A, 200,
	0x008DA2)
{ sc_run(ctx, 0x008E5A); }
HLE_RANGE_PURE(0x008E96, Screens_008E96, 200,
	0x008DFC, 0x008EA4)
{ sc_run(ctx, 0x008E96); }
HLE_RANGE_PURE(0x008EA8, Screens_008EA8, 200,
	0x008EB6)
{ sc_run(ctx, 0x008EA8); }
HLE_RANGE_PURE(0x008EC0, Screens_008EC0, 200,
	0x008EC4)
{ sc_run(ctx, 0x008EC0); }
HLE_RANGE_PURE(0x008EC8, Screens_008EC8, 200,
	0x008DFC)
{ sc_run(ctx, 0x008EC8); }
HLE_RANGE_PURE(0x01524A, InitialsAI, 200,
	0x01525A, 0x01526C)
{ sc_run(ctx, 0x01524A); }
HLE_RANGE_PURE(0x01525E, Screens_01525E, 200,
	0x015268)
{ sc_run(ctx, 0x01525E); }
HLE_RANGE_PURE(0x015272, Screens_015272, 200,
	HLE_EXIT_SP(0x015282, -4), 0x015288)
{ sc_run(ctx, 0x015272); }
HLE_RANGE_PURE(0x015286, Screens_015286, 200,
	HLE_EXIT_SP(0x015288, 4))
{ sc_run(ctx, 0x015286); }
HLE_RANGE_PURE(0x0152A2, Screens_0152A2, 200,
	0x01528A, 0x0152A6)
{ sc_run(ctx, 0x0152A2); }
