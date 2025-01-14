// SPDX-License-Identifier: GPL-2.0
/*
 * This file contains functions which manage clock event devices.
 *
 * Copyright(C) 2005-2006, Thomas Gleixner <tglx@linutronix.de>
 * Copyright(C) 2005-2007, Red Hat, Inc., Ingo Molnar
 * Copyright(C) 2006-2007, Timesys Corp., Thomas Gleixner
 */

#include <linux/clockchips.h>
#include <linux/hrtimer.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/smp.h>
#include <linux/device.h>

#include "tick-internal.h"

/* The registered clock event devices */
static LIST_HEAD(clockevent_devices);
static LIST_HEAD(clockevents_released);
/* Protection for the above */
static DEFINE_RAW_SPINLOCK(clockevents_lock);
/* Protection for unbind operations */
static DEFINE_MUTEX(clockevents_mutex);

struct ce_unbind {
	struct clock_event_device *ce;
	int res;
};

/*
 * IAMROOT, 2022.08.27:
 * @return 최솟값이 1000ns이상인 latch -> ns 변환값.
 * - @evt의 mult, shift값을 사용해 latch의 ns를 구해온다.
 * - ex) mult = 0xdd2_f1aa, shift = 32
 *   1. latch = 0xf = > 숫자가 작기때문에 1000
 *   2. latch = 0x7fffffff
 *	clc = 0x7fffffff = 0x7fffffff_0000_0000
 *	rnd = mult - 1 = 0xdd2_f1a9
 *	clc += rnd ==> clc = 0x7fffffff_0dd2_f1a9
 *	clc = clc / 0xdd2_f1a9
 *	    = 39768215854 (ns)
 *	    = 39 (sec)
 */
static u64 cev_delta2ns(unsigned long latch, struct clock_event_device *evt,
			bool ismax)
{
/*
 * IAMROOT, 2022.08.27:
 * - clc = (정확도가 고려된 cycle)
 *   mult = (정확도가 고려된 1 ns당 cycle 개수)
 */
	u64 clc = (u64) latch << evt->shift;
	u64 rnd;

	if (WARN_ON(!evt->mult))
		evt->mult = 1;
/*
 * IAMROOT, 2022.08.27:
 * - 반올 손상 보정.
 */
	rnd = (u64) evt->mult - 1;

	/*
	 * Upper bound sanity check. If the backwards conversion is
	 * not equal latch, we know that the above shift overflowed.
	 */
/*
 * IAMROOT, 2022.08.27:
 * - overflow check. 
 */
	if ((clc >> evt->shift) != (u64)latch)
		clc = ~0ULL;

	/*
	 * Scaled math oddities:
	 *
	 * For mult <= (1 << shift) we can safely add mult - 1 to
	 * prevent integer rounding loss. So the backwards conversion
	 * from nsec to device ticks will be correct.
	 *
	 * For mult > (1 << shift), i.e. device frequency is > 1GHz we
	 * need to be careful. Adding mult - 1 will result in a value
	 * which when converted back to device ticks can be larger
	 * than latch by up to (mult - 1) >> shift. For the min_delta
	 * calculation we still want to apply this in order to stay
	 * above the minimum device ticks limit. For the upper limit
	 * we would end up with a latch value larger than the upper
	 * limit of the device, so we omit the add to stay below the
	 * device upper boundary.
	 *
	 * Also omit the add if it would overflow the u64 boundary.
	 */
/*
 * IAMROOT, 2022.08.27:
 * - papago
 *   스케일링된 수학 기이함:
 *   mult <= (1 << shift)의 경우 정수 반올림 손실을 방지하기 위해 mult - 1을
 *   안전하게 추가할 수 있습니다. 따라서 nsec에서 장치 틱으로의 역변환은
 *   정확할 것입니다.
 *
 *   mult > (1 << shift), 즉 장치 주파수가 > 1GHz인 경우 주의해야 합니다.
 *   mult - 1을 추가하면 장치 틱으로 다시 변환될 때 최대
 *   (mult - 1) >> shift만큼 래치보다 클 수 있는 값이 생성됩니다. min_delta
 *   계산의 경우 최소 장치 틱 제한 이상을 유지하기 위해 여전히 이를 적용하려고
 *   합니다. 상한선의 경우 장치의 상한선보다 큰 래치 값으로 끝나므로 장치
 *   상한선 아래에 머물기 위해 추가를 생략합니다.
 *
 *   또한 u64 경계를 넘을 경우 추가를 생략합니다.
 *
 * - 반올림을 계산한다. 이 계산으로 인한 상한값 overflow가 발생안하게 하기위한
 *   예외처리.
 */
	if ((~0ULL - clc > rnd) &&
	    (!ismax || evt->mult <= (1ULL << evt->shift)))
		clc += rnd;

	do_div(clc, evt->mult);

	/* Deltas less than 1usec are pointless noise */
	return clc > 1000 ? clc : 1000;
}

/**
 * clockevents_delta2ns - Convert a latch value (device ticks) to nanoseconds
 * @latch:	value to convert
 * @evt:	pointer to clock event device descriptor
 *
 * Math helper, returns latch value converted to nanoseconds (bound checked)
 */
u64 clockevent_delta2ns(unsigned long latch, struct clock_event_device *evt)
{
	return cev_delta2ns(latch, evt, false);
}
EXPORT_SYMBOL_GPL(clockevent_delta2ns);

/*
 * IAMROOT, 2022.12.03:
 * - @state에따라 필요한 callback 함수를 실행한다.
 */
static int __clockevents_switch_state(struct clock_event_device *dev,
				      enum clock_event_state state)
{
	if (dev->features & CLOCK_EVT_FEAT_DUMMY)
		return 0;

	/* Transition with new state-specific callbacks */
	switch (state) {
	case CLOCK_EVT_STATE_DETACHED:
		/* The clockevent device is getting replaced. Shut it down. */

	case CLOCK_EVT_STATE_SHUTDOWN:
		if (dev->set_state_shutdown)
			return dev->set_state_shutdown(dev);
		return 0;

	case CLOCK_EVT_STATE_PERIODIC:
		/* Core internal bug */
		if (!(dev->features & CLOCK_EVT_FEAT_PERIODIC))
			return -ENOSYS;
		if (dev->set_state_periodic)
			return dev->set_state_periodic(dev);
		return 0;

	case CLOCK_EVT_STATE_ONESHOT:
		/* Core internal bug */
		if (!(dev->features & CLOCK_EVT_FEAT_ONESHOT))
			return -ENOSYS;
		if (dev->set_state_oneshot)
			return dev->set_state_oneshot(dev);
		return 0;

	case CLOCK_EVT_STATE_ONESHOT_STOPPED:
		/* Core internal bug */
		if (WARN_ONCE(!clockevent_state_oneshot(dev),
			      "Current state: %d\n",
			      clockevent_get_state(dev)))
			return -EINVAL;

		if (dev->set_state_oneshot_stopped)
			return dev->set_state_oneshot_stopped(dev);
		else
			return -ENOSYS;

	default:
		return -ENOSYS;
	}
}

/**
 * clockevents_switch_state - set the operating state of a clock event device
 * @dev:	device to modify
 * @state:	new state
 *
 * Must be called with interrupts disabled !
 */
/*
 * IAMROOT, 2022.12.03:
 * - @state에 따른 callback을 수행하고 @dev의 state을 @state로 변경한다.
 */
void clockevents_switch_state(struct clock_event_device *dev,
			      enum clock_event_state state)
{
	if (clockevent_get_state(dev) != state) {
		if (__clockevents_switch_state(dev, state))
			return;

		clockevent_set_state(dev, state);

		/*
		 * A nsec2cyc multiplicator of 0 is invalid and we'd crash
		 * on it, so fix it up and emit a warning:
		 */
		if (clockevent_state_oneshot(dev)) {
			if (WARN_ON(!dev->mult))
				dev->mult = 1;
		}
	}
}

/**
 * clockevents_shutdown - shutdown the device and clear next_event
 * @dev:	device to shutdown
 */
void clockevents_shutdown(struct clock_event_device *dev)
{
	clockevents_switch_state(dev, CLOCK_EVT_STATE_SHUTDOWN);
	dev->next_event = KTIME_MAX;
}

/**
 * clockevents_tick_resume -	Resume the tick device before using it again
 * @dev:			device to resume
 */
int clockevents_tick_resume(struct clock_event_device *dev)
{
	int ret = 0;

	if (dev->tick_resume)
		ret = dev->tick_resume(dev);

	return ret;
}

#ifdef CONFIG_GENERIC_CLOCKEVENTS_MIN_ADJUST

/* Limit min_delta to a jiffie */
#define MIN_DELTA_LIMIT		(NSEC_PER_SEC / HZ)

/**
 * clockevents_increase_min_delta - raise minimum delta of a clock event device
 * @dev:       device to increase the minimum delta
 *
 * Returns 0 on success, -ETIME when the minimum delta reached the limit.
 */
static int clockevents_increase_min_delta(struct clock_event_device *dev)
{
	/* Nothing to do if we already reached the limit */
	if (dev->min_delta_ns >= MIN_DELTA_LIMIT) {
		printk_deferred(KERN_WARNING
				"CE: Reprogramming failure. Giving up\n");
		dev->next_event = KTIME_MAX;
		return -ETIME;
	}

	if (dev->min_delta_ns < 5000)
		dev->min_delta_ns = 5000;
	else
		dev->min_delta_ns += dev->min_delta_ns >> 1;

	if (dev->min_delta_ns > MIN_DELTA_LIMIT)
		dev->min_delta_ns = MIN_DELTA_LIMIT;

	printk_deferred(KERN_WARNING
			"CE: %s increased min_delta_ns to %llu nsec\n",
			dev->name ? dev->name : "?",
			(unsigned long long) dev->min_delta_ns);
	return 0;
}

/**
 * clockevents_program_min_delta - Set clock event device to the minimum delay.
 * @dev:	device to program
 *
 * Returns 0 on success, -ETIME when the retry loop failed.
 */
static int clockevents_program_min_delta(struct clock_event_device *dev)
{
	unsigned long long clc;
	int64_t delta;
	int i;

	for (i = 0;;) {
		delta = dev->min_delta_ns;
		dev->next_event = ktime_add_ns(ktime_get(), delta);

		if (clockevent_state_shutdown(dev))
			return 0;

		dev->retries++;
		clc = ((unsigned long long) delta * dev->mult) >> dev->shift;
		if (dev->set_next_event((unsigned long) clc, dev) == 0)
			return 0;

		if (++i > 2) {
			/*
			 * We tried 3 times to program the device with the
			 * given min_delta_ns. Try to increase the minimum
			 * delta, if that fails as well get out of here.
			 */
			if (clockevents_increase_min_delta(dev))
				return -ETIME;
			i = 0;
		}
	}
}

#else  /* CONFIG_GENERIC_CLOCKEVENTS_MIN_ADJUST */

/**
 * clockevents_program_min_delta - Set clock event device to the minimum delay.
 * @dev:	device to program
 *
 * Returns 0 on success, -ETIME when the retry loop failed.
 */
/*
 * IAMROOT, 2022.12.03:
 * - @set_next_event callback을 실행한다.
 */
static int clockevents_program_min_delta(struct clock_event_device *dev)
{
	unsigned long long clc;
	int64_t delta = 0;
	int i;

/*
 * IAMROOT, 2022.12.03:
 * - retry 10.
 *   programming이 불가능할 정도로 term이 짧은경우가 있을수있기 때문에 조금씩 늘려서
 *   재시도를 한다. 즉 최대 min_delta_ns * 10이후에 실행될수도 있게 된다.
 */
	for (i = 0; i < 10; i++) {
		delta += dev->min_delta_ns;
		dev->next_event = ktime_add_ns(ktime_get(), delta);

		if (clockevent_state_shutdown(dev))
			return 0;

		dev->retries++;
/*
 * IAMROOT, 2022.12.03:
 * - mult shift로 ns -> cycle화 한다.
 */
		clc = ((unsigned long long) delta * dev->mult) >> dev->shift;
		if (dev->set_next_event((unsigned long) clc, dev) == 0)
			return 0;
	}
	return -ETIME;
}

#endif /* CONFIG_GENERIC_CLOCKEVENTS_MIN_ADJUST */

/**
 * clockevents_program_event - Reprogram the clock event device.
 * @dev:	device to program
 * @expires:	absolute expiry time (monotonic clock)
 * @force:	program minimum delay if expires can not be set
 *
 * Returns 0 on success, -ETIME when the event is in the past.
 */
/*
 * IAMROOT, 2022.12.03:
 * - exipires시간으로 program한다.
 */
int clockevents_program_event(struct clock_event_device *dev, ktime_t expires,
			      bool force)
{
	unsigned long long clc;
	int64_t delta;
	int rc;

	if (WARN_ON_ONCE(expires < 0))
		return -ETIME;

	dev->next_event = expires;

	if (clockevent_state_shutdown(dev))
		return 0;

	/* We must be in ONESHOT state here */
	WARN_ONCE(!clockevent_state_oneshot(dev), "Current state: %d\n",
		  clockevent_get_state(dev));

	/* Shortcut for clockevent devices that can deal with ktime. */
/*
 * IAMROOT, 2022.12.03:
 * - ktime으로 timer로 설정이가능한경우.
 */
	if (dev->features & CLOCK_EVT_FEAT_KTIME)
		return dev->set_next_ktime(expires, dev);

	delta = ktime_to_ns(ktime_sub(expires, ktime_get()));
/*
 * IAMROOT, 2022.12.03:
 * - 이전시간이면 force가 있는경우에 한해서만 시도한다.
 */
	if (delta <= 0)
		return force ? clockevents_program_min_delta(dev) : -ETIME;

/*
 * IAMROOT, 2022.12.03:
 * - 요청한 expires값을 clamp하여 program을 시도한다.
 */
	delta = min(delta, (int64_t) dev->max_delta_ns);
	delta = max(delta, (int64_t) dev->min_delta_ns);

	clc = ((unsigned long long) delta * dev->mult) >> dev->shift;

/*
 * IAMROOT, 2022.12.03:
 * - prgram 시도.
 */
	rc = dev->set_next_event((unsigned long) clc, dev);

/*
 * IAMROOT, 2022.12.03:
 * - rc != 0 실패.
 *   force가 있다면 가능한한 시도해야되므로 clockevents_program_min_delta()를 수행한다.
 *   그게 아니거나(rc == 0) 성공했으면 return rc.
 */
	return (rc && force) ? clockevents_program_min_delta(dev) : rc;
}

/*
 * Called after a notify add to make devices available which were
 * released from the notifier call.
 */
/*
 * IAMROOT, 2022.08.27:
 * - TODO
 */
static void clockevents_notify_released(void)
{
	struct clock_event_device *dev;

	while (!list_empty(&clockevents_released)) {
		dev = list_entry(clockevents_released.next,
				 struct clock_event_device, list);
		list_move(&dev->list, &clockevent_devices);
		tick_check_new_device(dev);
	}
}

/*
 * Try to install a replacement clock event device
 */
static int clockevents_replace(struct clock_event_device *ced)
{
	struct clock_event_device *dev, *newdev = NULL;

	list_for_each_entry(dev, &clockevent_devices, list) {
		if (dev == ced || !clockevent_state_detached(dev))
			continue;

		if (!tick_check_replacement(newdev, dev))
			continue;

		if (!try_module_get(dev->owner))
			continue;

		if (newdev)
			module_put(newdev->owner);
		newdev = dev;
	}
	if (newdev) {
		tick_install_replacement(newdev);
		list_del_init(&ced->list);
	}
	return newdev ? 0 : -EBUSY;
}

/*
 * Called with clockevents_mutex and clockevents_lock held
 */
static int __clockevents_try_unbind(struct clock_event_device *ced, int cpu)
{
	/* Fast track. Device is unused */
	if (clockevent_state_detached(ced)) {
		list_del_init(&ced->list);
		return 0;
	}

	return ced == per_cpu(tick_cpu_device, cpu).evtdev ? -EAGAIN : -EBUSY;
}

/*
 * SMP function call to unbind a device
 */
static void __clockevents_unbind(void *arg)
{
	struct ce_unbind *cu = arg;
	int res;

	raw_spin_lock(&clockevents_lock);
	res = __clockevents_try_unbind(cu->ce, smp_processor_id());
	if (res == -EAGAIN)
		res = clockevents_replace(cu->ce);
	cu->res = res;
	raw_spin_unlock(&clockevents_lock);
}

/*
 * Issues smp function call to unbind a per cpu device. Called with
 * clockevents_mutex held.
 */
static int clockevents_unbind(struct clock_event_device *ced, int cpu)
{
	struct ce_unbind cu = { .ce = ced, .res = -ENODEV };

	smp_call_function_single(cpu, __clockevents_unbind, &cu, 1);
	return cu.res;
}

/*
 * Unbind a clockevents device.
 */
int clockevents_unbind_device(struct clock_event_device *ced, int cpu)
{
	int ret;

	mutex_lock(&clockevents_mutex);
	ret = clockevents_unbind(ced, cpu);
	mutex_unlock(&clockevents_mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(clockevents_unbind_device);

/**
 * clockevents_register_device - register a clock event device
 * @dev:	device to register
 */
/*
 * IAMROOT, 2022.08.27:
 * - cpumask 검사, state 변경.
 *   clock_event_device에 @dev 등록.
 *   pcp tick_cpu_device에 clock event 등록
 * - periodic timer로 등록될경우 tick_handle_periodic이 event handler로 등록되고
 *   동작을 시작한다.
 */
void clockevents_register_device(struct clock_event_device *dev)
{
	unsigned long flags;

	/* Initialize state to DETACHED */
	clockevent_set_state(dev, CLOCK_EVT_STATE_DETACHED);

/*
 * IAMROOT, 2022.08.27:
 * - cpumask가 지정이 안되있으면 현재 cpu만. all cpu면 possible로 지정한다.
 */
	if (!dev->cpumask) {
		WARN_ON(num_possible_cpus() > 1);
		dev->cpumask = cpumask_of(smp_processor_id());
	}

	if (dev->cpumask == cpu_all_mask) {
		WARN(1, "%s cpumask == cpu_all_mask, using cpu_possible_mask instead\n",
		     dev->name);
		dev->cpumask = cpu_possible_mask;
	}

	raw_spin_lock_irqsave(&clockevents_lock, flags);

	list_add(&dev->list, &clockevent_devices);
	tick_check_new_device(dev);
	clockevents_notify_released();

	raw_spin_unlock_irqrestore(&clockevents_lock, flags);
}
EXPORT_SYMBOL_GPL(clockevents_register_device);

/*
 * IAMROOT, 2022.08.27:
 * - @dev가 @freq로 지원하는 min / max nsec를 구한다.
 * - @freq, max_delta_ticks을 기준으로 mult, shift를 구한다.
 * - 구한 mult shift를 가지고 nsec기준의 min, max delta를 구한다.
 */
static void clockevents_config(struct clock_event_device *dev, u32 freq)
{
	u64 sec;

	if (!(dev->features & CLOCK_EVT_FEAT_ONESHOT))
		return;

	/*
	 * Calculate the maximum number of seconds we can sleep. Limit
	 * to 10 minutes for hardware which can program more than
	 * 32bit ticks so we still get reasonable conversion values.
	 */
	sec = dev->max_delta_ticks;
	do_div(sec, freq);
	if (!sec)
		sec = 1;
	else if (sec > 600 && dev->max_delta_ticks > UINT_MAX)
		sec = 600;

/*
 * IAMROOT, 2022.08.27:
 * - @dev의 mult, shift값을 계산해온다.
 */
	clockevents_calc_mult_shift(dev, freq, sec);

/*
 * IAMROOT, 2022.08.27:
 * - min, max tick을 가지고 nsec를 구한다.
 */
	dev->min_delta_ns = cev_delta2ns(dev->min_delta_ticks, dev, false);
	dev->max_delta_ns = cev_delta2ns(dev->max_delta_ticks, dev, true);
}

/**
 * clockevents_config_and_register - Configure and register a clock event device
 * @dev:	device to register
 * @freq:	The clock frequency
 * @min_delta:	The minimum clock ticks to program in oneshot mode
 * @max_delta:	The maximum clock ticks to program in oneshot mode
 *
 * min/max_delta can be 0 for devices which do not support oneshot mode.
 */
/*
 * IAMROOT, 2022.08.27:
 * - @freq을 기준으로 min_delta ~ max_delta사이에 동작하는 @dev를
 *   1. clockevent_devices에 등록
 *   2. pcp tick_cpu_device에 등록한다.
 * - clockevents의 @freq, 최대최소 nsec를 설정하고 시스템에 clock event를
 *   등록한다.
 */
void clockevents_config_and_register(struct clock_event_device *dev,
				     u32 freq, unsigned long min_delta,
				     unsigned long max_delta)
{
	dev->min_delta_ticks = min_delta;
	dev->max_delta_ticks = max_delta;
	clockevents_config(dev, freq);
	clockevents_register_device(dev);
}
EXPORT_SYMBOL_GPL(clockevents_config_and_register);

int __clockevents_update_freq(struct clock_event_device *dev, u32 freq)
{
	clockevents_config(dev, freq);

	if (clockevent_state_oneshot(dev))
		return clockevents_program_event(dev, dev->next_event, false);

	if (clockevent_state_periodic(dev))
		return __clockevents_switch_state(dev, CLOCK_EVT_STATE_PERIODIC);

	return 0;
}

/**
 * clockevents_update_freq - Update frequency and reprogram a clock event device.
 * @dev:	device to modify
 * @freq:	new device frequency
 *
 * Reconfigure and reprogram a clock event device in oneshot
 * mode. Must be called on the cpu for which the device delivers per
 * cpu timer events. If called for the broadcast device the core takes
 * care of serialization.
 *
 * Returns 0 on success, -ETIME when the event is in the past.
 */
int clockevents_update_freq(struct clock_event_device *dev, u32 freq)
{
	unsigned long flags;
	int ret;

	local_irq_save(flags);
	ret = tick_broadcast_update_freq(dev, freq);
	if (ret == -ENODEV)
		ret = __clockevents_update_freq(dev, freq);
	local_irq_restore(flags);
	return ret;
}

/*
 * Noop handler when we shut down an event device
 */
void clockevents_handle_noop(struct clock_event_device *dev)
{
}

/**
 * clockevents_exchange_device - release and request clock devices
 * @old:	device to release (can be NULL)
 * @new:	device to request (can be NULL)
 *
 * Called from various tick functions with clockevents_lock held and
 * interrupts disabled.
 */
/*
 * IAMROOT, 2023.01.03:
 * - @old는 ref down, detached, released로 이동 시킨다.
 *   @new는 일단 shutdonw 시키는걸로 준비를 하는걸로 간주한다.
 */
void clockevents_exchange_device(struct clock_event_device *old,
				 struct clock_event_device *new)
{
	/*
	 * Caller releases a clock event device. We queue it into the
	 * released list and do a notify add later.
	 */
	if (old) {
		module_put(old->owner);
		clockevents_switch_state(old, CLOCK_EVT_STATE_DETACHED);
		list_move(&old->list, &clockevents_released);
	}

	if (new) {
		BUG_ON(!clockevent_state_detached(new));
		clockevents_shutdown(new);
	}
}

/**
 * clockevents_suspend - suspend clock devices
 */
void clockevents_suspend(void)
{
	struct clock_event_device *dev;

	list_for_each_entry_reverse(dev, &clockevent_devices, list)
		if (dev->suspend && !clockevent_state_detached(dev))
			dev->suspend(dev);
}

/**
 * clockevents_resume - resume clock devices
 */
void clockevents_resume(void)
{
	struct clock_event_device *dev;

	list_for_each_entry(dev, &clockevent_devices, list)
		if (dev->resume && !clockevent_state_detached(dev))
			dev->resume(dev);
}

#ifdef CONFIG_HOTPLUG_CPU

# ifdef CONFIG_GENERIC_CLOCKEVENTS_BROADCAST
/**
 * tick_offline_cpu - Take CPU out of the broadcast mechanism
 * @cpu:	The outgoing CPU
 *
 * Called on the outgoing CPU after it took itself offline.
 */
void tick_offline_cpu(unsigned int cpu)
{
	raw_spin_lock(&clockevents_lock);
	tick_broadcast_offline(cpu);
	raw_spin_unlock(&clockevents_lock);
}
# endif

/**
 * tick_cleanup_dead_cpu - Cleanup the tick and clockevents of a dead cpu
 * @cpu:	The dead CPU
 */
void tick_cleanup_dead_cpu(int cpu)
{
	struct clock_event_device *dev, *tmp;
	unsigned long flags;

	raw_spin_lock_irqsave(&clockevents_lock, flags);

	tick_shutdown(cpu);
	/*
	 * Unregister the clock event devices which were
	 * released from the users in the notify chain.
	 */
	list_for_each_entry_safe(dev, tmp, &clockevents_released, list)
		list_del(&dev->list);
	/*
	 * Now check whether the CPU has left unused per cpu devices
	 */
	list_for_each_entry_safe(dev, tmp, &clockevent_devices, list) {
		if (cpumask_test_cpu(cpu, dev->cpumask) &&
		    cpumask_weight(dev->cpumask) == 1 &&
		    !tick_is_broadcast_device(dev)) {
			BUG_ON(!clockevent_state_detached(dev));
			list_del(&dev->list);
		}
	}
	raw_spin_unlock_irqrestore(&clockevents_lock, flags);
}
#endif

#ifdef CONFIG_SYSFS
static struct bus_type clockevents_subsys = {
	.name		= "clockevents",
	.dev_name       = "clockevent",
};

static DEFINE_PER_CPU(struct device, tick_percpu_dev);
static struct tick_device *tick_get_tick_dev(struct device *dev);

static ssize_t current_device_show(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	struct tick_device *td;
	ssize_t count = 0;

	raw_spin_lock_irq(&clockevents_lock);
	td = tick_get_tick_dev(dev);
	if (td && td->evtdev)
		count = snprintf(buf, PAGE_SIZE, "%s\n", td->evtdev->name);
	raw_spin_unlock_irq(&clockevents_lock);
	return count;
}
static DEVICE_ATTR_RO(current_device);

/* We don't support the abomination of removable broadcast devices */
static ssize_t unbind_device_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	char name[CS_NAME_LEN];
	ssize_t ret = sysfs_get_uname(buf, name, count);
	struct clock_event_device *ce;

	if (ret < 0)
		return ret;

	ret = -ENODEV;
	mutex_lock(&clockevents_mutex);
	raw_spin_lock_irq(&clockevents_lock);
	list_for_each_entry(ce, &clockevent_devices, list) {
		if (!strcmp(ce->name, name)) {
			ret = __clockevents_try_unbind(ce, dev->id);
			break;
		}
	}
	raw_spin_unlock_irq(&clockevents_lock);
	/*
	 * We hold clockevents_mutex, so ce can't go away
	 */
	if (ret == -EAGAIN)
		ret = clockevents_unbind(ce, dev->id);
	mutex_unlock(&clockevents_mutex);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(unbind_device);

#ifdef CONFIG_GENERIC_CLOCKEVENTS_BROADCAST
static struct device tick_bc_dev = {
	.init_name	= "broadcast",
	.id		= 0,
	.bus		= &clockevents_subsys,
};

static struct tick_device *tick_get_tick_dev(struct device *dev)
{
	return dev == &tick_bc_dev ? tick_get_broadcast_device() :
		&per_cpu(tick_cpu_device, dev->id);
}

static __init int tick_broadcast_init_sysfs(void)
{
	int err = device_register(&tick_bc_dev);

	if (!err)
		err = device_create_file(&tick_bc_dev, &dev_attr_current_device);
	return err;
}
#else
static struct tick_device *tick_get_tick_dev(struct device *dev)
{
	return &per_cpu(tick_cpu_device, dev->id);
}
static inline int tick_broadcast_init_sysfs(void) { return 0; }
#endif

static int __init tick_init_sysfs(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		struct device *dev = &per_cpu(tick_percpu_dev, cpu);
		int err;

		dev->id = cpu;
		dev->bus = &clockevents_subsys;
		err = device_register(dev);
		if (!err)
			err = device_create_file(dev, &dev_attr_current_device);
		if (!err)
			err = device_create_file(dev, &dev_attr_unbind_device);
		if (err)
			return err;
	}
	return tick_broadcast_init_sysfs();
}

static int __init clockevents_init_sysfs(void)
{
	int err = subsys_system_register(&clockevents_subsys, NULL);

	if (!err)
		err = tick_init_sysfs();
	return err;
}
device_initcall(clockevents_init_sysfs);
#endif /* SYSFS */
