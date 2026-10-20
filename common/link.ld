ENTRY(_head)
SECTIONS
{
	. = 0;
	.text : { *(.text.head) *(.text*) }
	.rodata : { *(.rodata*) }
	.data : { *(.data*) }
	/* Optional blob linked into the image (the shim's XNU payload). */
	. = ALIGN(16);
	.payload : { _payload_start = .; KEEP(*(.payload)) _payload_end = .; }
	. = ALIGN(16);
	_bss_start = .;
	.bss : { *(.bss*) *(COMMON) }
	. = ALIGN(16);
	_bss_end = .;
	. += 0x10000;
	. = ALIGN(16);
	_stack_top = .;
	. = ALIGN(4096);
	_end = .;
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
