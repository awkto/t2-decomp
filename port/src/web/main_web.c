/* The browser host (docs/web.md): the same Genesis Plus GX core and wrapper (emu.c, gun.c) as the
 * desktop t2port, built with Emscripten and SDL2 (Emscripten's port).
 *
 * What differs from the desktop host (src/main.c):
 * - No blocking loop. The browser calls web_frame() on every display refresh (requestAnimationFrame);
 *   the emulation keeps its own clock (an accumulator of real time, 59.92 Hz) and runs the frames
 *   that are due (usually 0 or 1, at most 4 to catch up; after a long stall it restarts the clock).
 * - The page (web/app.js) mounts IndexedDB at /t2 (IDBFS) before main() and puts the ROM in it (or
 *   in /tmp for a ROM served beside the page); main() gets the ROM path as argv[1]. The options ini
 *   and the savestates live in /t2; every write asks the page to sync.
 * - Mouse = Menacer as on the desktop: the pointer position (CSS pixels of the canvas) is mapped
 *   through the placement of the picture to a game pixel; off the picture the gun points away.
 *   Left = trigger, middle = Start, right = option mouse_right. A touch tap arrives from SDL as a
 *   mouse move + left click, so it aims and fires. A press is held for at least MIN_HOLD emulated
 *   frames so a click shorter than a frame is not lost.
 * - Audio is SDL2's Web Audio device; the page resumes the AudioContext on the first gesture. */
#include <SDL.h>
#include <emscripten.h>
#include <emscripten/html5.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "emu.h"
#include "gun.h"
#include "hle.h"
#include "mouse.h"
#include "options.h"
#include "overlay.h"
#include "render/hifps.h"
#include "render/render_emu.h"
#include "sha1.h"

#define WEB_DIR "/t2"                       /* IDBFS mount (web/app.js) */
#define WEB_INI WEB_DIR "/t2.ini"
#define WEB_SAVES WEB_DIR "/saves"
#define MIN_HOLD 4                          /* emulated frames a mouse press is held at least */

static void web_sync(void)
{
	EM_ASM({ if (Module.t2Sync) Module.t2Sync(); });
}

static void web_status(const char *s)
{
	EM_ASM({ if (Module.t2Status) Module.t2Status(UTF8ToString($0)); }, s);
}

/* is the file at `path` the right ROM (raw or .smd)? The page calls this before main(). 1 yes, 0 no */
EMSCRIPTEN_KEEPALIVE int web_rom_check(const char *path)
{
	char err[512], hex[41];
	size_t len;
	uint8_t *rom = emu_read_rom(path, &len, err, sizeof err);
	if (!rom) {
		fprintf(stderr, "%s\n", err);
		return 0;
	}
	sha1_hex(rom, len, hex);
	free(rom);
	if (strcmp(hex, T2_ROM_SHA1)) {
		fprintf(stderr, "%s is not the right ROM (sha1 %s)\n", path, hex);
		return 0;
	}
	return 1;
}

/* for the page and tests: emulated frames so far, a work RAM word, the gun state
 * (0 x, 1 y, 2 off screen, 3 button mask) */
EMSCRIPTEN_KEEPALIVE double web_frame_count(void) { return (double)emu_frame_count(); }
EMSCRIPTEN_KEEPALIVE int web_ram16(int addr) { return emu_ram16((uint32_t)addr); }
EMSCRIPTEN_KEEPALIVE int web_gun_state(int which)
{
	int x, y;
	bool off;
	unsigned b;
	gun_state(&x, &y, &off, &b);
	return which == 0 ? x : which == 1 ? y : which == 2 ? off : (int)b;
}

/* the screen the mouse logic sees (SCR_* of mouse.h): 0 game, 1 logo, 2 title, 3 menu, 4 options, 5 continue, 6 story intro */
EMSCRIPTEN_KEEPALIVE int web_screen(void) { return mouse_screen(); }

/* how many C replacements (HLE, docs/porting.md) are active */
EMSCRIPTEN_KEEPALIVE int web_hle_count(void) { return hle_count_active(); }

/* high frame rate (docs/hifps.md): "off", "on"/"auto" (interpolated pictures on every display refresh,
 * the logic stays at 59.92 Hz) or a rate in Hz. Needs the C renderer's scene, which it selects. */
static void web_apply_fps(const char *val)
{
	int hz = 0;
	if (!strcasecmp(val, "off") || !strcmp(val, "0") || !*val)
		hz = 0;
	else if (!strcasecmp(val, "on") || !strcasecmp(val, "auto"))
		hz = 120;
	else
		hz = atoi(val) >= 61 ? atoi(val) : 0;
	if (hz && render_emu_mode() == RENDER_GPGX)
		render_emu_configure("c");
	hifps_configure(hz);
}

/* the page's settings (right button): set an option, keep it in the ini */
EMSCRIPTEN_KEEPALIVE int web_set_option(const char *key, const char *val)
{
	char err[200];
	if (!options_set(key, val, OPT_INI, err, sizeof err))
		return 0;
	if (!strcmp(key, "aim_correct"))
		mouse_set_aim_correct(options_bool("aim_correct"));
	if (!strcmp(key, "mouse_right"))
		gun_set_right(options_str("mouse_right")[0]);
	if (!strcmp(key, "fps"))
		web_apply_fps(val);
	if (options_save(WEB_INI))
		web_sync();
	return 1;
}

/* ------------------------------------------------------------------ input (as main.c) */
static unsigned g_keys, g_padbtn;
static int g_axis_x, g_axis_y;

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
			if (k != SDLK_UNKNOWN && n < MAX_KEYS_PER_BUTTON)
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

/* a pad SDL has no controller mapping for: the browser's "standard" Gamepad layout */
static unsigned joybutton_to_pad(Uint8 b)
{
	switch (b) {
	case 12: return PAD_UP;
	case 13: return PAD_DOWN;
	case 14: return PAD_LEFT;
	case 15: return PAD_RIGHT;
	case 2: case 3: return PAD_A;
	case 0: return PAD_B;
	case 1: case 5: return PAD_C;
	case 9: return PAD_START;
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

/* mouse buttons: held while down, and for at least MIN_HOLD emulated frames */
static bool m_down[4];
static int m_hold[4];

static void web_mouse_button(int b, bool down)
{
	m_down[b] = down;
	if (down)
		m_hold[b] = MIN_HOLD;
	if (down || m_hold[b] <= 0)
		mouse_button(b, down);
}

static void mouse_frame_end(void)
{
	for (int b = MOUSE_LEFT; b <= MOUSE_MIDDLE; b++)
		if (m_hold[b] > 0 && --m_hold[b] == 0 && !m_down[b])
			mouse_button(b, false);
}

/* ------------------------------------------------------------------ video */
static struct {
	SDL_Window *win;
	SDL_Renderer *ren;
	SDL_Texture *tex;
	int tw, th;                 /* texture = game frame size */
	uint32_t buf[720 * 576];
} V;

/* keep the SDL window (= the canvas's CSS size; the drawing buffer is that x devicePixelRatio
 * with SDL_WINDOW_ALLOW_HIGHDPI) the size the page gave the canvas */
static void follow_canvas(void)
{
	double cw, ch;
	if (emscripten_get_element_css_size("#canvas", &cw, &ch) != EMSCRIPTEN_RESULT_SUCCESS || cw < 1 || ch < 1)
		return;
	int ww, wh;
	SDL_GetWindowSize(V.win, &ww, &wh);
	if (ww != (int)cw || wh != (int)ch)
		SDL_SetWindowSize(V.win, (int)cw, (int)ch);
}

/* where the picture goes in an ow x oh output: the largest whole-number scale that fits, centred
 * (as the desktop host); a canvas smaller than the picture gets the picture fitted to it */
static void place(int ow, int oh, int tw, int th, int *x0, int *y0, int *w, int *h)
{
	int s = ow / tw < oh / th ? ow / tw : oh / th;
	if (s >= 1) {
		*w = tw * s;
		*h = th * s;
	} else {
		double f = (double)ow / tw < (double)oh / th ? (double)ow / tw : (double)oh / th;
		*w = (int)(tw * f);
		*h = (int)(th * f);
		if (*w < 1) *w = 1;
		if (*h < 1) *h = 1;
	}
	*x0 = (ow - *w) / 2;
	*y0 = (oh - *h) / 2;
}

static void video_present(const uint32_t *px, int w, int h)
{
	if (!V.tex || V.tw != w || V.th != h) {
		if (V.tex)
			SDL_DestroyTexture(V.tex);
		V.tex = SDL_CreateTexture(V.ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, w, h);
		SDL_SetTextureScaleMode(V.tex, SDL_ScaleModeNearest);
		V.tw = w;
		V.th = h;
	}
	SDL_UpdateTexture(V.tex, NULL, px, w * 4);
	int ow, oh, x0, y0, dw, dh;
	SDL_GetRendererOutputSize(V.ren, &ow, &oh);
	place(ow, oh, w, h, &x0, &y0, &dw, &dh);
	SDL_Rect dst = { x0, y0, dw, dh };
	SDL_SetRenderDrawColor(V.ren, 0, 0, 0, 255);
	SDL_RenderClear(V.ren);
	SDL_RenderCopy(V.ren, V.tex, NULL, &dst);
	SDL_RenderPresent(V.ren);
}

/* window (CSS px) -> game pixel; 1 on the picture, 0 off it, -1 unknown */
static int window_to_game(int x, int y, int *gx, int *gy)
{
	int ww, wh, ow, oh, x0, y0, dw, dh;
	if (!V.tw)
		return -1;
	SDL_GetWindowSize(V.win, &ww, &wh);
	if (SDL_GetRendererOutputSize(V.ren, &ow, &oh) || ww <= 0 || wh <= 0)
		return -1;
	place(ow, oh, V.tw, V.th, &x0, &y0, &dw, &dh);
	int dx = x * ow / ww - x0, dy = y * oh / wh - y0;
	if (dx < 0 || dy < 0 || dx >= dw || dy >= dh)
		return 0;
	*gx = dx * V.tw / dw;
	*gy = dy * V.th / dh;
	return 1;
}

static void mouse_from_window(int x, int y)
{
	int gx, gy, on = window_to_game(x, y, &gx, &gy);
	if (on == 0)
		mouse_leave();
	else if (on > 0)
		mouse_move(gx, gy);
}

static uint32_t rgba_from_565(uint16_t p)
{
	uint32_t r = (uint32_t)(p >> 11) << 3, g = (uint32_t)((p >> 5) & 63) << 2, b = (uint32_t)(p & 31) << 3;
	r |= r >> 5;
	g |= g >> 6;
	b |= b >> 5;
	return r | g << 8 | b << 16 | 0xFF000000u;
}

static bool game_frame(int *w, int *h)
{
	unsigned gw, gh;
	size_t pitch;
	const uint16_t *px = emu_video(&gw, &gh, &pitch);
	if (!px || gw * gh > sizeof V.buf / sizeof V.buf[0])
		return false;
	for (unsigned i = 0; i < gw * gh; i++)
		V.buf[i] = rgba_from_565(px[i]);
	*w = (int)gw;
	*h = (int)gh;
	return true;
}

/* ------------------------------------------------------------------ state */
static struct {
	SDL_AudioDeviceID adev;
	int rate, volume, slot;
	double frame_ms, acc, last;
	bool paused, step, ff, overlay;
	char msg[128], ovl[256];
	int msg_timer;
	double t_fps;
	int n_logic, n_present;
	double fps_logic, fps_present;
} S;

static void set_msg(const char *m, int frames)
{
	snprintf(S.msg, sizeof S.msg, "%s", m);
	S.msg_timer = frames;
}

static void handle_key(SDL_Keycode k, bool repeat)
{
	if (!repeat) {
		char m[64];
		if (k == SDLK_F1)
			S.overlay = !S.overlay;
		else if (k == SDLK_F5 || k == SDLK_F9) {
			char p[96];
			snprintf(p, sizeof p, WEB_SAVES "/t2.slot%d.state", S.slot);
			bool ok = k == SDLK_F5 ? emu_state_save_file(p) : emu_state_load_file(p);
			snprintf(m, sizeof m, "%s SLOT %d%s", k == SDLK_F5 ? "SAVED" : "LOADED", S.slot, ok ? "" : " - FAILED");
			set_msg(m, 120);
			if (ok && k == SDLK_F5)
				web_sync();
			if (ok && k == SDLK_F9 && S.adev)
				SDL_ClearQueuedAudio(S.adev);
		} else if (k == SDLK_F6 || k == SDLK_F7) {
			S.slot = (S.slot + (k == SDLK_F7 ? 1 : 9)) % 10;
			snprintf(m, sizeof m, "SLOT %d", S.slot);
			set_msg(m, 90);
		} else if (k == SDLK_p || k == SDLK_PAUSE)
			S.paused = !S.paused;
		else if (k == SDLK_n || k == SDLK_PERIOD) {
			S.paused = true;
			S.step = true;
		}
	}
	if (k == SDLK_TAB)
		S.ff = true;
	g_keys |= key_to_pad(k);
}

static void poll_events(void)
{
	SDL_Event e;
	while (SDL_PollEvent(&e)) {
		switch (e.type) {
		case SDL_KEYDOWN:
			handle_key(e.key.keysym.sym, e.key.repeat);
			break;
		case SDL_KEYUP:
			if (e.key.keysym.sym == SDLK_TAB)
				S.ff = false;
			g_keys &= ~key_to_pad(e.key.keysym.sym);
			break;
		case SDL_MOUSEMOTION:
			mouse_from_window(e.motion.x, e.motion.y);
			break;
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP:
			mouse_from_window(e.button.x, e.button.y);
			web_mouse_button(e.button.button == SDL_BUTTON_RIGHT ? MOUSE_RIGHT :
			             e.button.button == SDL_BUTTON_MIDDLE ? MOUSE_MIDDLE : MOUSE_LEFT,
			             e.type == SDL_MOUSEBUTTONDOWN);
			break;
		case SDL_MOUSEWHEEL:
			mouse_wheel(e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y);
			break;
		case SDL_WINDOWEVENT:
			if (e.window.event == SDL_WINDOWEVENT_LEAVE) {
				mouse_leave();
				for (int b = MOUSE_LEFT; b <= MOUSE_MIDDLE; b++)
					web_mouse_button(b, false);
			}
			break;
		case SDL_JOYDEVICEADDED:
			if (SDL_IsGameController(e.jdevice.which))
				SDL_GameControllerOpen(e.jdevice.which);
			else
				SDL_JoystickOpen(e.jdevice.which);
			set_msg("GAMEPAD CONNECTED", 120);
			break;
		case SDL_CONTROLLERBUTTONDOWN:
			if (e.cbutton.button == SDL_CONTROLLER_BUTTON_BACK)
				S.paused = !S.paused;
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
		case SDL_JOYBUTTONDOWN:
		case SDL_JOYBUTTONUP:
			if (SDL_GameControllerFromInstanceID(e.jbutton.which))
				break;
			if (e.type == SDL_JOYBUTTONDOWN) {
				if (e.jbutton.button == 8)
					S.paused = !S.paused;
				g_padbtn |= joybutton_to_pad(e.jbutton.button);
			} else
				g_padbtn &= ~joybutton_to_pad(e.jbutton.button);
			break;
		case SDL_JOYAXISMOTION:
			if (SDL_GameControllerFromInstanceID(e.jaxis.which))
				break;
			if (e.jaxis.axis == 0)
				g_axis_x = e.jaxis.value;
			else if (e.jaxis.axis == 1)
				g_axis_y = e.jaxis.value;
			break;
		}
	}
}

static void queue_audio(void)
{
	size_t n;
	const int16_t *s = emu_audio(&n);
	if (!S.adev || !n)
		return;
	/* a suspended AudioContext (no gesture yet) does not drain: don't let the queue grow */
	if (SDL_GetQueuedAudioSize(S.adev) > (Uint32)(S.rate * 4 / 4))
		SDL_ClearQueuedAudio(S.adev);
	if (S.volume < 100) {
		static int16_t vbuf[4096];
		size_t m = n * 2 < 4096 ? n * 2 : 4096;
		for (size_t i = 0; i < m; i++)
			vbuf[i] = (int16_t)(s[i] * S.volume / 100);
		SDL_QueueAudio(S.adev, vbuf, (Uint32)(m * 2));
	} else
		SDL_QueueAudio(S.adev, s, (Uint32)(n * 4));
}

/* one emulated frame (59.92 Hz) */
static bool logic_frame(void)
{
	if (S.paused && !S.step)
		return false;
	emu_set_pad(0, live_pad());
	emu_set_pad(0, emu_get_pad(0) | mouse_frame());
	if (hifps_enabled() && gun_enabled()) {
		int gx, gy;
		bool off;
		unsigned gb;
		gun_state(&gx, &gy, &off, &gb);
		hifps_note_mouse(gx, gy, off);
	}
	emu_run_frame();
	mouse_frame_end();
	S.step = false;
	S.n_logic++;
	return true;
}

static void draw_overlays(int w, int h)
{
	if (S.overlay) {
		int gx, gy;
		bool off;
		unsigned gb;
		gun_state(&gx, &gy, &off, &gb);
		snprintf(S.ovl, sizeof S.ovl, "F %ld  %.1f LOGIC/S\nGUN %d,%d%s %c%c%c%c\nPAD %02X  AQ %u MS%s",
		         emu_frame_count(), S.fps_logic, gx, gy, off ? " OFF" : "", gb & GUN_A ? 'A' : '-',
		         gb & GUN_B ? 'B' : '-', gb & GUN_C ? 'C' : '-', gb & GUN_START ? 'S' : '-', emu_get_pad(0),
		         S.adev ? (unsigned)(SDL_GetQueuedAudioSize(S.adev) * 1000 / (S.rate * 4)) : 0,
		         S.paused ? "  PAUSED" : S.ff ? "  FF" : "");
		overlay_text_rgba(V.buf, w, h, 2, 2, 0xFF00FFFFu, S.ovl);
	}
	if (S.msg_timer > 0)
		overlay_text_rgba(V.buf, w, h, 2, h - 8, 0xFFFFFFFFu, S.msg);
}

/* the browser's frame callback (requestAnimationFrame) */
static void web_frame(void)
{
	double now = emscripten_get_now();
	follow_canvas();
	poll_events();

	double dt = now - S.last;
	S.last = now;
	if (dt < 0 || dt > 250)              /* a hidden tab, a stall: restart the clock, don't fast-forward */
		dt = S.frame_ms;
	S.acc += S.ff ? 4 * dt : dt;
	int ran = 0;
	while (S.acc >= S.frame_ms) {
		S.acc -= S.frame_ms;
		if (ran >= (S.ff ? 8 : 4)) {     /* cannot keep up: drop the backlog */
			S.acc = 0;
			break;
		}
		if (logic_frame()) {
			ran++;
			if (S.msg_timer > 0)
				S.msg_timer--;
			if (!S.ff)
				queue_audio();
		}
	}
	/* the Web Audio clock is not this clock: keep the queue near 60 ms (1/32 frame nudges) */
	if (S.adev && ran && !S.ff) {
		Uint32 q = SDL_GetQueuedAudioSize(S.adev), fb = (Uint32)(S.rate * 4 * S.frame_ms / 1000.0);
		Uint32 target = (Uint32)(S.rate * 4 * 0.060);
		if (q > target + 2 * fb)
			S.acc -= S.frame_ms / 32;
		else if (q + fb < target)
			S.acc += S.frame_ms / 32;
	}

	int w, h;
	if (hifps_enabled() && !S.paused && !S.ff && hifps_ready()) {
		/* high frame rate: every display refresh shows the scene at the fraction of the logic step that
		 * has gone by, the pointer at its position now (the whole frame is shown as the step ends) */
		double f = S.acc / S.frame_ms;
		int gx, gy;
		bool off;
		unsigned gb;
		gun_state(&gx, &gy, &off, &gb);
		hifps_mouse_now(gx, gy, off);
		unsigned pw, ph;
		int kind;
		const uint16_t *px = hifps_picture(f < 0 ? 0 : f > 1 ? 1 : f, &pw, &ph, &kind);
		if (px && pw * ph <= sizeof V.buf / sizeof V.buf[0]) {
			for (unsigned i = 0; i < pw * ph; i++)
				V.buf[i] = rgba_from_565(px[i]);
			draw_overlays((int)pw, (int)ph);
			video_present(V.buf, (int)pw, (int)ph);
			S.n_present++;
		}
	} else if ((ran || S.paused || !V.tex) && game_frame(&w, &h)) {
		draw_overlays(w, h);
		video_present(V.buf, w, h);
		S.n_present++;
	}

	if (now - S.t_fps >= 1000) {
		S.fps_logic = S.n_logic * 1000.0 / (now - S.t_fps);
		S.fps_present = S.n_present * 1000.0 / (now - S.t_fps);
		S.n_logic = S.n_present = 0;
		S.t_fps = now;
		char st[160];
		if (hifps_enabled())
			snprintf(st, sizeof st, "%.1f frames/s, %.0f shown/s%s", S.fps_logic, S.fps_present, S.paused ? ", paused" : "");
		else
			snprintf(st, sizeof st, "%.1f frames/s%s", S.fps_logic, S.paused ? ", paused" : "");
		web_status(st);
	}
}

/* argv[1]: the ROM's path in the page's file system; argv[2] (optional): the HLE set from the page's
 * ?hle= parameter, which overrides the ini's `hle` option for this run (default none); argv[3]
 * (optional): the ?fps= parameter (off, on, a rate), which overrides the ini's `fps` option */
int main(int argc, char **argv)
{
	const char *rom = argc > 1 ? argv[1] : WEB_DIR "/rom/baserom.md";
	char err[512];
	mkdir(WEB_DIR, 0777);
	mkdir(WEB_SAVES, 0777);

	options_init();
	struct stat st;
	bool had_ini = stat(WEB_INI, &st) == 0;
	options_load(WEB_INI);
	if (!had_ini && options_save(WEB_INI))
		web_sync();
	const bool mouse_on = options_bool("mouse");
	S.volume = options_int("volume");
	mouse_set_enabled(mouse_on);
	gun_set_right(options_str("mouse_right")[0]);
	mouse_set_aim_correct(options_bool("aim_correct"));
	emu_set_aim_fast(!strcmp(options_str("aim"), "fast"));    /* read once: a change needs a reload */

	const char *hle_spec = argc > 2 && argv[2][0] ? argv[2] : options_str("hle");
	if (!hle_configure(hle_spec)) {
		fprintf(stderr, "hle: unknown function in '%s', running with none\n", hle_spec);
		hle_configure("none");
	}

	if (!emu_init(rom, WEB_SAVES, mouse_on, err, sizeof err)) {
		fprintf(stderr, "%s\n", err);
		EM_ASM({ if (Module.t2Fatal) Module.t2Fatal(UTF8ToString($0)); }, err);
		return 1;
	}
	keys_configure();
	web_apply_fps(argc > 3 && argv[3][0] ? argv[3] : options_str("fps"));

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) {
		fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	V.win = SDL_CreateWindow("T2: The Arcade Game", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 960, 672,
	                         SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
	V.ren = V.win ? SDL_CreateRenderer(V.win, -1, SDL_RENDERER_ACCELERATED) : NULL;
	if (!V.ren && V.win)                     /* no WebGL: SDL's software renderer (canvas 2D) */
		V.ren = SDL_CreateRenderer(V.win, -1, SDL_RENDERER_SOFTWARE);
	if (!V.ren) {
		fprintf(stderr, "no renderer: %s\n", SDL_GetError());
		return 1;
	}
	follow_canvas();
	SDL_ShowCursor(SDL_DISABLE);             /* the game draws its own crosshair */

	S.rate = (int)(emu_sample_rate() + 0.5);
	if (!options_bool("mute")) {
		SDL_AudioSpec want = { 0 }, have;
		want.freq = S.rate;
		want.format = AUDIO_S16SYS;
		want.channels = 2;
		want.samples = 1024;
		S.adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
		if (S.adev)
			SDL_PauseAudioDevice(S.adev, 0);
		else
			fprintf(stderr, "audio unavailable: %s\n", SDL_GetError());
	}

	S.frame_ms = 1000.0 / emu_fps();
	S.last = S.t_fps = emscripten_get_now();
	EM_ASM({ if (Module.t2Started) Module.t2Started(); });
	emscripten_set_main_loop(web_frame, 0, 1);      /* 0: requestAnimationFrame; never returns */
	return 0;
}
