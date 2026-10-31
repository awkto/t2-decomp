/* Headless libretro harness for reverse engineering.
 *
 *   harness <core.so> <rom> <script> <outdir>
 *
 * Script lines (blank / '#' ignored):
 *   run N                 advance N frames with current input
 *   press BTN... N        hold buttons for N frames, then release   (BTN: U D L R A B C S)
 *   shot NAME             write outdir/NAME.ppm of the current frame
 *   ram NAME              write outdir/NAME.ram (64 KiB 68k work RAM)
 *   save NAME / load NAME savestates
 *   record NAME / stoprecord   log the pad state of every frame to outdir/NAME.inp
 *                         (run-length text: "<hexmask> <frames>" lines, hexmask = retro joypad bits)
 *   replay PATH           replay such a log (PATH relative to outdir unless absolute or starting
 *                         with ./); deterministic: same state + same log => same result
 *   peek LABEL ADDR [W]   append "frame LABEL value" to outdir/peek.log; ADDR hex 68k RAM address
 *                         ($FF0000-$FFFFFF), W = 1/2/4 bytes (default 2)
 *   roomsave PREFIX       watch $FF06AA/$FF06AC every frame: log room changes to peek.log, and 90
 *                         frames after the first sighting of each (mystery,room) write
 *                         outdir/PREFIX_<myst>_<room>.state   (roomsave off  to stop)
 *   poke ADDR W VAL       write W (1/2/4) bytes, big-endian value VAL (hex), to 68k work RAM at ADDR (cheat; used to
 *                         enable debug menus, e.g. `poke FF09ED 1 03`)
 *   echo TEXT             append "frame TEXT" to outdir/peek.log
 *   vdp NAME              write outdir/NAME.{vram,cram,vsram,reg}: VRAM (64 KiB, big-endian
 *                         words as on hardware), CRAM (64 words, hardware 0000BBB0GGG0RRR0),
 *                         VSRAM (40 words), VDP registers (32 bytes)   [needs vdptrace core]
 *   trace NAME / untrace  start/stop writing outdir/NAME.trace, one text line per event:
 *                           W <frame> <V|C|S> <vdpaddr> <data> D <src>          (DMA from 68k bus)
 *                           W <frame> <V|C|S> <vdpaddr> <data> P <pc> <lastrom> (CPU port write)
 *                           R <frame> <pc> <romaddr> <width>                    (68k ROM data read)
 *                         <lastrom> = most recent ROM data read address (or -1).
 *                         VDP events need a core built with tools/patches/gpgx-vdptrace.patch.
 *
 *   gun X Y BTN N         Menacer on port 2 (only with env GUN=1): aim at game pixel X,Y, hold BTN for N
 *                         frames (BTN bits: 1 trigger, 2 Menacer A, 4 Menacer B, 8 Start), then release.
 *                         Bit 2 = libretro LIGHTGUN_AUX_A = GPGX INPUT_B = Menacer data bit D0,
 *                         bit 4 = LIGHTGUN_AUX_B = INPUT_C = D2, 8 = LIGHTGUN_START = D3, 1 = trigger D1.
 *                         By default (no core option) GPGX reads the gun as RETRO_DEVICE_POINTER
 *                         ("touchscreen" gun mode), where ONLY the trigger is passed through (the
 *                         non-bitmask input path ignores POINTER_COUNT). Run with env GUNMODE=lightgun
 *                         to get A/B/Start too. Both POINTER and LIGHTGUN queries are answered.
 *
 * Env: GUN=1 plugs the Menacer into port 2 (GPGX also forces it for T2 by default); PAD2=1 plugs a
 *   3-button pad into port 2 instead (driven by `press2` and the bot's pad2 device);
 *   GUNMODE=lightgun|touchscreen sets the core option genesis_plus_gx_gun_input;
 *   CORE_OPTS="key=value,key=value" answers other core options.
 *   press2 BTN... N       like press, for the pad on port 2 (PAD2=1)
 *
 *   Gun-bot (tools/harness/bot.c; T2-specific, reads the object table every frame):
 *   bot N                 play N frames with the bot
 *   bot until LEVEL MAX   play until the level variable $FF80CC equals LEVEL (hex), at most MAX frames
 *   bot while LEVEL MAX   play while it equals LEVEL
 *   botcfg KEY VALUE      p1|p2 gun|pad1|pad2|off  device the bot drives for each player (default
 *                           p1 gun, p2 off; mode 0 = p1 gun + p2 pad1, mode 1 = pad1 + pad2)
 *                         missiles 0|1, missile_hp N (default 40), missile_crowd N (6), missile_keep N (5)
 *                         continue 0|1 (press Start to continue, default 1), hiscore 0|1 (enter initials)
 *                         focus N   alternate threat-first / big-target-first every N frames (300; 0 = off)
 *                         savelevels PREFIX   save outdir/PREFIX<level>.state at each level change ("off")
 *                         verbose 0|1|2   bot.log detail (2 = every target choice)
 *                         Cheats (the run is marked CHEATED in bot.log):
 *                         skip N    N frames into each level: set the level-skip flag and pause + B+C
 *                         energy 1  refill energy every frame
 *                         protect 1 keep the escorted truck/van at full hit points
 *                         freeze 1  steel mill: let the T-1000 freeze on the next nitrogen contact
 *                         Logs to outdir/bot.log: level changes, deaths, casualties, status every 1800 frames.
 *   objs NAME             append the live object slots ($FF87B2, raw words) and the $FF9AB2 table to
 *                         outdir/NAME.obj
 *   gunoff 0|1            report the gun as off screen
 *   gunhold X Y BTN       set the Menacer position/buttons and keep them (no frames run); later
 *                         run/press/gun commands see this state ("gun" resets buttons to 0 after it)
 *
 *   Debug helpers (generic, use the cpu hook):
 *   watch ADDR LEN        log every 68k write that touches [ADDR, ADDR+LEN) (any bus address, hex) to
 *                         outdir/watch.log: "<frame> <pc> W<width> <addr> <value>"; up to 16 ranges
 *   unwatch               remove all watch ranges
 *   bp ADDR               log every execution of the instruction at ADDR (hex) to outdir/bp.log:
 *                         "<frame> <addr>"; up to 32 addresses; bpclear removes them.
 *                         Both logs stop after 200000 lines.
 *   peekeach LABEL ADDR [W]   like peek, but checked after every frame and logged when the value
 *                         changes (up to 32); "peekeach off" clears the list
 *   waitfor ADDR W VAL MAX    run frames until the RAM value at ADDR (W bytes) equals VAL (hex), at
 *                         most MAX frames; logs "<frame> waitfor ADDR ok|timeout" to peek.log
 *   sprites NAME          write outdir/NAME.spr: the VDP sprite table walked in link order, one line
 *                         per sprite "idx x y w h tile pal pri hf vf link" with x/y in screen pixels
 *                         (raw - 128)  [needs vdptrace core exports vram/reg]
 *   (roomsave is a scooby-decomp leftover: it watches Scooby's room variables)
 *
 * At exit also writes outdir/ijump.txt: "site target count" for every register-indirect jmp/jsr
 * ((An), d16(An), d8(An,Xn)) executed, with its observed target (merge: tools/py/ijmerge.py).
 * At exit writes outdir/cov.bin: one byte per ROM byte,
 *   bit0 = instruction start executed, bit1 = read as data by the 68k,
 *   bit2 = byte belongs to an executed instruction's opcode word
 */
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libretro.h"

#define ROM_MAX (4 * 1024 * 1024)

static uint8_t cov[ROM_MAX];
static uint8_t ramexec[0x10000];
static const char *outdir;

static const void *fb;
static unsigned fb_w, fb_h;
static size_t fb_pitch;
static enum retro_pixel_format pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;
static uint16_t pad;   /* bitmask of RETRO_DEVICE_ID_JOYPAD_* */
static uint16_t pad2;  /* joypad on port 2 (bot pad2 player, `press2`), used when GUN is not set */
static const uint8_t *romb; /* ROM bytes */
static void bot_hook(int type, unsigned addr);
static long rom_size;

/* runtime targets of register-indirect jmp/jsr: (site, target) pairs, written to outdir/ijump.txt */
#define IJ_MAX 8192
static struct { unsigned site, target; long n; } ij[IJ_MAX];
static int nij;
static unsigned ij_vec[8];
static void ijump_note(unsigned prev, unsigned addr)
{
	if (prev + 2 > (unsigned)rom_size || !romb)
		return;
	unsigned op = romb[prev] << 8 | romb[prev + 1];
	unsigned kind = op & 0xFFC0, mode = (op >> 3) & 7;
	if ((kind != 0x4E80 && kind != 0x4EC0) || (mode != 2 && mode != 5 && mode != 6))
		return;
	for (int i = 0; i < 8; i++)          /* an interrupt taken right after the jump */
		if (addr == ij_vec[i]) return;
	for (int i = 0; i < nij; i++)
		if (ij[i].site == prev && ij[i].target == addr) { ij[i].n++; return; }
	if (nij < IJ_MAX) { ij[nij].site = prev; ij[nij].target = addr; ij[nij].n = 1; nij++; }
}

enum { HOOK_M68K_E = 1, HOOK_M68K_R = 2, HOOK_M68K_W = 4,
       HOOK_VRAM_W = 16, HOOK_CRAM_W = 64, HOOK_VSRAM_W = 256 }; /* from gpgx core/debug/cpuhook.h */

/* optional symbols exported by the vdptrace core patch */
static unsigned int *p_hook_dma_src;
static uint8_t *p_vram, *p_cram, *p_vsram, *p_reg;
static FILE *tracef;
static long frame;
static FILE *recf;
static uint16_t rec_pad;
static long rec_n;
static FILE *peekf;
static void *(*p_retro_get_memory_data)(unsigned);
static void (*p_retro_run)(void);

static void rec_flush(void)
{
	if (recf && rec_n)
		fprintf(recf, "%x %ld\n", rec_pad, rec_n);
	rec_n = 0;
}

static char rs_prefix[256];
static int rs_on;
static unsigned rs_last = 0xFFFFFFFF;
static long rs_due = -1;
static unsigned rs_due_key;
static uint8_t rs_seen[2][256];
static size_t (*p_retro_serialize_size)(void);
static bool (*p_retro_serialize)(void *, size_t);

static unsigned rd_ram16(unsigned a)
{
	const uint8_t *wram = p_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
	return (wram[(a & 0xFFFF) ^ 1] << 8) | wram[((a & 0xFFFF) + 1) ^ 1];
}

static void roomwatch(void)
{
	unsigned key = ((rd_ram16(0xFF06AA) & 1) << 8) | (rd_ram16(0xFF06AC) & 0xFF);
	if (key != rs_last) {
		rs_last = key;
		if (!peekf) { char pp[1024]; snprintf(pp, sizeof pp, "%s/peek.log", outdir); peekf = fopen(pp, "a"); }
		fprintf(peekf, "%ld roomchange m%u r%u\n", frame, key >> 8, key & 0xFF);
		fflush(peekf);
		if (!rs_seen[key >> 8][key & 0xFF]) {
			rs_seen[key >> 8][key & 0xFF] = 1;
			rs_due = frame + 90;
			rs_due_key = key;
		}
	}
	if (rs_due >= 0 && frame >= rs_due) {
		char path[1024];
		snprintf(path, sizeof path, "%s/%s_%u_%u.state", outdir, rs_prefix, rs_due_key >> 8, rs_due_key & 0xFF);
		size_t sz = p_retro_serialize_size();
		void *buf = malloc(sz);
		p_retro_serialize(buf, sz);
		FILE *f = fopen(path, "wb"); fwrite(buf, 1, sz, f); fclose(f);
		free(buf);
		rs_due = -1;
	}
}

static struct { char lab[64]; unsigned a; int w; uint32_t last; int init; } peeks[32];
static int npeek;
static uint32_t rd_ram(unsigned a, int w)
{
	const uint8_t *wram = p_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
	uint32_t v = 0;
	for (int i = 0; i < w; i++)
		v = (v << 8) | wram[((a & 0xFFFF) + i) ^ 1];
	return v;
}
static FILE *peeklog(void)
{
	if (!peekf) { char pp[1024]; snprintf(pp, sizeof pp, "%s/peek.log", outdir); peekf = fopen(pp, "a"); }
	return peekf;
}

static void step(void)
{
	if (recf) {
		if (rec_n && pad != rec_pad)
			rec_flush();
		rec_pad = pad;
		rec_n++;
	}
	p_retro_run();
	frame++;
	for (int i = 0; i < npeek; i++) {
		uint32_t v = rd_ram(peeks[i].a, peeks[i].w);
		if (!peeks[i].init || v != peeks[i].last) {
			fprintf(peeklog(), "%ld %s %0*x\n", frame, peeks[i].lab, peeks[i].w * 2, v);
			peeks[i].last = v; peeks[i].init = 1;
		}
	}
	if (rs_on)
		roomwatch();
}
static unsigned cur_pc;
static long last_rom = -1;

static struct { unsigned a, n; } watches[16];
static int nwatch;
static unsigned bps[32];
static int nbp;
static FILE *watchf, *bpf;
static long watch_lines, bp_lines;

static FILE *open_log(FILE **f, const char *name)
{
	if (!*f) {
		char pp[1024];
		snprintf(pp, sizeof pp, "%s/%s", outdir, name);
		*f = fopen(pp, "a");
	}
	return *f;
}

static void dbg_hook(int type, int width, unsigned addr, unsigned value)
{
	if (type == HOOK_M68K_W) {
		for (int i = 0; i < nwatch; i++)
			if (addr + width > watches[i].a && addr < watches[i].a + watches[i].n) {
				if (watch_lines++ < 200000 && open_log(&watchf, "watch.log"))
					fprintf(watchf, "%ld %06x W%d %06x %0*x\n", frame, cur_pc, width, addr, width * 2,
					        value & (width == 4 ? 0xFFFFFFFFu : (1u << (8 * width)) - 1));
				break;
			}
	} else if (type == HOOK_M68K_E) {
		for (int i = 0; i < nbp; i++)
			if (bps[i] == addr) {
				if (bp_lines++ < 200000 && open_log(&bpf, "bp.log"))
					fprintf(bpf, "%ld %06x\n", frame, addr);
				break;
			}
	}
}

static void hook(int type, int width, unsigned int addr, unsigned int value)
{
	if (type == HOOK_VRAM_W || type == HOOK_CRAM_W || type == HOOK_VSRAM_W) {
		if (!tracef)
			return;
		char k = type == HOOK_VRAM_W ? 'V' : type == HOOK_CRAM_W ? 'C' : 'S';
		unsigned src = p_hook_dma_src ? *p_hook_dma_src : 0xFFFFFFFFu;
		if (src != 0xFFFFFFFFu)
			fprintf(tracef, "W %ld %c %04x %04x D %06x\n", frame, k, addr & 0xFFFF, value & 0xFFFF, src & 0xFFFFFF);
		else
			fprintf(tracef, "W %ld %c %04x %04x P %06x %lx\n", frame, k, addr & 0xFFFF, value & 0xFFFF, cur_pc, last_rom);
		return;
	}
	addr &= 0xFFFFFF;
	if (nwatch || nbp)
		dbg_hook(type, width, addr, value);
	if (type == HOOK_M68K_E) {
		ijump_note(cur_pc, addr);
		cur_pc = addr;
	}
	bot_hook(type, addr);
	if (type == HOOK_M68K_E && addr >= 0xFF0000) {
		ramexec[addr & 0xFFFF] = 1;
		return;
	}
	if (addr >= (unsigned)rom_size)
		return;
	if (type == HOOK_M68K_E) {
		cov[addr] |= 1;
		cov[addr] |= 4;
		if (addr + 1 < (unsigned)rom_size)
			cov[addr + 1] |= 4;
	} else if (type == HOOK_M68K_R) {
		last_rom = addr;
		if (tracef)
			fprintf(tracef, "R %ld %06x %06x %d\n", frame, cur_pc, addr, width);
		for (int i = 0; i < width && addr + i < (unsigned)rom_size; i++)
			cov[addr + i] |= 2;
	}
}

static void log_cb(enum retro_log_level level, const char *fmt, ...)
{
	if (level < RETRO_LOG_WARN)
		return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static bool env_cb(unsigned cmd, void *data)
{
	switch (cmd) {
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		((struct retro_log_callback *)data)->log = log_cb;
		return true;
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		pixfmt = *(enum retro_pixel_format *)data;
		return true;
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		*(const char **)data = outdir;
		return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE: {
		/* core options from env CORE_OPTS="key=value,key=value"; GUNMODE=lightgun|touchscreen
		   is a shortcut for genesis_plus_gx_gun_input */
		struct retro_variable *v = data;
		static char val[16][128];
		static int vi;
		const char *gm = getenv("GUNMODE");
		if (gm && !strcmp(v->key, "genesis_plus_gx_gun_input")) {
			v->value = gm;
			return true;
		}
		const char *o = getenv("CORE_OPTS");
		size_t kl = strlen(v->key);
		while (o && *o) {
			if (!strncmp(o, v->key, kl) && o[kl] == '=') {
				const char *e = strchr(o + kl + 1, ',');
				size_t n = e ? (size_t)(e - (o + kl + 1)) : strlen(o + kl + 1);
				char *b = val[vi++ & 15];
				snprintf(b, 128, "%.*s", (int)n, o + kl + 1);
				v->value = b;
				return true;
			}
			o = strchr(o, ',');
			if (o) o++;
		}
		v->value = NULL;
		return false;
	}
	case RETRO_ENVIRONMENT_GET_CAN_DUPE:
		*(bool *)data = true;
		return true;
	default:
		return false;
	}
}

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
	if (data) {
		fb = data;
		fb_w = w;
		fb_h = h;
		fb_pitch = pitch;
	}
}
static void audio_cb(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t audio_batch_cb(const int16_t *d, size_t n) { (void)d; return n; }
static void poll_cb(void) {}
static int gun_x = 160, gun_y = 112, gun_btn, gun_off;
#define RETRO_DEVICE_MENACER RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_LIGHTGUN, 1)
static int16_t input_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
	(void)index;
	if (port == 1 && device == RETRO_DEVICE_POINTER) {
		switch (id) {
		case RETRO_DEVICE_ID_POINTER_X: return (int)((gun_x + 0.5) * 0xfffe / (fb_w ? fb_w : 320)) - 0x7fff;
		case RETRO_DEVICE_ID_POINTER_Y: return (int)((gun_y + 0.5) * 0xfffe / (fb_h ? fb_h : 224)) - 0x7fff;
		case RETRO_DEVICE_ID_POINTER_PRESSED: return gun_btn & 1;
		case RETRO_DEVICE_ID_POINTER_COUNT: return (gun_btn & 2) ? 2 : (gun_btn & 8) ? 3 : (gun_btn & 4) ? 4 : (gun_btn & 1);
		}
		return 0;
	}
	if (port == 1 && device == RETRO_DEVICE_LIGHTGUN) {
		switch (id) {
		case RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN: return gun_off;
		case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X: return (int)((gun_x + 0.5) * 0xfffe / (fb_w ? fb_w : 320)) - 0x7fff;
		case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y: return (int)((gun_y + 0.5) * 0xfffe / (fb_h ? fb_h : 224)) - 0x7fff;
		case RETRO_DEVICE_ID_LIGHTGUN_TRIGGER: return gun_btn & 1;
		case RETRO_DEVICE_ID_LIGHTGUN_AUX_A: return (gun_btn >> 1) & 1;
		case RETRO_DEVICE_ID_LIGHTGUN_AUX_B: return (gun_btn >> 2) & 1;
		case RETRO_DEVICE_ID_LIGHTGUN_START: return (gun_btn >> 3) & 1;
		}
		return 0;
	}
	if (port > 1 || device != RETRO_DEVICE_JOYPAD)
		return 0;
	uint16_t m = port ? pad2 : pad;
	if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
		return m;
	return (m >> id) & 1;
}

static void write_shot(const char *name)
{
	char path[1024];
	snprintf(path, sizeof path, "%s/%s.ppm", outdir, name);
	FILE *f = fopen(path, "wb");
	if (!f || !fb)
		return;
	fprintf(f, "P6\n%u %u\n255\n", fb_w, fb_h);
	for (unsigned y = 0; y < fb_h; y++) {
		for (unsigned x = 0; x < fb_w; x++) {
			uint8_t rgb[3];
			if (pixfmt == RETRO_PIXEL_FORMAT_XRGB8888) {
				uint32_t p = ((const uint32_t *)((const uint8_t *)fb + y * fb_pitch))[x];
				rgb[0] = p >> 16; rgb[1] = p >> 8; rgb[2] = p;
			} else {
				uint16_t p = ((const uint16_t *)((const uint8_t *)fb + y * fb_pitch))[x];
				if (pixfmt == RETRO_PIXEL_FORMAT_RGB565) {
					rgb[0] = (p >> 11) << 3; rgb[1] = ((p >> 5) & 63) << 2; rgb[2] = (p & 31) << 3;
				} else {
					rgb[0] = ((p >> 10) & 31) << 3; rgb[1] = ((p >> 5) & 31) << 3; rgb[2] = (p & 31) << 3;
				}
			}
			fwrite(rgb, 1, 3, f);
		}
	}
	fclose(f);
}

static int btn_id(char c)
{
	/* GPGX default mapping: retro B=Gen A, retro A=Gen B, retro Y=Gen C? keep explicit */
	switch (c) {
	case 'U': return RETRO_DEVICE_ID_JOYPAD_UP;
	case 'D': return RETRO_DEVICE_ID_JOYPAD_DOWN;
	case 'L': return RETRO_DEVICE_ID_JOYPAD_LEFT;
	case 'R': return RETRO_DEVICE_ID_JOYPAD_RIGHT;
	case 'A': return RETRO_DEVICE_ID_JOYPAD_Y;
	case 'B': return RETRO_DEVICE_ID_JOYPAD_B;
	case 'C': return RETRO_DEVICE_ID_JOYPAD_A;
	case 'S': return RETRO_DEVICE_ID_JOYPAD_START;
	}
	return -1;
}

#include "bot.c"

#define SYM(name) \
	do { \
		*(void **)&p_##name = dlsym(core, #name); \
		if (!p_##name) { fprintf(stderr, "missing %s\n", #name); return 1; } \
	} while (0)

int main(int argc, char **argv)
{
	if (argc != 5) {
		fprintf(stderr, "usage: %s core.so rom script outdir\n", argv[0]);
		return 2;
	}
	outdir = argv[4];
	void *core = dlopen(argv[1], RTLD_NOW);
	if (!core) {
		fprintf(stderr, "%s\n", dlerror());
		return 1;
	}
	void (*p_retro_set_environment)(retro_environment_t);
	void (*p_retro_set_video_refresh)(retro_video_refresh_t);
	void (*p_retro_set_audio_sample)(retro_audio_sample_t);
	void (*p_retro_set_audio_sample_batch)(retro_audio_sample_batch_t);
	void (*p_retro_set_input_poll)(retro_input_poll_t);
	void (*p_retro_set_input_state)(retro_input_state_t);
	void (*p_retro_init)(void);
	bool (*p_retro_load_game)(const struct retro_game_info *);
	bool (*p_retro_unserialize)(const void *, size_t);
	void (*p_set_cpu_hook)(void (*)(int, int, unsigned, unsigned));
	SYM(retro_set_environment); SYM(retro_set_video_refresh); SYM(retro_set_audio_sample);
	SYM(retro_set_audio_sample_batch); SYM(retro_set_input_poll); SYM(retro_set_input_state);
	SYM(retro_init); SYM(retro_load_game); SYM(retro_run); SYM(retro_get_memory_data);
	SYM(retro_serialize_size); SYM(retro_serialize); SYM(retro_unserialize); SYM(set_cpu_hook);
	p_hook_dma_src = dlsym(core, "hook_dma_src");
	p_vram = dlsym(core, "vram"); p_cram = dlsym(core, "cram");
	p_vsram = dlsym(core, "vsram"); p_reg = dlsym(core, "reg");

	FILE *rf = fopen(argv[2], "rb");
	if (!rf) { perror(argv[2]); return 1; }
	static uint8_t rom[ROM_MAX];
	rom_size = fread(rom, 1, ROM_MAX, rf);
	romb = rom;
	for (int i = 0; i < 8; i++)          /* spurious + level 1..7 autovectors */
		ij_vec[i] = (rom[0x60 + 4 * i] << 24 | rom[0x61 + 4 * i] << 16 | rom[0x62 + 4 * i] << 8 | rom[0x63 + 4 * i]) & 0xFFFFFF;
	fclose(rf);

	p_retro_set_environment(env_cb);
	p_retro_set_video_refresh(video_cb);
	p_retro_set_audio_sample(audio_cb);
	p_retro_set_audio_sample_batch(audio_batch_cb);
	p_retro_set_input_poll(poll_cb);
	p_retro_set_input_state(input_cb);
	p_retro_init();
	struct retro_game_info gi = { argv[2], rom, (size_t)rom_size, NULL };
	if (!p_retro_load_game(&gi)) { fprintf(stderr, "load failed\n"); return 1; }
	p_set_cpu_hook(hook);
	{ void (*spd)(unsigned, unsigned) = dlsym(core, "retro_set_controller_port_device");
	  if (getenv("GUN") && spd) spd(1, RETRO_DEVICE_MENACER);
	  else if (getenv("PAD2") && spd) spd(1, RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 0)); }

	FILE *sf = fopen(argv[3], "r");
	if (!sf) { perror(argv[3]); return 1; }
	char line[512];
	while (fgets(line, sizeof line, sf)) {
		char cmd[32], arg[256];
		int n;
		if (sscanf(line, "%31s", cmd) != 1 || cmd[0] == '#')
			continue;
		int gx, gy, gb;
		if (!strcmp(cmd, "gun") && sscanf(line, "%*s %d %d %d %d", &gx, &gy, &gb, &n) == 4) {
			gun_x = gx; gun_y = gy; gun_btn = gb; while (n--) step(); gun_btn = 0;
		} else if (!strcmp(cmd, "gunhold") && sscanf(line, "%*s %d %d %d", &gx, &gy, &gb) == 3) {
			gun_x = gx; gun_y = gy; gun_btn = gb;
		} else if (!strcmp(cmd, "watch")) {
			unsigned a, len = 1;
			if (sscanf(line, "%*s %x %x", &a, &len) >= 1 && nwatch < 16) {
				watches[nwatch].a = a & 0xFFFFFF; watches[nwatch].n = len; nwatch++;
			}
		} else if (!strcmp(cmd, "unwatch")) {
			nwatch = 0;
			if (watchf) fflush(watchf);
		} else if (!strcmp(cmd, "bp")) {
			unsigned a;
			if (sscanf(line, "%*s %x", &a) == 1 && nbp < 32) bps[nbp++] = a & 0xFFFFFF;
		} else if (!strcmp(cmd, "bpclear")) {
			nbp = 0;
			if (bpf) fflush(bpf);
		} else if (!strcmp(cmd, "peekeach")) {
			char lab[64]; unsigned a; int w = 2;
			if (sscanf(line, "%*s %63s", lab) == 1 && !strcmp(lab, "off"))
				npeek = 0;
			else if (sscanf(line, "%*s %63s %x %d", lab, &a, &w) >= 2 && npeek < 32) {
				snprintf(peeks[npeek].lab, sizeof peeks[npeek].lab, "%s", lab);
				peeks[npeek].a = a; peeks[npeek].w = w; peeks[npeek].init = 0; npeek++;
			}
		} else if (!strcmp(cmd, "waitfor")) {
			unsigned a, v; int w, max;
			if (sscanf(line, "%*s %x %d %x %d", &a, &w, &v, &max) == 4) {
				int ok = 0;
				while (max-- > 0) {
					if (rd_ram(a, w) == v) { ok = 1; break; }
					step();
				}
				if (!ok) ok = rd_ram(a, w) == v;
				fprintf(peeklog(), "%ld waitfor %06x %s\n", frame, a, ok ? "ok" : "timeout");
				fflush(peekf);
			}
		} else if (!strcmp(cmd, "sprites") && sscanf(line, "%*s %255s", arg) == 1) {
			if (!p_vram || !p_reg) { fprintf(stderr, "sprites: core lacks vram/reg exports\n"); continue; }
			char path[1024];
			snprintf(path, sizeof path, "%s/%s.spr", outdir, arg);
			FILE *f = fopen(path, "w");
			unsigned base = (p_reg[5] & ((p_reg[12] & 1) ? 0x7E : 0x7F)) << 9;
			int max = (p_reg[12] & 1) ? 80 : 64, idx = 0;
			fprintf(f, "# SAT base %04x\n# idx x y w h tile pal pri hf vf link\n", base);
			for (int n = 0; n < max; n++) {
				const uint16_t *e = (const uint16_t *)(p_vram + ((base + idx * 8) & 0xFFFF));
				unsigned y = e[0] & 0x3FF, sz = (e[1] >> 8) & 15, link = e[1] & 0x7F, at = e[2], x = e[3] & 0x1FF;
				fprintf(f, "%d %d %d %d %d %03x %u %u %u %u %u\n", idx, (int)x - 128, (int)y - 128,
				        ((sz >> 2) & 3) + 1, (sz & 3) + 1, at & 0x7FF, (at >> 13) & 3, at >> 15,
				        (at >> 11) & 1, (at >> 12) & 1, link);
				if (!link || link >= (unsigned)max) break;
				idx = link;
			}
			fclose(f);
		} else if (!strcmp(cmd, "objs") && sscanf(line, "%*s %255s", arg) == 1) {
			objs_dump(arg);
		} else if (!strcmp(cmd, "bot")) {
			bot_cmd(line);
		} else if (!strcmp(cmd, "botcfg")) {
			char k[64], v[64];
			if (sscanf(line, "%*s %63s %63s", k, v) == 2) bot_config(k, v);
		} else if (!strcmp(cmd, "press2") && sscanf(line, "%*s %255s %d", arg, &n) == 2) {
			pad2 = 0;
			for (char *c = arg; *c; c++)
				if (btn_id(*c) >= 0) pad2 |= 1u << btn_id(*c);
			while (n--) step();
			pad2 = 0;
		} else if (!strcmp(cmd, "gunoff") && sscanf(line, "%*s %d", &gun_off) == 1) {
		} else if (!strcmp(cmd, "run") && sscanf(line, "%*s %d", &n) == 1) {
			while (n--) step();
		} else if (!strcmp(cmd, "press") && sscanf(line, "%*s %255s %d", arg, &n) == 2) {
			pad = 0;
			for (char *c = arg; *c; c++)
				if (btn_id(*c) >= 0) pad |= 1u << btn_id(*c);
			while (n--) step();
			pad = 0;
		} else if (!strcmp(cmd, "record") && sscanf(line, "%*s %255s", arg) == 1) {
			char path[1024];
			snprintf(path, sizeof path, "%s/%s.inp", outdir, arg);
			rec_flush();
			if (recf) fclose(recf);
			recf = fopen(path, "w");
		} else if (!strcmp(cmd, "stoprecord")) {
			rec_flush();
			if (recf) fclose(recf);
			recf = NULL;
		} else if (!strcmp(cmd, "replay") && sscanf(line, "%*s %255s", arg) == 1) {
			char path[1024];
			if (arg[0] == '/' || !strncmp(arg, "./", 2)) snprintf(path, sizeof path, "%s", arg);
			else snprintf(path, sizeof path, "%s/%s", outdir, arg);
			FILE *rp = fopen(path, "r");
			if (!rp) { perror(path); continue; }
			unsigned m; long k;
			while (fscanf(rp, "%x %ld", &m, &k) == 2) {
				pad = m;
				while (k-- > 0) step();
			}
			pad = 0;
			fclose(rp);
		} else if (!strcmp(cmd, "roomsave") && sscanf(line, "%*s %255s", arg) == 1) {
			rs_on = strcmp(arg, "off") != 0;
			if (rs_on) snprintf(rs_prefix, sizeof rs_prefix, "%s", arg);
			rs_last = 0xFFFFFFFF;
		} else if (!strcmp(cmd, "poke")) {
			unsigned a, v; int w = 2;
			if (sscanf(line, "%*s %x %d %x", &a, &w, &v) == 3) {
				uint8_t *wram = p_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
				for (int i = 0; i < w; i++)
					wram[((a & 0xFFFF) + i) ^ 1] = v >> (8 * (w - 1 - i));
			}
		} else if (!strcmp(cmd, "peek")) {
			char lab[64]; unsigned a; int w = 2;
			if (sscanf(line, "%*s %63s %x %d", lab, &a, &w) >= 2) {
				const uint8_t *wram = p_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
				uint32_t v = 0;
				for (int i = 0; i < w; i++)
					v = (v << 8) | wram[((a & 0xFFFF) + i) ^ 1];
				if (!peekf) { char pp[1024]; snprintf(pp, sizeof pp, "%s/peek.log", outdir); peekf = fopen(pp, "a"); }
				fprintf(peekf, "%ld %s %0*x\n", frame, lab, w * 2, v);
				fflush(peekf);
			}
		} else if (!strcmp(cmd, "echo")) {
			if (!peekf) { char pp[1024]; snprintf(pp, sizeof pp, "%s/peek.log", outdir); peekf = fopen(pp, "a"); }
			fprintf(peekf, "%ld %s", frame, line + 5);
			fflush(peekf);
		} else if (!strcmp(cmd, "shot") && sscanf(line, "%*s %255s", arg) == 1) {
			write_shot(arg);
		} else if (!strcmp(cmd, "ram") && sscanf(line, "%*s %255s", arg) == 1) {
			char path[1024];
			snprintf(path, sizeof path, "%s/%s.ram", outdir, arg);
			FILE *f = fopen(path, "wb");
			/* GPGX keeps 68k work RAM byte-swapped within 16-bit words on little-endian hosts */
			const uint8_t *wram = p_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
			for (unsigned a = 0; a < 0x10000; a++)
				fputc(wram[a ^ 1], f);   /* natural 68k byte order */
			fclose(f);
		} else if ((!strcmp(cmd, "save") || !strcmp(cmd, "load")) && sscanf(line, "%*s %255s", arg) == 1) {
			char path[1024];
			snprintf(path, sizeof path, "%s/%s.state", outdir, arg);
			size_t sz = p_retro_serialize_size();
			void *buf = malloc(sz);
			if (cmd[0] == 's') {
				p_retro_serialize(buf, sz);
				FILE *f = fopen(path, "wb"); fwrite(buf, 1, sz, f); fclose(f);
			} else {
				FILE *f = fopen(path, "rb");
				if (f) { if (fread(buf, 1, sz, f) == sz) p_retro_unserialize(buf, sz); fclose(f); }
			}
			free(buf);
		} else if (!strcmp(cmd, "trace") && sscanf(line, "%*s %255s", arg) == 1) {
			char path[1024];
			snprintf(path, sizeof path, "%s/%s.trace", outdir, arg);
			if (tracef) fclose(tracef);
			tracef = fopen(path, "w");
			if (!p_hook_dma_src)
				fprintf(stderr, "warning: core lacks hook_dma_src (vdptrace patch)\n");
		} else if (!strcmp(cmd, "untrace")) {
			if (tracef) fclose(tracef);
			tracef = NULL;
		} else if (!strcmp(cmd, "vdp") && sscanf(line, "%*s %255s", arg) == 1) {
			if (!p_vram || !p_cram || !p_vsram || !p_reg) {
				fprintf(stderr, "vdp: core lacks vram/cram/vsram/reg exports\n");
				continue;
			}
			char path[1024];
			static uint8_t buf[0x10000];
			/* GPGX keeps VRAM as native-endian 16-bit words */
			for (int i = 0; i < 0x10000; i += 2) {
				uint16_t w = *(uint16_t *)(p_vram + i);
				buf[i] = w >> 8; buf[i + 1] = w;
			}
			snprintf(path, sizeof path, "%s/%s.vram", outdir, arg);
			FILE *f = fopen(path, "wb"); fwrite(buf, 1, 0x10000, f); fclose(f);
			/* CRAM is stored packed as 9-bit BBBGGGRRR */
			for (int i = 0; i < 64; i++) {
				uint16_t c = *(uint16_t *)(p_cram + 2 * i);
				uint16_t w = ((c & 0x1C0) << 3) | ((c & 0x038) << 2) | ((c & 0x007) << 1);
				buf[2 * i] = w >> 8; buf[2 * i + 1] = w;
			}
			snprintf(path, sizeof path, "%s/%s.cram", outdir, arg);
			f = fopen(path, "wb"); fwrite(buf, 1, 128, f); fclose(f);
			for (int i = 0; i < 0x80; i += 2) {
				uint16_t w = *(uint16_t *)(p_vsram + i);
				buf[i] = w >> 8; buf[i + 1] = w;
			}
			snprintf(path, sizeof path, "%s/%s.vsram", outdir, arg);
			f = fopen(path, "wb"); fwrite(buf, 1, 0x50, f); fclose(f);
			snprintf(path, sizeof path, "%s/%s.reg", outdir, arg);
			f = fopen(path, "wb"); fwrite(p_reg, 1, 0x20, f); fclose(f);
		} else {
			fprintf(stderr, "bad script line: %s", line);
		}
	}
	rec_flush();
	if (recf) fclose(recf);
	if (watchf) fclose(watchf);
	if (bpf) fclose(bpf);
	char path[1024];
	snprintf(path, sizeof path, "%s/cov.bin", outdir);
	FILE *cf = fopen(path, "wb");
	fwrite(cov, 1, rom_size, cf);
	fclose(cf);
	snprintf(path, sizeof path, "%s/ijump.txt", outdir);
	cf = fopen(path, "w");
	for (int i = 0; i < nij; i++) fprintf(cf, "%06x %06x %ld\n", ij[i].site, ij[i].target & 0xFFFFFF, ij[i].n);
	fclose(cf);
	snprintf(path, sizeof path, "%s/ramexec.bin", outdir);
	cf = fopen(path, "wb");
	fwrite(ramexec, 1, sizeof ramexec, cf);
	fclose(cf);
	fprintf(stderr, "ran %ld frames\n", frame);
	return 0;
}
