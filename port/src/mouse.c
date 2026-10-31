/* Whole-game mouse control: see mouse.h and docs/gun.md ("Menus by mouse"). */
#include "mouse.h"
#include "emu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aimtab.h"

/* work RAM (data/ram.txt) */
#define RAM_PAD1 0xFF8050           /* ReadPad1 result: b7 Start b6 A b5 C b4 B b3 R b2 L b1 D b0 U */
#define RAM_P1_STATE 0xFF8022
#define RAM_CONTROL_MODE 0xFF9DF5
#define RAM_MENU_CURSOR 0xFF9F60
#define RAM_VBLANK 0xFFA0EE

/* code addresses (data/symbols.txt): the loop that called WaitVBlank tells the screen */
#define WAITVBLANK_LO 0x0086E2
#define WAITVBLANK_HI 0x0086F6
static const struct { unsigned lo, hi; int scr; } screens[] = {
	{ 0x000E22, 0x000E60, SCR_LOGO },       /* WaitStartWindow (the three logo screens) */
	{ 0x0042CE, 0x004650, SCR_INTRO },      /* the story text before the title; Start (held) skips it */
	{ 0x001B72, 0x001BD4, SCR_TITLE },      /* PressStartLoop */
	{ 0x001164, 0x0011CE, SCR_MENU },       /* MainMenuInput */
	{ 0x0013A2, 0x0014A4, SCR_OPTIONS },    /* OptionsInput */
	{ 0x008DFE, 0x008E68, SCR_CONTINUE },   /* ContinueCountdown (1 player) */
};

#define MENU_ROWS 5                 /* both menus: 5 rows of 24 px, the first one's text at y = 44 */
#define ROW_Y0 32
#define ROW_H 24
#define OPT_EXIT_ROW 4
#define MENU_NO_MENACER_ROW 3       /* "2 JOYPAD" needs a pad on port 2: the game refuses it with a gun */

#define PRESS_FRAMES 2
#define GAP_FRAMES 2
#define LATCH_TIMEOUT 20

static const char *scr_names[] = { "game", "logo", "title", "menu", "options", "continue", "intro" };

typedef struct { unsigned pad; int minf; } step;

static struct {
	bool aim_correct;
	int screen, unknown;         /* unknown: frames since the 68k was last seen waiting for vblank */
	int mx, my;
	bool inside, down[4];
	int hover_row;               /* row under the mouse as of its last move (-1 none) */
	int target;                  /* menu / options row the cursor should go to (-1 none) */
	bool start_after;            /* press Start once the cursor is there (or at once if no cursor) */
	int start_tries;
	int screen_age;              /* frames on this screen */
	step q[12];
	int nq, cur_t;               /* queued steps, frames spent in the front one */
} M = { .aim_correct = true, .hover_row = -1, .target = -1 };

void mouse_set_enabled(bool on) { gun_set_enabled(on); }
void mouse_set_aim_correct(bool on) { M.aim_correct = on; }
int mouse_screen(void) { return M.screen; }
const char *mouse_screen_name(void) { return scr_names[M.screen]; }

void mouse_host_reset(void)
{
	M.screen = SCR_GAME;
	M.unknown = 0;
	M.nq = 0;
	M.cur_t = 0;
	M.target = -1;
	M.start_after = false;
	M.hover_row = -1;
	M.screen_age = 0;
}

int mouse_aim_x(int x)
{
	if (x < 0)
		x = 0;
	if (x > 319)
		x = 319;
	return M.aim_correct ? aim_cmd_x[x] : x;
}

/* ------------------------------------------------------------------ the pad queue */
static unsigned ram_pad_mask(unsigned pad)       /* PAD_* -> bits of $FF8050 */
{
	unsigned m = pad & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT | PAD_START);
	if (pad & PAD_A) m |= 0x40;
	if (pad & PAD_B) m |= 0x10;
	if (pad & PAD_C) m |= 0x20;
	return m;
}

static void push(unsigned pad, int frames)
{
	if (M.nq < (int)(sizeof M.q / sizeof M.q[0]) - 1) {
		M.q[M.nq++] = (step){ pad, frames };
		M.q[M.nq++] = (step){ 0, GAP_FRAMES };
	}
}

static void queue_clear(void)
{
	M.nq = 0;
	M.cur_t = 0;
}

/* The pad bits for this frame: the front step is held for at least its frames and until the
 * game has latched it (ReadPad1 stored it in $FF8050; a press must be seen, and so must the
 * release, or the game's edge detection would miss it). */
static unsigned queue_run(void)
{
	if (!M.nq)
		return 0;
	step *s = &M.q[0];
	unsigned m = ram_pad_mask(s->pad), seen = emu_ram16(RAM_PAD1);
	if (M.cur_t >= s->minf) {
		bool latched = s->pad ? (seen & m) == m : (seen & 0xFF) == 0;
		if (latched || M.cur_t >= s->minf + LATCH_TIMEOUT) {
			memmove(M.q, M.q + 1, (size_t)(--M.nq) * sizeof M.q[0]);
			M.cur_t = 0;
			if (!M.nq)
				return 0;
			s = &M.q[0];
		}
	}
	M.cur_t++;
	return s->pad;
}

/* ------------------------------------------------------------------ screens */
static int classify(void)
{
	unsigned pc = emu_cpu_pc(), sp = emu_cpu_sp() & 0xFFFFFF;
	if (pc < WAITVBLANK_LO || pc >= WAITVBLANK_HI || sp < 0xFF0000 || sp > 0xFFFFF0)
		return -1;
	unsigned ret = emu_ram32(sp + 2);       /* WaitVBlank pushes d0 first */
	for (unsigned i = 0; i < sizeof screens / sizeof screens[0]; i++)
		if (ret >= screens[i].lo && ret < screens[i].hi)
			return screens[i].scr;
	return SCR_GAME;
}

static int row_at(int y)
{
	if (y < ROW_Y0)
		return -1;
	int r = (y - ROW_Y0) / ROW_H;
	return r < MENU_ROWS ? r : -1;
}

static int cursor(void) { return emu_ram16(RAM_MENU_CURSOR); }

static void screen_changed(int scr)
{
	M.screen = scr;
	M.screen_age = 0;
	queue_clear();
	M.target = -1;
	M.start_after = false;
	M.start_tries = 0;
	M.hover_row = -1;
}

void mouse_move(int gx, int gy)
{
	gun_move(gx, gy);
	M.mx = gx;
	M.my = gy;
	M.inside = true;
	if (M.screen == SCR_MENU || M.screen == SCR_OPTIONS) {
		int r = row_at(gy);
		if (r != M.hover_row) {
			M.hover_row = r;
			if (r >= 0)
				M.target = r;
		}
	}
}

void mouse_leave(void)
{
	gun_leave();
	M.inside = false;
}

static void press_start(void)
{
	M.start_after = true;
	M.start_tries = 0;
}

void mouse_button(int button, bool down)
{
	gun_button(button, down);
	if (button < MOUSE_LEFT || button > MOUSE_MIDDLE)
		return;
	M.down[button] = down;
	if (!down)
		return;
	switch (M.screen) {
	case SCR_LOGO:
	case SCR_TITLE:
	case SCR_INTRO:
		press_start();
		break;
	case SCR_MENU: {
		int r = M.inside ? row_at(M.my) : -1;
		if (button == MOUSE_MIDDLE) {
			M.target = cursor();
			press_start();
		} else if (button == MOUSE_LEFT && r >= 0) {
			M.target = r;
			if (r != MENU_NO_MENACER_ROW)
				press_start();
		}
		break;
	}
	case SCR_OPTIONS: {
		int r = M.inside ? row_at(M.my) : -1;
		if (button == MOUSE_MIDDLE) {
			press_start();
		} else if (r >= 0) {
			M.target = r;
			if (r == OPT_EXIT_ROW) {
				if (button == MOUSE_LEFT)
					press_start();
			} else if (r == cursor()) {
				unsigned dir = button == MOUSE_LEFT ? PAD_RIGHT : PAD_LEFT;
				push(dir, PRESS_FRAMES);
				if (r >= 1)
					push(PAD_A, PRESS_FRAMES);       /* the sound rows play what is dialled in */
			}
		}
		break;
	}
	default:
		break;
	}
}

void mouse_wheel(int dy)
{
	if (!dy)
		return;
	if (M.screen == SCR_MENU) {
		int t = (M.target >= 0 ? M.target : cursor()) + (dy > 0 ? -1 : 1);
		M.target = t < 0 ? 0 : t >= MENU_ROWS ? MENU_ROWS - 1 : t;
	} else if (M.screen == SCR_OPTIONS && cursor() < OPT_EXIT_ROW) {
		push(dy > 0 ? PAD_RIGHT : PAD_LEFT, PRESS_FRAMES);
	}
}

/* what the menu logic wants this frame (nothing queued) */
static void plan(void)
{
	int c = cursor();
	switch (M.screen) {
	case SCR_LOGO: {
		unsigned vb = emu_ram16(RAM_VBLANK);
		/* WaitStartWindow takes a Start edge only while vblank_count is 180..299 */
		if (M.start_after && vb >= 178 && vb < 296) {
			push(PAD_START, 3);
			M.start_after = false;
		}
		break;
	}
	case SCR_TITLE:
	case SCR_INTRO:
		if (M.start_after && M.screen_age > 2) {
			push(PAD_START, 3);
			if (++M.start_tries >= 4)
				M.start_after = false;
		}
		break;
	case SCR_MENU:
	case SCR_OPTIONS:
		if (M.target >= 0 && c != M.target && c < MENU_ROWS) {
			push(M.target < c ? PAD_UP : PAD_DOWN, PRESS_FRAMES);
		} else if (M.target >= 0 && c == M.target) {
			M.target = -1;
		} else if (M.start_after && M.target < 0) {
			push(PAD_START, 3);
			if (++M.start_tries >= 3)     /* the game may refuse (e.g. 2 JOYPAD with a gun) */
				M.start_after = false;
		}
		break;
	default:
		break;
	}
}

unsigned mouse_frame(void)
{
	int scr = classify();
	if (scr < 0) {
		if (M.unknown < 1000)
			M.unknown++;
		if (M.unknown > 3 && M.screen != SCR_GAME)
			screen_changed(SCR_GAME);
	} else {
		M.unknown = 0;
		if (scr != M.screen)
			screen_changed(scr);
	}
	M.screen_age++;

	unsigned pad = 0;
	if (gun_enabled() && M.screen != SCR_GAME) {
		if (!M.nq)
			plan();
		pad = queue_run();
	}

	int gx, gy;
	bool off;
	unsigned b;
	gun_state(&gx, &gy, &off, &b);
	/* the gun's Start is the only way to continue; the left button is the trigger, which the
	 * continue screen ignores (and a dead gun player in a 2P game waits for Start too) */
	unsigned mode = emu_ram8(RAM_CONTROL_MODE);
	bool gun_p1 = !(mode & 1) && mode < 4 + 4;
	if (M.down[MOUSE_LEFT] && (M.screen == SCR_CONTINUE || (gun_p1 && emu_ram16(RAM_P1_STATE) == 7)))
		b |= GUN_START;
	if (gun_enabled())
		emu_set_gun(mouse_aim_x(gx), gy, off, b);
	return pad;
}
