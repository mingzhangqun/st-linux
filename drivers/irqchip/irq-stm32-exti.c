// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) Maxime Coquelin 2015
 * Copyright (C) STMicroelectronics 2017
 * Author:  Maxime Coquelin <mcoquelin.stm32@gmail.com>
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/hwspinlock.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/syscore_ops.h>

#include <dt-bindings/interrupt-controller/arm-gic.h>

#define IRQS_PER_BANK 32

#define HWSPNLCK_TIMEOUT	1000 /* usec */

struct stm32_exti_bank {
	u32 imr_ofst;
	u32 emr_ofst;
	u32 rtsr_ofst;
	u32 ftsr_ofst;
	u32 swier_ofst;
	u32 rpr_ofst;
	u32 fpr_ofst;
	u32 trg_ofst;
};

#define UNDEF_REG ~0

struct stm32_exti_drv_data {
	const struct stm32_exti_bank **exti_banks;
	const u8 *desc_irqs;
	u32 bank_nr;
};

struct stm32_exti_chip_data {
	struct stm32_exti_host_data *host_data;
	const struct stm32_exti_bank *reg_bank;
	struct raw_spinlock rlock;
	u32 wake_active;
	u32 mask_cache;
	u32 rtsr_cache;
	u32 ftsr_cache;
};

struct stm32_exti_host_data {
	struct list_head lh;
	void __iomem *base;
	struct stm32_exti_chip_data *chips_data;
	const struct stm32_exti_drv_data *drv_data;
	struct hwspinlock *hwlock;
	struct device_node *irq_map_node;
};

static LIST_HEAD(stm32_host_data_list);

static const struct stm32_exti_bank stm32f4xx_exti_b1 = {
	.imr_ofst	= 0x00,
	.emr_ofst	= 0x04,
	.rtsr_ofst	= 0x08,
	.ftsr_ofst	= 0x0C,
	.swier_ofst	= 0x10,
	.rpr_ofst	= 0x14,
	.fpr_ofst	= UNDEF_REG,
	.trg_ofst	= UNDEF_REG,
};

static const struct stm32_exti_bank *stm32f4xx_exti_banks[] = {
	&stm32f4xx_exti_b1,
};

static const struct stm32_exti_drv_data stm32f4xx_drv_data = {
	.exti_banks = stm32f4xx_exti_banks,
	.bank_nr = ARRAY_SIZE(stm32f4xx_exti_banks),
};

static const struct stm32_exti_bank stm32h7xx_exti_b1 = {
	.imr_ofst	= 0x80,
	.emr_ofst	= 0x84,
	.rtsr_ofst	= 0x00,
	.ftsr_ofst	= 0x04,
	.swier_ofst	= 0x08,
	.rpr_ofst	= 0x88,
	.fpr_ofst	= UNDEF_REG,
	.trg_ofst	= UNDEF_REG,
};

static const struct stm32_exti_bank stm32h7xx_exti_b2 = {
	.imr_ofst	= 0x90,
	.emr_ofst	= 0x94,
	.rtsr_ofst	= 0x20,
	.ftsr_ofst	= 0x24,
	.swier_ofst	= 0x28,
	.rpr_ofst	= 0x98,
	.fpr_ofst	= UNDEF_REG,
	.trg_ofst	= UNDEF_REG,
};

static const struct stm32_exti_bank stm32h7xx_exti_b3 = {
	.imr_ofst	= 0xA0,
	.emr_ofst	= 0xA4,
	.rtsr_ofst	= 0x40,
	.ftsr_ofst	= 0x44,
	.swier_ofst	= 0x48,
	.rpr_ofst	= 0xA8,
	.fpr_ofst	= UNDEF_REG,
	.trg_ofst	= UNDEF_REG,
};

static const struct stm32_exti_bank *stm32h7xx_exti_banks[] = {
	&stm32h7xx_exti_b1,
	&stm32h7xx_exti_b2,
	&stm32h7xx_exti_b3,
};

static const struct stm32_exti_drv_data stm32h7xx_drv_data = {
	.exti_banks = stm32h7xx_exti_banks,
	.bank_nr = ARRAY_SIZE(stm32h7xx_exti_banks),
};

static const struct stm32_exti_bank stm32mp1_exti_b1 = {
	.imr_ofst	= 0x80,
	.rtsr_ofst	= 0x00,
	.ftsr_ofst	= 0x04,
	.swier_ofst	= 0x08,
	.rpr_ofst	= 0x0C,
	.fpr_ofst	= 0x10,
	.trg_ofst	= 0x3EC,
};

static const struct stm32_exti_bank stm32mp1_exti_b2 = {
	.imr_ofst	= 0x90,
	.rtsr_ofst	= 0x20,
	.ftsr_ofst	= 0x24,
	.swier_ofst	= 0x28,
	.rpr_ofst	= 0x2C,
	.fpr_ofst	= 0x30,
	.trg_ofst	= 0x3E8,
};

static const struct stm32_exti_bank stm32mp1_exti_b3 = {
	.imr_ofst	= 0xA0,
	.rtsr_ofst	= 0x40,
	.ftsr_ofst	= 0x44,
	.swier_ofst	= 0x48,
	.rpr_ofst	= 0x4C,
	.fpr_ofst	= 0x50,
	.trg_ofst	= 0x3E4,
};

static const struct stm32_exti_bank *stm32mp1_exti_banks[] = {
	&stm32mp1_exti_b1,
	&stm32mp1_exti_b2,
	&stm32mp1_exti_b3,
};

static struct irq_chip stm32_exti_h_chip;
static struct irq_chip stm32_exti_h_chip_direct;

#define EXTI_INVALID_IRQ       U8_MAX
#define STM32MP1_DESC_IRQ_SIZE (ARRAY_SIZE(stm32mp1_exti_banks) * IRQS_PER_BANK)

static const u8 stm32mp1_desc_irq[] = {
	/* default value */
	[0 ... (STM32MP1_DESC_IRQ_SIZE - 1)] = EXTI_INVALID_IRQ,

	[0] = 6,
	[1] = 7,
	[2] = 8,
	[3] = 9,
	[4] = 10,
	[5] = 23,
	[6] = 64,
	[7] = 65,
	[8] = 66,
	[9] = 67,
	[10] = 40,
	[11] = 42,
	[12] = 76,
	[13] = 77,
	[14] = 121,
	[15] = 127,
	[16] = 1,
	[19] = 3,
	[21] = 31,
	[22] = 33,
	[23] = 72,
	[24] = 95,
	[25] = 107,
	[26] = 37,
	[27] = 38,
	[28] = 39,
	[29] = 71,
	[30] = 52,
	[31] = 53,
	[32] = 82,
	[33] = 83,
	[43] = 75,
	[44] = 98,
	[47] = 93,
	[48] = 138,
	[50] = 139,
	[52] = 140,
	[53] = 141,
	[54] = 135,
	[61] = 100,
	[65] = 144,
	[68] = 143,
	[70] = 62,
	[73] = 129,
};

static const u8 stm32mp13_desc_irq[] = {
	/* default value */
	[0 ... (STM32MP1_DESC_IRQ_SIZE - 1)] = EXTI_INVALID_IRQ,

	[0] = 6,
	[1] = 7,
	[2] = 8,
	[3] = 9,
	[4] = 10,
	[5] = 24,
	[6] = 65,
	[7] = 66,
	[8] = 67,
	[9] = 68,
	[10] = 41,
	[11] = 43,
	[12] = 77,
	[13] = 78,
	[14] = 106,
	[15] = 109,
	[16] = 1,
	[19] = 3,
	[21] = 32,
	[22] = 34,
	[23] = 73,
	[24] = 93,
	[25] = 114,
	[26] = 38,
	[27] = 39,
	[28] = 40,
	[29] = 72,
	[30] = 53,
	[31] = 54,
	[32] = 83,
	[33] = 84,
	[44] = 96,
	[47] = 92,
	[48] = 116,
	[50] = 117,
	[52] = 118,
	[53] = 119,
	[68] = 63,
	[70] = 98,
};

static const struct stm32_exti_drv_data stm32mp1_drv_data = {
	.exti_banks = stm32mp1_exti_banks,
	.bank_nr = ARRAY_SIZE(stm32mp1_exti_banks),
	.desc_irqs = stm32mp1_desc_irq,
};

static const struct stm32_exti_drv_data stm32mp13_drv_data = {
	.exti_banks = stm32mp1_exti_banks,
	.bank_nr = ARRAY_SIZE(stm32mp1_exti_banks),
	.desc_irqs = stm32mp13_desc_irq,
};

static unsigned long stm32_exti_pending(struct irq_chip_generic *gc)
{
	struct stm32_exti_chip_data *chip_data = gc->private;
	const struct stm32_exti_bank *stm32_bank = chip_data->reg_bank;
	unsigned long pending;

	pending = irq_reg_readl(gc, stm32_bank->rpr_ofst);
	if (stm32_bank->fpr_ofst != UNDEF_REG)
		pending |= irq_reg_readl(gc, stm32_bank->fpr_ofst);

	return pending;
}

static void stm32_irq_handler(struct irq_desc *desc)
{
	struct irq_domain *domain = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	unsigned int nbanks = domain->gc->num_chips;
	struct irq_chip_generic *gc;
	unsigned long pending;
	int n, i, irq_base = 0;

	chained_irq_enter(chip, desc);

	for (i = 0; i < nbanks; i++, irq_base += IRQS_PER_BANK) {
		gc = irq_get_domain_generic_chip(domain, irq_base);

		while ((pending = stm32_exti_pending(gc))) {
			for_each_set_bit(n, &pending, IRQS_PER_BANK)
				generic_handle_domain_irq(domain, irq_base + n);
 		}
	}

	chained_irq_exit(chip, desc);
}

static int stm32_exti_set_type(struct irq_data *d,
			       unsigned int type, u32 *rtsr, u32 *ftsr)
{
	u32 mask = BIT(d->hwirq % IRQS_PER_BANK);

	switch (type) {
	case IRQ_TYPE_EDGE_RISING:
		*rtsr |= mask;
		*ftsr &= ~mask;
		break;
	case IRQ_TYPE_EDGE_FALLING:
		*rtsr &= ~mask;
		*ftsr |= mask;
		break;
	case IRQ_TYPE_EDGE_BOTH:
		*rtsr |= mask;
		*ftsr |= mask;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int stm32_irq_set_type(struct irq_data *d, unsigned int type)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);
	struct stm32_exti_chip_data *chip_data = gc->private;
	const struct stm32_exti_bank *stm32_bank = chip_data->reg_bank;
	struct hwspinlock *hwlock = chip_data->host_data->hwlock;
	u32 rtsr, ftsr;
	int err;

	irq_gc_lock(gc);

	if (hwlock) {
		err = hwspin_lock_timeout_in_atomic(hwlock, HWSPNLCK_TIMEOUT);
		if (err) {
			pr_err("%s can't get hwspinlock (%d)\n", __func__, err);
			goto unlock;
		}
	}

	rtsr = irq_reg_readl(gc, stm32_bank->rtsr_ofst);
	ftsr = irq_reg_readl(gc, stm32_bank->ftsr_ofst);

	err = stm32_exti_set_type(d, type, &rtsr, &ftsr);
	if (err)
		goto unspinlock;

	irq_reg_writel(gc, rtsr, stm32_bank->rtsr_ofst);
	irq_reg_writel(gc, ftsr, stm32_bank->ftsr_ofst);

unspinlock:
	if (hwlock)
		hwspin_unlock_in_atomic(hwlock);
unlock:
	irq_gc_unlock(gc);

	return err;
}

static void stm32_chip_suspend(struct stm32_exti_chip_data *chip_data,
			       u32 wake_active)
{
	const struct stm32_exti_bank *stm32_bank = chip_data->reg_bank;
	void __iomem *base = chip_data->host_data->base;

	/* save rtsr, ftsr registers */
	chip_data->rtsr_cache = readl_relaxed(base + stm32_bank->rtsr_ofst);
	chip_data->ftsr_cache = readl_relaxed(base + stm32_bank->ftsr_ofst);

	writel_relaxed(wake_active, base + stm32_bank->imr_ofst);
}

static void stm32_chip_resume(struct stm32_exti_chip_data *chip_data,
			      u32 mask_cache)
{
	const struct stm32_exti_bank *stm32_bank = chip_data->reg_bank;
	void __iomem *base = chip_data->host_data->base;

	/* restore rtsr, ftsr, registers */
	writel_relaxed(chip_data->rtsr_cache, base + stm32_bank->rtsr_ofst);
	writel_relaxed(chip_data->ftsr_cache, base + stm32_bank->ftsr_ofst);

	writel_relaxed(mask_cache, base + stm32_bank->imr_ofst);
}

static void stm32_irq_suspend(struct irq_chip_generic *gc)
{
	struct stm32_exti_chip_data *chip_data = gc->private;

	irq_gc_lock(gc);
	stm32_chip_suspend(chip_data, gc->wake_active);
	irq_gc_unlock(gc);
}

static void stm32_irq_resume(struct irq_chip_generic *gc)
{
	struct stm32_exti_chip_data *chip_data = gc->private;

	irq_gc_lock(gc);
	stm32_chip_resume(chip_data, gc->mask_cache);
	irq_gc_unlock(gc);
}

static int stm32_exti_alloc(struct irq_domain *d, unsigned int virq,
			    unsigned int nr_irqs, void *data)
{
	struct irq_fwspec *fwspec = data;
	irq_hw_number_t hwirq;

	hwirq = fwspec->param[0];

	irq_map_generic_chip(d, virq, hwirq);

	return 0;
}

static void stm32_exti_free(struct irq_domain *d, unsigned int virq,
			    unsigned int nr_irqs)
{
	struct irq_data *data = irq_domain_get_irq_data(d, virq);

	irq_domain_reset_irq_data(data);
}

static const struct irq_domain_ops irq_exti_domain_ops = {
	.map	= irq_map_generic_chip,
	.alloc  = stm32_exti_alloc,
	.free	= stm32_exti_free,
	.xlate	= irq_domain_xlate_twocell,
};

static void stm32_irq_ack(struct irq_data *d)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);
	struct stm32_exti_chip_data *chip_data = gc->private;
	const struct stm32_exti_bank *stm32_bank = chip_data->reg_bank;

	irq_gc_lock(gc);

	irq_reg_writel(gc, d->mask, stm32_bank->rpr_ofst);
	if (stm32_bank->fpr_ofst != UNDEF_REG)
		irq_reg_writel(gc, d->mask, stm32_bank->fpr_ofst);

	irq_gc_unlock(gc);
}

/* directly set the target bit without reading first. */
static inline void stm32_exti_write_bit(struct irq_data *d, u32 reg)
{
	struct stm32_exti_chip_data *chip_data = irq_data_get_irq_chip_data(d);
	void __iomem *base = chip_data->host_data->base;
	u32 val = BIT(d->hwirq % IRQS_PER_BANK);

	writel_relaxed(val, base + reg);
}

static inline u32 stm32_exti_set_bit(struct irq_data *d, u32 reg)
{
	struct stm32_exti_chip_data *chip_data = irq_data_get_irq_chip_data(d);
	void __iomem *base = chip_data->host_data->base;
	u32 val;

	val = readl_relaxed(base + reg);
	val |= BIT(d->hwirq % IRQS_PER_BANK);
	writel_relaxed(val, base + reg);

	return val;
}

static inline u32 stm32_exti_clr_bit(struct irq_data *d, u32 reg)
{
	struct stm32_exti_chip_data *chip_data = irq_data_get_irq_chip_data(d);
	void __iomem *base = chip_data->host_data->base;
	u32 val;

	val = readl_relaxed(base + reg);
	val &= ~BIT(d->hwirq % IRQS_PER_BANK);
	writel_relaxed(val, base + reg);

	return val;
}

static void stm32_exti_h_eoi(struct irq_data *d)
{
	struct stm32_exti_chip_data *chip_data = irq_data_get_irq_chip_data(d);
	const struct stm32_exti_bank *stm32_bank = chip_data->reg_bank;

	raw_spin_lock(&chip_data->rlock);

	stm32_exti_write_bit(d, stm32_bank->rpr_ofst);
	if (stm32_bank->fpr_ofst != UNDEF_REG)
		stm32_exti_write_bit(d, stm32_bank->fpr_ofst);

	raw_spin_unlock(&chip_data->rlock);

	if (d->parent_data->chip)
		irq_chip_eoi_parent(d);
}

static void stm32_exti_h_mask(struct irq_data *d)
{
	struct stm32_exti_chip_data *chip_data = irq_data_get_irq_chip_data(d);
	const struct stm32_exti_bank *stm32_bank = chip_data->reg_bank;

	raw_spin_lock(&chip_data->rlock);
	chip_data->mask_cache = stm32_exti_clr_bit(d, stm32_bank->imr_ofst);
	raw_spin_unlock(&chip_data->rlock);

	if (d->parent_data->chip)
		irq_chip_mask_parent(d);
}

static void stm32_exti_h_unmask(struct irq_data *d)
{
	struct stm32_exti_chip_data *chip_data = irq_data_get_irq_chip_data(d);
	const struct stm32_exti_bank *stm32_bank = chip_data->reg_bank;

	raw_spin_lock(&chip_data->rlock);
	chip_data->mask_cache = stm32_exti_set_bit(d, stm32_bank->imr_ofst);
	raw_spin_unlock(&chip_data->rlock);

	if (d->parent_data->chip)
		irq_chip_unmask_parent(d);
}

static int stm32_exti_h_request_resources(struct irq_data *data)
{
	data = data->parent_data;

	if (data->chip->irq_request_resources)
		return data->chip->irq_request_resources(data);

	return 0;
}

static int stm32_exti_h_set_type(struct irq_data *d, unsigned int type)
{
	struct stm32_exti_chip_data *chip_data = irq_data_get_irq_chip_data(d);
	const struct stm32_exti_bank *stm32_bank = chip_data->reg_bank;
	struct hwspinlock *hwlock = chip_data->host_data->hwlock;
	void __iomem *base = chip_data->host_data->base;
	u32 rtsr, ftsr;
	int err;

	raw_spin_lock(&chip_data->rlock);

	if (hwlock) {
		err = hwspin_lock_timeout_in_atomic(hwlock, HWSPNLCK_TIMEOUT);
		if (err) {
			pr_err("%s can't get hwspinlock (%d)\n", __func__, err);
			goto unlock;
		}
	}

	rtsr = readl_relaxed(base + stm32_bank->rtsr_ofst);
	ftsr = readl_relaxed(base + stm32_bank->ftsr_ofst);

	err = stm32_exti_set_type(d, type, &rtsr, &ftsr);
	if (err)
		goto unspinlock;

	writel_relaxed(rtsr, base + stm32_bank->rtsr_ofst);
	writel_relaxed(ftsr, base + stm32_bank->ftsr_ofst);

unspinlock:
	if (hwlock)
		hwspin_unlock_in_atomic(hwlock);
unlock:
	raw_spin_unlock(&chip_data->rlock);

	if (d->parent_data->chip)
		irq_chip_set_type_parent(d, type);

	return err;
}

static int stm32_exti_h_set_wake(struct irq_data *d, unsigned int on)
{
	struct stm32_exti_chip_data *chip_data = irq_data_get_irq_chip_data(d);
	u32 mask = BIT(d->hwirq % IRQS_PER_BANK);

	raw_spin_lock(&chip_data->rlock);

	if (on)
		chip_data->wake_active |= mask;
	else
		chip_data->wake_active &= ~mask;

	raw_spin_unlock(&chip_data->rlock);

	if (d->parent_data->chip)
		irq_chip_set_wake_parent(d, on);

	return 0;
}

static int stm32_exti_h_set_affinity(struct irq_data *d,
				     const struct cpumask *dest, bool force)
{
	if (d->parent_data->chip)
		return irq_chip_set_affinity_parent(d, dest, force);

	return IRQ_SET_MASK_OK_DONE;
}

static void stm32_exti_h_ack(struct irq_data *d)
{
	if (d->parent_data->chip)
		irq_chip_ack_parent(d);
}

static int stm32_exti_h_suspend(void)
{
	struct stm32_exti_chip_data *chip_data;
	struct stm32_exti_host_data *host_data;
	int i;

	list_for_each_entry(host_data, &stm32_host_data_list, lh) {
		for (i = 0; i < host_data->drv_data->bank_nr; i++) {
			chip_data = &host_data->chips_data[i];
			raw_spin_lock(&chip_data->rlock);
			stm32_chip_suspend(chip_data, chip_data->wake_active);
			raw_spin_unlock(&chip_data->rlock);
		}
	}

	return 0;
}

static void stm32_exti_h_resume(void)
{
	struct stm32_exti_chip_data *chip_data;
	struct stm32_exti_host_data *host_data;
	int i;

	list_for_each_entry(host_data, &stm32_host_data_list, lh) {
		for (i = 0; i < host_data->drv_data->bank_nr; i++) {
			chip_data = &host_data->chips_data[i];
			raw_spin_lock(&chip_data->rlock);
			stm32_chip_resume(chip_data, chip_data->mask_cache);
			raw_spin_unlock(&chip_data->rlock);
		}
	}
}

static struct syscore_ops stm32_exti_h_syscore_ops = {
	.suspend	= stm32_exti_h_suspend,
	.resume		= stm32_exti_h_resume,
};

static void stm32_exti_h_syscore_init(struct stm32_exti_host_data *host_data)
{
	if (IS_ENABLED(CONFIG_PM_SLEEP)) {
		if (list_empty(&stm32_host_data_list))
			register_syscore_ops(&stm32_exti_h_syscore_ops);

		list_add_tail(&host_data->lh, &stm32_host_data_list);
	}
}

static void stm32_exti_h_syscore_deinit(struct platform_device *pdev)
{
	struct stm32_exti_host_data *host_data = platform_get_drvdata(pdev);

	if (IS_ENABLED(CONFIG_PM_SLEEP)) {
		list_del(&host_data->lh);

		if (list_empty(&stm32_host_data_list))
			unregister_syscore_ops(&stm32_exti_h_syscore_ops);
	}
}

static int stm32_exti_h_retrigger(struct irq_data *d)
{
	struct stm32_exti_chip_data *chip_data = irq_data_get_irq_chip_data(d);
	const struct stm32_exti_bank *stm32_bank = chip_data->reg_bank;
	void __iomem *base = chip_data->host_data->base;
	u32 mask = BIT(d->hwirq % IRQS_PER_BANK);

	writel_relaxed(mask, base + stm32_bank->swier_ofst);

	return 0;
}

static struct irq_chip stm32_exti_h_chip = {
	.name			= "stm32-exti-h",
	.irq_eoi		= stm32_exti_h_eoi,
	.irq_ack		= stm32_exti_h_ack,
	.irq_mask		= stm32_exti_h_mask,
	.irq_unmask		= stm32_exti_h_unmask,
	.irq_request_resources	= stm32_exti_h_request_resources,
	.irq_release_resources	= irq_chip_release_resources_parent,
	.irq_retrigger		= stm32_exti_h_retrigger,
	.irq_set_type		= stm32_exti_h_set_type,
	.irq_set_wake		= stm32_exti_h_set_wake,
	.flags			= IRQCHIP_MASK_ON_SUSPEND,
	.irq_set_affinity	= IS_ENABLED(CONFIG_SMP) ? stm32_exti_h_set_affinity : NULL,
};

static struct irq_chip stm32_exti_h_chip_direct = {
	.name			= "stm32-exti-h-direct",
	.irq_eoi		= irq_chip_eoi_parent,
	.irq_ack		= irq_chip_ack_parent,
	.irq_mask		= stm32_exti_h_mask,
	.irq_unmask		= stm32_exti_h_unmask,
	.irq_request_resources	= stm32_exti_h_request_resources,
	.irq_release_resources	= irq_chip_release_resources_parent,
	.irq_retrigger		= irq_chip_retrigger_hierarchy,
	.irq_set_type		= irq_chip_set_type_parent,
	.irq_set_wake		= stm32_exti_h_set_wake,
	.flags			= IRQCHIP_MASK_ON_SUSPEND,
	.irq_set_affinity	= IS_ENABLED(CONFIG_SMP) ? irq_chip_set_affinity_parent : NULL,
};

static int stm32_exti_h_domain_match(struct irq_domain *dm,
				     struct device_node *node,
				     enum irq_domain_bus_token bus_token)
{
	struct stm32_exti_host_data *host_data = dm->host_data;

	if (!node ||
	    (bus_token != DOMAIN_BUS_ANY && dm->bus_token != bus_token))
		return 0;

	if (!host_data->irq_map_node)
		return (to_of_node(dm->fwnode) == node);

	if (node != host_data->irq_map_node->parent)
		return 0;

	return (to_of_node(dm->parent->fwnode) == of_irq_find_parent(host_data->irq_map_node->parent));
}

static int stm32_exti_h_domain_select(struct irq_domain *dm,
				      struct irq_fwspec *fwspec,
				      enum irq_domain_bus_token bus_token)
{
	struct fwnode_handle *fwnode = fwspec->fwnode;
	struct stm32_exti_host_data *host_data = dm->host_data;
	struct of_phandle_args out_irq;
	int ret;

	if (!fwnode ||
	    (bus_token != DOMAIN_BUS_ANY && dm->bus_token != bus_token))
		return 0;

	if (!host_data->irq_map_node)
		return (dm->fwnode == fwnode);

	if (fwnode != of_node_to_fwnode(host_data->irq_map_node->parent))
		return 0;

	out_irq.np = host_data->irq_map_node;
	out_irq.args_count = 2;
	out_irq.args[0] = fwspec->param[0];
	out_irq.args[1] = fwspec->param[1];

	ret = of_irq_parse_raw(NULL, &out_irq);
	if (ret)
		return ret;

	return (dm->parent->fwnode == of_node_to_fwnode(out_irq.np));
}

static int stm32_exti_h_domain_alloc(struct irq_domain *dm,
				     unsigned int virq,
				     unsigned int nr_irqs, void *data)
{
	struct stm32_exti_host_data *host_data = dm->host_data;
	struct stm32_exti_chip_data *chip_data;
	u8 desc_irq;
	struct irq_fwspec *fwspec = data;
	struct irq_fwspec p_fwspec;
	struct of_phandle_args out_irq;
	irq_hw_number_t hwirq;
	int bank, ret;
	u32 event_trg;
	struct irq_chip *chip;

	hwirq = fwspec->param[0];
	if (hwirq >= host_data->drv_data->bank_nr * IRQS_PER_BANK)
		return -EINVAL;

	bank  = hwirq / IRQS_PER_BANK;
	chip_data = &host_data->chips_data[bank];

	event_trg = readl_relaxed(host_data->base + chip_data->reg_bank->trg_ofst);
	chip = (event_trg & BIT(hwirq % IRQS_PER_BANK)) ?
	       &stm32_exti_h_chip : &stm32_exti_h_chip_direct;

	irq_domain_set_hwirq_and_chip(dm, virq, hwirq, chip, chip_data);

	if (host_data->irq_map_node) {
		out_irq.np = host_data->irq_map_node;
		out_irq.args_count = 2;
		out_irq.args[0] = fwspec->param[0];
		out_irq.args[1] = fwspec->param[1];

		ret = of_irq_parse_raw(NULL, &out_irq);
		if (ret)
			return ret;

		of_phandle_args_to_fwspec(out_irq.np, out_irq.args,
					  out_irq.args_count, &p_fwspec);

		return irq_domain_alloc_irqs_parent(dm, virq, 1, &p_fwspec);
	}

	if (!host_data->drv_data || !host_data->drv_data->desc_irqs)
		return -EINVAL;

	desc_irq = host_data->drv_data->desc_irqs[hwirq];
	if (desc_irq != EXTI_INVALID_IRQ) {
		p_fwspec.fwnode = dm->parent->fwnode;
		p_fwspec.param_count = 3;
		p_fwspec.param[0] = GIC_SPI;
		p_fwspec.param[1] = desc_irq;
		p_fwspec.param[2] = IRQ_TYPE_LEVEL_HIGH;

		return irq_domain_alloc_irqs_parent(dm, virq, 1, &p_fwspec);
	}

	return 0;
}

static struct
stm32_exti_host_data *stm32_exti_host_init(const struct stm32_exti_drv_data *dd,
					   struct device_node *node)
{
	struct stm32_exti_host_data *host_data;

	host_data = kzalloc(sizeof(*host_data), GFP_KERNEL);
	if (!host_data)
		return NULL;

	host_data->drv_data = dd;
	host_data->chips_data = kcalloc(dd->bank_nr,
					sizeof(struct stm32_exti_chip_data),
					GFP_KERNEL);
	if (!host_data->chips_data)
		goto free_host_data;

	host_data->base = of_iomap(node, 0);
	if (!host_data->base) {
		pr_err("%pOF: Unable to map registers\n", node);
		goto free_chips_data;
	}

	return host_data;

free_chips_data:
	kfree(host_data->chips_data);
free_host_data:
	kfree(host_data);

	return NULL;
}

static struct
stm32_exti_chip_data *stm32_exti_chip_init(struct stm32_exti_host_data *h_data,
					   u32 bank_idx,
					   struct device_node *node)
{
	const struct stm32_exti_bank *stm32_bank;
	struct stm32_exti_chip_data *chip_data;
	void __iomem *base = h_data->base;

	stm32_bank = h_data->drv_data->exti_banks[bank_idx];
	chip_data = &h_data->chips_data[bank_idx];
	chip_data->host_data = h_data;
	chip_data->reg_bank = stm32_bank;

	raw_spin_lock_init(&chip_data->rlock);

	/*
	 * This IP has no reset, so after hot reboot we should
	 * clear registers to avoid residue
	 */
	writel_relaxed(0, base + stm32_bank->imr_ofst);
	if (stm32_bank->emr_ofst)
		writel_relaxed(0, base + stm32_bank->emr_ofst);

	pr_info("%pOF: bank%d\n", node, bank_idx);

	return chip_data;
}

static int __init stm32_exti_init(const struct stm32_exti_drv_data *drv_data,
				  struct device_node *node)
{
	struct stm32_exti_host_data *host_data;
	unsigned int clr = IRQ_NOREQUEST | IRQ_NOPROBE | IRQ_NOAUTOEN;
	int nr_irqs, ret, i;
	struct irq_chip_generic *gc;
	struct irq_domain *domain;

	host_data = stm32_exti_host_init(drv_data, node);
	if (!host_data)
		return -ENOMEM;

	domain = irq_domain_add_linear(node, drv_data->bank_nr * IRQS_PER_BANK,
				       &irq_exti_domain_ops, NULL);
	if (!domain) {
		pr_err("%pOFn: Could not register interrupt domain.\n",
		       node);
		ret = -ENOMEM;
		goto out_unmap;
	}

	ret = irq_alloc_domain_generic_chips(domain, IRQS_PER_BANK, 1, "exti",
					     handle_edge_irq, clr, 0, 0);
	if (ret) {
		pr_err("%pOF: Could not allocate generic interrupt chip.\n",
		       node);
		goto out_free_domain;
	}

	for (i = 0; i < drv_data->bank_nr; i++) {
		const struct stm32_exti_bank *stm32_bank;
		struct stm32_exti_chip_data *chip_data;

		stm32_bank = drv_data->exti_banks[i];
		chip_data = stm32_exti_chip_init(host_data, i, node);

		gc = irq_get_domain_generic_chip(domain, i * IRQS_PER_BANK);

		gc->reg_base = host_data->base;
		gc->chip_types->type = IRQ_TYPE_EDGE_BOTH;
		gc->chip_types->chip.irq_ack = stm32_irq_ack;
		gc->chip_types->chip.irq_mask = irq_gc_mask_clr_bit;
		gc->chip_types->chip.irq_unmask = irq_gc_mask_set_bit;
		gc->chip_types->chip.irq_set_type = stm32_irq_set_type;
		gc->chip_types->chip.irq_set_wake = irq_gc_set_wake;
		gc->suspend = stm32_irq_suspend;
		gc->resume = stm32_irq_resume;
		gc->wake_enabled = IRQ_MSK(IRQS_PER_BANK);

		gc->chip_types->regs.mask = stm32_bank->imr_ofst;
		gc->private = (void *)chip_data;
	}

	nr_irqs = of_irq_count(node);
	for (i = 0; i < nr_irqs; i++) {
		unsigned int irq = irq_of_parse_and_map(node, i);

		irq_set_handler_data(irq, domain);
		irq_set_chained_handler(irq, stm32_irq_handler);
	}

	return 0;

out_free_domain:
	irq_domain_remove(domain);
out_unmap:
	iounmap(host_data->base);
	kfree(host_data->chips_data);
	kfree(host_data);
	return ret;
}

static const struct irq_domain_ops stm32_exti_h_domain_ops = {
	.match	= stm32_exti_h_domain_match,
	.select = stm32_exti_h_domain_select,
	.alloc	= stm32_exti_h_domain_alloc,
	.free	= irq_domain_free_irqs_common,
	.xlate = irq_domain_xlate_twocell,
};

static void stm32_exti_remove_irq(void *data)
{
	struct irq_domain *domain = data;
	struct fwnode_handle *fwnode = domain->fwnode;

	irq_domain_remove(domain);

	if (is_fwnode_irqchip(fwnode))
		irq_domain_free_fwnode(fwnode);
}

static int stm32_exti_remove(struct platform_device *pdev)
{
	stm32_exti_h_syscore_deinit(pdev);
	return 0;
}

static int stm32_exti_probe(struct platform_device *pdev)
{
	int ret, i;
	struct device *dev = &pdev->dev;
	struct device_node *child, *np = dev->of_node, *wakeup_np;
	struct irq_domain *parent_domain, *domain, *wakeup_domain;
	struct fwnode_handle *fwnode;
	struct stm32_exti_host_data *host_data;
	const struct stm32_exti_drv_data *drv_data;
	struct resource *res;
	char *name;

	host_data = devm_kzalloc(dev, sizeof(*host_data), GFP_KERNEL);
	if (!host_data)
		return -ENOMEM;

	platform_set_drvdata(pdev, host_data);

	/* check for optional hwspinlock which may be not available yet */
	ret = of_hwspin_lock_get_id(np, 0);
	if (ret == -EPROBE_DEFER)
		/* hwspinlock framework not yet ready */
		return ret;

	if (ret >= 0) {
		host_data->hwlock = devm_hwspin_lock_request_specific(dev, ret);
		if (!host_data->hwlock) {
			dev_err(dev, "Failed to request hwspinlock\n");
			return -EINVAL;
		}
	} else if (ret != -ENOENT) {
		/* note: ENOENT is a valid case (means 'no hwspinlock') */
		dev_err(dev, "Failed to get hwspinlock\n");
		return ret;
	}

	/* initialize host_data */
	drv_data = of_device_get_match_data(dev);
	if (!drv_data) {
		dev_err(dev, "no of match data\n");
		return -ENODEV;
	}
	host_data->drv_data = drv_data;

	host_data->chips_data = devm_kcalloc(dev, drv_data->bank_nr,
					     sizeof(*host_data->chips_data),
					     GFP_KERNEL);
	if (!host_data->chips_data)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	host_data->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(host_data->base))
		return PTR_ERR(host_data->base);

	for (i = 0; i < drv_data->bank_nr; i++)
		stm32_exti_chip_init(host_data, i, np);

	parent_domain = irq_find_host(of_irq_find_parent(np));
	if (!parent_domain) {
		dev_err(dev, "GIC interrupt-parent not found\n");
		return -EINVAL;
	}

	domain = irq_domain_add_hierarchy(parent_domain, 0,
					  drv_data->bank_nr * IRQS_PER_BANK,
					  np, &stm32_exti_h_domain_ops,
					  host_data);

	if (!domain) {
		dev_err(dev, "Could not register exti domain\n");
		return -ENOMEM;
	}

	ret = devm_add_action_or_reset(dev, stm32_exti_remove_irq, domain);
	if (ret)
		return ret;

	child = of_get_child_by_name(np, "exti-interrupt-map");
	if (child && of_property_read_bool(child, "interrupt-map"))
		host_data->irq_map_node = child;

	wakeup_np = of_parse_phandle(np, "wakeup-parent", 0);
	if (wakeup_np && !host_data->irq_map_node) {
		dev_warn(dev, "wakeup-parent ignored due to missing interrupt-map nexus node");
		of_node_put(wakeup_np);
		wakeup_np = NULL;
	}
	if (wakeup_np) {
		wakeup_domain = irq_find_host(wakeup_np);
		of_node_put(wakeup_np);
		if (!wakeup_domain)
			return -EPROBE_DEFER;

		/* as in __irq_domain_add() */
		name = kasprintf(GFP_KERNEL, "%pOF-wakeup", np);
		if (!name)
			return -ENOMEM;
		strreplace(name, '/', ':');

		fwnode = irq_domain_alloc_named_fwnode(name);
		kfree(name);
		if (!fwnode)
			return -ENOMEM;

		domain = irq_domain_create_hierarchy(wakeup_domain, 0,
						     drv_data->bank_nr * IRQS_PER_BANK,
						     fwnode, &stm32_exti_h_domain_ops,
						     host_data);
		if (!domain) {
			dev_err(dev, "Could not register exti domain\n");
			irq_domain_free_fwnode(fwnode);
			return -ENOMEM;
		}

		ret = devm_add_action_or_reset(dev, stm32_exti_remove_irq, domain);
		if (ret)
			return ret;
	}

	stm32_exti_h_syscore_init(host_data);

	return 0;
}

/* platform driver only for MP1 */
static const struct of_device_id stm32_exti_ids[] = {
	{ .compatible = "st,stm32mp1-exti", .data = &stm32mp1_drv_data},
	{ .compatible = "st,stm32mp13-exti", .data = &stm32mp13_drv_data},
	{},
};
MODULE_DEVICE_TABLE(of, stm32_exti_ids);

static struct platform_driver stm32_exti_driver = {
	.probe		= stm32_exti_probe,
	.remove		= stm32_exti_remove,
	.driver		= {
		.name	= "stm32_exti",
		.of_match_table = stm32_exti_ids,
	},
};

static int __init stm32_exti_arch_init(void)
{
	return platform_driver_register(&stm32_exti_driver);
}

static void __exit stm32_exti_arch_exit(void)
{
	return platform_driver_unregister(&stm32_exti_driver);
}

arch_initcall(stm32_exti_arch_init);
module_exit(stm32_exti_arch_exit);

/* no platform driver for F4 and H7 */
static int __init stm32f4_exti_of_init(struct device_node *np,
				       struct device_node *parent)
{
	return stm32_exti_init(&stm32f4xx_drv_data, np);
}

IRQCHIP_DECLARE(stm32f4_exti, "st,stm32-exti", stm32f4_exti_of_init);

static int __init stm32h7_exti_of_init(struct device_node *np,
				       struct device_node *parent)
{
	return stm32_exti_init(&stm32h7xx_drv_data, np);
}

IRQCHIP_DECLARE(stm32h7_exti, "st,stm32h7-exti", stm32h7_exti_of_init);
