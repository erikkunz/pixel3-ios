/*
 * Stand-in for XNU to test the shim end to end in QEMU (test/run-qemu.sh).
 *
 * Built as an arm64 Mach-O linked at a kernel VA, like XNU, with chained
 * fixups. Entered like XNU: MMU off, running at the physical copy,
 * x0 = boot_args. It checks what the shim and mkpayload.py produced and
 * paints the screen: green if everything holds, otherwise red with one
 * white bar per failed check (counted from the left).
 */
#include <stdint.h>

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
	char CommandLine[608];
	uint64_t bootFlags, memSizeActual;
};

/* Needs a chained rebase: the shim must see the fixed-up VA. */
static const char marker[] = "pixel3 testkernel";
static const char *volatile marker_ptr = marker;

/* Zero-filled tail of the data segment: the shim must clear it. */
static volatile uint64_t zeroed[4096];

static int eq(const char *a, const char *b, int n)
{
	for (int i = 0; i < n; i++)
		if (a[i] != b[i])
			return 0;
	return 1;
}

void test_main(struct boot_args *ba)
{
	uint64_t pa_off = ba->physBase - ba->virtBase;
	int bad[8] = {0};

	bad[0] = !(ba->Revision == 2 && ba->Version == 2);
	bad[1] = !(ba->physBase == 0xf4000000 && ba->memSize == 0x8a000000);
	/* ADT: root node, first property "name" = "device-tree" */
	const char *adt = (const char *)(ba->deviceTreeP + pa_off);
	bad[2] = !(ba->deviceTreeLength > 64 && eq(adt + 8, "name", 5) && eq(adt + 8 + 36, "device-tree", 12));
	bad[3] = !eq(ba->CommandLine, "testkernel ok", 13);
	bad[4] = !eq((const char *)((uint64_t)marker_ptr + pa_off), "pixel3 testkernel", 18);
	uint64_t nz = 0;
	for (int i = 0; i < 4096; i++)
		nz |= zeroed[i];
	bad[5] = nz != 0;
	bad[6] = ba->topOfKernelData < (uint64_t)ba + 0x4000;
	bad[7] = ba->Video.v_width != 1080 || ba->Video.v_height != 2160;

	int fails = 0;
	for (int i = 0; i < 8; i++)
		fails += bad[i];
	volatile uint32_t *fb = (volatile uint32_t *)ba->Video.v_baseAddr;
	uint32_t w = (uint32_t)ba->Video.v_width, h = (uint32_t)ba->Video.v_height;
	uint32_t stride = (uint32_t)ba->Video.v_rowBytes / 4;
	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < w; x++) {
			uint32_t c = fails ? 0xffc00000 : 0xff00c000;
			if (y > h / 2 && y < h / 2 + 200 && x / 120 < 8 && bad[x / 120] && x % 120 < 100)
				c = 0xffffffff;
			fb[y * stride + x] = c;
		}
	for (;;)
		__asm__ volatile("wfe");
}

/* Entry (LC_UNIXTHREAD pc): stack in the boot_args page, then C. */
__asm__(
	".globl _start\n"
	"_start:\n"
	"	add	sp, x0, #0x4000\n"
	"	b	_test_main\n");
