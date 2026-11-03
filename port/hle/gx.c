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
/* after rewinding the CPU clock: FIFO entries stamped in the (now) future would stall the
 * next VDP data write until then */
void gx_vdp_fifo_clamp(void)
{
	for (int i = 0; i < 4; i++)
		if ((int)fifo_cycles[i] > m68k.cycles)
			fifo_cycles[i] = m68k.cycles;
}
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
int gx_vdp_next_toggle_clock(void)
{
	int l = v_counter, h = bitmap.viewport.h, last = lines_per_frame - 1;
	int t = l < h ? h : (l < last ? last : last + 1);
	if (l == h)                      /* the V-Int is raised a little after this line starts */
		return (int)mcycles_vdp;
	if (reg[0] & 0x10)
		t = l + 1;
	for (int port = 4; port < 6; port++) {
		int y = input.analog[port][1] + input.y_offset;
		if (y > l && y < t)
			t = y;
		if (y + 1 > l && y + 1 < t)
			t = y + 1;
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
int gx_m68k_slice_left(void)
{
	if (!m68k.cycle_end || (int)m68k.cycle_end <= m68k.cycles)
		return 0;
	return ((int)m68k.cycle_end - m68k.cycles + 6) / 7;
}
