// SPDX-License-Identifier: GPL-2.0-only
/*
 * kernel/power/suspend.c - Suspend to RAM and standby functionality.
 *
 * Copyright (c) 2003 Patrick Mochel
 * Copyright (c) 2003 Open Source Development Lab
 * Copyright (c) 2009 Rafael J. Wysocki <rjw@sisk.pl>, Novell Inc.
 */

#define pr_fmt(fmt) "PM: " fmt

#include <linux/string.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/console.h>
#include <linux/cpu.h>
#include <linux/cpuidle.h>
#include <linux/gfp.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/export.h>
#include <linux/suspend.h>
#include <linux/syscore_ops.h>
#include <linux/swait.h>
#include <linux/ftrace.h>
#include <trace/events/power.h>
#include <linux/compiler.h>
#include <linux/moduleparam.h>

#include "power.h"

const char * const pm_labels[] = {
	[PM_SUSPEND_TO_IDLE] = "freeze",
	[PM_SUSPEND_STANDBY] = "standby",
	[PM_SUSPEND_MEM] = "mem",
};
/*
 * IAMROOT, 2021.12.18:
 * - 해당 PM_SUSPEND_XXX가 지원한다면 labels에서 string pointer를 가져와 set한다.
 * */
const char *pm_states[PM_SUSPEND_MAX];
static const char * const mem_sleep_labels[] = {
	[PM_SUSPEND_TO_IDLE] = "s2idle",
	[PM_SUSPEND_STANDBY] = "shallow",
	[PM_SUSPEND_MEM] = "deep",
};
/*
 * IAMROOT, 2021.12.18:
 * - 해당 PM_SUSPEND_XXX가 지원한다면 labels에서 string pointer를 가져와 set한다.
 */
const char *mem_sleep_states[PM_SUSPEND_MAX];

/*
 * IAMROOT, 2021.12.18:
 * - 초기값은 idle인데 psci에서 suspend관련 초기화를 할때
 *   PM_SUSPEND_STANDBY or PM_SUSPEND_MEM이 설정된다.
 */
suspend_state_t mem_sleep_current = PM_SUSPEND_TO_IDLE;
suspend_state_t mem_sleep_default = PM_SUSPEND_MAX;
suspend_state_t pm_suspend_target_state;
EXPORT_SYMBOL_GPL(pm_suspend_target_state);

unsigned int pm_suspend_global_flags;
EXPORT_SYMBOL_GPL(pm_suspend_global_flags);

/*
 * IAMROOT, 2021.12.18:
 * - psci에서 suspend 관련 초기화를 할때 설정된다.
 */
static const struct platform_suspend_ops *suspend_ops;
static const struct platform_s2idle_ops *s2idle_ops;
static DECLARE_SWAIT_QUEUE_HEAD(s2idle_wait_head);

/*
 * IAMROOT, 2023.03.16:
 * --- chat openai ----
 * s2idle_state는 S2idle(suspend-to-idle) 저전력 유휴 상태에서 시스템의
 * 현재 상태를 나타냅니다.
 *
 * S2idle 상태는 저전력 유휴 상태로, 시스템이 전력을 절약하면서 사용자 입력에
 * 일정 수준의 응답성을 유지합니다. 이 상태에서 시스템은 사용자 활동에 응답하여
 * 전체 전원 모드로 빠르게 다시 전환할 수 있습니다.
 *
 * S2Idle(suspend-to-idle) 기능의 세 가지 가능한 상태를 정의합니다.
 * 이 기능은 웨이크업 시간이 빠른 최신 시스템에 최적화된 저전력 절전
 * 모드입니다. 상태는 다음과 같습니다.
 *
 * S2IDLE_STATE_NONE: 이 상태는 시스템이 일시 중지 또는 일시 중단되지
 * 않았음을 나타냅니다. 즉, 시스템이 정상 작동 상태에 있습니다.
 *
 * S2IDLE_STATE_ENTER: 이 상태는 시스템이 S2Idle 상태로 들어가고 있음을
 * 나타냅니다. 이것은 시스템이 일정 시간 동안 유휴 상태이고 커널이 에너지를
 * 절약하기 위해 시스템을 저전력 절전 모드로 전환하기로 결정할 때 발생합니다.
 *
 * S2IDLE_STATE_WAKE: 이 상태는 시스템이 S2Idle 상태에서 깨어나고 있음을
 * 나타냅니다. 이는 시스템이 정상 작동을 재개해야 하는 인터럽트 또는 기타
 * 이벤트가 발생할 때 발생합니다.
 *
 * 이러한 상태는 커널에서 S2Idle 기능과 관련하여 시스템의 현재 상태를
 * 추적하는 데 사용됩니다. 그렇게 함으로써 커널은 S2Idle 상태에 들어가고
 * 나가는 적절한 작업을 수행할 수 있을 뿐만 아니라 시스템이 S2Idle 상태에
 * 있는 동안 발생하는 인터럽트 및 기타 이벤트를 처리할 수 있습니다.
 *
 * ---- Documentation/admin-guide/pm/sleep-states.rst ----
 * - s2idle (suspend-to-idle)
 *   이는 일반, 순수 소프트웨어, 시스템 일시 중단의 경량 
 *   변형입니다(S2I 또는 S2Idle이라고도 함). 사용자 공간을 동결하고 시간 
 *   기록을 중단하고 모든 I/O 장치를 저전력 상태(작동 상태에서 사용 가능한 
 *   것보다 더 낮은 전력)로 전환하여 런타임 유휴 상태에 비해 더 많은 
 *   에너지를 절약할 수 있습니다. 시스템이 일시 중단된 동안 가장 깊은 
 *   유휴 상태에 있는 시간입니다.
 *
 *   시스템은 대역 내 인터럽트에 의해 이 상태에서 깨어나므로 이론적으로 
 *   작동 상태에서 인터럽트를 생성할 수 있는 모든 장치를 S2Idle용 웨이크업 
 *   장치로 설정할 수 있습니다. 
 *
 *   이 상태는 :ref:`standby <standby>` 또는 :ref:`suspend-to-RAM <s2ram>`을 
 *   지원하지 않는 플랫폼에서 사용할 수 있습니다. 재개 대기 시간 감소를 
 *   제공합니다. :c:macro:`CONFIG_SUSPEND` 커널 구성 옵션이 설정되어 있으면 
 *   항상 지원됩니다.
 *
 * - s2ram (suspend-to-ram)
 *   이 상태(STR 또는 S2RAM이라고도 함)는 지원되는 경우 메모리를 제외하고 
 *   시스템의 모든 것이 저전력 상태로 전환되므로 상당한 에너지 절감 효과를 
 *   제공합니다. 내용물. 대기 <대기>'에 들어갈 때 수행되는 모든 단계는 
 *   S2RAM으로 전환하는 동안에도 수행됩니다. 플랫폼 기능에 따라 추가 작업이 
 *   발생할 수 있습니다. 특히, ACPI 기반 시스템에서 커널은 S2RAM 전환 중 
 *   마지막 단계로 플랫폼 펌웨어(BIOS)에 제어권을 넘기고 그 결과 일반적으로 
 *   커널이 직접 제어하지 않는 일부 하위 수준 구성 요소의 전원이 꺼집니다.
 *
 *   장치 및 CPU의 상태는 메모리에 저장되고 유지됩니다. 모든 장치가 일시 
 *   중단되고 저전력 상태로 전환됩니다. 많은 경우에 모든 주변 장치 버스는 
 *   S2RAM에 진입할 때 전력이 손실되므로 장치는 "켜짐" 상태로의 전환을 
 *   처리할 수 있어야 합니다.  
 *
 *   ACPI 기반 시스템에서 S2RAM은 플랫폼 펌웨어에서 시스템을 재개하기 위해 
 *   최소한의 부트스트래핑 코드가 필요합니다. 다른 플랫폼에서도 마찬가지일 
 *   수 있습니다.
 *
 *   S2RAM에서 시스템을 깨울 수 있는 장치 세트는 일반적으로 일시 중지에서 
 *   유휴 <s2idle>` 및 대기 <standby>`에 비해 줄어들며 플랫폼에 의존해야 할 
 *   수도 있습니다. 웨이크업 기능을 적절하게 설정하기 위해.
 *
 *   S2RAM은 :c:macro:`CONFIG_SUSPEND` 커널 구성 옵션이 설정되고 이에 대한 
 *   지원이 코어 시스템 일시 중지 하위 시스템이 있는 플랫폼에서 등록된 경우 
 *   지원됩니다. ACPI 기반 시스템에서는 ACPI에서 정의한 S3 시스템 상태에 
 *   매핑됩니다. 
 *
 * - standby
 *   지원되는 경우 이 상태는 작동 상태로의 비교적 간단한 전환을 제공하면서 
 *   중간 정도의 실질적인 에너지 절감 효과를 제공합니다. 작동 상태가 
 *   손실되지 않으므로(시스템 코어 로직이 전원을 유지함) 시스템이 중단된
 *   위치로 충분히 쉽게 돌아갈 수 있습니다.  사용자 공간 정지, 시간 기록 
 *   일시 중단 및 모든 I/O 장치를 저전력 상태로 전환하는 것 외에도 유휴 
 *   일시 중단 <s2idle>`에 대해서도 수행되며 부팅되지 않는 CPU는 오프라인 
 *   상태가 되며 모두 로우 상태가 됩니다. -레벨 시스템 기능은 이 상태로 
 *   전환하는 동안 일시 중지됩니다. 이러한 이유로 일시 중단에서 유휴
 *   <s2idle>`에 비해 더 많은 에너지를 절약할 수 있어야 하지만 재개 대기
 *   시간은 일반적으로 해당 상태보다 큽니다.
 *
 *   이 상태에서 시스템을 깨울 수 있는 장치 집합은 일반적으로
 *   'suspend-to-idle <s2idle>'에 비해 줄어들며 깨우기 기능을 적절하게
 *   설정하기 위해 플랫폼에 의존해야 할 수도 있습니다.
 *
 *   이 상태는 :c:macro:`CONFIG_SUSPEND` 커널 구성 옵션이 설정되고 이에 
 *   대한 지원이 코어 시스템 일시 중지 하위 시스템이 있는 플랫폼에서 등록된
 *   경우에 지원됩니다. ACPI 기반 시스템에서 이 상태는 ACPI에서 정의한 S1 
 *   시스템 상태에 매핑됩니다. 
 *
 * - 절전 진입 및 복귀 속도 
 *   s2idle > standby > s2ram > s2disk
 * ---  sysfs -------
 * - sysfs에서 확인 및 진입방법
 *   - s2idle
 *		/sys/power/state		freeze
 *		/sys/power/mem_sleep	s2idle 로 변경 후 /sys/power/state mem 입력.
 *   - standby
 *		/sys/power/state		standby
 *		/sys/power/mem_sleep	shallow 로 변경 후 /sys/power/state mem 입력.
 *   - s2ram
 *		/sys/power/mem_sleep	deep 로 변경후 /sys/power/state mem 입력.
 *   - s2disk
 *      /sys/power/state		disk 
 *      /sys/power/disk에 선택된 설정이 동작한다.
 *      (shutdown, reboot, suspend, test_resume)
 * - 사용예 
 *  1. 
 *   KVM /sys/power$ echo reboot > disk 
 *   KVM /sys/power$ cat disk 
 *   shutdown [reboot] suspend test_resume
 *  2. 
 *  KVM /sys/power$ echo disk > state 
 *  [   87.915123] PM: hibernation: hibernation entry
 *
 *  3. 
 *  KVM /sys/power$ cat mem_sleep 
 *  [s2idle] 
 *  KVM /sys/power$ echo mem > state 
 *  [  153.478136] PM: suspend entry (s2idle)
 * ----------------------------------------------------------
 *
 */
enum s2idle_states __read_mostly s2idle_state;
static DEFINE_RAW_SPINLOCK(s2idle_lock);

/**
 * pm_suspend_default_s2idle - Check if suspend-to-idle is the default suspend.
 *
 * Return 'true' if suspend-to-idle has been selected as the default system
 * suspend method.
 */
bool pm_suspend_default_s2idle(void)
{
	return mem_sleep_current == PM_SUSPEND_TO_IDLE;
}
EXPORT_SYMBOL_GPL(pm_suspend_default_s2idle);

void s2idle_set_ops(const struct platform_s2idle_ops *ops)
{
	lock_system_sleep();
	s2idle_ops = ops;
	unlock_system_sleep();
}

static void s2idle_begin(void)
{
	s2idle_state = S2IDLE_STATE_NONE;
}

static void s2idle_enter(void)
{
	trace_suspend_resume(TPS("machine_suspend"), PM_SUSPEND_TO_IDLE, true);

	raw_spin_lock_irq(&s2idle_lock);
	if (pm_wakeup_pending())
		goto out;

	s2idle_state = S2IDLE_STATE_ENTER;
	raw_spin_unlock_irq(&s2idle_lock);

	cpus_read_lock();
	cpuidle_resume();

	/* Push all the CPUs into the idle loop. */
	wake_up_all_idle_cpus();
	/* Make the current CPU wait so it can enter the idle loop too. */
	swait_event_exclusive(s2idle_wait_head,
		    s2idle_state == S2IDLE_STATE_WAKE);

	cpuidle_pause();
	cpus_read_unlock();

	raw_spin_lock_irq(&s2idle_lock);

 out:
	s2idle_state = S2IDLE_STATE_NONE;
	raw_spin_unlock_irq(&s2idle_lock);

	trace_suspend_resume(TPS("machine_suspend"), PM_SUSPEND_TO_IDLE, false);
}

static void s2idle_loop(void)
{
	pm_pr_dbg("suspend-to-idle\n");

	/*
	 * Suspend-to-idle equals:
	 * frozen processes + suspended devices + idle processors.
	 * Thus s2idle_enter() should be called right after all devices have
	 * been suspended.
	 *
	 * Wakeups during the noirq suspend of devices may be spurious, so try
	 * to avoid them upfront.
	 */
	for (;;) {
		if (s2idle_ops && s2idle_ops->wake) {
			if (s2idle_ops->wake())
				break;
		} else if (pm_wakeup_pending()) {
			break;
		}

		pm_wakeup_clear(false);

		s2idle_enter();
	}

	pm_pr_dbg("resume from suspend-to-idle\n");
}

void s2idle_wake(void)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&s2idle_lock, flags);
	if (s2idle_state > S2IDLE_STATE_NONE) {
		s2idle_state = S2IDLE_STATE_WAKE;
		swake_up_one(&s2idle_wait_head);
	}
	raw_spin_unlock_irqrestore(&s2idle_lock, flags);
}
EXPORT_SYMBOL_GPL(s2idle_wake);

static bool valid_state(suspend_state_t state)
{
	/*
	 * PM_SUSPEND_STANDBY and PM_SUSPEND_MEM states need low level
	 * support and need to be valid to the low level
	 * implementation, no valid callback implies that none are valid.
	 */
	return suspend_ops && suspend_ops->valid && suspend_ops->valid(state);
}

void __init pm_states_init(void)
{
	/* "mem" and "freeze" are always present in /sys/power/state. */
	pm_states[PM_SUSPEND_MEM] = pm_labels[PM_SUSPEND_MEM];
	pm_states[PM_SUSPEND_TO_IDLE] = pm_labels[PM_SUSPEND_TO_IDLE];
	/*
	 * Suspend-to-idle should be supported even without any suspend_ops,
	 * initialize mem_sleep_states[] accordingly here.
	 */
	mem_sleep_states[PM_SUSPEND_TO_IDLE] = mem_sleep_labels[PM_SUSPEND_TO_IDLE];
}

static int __init mem_sleep_default_setup(char *str)
{
	suspend_state_t state;

	for (state = PM_SUSPEND_TO_IDLE; state <= PM_SUSPEND_MEM; state++)
		if (mem_sleep_labels[state] &&
		    !strcmp(str, mem_sleep_labels[state])) {
			mem_sleep_default = state;
			break;
		}

	return 1;
}
__setup("mem_sleep_default=", mem_sleep_default_setup);

/**
 * suspend_set_ops - Set the global suspend method table.
 * @ops: Suspend operations to use.
 */
/*
 * IAMROOT, 2021.12.18:
 * - system이 suspend상태로 동작할수있는 platform_suspend_ops를 설정한다.
 * - psci_init_system_suspend에서 호출됬을때의 ops는 psci_suspend_ops이고
 *   여기선 state가 PM_SUSPEND_MEM이므로 PM_SUSPEND_MEM쪽으로 초기화가 일단
 *   진행하는것으로 보인다.
 */
void suspend_set_ops(const struct platform_suspend_ops *ops)
{
	lock_system_sleep();

	suspend_ops = ops;

	if (valid_state(PM_SUSPEND_STANDBY)) {
		mem_sleep_states[PM_SUSPEND_STANDBY] = mem_sleep_labels[PM_SUSPEND_STANDBY];
		pm_states[PM_SUSPEND_STANDBY] = pm_labels[PM_SUSPEND_STANDBY];
		if (mem_sleep_default == PM_SUSPEND_STANDBY)
			mem_sleep_current = PM_SUSPEND_STANDBY;
	}
	if (valid_state(PM_SUSPEND_MEM)) {
		mem_sleep_states[PM_SUSPEND_MEM] = mem_sleep_labels[PM_SUSPEND_MEM];
		if (mem_sleep_default >= PM_SUSPEND_MEM)
			mem_sleep_current = PM_SUSPEND_MEM;
	}

	unlock_system_sleep();
}
EXPORT_SYMBOL_GPL(suspend_set_ops);

/**
 * suspend_valid_only_mem - Generic memory-only valid callback.
 * @state: Target system sleep state.
 *
 * Platform drivers that implement mem suspend only and only need to check for
 * that in their .valid() callback can use this instead of rolling their own
 * .valid() callback.
 */
int suspend_valid_only_mem(suspend_state_t state)
{
	return state == PM_SUSPEND_MEM;
}
EXPORT_SYMBOL_GPL(suspend_valid_only_mem);

static bool sleep_state_supported(suspend_state_t state)
{
	return state == PM_SUSPEND_TO_IDLE || (suspend_ops && suspend_ops->enter);
}

static int platform_suspend_prepare(suspend_state_t state)
{
	return state != PM_SUSPEND_TO_IDLE && suspend_ops->prepare ?
		suspend_ops->prepare() : 0;
}

static int platform_suspend_prepare_late(suspend_state_t state)
{
	return state == PM_SUSPEND_TO_IDLE && s2idle_ops && s2idle_ops->prepare ?
		s2idle_ops->prepare() : 0;
}

static int platform_suspend_prepare_noirq(suspend_state_t state)
{
	if (state == PM_SUSPEND_TO_IDLE)
		return s2idle_ops && s2idle_ops->prepare_late ?
			s2idle_ops->prepare_late() : 0;

	return suspend_ops->prepare_late ? suspend_ops->prepare_late() : 0;
}

static void platform_resume_noirq(suspend_state_t state)
{
	if (state == PM_SUSPEND_TO_IDLE) {
		if (s2idle_ops && s2idle_ops->restore_early)
			s2idle_ops->restore_early();
	} else if (suspend_ops->wake) {
		suspend_ops->wake();
	}
}

static void platform_resume_early(suspend_state_t state)
{
	if (state == PM_SUSPEND_TO_IDLE && s2idle_ops && s2idle_ops->restore)
		s2idle_ops->restore();
}

static void platform_resume_finish(suspend_state_t state)
{
	if (state != PM_SUSPEND_TO_IDLE && suspend_ops->finish)
		suspend_ops->finish();
}

static int platform_suspend_begin(suspend_state_t state)
{
	if (state == PM_SUSPEND_TO_IDLE && s2idle_ops && s2idle_ops->begin)
		return s2idle_ops->begin();
	else if (suspend_ops && suspend_ops->begin)
		return suspend_ops->begin(state);
	else
		return 0;
}

static void platform_resume_end(suspend_state_t state)
{
	if (state == PM_SUSPEND_TO_IDLE && s2idle_ops && s2idle_ops->end)
		s2idle_ops->end();
	else if (suspend_ops && suspend_ops->end)
		suspend_ops->end();
}

static void platform_recover(suspend_state_t state)
{
	if (state != PM_SUSPEND_TO_IDLE && suspend_ops->recover)
		suspend_ops->recover();
}

static bool platform_suspend_again(suspend_state_t state)
{
	return state != PM_SUSPEND_TO_IDLE && suspend_ops->suspend_again ?
		suspend_ops->suspend_again() : false;
}

#ifdef CONFIG_PM_DEBUG
static unsigned int pm_test_delay = 5;
module_param(pm_test_delay, uint, 0644);
MODULE_PARM_DESC(pm_test_delay,
		 "Number of seconds to wait before resuming from suspend test");
#endif

static int suspend_test(int level)
{
#ifdef CONFIG_PM_DEBUG
	if (pm_test_level == level) {
		pr_info("suspend debug: Waiting for %d second(s).\n",
				pm_test_delay);
		mdelay(pm_test_delay * 1000);
		return 1;
	}
#endif /* !CONFIG_PM_DEBUG */
	return 0;
}

/**
 * suspend_prepare - Prepare for entering system sleep state.
 * @state: Target system sleep state.
 *
 * Common code run for every system sleep state that can be entered (except for
 * hibernation).  Run suspend notifiers, allocate the "suspend" console and
 * freeze processes.
 */
static int suspend_prepare(suspend_state_t state)
{
	int error;

	if (!sleep_state_supported(state))
		return -EPERM;

	pm_prepare_console();

	error = pm_notifier_call_chain_robust(PM_SUSPEND_PREPARE, PM_POST_SUSPEND);
	if (error)
		goto Restore;

	trace_suspend_resume(TPS("freeze_processes"), 0, true);
	error = suspend_freeze_processes();
	trace_suspend_resume(TPS("freeze_processes"), 0, false);
	if (!error)
		return 0;

	suspend_stats.failed_freeze++;
	dpm_save_failed_step(SUSPEND_FREEZE);
	pm_notifier_call_chain(PM_POST_SUSPEND);
 Restore:
	pm_restore_console();
	return error;
}

/* default implementation */
void __weak arch_suspend_disable_irqs(void)
{
	local_irq_disable();
}

/* default implementation */
void __weak arch_suspend_enable_irqs(void)
{
	local_irq_enable();
}

/**
 * suspend_enter - Make the system enter the given sleep state.
 * @state: System sleep state to enter.
 * @wakeup: Returns information that the sleep state should not be re-entered.
 *
 * This function should be called after devices have been suspended.
 */
static int suspend_enter(suspend_state_t state, bool *wakeup)
{
	int error;

	error = platform_suspend_prepare(state);
	if (error)
		goto Platform_finish;

	error = dpm_suspend_late(PMSG_SUSPEND);
	if (error) {
		pr_err("late suspend of devices failed\n");
		goto Platform_finish;
	}
	error = platform_suspend_prepare_late(state);
	if (error)
		goto Devices_early_resume;

	error = dpm_suspend_noirq(PMSG_SUSPEND);
	if (error) {
		pr_err("noirq suspend of devices failed\n");
		goto Platform_early_resume;
	}
	error = platform_suspend_prepare_noirq(state);
	if (error)
		goto Platform_wake;

	if (suspend_test(TEST_PLATFORM))
		goto Platform_wake;

	if (state == PM_SUSPEND_TO_IDLE) {
		s2idle_loop();
		goto Platform_wake;
	}

	error = suspend_disable_secondary_cpus();
	if (error || suspend_test(TEST_CPUS))
		goto Enable_cpus;

	arch_suspend_disable_irqs();
	BUG_ON(!irqs_disabled());

	system_state = SYSTEM_SUSPEND;

	error = syscore_suspend();
	if (!error) {
		*wakeup = pm_wakeup_pending();
		if (!(suspend_test(TEST_CORE) || *wakeup)) {
			trace_suspend_resume(TPS("machine_suspend"),
				state, true);
			error = suspend_ops->enter(state);
			trace_suspend_resume(TPS("machine_suspend"),
				state, false);
		} else if (*wakeup) {
			error = -EBUSY;
		}
		syscore_resume();
	}

	system_state = SYSTEM_RUNNING;

	arch_suspend_enable_irqs();
	BUG_ON(irqs_disabled());

 Enable_cpus:
	suspend_enable_secondary_cpus();

 Platform_wake:
	platform_resume_noirq(state);
	dpm_resume_noirq(PMSG_RESUME);

 Platform_early_resume:
	platform_resume_early(state);

 Devices_early_resume:
	dpm_resume_early(PMSG_RESUME);

 Platform_finish:
	platform_resume_finish(state);
	return error;
}

/**
 * suspend_devices_and_enter - Suspend devices and enter system sleep state.
 * @state: System sleep state to enter.
 */
int suspend_devices_and_enter(suspend_state_t state)
{
	int error;
	bool wakeup = false;

	if (!sleep_state_supported(state))
		return -ENOSYS;

	pm_suspend_target_state = state;

	if (state == PM_SUSPEND_TO_IDLE)
		pm_set_suspend_no_platform();

	error = platform_suspend_begin(state);
	if (error)
		goto Close;

	suspend_console();
	suspend_test_start();
	error = dpm_suspend_start(PMSG_SUSPEND);
	if (error) {
		pr_err("Some devices failed to suspend, or early wake event detected\n");
		goto Recover_platform;
	}
	suspend_test_finish("suspend devices");
	if (suspend_test(TEST_DEVICES))
		goto Recover_platform;

	do {
		error = suspend_enter(state, &wakeup);
	} while (!error && !wakeup && platform_suspend_again(state));

 Resume_devices:
	suspend_test_start();
	dpm_resume_end(PMSG_RESUME);
	suspend_test_finish("resume devices");
	trace_suspend_resume(TPS("resume_console"), state, true);
	resume_console();
	trace_suspend_resume(TPS("resume_console"), state, false);

 Close:
	platform_resume_end(state);
	pm_suspend_target_state = PM_SUSPEND_ON;
	return error;

 Recover_platform:
	platform_recover(state);
	goto Resume_devices;
}

/**
 * suspend_finish - Clean up before finishing the suspend sequence.
 *
 * Call platform code to clean up, restart processes, and free the console that
 * we've allocated. This routine is not called for hibernation.
 */
static void suspend_finish(void)
{
	suspend_thaw_processes();
	pm_notifier_call_chain(PM_POST_SUSPEND);
	pm_restore_console();
}

/**
 * enter_state - Do common work needed to enter system sleep state.
 * @state: System sleep state to enter.
 *
 * Make sure that no one else is trying to put the system into a sleep state.
 * Fail if that's not the case.  Otherwise, prepare for system suspend, make the
 * system enter the given sleep state and clean up after wakeup.
 */
static int enter_state(suspend_state_t state)
{
	int error;

	trace_suspend_resume(TPS("suspend_enter"), state, true);
	if (state == PM_SUSPEND_TO_IDLE) {
#ifdef CONFIG_PM_DEBUG
		if (pm_test_level != TEST_NONE && pm_test_level <= TEST_CPUS) {
			pr_warn("Unsupported test mode for suspend to idle, please choose none/freezer/devices/platform.\n");
			return -EAGAIN;
		}
#endif
	} else if (!valid_state(state)) {
		return -EINVAL;
	}
	if (!mutex_trylock(&system_transition_mutex))
		return -EBUSY;

	if (state == PM_SUSPEND_TO_IDLE)
		s2idle_begin();

	if (sync_on_suspend_enabled) {
		trace_suspend_resume(TPS("sync_filesystems"), 0, true);
		ksys_sync_helper();
		trace_suspend_resume(TPS("sync_filesystems"), 0, false);
	}

	pm_pr_dbg("Preparing system for sleep (%s)\n", mem_sleep_labels[state]);
	pm_suspend_clear_flags();
	error = suspend_prepare(state);
	if (error)
		goto Unlock;

	if (suspend_test(TEST_FREEZER))
		goto Finish;

	trace_suspend_resume(TPS("suspend_enter"), state, false);
	pm_pr_dbg("Suspending system (%s)\n", mem_sleep_labels[state]);
	pm_restrict_gfp_mask();
	error = suspend_devices_and_enter(state);
	pm_restore_gfp_mask();

 Finish:
	events_check_enabled = false;
	pm_pr_dbg("Finishing wakeup.\n");
	suspend_finish();
 Unlock:
	mutex_unlock(&system_transition_mutex);
	return error;
}

/**
 * pm_suspend - Externally visible function for suspending the system.
 * @state: System sleep state to enter.
 *
 * Check if the value of @state represents one of the supported states,
 * execute enter_state() and update system suspend statistics.
 */
int pm_suspend(suspend_state_t state)
{
	int error;

	if (state <= PM_SUSPEND_ON || state >= PM_SUSPEND_MAX)
		return -EINVAL;

	pr_info("suspend entry (%s)\n", mem_sleep_labels[state]);
	error = enter_state(state);
	if (error) {
		suspend_stats.fail++;
		dpm_save_failed_errno(error);
	} else {
		suspend_stats.success++;
	}
	pr_info("suspend exit\n");
	return error;
}
EXPORT_SYMBOL(pm_suspend);
