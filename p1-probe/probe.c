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
#include "board.h"
#include "fb.h"
#include "fdt.h"

#ifdef QEMU	/* emulator test build: short timings */
#define ANIM_SECONDS		3
#define HOLD_SECONDS		2
#define PSCI_INSN		"hvc #0"
#else
#define ANIM_SECONDS		30
#define HOLD_SECONDS		90
#define PSCI_INSN		"smc #0"
#endif

/* ---- secondary cores: does each one's virtual counter match? ---- */

/*
 * XNU keeps time with the virtual counter CNTVCT = CNTPCT - CNTVOFF, where
 * CNTVOFF is set per core by whatever runs at EL2 (here Qualcomm's
 * hypervisor). If the cores disagree, timers armed on one core fire late
 * or early on another. Each core reports both counters; the boot core
 * prints CNTPCT - CNTVCT (= that core's CNTVOFF) for all of them.
 */
#define PSCI_CPU_ON		0xC4000003UL
#define NCORES			8

struct core_report {
	uint64_t done, mpidr, el, frq, vct, pct;
	uint64_t gicr, waker, wake_ticks, hppir, ctl, woke;
};
static volatile struct core_report reports[NCORES];
extern char secondary_entry[];

static uint64_t smc3(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile(PSCI_INSN
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
			 :
			 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13",
			   "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static void report_here(unsigned i)
{
	volatile struct core_report *r = &reports[i];
	__asm__ volatile("isb" ::: "memory");
	r->vct = rsr("cntvct_el0");
	r->pct = rsr("cntpct_el0");
	r->mpidr = rsr("mpidr_el1");
	r->el = rsr("CurrentEL") >> 2;
	r->frq = rsr("cntfrq_el0");
	__asm__ volatile("dsb sy" ::: "memory");
	r->done = 1;
	__asm__ volatile("dsb sy" ::: "memory");
}

/*
 * Timer interrupt test, as XNU sets it up (pe_fiq.c): this core's GIC
 * redistributor awake, INTID 19 (virtual timer) Group 1 and enabled, CPU
 * interface on, then a 10 ms virtual timer and WFI. With interrupts masked
 * in PSTATE, a pending IRQ still ends WFI; ICC_HPPIR1 then names it.
 */
#ifdef QEMU
#define GICD_BASE	0x08000000UL
#define GICR_BASE	0x080a0000UL
#define VTIMER_INTID	27
#else
#define GICD_BASE	0x17a00000UL
#define GICR_BASE	0x17a60000UL
#define VTIMER_INTID	19
#endif
#define GICR_FRAME	0x20000UL
#define mmio32(a)	(*(volatile uint32_t *)(a))
#define wsr(r, v)	__asm__ volatile("msr " r ", %0" :: "r"((uint64_t)(v)))

static void timer_test(volatile struct core_report *r)
{
	uint64_t aff = rsr("mpidr_el1") & 0xffff, gicr = 0;
	for (unsigned f = 0; f < 8; f++) {
		uint64_t base = GICR_BASE + f * GICR_FRAME;
		uint64_t typer = *(volatile uint64_t *)(base + 8);
		if (((typer >> 32) & 0xffff) == aff) {
			gicr = base;
			break;
		}
	}
	r->gicr = gicr;
	if (!gicr)
		return;
	mmio32(gicr + 0x14) &= ~2U;			/* WAKER.ProcessorSleep */
	for (int n = 0; n < 1000000 && (mmio32(gicr + 0x14) & 4U); n++)
		;
	r->waker = mmio32(gicr + 0x14);
	uint64_t sgi = gicr + 0x10000;
	mmio32(sgi + 0x80) = 0xffffffffU;		/* IGROUPR0: Group 1 */
	*(volatile uint8_t *)(sgi + 0x400 + VTIMER_INTID) = 0xa0;
	mmio32(sgi + 0x100) = 1U << VTIMER_INTID;	/* ISENABLER0 */
	wsr("ICC_SRE_EL1", rsr("ICC_SRE_EL1") | 1);
	__asm__ volatile("isb");
	wsr("ICC_PMR_EL1", 0xff);
	wsr("ICC_BPR1_EL1", 0);
	wsr("ICC_IGRPEN1_EL1", 1);
	__asm__ volatile("isb");

	uint64_t frq = rsr("cntfrq_el0");
	wsr("CNTV_CTL_EL0", 0);
	wsr("CNTV_TVAL_EL0", frq / 100);		/* 10 ms */
	wsr("CNTV_CTL_EL0", 1);			/* enabled, not masked */
	__asm__ volatile("isb");
	uint64_t t0 = now();
	for (int n = 0; n < 4; n++) {		/* allow a few spurious wakes */
		__asm__ volatile("dsb sy; wfi" ::: "memory");
		r->hppir = rsr("ICC_HPPIR1_EL1");
		if ((r->hppir & 0xffffff) == VTIMER_INTID)
			break;
	}
	r->wake_ticks = now() - t0;
	r->ctl = rsr("CNTV_CTL_EL0");			/* bit 2: ISTATUS */
	r->woke = 1;
	wsr("CNTV_CTL_EL0", 2);			/* masked: stop it firing */
	__asm__ volatile("isb");
}

void secondary_main(uint64_t i)
{
	if (i >= NCORES)
		return;
	report_here((unsigned)i);
	timer_test(&reports[i]);
	__asm__ volatile("dsb sy" ::: "memory");
	reports[i].done = 2;
	__asm__ volatile("dsb sy" ::: "memory");
}

static void smp_test(void)
{
	char buf[96], *p;
	int64_t rc[NCORES] = {0};

	/* GICD: Group 1 non-secure on (ABL may leave it off), as XNU does. */
	mmio32(GICD_BASE) |= 2U;
	for (int n = 0; n < 1000000 && (mmio32(GICD_BASE) & 0x80000000U); n++)
		;
	report_here(0);
	timer_test(&reports[0]);
	reports[0].done = 2;
	for (unsigned i = 1; i < NCORES; i++) {
#ifdef QEMU
		uint64_t target = i;			/* QEMU virt: core in Aff0 */
#else
		uint64_t target = (uint64_t)i << 8;	/* SDM845: core in Aff1 */
#endif
		rc[i] = (int64_t)smc3(PSCI_CPU_ON, target, (uint64_t)secondary_entry, i);
		uint64_t end = now() + 2 * ticks_per_s();
		while (reports[i].done != 2 && now() < end)
			;
	}
	con_line("SMP", "core mpidr  EL CNTVOFF  timer: wake  hppir  ctl");
	for (unsigned i = 0; i < NCORES; i++) {
		volatile struct core_report *r = &reports[i];
		p = fmt_dec(buf, i);
		p = fmt_str(p, "    ");
		if (!r->done) {
			p = fmt_str(p, "no report, CPU_ON rc=");
			fmt_hex(p, (uint64_t)rc[i], 8);
		} else {
			p = fmt_hex(p, r->mpidr & 0xffff, 4);
			p = fmt_str(p, " ");
			p = fmt_dec(p, r->el);
			p = fmt_str(p, "  ");
			p = fmt_hex(p, r->pct - r->vct, 4);
			p = fmt_str(p, r->frq == reports[0].frq ? "  " : "! ");
			if (!r->gicr) {
				fmt_str(p, "no GICR frame");
			} else if (!r->woke) {
				fmt_str(p, "NEVER WOKE");
			} else {
				p = fmt_dec(p, r->wake_ticks * 1000 / (r->frq ? r->frq : 19200000));
				p = fmt_str(p, "ms  ");
				p = fmt_hex(p, r->hppir & 0xffffff, 3);
				p = fmt_str(p, "  ");
				fmt_hex(p, r->ctl, 1);
			}
		}
		con_line("", buf);
	}
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

void entry_main(uintptr_t dtb, uintptr_t load)
{
	con_reset(C_BLACK);
	con_at(0, 0, "PIXEL 3 PROBE p1   ios-sweet port, step 1", C_GREEN, C_BLACK);
	con_set_row(2);
	colour_bars();
	con_skip();
	show_cpu(load, dtb);
	con_skip();
	smp_test();
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
