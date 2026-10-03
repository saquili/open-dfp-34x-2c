// SPDX-License-Identifier: GPL-2.0-only
/*
 * Early console of the Realtek RTL9602C.
 *
 * The MIPS early console calls prom_putchar() for every character from
 * setup_early_printk() on, until the 8250 driver takes over the console
 * UART. It has to work where nothing else does, so it polls the UART
 * directly, takes no lock and never waits without a bound.
 */

#include <linux/io.h>
#include <linux/serial_reg.h>

#include <asm/addrspace.h>
#include <asm/setup.h>
#include <asm/mach-rtl9602c/rtl9602c.h>

/*
 * 16550 register n sits at byte offset 4 * n and is accessed as a byte,
 * which on this big-endian bus is the most significant byte lane of its
 * 32-bit slot: register 0 (transmit holding), 2 (FIFO control) and 5 (line
 * status).
 */
#define RTL9602C_UART_THR	0x00
#define RTL9602C_UART_FCR	0x08
#define RTL9602C_UART_LSR	0x14

/*
 * Written when the transmitter stays busy: the transmit FIFO is reset, and the
 * FIFO-enable bit keeps both FIFOs on, which a 16550 written without it turns
 * off and empties. Whether the write frees a stuck transmitter on this UART is
 * not verified.
 */
#define RTL9602C_UART_FCR_TX_FIFO_RESET \
	(UART_FCR_ENABLE_FIFO | UART_FCR_CLEAR_XMIT | UART_FCR_TRIGGER_14)

/* Polls for an empty holding register before giving up; a software bound */
#define RTL9602C_UART_TX_POLL_MAX	30000

/* Through KSEG1: unmapped and uncached, usable before any mapping exists */
static void __iomem *uart_base =
	(void __iomem *)KSEG1ADDR(RTL9602C_UART0_BASE);

/**
 * prom_putchar() - write one character to the console UART
 * @c: the character
 *
 * Polls the line status until the transmit holding register can take a
 * character, then writes it. If the register is still full after
 * RTL9602C_UART_TX_POLL_MAX polls, the character is dropped and the FIFO
 * control register is written in an attempt to free the transmitter: a
 * stuck transmitter must not hang the boot at a point where nothing can
 * report it.
 */
void prom_putchar(char c)
{
	unsigned int busy = 0;

	while (!(__raw_readb(uart_base + RTL9602C_UART_LSR) & UART_LSR_THRE)) {
		if (++busy >= RTL9602C_UART_TX_POLL_MAX) {
			__raw_writeb(RTL9602C_UART_FCR_TX_FIFO_RESET,
				     uart_base + RTL9602C_UART_FCR);
			return;
		}
	}

	__raw_writeb(c, uart_base + RTL9602C_UART_THR);
}
