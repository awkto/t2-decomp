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

/* The common TH read. `data` is the port's data register, `dest` the button word. */
static void read_pad(hle_ctx *ctx, uint32_t data, uint32_t dest)
{
	bus_w16(Z80_BUSREQ, 0x0100);            /* move.w #$100,($A11100).l */
	uint8_t v;
	long guard = 0;
	do {
		v = bus_r8(Z80_BUSREQ);         /* move.b ($A11100).l,d0 / andi.b #1 / bne */
	} while ((v & 1) && ++guard < 1000000);

	bus_w8(data, 0x00);                     /* TH = 0 */
	uint8_t b0 = bus_r8(data);              /* read: ..SA..DU */
	bus_w8(data, 0x40);                     /* TH = 1 */
	uint8_t b1 = bus_r8(data);              /* read: ..CBRLDU */
	bus_w8(data, 0x00);                     /* TH = 0 again */

	uint32_t low = ((uint32_t)((b0 >> 4) & 0x3) << 6) | (b1 & 0x3F);
	uint32_t out = (~low) & 0xFF;           /* not.b d0 (bits 8-15 are 0) */

	RAM16(dest + 2) = RAM16(dest);          /* previous buttons */
	RAM16(dest) = (uint16_t)out;

	set_w(&ctx->d[0], out);                 /* move.w d0,(dest).l */
	set_w(&ctx->d[1], b1 & 0x3F);           /* d1 low word = TH=1 byte & $3F */
	ctx->sr = 0x2000;                       /* move.w #$2000,sr: X N Z V C cleared */
	bus_w16(Z80_BUSREQ, 0x0000);            /* move.w #$0,($A11100).l */
}

HLE_FUNC(0x0088A0, ReadPad1, 290)
{
	read_pad(ctx, PAD1_DATA, PAD1_WORD);
}

HLE_FUNC(0x008908, ReadPad2, 290)
{
	read_pad(ctx, PAD2_DATA, PAD2_WORD);
}
