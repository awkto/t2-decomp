#include "gx.h"
#include "shared.h"

extern void vdp_hle_get_internal(int out[6]);   /* gpgx-hle.patch */

uint8_t *gx_work_ram(void) { return work_ram; }
uint8_t *gx_rom(void) { return cart.rom; }
uint8_t *gx_vram(void) { return vram; }
uint8_t *gx_cram(void) { return cram; }
uint8_t *gx_vsram(void) { return vsram; }
uint8_t *gx_vdp_reg(void) { return reg; }
uint8_t *gx_zram(void) { return zram; }
uint8_t *gx_io_reg(void) { return io_reg; }
uint16_t *gx_input_pad(void) { return input.pad; }

uint32_t *gx_m68k_dar(void) { return (uint32_t *)m68k.dar; }
uint32_t gx_m68k_pc(void) { return m68k.pc; }
void gx_m68k_set_pc(uint32_t pc) { m68k.pc = pc; }
/* Musashi: sp[0] = USP, sp[4] = SSP; the active one lives in dar[15] */
uint32_t gx_m68k_other_sp(void) { return m68k.sp[m68k.s_flag ? 0 : 4]; }
void gx_m68k_set_usp(uint32_t v) { if (m68k.s_flag) m68k.sp[0] = v; else m68k.dar[15] = v; }
int gx_m68k_cycles(void) { return m68k.cycles; }
void gx_m68k_set_cycles(int c) { m68k.cycles = c; }
int gx_m68k_refresh_cycles(void) { return m68k.refresh_cycles; }
void gx_m68k_set_refresh_cycles(int c) { m68k.refresh_cycles = c; }
void gx_m68k_set_ir(unsigned op) { m68k.ir = op; }
/* after rewinding the CPU clock: FIFO entries stamped in the (now) future would stall the
 * next VDP data write until then */
void gx_vdp_fifo_clamp(void)
{
	for (int i = 0; i < 4; i++)
		if ((int)fifo_cycles[i] > m68k.cycles)
			fifo_cycles[i] = m68k.cycles;
}
int gx_m68k_pending_irq(void) { return (m68k.int_level >> 8) & 7; }
void gx_m68k_clear_irq(void) { m68k.int_level = 0; }
/* the interrupt level the VDP is asserting right now (VINT has priority) */
void gx_m68k_resync_irq(void)
{
	m68k.int_level = ((reg[1] & vint_pending) ? 6 : (reg[0] & hint_pending) ? 4 : 0) << 8;
}

uint32_t gx_dma_length(void) { return dma_length; }
void gx_vdp_latch(int out[6]) { vdp_hle_get_internal(out); }

void gx_flush_dma(void)
{
	int guard = 0;
	while (dma_length && guard++ < 100000) {
		uint16 st = status;
		status &= ~8;                /* pretend active display: per-line chunks */
		mcycles_vdp = m68k.cycles;
		vdp_dma_update(m68k.cycles);
		status = (status & ~8) | (st & 8);
	}
}

uint8_t gx_zstate(void) { return zstate; }
uint32_t gx_zbank(void) { return zbank; }
void gx_z80_freeze(void) { Z80.cycles = 0xF0000000u; }

/* VDP data port writes made by C code are "untimed": the 68k FIFO wait model in
 * vdp_68k_data_w only works while the CPU stays within ~2 lines of mcycles_vdp, which a
 * C loop of hundreds of writes inside one m68k_run slice violates (it then reads past
 * fifo_timing[] and stalls the CPU for seconds). Marking the write as happening in
 * blanking (status bit 3) skips only that FIFO accounting; the C function's cost is
 * charged through its cycle count instead. Not used while a DMA fill is pending (the
 * data write starts the fill, whose rate depends on bit 3). */
static int g_untimed, g_saved_vb;

void gx_vdp_untimed_begin(void)
{
	int l[6];
	vdp_hle_get_internal(l);
	g_untimed = !l[4];
	if (g_untimed) {
		g_saved_vb = status & 8;
		status |= 8;
	}
}

void gx_vdp_untimed_end(void)
{
	if (g_untimed)
		status = (status & ~8) | g_saved_vb;
	g_untimed = 0;
}

unsigned gx_vdp_line_base(void) { return mcycles_vdp; }
/* Clock (mclks) of the start of the next line at which the frame loop (not the CPU) changes
 * state that VDP port timing or an interrupt depends on: the VBLANK flag set (line
 * viewport.h; the V-Int rises just after it starts), cleared (last line) and the frame wrap
 * (fifo reset, clock rebase); the light gun's lines (input_refresh: HV latch, external interrupt, on its line and the next one);
 * every line when the H-Int is enabled. A C replacement that runs across one of them cannot
 * be interleaved with it the way the real CPU is. */
static int g_toggle_kind;   /* what gx_vdp_next_toggle_clock() last found: 1 V-Int line, 2 VBLANK-clear/wrap, 3 H-Int, 4 light gun */
int gx_vdp_toggle_kind(void) { return g_toggle_kind; }
int gx_vdp_next_toggle_clock(void)
{
	int l = v_counter, h = bitmap.viewport.h, last = lines_per_frame - 1;
	int t = l < h ? h : (l < last ? last : last + 1);
	g_toggle_kind = l < h ? 1 : 2;
	if (l == h) {                    /* the V-Int is raised a little after this line starts */
		g_toggle_kind = 1;
		return (int)mcycles_vdp;
	}
	if (reg[0] & 0x10) {
		t = l + 1;
		g_toggle_kind = 3;
	}
	for (int port = 4; port < 6; port++) {
		int y = input.analog[port][1] + input.y_offset;
		if (y > l && y < t) {
			t = y;
			g_toggle_kind = 4;
		}
		if (y + 1 > l && y + 1 < t) {
			t = y + 1;
			g_toggle_kind = 4;
		}
	}
	return (int)mcycles_vdp + (t - l) * MCYCLES_PER_LINE;
}
int gx_vdp_vcounter(void) { return v_counter; }
void gx_vdp_set_vcounter(int v) { v_counter = v; }
/* nested 68k inside C (#33): the raster advances with the CPU, V counter included */
static int g_follow_z80;
/* exact sessions (hle_exact_session): also run the Z80 line by line like the frame loop does
 * after each m68k_run() slice. A Z80 access to the 68k bus (sound driver reading samples from
 * the ROM bank) delays the 68k by ~10 cycles, so a 68k running across a line start must see
 * those delays where the real CPU would. */
void gx_follow_z80(int on) { g_follow_z80 = on; }
void gx_vdp_follow_cpu_lines(void)
{
	while (m68k.cycles >= (int)(mcycles_vdp + MCYCLES_PER_LINE)) {
		if (g_follow_z80 && zstate == 1)
			z80_run(mcycles_vdp + MCYCLES_PER_LINE);
		mcycles_vdp += MCYCLES_PER_LINE;
		v_counter = (v_counter + 1) % lines_per_frame;
	}
}
void gx_vdp_set_line_base(unsigned v) { mcycles_vdp = v; }

/* sandbox: let the VDP line base follow the CPU (the frame loop normally does this) */
void gx_vdp_follow_cpu(void)
{
	while (m68k.cycles >= (int)(mcycles_vdp + MCYCLES_PER_LINE))
		mcycles_vdp += MCYCLES_PER_LINE;
}

/* 68k cycles left in the CPU's current time slice of the frame loop (hle_idle) */
/* instructions the 68k core executed (gpgx-native.patch; the native build has no core: 0) */
#ifdef T2_NO_68K
unsigned long gx_m68k_insn_count(void) { return 0; }
#else
extern unsigned long m68k_insn_count;
unsigned long gx_m68k_insn_count(void) { return m68k_insn_count; }
#endif

int gx_m68k_slice_left(void)
{
	if (!m68k.cycle_end || (int)m68k.cycle_end <= m68k.cycles)
		return 0;
	return ((int)m68k.cycle_end - m68k.cycles + 6) / 7;
}

/* Interrupt boundaries (the slice boundaries at which the frame loop raises an IRQ).
 * The frame loop (system.c, system_frame_gen) runs the 68k to a slice end, then raises the
 * interrupts due at that point and runs the next slice, whose first act is to take them; so
 * the original takes an interrupt at the first INSTRUCTION boundary after the boundary was
 * crossed. A C function charged as one lump cannot be interrupted in the middle. This finds
 * the first IRQ-raising boundary B in (c0, c1] (mcycles, in the 68k clock's own frame-relative
 * coordinates), so the layer can run the original 68k instead when the interrupt would land
 * inside a function. Sources, as the game configures the VDP (HINT is off): VINT at
 * vint_cycle of the first line (level 6), and the Menacer's external interrupt at the start of
 * the gun's line (level 2, lightgun_refresh). Returns the level (0 = none) and B in *at. */
static int gun_hit(const int *gy, int v)
{
	for (int p = 0; p < MAX_DEVICES; p++)
		if (gy[p] == v)
			return 1;
	return 0;
}

int gx_irq_boundary_in(int c0, int c1, int *at)
{
	int h = bitmap.viewport.h;
	int lpf = lines_per_frame;
	int gun_y[MAX_DEVICES];
	for (int p = 0; p < MAX_DEVICES; p++)
		gun_y[p] = input.dev[p] == DEVICE_LIGHTGUN ? input.analog[p][1] + input.y_offset : -1;
	int exint = (reg[11] & 0x08) && (io_reg[5] & 0x80);
	int vint = (reg[1] & 0x20) != 0;
	long frame = (long)lpf * MCYCLES_PER_LINE;
	/* time-order line index of the current line (line h is first in a frame) */
	long base = (long)mcycles_vdp;
	for (long f = 0; f < 3; f++) {
		for (long j = (f == 0 ? base / MCYCLES_PER_LINE : 0); j < lpf; j++) {
			long t0 = f * frame + j * MCYCLES_PER_LINE;
			if (j == 0 && vint && t0 + (long)vint_cycle > c0 && t0 + (long)vint_cycle <= c1) {
				*at = (int)(t0 + vint_cycle);
				return 6;
			}
			long t = t0 + MCYCLES_PER_LINE;          /* end of line j: the IRQs of line j+1 */
			if (t > c1)
				return 0;
			if (t <= c0)
				continue;
			if (exint && j + 1 < lpf) {
				int nv = (int)((h + j + 1) % lpf);
				if (nv < h && gun_hit(gun_y, nv)) {
					*at = (int)t;
					return 2;
				}
			}
		}
	}
	return 0;
}

/* The slice end the frame loop gave the 68k, and the one after it: the frame loop runs the Z80
 * up to every slice end (only while the 68k does not hold the Z80 bus), which a C function that
 * flushes its timeline across a slice end must do itself (hle_sync). */
int gx_m68k_cycle_end(void) { return (int)m68k.cycle_end; }
int gx_next_slice_end(int b)
{
	int line_end = (int)(mcycles_vdp + MCYCLES_PER_LINE);
	return b < line_end ? line_end : b + MCYCLES_PER_LINE;
}
void gx_z80_catchup(int b)
{
	if (zstate == 1)
		z80_run((unsigned)b);
}
int gx_frame_end_clock(void) { return (int)((long)lines_per_frame * MCYCLES_PER_LINE); }
