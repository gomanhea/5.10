// SPDX-License-Identifier: GPL-2.0
/*
 * This file contains the base functions to manage periodic tick
 * related events.
 *
 * Copyright(C) 2005-2006, Thomas Gleixner <tglx@linutronix.de>
 * Copyright(C) 2005-2007, Red Hat, Inc., Ingo Molnar
 * Copyright(C) 2006-2007, Timesys Corp., Thomas Gleixner
 */
#include <linux/cpu.h>
#include <linux/err.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/nmi.h>
#include <linux/percpu.h>
#include <linux/profile.h>
#include <linux/sched.h>
#include <linux/module.h>
#include <trace/events/power.h>

#include <asm/irq_regs.h>

#include "tick-internal.h"

/*
 * Tick devices
 */
/*
 * IAMROOT, 2022.08.27:
 * - sche tick. 
 */
DEFINE_PER_CPU(struct tick_device, tick_cpu_device);
/*
 * Tick next event: keeps track of the tick time. It's updated by the
 * CPU which handles the tick and protected by jiffies_lock. There is
 * no requirement to write hold the jiffies seqcount for it.
 */
/*
 * IAMROOT, 2022.12.03:
 * - papago
 *   다음 이벤트 선택: 틱 시간을 추적합니다. 틱을 처리하는 CPU에 의해 업데이트되고
 *   jiffies_lock에 의해 보호됩니다. 그것에 대한 jiffies seqcount를 보류할 필요는 없습니다.
 *
 * - 1tick이 더해진 next period를 저장해놓는다.
 */
ktime_t tick_next_period;

/*
 * tick_do_timer_cpu is a timer core internal variable which holds the CPU NR
 * which is responsible for calling do_timer(), i.e. the timekeeping stuff. This
 * variable has two functions:
 *
 * 1) Prevent a thundering herd issue of a gazillion of CPUs trying to grab the
 *    timekeeping lock all at once. Only the CPU which is assigned to do the
 *    update is handling it.
 *
 * 2) Hand off the duty in the NOHZ idle case by setting the value to
 *    TICK_DO_TIMER_NONE, i.e. a non existing CPU. So the next cpu which looks
 *    at it will take over and keep the time keeping alive.  The handover
 *    procedure also covers cpu hotplug.
 */
/*
 * IAMROOT, 2022.12.03:
 * - papago
 *   tick_do_timer_cpu는 do_timer() 호출을 담당하는 CPU NR을 보유하는 타이머 코어 내부
 *   변수입니다. 이 변수에는 두 가지 기능이 있습니다.
 *
 *   1) 시간 기록 잠금을 한 번에 모두 잡으려고 시도하는 수많은 CPU의 천둥 무리 문제를
 *   방지합니다. 업데이트를 수행하도록 할당된 CPU만 업데이트를 처리합니다.
 *
 *   2) 값을 TICK_DO_TIMER_NONE(즉, 존재하지 않는 CPU)으로 설정하여 NOHZ 유휴 상태에서 작업을
 *   해제합니다. 따라서 그것을 보는 다음 CPU가 시간을 이어받아 계속 살아있게 합니다. 핸드오버
 *   절차에는 CPU 핫플러그도 포함됩니다.
 *
 * - 시간 갱신을 하는 cpu가 저장된다. 결정되기 전에는 TICK_DO_TIMER_BOOT가 저장되있다.
 */
int tick_do_timer_cpu __read_mostly = TICK_DO_TIMER_BOOT;
#ifdef CONFIG_NO_HZ_FULL
/*
 * tick_do_timer_boot_cpu indicates the boot CPU temporarily owns
 * tick_do_timer_cpu and it should be taken over by an eligible secondary
 * when one comes online.
 */
static int tick_do_timer_boot_cpu __read_mostly = -1;
#endif

/*
 * Debugging: see timer_list.c
 */
struct tick_device *tick_get_device(int cpu)
{
	return &per_cpu(tick_cpu_device, cpu);
}

/**
 * tick_is_oneshot_available - check for a oneshot capable event device
 */
/*
 * IAMROOT, 2022.12.03:
 * - oneshot이 enable되있는지 확인한다.
 */
int tick_is_oneshot_available(void)
{
	struct clock_event_device *dev = __this_cpu_read(tick_cpu_device.evtdev);

	if (!dev || !(dev->features & CLOCK_EVT_FEAT_ONESHOT))
		return 0;

/*
 * IAMROOT, 2022.12.03:
 * - clock 절전기능이 없으면 return 1.
 *   이 기능이 없는경우 전원을 아에 껏다가 켜야되는데, 짧은 시간을 껏다 켜야되는경우
 *   성능이 오히려 느려져 시스템에 영향이 있다.
 */
	if (!(dev->features & CLOCK_EVT_FEAT_C3STOP))
		return 1;
	return tick_broadcast_oneshot_available();
}

/*
 * Periodic tick
 */
/*
 * IAMROOT, 2022.12.03:
 * - @cpu가 jiffies을 담당한다면 시간 갱신을 한다.
 * - timer event가 발생할 예정인지 확인하고 그럴 경우 해당 타이머 기능이
 *   실행되도록 예약한다.
 * - user_mode에서 실행중인 process에 대한 런타임 통계를 업데이트.
 * - @cpu에 대한 profile update.
 */
static void tick_periodic(int cpu)
{
/*
 * IAMROOT, 2022.12.03:
 * - @cpu가 jiffies을 계산하는 cpu가 맞다면 시간갱신을한다.
 */
	if (tick_do_timer_cpu == cpu) {
		raw_spin_lock(&jiffies_lock);
		write_seqcount_begin(&jiffies_seq);

		/* Keep track of the next tick event */
/*
 * IAMROOT, 2022.12.03:
 * - +1 tick
 */
		tick_next_period = ktime_add_ns(tick_next_period, TICK_NSEC);

		do_timer(1);
		write_seqcount_end(&jiffies_seq);
		raw_spin_unlock(&jiffies_lock);
		update_wall_time();
	}

	update_process_times(user_mode(get_irq_regs()));
	profile_tick(CPU_PROFILING);
}

/*
 * Event handler for periodic ticks
 */
/*
 * IAMROOT, 2022.12.03:
 * - event_handler에 등록되서 사용된다. timer interrupt
 *   hrtimer가 활성화 되기 전에 timer interrupt가 이 함수로 진입한다.
 * - schedule tick의 경우
 *   활성화가 된후에는 hrtimer_interrupt를 통해서 tick_sched_timer가 호출된다.
 */
void tick_handle_periodic(struct clock_event_device *dev)
{
	int cpu = smp_processor_id();
	ktime_t next = dev->next_event;

	tick_periodic(cpu);

#if defined(CONFIG_HIGH_RES_TIMERS) || defined(CONFIG_NO_HZ_COMMON)
	/*
	 * The cpu might have transitioned to HIGHRES or NOHZ mode via
	 * update_process_times() -> run_local_timers() ->
	 * hrtimer_run_queues().
	 */
	if (dev->event_handler != tick_handle_periodic)
		return;
#endif

	if (!clockevent_state_oneshot(dev))
		return;
	for (;;) {
		/*
		 * Setup the next period for devices, which do not have
		 * periodic mode:
		 */
		next = ktime_add_ns(next, TICK_NSEC);

		if (!clockevents_program_event(dev, next, false))
			return;
		/*
		 * Have to be careful here. If we're in oneshot mode,
		 * before we call tick_periodic() in a loop, we need
		 * to be sure we're using a real hardware clocksource.
		 * Otherwise we could get trapped in an infinite
		 * loop, as the tick_periodic() increments jiffies,
		 * which then will increment time, possibly causing
		 * the loop to trigger again and again.
		 */
		if (timekeeping_valid_for_hres())
			tick_periodic(cpu);
	}
}

/*
 * Setup the device for a periodic tick
 */
void tick_setup_periodic(struct clock_event_device *dev, int broadcast)
{
	tick_set_periodic_handler(dev, broadcast);

	/* Broadcast setup ? */
	if (!tick_device_is_functional(dev))
		return;

	if ((dev->features & CLOCK_EVT_FEAT_PERIODIC) &&
	    !tick_broadcast_oneshot_active()) {
		clockevents_switch_state(dev, CLOCK_EVT_STATE_PERIODIC);
	} else {
		unsigned int seq;
		ktime_t next;

		do {
			seq = read_seqcount_begin(&jiffies_seq);
			next = tick_next_period;
		} while (read_seqcount_retry(&jiffies_seq, seq));

		clockevents_switch_state(dev, CLOCK_EVT_STATE_ONESHOT);

		for (;;) {
			if (!clockevents_program_event(dev, next, false))
				return;
			next = ktime_add_ns(next, TICK_NSEC);
		}
	}
}

#ifdef CONFIG_NO_HZ_FULL
static void giveup_do_timer(void *info)
{
	int cpu = *(unsigned int *)info;

	WARN_ON(tick_do_timer_cpu != smp_processor_id());

	tick_do_timer_cpu = cpu;
}

static void tick_take_do_timer_from_boot(void)
{
	int cpu = smp_processor_id();
	int from = tick_do_timer_boot_cpu;

	if (from >= 0 && from != cpu)
		smp_call_function_single(from, giveup_do_timer, &cpu, 1);
}
#endif

/*
 * Setup the tick device
 */
/*
 * IAMROOT, 2022.12.03:
 * - 최초의 진입이라면(tick device setup) @cpu를 tick_do_timer_cpu로 정한다.
 */
static void tick_setup_device(struct tick_device *td,
			      struct clock_event_device *newdev, int cpu,
			      const struct cpumask *cpumask)
{
	void (*handler)(struct clock_event_device *) = NULL;
	ktime_t next_event = 0;

	/*
	 * First device setup ?
	 */
	if (!td->evtdev) {
		/*
		 * If no cpu took the do_timer update, assign it to
		 * this cpu:
		 */
/*
 * IAMROOT, 2022.12.03:
 * - @cpu로 tick_next_period를 정한다.
 */
		if (tick_do_timer_cpu == TICK_DO_TIMER_BOOT) {
			tick_do_timer_cpu = cpu;

			tick_next_period = ktime_get();
#ifdef CONFIG_NO_HZ_FULL
			/*
			 * The boot CPU may be nohz_full, in which case set
			 * tick_do_timer_boot_cpu so the first housekeeping
			 * secondary that comes up will take do_timer from
			 * us.
			 */
			if (tick_nohz_full_cpu(cpu))
				tick_do_timer_boot_cpu = cpu;

		} else if (tick_do_timer_boot_cpu != -1 &&
						!tick_nohz_full_cpu(cpu)) {
			tick_take_do_timer_from_boot();
			tick_do_timer_boot_cpu = -1;
			WARN_ON(tick_do_timer_cpu != cpu);
#endif
		}

		/*
		 * Startup in periodic mode first.
		 */
		td->mode = TICKDEV_MODE_PERIODIC;
	} else {
		handler = td->evtdev->event_handler;
		next_event = td->evtdev->next_event;
		td->evtdev->event_handler = clockevents_handle_noop;
	}

	td->evtdev = newdev;

	/*
	 * When the device is not per cpu, pin the interrupt to the
	 * current cpu:
	 */
	if (!cpumask_equal(newdev->cpumask, cpumask))
		irq_set_affinity(newdev->irq, cpumask);

	/*
	 * When global broadcasting is active, check if the current
	 * device is registered as a placeholder for broadcast mode.
	 * This allows us to handle this x86 misfeature in a generic
	 * way. This function also returns !=0 when we keep the
	 * current active broadcast state for this CPU.
	 */
	if (tick_device_uses_broadcast(newdev, cpu))
		return;

	if (td->mode == TICKDEV_MODE_PERIODIC)
		tick_setup_periodic(newdev, 0);
	else
		tick_setup_oneshot(newdev, handler, next_event);
}

void tick_install_replacement(struct clock_event_device *newdev)
{
	struct tick_device *td = this_cpu_ptr(&tick_cpu_device);
	int cpu = smp_processor_id();

	clockevents_exchange_device(td->evtdev, newdev);
	tick_setup_device(td, newdev, cpu, cpumask_of(cpu));
	if (newdev->features & CLOCK_EVT_FEAT_ONESHOT)
		tick_oneshot_notify();
}

static bool tick_check_percpu(struct clock_event_device *curdev,
			      struct clock_event_device *newdev, int cpu)
{
	if (!cpumask_test_cpu(cpu, newdev->cpumask))
		return false;
	if (cpumask_equal(newdev->cpumask, cpumask_of(cpu)))
		return true;
	/* Check if irq affinity can be set */
	if (newdev->irq >= 0 && !irq_can_set_affinity(newdev->irq))
		return false;
	/* Prefer an existing cpu local device */
	if (curdev && cpumask_equal(curdev->cpumask, cpumask_of(cpu)))
		return false;
	return true;
}

static bool tick_check_preferred(struct clock_event_device *curdev,
				 struct clock_event_device *newdev)
{
	/* Prefer oneshot capable device */
	if (!(newdev->features & CLOCK_EVT_FEAT_ONESHOT)) {
		if (curdev && (curdev->features & CLOCK_EVT_FEAT_ONESHOT))
			return false;
		if (tick_oneshot_mode_active())
			return false;
	}

	/*
	 * Use the higher rated one, but prefer a CPU local device with a lower
	 * rating than a non-CPU local device
	 */
	return !curdev ||
		newdev->rating > curdev->rating ||
	       !cpumask_equal(curdev->cpumask, newdev->cpumask);
}

/*
 * Check whether the new device is a better fit than curdev. curdev
 * can be NULL !
 */
bool tick_check_replacement(struct clock_event_device *curdev,
			    struct clock_event_device *newdev)
{
	if (!tick_check_percpu(curdev, newdev, smp_processor_id()))
		return false;

	return tick_check_preferred(curdev, newdev);
}

/*
 * Check, if the new registered device should be used. Called with
 * clockevents_lock held and interrupts disabled.
 */
/*
 * IAMROOT, 2022.08.27:
 * - @newdev를 검사하여 tick_do_timer_cpu등을 결정한다.
 */
void tick_check_new_device(struct clock_event_device *newdev)
{
	struct clock_event_device *curdev;
	struct tick_device *td;
	int cpu;

	cpu = smp_processor_id();
	td = &per_cpu(tick_cpu_device, cpu);
	curdev = td->evtdev;

	if (!tick_check_replacement(curdev, newdev))
		goto out_bc;

	if (!try_module_get(newdev->owner))
		return;

	/*
	 * Replace the eventually existing device by the new
	 * device. If the current device is the broadcast device, do
	 * not give it back to the clockevents layer !
	 */
	if (tick_is_broadcast_device(curdev)) {
		clockevents_shutdown(curdev);
		curdev = NULL;
	}
	clockevents_exchange_device(curdev, newdev);
	tick_setup_device(td, newdev, cpu, cpumask_of(cpu));
	if (newdev->features & CLOCK_EVT_FEAT_ONESHOT)
		tick_oneshot_notify();
	return;

out_bc:
	/*
	 * Can the new device be used as a broadcast device ?
	 */
	tick_install_broadcast_device(newdev, cpu);
}

/**
 * tick_broadcast_oneshot_control - Enter/exit broadcast oneshot mode
 * @state:	The target state (enter/exit)
 *
 * The system enters/leaves a state, where affected devices might stop
 * Returns 0 on success, -EBUSY if the cpu is used to broadcast wakeups.
 *
 * Called with interrupts disabled, so clockevents_lock is not
 * required here because the local clock event device cannot go away
 * under us.
 */
int tick_broadcast_oneshot_control(enum tick_broadcast_state state)
{
	struct tick_device *td = this_cpu_ptr(&tick_cpu_device);

	if (!(td->evtdev->features & CLOCK_EVT_FEAT_C3STOP))
		return 0;

	return __tick_broadcast_oneshot_control(state);
}
EXPORT_SYMBOL_GPL(tick_broadcast_oneshot_control);

#ifdef CONFIG_HOTPLUG_CPU
/*
 * Transfer the do_timer job away from a dying cpu.
 *
 * Called with interrupts disabled. No locking required. If
 * tick_do_timer_cpu is owned by this cpu, nothing can change it.
 */
void tick_handover_do_timer(void)
{
	if (tick_do_timer_cpu == smp_processor_id())
		tick_do_timer_cpu = cpumask_first(cpu_online_mask);
}

/*
 * Shutdown an event device on a given cpu:
 *
 * This is called on a life CPU, when a CPU is dead. So we cannot
 * access the hardware device itself.
 * We just set the mode and remove it from the lists.
 */
void tick_shutdown(unsigned int cpu)
{
	struct tick_device *td = &per_cpu(tick_cpu_device, cpu);
	struct clock_event_device *dev = td->evtdev;

	td->mode = TICKDEV_MODE_PERIODIC;
	if (dev) {
		/*
		 * Prevent that the clock events layer tries to call
		 * the set mode function!
		 */
		clockevent_set_state(dev, CLOCK_EVT_STATE_DETACHED);
		clockevents_exchange_device(dev, NULL);
		dev->event_handler = clockevents_handle_noop;
		td->evtdev = NULL;
	}
}
#endif

/**
 * tick_suspend_local - Suspend the local tick device
 *
 * Called from the local cpu for freeze with interrupts disabled.
 *
 * No locks required. Nothing can change the per cpu device.
 */
void tick_suspend_local(void)
{
	struct tick_device *td = this_cpu_ptr(&tick_cpu_device);

	clockevents_shutdown(td->evtdev);
}

/**
 * tick_resume_local - Resume the local tick device
 *
 * Called from the local CPU for unfreeze or XEN resume magic.
 *
 * No locks required. Nothing can change the per cpu device.
 */
void tick_resume_local(void)
{
	struct tick_device *td = this_cpu_ptr(&tick_cpu_device);
	bool broadcast = tick_resume_check_broadcast();

	clockevents_tick_resume(td->evtdev);
	if (!broadcast) {
		if (td->mode == TICKDEV_MODE_PERIODIC)
			tick_setup_periodic(td->evtdev, 0);
		else
			tick_resume_oneshot();
	}

	/*
	 * Ensure that hrtimers are up to date and the clockevents device
	 * is reprogrammed correctly when high resolution timers are
	 * enabled.
	 */
	hrtimers_resume_local();
}

/**
 * tick_suspend - Suspend the tick and the broadcast device
 *
 * Called from syscore_suspend() via timekeeping_suspend with only one
 * CPU online and interrupts disabled or from tick_unfreeze() under
 * tick_freeze_lock.
 *
 * No locks required. Nothing can change the per cpu device.
 */
void tick_suspend(void)
{
	tick_suspend_local();
	tick_suspend_broadcast();
}

/**
 * tick_resume - Resume the tick and the broadcast device
 *
 * Called from syscore_resume() via timekeeping_resume with only one
 * CPU online and interrupts disabled.
 *
 * No locks required. Nothing can change the per cpu device.
 */
void tick_resume(void)
{
	tick_resume_broadcast();
	tick_resume_local();
}

#ifdef CONFIG_SUSPEND
static DEFINE_RAW_SPINLOCK(tick_freeze_lock);
static unsigned int tick_freeze_depth;

/**
 * tick_freeze - Suspend the local tick and (possibly) timekeeping.
 *
 * Check if this is the last online CPU executing the function and if so,
 * suspend timekeeping.  Otherwise suspend the local tick.
 *
 * Call with interrupts disabled.  Must be balanced with %tick_unfreeze().
 * Interrupts must not be enabled before the subsequent %tick_unfreeze().
 */
void tick_freeze(void)
{
	raw_spin_lock(&tick_freeze_lock);

	tick_freeze_depth++;
	if (tick_freeze_depth == num_online_cpus()) {
		trace_suspend_resume(TPS("timekeeping_freeze"),
				     smp_processor_id(), true);
		system_state = SYSTEM_SUSPEND;
		sched_clock_suspend();
		timekeeping_suspend();
	} else {
		tick_suspend_local();
	}

	raw_spin_unlock(&tick_freeze_lock);
}

/**
 * tick_unfreeze - Resume the local tick and (possibly) timekeeping.
 *
 * Check if this is the first CPU executing the function and if so, resume
 * timekeeping.  Otherwise resume the local tick.
 *
 * Call with interrupts disabled.  Must be balanced with %tick_freeze().
 * Interrupts must not be enabled after the preceding %tick_freeze().
 */
void tick_unfreeze(void)
{
	raw_spin_lock(&tick_freeze_lock);

	if (tick_freeze_depth == num_online_cpus()) {
		timekeeping_resume();
		sched_clock_resume();
		system_state = SYSTEM_RUNNING;
		trace_suspend_resume(TPS("timekeeping_freeze"),
				     smp_processor_id(), false);
	} else {
		touch_softlockup_watchdog();
		tick_resume_local();
	}

	tick_freeze_depth--;

	raw_spin_unlock(&tick_freeze_lock);
}
#endif /* CONFIG_SUSPEND */

/**
 * tick_init - initialize the tick control
 */
void __init tick_init(void)
{
	tick_broadcast_init();
	tick_nohz_init();
}
