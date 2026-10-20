/*
 * Entry for secondary cores started with PSCI CPU_ON (x0 = context =
 * core index). MMU and caches are off, as on the boot core. Each core
 * gets a 4 KiB stack, reports in secondary_main() and parks.
 */
	.text
	.globl secondary_entry
secondary_entry:
	msr	daifset, #0xf
	msr	spsel, #1
	adrp	x1, sec_stacks
	add	x1, x1, :lo12:sec_stacks
	add	x2, x0, #1
	lsl	x2, x2, #12
	add	sp, x1, x2
	adr	x1, vectors
	msr	vbar_el1, x1
	isb
	bl	secondary_main
1:	wfe
	b	1b

	.bss
	.balign	16
sec_stacks:
	.space	8 * 4096
