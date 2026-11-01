/* Score BCD add: AddScoreBcd ($007376) + the AddScore250..10000 entry points
 * ($0072F8..$007368) and BcdAdd ($007A92, decimal formatting for the HUD/results screens).
 *
 * AddScoreBcd (src/t2.s $007376):
 *     lea.l   ($FFFF9EAC).l,a0        ; a0 = p1_score + 4 (the addend scratch)
 *     tst.w   ($24,a4)                ; player index
 *     bmi.s   $00739A                 ; negative: not playing/dead -> no add
 *     beq.s   $00738A                 ; zero: P1
 *     lea.l   ($FFFF9EB4).l,a0        ; else P2 (p2_score + 4)
 * $00738A:
 *     movea.l a0,a1
 *     move.l  d0,(a1)+                ; store the addend at a0..a0+3, a1 = a0+4
 *     andi.b  #$ef,ccr                ; clear X
 *     abcd -(a1),-(a0)                ; four BCD bytes, least significant first:
 *     abcd -(a1),-(a0)                ;   score[3] += addend[3] + X
 *     abcd -(a1),-(a0)                ;   ... up to score[0] += addend[0] + X
 *     abcd -(a1),-(a0)
 * $00739A:
 *     movem.l (sp)+,d0/a0-a1          ; pop the caller's (wrapper's) saved regs
 *     rts
 *
 * NOTE the frame: AddScoreBcd does NOT push d0/a0-a1 - the 14-byte entry stubs push them
 * and tail-jump here, and this routine pops them. So a captured entry state at $007376 has
 * the wrapper's 12-byte frame at SP (the return address sits at SP+12, not SP), and the
 * routine leaves with SP = entry SP + 16. difftest's ordinary-function model (return at
 * [SP], exit SP = entry+4) therefore does not fit; AddScoreBcd is registered as an
 * HLE_RANGE that stops right before the epilogue at $00739A, where SP is still the entry
 * SP. The 68k `movem/rts` then run unchanged, both in the game and in difftest. The shared
 * addscorebcd_impl() is also what the entry stubs call directly (their C versions).
 *
 * The score is a 4-byte BCD long at p1_score $FF9EA8 / p2_score $FF9EB0 (data/ram.txt);
 * $FF9EAC / $FF9EB4 is the 4-byte addend scratch just above it. Every register the routine
 * itself touches (d0/a0/a1) is restored by the wrapper, and d1-d7/a2-a7 are never touched,
 * so only RAM and the flags are observable. On the P1/P2 path the final CCR is the 4th
 * abcd's; on the skip path it is `tst.w ($24,a4)` (N/Z, V=C=0, X untouched).
 *
 * The abcd implementation below is GPGX's (core/m68k/m68kops.h m68k_op_abcd_8_mm), the one
 * difftest compares against: X=C=decimal carry; Z is only ever cleared (FLAG_Z |= res with
 * FLAG_Z the inverted not_z_flag); N and V use the core's "undefined" formulas (V = (~r2) &
 * r3, N from bit 7 of the result). X is 0 at the first abcd (andi.b #$ef).
 *
 * The AddScore250..10000 entries are 14-byte stubs: `movem.l d0/a0-a1,-(sp)` +
 * `move.l #VALUE,d0` + `bra.w AddScoreBcd`. Their C versions set d0, call the shared
 * addscorebcd_impl() (i.e. the AddScoreBcd C code) directly, and restore d0/a0/a1; difftest
 * confirms they match. $007306 (AddScore500) is never referenced anywhere in the ROM
 * (data/functions.txt, xref): it is dead code, so no state can be captured at its address.
 * It is proven with the AddScore250 entry states through difftest's --entry mode (same
 * calling context, PC redirected to $007306):
 *     build/port/difftest run AddScore500 --entry --states runs/states/0072F8
 *
 * BcdAdd (src/t2.s $007A92): convert d4 to (d3+1) ASCII decimal digits at $FFA0BA, most
 * significant first, NUL-terminated, then rts. It calls sub_007AD4 (d0 *= 10) d3 times to
 * form each place value; that helper is inlined here. Registers d0-d7/a1 are saved and
 * restored, so only RAM $FFA0BA.. and the flags are observable. The final flag-setting
 * instruction is `clr.b (a1,d7.w)` (N=0,Z=1,V=C=0); X is left by the last `addq.w #1,d7`
 * (no carry for any real d3). */
#include "hle.h"

#include <stdlib.h>

#define P1_ADDEND 0xFF9EAC
#define P2_ADDEND 0xFF9EB4
#define BCD_OUT   0xFFFFA0BA   /* BcdAdd's output buffer ($FFA0BA) */

/* How many AddScoreBcd calls took the P1 / P2 / skip path and which BcdAdd d3 (digit
 * counts) were seen. Gated behind T2_SCORE_STATS so it never affects a normal run; used
 * to show the captured states cover both players and several digit widths. */
static unsigned long g_p1, g_p2, g_skip, g_carry;
static unsigned long g_d3[16];
static int g_stats = -1;

static void score_stats_print(void)
{
	if (g_stats != 1)
		return;
	fprintf(stderr, "AddScoreBcd paths: P1=%lu P2=%lu skip=%lu carry=%lu; BcdAdd d3:", g_p1, g_p2, g_skip, g_carry);
	for (int i = 0; i < 16; i++)
		if (g_d3[i])
			fprintf(stderr, " %d=%lu", i, g_d3[i]);
	fprintf(stderr, "\n");
}

static int score_stats_on(void)
{
	if (g_stats < 0) {
		g_stats = getenv("T2_SCORE_STATS") != NULL;
		if (g_stats)
			atexit(score_stats_print);
	}
	return g_stats == 1;
}

/* one 68k abcd: dst = dst + src + X (BCD byte); returns the result byte. Mirrors
 * GPGX's m68k_op_abcd_8_mm including the undefined N/V formulas. */
static int g_bcd_carry;   /* set when the last abcd produced a decimal carry (stats only) */

static uint32_t bcd_add_byte(hle_ctx *ctx, uint32_t dst, uint32_t src)
{
	uint32_t x = (ctx->sr & CC_X) ? 1 : 0;
	uint32_t lo = (src & 0x0F) + (dst & 0x0F) + x;
	uint32_t corf = (lo > 9) ? 6 : 0;
	uint32_t r2 = lo + (src & 0xF0) + (dst & 0xF0);   /* before the carry fixup */
	uint32_t v_raw = ~r2;
	uint32_t r3 = r2 + corf;
	uint32_t carry = (r3 > 0x9F) ? 1 : 0;
	if (carry)
		r3 -= 0xA0;
	uint32_t v = v_raw & r3;                          /* undefined V */
	uint32_t res = r3 & 0xFF;

	if (carry)
		g_bcd_carry = 1;
	unsigned bits = 0;
	if (carry)
		bits |= CC_X | CC_C;
	if (res & 0x80)
		bits |= CC_N;
	if (v & 0x80)
		bits |= CC_V;
	cc_set(ctx, CC_X | CC_N | CC_V | CC_C, bits);
	if (res)
		cc_set(ctx, CC_Z, 0);   /* abcd only clears Z, never sets it */
	return res;
}

/* the lea/tst/abcd body, leaving a0/a1 exactly as the 68k does at $00739A */
static void addscorebcd_impl(hle_ctx *ctx)
{
	ctx->a[0] = 0xFFFF0000u | P1_ADDEND;      /* lea.l ($FFFF9EAC).l,a0 */
	uint32_t idx = RAM16(ctx->a[4] + 0x24);
	if (idx & 0x8000) {
		/* tst.w ($24,a4): N/Z from the word, V=C=0, X untouched; a1 unchanged */
		if (score_stats_on())
			g_skip++;
		cc_set(ctx, CC_N | CC_Z | CC_V | CC_C, CC_N);
		return;
	}
	uint32_t base;
	if (idx == 0) {
		base = 0xFFFF0000u | P1_ADDEND;   /* beq: keep a0 */
		if (score_stats_on())
			g_p1++;
	} else {
		base = 0xFFFF0000u | P2_ADDEND;   /* else: lea.l ($FFFF9EB4).l,a0 */
		ctx->a[0] = base;
		if (score_stats_on())
			g_p2++;
	}
	ctx->a[1] = base;                 /* movea.l a0,a1 */
	uint32_t addend = ctx->d[0];
	RAM32_W(base, addend);            /* move.l d0,(a1)+ */
	ctx->a[1] = base + 4;
	cc_set(ctx, CC_X, 0);             /* andi.b #$ef,ccr clears X */
	cc_set(ctx, CC_Z, addend ? 0 : CC_Z);   /* the move.l's Z, preserved by the andi */
	g_bcd_carry = 0;
	for (int i = 0; i < 4; i++) {     /* abcd -(a1),-(a0) x4 */
		uint32_t src = RAM8(base + 3 - i);
		uint32_t dstaddr = base - 1 - i;
		RAM8(dstaddr) = (uint8_t)bcd_add_byte(ctx, RAM8(dstaddr), src);
	}
	if (score_stats_on() && g_bcd_carry)
		g_carry++;
	ctx->a[0] = base - 4;
	ctx->a[1] = base;
}

/* $007376: the body above, stopped just before the movem/rts at $00739A (see the frame
 * note in the header). The 68k epilogue then runs and returns to the wrapper's caller. */
HLE_RANGE(0x007376, AddScoreBcd, 150, 0x00739A)
{
	addscorebcd_impl(ctx);
	hle_jump(ctx, 0x00739A);
}

/* The 14-byte stubs. Each saves d0/a0-a1, sets d0, tail-jumps to AddScoreBcd. The C
 * version calls the AddScoreBcd C code directly and restores the three registers. */
#define ADDSCORE_STUB(ADDR, NAME, VALUE)                     \
	HLE_FUNC(ADDR, NAME, 256)                             \
	{                                                    \
		uint32_t d0 = ctx->d[0], a0 = ctx->a[0], a1 = ctx->a[1]; \
		ctx->d[0] = (VALUE);                         \
		addscorebcd_impl(ctx);                       \
		ctx->d[0] = d0;                              \
		ctx->a[0] = a0;                              \
		ctx->a[1] = a1;                              \
	}

ADDSCORE_STUB(0x0072F8, AddScore250, 0x250)
ADDSCORE_STUB(0x007306, AddScore500, 0x500)
ADDSCORE_STUB(0x007314, AddScore750, 0x750)
ADDSCORE_STUB(0x007322, AddScore1000, 0x1000)
ADDSCORE_STUB(0x007330, AddScore1250, 0x1250)
ADDSCORE_STUB(0x00733E, AddScore2000, 0x2000)
ADDSCORE_STUB(0x00734C, AddScore4000, 0x4000)
ADDSCORE_STUB(0x00735A, AddScore5000, 0x5000)
ADDSCORE_STUB(0x007368, AddScore10000, 0x10000)

HLE_FUNC(0x007A92, BcdAdd, 7893)
{
	uint32_t d4 = ctx->d[4];
	uint32_t d3 = ctx->d[3] & 0xFFFF;   /* dbra uses the low word */
	uint32_t d3_first = d3;
	uint32_t d7 = 0;

	if (score_stats_on() && d3 < 16)
		g_d3[d3]++;

	for (;;) {
		uint32_t d0 = 1;            /* moveq #1,d0; then mul10 d3 times -> 10^d3 */
		for (uint32_t i = 0; i < d3; i++)
			d0 *= 10;
		uint32_t d6 = 0xFF;         /* st.b d6 */
		uint32_t d1 = 0;
		do {                        /* count how many d0 fit in d4 (signed cmp) */
			d6 = (d6 + 1) & 0xFF;
			d1 += d0;
		} while ((int32_t)d4 >= (int32_t)d1);
		d1 -= d0;
		d4 -= d1;                   /* the remainder feeds the lower digits */
		d6 = (d6 + 0x30) & 0xFF;    /* to ASCII */
		RAM8(BCD_OUT + d7) = (uint8_t)d6;
		d7 = (d7 + 1) & 0xFFFF;     /* addq.w #1,d7 */
		if (d3 == 0)                /* dbra d3 */
			break;
		d3--;
	}
	RAM8(BCD_OUT + d7) = 0;             /* clr.b (a1,d7.w): N=0 Z=1 V=C=0 */

	/* X is left by the final addq.w #1,d7 (d7 was d3_first before it) */
	uint32_t xf = (d3_first == 0xFFFF) ? CC_X : 0;
	cc_set(ctx, CC_X | CC_N | CC_Z | CC_V | CC_C, xf | CC_Z);
}
