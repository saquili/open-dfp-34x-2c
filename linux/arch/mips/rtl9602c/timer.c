// SPDX-License-Identifier: GPL-2.0-only
/*
 * The tick and the clock of the Realtek RTL9602C.
 *
 * MIPS I defines no CP0 Count and Compare registers, and the kernel uses no
 * CP0 counter on this core: the tick comes from general-purpose timer 0 of
 * the SoC, a periodic clockevent that counts the LX bus clock divided by
 * 1000 and expires once per jiffy. There is no one-shot mode, so the kernel
 * runs a periodic tick. Timer 1, which the boot loader leaves running, is
 * set free-running at 100 MHz and is the clocksource and sched_clock, with
 * its interrupt disabled.
 */

#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/sched_clock.h>

#include <asm/time.h>

#include <asm/mach-rtl9602c/rtl9602c.h>

/*
 * General-purpose timers, 0x10 apart. Per timer: the reload value, in
 * counts per period, then the current count, control and interrupt. The
 * count runs up from 0 to the reload value and starts again at 0; both are
 * 28 bits wide, and writing the reload value sets the count to 0.
 */
#define RTL9602C_TIMER_BASE	(RTL9602C_IOMEM_BASE + 0x3200)
#define RTL9602C_TIMER_STRIDE		0x10
#define RTL9602C_TIMER_RELOAD		0x00
#define  RTL9602C_TIMER_RELOAD_MAX	0x0fffffff
#define RTL9602C_TIMER_COUNT		0x04
#define RTL9602C_TIMER_BITS		28

/*
 * Control: EN starts the timer, PERIODIC makes it reload and expire
 * repeatedly, and the low bits hold the clock divisor, in a field whose width
 * is not verified. Interrupt: IRQ_EN lets the timer raise its interrupt, and
 * PENDING is set on expiry and cleared by writing 1 to it.
 */
#define RTL9602C_TIMER_CTRL		0x08
#define  RTL9602C_TIMER_CTRL_EN		BIT(28)
#define  RTL9602C_TIMER_CTRL_PERIODIC	BIT(24)
#define RTL9602C_TIMER_IRQ		0x0c
#define  RTL9602C_TIMER_IRQ_EN		BIT(20)
#define  RTL9602C_TIMER_IRQ_PENDING		BIT(16)

/* Timer 0 drives the tick, timer 1 the clock. */
#define RTL9602C_TIMER_TICK_IDX		0
#define RTL9602C_TIMER_CLOCK_IDX	1

/*
 * The timers count the LX bus clock divided by RTL9602C_TIMER_DIVISOR,
 * 200 kHz (measured on this board). Whether a timer divides by the divisor
 * and the reload value exactly or by one more each is not verified; it
 * would make an error of about 0.1 %.
 */
#define RTL9602C_TIMER_DIVISOR		1000
#define RTL9602C_TIMER_HZ		(RTL9602C_LX_HZ / RTL9602C_TIMER_DIVISOR)

/*
 * The clock counts the LX bus clock divided by 2, 100 MHz (measured on this
 * board); a divisor of 1 does not count. Its 28 bits wrap every 2.68 s.
 */
#define RTL9602C_TIMER_CLOCK_DIVISOR	2
#define RTL9602C_TIMER_CLOCK_HZ		(RTL9602C_LX_HZ / RTL9602C_TIMER_CLOCK_DIVISOR)

static void __iomem *timer_base;

static void __iomem *timer_reg(unsigned int idx, unsigned int off)
{
	return timer_base + idx * RTL9602C_TIMER_STRIDE + off;
}

static void __iomem *tick_reg(unsigned int off)
{
	return timer_reg(RTL9602C_TIMER_TICK_IDX, off);
}

/*
 * Stop counting, then clear an expiry left pending; writing the pending bit
 * alone also clears the interrupt enable.
 */
static void rtl9602c_timer_stop(unsigned int idx)
{
	__raw_writel(0, timer_reg(idx, RTL9602C_TIMER_CTRL));
	__raw_writel(RTL9602C_TIMER_IRQ_PENDING, timer_reg(idx, RTL9602C_TIMER_IRQ));
}

static void rtl9602c_tick_stop(void)
{
	rtl9602c_timer_stop(RTL9602C_TIMER_TICK_IDX);
}

/*
 * Start from a stopped timer: the period, then the divisor while the timer
 * is still stopped, the interrupt enable, and the start last.
 */
static void rtl9602c_tick_start(unsigned long count)
{
	rtl9602c_tick_stop();
	__raw_writel(count, tick_reg(RTL9602C_TIMER_RELOAD));
	__raw_writel(RTL9602C_TIMER_DIVISOR, tick_reg(RTL9602C_TIMER_CTRL));
	__raw_writel(RTL9602C_TIMER_IRQ_EN, tick_reg(RTL9602C_TIMER_IRQ));
	__raw_writel(RTL9602C_TIMER_DIVISOR | RTL9602C_TIMER_CTRL_EN | RTL9602C_TIMER_CTRL_PERIODIC,
		     tick_reg(RTL9602C_TIMER_CTRL));
}

/* One expiry per jiffy: 800 counts at HZ = 250 */
static int rtl9602c_set_periodic(struct clock_event_device *evt)
{
	rtl9602c_tick_start(RTL9602C_TIMER_HZ / HZ);
	return 0;
}

static int rtl9602c_shutdown(struct clock_event_device *evt)
{
	rtl9602c_tick_stop();
	return 0;
}

/*
 * Periodic only: there is no set_next_event. The CPU mask is narrowed to
 * CPU 0 before the device is registered, since the clockevents core warns
 * about cpu_all_mask.
 */
static struct clock_event_device rtl9602c_clockevent = {
	.name			= "rtl9602c-tc0",
	.features		= CLOCK_EVT_FEAT_PERIODIC,
	.rating			= 300,
	.set_state_periodic	= rtl9602c_set_periodic,
	.set_state_shutdown	= rtl9602c_shutdown,
	.cpumask		= cpu_all_mask,
};

/*
 * Clear the expiry by writing the pending bit back, keeping the interrupt
 * enable. The source is not checked: the SoC interrupt controller calls this
 * only while timer 0's source is pending and enabled.
 */
static irqreturn_t rtl9602c_timer_interrupt(int irq, void *dev_id)
{
	struct clock_event_device *evt = dev_id;

	__raw_writel(__raw_readl(tick_reg(RTL9602C_TIMER_IRQ)) | RTL9602C_TIMER_IRQ_PENDING,
		     tick_reg(RTL9602C_TIMER_IRQ));

	evt->event_handler(evt);
	return IRQ_HANDLED;
}

static u64 notrace rtl9602c_clock_count(void)
{
	return __raw_readl(timer_reg(RTL9602C_TIMER_CLOCK_IDX, RTL9602C_TIMER_COUNT));
}

static u64 rtl9602c_clocksource_read(struct clocksource *cs)
{
	return rtl9602c_clock_count();
}

static struct clocksource rtl9602c_clocksource = {
	.name	= "rtl9602c-tc1",
	.rating	= 300,
	.read	= rtl9602c_clocksource_read,
	.mask	= CLOCKSOURCE_MASK(RTL9602C_TIMER_BITS),
	.flags	= CLOCK_SOURCE_IS_CONTINUOUS,
};

/*
 * Timer 1 free-running from 0 through its whole range, its interrupt off: the
 * reload value, which also sets the count to 0, then the divisor while the
 * timer is stopped, and the start last. It then serves as the clocksource and
 * as sched_clock.
 */
static void __init rtl9602c_clock_init(void)
{
	unsigned int idx = RTL9602C_TIMER_CLOCK_IDX;

	rtl9602c_timer_stop(idx);
	__raw_writel(RTL9602C_TIMER_RELOAD_MAX, timer_reg(idx, RTL9602C_TIMER_RELOAD));
	__raw_writel(RTL9602C_TIMER_CLOCK_DIVISOR, timer_reg(idx, RTL9602C_TIMER_CTRL));
	__raw_writel(RTL9602C_TIMER_CLOCK_DIVISOR | RTL9602C_TIMER_CTRL_EN |
		     RTL9602C_TIMER_CTRL_PERIODIC,
		     timer_reg(idx, RTL9602C_TIMER_CTRL));

	if (clocksource_register_hz(&rtl9602c_clocksource, RTL9602C_TIMER_CLOCK_HZ))
		pr_err("rtl9602c: cannot register the clocksource\n");
	sched_clock_register(rtl9602c_clock_count, RTL9602C_TIMER_BITS,
			     RTL9602C_TIMER_CLOCK_HZ);
}

/**
 * plat_time_init() - set up the tick and the clock
 *
 * Stops timer 0, clearing whatever it was doing before the kernel, and
 * registers it as a clockevent at 200 kHz; the clockevents core then
 * switches it to periodic mode, which starts it. The delta limits, 0x100 to
 * 0xffffff, matter only in one-shot mode, which the device lacks; the
 * counter itself is 28 bits wide. Requesting the interrupt
 * last enables timer 0's source, and with the way to the upper sources
 * already open the tick reaches the CPU. Timer 1 becomes the clock before
 * the tick starts (rtl9602c_clock_init()).
 */
void __init plat_time_init(void)
{
	unsigned int irq = RTL9602C_SOC_IRQ(RTL9602C_INTC_SRC_TIMER0);
	int err;

	timer_base = (void __iomem *)KSEG1ADDR(RTL9602C_TIMER_BASE);

	rtl9602c_tick_stop();
	rtl9602c_clock_init();

	rtl9602c_clockevent.cpumask = cpumask_of(0);
	clockevents_config_and_register(&rtl9602c_clockevent, RTL9602C_TIMER_HZ,
					0x100, 0xffffff);

	err = request_irq(irq, rtl9602c_timer_interrupt, IRQF_TIMER,
			  "rtl9602c-tick", &rtl9602c_clockevent);
	if (err) {
		pr_err("rtl9602c: cannot claim tick irq %u: %d\n", irq, err);
		return;
	}
}
