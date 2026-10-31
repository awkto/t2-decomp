#include "script.h"
#include "emu.h"
#include "gun.h"

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
		if (s->frames_left > 0) {
			s->frames_left--;
			*pad = s->cur_pad;
			return true;
		}
		if (s->release_button) {
			gun_button(s->release_button, false);
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
			gun_leave();
		} else if (!strcmp(cmd, "mouse") && sscanf(line, "%*s %511s %511s", a1, a2) == 2) {
			gun_move(atoi(a1), atoi(a2));
		} else if (!strcmp(cmd, "click") && sscanf(line, "%*s %511s", a1) == 1) {
			int b = a1[0] == 'R' || a1[0] == 'r' ? MOUSE_RIGHT : a1[0] == 'M' || a1[0] == 'm' ? MOUSE_MIDDLE :
			        a1[0] == 'L' || a1[0] == 'l' ? MOUSE_LEFT : 0;
			n = 6;
			sscanf(line, "%*s %*s %ld", &n);
			if (!b || n < 1) {
				err(s, "click needs L, R or M and a frame count >= 1", line);
				continue;
			}
			gun_button(b, true);
			s->release_button = b;
			s->frames_left = n;
			s->cur_pad = s->held;
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
		gun_button(s->release_button, false);
	if (s->peek)
		fclose(s->peek);
	fclose(s->f);
	free(s);
}
