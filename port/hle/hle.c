/* HLE registry and CPU glue. The patched core (tools/patches/gpgx-hle.patch) calls
 * m68k_hle_handler(pc) before fetching an instruction whose address has its bit set in
 * m68k_hle_bitmap. */
#include "hle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gx.h"

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

#define MAX_DEFS 4096
#define SENTINEL_RET 0x0000F0u   /* return address used for hle_call68k (never executed) */
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
		fprintf(stderr, "hle: $%06X registered twice (%s, %s)\n", def->addr, e->def.name, def->name);
		abort();
	}
	if (g_n == MAX_DEFS)
		abort();
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
void hle_frame_begin(void) { g_frame_calls = 0; }

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
}

/* ---- instruction timeline (hle.h) */
static int g_nest;             /* (defined below) depth of nested 68k execution */
static bool g_top;             /* (defined below) a top-level C function is running */
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
		gx_z80_catchup(g_bnd);
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
static void tl_start(hle_ctx *c)
{
	c->tl_used = 1;
	tl_boundary(c);
	tl_refresh(c);
}
void hle_insn(hle_ctx *c, int n)
{
	tl_start(c);
	c->tl += (long)n * 7;
	c->tl_last = (long)n * 7;
	if (!c->dry)
		seq_add(n);
}
void hle_insn_x(hle_ctx *c, int n, int extra)
{
	tl_start(c);
	if (!hle_post_access && gx_m68k_cycles() + (int)c->tl + extra * 7 >= gx_m68k_refresh_cycles())
		gx_m68k_set_refresh_cycles(gx_m68k_refresh_cycles() + 128 * 7);   /* SKIP_BUS_REFRESH */
	c->tl += (long)n * 7;
	c->tl_last = (long)n * 7;
	if (!c->dry)
		seq_add(n);
}
static int g_pre_clock;
void hle_sync(hle_ctx *c)
{
	c->tl_used = 1;
	tl_boundary(c);
	tl_flush(c);
	g_pre_clock = gx_m68k_cycles();
}
void hle_pre(hle_ctx *c)
{
	c->tl_used = 1;
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
void hle_decline(void) { if (!hle_post_access) g_decline = true; }
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

int hle_nest_depth(void) { return g_nest; }
const hle_def *hle_current_def(void) { return g_cur_def; }
/* would the CPU hand this pc to a C function instead of executing the instruction? (profilers) */
bool hle_pc_replaced(uint32_t pc)
{
	if (pc == STALL_PC)
		return true;
	if (pc >= 0x400000 || !(g_bitmap[pc >> 4] & (1u << ((pc >> 1) & 7))) || pc == m68k_hle_skip_pc)
		return false;
	entry *e = find_addr(pc);
	return e && e->enabled;
}

static int run_def(const hle_def *def, entry *e)
{
	hle_ctx c;
	const hle_def *outer_def = g_cur_def;
	bool top = g_nest == 0 && !g_top;
	/* Interrupt atomicity (docs/porting.md, "Interrupts inside a replaced function"): when the
	 * original would have been interrupted INSIDE this call, it must run as 68k. A pure function
	 * (HLE_F_PURE: work RAM only) is run once dry to learn its real length; the others are judged
	 * by def->cycles, the longest window they take. */
	int irq_mask = (m68k_hle_get_sr() >> 8) & 7;
	int clock0 = gx_m68k_cycles();
	bool lowmask = (def->flags & HLE_F_LOWMASK) != 0;
	if (top)
		g_bnd = gx_m68k_cycle_end();
	if (g_decline_on && top && !hle_post_access && !def_is_orig(def) && !g_stall_debt) {
		long window = -1, span = (long)def->cycles * 7;   /* span: how long the call can take */
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
			/* the dry run has no Z80 stall at slice ends (up to ~80 mcycles each): margin */
			long margin = 300 + (d.tl / 3420 + 1) * 100;   /* one Z80 stall per slice end crossed */
			span = d.tl + margin;
			if (irq_mask < 6)
				window = d.tl - d.tl_last + margin;
		} else if (irq_mask < 6 || lowmask) {
			window = span;
			if (lowmask) {
				irq_mask = 0;
				if (gx_m68k_pending_irq()) {   /* taken as soon as the function's ReadPad drops the mask */
					g_declined++;
					return -1;
				}
			}
		}
		/* The frame loop rebases the clocks at the end of the frame and starts the next one
		 * (VDP frame work, the VINT slice...): a call that straddles it cannot be a C lump. */
		if (clock0 + span > gx_frame_end_clock()) {
			g_declined++;
			return -1;
		}
		if (window >= 0) {
			int at, lvl = gx_irq_boundary_in(clock0, clock0 + (int)window, &at);
			if (lvl && irq_mask < lvl) {
				if (getenv("T2_HLE_DECLINES"))
					fprintf(stderr, "decline %s: clock %d window %ld lvl %d at %d mask %d\n", def->name, clock0, window, lvl, at, irq_mask);
				g_declined++;
				return -1;
			}
		}
	}
	if (top)
		g_top = true;
	ctx_load(&c);
	uint32_t sp0 = c.a[7];
	bool unmasked = ((c.sr >> 8) & 7) < 6;
	c.def = def;
	c.cycles = def->cycles;
	if (e)
		e->calls++;
	g_frame_calls++;
	g_cur_def = def;
	if (def_is_orig(def)) {
		/* passthrough: run the original 68k and log its real cost vs the C charge */
		long c0 = gx_m68k_cycles();
		if (def->exits)
			hle_run68k_until(&c, def->exits);
		else
			hle_call68k_orig(&c, c.pc);
		long d = (gx_m68k_cycles() - c0) / 7;
		fprintf(stderr, "orig %-18s real %ld cyc  (C charge %d, delta %ld)\n",
		        def->name, d, def->cycles, d - def->cycles);
		c.cycles = (int)(d > 0 ? d : 0);
	} else {
		def->fn(&c);
		if (c.tl_used)
			c.cycles = (int)(c.tl / 7);
	}
	g_cur_def = outer_def;
	if (g_decline) {
		g_decline = false;
		if (top)
			g_top = false;
		if (e)
			e->calls--;
		g_frame_calls--;
		g_seq_n = 0;
		return -1;
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
		if (!c.jumped || !hle_exit_match(def->exits, c.pc, c.a[7], sp0)) {
			fprintf(stderr, "hle: range %s ($%06X) left via $%06X with SP %+d%s, not a declared exit\n",
			        def->name, def->addr, c.pc, (int)(c.a[7] - sp0), c.jumped ? "" : " (no hle_jump)");
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
	if (!def_is_orig(def) && total > 0 && !hle_post_access && !c.tl_used && !(top && g_exact)) {
		int delta = (int)(total * 7);
		int R = gx_m68k_refresh_cycles(), C = gx_m68k_cycles();
		int gap = R - C;
		int n = delta >= gap ? 1 + (delta - gap) / 882 : 0;
		if (n > 0) {
			total += 2 * n;
			gx_m68k_set_refresh_cycles(R + 896 * n);
		}
	}
	if (getenv("T2_HLE_CHARGE"))
		fprintf(stderr, "charge %-18s %ld (ctx %d + nested %ld)\n", def->name, total, c.cycles, nested);
	if (unmasked && total > STALL_SLICE && !hle_post_access && g_nest == 0 && !g_stall_debt) {
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
	return (int)total;
}

void hle_state_reset(void) { g_cur_def = NULL; g_stall_debt = 0; g_sess = false; g_sess_timed = false; g_vdp_timed = false; gx_follow_z80(0); g_top = false; g_nest = 0; }

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

static int g_trace = -1;   /* T2_HLE_TRACE=1: log every dispatched call to stderr */

static unsigned g_dbgpc;
static int dispatch(unsigned int pc)
{
	if (pc == STALL_PC)
		return stall();
	entry *e = find_addr(pc);
	if (g_dbgpc && pc == g_dbgpc) {
		int clk = gx_m68k_cycles();
		if (clk >= gx_m68k_refresh_cycles())
			clk += 14;
		fprintf(stderr, "PCLOG clk=%d R=%d en=%d\n", clk, gx_m68k_refresh_cycles(), e && e->enabled);
	}
	if (!e || !e->enabled)
		return -1;
	if (g_trace < 0)
		g_trace = getenv("T2_HLE_TRACE") ? atoi(getenv("T2_HLE_TRACE")) : 0;
	if (!g_trace)
		return run_def(&e->def, e);
	int c0 = gx_m68k_cycles();
	uint32_t d0 = gx_m68k_dar()[0], d1 = gx_m68k_dar()[1], sp = gx_m68k_dar()[15];
	int r = run_def(&e->def, e);
	fprintf(stderr, "hle %-18s D0=%08X D1=%08X SP=%06X -> ret %06X  mclk %d -> %d (+%d charged)\n",
	        e->def.name, d0, d1, sp, gx_m68k_pc(), c0, gx_m68k_cycles(), r * 7);
	return r;
}

int hle_invoke(const hle_def *def)
{
	return run_def(def, find_addr(def->addr));
}

static void declines_print(void) { fprintf(stderr, "hle: %lu calls ran as 68k (interrupt inside)\n", g_declined); }

void hle_install(void)
{
	const char *o = getenv("T2_HLE_ORIG");
	if (o)
		snprintf(g_orig, sizeof g_orig, "%s", o);
	hle_rom = gx_rom();
	g_seq_on = getenv("T2_HLE_SEQ") != NULL;
	g_decline_on = getenv("T2_HLE_NODECLINE") == NULL;
	if (getenv("T2_HLE_DBGPC")) {
		g_dbgpc = (unsigned)strtoul(getenv("T2_HLE_DBGPC"), NULL, 16);
		bitmap_set(g_dbgpc, true);
	}
	if (getenv("T2_HLE_DECLINES"))
		atexit(declines_print);
	m68k_hle_handler = dispatch;
	m68k_hle_bitmap = g_bitmap;
	bitmap_set(STALL_PC, true);
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

static void nest_step_done(void)
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
	if (gx_dma_length())
		gx_flush_dma();
}

static void nest_end(void)
{
	if (--g_nest == 0 && !g_top)    /* nested call made outside a C function (tools) */
		sess_end(false);
}

static void call68k(hle_ctx *c, uint32_t addr, bool orig)
{
	/* push the sentinel return address, run until it is popped */
	hle_ctx save = *c;
	c->a[7] -= 4;
	bus_w32(c->a[7], SENTINEL_RET);
	uint32_t sp_done = c->a[7] + 4;
	c->pc = addr;
	nest_begin();
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
	ctx_load(c);
	c->def = save.def;
	c->pc = save.pc;
	c->jumped = save.jumped;
	c->tl = save.tl;
	c->tl_used = save.tl_used;
	c->tl_last = save.tl_last;
	c->tl_flushed = save.tl_flushed;
}

void hle_call68k(hle_ctx *c, uint32_t addr) { call68k(c, addr, false); }

/* VDP DMA started by C: behave like a nested 68k instruction (see hle.h) */
void hle_dma_begin(void) { nest_begin(); }
void hle_dma_complete(void) { nest_step_done(); }
void hle_dma_end(void)
{
	nest_step_done();
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
	int left = gx_m68k_slice_left();
	c->cycles = left > 4 ? left : 4;
}

void hle_run68k_until(hle_ctx *c, const uint32_t *exits)
{
	hle_ctx save = *c;
	uint32_t sp = c->a[7], start = c->pc;
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
