// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2002 ARM Limited, All Rights Reserved.
 */

#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqchip/arm-gic.h>

#include "irq-gic-common.h"

static DEFINE_RAW_SPINLOCK(irq_controller_lock);

/*
 * IAMROOT, 2022.10.01:
 * - @quirks(ex gic_quirks)를 돌며 init 처리. 에라타같은것.
 */
void gic_enable_of_quirks(const struct device_node *np,
			  const struct gic_quirk *quirks, void *data)
{
	for (; quirks->desc; quirks++) {
		if (!of_device_is_compatible(np, quirks->compatible))
			continue;
		if (quirks->init(data))
			pr_info("GIC: enabling workaround for %s\n",
				quirks->desc);
	}
}

/*
 * IAMROOT, 2022.10.01:
 * - gic_enable_of_quirks랑 비슷한데 iddr이 일치하는지만 한번더 따진것.
 */
void gic_enable_quirks(u32 iidr, const struct gic_quirk *quirks,
		void *data)
{
	for (; quirks->desc; quirks++) {
		if (quirks->compatible)
			continue;
		if (quirks->iidr != (quirks->mask & iidr))
			continue;
		if (quirks->init(data))
			pr_info("GIC: enabling workaround for %s\n",
				quirks->desc);
	}
}

/*
 * IAMROOT, 2022.10.01:
 * @return 0 = IRQ_SET_MASK_OK
 *
 * -0b00 Corresponding interrupt is level-sensitive.
 *  0b10 Corresponding interrupt is edge-triggered.
 * - level인지, edge인지에 따라 @irq에 해당하는 bit 자리를 찾아서 set한다.
 */
int gic_configure_irq(unsigned int irq, unsigned int type,
		       void __iomem *base, void (*sync_access)(void))
{
	u32 confmask = 0x2 << ((irq % 16) * 2);
	u32 confoff = (irq / 16) * 4;
	u32 val, oldval;
	int ret = 0;
	unsigned long flags;

	/*
	 * Read current configuration register, and insert the config
	 * for "irq", depending on "type".
	 */
	raw_spin_lock_irqsave(&irq_controller_lock, flags);
	val = oldval = readl_relaxed(base + confoff);
	if (type & IRQ_TYPE_LEVEL_MASK)
		val &= ~confmask;
	else if (type & IRQ_TYPE_EDGE_BOTH)
		val |= confmask;

	/* If the current configuration is the same, then we are done */
	if (val == oldval) {
		raw_spin_unlock_irqrestore(&irq_controller_lock, flags);
		return 0;
	}

	/*
	 * Write back the new configuration, and possibly re-enable
	 * the interrupt. If we fail to write a new configuration for
	 * an SPI then WARN and return an error. If we fail to write the
	 * configuration for a PPI this is most likely because the GIC
	 * does not allow us to set the configuration or we are in a
	 * non-secure mode, and hence it may not be catastrophic.
	 */
	writel_relaxed(val, base + confoff);
	if (readl_relaxed(base + confoff) != val)
		ret = -EINVAL;

	raw_spin_unlock_irqrestore(&irq_controller_lock, flags);

	if (sync_access)
		sync_access();

	return ret;
}

/*
 * IAMROOT, 2022.10.08:
 * - v1 ~ v2는 이 함수만 호출하고, v3는 다른걸(extend register등) 설정하고
 *   이걸 호출한다.
 */
void gic_dist_config(void __iomem *base, int gic_irqs,
		     void (*sync_access)(void))
{
	unsigned int i;

	/*
	 * Set all global interrupts to be level triggered, active low.
	 */
/*
 * IAMROOT, 2022.10.08:
 * - active low lvl trigger로 설정.
 */
	for (i = 32; i < gic_irqs; i += 16)
		writel_relaxed(GICD_INT_ACTLOW_LVLTRIG,
					base + GIC_DIST_CONFIG + i / 4);

	/*
	 * Set priority on all global interrupts.
	 */
	for (i = 32; i < gic_irqs; i += 4)
		writel_relaxed(GICD_INT_DEF_PRI_X4, base + GIC_DIST_PRI + i);

	/*
	 * Deactivate and disable all SPIs. Leave the PPI and SGIs
	 * alone as they are in the redistributor registers on GICv3.
	 */
	for (i = 32; i < gic_irqs; i += 32) {
		writel_relaxed(GICD_INT_EN_CLR_X32,
			       base + GIC_DIST_ACTIVE_CLEAR + i / 8);
		writel_relaxed(GICD_INT_EN_CLR_X32,
			       base + GIC_DIST_ENABLE_CLEAR + i / 8);
	}

	if (sync_access)
		sync_access();
}

/*
 * IAMROOT, 2022.10.08:
 * - v1 ~ v2는 이 함수만 호출하고, v3는 다른걸(extend register등) 설정하고
 *   이걸 호출한다.
 */
void gic_cpu_config(void __iomem *base, int nr, void (*sync_access)(void))
{
	int i;

	/*
	 * Deal with the banked PPI and SGI interrupts - disable all
	 * private interrupts. Make sure everything is deactivated.
	 */
/*
 * IAMROOT, 2022.10.08:
 * - 끈다.
 */
	for (i = 0; i < nr; i += 32) {
		writel_relaxed(GICD_INT_EN_CLR_X32,
			       base + GIC_DIST_ACTIVE_CLEAR + i / 8);
		writel_relaxed(GICD_INT_EN_CLR_X32,
			       base + GIC_DIST_ENABLE_CLEAR + i / 8);
	}

	/*
	 * Set priority on PPI and SGI interrupts
	 */
/*
 * IAMROOT, 2022.10.08:
 * - 0xA0A0A0A0으로 설정한다.
 */
	for (i = 0; i < nr; i += 4)
		writel_relaxed(GICD_INT_DEF_PRI_X4,
					base + GIC_DIST_PRI + i * 4 / 4);

	if (sync_access)
		sync_access();
}
