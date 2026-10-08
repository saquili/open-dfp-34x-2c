// SPDX-License-Identifier: GPL-2.0-only
/*
 * Watchdog driver for the Realtek RTL9602C.
 *
 * The block divides the 200 MHz LX bus clock into ticks and counts them
 * through two phases: the end of phase one sets a flag, the end of phase
 * two resets the whole chip, the switch included. Writing the kick bit
 * restarts the count. The driver arms the watchdog at probe, and the
 * watchdog core feeds it until userspace opens /dev/watchdog: a slow
 * userspace is not reset, a kernel that stops running is.
 *
 * It does not recover a CPU stalled in a bus read that is never answered:
 * then no reset comes at all (measured on this board), and only a power
 * cycle brings the board back.
 */

#include <linux/bitfield.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/watchdog.h>

/* Writing the kick bit restarts the count. */
#define RTL9602C_WDT_KEEPALIVE		0x00
#define RTL9602C_WDT_KEEPALIVE_KICK		BIT(31)

/*
 * The phase-one flag sets when phase one expires and stays set until the
 * expiry action. That writing it back clears it is not verified.
 */
#define RTL9602C_WDT_IRQ_STAT		0x04
#define RTL9602C_WDT_IRQ_STAT_PHASE1		BIT(31)

/*
 * The enable arms the watchdog when written set and disarms it when written
 * clear. A tick is 2^(RTL9602C_WDT_TICK_SHIFT + PRESCALE) cycles of the LX
 * bus clock; phase one lasts PHASE1 + 1 ticks, and phase two PHASE2 + 1
 * ticks after it. The other bits of the word hold nothing: there is no
 * interrupt enable.
 */
#define RTL9602C_WDT_CTRL		0x08
#define RTL9602C_WDT_CTRL_EN			BIT(31)
#define RTL9602C_WDT_CTRL_PRESCALE		GENMASK(30, 29)
#define RTL9602C_WDT_CTRL_PHASE1		GENMASK(26, 22)
#define RTL9602C_WDT_CTRL_PHASE2		GENMASK(19, 15)
#define RTL9602C_WDT_CTRL_ACTION		GENMASK(1, 0)

/*
 * Expiry resets the whole chip, the switch included, although the switch's
 * table memories keep their contents. The action needs nothing from the
 * CPU, which is what a watchdog for a kernel that no longer runs requires.
 */
#define RTL9602C_WDT_CTRL_ACTION_CHIP_RESET	0

/* The tick shift and the clock rate are measured on this board. */
#define RTL9602C_WDT_TICK_SHIFT		25
#define RTL9602C_WDT_CTRL_PRESCALE_MAX		3
#define RTL9602C_WDT_CTRL_PHASE_MAX		31
#define RTL9602C_WDT_LX_HZ		200000000UL

/* Phase two, in ticks, when no pretimeout is set */
#define RTL9602C_WDT_PHASE2_TICKS		1

/**
 * struct rtl9602c_wdt - driver state
 * @wdd: the watchdog core's device
 * @base: the three registers
 * @irq: the phase-one interrupt, or negative when there is none
 * @irq_masked: the interrupt was disabled after a notification
 * @pretimeouts: phase-one notifications delivered
 */
struct rtl9602c_wdt {
	struct watchdog_device	wdd;
	void __iomem		*base;
	int			irq;
	bool			irq_masked;
	unsigned int		pretimeouts;
};

/*
 * One tick at prescale @sc in whole milliseconds: 167, 335, 671 and 1342 ms.
 * Ticks per second would not do: at prescale 3 a tick is longer than a
 * second, so the count would be zero and a later division would divide by
 * it. Dividing by the clock in kHz keeps the numerator, at most 2^28,
 * within 32 bits.
 */
static unsigned long rtl9602c_wdt_tick_ms(unsigned int sc)
{
	return (1UL << (RTL9602C_WDT_TICK_SHIFT + sc)) /
	       (RTL9602C_WDT_LX_HZ / 1000);
}

/*
 * The longest deadline at prescale @sc in whole seconds: all 32 ticks of
 * phase one and one of phase two, which gives 5, 11, 22 and 44 s.
 */
static unsigned int rtl9602c_wdt_max_at(unsigned int sc)
{
	unsigned long ticks = RTL9602C_WDT_CTRL_PHASE_MAX + 1 + RTL9602C_WDT_PHASE2_TICKS;

	return (ticks * rtl9602c_wdt_tick_ms(sc)) / 1000;
}

/*
 * Program a deadline of @secs seconds with a warning @pre seconds before it.
 * One prescale serves both phases, the finest whose longest deadline
 * reaches @secs. Phase two gets @pre (one tick without a warning, at most
 * 32 ticks) and phase one the rest, both rounded down to whole ticks: the
 * default 44 s programs 32 ticks at prescale 3, which is 42.9 s.
 *
 * The enable shares the word with the timeout fields, so it is kept as the
 * register reads it. A word composed afresh would disarm a running watchdog
 * silently for the rest of the boot: the core, finding it running, pings it
 * and does not call start again.
 */
static int rtl9602c_wdt_program(struct rtl9602c_wdt *wdt, unsigned int secs,
				unsigned int pre)
{
	unsigned int sc, ph1, ph2t;
	unsigned long ticks;
	u32 ctrl;

	for (sc = 0; sc < RTL9602C_WDT_CTRL_PRESCALE_MAX; sc++)
		if (rtl9602c_wdt_max_at(sc) >= secs)
			break;

	ticks = (secs * 1000UL) / rtl9602c_wdt_tick_ms(sc);
	ph2t = pre ? (pre * 1000UL) / rtl9602c_wdt_tick_ms(sc) :
		     RTL9602C_WDT_PHASE2_TICKS;
	if (ph2t < 1)
		ph2t = 1;
	if (ph2t > RTL9602C_WDT_CTRL_PHASE_MAX + 1)
		ph2t = RTL9602C_WDT_CTRL_PHASE_MAX + 1;
	if (ticks < ph2t + 1)
		ticks = ph2t + 1;
	ph1 = ticks - ph2t - 1;
	if (ph1 > RTL9602C_WDT_CTRL_PHASE_MAX)
		ph1 = RTL9602C_WDT_CTRL_PHASE_MAX;

	ctrl = FIELD_PREP(RTL9602C_WDT_CTRL_PRESCALE, sc) |
	       FIELD_PREP(RTL9602C_WDT_CTRL_PHASE1, ph1) |
	       FIELD_PREP(RTL9602C_WDT_CTRL_PHASE2, ph2t - 1) |
	       FIELD_PREP(RTL9602C_WDT_CTRL_ACTION, RTL9602C_WDT_CTRL_ACTION_CHIP_RESET);
	ctrl |= readl(wdt->base + RTL9602C_WDT_CTRL) & RTL9602C_WDT_CTRL_EN;
	writel(ctrl, wdt->base + RTL9602C_WDT_CTRL);
	return 0;
}

/*
 * The phase-one interrupt. The block has not been seen to raise it: with
 * the phase-one flag standing for seconds, nothing reached the interrupt
 * controller (measured on this board). So this handler has never run, and
 * what it relies on is not verified. It writes the flag back to clear it,
 * then disables the line, so that a flag that did not clear cannot repeat
 * the interrupt at once; start enables the line again.
 */
static irqreturn_t rtl9602c_wdt_irq(int irq, void *dev_id)
{
	struct rtl9602c_wdt *wdt = dev_id;
	u32 st = readl(wdt->base + RTL9602C_WDT_IRQ_STAT);

	if (!(st & RTL9602C_WDT_IRQ_STAT_PHASE1))
		return IRQ_NONE;

	writel(RTL9602C_WDT_IRQ_STAT_PHASE1, wdt->base + RTL9602C_WDT_IRQ_STAT);
	disable_irq_nosync(irq);
	wdt->irq_masked = true;
	wdt->pretimeouts++;
	watchdog_notify_pretimeout(&wdt->wdd);
	return IRQ_HANDLED;
}

static int rtl9602c_wdt_start(struct watchdog_device *wdd)
{
	struct rtl9602c_wdt *wdt = watchdog_get_drvdata(wdd);
	u32 ctrl;

	rtl9602c_wdt_program(wdt, wdd->timeout, wdd->pretimeout);

	/*
	 * Kick before enabling, so the count starts from zero whatever an
	 * earlier owner left. Whether enabling alone clears the count is not
	 * verified.
	 */
	writel(RTL9602C_WDT_KEEPALIVE_KICK, wdt->base + RTL9602C_WDT_KEEPALIVE);

	ctrl = readl(wdt->base + RTL9602C_WDT_CTRL);
	writel(ctrl | RTL9602C_WDT_CTRL_EN, wdt->base + RTL9602C_WDT_CTRL);

	if (wdt->irq > 0 && wdt->irq_masked) {
		wdt->irq_masked = false;
		enable_irq(wdt->irq);
	}
	return 0;
}

static int rtl9602c_wdt_stop(struct watchdog_device *wdd)
{
	struct rtl9602c_wdt *wdt = watchdog_get_drvdata(wdd);
	u32 ctrl = readl(wdt->base + RTL9602C_WDT_CTRL);

	writel(ctrl & ~RTL9602C_WDT_CTRL_EN, wdt->base + RTL9602C_WDT_CTRL);
	return 0;
}

static int rtl9602c_wdt_ping(struct watchdog_device *wdd)
{
	struct rtl9602c_wdt *wdt = watchdog_get_drvdata(wdd);

	writel(RTL9602C_WDT_KEEPALIVE_KICK, wdt->base + RTL9602C_WDT_KEEPALIVE);
	return 0;
}

static int rtl9602c_wdt_set_timeout(struct watchdog_device *wdd,
				    unsigned int secs)
{
	struct rtl9602c_wdt *wdt = watchdog_get_drvdata(wdd);

	wdd->timeout = secs;
	if (watchdog_active(wdd)) {
		rtl9602c_wdt_program(wdt, secs, wdd->pretimeout);

		/*
		 * Count the new deadline from this request. Whether a control
		 * write restarts the count by itself is not verified; without
		 * the kick, a deadline shorter than the count already reached
		 * might expire at once.
		 */
		writel(RTL9602C_WDT_KEEPALIVE_KICK, wdt->base + RTL9602C_WDT_KEEPALIVE);
	}
	return 0;
}

/*
 * The core does not call this, because the driver does not advertise
 * WDIOF_PRETIMEOUT: the pretimeout stays zero and phase two one tick.
 */
static int rtl9602c_wdt_set_pretimeout(struct watchdog_device *wdd,
				       unsigned int secs)
{
	struct rtl9602c_wdt *wdt = watchdog_get_drvdata(wdd);

	wdd->pretimeout = secs;
	if (watchdog_active(wdd)) {
		rtl9602c_wdt_program(wdt, wdd->timeout, secs);
		writel(RTL9602C_WDT_KEEPALIVE_KICK, wdt->base + RTL9602C_WDT_KEEPALIVE);
	}
	return 0;
}

static const struct watchdog_info rtl9602c_wdt_info = {
	.identity = "RTL9602C watchdog",
	/*
	 * No WDIOF_PRETIMEOUT: the block raises no interrupt when phase one
	 * ends (measured on this board), so a pretimeout cannot be delivered.
	 */
	.options = WDIOF_SETTIMEOUT | WDIOF_MAGICCLOSE | WDIOF_KEEPALIVEPING,
};

static const struct watchdog_ops rtl9602c_wdt_ops = {
	.owner = THIS_MODULE,
	.start = rtl9602c_wdt_start,
	.stop = rtl9602c_wdt_stop,
	.ping = rtl9602c_wdt_ping,
	.set_timeout = rtl9602c_wdt_set_timeout,
	.set_pretimeout = rtl9602c_wdt_set_pretimeout,
};

static int rtl9602c_wdt_probe(struct platform_device *pdev)
{
	struct rtl9602c_wdt *wdt;

	wdt = devm_kzalloc(&pdev->dev, sizeof(*wdt), GFP_KERNEL);
	if (!wdt)
		return -ENOMEM;

	wdt->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(wdt->base))
		return PTR_ERR(wdt->base);

	/*
	 * The phase-one interrupt is optional and is requested before the
	 * counter runs. The line the device tree names for it is not
	 * verified, since the block has not been seen to raise it.
	 */
	wdt->irq = platform_get_irq_optional(pdev, 0);
	if (wdt->irq > 0) {
		int err = devm_request_irq(&pdev->dev, wdt->irq,
					   rtl9602c_wdt_irq, 0,
					   "rtl9602c-wdt", wdt);

		if (err) {
			dev_warn(&pdev->dev,
				 "cannot take interrupt %d: %pe; a hang will reset without a word\n",
				 wdt->irq, ERR_PTR(err));
			wdt->irq = -1;
		}
	}

	wdt->wdd.info = &rtl9602c_wdt_info;
	wdt->wdd.ops = &rtl9602c_wdt_ops;
	wdt->wdd.parent = &pdev->dev;
	/* 1 to 44 s; 44 s unless the device tree gives timeout-sec */
	wdt->wdd.min_timeout = 1;
	wdt->wdd.max_timeout = rtl9602c_wdt_max_at(RTL9602C_WDT_CTRL_PRESCALE_MAX);
	wdt->wdd.timeout = wdt->wdd.max_timeout;
	watchdog_set_drvdata(&wdt->wdd, wdt);
	watchdog_init_timeout(&wdt->wdd, 0, &pdev->dev);

	/*
	 * The platform code writes 0 to the control register at every boot,
	 * which stops a count left running across a restart, so the watchdog
	 * is found stopped here. Arm it, and let the core feed it until
	 * userspace opens /dev/watchdog.
	 */
	rtl9602c_wdt_start(&wdt->wdd);
	set_bit(WDOG_HW_RUNNING, &wdt->wdd.status);

	/*
	 * Stop it on an orderly reboot and on unregister, so a reboot is not
	 * cut short; the platform's restart path then arms it at its shortest
	 * deadline to reset the chip.
	 */
	watchdog_stop_on_reboot(&wdt->wdd);
	watchdog_stop_on_unregister(&wdt->wdd);

	dev_info(&pdev->dev,
		 "watchdog, %u..%u s, two-phase, full chip reset; armed at %u s and fed by the kernel; phase-one interrupt %s but never asserted by the block\n",
		 wdt->wdd.min_timeout, wdt->wdd.max_timeout, wdt->wdd.timeout,
		 wdt->irq > 0 ? "taken" : "unavailable");

	return devm_watchdog_register_device(&pdev->dev, &wdt->wdd);
}

static const struct of_device_id rtl9602c_wdt_of_match[] = {
	{ .compatible = "realtek,rtl9602c-wdt" },
	{ }
};
MODULE_DEVICE_TABLE(of, rtl9602c_wdt_of_match);

static struct platform_driver rtl9602c_wdt_driver = {
	.probe = rtl9602c_wdt_probe,
	.driver = {
		.name = "rtl9602c-wdt",
		.of_match_table = rtl9602c_wdt_of_match,
	},
};
module_platform_driver(rtl9602c_wdt_driver);

MODULE_DESCRIPTION("Watchdog for the Realtek RTL9602C");
MODULE_LICENSE("GPL");
