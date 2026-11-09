/* Widescreen view (GitLab github/t2-decomp#13), see docs/widescreen.md.
 *
 * Presentation only: a second, wider frame is drawn from the same live VDP state at the same
 * per-line moments as the normal 320-pixel frame (render_emu.c calls the hooks below next to its
 * own). Nothing here writes to the emulated machine, so the game state is exactly that of the
 * normal view. Lines the game has no data for (HUD bars, text screens, menus, logos) are the
 * 320-pixel line centred with black sides. Needs the C renderer (render_emu.c turns it on). */
#ifndef T2_RENDER_WIDE_H
#define T2_RENDER_WIDE_H
#include <stdbool.h>
#include <stdint.h>
#include "render.h"

/* spec: "off", "16:9", "21:9", W:H, or a width in pixels (320..640, rounded up to a multiple of
 * 16); NULL reads the T2_WIDE environment variable, then "off". false for a bad spec. */
bool render_wide_configure(const char *spec);
bool render_wide_enabled(void);            /* configured on */
int render_wide_width(void);               /* output width (0 when off) */
int render_wide_margin(void);              /* pixels added on each side of the 320 */

/* the hooks (render_emu.c). `masking_before` is the sprite-masking flag before the normal line. */
void render_wide_line(const render_ctx *rc43, uint8_t masking_before, const render_vdp *v, int line);
void render_wide_remap(const render_vdp *v, int line, const uint32_t *line43);
void render_wide_frame_end(long frame, int h);

/* the last complete wide frame (RGBA, pitch in pixels) or NULL; true when this frame is shown wide
 * (false: a 320-pixel screen, pillarboxed inside the same size so the window never changes size) */
const uint32_t *render_wide_frame(int *w, int *h, int *pitch);
bool render_wide_frame_is_wide(void);

/* the same frame as RGB565 for emu_video(); false when there is none */
bool render_wide_frame565(uint16_t *dst, unsigned *w, unsigned *h);

#endif
