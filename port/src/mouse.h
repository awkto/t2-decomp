/* Whole-game mouse control, host side (issue #10). SDL-free: main.c, web/main_web.c and script.c
 * feed it mouse events, and call mouse_frame() once before every emulated frame.
 *
 * In game the mouse is the Menacer (gun.c): position = aim, left = trigger, right = missile
 * (option mouse_right), middle = Start. The game reads only pad 1 on the title, main menu and
 * OPTIONS screens, so there the host works out which screen is up and turns clicks into short
 * pad-1 button presses (the same thing a player would press; nothing in the game is patched):
 *
 *   logo / story intro / PRESS START   any click = Start (the logo screens only take Start in a window of their
 *                        300 frames, so the press waits for it)
 *   main menu            moving over a row moves the cursor there (Up/Down presses until
 *                        menu_cursor matches), left click = go to the row and Start, wheel = Up/Down,
 *                        middle = Start on the current row
 *   OPTIONS              hover = select the row; left click / wheel up = value up (Right, then A on
 *                        the sound rows to play it), right click / wheel down = value down, click
 *                        on EXIT or middle = leave
 *   continue             left click = the gun's Start (also for a dead gun player in a 2P game)
 *
 * Which screen is up is read from the 68k: between frames the CPU sits in WaitVBlank ($0086E2),
 * and the return address on the stack says which loop called it (docs/gun.md, "Menus by mouse").
 *
 * mouse_frame() also applies the host-side X correction (option aim_correct): the game's X is
 * 2 x a table value and drifts by -2..+4 px, so the commanded X is looked up in a table that
 * makes the game's X land on the mouse (tools/py/aimtab.py measures it). */
#ifndef T2_MOUSE_H
#define T2_MOUSE_H
#include <stdbool.h>

#include "gun.h"

enum { SCR_GAME = 0, SCR_LOGO, SCR_TITLE, SCR_MENU, SCR_OPTIONS, SCR_CONTINUE, SCR_INTRO };

void mouse_set_enabled(bool on);        /* same as gun_set_enabled */
void mouse_set_aim_correct(bool on);    /* option aim_correct (default on) */
void mouse_host_reset(void);                 /* forget plans and the screen (after a state load / reset) */

void mouse_move(int gx, int gy);        /* game pixel under the mouse */
void mouse_move_frame(int fx, int fy, int margin);   /* pixel of the shown frame; margin = widescreen pixels per side */
bool mouse_in_margin(void);             /* the last mouse_move_frame was over a margin */
void mouse_leave(void);
void mouse_button(int button, bool down);   /* MOUSE_LEFT / MOUSE_RIGHT / MOUSE_MIDDLE */
void mouse_wheel(int dy);               /* dy > 0 = wheel up */

/* Once per emulated frame, before it runs: hands the aim and buttons to the core and returns the
 * pad-1 bits (PAD_*) to OR into the real pad. */
unsigned mouse_frame(void);

int mouse_screen(void);                 /* SCR_*, as of the last frame */
const char *mouse_screen_name(void);

/* the commanded X that makes the game's X land on x (identity when the correction is off) */
int mouse_aim_x(int x);

#endif
