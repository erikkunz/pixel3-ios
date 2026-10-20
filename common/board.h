#pragma once
#include <stdint.h>

/*
 * Pixel 3 board helpers shared by the probe and the shim.
 * Built with -DQEMU for the emulator: PSCI via hvc and no reset.
 */

/* Downstream sdm845.dtsi: qcom,msm-imem@146bf000, restart_reason@65c. */
#define IMEM_RESTART_REASON	0x146bf65cUL
#define RESTART_BOOTLOADER	0x77665500U
#define PSHOLD			0x0c264000UL	/* qcom,pshold */

#define PSCI_VERSION		0x84000000UL
#define PSCI_SYSTEM_RESET	0x84000009UL
#define SMCCC_VERSION		0x80000000UL

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

uint64_t smc(uint64_t fn);		/* PSCI/SMCCC call, no arguments */
uint64_t now(void);			/* CNTPCT */
uint64_t ticks_per_s(void);
void delay_s(unsigned s);
void __attribute__((noreturn)) reboot_to_fastboot(void);

/* Defined by each program; start.S calls it with the DTB and load address. */
void entry_main(uintptr_t dtb, uintptr_t load);
void exc_handler(uint64_t slot);
