/* Native renderer <-> emulator bridge (github/t2-decomp#12), see docs/render.md. */
#ifndef T2_RENDER_EMU_H
#define T2_RENDER_EMU_H
#include <stdbool.h>
#include <stdint.h>
#include "render.h"

typedef enum {
	RENDER_GPGX = 0,       /* GPGX's picture (default) */
	RENDER_C,              /* the C renderer's picture replaces it */
	RENDER_COMPARE,        /* both are drawn every frame, GPGX's is shown, differences are reported */
} render_mode;

/* Select the renderer: "gpgx", "c" or "compare" (also "native", "off"). NULL/"" reads the
 * T2_RENDER environment variable. false for an unknown name. Call before the first frame. */
bool render_emu_configure(const char *name);
render_mode render_emu_mode(void);

/* emu.c, after each emulated frame. `px` is the frame as RGB565 (w x h, pitch w): in C mode it
 * is overwritten with the C renderer's picture; in compare mode it is checked against it. */
void render_emu_frame_end(long frame, uint16_t *px, unsigned w, unsigned h);

/* The C renderer's last complete frame (RGBA, pitch in pixels); NULL if none was drawn.
 * This is the entry point for a presentation path that wants more than RGB565. */
const uint32_t *render_emu_frame(int *w, int *h, int *pitch);

/* Compare statistics: frames compared, frames that differ, pixels that differ in total. */
void render_emu_stats(long *frames, long *bad, long *pixels);
#endif
