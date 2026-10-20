/*
 * Pixel 3 probe, step 1 of the ios-sweet port.
 *
 * One `fastboot boot` answers the questions that decide the port, all
 * on screen so a photo is the whole log:
 *   1. Does ABL run our image, and is the framebuffer where we think?
 *   2. Are the colours a8r8g8b8 as mainline says?
 *   3. Does the command-mode panel keep refreshing on its own after
 *      ABL hands over (moving bar + counter)? If not, the screen stays
 *      on the fastboot menu or freezes on the first frame.
 *   4. CPU, PSCI and 16K-page support; where ABL loaded us and the DTB.
 *   5. The live /memory map and /reserved-memory carve-outs, to plan
 *      where XNU can live.
 * Then it reboots straight back into fastboot.
 */
#include <stdint.h>
#include "fb.h"
#include "fdt.h"

/* Downstream sdm845.dtsi: qcom,msm-imem@146bf000, restart_reason@65c. */
#define IMEM_RESTART_REASON	0x146bf65cUL
#define RESTART_BOOTLOADER	0x77665500U
#define PSHOLD			0x0c264000UL	/* qcom,pshold */

#define PSCI_VERSION		0x84000000UL
#define PSCI_SYSTEM_RESET	0x84000009UL
#define SMCCC_VERSION		0x80000000UL

#ifdef QEMU	/* emulator test build: PSCI via hvc, short timings, no reset */
#define ANIM_SECONDS		3
#define HOLD_SECONDS		2
#define CALL_INSN		"hvc #0"
#else
#define ANIM_SECONDS		30
#define HOLD_SECONDS		90
#define CALL_INSN		"smc #0"
#endif

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

/* The compiler may emit calls to these even in freestanding code. */
void *memset(void *d, int c, unsigned long n)
{
	volatile unsigned char *p = d;
	while (n--)
		*p++ = (unsigned char)c;
	return d;
}

void *memcpy(void *d, const void *s, unsigned long n)
{
	volatile unsigned char *dp = d;
	const volatile unsigned char *sp = s;
	while (n--)
		*dp++ = *sp++;
	return d;
}

static uint64_t smc(uint64_t fn)
{
	register uint64_t x0 __asm__("x0") = fn;
	__asm__ volatile(CALL_INSN
			 : "+r"(x0)
			 :
			 : "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9",
			   "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static uint64_t now(void)
{
	__asm__ volatile("isb" ::: "memory");
	return rsr("cntpct_el0");
}

static uint64_t ticks_per_s(void)
{
	uint64_t f = rsr("cntfrq_el0");
	return f ? f : 19200000;	/* SDM845 arch timer runs at 19.2 MHz */
}

static void delay_s(unsigned s)
{
	uint64_t end = now() + ticks_per_s() * s;
	while (now() < end)
		;
}

static void __attribute__((noreturn)) reboot_to_fastboot(void)
{
#ifdef QEMU
	for (;;)
		__asm__ volatile("wfe");
#endif
	*(volatile uint32_t *)IMEM_RESTART_REASON = RESTART_BOOTLOADER;
	__asm__ volatile("dsb sy" ::: "memory");
	smc(PSCI_SYSTEM_RESET);
	*(volatile uint32_t *)PSHOLD = 0;	/* if PSCI returned */
	for (;;)
		__asm__ volatile("wfe");
}

/* ---- device tree survey ---- */

#define MAX_MEM	16
#define MAX_RSV	64

struct region {
	const char *name;
	uint64_t base, size;
	int dynamic;	/* "size" + "alloc-ranges", placed by the OS */
};

struct survey {
	const char *model;
	const char *compat;
	uint64_t initrd_lo, initrd_hi;
	uint32_t bootargs_len;
	int n_mem, n_rsv, mem_overflow, rsv_overflow;
	struct region mem[MAX_MEM];
	struct region rsv[MAX_RSV];
};

static struct survey sv;

static void add_rsv(const char *name, uint64_t base, uint64_t size, int dynamic)
{
	if (sv.n_rsv >= MAX_RSV) {
		sv.rsv_overflow++;
		return;
	}
	struct region *r = &sv.rsv[sv.n_rsv++];
	r->name = name;
	r->base = base;
	r->size = size;
	r->dynamic = dynamic;
}

static void survey_cb(const struct fdt_prop *p, void *ctx)
{
	(void)ctx;
	if (p->depth == 0) {
		if (fdt_streq(p->name, "model"))
			sv.model = (const char *)p->val;
		else if (fdt_streq(p->name, "compatible"))
			sv.compat = (const char *)p->val;
		return;
	}
	if (p->depth == 1 && fdt_streq(p->node, "chosen")) {
		uint64_t v = p->len == 8 ? fdt_cells(p->val, 2) : p->len == 4 ? fdt_be32(p->val) : 0;
		if (fdt_streq(p->name, "linux,initrd-start"))
			sv.initrd_lo = v;
		else if (fdt_streq(p->name, "linux,initrd-end"))
			sv.initrd_hi = v;
		else if (fdt_streq(p->name, "bootargs"))
			sv.bootargs_len = p->len;
		return;
	}
	if (p->depth == 1 && fdt_startswith(p->node, "memory") && fdt_streq(p->name, "reg")) {
		int step = 4 * (p->addr_cells + p->size_cells);
		for (uint32_t off = 0; step && off + step <= p->len; off += step) {
			if (sv.n_mem >= MAX_MEM) {
				sv.mem_overflow++;
				continue;
			}
			struct region *r = &sv.mem[sv.n_mem++];
			r->name = p->node;
			r->base = fdt_cells(p->val + off, p->addr_cells);
			r->size = fdt_cells(p->val + off + 4 * p->addr_cells, p->size_cells);
		}
		return;
	}
	if (p->depth == 2 && fdt_streq(p->parent, "reserved-memory")) {
		if (fdt_streq(p->name, "reg")) {
			int step = 4 * (p->addr_cells + p->size_cells);
			for (uint32_t off = 0; step && off + step <= p->len; off += step)
				add_rsv(p->node, fdt_cells(p->val + off, p->addr_cells),
					fdt_cells(p->val + off + 4 * p->addr_cells, p->size_cells), 0);
		} else if (fdt_streq(p->name, "size")) {
			add_rsv(p->node, 0, fdt_cells(p->val, p->size_cells), 1);
		}
	}
}

/* Copy at most n chars of a DT node name, without its "@unit" suffix. */
static char *fmt_name(char *out, const char *s, int n)
{
	while (n-- > 0 && *s && *s != '@')
		*out++ = *s++;
	*out = 0;
	return out;
}

/* ---- screens ---- */

static void colour_bars(void)
{
	int row = con_row();
	int y = CON_MARGIN + row * CON_CH;
	int w = (FB_W - 2 * CON_MARGIN) / 3;
	fb_rect(CON_MARGIN, y, w, CON_CH * 2, C_RED);
	fb_rect(CON_MARGIN + w, y, w, CON_CH * 2, C_GREEN);
	fb_rect(CON_MARGIN + 2 * w, y, w, CON_CH * 2, C_BLUE);
	con_at(2, row, "RED", C_BLACK, C_RED);
	con_at(CON_COLS / 3 + 2, row, "GREEN", C_BLACK, C_GREEN);
	con_at(2 * CON_COLS / 3 + 2, row, "BLUE", C_WHITE, C_BLUE);
	con_set_row(row + 2);
}

static void show_cpu(uintptr_t load, uintptr_t dtb)
{
	uint64_t mmfr0 = rsr("id_aa64mmfr0_el1");
	uint64_t pfr0 = rsr("id_aa64pfr0_el1");
	char buf[96], *p;

	con_kv("CPU", "midr=", rsr("midr_el1"), "mpidr=", rsr("mpidr_el1"));
	con_kv("EL", "current=", rsr("CurrentEL") >> 2, "sctlr=", rsr("sctlr_el1"));
	p = fmt_str(buf, "tgran16=");
	p = fmt_hex(p, (mmfr0 >> 20) & 0xf, 1);
	p = fmt_str(p, (mmfr0 >> 20) & 0xf ? " (16K ok)" : " (NO 16K!)");
	p = fmt_str(p, "  parange=");
	p = fmt_hex(p, mmfr0 & 0xf, 1);
	p = fmt_str(p, "  gic=");
	fmt_hex(p, (pfr0 >> 24) & 0xf, 1);
	con_line("MMFR0", buf);
	con_kv("TIMER", "cntfrq=", rsr("cntfrq_el0"), 0, 0);
	con_kv("PSCI", "ver=", smc(PSCI_VERSION), "smccc=", smc(SMCCC_VERSION));
	con_kv("LOADED", "at=", load, "dtb=", dtb);
}

static void show_dt(uintptr_t dtb)
{
	char buf[96], *p;

	if (!fdt_valid(dtb)) {
		con_kv("DTB", "INVALID at ", dtb, 0, 0);
		return;
	}
	int rc = fdt_walk(dtb, survey_cb, 0);
	p = fmt_str(buf, "size=");
	p = fmt_dec(p, fdt_size(dtb));
	p = fmt_str(p, " walk=");
	fmt_str(p, rc ? "ERROR" : "ok");
	con_line("DTB", buf);
	if (sv.model) {
		fmt_name(buf, sv.model, CON_COLS - 10);
		con_line("MODEL", buf);
	}
	if (sv.compat) {
		fmt_name(buf, sv.compat, CON_COLS - 10);
		con_line("COMPAT", buf);
	}
	con_kv("INITRD", "lo=", sv.initrd_lo, "hi=", sv.initrd_hi);
	con_skip();

	uint64_t total = 0;
	for (int i = 0; i < sv.n_mem; i++) {
		p = fmt_hex(buf, sv.mem[i].base, 9);
		p = fmt_str(p, " size ");
		p = fmt_hex(p, sv.mem[i].size, 9);
		p = fmt_str(p, " = ");
		p = fmt_dec(p, sv.mem[i].size >> 20);
		fmt_str(p, " MiB");
		con_line(i ? "" : "MEMORY", buf);
		total += sv.mem[i].size;
	}
	p = fmt_dec(buf, total >> 20);
	p = fmt_str(p, " MiB in ");
	p = fmt_dec(p, sv.n_mem);
	p = fmt_str(p, " ranges");
	if (sv.mem_overflow)
		fmt_str(p, " (+more)");
	con_line("TOTAL", buf);
	con_skip();

	/* Reserved regions, two per row: "base     size     name". */
	p = fmt_dec(buf, sv.n_rsv);
	p = fmt_str(p, " carve-outs (base size name)");
	if (sv.rsv_overflow)
		fmt_str(p, " +more");
	con_line("RESERVED", buf);
	int half = CON_COLS / 2;
	for (int i = 0; i < sv.n_rsv; i += 2) {
		int row = con_row();
		if (row >= CON_ROWS - 5)	/* keep the bottom rows for the refresh test */
			break;
		for (int j = 0; j < 2 && i + j < sv.n_rsv; j++) {
			const struct region *r = &sv.rsv[i + j];
			char *q = buf;
			if (r->dynamic)
				q = fmt_str(q, "  dynamic ");
			else {
				q = fmt_hex(q, r->base, 8);
				q = fmt_str(q, " ");
			}
			q = fmt_hex(q, r->size, 7);
			q = fmt_str(q, " ");
			fmt_name(q, r->name, half - 23);
			con_at(j * half, row, buf, r->dynamic ? C_GREY : C_WHITE, C_BLACK);
		}
		con_skip();
	}
}

/*
 * Refresh test: a bar sweeps across the bottom and a counter ticks.
 * On a command-mode panel this only moves if ABL left the display
 * engine refreshing the panel from memory by itself.
 */
static void refresh_test(void)
{
	char buf[64], *p;
	int row = CON_ROWS - 3;
	int y = CON_MARGIN + (row + 1) * CON_CH;
	int bw = 120, span = FB_W - 2 * CON_MARGIN - bw;
	uint64_t tps = ticks_per_s(), start = now(), last_s = ~0ULL;
	int x_prev = -1;

	con_at(0, row, "REFRESH  bar should sweep, counter count up", C_GREEN, C_BLACK);
	for (;;) {
		uint64_t t = now() - start;
		uint64_t s = t / tps;
		if (s >= ANIM_SECONDS)
			break;
		/* triangle wave, one sweep per 2 s */
		uint64_t phase = (t % (2 * tps)) * 2 * span / (2 * tps);
		int x = CON_MARGIN + (int)(phase <= (uint64_t)span ? phase : 2 * span - phase);
		if (x != x_prev) {
			if (x_prev >= 0)
				fb_rect(x_prev, y, bw, CON_CH, C_BLACK);
			fb_rect(x, y, bw, CON_CH, C_YELLOW);
			x_prev = x;
		}
		if (s != last_s) {
			p = fmt_str(buf, "t=");
			p = fmt_dec(p, s);
			fmt_str(p, "s  ");
			con_at(CON_COLS - 8, row, buf, C_WHITE, C_BLACK);
			last_s = s;
		}
	}
	fb_rect(CON_MARGIN, y, FB_W - 2 * CON_MARGIN, CON_CH, C_BLACK);
}

static void countdown(void)
{
	char buf[64], *p;
	int row = CON_ROWS - 1;
	for (int s = HOLD_SECONDS; s > 0; s--) {
		p = fmt_str(buf, "Photo this screen. Back to fastboot in ");
		p = fmt_dec(p, s);
		fmt_str(p, "s  ");
		con_at(0, row, buf, C_GREEN, C_BLACK);
		delay_s(1);
	}
	con_at(0, row, "Rebooting to fastboot...                     ", C_GREEN, C_BLACK);
}

void probe_main(uintptr_t dtb, uintptr_t load)
{
	con_reset(C_BLACK);
	con_at(0, 0, "PIXEL 3 PROBE p1   ios-sweet port, step 1", C_GREEN, C_BLACK);
	con_set_row(2);
	colour_bars();
	con_skip();
	show_cpu(load, dtb);
	con_skip();
	show_dt(dtb);
	refresh_test();
	countdown();
	reboot_to_fastboot();
}

/* Any exception: blue screen with the syndrome, then back to fastboot. */
void exc_handler(uint64_t slot)
{
	con_reset(C_PANIC);
	con_at(0, 0, "PIXEL 3 PROBE p1   EXCEPTION", C_YELLOW, C_PANIC);
	con_set_row(2);
	con_kv("SLOT", "n=", slot, 0, 0);
	con_kv("ESR", "", rsr("esr_el1"), 0, 0);
	con_kv("ELR", "", rsr("elr_el1"), 0, 0);
	con_kv("FAR", "", rsr("far_el1"), 0, 0);
	con_kv("SPSR", "", rsr("spsr_el1"), 0, 0);
	con_skip();
	con_line("", "Photo this screen. Back to fastboot in 60s.");
	delay_s(60);
	reboot_to_fastboot();
}
