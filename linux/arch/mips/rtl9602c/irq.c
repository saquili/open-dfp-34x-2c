// SPDX-License-Identifier: GPL-2.0-only
/*
 * Interrupts of the Realtek RTL9602C.
 *
 * Two levels: the CPU's eight interrupt lines, Linux interrupts 0 to 7, and
 * a second-level controller with 64 sources, every one routed to CPU line 2.
 * Source s is Linux interrupt RTL9602C_SOC_IRQ(s), through a legacy domain
 * anchored to the controller's device tree node, where an interrupt
 * specifier is the source number.
 *
 * The controller has an enable and a status register for each bank of 32
 * sources and no acknowledge register: a status bit clears when the device
 * that raised it is serviced. So the irq_chip only masks and unmasks, and
 * every source is handled as level-triggered. Sources 32 to 63 have no line
 * of their own; they reach the CPU through source 12 of the lower bank,
 * which stays enabled.
 */

#include <linux/bitops.h>
#include <linux/init.h>
#include <linux/irq.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/of.h>
#include <linux/spinlock.h>

#include <asm/addrspace.h>
#include <asm/irq_cpu.h>
#include <asm/irq_regs.h>
#include <asm/mipsregs.h>

#include <asm/mach-rtl9602c/rtl9602c.h>

/*
 * Per bank of 32 sources: a set enable bit lets the source through, and a
 * status bit is set while the source asserts, whether it is enabled or not.
 */
#define RTL9602C_INTC_EN(hw)		(((hw) < 32) ? 0x00 : 0x04)
#define RTL9602C_INTC_STAT(hw)		(((hw) < 32) ? 0x08 : 0x0c)
#define RTL9602C_INTC_SRC_BIT(hw)	BIT((hw) % 32)

/*
 * Seven routing registers of eight 4-bit fields, each field naming the CPU
 * line of one source. Which field belongs to which source is not verified,
 * so the registers are written whole, with the same line in every field.
 */
#define RTL9602C_INTC_ROUTE		0x10
#define RTL9602C_INTC_ROUTE_NUM		7
#define RTL9602C_INTC_ROUTE_ALL(ip)	((ip) * 0x11111111u)

/* The CPU line every source is routed to */
#define RTL9602C_INTC_CPU_IRQ			2

static void __iomem *intc_base;
static struct irq_domain *intc_domain;

/* Each enable register is shared by 32 sources. */
static DEFINE_RAW_SPINLOCK(intc_lock);

static void rtl9602c_intc_set(irq_hw_number_t hw, bool on)
{
	void __iomem *reg = intc_base + RTL9602C_INTC_EN(hw);
	unsigned long flags;
	u32 v;

	raw_spin_lock_irqsave(&intc_lock, flags);
	v = __raw_readl(reg);
	if (on)
		v |= RTL9602C_INTC_SRC_BIT(hw);
	else
		v &= ~RTL9602C_INTC_SRC_BIT(hw);
	__raw_writel(v, reg);
	raw_spin_unlock_irqrestore(&intc_lock, flags);
}

static void rtl9602c_intc_mask(struct irq_data *d)
{
	rtl9602c_intc_set(irqd_to_hwirq(d), false);
}

static void rtl9602c_intc_unmask(struct irq_data *d)
{
	rtl9602c_intc_set(irqd_to_hwirq(d), true);
}

static struct irq_chip rtl9602c_intc_chip = {
	.name		= "rtl9602c-intc",
	.irq_mask	= rtl9602c_intc_mask,
	.irq_unmask	= rtl9602c_intc_unmask,
};

static int rtl9602c_intc_map(struct irq_domain *d, unsigned int virq,
			     irq_hw_number_t hw)
{
	irq_set_chip_and_handler(virq, &rtl9602c_intc_chip, handle_level_irq);
	return 0;
}

static const struct irq_domain_ops rtl9602c_intc_ops = {
	.map	= rtl9602c_intc_map,
	.xlate	= irq_domain_xlate_onecell,
};

/*
 * The sources of one bank that are both asserted and enabled: a masked
 * source can stay asserted in status, and dispatching it would call a
 * handler that asked not to be called.
 */
static u32 rtl9602c_intc_pending(irq_hw_number_t bank)
{
	return __raw_readl(intc_base + RTL9602C_INTC_STAT(bank)) &
	       __raw_readl(intc_base + RTL9602C_INTC_EN(bank));
}

/*
 * The chained handler of CPU line 2. The lower bank goes first, lowest
 * source first, without source 12, which is set whenever an upper source is
 * pending and has no handler of its own; then the upper bank.
 */
static void rtl9602c_intc_handler(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);
	u32 pend;

	chained_irq_enter(chip, desc);

	pend = rtl9602c_intc_pending(0) & ~BIT(RTL9602C_INTC_SRC_CASCADE);
	while (pend) {
		unsigned int hw = __ffs(pend);

		pend &= ~BIT(hw);
		generic_handle_domain_irq(intc_domain, hw);
	}

	pend = rtl9602c_intc_pending(32);
	while (pend) {
		unsigned int hw = __ffs(pend);

		pend &= ~BIT(hw);
		generic_handle_domain_irq(intc_domain, 32 + hw);
	}

	chained_irq_exit(chip, desc);
}

/**
 * arch_init_irq() - set up the CPU's interrupt lines and the SoC controller
 *
 * Registers the CPU's eight lines, starts the SoC controller with every
 * source disabled and routed to CPU line 2, creates the domain of the SoC
 * sources, opens the way to sources 32 to 63 and chains the controller's
 * handler to CPU line 2, which unmasks that line.
 */
void __init arch_init_irq(void)
{
	struct device_node *np;
	unsigned int i;

	/* The fixed numbers clear the CPU's lines and fit within NR_IRQS. */
	BUILD_BUG_ON(MIPS_CPU_IRQ_BASE + 8 > RTL9602C_SOC_IRQ_BASE);
	BUILD_BUG_ON(RTL9602C_SOC_IRQ(RTL9602C_SOC_IRQS - 1) >= NR_IRQS);

	mips_cpu_irq_init();

	intc_base = (void __iomem *)KSEG1ADDR(RTL9602C_INTC_BASE);

	/* Start from silence: nothing is enabled that no driver asked for. */
	__raw_writel(0, intc_base + RTL9602C_INTC_EN(0));
	__raw_writel(0, intc_base + RTL9602C_INTC_EN(32));

	/*
	 * Every source to CPU line 2, so that the enable registers alone
	 * decide what may interrupt.
	 */
	for (i = 0; i < RTL9602C_INTC_ROUTE_NUM; i++)
		__raw_writel(RTL9602C_INTC_ROUTE_ALL(RTL9602C_INTC_CPU_IRQ),
			     intc_base + RTL9602C_INTC_ROUTE + i * 4);

	/* Device nodes name their source against this node. */
	np = of_find_compatible_node(NULL, NULL, "realtek,rtl9602c-intc");

	intc_domain = irq_domain_create_legacy(of_fwnode_handle(np),
					       RTL9602C_SOC_IRQS,
					       RTL9602C_SOC_IRQ_BASE, 0,
					       &rtl9602c_intc_ops, NULL);
	if (!intc_domain)
		panic("rtl9602c: cannot create the SoC interrupt domain");

	/*
	 * Sources 32 to 63 reach the CPU only while source 12 is enabled as
	 * well. It is enabled once and never masked.
	 */
	rtl9602c_intc_set(RTL9602C_INTC_SRC_CASCADE, true);

	irq_set_chained_handler(MIPS_CPU_IRQ_BASE + RTL9602C_INTC_CPU_IRQ,
				rtl9602c_intc_handler);
}

/**
 * plat_irq_dispatch() - dispatch an interrupt taken by the CPU
 *
 * Called from the exception handler; the platform must provide it. Hands the
 * highest CPU line that is both pending in Cause and unmasked in Status to
 * do_IRQ(); line 2 carries the SoC controller's chained handler. An
 * interrupt with no such line is counted as spurious.
 */
asmlinkage void plat_irq_dispatch(void)
{
	unsigned int pending = read_c0_status() & read_c0_cause() & ST0_IM;

	if (!pending) {
		spurious_interrupt();
		return;
	}

	do_IRQ(MIPS_CPU_IRQ_BASE + fls(pending) - CAUSEB_IP - 1);
}
