/*
 * Pixel 3 input probe: buttons over SPMI and where the touchscreen is.
 *
 * 1. Touch: walks the live device tree (ABL merged the board overlay in)
 *    for the touchscreen node and prints its bus, address and the
 *    GPIO/regulator properties a driver will need.
 * 2. Buttons: finds the PMIC arbiter's channels for PM8998's PON block
 *    (power = KPDPWR, volume down = RESIN) and GPIO 6 (volume up) and
 *    shows their live state for 60 s: press the keys and watch.
 *
 * PMIC access is read-only. The only register writes go to the
 * arbiter's observer channel command register to request one-byte reads
 * (EXT_READL); no PMIC register is ever written. Then back to fastboot.
 */
#include <stdint.h>
#include "board.h"
#include "fb.h"
#include "fdt.h"

#ifdef QEMU
#define LIVE_SECONDS	3
#define HOLD_SECONDS	2
#else
#define LIVE_SECONDS	60
#define HOLD_SECONDS	30
#endif

/* ---- device tree: find the touchscreen ---- */

#define MAX_DEPTH	16
#define MAX_PROPS	12

static struct {
	const char *name[MAX_DEPTH], *compat[MAX_DEPTH];
	uint64_t reg[MAX_DEPTH];
	int depth;			/* depth of the touch node, -1 = none yet */
	const char *node, *compat_s, *bus, *bus_compat;
	uint64_t addr, bus_reg;
	int nprops;
	const char *pname[MAX_PROPS];
	uintptr_t pval[MAX_PROPS];
	uint32_t plen[MAX_PROPS];
} ts = { .depth = -1 };

static int contains(const char *s, uint32_t len, const char *needle)
{
	for (uint32_t i = 0; i < len; i++) {
		const char *a = s + i, *b = needle;
		while (*b && *a == *b)
			a++, b++;
		if (!*b)
			return 1;
	}
	return 0;
}

static int interesting(const char *name)
{
	static const char *const keys[] = { "gpio", "supply", "interrupt", "reset", "irq", 0 };
	for (int k = 0; keys[k]; k++)
		if (contains(name, 32, keys[k]))
			return 1;
	return 0;
}

static void dt_cb(const struct fdt_prop *p, void *ctx)
{
	(void)ctx;
	int d = p->depth;
	if (d < 0 || d >= MAX_DEPTH)
		return;
	if (ts.name[d] != p->node) {		/* first property of a new node */
		ts.name[d] = p->node;
		ts.compat[d] = 0;
		ts.reg[d] = 0;
	}
	if (fdt_streq(p->name, "compatible")) {
		ts.compat[d] = (const char *)p->val;
		if (ts.depth < 0 && d > 0 && (contains((const char *)p->val, p->len, "fts") ||
		    contains((const char *)p->val, p->len, "touch"))) {
			ts.depth = d;
			ts.node = p->node;
			ts.compat_s = (const char *)p->val;
			ts.bus = ts.name[d - 1];
			ts.bus_compat = ts.compat[d - 1];
			ts.bus_reg = ts.reg[d - 1];
		}
	} else if (fdt_streq(p->name, "reg") && p->len >= 4) {
		ts.reg[d] = fdt_cells(p->val, p->addr_cells ? p->addr_cells : 1);
		if (ts.depth == d && ts.node == p->node)
			ts.addr = ts.reg[d];
	}
	if (ts.depth == d && ts.node == p->node && interesting(p->name) && ts.nprops < MAX_PROPS) {
		ts.pname[ts.nprops] = p->name;
		ts.pval[ts.nprops] = p->val;
		ts.plen[ts.nprops] = p->len;
		ts.nprops++;
	}
}

static void show_touch(uintptr_t dtb)
{
	char buf[96], *p;
	if (!fdt_valid(dtb) || fdt_walk(dtb, dt_cb, 0)) {
		con_line("TOUCH", "device tree walk failed");
		return;
	}
	if (ts.depth < 0) {
		con_line("TOUCH", "no node with \"fts\"/\"touch\" in compatible");
		return;
	}
	p = fmt_str(buf, ts.node);
	p = fmt_str(p, "  i2c addr ");
	fmt_hex(p, ts.addr, 2);
	con_line("TOUCH", buf);
	con_line("", ts.compat_s);
	p = fmt_str(buf, "bus ");
	p = fmt_str(p, ts.bus);
	p = fmt_str(p, " reg ");
	fmt_hex(p, ts.bus_reg, 8);
	con_line("", buf);
	if (ts.bus_compat)
		con_line("", ts.bus_compat);
	for (int i = 0; i < ts.nprops; i++) {
		p = fmt_str(buf, ts.pname[i]);
		p = fmt_str(p, " =");
		for (uint32_t c = 0; c < ts.plen[i] / 4 && c < 4; c++) {
			p = fmt_str(p, " ");
			p = fmt_hex(p, fdt_be32(ts.pval[i] + 4 * c), 2);
		}
		con_line("", buf);
	}
}

/* ---- SPMI PMIC arbiter v5, read-only ---- */

#define ARB_CORE	0x0c440000UL	/* "core": version, features, APID->PPID map */
#define ARB_OBS		0x0e600000UL	/* "obsrvr": read-only command channels */
#define ARB_CNFG	0x0c40a000UL	/* "cnfg": APID ownership */
#define NO_APID		0xffffU

static uint32_t rd32(uintptr_t a)
{
	uint32_t v = *(volatile uint32_t *)a;
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static uint32_t arb_version, arb_apids, last_status;

static uint16_t find_apid(uint16_t ppid)
{
	uint16_t found = NO_APID;
	for (uint32_t i = 0; i < arb_apids; i++) {
		uint32_t map = rd32(ARB_CORE + 0x900 + 4 * i);
		if (!map || ((map >> 8) & 0xfff) != ppid)
			continue;
		/* A PPID may be mapped more than once; prefer the apps-owned one. */
		if (found == NO_APID || (rd32(ARB_CNFG + 0x700 + 4 * i) & 7) == 0)
			found = (uint16_t)i;
	}
	return found;
}

/* One-byte EXT_READL through this EE's (0) observer channel. */
static int spmi_read(uint16_t apid, uint16_t addr, uint8_t *out)
{
	if (apid == NO_APID)
		return 0;
	uintptr_t ch = ARB_OBS + 0x80UL * apid;
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)ch = (1U << 27) | ((uint32_t)(addr & 0xff) << 4);
	__asm__ volatile("dsb osh" ::: "memory");
	uint64_t end = now() + ticks_per_s() / 100;
	while (now() < end) {
		last_status = rd32(ch + 0x08);
		if (last_status & 1) {
			if (last_status & 0x0e)
				return 0;
			*out = (uint8_t)rd32(ch + 0x18);
			return 1;
		}
	}
	return 0;
}

static void show_keys(void)
{
	char buf[96], *p;
	arb_version = rd32(ARB_CORE);
	arb_apids = rd32(ARB_CORE + 4) & 0x7ff;
	con_kv("SPMI", "arbiter=", arb_version, "apids=", arb_apids);
	if (arb_version < 0x50000000U || arb_version >= 0x70000000U || !arb_apids || arb_apids > 512) {
		con_line("", "not a v5 arbiter: stopping here (nothing read)");
		return;
	}
	uint16_t pon = find_apid(0x008);	/* SID 0, peripheral 0x08: PON */
	uint16_t gpio = find_apid(0x0c5);	/* SID 0, peripheral 0xc5: GPIO 6 */
	con_kv("APID", "pon=", pon, "gpio6=", gpio);

	/* GPIO 6 setup as firmware left it (shown, never changed). */
	uint8_t subtype = 0, mode = 0, en = 0;
	int ok = spmi_read(gpio, 0xc505, &subtype) && spmi_read(gpio, 0xc540, &mode) &&
	    spmi_read(gpio, 0xc546, &en);
	p = fmt_str(buf, ok ? "subtype=" : "READ FAILED status=");
	if (ok) {
		p = fmt_hex(p, subtype, 2);
		p = fmt_str(p, " mode=");
		p = fmt_hex(p, mode, 2);
		p = fmt_str(p, " en=");
		fmt_hex(p, en, 2);
	} else {
		fmt_hex(p, last_status, 8);
	}
	con_line("GPIO6", buf);

	con_skip();
	con_line("KEYS", "press power / volume up / volume down now");
	int row = con_row();
	uint64_t end = now() + ticks_per_s() * LIVE_SECONDS;
	while (now() < end) {
		uint8_t rt = 0, st1 = 0, rt6 = 0;
		int a = spmi_read(pon, 0x810, &rt);	/* PON INT_RT_STS */
		int b = spmi_read(gpio, 0xc508, &st1);	/* GPIO STATUS1 */
		int c = spmi_read(gpio, 0xc510, &rt6);	/* GPIO INT_RT_STS */
		p = fmt_str(buf, "power ");
		p = fmt_str(p, !a ? "?   " : (rt & 1) ? "DOWN" : "up  ");
		p = fmt_str(p, "  vol- ");
		p = fmt_str(p, !a ? "?   " : (rt & 2) ? "DOWN" : "up  ");
		p = fmt_str(p, "  pon=");
		p = fmt_hex(p, rt, 2);
		p = fmt_str(p, " g6 st1=");
		p = fmt_hex(p, b ? st1 : 0xff, 2);
		p = fmt_str(p, " rt=");
		fmt_hex(p, c ? rt6 : 0xff, 2);
		con_at(0, row, buf, (rt & 3) ? C_YELLOW : C_WHITE, C_BLACK);
		uint64_t t = now() + ticks_per_s() / 20;
		while (now() < t)
			;
	}
	con_set_row(row + 1);
}

void entry_main(uintptr_t dtb, uintptr_t load)
{
	(void)load;
	con_reset(C_BLACK);
	con_at(0, 0, "PIXEL 3 INPUT p3   buttons + touch survey", C_GREEN, C_BLACK);
	con_set_row(2);
	show_touch(dtb);
	con_skip();
#ifdef QEMU
	con_line("SPMI", "skipped in QEMU (no PMIC arbiter)");
#else
	show_keys();
#endif
	con_skip();
	con_line("", "Photo this screen. Back to fastboot shortly.");
	delay_s(HOLD_SECONDS);
	reboot_to_fastboot();
}

void exc_handler(uint64_t slot)
{
	con_reset(C_PANIC);
	con_at(0, 0, "PIXEL 3 INPUT p3   EXCEPTION", C_YELLOW, C_PANIC);
	con_set_row(2);
	con_kv("SLOT", "n=", slot, 0, 0);
	con_kv("ESR", "", rsr("esr_el1"), 0, 0);
	con_kv("ELR", "", rsr("elr_el1"), 0, 0);
	con_kv("FAR", "", rsr("far_el1"), 0, 0);
	con_skip();
	con_line("", "Photo this screen. Back to fastboot in 60s.");
	delay_s(60);
	reboot_to_fastboot();
}
