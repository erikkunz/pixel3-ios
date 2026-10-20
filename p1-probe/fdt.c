/*
 * Minimal flattened device tree walker (read-only, no allocation).
 * Calls cb() for every property, with the node's own name, its parent's
 * name, and the #address-cells/#size-cells that apply to its "reg".
 */
#include "fdt.h"

#define FDT_MAGIC	0xd00dfeed
#define FDT_BEGIN_NODE	1
#define FDT_END_NODE	2
#define FDT_PROP	3
#define FDT_NOP		4
#define FDT_END		9
#define MAX_DEPTH	16

uint32_t fdt_be32(uintptr_t a)
{
	return __builtin_bswap32(*(volatile uint32_t *)a);
}

int fdt_streq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

int fdt_startswith(const char *s, const char *prefix)
{
	while (*prefix)
		if (*s++ != *prefix++)
			return 0;
	return 1;
}

int fdt_valid(uintptr_t fdt)
{
	return fdt && !(fdt & 3) && fdt_be32(fdt) == FDT_MAGIC;
}

uint32_t fdt_size(uintptr_t fdt)
{
	return fdt_be32(fdt + 4);
}

uint64_t fdt_cells(uintptr_t val, int n)
{
	uint64_t v = 0;
	for (int i = 0; i < n; i++)
		v = v << 32 | fdt_be32(val + 4 * i);
	return v;
}

static uintptr_t skip_str(uintptr_t p)
{
	while (*(volatile char *)p)
		p++;
	return p + 1;
}

int fdt_walk(uintptr_t fdt, fdt_prop_cb cb, void *ctx)
{
	if (!fdt_valid(fdt))
		return -1;
	uintptr_t p = fdt + fdt_be32(fdt + 8);		/* off_dt_struct */
	uintptr_t strings = fdt + fdt_be32(fdt + 12);	/* off_dt_strings */
	const char *names[MAX_DEPTH];
	int ac[MAX_DEPTH], sc[MAX_DEPTH];
	int depth = -1;

	for (int guard = 0; guard < 2000000; guard++) {
		uint32_t tok = fdt_be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE:
			if (++depth >= MAX_DEPTH)
				return -2;
			names[depth] = (const char *)p;
			ac[depth] = 2;	/* defaults per the DT spec */
			sc[depth] = 1;
			p = (skip_str(p) + 3) & ~3UL;
			break;
		case FDT_END_NODE:
			if (--depth < -1)
				return -3;
			break;
		case FDT_PROP: {
			uint32_t len = fdt_be32(p), nameoff = fdt_be32(p + 4);
			uintptr_t val = p + 8;
			const char *pname = (const char *)(strings + nameoff);
			if (depth >= 0) {
				if (fdt_streq(pname, "#address-cells") && len == 4)
					ac[depth] = fdt_be32(val);
				else if (fdt_streq(pname, "#size-cells") && len == 4)
					sc[depth] = fdt_be32(val);
				struct fdt_prop pr = {
					.depth = depth,
					.node = names[depth],
					.parent = depth > 0 ? names[depth - 1] : "",
					.name = pname,
					.val = val,
					.len = len,
					.addr_cells = depth > 0 ? ac[depth - 1] : 2,
					.size_cells = depth > 0 ? sc[depth - 1] : 1,
				};
				cb(&pr, ctx);
			}
			p = (val + len + 3) & ~3UL;
			break;
		}
		case FDT_NOP:
			break;
		case FDT_END:
			return 0;
		default:
			return -4;
		}
	}
	return -5;
}
