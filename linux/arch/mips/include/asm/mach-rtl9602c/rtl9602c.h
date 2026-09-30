/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Realtek RTL9602C: the addresses, interrupt numbers and bus clock that the
 * platform code in arch/mips/rtl9602c/ shares.
 */

#ifndef __ASM_MACH_RTL9602C_H
#define __ASM_MACH_RTL9602C_H

#include <linux/bits.h>

/*
 * The peripheral window, as a physical address. The platform code reaches
 * it through KSEG1, which maps the low 512 MiB of physical space unmapped
 * and uncached, so it needs no ioremap() and no access is cached.
 *
 * Some addresses stall the CPU on a load instead of answering or raising a
 * bus error, and the watchdog cannot be relied on to recover the board from
 * that state: the platform code reads only the blocks it drives.
 */
#define RTL9602C_IOMEM_BASE	0x18000000

/* The console UART, a 16550A */
#define RTL9602C_UART0_BASE	(RTL9602C_IOMEM_BASE + 0x2000)

/*
 * The second-level interrupt controller that delivers this CPU's
 * interrupts. The block at +0x3100 has the same layout but is a separate
 * controller: programming it delivers nothing here.
 */
#define RTL9602C_INTC_BASE	(RTL9602C_IOMEM_BASE + 0x3000)

/*
 * SoC interrupt source s is Linux interrupt RTL9602C_SOC_IRQ(s), just above
 * the CPU's eight lines. The numbers are fixed because the console UART is
 * registered with its interrupt before the interrupt domain exists.
 */
#define RTL9602C_SOC_IRQS	64
#define RTL9602C_SOC_IRQ_BASE	8
#define RTL9602C_SOC_IRQ(hw)	(RTL9602C_SOC_IRQ_BASE + (hw))

/*
 * Sources 32 to 63 reach the CPU only while this source of the lower bank
 * is enabled as well.
 */
#define RTL9602C_INTC_SRC_CASCADE	12

#define RTL9602C_INTC_SRC_TIMER0	43
#define RTL9602C_INTC_SRC_UART0	49

/* The LX bus clock, which the timers count; measured on this board */
#define RTL9602C_LX_HZ		200000000

#endif /* __ASM_MACH_RTL9602C_H */
