// SPDX-License-Identifier: GPL-2.0-only
/*
 * Platform setup of the Realtek RTL9602C: the command line from the boot
 * loader, memory and the built-in device tree, the console UART, halt, the
 * timers, and the write-buffer flush. The watchdog's driver restarts the
 * board.
 */

#include <linux/clk-provider.h>
#include <linux/clocksource.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/memblock.h>
#include <linux/of_fdt.h>
#include <linux/of_platform.h>
#include <linux/serial_8250.h>
#include <linux/serial_core.h>
#include <linux/serial_reg.h>

#include <asm/bootinfo.h>
#include <asm/prom.h>
#include <asm/reboot.h>
#include <asm/fw/fw.h>
#include <asm/time.h>
#include <asm/wbflush.h>

#include <asm/mach-rtl9602c/rtl9602c.h>

/*
 * The board's DRAM. It is added to memblock before the device tree is
 * parsed, so that early allocations work whatever the tree says; the tree's
 * memory node describes the same range.
 */
#define RTL9602C_RAM_BASE	0x00000000
#define RTL9602C_RAM_SIZE	0x02000000

/*
 * The boot loader leaves the console UART at 115200 baud, the rate the
 * console keeps. RTL9602C_UART_CLK is the clock the port gets when the boot
 * loader's divisor cannot be read; the 8250 core rounds it to divisor 107.
 * 16550 register n sits at byte offset 4 * n.
 */
#define RTL9602C_CONSOLE_BAUD	115200
#define RTL9602C_UART_CLK	(RTL9602C_LX_HZ - RTL9602C_CONSOLE_BAUD * 24)
#define RTL9602C_UART_SHIFT	2

/**
 * get_system_type() - name the machine
 *
 * Return: the system type line of /proc/cpuinfo.
 */
const char *get_system_type(void)
{
	return "Realtek RTL9602C";
}

/*
 * Halt and power off. The board cannot switch off its own supply, which
 * comes from the host's SFP cage, so both disable interrupts and spin. Not
 * verified on the board.
 */
static void rtl9602c_machine_halt(void)
{
	local_irq_disable();
	while (1)
		cpu_relax();
}

/**
 * prom_init() - take the command line from the boot loader
 *
 * The boot loader passes the argument count in a0 and an argument vector in
 * a1; fw_init_cmdline() joins the arguments after argv[0] into the firmware
 * command line.
 */
void __init prom_init(void)
{
	fw_init_cmdline();
}

/*
 * Register the console UART in the 8250 driver's table of early ports, so
 * that it is there when console_init() runs. The early console is not a tty
 * and cannot back /dev/console: without this port, init runs with no output
 * at all.
 *
 * The port's clock is chosen to keep the divisor the boot loader left: the
 * 8250 core programs DIV_ROUND_CLOSEST(uartclk, 16 * baud), which for
 * divisor * 16 * 115200 at 115200 baud is that divisor again. The figure
 * preserves a divisor that works; it is not a measurement of the UART's
 * input clock. A divisor of 0 (never programmed) or 0xffff (nothing
 * answering) gives no reference, and the port then gets RTL9602C_UART_CLK,
 * with a warning. That fallback is not verified, since this board's boot
 * loader always leaves 109; the divisor it gives, 107, works at 115200 but
 * garbles a character at the hand-over from 109 (measured on this board).
 */
static void __init rtl9602c_serial_init(void)
{
	void __iomem *base = (void __iomem *)KSEG1ADDR(RTL9602C_UART0_BASE);
	struct uart_port up;
	unsigned int theirs, ours;
	u8 lcr;

	/*
	 * Read the divisor latch: set DLAB, read DLL and DLM, restore LCR.
	 * Nothing may be printed in between: with DLAB set, register 0 is DLL,
	 * and a transmitted byte would become the low divisor byte.
	 */
	lcr = __raw_readb(base + (UART_LCR << RTL9602C_UART_SHIFT));
	__raw_writeb(lcr | UART_LCR_DLAB,
		     base + (UART_LCR << RTL9602C_UART_SHIFT));
	theirs = __raw_readb(base + (UART_DLL << RTL9602C_UART_SHIFT)) |
		 __raw_readb(base + (UART_DLM << RTL9602C_UART_SHIFT)) << 8;
	__raw_writeb(lcr, base + (UART_LCR << RTL9602C_UART_SHIFT));

	if (theirs == 0 || theirs == 0xffff) {
		ours = RTL9602C_UART_CLK;
		pr_warn("rtl9602c: no usable uart divisor from the bootloader (%u); assuming a %u Hz clock\n",
			theirs, ours);
	} else {
		ours = theirs * 16 * RTL9602C_CONSOLE_BAUD;
	}

	/*
	 * With UPF_BOOT_AUTOCONF the 8250 core detects the type and the FIFO
	 * itself, so PORT_16550A and a FIFO of 1 are only starting values.
	 * UPF_SKIP_TEST skips its loopback test, which this UART is not
	 * verified to pass. The interrupt makes console input
	 * interrupt-driven: polled at 115200 baud, a pasted line overflows the
	 * receive FIFO (measured on this board).
	 */
	memset(&up, 0, sizeof(up));
	up.line = 0;
	up.type = PORT_16550A;
	up.iotype = UPIO_MEM;
	up.regshift = RTL9602C_UART_SHIFT;
	up.membase = base;
	up.mapbase = RTL9602C_UART0_BASE;
	up.uartclk = ours;
	up.irq = RTL9602C_SOC_IRQ(RTL9602C_INTC_SRC_UART0);
	up.fifosize = 1;
	up.flags = UPF_SKIP_TEST | UPF_BOOT_AUTOCONF;

	if (early_serial_setup(&up)) {
		pr_err("rtl9602c: could not register the console UART\n");
		return;
	}

	/*
	 * The divisor in parentheses is computed from RTL9602C_UART_CLK, not
	 * from the 200 MHz the message names: the 8250 core would round a
	 * 200 MHz clock to divisor 109 as well.
	 */
	pr_info("rtl9602c: uart clock %u Hz, from the divisor %u the bootloader left (a %u Hz bus would have given %u)\n",
		ours, theirs, RTL9602C_LX_HZ,
		RTL9602C_UART_CLK / (16 * RTL9602C_CONSOLE_BAUD));
}

/**
 * plat_mem_setup() - platform setup, called early from setup_arch()
 *
 * Adds the DRAM to memblock, hands the built-in device tree to the OF core,
 * registers the console UART and installs halt and power off.
 *
 * A watchdog count that the boot loader armed for a trial boot runs on into
 * the kernel, until the watchdog's driver takes it over.
 */
void __init plat_mem_setup(void)
{
	memblock_add(RTL9602C_RAM_BASE, RTL9602C_RAM_SIZE);

	/*
	 * The boot loader passes no device tree. The kernel links in this
	 * board's, the only blob it carries.
	 */
	__dt_setup_arch(__dtb_start);

	rtl9602c_serial_init();

	_machine_halt = rtl9602c_machine_halt;
	pm_power_off = rtl9602c_machine_halt;
}

/**
 * plat_time_init() - set up the tick and the clock
 *
 * The LX bus clock is a fixed clock of the device tree, which the timer
 * driver needs before the timers: the Otto timer driver then makes timer 0
 * the clock event device and timer 1 the clocksource and sched_clock.
 */
void __init plat_time_init(void)
{
	of_clk_init(NULL);
	timer_probe();
}

/*
 * Create the devices of the tree, those on the soc bus and the flash at the
 * root, at arch_initcall, so that they exist before any driver's
 * device_initcall probes.
 */
static int __init rtl9602c_populate_dt(void)
{
	return of_platform_default_populate(NULL, NULL, NULL);
}
arch_initcall(rtl9602c_populate_dt);

/*
 * CPU_R3000 selects CPU_HAS_WB: the core is taken to have a write buffer
 * between it and the bus, and mb() and iob() call __wbflush() to drain it.
 * The pointer is set at compile time rather than by wbflush_setup(), because
 * code runs before plat_mem_setup(), the early console first, and a pointer
 * filled in at run time would still be NULL then.
 */
static void rtl9602c_wbflush(void);

void (*__wbflush)(void) = rtl9602c_wbflush;
EXPORT_SYMBOL(__wbflush);

/*
 * An uncached load from physical 0, taken to wait until the writes buffered
 * before it have drained. That is the R3000-class convention; it is not
 * verified on this core.
 */
static void rtl9602c_wbflush(void)
{
	__asm__ __volatile__(".set	push\n\t"
		".set	noreorder\n\t"
		"lw	$0, %0\n\t"
		"nop\n\t"
		".set	pop"
		:
		: "m" (*(int *)KSEG1)
		: "memory");
}

/**
 * wbflush_setup() - install the write-buffer flush
 *
 * Sets __wbflush to this platform's flush, which it already holds.
 * asm/wbflush.h declares this function for platforms that choose their flush
 * at run time; nothing calls it on this one.
 */
void __init wbflush_setup(void)
{
	__wbflush = rtl9602c_wbflush;
}
