/*
 * Pixel 3 shim, step 2 of the ios-sweet port: act as iBoot for XNU.
 *
 * ABL loads this like a Linux kernel. The payload (tools/mkpayload.py:
 * kernel segments with fixups already applied, ADT, boot-args) is linked
 * into this image, so ABL's ramdisk placement does not matter.
 *
 * Before touching memory the shim checks the payload against the live
 * device tree: the kernel's RAM must be inside /memory and clear of every
 * /reserved-memory region with a fixed address. Then it copies the
 * segments to dram_base + (vmaddr - virt_base), puts the ADT and
 * boot_args after them, and jumps to the entry point with the MMU and
 * caches off and x0 = boot_args (physical), the way iBoot does.
 *
 * Any problem: the reason on screen, then back to fastboot.
 */
#include <stdint.h>
#include "board.h"
#include "fb.h"
#include "fdt.h"

#define PAGE_16K		0x4000UL
#define BOOT_LINE_LENGTH	608

#ifdef QEMU
#define PAUSE_SECONDS		1
#define FAIL_SECONDS		2
#else
#define PAUSE_SECONDS		3	/* time to photo the screen before XNU */
#define FAIL_SECONDS		60
#endif

/* tools/mkpayload.py */
struct payload_seg {
	uint64_t vmaddr, vmsize, data_off, data_size;
};

struct payload {
	char magic[8];
	uint32_t version, nsegs;
	uint64_t virt_base, entry, dram_base, dram_size, kernel_end, adt_off, adt_size;
	char cmdline[BOOT_LINE_LENGTH];
	struct payload_seg segs[];
};
_Static_assert(__builtin_offsetof(struct payload, segs) == 0x2a8, "payload header");

/* xnu pexpert/pexpert/arm64/boot.h, Revision 2 / Version 2 */
struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

struct boot_args {
	uint16_t Revision, Version;
	uint64_t virtBase, physBase, memSize, topOfKernelData;
	struct boot_video Video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char CommandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags, memSizeActual;
};
_Static_assert(__builtin_offsetof(struct boot_args, Video) == 40, "boot_args");
_Static_assert(__builtin_offsetof(struct boot_args, CommandLine) == 108, "boot_args");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args");

extern const char _payload_start[], _payload_end[], _head[], _end[];

/* ---- live device tree: RAM and fixed carve-outs ---- */

#define MAX_REGIONS 64

struct range {
	uint64_t base, size;
	const char *name;
};

static struct {
	struct range mem[MAX_REGIONS], rsv[MAX_REGIONS];
	int n_mem, n_rsv, overflow;
} dt;

static void add(struct range *r, int *n, const char *name, uint64_t base, uint64_t size)
{
	if (*n >= MAX_REGIONS) {
		dt.overflow = 1;
		return;
	}
	r[*n].name = name;
	r[*n].base = base;
	r[*n].size = size;
	(*n)++;
}

static void dt_cb(const struct fdt_prop *p, void *ctx)
{
	(void)ctx;
	if (!fdt_streq(p->name, "reg"))
		return;
	int memory = p->depth == 1 && fdt_startswith(p->node, "memory");
	int reserved = p->depth == 2 && fdt_streq(p->parent, "reserved-memory");
	if (!memory && !reserved)
		return;
	int step = 4 * (p->addr_cells + p->size_cells);
	for (uint32_t off = 0; step && off + step <= p->len; off += step) {
		uint64_t base = fdt_cells(p->val + off, p->addr_cells);
		uint64_t size = fdt_cells(p->val + off + 4 * p->addr_cells, p->size_cells);
		if (memory)
			add(dt.mem, &dt.n_mem, p->node, base, size);
		else
			add(dt.rsv, &dt.n_rsv, p->node, base, size);
	}
}

static int overlaps(uint64_t a, uint64_t alen, uint64_t b, uint64_t blen)
{
	return a < b + blen && b < a + alen;
}

static int inside_memory(uint64_t base, uint64_t size)
{
	for (int i = 0; i < dt.n_mem; i++)
		if (base >= dt.mem[i].base && base + size <= dt.mem[i].base + dt.mem[i].size)
			return 1;
	return 0;
}

/* ---- failure path ---- */

static void __attribute__((noreturn)) fail(const char *why, const char *detail)
{
	char buf[96], *p;
	con_skip();
	p = fmt_str(buf, why);
	if (detail) {
		p = fmt_str(p, ": ");
		fmt_str(p, detail);
	}
	con_at(0, con_row(), "FAILED", C_RED, C_BLACK);
	con_line("", buf);
	con_line("", "Photo this screen. Back to fastboot shortly.");
	delay_s(FAIL_SECONDS);
	reboot_to_fastboot();
}

/* ---- copying with the MMU off (Device memory: aligned 8-byte accesses) ---- */

static void copy64(uint64_t dst, uint64_t src, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	const volatile uint64_t *s = (const volatile uint64_t *)src;
	for (uint64_t i = 0; i < (n + 7) / 8; i++)
		d[i] = s[i];
}

static void zero64(uint64_t dst, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	for (uint64_t i = 0; i < (n + 7) / 8; i++)
		d[i] = 0;
}

static int magic_ok(const char *m)
{
	static const char want[] = "PIXEL3PL";
	for (int i = 0; i < 8; i++)
		if (m[i] != want[i])
			return 0;
	return 1;
}

typedef void (*xnu_entry_t)(uint64_t boot_args);

void entry_main(uintptr_t dtb, uintptr_t load)
{
	const struct payload *pl = (const struct payload *)_payload_start;
	uint64_t pl_size = (uint64_t)(_payload_end - _payload_start);
	char buf[96], *p;

	con_reset(C_BLACK);
	con_at(0, 0, "PIXEL 3 SHIM p2   ios-sweet port, XNU loader", C_GREEN, C_BLACK);
	con_set_row(2);
	con_kv("LOADED", "at=", load, "dtb=", dtb);
	con_kv("IMAGE", "end=", load + (uint64_t)(_end - _head), "payload=", pl_size);

	if (!fdt_valid(dtb))
		fail("no valid device tree from the bootloader", 0);
	if (fdt_walk(dtb, dt_cb, 0))
		fail("device tree walk failed", 0);
	if (dt.overflow)
		fail("too many memory/reserved regions", 0);
	con_kv("DT", "memory=", (uint64_t)dt.n_mem, "fixed carve-outs=", (uint64_t)dt.n_rsv);
	if (overlaps(dtb, fdt_size(dtb), load, (uint64_t)(_end - _head)))
		fail("device tree lies inside our image", 0);

	if (pl_size < sizeof(*pl) || !magic_ok(pl->magic) || pl->version != 1)
		fail("no PIXEL3PL payload in this image", 0);
	if (pl->adt_off + pl->adt_size > pl_size || (pl->adt_off & 7))
		fail("payload: bad ADT offset", 0);
	for (uint32_t i = 0; i < pl->nsegs; i++)
		if (pl->segs[i].data_off + pl->segs[i].data_size > pl_size ||
		    pl->segs[i].data_size > pl->segs[i].vmsize || (pl->segs[i].data_off & 7))
			fail("payload: bad segment", 0);

	uint64_t base = pl->dram_base, size = pl->dram_size;
	uint64_t pa_off = base - pl->virt_base;		/* pa = va + pa_off */
	con_kv("DRAM", "base=", base, "size=", size);
	con_kv("ENTRY", "va=", pl->entry, "pa=", pl->entry + pa_off);

	/* The kernel's RAM: real memory, free of fixed carve-outs, not us. */
	if (!inside_memory(base, size))
		fail("kernel RAM is not inside /memory", 0);
	for (int i = 0; i < dt.n_rsv; i++)
		if (overlaps(base, size, dt.rsv[i].base, dt.rsv[i].size))
			fail("kernel RAM overlaps carve-out", dt.rsv[i].name);
	if (overlaps(base, size, load, (uint64_t)(_end - _head)) || overlaps(base, size, dtb, fdt_size(dtb)))
		fail("kernel RAM overlaps the shim or the DTB", 0);
	if (overlaps(base, size, FB_BASE, FB_STRIDE * FB_H))
		fail("kernel RAM overlaps the framebuffer", 0);

	/* Layout behind the kernel: ADT, then one page of boot_args. */
	uint64_t adt_pa = pl->kernel_end + pa_off;
	uint64_t ba_pa = (adt_pa + pl->adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1);
	uint64_t top = ba_pa + PAGE_16K;
	if (pl->kernel_end < pl->virt_base || top > base + size / 2)
		fail("payload does not fit the kernel RAM", 0);
	con_kv("ADT", "pa=", adt_pa, "size=", pl->adt_size);
	con_kv("BOOTARGS", "pa=", ba_pa, "top=", top);
	p = fmt_str(buf, "");
	for (int i = 0; i < BOOT_LINE_LENGTH && pl->cmdline[i] && i < CON_COLS - 10; i++)
		*p++ = pl->cmdline[i];
	*p = 0;
	con_line("CMDLINE", buf);

	p = fmt_str(buf, "jumping to XNU in ");
	p = fmt_dec(p, PAUSE_SECONDS);
	fmt_str(p, "s");
	con_skip();
	con_line("XNU", buf);
	delay_s(PAUSE_SECONDS);

	for (uint32_t i = 0; i < pl->nsegs; i++) {
		const struct payload_seg *s = &pl->segs[i];
		uint64_t dst = s->vmaddr + pa_off;
		uint64_t data = (s->data_size + 7) & ~7UL;
		copy64(dst, (uint64_t)_payload_start + s->data_off, s->data_size);
		if (s->vmsize > data)
			zero64(dst + data, s->vmsize - data);
	}
	copy64(adt_pa, (uint64_t)_payload_start + pl->adt_off, pl->adt_size);

	zero64(ba_pa, PAGE_16K);
	volatile struct boot_args *ba = (volatile struct boot_args *)ba_pa;
	ba->Revision = 2;
	ba->Version = 2;
	ba->virtBase = pl->virt_base;
	ba->physBase = base;
	ba->memSize = size;
	ba->topOfKernelData = top;
	ba->Video.v_baseAddr = FB_BASE;
	ba->Video.v_display = 1;
	ba->Video.v_rowBytes = FB_STRIDE;
	ba->Video.v_width = FB_W;
	ba->Video.v_height = FB_H;
	ba->Video.v_depth = 32;
	ba->deviceTreeP = adt_pa - pa_off;	/* XNU reads it after the MMU is on: a VA */
	ba->deviceTreeLength = (uint32_t)pl->adt_size;
	for (int i = 0; i < BOOT_LINE_LENGTH; i++)
		ba->CommandLine[i] = pl->cmdline[i];
	ba->memSizeActual = size;

	/* Written with the MMU and caches off; drop stale instruction lines. */
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	((xnu_entry_t)(pl->entry + pa_off))(ba_pa);
	fail("XNU returned", 0);
}

/* Any exception before XNU runs: syndrome on screen, then fastboot. */
void exc_handler(uint64_t slot)
{
	con_reset(C_PANIC);
	con_at(0, 0, "PIXEL 3 SHIM p2   EXCEPTION", C_YELLOW, C_PANIC);
	con_set_row(2);
	con_kv("SLOT", "n=", slot, 0, 0);
	con_kv("ESR", "", rsr("esr_el1"), 0, 0);
	con_kv("ELR", "", rsr("elr_el1"), 0, 0);
	con_kv("FAR", "", rsr("far_el1"), 0, 0);
	con_kv("SPSR", "", rsr("spsr_el1"), 0, 0);
	con_skip();
	con_line("", "Photo this screen. Back to fastboot in 60s.");
	delay_s(FAIL_SECONDS);
	reboot_to_fastboot();
}
