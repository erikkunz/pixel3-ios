/*
 * Pixel 3 (blueline) bare-metal programs: entry point.
 *
 * The file starts with an arm64 Linux Image header so the Pixel 3
 * bootloader (ABL) loads and enters it like a kernel: EL1, MMU and
 * D-cache off, x0 = physical address of the device tree it prepared.
 *
 * text_offset is 0x80000 on purpose: mainline Linux notes that recent
 * Pixel 3 bootloaders refuse an Image whose header has text_offset 0.
 * The code is position independent (adr only), so the load address
 * does not matter.
 */
	.section .text.head, "ax"
	.globl _head
_head:
	b	_start			/* code0 */
	.long	0			/* code1 */
	.quad	0x80000			/* text_offset */
	.quad	_end			/* image_size (linked at 0, file padded to it) */
	.quad	0xa			/* flags: little endian, 4K pages, anywhere */
	.quad	0			/* res2 */
	.quad	0			/* res3 */
	.quad	0			/* res4 */
	.ascii	"ARM\x64"		/* magic */
	.long	0			/* res5 */

	.text
	.globl _start
_start:
	msr	daifset, #0xf
	mov	x19, x0			/* DTB from ABL */
	msr	spsel, #1
	adr	x1, _stack_top
	mov	sp, x1
	adr	x1, vectors
	msr	vbar_el1, x1
	isb
	adr	x1, _bss_start
	adr	x2, _bss_end
1:	cmp	x1, x2
	b.hs	2f
	str	xzr, [x1], #8
	b	1b
2:	mov	x0, x19
	adr	x1, _head		/* where ABL put us */
	bl	entry_main
3:	wfe
	b	3b

/*
 * Every exception goes to exc_handler(slot), which draws a panic
 * screen and resets. Nothing here expects to return.
 */
.macro vent slot
	.balign	0x80
	mov	x0, #\slot
	b	exc_entry
.endm

	.balign	0x800
	.globl vectors
vectors:
	vent 0
	vent 1
	vent 2
	vent 3
	vent 4
	vent 5
	vent 6
	vent 7
	vent 8
	vent 9
	vent 10
	vent 11
	vent 12
	vent 13
	vent 14
	vent 15

exc_entry:
	adr	x1, _stack_top		/* fresh stack: the old one may be the problem */
	mov	sp, x1
	bl	exc_handler
4:	wfe
	b	4b
