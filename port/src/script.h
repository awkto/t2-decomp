/* Input/command scripts, compatible with tools/harness scripts:
 *   run N            advance N frames with the held pad buttons (none by default)
 *   press BTNS N     hold pad BTNS (letters U D L R A B C S) for N frames, then release
 *   hold BTNS | hold -   set / clear pad buttons held during `run`
 *   mouse X Y        aim the Menacer at game pixel X,Y (no frame passes)
 *   mouse leave      point it away from the screen
 *   click L|R|M [N]  hold the left (trigger) / right / middle (Start) mouse button for N frames
 *                    (default 6); the frames run, then the button is released
 *   shot NAME        OUTDIR/NAME.ppm          ram NAME   OUTDIR/NAME.ram (68k byte order)
 *   save NAME / load NAME   OUTDIR/NAME.state (NAME containing '/' is used as a path)
 *   echo TEXT        stderr and "FRAME TEXT" in OUTDIR/peek.log
 *   peek LABEL ADDR [W]   append "frame LABEL value" (W bytes, hex) to OUTDIR/peek.log
 *   poke ADDR W VAL  write W (1/2/4) bytes of work RAM, big-endian hex VAL
 *   trace/untrace    ignored (harness only)
 * Blank lines and lines starting with '#' are ignored. */
#ifndef T2_SCRIPT_H
#define T2_SCRIPT_H
#include <stdbool.h>

typedef struct script script;

script *script_open(const char *path, const char *outdir);
/* Execute commands until the next frame must be emulated; returns true and the pad mask
 * for that frame, or false when the script has ended. */
bool script_step(script *s, unsigned *pad);
int script_errors(const script *s);
void script_close(script *s);

#endif
