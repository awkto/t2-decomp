/* HLE registry and CPU glue. The patched core (tools/patches/gpgx-hle.patch) calls
 * m68k_hle_handler(pc) before fetching an instruction whose address has its bit set in
 * m68k_hle_bitmap. */
#include "hle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>

#include "gx.h"
#include "native.h"

/* from gpgx-hle.patch */
extern unsigned char *m68k_hle_bitmap;
extern int (*m68k_hle_handler)(unsigned int pc);
extern unsigned int m68k_hle_skip_pc;
extern unsigned int m68k_bus_read8(unsigned int a);
extern unsigned int m68k_bus_read16(unsigned int a);
extern unsigned int m68k_bus_read32(unsigned int a);
extern void m68k_bus_write8(unsigned int a, unsigned int v);
extern void m68k_bus_write16(unsigned int a, unsigned int v);
extern void m68k_bus_write32(unsigned int a, unsigned int v);
extern unsigned int m68k_hle_get_sr(void);
extern void m68k_hle_set_ccr(unsigned int v);
extern void m68k_hle_set_sr(unsigned int v);
extern int m68k_step_one(void);

#define MAX_DEFS 8192   /* glue.c and auto1.c (t2-decomp#11) brought the registry past 4096 */
#define SENTINEL_RET 0x0000F0u   /* return address used for hle_call68k (never executed) */
#define YIELD_SPAN   (60000L * 7)   /* minimum window (mclk) in which an event makes a YIELD function a coroutine */
#define YIELD_PC     0x0000F8u   /* a function yielded to the frame loop here (cor_run, resume_cor) */
#define STALL_PC     0x0000F4u   /* "burn the rest of a long C function's cost" trap (#31) */
#define STALL_SLICE  256       /* 68k cycles burnt per STALL_PC visit. Must stay below a scanline (~488 68k cycles): a
                                 * slice that overshoots the line boundary makes the next m68k_run() skip its
                                 * interrupt check, which drops the Menacer TH latch on that line (#7 grid). */

typedef struct {
	hle_def def;
	bool enabled;
	unsigned long calls;
} entry;

static entry g_ent[MAX_DEFS];
static int g_n;
static bool g_aim_fast = true;   /* aim=fast: see hle.h */
/* address -> entry index + 1, open addressing (dispatch runs for every C block) */
#define HASH_SIZE 16384
static uint16_t g_hash[HASH_SIZE];
static uint8_t g_bitmap[0x400000 >> 4];
static bool g_installed;
static unsigned long g_frame_calls;

uint8_t *hle_rom;
void (*hle_post_access)(void);

/* ------------------------------------------------------------------ registry */
static unsigned hash_slot(uint32_t addr) { return (unsigned)((addr * 2654435761u) >> 18) & (HASH_SIZE - 1); }

static entry *find_addr(uint32_t addr)
{
	for (unsigned h = hash_slot(addr); g_hash[h]; h = (h + 1) & (HASH_SIZE - 1))
		if (g_ent[g_hash[h] - 1].def.addr == addr)
			return &g_ent[g_hash[h] - 1];
	return NULL;
}

static void bitmap_set(uint32_t addr, bool on)
{
	if (addr >= 0x400000 || (addr & 1))
		return;
	uint8_t bit = (uint8_t)(1u << ((addr >> 1) & 7));
	if (on)
		g_bitmap[addr >> 4] |= bit;
	else
		g_bitmap[addr >> 4] &= (uint8_t)~bit;
}

void hle_register_def(const hle_def *def)
{
	if (def->addr >= 0x400000 || (def->addr & 1)) {
		fprintf(stderr, "hle: %s: bad address $%06X\n", def->name, def->addr);
		abort();
	}
	entry *e = find_addr(def->addr);
	if (e) {
		/* a whole-function port that yields (HLE_F_YIELD) supersedes a piece of the same entry: the census
		 * generators (auto1/auto2/glue) cover the addresses that were still 68k, the yielding port covers them
		 * exactly at interrupts; two of the same kind is a mistake */
		bool ny = (def->flags & HLE_F_YIELD) != 0, ey = (e->def.flags & HLE_F_YIELD) != 0;
		if (ny != ey) {
			if (ny)
				e->def = *def;
			return;
		}
		fprintf(stderr, "hle: $%06X registered twice (%s, %s)\n", def->addr, e->def.name, def->name);
		abort();
	}
	if (g_n == MAX_DEFS) {
		fprintf(stderr, "hle: registry full (%d definitions)\n", MAX_DEFS);
		abort();
	}
	e = &g_ent[g_n++];
	unsigned h = hash_slot(def->addr);
	while (g_hash[h])
		h = (h + 1) & (HASH_SIZE - 1);
	g_hash[h] = (uint16_t)g_n;
	e->def = *def;
	/* Every replacement starts disabled: plain t2port runs the original 68k code until
	 * hle_configure() / hle_set_all() enables something (docs/porting.md). The bitmap
	 * starts zeroed, so there is nothing to clear here. */
	e->enabled = false;
}

void hle_register_named(uint32_t addr, const char *name, hle_fn fn, int cycles)
{
	hle_def d = { addr, name, fn, cycles, "(runtime)", NULL, NULL, 0 };
	if (!name) {
		char *buf = malloc(16);
		snprintf(buf, 16, "sub_%06X", addr);
		d.name = buf;
	}
	hle_register_def(&d);
}

void hle_register(uint32_t addr, hle_fn fn) { hle_register_named(addr, NULL, fn, 0); }

int hle_count(void) { return g_n; }
const hle_def *hle_get(int i) { return i >= 0 && i < g_n ? &g_ent[i].def : NULL; }
bool hle_is_enabled(int i) { return i >= 0 && i < g_n && g_ent[i].enabled; }
unsigned long hle_calls(int i) { return i >= 0 && i < g_n ? g_ent[i].calls : 0; }
unsigned long hle_calls_this_frame(void) { return g_frame_calls; }
static long g_frames_run;      /* frames started (T2_HLE_PROFILE) */
static const hle_def *g_cur_def;   /* forward (defined below) */
static long g_it_frame = -1;
static void lh_frame(void);
void hle_frame_begin(void) { g_frame_calls = 0; g_frames_run++; lh_frame(); }

static entry *lookup(const char *s)
{
	if (!s || !*s)
		return NULL;
	if (*s == '$' || (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))) {
		uint32_t a = (uint32_t)strtoul(s + (*s == '$' ? 1 : 2), NULL, 16);
		return find_addr(a);
	}
	for (int i = 0; i < g_n; i++)
		if (!strcmp(g_ent[i].def.name, s))
			return &g_ent[i];
	return NULL;
}

const hle_def *hle_find(const char *s)
{
	entry *e = lookup(s);
	return e ? &e->def : NULL;
}

bool hle_set_enabled(const char *s, bool on)
{
	entry *e = lookup(s);
	if (!e)
		return false;
	e->enabled = on;
	bitmap_set(e->def.addr, on);
	return true;
}

#define MAX_GROUPS 8
static const char *g_groups[MAX_GROUPS];   /* active opt-in groups */

bool hle_group_active(const char *group)
{
	for (int i = 0; i < MAX_GROUPS; i++)
		if (g_groups[i] && group && !strcmp(g_groups[i], group))
			return true;
	return false;
}

void hle_set_all(bool on)
{
	for (int i = 0; i < g_n; i++) {
		bool e = on && (!g_ent[i].def.group || hle_group_active(g_ent[i].def.group));
		g_ent[i].enabled = e;
		bitmap_set(g_ent[i].def.addr, e);
	}
}

void hle_set_group(const char *group, bool on)
{
	if (on && !hle_group_active(group)) {
		for (int i = 0; i < MAX_GROUPS; i++)
			if (!g_groups[i]) {
				g_groups[i] = group;
				break;
			}
	} else if (!on) {
		for (int i = 0; i < MAX_GROUPS; i++)
			if (g_groups[i] && !strcmp(g_groups[i], group))
				g_groups[i] = NULL;
	}
	/* members follow the ordinary replacements: on if any of those is on */
	bool any = false;
	for (int i = 0; i < g_n; i++)
		if (!g_ent[i].def.group && g_ent[i].enabled)
			any = true;
	for (int i = 0; i < g_n; i++)
		if (g_ent[i].def.group && !strcmp(g_ent[i].def.group, group)) {
			g_ent[i].enabled = on && any;
			bitmap_set(g_ent[i].def.addr, g_ent[i].enabled);
		}
}

void hle_set_aim_fast(bool on) { g_aim_fast = on; }
bool hle_aim_fast(void) { return g_aim_fast; }

bool hle_configure(const char *spec)
{
	if (!spec)
		return true;
	char buf[4096];
	snprintf(buf, sizeof buf, "%s", spec);
	bool ok = true;
	/* Both ',' and '+' separate names. --options splits its own list on commas, so a list
	 * passed there (--options hle=Random+MenacerReadXY) must use '+'. A leading '+' or '-'
	 * on a single name still means on/off (it is consumed as a separator here). */
	for (char *t = strtok(buf, ",+"); t; t = strtok(NULL, ",+")) {
		if (!strcmp(t, "all") || !strcmp(t, "on"))
			hle_set_all(true);
		else if (!strcmp(t, "none") || !strcmp(t, "off"))
			hle_set_all(false);
		else {
			bool on = *t != '-';
			const char *n = *t == '-' ? t + 1 : t;
			if (!*n || !hle_set_enabled(n, on)) {
				fprintf(stderr, "hle: unknown function '%s' (see `t2port --hle-list`)\n", *n ? n : t);
				ok = false;
			}
		}
	}
	return ok;
}

void hle_list(void)
{
	for (int i = 0; i < g_n; i++)
		printf("$%06X  %-24s %-3s cyc=%-5d %s%s%s\n", g_ent[i].def.addr, g_ent[i].def.name,
		       g_ent[i].enabled ? "on" : "off", g_ent[i].def.cycles, g_ent[i].def.file,
		       g_ent[i].def.group ? "  group=" : "", g_ent[i].def.group ? g_ent[i].def.group : "");
}

int hle_count_active(void)
{
	int n = 0;
	for (int i = 0; i < g_n; i++)
		if (g_ent[i].enabled)
			n++;
	return n;
}

void hle_list_active(FILE *f)
{
	int n = 0;
	for (int i = 0; i < g_n; i++)
		if (g_ent[i].enabled)
			fprintf(f, "%s%s ($%06X)", n++ ? ", " : "", g_ent[i].def.name, g_ent[i].def.addr);
	fprintf(f, "%s\n", n ? "" : "(none)");
}

/* ------------------------------------------------------------------ CPU state <-> ctx */
static void ctx_load(hle_ctx *c)
{
	uint32_t *dar = gx_m68k_dar();
	memcpy(c->d, dar, sizeof c->d);
	memcpy(c->a, dar + 8, sizeof c->a);
	c->sr = (uint16_t)m68k_hle_get_sr();
	c->pc = gx_m68k_pc();
	c->jumped = 0;
	c->def = NULL;
	c->tl = 0;
	c->tl_used = 0;
	c->tl_last = 0;
	c->tl_flushed = 0;
	c->dry = 0;
	c->bail = 0;
}

/* ---- instruction timeline (hle.h) */
static int g_nest;             /* (defined below) depth of nested 68k execution */
static bool g_top;             /* (defined below) a top-level C function is running */
static void cor_check(hle_ctx *c);
static int g_bnd;              /* the next slice end the frame loop would have processed (tl_boundary) */
/* T2_HLE_SEQ=1: log, per top-level call, the list of instruction costs (the C side: what was
 * passed to hle_insn/hle_post; the T2_HLE_ORIG side: what each original instruction cost, without
 * the 14-mcycle refresh) so a C timeline can be diffed against the original (port/tools/seqcmp.py). */
static int g_seq_on = -1;
static int g_seq[1 << 16], g_seq_n;
static void seq_add(int n)
{
	if (g_seq_on > 0 && g_seq_n < (int)(sizeof g_seq / sizeof g_seq[0]))
		g_seq[g_seq_n++] = n;
}

/* Put the pending timeline on the CPU clock. */
static void tl_flush(hle_ctx *c)
{
	c->tl_flushed = 1;
	if (!hle_post_access) {
		gx_m68k_set_cycles(gx_m68k_cycles() + (int)c->tl);
		c->tl = 0;
	}
}

/* Between two of the original's instructions the frame loop may end the CPU's time slice (a line
 * end, or the VINT point): it runs the Z80 up to that point, and a Z80 read of 68k ROM then stalls
 * the 68k (memz80.c), shifting every later instruction - and with it the bus-refresh phase. A C
 * function that spans a slice end must do that at the same point of its own timeline. (Whether an
 * interrupt is raised there is decided before the call, see run_def.) Not in a dry run, nested, or
 * in the sandbox. */
static void tl_boundary(hle_ctx *c)
{
	if (hle_post_access || c->dry || !g_top)
		return;
	while (g_bnd <= gx_m68k_cycles() + (int)c->tl) {
		tl_flush(c);
		int b0 = gx_m68k_cycles();
		gx_z80_catchup(g_bnd);
		if (g_it_frame == g_frames_run)
			fprintf(stderr, "TB bnd %d at %d stall %d\n", g_bnd, b0, gx_m68k_cycles() - b0);
		g_bnd = gx_next_slice_end(g_bnd);
	}
}

/* the 68k's bus refresh: 2 cycles when the clock has reached refresh_cycles, checked at the start
 * of every instruction (m68kcpu.c) */
static void tl_refresh(hle_ctx *c)
{
	if (hle_post_access)
		return;
	int v = gx_m68k_cycles() + (int)c->tl;
	if (v >= gx_m68k_refresh_cycles()) {
		gx_m68k_set_refresh_cycles(v + 128 * 7);
		c->tl += 2 * 7;
	}
}
static int g_it_n;
static void it_c(hle_ctx *c, int n);
static void tl_start(hle_ctx *c)
{
	c->tl_used = 1;
	tl_boundary(c);
	it_c(c, g_it_n);
	tl_refresh(c);
}
static int g_it_lo, g_it_hi;
static void it_c(hle_ctx *c, int n)
{
	if (g_it_frame == g_frames_run && !c->dry) {
		int t = gx_m68k_cycles() + (int)c->tl;
		if (t >= g_it_lo && t < g_it_hi)
			fprintf(stderr, "IT %d %d %s r=%d\n", t, n, g_cur_def ? g_cur_def->name : "?", gx_m68k_refresh_cycles());
	}
}
void hle_insn(hle_ctx *c, int n)
{
	g_it_n = n;
	tl_start(c);
	c->tl += (long)n * 7;
	c->tl_last = (long)n * 7;
	if (!c->dry)
		seq_add(n);
	cor_check(c);
}
void hle_insn_x(hle_ctx *c, int n, int extra)
{
	g_it_n = n;
	tl_start(c);
	if (!hle_post_access && gx_m68k_cycles() + (int)c->tl + extra * 7 >= gx_m68k_refresh_cycles())
		gx_m68k_set_refresh_cycles(gx_m68k_refresh_cycles() + 128 * 7);   /* SKIP_BUS_REFRESH */
	c->tl += (long)n * 7;
	c->tl_last = (long)n * 7;
	if (!c->dry)
		seq_add(n);
	cor_check(c);
}
static int g_pre_clock;
void hle_sync(hle_ctx *c)
{
	c->tl_used = 1;
	cor_check(c);
	tl_boundary(c);
	tl_flush(c);
	g_pre_clock = gx_m68k_cycles();
}
void hle_pre(hle_ctx *c)
{
	c->tl_used = 1;
	cor_check(c);
	tl_boundary(c);
	tl_refresh(c);
	tl_flush(c);
	g_pre_clock = gx_m68k_cycles();
}
/* the access itself can stall the CPU (a Z80 ROM read at a bus request, VDP FIFO...): that time is
 * already on the clock; only the log shows it as part of the instruction */
void hle_post(hle_ctx *c, int n)
{
	c->tl += (long)n * 7;
	c->tl_last = (long)n * 7;
	seq_add(n + (hle_post_access ? 0 : (gx_m68k_cycles() - g_pre_clock) / 7));
	cor_check(c);
}
void hle_seq(hle_ctx *c, const int *costs, int n)
{
	for (int i = 0; i < n; i++)
		hle_insn(c, costs[i]);
}

/* Registers and PC are stored before SR: lowering the interrupt mask can take a pending
 * interrupt right away, which must stack the final PC/SP (as after the original rts...).
 * A C function must not change the S bit (the game always runs in supervisor mode). */
static void ctx_store(const hle_ctx *c)
{
	uint16_t old = (uint16_t)m68k_hle_get_sr();
	uint32_t *dar = gx_m68k_dar();
	memcpy(dar, c->d, sizeof c->d);
	memcpy(dar + 8, c->a, sizeof c->a);
	gx_m68k_set_pc(c->pc);
	if ((old & 0xFF00) == (c->sr & 0xFF00))
		m68k_hle_set_ccr(c->sr & 0xFF);
	else
		m68k_hle_set_sr(c->sr);
}

static long g_stall_debt;      /* cycles still to burn at STALL_PC */
static int g_nest;             /* depth of nested 68k execution */
static bool g_top;             /* a top-level C function is running */
/* "session": the nested 68k time of the current top-level C function. The clock, raster
 * line base and V counter keep advancing from one nested call to the next (so code after
 * the 2nd call sees the time the 1st took, as in the original, #33) and are rewound when
 * the C function returns; the time becomes part of its charge (#31). */
static bool g_sess;
static bool g_sess_timed;      /* the session made timed VDP data writes (hle_vdp_timed) */
static bool g_vdp_timed;
static bool g_decline;
static int g_why = 15;
void hle_decline_why(int why) { if (!hle_post_access) { g_decline = true; g_why = why; } }
void hle_decline(void) { hle_decline_why(15); }
/* decline statistics: per replaced function and reason (1 frame end, 2 interrupt window, 3 pending irq, 10.. see hle.h) */
#define DWHY 20
static struct { const char *name; unsigned long n[DWHY]; } g_dstat[256];
static unsigned long g_dcalls[256];
static int g_dn;
static void dstat(const hle_def *d, int why)
{
	int i;
	for (i = 0; i < g_dn; i++)
		if (g_dstat[i].name == d->name)
			break;
	if (i == g_dn) {
		if (g_dn == 256)
			return;
		g_dstat[g_dn++].name = d->name;
	}
	g_dstat[i].n[why]++;
}
static int g_sess_c0, g_sess_vc, g_sess_r0;
static unsigned g_sess_line_base;

int hle_nested_cycles(void) { return g_sess ? (gx_m68k_cycles() - g_sess_c0) / 7 : 0; }

/* Nested 68k runs for real, so its measured time includes the 68k bus-refresh cycles the
 * original adds every 128 cycles; subtract them so the caller can re-account refresh over
 * the whole function (run_def). Returns the nested BASE cycles and rewinds clock + refresh
 * phase to the session start. */
static bool g_exact;           /* the last session's time is exact (refresh and stalls included) */

static int sess_end(bool exact)
{
	if (!g_sess)
		return 0;
	g_exact = false;
	if (g_sess_timed) {
		/* The C code advanced the real clock instruction by instruction (refresh and VDP
		 * stalls included): keep the refresh phase and the FIFO stamps, charge the elapsed
		 * time, and keep its sub-cycle remainder (a stall ends on a 7-mclk boundary) so the
		 * clock after the frame loop adds the charge is exactly the real one. */
		int el = gx_m68k_cycles() - g_sess_c0;
		gx_m68k_set_cycles(g_sess_c0 + el % 7);
		gx_vdp_set_line_base(g_sess_line_base);
		gx_vdp_set_vcounter(g_sess_vc);
		gx_m68k_resync_irq();
		g_sess = false;
		g_sess_timed = false;
		gx_follow_z80(0);
		g_exact = true;
		return el / 7;
	}
	int n_nested = (gx_m68k_refresh_cycles() - g_sess_r0) / 896;
	int n = (gx_m68k_cycles() - g_sess_c0) / 7 - 2 * n_nested;
	if (exact) {
		/* the C function keeps an exact instruction timeline (hle_insn): the nested time, bus
		 * refresh included, is real; keep the refresh phase it ended with */
		n = (gx_m68k_cycles() - g_sess_c0) / 7;
	} else {
		gx_m68k_set_refresh_cycles(g_sess_r0);
	}
	gx_m68k_set_cycles(g_sess_c0);
	gx_vdp_fifo_clamp();
	gx_vdp_set_line_base(g_sess_line_base);
	gx_vdp_set_vcounter(g_sess_vc);
	gx_m68k_resync_irq();
	g_sess = false;
	return n > 0 ? n : 0;
}

static bool g_decline_on = true;   /* T2_HLE_NODECLINE=1 turns the interrupt-atomicity fallback off */
static unsigned long g_declined;
static uint8_t g_snap[0x10000];
static const hle_def *g_cur_def;   /* innermost C function running (NULL at 68k top level) */

/* T2_HLE_ORIG=name[,name...]: run these replacements as the ORIGINAL 68k code (like difftest
 * --passthrough) and log the real cycle cost next to the C charge. Debugging aid for the
 * lockstep gate: it shows how far a charge is from the original in the full frame loop. */
static char g_orig[1024];
static bool def_is_orig(const hle_def *d)
{
	if (!g_orig[0] || !d->name)
		return false;
	const char *p = g_orig;
	size_t l = strlen(d->name);
	while (*p) {
		const char *e = strpbrk(p, ",");
		size_t n = e ? (size_t)(e - p) : strlen(p);
		if (n == l && !strncmp(p, d->name, l))
			return true;
		if (!e)
			break;
		p = e + 1;
	}
	return false;
}

/* T2_HLE_BOUNDS=1 (check mode): measure every executed top-level non-pure call from entry to return
 * (C + nested 68k) and compare it with the bound the decline guard uses: HLE_FUNC's cycle argument,
 * or, for the VDP list commands, the safe_run() bound noted through hle_chk_note (one segment per
 * noted bound, up to the next note or the end of the call). At exit it prints, per function, the
 * declared bound, the worst observed span, the calls, and OVER when a call exceeded its bound. */
bool hle_chk_on;
typedef struct { const hle_def *def; long calls, over, maxobs, maxbound, wobs, wbound; } chk_ent;
static chk_ent g_chk[256];
static int g_chk_n;
static long g_chk_clock0;      /* entry clock of the running top-level call */
static const hle_def *g_chk_def;
static long g_chk_seg_start, g_chk_seg_bound;
static bool g_chk_seg;
static hle_ctx *g_chk_ctx;

static chk_ent *chk_find(const hle_def *d)
{
	for (int i = 0; i < g_chk_n; i++)
		if (g_chk[i].def == d)
			return &g_chk[i];
	if (g_chk_n == (int)(sizeof g_chk / sizeof g_chk[0]))
		return &g_chk[g_chk_n - 1];
	g_chk[g_chk_n].def = d;
	return &g_chk[g_chk_n++];
}
static void chk_record(const hle_def *d, long obs, long bound)
{
	chk_ent *e = chk_find(d);
	e->calls++;
	if (obs > e->maxobs)
		e->maxobs = obs;
	if (bound > e->maxbound)
		e->maxbound = bound;
	if (obs > bound)
		e->over++;
	if (!e->wbound || obs * e->wbound > e->wobs * bound) {   /* worst obs/bound ratio */
		e->wobs = obs;
		e->wbound = bound;
	}
}
void hle_chk_note(int bound_cycles)
{
	if (!g_chk_ctx || !g_chk_def)
		return;
	long now = (gx_m68k_cycles() + g_chk_ctx->tl - g_chk_clock0) / 7;
	if (g_chk_seg)
		chk_record(g_chk_def, now - g_chk_seg_start, g_chk_seg_bound);
	g_chk_seg = true;
	g_chk_seg_start = now;
	g_chk_seg_bound = bound_cycles;
}
static void chk_print(void)
{
	fprintf(stderr, "hle bounds: %-22s %8s %9s %8s %s\n", "function", "bound", "max obs", "calls", "worst obs/bound");
	for (int i = 0; i < g_chk_n; i++) {
		const chk_ent *e = &g_chk[i];
		const char *flag = e->over ? "OVER" : (e->wbound && e->wobs * 10 > e->wbound * 8) ? "near" : "";
		fprintf(stderr, "hle bounds: %-22s %8ld %9ld %8ld %ld/%ld %s%s\n", e->def->name, e->maxbound, e->maxobs,
		        e->calls, e->wobs, e->wbound, flag, e->maxbound ? "" : " NOBOUND");
	}
}

int hle_nest_depth(void) { return g_nest; }
const hle_def *hle_current_def(void) { return g_cur_def; }
/* would the CPU hand this pc to a C function instead of executing the instruction? (profilers) */
bool hle_pc_replaced(uint32_t pc)
{
	if (pc == STALL_PC || pc == YIELD_PC)
		return true;
	if (pc >= 0x400000 || !(g_bitmap[pc >> 4] & (1u << ((pc >> 1) & 7))) || pc == m68k_hle_skip_pc)
		return false;
	entry *e = find_addr(pc);
	return e && e->enabled;
}

unsigned long hle_declined_total(void) { return g_declined; }
static unsigned long g_bailed;
unsigned long hle_bailed_total(void) { return g_bailed; }
static bool g_bail_on;     /* a top-level HLE_F_BAIL call is running */
static int g_bail_at;      /* its first event: the first interrupt-raising slice end or the frame end (mclk) */
bool hle_bail_due(hle_ctx *c, int chunk_cycles)
{
	if (!g_bail_on || c->dry || hle_post_access)
		return false;
	return gx_m68k_cycles() + (int)c->tl + chunk_cycles * 7 >= g_bail_at;
}
static void dstat(const hle_def *d, int why);
void hle_bail_to(hle_ctx *c, uint32_t pc)
{
	if (g_bail_on && g_cur_def)
		dstat(g_cur_def, 9);
	g_bailed++;            /* counted as "bails:" in the T2_HLE_DECLINES table */
	c->pc = pc;
	c->jumped = 1;
	c->bail = 1;
}

static const hle_def *g_last_entered;
/* ---- coroutines: a C function that is interrupted by the frame loop (docs/porting.md, "Yielding").
 * A top-level call of a yield-capable function (HLE_F_YIELD / HLE_F_BAIL, or a PURE function with an
 * exact timeline whose window holds an event) runs on its own stack. At every instruction boundary at
 * or after the CPU's slice end (m68k.cycle_end: where the core's loop would stop) the function
 * yields: the time so far is charged, the CPU PC becomes YIELD_PC and the hook returns, so the
 * frame loop does its line work and the core takes an interrupt exactly where the original does. The
 * interrupt returns to YIELD_PC, whose hook switches back into the function. No 68k code is needed. */
#define COR_STACK (256 * 1024)
#define COR_MAX 6
typedef struct rs rs;
struct rs {
	hle_ctx c;
	const hle_def *def;
	entry *e;
	const hle_def *outer_def;
	bool top, chk, unmasked, orig, cor, done, used;
	bool irqd_ok, y_force;     /* delayed-IRQ model enabled (hle_irqd_enable); the yield forces the interrupt check */
	uint32_t sp0;
	int clock0;
	int y_nest;
	bool y_sess, y_timed;
	ucontext_t uc, back;
	char *stack;
};
static rs g_pool[COR_MAX];
static rs *g_susp[COR_MAX];
static int g_nsusp;
static rs *g_cor;              /* the coroutine whose function is running now */
static int g_cor_block;        /* >0 inside hle_call68k / hle_run68k_until made by the function */
static bool g_cor_on = true;   /* T2_HLE_NOCOR=1: decline / bail as before */
unsigned long g_yields;

static char g_nocor_file[1024];  /* T2_HLE_NOCOR_FILE=a.c,b.c (debugging) */
static char g_nocor_fn[1024];  /* T2_HLE_NOCOR_FN=name,name: these decline / bail as before (debugging) */
static bool name_listed(const char *list, const char *name)
{
	if (!list[0] || !name)
		return false;
	size_t l = strlen(name);
	for (const char *p = list; *p;) {
		const char *e = strchr(p, ',');
		size_t n = e ? (size_t)(e - p) : strlen(p);
		if (n == l && !strncmp(p, name, l))
			return true;
		if (!e)
			break;
		p = e + 1;
	}
	return false;
}

static rs *rs_alloc(void)
{
	for (int i = 0; i < COR_MAX; i++)
		if (!g_pool[i].used) {
			g_pool[i].used = true;
			return &g_pool[i];
		}
	return NULL;
}

bool hle_yielding(void) { return g_cor && !g_cor_block && !g_cor->c.dry; }

/* T2_LINEHASH=LO-HI (work RAM offsets, hex): print "LH frame slice_end hash" at every slice end - in the C
 * build at each yield, in the 68k-only build at the first instruction of the next slice - so the two runs can
 * be diffed line by line to find where a region first differs (debugging aid, no effect when unset). */
static bool g_lh_on;
static uint32_t g_lh_lo, g_lh_hi;
static int g_lh_end = -1;
static const char *g_lh_who = "";
static void lh_emit(int end)
{
	uint64_t h = 1469598103934665603ull;
	for (uint32_t i = g_lh_lo; i < g_lh_hi; i++)
		h = (h ^ work_ram[i]) * 1099511628211ull;
	fprintf(stderr, "LH %ld %d %016llx %s\n", g_frames_run, end, (unsigned long long)h, g_lh_who);
	if (getenv("T2_LH_DUMP") && g_frames_run == atol(getenv("T2_LH_DUMP"))) {
		fprintf(stderr, "LD %ld %d ", g_frames_run, end);
		for (uint32_t i = g_lh_lo; i < g_lh_hi; i++)
			fprintf(stderr, "%02x", work_ram[i]);
		fprintf(stderr, "\n");
	}
}
static void lh_frame(void) { if (g_lh_on) lh_emit(0); }
static bool lh_vec(uint32_t pc)
{
	for (int v = 0x64; v <= 0x7C; v += 4)
		if (pc == (ROM32(v) & 0xFFFFFF))
			return true;
	return false;
}
static void lh_hook(int type, int width, unsigned addr, unsigned value)
{
	(void)width; (void)value;
	if (type == 1 && g_it_frame == g_frames_run) {
		int t = gx_m68k_cycles();
		if (t >= g_it_lo && t < g_it_hi)
			fprintf(stderr, "IX %d %06X r=%d\n", t, addr, gx_m68k_refresh_cycles());
	}
	if (type == 1 && lh_vec(addr))
		lh_emit(gx_m68k_cycles());
}

static void cor_do_yield(rs *s)
{
	tl_flush(&s->c);
	g_yields++;
	dstat(s->def, 8);
	if (getenv("T2_HLE_YLOG"))
		fprintf(stderr, "yield %s clock %d end %d sp %06X frame %ld\n", s->def->name, gx_m68k_cycles(), gx_m68k_cycle_end(), s->c.a[7], g_frames_run);
	swapcontext(&s->uc, &s->back);
}
static void cor_check(hle_ctx *c)
{
	if (g_cor && c == &g_cor->c && !g_cor_block && gx_m68k_cycles() + (int)c->tl >= gx_m68k_cycle_end())
		cor_do_yield(g_cor);
}
/* The delayed IRQ of a VDP control port write (m68k_set_irq_delay, gpgx-irqdelay.patch): a write that enables
 * the frame/line interrupt while one is pending makes the original core run the NEXT instruction and then take the
 * interrupt (the nested instruction has no refresh check, its cost is added after the exception). A coroutine
 * that opted in (hle_irqd_enable, game/vdpfill.c) models it: the hook below arms it during the write, the
 * instruction after the write runs (without a refresh check: hle_irqd_nested), and at the next instruction
 * boundary the function yields at once - however far the slice end is - with that instruction's charge held back
 * (m68k_hle_irq_extra: the exception adds it after its own cycles) and the core checking the interrupt as soon as
 * the hook returns. A natural slice end between the write and the next instruction does not yield. */
extern int (*m68k_hle_irq_delay)(unsigned int level);
extern int m68k_hle_irq_check, m68k_hle_irq_extra;
static int g_irqd;        /* 0 idle, 1 the write is running, 2 the instruction after it is running */
static int g_irqd_last;   /* master cycles of the last instruction the exact code charged (hle_irqd_note) */
static int irqd_hook(unsigned int level)
{
	(void)level;
	if (!g_cor || g_cor_block || !g_cor->irqd_ok || g_irqd || g_cor->c.dry)
		return 0;
	g_irqd = 1;
	return 1;
}
void hle_irqd_enable(void) { if (g_cor) g_cor->irqd_ok = true; }
void hle_irqd_note(int mclk) { g_irqd_last = mclk; }
bool hle_irqd_nested(void) { return g_irqd == 2; }
void hle_yield_point(void)
{
	if (!g_cor || g_cor_block)
		return;
	if (g_irqd == 1) {                /* the boundary right after the write: the next instruction runs first */
		g_irqd = 2;
		return;
	}
	if (g_irqd == 2) {                /* ...and now the interrupt: before that instruction's charge */
		g_irqd = 0;
		gx_m68k_set_cycles(gx_m68k_cycles() - g_irqd_last);
		m68k_hle_irq_extra = g_irqd_last;
		m68k_hle_irq_check = 1;
		g_cor->y_force = true;
		cor_do_yield(g_cor);
		return;
	}
	if (gx_m68k_cycles() + (int)g_cor->c.tl >= gx_m68k_cycle_end())
		cor_do_yield(g_cor);
}

static void cor_entry(void)
{
	rs *s = g_cor;
	s->def->fn(&s->c);
	s->done = true;
	if (g_irqd) {
		fprintf(stderr, "hle: %s ended with a delayed IRQ armed (state %d)\n", s->def->name, g_irqd);
		g_irqd = 0;
	}
	swapcontext(&s->uc, &s->back);
}
static void cor_run(rs *s, bool start)
{
	if (start) {
		if (!s->stack)
			s->stack = malloc(COR_STACK);
		getcontext(&s->uc);
		s->uc.uc_stack.ss_sp = s->stack;
		s->uc.uc_stack.ss_size = COR_STACK;
		s->uc.uc_link = NULL;
		makecontext(&s->uc, cor_entry, 0);
	}
	g_cor = s;
	swapcontext(&s->back, &s->uc);
	g_cor = NULL;
}

static int rs_finish(rs *s);

/* the VDP session code whose waits need the frame loop (docs/native.md): vdpfill.c, vdplist.c, textprint.c */
static bool nat_cor_file(const hle_def *def)
{
	const char *f = strrchr(def->file ? def->file : "", '/');
	f = f ? f + 1 : (def->file ? def->file : "");
	(void)f;
	return def->addr == 0x00800C || def->addr == 0x00807C || def->addr == 0x007DEE;
}

static bool cor_ok_early(const hle_def *def)
{
	return g_cor_on && (def->flags & HLE_F_YIELD);
}

static int run_def(const hle_def *def, entry *e)
{
	g_last_entered = def;
	rs local;
	rs *s = &local;
	const hle_def *outer_def = g_cur_def;
	bool top = g_nest == 0 && !g_top;
	bool cor = false;
	/* Interrupt atomicity (docs/porting.md, "Interrupts inside a replaced function"): when the
	 * original would have been interrupted INSIDE this call, it must run as 68k. A pure function
	 * (HLE_F_PURE: work RAM only) is run once dry to learn its real length; the others are judged
	 * by def->cycles, the longest window they take. A function with an exact instruction timeline
	 * does not decline: it runs as a coroutine and yields to the frame loop at the event. */
	int irq_mask = (m68k_hle_get_sr() >> 8) & 7;
	int clock0 = gx_m68k_cycles();
	bool lowmask = (def->flags & HLE_F_LOWMASK) != 0;
	if (top)
		g_bnd = gx_m68k_cycle_end();
	if (top)
		g_bail_on = false;
	/* the native runner has no declines, but an exact-clock session function (cycles 0: VDP code, the DMA
	 * routines that poll a busy flag the frame loop clears) still has to hand the machine to the frame loop */
	bool nat_cor = native_active && cor_ok_early(def) && nat_cor_file(def) && top && !hle_post_access && !def_is_orig(def);
	bool guard = g_decline_on && top && !hle_post_access && !def_is_orig(def) && !g_stall_debt;
	const char *fbase = strrchr(def->file ? def->file : "", '/');
	fbase = fbase ? fbase + 1 : (def->file ? def->file : "");
	bool cor_ok = g_cor_on && !name_listed(g_nocor_fn, def->name) && !name_listed(g_nocor_file, fbase);
	bool yieldf = cor_ok && (def->flags & HLE_F_YIELD);
	if (nat_cor) {
		cor = true;
	} else if (guard && yieldf && (def->cycles == 0 || (def->flags & HLE_F_BAIL))) {
		/* an exact-clock session function (VDP list, text printers): its own guards need the frame loop
		 * to run in between at any line, so it is always a coroutine */
		cor = true;
	} else if (guard && !yieldf && (def->flags & HLE_F_BAIL)) {
		/* no decline: the function bails to the 68k before the first event (hle_bail_due) */
		int at = gx_frame_end_clock();
		if (irq_mask < 6) {
			int c0 = clock0;
			for (;;) {
				int a2, lvl = gx_irq_boundary_in(c0, at, &a2);
				if (!lvl)
					break;
				if (irq_mask < lvl) {
					at = a2;
					break;
				}
				c0 = a2;
			}
		}
		g_bail_at = at;
		g_bail_on = true;
	} else if (guard) {
		long window = -1, span = (long)def->cycles * 7;   /* span: how long the call can take */
		bool exact = false;
		if (def->flags & HLE_F_PURE) {
			hle_ctx d;
			int refresh0 = gx_m68k_refresh_cycles();
			ctx_load(&d);
			d.def = def;
			d.dry = 1;
			memcpy(g_snap, work_ram, sizeof g_snap);
			def->fn(&d);
			memcpy(work_ram, g_snap, sizeof g_snap);
			gx_m68k_set_refresh_cycles(refresh0);
			exact = cor_ok && d.tl_used && !getenv("T2_HLE_NOPUREEXACT");
			/* the dry run has no Z80 stall at slice ends (up to ~80 mcycles each): margin */
			long margin = 300 + (d.tl / 3420 + 1) * 100;   /* one Z80 stall per slice end crossed */
			span = d.tl + margin;
			if (irq_mask < 6)
				window = d.tl - d.tl_last + margin;
		} else if (yieldf) {
			exact = true;               /* exact timeline: a coroutine instead of a decline (below) */
			/* the declared cycles of a generated whole-function port (piecegen: 4000) is not a bound: a loop in
			 * it (SpawnRowB's ObjAlloc spawns) can run across the frame end. Judge by a generous span, so an
			 * event that falls inside makes it a coroutine. */
			if (span < YIELD_SPAN)
				span = YIELD_SPAN;
			if (irq_mask < 6)
				window = span;
		} else if (irq_mask < 6 || lowmask) {
			window = span;
			if (lowmask) {
				irq_mask = 0;
				if (gx_m68k_pending_irq()) {   /* taken as soon as the function's ReadPad drops the mask */
					dstat(def, 3);
					g_declined++;
					return -1;
				}
			}
		}
		/* The frame loop rebases the clocks at the end of the frame and starts the next one
		 * (VDP frame work, the VINT slice...): a call that straddles it cannot be a C lump. */
		if (clock0 + span > gx_frame_end_clock()) {
			if (exact) {
				cor = true;
			} else {
				dstat(def, 1);
				g_declined++;
				return -1;
			}
		}
		if (!cor && window >= 0) {
			int at, lvl = gx_irq_boundary_in(clock0, clock0 + (int)window, &at);
			if (lvl && irq_mask < lvl) {
				if (exact) {
					cor = true;
				} else {
					if (getenv("T2_HLE_DECLINES"))
						fprintf(stderr, "decline %s: clock %d window %ld lvl %d at %d mask %d\n", def->name, clock0, window, lvl, at, irq_mask);
					dstat(def, 2);
					g_declined++;
					return -1;
				}
			}
		}
	}
	if (cor) {
		s = rs_alloc();
		if (!s) {
			fprintf(stderr, "hle: out of coroutine slots (%s)\n", def->name);
			abort();
		}
	} else {
		s->stack = NULL;
	}
	s->def = def;
	s->e = e;
	s->outer_def = outer_def;
	s->top = top;
	s->cor = cor;
	s->done = false;
	s->irqd_ok = false;
	s->y_force = false;
	s->clock0 = clock0;
	s->orig = def_is_orig(def);
	s->chk = hle_chk_on && top && !hle_post_access && !(def->flags & HLE_F_PURE) && !s->orig;
	if (top)
		g_top = true;
	hle_ctx *cp = &s->c;
	ctx_load(cp);
	if (s->chk) {
		g_chk_clock0 = clock0;
		g_chk_def = def;
		g_chk_ctx = cp;
		g_chk_seg = false;
	}
	s->sp0 = cp->a[7];
	s->unmasked = ((cp->sr >> 8) & 7) < 6;
	cp->def = def;
	cp->cycles = def->cycles;
	if (e)
		e->calls++;
	g_frame_calls++;
	g_cur_def = def;
	if (s->orig) {
		/* passthrough: run the original 68k and log its real cost vs the C charge */
		long c0 = gx_m68k_cycles();
		if (def->exits)
			hle_run68k_until(cp, def->exits);
		else
			hle_call68k_orig(cp, cp->pc);
		long d = (gx_m68k_cycles() - c0) / 7;
		fprintf(stderr, "orig %-18s real %ld cyc  (C charge %d, delta %ld)\n",
		        def->name, d, def->cycles, d - def->cycles);
		cp->cycles = (int)(d > 0 ? d : 0);
	} else if (cor) {
		cor_run(s, true);
	} else {
		def->fn(cp);
	}
	return rs_finish(s);
}

/* what is left of a call once its function has returned or yielded */
static int rs_finish(rs *s)
{
	hle_ctx *cp = &s->c;
#define c (*cp)
	const hle_def *def = s->def;
	entry *e = s->e;
	bool top = s->top;
	bool yielded = s->cor && !s->done;
	if (!s->orig) {
		if (c.tl_used)
			c.cycles = (int)(c.tl / 7);
		else if (yielded)
			c.cycles = 0;
	}
	if (top)
		g_bail_on = false;
	g_cur_def = s->outer_def;
	if (g_decline) {
		g_decline = false;
		if (yielded) {
			fprintf(stderr, "hle: %s declined after a yield\n", def->name);
			abort();
		}
		if (top) {
			dstat(def, g_why);
			g_declined++;
		}
		g_why = 15;
		if (top)
			g_top = false;
		if (e)
			e->calls--;
		g_frame_calls--;
		g_seq_n = 0;
		if (s->cor)
			s->used = false;
		return -1;
	}
	if (yielded) {
		/* hand the machine to the frame loop at this instruction boundary: the time so far is the
		 * charge, the session (if the function has one) ends and is re-opened on resume */
		int lvl = gx_m68k_pending_irq();
		s->y_nest = g_nest;
		s->y_sess = g_sess;
		s->y_timed = g_sess_timed;
		long nested = sess_end(c.tl_used);
		g_nest = 0;
		g_top = false;
		if (s->y_force)
			s->y_force = false;          /* the VDP's own level (sess_end resynced it): the delayed IRQ is due now */
		else
			gx_m68k_set_irq_level(lvl);
		long total = (c.cycles > 0 ? c.cycles : 0) + nested;
		c.cycles = 0;
		uint32_t pc_keep = c.pc;
		c.pc = YIELD_PC;
		ctx_store(cp);
		c.pc = pc_keep;
		g_susp[g_nsusp++] = s;
		return (int)total;
	}
	if (g_seq_on > 0 && top) {
		fprintf(stderr, "seq %s:", def->name);
		for (int i = 0; i < g_seq_n; i++)
			fprintf(stderr, " %d", g_seq[i]);
		fprintf(stderr, "\n");
		g_seq_n = 0;
	}
	if (def->exits) {
		/* HLE_RANGE: must leave through a declared exit */
		if (!c.jumped || !(hle_exit_match(def->exits, c.pc, c.a[7], s->sp0) || (c.bail && c.a[7] == s->sp0))) {
			fprintf(stderr, "hle: range %s ($%06X) left via $%06X with SP %+d%s, not a declared exit\n",
			        def->name, def->addr, c.pc, (int)(c.a[7] - s->sp0), c.jumped ? "" : " (no hle_jump)");
			abort();
		}
	} else if (!c.jumped) {
		c.pc = bus_r32(c.a[7]);   /* rts */
		c.a[7] += 4;
	}
	long nested = 0;
	g_exact = false;
	if (top) {
		nested = sess_end(c.tl_used);
		g_top = false;
	}
	long total = (c.cycles > 0 ? c.cycles : 0) + nested;
	/* The original 68k adds 2 bus-refresh cycles every 128 cycles while executing
	 * instructions (m68kcpu.c, `refresh_cycles`); the lump charge for the C part does not,
	 * so the CPU clock ends short and the frame phase drifts. Add the refresh the C part
	 * would have accrued (the gap after each refresh is 896-14 = 882 mcycles) and advance
	 * refresh_cycles so the next real instruction keeps the right phase. Nested 68k ran for
	 * real, so its refresh is already inside `nested`. */
	if (!s->orig && total > 0 && !hle_post_access && !c.tl_used && !(top && g_exact)) {
		int delta = (int)(total * 7);
		int R = gx_m68k_refresh_cycles(), C = gx_m68k_cycles();
		int gap = R - C;
		int n = delta >= gap ? 1 + (delta - gap) / 882 : 0;
		if (n > 0) {
			total += 2 * n;
			gx_m68k_set_refresh_cycles(R + 896 * n);
		}
	}
	if (s->chk) {
		g_chk_ctx = NULL;
		g_chk_def = NULL;
		if (g_chk_seg)
			chk_record(def, total - g_chk_seg_start, g_chk_seg_bound);
		else
			chk_record(def, total, def->cycles);
	}
	if (getenv("T2_HLE_CHARGE"))
		fprintf(stderr, "charge %-18s %ld (ctx %d + nested %ld)\n", def->name, total, c.cycles, nested);
	/* A bail (game/vdplist.c) ends at the very instruction boundary where the original's slice ends and
	 * the frame loop raises the interrupt: the lump must not be cut in slices (an interrupt would be
	 * taken before its last few cycles, which belong in front of it). */
	if (s->unmasked && total > STALL_SLICE && !c.bail && !s->cor && !hle_post_access && g_nest == 0 && !g_stall_debt) {
		/* Long function entered with VBlank unmasked: the original would have been
		 * interrupted along the way. Spend the cost in slices at STALL_PC, a trap that
		 * "returns" to the real continuation (pushed like a jsr, so a savestate taken
		 * meanwhile just resumes there) - interrupts are taken between the slices. */
		c.a[7] -= 4;
		bus_w32(c.a[7], c.pc);
		c.pc = STALL_PC;
		g_stall_debt = total - STALL_SLICE;
		total = STALL_SLICE;
	}
	ctx_store(&c);
	if (s->cor)
		s->used = false;
	return (int)total;
#undef c
}

/* YIELD_PC: an interrupt (or nothing) brought the CPU back to where a function yielded: continue it */
static int resume_cor(void)
{
	if (!g_nsusp) {
		fprintf(stderr, "hle: YIELD_PC reached with no suspended function\n");
		abort();
	}
	rs *s = g_susp[--g_nsusp];
	if (m68k_hle_irq_extra) {            /* the delayed IRQ was masked: the held-back instruction is charged now */
		gx_m68k_set_cycles(gx_m68k_cycles() + m68k_hle_irq_extra);
		m68k_hle_irq_extra = 0;
	}
	g_top = true;
	g_cur_def = s->def;
	g_bnd = gx_m68k_cycle_end();
	g_bail_on = false;
	g_nest = s->y_nest;
	if (s->y_sess) {
		g_sess = true;
		g_sess_c0 = gx_m68k_cycles();
		g_sess_r0 = gx_m68k_refresh_cycles();
		g_sess_line_base = gx_vdp_line_base();
		g_sess_vc = gx_vdp_vcounter();
		if (s->y_timed) {
			g_sess_timed = true;
			gx_follow_z80(1);
		}
	}
	cor_run(s, false);
	return rs_finish(s);
}

/* hle_callc: a `jsr` made by a coroutine to a function that is C (and keeps an exact timeline) runs as a
 * direct C call that shares ctx, so the frame loop can interrupt the callee too; anything else (not C,
 * disabled, a range, a lump-charged function, or the caller is not a coroutine) is hle_call68k. */
static int g_trace = -1;   /* T2_HLE_TRACE=1: log every dispatched call, 2: also hle_callc */
static unsigned long g_callc_fb[64][2];
static int g_callc_nfb;
/* can the registered function at e be called directly (it keeps an exact timeline and leaves the stack as a
 * plain function does)? */
static bool callc_exact(entry *e, hle_ctx *c)
{
	if (!e || !e->enabled || (e->def.flags & (HLE_F_WAIT | HLE_F_LOWMASK)))
		return false;
	if (e->def.exits)                  /* a range: only if every exit leaves with the stack as it was */
		for (const uint32_t *x = e->def.exits; *x; x++)
			if (hle_exit_sp_delta(*x) != 0)
				return false;
	if (e->def.flags & HLE_F_YIELD)
		return true;
	if (e->def.flags & HLE_F_PURE) {   /* exact when the dry run keeps a timeline */
		hle_ctx d = *c;
		int refresh0 = gx_m68k_refresh_cycles();
		d.dry = 1;
		d.tl = 0;
		d.tl_used = 0;
		d.jumped = 0;
		memcpy(g_snap, work_ram, sizeof g_snap);
		e->def.fn(&d);
		memcpy(work_ram, g_snap, sizeof g_snap);
		gx_m68k_set_refresh_cycles(refresh0);
		return d.tl_used != 0;
	}
	return false;
}

void hle_callc(hle_ctx *c, uint32_t addr)
{
	if (g_trace > 1)
		fprintf(stderr, "callc $%06X (cor %d)\n", addr, g_cor != NULL);
	if (g_cor && !g_cor_block && c == &g_cor->c && !hle_post_access && !c->dry && !getenv("T2_HLE_NOCALLC")) {
		entry *e = find_addr(addr);
		if (callc_exact(e, c)) {
			/* a real jsr: the return address (a sentinel) is on the stack below whatever the callee pushes, so
			 * its rts, and a tail jump into 68k code that pops and returns, work as on the 68k */
			e->calls++;
			g_frame_calls++;
			c->a[7] -= 4;
			bus_w32(c->a[7], SENTINEL_RET);
			uint32_t sp_ret = c->a[7];
			hle_fn fn = e->def.fn;
			for (;;) {
				fn(c);
				if (!c->jumped) {              /* the rts */
					c->a[7] += 4;
					return;
				}
				uint32_t t = c->pc;            /* a tail jump */
				c->jumped = 0;
				entry *e2 = find_addr(t);
				if (callc_exact(e2, c)) {
					e2->calls++;
					fn = e2->def.fn;
					continue;
				}
				hle_call68k_nopush(c, t, sp_ret + 4);   /* the original continues at t and returns to the sentinel */
				return;
			}
		}
		/* statistics: what a coroutine still runs as nested 68k */
		int i;
		for (i = 0; i < g_callc_nfb; i++)
			if (g_callc_fb[i][0] == addr)
				break;
		if (i == g_callc_nfb && g_callc_nfb < 64) {
			g_callc_fb[g_callc_nfb][0] = addr;
			g_callc_fb[g_callc_nfb++][1] = 0;
		}
		if (i < g_callc_nfb)
			g_callc_fb[i][1]++;
	}
	hle_call68k(c, addr);
}

void hle_state_reset(void)
{
	g_cur_def = NULL; g_stall_debt = 0; g_sess = false; g_sess_timed = false; g_vdp_timed = false;
	gx_follow_z80(0); g_top = false; g_nest = 0;
	g_nsusp = 0; g_cor = NULL; g_cor_block = 0;
	for (int i = 0; i < COR_MAX; i++)
		g_pool[i].used = false;
}

/* STALL_PC: burn the next slice of g_stall_debt; then rts to the continuation */
static int stall(void)
{
	long n = g_stall_debt < STALL_SLICE ? g_stall_debt : STALL_SLICE;
	g_stall_debt -= n;
	if (g_stall_debt <= 0) {
		g_stall_debt = 0;
		uint32_t *dar = gx_m68k_dar();
		gx_m68k_set_pc(bus_r32(dar[15]) & 0xFFFFFF);
		dar[15] += 4;
	}
	return (int)(n > 0 ? n : 4);
}

/* ------------------------------------------------------------------ T2_HLE_PROFILE */
/* T2_HLE_PROFILE=1: where does the 68k time go? The CPU exec hook attributes the master clocks
 * elapsed since the previous instruction to that instruction's function (data/functions.txt);
 * the cycles an HLE call charges are counted as "C". Wait loops (WaitVBlank, FramePaceWait's
 * loop) are "idle". 68k run nested inside a C call (hle_call68k...) is part of that call's
 * charge: it is counted in C and listed apart as "nested". Summary at exit; the "PROF" lines
 * are read by port/tools/profmerge.py. Off by default: no hook is installed, no cost. */
extern void emu_add_hook(void (*fn)(int type, int width, unsigned int addr, unsigned int value));
typedef struct { uint32_t lo, hi; int fn; } prof_rng;
typedef struct { uint32_t addr; char name[48]; long long mclk, nmclk; unsigned long calls, decl; } prof_fn;
static prof_rng *g_prng;
static int g_nprng;
static prof_fn *g_pfn;
static int g_npfn, g_fpw = -1;
static long long g_p_real, g_p_nested, g_p_idle, g_p_c, g_p_other;
static long long g_p_unm[0x1001];   /* real time at PCs outside data/functions.txt, per 256 bytes */
static int g_p_last_pc = -1, g_p_last_clk, g_p_last_nest;

static int prof_cmp(const void *a, const void *b)
{
	uint32_t x = ((const prof_rng *)a)->lo, y = ((const prof_rng *)b)->lo;
	return x < y ? -1 : x > y;
}

static void prof_add_rng(uint32_t lo, uint32_t sz, int fi)
{
	g_prng = realloc(g_prng, (g_nprng + 1) * sizeof *g_prng);
	g_prng[g_nprng++] = (prof_rng){ lo, lo + sz, fi };
}

static void prof_load(void)
{
	FILE *f = fopen("data/functions.txt", "r");
	if (!f) {
		fprintf(stderr, "hle profile: data/functions.txt not found (run from the repo root); no names\n");
		return;
	}
	char line[512];
	int cap = 0;
	while (fgets(line, sizeof line, f)) {
		unsigned a, sz;
		char name[64];
		if (line[0] == '#' || sscanf(line, "%x %u %63s", &a, &sz, name) != 3)
			continue;
		if (g_npfn == cap) {
			cap = cap ? cap * 2 : 512;
			g_pfn = realloc(g_pfn, cap * sizeof *g_pfn);
		}
		prof_fn *pf = &g_pfn[g_npfn];
		memset(pf, 0, sizeof *pf);
		pf->addr = a;
		snprintf(pf->name, sizeof pf->name, "%s", name);
		int fi = g_npfn++;
		prof_add_rng(a, sz, fi);
		for (char *q = strchr(line, '#'); q && (q = strchr(q, '+')); q++) {
			unsigned a2, s2;
			if (sscanf(q + 1, "%x:%x", &a2, &s2) == 2)
				prof_add_rng(a2, s2, fi);
		}
	}
	fclose(f);
	/* FramePaceWait's loop sits inside GameStart's range in functions.txt: its own entry */
	g_pfn = realloc(g_pfn, (g_npfn + 1) * sizeof *g_pfn);
	memset(&g_pfn[g_npfn], 0, sizeof *g_pfn);
	g_pfn[g_npfn].addr = 0x0078A6;
	snprintf(g_pfn[g_npfn].name, sizeof g_pfn->name, "FramePaceWait");
	g_fpw = g_npfn++;
	qsort(g_prng, g_nprng, sizeof *g_prng, prof_cmp);
	/* an HLE entry that is a piece of another function (a bra.w target, say) gets its own
	 * bucket, so its declined calls (run as 68k) show under the replaced routine's name */
	for (int i = 0; i < g_n; i++) {
		uint32_t a = g_ent[i].def.addr;
		int j = -1, ex = -1;
		for (int k = 0; k < g_nprng; k++) {
			if (g_prng[k].lo <= a && a < g_prng[k].hi)
				j = k;
			if (g_prng[k].lo == a)
				ex = k;
		}
		if ((ex >= 0 && g_pfn[g_prng[ex].fn].addr == a) || (ex < 0 && j < 0))
			continue;
		g_pfn = realloc(g_pfn, (g_npfn + 1) * sizeof *g_pfn);
		memset(&g_pfn[g_npfn], 0, sizeof *g_pfn);
		g_pfn[g_npfn].addr = a;
		snprintf(g_pfn[g_npfn].name, sizeof g_pfn->name, "%s", g_ent[i].def.name);
		if (ex >= 0) {
			g_prng[ex].fn = g_npfn++;
		} else {
			uint32_t hi = g_prng[j].hi;
			g_prng[j].hi = a;
			prof_add_rng(a, hi - a, g_npfn++);
			qsort(g_prng, g_nprng, sizeof *g_prng, prof_cmp);
		}
	}
}

static int prof_find(uint32_t pc)
{
	int lo = 0, hi = g_nprng - 1, r = -1;
	while (lo <= hi) {
		int m = (lo + hi) / 2;
		if (g_prng[m].lo <= pc) {
			r = m;
			lo = m + 1;
		} else
			hi = m - 1;
	}
	return r >= 0 && pc < g_prng[r].hi ? g_prng[r].fn : -1;
}

/* WaitVBlank, the compare/branch loop at the top of FramePaceWait, the $01DC4E VDP wait */
static bool prof_idle(uint32_t pc)
{
	return (pc >= 0x0086E2 && pc < 0x0086E2 + 20) || (pc >= 0x0078A6 && pc < 0x0078AE) ||
	       (pc >= 0x01DC4E && pc < 0x01DC68);   /* spin on the VDP status / HV counter */
}

static void prof_hook(int type, int width, unsigned addr, unsigned value)
{
	(void)width;
	(void)value;
	if (type != 1)
		return;
	int now = gx_m68k_cycles();
	if (g_p_last_pc >= 0 && now >= g_p_last_clk && now - g_p_last_clk < 2000000) {
		long long d = now - g_p_last_clk;
		int fi = prof_find((uint32_t)g_p_last_pc);
		if (g_fpw >= 0 && g_p_last_pc >= 0x0078A6 && g_p_last_pc < 0x0078AE)
			fi = g_fpw;
		if (g_p_last_nest) {
			g_p_nested += d;
			if (fi >= 0)
				g_pfn[fi].nmclk += d;
		} else {
			if (prof_idle((uint32_t)g_p_last_pc))
				g_p_idle += d;
			else
				g_p_real += d;
			if (fi >= 0)
				g_pfn[fi].mclk += d;
			else {
				g_p_other += d;
				g_p_unm[(uint32_t)g_p_last_pc < 0x100000 ? (uint32_t)g_p_last_pc >> 8 : 0x1000] += d;
			}
		}
	}
	g_p_last_pc = (int)addr;
	g_p_last_clk = now;
	g_p_last_nest = g_nest > 0 || g_top;
	if (!g_p_last_nest) {
		int fi = prof_find(addr);
		if (fi >= 0 && g_pfn[fi].addr == addr)
			g_pfn[fi].calls++;
	}
}

static int prof_rank(const void *a, const void *b)
{
	long long x = ((const prof_fn *)a)->mclk, y = ((const prof_fn *)b)->mclk;
	return x < y ? 1 : x > y ? -1 : 0;
}

static void prof_print(void)
{
	long long tot = g_p_real + g_p_idle + g_p_c;
	double t = tot ? (double)tot : 1, busy = (double)(g_p_c + g_p_real) + 1;
	fprintf(stderr, "hle profile: %ld frames, %lld 68k cycles: 68k real %lld, idle %lld, C %lld (incl. nested 68k %lld)\n",
	        g_frames_run, tot / 7, g_p_real / 7, g_p_idle / 7, g_p_c / 7, g_p_nested / 7);
	fprintf(stderr, "hle profile: C %.2f%%  68k real %.2f%%  idle %.2f%%   (busy only: C %.2f%%  68k %.2f%%)\n",
	        100 * g_p_c / t, 100 * g_p_real / t, 100 * g_p_idle / t, 100 * g_p_c / busy, 100 * g_p_real / busy);
	fprintf(stderr, "PROF tot %ld %lld %lld %lld %lld %lld\n", g_frames_run, g_p_real, g_p_idle, g_p_c, g_p_nested, g_p_other);
	for (int i = 0; i < g_npfn; i++)
		if (g_pfn[i].mclk || g_pfn[i].nmclk)
			{
			entry *he = find_addr(g_pfn[i].addr);   /* 1: replaced by C, so this is the declined-call (ran as 68k) time */
			fprintf(stderr, "PROF fn %06X %s %lld %lld %lu %d %lu\n", g_pfn[i].addr, g_pfn[i].name, g_pfn[i].mclk, g_pfn[i].nmclk,
			        g_pfn[i].calls, he && he->enabled, g_pfn[i].decl);
		}
	for (int n = 0; n < 6; n++) {
		int b = 0;
		for (int i = 1; i <= 0x1000; i++)
			if (g_p_unm[i] > g_p_unm[b])
				b = i;
		if (!g_p_unm[b])
			break;
		fprintf(stderr, "PROF unm %s%05X %lld\n", b == 0x1000 ? ">=" : "", b == 0x1000 ? 0x100000 : b << 8, g_p_unm[b]);
		g_p_unm[b] = 0;
	}
	qsort(g_pfn, g_npfn, sizeof *g_pfn, prof_rank);
	for (int i = 0; i < 40 && i < g_npfn && g_pfn[i].mclk; i++)
		fprintf(stderr, "  %06X %-26s %12lld %6.2f%% %9lu calls\n", g_pfn[i].addr, g_pfn[i].name,
		        g_pfn[i].mclk / 7, 100 * g_pfn[i].mclk / t, g_pfn[i].calls);
}

static void prof_install(void)
{
	prof_load();
	emu_add_hook(prof_hook);
	atexit(prof_print);
}

static bool g_prof;        /* T2_HLE_PROFILE */

static int dispatch(unsigned int pc)
{
	if (pc == YIELD_PC) {
		if (g_trace > 1)
			fprintf(stderr, "resume clock %d\n", gx_m68k_cycles());
		int r = resume_cor();
		if (g_prof) {
			g_p_c += (long long)r * 7;
			g_p_last_pc = -1;
		}
		return r;
	}
	if (pc == STALL_PC) {
		int n = stall();
		if (g_prof) {
			g_p_c += (long long)n * 7;   /* the rest of a long C function's charge */
			g_p_last_pc = -1;
		}
		return n;
	}
	if (g_lh_on && lh_vec(pc))
		lh_emit(gx_m68k_cycles());
	entry *e = find_addr(pc);
	if (!e || !e->enabled)
		return -1;
	if (g_trace < 0)
		g_trace = getenv("T2_HLE_TRACE") ? atoi(getenv("T2_HLE_TRACE")) : 0;
	if (g_prof) {
		int r = run_def(&e->def, e);
		if (r >= 0) {
			g_p_c += (long long)r * 7;   /* nested 68k is inside r */
			g_p_last_pc = -1;
		} else {
			int fi = prof_find(e->def.addr);   /* declined: the original runs as 68k */
			if (fi >= 0)
				g_pfn[fi].decl++;
		}
		return r;
	}
	if (!g_trace)
		return run_def(&e->def, e);
	int c0 = gx_m68k_cycles();
	uint32_t d0 = gx_m68k_dar()[0], d1 = gx_m68k_dar()[1], sp = gx_m68k_dar()[15];
	if (g_trace > 1)
		fprintf(stderr, "enter %s clock %d nest %d top %d bnd %d end %d cor %d frame %ld addr %06X\n", e->def.name, c0, g_nest, (int)g_top, g_bnd, gx_m68k_cycle_end(), g_cor != NULL, g_frames_run, e->def.addr);
	int r = run_def(&e->def, e);
	fprintf(stderr, "hle %-18s D0=%08X D1=%08X SP=%06X -> ret %06X  mclk %d -> %d (+%d charged)\n",
	        e->def.name, d0, d1, sp, gx_m68k_pc(), c0, gx_m68k_cycles(), r * 7);
	return r;
}

int hle_invoke(const hle_def *def)
{
	return run_def(def, find_addr(def->addr));
}

static void declines_print(void)
{
	fprintf(stderr, "hle: %lu calls ran as 68k (interrupt inside)\n", g_declined);
	fprintf(stderr, "declines: function  frameend irqwin pendirq | dma vint vclr hint gun other\n");
	for (int i = 0; i < g_dn; i++) {
		unsigned long *n = g_dstat[i].n;
		fprintf(stderr, "declines: %-20s %lu %lu %lu | %lu %lu %lu %lu %lu %lu\n", g_dstat[i].name,
		        n[1], n[2], n[3], n[10], n[11], n[12], n[13], n[14], n[15]);
		if (n[9])
			fprintf(stderr, "bails: %-20s %lu\n", g_dstat[i].name, n[9]);
		if (n[8])
			fprintf(stderr, "yields: %-20s %lu\n", g_dstat[i].name, n[8]);
	}
	for (int i = 0; i < g_callc_nfb; i++) {
		entry *e = find_addr((uint32_t)g_callc_fb[i][0]);
		fprintf(stderr, "nested68k: $%06X %-20s %lu\n", (unsigned)g_callc_fb[i][0], e ? e->def.name : "(68k)", g_callc_fb[i][1]);
	}
}

#include <signal.h>
#include <unistd.h>
/* T2_HLE_WATCHDOG=N: after N seconds say which C block was running and stop (a piece that never returns) */
static void watchdog(int sig)
{
	(void)sig;
	fprintf(stderr, "hle watchdog: last entered %s ($%06X); in %s ($%06X), pc $%06X\n", g_last_entered ? g_last_entered->name : "-", g_last_entered ? g_last_entered->addr : 0, g_cur_def ? g_cur_def->name : "(68k)",
	        g_cur_def ? g_cur_def->addr : 0, gx_m68k_pc());
	_exit(3);
}

void hle_install(void)
{
	if (getenv("T2_HLE_WATCHDOG")) {
		signal(SIGALRM, watchdog);
		alarm((unsigned)atoi(getenv("T2_HLE_WATCHDOG")));
	}
	if (getenv("T2_ITRACE") && sscanf(getenv("T2_ITRACE"), "%ld,%d,%d", &g_it_frame, &g_it_lo, &g_it_hi) == 3) {
		g_lh_on = true;
		g_lh_lo = g_lh_hi = 0;
		emu_add_hook(lh_hook);
	}
	if (getenv("T2_LINEHASH") && sscanf(getenv("T2_LINEHASH"), "%x-%x", &g_lh_lo, &g_lh_hi) == 2) {
		g_lh_on = true;
		g_lh_lo &= 0xFFFF; g_lh_hi &= 0x1FFFF;
		emu_add_hook(lh_hook);
	}
	const char *o = getenv("T2_HLE_ORIG");
	if (o)
		snprintf(g_orig, sizeof g_orig, "%s", o);
	hle_rom = gx_rom();
	g_seq_on = getenv("T2_HLE_SEQ") != NULL;
	g_decline_on = getenv("T2_HLE_NODECLINE") == NULL;
	if (getenv("T2_HLE_DECLINES"))
		atexit(declines_print);
	if (getenv("T2_HLE_BOUNDS")) {
		hle_chk_on = true;
		atexit(chk_print);
	}
	if (getenv("T2_HLE_PROFILE")) {
		g_prof = true;
		prof_install();
	}
	m68k_hle_handler = dispatch;
	m68k_hle_irq_delay = irqd_hook;
	m68k_hle_bitmap = g_bitmap;
	bitmap_set(STALL_PC, true);
	bitmap_set(YIELD_PC, true);
	g_cor_on = getenv("T2_HLE_NOCOR") == NULL;
	if (getenv("T2_HLE_NOCOR_FILE"))
		snprintf(g_nocor_file, sizeof g_nocor_file, "%s", getenv("T2_HLE_NOCOR_FILE"));
	if (getenv("T2_HLE_NOCOR_FN"))
		snprintf(g_nocor_fn, sizeof g_nocor_fn, "%s", getenv("T2_HLE_NOCOR_FN"));
	g_installed = true;
}

/* ------------------------------------------------------------------ nested 68k calls */
/* 68k code run nested inside a C function (hle_call68k, hle_run68k_until) executes
 * inside ONE time slice of the emulator's frame loop: the loop can't advance the raster
 * until the C function returns. Two things must not leak out of that (#30):
 *  - Interrupts. m68k_step_one() doesn't poll them, but any instruction that writes SR
 *    (move to SR, andi/ori to SR, rte) checks them, so a pending HBlank would be taken
 *    in the middle of the nested call. The C function takes no emulated time, so the
 *    interrupt belongs after it: the level is cleared after every nested instruction and
 *    recomputed from the VDP's pending flags when the outermost nested call returns.
 *  - The VDP line base. Nested code can run for many lines' worth of CPU cycles. A 68k
 *    VDP data-port write computes its FIFO slot from (CPU cycles - line base) and reads
 *    past GPGX's fifo_timing[] when that exceeds ~2 lines, which stalls the CPU by
 *    millions of cycles: the next ~15 VBlanks were lost (vblank_main skipped, input lag).
 *    So the line base - and the V counter (#33) - follow the CPU during nested execution
 *    and are restored when the C function returns (frame loop bookkeeping).
 *  - DMA (#31): a VDP DMA started by nested code is advanced by the frame loop, line by
 *    line, which can't run until C returns: the nested code's DMA-busy poll spun or the
 *    transfer stalled the CPU clock for seconds, and room_load's plane uploads never
 *    reached VRAM (Lobby drawn without its background). Nested DMA completes at once, as
 *    in the sandbox; the CPU is halted during a 68k->VDP DMA anyway.
 *  - The CPU clock (#31): time keeps running across the nested calls of one C function
 *    (an HV read after a nested call sees the time it took, as in the original: #33) and
 *    is rewound when the C function returns; the nested time is added to its charge
 *    (run_def), so a later Z80 bus request doesn't catch the Z80/YM2612 up by frames. */
static void seq_step(int c0, int r0)
{
	if (g_seq_on <= 0)
		return;
	int d = gx_m68k_cycles() - c0;
	if (gx_m68k_refresh_cycles() != r0)
		d -= 14;                       /* the 2-cycle bus refresh taken before this instruction */
	seq_add(d / 7);
}

static void nest_begin(void)
{
	if (g_nest++ == 0 && !hle_post_access && !g_sess) {   /* the sandbox manages time itself */
		g_sess = true;
		g_sess_c0 = gx_m68k_cycles();
		g_sess_r0 = gx_m68k_refresh_cycles();
		g_sess_line_base = gx_vdp_line_base();
		g_sess_vc = gx_vdp_vcounter();
	}
	gx_m68k_clear_irq();
	if (g_sess)
		gx_vdp_follow_cpu_lines();
}

static void nest_step_done_x(bool flush)
{
	/* a slice end of the original's frame loop falls between this instruction and the next:
	 * run the Z80 up to it (tl_boundary) */
	if (g_top && !hle_post_access)
		while (g_bnd <= gx_m68k_cycles()) {
			gx_z80_catchup(g_bnd);
			g_bnd = gx_next_slice_end(g_bnd);
		}
	gx_m68k_clear_irq();
	if (g_sess)
		gx_vdp_follow_cpu_lines();
	if (flush && gx_dma_length())
		gx_flush_dma();
}
static void nest_step_done(void) { nest_step_done_x(true); }

static void nest_end(void)
{
	if (--g_nest == 0 && !g_top)    /* nested call made outside a C function (tools) */
		sess_end(false);
}

static void no_original(const char *what, uint32_t addr)
{
	fprintf(stderr, "native: %s($%06X): the original 68k code cannot run in the native runner "
	        "(a path no route reaches; docs/native.md)\n", what, addr);
	abort();
}

/* the native runner never declines: nothing is left to hand to a 68k (called by native_init) */
void hle_native_mode(void) { g_decline_on = false; hle_set_group("native", true); }

static void call68k2(hle_ctx *c, uint32_t addr, bool orig, bool push, uint32_t sp_done_in)
{
	if (orig && native_active)
		no_original("hle_call68k_orig", addr);
	/* push the sentinel return address (unless hle_callc already did), run until it is popped */
	hle_ctx save = *c;
	if (push) {
		c->a[7] -= 4;
		bus_w32(c->a[7], SENTINEL_RET);
	}
	uint32_t sp_done = push ? c->a[7] + 4 : sp_done_in;
	c->pc = addr;
	g_cor_block++;
	nest_begin();
	/* A function with an instruction timeline has charged its instructions so far on c->tl, not on the clock:
	 * the nested 68k would run in the past (its bus refresh and the Z80 stalls at slice ends would fall on other
	 * instructions than in the original). Put the time on the clock first; the session end counts it. */
	if (g_top && g_sess && !hle_post_access && !c->dry && c->tl_used && c->tl) {
		tl_flush(c);
		save.tl = 0;
	}
	ctx_store(c);
	if (orig)
		m68k_hle_skip_pc = addr;
	unsigned long n = 0;
	while (!(gx_m68k_pc() == SENTINEL_RET && gx_m68k_dar()[15] == sp_done)) {
		int sc0 = gx_m68k_cycles(), sr0 = gx_m68k_refresh_cycles();
		int r = m68k_step_one();
		seq_step(sc0, sr0);
		nest_step_done();
		if (r < 0) {
			fprintf(stderr, "hle_call68k($%06X): address error at pc $%06X\n", addr, gx_m68k_pc());
			abort();
		}
		if (hle_post_access)
			hle_post_access();
		if (++n > 200000000ul) {
			fprintf(stderr, "hle_call68k($%06X): no return after %lu instructions\n", addr, n);
			abort();
		}
	}
	m68k_hle_skip_pc = 0xFFFFFFFF;
	nest_end();
	g_cor_block--;
	ctx_load(c);
	c->def = save.def;
	c->pc = save.pc;
	c->jumped = save.jumped;
	c->tl = save.tl;
	c->tl_used = save.tl_used;
	c->tl_last = save.tl_last;
	c->tl_flushed = save.tl_flushed;
}

static void call68k(hle_ctx *c, uint32_t addr, bool orig) { call68k2(c, addr, orig, true, 0); }
void hle_call68k(hle_ctx *c, uint32_t addr) { call68k(c, addr, false); }
/* the 68k continues at addr; the sentinel return address is already on the stack, and the call is done when
 * it is popped (SP = sp_done), whatever has been pushed above it meanwhile (hle_callc) */
void hle_call68k_nopush(hle_ctx *c, uint32_t addr, uint32_t sp_done) { call68k2(c, addr, false, false, sp_done); }

/* VDP DMA started by C: behave like a nested 68k instruction (see hle.h) */
void hle_dma_begin(void) { nest_begin(); }
void hle_dma_complete(void) { nest_step_done(); }
void hle_dma_end(void)
{
	nest_step_done();
	nest_end();
}
/* end of a block that opened its session with hle_dma_begin() for exact VDP access (generated boot code,
 * game/boot.c): a DMA still running (a VRAM fill, the 68k is not halted) goes on in the frame loop as in the original */
void hle_vdp_end(void)
{
	nest_step_done_x(false);
	nest_end();
}
void hle_set_usp(uint32_t usp) { gx_m68k_set_usp(usp); }

/* The original spins here until an interrupt changes something (wait primitives):
 * charge the rest of the CPU's current time slice, so the frame loop moves on and the interrupt
 * is taken at the raster line where the original's spin would have taken it. */
void hle_idle(hle_ctx *c)
{
	if (g_nest > 0 || hle_post_access) {
		fprintf(stderr, "hle: %s ($%06X) waits for an interrupt inside a nested call / the sandbox\n",
		        c->def ? c->def->name : "?", c->def ? c->def->addr : 0);
		abort();
	}
	int left = native_active ? native_slice_left() : gx_m68k_slice_left();
	c->cycles = left > 4 ? left : 4;
}

void hle_run68k_until(hle_ctx *c, const uint32_t *exits)
{
	if (native_active)
		no_original("hle_run68k_until", c->pc);
	hle_ctx save = *c;
	uint32_t sp = c->a[7], start = c->pc;
	g_cor_block++;
	nest_begin();
	ctx_store(c);
	m68k_hle_skip_pc = start;
	for (unsigned long n = 0;; n++) {
		if (n && hle_exit_match(exits, gx_m68k_pc(), gx_m68k_dar()[15], sp))
			break;
		int sc0 = gx_m68k_cycles(), sr0 = gx_m68k_refresh_cycles();
		int r = m68k_step_one();
		seq_step(sc0, sr0);
		nest_step_done();
		if (r < 0) {
			fprintf(stderr, "hle_run68k_until($%06X): address error at pc $%06X\n", start, gx_m68k_pc());
			abort();
		}
		if (hle_post_access)
			hle_post_access();
		if (n > 200000000ul) {
			fprintf(stderr, "hle_run68k_until($%06X): no exit after %lu instructions\n", start, n);
			abort();
		}
	}
	m68k_hle_skip_pc = 0xFFFFFFFF;
	nest_end();
	g_cor_block--;
	ctx_load(c);
	c->def = save.def;
	c->cycles = save.cycles;
	c->tl = save.tl;
	c->tl_used = save.tl_used;
	c->tl_last = save.tl_last;
	c->tl_flushed = save.tl_flushed;
	c->jumped = 1;
}
void hle_call68k_orig(hle_ctx *c, uint32_t addr) { call68k(c, addr, true); }

/* ------------------------------------------------------------------ bus */
#define CHECK_ALIGN(a) do { if ((a) & 1) { fprintf(stderr, "hle: odd word access $%06X\n", (unsigned)(a)); abort(); } } while (0)

uint8_t bus_r8(uint32_t a) { return (uint8_t)m68k_bus_read8(a); }
uint16_t bus_r16(uint32_t a) { CHECK_ALIGN(a); return (uint16_t)m68k_bus_read16(a); }
uint32_t bus_r32(uint32_t a) { CHECK_ALIGN(a); return m68k_bus_read32(a); }

/* VDP data port ($C00000-$C00003): see gx_vdp_untimed_begin() */
void hle_vdp_timed(int on)
{
	g_vdp_timed = on;
	if (on && g_sess)
		g_sess_timed = true;
}
void hle_exact_end(void) { nest_end(); }
void hle_exact_session(void)
{
	if (g_sess) {
		g_sess_timed = true;
		gx_follow_z80(1);
	}
}

static bool is_vdp_data(uint32_t a)
{
	return !g_vdp_timed && (a & 0xFFFFFC) == 0xC00000;
}

void bus_w8(uint32_t a, uint8_t v)
{
	bool vd = is_vdp_data(a);
	if (vd)
		gx_vdp_untimed_begin();
	m68k_bus_write8(a, v);
	if (vd)
		gx_vdp_untimed_end();
	if (hle_post_access)
		hle_post_access();
}

void bus_w16(uint32_t a, uint16_t v)
{
	CHECK_ALIGN(a);
	bool vd = is_vdp_data(a);
	if (vd)
		gx_vdp_untimed_begin();
	m68k_bus_write16(a, v);
	if (vd)
		gx_vdp_untimed_end();
	if (hle_post_access)
		hle_post_access();
}

void bus_w32(uint32_t a, uint32_t v)
{
	CHECK_ALIGN(a);
	bool vd = is_vdp_data(a);
	if (vd)
		gx_vdp_untimed_begin();
	m68k_bus_write32(a, v);
	if (vd)
		gx_vdp_untimed_end();
	if (hle_post_access)
		hle_post_access();
}

long emu_frame_count(void);
void native_fallback_report(unsigned pc, unsigned long n, unsigned long e) { fprintf(stderr, "native-fallback: $%06X %lu %lu\n", pc & 0xFFFFFF, n, e); }
/* fatal errors of the native runner (native.c cannot use stdio: GPGX's headers remap it) */
void native_fatal(const char *what, unsigned pc, unsigned sr, unsigned sp, unsigned ret)
{
	fprintf(stderr, "native: %s at $%06X after %ld frames (SR $%04X, SP $%06X, long at SP $%06X).\n"
	        "        Every 68k address the game reaches must be a registered C block (HLE function or\n"
	        "        range); nothing is interpreted in the native runner (docs/native.md).\n",
	        what, pc & 0xFFFFFF, emu_frame_count(), sr, sp, ret);
	abort();
}
