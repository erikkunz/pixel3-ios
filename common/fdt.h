#pragma once
#include <stdint.h>

struct fdt_prop {
	int depth;		/* 0 = root node */
	const char *node;	/* this node's name ("" for root) */
	const char *parent;	/* parent node's name */
	const char *name;	/* property name */
	uintptr_t val;		/* property value (big endian) */
	uint32_t len;
	int addr_cells, size_cells;	/* from the parent, for "reg" */
};

typedef void (*fdt_prop_cb)(const struct fdt_prop *p, void *ctx);

int fdt_valid(uintptr_t fdt);
uint32_t fdt_size(uintptr_t fdt);
int fdt_walk(uintptr_t fdt, fdt_prop_cb cb, void *ctx);	/* 0 on success */
uint32_t fdt_be32(uintptr_t a);
uint64_t fdt_cells(uintptr_t val, int n);
int fdt_streq(const char *a, const char *b);
int fdt_startswith(const char *s, const char *prefix);
