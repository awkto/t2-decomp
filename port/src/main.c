/* t2port: native SDL2 host for T2: The Arcade Game (Genesis Plus GX core, linked statically).
 *
 * Mouse = Sega Menacer light gun on port 2 (the game's own "USE MENACER" mode):
 *   mouse position = gun aim, left button = trigger, right button = Menacer A/B/C (option
 *   mouse_right, default B), middle button = Start. The OS cursor is hidden over the game
 *   image (the game draws its own crosshair). --no-mouse leaves port 2 empty.
 * Keyboard / gamepad = 3/6-button pad on port 1:
 *   arrows = D-pad, Z/A = A, X/S = B, C/D = C, Enter = Start
 *   F1 debug overlay, F5 save state, F9 load state, F6/F7 previous/next state slot, P pause,
 *   N or . frame advance (pauses), Tab (hold) fast forward, F11 or Alt+Enter fullscreen,
 *   F12 screenshot, Esc quit.
 * Gamepad (SDL GameController): D-pad / left stick, X/Y = A, A = B, B/RB = C, Start, Back = pause.
 * Run with --help for the command line (headless --script / --frames / --screenshot). */
#include <SDL.h>

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/stat.h>

#include "emu.h"
#include "gun.h"
#include "hle.h"
#include "ipprof.h"
#include "mouse.h"
#include "options.h"
#include "render/hifps.h"
#include "render/render_emu.h"
#include "overlay.h"
#include "romfind.h"
#include "script.h"

typedef struct {
	const char *rom, *script, *outdir, *screenshot, *window_shot, *savedir, *config, *dump_ram, *hle;
	const char *framehash, *memdump;
	bool realtime_script, no_throttle, overlay, hle_list;
	long frames;
} args_t;

static void usage(const char *argv0)
{
	printf("usage: %s [options]\n"
	       "  --rom PATH              ROM (default: ini, next to the binary, file picker; sha1 must match;\n"
	       "                          a raw .md/.bin or an interleaved .smd dump)\n"
	       "  --options K=V,...       set options for this run (--options --json lists them all); an\n"
	       "                          ini file ~/.config/t2-decomp/t2.ini holds the permanent ones\n"
	       "  --config FILE           use this ini instead\n"
	       "  --dump-ram FILE         at exit, write the 64 KiB work RAM\n"
	       "  --framehash FILE        one line per emulated frame: hashes of work RAM, VRAM, CRAM,\n"
	       "                          VSRAM, VDP registers (lockstep gate; port/tools/lockstep.sh)\n"
	       "  --memdump FILE          at exit, write RAM|VRAM|CRAM|VSRAM|VDPREG concatenated\n"
	       "  --scale N               initial window scale (default 3)\n"
	       "  --fullscreen            start fullscreen (F11 toggles)\n"
	       "  --no-vsync              don't sync presentation to the display\n"
	       "  --mute                  no audio\n"
	       "  --overlay               start with the debug overlay on (F1)\n"
	       "  --script FILE           run a harness-style script first (unthrottled); see docs/port.md\n"
	       "  --realtime-script       run the script (and --frames) at normal speed, with audio\n"
	       "  --frames N              exit after N frames (after the script)\n"
	       "  --screenshot FILE.ppm   at exit, write the last emulated frame\n"
	       "  --window-shot FILE.ppm  at exit, write the rendered window (scaling + letterbox)\n"
	       "  --outdir DIR            where script shot/ram/save files go (default runs/port)\n"
	       "  --savedir DIR           quick states (default ~/.local/share/t2-decomp)\n"
	       "  --no-throttle           run as fast as possible\n"
	       "  --no-mouse              no Menacer on port 2 (keyboard/gamepad only)\n"
	       "  --hle SPEC              run 68k functions as C: none (default), all, or a list of names\n"
	       "                          separated by ',' or '+' (-name off, +name on); see docs/porting.md\n"
	       "  --render MODE           gpgx (default), c (the C renderer, docs/render.md) or compare; env T2_RENDER too\n"
	       "  --fps RATE              high frame rate (docs/hifps.md): off, auto (the display's refresh rate)\n"
	       "                          or Hz (120, 144, 240): the game stays at 60 Hz, in-between pictures are\n"
	       "                          interpolated; F4 toggles; scripted runs stay at 60 Hz unless given\n"
	       "  --hle-list              print the active C replacements and exit\n"
	       "Headless: SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy %s --script s.txt --screenshot out.ppm\n",
	       argv0, argv0);
}

static void mkdirs(const char *dir)
{
	char tmp[1024];
	snprintf(tmp, sizeof tmp, "%s", dir);
	for (char *p = tmp + 1; *p; p++)
		if (*p == '/') {
			*p = 0;
			mkdir(tmp, 0777);
			*p = '/';
		}
	mkdir(tmp, 0777);
}

/* ------------------------------------------------------------------ input */
static unsigned g_keys, g_padbtn;
static int g_axis_x, g_axis_y;

/* keyboard bindings: the key_* options (SDL names, several per button joined with |) */
#define MAX_KEYS_PER_BUTTON 6
static const struct { const char *opt; unsigned pad; } keybind[] = {
	{ "key_up", PAD_UP }, { "key_down", PAD_DOWN }, { "key_left", PAD_LEFT }, { "key_right", PAD_RIGHT },
	{ "key_a", PAD_A }, { "key_b", PAD_B }, { "key_c", PAD_C }, { "key_start", PAD_START },
};
static SDL_Keycode keymap[8][MAX_KEYS_PER_BUTTON];

static void keys_configure(void)
{
	for (int b = 0; b < 8; b++) {
		char list[256];
		snprintf(list, sizeof list, "%s", options_str(keybind[b].opt));
		int n = 0;
		for (char *save, *t = strtok_r(list, "|", &save); t; t = strtok_r(NULL, "|", &save)) {
			SDL_Keycode k = SDL_GetKeyFromName(t);
			if (k == SDLK_UNKNOWN)
				fprintf(stderr, "%s: unknown key name '%s' ignored\n", keybind[b].opt, t);
			else if (n < MAX_KEYS_PER_BUTTON)
				keymap[b][n++] = k;
		}
	}
}

static unsigned key_to_pad(SDL_Keycode k)
{
	unsigned m = 0;
	for (int b = 0; b < 8; b++)
		for (int i = 0; i < MAX_KEYS_PER_BUTTON; i++)
			if (keymap[b][i] == k)
				m |= keybind[b].pad;
	return m;
}

static unsigned button_to_pad(Uint8 b)
{
	switch (b) {
	case SDL_CONTROLLER_BUTTON_DPAD_UP: return PAD_UP;
	case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return PAD_DOWN;
	case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return PAD_LEFT;
	case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return PAD_RIGHT;
	case SDL_CONTROLLER_BUTTON_X: case SDL_CONTROLLER_BUTTON_Y: return PAD_A;
	case SDL_CONTROLLER_BUTTON_A: return PAD_B;
	case SDL_CONTROLLER_BUTTON_B: case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return PAD_C;
	case SDL_CONTROLLER_BUTTON_START: return PAD_START;
	}
	return 0;
}

static unsigned live_pad(void)
{
	unsigned m = g_keys | g_padbtn;
	const int dz = 16000;
	if (g_axis_x < -dz) m |= PAD_LEFT;
	if (g_axis_x > dz) m |= PAD_RIGHT;
	if (g_axis_y < -dz) m |= PAD_UP;
	if (g_axis_y > dz) m |= PAD_DOWN;
	return m;
}

/* ------------------------------------------------------------------ video */
/* The game frame (RGB565 -> RGBA) is drawn centred at the largest whole-number scale that
 * fits the window; the rest is black. gun_map_window() inverts that placement for the mouse. */
typedef struct {
	SDL_Window *win;
	SDL_Renderer *ren;
	SDL_Texture *tex;
	int tw, th;                 /* texture (= game frame) size */
	uint32_t buf[720 * 576];
} video_t;

static bool video_init(video_t *v, int scale, bool fullscreen, bool vsync)
{
	v->win = SDL_CreateWindow("T2: The Arcade Game", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
	                          320 * scale, 224 * scale,
	                          SDL_WINDOW_RESIZABLE | (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
	if (!v->win)
		return false;
	v->ren = SDL_CreateRenderer(v->win, -1, SDL_RENDERER_ACCELERATED | (vsync ? SDL_RENDERER_PRESENTVSYNC : 0));
	if (!v->ren)
		v->ren = SDL_CreateRenderer(v->win, -1, 0);
	return v->ren != NULL;
}

/* destination rectangle of the image in renderer output pixels */
static SDL_Rect video_rect(const video_t *v, int ow, int oh)
{
	int s = ow / v->tw < oh / v->th ? ow / v->tw : oh / v->th;
	if (s < 1)
		s = 1;
	SDL_Rect r = { (ow - v->tw * s) / 2, (oh - v->th * s) / 2, v->tw * s, v->th * s };
	return r;
}

static void video_draw(video_t *v)
{
	int ow, oh;
	SDL_GetRendererOutputSize(v->ren, &ow, &oh);
	SDL_SetRenderDrawColor(v->ren, 0, 0, 0, 255);
	SDL_RenderClear(v->ren);
	if (v->tex) {
		SDL_Rect dst = video_rect(v, ow, oh);
		SDL_RenderCopy(v->ren, v->tex, NULL, &dst);
	}
}

/* upload the frame (v->buf, w x h RGBA) and draw it into the back buffer */
static bool video_prepare(video_t *v, int w, int h)
{
	if (!v->ren)
		return false;
	if (!v->tex || v->tw != w || v->th != h) {
		if (v->tex)
			SDL_DestroyTexture(v->tex);
		v->tex = SDL_CreateTexture(v->ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, w, h);
		SDL_SetTextureScaleMode(v->tex, SDL_ScaleModeNearest);
		v->tw = w;
		v->th = h;
	}
	SDL_UpdateTexture(v->tex, NULL, v->buf, w * 4);
	video_draw(v);
	return true;
}

static uint32_t rgba_from_565(uint16_t p)
{
	uint32_t r = (uint32_t)(p >> 11) << 3, g = (uint32_t)((p >> 5) & 63) << 2, b = (uint32_t)(p & 31) << 3;
	r |= r >> 5;
	g |= g >> 6;
	b |= b >> 5;
	return r | g << 8 | b << 16 | 0xFF000000u;       /* RGBA32 byte order on little endian */
}

static bool game_frame(video_t *v, int *w, int *h)
{
	unsigned gw, gh;
	size_t pitch;
	const uint16_t *px = emu_video(&gw, &gh, &pitch);
	if (!px || gw * gh > sizeof v->buf / sizeof v->buf[0])
		return false;
	for (unsigned i = 0; i < gw * gh; i++)
		v->buf[i] = rgba_from_565(px[i]);
	*w = (int)gw;
	*h = (int)gh;
	return true;
}

/* a picture (RGB565, as emu_video gives it) -> v->buf */
static bool picture_to_buf(video_t *v, const uint16_t *px, unsigned gw, unsigned gh)
{
	if (!px || gw * gh > sizeof v->buf / sizeof v->buf[0])
		return false;
	for (unsigned i = 0; i < gw * gh; i++)
		v->buf[i] = rgba_from_565(px[i]);
	return true;
}

static double now_s(void)
{
	return (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency();
}

/* sleep until t (seconds on now_s()'s clock): nanosleep up to 0.4 ms before, the rest spinning */
static void wait_until(double t)
{
	for (;;) {
		double left = t - now_s();
		if (left <= 0)
			return;
		if (left > 0.0006) {
			struct timespec ts = { 0, (long)((left - 0.0004) * 1e9) };
			nanosleep(&ts, NULL);
		}
	}
}

static bool window_shot(video_t *v, const char *path)
{
	int w, h;
	if (!v->ren || SDL_GetRendererOutputSize(v->ren, &w, &h))
		return false;
	uint8_t *rgb = calloc((size_t)w * h, 3);
	video_draw(v);      /* the back buffer is undefined after present: draw again */
	bool ok = SDL_RenderReadPixels(v->ren, NULL, SDL_PIXELFORMAT_RGB24, rgb, w * 3) == 0;
	FILE *f = ok ? fopen(path, "wb") : NULL;
	if (f) {
		fprintf(f, "P6\n%d %d\n255\n", w, h);
		fwrite(rgb, 1, (size_t)w * h * 3, f);
		fclose(f);
	}
	free(rgb);
	return f != NULL;
}

/* mouse position (window coordinates) -> game pixel through the placement of video_draw.
 * Returns 1 and the game pixel when the pointer is on the image, 0 when off it, -1 if unknown. */
static int window_to_game(video_t *v, int x, int y, int *gx, int *gy)
{
	int ww, wh, ow, oh;
	if (!v->win || !v->ren || !v->tw)
		return -1;
	SDL_GetWindowSize(v->win, &ww, &wh);
	if (SDL_GetRendererOutputSize(v->ren, &ow, &oh) || ww <= 0 || wh <= 0)
		return -1;
	return gun_map_window(x * ow / ww, y * oh / wh, ow, oh, v->tw, v->th, gx, gy) ? 1 : 0;
}

static void mouse_from_window(video_t *v, int x, int y)
{
	int gx, gy, on = window_to_game(v, x, y, &gx, &gy);
	if (on == 0) {
		mouse_leave();
		SDL_ShowCursor(SDL_ENABLE);       /* outside the image: the OS cursor is useful again */
	} else if (on > 0) {
		mouse_move(gx, gy);
		SDL_ShowCursor(gun_enabled() ? SDL_DISABLE : SDL_ENABLE);
	}
}

/* high frame rate (docs/hifps.md): the rate for the option value off / auto / Hz, 0 = off; capped to
 * the display's refresh when vsync is on (every flip waits for a refresh) */
static int display_hz(void)
{
	SDL_DisplayMode m;
	return SDL_GetCurrentDisplayMode(0, &m) == 0 ? m.refresh_rate : 0;
}

static int fps_rate(const char *val, bool vsync)
{
	int hz = 0, disp = display_hz();
	if (!strcasecmp(val, "off") || !strcmp(val, "0"))
		return 0;
	if (!strcasecmp(val, "auto"))
		hz = disp > 60 ? disp : 0;
	else
		hz = atoi(val);
	if (hz < 61 || hz > 1000) {
		if (strcasecmp(val, "auto"))
			fprintf(stderr, "fps: '%s' is not off, auto or a rate in 61..1000 Hz; high frame rate off\n", val);
		return 0;
	}
	if (vsync && disp > 0 && hz > disp)
		hz = disp;
	return hz;
}

/* the pointer now, in game pixels, for the crosshair of every picture */
static void hf_pointer(video_t *v)
{
	if (!gun_enabled() || !v->win)
		return;
	SDL_PumpEvents();
	int mx, my, gx = 0, gy = 0;
	SDL_GetMouseState(&mx, &my);
	bool in = (SDL_GetWindowFlags(v->win) & SDL_WINDOW_MOUSE_FOCUS) && window_to_game(v, mx, my, &gx, &gy) > 0;
	hifps_mouse_now(gx, gy, !in);
}

int main(int argc, char **argv)
{
	args_t a = { 0 };
	a.frames = -1;
	a.outdir = "runs/port";
	int scale_arg = 0;

	/* options: defaults < ini < command line.  --config is looked for first so that the ini
	 * is read before the flags that override it. */
	char cfgpath[1024], operr[300];
	options_default_path(cfgpath, sizeof cfgpath);
	for (int i = 1; i + 1 < argc; i++)
		if (!strcmp(argv[i], "--config"))
			snprintf(cfgpath, sizeof cfgpath, "%s", argv[i + 1]);
	options_init();
	options_load(cfgpath);
	for (int i = 1; i < argc; i++) {
		const char *s = argv[i];
#define NEXT() (i + 1 < argc ? argv[++i] : (usage(argv[0]), exit(2), ""))
#define SETOPT(k, v) do { if (!options_set(k, v, OPT_CLI, operr, sizeof operr)) { fprintf(stderr, "%s\n", operr); return 2; } } while (0)
		if (!strcmp(s, "--rom")) a.rom = NEXT();
		else if (!strcmp(s, "--config")) a.config = NEXT();
		else if (!strcmp(s, "--options")) {
			const char *v = NEXT();
			if (!strcmp(v, "--json") || !strcmp(v, "list")) {
				options_print(stdout, !strcmp(v, "--json"));
				return 0;
			}
			if (!options_parse_list(v, OPT_CLI, operr, sizeof operr)) {
				fprintf(stderr, "--options: %s\n", operr);
				return 2;
			}
		}
		else if (!strcmp(s, "--dump-ram")) a.dump_ram = NEXT();
		else if (!strcmp(s, "--framehash")) a.framehash = NEXT();
		else if (!strcmp(s, "--memdump")) a.memdump = NEXT();
		else if (!strcmp(s, "--scale")) { SETOPT("scale", NEXT()); scale_arg = 1; }
		else if (!strcmp(s, "--fullscreen")) SETOPT("fullscreen", "on");
		else if (!strcmp(s, "--no-vsync")) SETOPT("vsync", "off");
		else if (!strcmp(s, "--mute")) SETOPT("mute", "on");
		else if (!strcmp(s, "--overlay")) a.overlay = true;
		else if (!strcmp(s, "--script")) a.script = NEXT();
		else if (!strcmp(s, "--realtime-script")) a.realtime_script = true;
		else if (!strcmp(s, "--frames")) a.frames = atol(NEXT());
		else if (!strcmp(s, "--screenshot")) a.screenshot = NEXT();
		else if (!strcmp(s, "--window-shot")) a.window_shot = NEXT();
		else if (!strcmp(s, "--outdir")) a.outdir = NEXT();
		else if (!strcmp(s, "--savedir")) a.savedir = NEXT();
		else if (!strcmp(s, "--no-throttle")) a.no_throttle = true;
		else if (!strcmp(s, "--no-mouse")) SETOPT("mouse", "off");
		else if (!strcmp(s, "--hle")) a.hle = NEXT();
		else if (!strcmp(s, "--render")) SETOPT("render", NEXT());
		else if (!strcmp(s, "--fps")) SETOPT("fps", NEXT());
		else if (!strcmp(s, "--hle-list")) a.hle_list = true;
		else if (!strcmp(s, "-h") || !strcmp(s, "--help")) { usage(argv[0]); return 0; }
		else { fprintf(stderr, "unknown option %s\n", s); usage(argv[0]); return 2; }
	}
	(void)scale_arg;
	const int scale = options_int("scale");
	const bool fullscreen = options_bool("fullscreen");
	const bool vsync = options_bool("vsync");
	const bool mute = options_bool("mute");
	const bool no_mouse = !options_bool("mouse");
	const int volume = options_int("volume");
	mouse_set_enabled(!no_mouse);
	gun_set_right(options_str("mouse_right")[0]);
	mouse_set_aim_correct(options_bool("aim_correct"));
	emu_set_aim_fast(!strcmp(options_str("aim"), "fast"));

	/* C replacements for 68k functions: --hle overrides the hle option (default none). Every
	 * replacement starts disabled, so plain t2port and --hle none run the original 68k code. */
	const char *hle_spec = a.hle ? a.hle : options_str("hle");
	if (!hle_configure(hle_spec)) {
		fprintf(stderr, "--hle: unknown function in '%s' (see docs/porting.md; --hle-list shows the registry)\n", hle_spec);
		return 2;
	}
	if (a.hle_list) {
		hle_list_active(stdout);
		return 0;
	}
	ipprof_init(hle_spec);   /* T2_IPPROF=file: interpreted-instruction profile (src/ipprof.c) */
	fprintf(stderr, "hle: %d active: ", hle_count_active());
	hle_list_active(stderr);

	char savedir[1024];
	if (a.savedir)
		snprintf(savedir, sizeof savedir, "%s", a.savedir);
	else
		options_data_dir(savedir, sizeof savedir);

	/* the ROM: --rom, ini, next to the binary, ./rom/baserom.md, then ask (picker / drop window) */
	bool batch = a.script || a.frames >= 0 || a.screenshot || a.window_shot;
	char rompath[1400], err[512];
	if (!rom_locate(a.rom, options_str("rom"), argv[0], rompath, sizeof rompath)) {
		if (batch || !rom_ask_user(rompath, sizeof rompath, err, sizeof err)) {
			fprintf(stderr, "T2: The Arcade Game ROM not found. Put the ROM (sha1 %s) next to the program,\n"
			        "or pass --rom FILE, or set rom = FILE in %s\n", T2_ROM_SHA1, cfgpath);
			return 1;
		}
	}
	mkdirs(savedir);
	mkdirs(a.outdir);

	if (!emu_init(rompath, savedir, !no_mouse, err, sizeof err)) {
		fprintf(stderr, "%s\n", err);
		return 1;
	}
	if (!batch) {      /* remember a ROM that worked */
		char *abs = realpath(rompath, NULL);
		if (abs && strcmp(abs, options_str("rom"))) {
			options_set("rom", abs, OPT_INI, err, sizeof err);
			if (!options_save(cfgpath))
				fprintf(stderr, "cannot write %s\n", cfgpath);
		}
		free(abs);
	}
	if (!getenv("T2_RENDER") && !render_emu_configure(options_str("render")))
		fprintf(stderr, "render: unknown renderer, using gpgx\n");
	keys_configure();

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER) != 0) {
		fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	static video_t v;
	if (!video_init(&v, scale, fullscreen, vsync))
		fprintf(stderr, "video unavailable (%s), running without a window\n", SDL_GetError());

	/* high frame rate: scripted / batch runs stay at 60 Hz unless the rate is given explicitly */
	int hf_opt_hz = 0, hf_toggle_hz = 0;
	{
		bool explicit_rate = options_source("fps") == OPT_CLI;
		if (!batch || explicit_rate)
			hf_opt_hz = fps_rate(options_str("fps"), vsync && v.ren);
		hf_toggle_hz = hf_opt_hz ? hf_opt_hz : (display_hz() > 60 ? display_hz() : 120);
		if (hf_opt_hz) {
			if (render_emu_mode() == RENDER_GPGX)
				render_emu_configure("c");        /* the in-betweens come from the C renderer's scene */
			if (hifps_configure(hf_opt_hz))
				fprintf(stderr, "hifps: %d Hz, %d pictures a frame\n", hf_opt_hz, hifps_per_frame());
		}
	}

	SDL_AudioDeviceID adev = 0;
	int rate = (int)(emu_sample_rate() + 0.5);
	if (!mute) {
		SDL_AudioSpec want = { 0 }, have;
		want.freq = rate;
		want.format = AUDIO_S16SYS;
		want.channels = 2;
		want.samples = 512;
		adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
		if (adev)
			SDL_PauseAudioDevice(adev, 0);
		else
			fprintf(stderr, "audio unavailable: %s\n", SDL_GetError());
	}
	/* queue target: ~3 frames of audio (50 ms) keeps latency low without underruns */
	const Uint32 q_target = (Uint32)(rate * 4 * 0.050);

	script *scr = NULL;
	if (a.script) {
		scr = script_open(a.script, a.outdir);
		if (!scr)
			return 1;
	}
	FILE *fh = NULL;
	if (a.framehash) {
		fh = fopen(a.framehash, "w");
		if (!fh) {
			fprintf(stderr, "cannot write %s\n", a.framehash);
			return 1;
		}
	}
	int rc = 0;
	bool quit = false, paused = false, step = false, ff = false, overlay = a.overlay;
	int slot = 0;
	long frames_after_script = 0;
	char msg[128] = "";
	int msg_timer = 0;
	double fps_show = 0;
	Uint64 perf_freq = SDL_GetPerformanceFrequency(), t_start = SDL_GetPerformanceCounter();
	Uint64 t_fps = t_start, next_tick = t_start;
	long fps_frames = 0, total = 0;
	const double frame_dt = 1.0 / emu_fps();
	static char ovl_text[256];
	bool drew_once = false;
	const double T = 1.0 / emu_fps();
	double hf_t0 = 0;                    /* when R(k-1) was (to be) presented */
	bool hf_clock = false;
	long hf_pics = 0, hf_logic = 0, hf_sp_n = 0, hf_sp_ok = 0;
	double hf_last = 0, hf_sp_sum = 0, hf_sp_sq = 0, hf_prep = 0, hf_flip = 0;
	struct rusage ru0;
	getrusage(RUSAGE_SELF, &ru0);
	double wall0 = now_s();

	while (!quit) {
		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			switch (e.type) {
			case SDL_QUIT:
				quit = true;
				break;
			case SDL_DROPFILE: {         /* a ROM dropped on the window: check it and remember it */
				char derr[400];
				if (rom_verify(e.drop.file, derr, sizeof derr)) {
					options_set("rom", e.drop.file, OPT_INI, derr, sizeof derr);
					options_save(cfgpath);
					snprintf(msg, sizeof msg, "ROM REMEMBERED");
				} else {
					snprintf(msg, sizeof msg, "NOT THE RIGHT ROM");
				}
				msg_timer = 150;
				SDL_free(e.drop.file);
				break;
			}
			case SDL_KEYDOWN: {
				SDL_Keycode k = e.key.keysym.sym;
				if (!e.key.repeat) {
					if (k == SDLK_ESCAPE)
						quit = true;
					else if (k == SDLK_F11 || (k == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT))) {
						bool fs = SDL_GetWindowFlags(v.win) & SDL_WINDOW_FULLSCREEN_DESKTOP;
						SDL_SetWindowFullscreen(v.win, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
						break;
					} else if (k == SDLK_F1)
						overlay = !overlay;
					else if (k == SDLK_F5 || k == SDLK_F9) {
						char p[1200];
						snprintf(p, sizeof p, "%s/t2.slot%d.state", savedir, slot);
						bool ok = k == SDLK_F5 ? emu_state_save_file(p) : emu_state_load_file(p);
						snprintf(msg, sizeof msg, "%s SLOT %d%s", k == SDLK_F5 ? "SAVED" : "LOADED", slot, ok ? "" : " FAILED");
						msg_timer = 120;
						printf("%s %s%s\n", k == SDLK_F5 ? "save" : "load", p, ok ? "" : " FAILED");
						if (adev && k == SDLK_F9)
							SDL_ClearQueuedAudio(adev);
					} else if (k == SDLK_F4) {
						if (hifps_enabled()) {
							hifps_configure(0);
							snprintf(msg, sizeof msg, "60 HZ");
						} else {
							int hz = hf_toggle_hz;
							if (render_emu_mode() == RENDER_GPGX)
								render_emu_configure("c");
							hifps_configure(hz);
							snprintf(msg, sizeof msg, "%d HZ", hz);
						}
						hf_clock = false;
						msg_timer = 90;
					} else if (k == SDLK_F6 || k == SDLK_F7) {
						slot = (slot + (k == SDLK_F7 ? 1 : 9)) % 10;
						snprintf(msg, sizeof msg, "SLOT %d", slot);
						msg_timer = 90;
					} else if (k == SDLK_F12) {
						char p[1200];
						snprintf(p, sizeof p, "%s/shot-%06ld.ppm", a.outdir, emu_frame_count());
						emu_write_ppm(p);
						snprintf(msg, sizeof msg, "SCREENSHOT %06ld", emu_frame_count());
						msg_timer = 90;
						int gx, gy;
						bool off;
						unsigned gb;
						gun_state(&gx, &gy, &off, &gb);
						printf("wrote %s (gun %d,%d%s buttons %X)\n", p, gx, gy, off ? " off-screen" : "", gb);
					} else if (k == SDLK_p || k == SDLK_PAUSE)
						paused = !paused;
					else if (k == SDLK_n || k == SDLK_PERIOD) {
						paused = true;
						step = true;
					}
				}
				if (k == SDLK_TAB)
					ff = true;
				g_keys |= key_to_pad(k);
				break;
			}
			case SDL_KEYUP:
				if (e.key.keysym.sym == SDLK_TAB)
					ff = false;
				g_keys &= ~key_to_pad(e.key.keysym.sym);
				break;
			case SDL_MOUSEMOTION: {
				int mx, my;
				SDL_GetMouseState(&mx, &my);   /* window coordinates (events are rescaled) */
				mouse_from_window(&v, mx, my);
				break;
			}
			case SDL_MOUSEBUTTONDOWN:
			case SDL_MOUSEBUTTONUP: {
				int mx, my;
				SDL_GetMouseState(&mx, &my);
				mouse_from_window(&v, mx, my);
				bool down = e.type == SDL_MOUSEBUTTONDOWN;
				mouse_button(e.button.button == SDL_BUTTON_RIGHT ? MOUSE_RIGHT :
				           e.button.button == SDL_BUTTON_MIDDLE ? MOUSE_MIDDLE : MOUSE_LEFT, down);
				break;
			}
			case SDL_MOUSEWHEEL:
				mouse_wheel(e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y);
				break;
			case SDL_WINDOWEVENT:
				if (e.window.event == SDL_WINDOWEVENT_LEAVE) {
					mouse_leave();
					mouse_button(MOUSE_LEFT, false);
					mouse_button(MOUSE_RIGHT, false);
					mouse_button(MOUSE_MIDDLE, false);
					SDL_ShowCursor(SDL_ENABLE);
				}
				break;
			case SDL_CONTROLLERDEVICEADDED:
				SDL_GameControllerOpen(e.cdevice.which);
				break;
			case SDL_CONTROLLERBUTTONDOWN:
				if (e.cbutton.button == SDL_CONTROLLER_BUTTON_BACK)
					paused = !paused;
				g_padbtn |= button_to_pad(e.cbutton.button);
				break;
			case SDL_CONTROLLERBUTTONUP:
				g_padbtn &= ~button_to_pad(e.cbutton.button);
				break;
			case SDL_CONTROLLERAXISMOTION:
				if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX)
					g_axis_x = e.caxis.value;
				else if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY)
					g_axis_y = e.caxis.value;
				break;
			}
		}
		if (quit)
			break;

		bool scripted = false;
		bool ran = false;
		if (scr) {
			unsigned pad;
			if (script_step(scr, &pad)) {
				emu_set_pad(0, pad);
				scripted = true;
			} else {
				if (script_errors(scr))
					fprintf(stderr, "script finished with %d error(s)\n", script_errors(scr));
				script_close(scr);
				scr = NULL;
				if (batch && a.frames < 0)
					break;
				continue;
			}
		}
		if (!scripted) {
			if (batch && frames_after_script >= a.frames && a.frames >= 0)
				break;
			emu_set_pad(0, live_pad());
		}
		if (scripted || !paused || step) {
			emu_set_pad(0, emu_get_pad(0) | mouse_frame());
			if (hifps_enabled() && gun_enabled()) {
				int gx, gy;
				bool off;
				unsigned gb;
				gun_state(&gx, &gy, &off, &gb);
				hifps_note_mouse(gx, gy, off);
			}
			emu_run_frame();
			ran = true;
			step = false;
			total++;
			fps_frames++;
			if (!scripted)
				frames_after_script++;
			if (fh)
				emu_frame_hash(fh, emu_frame_count());
		}

		bool fast = a.no_throttle || ff || ((scripted || batch) && !a.realtime_script);
		/* audio: queue this frame's samples, and pace on the queue length */
		if (ran && adev && !fast) {
			size_t n;
			const int16_t *s = emu_audio(&n);
			if (volume < 100) {
				static int16_t vbuf[4096];
				size_t m = n * 2 < 4096 ? n * 2 : 4096;
				for (size_t i = 0; i < m; i++)
					vbuf[i] = (int16_t)(s[i] * volume / 100);
				SDL_QueueAudio(adev, vbuf, (Uint32)(m * 2));
			} else {
				SDL_QueueAudio(adev, s, (Uint32)(n * 4));
			}
		} else if (adev && fast) {
			SDL_ClearQueuedAudio(adev);
		}

		/* video (skip most frames when running unthrottled). With a high frame rate each logic frame
		 * shows n pictures: in-betweens at j/n of the way from the last frame to this one, the whole
		 * frame last (docs/hifps.md); the host waits for each picture's time, then flips. */
		bool hf_run = ran && hifps_enabled() && !fast && v.ren && hifps_ready();
		if (ran && hifps_enabled() && fast && !ff)
			hifps_eager();                /* batch: draw (and count) the in-betweens, present nothing */
		int npics = 1;
		if (hf_run) {
			npics = hifps_per_frame();
			double t = now_s();
			if (!hf_clock || t - hf_t0 > 4 * T) {
				hf_t0 = t;
				hf_clock = true;
			}
		}
		bool show = !fast || (total % 30) == 0 || paused || !drew_once;
		for (int j = 1; j <= npics; j++) {
			int w = 0, h = 0;
			bool got;
			double t_prep = hf_run ? now_s() : 0;
			if (hf_run) {
				hf_pointer(&v);
				unsigned pw = 0, ph = 0;
				int kind;
				got = picture_to_buf(&v, hifps_picture((double)j / npics, &pw, &ph, &kind), pw, ph);
				w = (int)pw;
				h = (int)ph;
			} else {
				got = v.ren && show && game_frame(&v, &w, &h);
			}
			if (!got)
				continue;
			if (overlay) {
				int gx, gy;
				bool off;
				unsigned gb;
				gun_state(&gx, &gy, &off, &gb);
				snprintf(ovl_text, sizeof ovl_text, "F %ld  %.1f FPS%s%s%s\nGUN %d,%d%s %c%c%c%c\nPAD %02X  AQ %u MS",
				         emu_frame_count(), fps_show, hifps_enabled() ? " HI" : "", paused ? "  PAUSED" : "", ff ? "  FF" : "",
				         gx, gy, off ? " OFF" : "", gb & GUN_A ? 'A' : '-', gb & GUN_B ? 'B' : '-',
				         gb & GUN_C ? 'C' : '-', gb & GUN_START ? 'S' : '-', emu_get_pad(0),
				         adev ? (unsigned)(SDL_GetQueuedAudioSize(adev) * 1000 / (rate * 4)) : 0);
				overlay_text_rgba(v.buf, w, h, 2, 2, 0xFF00FFFFu, ovl_text);
			}
			if (msg_timer > 0) {
				overlay_text_rgba(v.buf, w, h, 2, h - 8, 0xFFFFFFFFu, msg);
				if (j == npics)
					msg_timer--;
			}
			if (video_prepare(&v, w, h)) {
				if (hf_run) {
					hf_prep += now_s() - t_prep;
					wait_until(hf_t0 + j * T / npics);
					hf_pics++;
				}
				double t_flip = hf_run ? now_s() : 0;
				SDL_RenderPresent(v.ren);
				if (hf_run)
					hf_flip += now_s() - t_flip;
				drew_once = true;
				if (hf_run) {       /* the spacing of consecutive presents against T / n */
					double t = now_s();
					if (hf_last > 0 && t - hf_last < 2 * T) {
						double dev = (t - hf_last) - T / npics;
						hf_sp_n++;
						hf_sp_ok += dev > -0.001 && dev < 0.001;
						hf_sp_sum += dev;
						hf_sp_sq += dev * dev;
					}
					hf_last = t;
				}
			}
		}
		if (hf_run) {
			hf_t0 += T;
			hf_logic++;
			/* audio: keep its queue near the target by nudging the clock a 32nd of a frame */
			if (adev) {
				Uint32 q = SDL_GetQueuedAudioSize(adev), fb = (Uint32)(rate * 4 * T);
				if (q > q_target + fb)
					hf_t0 += T / 32;
				else if (q + fb < q_target)
					hf_t0 -= T / 32;
			}
		}

		/* throttle */
		if (hf_run) {
			next_tick = SDL_GetPerformanceCounter();      /* the picture clock above paces */
		} else if (!fast) {
			if (adev) {
				while (SDL_GetQueuedAudioSize(adev) > q_target)
					SDL_Delay(1);
			} else {
				next_tick += (Uint64)(frame_dt * perf_freq);
				Uint64 now = SDL_GetPerformanceCounter();
				if (next_tick > now)
					SDL_Delay((Uint32)((next_tick - now) * 1000 / perf_freq));
				else if (now - next_tick > perf_freq / 4)
					next_tick = now;
			}
		} else {
			next_tick = SDL_GetPerformanceCounter();
		}

		Uint64 now = SDL_GetPerformanceCounter();
		if (now - t_fps >= perf_freq) {
			fps_show = fps_frames * (double)perf_freq / (double)(now - t_fps);
			fps_frames = 0;
			t_fps = now;
			if (v.win) {
				char title[128];
				snprintf(title, sizeof title, "T2: The Arcade Game - %.1f fps%s", fps_show, paused ? " (paused)" : "");
				SDL_SetWindowTitle(v.win, title);
			}
		}
	}

	double secs = (double)(SDL_GetPerformanceCounter() - t_start) / (double)perf_freq;
	if (a.screenshot) {
		if (emu_write_ppm(a.screenshot))
			printf("wrote %s\n", a.screenshot);
		else
			fprintf(stderr, "cannot write %s\n", a.screenshot);
	}
	if (a.window_shot) {
		int w, h;
		if (v.ren && game_frame(&v, &w, &h))
			video_prepare(&v, w, h);
		if (window_shot(&v, a.window_shot))
			printf("wrote %s\n", a.window_shot);
		else
			fprintf(stderr, "cannot write window shot: %s\n", SDL_GetError());
	}
	if (a.dump_ram && !emu_write_ram(a.dump_ram)) {
		fprintf(stderr, "cannot write %s\n", a.dump_ram);
		rc = 1;
	}
	if (fh)
		fclose(fh);
	if (a.memdump && !emu_write_memdump(a.memdump)) {
		fprintf(stderr, "cannot write %s\n", a.memdump);
		rc = 1;
	}
	printf("%ld frames in %.2f s (%.1f fps)\n", total, secs, total / (secs > 0 ? secs : 1));
	if (hf_pics) {
		struct rusage ru1;
		getrusage(RUSAGE_SELF, &ru1);
		double cpu = (ru1.ru_utime.tv_sec - ru0.ru_utime.tv_sec) + (ru1.ru_utime.tv_usec - ru0.ru_utime.tv_usec) / 1e6 +
		             (ru1.ru_stime.tv_sec - ru0.ru_stime.tv_sec) + (ru1.ru_stime.tv_usec - ru0.ru_stime.tv_usec) / 1e6;
		double wall = now_s() - wall0;
		printf("hifps: %ld pictures presented in %.2f s (%.1f a second) for %ld paced logic frames (%.1f a second); cpu %.2f s = %.1f%% of one core\n",
		       hf_pics, wall, hf_pics / wall, hf_logic, hf_logic / wall, cpu, 100.0 * cpu / wall);
		printf("hifps: per picture: %.3f ms to prepare (draw, convert, overlay, upload, scale into the back buffer), %.3f ms in the flip\n",
		       hf_prep / hf_pics * 1000, hf_flip / hf_pics * 1000);
		if (hf_sp_n > 1) {
			double mean = hf_sp_sum / hf_sp_n, sd = sqrt(hf_sp_sq / hf_sp_n - mean * mean);
			printf("hifps: spacing of consecutive presents against T/n: %.1f%% within 1 ms, mean %+.3f ms, sd %.3f ms (%ld presents)\n",
			       100.0 * hf_sp_ok / hf_sp_n, mean * 1000, sd * 1000, hf_sp_n);
		}
	}
	if (adev)
		SDL_CloseAudioDevice(adev);
	SDL_Quit();
	emu_shutdown();
	return rc;
}
