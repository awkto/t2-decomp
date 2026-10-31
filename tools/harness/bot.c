/* Gun-bot for T2: The Arcade Game (included by harness.c, one translation unit).
 *
 * Plays the game from RAM: every frame it walks the object table, picks a target, aims the
 * Menacer (or a pad crosshair) at it and fires. See the harness header for the script commands
 * (bot, botcfg, objs) and docs/levels.md for results. Everything here is read from 68k work RAM or
 * learned through the cpu hook; nothing in the game is changed unless a cheat option is set.
 *
 * Game facts used (docs/gun.md, data/ram.txt):
 *   object table $FF87B2, 79 slots of $2C bytes: +0 word flags (b15 shootable, low byte = type),
 *     +2 X, +4 Y (screen px), +7 byte depth (ShotHitTest damages the lowest depth hit),
 *     +8 hit points, +$1C hit-test handler, +$20 AI routine.
 *   player structs $FF8000 / $FF8050: +4 X, +6 Y (crosshair), +$18 missiles (BCD),
 *     +$1C gunpower, +$1E energy, +$22 state, +$24 index ($FFFF = not playing / dead),
 *     +$38 human casualties.
 *   level $FF80CC ($0A = high-score entry), credits $FF80CA, control mode $FF9DF5.
 *
 * Learned through the cpu hook (no static tables needed):
 *   hit boxes: HitBox ($005940) reads the object at (2,a6) (pc $005948) and the box table
 *     at (a3) (pc $00594E): box = [x - b0, x - b0 + b2] x [y - b1, y - b1 + b3].
 *   danger: an object whose AI routine (called by the object loop at $00B3B2, which reads
 *     (+$20,a6)) reaches the energy-loss routine $0090B2 marks its type dangerous.
 *   threats: an AI routine that writes the hit points of the protected object (pointer at $FF9ECA, while it is the truck or van:
 *     John Connor's truck on level 3, ...) marks its type as a threat (top priority).
 *   humans: the hit-test handlers $005F04, $0065FC and $006652 end in HUD message 6
 *     ("HUMAN CASUALTY"); types $13 (soldiers) is one of them. A casualty counter increase right
 *     after a hit (ShotHitTest reads the hit object at pc $005918) adds that handler too.
 */
#define OBJ_TABLE 0xFF87B2
#define OBJ_STRIDE 0x2C
#define OBJ_SLOTS 79
#define AUX_TABLE 0xFF9AB2

static void objs_dump(const char *name)
{
	char path[1024];
	snprintf(path, sizeof path, "%s/%s.obj", outdir, name);
	FILE *f = fopen(path, "a");
	if (!f) return;
	fprintf(f, "# frame %ld level %u live %u\n", frame, rd_ram(0xFF80CC, 2), rd_ram(0xFF80DA, 2));
	for (int i = 0; i < OBJ_SLOTS; i++) {
		unsigned a = OBJ_TABLE + i * OBJ_STRIDE;
		if (!rd_ram(a, 2)) continue;
		fprintf(f, "%2d", i);
		for (int k = 0; k < OBJ_STRIDE; k += 2) fprintf(f, " %04x", rd_ram(a + k, 2));
		fputc('\n', f);
	}
	fprintf(f, "# aux table FF9AB2 live %u\n", rd_ram(0xFF80E0, 2));
	for (int i = 0; i < 32; i++) {
		unsigned a = AUX_TABLE + i * 0x1A;
		if (!rd_ram(a, 2)) continue;
		fprintf(f, "a%2d", i);
		for (int k = 0; k < 0x1A; k += 2) fprintf(f, " %04x", rd_ram(a + k, 2));
		fputc('\n', f);
	}
	fclose(f);
}

/* ---- state learned by the cpu hook ---- */
static struct { long frame; int n; unsigned box[2]; } bot_hb[OBJ_SLOTS];
static unsigned bot_hb_obj;                 /* object being box-tested */
static int bot_ai_slot = -1;                /* object whose AI routine is running */
static int bot_hit_slot = -1;
/* gun hits per slot and the hit points at the first one: an object that takes many hits without
   losing hit points is armoured against the gun (truck, T-1000) and gets missiles instead */
static struct { int n, hp0; unsigned handler; } bot_hits[OBJ_SLOTS];               /* last object ShotHitTest damaged */
static long bot_hit_frame;
static unsigned bot_danger[256];            /* energy-loss events per object type */
static unsigned bot_human_h[64] = { 0x5F04, 0x65FC, 0x6652 };
static int bot_nhuman = 3;
static long bot_dmg_events;
static unsigned bot_threat[256];            /* hit-point writes to the protected object, per type */
static unsigned bot_prot;                   /* protected object ($FF9ECA: John Connor's truck etc.), 0 = none */

static void bot_hook(int type, unsigned addr)
{
	if (type == HOOK_M68K_R) {
		if (cur_pc == 0x005948 && addr >= OBJ_TABLE + 2 && addr < OBJ_TABLE + OBJ_SLOTS * OBJ_STRIDE)
			bot_hb_obj = addr - 2;
		else if (cur_pc == 0x00594E && bot_hb_obj) {
			int s = (bot_hb_obj - OBJ_TABLE) / OBJ_STRIDE;
			if (bot_hb[s].frame != frame) { bot_hb[s].frame = frame; bot_hb[s].n = 0; }
			if (bot_hb[s].n < 2 && (bot_hb[s].n == 0 || bot_hb[s].box[0] != addr))
				bot_hb[s].box[bot_hb[s].n++] = addr;
			bot_hb_obj = 0;
		} else if (cur_pc == 0x00B3B2 && addr >= OBJ_TABLE + 0x20 && addr < OBJ_TABLE + OBJ_SLOTS * OBJ_STRIDE)
			bot_ai_slot = (addr - 0x20 - OBJ_TABLE) / OBJ_STRIDE;
		else if (cur_pc == 0x005918 && addr >= OBJ_TABLE && addr < OBJ_TABLE + OBJ_SLOTS * OBJ_STRIDE) {
			bot_hit_slot = (addr - OBJ_TABLE) / OBJ_STRIDE;
			bot_hits[bot_hit_slot].n++;
			bot_hit_frame = frame;
		}
	} else if (type == HOOK_M68K_W) {
		if (bot_prot && addr == bot_prot + 8 && bot_ai_slot >= 0 && OBJ_TABLE + bot_ai_slot * OBJ_STRIDE != bot_prot)
			bot_threat[rd_ram(OBJ_TABLE + bot_ai_slot * OBJ_STRIDE, 2) & 0xFF]++;
	} else if (type == HOOK_M68K_E) {
		if (addr == 0x00B3C6 || addr == 0x00B398)
			bot_ai_slot = -1;
		else if (addr == 0x0090B2 && bot_ai_slot >= 0) {
			unsigned w = rd_ram(OBJ_TABLE + bot_ai_slot * OBJ_STRIDE, 2);
			bot_danger[w & 0xFF]++;
			bot_dmg_events++;
		}
	}
}

/* ---- configuration (botcfg KEY VALUE) ---- */
enum { DEV_OFF, DEV_GUN, DEV_PAD1, DEV_PAD2 };
static struct {
	int dev[2];          /* device driving P1 / P2 */
	int cheat_energy;    /* refill energy every frame (marks the run as cheated) */
	int cheat_skip;      /* frames after which a level is skipped with the level-skip cheat (0 = never) */
	int missiles;        /* use missiles */
	int missile_hp;      /* fire a missile at targets with at least this many hit points */
	int missile_crowd;   /* ... or when at least this many targets are on screen */
	int missile_keep;    /* missiles kept in reserve for targets above missile_hp */
	int continues;       /* press Start to continue */
	int hiscore;         /* enter initials (shoot ENTER) on the high-score screen */
	int verbose;
	int protect;         /* CHEAT: escort levels 3 and 7 cannot fail (see bot_frame) */
	int freeze;          /* CHEAT: steel mill, set the T-1000 frozen flag $FF9EC2 (he shatters by himself) */
	int focus;           /* frames per phase: alternate threat-first and big-target-first targeting (0 = off) */
	char saveprefix[64]; /* save outdir/PREFIX<level>.state at each level change ("" = off) */
} bcfg = { { DEV_GUN, DEV_OFF }, 0, 0, 1, 40, 6, 5, 1, 1, 0, 0, 0, 300, "" };
static FILE *botf;
static int bot_cheated;

static FILE *botlog(void) { return open_log(&botf, "bot.log"); }

static int bcd2(unsigned v) { return ((v >> 12) & 15) * 1000 + ((v >> 8) & 15) * 100 + ((v >> 4) & 15) * 10 + (v & 15); }

static int bot_is_human(unsigned handler)
{
	for (int i = 0; i < bot_nhuman; i++)
		if (bot_human_h[i] == handler) return 1;
	return 0;
}

typedef struct { int x0, y0, x1, y1; } rect;
static int in_rect(const rect *r, int x, int y) { return x >= r->x0 && x <= r->x1 && y >= r->y0 && y <= r->y1; }

struct bobj { int slot, type, x, y, hp, depth, shoot, human, nbox, learned, armoured; unsigned handler; rect box[2]; };
static struct bobj bo[OBJ_SLOTS];
static int nbo;

static void bot_scan(void)
{
	nbo = 0;
	for (int i = 0; i < OBJ_SLOTS; i++) {
		unsigned a = OBJ_TABLE + i * OBJ_STRIDE, w = rd_ram(a, 2);
		if (!w) continue;
		struct bobj *o = &bo[nbo++];
		o->slot = i; o->type = w & 0xFF; o->shoot = (w >> 15) & 1;
		o->x = (int16_t)rd_ram(a + 2, 2); o->y = (int16_t)rd_ram(a + 4, 2);
		o->hp = (int16_t)rd_ram(a + 8, 2);
		o->depth = rd_ram(a + 7, 1);
		o->handler = rd_ram(a + 0x1C, 4) & 0xFFFFFF;
		o->human = bot_is_human(o->handler);
		/* a hit-test handler that is a bare rts ($00597A) cannot be hit */
		if (o->handler + 2 <= (unsigned)rom_size && romb[o->handler] == 0x4E && romb[o->handler + 1] == 0x75)
			o->shoot = 0;
		o->nbox = 0;
		if (bot_hb[i].frame >= frame - 8 && bot_hb[i].n) {
			for (int k = 0; k < bot_hb[i].n; k++) {
				unsigned b = bot_hb[i].box[k];
				if (b + 8 > (unsigned)rom_size) continue;
				int bx = (int16_t)(romb[b] << 8 | romb[b + 1]), by = (int16_t)(romb[b + 2] << 8 | romb[b + 3]);
				int bw = (int16_t)(romb[b + 4] << 8 | romb[b + 5]), bh = (int16_t)(romb[b + 6] << 8 | romb[b + 7]);
				rect r = { o->x - bx, o->y - by, o->x - bx + bw, o->y - by + bh };
				o->box[o->nbox++] = r;
			}
		}
		o->learned = o->nbox > 0;
		if (bot_hits[i].handler != o->handler) {   /* a new object in this slot */
			bot_hits[i].handler = o->handler; bot_hits[i].n = 0;
		}
		if (bot_hits[i].n == 0) bot_hits[i].hp0 = o->hp;
		o->armoured = bot_hits[i].n >= 20 && o->hp >= bot_hits[i].hp0;
		if (!o->nbox) {     /* not box-tested lately: assume a 24x40 box standing on (x, y) */
			rect r = { o->x - 12, o->y - 40, o->x + 12, o->y };
			o->box[0] = r; o->nbox = 1;
		}
	}
}

static int bot_point_human(int x, int y)
{
	for (int i = 0; i < nbo; i++)
		if (bo[i].human && bo[i].shoot)
			for (int k = 0; k < bo[i].nbox; k++) {
				rect r = bo[i].box[k];
				r.x0 -= 3; r.y0 -= 3; r.x1 += 3; r.y1 += 3;   /* margin: aim error up to 4 px */
				if (in_rect(&r, x, y)) return 1;
			}
	return 0;
}

/* the object a shot at (x, y) would damage, as ShotHitTest decides: of all objects whose box
   contains the point, the lowest depth byte (+7) wins, ties go to the lower slot. Objects
   without a learned box only count if they are the candidate itself. -1 = none */
static int bot_front_at(int x, int y, int self)
{
	int best = -1;
	for (int i = 0; i < nbo; i++) {
		if (!bo[i].learned && i != self) continue;
		int in = 0;
		for (int k = 0; k < bo[i].nbox; k++)
			if (in_rect(&bo[i].box[k], x, y)) in = 1;
		if (in && (best < 0 || bo[i].depth < bo[best].depth)) best = i;
	}
	return best;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* a safe aim point inside the target's first box, or 0 */
static int bot_aim_point(const struct bobj *o, int *ax, int *ay)
{
	int self = (int)(o - bo);
	const rect *r = &o->box[0];
	static const int fx[9] = { 2, 1, 3, 2, 2, 1, 3, 1, 3 }, fy[9] = { 2, 2, 2, 1, 3, 1, 1, 3, 3 };
	for (int k = 0; k < 9; k++) {
		int x = r->x0 + (r->x1 - r->x0) * fx[k] / 4, y = r->y0 + (r->y1 - r->y0) * fy[k] / 4;
		x = clampi(x, 4, 316); y = clampi(y, 24, 200);
		if (!in_rect(r, x, y)) continue;
		if (bot_point_human(x, y)) continue;
		if (bot_front_at(x, y, self) != self) continue;   /* hidden behind another object */
		*ax = x; *ay = y;
		return 1;
	}
	return 0;
}

struct bplayer {
	int target_slot;      /* sticky target */
	long missile_wait;    /* frame before which no missile is fired */
	int missile_phase;    /* frames of B held */
	unsigned last_cas;    /* humans counter (+$38) seen last frame */
	int last_index, dead;
	long shots;
};
static struct bplayer bp_[2] = { { .target_slot = -1 }, { .target_slot = -1 } };
static unsigned bot_last_level = 0xFFFF;
static long bot_level_start;

/* output for one player's device: aim point, trigger, missile button, Start */
struct bout { int x, y, fire, missile, start; };

static void bot_player(int p, struct bout *out)
{
	unsigned P = 0xFF8000 + 0x50 * p;
	struct bplayer *b = &bp_[p];
	int px = (int16_t)rd_ram(P + 4, 2), py = (int16_t)rd_ram(P + 6, 2);
	unsigned index = rd_ram(P + 0x24, 2), level = rd_ram(0xFF80CC, 2);
	memset(out, 0, sizeof *out);
	out->x = px; out->y = py;

	/* casualty bookkeeping: a hit just before the counter rose identifies a human handler */
	unsigned cas = rd_ram(P + 0x38, 2);
	if (cas != b->last_cas) {
		if (cas > b->last_cas && bot_hit_slot >= 0 && frame - bot_hit_frame < 4) {
			unsigned h = rd_ram(OBJ_TABLE + bot_hit_slot * OBJ_STRIDE + 0x1C, 4) & 0xFFFFFF;
			fprintf(botlog(), "%ld P%d casualty #%u: hit slot %d type %02x handler %06x%s\n", frame, p + 1, cas,
			        bot_hit_slot, rd_ram(OBJ_TABLE + bot_hit_slot * OBJ_STRIDE, 2) & 0xFF, h,
			        bot_is_human(h) ? " (known human)" : " (new human handler)");
			if (!bot_is_human(h) && bot_nhuman < 64) bot_human_h[bot_nhuman++] = h;
		}
		b->last_cas = cas;
	}

	if (index == 0xFFFF) {      /* dead / waiting to continue, level intro, or not joined */
		if (!b->dead && rd_ram(P + 0x22, 2) != 0) {   /* state 2/3 dying, 7 waiting to continue */
			b->dead = 1;
			fprintf(botlog(), "%ld P%d terminated (level %u, credits %u)\n", frame, p + 1, level, rd_ram(0xFF80CA, 1));
		}
		b->last_index = index;
		if (bcfg.continues && b->dead && level && level != 0x0A && (frame & 15) < 3) out->start = 1;
		return;
	}
	b->dead = 0;
	b->last_index = index;
	if (bcfg.cheat_energy) {
		uint8_t *wram = p_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
		wram[((P + 0x1E) & 0xFFFF) ^ 1] = 0xFF; wram[(((P + 0x1E) & 0xFFFF) + 1) ^ 1] = 0xFF;
		bot_cheated = 1;
	}

	/* Steel mill (level 8) tactic: the T-1000 ($FF9ED6, hit handler $006C2E) only dies frozen. Shots
	   knock him down; a shot tanker panel (handler $006EF8) leaks nitrogen, and standing within 10 px of a
	   leak cools him ($FF9F60 -= $3C per frame, +$C when not) until he freezes and shatters by himself.
	   So: shoot the tanker just above him and never him. Not enough yet: from warm he must spend about
	   1400 frames next to one leak, and a leak lasts $80 frames. */
	{
		unsigned tp = rd_ram(0xFF9ED6, 4) & 0xFFFFFF;
		int tank = -1, tman = -1;
		for (int i = 0; i < nbo; i++) {
			if (bo[i].handler == 0x006EF8) tank = i;
			if ((unsigned)(OBJ_TABLE + bo[i].slot * OBJ_STRIDE) == tp && bo[i].handler == 0x006C2E) tman = i;
		}
		static long frozen_at = -1;
		if (tank >= 0 && tman >= 0 && bcfg.freeze && frozen_at < bot_level_start && frame - bot_level_start > 1500) {
			frozen_at = frame;
			uint8_t *wram = p_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
			wram[0x9EC2 ^ 1] = 0xFF;   /* t1000_frozen: what the cold check at $011CC8 sets */
			bot_cheated = 1;
		}
		if (tank >= 0 && tman >= 0) {
			/* panels are 16x16 cells from Y $50, 20 per row, $58 hit points each ($FFAE84 words): stay on
			   the bottom-row panel of his column until it leaks, then follow him to the next column */
			static int col = -1;
			int hcol = clampi(bo[tman].x, 8, 312) >> 4;
			if (col < 0 || !rd_ram(0xFFAE84 + 2 * (3 * 20 + col), 2)) col = hcol;
			int ax = col * 16 + 8, ay = 0x88;
			out->x = ax; out->y = ay;
			int on_him = abs(px - bo[tman].x) < 14 && py > bo[tman].y - 72;
			if (abs(px - ax) < 6 && abs(py - ay) < 6 && !on_him && (frame & 1)) {
				out->fire = 1; b->shots++;
			}
			return;
		}
	}

	/* pick a target */
	int best = -1, bx = 0, by = 0, ntarget = 0;
	long bestscore = -1000000;
	for (int i = 0; i < nbo; i++) {
		struct bobj *o = &bo[i];
		if (!o->shoot || o->human) continue;
		if ((unsigned)(OBJ_TABLE + o->slot * OBJ_STRIDE) == bot_prot) continue;   /* never the escorted object */
		int ax, ay;
		if (!bot_aim_point(o, &ax, &ay)) continue;
		if (ax < 0 || ax > 319) continue;
		if (o->type != 0x14 && o->type != 0x0F) ntarget++;
		long d = abs(ax - px) + abs(ay - py);
		long score = -d;
		int focus = bcfg.focus && (frame / bcfg.focus) % 2;   /* boss phase: big targets first */
		if (focus) score += (o->hp > 250 ? 250 : o->hp < 0 ? 0 : o->hp) * 4;
		else if (bot_danger[o->type]) score += 400 + (bot_danger[o->type] > 50 ? 50 : bot_danger[o->type]) * 4;
		if (bot_threat[o->type]) score += 800;
		if (o->armoured) score -= 300;               /* gun does nothing: missiles only */
		if (o->hp < 0) score -= 200;                 /* spent pickups; some bosses run below 0 too */
		if (bot_prot) {                              /* escort levels: enemies near the protected object first */
			int qx = (int16_t)rd_ram(bot_prot + 2, 2), qy = (int16_t)rd_ram(bot_prot + 4, 2);
			int dq = abs(o->x - qx) + abs(o->y - qy);
			if ((unsigned)(OBJ_TABLE + o->slot * OBJ_STRIDE) != bot_prot && dq < 400) score += 400 - dq;
		}
		if (o->slot == b->target_slot) score += 250;
		if (o->type == 0x14) score -= 300;          /* level-1 wall segments: harmless */
		if (score > bestscore) { bestscore = score; best = i; bx = ax; by = ay; }
	}
	if (best < 0) {
		b->target_slot = -1;
		return;
	}
	struct bobj *t = &bo[best];
	if (bcfg.verbose > 1 && (t->slot != b->target_slot || frame % 30 == 0)) {
		int f = bot_front_at(px, py, best);
		fprintf(botlog(), "%ld P%d target slot %d type %02x hp %d at (%d,%d) box %d,%d-%d,%d%s; crosshair (%d,%d) front %d\n",
		        frame, p + 1, t->slot, t->type, t->hp, bx, by, t->box[0].x0, t->box[0].y0, t->box[0].x1, t->box[0].y1,
		        t->learned ? "" : " (guessed)", px, py, f >= 0 ? bo[f].slot : -1);
	}
	b->target_slot = t->slot;
	out->x = bx; out->y = by;

	/* fire when the crosshair is on a non-human box, tapping (1 on / 1 off keeps gunpower full) */
	if (!bot_point_human(px, py)) {
		int f = bot_front_at(px, py, best);
		int on = f >= 0 && bo[f].shoot && !bo[f].human;
		if (on && (frame & 1)) { out->fire = 1; b->shots++; }
	}

	/* missiles: big targets or crowds */
	unsigned miss = bcd2(rd_ram(P + 0x18, 2));
	if (b->missile_phase) {
		out->missile = 1;
		if (++b->missile_phase > 2) { b->missile_phase = 0; out->missile = 0; }
	} else if (bcfg.missiles && miss > 0 && frame >= b->missile_wait && abs(px - bx) < 12 && abs(py - by) < 12) {
		int big = t->hp >= bcfg.missile_hp || t->armoured;
		int crowd = ntarget >= bcfg.missile_crowd && (int)miss > bcfg.missile_keep;
		if (bot_prot && (int)miss > bcfg.missile_keep) {   /* escort: missiles at anything close to it */
			int qx = (int16_t)rd_ram(bot_prot + 2, 2), qy = (int16_t)rd_ram(bot_prot + 4, 2);
			if (abs(t->x - qx) + abs(t->y - qy) < 160) crowd = 1;
		}
		if (big || crowd) {
			b->missile_phase = 1; out->missile = 1;
			b->missile_wait = frame + (big ? 30 : 90);
			if (bcfg.verbose)
				fprintf(botlog(), "%ld P%d missile at slot %d type %02x hp %d (%d targets, %u left)\n", frame, p + 1,
				        t->slot, t->type, t->hp, ntarget, miss);
		}
	}
}

static int bot_hiscore(struct bout *o)
{
	/* initials screen: shoot A, B, C, then ENTER (positions from scripts/gun_screens.txt) */
	static const int px[4] = { 35, 67, 98, 274 }, py[4] = { 62, 62, 62, 158 };
	long t = frame - bot_level_start;
	int k = (int)(t / 48);
	if (k > 3) k = 3;
	memset(o, 0, sizeof *o);
	o->x = px[k]; o->y = py[k];
	o->fire = (t % 48) >= 24 && (t % 48) < 28;
	return 1;
}

/* apply one player's output to its device */
static void bot_drive(int p, const struct bout *o)
{
	int dev = bcfg.dev[p];
	if (dev == DEV_GUN) {
		gun_x = o->x; gun_y = o->y;
		gun_btn = (o->fire ? 1 : 0) | (o->missile ? 2 : 0) | (o->start ? 8 : 0);
		return;
	}
	if (dev != DEV_PAD1 && dev != DEV_PAD2) return;
	unsigned P = 0xFF8000 + 0x50 * p;
	int px = (int16_t)rd_ram(P + 4, 2), py = (int16_t)rd_ram(P + 6, 2);
	uint16_t m = 0;
	int dx = o->x - px, dy = o->y - py;
	/* pad crosshair accelerates up to 8 px/frame: release early to avoid overshoot */
	if (dx > 3) m |= 1u << RETRO_DEVICE_ID_JOYPAD_RIGHT;
	if (dx < -3) m |= 1u << RETRO_DEVICE_ID_JOYPAD_LEFT;
	if (dy > 3) m |= 1u << RETRO_DEVICE_ID_JOYPAD_DOWN;
	if (dy < -3) m |= 1u << RETRO_DEVICE_ID_JOYPAD_UP;
	if (abs(dx) < 16 && abs(dx) > 3 && (frame & 1)) m &= ~(3u << RETRO_DEVICE_ID_JOYPAD_LEFT);
	if (abs(dy) < 16 && abs(dy) > 3 && (frame & 1)) m &= ~((1u << RETRO_DEVICE_ID_JOYPAD_UP) | (1u << RETRO_DEVICE_ID_JOYPAD_DOWN));
	if (o->fire) m |= 1u << RETRO_DEVICE_ID_JOYPAD_Y;       /* Genesis A */
	if (o->missile == 1) m |= 1u << RETRO_DEVICE_ID_JOYPAD_B; /* Genesis B (pad: edge only) */
	if (o->start) m |= 1u << RETRO_DEVICE_ID_JOYPAD_START;
	if (dev == DEV_PAD1) pad = m; else pad2 = m;
}

static void bot_summary(const char *why)
{
	fprintf(botlog(), "%ld %s: level %u score %08x/%08x energy %04x/%04x missiles %04x/%04x credits %u "
	        "casualties %u/%u shots %ld/%ld dmg-events %ld%s\n", frame, why, rd_ram(0xFF80CC, 2),
	        rd_ram(0xFF9EA8, 4), rd_ram(0xFF9EB0, 4), rd_ram(0xFF801E, 2), rd_ram(0xFF806E, 2),
	        rd_ram(0xFF8018, 2), rd_ram(0xFF8068, 2), rd_ram(0xFF80CA, 1), rd_ram(0xFF8038, 2),
	        rd_ram(0xFF8088, 2), bp_[0].shots, bp_[1].shots, bot_dmg_events, bot_cheated ? " CHEATED" : "");
	if (!strcmp(why, "bot end")) {
		fprintf(botf, "  danger by type:");
		for (int t = 0; t < 256; t++)
			if (bot_danger[t]) fprintf(botf, " %02x:%u", t, bot_danger[t]);
		fprintf(botf, "\n  threats to the protected object by type:");
		for (int t = 0; t < 256; t++)
			if (bot_threat[t]) fprintf(botf, " %02x:%u", t, bot_threat[t]);
		fprintf(botf, "\n  human handlers:");
		for (int i = 0; i < bot_nhuman; i++) fprintf(botf, " %06x", bot_human_h[i]);
		fputc('\n', botf);
	}
	fflush(botf);
}

/* one bot frame: decide inputs, then step */
static void bot_frame(void)
{
	unsigned level = rd_ram(0xFF80CC, 2);
	if (level != bot_last_level) {
		bot_summary("level change");
		fprintf(botlog(), "%ld LEVEL %u (was %u, %ld frames)\n", frame, level, bot_last_level,
		        frame - bot_level_start);
		bot_last_level = level;
		bot_level_start = frame;
		if (bcfg.saveprefix[0]) {
			char path[1024];
			snprintf(path, sizeof path, "%s/%s%02x.state", outdir, bcfg.saveprefix, level);
			size_t sz = p_retro_serialize_size();
			void *buf = malloc(sz);
			p_retro_serialize(buf, sz);
			FILE *f = fopen(path, "wb");
			if (f) { fwrite(buf, 1, sz, f); fclose(f); }
			free(buf);
		}
	}
	/* level-skip cheat through the game's own input path: cheat flag on, pause with Start, then B+C */
	static long skip_at = -1, skip_t;
	if (rd_ram(0xFFA234, 1)) {        /* level_exit set: stats screen / game over follows, no input */
		skip_t = 0; skip_at = frame + bcfg.cheat_skip;
		gun_btn = 0;
		if (bcfg.dev[0] == DEV_PAD1 || bcfg.dev[1] == DEV_PAD1) pad = 0;
		if (bcfg.dev[0] == DEV_PAD2 || bcfg.dev[1] == DEV_PAD2) pad2 = 0;
		step();
		return;
	}
	if (bcfg.cheat_skip && level && level != 0x0A) {
		if (skip_at < bot_level_start) skip_at = bot_level_start + bcfg.cheat_skip;
		int p = bcfg.dev[0] != DEV_OFF && rd_ram(0xFF8024, 2) != 0xFFFF ? 0 : 1;
		if (frame >= skip_at && bcfg.dev[p] != DEV_OFF && rd_ram(0xFF8024 + 0x50 * p, 2) != 0xFFFF) {
			if (skip_t == 0) {
				uint8_t *wram = p_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
				wram[0xA0A6 ^ 1] = 0xFF;    /* cheat_levelskip, as after U D L R U D L R at PRESS START */
				bot_cheated = 1;
				fprintf(botlog(), "%ld level %u: level-skip cheat (P%d pause + B+C)\n", frame, level, p + 1);
			}
			struct bout o = { (int16_t)rd_ram(0xFF8004 + 0x50 * p, 2), (int16_t)rd_ram(0xFF8006 + 0x50 * p, 2), 0, 0, 0 };
			int t = skip_t++;
			if (t < 3) o.start = 1;
			bot_drive(p, &o);
			if (t >= 30 && t < 36) {
				if (bcfg.dev[p] == DEV_GUN) gun_btn = 2 | 4;
				else if (bcfg.dev[p] == DEV_PAD1) pad = (1u << RETRO_DEVICE_ID_JOYPAD_B) | (1u << RETRO_DEVICE_ID_JOYPAD_A);
				else pad2 = (1u << RETRO_DEVICE_ID_JOYPAD_B) | (1u << RETRO_DEVICE_ID_JOYPAD_A);
			}
			if (t >= 40) {                  /* retry later if the level did not end */
				skip_t = 0; skip_at = frame + 400;
				if (bcfg.dev[p] == DEV_PAD1) pad = 0;
				if (bcfg.dev[p] == DEV_PAD2) pad2 = 0;
			}
			step();
			return;
		}
		skip_t = 0;
	}
	{
		unsigned pr = rd_ram(0xFF9ECA, 4) & 0xFFFFFF;
		/* $FF9ECA is left stale between levels: only trust it while it points at a live escort
		   (hit-test handler $00627A = level 3 truck, $0062B0 = level 7 SWAT van) */
		unsigned ph = pr >= OBJ_TABLE && pr < OBJ_TABLE + OBJ_SLOTS * OBJ_STRIDE ? rd_ram(pr + 0x1C, 4) & 0xFFFFFF : 0;
		bot_prot = (ph == 0x00627A || ph == 0x0062B0) && rd_ram(pr, 2) ? pr : 0;
		/* protect cheat. Level 3: the truck fails when all 16 damage cells $FFAE84..$FFAE93 (one byte
		   each, decremented at random by enemy hits, sub_00D322) reach 0 -> keep them >= 1. Level 7: on each
		   helicopter pass the T-1000 on it must be hit more than 4 times ($FFA23E, sub_010E82) or the run
		   fails -> keep the count at 5. */
		if (bcfg.protect) {
			uint8_t *wram = p_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
			if (level == 3)
				for (int i = 0; i < 16; i++)
					if (!wram[(0xAE84 + i) ^ 1]) { wram[(0xAE84 + i) ^ 1] = 1; bot_cheated = 1; }
			if (level == 7 && rd_ram(0xFFA23E, 2) < 5) {
				wram[0xA23E ^ 1] = 0; wram[0xA23F ^ 1] = 5; bot_cheated = 1;
			}
		}
	}
	bot_scan();
	if (bcfg.dev[0] == DEV_PAD1 || bcfg.dev[1] == DEV_PAD1) pad = 0;
	if (bcfg.dev[0] == DEV_PAD2 || bcfg.dev[1] == DEV_PAD2) pad2 = 0;
	for (int p = 0; p < 2; p++) {
		if (bcfg.dev[p] == DEV_OFF) continue;
		struct bout o;
		if (level == 0x0A) {
			if (!bcfg.hiscore) continue;
			bot_hiscore(&o);
		} else
			bot_player(p, &o);
		bot_drive(p, &o);
	}
	step();
	if (frame % 1800 == 0) bot_summary("status");
}

static int bot_dev(const char *s)
{
	if (!strcmp(s, "gun")) return DEV_GUN;
	if (!strcmp(s, "pad1")) return DEV_PAD1;
	if (!strcmp(s, "pad2")) return DEV_PAD2;
	return DEV_OFF;
}

static void bot_config(const char *key, const char *val)
{
	int v = (int)strtol(val, NULL, 0);
	if (!strcmp(key, "p1")) bcfg.dev[0] = bot_dev(val);
	else if (!strcmp(key, "p2")) bcfg.dev[1] = bot_dev(val);
	else if (!strcmp(key, "energy")) bcfg.cheat_energy = v;
	else if (!strcmp(key, "skip")) bcfg.cheat_skip = v;
	else if (!strcmp(key, "missiles")) bcfg.missiles = v;
	else if (!strcmp(key, "missile_hp")) bcfg.missile_hp = v;
	else if (!strcmp(key, "missile_crowd")) bcfg.missile_crowd = v;
	else if (!strcmp(key, "missile_keep")) bcfg.missile_keep = v;
	else if (!strcmp(key, "continue")) bcfg.continues = v;
	else if (!strcmp(key, "hiscore")) bcfg.hiscore = v;
	else if (!strcmp(key, "verbose")) bcfg.verbose = v;
	else if (!strcmp(key, "focus")) bcfg.focus = v;
	else if (!strcmp(key, "protect")) bcfg.protect = v;
	else if (!strcmp(key, "freeze")) bcfg.freeze = v;
	else if (!strcmp(key, "savelevels")) snprintf(bcfg.saveprefix, sizeof bcfg.saveprefix, "%s", strcmp(val, "off") ? val : "");
	else fprintf(stderr, "botcfg: unknown key %s\n", key);
}

/* bot N | bot until LEVEL MAX | bot while LEVEL MAX ; LEVEL hex, MAX frames */
static void bot_cmd(const char *line)
{
	char a[32]; unsigned lv; long n;
	bot_last_level = rd_ram(0xFF80CC, 2);
	bot_level_start = frame;
	for (int p = 0; p < 2; p++) {
		bp_[p].last_cas = rd_ram(0xFF8038 + 0x50 * p, 2);
		bp_[p].last_index = rd_ram(0xFF8024 + 0x50 * p, 2);
	}
	if (sscanf(line, "%*s %31s %x %ld", a, &lv, &n) == 3 && (!strcmp(a, "until") || !strcmp(a, "while"))) {
		int until = a[0] == 'u';
		while (n-- > 0 && ((rd_ram(0xFF80CC, 2) == lv) != until)) bot_frame();
		fprintf(botlog(), "%ld bot %s %x: %s\n", frame, a, lv, (rd_ram(0xFF80CC, 2) == lv) == until ? "reached" : "timeout");
	} else if (sscanf(line, "%*s %ld", &n) == 1) {
		while (n-- > 0) bot_frame();
	}
	gun_btn = 0;
	if (bcfg.dev[0] == DEV_PAD1 || bcfg.dev[1] == DEV_PAD1) pad = 0;
	pad2 = 0;
	bot_summary("bot end");
}
