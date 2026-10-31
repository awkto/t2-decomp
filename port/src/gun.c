#include "gun.h"
#include "emu.h"

static bool g_on = true;
static int g_x = 160, g_y = 112;
static bool g_off = true;
static bool g_down[4];          /* indexed by MOUSE_* */
static unsigned g_right = GUN_B;

void gun_set_enabled(bool on) { g_on = on; }
bool gun_enabled(void) { return g_on; }

void gun_set_right(char which)
{
	g_right = which == 'A' ? GUN_A : which == 'C' ? GUN_C : GUN_B;
}

void gun_move(int gx, int gy)
{
	g_x = gx;
	g_y = gy;
	g_off = false;
}

void gun_leave(void) { g_off = true; }

void gun_button(int button, bool down)
{
	if (button >= MOUSE_LEFT && button <= MOUSE_MIDDLE)
		g_down[button] = down;
}

static unsigned buttons(void)
{
	unsigned b = 0;
	if (g_down[MOUSE_LEFT])
		b |= GUN_A;
	if (g_down[MOUSE_RIGHT])
		b |= g_right;
	if (g_down[MOUSE_MIDDLE])
		b |= GUN_START;
	return b;
}

void gun_state(int *gx, int *gy, bool *off, unsigned *b)
{
	*gx = g_x;
	*gy = g_y;
	*off = g_off;
	*b = buttons();
}

void gun_frame(void)
{
	if (g_on)
		emu_set_gun(g_x, g_y, g_off, buttons());
}

bool gun_map_window(int wx, int wy, int out_w, int out_h, int tex_w, int tex_h, int *gx, int *gy)
{
	if (tex_w <= 0 || tex_h <= 0)
		return false;
	int s = out_w / tex_w < out_h / tex_h ? out_w / tex_w : out_h / tex_h;
	if (s < 1)
		s = 1;
	int x0 = (out_w - tex_w * s) / 2, y0 = (out_h - tex_h * s) / 2;
	int dx = wx - x0, dy = wy - y0;
	if (dx < 0 || dy < 0)
		return false;
	*gx = dx / s;
	*gy = dy / s;
	return *gx < tex_w && *gy < tex_h;
}
