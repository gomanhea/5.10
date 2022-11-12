// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 1992, 1998-2006 Linus Torvalds, Ingo Molnar
 * Copyright (C) 2005-2006, Thomas Gleixner, Russell King
 *
 * This file contains the core interrupt handling code, for irq-chip based
 * architectures. Detailed information is available in
 * Documentation/core-api/genericirq.rst
 */

#include <linux/irq.h>
#include <linux/msi.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/kernel_stat.h>
#include <linux/irqdomain.h>

#include <trace/events/irq.h>

#include "internals.h"

static irqreturn_t bad_chained_irq(int irq, void *dev_id)
{
	WARN_ONCE(1, "Chained irq %d should not call an action\n", irq);
	return IRQ_NONE;
}

/*
 * Chained handlers should never call action on their IRQ. This default
 * action will emit warning if such thing happens.
 */
struct irqaction chained_action = {
	.handler = bad_chained_irq,
};

/**
 *	irq_set_chip - set the irq chip for an irq
 *	@irq:	irq number
 *	@chip:	pointer to irq chip description structure
 */
int irq_set_chip(unsigned int irq, struct irq_chip *chip)
{
	unsigned long flags;
	struct irq_desc *desc = irq_get_desc_lock(irq, &flags, 0);

	if (!desc)
		return -EINVAL;

	if (!chip)
		chip = &no_irq_chip;

	desc->irq_data.chip = chip;
	irq_put_desc_unlock(desc, flags);
	/*
	 * For !CONFIG_SPARSE_IRQ make the irq show up in
	 * allocated_irqs.
	 */
	irq_mark_irq(irq);
	return 0;
}
EXPORT_SYMBOL(irq_set_chip);

/**
 *	irq_set_irq_type - set the irq trigger type for an irq
 *	@irq:	irq number
 *	@type:	IRQ_TYPE_{LEVEL,EDGE}_* value - see include/linux/irq.h
 */
int irq_set_irq_type(unsigned int irq, unsigned int type)
{
	unsigned long flags;
	struct irq_desc *desc = irq_get_desc_buslock(irq, &flags, IRQ_GET_DESC_CHECK_GLOBAL);
	int ret = 0;

	if (!desc)
		return -EINVAL;

	ret = __irq_set_trigger(desc, type);
	irq_put_desc_busunlock(desc, flags);
	return ret;
}
EXPORT_SYMBOL(irq_set_irq_type);

/**
 *	irq_set_handler_data - set irq handler data for an irq
 *	@irq:	Interrupt number
 *	@data:	Pointer to interrupt specific data
 *
 *	Set the hardware irq controller data for an irq
 */
/*
 * IAMROOT, 2022.10.08:
 * - private data를 설정해준다.
 */
int irq_set_handler_data(unsigned int irq, void *data)
{
	unsigned long flags;
	struct irq_desc *desc = irq_get_desc_lock(irq, &flags, 0);

	if (!desc)
		return -EINVAL;
	desc->irq_common_data.handler_data = data;
	irq_put_desc_unlock(desc, flags);
	return 0;
}
EXPORT_SYMBOL(irq_set_handler_data);

/**
 *	irq_set_msi_desc_off - set MSI descriptor data for an irq at offset
 *	@irq_base:	Interrupt number base
 *	@irq_offset:	Interrupt number offset
 *	@entry:		Pointer to MSI descriptor data
 *
 *	Set the MSI descriptor entry for an irq at offset
 */
int irq_set_msi_desc_off(unsigned int irq_base, unsigned int irq_offset,
			 struct msi_desc *entry)
{
	unsigned long flags;
	struct irq_desc *desc = irq_get_desc_lock(irq_base + irq_offset, &flags, IRQ_GET_DESC_CHECK_GLOBAL);

	if (!desc)
		return -EINVAL;
	desc->irq_common_data.msi_desc = entry;
	if (entry && !irq_offset)
		entry->irq = irq_base;
	irq_put_desc_unlock(desc, flags);
	return 0;
}

/**
 *	irq_set_msi_desc - set MSI descriptor data for an irq
 *	@irq:	Interrupt number
 *	@entry:	Pointer to MSI descriptor data
 *
 *	Set the MSI descriptor entry for an irq
 */
int irq_set_msi_desc(unsigned int irq, struct msi_desc *entry)
{
	return irq_set_msi_desc_off(irq, 0, entry);
}

/**
 *	irq_set_chip_data - set irq chip data for an irq
 *	@irq:	Interrupt number
 *	@data:	Pointer to chip specific data
 *
 *	Set the hardware irq chip data for an irq
 */
int irq_set_chip_data(unsigned int irq, void *data)
{
	unsigned long flags;
	struct irq_desc *desc = irq_get_desc_lock(irq, &flags, 0);

	if (!desc)
		return -EINVAL;
	desc->irq_data.chip_data = data;
	irq_put_desc_unlock(desc, flags);
	return 0;
}
EXPORT_SYMBOL(irq_set_chip_data);

/*
 * IAMROOT, 2022.10.01:
 * - virq로 desc를 가져와서 irq_data를 가져온다.
 */
struct irq_data *irq_get_irq_data(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);

	return desc ? &desc->irq_data : NULL;
}
EXPORT_SYMBOL_GPL(irq_get_irq_data);

static void irq_state_clr_disabled(struct irq_desc *desc)
{
	irqd_clear(&desc->irq_data, IRQD_IRQ_DISABLED);
}

static void irq_state_clr_masked(struct irq_desc *desc)
{
	irqd_clear(&desc->irq_data, IRQD_IRQ_MASKED);
}

static void irq_state_clr_started(struct irq_desc *desc)
{
	irqd_clear(&desc->irq_data, IRQD_IRQ_STARTED);
}

static void irq_state_set_started(struct irq_desc *desc)
{
	irqd_set(&desc->irq_data, IRQD_IRQ_STARTED);
}

enum {
	IRQ_STARTUP_NORMAL,
	IRQ_STARTUP_MANAGED,
	IRQ_STARTUP_ABORT,
};

#ifdef CONFIG_SMP
/*
 * IAMROOT, 2022.10.01:
 * @return IRQ_STARTUP_NORMAL : affinity managed가 아니면 그냥 normal로 return.
 *         IRQ_STARTUP_MANAGED : 모든 parent까지 activate 성공.
 *         IRQ_STARTUP_ABORT : ativate 실패
 */
static int
__irq_startup_managed(struct irq_desc *desc, struct cpumask *aff, bool force)
{
	struct irq_data *d = irq_desc_get_irq_data(desc);

	if (!irqd_affinity_is_managed(d))
		return IRQ_STARTUP_NORMAL;

	irqd_clr_managed_shutdown(d);

/*
 * prifri, 2022.10.01:
 * - online cpu중에 aff와 겹치는게 없다면 return IRQ_STARTUP_ABORT.
 *   @force 요청이 있다면 warning print를 해준다.
 */
	if (cpumask_any_and(aff, cpu_online_mask) >= nr_cpu_ids) {
		/*
		 * Catch code which fiddles with enable_irq() on a managed
		 * and potentially shutdown IRQ. Chained interrupt
		 * installment or irq auto probing should not happen on
		 * managed irqs either.
		 */
/*
 * IAMROOT, 2022.10.01:
 * - papago
 *   관리되고 잠재적으로 종료되는 IRQ에서 enable_irq()를 사용하는 코드를 잡아라.
 *   체인 인터럽트 설치 또는 irq 자동 프로빙은 관리되는 irq에서도 발생하지 않아야
 *   합니다.
 */
		if (WARN_ON_ONCE(force))
			return IRQ_STARTUP_ABORT;
		/*
		 * The interrupt was requested, but there is no online CPU
		 * in it's affinity mask. Put it into managed shutdown
		 * state and let the cpu hotplug mechanism start it up once
		 * a CPU in the mask becomes available.
		 */
/*
 * IAMROOT, 2022.10.01:
 * - papago
 *   인터럽트가 요청되었지만 해당 선호도 마스크에 온라인 CPU가 없습니다.
 *   관리 종료 상태로 전환하고 마스크의 CPU를 사용할 수 있게 되면 CPU 핫플러그
 *   메커니즘이 이를 시작하도록 합니다.
 */
		return IRQ_STARTUP_ABORT;
	}
	/*
	 * Managed interrupts have reserved resources, so this should not
	 * happen.
	 */
	if (WARN_ON(irq_domain_activate_irq(d, false)))
		return IRQ_STARTUP_ABORT;
	return IRQ_STARTUP_MANAGED;
}
#else
static __always_inline int
__irq_startup_managed(struct irq_desc *desc, struct cpumask *aff, bool force)
{
	return IRQ_STARTUP_NORMAL;
}
#endif

/*
 * IAMROOT, 2022.10.01:
 * - startup이 있으면 startup -> clr disable, clr mask.
 *   그게 아니면 irq_enable
 * - 주로 gpio control등을 수행한다. ex)npcmgpio_irq_startup
 * - set bit IRQD_IRQ_STARTED
 */
static int __irq_startup(struct irq_desc *desc)
{
	struct irq_data *d = irq_desc_get_irq_data(desc);
	int ret = 0;

	/* Warn if this interrupt is not activated but try nevertheless */
	WARN_ON_ONCE(!irqd_is_activated(d));

	if (d->chip->irq_startup) {
		ret = d->chip->irq_startup(d);
		irq_state_clr_disabled(desc);
		irq_state_clr_masked(desc);
	} else {
		irq_enable(desc);
	}
	irq_state_set_started(desc);
	return ret;
}

/*
 * IAMROOT, 2022.10.01:
 * - @desc에 해당하는 irq를 startup한다.
 *   1. 이미 되있다면 enable.
 *   2. 안되있다면 domain activate 및 affinity를 설정하고 startup 수행.
 * - @resend가 true이면, pending이 startup전에 되있었다면 irq를 처리해준다.
 */
int irq_startup(struct irq_desc *desc, bool resend, bool force)
{
	struct irq_data *d = irq_desc_get_irq_data(desc);
	struct cpumask *aff = irq_data_get_affinity_mask(d);
	int ret = 0;

	desc->depth = 0;

/*
 * IAMROOT, 2022.10.01:
 * - 이미 started면 enable.
 */
	if (irqd_is_started(d)) {
		irq_enable(desc);
	} else {
		switch (__irq_startup_managed(desc, aff, force)) {
		case IRQ_STARTUP_NORMAL:

/*
 * IAMROOT, 2022.10.01:
 * - IRQCHIP_AFFINITY_PRE_STARTUP이 있으면 먼저 irq_setup_affinity를 먼저 수행하고
 *   그게 아니면 __irq_startup후에 수행한다.
 */
			if (d->chip->flags & IRQCHIP_AFFINITY_PRE_STARTUP)
				irq_setup_affinity(desc);
			ret = __irq_startup(desc);
			if (!(d->chip->flags & IRQCHIP_AFFINITY_PRE_STARTUP))
				irq_setup_affinity(desc);
			break;
		case IRQ_STARTUP_MANAGED:
			irq_do_set_affinity(d, aff, false);
			ret = __irq_startup(desc);
			break;
		case IRQ_STARTUP_ABORT:
			irqd_set_managed_shutdown(d);
			return 0;
		}
	}
	if (resend)
		check_irq_resend(desc, false);

	return ret;
}

/*
 * IAMROOT, 2022.10.29:
 * - affinity managed가 설정이 되있다면 이미 activate했다고 가정하고 즉시 성공.
 */
int irq_activate(struct irq_desc *desc)
{
	struct irq_data *d = irq_desc_get_irq_data(desc);

	if (!irqd_affinity_is_managed(d))
		return irq_domain_activate_irq(d, false);
	return 0;
}

/*
 * IAMROOT, 2022.10.01:
 * - irq domain activate 및 irq startup 수행.
 *   @resend가 true이면 startup전에 irq pending이 들어왔을시 처리해준다.
 */
int irq_activate_and_startup(struct irq_desc *desc, bool resend)
{

/*
 * IAMROOT, 2022.10.01:
 * - 이미 activate 상태라면 warning
 */
	if (WARN_ON(irq_activate(desc)))
		return 0;
	return irq_startup(desc, resend, IRQ_START_FORCE);
}

static void __irq_disable(struct irq_desc *desc, bool mask);

void irq_shutdown(struct irq_desc *desc)
{
	if (irqd_is_started(&desc->irq_data)) {
		desc->depth = 1;
		if (desc->irq_data.chip->irq_shutdown) {
			desc->irq_data.chip->irq_shutdown(&desc->irq_data);
			irq_state_set_disabled(desc);
			irq_state_set_masked(desc);
		} else {
			__irq_disable(desc, true);
		}
		irq_state_clr_started(desc);
	}
}


void irq_shutdown_and_deactivate(struct irq_desc *desc)
{
	irq_shutdown(desc);
	/*
	 * This must be called even if the interrupt was never started up,
	 * because the activation can happen before the interrupt is
	 * available for request/startup. It has it's own state tracking so
	 * it's safe to call it unconditionally.
	 */
	irq_domain_deactivate_irq(&desc->irq_data);
}

/*
 * IAMROOT, 2022.10.01:
 * - irq_enable.
 *   enable엔 unmask도 포함된다.
 */
void irq_enable(struct irq_desc *desc)
{
/*
 * IAMROOT, 2022.10.01:
 * - disable 상태가 아니면 unmask. 
 */
	if (!irqd_irq_disabled(&desc->irq_data)) {
		unmask_irq(desc);
	} else {
/*
 * IAMROOT, 2022.10.01:
 * - disable 상태라면 irq_enable이 필요한 hw일 경우
 *   disable clr -> irq_enable -> irq_state_clr_masked로 진행한다.
 */
		irq_state_clr_disabled(desc);
		if (desc->irq_data.chip->irq_enable) {
			desc->irq_data.chip->irq_enable(&desc->irq_data);
			irq_state_clr_masked(desc);
		} else {
/*
 * IAMROOT, 2022.10.01:
 * - irq_enable이 없다면 unmask만 해주면되는듯
 *   irq_unmask + irq_state_clr_masked
 */
			unmask_irq(desc);
		}
	}
}

/*
 * IAMROOT, 2022.10.01:
 * - irq disable
 *   disable엔 unmask도 포함된다.
 */
static void __irq_disable(struct irq_desc *desc, bool mask)
{
	if (irqd_irq_disabled(&desc->irq_data)) {
		if (mask)
			mask_irq(desc);
	} else {
		irq_state_set_disabled(desc);
		if (desc->irq_data.chip->irq_disable) {
			desc->irq_data.chip->irq_disable(&desc->irq_data);
			irq_state_set_masked(desc);
		} else if (mask) {
			mask_irq(desc);
		}
	}
}

/**
 * irq_disable - Mark interrupt disabled
 * @desc:	irq descriptor which should be disabled
 *
 * If the chip does not implement the irq_disable callback, we
 * use a lazy disable approach. That means we mark the interrupt
 * disabled, but leave the hardware unmasked. That's an
 * optimization because we avoid the hardware access for the
 * common case where no interrupt happens after we marked it
 * disabled. If an interrupt happens, then the interrupt flow
 * handler masks the line at the hardware level and marks it
 * pending.
 *
 * If the interrupt chip does not implement the irq_disable callback,
 * a driver can disable the lazy approach for a particular irq line by
 * calling 'irq_set_status_flags(irq, IRQ_DISABLE_UNLAZY)'. This can
 * be used for devices which cannot disable the interrupt at the
 * device level under certain circumstances and have to use
 * disable_irq[_nosync] instead.
 */
void irq_disable(struct irq_desc *desc)
{
	__irq_disable(desc, irq_settings_disable_unlazy(desc));
}

/*
 * IAMROOT, 2022.10.29:
 * - irq_enable or irq_unmask 실행.
 * - ex) giv3
 *   irq_enable이 없다. gic_unmask_irq() 가 실행될것이다.
 */
void irq_percpu_enable(struct irq_desc *desc, unsigned int cpu)
{
	if (desc->irq_data.chip->irq_enable)
		desc->irq_data.chip->irq_enable(&desc->irq_data);
	else
		desc->irq_data.chip->irq_unmask(&desc->irq_data);
	cpumask_set_cpu(cpu, desc->percpu_enabled);
}

/*
 * IAMROOT, 2022.11.12:
 * - disable이 있다면 disable. 아니면 mask.
 * - irq disalbe한다(gic. gic_mask_irq)
 */
void irq_percpu_disable(struct irq_desc *desc, unsigned int cpu)
{
	if (desc->irq_data.chip->irq_disable)
		desc->irq_data.chip->irq_disable(&desc->irq_data);
	else
		desc->irq_data.chip->irq_mask(&desc->irq_data);
	cpumask_clear_cpu(cpu, desc->percpu_enabled);
}

static inline void mask_ack_irq(struct irq_desc *desc)
{
	if (desc->irq_data.chip->irq_mask_ack) {
		desc->irq_data.chip->irq_mask_ack(&desc->irq_data);
		irq_state_set_masked(desc);
	} else {
		mask_irq(desc);
		if (desc->irq_data.chip->irq_ack)
			desc->irq_data.chip->irq_ack(&desc->irq_data);
	}
}

/*
 * IAMROOT, 2022.10.01:
 * - controller에 있는 irq_mask를 막는다.
 *   ex) gic_mask_irq
 */
void mask_irq(struct irq_desc *desc)
{
/*
 * IAMROOT, 2022.10.01:
 * - @desc의 irqd에 이미 mask되있다고 하면 return.
 */
	if (irqd_irq_masked(&desc->irq_data))
		return;

	if (desc->irq_data.chip->irq_mask) {
/*
 * IAMROOT, 2022.10.01:
 * - regiset 조작.
 */
		desc->irq_data.chip->irq_mask(&desc->irq_data);
/*
 * IAMROOT, 2022.10.01:
 * - @desc의 irqd에 mask set.
 */
		irq_state_set_masked(desc);
	}
}

/*
 * IAMROOT, 2022.10.01:
 * - irq_unmask + irq_state_clr_masked 수행
 * - unmask가 되있다면 안한다. unmask수행
 */
void unmask_irq(struct irq_desc *desc)
{
	if (!irqd_irq_masked(&desc->irq_data))
		return;

	if (desc->irq_data.chip->irq_unmask) {
		desc->irq_data.chip->irq_unmask(&desc->irq_data);
		irq_state_clr_masked(desc);
	}
}

void unmask_threaded_irq(struct irq_desc *desc)
{
	struct irq_chip *chip = desc->irq_data.chip;

	if (chip->flags & IRQCHIP_EOI_THREADED)
		chip->irq_eoi(&desc->irq_data);

	unmask_irq(desc);
}

/*
 *	handle_nested_irq - Handle a nested irq from a irq thread
 *	@irq:	the interrupt number
 *
 *	Handle interrupts which are nested into a threaded interrupt
 *	handler. The handler function is called inside the calling
 *	threads context.
 */
void handle_nested_irq(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);
	struct irqaction *action;
	irqreturn_t action_ret;

	might_sleep();

	raw_spin_lock_irq(&desc->lock);

	desc->istate &= ~(IRQS_REPLAY | IRQS_WAITING);

	action = desc->action;
	if (unlikely(!action || irqd_irq_disabled(&desc->irq_data))) {
		desc->istate |= IRQS_PENDING;
		goto out_unlock;
	}

	kstat_incr_irqs_this_cpu(desc);
	irqd_set(&desc->irq_data, IRQD_IRQ_INPROGRESS);
	raw_spin_unlock_irq(&desc->lock);

	action_ret = IRQ_NONE;
	for_each_action_of_desc(desc, action)
		action_ret |= action->thread_fn(action->irq, action->dev_id);

	if (!irq_settings_no_debug(desc))
		note_interrupt(desc, action_ret);

	raw_spin_lock_irq(&desc->lock);
	irqd_clear(&desc->irq_data, IRQD_IRQ_INPROGRESS);

out_unlock:
	raw_spin_unlock_irq(&desc->lock);
}
EXPORT_SYMBOL_GPL(handle_nested_irq);

/*
 * IAMROOT, 2022.11.12:
 * @return false irq가 inprogress가 아님
 *         irq가 inprogress일때
 *				current cpu가 irq_poll_cpu라면. (예외처리 일종)
*				inprogress가 기다림 -> 기다리고 난후 irq가 disable이거나
*				                       action handler가 없다면.
*		   true inprogress기다림 -> 기다리고 난 후 irq가 enable상태면서
*		                            action handler가 존재.
* - inprogress중이라면 기다린다.
*/
static bool irq_check_poll(struct irq_desc *desc)
{
	if (!(desc->istate & IRQS_POLL_INPROGRESS))
		return false;
	return irq_wait_for_poll(desc);
}


/*
 * IAMROOT, 2022.11.12:
 * - irq가 run 시킬수 있는지 확인한다. 필요하다면 wait까지 해본다.
 * @return true.(running가능)
 *          inprogress, wakeup armed상태가 아니라면, 혹은 wait후에도 irq가
 *          enable 상태라면.
 */
static bool irq_may_run(struct irq_desc *desc)
{
	unsigned int mask = IRQD_IRQ_INPROGRESS | IRQD_WAKEUP_ARMED;

	/*
	 * If the interrupt is not in progress and is not an armed
	 * wakeup interrupt, proceed.
	 */
/*
 * IAMROOT, 2022.11.12:
 * - 해당 interrupt가 진행중인지 확인한다. spi는 동시에 진입할수없다.
 *   동시에 되는게 이상한상태.
 */
	if (!irqd_has_set(&desc->irq_data, mask))
		return true;

	/*
	 * If the interrupt is an armed wakeup source, mark it pending
	 * and suspended, disable it and notify the pm core about the
	 * event.
	 */
/*
 * IAMROOT, 2022.11.12:
 * - wakeup업중인지 확인하고, wakeup이 아직 안되있으면
 *   irq를 끄고 pending으로 한후 wakepup시킨다.
 */
	if (irq_pm_check_wakeup(desc))
		return false;

	/*
	 * Handle a potential concurrent poll on a different core.
	 */
	return irq_check_poll(desc);
}

/**
 *	handle_simple_irq - Simple and software-decoded IRQs.
 *	@desc:	the interrupt description structure for this irq
 *
 *	Simple interrupts are either sent from a demultiplexing interrupt
 *	handler or come from hardware, where no interrupt hardware control
 *	is necessary.
 *
 *	Note: The caller is expected to handle the ack, clear, mask and
 *	unmask issues if necessary.
 */
void handle_simple_irq(struct irq_desc *desc)
{
	raw_spin_lock(&desc->lock);

	if (!irq_may_run(desc))
		goto out_unlock;

	desc->istate &= ~(IRQS_REPLAY | IRQS_WAITING);

	if (unlikely(!desc->action || irqd_irq_disabled(&desc->irq_data))) {
		desc->istate |= IRQS_PENDING;
		goto out_unlock;
	}

	kstat_incr_irqs_this_cpu(desc);
	handle_irq_event(desc);

out_unlock:
	raw_spin_unlock(&desc->lock);
}
EXPORT_SYMBOL_GPL(handle_simple_irq);

/**
 *	handle_untracked_irq - Simple and software-decoded IRQs.
 *	@desc:	the interrupt description structure for this irq
 *
 *	Untracked interrupts are sent from a demultiplexing interrupt
 *	handler when the demultiplexer does not know which device it its
 *	multiplexed irq domain generated the interrupt. IRQ's handled
 *	through here are not subjected to stats tracking, randomness, or
 *	spurious interrupt detection.
 *
 *	Note: Like handle_simple_irq, the caller is expected to handle
 *	the ack, clear, mask and unmask issues if necessary.
 */
void handle_untracked_irq(struct irq_desc *desc)
{
	unsigned int flags = 0;

	raw_spin_lock(&desc->lock);

	if (!irq_may_run(desc))
		goto out_unlock;

	desc->istate &= ~(IRQS_REPLAY | IRQS_WAITING);

	if (unlikely(!desc->action || irqd_irq_disabled(&desc->irq_data))) {
		desc->istate |= IRQS_PENDING;
		goto out_unlock;
	}

	desc->istate &= ~IRQS_PENDING;
	irqd_set(&desc->irq_data, IRQD_IRQ_INPROGRESS);
	raw_spin_unlock(&desc->lock);

	__handle_irq_event_percpu(desc, &flags);

	raw_spin_lock(&desc->lock);
	irqd_clear(&desc->irq_data, IRQD_IRQ_INPROGRESS);

out_unlock:
	raw_spin_unlock(&desc->lock);
}
EXPORT_SYMBOL_GPL(handle_untracked_irq);

/*
 * Called unconditionally from handle_level_irq() and only for oneshot
 * interrupts from handle_fasteoi_irq()
 */
static void cond_unmask_irq(struct irq_desc *desc)
{
	/*
	 * We need to unmask in the following cases:
	 * - Standard level irq (IRQF_ONESHOT is not set)
	 * - Oneshot irq which did not wake the thread (caused by a
	 *   spurious interrupt or a primary handler handling it
	 *   completely).
	 */
	if (!irqd_irq_disabled(&desc->irq_data) &&
	    irqd_irq_masked(&desc->irq_data) && !desc->threads_oneshot)
		unmask_irq(desc);
}

/**
 *	handle_level_irq - Level type irq handler
 *	@desc:	the interrupt description structure for this irq
 *
 *	Level type interrupts are active as long as the hardware line has
 *	the active level. This may require to mask the interrupt and unmask
 *	it after the associated handler has acknowledged the device, so the
 *	interrupt line is back to inactive.
 */
void handle_level_irq(struct irq_desc *desc)
{
	raw_spin_lock(&desc->lock);
	mask_ack_irq(desc);

	if (!irq_may_run(desc))
		goto out_unlock;

	desc->istate &= ~(IRQS_REPLAY | IRQS_WAITING);

	/*
	 * If its disabled or no action available
	 * keep it masked and get out of here
	 */
	if (unlikely(!desc->action || irqd_irq_disabled(&desc->irq_data))) {
		desc->istate |= IRQS_PENDING;
		goto out_unlock;
	}

	kstat_incr_irqs_this_cpu(desc);
	handle_irq_event(desc);

	cond_unmask_irq(desc);

out_unlock:
	raw_spin_unlock(&desc->lock);
}
EXPORT_SYMBOL_GPL(handle_level_irq);

/*
 * IAMROOT, 2022.11.12:
 * - irq_eoi + unmask를 상황에 따라 처리한다.
 * - 다음과 같은 조건들을 판단한다.
 *   IRQS_ONESHOT
 *   desc->threads_oneshot
 *   irqd_irq_masked
 *   irqd_irq_disabled
 *   IRQCHIP_EOI_THREADED
 */
static void cond_unmask_eoi_irq(struct irq_desc *desc, struct irq_chip *chip)
{

/*
 * IAMROOT, 2022.11.12:
 * - oneshot이 아니면 irq_eoi만 처리.
 */
	if (!(desc->istate & IRQS_ONESHOT)) {
		chip->irq_eoi(&desc->irq_data);
		return;
	}
	/*
	 * We need to unmask in the following cases:
	 * - Oneshot irq which did not wake the thread (caused by a
	 *   spurious interrupt or a primary handler handling it
	 *   completely).
	 */
/*
 * IAMROOT, 2022.11.12:
 * - papago
 *   다음과 같은 경우 마스크를 해제해야 합니다.
 *   - 스레드를 깨우지 않은 Oneshot irq(spurious 인터럽트 또
 *     이를 완전히 처리하는 기본 핸들러로 인해 발생).
 *
 * - thread인 경우 isr이 매우 짧다(thread만 생성하고 끝나기때문에)
 *   이런 경우 만약 isr만 처리하고 eoi를 보낼경우 level 인경우 매우많은
 *   thread가 생길 위험이 있다. 이 경우에 대한 처리등을 진행한다. 즉
 *   1.!irqd_irq_disabled(&desc->irq_data) => irq가 enable상태
 *     irqd_irq_masked(&desc->irq_data)    => mask되있는 상태.
 *     !desc->threads_oneshot              => oneshot thread가 아닌 상태
 *     이 경우 eoi를 하고 다시 unmask를 하여 irq를 받아도 되는 상태.
 *   2. !(chip->flags & IRQCHIP_EOI_THREADED) => thread에서 eoi처리를 
 *     안하는 상황. 즉 flow handler에서 처리한다.
 *   3. thread에서 처리하거나, thread가 oneshot인 경우등은 eoi를
 *      여기서 처리 안한다.
 *
 */
	if (!irqd_irq_disabled(&desc->irq_data) &&
	    irqd_irq_masked(&desc->irq_data) && !desc->threads_oneshot) {
		chip->irq_eoi(&desc->irq_data);
		unmask_irq(desc);
	} else if (!(chip->flags & IRQCHIP_EOI_THREADED)) {
		chip->irq_eoi(&desc->irq_data);
	}
}

/**
 *	handle_fasteoi_irq - irq handler for transparent controllers
 *	@desc:	the interrupt description structure for this irq
 *
 *	Only a single callback will be issued to the chip: an ->eoi()
 *	call when the interrupt has been serviced. This enables support
 *	for modern forms of interrupt handlers, which handle the flow
 *	details in hardware, transparently.
 */

/*
 * IAMROOT, 2022.11.12:
 * - 1. running 가능한지 확인한다.
 *   2. oneshot인경우 masking.
 */
void handle_fasteoi_irq(struct irq_desc *desc)
{
	struct irq_chip *chip = desc->irq_data.chip;

	raw_spin_lock(&desc->lock);

	if (!irq_may_run(desc))
		goto out;

	desc->istate &= ~(IRQS_REPLAY | IRQS_WAITING);

	/*
	 * If its disabled or no action available
	 * then mask it and get out of here:
	 */

/*
 * IAMROOT, 2022.11.12:
 * - running할수있는 단계까지 왓는데 @desc가 action handler가 없거나
 *   irq disable상태다. pending으로 표시 및 masking하고 out.
 *   추후에 enable될때 edge trigger만 재전송한다.
 */
	if (unlikely(!desc->action || irqd_irq_disabled(&desc->irq_data))) {
		desc->istate |= IRQS_PENDING;
		mask_irq(desc);
		goto out;
	}

	kstat_incr_irqs_this_cpu(desc);

/*
 * IAMROOT, 2022.11.12:
 * - oneshot이면 @desc를 masking한다.
 */
	if (desc->istate & IRQS_ONESHOT)
		mask_irq(desc);

/*
 * IAMROOT, 2022.11.12:
 * - action event handler 처리.
 */
	handle_irq_event(desc);

/*
 * IAMROOT, 2022.11.12:
 * - eoi + unmask 처리 여부 판단
 */
	cond_unmask_eoi_irq(desc, chip);

	raw_spin_unlock(&desc->lock);
	return;
out:
	if (!(chip->flags & IRQCHIP_EOI_IF_HANDLED))
		chip->irq_eoi(&desc->irq_data);
	raw_spin_unlock(&desc->lock);
}
EXPORT_SYMBOL_GPL(handle_fasteoi_irq);

/**
 *	handle_fasteoi_nmi - irq handler for NMI interrupt lines
 *	@desc:	the interrupt description structure for this irq
 *
 *	A simple NMI-safe handler, considering the restrictions
 *	from request_nmi.
 *
 *	Only a single callback will be issued to the chip: an ->eoi()
 *	call when the interrupt has been serviced. This enables support
 *	for modern forms of interrupt handlers, which handle the flow
 *	details in hardware, transparently.
 */
void handle_fasteoi_nmi(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);
	struct irqaction *action = desc->action;
	unsigned int irq = irq_desc_get_irq(desc);
	irqreturn_t res;

	__kstat_incr_irqs_this_cpu(desc);

	trace_irq_handler_entry(irq, action);
	/*
	 * NMIs cannot be shared, there is only one action.
	 */
	res = action->handler(irq, action->dev_id);
	trace_irq_handler_exit(irq, action, res);

	if (chip->irq_eoi)
		chip->irq_eoi(&desc->irq_data);
}
EXPORT_SYMBOL_GPL(handle_fasteoi_nmi);

/**
 *	handle_edge_irq - edge type IRQ handler
 *	@desc:	the interrupt description structure for this irq
 *
 *	Interrupt occurs on the falling and/or rising edge of a hardware
 *	signal. The occurrence is latched into the irq controller hardware
 *	and must be acked in order to be reenabled. After the ack another
 *	interrupt can happen on the same source even before the first one
 *	is handled by the associated event handler. If this happens it
 *	might be necessary to disable (mask) the interrupt depending on the
 *	controller hardware. This requires to reenable the interrupt inside
 *	of the loop which handles the interrupts which have arrived while
 *	the handler was running. If all pending interrupts are handled, the
 *	loop is left.
 */
void handle_edge_irq(struct irq_desc *desc)
{
	raw_spin_lock(&desc->lock);

	desc->istate &= ~(IRQS_REPLAY | IRQS_WAITING);

	if (!irq_may_run(desc)) {
		desc->istate |= IRQS_PENDING;
		mask_ack_irq(desc);
		goto out_unlock;
	}

	/*
	 * If its disabled or no action available then mask it and get
	 * out of here.
	 */
	if (irqd_irq_disabled(&desc->irq_data) || !desc->action) {
		desc->istate |= IRQS_PENDING;
		mask_ack_irq(desc);
		goto out_unlock;
	}

	kstat_incr_irqs_this_cpu(desc);

	/* Start handling the irq */
	desc->irq_data.chip->irq_ack(&desc->irq_data);

	do {
		if (unlikely(!desc->action)) {
			mask_irq(desc);
			goto out_unlock;
		}

		/*
		 * When another irq arrived while we were handling
		 * one, we could have masked the irq.
		 * Reenable it, if it was not disabled in meantime.
		 */
		if (unlikely(desc->istate & IRQS_PENDING)) {
			if (!irqd_irq_disabled(&desc->irq_data) &&
			    irqd_irq_masked(&desc->irq_data))
				unmask_irq(desc);
		}

		handle_irq_event(desc);

	} while ((desc->istate & IRQS_PENDING) &&
		 !irqd_irq_disabled(&desc->irq_data));

out_unlock:
	raw_spin_unlock(&desc->lock);
}
EXPORT_SYMBOL(handle_edge_irq);

#ifdef CONFIG_IRQ_EDGE_EOI_HANDLER
/**
 *	handle_edge_eoi_irq - edge eoi type IRQ handler
 *	@desc:	the interrupt description structure for this irq
 *
 * Similar as the above handle_edge_irq, but using eoi and w/o the
 * mask/unmask logic.
 */
void handle_edge_eoi_irq(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);

	raw_spin_lock(&desc->lock);

	desc->istate &= ~(IRQS_REPLAY | IRQS_WAITING);

	if (!irq_may_run(desc)) {
		desc->istate |= IRQS_PENDING;
		goto out_eoi;
	}

	/*
	 * If its disabled or no action available then mask it and get
	 * out of here.
	 */
	if (irqd_irq_disabled(&desc->irq_data) || !desc->action) {
		desc->istate |= IRQS_PENDING;
		goto out_eoi;
	}

	kstat_incr_irqs_this_cpu(desc);

	do {
		if (unlikely(!desc->action))
			goto out_eoi;

		handle_irq_event(desc);

	} while ((desc->istate & IRQS_PENDING) &&
		 !irqd_irq_disabled(&desc->irq_data));

out_eoi:
	chip->irq_eoi(&desc->irq_data);
	raw_spin_unlock(&desc->lock);
}
#endif

/**
 *	handle_percpu_irq - Per CPU local irq handler
 *	@desc:	the interrupt description structure for this irq
 *
 *	Per CPU interrupts on SMP machines without locking requirements
 */
void handle_percpu_irq(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);

	/*
	 * PER CPU interrupts are not serialized. Do not touch
	 * desc->tot_count.
	 */
	__kstat_incr_irqs_this_cpu(desc);

	if (chip->irq_ack)
		chip->irq_ack(&desc->irq_data);

	handle_irq_event_percpu(desc);

	if (chip->irq_eoi)
		chip->irq_eoi(&desc->irq_data);
}

/**
 * handle_percpu_devid_irq - Per CPU local irq handler with per cpu dev ids
 * @desc:	the interrupt description structure for this irq
 *
 * Per CPU interrupts on SMP machines without locking requirements. Same as
 * handle_percpu_irq() above but with the following extras:
 *
 * action->percpu_dev_id is a pointer to percpu variables which
 * contain the real device id for the cpu on which this handler is
 * called
 */
/*
 * IAMROOT, 2022.11.12:
 * - papago
 *   handle_percpu_devid_irq - per cpu dev ID가 있는 per cpu local irq handler
 *   @desc: 이 irq에 대한 인터럽트 설명 구조.
 *
 *   잠금 요구 사항이 없는 SMP 시스템의 CPU당 인터럽트. 위의
 *   handle_percpu_irq()와 동일하지만 다음과 같은 추가 사항이 있습니다. 
 *
 *   action->percpu_dev_id는 이 핸들러가 호출되는 CPU의 실제 장치 ID를
 *   포함하는 percpu 변수에 대한 포인터입니다.
 *
 * - action handler를 실행한다. 실행전후로 ack 및 irq_eoi를 수행한다.
 */
void handle_percpu_devid_irq(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);
	struct irqaction *action = desc->action;
	unsigned int irq = irq_desc_get_irq(desc);
	irqreturn_t res;

	/*
	 * PER CPU interrupts are not serialized. Do not touch
	 * desc->tot_count.
	 */
	__kstat_incr_irqs_this_cpu(desc);

/*
 * IAMROOT, 2022.11.12:
 * - irq_ack가 있으면 여기서 보낸다. gic경우는 iar을 읽을시 자동으로 ack가
 *   나가는 구조라 없다.
 */
	if (chip->irq_ack)
		chip->irq_ack(&desc->irq_data);

/*
 * IAMROOT, 2022.11.12:
 * - action이 없는데 들어왔다면 이상한 상황. irq disable한다.
 */
	if (likely(action)) {
		trace_irq_handler_entry(irq, action);
		res = action->handler(irq, raw_cpu_ptr(action->percpu_dev_id));
		trace_irq_handler_exit(irq, action, res);
	} else {
		unsigned int cpu = smp_processor_id();
		bool enabled = cpumask_test_cpu(cpu, desc->percpu_enabled);

		if (enabled)
			irq_percpu_disable(desc, cpu);

		pr_err_once("Spurious%s percpu IRQ%u on CPU%u\n",
			    enabled ? " and unmasked" : "", irq, cpu);
	}

	if (chip->irq_eoi)
		chip->irq_eoi(&desc->irq_data);
}

/**
 * handle_percpu_devid_fasteoi_nmi - Per CPU local NMI handler with per cpu
 *				     dev ids
 * @desc:	the interrupt description structure for this irq
 *
 * Similar to handle_fasteoi_nmi, but handling the dev_id cookie
 * as a percpu pointer.
 */
void handle_percpu_devid_fasteoi_nmi(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);
	struct irqaction *action = desc->action;
	unsigned int irq = irq_desc_get_irq(desc);
	irqreturn_t res;

	__kstat_incr_irqs_this_cpu(desc);

	trace_irq_handler_entry(irq, action);
	res = action->handler(irq, raw_cpu_ptr(action->percpu_dev_id));
	trace_irq_handler_exit(irq, action, res);

	if (chip->irq_eoi)
		chip->irq_eoi(&desc->irq_data);
}

/*
 * IAMROOT, 2022.10.08:
 * - irq desc의 flow handler 설정을한다.
 *   성공했으면 irq startup까지 수행한다.
 */
static void
__irq_do_set_handler(struct irq_desc *desc, irq_flow_handler_t handle,
		     int is_chained, const char *name)
{
	if (!handle) {
		handle = handle_bad_irq;
	} else {
		struct irq_data *irq_data = &desc->irq_data;
#ifdef CONFIG_IRQ_DOMAIN_HIERARCHY
		/*
		 * With hierarchical domains we might run into a
		 * situation where the outermost chip is not yet set
		 * up, but the inner chips are there.  Instead of
		 * bailing we install the handler, but obviously we
		 * cannot enable/startup the interrupt at this point.
		 */
		while (irq_data) {
/*
 * IAMROOT, 2022.10.01:
 * - chip 설정이 안된 irq_data를 찾는다.
 */
			if (irq_data->chip != &no_irq_chip)
				break;
			/*
			 * Bail out if the outer chip is not set up
			 * and the interrupt supposed to be started
			 * right away.
			 */
			if (WARN_ON(is_chained))
				return;
			/* Try the parent */
			irq_data = irq_data->parent_data;
		}
#endif
		if (WARN_ON(!irq_data || irq_data->chip == &no_irq_chip))
			return;
	}

	/* Uninstall? */
	if (handle == handle_bad_irq) {
		if (desc->irq_data.chip != &no_irq_chip)
			mask_ack_irq(desc);
		irq_state_set_disabled(desc);
		if (is_chained)
			desc->action = NULL;
		desc->depth = 1;
	}

	desc->handle_irq = handle;
	desc->name = name;

/*
 * IAMROOT, 2022.10.01:
 * - install이 됬고 계층구조에서 child로 요청된경우.
 */
	if (handle != handle_bad_irq && is_chained) {
		unsigned int type = irqd_get_trigger_type(&desc->irq_data);

		/*
		 * We're about to start this interrupt immediately,
		 * hence the need to set the trigger configuration.
		 * But the .set_type callback may have overridden the
		 * flow handler, ignoring that we're dealing with a
		 * chained interrupt. Reset it immediately because we
		 * do know better.
		 */
		if (type != IRQ_TYPE_NONE) {
			__irq_set_trigger(desc, type);
			desc->handle_irq = handle;
		}

/*
 * prifri, 2022.10.01:
 * - bit flag set.
 */
		irq_settings_set_noprobe(desc);
		irq_settings_set_norequest(desc);
		irq_settings_set_nothread(desc);
		desc->action = &chained_action;
		irq_activate_and_startup(desc, IRQ_RESEND);
	}
}

/*
 * IAMROOT, 2022.10.01:
 * - @handle을 set한다.
 */
void
__irq_set_handler(unsigned int irq, irq_flow_handler_t handle, int is_chained,
		  const char *name)
{
	unsigned long flags;
	struct irq_desc *desc = irq_get_desc_buslock(irq, &flags, 0);

	if (!desc)
		return;

	__irq_do_set_handler(desc, handle, is_chained, name);
	irq_put_desc_busunlock(desc, flags);
}
EXPORT_SYMBOL_GPL(__irq_set_handler);

void
irq_set_chained_handler_and_data(unsigned int irq, irq_flow_handler_t handle,
				 void *data)
{
	unsigned long flags;
	struct irq_desc *desc = irq_get_desc_buslock(irq, &flags, 0);

	if (!desc)
		return;

	desc->irq_common_data.handler_data = data;
	__irq_do_set_handler(desc, handle, 1, NULL);

	irq_put_desc_busunlock(desc, flags);
}
EXPORT_SYMBOL_GPL(irq_set_chained_handler_and_data);

void
irq_set_chip_and_handler_name(unsigned int irq, struct irq_chip *chip,
			      irq_flow_handler_t handle, const char *name)
{
	irq_set_chip(irq, chip);
	__irq_set_handler(irq, handle, 0, name);
}
EXPORT_SYMBOL_GPL(irq_set_chip_and_handler_name);

/*
 * IAMROOT, 2022.10.01:
 * - @irq에 대한 irq_desc를 구해와 flag를 @clr, @set한다.
 */
void irq_modify_status(unsigned int irq, unsigned long clr, unsigned long set)
{
	unsigned long flags, trigger, tmp;
	struct irq_desc *desc = irq_get_desc_lock(irq, &flags, 0);

	if (!desc)
		return;

	/*
	 * Warn when a driver sets the no autoenable flag on an already
	 * active interrupt.
	 */
	WARN_ON_ONCE(!desc->depth && (set & _IRQ_NOAUTOEN));

	irq_settings_clr_and_set(desc, clr, set);

	trigger = irqd_get_trigger_type(&desc->irq_data);

	irqd_clear(&desc->irq_data, IRQD_NO_BALANCING | IRQD_PER_CPU |
		   IRQD_TRIGGER_MASK | IRQD_LEVEL | IRQD_MOVE_PCNTXT);
	if (irq_settings_has_no_balance_set(desc))
		irqd_set(&desc->irq_data, IRQD_NO_BALANCING);
	if (irq_settings_is_per_cpu(desc))
		irqd_set(&desc->irq_data, IRQD_PER_CPU);
	if (irq_settings_can_move_pcntxt(desc))
		irqd_set(&desc->irq_data, IRQD_MOVE_PCNTXT);
	if (irq_settings_is_level(desc))
		irqd_set(&desc->irq_data, IRQD_LEVEL);

	tmp = irq_settings_get_trigger_mask(desc);
	if (tmp != IRQ_TYPE_NONE)
		trigger = tmp;

	irqd_set(&desc->irq_data, trigger);

	irq_put_desc_unlock(desc, flags);
}
EXPORT_SYMBOL_GPL(irq_modify_status);

/**
 *	irq_cpu_online - Invoke all irq_cpu_online functions.
 *
 *	Iterate through all irqs and invoke the chip.irq_cpu_online()
 *	for each.
 */
void irq_cpu_online(void)
{
	struct irq_desc *desc;
	struct irq_chip *chip;
	unsigned long flags;
	unsigned int irq;

	for_each_active_irq(irq) {
		desc = irq_to_desc(irq);
		if (!desc)
			continue;

		raw_spin_lock_irqsave(&desc->lock, flags);

		chip = irq_data_get_irq_chip(&desc->irq_data);
		if (chip && chip->irq_cpu_online &&
		    (!(chip->flags & IRQCHIP_ONOFFLINE_ENABLED) ||
		     !irqd_irq_disabled(&desc->irq_data)))
			chip->irq_cpu_online(&desc->irq_data);

		raw_spin_unlock_irqrestore(&desc->lock, flags);
	}
}

/**
 *	irq_cpu_offline - Invoke all irq_cpu_offline functions.
 *
 *	Iterate through all irqs and invoke the chip.irq_cpu_offline()
 *	for each.
 */
void irq_cpu_offline(void)
{
	struct irq_desc *desc;
	struct irq_chip *chip;
	unsigned long flags;
	unsigned int irq;

	for_each_active_irq(irq) {
		desc = irq_to_desc(irq);
		if (!desc)
			continue;

		raw_spin_lock_irqsave(&desc->lock, flags);

		chip = irq_data_get_irq_chip(&desc->irq_data);
		if (chip && chip->irq_cpu_offline &&
		    (!(chip->flags & IRQCHIP_ONOFFLINE_ENABLED) ||
		     !irqd_irq_disabled(&desc->irq_data)))
			chip->irq_cpu_offline(&desc->irq_data);

		raw_spin_unlock_irqrestore(&desc->lock, flags);
	}
}

#ifdef	CONFIG_IRQ_DOMAIN_HIERARCHY

#ifdef CONFIG_IRQ_FASTEOI_HIERARCHY_HANDLERS
/**
 *	handle_fasteoi_ack_irq - irq handler for edge hierarchy
 *	stacked on transparent controllers
 *
 *	@desc:	the interrupt description structure for this irq
 *
 *	Like handle_fasteoi_irq(), but for use with hierarchy where
 *	the irq_chip also needs to have its ->irq_ack() function
 *	called.
 */
void handle_fasteoi_ack_irq(struct irq_desc *desc)
{
	struct irq_chip *chip = desc->irq_data.chip;

	raw_spin_lock(&desc->lock);

	if (!irq_may_run(desc))
		goto out;

	desc->istate &= ~(IRQS_REPLAY | IRQS_WAITING);

	/*
	 * If its disabled or no action available
	 * then mask it and get out of here:
	 */
	if (unlikely(!desc->action || irqd_irq_disabled(&desc->irq_data))) {
		desc->istate |= IRQS_PENDING;
		mask_irq(desc);
		goto out;
	}

	kstat_incr_irqs_this_cpu(desc);
	if (desc->istate & IRQS_ONESHOT)
		mask_irq(desc);

	/* Start handling the irq */
	desc->irq_data.chip->irq_ack(&desc->irq_data);

	handle_irq_event(desc);

	cond_unmask_eoi_irq(desc, chip);

	raw_spin_unlock(&desc->lock);
	return;
out:
	if (!(chip->flags & IRQCHIP_EOI_IF_HANDLED))
		chip->irq_eoi(&desc->irq_data);
	raw_spin_unlock(&desc->lock);
}
EXPORT_SYMBOL_GPL(handle_fasteoi_ack_irq);

/**
 *	handle_fasteoi_mask_irq - irq handler for level hierarchy
 *	stacked on transparent controllers
 *
 *	@desc:	the interrupt description structure for this irq
 *
 *	Like handle_fasteoi_irq(), but for use with hierarchy where
 *	the irq_chip also needs to have its ->irq_mask_ack() function
 *	called.
 */
void handle_fasteoi_mask_irq(struct irq_desc *desc)
{
	struct irq_chip *chip = desc->irq_data.chip;

	raw_spin_lock(&desc->lock);
	mask_ack_irq(desc);

	if (!irq_may_run(desc))
		goto out;

	desc->istate &= ~(IRQS_REPLAY | IRQS_WAITING);

	/*
	 * If its disabled or no action available
	 * then mask it and get out of here:
	 */
	if (unlikely(!desc->action || irqd_irq_disabled(&desc->irq_data))) {
		desc->istate |= IRQS_PENDING;
		mask_irq(desc);
		goto out;
	}

	kstat_incr_irqs_this_cpu(desc);
	if (desc->istate & IRQS_ONESHOT)
		mask_irq(desc);

	handle_irq_event(desc);

	cond_unmask_eoi_irq(desc, chip);

	raw_spin_unlock(&desc->lock);
	return;
out:
	if (!(chip->flags & IRQCHIP_EOI_IF_HANDLED))
		chip->irq_eoi(&desc->irq_data);
	raw_spin_unlock(&desc->lock);
}
EXPORT_SYMBOL_GPL(handle_fasteoi_mask_irq);

#endif /* CONFIG_IRQ_FASTEOI_HIERARCHY_HANDLERS */

/**
 * irq_chip_set_parent_state - set the state of a parent interrupt.
 *
 * @data: Pointer to interrupt specific data
 * @which: State to be restored (one of IRQCHIP_STATE_*)
 * @val: Value corresponding to @which
 *
 * Conditional success, if the underlying irqchip does not implement it.
 */
int irq_chip_set_parent_state(struct irq_data *data,
			      enum irqchip_irq_state which,
			      bool val)
{
	data = data->parent_data;

	if (!data || !data->chip->irq_set_irqchip_state)
		return 0;

	return data->chip->irq_set_irqchip_state(data, which, val);
}
EXPORT_SYMBOL_GPL(irq_chip_set_parent_state);

/**
 * irq_chip_get_parent_state - get the state of a parent interrupt.
 *
 * @data: Pointer to interrupt specific data
 * @which: one of IRQCHIP_STATE_* the caller wants to know
 * @state: a pointer to a boolean where the state is to be stored
 *
 * Conditional success, if the underlying irqchip does not implement it.
 */
int irq_chip_get_parent_state(struct irq_data *data,
			      enum irqchip_irq_state which,
			      bool *state)
{
	data = data->parent_data;

	if (!data || !data->chip->irq_get_irqchip_state)
		return 0;

	return data->chip->irq_get_irqchip_state(data, which, state);
}
EXPORT_SYMBOL_GPL(irq_chip_get_parent_state);

/**
 * irq_chip_enable_parent - Enable the parent interrupt (defaults to unmask if
 * NULL)
 * @data:	Pointer to interrupt specific data
 */
void irq_chip_enable_parent(struct irq_data *data)
{
	data = data->parent_data;
	if (data->chip->irq_enable)
		data->chip->irq_enable(data);
	else
		data->chip->irq_unmask(data);
}
EXPORT_SYMBOL_GPL(irq_chip_enable_parent);

/**
 * irq_chip_disable_parent - Disable the parent interrupt (defaults to mask if
 * NULL)
 * @data:	Pointer to interrupt specific data
 */
void irq_chip_disable_parent(struct irq_data *data)
{
	data = data->parent_data;
	if (data->chip->irq_disable)
		data->chip->irq_disable(data);
	else
		data->chip->irq_mask(data);
}
EXPORT_SYMBOL_GPL(irq_chip_disable_parent);

/**
 * irq_chip_ack_parent - Acknowledge the parent interrupt
 * @data:	Pointer to interrupt specific data
 */
void irq_chip_ack_parent(struct irq_data *data)
{
	data = data->parent_data;
	data->chip->irq_ack(data);
}
EXPORT_SYMBOL_GPL(irq_chip_ack_parent);

/**
 * irq_chip_mask_parent - Mask the parent interrupt
 * @data:	Pointer to interrupt specific data
 */
void irq_chip_mask_parent(struct irq_data *data)
{
	data = data->parent_data;
	data->chip->irq_mask(data);
}
EXPORT_SYMBOL_GPL(irq_chip_mask_parent);

/**
 * irq_chip_mask_ack_parent - Mask and acknowledge the parent interrupt
 * @data:	Pointer to interrupt specific data
 */
void irq_chip_mask_ack_parent(struct irq_data *data)
{
	data = data->parent_data;
	data->chip->irq_mask_ack(data);
}
EXPORT_SYMBOL_GPL(irq_chip_mask_ack_parent);

/**
 * irq_chip_unmask_parent - Unmask the parent interrupt
 * @data:	Pointer to interrupt specific data
 */
void irq_chip_unmask_parent(struct irq_data *data)
{
	data = data->parent_data;
	data->chip->irq_unmask(data);
}
EXPORT_SYMBOL_GPL(irq_chip_unmask_parent);

/**
 * irq_chip_eoi_parent - Invoke EOI on the parent interrupt
 * @data:	Pointer to interrupt specific data
 */
void irq_chip_eoi_parent(struct irq_data *data)
{
	data = data->parent_data;
	data->chip->irq_eoi(data);
}
EXPORT_SYMBOL_GPL(irq_chip_eoi_parent);

/**
 * irq_chip_set_affinity_parent - Set affinity on the parent interrupt
 * @data:	Pointer to interrupt specific data
 * @dest:	The affinity mask to set
 * @force:	Flag to enforce setting (disable online checks)
 *
 * Conditional, as the underlying parent chip might not implement it.
 */
int irq_chip_set_affinity_parent(struct irq_data *data,
				 const struct cpumask *dest, bool force)
{
	data = data->parent_data;
	if (data->chip->irq_set_affinity)
		return data->chip->irq_set_affinity(data, dest, force);

	return -ENOSYS;
}
EXPORT_SYMBOL_GPL(irq_chip_set_affinity_parent);

/**
 * irq_chip_set_type_parent - Set IRQ type on the parent interrupt
 * @data:	Pointer to interrupt specific data
 * @type:	IRQ_TYPE_{LEVEL,EDGE}_* value - see include/linux/irq.h
 *
 * Conditional, as the underlying parent chip might not implement it.
 */
int irq_chip_set_type_parent(struct irq_data *data, unsigned int type)
{
	data = data->parent_data;

	if (data->chip->irq_set_type)
		return data->chip->irq_set_type(data, type);

	return -ENOSYS;
}
EXPORT_SYMBOL_GPL(irq_chip_set_type_parent);

/**
 * irq_chip_retrigger_hierarchy - Retrigger an interrupt in hardware
 * @data:	Pointer to interrupt specific data
 *
 * Iterate through the domain hierarchy of the interrupt and check
 * whether a hw retrigger function exists. If yes, invoke it.
 */

/*
 * IAMROOT, 2022.10.08:
 * - 상위로 올라가면서 irq_retrigger수행한다.
 */
int irq_chip_retrigger_hierarchy(struct irq_data *data)
{
	for (data = data->parent_data; data; data = data->parent_data)
		if (data->chip && data->chip->irq_retrigger)
			return data->chip->irq_retrigger(data);

	return 0;
}
EXPORT_SYMBOL_GPL(irq_chip_retrigger_hierarchy);

/**
 * irq_chip_set_vcpu_affinity_parent - Set vcpu affinity on the parent interrupt
 * @data:	Pointer to interrupt specific data
 * @vcpu_info:	The vcpu affinity information
 */
int irq_chip_set_vcpu_affinity_parent(struct irq_data *data, void *vcpu_info)
{
	data = data->parent_data;
	if (data->chip->irq_set_vcpu_affinity)
		return data->chip->irq_set_vcpu_affinity(data, vcpu_info);

	return -ENOSYS;
}
EXPORT_SYMBOL_GPL(irq_chip_set_vcpu_affinity_parent);
/**
 * irq_chip_set_wake_parent - Set/reset wake-up on the parent interrupt
 * @data:	Pointer to interrupt specific data
 * @on:		Whether to set or reset the wake-up capability of this irq
 *
 * Conditional, as the underlying parent chip might not implement it.
 */
int irq_chip_set_wake_parent(struct irq_data *data, unsigned int on)
{
	data = data->parent_data;

	if (data->chip->flags & IRQCHIP_SKIP_SET_WAKE)
		return 0;

	if (data->chip->irq_set_wake)
		return data->chip->irq_set_wake(data, on);

	return -ENOSYS;
}
EXPORT_SYMBOL_GPL(irq_chip_set_wake_parent);

/**
 * irq_chip_request_resources_parent - Request resources on the parent interrupt
 * @data:	Pointer to interrupt specific data
 */
int irq_chip_request_resources_parent(struct irq_data *data)
{
	data = data->parent_data;

	if (data->chip->irq_request_resources)
		return data->chip->irq_request_resources(data);

	return -ENOSYS;
}
EXPORT_SYMBOL_GPL(irq_chip_request_resources_parent);

/**
 * irq_chip_release_resources_parent - Release resources on the parent interrupt
 * @data:	Pointer to interrupt specific data
 */
void irq_chip_release_resources_parent(struct irq_data *data)
{
	data = data->parent_data;
	if (data->chip->irq_release_resources)
		data->chip->irq_release_resources(data);
}
EXPORT_SYMBOL_GPL(irq_chip_release_resources_parent);
#endif

/**
 * irq_chip_compose_msi_msg - Compose msi message for a irq chip
 * @data:	Pointer to interrupt specific data
 * @msg:	Pointer to the MSI message
 *
 * For hierarchical domains we find the first chip in the hierarchy
 * which implements the irq_compose_msi_msg callback. For non
 * hierarchical we use the top level chip.
 */
int irq_chip_compose_msi_msg(struct irq_data *data, struct msi_msg *msg)
{
	struct irq_data *pos;

	for (pos = NULL; !pos && data; data = irqd_get_parent_data(data)) {
		if (data->chip && data->chip->irq_compose_msi_msg)
			pos = data;
	}

	if (!pos)
		return -ENOSYS;

	pos->chip->irq_compose_msi_msg(pos, msg);
	return 0;
}

/**
 * irq_chip_pm_get - Enable power for an IRQ chip
 * @data:	Pointer to interrupt specific data
 *
 * Enable the power to the IRQ chip referenced by the interrupt data
 * structure.
 */
/*
 * IAMROOT, 2022.10.15:
 * - chip control를 절전상태라면 깨운다.
 */
int irq_chip_pm_get(struct irq_data *data)
{
	int retval;

	if (IS_ENABLED(CONFIG_PM) && data->chip->parent_device) {

/*
 * IAMROOT, 2022.10.15:
 * - 동기화(기다리면서) 깨운다 라는것.
 */
		retval = pm_runtime_get_sync(data->chip->parent_device);
		if (retval < 0) {
			pm_runtime_put_noidle(data->chip->parent_device);
			return retval;
		}
	}

	return 0;
}

/**
 * irq_chip_pm_put - Disable power for an IRQ chip
 * @data:	Pointer to interrupt specific data
 *
 * Disable the power to the IRQ chip referenced by the interrupt data
 * structure, belongs. Note that power will only be disabled, once this
 * function has been called for all IRQs that have called irq_chip_pm_get().
 */
int irq_chip_pm_put(struct irq_data *data)
{
	int retval = 0;

	if (IS_ENABLED(CONFIG_PM) && data->chip->parent_device)
		retval = pm_runtime_put(data->chip->parent_device);

	return (retval < 0) ? retval : 0;
}
