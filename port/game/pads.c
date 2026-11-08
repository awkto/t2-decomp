/* ReadPad1 $0088A0 / ReadPad2 $008908: read one 3-button pad through the I/O data port
 * (port 1 = $A10003 -> $FF8050, port 2 = $A10005 -> $FF8000). docs/gun.md section 1.
 *
 * Original (src/t2.s, $0088A0 / $008908; the two differ only in the port/address):
 *     move.w  #$2700,sr                  ; mask interrupts
 *     move.w  #$100,($A11100).l          ; request the Z80 bus
 * .wait:
 *     move.b  ($A11100).l,d0             ; ... and spin until BUSACK (bit 0 clear)
 *     andi.b  #$1,d0
 *     bne.s   .wait
 *     move.b  #$0,($A10003).l            ; TH = 0
 *     move.b  ($A10003).l,d0             ; ..SA..DU  (A/Start)
 *     move.b  #$40,($A10003).l           ; TH = 1
 *     lsl.w   #$2,d0
 *     andi.w  #$C0,d0
 *     move.b  ($A10003).l,d1             ; ..CBRLDU
 *     andi.w  #$3F,d1
 *     or.w    d1,d0
 *     move.b  #$0,($A10003).l            ; TH = 0
 *     move.w  ($FFFF8050).l,($FFFF8052).l  ; previous buttons
 *     not.b   d0                         ; pad is active low -> active high
 *     move.w  d0,($FFFF8050).l
 *     move.w  #$0,($A11100).l            ; release the Z80 bus
 *     move.w  #$2000,sr                  ; restore SR (all flags cleared)
 *     rts
 *
 * The first read (TH=0) carries A in bit 4 and Start in bit 5; those two bits are shifted
 * left by 2 into bits 6/7. The second read (TH=1) carries B/C in bits 4/5; its low 6 bits
 * (U D L R B C) are OR-ed in. The result is inverted so 1 = pressed. Only d0 (partial: the
 * low word is built, the high word survives) and d1 (low word = the TH=1 byte & $3F) are
 * written; a0-a7 and d2-d7 are untouched.
 *
 * Every I/O access is reproduced in order through the bus accessors: the Z80 bus request,
 * the two TH toggles with a read after each, and the final TH=0 write. The BUSACK spin is
 * emulated too (it ends at once when the bus is already granted, which is the case for the
 * captured states); the guard only stops a runaway if the emulated bus never grants.
 *
 * RAM (data/ram.txt): $FF8050 = pad 1 buttons / $FF8000 = pad 2 buttons (word, previous at
 * +2); $FF8050 is also P2's struct and $FF8000 P1's, swapped around the calls in pad modes.
 */
#include "hle.h"

#define Z80_BUSREQ 0xA11100
#define PAD1_DATA  0xA10003
#define PAD2_DATA  0xA10005
#define PAD1_WORD  0xFF8050
#define PAD2_WORD  0xFF8000

/* The common TH read. `data` is the port's data register, `dest` the button word.
 *
 * Timing (docs/porting.md, "The instruction timeline"): the Z80 bus request makes GPGX resync the
 * Z80 up to the 68k clock, and a Z80 read of 68k ROM then stalls the 68k by ~10 cycles
 * (memz80.c), so WHEN the I/O accesses happen decides how the two CPUs interleave. Every
 * access therefore goes through hle_pre()/hle_post() with the original's 68000 instruction
 * costs: move to SR #imm 16, move.w #imm,(abs).l 20, move.b (abs).l,Dn 16, andi.b/andi.w #imm,Dn 8,
 * bne.s 10 taken / 8 not, move.b #imm,(abs).l 20, lsl.w #2 10, or.w 4, move.w (abs).l,(abs).l 28,
 * not.b 4, move.w Dn,(abs).l 16, rts 16 (total 290).
 *
 * Interrupt-exact (docs/porting.md, "Yielding"): a coroutine. Each instruction's effect comes first, then its
 * charge; SR is $2700 from the first instruction on (a yield at a slice end inside the call leaves the
 * interrupt pending, as on the 68k) and $2000 after `move.w #$2000,sr`, where a pending interrupt is taken. */
static void read_pad(hle_ctx *ctx, uint32_t data, uint32_t dest)
{
	ctx->sr = 0x2700;                       /* move.w #$2700,sr */
	hle_insn(ctx, 16);
	hle_pre(ctx);
	bus_w16(Z80_BUSREQ, 0x0100);            /* move.w #$100,($A11100).l */
	hle_post(ctx, 20);
	uint8_t v;
	long guard = 0;
	for (;;) {
		hle_pre(ctx);
		v = bus_r8(Z80_BUSREQ);         /* move.b ($A11100).l,d0 */
		hle_post(ctx, 16);
		hle_insn(ctx, 8);               /* andi.b #1,d0 */
		if (!(v & 1) || ++guard >= 1000000) {
			hle_insn(ctx, 8);       /* bne.s not taken */
			break;
		}
		hle_insn(ctx, 10);              /* bne.s taken */
	}

	hle_pre(ctx);
	bus_w8(data, 0x00);                     /* TH = 0 */
	hle_post(ctx, 20);
	hle_pre(ctx);
	uint8_t b0 = bus_r8(data);              /* read: ..SA..DU */
	hle_post(ctx, 16);
	hle_pre(ctx);
	bus_w8(data, 0x40);                     /* TH = 1 */
	hle_post(ctx, 20);
	hle_insn(ctx, 10);                      /* lsl.w #2,d0 */
	hle_insn(ctx, 8);                       /* andi.w #$C0,d0 */
	hle_pre(ctx);
	uint8_t b1 = bus_r8(data);              /* read: ..CBRLDU */
	hle_post(ctx, 16);
	hle_insn(ctx, 8);                       /* andi.w #$3F,d1 */
	hle_insn(ctx, 4);                       /* or.w d1,d0 */
	hle_pre(ctx);
	bus_w8(data, 0x00);                     /* TH = 0 again */
	hle_post(ctx, 20);

	uint32_t low = ((uint32_t)((b0 >> 4) & 0x3) << 6) | (b1 & 0x3F);
	uint32_t out = (~low) & 0xFF;           /* not.b d0 (bits 8-15 are 0) */

	RAM16(dest + 2) = RAM16(dest);          /* move.w (dest).l,(dest+2).l: previous buttons */
	hle_insn(ctx, 28);
	hle_insn(ctx, 4);                       /* not.b d0 */
	RAM16(dest) = (uint16_t)out;            /* move.w d0,(dest).l */
	hle_insn(ctx, 16);

	set_w(&ctx->d[0], out);
	set_w(&ctx->d[1], b1 & 0x3F);           /* d1 low word = TH=1 byte & $3F */
	hle_pre(ctx);
	bus_w16(Z80_BUSREQ, 0x0000);            /* move.w #$0,($A11100).l */
	hle_post(ctx, 20);
	ctx->sr = 0x2000;                       /* move.w #$2000,sr: X N Z V C cleared, mask 0 */
	hle_insn(ctx, 16);
	hle_irq_poll(ctx);                      /* a pending interrupt is taken here */
	hle_insn(ctx, 16);                      /* rts */
}

HLE_FUNC_LOWMASK_YIELD(0x0088A0, ReadPad1, 330)
{
	read_pad(ctx, PAD1_DATA, PAD1_WORD);
}

HLE_FUNC_LOWMASK_YIELD(0x008908, ReadPad2, 330)
{
	read_pad(ctx, PAD2_DATA, PAD2_WORD);
}
