/* Emulator wrapper: statically linked Genesis Plus GX via its libretro entry points.
 *
 * Port 1 is a 3/6-button pad (answered through RETRO_DEVICE_JOYPAD). Port 2 is a Sega Menacer
 * (retro_set_controller_port_device(1, RETRO_DEVICE_MENACER)); the core is put in its
 * "lightgun" input mode (core option genesis_plus_gx_gun_input) and reads the gun through
 * RETRO_DEVICE_LIGHTGUN: IS_OFFSCREEN, SCREEN_X/Y (-0x7fff..0x7fff over the viewport),
 * TRIGGER = Menacer A, AUX_A = B, AUX_B = C, START = Start. */
#include "emu.h"
#include "hle.h"
#include "sha1.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "libretro.h"

#define RETRO_DEVICE_MENACER RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_LIGHTGUN, 1)

/* core/debug/cpuhook.c (HOOK_CPU build; type is hook_type_t, an enum) */
extern void set_cpu_hook(void (*hook)(int type, int width, unsigned int address, unsigned int value));

extern uint8_t work_ram[0x10000];     /* core/genesis.c */
/* the core's cartridge area (core/cart_hw/md_cart.h: ext.md_cart.rom, 16-bit byte-swapped on little endian) */
#define MAXROMSIZE 33554432          /* as the core is built (port/cmake/gpgx.cmake) */
#include "types.h"
#include "md_cart.h"
extern union { md_cart_t md_cart; } ext;    /* core/genesis.c: external_t (the cartridge is its first member) */

extern uint8_t sat[0x400];            /* core/vdp_ctrl.c: sprite attribute table, host-endian words */
extern unsigned int m68k_get_reg(int reg);   /* core/m68k/m68kcpu.c; M68K_REG_A7 = 15, M68K_REG_PC = 16 */

/* Lag patch (docs/gun.md, "Latency"; the same code as tools/py/gunpatch.py): MenacerReadXY
 * ($01E442) averages the last 8-16 per-frame HV latches, so the crosshair trails the mouse by
 * 4-8 frames. This replacement for the averaging body at $01E460 converts only the newest
 * latch (same H->X table, same Y offset). Applied to the in-memory copy after the sha1 check;
 * the ROM file is never written. */
#define AIM_PATCH_ADDR 0x1E460
static const uint8_t aim_patch[] = {
	0x20, 0x79, 0xff, 0xff, 0xa0, 0x9e,         /* movea.l ($FFA09E).l,a0   ring write pointer */
	0xb1, 0xfc, 0xff, 0xff, 0xa0, 0x7e,         /* cmpa.l  #$FFFFA07E,a0    at the ring start? */
	0x66, 0x06,                                 /* bne.s   +6 */
	0x41, 0xf9, 0xff, 0xff, 0xa0, 0x9e,         /* lea     ($FFA09E).l,a0   wrap to the ring end */
	0x70, 0x00,                                 /* moveq   #0,d0 */
	0x10, 0x20,                                 /* move.b  -(a0),d0         newest H counter */
	0x43, 0xf9, 0xff, 0xff, 0x9f, 0x7e,         /* lea     ($FF9F7E).l,a1   H -> X/2 table */
	0x10, 0x31, 0x00, 0x00,                     /* move.b  (a1,d0.w),d0 */
	0xd0, 0x40,                                 /* add.w   d0,d0            X */
	0x33, 0xc0, 0xff, 0xff, 0x9f, 0x6e,         /* move.w  d0,($FF9F6E).l */
	0x72, 0x00,                                 /* moveq   #0,d1 */
	0x12, 0x20,                                 /* move.b  -(a0),d1         newest V counter */
	0xd2, 0x79, 0xff, 0xff, 0x9f, 0x78,         /* add.w   ($FF9F78).l,d1   + Y offset */
	0x33, 0xc1, 0xff, 0xff, 0x9f, 0x70,         /* move.w  d1,($FF9F70).l */
	0x4e, 0x75,                                 /* rts */
};
static bool g_aim_fast = true;

static char g_workdir[1024] = ".";
static const uint16_t *g_fb;
static unsigned g_fb_w, g_fb_h;
static size_t g_fb_pitch;
static uint16_t g_fb_copy[720 * 576];      /* frame kept across state loads */
static unsigned g_pad[2];
static int g_gun_x, g_gun_y;
static bool g_gun_off = true;
static unsigned g_gun_btn;
static long g_frames;
static int16_t g_audio[2 * 4096];
static size_t g_audio_frames;
static struct retro_system_av_info g_av;
static bool g_up;
static void *g_boot;          /* power-on state for emu_reset */

/* ------------------------------------------------------------------ libretro callbacks */
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
		/* we only handle RGB565 (FRONTEND_SUPPORTS_RGB565 build) */
		return *(enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565;
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		*(const char **)data = g_workdir;
		return true;
	case RETRO_ENVIRONMENT_GET_CAN_DUPE:
		*(bool *)data = true;
		return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE: {
		struct retro_variable *var = data;
		if (var->key && !strcmp(var->key, "genesis_plus_gx_gun_input")) {
			var->value = "lightgun";       /* not "touchscreen": we want all four buttons */
			return true;
		}
		if (var->key && !strcmp(var->key, "genesis_plus_gx_gun_cursor")) {
			var->value = "disabled";        /* the game draws its own crosshair */
			return true;
		}
		/* the core reuses `var` for every key: always clear the value of the ones we do not set */
		if (var)
			var->value = NULL;
		return false;
	}
	default:
		return false;
	}
}

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
	if (!data)
		return;
	g_fb = data;
	g_fb_w = w;
	g_fb_h = h;
	g_fb_pitch = pitch;
}

static void audio_cb(int16_t l, int16_t r)
{
	if (g_audio_frames < sizeof g_audio / 4) {
		g_audio[2 * g_audio_frames] = l;
		g_audio[2 * g_audio_frames + 1] = r;
		g_audio_frames++;
	}
}

static size_t audio_batch_cb(const int16_t *d, size_t n)
{
	size_t room = sizeof g_audio / 4 - g_audio_frames;
	size_t k = n < room ? n : room;
	memcpy(g_audio + 2 * g_audio_frames, d, k * 4);
	g_audio_frames += k;
	return n;
}

static void poll_cb(void) {}

static const int retro_id[8] = {
	RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN, RETRO_DEVICE_ID_JOYPAD_LEFT,
	RETRO_DEVICE_ID_JOYPAD_RIGHT,
	RETRO_DEVICE_ID_JOYPAD_Y,      /* Genesis A (GPGX default mapping) */
	RETRO_DEVICE_ID_JOYPAD_B,      /* Genesis B */
	RETRO_DEVICE_ID_JOYPAD_A,      /* Genesis C */
	RETRO_DEVICE_ID_JOYPAD_START,
};

/* game pixel -> libretro screen coordinate; the core maps it back over the viewport size */
static int gun_coord(int p, unsigned size)
{
	if (!size)
		size = 1;
	return (int)(((long)(2 * p + 1) * 0xfffe) / (2 * (long)size)) - 0x7fff;
}

static int16_t input_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
	(void)index;
	if (port == 1 && device == RETRO_DEVICE_LIGHTGUN) {
		switch (id) {
		case RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN: return g_gun_off;
		case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X: return (int16_t)gun_coord(g_gun_x, g_fb_w ? g_fb_w : 320);
		case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y: return (int16_t)gun_coord(g_gun_y, g_fb_h ? g_fb_h : 224);
		case RETRO_DEVICE_ID_LIGHTGUN_TRIGGER: return (g_gun_btn & GUN_A) != 0;
		case RETRO_DEVICE_ID_LIGHTGUN_AUX_A: return (g_gun_btn & GUN_B) != 0;
		case RETRO_DEVICE_ID_LIGHTGUN_AUX_B: return (g_gun_btn & GUN_C) != 0;
		case RETRO_DEVICE_ID_LIGHTGUN_START: return (g_gun_btn & GUN_START) != 0;
		}
		return 0;
	}
	if (port > 1 || device != RETRO_DEVICE_JOYPAD)
		return 0;
	unsigned mask = 0;
	for (int i = 0; i < 8; i++)
		if (g_pad[port] & (1u << i))
			mask |= 1u << retro_id[i];
	if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
		return (int16_t)mask;
	return (mask >> id) & 1;
}

/* ------------------------------------------------------------------ hooks */
/* Fan-out of the core's cpu_hook (HOOK_CPU): the HLE sandbox records port writes, and the
 * entry-state capture fires on execute. */
#define MAX_HOOKS 8
static emu_hook_fn g_hooks[MAX_HOOKS];
static int g_nhooks;

static void hook_tramp(int type, int width, unsigned int addr, unsigned int value)
{
	for (int i = 0; i < g_nhooks; i++)
		g_hooks[i](type, width, addr, value);
}

void emu_add_hook(emu_hook_fn fn)
{
	for (int i = 0; i < g_nhooks; i++)
		if (g_hooks[i] == fn)
			return;
	if (g_nhooks < MAX_HOOKS)
		g_hooks[g_nhooks++] = fn;
	set_cpu_hook(hook_tramp);
}

void emu_remove_hook(emu_hook_fn fn)
{
	for (int i = 0; i < g_nhooks; i++)
		if (g_hooks[i] == fn) {
			memmove(g_hooks + i, g_hooks + i + 1, (g_nhooks - i - 1) * sizeof *g_hooks);
			g_nhooks--;
			break;
		}
	set_cpu_hook(g_nhooks ? hook_tramp : NULL);
}

/* ------------------------------------------------------------------ setup */
static bool file_exists(const char *p)
{
	return access(p, R_OK) == 0;
}

const char *emu_find_rom(const char *explicit_path, const char *argv0)
{
	static char buf[2048];
	if (explicit_path)
		return explicit_path;
	if (file_exists("rom/baserom.md"))
		return "rom/baserom.md";
#ifndef T2_DIST          /* the development tree the binary was built from (not in release builds) */
	snprintf(buf, sizeof buf, "%s/rom/baserom.md", T2_ROOT);
	if (file_exists(buf))
		return buf;
#endif
	/* <exe dir>/../rom, ../../rom, ../../../rom */
	char exe[1024];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
	if (n <= 0 && argv0)
		n = (ssize_t)snprintf(exe, sizeof exe, "%s", argv0);
	if (n > 0) {
		exe[n] = 0;
		char *s = strrchr(exe, '/');
		if (s)
			*s = 0;
		const char *ups[] = { "", "/..", "/../..", "/../../.." };
		for (int i = 0; i < 4; i++) {
			snprintf(buf, sizeof buf, "%s%s/rom/baserom.md", exe, ups[i]);
			if (file_exists(buf))
				return buf;
		}
	}
	return NULL;
}

uint8_t *emu_read_rom(const char *path, size_t *len, char *err, size_t errlen)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		snprintf(err, errlen, "%s: cannot open", path);
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz < 0x1000 || sz > 8 * 1024 * 1024) {
		fclose(f);
		snprintf(err, errlen, "%s: not a Genesis ROM (size %ld)", path, sz);
		return NULL;
	}
	uint8_t *rom = malloc((size_t)sz);
	if (!rom || fread(rom, 1, (size_t)sz, f) != (size_t)sz) {
		fclose(f);
		free(rom);
		snprintf(err, errlen, "%s: read error", path);
		return NULL;
	}
	fclose(f);
	size_t n = (size_t)sz;
	if (n % 16384 == 512) {
		/* SMD: 512-byte header, then 16 KiB blocks holding the odd bytes (first half) and the
		 * even bytes (second half) of 16 KiB of ROM (tools/py/smd2bin.py) */
		n -= 512;
		uint8_t *raw = malloc(n);
		if (!raw) {
			free(rom);
			snprintf(err, errlen, "out of memory");
			return NULL;
		}
		for (size_t b = 0; b < n; b += 16384) {
			const uint8_t *blk = rom + 512 + b;
			for (size_t i = 0; i < 8192; i++) {
				raw[b + 2 * i + 1] = blk[i];
				raw[b + 2 * i] = blk[8192 + i];
			}
		}
		free(rom);
		rom = raw;
	}
	*len = n;
	return rom;
}

bool emu_init(const char *rom_path, const char *workdir, bool use_gun, char *err, size_t errlen)
{
	if (workdir)
		snprintf(g_workdir, sizeof g_workdir, "%s", workdir);
	size_t sz;
	uint8_t *rom = emu_read_rom(rom_path, &sz, err, errlen);
	if (!rom)
		return false;
	char hex[41];
	sha1_hex(rom, sz, hex);
	if (strcmp(hex, T2_ROM_SHA1)) {
		free(rom);
		snprintf(err, errlen, "%s: sha1 %s does not match T2: The Arcade Game (JUE) %s - refusing to run",
		         rom_path, hex, T2_ROM_SHA1);
		return false;
	}

	retro_set_environment(env_cb);
	retro_set_video_refresh(video_cb);
	retro_set_audio_sample(audio_cb);
	retro_set_audio_sample_batch(audio_batch_cb);
	retro_set_input_poll(poll_cb);
	retro_set_input_state(input_cb);
	retro_init();
	struct retro_game_info gi = { rom_path, rom, sz, NULL };
	bool ok = retro_load_game(&gi);
	free(rom);          /* the core keeps its own copy */
	if (!ok) {
		snprintf(err, errlen, "core failed to load %s", rom_path);
		return false;
	}
	/* The core reads the ROM from the file, not from our buffer: patch its copy (after the sha1 check
	 * above; the file on disk is untouched). The core byte-swaps the ROM, hence the ^ 1. */
	if (g_aim_fast)
		for (size_t i = 0; i < sizeof aim_patch; i++)
			ext.md_cart.rom[(AIM_PATCH_ADDR + i) ^ 1] = aim_patch[i];
	retro_set_controller_port_device(1, use_gun ? RETRO_DEVICE_MENACER : RETRO_DEVICE_NONE);
	retro_get_system_av_info(&g_av);
	hle_install();          /* hook the CPU (docs/porting.md); no function is enabled by default */
	g_boot = malloc(emu_state_size());
	if (!emu_state_save(g_boot, emu_state_size())) {
		free(g_boot);
		g_boot = NULL;
	}
	g_up = true;
	return true;
}

void emu_shutdown(void)
{
	if (!g_up)
		return;
	retro_unload_game();
	retro_deinit();
	g_up = false;
}

/* ------------------------------------------------------------------ running */
void emu_set_pad(int port, unsigned buttons)
{
	if (port >= 0 && port < 2)
		g_pad[port] = buttons & 0xFF;
}

unsigned emu_get_pad(int port)
{
	return port >= 0 && port < 2 ? g_pad[port] : 0;
}

unsigned emu_pad_from_letters(const char *s)
{
	unsigned m = 0;
	for (; *s; s++) {
		switch (*s) {
		case 'U': m |= PAD_UP; break;
		case 'D': m |= PAD_DOWN; break;
		case 'L': m |= PAD_LEFT; break;
		case 'R': m |= PAD_RIGHT; break;
		case 'A': m |= PAD_A; break;
		case 'B': m |= PAD_B; break;
		case 'C': m |= PAD_C; break;
		case 'S': m |= PAD_START; break;
		}
	}
	return m;
}

/* the game's crosshair: the first 16x16 sprite using tile $7B6 (docs/gun.md); top-left corner */
bool emu_crosshair_sprite(int *x, int *y)
{
	const uint16_t *w = (const uint16_t *)sat;
	int n = 0, i = 0;
	do {
		const uint16_t *e = w + 4 * i;
		if ((e[2] & 0x7FF) == 0x7B6 && (e[1] & 0x0F00) == 0x0500) {
			*y = (e[0] & 0x3FF) - 128;
			*x = (e[3] & 0x1FF) - 128;
			return true;
		}
		i = e[1] & 0x7F;
	} while (i && ++n < 80);
	return false;
}

void emu_set_aim_fast(bool on) { g_aim_fast = on; }
uint32_t emu_cpu_pc(void) { return m68k_get_reg(16); }
uint32_t emu_cpu_sp(void) { return m68k_get_reg(15); }

void emu_set_gun(int x, int y, bool offscreen, unsigned buttons)
{
	g_gun_x = x;
	g_gun_y = y;
	g_gun_off = offscreen;
	g_gun_btn = buttons & 0xF;
}

void emu_run_frame(void)
{
	g_audio_frames = 0;
	hle_frame_begin();
	retro_run();
	g_frames++;
	if (g_fb && g_fb_w <= 720 && g_fb_h <= 576) {
		for (unsigned y = 0; y < g_fb_h; y++)
			memcpy(g_fb_copy + y * g_fb_w, (const uint8_t *)g_fb + y * g_fb_pitch, g_fb_w * 2);
	}
}

long emu_frame_count(void) { return g_frames; }

void emu_reset(void)
{
	hle_state_reset();
	/* restore the power-on snapshot: retro_reset() is a soft reset that keeps RAM, so runs
	 * after it are not reproducible */
	if (g_boot)
		emu_state_load(g_boot, emu_state_size());
	else
		retro_reset();
	g_frames = 0;
}

const uint16_t *emu_video(unsigned *w, unsigned *h, size_t *pitch)
{
	*w = g_fb_w;
	*h = g_fb_h;
	*pitch = g_fb_w * 2;
	return g_fb_w ? g_fb_copy : NULL;
}

const int16_t *emu_audio(size_t *frames)
{
	*frames = g_audio_frames;
	return g_audio;
}

double emu_fps(void) { return g_av.timing.fps; }
double emu_sample_rate(void) { return g_av.timing.sample_rate; }

/* ------------------------------------------------------------------ state */
size_t emu_state_size(void) { return retro_serialize_size(); }
bool emu_state_save(void *buf, size_t size) { return retro_serialize(buf, size); }
bool emu_state_load(const void *buf, size_t size) { hle_state_reset(); return retro_unserialize(buf, size); }

bool emu_state_save_file(const char *path)
{
	size_t sz = emu_state_size();
	void *buf = malloc(sz);
	bool ok = emu_state_save(buf, sz);
	FILE *f = ok ? fopen(path, "wb") : NULL;
	if (f) {
		ok = fwrite(buf, 1, sz, f) == sz;
		fclose(f);
	} else {
		ok = false;
	}
	free(buf);
	return ok;
}

bool emu_state_load_file(const char *path)
{
	size_t sz = emu_state_size();
	void *buf = malloc(sz);
	FILE *f = fopen(path, "rb");
	bool ok = false;
	if (f) {
		ok = fread(buf, 1, sz, f) == sz && emu_state_load(buf, sz);
		fclose(f);
	}
	free(buf);
	return ok;
}

/* ------------------------------------------------------------------ dumps */
bool emu_write_ppm(const char *path)
{
	unsigned w, h;
	size_t pitch;
	const uint16_t *px = emu_video(&w, &h, &pitch);
	FILE *f = px ? fopen(path, "wb") : NULL;
	if (!f)
		return false;
	fprintf(f, "P6\n%u %u\n255\n", w, h);
	for (unsigned y = 0; y < h; y++)
		for (unsigned x = 0; x < w; x++) {
			uint16_t p = px[y * w + x];
			uint8_t rgb[3] = { (uint8_t)((p >> 11) << 3), (uint8_t)(((p >> 5) & 63) << 2), (uint8_t)((p & 31) << 3) };
			fwrite(rgb, 1, 3, f);
		}
	fclose(f);
	return true;
}

bool emu_write_ram(const char *path)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return false;
	for (unsigned a = 0; a < 0x10000; a++)
		fputc(work_ram[a ^ 1], f);   /* GPGX keeps words host-endian: natural 68k byte order */
	fclose(f);
	return true;
}

uint8_t emu_ram8(uint32_t a) { return work_ram[(a & 0xFFFF) ^ 1]; }
uint16_t emu_ram16(uint32_t a) { return *(uint16_t *)(work_ram + (a & 0xFFFE)); }
uint32_t emu_ram32(uint32_t a) { return ((uint32_t)emu_ram16(a) << 16) | emu_ram16(a + 2); }
