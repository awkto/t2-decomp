/* Native renderer (GitLab github/t2-decomp#12): draws Genesis VDP mode-5 frames in C from VDP
 * state (VRAM / CRAM / VSRAM / registers / sprite cache) into an RGBA buffer.
 *
 * The renderer does not know about the emulator. It reads a `render_vdp` view of the VDP
 * memories and is driven line by line, in the order the VDP works:
 *
 *     render_parse_sprites(rc, v, -1);              sprites for line 0 (end of VBlank)
 *     for each line l:
 *         render_line_pixels(rc, v, l);             planes, window, sprites -> rc->idx
 *         render_parse_sprites(rc, v, l);           sprites for line l+1 (if l < h-1)
 *         render_line_colors(rc, v, fb + l*pitch);  rc->idx -> RGBA
 *
 * Two ways to call it (docs/render.md):
 *   - raster mode (render_emu.c): the emulator calls the three steps at the moments its own
 *     VDP renders, with the live VDP state, so every mid-frame change (a CRAM write in the
 *     middle of the frame, a scroll change on a line) is seen exactly where GPGX sees it;
 *   - frame mode (render_frame): one call per frame from a `render_scene` (a VDP snapshot).
 *     This is the contract for the widescreen / high-fps work (#13, #14), which build their
 *     own scene (wider viewport, interpolated scroll and sprite positions).
 *
 * Pixel semantics follow the VDP (and match GPGX bit for bit): 2-cell columns, h-scroll per
 * screen / cell / line, full or 2-cell v-scroll, window with its horizontal/vertical split,
 * sprites with the link list, 16/20 sprites and 256/320 pixels per line, sprite masking (x=0),
 * left column blanking, shadow/highlight, the 3-bit palette mode, H32 and H40.
 * Not supported (the game never uses them): interlace modes, mode 4 and below, 128 KiB VRAM.
 */
#ifndef T2_RENDER_H
#define T2_RENDER_H
#include <stdbool.h>
#include <stdint.h>

#define RENDER_W43 320            /* the VDP's own widest line (H40) */
#define RENDER_MAX_W 640          /* widest output: the widescreen view (render_wide.c, #13) */
#define RENDER_MAX_H 240

/* The VDP state the renderer reads. All pointers are read-only. */
typedef struct render_vdp {
	const uint16_t *vram;   /* 32 Ki words: VRAM byte address a is the word vram[a >> 1] */
	const uint16_t *cram;   /* 64 colours in the VDP's 9-bit form: bits 8-6 blue, 5-3 green, 2-0 red */
	const uint16_t *vsram;  /* 40 words: v-scroll, A/B alternating (word 0 = plane A, 1 = plane B) */
	const uint8_t *reg;     /* VDP registers 0..23 */
	const uint16_t *sat;    /* the VDP's internal sprite cache: 4 words per sprite, of which word 0
	                         * (y) and word 1 (size, link) are used; x and attribute come from VRAM.
	                         * It is updated by VRAM writes that hit the sprite table. */
	int width;              /* active width latched for this frame: 256 (H32) or 320 (H40) */
	/* wide view (#13): out_w > 0 draws out_w pixels per line, the VDP's own line sitting at
	 * x = xbias (output pixel 0 = screen x -xbias): both planes are scrolled xbias more, sprites
	 * are placed xbias further right. Sprite selection, the per-line limits and masking stay those
	 * of `width`. Both zero for the normal view. */
	int out_w, xbias;
	const uint32_t *wide_hist;   /* wide view: [2 planes][32 rows][64 cells]; a non-zero entry (bit 16 set,
	                              * low 16 bits = the name-table word) replaces that cell of plane A / B where it
	                              * is in the left margin: p >= wide_lz cells right of the picture's first cell */
	int wide_lz;
} render_vdp;

/* Everything one frame needs (frame mode). Widescreen and interpolation (#13, #14) add fields
 * here: a viewport rectangle, sub-pixel scroll values, a sprite list with fractional positions. */
typedef struct render_scene {
	render_vdp vdp;
	int height;              /* active lines: 224 (V28) or 240 (V30) */
} render_scene;

/* Renderer state that lives across lines and frames, like the VDP's internal state. */
typedef struct render_sprite { uint16_t y, x, attr, size, xfull; } render_sprite;   /* xfull: the whole x word, x = its low 9 bits */
typedef struct render_ctx {
	render_sprite obj[2][20];   /* sprites of the next line / this line (parity of the line) */
	uint8_t obj_count[2];
	uint8_t spr_masking;        /* sprite masking armed (a sprite with x > 0 seen on the line) */
	int width;                  /* width the last line was drawn with */
	uint8_t idx[RENDER_MAX_W];  /* the last line: per pixel 'IPpp cccc' (intensity / priority,
	                             * palette, colour); see render.c */
} render_ctx;

void render_tables_init(void);                       /* build the lookup tables (idempotent) */
void render_ctx_reset(render_ctx *rc);

/* Draw line `line` into rc->idx. */
void render_line_pixels(render_ctx *rc, const render_vdp *v, int line);
/* Parse the sprite table for line+1 (line = -1: for line 0). */
void render_parse_sprites(render_ctx *rc, const render_vdp *v, int line);
/* Convert rc->idx (rc->width pixels) to RGBA (bytes R, G, B, A in memory). */
void render_line_colors(const render_ctx *rc, const render_vdp *v, uint32_t *dst);

/* Frame mode: render a whole frame from a scene into fb (pitch in pixels). Returns the width. */
int render_frame(render_ctx *rc, const render_scene *s, uint32_t *fb, int pitch);

/* RGBA pixel helpers: the renderer's colours have 4-bit channels (c4 * 17 per byte). */
static inline uint32_t render_rgba(unsigned r4, unsigned g4, unsigned b4)
{
	return (r4 * 17u) | (g4 * 17u) << 8 | (b4 * 17u) << 16 | 0xFF000000u;
}
/* the same colour in GPGX's RGB565 encoding (for comparisons with its output) */
static inline uint16_t render_rgba_to_565(uint32_t c)
{
	unsigned r = (c & 0xFF) / 17, g = ((c >> 8) & 0xFF) / 17, b = ((c >> 16) & 0xFF) / 17;
	return (uint16_t)(r << 12 | (r >> 3) << 11 | g << 7 | (g >> 2) << 5 | b << 1 | b >> 3);
}
static inline uint32_t render_565_to_rgba(uint16_t p)
{
	return render_rgba((p >> 12) & 15, (p >> 7) & 15, (p >> 1) & 15);
}

#endif
