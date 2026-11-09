#include "script.h"
#include "emu.h"
#include "gun.h"
#include "mouse.h"
#include "render/render_wide.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct script {
	FILE *f;
	char outdir[1024];
	long frames_left;      /* of the current run/press/click */
	unsigned cur_pad;      /* pad for the current run/press */
	unsigned held;         /* `hold` buttons */
	int release_button;    /* mouse button to release when the current click ends (0: none) */
	int until_kind;        /* run: 1 = until RAM matches, 2 = until the mouse screen matches */
	uint32_t until_addr, until_val;
	int until_w, until_scr;
	int line;
	int errors;
	FILE *peek;            /* OUTDIR/peek.log (harness format) */
};

script *script_open(const char *path, const char *outdir)
{
	FILE *f = fopen(path, "r");
	if (!f) {
		perror(path);
		return NULL;
	}
	script *s = calloc(1, sizeof *s);
	s->f = f;
	snprintf(s->outdir, sizeof s->outdir, "%s", outdir ? outdir : ".");
	return s;
}

static void out_path(script *s, char *buf, size_t n, const char *name, const char *ext)
{
	if (strchr(name, '/'))
		snprintf(buf, n, "%s", name);
	else
		snprintf(buf, n, "%s/%s.%s", s->outdir, name, ext);
}

static void err(script *s, const char *msg, const char *line)
{
	fprintf(stderr, "script line %d: %s: %s", s->line, msg, line);
	s->errors++;
}

static FILE *peek_file(script *s)
{
	if (!s->peek) {
		char path[1600];
		snprintf(path, sizeof path, "%s/peek.log", s->outdir);
		s->peek = fopen(path, "a");
	}
	return s->peek;
}

bool script_step(script *s, unsigned *pad)
{
	for (;;) {
		if (s->frames_left > 0 && s->until_kind) {
			bool hit;
			if (s->until_kind == 2) {
				hit = mouse_screen() == s->until_scr;
			} else {
				uint32_t v = 0;
				for (int i = 0; i < s->until_w; i++)
					v = (v << 8) | emu_ram8(s->until_addr + (uint32_t)i);
				hit = v == s->until_val;
			}
			if (hit)
				s->frames_left = 0;
		}
		if (s->frames_left <= 0)
			s->until_kind = 0;
		if (s->frames_left > 0) {
			s->frames_left--;
			*pad = s->cur_pad;
			return true;
		}
		if (s->release_button) {
			mouse_button(s->release_button, false);
			s->release_button = 0;
		}
		char line[1024], cmd[64], a1[512], a2[512];
		if (!fgets(line, sizeof line, s->f))
			return false;
		s->line++;
		if (sscanf(line, "%63s", cmd) != 1 || cmd[0] == '#')
			continue;
		char path[1600];
		long n;
		if (!strcmp(cmd, "run") && sscanf(line, "%*s %ld", &n) == 1) {
			s->frames_left = n;
			s->cur_pad = s->held;
		} else if (!strcmp(cmd, "press") && sscanf(line, "%*s %511s %ld", a1, &n) == 2) {
			s->frames_left = n;
			s->cur_pad = emu_pad_from_letters(a1);
		} else if (!strcmp(cmd, "hold") && sscanf(line, "%*s %511s", a1) == 1) {
			s->held = strcmp(a1, "-") ? emu_pad_from_letters(a1) : 0;
		} else if (!strcmp(cmd, "mouse") && sscanf(line, "%*s %511s", a1) == 1 && !strcmp(a1, "leave")) {
			mouse_leave();
		} else if (!strcmp(cmd, "mouse") && sscanf(line, "%*s %511s %511s", a1, a2) == 2) {
			mouse_move(atoi(a1), atoi(a2));
		} else if (!strcmp(cmd, "fmouse") && sscanf(line, "%*s %511s %511s", a1, a2) == 2) {
			/* a pixel of the shown frame (the wide frame under widescreen), like the window's mouse */
			mouse_move_frame(atoi(a1), atoi(a2), render_wide_margin());
		} else if (!strcmp(cmd, "click") && sscanf(line, "%*s %511s", a1) == 1) {
			int b = a1[0] == 'R' || a1[0] == 'r' ? MOUSE_RIGHT : a1[0] == 'M' || a1[0] == 'm' ? MOUSE_MIDDLE :
			        a1[0] == 'L' || a1[0] == 'l' ? MOUSE_LEFT : 0;
			n = 6;
			sscanf(line, "%*s %*s %ld", &n);
			if (!b || n < 1) {
				err(s, "click needs L, R or M and a frame count >= 1", line);
				continue;
			}
			mouse_button(b, true);
			s->release_button = b;
			s->frames_left = n;
			s->cur_pad = s->held;
		} else if (!strcmp(cmd, "until") && sscanf(line, "%*s %511s %ld %511s %511s", a1, &n, a2, path) == 4) {
			/* until ADDR W VAL MAX: run (held pad) until the RAM value matches, at most MAX frames */
			s->until_kind = 1;
			s->until_addr = (uint32_t)strtoul(a1 + (a1[0] == '$'), NULL, 16);
			s->until_w = (int)n;
			s->until_val = (uint32_t)strtoul(a2 + (a2[0] == '$'), NULL, 16);
			s->frames_left = atol(path);
			s->cur_pad = s->held;
		} else if (!strcmp(cmd, "untilscreen") && sscanf(line, "%*s %511s %ld", a1, &n) == 2) {
			static const char *names[] = { "game", "logo", "title", "menu", "options", "continue", "intro" };
			s->until_scr = -1;
			for (int i = 0; i < 7; i++)
				if (!strcmp(a1, names[i]))
					s->until_scr = i;
			if (s->until_scr < 0) {
				err(s, "unknown screen", line);
				continue;
			}
			s->until_kind = 2;
			s->frames_left = n;
			s->cur_pad = s->held;
		} else if (!strcmp(cmd, "screen")) {
			FILE *p = peek_file(s);
			if (p) {
				fprintf(p, "%ld screen %s\n", emu_frame_count(), mouse_screen_name());
				fflush(p);
			}
		} else if (!strcmp(cmd, "crosshair") && sscanf(line, "%*s %511s", a1) == 1) {
			/* LABEL gun_x gun_y p1_x p1_y sprite_center_x sprite_center_y (-1 if no sprite) */
			int sx = -9, sy = -9;
			bool ok = emu_crosshair_sprite(&sx, &sy);
			FILE *p = peek_file(s);
			if (p) {
				fprintf(p, "%ld %s %u %u %u %u %d %d\n", emu_frame_count(), a1, emu_ram16(0xFF9F6E), emu_ram16(0xFF9F70),
				        emu_ram16(0xFF8004), emu_ram16(0xFF8006), ok ? sx + 8 : -1, ok ? sy + 8 : -1);
				fflush(p);
			}
		} else if (!strcmp(cmd, "wheel") && sscanf(line, "%*s %ld", &n) == 1) {
			mouse_wheel((int)n);
		} else if (!strcmp(cmd, "shot") && sscanf(line, "%*s %511s", a1) == 1) {
			out_path(s, path, sizeof path, a1, "ppm");
			if (!emu_write_ppm(path))
				err(s, "cannot write", path);
		} else if (!strcmp(cmd, "ram") && sscanf(line, "%*s %511s", a1) == 1) {
			out_path(s, path, sizeof path, a1, "ram");
			if (!emu_write_ram(path))
				err(s, "cannot write", path);
		} else if (!strcmp(cmd, "save") && sscanf(line, "%*s %511s", a1) == 1) {
			out_path(s, path, sizeof path, a1, "state");
			if (!emu_state_save_file(path))
				err(s, "cannot save", path);
		} else if (!strcmp(cmd, "load") && sscanf(line, "%*s %511s", a1) == 1) {
			out_path(s, path, sizeof path, a1, "state");
			if (!emu_state_load_file(path))
				err(s, "cannot load", path);
		} else if (!strcmp(cmd, "poke") && sscanf(line, "%*s %511s %ld %511s", a1, &n, a2) == 3) {
			uint32_t addr = (uint32_t)strtoul(a1 + (a1[0] == '$'), NULL, 16);
			uint32_t val = (uint32_t)strtoul(a2 + (a2[0] == '$'), NULL, 16);
			extern uint8_t work_ram[0x10000];
			for (long i = 0; i < n; i++)
				work_ram[((addr + (uint32_t)i) & 0xFFFF) ^ 1] = (uint8_t)(val >> (8 * (n - 1 - i)));
		} else if (!strcmp(cmd, "peek") && sscanf(line, "%*s %511s %511s", a1, a2) == 2) {
			int w = 2;
			sscanf(line, "%*s %*s %*s %d", &w);
			uint32_t a = (uint32_t)strtoul(a2 + (a2[0] == '$'), NULL, 16), v = 0;
			for (int i = 0; i < w && i < 4; i++)
				v = (v << 8) | emu_ram8(a + (uint32_t)i);
			FILE *p = peek_file(s);
			if (p) {
				fprintf(p, "%ld %s %0*x\n", emu_frame_count(), a1, w * 2, v);
				fflush(p);
			}
		} else if (!strcmp(cmd, "cpu") && sscanf(line, "%*s %511s", a1) == 1) {
			uint32_t sp = emu_cpu_sp();
			FILE *p = peek_file(s);
			if (p) {
				fprintf(p, "%ld %s pc=%06x sp=%06x ret=%06x\n", emu_frame_count(), a1, emu_cpu_pc(), sp, emu_ram32(sp + 2));
				fflush(p);
			}
		} else if (!strcmp(cmd, "echo")) {
			const char *t = strlen(line) > 5 ? line + 5 : "\n";
			FILE *p = peek_file(s);
			if (p) {
				fprintf(p, "%ld %s", emu_frame_count(), t);
				fflush(p);
			}
			fputs(t, stderr);
		} else if (!strcmp(cmd, "trace") || !strcmp(cmd, "untrace")) {
			/* harness-only bus trace: accepted and ignored so harness scripts run unchanged */
		} else {
			err(s, "bad command", line);
		}
	}
}

int script_errors(const script *s) { return s->errors; }

void script_close(script *s)
{
	if (!s)
		return;
	if (s->release_button)
		mouse_button(s->release_button, false);
	if (s->peek)
		fclose(s->peek);
	fclose(s->f);
	free(s);
}
