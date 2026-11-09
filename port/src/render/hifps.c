/* High frame rate presentation (github/t2-decomp#14), see hifps.h and docs/hifps.md.
 *
 * Presentation only: everything here reads host copies (a snapshot of the VDP at line 0 of each
 * frame and of the sprite queue in work RAM) and draws with the C renderer's frame mode
 * (render_frame). Nothing is written to the emulated machine. */
#include "hifps.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "emu.h"
#include "gx.h"

#define W_MAX RENDER_MAX_W
#define H_MAX RENDER_MAX_H
#define LOGIC_HZ 59.92

#define RAM_SPR_QUEUE_N   0xFF9DF2u
#define RAM_SPR_QUEUE     0xFFFFA250u
#define RAM_SPR_ARGS      0xFFFFA318u
#define RAM_SPR_TABLE     0xFFFFA638u
#define ROM_SPR_MAPPINGS  0x0152A8u
#define HS_W 0x1C0         /* words of the h-scroll table that are compared / kept */
#define LATE_MAX 112       /* a list run within this many lines of the display start counts as before it (adopt the end state) */
#define MAX_ENT 100
#define MAX_PIECES 80

#define HS_CUT 64          /* a scroll step above this is a cut, not a motion */
#define SPR_JUMP 40        /* a sprite that moved further than this is a different object */
#define MAX_BAD_LINES 80   /* frame-mode lines that may differ from the raster frame (copied from it) */

typedef struct { int frame, x, y, prio, first, np; } ent_t;

typedef struct {                 /* the sprite queue as the game's last BuildSpriteTable left it */
	bool read;
	int n;
	ent_t e[MAX_ENT];
	uint16_t tbl[MAX_PIECES * 4];
} qsnap;

typedef struct {
	bool snap;                   /* line-0 snapshot taken, frame not finished */
	bool valid;                  /* a finished frame */
	bool ok;                     /* frame mode reproduces the raster frame (up to the bad lines) */
	int w, h;
	uint16_t vram[32768], cram[64], vsram[40];
	uint8_t reg[32];
	uint16_t sat[512];
	bool owners;                 /* the sprite queue explains the sprite table */
	int ne;
	ent_t e[MAX_ENT];
	int cross;                   /* entry index of player 1's crosshair, -1 */
	int ref_x, ref_y;            /* pointer sample the crosshair follows (if cross_ok) */
	bool cross_ok;
	uint8_t bad[H_MAX];          /* lines where frame mode differs from the raster frame */
	int nbad;
	/* changes the game made during the display (a frame that overran: the VDP list ran late) */
	int sat_line, hs_line, cram_line;   /* first line that saw the new sprite table / h-scroll / palette, or -1 */
	bool vs_changed, reg_changed;       /* v-scroll / a register changed during the display */
	bool adopted;                       /* drawn from the end-of-display state (see hifps_lastline) */
	uint16_t late_hs[HS_W], late_cram[64];
	int16_t hs_eff[H_MAX][2];           /* the h-scroll value each table entry had when drawn (A, B) */
	uint16_t pic[W_MAX * H_MAX]; /* the raster frame, RGB565, pitch w */
} rec;

/* the relation between R(k-1) and R(k) */
typedef struct {
	int mode;                    /* PAIR_* */
	int16_t hd[H_MAX][2];        /* h-scroll step per table entry (A, B) */
	int16_t vd[40];
	int ne;
	int match[MAX_ENT];          /* cur entry -> prev entry or -1 */
	int dx[MAX_ENT], dy[MAX_ENT];
} pair_t;
enum { PAIR_NONE = 0, PAIR_STILL, PAIR_INTERP, PAIR_HELD };

typedef struct { int x, y; bool off; } mpos;

static struct {
	int hz, n;
	rec R[3];
	int icur, iprev, inext;      /* indices into R */
	bool have_cur, have_prev;
	pair_t pr;
	qsnap pend, pend2;           /* captured at the last display line of the previous two frames */
	qsnap c[4];                  /* owner candidates of the frame being drawn */
	mpos hist[4];
	int nhist;
	mpos now;
	bool have_now;
	/* output */
	uint16_t out[W_MAX * H_MAX];
	/* stats */
	long frames, gameplay_interp, still, held[8];
	long pieces, pieces_matched, ents, ents_matched;
	long check_fail, owners_unknown, owners_via[4], adopted;
	long pics, pics_interp, pics_held, pics_whole;
	double render_ns;
	long render_n;
	double render_max_ns;
	bool eager, atexit_set, stats_env;
	long eager_frame;
	char dump_dir[400];
	long dump_from, dump_to;
	long dump_count;
} H;

enum { HR_FIRST, HR_CHECK, HR_LAYOUT, HR_OWNERS, HR_CUT, HR_NOSNAP, HR_MID, HR_N };
static const char *held_name[HR_N] = { "no previous frame", "frame mode differs", "layout changed",
                                       "owners unknown", "scene cut", "no snapshot", "changed during the display" };

static double now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e9 + ts.tv_nsec;
}

static void report(void)
{
	if (!H.hz || (!H.stats_env && !H.eager))
		return;
	fprintf(stderr, "hifps: %d Hz (%d pictures a frame), %ld frames: %ld interpolated, %ld still, held:",
	        H.hz, H.n, H.frames, H.gameplay_interp, H.still);
	for (int i = 0; i < HR_N; i++)
		if (H.held[i])
			fprintf(stderr, " %ld %s;", H.held[i], held_name[i]);
	fprintf(stderr, "\nhifps: %ld of %ld sprite entries matched (%.2f%%), %ld of %ld pieces moved;"
	        " owners via the previous end-of-display queue %ld, two back %ld, line 0 %ld, this end %ld, unknown %ld; list ran early in the display %ld; frame-mode check failed %ld\n",
	        H.ents_matched, H.ents, H.ents ? 100.0 * H.ents_matched / H.ents : 100.0, H.pieces_matched,
	        H.pieces, H.owners_via[0], H.owners_via[1], H.owners_via[2], H.owners_via[3], H.owners_unknown, H.adopted, H.check_fail);
	if (H.render_n)
		fprintf(stderr, "hifps: %ld frame-mode pictures, %.3f ms each (max %.3f ms)\n", H.render_n,
		        H.render_ns / H.render_n / 1e6, H.render_max_ns / 1e6);
}

int hifps_configure(int hz)
{
	memset(&H.hz, 0, sizeof H.hz);
	H.n = 0;
	if (hz < 61)
		return 0;
	if (hz > 1000)
		hz = 1000;
	int n = (int)floor(hz / LOGIC_HZ + 0.5);
	if (n < 2)
		return 0;
	H.hz = hz;
	H.n = n;
	render_tables_init();
	const char *s = getenv("T2_HIFPS_STATS");
	H.stats_env = s && *s && *s != '0';
	const char *d = getenv("T2_HIFPS_DUMP");
	if (d && *d) {
		snprintf(H.dump_dir, sizeof H.dump_dir, "%s", d);
		char *c = strchr(H.dump_dir, ',');
		H.dump_from = 0;
		H.dump_to = 1L << 40;
		if (c) {
			*c = 0;
			sscanf(c + 1, "%ld-%ld", &H.dump_from, &H.dump_to);
		}
		mkdir(H.dump_dir, 0777);
	}
	if (!H.atexit_set) {
		H.atexit_set = true;
		atexit(report);
	}
	hifps_reset();
	return n;
}

bool hifps_enabled(void) { return H.hz != 0; }
bool hifps_ready(void) { return H.hz && H.have_cur; }
int hifps_per_frame(void) { return H.n; }

void hifps_reset(void)
{
	H.have_cur = H.have_prev = false;
	H.pend.read = false;
	H.nhist = 0;
	H.have_now = false;
	for (int i = 0; i < 3; i++)
		H.R[i].snap = H.R[i].valid = false;
	H.icur = 0;
	H.iprev = 1;
	H.inext = 2;
}

/* ------------------------------------------------------------------ sprite queue */
static inline uint16_t rom16(uint32_t a) { return *(const uint16_t *)(gx_rom() + (a & 0x3FFFFE)); }

static void read_queue(qsnap *q)
{
	q->read = false;
	q->n = 0;
	int n = emu_ram8(RAM_SPR_QUEUE_N);
	if (n > MAX_ENT)
		return;
	for (int i = 0; i < n; i++) {
		int idx = emu_ram8(RAM_SPR_QUEUE + 2 * (uint32_t)i);
		if (idx >= MAX_ENT)
			return;
		uint32_t a = RAM_SPR_ARGS + 8 * (uint32_t)idx;
		ent_t *e = &q->e[i];
		e->frame = emu_ram16(a);
		e->x = (int16_t)emu_ram16(a + 2);
		e->y = (int16_t)emu_ram16(a + 4);
		e->prio = emu_ram8(RAM_SPR_QUEUE + 2 * (uint32_t)i + 1);
		e->first = e->np = 0;
	}
	q->n = n;
	for (int i = 0; i < MAX_PIECES * 4; i++)
		q->tbl[i] = emu_ram16(RAM_SPR_TABLE + 2 * (uint32_t)i);
	q->read = true;
}

/* does the queue explain the sprite table in `vsat` (the VDP's copy, 4 words a piece)? Fills the
 * entries' first piece / piece count. */
static bool queue_explains(qsnap *q, const uint16_t *vsat)
{
	if (!q->read)
		return false;
	int piece = 0;
	for (int i = 0; i < q->n; i++) {
		ent_t *e = &q->e[i];
		e->first = piece;
		uint32_t p = ROM_SPR_MAPPINGS + rom16(ROM_SPR_MAPPINGS + 2 * (uint32_t)e->frame);
		for (int guard = 0; guard < 64; guard++, p += 10) {
			uint16_t size = rom16(p);
			if ((int16_t)size < 0)
				break;
			if (piece >= MAX_PIECES - 1)
				break;                                   /* the game's 79-piece cap */
			uint16_t y = (uint16_t)(rom16(p + 2) + 0x80 + e->y);
			uint16_t x = (uint16_t)(rom16(p + 8) + 0x80 + e->x);
			const uint16_t *t = &q->tbl[piece * 4];
			if (t[0] != y || (t[1] >> 8) != (size >> 8) || t[3] != x)
				return false;
			piece++;
		}
		e->np = piece - e->first;
		if (piece >= MAX_PIECES - 1) {
			q->n = i + 1;                                /* the rest of the queue was dropped */
			break;
		}
	}
	if (piece == 0)                                      /* empty queue: one dummy entry */
		return q->tbl[0] == 0 && vsat[0] == 0;
	if (memcmp(q->tbl, vsat, (size_t)piece * 8))
		return false;
	return true;
}

/* ------------------------------------------------------------------ snapshot */
/* the game flips the DMA-enable bit of register 1 around every DMA: not a change of the picture */
static inline int regmask(int r) { return r == 1 ? 0xEF : 0xFF; }

static uint32_t hs_base(const uint8_t *reg) { return ((uint32_t)reg[13] << 10) & 0xFC00; }

static uint32_t sat_base(const uint8_t *reg)
{
	return ((uint32_t)reg[5] << 9) & ((reg[12] & 1) ? 0xFC00 : 0xFE00);
}

void hifps_line0(const render_vdp *v, int height)
{
	if (!H.hz)
		return;
	rec *r = &H.R[H.inext];
	r->snap = true;
	r->valid = false;
	r->ok = false;
	r->w = v->width;
	r->h = height;
	r->sat_line = r->hs_line = r->cram_line = -1;
	r->vs_changed = r->reg_changed = r->adopted = false;
	memcpy(r->vram, v->vram, sizeof r->vram);
	memcpy(r->cram, v->cram, sizeof r->cram);
	memcpy(r->vsram, v->vsram, sizeof r->vsram);
	memcpy(r->reg, v->reg, 24);
	memcpy(r->sat, gx_sat(), sizeof r->sat);

	/* who owns the sprites is settled at the frame's end, from the queues seen at: the end of the
	 * previous display (the game has finished building the next frame by then), two displays back,
	 * now (if the 68k has not yet disturbed it) and the end of this display (frames whose sprite table
	 * was uploaded during the display) */
	H.c[0] = H.pend;
	H.c[1] = H.pend2;
	read_queue(&H.c[2]);
	H.c[3].read = false;
	H.pend2 = H.pend;
	H.pend.read = false;
}

/* the live VDP at the start of line `line`: did the game change what the line reads? */
void hifps_line(const render_vdp *v, int line)
{
	rec *r = &H.R[H.inext];
	if (!H.hz || !r->snap || line <= 0)
		return;
	uint32_t hb = hs_base(r->reg) >> 1;
	if (r->hs_line < 0 && memcmp(v->vram + hb, r->vram + hb, HS_W * 2))
		r->hs_line = line;
	if (r->cram_line < 0 && memcmp(v->cram, r->cram, sizeof r->cram))
		r->cram_line = line;
	{
		static const int regs[] = { 0, 1, 2, 3, 4, 5, 7, 11, 12, 13, 16, 17, 18 };
		if (!r->vs_changed && memcmp(v->vsram, r->vsram, sizeof r->vsram)) {
			r->vs_changed = true;
			if (getenv("T2_HIFPS_DEBUG"))
				fprintf(stderr, "hifps: frame %ld: vsram changed at line %d\n", emu_frame_count(), line);
		}
		for (unsigned i = 0; i < sizeof regs / sizeof regs[0]; i++)
			if (!r->reg_changed && ((v->reg[regs[i]] ^ r->reg[regs[i]]) & regmask(regs[i]))) {
				r->reg_changed = true;
				if (getenv("T2_HIFPS_DEBUG"))
					fprintf(stderr, "hifps: frame %ld: reg %d changed at line %d (%02X -> %02X)\n", emu_frame_count(), regs[i], line,
					        r->reg[regs[i]], v->reg[regs[i]]);
			}
	}
}

/* the sprite table for line+1 is read now */
void hifps_parse(const render_vdp *v, int line)
{
	rec *r = &H.R[H.inext];
	if (!H.hz || !r->snap || r->sat_line >= 0)
		return;
	uint32_t sb = sat_base(r->reg) >> 1;
	if (memcmp(v->vram + sb, r->vram + sb, 80 * 8) || memcmp(v->sat, r->sat, 80 * 8))
	{
		r->sat_line = line + 1;
		if (getenv("T2_HIFPS_DEBUG"))
			fprintf(stderr, "hifps: frame %ld: sprite table changed at line %d (vram %d cache %d)\n", emu_frame_count(), line + 1,
			        memcmp(v->vram + sb, r->vram + sb, 80 * 8) != 0, memcmp(v->sat, r->sat, 80 * 8) != 0);
	}
}

void hifps_lastline(void)
{
	if (!H.hz)
		return;
	{
		rec *r = &H.R[H.inext];
		if (r->snap && r->hs_line >= 0)
			memcpy(r->late_hs, (const uint16_t *)gx_vram() + (hs_base(r->reg) >> 1), sizeof r->late_hs);
		if (r->snap && r->cram_line >= 0)
			memcpy(r->late_cram, gx_cram(), sizeof r->late_cram);
		/* the game overran and its VDP list ran just after the display began: nearly the whole picture
		 * was drawn from the end-of-display state (the first lines are mended from the raster frame) */
		if (r->snap && ((r->sat_line >= 0 && r->sat_line <= LATE_MAX) || (r->hs_line >= 0 && r->hs_line <= LATE_MAX)) &&
		    r->sat_line <= LATE_MAX && !r->reg_changed) {
			memcpy(r->vram, gx_vram(), sizeof r->vram);
			memcpy(r->cram, gx_cram(), sizeof r->cram);
			memcpy(r->vsram, gx_vsram(), sizeof r->vsram);
			memcpy(r->sat, gx_sat(), sizeof r->sat);
			r->sat_line = r->hs_line = r->cram_line = -1;
			r->vs_changed = false;
			r->adopted = true;
			H.adopted++;
		}
	}
	if (getenv("T2_HIFPS_DEBUG") && H.R[H.inext].snap) {
		const uint16_t *lv = (const uint16_t *)gx_vram();
		const rec *r = &H.R[H.inext];
		int n = 0, first = -1, last = -1;
		char buf[600];
		buf[0] = 0;
		int prevblk = -1;
		for (int i = 0; i < 32768; i++)
			if (lv[i] != r->vram[i]) {
				n++;
				if (first < 0)
					first = i;
				last = i;
				if (i / 64 != prevblk && strlen(buf) < 500) {
					prevblk = i / 64;
					sprintf(buf + strlen(buf), " %04X", i * 2);
				}
			}
		if (n)
			fprintf(stderr, "hifps: frame %ld: VRAM changed during display: %d words, blocks:%s\n", emu_frame_count(), n, buf);
	}
	read_queue(&H.pend);
	H.c[3] = H.pend;
}

void hifps_note_mouse(int gx, int gy, bool off)
{
	if (!H.hz)
		return;
	for (int i = 3; i > 0; i--)
		H.hist[i] = H.hist[i - 1];
	H.hist[0] = (mpos){ gx, gy, off };
	if (H.nhist < 4)
		H.nhist++;
}

void hifps_mouse_now(int gx, int gy, bool off)
{
	H.now = (mpos){ gx, gy, off };
	H.have_now = true;
}

/* ------------------------------------------------------------------ rendering from a record */
static uint32_t g_fb[W_MAX * H_MAX];
static uint16_t w_vram[32768], w_vsram[40], w_sat[512];

static void to565(const uint32_t *fb, uint16_t *out, int w, int h)
{
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			out[y * w + x] = render_rgba_to_565(fb[y * W_MAX + x]);
}

/* frame mode from the working copies w_* (cram and registers from r) */
static void render_work(const rec *r, uint16_t *out)
{
	render_ctx rc;
	render_ctx_reset(&rc);
	render_scene s;
	s.vdp.vram = w_vram;
	s.vdp.cram = r->cram;
	s.vdp.vsram = w_vsram;
	s.vdp.reg = r->reg;
	s.vdp.sat = w_sat;
	s.vdp.width = r->w;
	s.height = r->h;
	double t0 = now_ns();
	{   /* render_frame, with the palette the game changed during the display switched in at its line */
		bool on = s.vdp.reg[1] & 0x40;
		if (on)
			render_parse_sprites(&rc, &s.vdp, -1);
		for (int line = 0; line < s.height && line < H_MAX; line++) {
			if (line == r->cram_line)
				s.vdp.cram = r->late_cram;
			render_line_pixels(&rc, &s.vdp, line);
			if (on && line < s.height - 1)
				render_parse_sprites(&rc, &s.vdp, line);
			render_line_colors(&rc, &s.vdp, g_fb + (size_t)line * W_MAX);
		}
	}
	to565(g_fb, out, r->w, r->h);
	double dt = now_ns() - t0;
	H.render_ns += dt;
	H.render_n++;
	if (dt > H.render_max_ns)
		H.render_max_ns = dt;
}

static void load_work(const rec *r)
{
	memcpy(w_vram, r->vram, sizeof w_vram);
	memcpy(w_vsram, r->vsram, sizeof w_vsram);
	memcpy(w_sat, r->sat, sizeof w_sat);
	/* the h-scroll table as it was drawn (entries after a mid-display upload come from the new one) */
	int mask = (int[]){ 0, 7, 0xF8, 0xFF }[r->reg[11] & 3];
	uint32_t hw = hs_base(r->reg) >> 1;
	for (int l = 0; l < r->h; l++) {
		int e = l & mask;
		if (e != l)
			continue;
		for (int pl = 0; pl < 2; pl++) {
			uint32_t wi = (hw + 2 * (uint32_t)e + (uint32_t)pl) & 0x7FFF;
			w_vram[wi] = (uint16_t)((w_vram[wi] & 0xFC00) | (r->hs_eff[e][pl] & 0x3FF));
		}
	}
}

/* move SAT piece i by (dx, dy) in the working copies; pieces at x = 0 are the VDP's sprite mask */
static void move_piece(const rec *r, int i, int dx, int dy)
{
	uint16_t *e = &w_vram[(sat_base(r->reg) >> 1) + 4 * (uint32_t)i];
	if ((e[3] & 0x1FF) == 0)
		return;
	e[3] = (uint16_t)((e[3] & 0xFE00) | ((e[3] + dx) & 0x1FF));
	e[0] = (uint16_t)((e[0] & 0xFE00) | ((e[0] + dy) & 0x1FF));
	w_sat[4 * i] = (uint16_t)((w_sat[4 * i] & 0xFE00) | ((w_sat[4 * i] + dy) & 0x1FF));
}

static void move_entry(const rec *r, const ent_t *e, int dx, int dy)
{
	for (int p = 0; p < e->np; p++)
		move_piece(r, e->first + p, dx, dy);
}

/* round half away from zero */
static int rnd(double x) { return x >= 0 ? (int)floor(x + 0.5) : -(int)floor(-x + 0.5); }

static void copy_bad_lines(const rec *r, uint16_t *out)
{
	if (!r->nbad)
		return;
	for (int y = 0; y < r->h; y++)
		if (r->bad[y])
			memcpy(out + (size_t)y * (size_t)r->w, r->pic + (size_t)y * (size_t)r->w, (size_t)r->w * 2);
}

/* the crosshair shift for record r: the pointer now against the sample the game drew it at */
static bool cross_shift(const rec *r, int *dx, int *dy)
{
	*dx = *dy = 0;
	if (!r->cross_ok || r->cross < 0 || !H.have_now || H.now.off)
		return false;
	const ent_t *c = &r->e[r->cross];
	*dx = H.now.x - r->ref_x;
	*dy = H.now.y - r->ref_y;
	int cy = c->y + *dy;
	if (cy < 24)
		cy = 24;
	if (cy > 200)
		cy = 200;
	*dy = cy - c->y;
	return *dx || *dy;
}

/* ------------------------------------------------------------------ the pair R(k-1), R(k) */
static bool layout_same(const rec *a, const rec *b)
{
	static const int regs[] = { 0, 1, 2, 3, 4, 5, 11, 12, 13, 16, 17, 18 };
	if (a->w != b->w || a->h != b->h)
		return false;
	for (unsigned i = 0; i < sizeof regs / sizeof regs[0]; i++)
		if ((a->reg[regs[i]] ^ b->reg[regs[i]]) & regmask(regs[i]))
			return false;
	return true;
}

static int sdiff10(int cur, int prev) { return (((cur - prev) + 512) & 0x3FF) - 512; }

static void decide_pair(const rec *prev, const rec *cur, pair_t *p)
{
	memset(p, 0, sizeof *p);
	int why = -1;
	if (!prev)
		why = HR_FIRST;
	else if (cur->sat_line >= 0 || prev->sat_line >= 0 || cur->vs_changed || prev->vs_changed || cur->reg_changed || prev->reg_changed)
		why = HR_MID;
	else if (!cur->ok)
		why = HR_CHECK;
	else if (!layout_same(prev, cur) || (cur->reg[11] & 3) == 1)
		why = HR_LAYOUT;
	if (why >= 0)
		goto held;

	bool any = false;
	{   /* scroll */
		int mask = (int[]){ 0, 7, 0xF8, 0xFF }[cur->reg[11] & 3];
		for (int l = 0; l < cur->h; l++) {
			int e = l & mask;
			if (e != l)
				continue;
			for (int pl = 0; pl < 2; pl++) {
				int d = sdiff10(cur->hs_eff[e][pl], prev->hs_eff[e][pl]);
				if (d > HS_CUT || d < -HS_CUT) {
					why = HR_CUT;
					goto held;
				}
				p->hd[e][pl] = (int16_t)d;
				any |= d != 0;
			}
		}
		for (int i = 0; i < 40; i++) {
			int d = sdiff10(cur->vsram[i] & 0x3FF, prev->vsram[i] & 0x3FF);
			if (d > HS_CUT || d < -HS_CUT) {
				why = HR_CUT;
				goto held;
			}
			p->vd[i] = (int16_t)d;
			any |= d != 0;
		}
	}
	/* sprites */
	if (!cur->owners || !prev->owners) {
		/* unknown owners: only a sprite table that did not change is safe */
		uint32_t cb = sat_base(cur->reg) >> 1;
		if (cur->owners != prev->owners ||
		    memcmp(cur->vram + cb, prev->vram + cb, 80 * 8) || memcmp(cur->sat, prev->sat, 80 * 8)) {
			why = HR_OWNERS;
			goto held;
		}
		p->mode = any ? PAIR_INTERP : PAIR_STILL;
		return;
	}
	{
		bool used[MAX_ENT] = { false };
		p->ne = cur->ne;
		for (int ci = 0; ci < cur->ne; ci++) {
			const ent_t *c = &cur->e[ci];
			p->match[ci] = -1;
			int best = -1, bestcost = 1 << 30;
			for (int pi = 0; pi < prev->ne; pi++) {
				if (used[pi])
					continue;
				const ent_t *o = &prev->e[pi];
				int ddx = c->x - o->x, ddy = c->y - o->y;
				if (ddx > SPR_JUMP || ddx < -SPR_JUMP || ddy > SPR_JUMP || ddy < -SPR_JUMP)
					continue;
				int cost = abs(ddx) + abs(ddy) + (c->frame != o->frame ? 12 : 0) + (c->np != o->np ? 6 : 0);
				if (c->frame <= 2 && o->frame != c->frame)      /* the crosshairs only match each other */
					continue;
				if (o->frame <= 2 && o->frame != c->frame)
					continue;
				if (cost < bestcost) {
					bestcost = cost;
					best = pi;
				}
			}
			H.ents++;
			H.pieces += c->np;
			if (best >= 0) {
				used[best] = true;
				p->match[ci] = best;
				p->dx[ci] = c->x - prev->e[best].x;
				p->dy[ci] = c->y - prev->e[best].y;
				H.ents_matched++;
				H.pieces_matched += c->np;
				any |= p->dx[ci] || p->dy[ci];
			}
		}
	}
	p->mode = any ? PAIR_INTERP : PAIR_STILL;
	return;
held:
	p->mode = PAIR_HELD;
	if (why >= 0 && why < HR_N)
		H.held[why]++;
}

/* ------------------------------------------------------------------ frame end */
static void find_cross(rec *r)
{
	r->cross = -1;
	r->cross_ok = false;
	for (int i = 0; i < r->ne; i++)
		if (r->e[i].frame == 1 && r->e[i].np > 0) {
			r->cross = i;
			break;
		}
	if (r->cross < 0 || H.nhist == 0)
		return;
	/* which pointer sample did the game draw it at? the nearest of the last ones, if close enough */
	const ent_t *c = &r->e[r->cross];
	int best = -1, bd = 1 << 30;
	for (int i = 0; i < H.nhist; i++) {
		if (H.hist[i].off)
			continue;
		int my = H.hist[i].y < 24 ? 24 : H.hist[i].y > 200 ? 200 : H.hist[i].y;
		int d = abs(c->x - H.hist[i].x) + abs(c->y - my);
		if (d < bd) {
			bd = d;
			best = i;
		}
	}
	if (best >= 0 && bd <= 16) {
		r->ref_x = H.hist[best].x;
		r->ref_y = H.hist[best].y < 24 ? 24 : H.hist[best].y > 200 ? 200 : H.hist[best].y;
		r->cross_ok = true;
	}
}

static void dump_ppm(const char *dir, const char *name, const uint16_t *px, unsigned w, unsigned h)
{
	char path[600];
	snprintf(path, sizeof path, "%s/%s", dir, name);
	FILE *f = fopen(path, "wb");
	if (!f)
		return;
	fprintf(f, "P6\n%u %u\n255\n", w, h);
	for (unsigned i = 0; i < w * h; i++) {
		uint32_t c = render_565_to_rgba(px[i]);
		uint8_t rgb[3] = { (uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16) };
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
}

void hifps_frame_end(const uint16_t *px, unsigned w, unsigned h, bool complete)
{
	if (!H.hz)
		return;
	rec *r = &H.R[H.inext];
	if (!r->snap || !px || !complete || (unsigned)r->w != w || (unsigned)r->h != h || w > W_MAX || h > H_MAX) {
		if (!r->snap)
			H.held[HR_NOSNAP]++;
		r->snap = false;
		H.have_cur = H.have_prev = false;     /* a frame we cannot use: start over */
		return;
	}
	r->snap = false;
	memcpy(r->pic, px, (size_t)w * h * 2);

	{   /* the h-scroll values the lines were drawn with */
		int mask = (int[]){ 0, 7, 0xF8, 0xFF }[r->reg[11] & 3];
		uint32_t hw = hs_base(r->reg) >> 1;
		memset(r->hs_eff, 0, sizeof r->hs_eff);
		for (int l = 0; l < r->h; l++) {
			int e = l & mask;
			if (e != l)
				continue;
			for (int pl = 0; pl < 2; pl++) {
				uint32_t k = 2 * (uint32_t)e + (uint32_t)pl;
				uint16_t val = (r->hs_line >= 0 && e >= r->hs_line && k < HS_W) ? r->late_hs[k] : r->vram[(hw + k) & 0x7FFF];
				r->hs_eff[e][pl] = (int16_t)(val & 0x3FF);
			}
		}
	}
	{   /* who owns the sprites */
		const uint16_t *vsat = r->vram + (sat_base(r->reg) >> 1);
		r->owners = false;
		r->ne = 0;
		for (int i = 0; i < 4; i++)
			if (queue_explains(&H.c[i], vsat)) {
				r->owners = true;
				r->ne = H.c[i].n;
				memcpy(r->e, H.c[i].e, sizeof(ent_t) * (size_t)H.c[i].n);
				H.owners_via[i]++;
				break;
			}
		if (!r->owners) {
			H.owners_unknown++;
			if (getenv("T2_HIFPS_DEBUG"))
				fprintf(stderr, "hifps: frame %ld: owners unknown (queues: %d %d %d %d)\n", emu_frame_count(),
				        H.c[0].read ? H.c[0].n : -1, H.c[1].read ? H.c[1].n : -1, H.c[2].read ? H.c[2].n : -1, H.c[3].read ? H.c[3].n : -1);
		}
	}
	/* the check: frame mode from the snapshot, no motion, against the raster frame */
	load_work(r);
	static uint16_t chk[W_MAX * H_MAX];
	render_work(r, chk);
	r->nbad = 0;
	memset(r->bad, 0, sizeof r->bad);
	for (unsigned y = 0; y < h; y++)
		if (memcmp(chk + y * w, px + y * w, w * 2)) {
			r->bad[y] = 1;
			r->nbad++;
		}
	r->ok = r->nbad <= MAX_BAD_LINES;
	if (getenv("T2_HIFPS_DEBUG") && r->nbad) {
		fprintf(stderr, "hifps: frame %ld: %d lines differ:", emu_frame_count(), r->nbad);
		int shown = 0;
		for (unsigned y = 0; y < h && shown < 40; y++)
			if (r->bad[y]) {
				fprintf(stderr, " %u", y);
				shown++;
			}
		fprintf(stderr, "\n");
	}
	if (!r->ok)
		H.check_fail++;
	r->valid = true;
	find_cross(r);

	/* rotate: cur becomes prev, the new record is cur */
	int t = H.iprev;
	H.iprev = H.icur;
	H.icur = H.inext;
	H.inext = t;
	H.have_prev = H.have_cur;
	H.have_cur = true;
	decide_pair(H.have_prev ? &H.R[H.iprev] : NULL, &H.R[H.icur], &H.pr);
	H.frames++;
	if (H.pr.mode == PAIR_INTERP)
		H.gameplay_interp++;
	else if (H.pr.mode == PAIR_STILL)
		H.still++;
}

/* ------------------------------------------------------------------ pictures */
const uint16_t *hifps_picture(double f, unsigned *w, unsigned *h, int *kind)
{
	if (!H.hz || !H.have_cur)
		return NULL;
	const rec *cur = &H.R[H.icur], *prev = H.have_prev ? &H.R[H.iprev] : NULL;
	int k = HIFPS_WHOLE, cdx, cdy;
	*w = (unsigned)cur->w;
	*h = (unsigned)cur->h;
	H.pics++;
	if (f >= 1.0) {
		bool sh = cur->ok && cross_shift(cur, &cdx, &cdy);
		if (!sh) {
			H.pics_whole++;
			*kind = k;
			return cur->pic;
		}
		load_work(cur);
		const ent_t *c = &cur->e[cur->cross];
		move_entry(cur, c, cdx, cdy);
		render_work(cur, H.out);
		copy_bad_lines(cur, H.out);
		H.pics_whole++;
		*kind = k;
		return H.out;
	}
	if (f < 0)
		f = 0;
	const pair_t *p = &H.pr;
	if (p->mode == PAIR_STILL || p->mode == PAIR_INTERP) {
		k = p->mode == PAIR_INTERP ? HIFPS_INTERP : HIFPS_HELD;
		bool sh = cross_shift(cur, &cdx, &cdy);
		if (p->mode == PAIR_STILL && !sh) {
			*kind = HIFPS_HELD;
			H.pics_held++;
			return cur->pic;
		}
		double b = 1.0 - f;
		load_work(cur);
		if (p->mode == PAIR_INTERP) {
			{   /* scroll: both planes' table entries, the vertical scroll */
				int mask = (int[]){ 0, 7, 0xF8, 0xFF }[cur->reg[11] & 3];
				uint32_t hw = ((uint32_t)cur->reg[13] << 10 & 0xFC00) >> 1;
				for (int l = 0; l < cur->h; l++) {
					int e = l & mask;
					if (e != l)
						continue;
					for (int pl = 0; pl < 2; pl++) {
						int d = p->hd[e][pl];
						if (!d)
							continue;
						uint32_t wi = (hw + 2 * (uint32_t)e + (uint32_t)pl) & 0x7FFF;
						w_vram[wi] = (uint16_t)((w_vram[wi] & 0xFC00) | ((w_vram[wi] - rnd(d * b)) & 0x3FF));
					}
				}
				for (int i = 0; i < 40; i++)
					if (p->vd[i])
						w_vsram[i] = (uint16_t)((w_vsram[i] & 0xFC00) | ((w_vsram[i] - rnd(p->vd[i] * b)) & 0x3FF));
			}
			for (int ci = 0; ci < p->ne; ci++) {
				if (p->match[ci] < 0)
					continue;
				if (ci == cur->cross && cur->cross_ok)
					continue;                 /* drawn at the pointer below */
				int ddx = rnd(p->dx[ci] * b), ddy = rnd(p->dy[ci] * b);
				if (ddx || ddy)
					move_entry(cur, &cur->e[ci], -ddx, -ddy);
			}
			k = HIFPS_INTERP;
		}
		if (sh)
			move_entry(cur, &cur->e[cur->cross], cdx, cdy);
		render_work(cur, H.out);
		copy_bad_lines(cur, H.out);
		if (k == HIFPS_INTERP)
			H.pics_interp++;
		else
			H.pics_held++;
		*kind = k;
		return H.out;
	}
	/* held: the previous frame, with the crosshair at the pointer */
	const rec *base = prev ? prev : cur;
	H.pics_held++;
	*kind = HIFPS_HELD;
	*w = (unsigned)base->w;
	*h = (unsigned)base->h;
	if (base->ok && cross_shift(base, &cdx, &cdy)) {
		load_work(base);
		move_entry(base, &base->e[base->cross], cdx, cdy);
		render_work(base, H.out);
		copy_bad_lines(base, H.out);
		return H.out;
	}
	return base->pic;
}

void hifps_eager(void)
{
	if (!H.hz || !H.have_cur)
		return;
	H.eager = true;
	long k = H.eager_frame++;
	bool dump = H.dump_dir[0] && k >= H.dump_from && k <= H.dump_to;
	const char *tm = getenv("T2_HIFPS_MOUSE");     /* test: the pointer is this far from where the game read it */
	int tdx = 0, tdy = 0;
	if (tm && sscanf(tm, "%d,%d", &tdx, &tdy) == 2 && H.nhist)
		hifps_mouse_now(H.hist[0].x + tdx, H.hist[0].y + tdy, H.hist[0].off);
	for (int j = 1; j <= H.n; j++) {
		unsigned w, h;
		int kind;
		const uint16_t *px = hifps_picture((double)j / H.n, &w, &h, &kind);
		if (px && dump) {
			char nm[64];
			snprintf(nm, sizeof nm, "s%06ld_%02d_%c.ppm", k, j, kind == HIFPS_WHOLE ? 'w' : kind == HIFPS_INTERP ? 'i' : 'h');
			dump_ppm(H.dump_dir, nm, px, w, h);
			H.dump_count++;
		}
	}
}
