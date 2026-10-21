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
#define LIVE_SECONDS	30
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

/* ---- touch bus: GENI I2C on QUP wrapper 0, SE2 (i2c@888000) ---- */

/*
 * Only these writes: GCC clock votes (wrapper 0 m/s AHB, SE2) and the SE2
 * clock source set to the 19.2 MHz crystal (gcc-sdm845.c), the TLMM mux of
 * GPIO 27/28 to qup2 (pinctrl-sdm845.c), and the serial engine's own
 * registers (i2c-qcom-geni.c / qcom-geni-se.h). Reset, IRQ and bus-switch
 * GPIOs are only read. The touch chip only gets read commands.
 */
#define GCC		0x00100000UL
#define GCC_VOTE	(GCC + 0x5200c)		/* bits 6 m_ahb, 7 s_ahb, 12 s2 */
#define GCC_S2_CBCR	(GCC + 0x17290)
#define GCC_S2_RCG	(GCC + 0x17294)		/* CMD; CFG at +4 */
#define TLMM_EAST	0x03500000UL
#define TLMM_NORTH	0x03900000UL
#define GPIO_CFG(t, n)	((t) + 0x1000UL * (n))
#define GPIO_IO(t, n)	(GPIO_CFG(t, n) + 4)
#define SE		0x00888000UL
#define SE_FW_REV	0x068
#define SE_M_CLK_CFG	0x048
#define SE_CLK_SEL	0x07c
#define SE_DMA_MODE	0x258
#define SE_TX_PACK0	0x260
#define SE_TX_PACK1	0x264
#define SE_I2C_TX_LEN	0x26c
#define SE_I2C_RX_LEN	0x270
#define SE_I2C_SCL	0x278
#define SE_RX_PACK0	0x284
#define SE_RX_PACK1	0x288
#define SE_M_CMD0	0x600
#define SE_M_IRQ_STAT	0x610
#define SE_M_IRQ_CLR	0x618
#define SE_TX_FIFO	0x700
#define SE_RX_FIFO	0x780
#define SE_RX_FIFO_ST	0x804
#define M_CMD_DONE	(1U << 0)
#define M_ERRORS	(0x0eU | (0x3fU << 9))	/* overrun/illegal/fail + GP IRQs (NACK = bit 10) */
#define I2C_WRITE	1U
#define I2C_READ	2U
#define FTS_ADDR	0x49U

#define w32(a, v)	(*(volatile uint32_t *)(a) = (v))

static uint32_t rd32(uintptr_t a);	/* below, with the SPMI code */

static uint32_t i2c_status;

static void spin_us(uint64_t us)
{
	uint64_t end = now() + ticks_per_s() * us / 1000000;
	while (now() < end)
		;
}

static int i2c_wait(void)
{
	uint64_t end = now() + ticks_per_s() / 50;	/* 20 ms */
	while (now() < end) {
		i2c_status = rd32(SE + SE_M_IRQ_STAT);
		if (i2c_status & (M_CMD_DONE | M_ERRORS)) {
			w32(SE + SE_M_IRQ_CLR, i2c_status);
			return (i2c_status & M_ERRORS) ? -1 : 0;
		}
	}
	return -2;
}

/* One I2C message; stretch = repeated start follows (no STOP). */
static int i2c_xfer(int read, const uint8_t *tx, uint8_t *rx, uint32_t len, int stretch)
{
	w32(SE + SE_M_IRQ_CLR, 0xffffffffU);
	w32(SE + (read ? SE_I2C_RX_LEN : SE_I2C_TX_LEN), len);
	w32(SE + SE_M_CMD0, ((read ? I2C_READ : I2C_WRITE) << 27) | (FTS_ADDR << 9) | (stretch ? 4U : 0));
	if (!read) {
		for (uint32_t i = 0; i < len; i += 4) {
			uint32_t word = 0;
			for (uint32_t b = 0; b < 4 && i + b < len; b++)
				word |= (uint32_t)tx[i + b] << (8 * b);
			w32(SE + SE_TX_FIFO, word);
		}
	}
	int rc = i2c_wait();
	if (read && rc == 0) {
		uint32_t words = rd32(SE + SE_RX_FIFO_ST) & 0x1ffffff, got = 0;
		for (uint32_t i = 0; i < words; i++) {
			uint32_t word = rd32(SE + SE_RX_FIFO);
			for (uint32_t b = 0; b < 4 && got < len; b++)
				rx[got++] = (uint8_t)(word >> (8 * b));
		}
		if (got < len)
			rc = -3;
	}
	return rc;
}

static int fts_read(const uint8_t *cmd, uint32_t clen, uint8_t *out, uint32_t len)
{
	int rc = i2c_xfer(0, cmd, 0, clen, 1);
	return rc ? rc : i2c_xfer(1, 0, out, len, 0);
}

static void show_touch_bus(void)
{
	char buf[96], *p;

	/* Clocks: wrapper 0 AHB + SE2 on, SE2 from the crystal (CFG src 0, div 1). */
	w32(GCC_VOTE, rd32(GCC_VOTE) | (1U << 6) | (1U << 7) | (1U << 12));
	w32(GCC_S2_RCG + 4, 1U);
	w32(GCC_S2_RCG, rd32(GCC_S2_RCG) | 1U);		/* CMD_UPDATE */
	spin_us(100);
	/* SDA/SCL: GPIO 27/28 -> qup2 (function 1), no pull, 2 mA. */
	w32(GPIO_CFG(TLMM_EAST, 27), 1U << 2);
	w32(GPIO_CFG(TLMM_EAST, 28), 1U << 2);

	p = fmt_str(buf, "gcc vote=");
	p = fmt_hex(p, rd32(GCC_VOTE), 8);
	p = fmt_str(p, " s2 cbcr=");
	fmt_hex(p, rd32(GCC_S2_CBCR), 8);
	con_line("I2C", buf);
	/* Wrapper 0 core clocks (CBCR bit 31 = off): shown, then voted (bits 8, 9). */
	p = fmt_str(buf, "core cbcr 0x1700c=");
	p = fmt_hex(p, rd32(GCC + 0x1700c), 8);
	p = fmt_str(p, " 0x17010=");
	fmt_hex(p, rd32(GCC + 0x17010), 8);
	con_line("", buf);
	w32(GCC_VOTE, rd32(GCC_VOTE) | (1U << 8) | (1U << 9));
	spin_us(100);
	uint32_t fw = rd32(SE + SE_FW_REV);
	p = fmt_str(buf, "se fw=");
	p = fmt_hex(p, fw, 8);
	p = fmt_str(p, (fw >> 8 & 0xff) == 3 ? " (I2C)" : " (NOT I2C)");
	con_line("", buf);
	p = fmt_str(buf, "rst99=");
	p = fmt_hex(p, rd32(GPIO_IO(TLMM_NORTH, 99)) & 3, 1);
	p = fmt_str(p, " irq125=");
	p = fmt_hex(p, rd32(GPIO_IO(TLMM_EAST, 125)) & 3, 1);
	p = fmt_str(p, " sw136=");
	p = fmt_hex(p, rd32(GPIO_IO(TLMM_NORTH, 136)) & 3, 1);
	p = fmt_str(p, " cfg=");
	fmt_hex(p, rd32(GPIO_CFG(TLMM_NORTH, 136)) & 0x3ff, 3);
	con_line("PINS", buf);
	if ((fw >> 8 & 0xff) != 3)
		return;

	/* geni_se_io_init: internal clock gating on, outputs enabled, defaults. */
	w32(SE + 0x028, rd32(SE + 0x028) | 0x7fU);	/* GENI_CGC_CTRL */
	w32(SE + 0x024, 0x7fU);				/* GENI_OUTPUT_CTRL */
	w32(SE + 0x020, 1U);				/* GENI_FORCE_DEFAULT_REG */
	/* FIFO mode, byte packing (first byte in bits 7:0), 400 kHz from 19.2 MHz. */
	w32(SE + SE_DMA_MODE, 0);
	w32(SE + SE_TX_PACK0, 0x0004380eU);
	w32(SE + SE_TX_PACK1, 0x000c3e0eU);
	w32(SE + SE_RX_PACK0, 0x0004380eU);
	w32(SE + SE_RX_PACK1, 0x000c3e0eU);
	w32(SE + SE_CLK_SEL, 0);
	w32(SE + SE_M_CLK_CFG, (2U << 4) | 1U);
	w32(SE + SE_I2C_SCL, (5U << 20) | (12U << 10) | 24U);
	w32(SE + 0x614, M_CMD_DONE | M_ERRORS);		/* M_IRQ_EN: let status latch */
	w32(SE + 0xe1c, rd32(SE + 0xe1c) | 7U);		/* SE_IRQ_EN: M, S, DMA */

	/* Chip ID: 0xFA + hw address 0x20000000, one dummy byte first. */
	static const uint8_t id_cmd[5] = { 0xfa, 0x20, 0x00, 0x00, 0x00 };
	uint8_t id[4] = { 0 };
	p = fmt_str(buf, "geni st=");
	p = fmt_hex(p, rd32(SE + 0x040), 8);
	p = fmt_str(p, " scl rb=");
	p = fmt_hex(p, rd32(SE + SE_I2C_SCL), 8);	/* = 0x00503018 when writes stick */
	p = fmt_str(p, " irqen=");
	fmt_hex(p, rd32(SE + 0x614), 4);
	con_line("", buf);
	int rc = fts_read(id_cmd, sizeof(id_cmd), id, sizeof(id));
	p = fmt_str(buf, rc ? "chip id read failed rc=" : "chip id ");
	if (rc) {
		p = fmt_dec(p, (uint64_t)(-rc));
		p = fmt_str(p, " status=");
		fmt_hex(p, i2c_status, 8);
	} else {
		for (int i = 0; i < 4; i++) {
			p = fmt_hex(p, id[i], 2);
			p = fmt_str(p, " ");
		}
	}
	con_line("FTS", buf);
	if (rc)
		return;

	/* Event FIFO for 20 s: tap the screen. */
	con_line("", "tap the screen now (20 s)");
	int row = con_row();
	unsigned events = 0;
	uint64_t end = now() + ticks_per_s() * LIVE_SECONDS / 3;
	static const uint8_t fifo_cmd[1] = { 0x86 };
	while (now() < end) {
		uint8_t e[8] = { 0 };
		if (fts_read(fifo_cmd, 1, e, 8) == 0 && e[0]) {
			events++;
			p = fmt_dec(buf, events);
			p = fmt_str(p, ": ");
			for (int i = 0; i < 8; i++) {
				p = fmt_hex(p, e[i], 2);
				p = fmt_str(p, " ");
			}
			con_at(0, row, buf, C_YELLOW, C_BLACK);
		}
		spin_us(10000);
	}
	if (!events)
		con_at(0, row, "no events (chip not scanning?)", C_WHITE, C_BLACK);
	con_set_row(row + 1);
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
#ifndef QEMU
	show_touch_bus();
	con_skip();
#endif
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
