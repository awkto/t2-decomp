/* High frame rate presentation (GitLab github/t2-decomp#14), see docs/hifps.md.
 *
 * The game logic stays at 60 Hz. For every logic frame k the C renderer (render.c) draws the whole
 * frame R(k) as before; this module keeps a copy of the VDP state R(k) was drawn from (taken at
 * line 0 of the display) and draws n-1 in-between pictures from it with the scroll values and the
 * sprite positions moved back toward R(k-1): the in-between at fraction f (0 < f < 1) shows the
 * scene at (k-1) + f. Nothing is written to the emulated machine: it is presentation only.
 *
 * What moves: the per-row horizontal scroll of both planes, the vertical scroll, and the sprites of
 * the objects that the previous frame also drew (matched through the sprite queue, see hifps.c).
 * The Menacer crosshair is drawn at the pointer's position now in every picture, whole frames
 * included.
 *
 * Frames that cannot be interpolated (a scene cut, a layout change, a frame the frame renderer does
 * not reproduce exactly, unknown sprite owners) are held: the in-between pictures show R(k-1). */
#ifndef T2_HIFPS_H
#define T2_HIFPS_H
#include <stdbool.h>
#include <stdint.h>
#include "render.h"

enum { HIFPS_WHOLE = 0, HIFPS_INTERP, HIFPS_HELD };

/* Rate in Hz: 0 = off, else the number of pictures a second (61..1000). Returns the pictures per
 * logic frame n = round(rate * T) (>= 2), or 0 when off or when the rate is too low to add any. */
int hifps_configure(int hz);
bool hifps_enabled(void);
bool hifps_ready(void);                           /* a finished frame to draw from */
int hifps_per_frame(void);
void hifps_reset(void);                           /* state load / reset: forget the previous frame */

/* render_emu.c hooks (the live VDP at line 0 and at the end of the display, then the finished frame) */
void hifps_line0(const render_vdp *live, int height);
void hifps_line(const render_vdp *live, int line);    /* start of a line: the game changed the VDP during the display? */
void hifps_parse(const render_vdp *live, int line);   /* the sprite table for line + 1 is read */
void hifps_lastline(void);
void hifps_frame_end(const uint16_t *px, unsigned w, unsigned h, bool complete);

/* the host's pointer in game pixels: noted once per emulated frame (what the game reads), and now */
void hifps_note_mouse(int gx, int gy, bool off);
void hifps_mouse_now(int gx, int gy, bool off);

/* A picture of the scene at fraction f of the last logic step: f = 1 is R(k), 0 < f < 1 is in
 * between, f = 0 is R(k-1). RGB565 like emu_video, valid until the next call; NULL before the first
 * frame. *kind: HIFPS_WHOLE / INTERP / HELD. */
const uint16_t *hifps_picture(double f, unsigned *w, unsigned *h, int *kind);

/* run an eager pass (batch runs): draw and count the in-betweens of the frame just finished,
 * nothing is presented. Dumps pictures when T2_HIFPS_DUMP is set. */
void hifps_eager(void);
#endif
