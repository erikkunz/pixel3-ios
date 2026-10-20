#include "board.h"

#ifdef QEMU
#define CALL_INSN	"hvc #0"
#else
#define CALL_INSN	"smc #0"
#endif

/* The compiler may emit calls to these even in freestanding code. */
void *memset(void *d, int c, unsigned long n)
{
	volatile unsigned char *p = d;
	while (n--)
		*p++ = (unsigned char)c;
	return d;
}

void *memcpy(void *d, const void *s, unsigned long n)
{
	volatile unsigned char *dp = d;
	const volatile unsigned char *sp = s;
	while (n--)
		*dp++ = *sp++;
	return d;
}

uint64_t smc(uint64_t fn)
{
	register uint64_t x0 __asm__("x0") = fn;
	__asm__ volatile(CALL_INSN
			 : "+r"(x0)
			 :
			 : "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9",
			   "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

uint64_t now(void)
{
	__asm__ volatile("isb" ::: "memory");
	return rsr("cntpct_el0");
}

uint64_t ticks_per_s(void)
{
	uint64_t f = rsr("cntfrq_el0");
	return f ? f : 19200000;	/* SDM845 arch timer runs at 19.2 MHz */
}

void delay_s(unsigned s)
{
	uint64_t end = now() + ticks_per_s() * s;
	while (now() < end)
		;
}

void reboot_to_fastboot(void)
{
#ifdef QEMU
	for (;;)
		__asm__ volatile("wfe");
#endif
	*(volatile uint32_t *)IMEM_RESTART_REASON = RESTART_BOOTLOADER;
	__asm__ volatile("dsb sy" ::: "memory");
	smc(PSCI_SYSTEM_RESET);
	*(volatile uint32_t *)PSHOLD = 0;	/* if PSCI returned */
	for (;;)
		__asm__ volatile("wfe");
}
