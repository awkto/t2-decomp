/* Widescreen view (github/t2-decomp#13): see render_wide.h and docs/widescreen.md. */
#include "render_wide.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "emu.h"

enum { K_NONE = 0, K_WIDE, K_PILLAR };

/* The planes are 64 cells = 512 pixels; the 4:3 picture needs 41 of them and each margin cells of its
 * own, so the margins together can be at most 22 cells: 80 pixels each, 480 wide (2.14:1; 21:9 is capped). */
#define WIDE_MAX_W 480

static int g_w, g_m;                       /* output width, margin on each side (0 = off) */
static render_ctx g_rc;                    /* the wide line's context */
static uint32_t g_fb[RENDER_MAX_W * RENDER_MAX_H];
static uint8_t g_kind[RENDER_MAX_H];       /* per line of the frame being drawn */
static int g_done, g_h;
static bool g_have, g_have_wide, g_frame_wide;
static uint16_t g_px[RENDER_MAX_W * RENDER_MAX_H];
static bool g_debug, g_stats, g_nohist;
static char g_last_feat[200];

/* what the margins show (see "The margins" in docs/widescreen.md) */
static uint32_t g_ov[2 * 2048];            /* render_vdp.wide_hist: left-margin cells that were overwritten since */
static uint16_t g_last[2 * 2048];          /* the last value a cell had while it was in the 4:3 picture */
static uint8_t g_seen[2 * 2048];
static uint16_t g_snap[2 * 2048];          /* check: the value of a cell as the right margin showed it */
static uint8_t g_snapv[2 * 2048];
static unsigned g_level_prev = ~0u;

/* counters (T2_WIDE_STATS=1 prints them at exit) */
static long c_frames, c_wide, c_pillar, c_lines, c_centre_bad, c_stale, c_stale_frames, c_wrapped, c_vs_odd;

/* work RAM (data/ram.txt) */
#define RAM_LEVEL 0xFF80CC

static void stats_print(void)
{
	fprintf(stderr, "wide: %ld frames (%ld wide, %ld pillarboxed), %ld wide lines, %ld centre differences,"
	        " right-margin cells stale %ld (in %ld frames), %ld sprites with x outside 9 bits, %ld plane-frames with an odd v-scroll\n",
	        c_frames, c_wide, c_pillar, c_lines, c_centre_bad, c_stale, c_stale_frames, c_wrapped, c_vs_odd);
}

bool render_wide_enabled(void) { return g_w != 0; }
int render_wide_width(void) { return g_w; }
int render_wide_margin(void) { return g_m; }

bool render_wide_configure(const char *spec)
{
	if (!spec || !*spec)
		spec = getenv("T2_WIDE");
	if (!spec || !*spec || !strcasecmp(spec, "off") || !strcmp(spec, "0") || !strcasecmp(spec, "no") ||
	    !strcasecmp(spec, "false") || !strcasecmp(spec, "4:3")) {
		g_w = g_m = 0;
		return true;
	}
	double aspect = 0;
	int a, b, w;
	if (!strcasecmp(spec, "on") || !strcasecmp(spec, "yes") || !strcasecmp(spec, "true"))
		aspect = 16.0 / 9.0;
	else if (sscanf(spec, "%d:%d", &a, &b) == 2 && a > 0 && b > 0)
		aspect = (double)a / b;
	else if (sscanf(spec, "%d", &w) == 1 && w >= 320 && w <= 640 && !strchr(spec, ':'))
		aspect = (double)w / 224;
	else
		return false;
	int ww = (int)(224 * aspect + 0.999);
	ww = (ww + 15) & ~15;                  /* whole 2-cell columns, an even margin of whole cells */
	if (ww > WIDE_MAX_W)
		ww = WIDE_MAX_W;
	if (ww <= RENDER_W43) {
		g_w = g_m = 0;
		return true;
	}
	g_w = ww;
	g_m = (ww - RENDER_W43) / 2;
	g_debug = getenv("T2_WIDE_DEBUG") != NULL;
	g_stats = getenv("T2_WIDE_STATS") != NULL;
	g_nohist = getenv("T2_WIDE_NOHIST") != NULL;      /* development: show the planes as they are in the left margin too */
	if (g_stats)
		atexit(stats_print);
	return true;
}

/* ------------------------------------------------------------------ what is shown wide */
/* The planes hold game data for the scrolling gameplay layout only: 64 x 32 cells, H40, per-cell
 * h-scroll, full-screen v-scroll, no window. Every other screen (logos, title, menus, text, the
 * continue and high-score screens) is a 320-pixel picture and is pillarboxed. */
static bool frame_is_wide(const render_vdp *v)
{
	const uint8_t *r = v->reg;
	if (!(r[12] & 1) || v->width != 320)
		return false;
	if ((r[16] & 0x33) != 0x01)            /* planes 64 x 32 */
		return false;
	if ((r[11] & 7) != 2)                  /* cell h-scroll, full-screen v-scroll */
		return false;
	if (r[17] || r[18])                    /* no window */
		return false;
	unsigned level = emu_ram16(RAM_LEVEL);
	if (level < 1 || level > 8)
		return false;
	return true;
}

static void debug_features(const render_vdp *v, bool wide, long frame)
{
	const uint8_t *r = v->reg;
	char f[200];
	snprintf(f, sizeof f, "wide=%d lvl=%u r0=%02X r1=%02X r2=%02X r3=%02X r4=%02X r11=%02X r12=%02X r13=%02X r16=%02X r17=%02X r18=%02X w=%d",
	         wide, emu_ram16(RAM_LEVEL), r[0], r[1], r[2], r[3], r[4], r[11], r[12], r[13], r[16], r[17], r[18], v->width);
	if (strcmp(f, g_last_feat)) {
		fprintf(stderr, "wide: frame %ld: %s\n", frame, f);
		strcpy(g_last_feat, f);
	}
}


/* ------------------------------------------------------------------ the margins */
/* The planes are 64 cells wide, the picture shows 41 of them, so 23 cells are off screen. The game's
 * streamer (BgStreamLayers) writes the next block of the level into the cells that have just scrolled
 * off the left; the cells ahead of the right edge are streamed 8 or more cells before they come in.
 *   right margin: the plane as it is (checked below: stale cells are counted);
 *   left margin:  the cells just scrolled off the left are overwritten by the streamer with data for the
 *                 far right, so each cell is shown as it was the last time it was in the 4:3 picture
 *                 (g_last), which is exactly what scrolled off.
 * Cell p of a row is p cells right of the picture's first cell: p 0..40 are in the picture. Per frame,
 * per plane and name-table row (the row's h-scroll is that of the screen row it is drawn on). */
static void track_reset(void)
{
	memset(g_seen, 0, sizeof g_seen);
	memset(g_snapv, 0, sizeof g_snapv);
	memset(g_ov, 0, sizeof g_ov);
}

static void track_frame(const render_vdp *v)
{
	const uint8_t *r = v->reg;
	int nl = (g_m + 7) / 8;                           /* cells of the left margin */
	int nr = (g_m + 7) / 8 + 1;                       /* right margin (the picture's partial cell is p 40) */
	uint32_t nt[2] = { ((uint32_t)r[2] << 10) & 0xE000, ((uint32_t)r[4] << 13) & 0xE000 };
	uint32_t hscb = ((uint32_t)r[13] << 10) & 0xFC00;
	bool stale_frame = false;
	for (int pl = 0; pl < 2; pl++) {
		int vs = v->vsram[pl];                        /* plane A: word 0, B: word 1 (full-screen v-scroll) */
		if (vs & 7)
			c_vs_odd++;
		for (int row = 0; row < 32; row++) {
			/* the screen cell row(s) this name-table row is drawn on: two when the v-scroll is not a multiple of 8 */
			int sr = (((row * 8 - vs) & 255) >> 3), sr2 = (vs & 7) ? (sr + 1) & 31 : sr;
			unsigned hs = v->vram[((hscb + (uint32_t)sr * 32 + (uint32_t)pl * 2) & 0xFFFF) >> 1];
			unsigned hs2 = v->vram[((hscb + (uint32_t)sr2 * 32 + (uint32_t)pl * 2) & 0xFFFF) >> 1];
			int vl = (int)(((0u - hs) & 0x3FF) >> 3), vl2 = (int)(((0u - hs2) & 0x3FF) >> 3);
			for (int c = 0; c < 64; c++) {
				int p = (c - vl) & 63, p2 = (c - vl2) & 63, i = pl * 2048 + row * 64 + c;
				uint16_t w = v->vram[((nt[pl] + (uint32_t)(row * 64 + c) * 2) & 0xFFFF) >> 1];
				if (p <= 40 || p2 <= 40) {
					g_last[i] = w;
					g_seen[i] = 1;
					g_ov[i] = 0;
					if (g_snapv[i] && g_snap[i] != w) {
						if (g_debug && c_stale < 12)
							fprintf(stderr, "wide: stale cell plane %c row %d col %d p %d: %04X -> %04X (frame %ld)\n",
							        pl ? 'B' : 'A', row, c, p, g_snap[i], w, emu_frame_count());
						c_stale++;
						stale_frame = true;
					}
					g_snapv[i] = 0;
				} else if (p >= 64 - nl && p2 >= 64 - nl) {
					g_ov[i] = g_seen[i] ? (0x10000u | g_last[i]) : 0;
					g_snapv[i] = 0;
				} else {
					g_seen[i] = 0;
					g_ov[i] = 0;
					if (p <= 40 + nr) {
						g_snap[i] = w;
						g_snapv[i] = 1;
					} else {
						g_snapv[i] = 0;
					}
				}
			}
		}
	}
	if (stale_frame) {
		c_stale_frames++;
		if (g_debug)
			fprintf(stderr, "wide: stale right-margin cells in frame %ld (level %u)\n", emu_frame_count(), emu_ram16(RAM_LEVEL));
	}
}

static int line_kind(const render_vdp *v, int line)
{
	if (!g_frame_wide)
		return K_PILLAR;
	if (v->reg[0] & 0x20)
		return K_PILLAR;
	return K_WIDE;
}

void render_wide_line(const render_ctx *rc43, uint8_t masking_before, const render_vdp *v, int line)
{
	if (!g_w || line < 0 || line >= RENDER_MAX_H)
		return;
	if (line == 0) {
		memset(g_kind, 0, sizeof g_kind);
		g_done = 0;
		g_frame_wide = frame_is_wide(v);
		c_frames++;
		if (g_frame_wide) {
			c_wide++;
			unsigned level = emu_ram16(RAM_LEVEL);
			if (level != g_level_prev)
				track_reset();
			g_level_prev = level;
			track_frame(v);
		} else {
			c_pillar++;
			track_reset();
			g_level_prev = ~0u;
		}
		if (g_debug)
			debug_features(v, g_frame_wide, emu_frame_count());
	}
	int k = line_kind(v, line);
	g_kind[line] = (uint8_t)k;
	if (k != K_WIDE)
		return;
	if (g_stats)
		for (int i = 0; i < rc43->obj_count[line & 1]; i++)
			if (rc43->obj[line & 1][i].xfull & ~0x1FFu) {
				if (++c_wrapped <= 8 && g_debug)
					fprintf(stderr, "wide: sprite x word %04X (y %u attr %04X) frame %ld line %d\n",
					        rc43->obj[line & 1][i].xfull, rc43->obj[line & 1][i].y, rc43->obj[line & 1][i].attr, emu_frame_count(), line);
			}
	memcpy(g_rc.obj, rc43->obj, sizeof g_rc.obj);
	memcpy(g_rc.obj_count, rc43->obj_count, sizeof g_rc.obj_count);
	g_rc.spr_masking = masking_before;
	render_vdp wv = *v;
	wv.out_w = g_w;
	wv.xbias = g_m;
	wv.wide_hist = g_nohist ? NULL : g_ov;
	wv.wide_lz = 64 - (g_m + 7) / 8;
	render_line_pixels(&g_rc, &wv, line);
	c_lines++;
	if (memcmp(g_rc.idx + g_m, rc43->idx, RENDER_W43))      /* the 4:3 part of the wide line is the 4:3 line */
	{
		c_centre_bad++;
		if (g_debug && c_centre_bad <= 12) {
			int x = 0;
			while (g_rc.idx[g_m + x] == rc43->idx[x])
				x++;
			fprintf(stderr, "wide: centre differs: frame %ld line %d first x %d wide %02X 4:3 %02X (r0=%02X r11=%02X r17=%02X r18=%02X)\n",
			        emu_frame_count(), line, x, g_rc.idx[g_m + x], rc43->idx[x], v->reg[0], v->reg[11], v->reg[17], v->reg[18]);
		}
	}
}

void render_wide_remap(const render_vdp *v, int line, const uint32_t *line43)
{
	if (!g_w || line < 0 || line >= RENDER_MAX_H || !g_kind[line])
		return;
	uint32_t *dst = g_fb + (size_t)line * RENDER_MAX_W;
	if (g_kind[line] == K_WIDE) {
		render_line_colors(&g_rc, v, dst);
	} else {
		int w43 = v->width, off = (g_w - w43) / 2;
		for (int x = 0; x < g_w; x++)
			dst[x] = 0xFF000000u;
		memcpy(dst + off, line43, (size_t)w43 * sizeof dst[0]);
	}
	g_done++;
}

void render_wide_frame_end(long frame, int h)
{
	(void)frame;
	g_have = g_w && g_done == h;
	g_have_wide = g_have && g_frame_wide;
	g_h = h;
	if (g_have)
		for (int y = 0; y < h; y++)
			for (int x = 0; x < g_w; x++)
				g_px[y * g_w + x] = render_rgba_to_565(g_fb[y * RENDER_MAX_W + x]);
}

const uint32_t *render_wide_frame(int *w, int *h, int *pitch)
{
	if (!g_have)
		return NULL;
	*w = g_w;
	*h = g_h;
	*pitch = RENDER_MAX_W;
	return g_fb;
}

bool render_wide_frame_is_wide(void) { return g_have_wide; }

bool render_wide_frame565(uint16_t *dst, unsigned *w, unsigned *h)
{
	if (!g_have)
		return false;
	memcpy(dst, g_px, (size_t)g_w * (size_t)g_h * sizeof g_px[0]);
	*w = (unsigned)g_w;
	*h = (unsigned)g_h;
	return true;
}
