/* Native renderer <-> emulator bridge (github/t2-decomp#12), see render_emu.h and docs/render.md.
 *
 * Raster mode: GPGX calls three hooks (tools/patches/gpgx-render.patch) at the moments it renders
 * a line, parses the sprite table for the next line and converts a line to colours. They draw
 * the same line from the same live VDP state (the view points at GPGX's memories), so a change
 * the game makes in the middle of the frame lands on the same line in both pictures. */
#include "render_emu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "gx.h"
#include "render_wide.h"
#include "hifps.h"

static render_mode g_mode = RENDER_GPGX;
static bool g_atexit, g_hooked;
static render_ctx g_rc;
static uint32_t g_fb[RENDER_MAX_W * RENDER_MAX_H];
static bool g_line_done[RENDER_MAX_H];
static int g_lines_done, g_fb_w, g_fb_h;
static bool g_have_frame;

static long g_frames, g_bad, g_pixels;
static long g_first_bad = -1;
static char g_dump_dir[512];
static int g_dumps;

static render_vdp live_view(void)
{
	render_vdp v = { 0 };
	v.vram = (const uint16_t *)gx_vram();
	v.cram = (const uint16_t *)gx_cram();
	v.vsram = (const uint16_t *)gx_vsram();
	v.reg = gx_vdp_reg();
	v.sat = (const uint16_t *)gx_sat();
	v.width = gx_viewport_w();
	return v;
}

static void on_line(int line)
{
	if (line < 0 || line >= RENDER_MAX_H)
		return;
	if (line == 0)
		g_lines_done = 0, memset(g_line_done, 0, sizeof g_line_done);
	render_vdp v = live_view();
	if (line == 0)
		hifps_line0(&v, gx_viewport_h());      /* high frame rate: the scene of this frame (docs/hifps.md) */
	else
		hifps_line(&v, line);
	uint8_t masking = g_rc.spr_masking;
	render_line_pixels(&g_rc, &v, line);
	render_wide_line(&g_rc, masking, &v, line);      /* widescreen (#13): the same line, wider; no-op when off */
}

static void on_parse(int line)
{
	render_vdp v = live_view();
	hifps_parse(&v, line);
	render_parse_sprites(&g_rc, &v, line);
}

static void on_remap(int line)
{
	if (line < 0 || line >= RENDER_MAX_H || line >= gx_viewport_h())
		return;
	render_vdp v = live_view();
	render_line_colors(&g_rc, &v, g_fb + (size_t)line * RENDER_MAX_W);
	render_wide_remap(&v, line, g_fb + (size_t)line * RENDER_MAX_W);
	if (!g_line_done[line]) {
		g_line_done[line] = true;
		g_lines_done++;
	}
	g_fb_w = g_rc.width;
	g_fb_h = gx_viewport_h();
	if (line == g_fb_h - 1)
		hifps_lastline();
}

static void report(void)
{
	if (g_mode != RENDER_COMPARE)
		return;
	fprintf(stderr, "rendercmp: %ld frames compared, %ld differ, %ld pixels differ%s\n",
	        g_frames, g_bad, g_pixels, g_bad ? "" : " (all identical)");
	if (g_bad)
		fprintf(stderr, "rendercmp: first differing frame %ld\n", g_first_bad);
}

bool render_emu_configure(const char *name)
{
	if (!name || !*name)
		name = getenv("T2_RENDER");
	if (!name || !*name)
		name = "gpgx";
	render_mode m;
	if (!strcasecmp(name, "gpgx") || !strcasecmp(name, "off") || !strcmp(name, "0"))
		m = RENDER_GPGX;
	else if (!strcasecmp(name, "c") || !strcasecmp(name, "native") || !strcmp(name, "1"))
		m = RENDER_C;
	else if (!strcasecmp(name, "compare") || !strcasecmp(name, "cmp"))
		m = RENDER_COMPARE;
	else
		return false;
	if (m == RENDER_GPGX && render_wide_enabled())
		m = RENDER_C;          /* the widescreen view (#13) is drawn by the C renderer */
	g_mode = m;
	if (m != RENDER_GPGX && !g_hooked) {
		render_tables_init();
		render_ctx_reset(&g_rc);
		gx_set_render_hooks(on_line, on_parse, on_remap);
		g_hooked = true;
		const char *d = getenv("T2_RENDER_DUMP");
		if (d)
			snprintf(g_dump_dir, sizeof g_dump_dir, "%s", d);
		if (!g_atexit) {
			g_atexit = true;
			atexit(report);
		}
	} else if (m == RENDER_GPGX && g_hooked) {
		gx_set_render_hooks(NULL, NULL, NULL);
		g_hooked = false;
	}
	return true;
}

render_mode render_emu_mode(void) { return g_mode; }

const uint32_t *render_emu_frame(int *w, int *h, int *pitch)
{
	if (!g_have_frame)
		return NULL;
	*w = g_fb_w;
	*h = g_fb_h;
	*pitch = RENDER_MAX_W;
	return g_fb;
}

void render_emu_stats(long *frames, long *bad, long *pixels)
{
	*frames = g_frames;
	*bad = g_bad;
	*pixels = g_pixels;
}

static void dump_ppm(const char *path, const uint16_t *px, unsigned w, unsigned h, const uint32_t *rgba)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return;
	fprintf(f, "P6\n%u %u\n255\n", w, h);
	for (unsigned y = 0; y < h; y++)
		for (unsigned x = 0; x < w; x++) {
			uint32_t c = rgba ? rgba[y * RENDER_MAX_W + x] : render_565_to_rgba(px[y * w + x]);
			uint8_t rgb[3] = { (uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16) };
			fwrite(rgb, 1, 3, f);
		}
	fclose(f);
}

void render_emu_frame_end(long frame, uint16_t *px, unsigned w, unsigned h)
{
	if (g_mode == RENDER_GPGX || !px)
		return;
	/* a frame the C renderer drew completely, at the size GPGX presented */
	bool complete = (unsigned)g_fb_w == w && (unsigned)g_fb_h == h && g_lines_done == (int)h;
	g_have_frame = complete;
	render_wide_frame_end(frame, complete ? (int)h : -1);
	if (hifps_enabled()) {
		/* the C renderer's frame is the whole frame R(k); it must be in px (c mode) or checked (compare) */
		if (g_mode == RENDER_C && complete)
			for (unsigned y = 0; y < h; y++)
				for (unsigned x = 0; x < w; x++)
					px[y * w + x] = render_rgba_to_565(g_fb[y * RENDER_MAX_W + x]);
		hifps_frame_end(px, w, h, complete);
	}
	if (g_mode == RENDER_C) {
		if (complete)
			for (unsigned y = 0; y < h; y++)
				for (unsigned x = 0; x < w; x++)
					px[y * w + x] = render_rgba_to_565(g_fb[y * RENDER_MAX_W + x]);
		return;
	}
	g_frames++;
	long diff = 0;
	int fx = -1, fy = -1;
	if (complete) {
		for (unsigned y = 0; y < h; y++)
			for (unsigned x = 0; x < w; x++)
				if (render_rgba_to_565(g_fb[y * RENDER_MAX_W + x]) != px[y * w + x]) {
					if (!diff) {
						fx = (int)x;
						fy = (int)y;
					}
					diff++;
				}
	}
	if (complete && !diff)
		return;
	g_bad++;
	g_pixels += diff;
	if (g_first_bad < 0)
		g_first_bad = frame;
	if (g_bad <= 20) {
		if (!complete)
			fprintf(stderr, "rendercmp: frame %ld: C frame incomplete or wrong size (%dx%d, %d lines; gpgx %ux%u)\n",
			        frame, g_fb_w, g_fb_h, g_lines_done, w, h);
		else
			fprintf(stderr, "rendercmp: frame %ld: %ld pixels differ, first at x=%d line=%d c=%04X gpgx=%04X\n",
			        frame, diff, fx, fy, render_rgba_to_565(g_fb[fy * RENDER_MAX_W + fx]), px[fy * w + fx]);
	}
	if (g_dump_dir[0] && g_dumps < 6) {
		char p[700];
		if (!g_dumps)
			mkdir(g_dump_dir, 0777);
		snprintf(p, sizeof p, "%s/f%07ld-c.ppm", g_dump_dir, frame);
		dump_ppm(p, NULL, w, h, g_fb);
		snprintf(p, sizeof p, "%s/f%07ld-gpgx.ppm", g_dump_dir, frame);
		dump_ppm(p, px, w, h, NULL);
		g_dumps++;
	}
}
