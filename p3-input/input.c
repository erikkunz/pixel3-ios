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
#define HOLD_SECONDS	90
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

/*
 * ---- GPI DMA ----
 * This SE has its FIFO interface disabled (GENI_IF_DISABLE_RO bit 0): the
 * bytes move only through QUP wrapper 0's GPI engine, as Linux's gpi.c and
 * i2c-qcom-geni.c (GPI mode) do it. One GPI interface ("GPII") of the apps
 * processor's mask (DT qcom,gpii-mask = 0xfa) with its two channels (0 = TX,
 * 1 = RX) and one event ring; each channel names the SE it serves in its
 * scratch register. Polled: command completion from the channel state,
 * transfer completion from the event ring. MMU and caches are off here, so
 * the rings are at their physical addresses and need no cache maintenance.
 */
#define GPI		0x00800000UL
#define GPI_SEID	2U			/* SE2 */
#define GPI_PROTO_I2C	3U
#define GPI_MASK	0xfaU			/* GPIIs the apps processor may use */
#define GPII(n, o)	(GPI + 0x4000UL * (n) + (o))
#define CH_CNTXT(n, k)	GPII(n, 0x20000UL + 0x80UL * (k))	/* +0 config/state, +4 length, +8/+c base, +10 RP */
#define EV_CNTXT(n)	GPII(n, 0x21000UL)
#define CH_DB(n, k)	GPII(n, 0x22000UL + 0x8UL * (k))
#define EV_DB(n)	GPII(n, 0x22100UL)
#define CH_CMD(n)	GPII(n, 0x23008UL)
#define EV_CMD(n)	GPII(n, 0x23010UL)
#define TRE_CHAIN	(1U << 0)
#define TRE_IEOT	(1U << 9)
#define TRE_LINK	(1U << 11)
#define TRE_DMA		(0x10U << 16)
#define TRE_GO		(0x20U << 16)
#define TRE_CONFIG0	(0x22U << 16)
#define CH_TRES		64U
#define EV_ELEMS	256U

static volatile uint32_t gpi_ch_ring[2][CH_TRES * 4] __attribute__((aligned(1024)));
static volatile uint32_t gpi_ev_ring[EV_ELEMS * 4] __attribute__((aligned(4096)));
static volatile uint8_t gpi_buf[2][64] __attribute__((aligned(64)));
static uint32_t gpi_ch_wp[2], gpi_ev_rp, gpi_ev_wp, gpii, gpi_mode;
static uint32_t gpi_ev[4];		/* the last event, for the screen */
static uint32_t gpi_trace = 1;		/* progress lines for the first transfer */
static void smmu_show_faults(const char *when);

static uint32_t gpi_state(uintptr_t cntxt)
{
	return rd32(cntxt) >> 20 & 0xf;
}

static int gpi_wait_state(uintptr_t cntxt, uint32_t state)
{
	uint64_t end = now() + ticks_per_s() / 4;	/* Linux: 250 ms */
	while (now() < end)
		if (gpi_state(cntxt) == state)
			return 0;
	return -1;
}

static void gpi_clear_ctrl_irqs(uint32_t n)
{
	w32(GPII(n, 0x230a0), rd32(GPII(n, 0x23090)));	/* channel control */
	w32(GPII(n, 0x230a4), rd32(GPII(n, 0x23094)));	/* event ring control */
}

/* Linux's gpi_ch_init(): interrupts configured, event ring allocated, both channels allocated and started. */
static int gpi_init(uint32_t n)
{
	uintptr_t ev = (uintptr_t)gpi_ev_ring;

	w32(GPII(n, 0x23088), 0x4fU);		/* TYPE_IRQ_MSK: general, IEOB, glob, ev ctrl, ch ctrl */
	w32(GPII(n, 0x230b8), 1U);		/* IEOB */
	w32(GPII(n, 0x23098), 3U);		/* channels */
	w32(GPII(n, 0x2309c), 1U);		/* event ring */
	w32(GPII(n, 0x23108), 0xfU);		/* GLOB_IRQ_EN */
	w32(GPII(n, 0x23120), 0xfU);		/* GPII_IRQ_EN */
	w32(GPII(n, 0x23188), 0);		/* MSI base */
	w32(GPII(n, 0x2318c), 0);
	w32(GPII(n, 0x23400), 0);		/* scratch */
	w32(GPII(n, 0x23404), 0);
	w32(GPII(n, 0x23180), 1U);		/* INTSET */
	w32(GPII(n, 0x23200), 0);		/* error log */

	w32(EV_CMD(n), 0);			/* ALLOCATE */
	if (gpi_wait_state(EV_CNTXT(n), 1))
		return -1;
	w32(EV_CNTXT(n) + 0x00, (16U << 24) | (1U << 16) | 2U);	/* 16-byte elements, IRQ type, GPI event ring */
	w32(EV_CNTXT(n) + 0x04, EV_ELEMS * 16);
	w32(EV_CNTXT(n) + 0x08, (uint32_t)ev);
	w32(EV_CNTXT(n) + 0x0c, (uint32_t)((uint64_t)ev >> 32));
	w32(EV_DB(n) + 4, (uint32_t)((uint64_t)ev >> 32));
	w32(EV_CNTXT(n) + 0x20, 0);		/* interrupt moderation */
	w32(EV_CNTXT(n) + 0x28, 0);
	w32(EV_CNTXT(n) + 0x2c, 0);
	w32(EV_CNTXT(n) + 0x30, 0);
	w32(EV_CNTXT(n) + 0x34, 0);
	gpi_ev_rp = 0;
	gpi_ev_wp = EV_ELEMS - 1;		/* the whole ring is the engine's to fill */
	w32(EV_DB(n), (uint32_t)(ev + 16 * gpi_ev_wp));
	gpi_clear_ctrl_irqs(n);

	for (uint32_t k = 0; k < 2; k++) {
		uintptr_t r = (uintptr_t)gpi_ch_ring[k];

		w32(CH_CMD(n), k);		/* ALLOCATE */
		if (gpi_wait_state(CH_CNTXT(n, k), 1))
			return -2 - (int)k;
		w32(CH_CNTXT(n, k) + 0x00, (16U << 24) | ((k == 0) << 3) | 2U);	/* TX is "out" */
		w32(CH_CNTXT(n, k) + 0x04, CH_TRES * 16);
		w32(CH_CNTXT(n, k) + 0x08, (uint32_t)r);
		w32(CH_CNTXT(n, k) + 0x0c, (uint32_t)((uint64_t)r >> 32));
		w32(CH_DB(n, k) + 4, (uint32_t)((uint64_t)r >> 32));
		w32(GPII(n, 0x20060 + 0x80 * k), ((1U - k) << 16) | (GPI_PROTO_I2C << 4) | GPI_SEID);
		w32(GPII(n, 0x20064 + 0x80 * k), 0);
		w32(GPII(n, 0x20068 + 0x80 * k), 0);
		w32(GPII(n, 0x2006c + 0x80 * k), 0);
		w32(GPII(n, 0x2005c + 0x80 * k), 1U);	/* QOS */
		gpi_ch_wp[k] = 0;
		gpi_clear_ctrl_irqs(n);
	}
	for (uint32_t k = 0; k < 2; k++) {
		w32(CH_CMD(n), (1U << 24) | k);	/* START */
		if (gpi_wait_state(CH_CNTXT(n, k), 2))
			return -4 - (int)k;
		gpi_clear_ctrl_irqs(n);
	}
	return 0;
}

static void gpi_tre(uint32_t k, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3)
{
	volatile uint32_t *t = &gpi_ch_ring[k][4 * (gpi_ch_wp[k] % CH_TRES)];

	t[0] = d0;
	t[1] = d1;
	t[2] = d2;
	t[3] = d3;
	gpi_ch_wp[k]++;
}

static void gpi_ring(uint32_t k)
{
	__asm__ volatile("dsb sy" ::: "memory");
	w32(CH_DB(gpii, k), (uint32_t)((uintptr_t)gpi_ch_ring[k] + 16 * (gpi_ch_wp[k] % CH_TRES)));
}

/* Until a transfer-complete event of channel chid: 0, or <0 (code/notification/timeout). */
static int gpi_wait_xfer(uint32_t chid)
{
	uintptr_t base = (uintptr_t)gpi_ev_ring;
	uint64_t end = now() + ticks_per_s() / 50;	/* 20 ms */

	while (now() < end) {
		uint32_t hw = (rd32(EV_CNTXT(gpii) + 0x10) - (uint32_t)base) / 16;

		while (gpi_ev_rp != hw % EV_ELEMS) {
			volatile uint32_t *e = &gpi_ev_ring[4 * gpi_ev_rp];

			for (int i = 0; i < 4; i++)
				gpi_ev[i] = e[i];
			gpi_ev_rp = (gpi_ev_rp + 1) % EV_ELEMS;
			gpi_ev_wp = (gpi_ev_wp + 1) % EV_ELEMS;
			w32(EV_DB(gpii), (uint32_t)(base + 16 * gpi_ev_wp));
			w32(GPII(gpii, 0x230c0), 1U);	/* IEOB clear */
			uint32_t type = gpi_ev[3] >> 16 & 0xff, ch = gpi_ev[3] >> 24, code = gpi_ev[2] >> 24;
			if (type == 0x31)		/* QUP notification: an error on the bus (NACK ...) */
				return -4;
			if (type == 0x22 && ch == chid)
				return (code == 1 || code == 2 || code == 4) ? 0 : -5;
		}
	}
	return -2;
}

/* Write cmd (repeated start), then read len bytes: Linux's geni_i2c_gpi_xfer() for two messages. */
static int gpi_fts_read(const uint8_t *cmd, uint32_t clen, uint8_t *out, uint32_t len)
{
	uintptr_t tx = (uintptr_t)gpi_buf[0], rx = (uintptr_t)gpi_buf[1];
	int rc;

	for (uint32_t i = 0; i < clen; i++)
		gpi_buf[0][i] = cmd[i];
	for (uint32_t i = 0; i < len; i++)
		gpi_buf[1][i] = 0;
	/* 400 kHz from 19.2 MHz (t_low 12, t_high 5, t_cycle 24, div 2), TX/RX packing */
	gpi_tre(0, 12U | (5U << 8) | (24U << 16) | (3U << 24), 0, 2U, TRE_CONFIG0 | TRE_CHAIN);
	gpi_tre(0, I2C_WRITE | (FTS_ADDR << 8) | (1U << 26), 0, clen, TRE_GO | TRE_CHAIN);
	gpi_tre(0, (uint32_t)tx, (uint32_t)((uint64_t)tx >> 32), clen, TRE_DMA | TRE_IEOT);
	if (gpi_trace) {
		con_line("", "gpi: write TREs queued, ringing TX doorbell");
		smmu_show_faults("before");
	}
	gpi_ring(0);
	if (gpi_trace) {
		spin_us(1000);
		smmu_show_faults("1ms");
		spin_us(100000);
		smmu_show_faults("100ms");
	}
	rc = gpi_wait_xfer(0);
	if (gpi_trace)
		con_line("", rc ? "gpi: write failed" : "gpi: write done, reading");
	if (rc)
		return rc;
	gpi_tre(1, (uint32_t)rx, (uint32_t)((uint64_t)rx >> 32), len, TRE_DMA | TRE_IEOT);
	gpi_ring(1);
	gpi_tre(0, I2C_READ | (FTS_ADDR << 8), 0, len, TRE_GO | TRE_LINK);
	gpi_ring(0);
	rc = gpi_wait_xfer(1);
	for (uint32_t i = 0; i < len; i++)
		out[i] = gpi_buf[1][i];
	return rc ? rc - 10 : 0;
}

/*
 * The GPI engine's DMA goes through the apps SMMU as stream 0x16 (DT iommus).
 * Unless the firmware matched that stream, its first descriptor fetch faults.
 * Linux's way on SDM845 (arm-smmu-qcom.c: the hypervisor turns S2CR "bypass"
 * into "fault"): the last context bank with translation off (S1 bypass),
 * and a free stream match entry for 0x16 pointing at it.
 */
#define SMMU		0x15000000UL
#define GPI_SID		0x16U

static uintptr_t smmu_cb;		/* the bypass context bank's registers, 0 if none */

/* Global fault (sGFSR: bit 1 = unmatched stream; sGFSYNR1 = stream ID) and the bypass bank's FSR/FAR. */
static void smmu_show_faults(const char *when)
{
	char buf[96], *p;

	p = fmt_str(buf, when);
	p = fmt_str(p, " gfsr=");
	p = fmt_hex(p, rd32(SMMU + 0x48), 8);
	p = fmt_str(p, " syn0=");
	p = fmt_hex(p, rd32(SMMU + 0x50), 8);
	p = fmt_str(p, " syn1=");
	p = fmt_hex(p, rd32(SMMU + 0x54), 8);
	if (smmu_cb) {
		p = fmt_str(p, " cbfsr=");
		p = fmt_hex(p, rd32(smmu_cb + 0x58), 8);
		p = fmt_str(p, " far=");
		fmt_hex(p, rd32(smmu_cb + 0x60), 8);
	}
	con_line("", buf);
}

static int smmu_bypass_gpi(char *buf)
{
	uint32_t idr0 = rd32(SMMU + 0x20), idr1 = rd32(SMMU + 0x24);
	uint32_t nsmr = idr0 & 0xff, ncb = idr1 & 0xff, pgshift = (idr1 >> 31) ? 16 : 12;
	uint32_t numpage = 1U << ((idr1 >> 28 & 7) + 1);
	int match = -1, free_smr = -1, valid = 0;
	char *p;

	if (nsmr > 128)
		nsmr = 128;
	for (uint32_t i = 0; i < nsmr; i++) {
		uint32_t smr = rd32(SMMU + 0x800 + 4 * i);
		if (!(smr >> 31)) {
			if (free_smr < 0)
				free_smr = (int)i;
			continue;
		}
		valid++;
		uint32_t mask = smr >> 16 & 0x7fff, id = smr & 0xffff;
		if (((id ^ GPI_SID) & ~mask) == 0 && match < 0)
			match = (int)i;
	}
	p = fmt_str(buf, "scr0=");
	p = fmt_hex(p, rd32(SMMU), 8);
	p = fmt_str(p, " smr ");
	p = fmt_dec(p, (uint64_t)valid);
	p = fmt_str(p, "/");
	p = fmt_dec(p, nsmr);
	p = fmt_str(p, " cb ");
	p = fmt_dec(p, ncb);
	p = fmt_str(p, " sid16: ");
	if (match >= 0) {
		p = fmt_str(p, "smr ");
		p = fmt_dec(p, (uint64_t)match);
		p = fmt_str(p, " s2cr=");
		fmt_hex(p, rd32(SMMU + 0xc00 + 4 * (uint32_t)match), 8);
		con_line("SMMU", buf);
		return 0;		/* already routed by the firmware: leave it */
	}
	p = fmt_str(p, "none");
	con_line("SMMU", buf);
	delay_s(3);				/* each risky step stays on screen */
	if (free_smr < 0 || ncb == 0)
		return -1;
	uint32_t cb = ncb - 1;
	smmu_cb = SMMU + ((uintptr_t)(numpage + cb) << pgshift);
	w32(SMMU + ((uintptr_t)(numpage + cb) << pgshift), 0);		/* CB SCTLR: translation off */
	w32(SMMU + (1UL << pgshift) + 4 * cb, 1U << 16);		/* GR1 CBAR: S1 (bypassed), S2 bypass */
	w32(SMMU + 0xc00 + 4 * (uint32_t)free_smr, cb);			/* S2CR: translate via cb */
	w32(SMMU + 0x800 + 4 * (uint32_t)free_smr, (1U << 31) | GPI_SID);	/* SMR: valid, exact match */
	p = fmt_str(buf, "sid16 -> smr ");
	p = fmt_dec(p, (uint64_t)free_smr);
	p = fmt_str(p, " cb ");
	p = fmt_dec(p, cb);
	p = fmt_str(p, " readback smr=");
	p = fmt_hex(p, rd32(SMMU + 0x800 + 4 * (uint32_t)free_smr), 8);
	p = fmt_str(p, " s2cr=");
	fmt_hex(p, rd32(SMMU + 0xc00 + 4 * (uint32_t)free_smr), 8);
	con_line("", buf);
	delay_s(3);
	return 0;
}

/* GENI side of GPI mode (geni_se_select_gpi_mode()), then a free GPII. */
static int gpi_setup(char *buf)
{
	char *p;
	int rc = -1;

	w32(SE + SE_M_IRQ_CLR, 0xffffffffU);
	w32(SE + 0x648, 0xffffffffU);		/* S_IRQ_CLEAR */
	w32(SE + 0xc44, 0xffffffffU);		/* DMA TX/RX IRQ clear */
	w32(SE + 0xd44, 0xffffffffU);
	w32(SE + 0xe1c, 0);			/* SE_IRQ_EN */
	w32(SE + 0x614, rd32(SE + 0x614) & ~(M_CMD_DONE | (1U << 26) | (1U << 27) | (1U << 30)));
	w32(SE + SE_DMA_MODE, 1U);		/* GENI_DMA_MODE_EN */
	w32(SE + 0xe18, rd32(SE + 0xe18) | 0xfU);	/* SE_GSI_EVENT_EN: DMA RX/TX, M, S */
	if (smmu_bypass_gpi(buf))
		return -1;

	/* States the firmware left (ch0 ch1 ev, 0 = free); the first free GPII of the mask is taken. */
	p = fmt_str(buf, "gpii");
	for (uint32_t n = 0; n < 8; n++) {
		if (!(GPI_MASK >> n & 1))
			continue;
		p = fmt_str(p, " ");
		p = fmt_dec(p, n);
		p = fmt_str(p, ":");
		p = fmt_hex(p, gpi_state(CH_CNTXT(n, 0)), 1);
		p = fmt_hex(p, gpi_state(CH_CNTXT(n, 1)), 1);
		p = fmt_hex(p, gpi_state(EV_CNTXT(n)), 1);
	}
	con_line("GPI", buf);
	for (uint32_t n = 0; n < 8 && rc; n++) {
		if (!(GPI_MASK >> n & 1) || gpi_state(CH_CNTXT(n, 0)) || gpi_state(CH_CNTXT(n, 1)) ||
		    gpi_state(EV_CNTXT(n)))
			continue;
		gpii = n;
		rc = gpi_init(n);
		p = fmt_str(buf, "init gpii ");
		p = fmt_dec(p, n);
		p = fmt_str(p, rc ? " FAILED rc=-" : " ok, channels started");
		if (rc)
			p = fmt_dec(p, (uint64_t)(-rc));
		p = fmt_str(p, " st=");
		p = fmt_hex(p, gpi_state(CH_CNTXT(n, 0)), 1);
		p = fmt_hex(p, gpi_state(CH_CNTXT(n, 1)), 1);
		p = fmt_hex(p, gpi_state(EV_CNTXT(n)), 1);
		p = fmt_str(p, " err=");
		fmt_hex(p, rd32(GPII(n, 0x23200)), 8);
		con_line("", buf);
		delay_s(3);
	}
	gpi_mode = rc == 0;
	return rc;
}

static int fts_read(const uint8_t *cmd, uint32_t clen, uint8_t *out, uint32_t len)
{
	if (gpi_mode)
		return gpi_fts_read(cmd, clen, out, len);
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
	/* Linux refuses FIFO mode when bit 0 here is set (GPI DMA only). */
	p = fmt_str(buf, "if_disable=");
	p = fmt_hex(p, rd32(SE + 0x064), 8);
	p = fmt_str(p, rd32(SE + 0x064) & 1 ? " FIFO OFF (DMA only)" : " fifo ok");
	con_line("", buf);
	if ((rd32(SE + 0x064) & 1) && gpi_setup(buf))
		return;
	p = fmt_str(buf, "geni st=");
	p = fmt_hex(p, rd32(SE + 0x040), 8);
	p = fmt_str(p, " scl rb=");
	p = fmt_hex(p, rd32(SE + SE_I2C_SCL), 8);	/* = 0x00503018 when writes stick */
	p = fmt_str(p, " irqen=");
	fmt_hex(p, rd32(SE + 0x614), 4);
	con_line("", buf);
	int rc = fts_read(id_cmd, sizeof(id_cmd), id, sizeof(id));
	gpi_trace = 0;
	p = fmt_str(buf, "after cmd: m_cmd0=");
	p = fmt_hex(p, rd32(SE + SE_M_CMD0), 8);
	p = fmt_str(p, " st=");
	p = fmt_hex(p, rd32(SE + 0x040), 8);
	p = fmt_str(p, " txcnt=");
	fmt_hex(p, rd32(SE + SE_TX_FIFO + 0x100) & 0xfffffff, 4);	/* TX_FIFO_STATUS 0x800 */
	con_line("", buf);
	p = fmt_str(buf, rc ? "chip id read failed rc=" : "chip id ");
	if (rc) {
		p = fmt_dec(p, (uint64_t)(-rc));
		p = fmt_str(p, " status=");
		p = fmt_hex(p, i2c_status, 8);
		if (gpi_mode) {
			p = fmt_str(p, " ev=");
			for (int i = 0; i < 4; i++) {
				p = fmt_hex(p, gpi_ev[i], 8);
				p = fmt_str(p, " ");
			}
		}
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

#ifdef VOLUP_PULLUP
/*
 * The one PMIC write, only in the -DVOLUP_PULLUP build (input-volup.img):
 * GPIO 6 DIG_PULL_CTL (0xc542) = 0, a 30 uA pull-up, as Linux configures it
 * for the volume-up key (pinctrl-spmi-gpio, bias-pull-up). EXT_WRITEL
 * (opcode 0) on this EE's read/write channel ("chnls", 0x10000 per APID),
 * only if the apps EE owns that APID.
 */
#define ARB_CHNLS	0x0c600000UL
static int spmi_write(uint16_t apid, uint16_t addr, uint8_t val)
{
	if (apid == NO_APID || (rd32(ARB_CNFG + 0x700 + 4UL * apid) & 7) != 0)
		return 0;
	uintptr_t ch = ARB_CHNLS + 0x10000UL * apid;
	*(volatile uint32_t *)(ch + 0x10) = val;			/* WDATA0 */
	__asm__ volatile("dsb osh" ::: "memory");
	*(volatile uint32_t *)ch = ((uint32_t)(addr & 0xff) << 4);	/* EXT_WRITEL, 1 byte */
	__asm__ volatile("dsb osh" ::: "memory");
	uint64_t end = now() + ticks_per_s() / 100;
	while (now() < end) {
		last_status = rd32(ch + 0x08);
		if (last_status & 1)
			return !(last_status & 0x0e);
	}
	return 0;
}
#endif

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
#ifdef VOLUP_PULLUP
	uint8_t pull = 0xff;
	spmi_read(gpio, 0xc542, &pull);
	int wok = spmi_write(gpio, 0xc542, 0x00);
	uint8_t pull2 = 0xff;
	spmi_read(gpio, 0xc542, &pull2);
	p = fmt_str(buf, "pull-up write ");
	p = fmt_str(p, wok ? "ok" : "FAILED");
	p = fmt_str(p, ": pull ");
	p = fmt_hex(p, pull, 2);
	p = fmt_str(p, " -> ");
	fmt_hex(p, pull2, 2);
	con_line("", buf);
#endif

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
