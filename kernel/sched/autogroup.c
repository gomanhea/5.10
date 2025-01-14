// SPDX-License-Identifier: GPL-2.0
/*
 * Auto-group scheduling implementation:
 */
#include <linux/nospec.h>
#include "sched.h"

unsigned int __read_mostly sysctl_sched_autogroup_enabled = 1;
static struct autogroup autogroup_default;
static atomic_t autogroup_seq_nr;


/*
 * IAMROOT, 2023.05.07:
 * --- chatopenai ----
 * - autogroup 이란
 *   Linux 커널에서 자동 그룹은 동일한 사용자가 생성하고 서로 관련된 
 *   작업을 자동으로 함께 그룹화하는 프로세스 스케줄러의 기능입니다. 
 *   autogroup 기능은 서로 다른 사용자의 많은 수의 프로세스가 시스템에서 
 *   실행 중일 때 발생할 수 있는 CPU 사용 불균형 문제를 해결하기 위해 
 *   커널 버전 2.6.38에 도입되었습니다.
 *
 *   자동 그룹 기능은 시스템에 로그인하는 각 사용자에 대해 별도의 일정 
 *   그룹을 생성하여 작동합니다. 사용자가 생성한 각각의 새 프로세스는 
 *   자체 스케줄링 정책이 있고 상대 가중치를 기반으로 다른 자동 그룹과 
 *   CPU 시간을 공유하는 해당 자동 그룹에 자동으로 할당됩니다.
 *
 *   관련 작업을 함께 그룹화함으로써 자동 그룹 기능은 서로 다른 프로세스 
 *   간의 컨텍스트 스위치 수를 줄이고 캐시 스래싱을 최소화하여 CPU 
 *   사용률을 향상시킬 수 있습니다. 또한 한 사용자가 다른 사용자를 
 *   희생시키면서 시스템 리소스를 독점하는 것을 방지할 수 있습니다.
 *
 *   전반적으로 자동 그룹 기능은 시스템의 작업 부하와 프로세스를 실행 
 *   중인 사용자의 동작에 동적으로 적응하여 Linux 스케줄러의 공정성과 
 *   효율성을 개선하도록 설계되었습니다. 
 *
 * - sched debug를 확인할때, cfs_rq[1]:/autogroup-17과 cfs_rq[1]:/ 
 *   라는게 있는게 확인됬다. 1은 cpu 번호를 말하는데,
 *   /autogroup-17, /의 차이점은 무엇인가?
 *
 *   언급한 디버그 출력에서 cfs_rq[1]:/autogroup-17 및 cfs_rq[1]:/는 
 *   CPU 1에 존재하는 두 개의 서로 다른 CFS 실행 대기열을 참조합니다.
 *
 *   /autogroup-17은 사용자 ID 17이 생성한 작업을 포함하는 자동 그룹의 
 *   CFS 실행 대기열을 나타냅니다. 이는 사용자 17이 생성한 모든 작업이 
 *   동일한 CFS 실행 대기열에서 예약되고 동일한 예약 정책 및 매개변수를 
 *   공유함을 의미합니다. autogroup-17 실행 대기열에는 다른 CFS 실행 
 *   대기열과 독립적인 가중치 및 최소 세분성과 같은 자체 스케줄링 
 *   매개변수가 있습니다.
 *
 *   반면 /는 CPU 1의 다른 모든 비 자동 그룹 작업에 대한 CFS 실행 
 *   대기열을 나타냅니다. 여기에는 일반적으로 시스템 데몬 또는 기타 
 *   비대화형 프로그램에 의해 시작되는 프로세스인 자동 그룹의 일부가 
 *   아닌 모든 작업이 포함됩니다.
 *
 *   따라서 cfs_rq[1]:/autogroup-17과 cfs_rq[1]:/의 차이점은 전자는 
 *   특정 자동 그룹(이 경우 사용자 17이 생성한 작업을 포함하는 그룹)에 
 *   대한 CFS 실행 대기열을 나타내는 반면 후자는 동일한 CPU에서 자동 
 *   그룹이 아닌 다른 모든 작업에 대한 CFS 실행 대기열을 나타냅니다. 
 * -------------------
 *  - autogroup은 로긴하는 유저를 대상으로 자동으로 그룹을 만들어줍니다. 
 *  A 사용자가 nice -5 task를 10개, B 사용자가 nice -0 task를 1개 만들었을 
 *  때 두 사용자간의 시간 점유율을 50:50으로 공평하게 나누기 위해 만들었습니다. 
 *
 *   결국 cgroup cpu 서브시스템에 user-A 디렉토리, user-B 디렉토리를 만들고 
 *   그 안에 서로 다른 nice 값을 가진 태스크들을 몇 개씩 넣어 사용해도, 
 *   user-A와 user-B는 동일한 50:50의 비율을 사용하게 되어 있습니다. 
 *
 *   물론 A 사용자에게 다른 사용자들보다 2배의 시간 분배를 부여하려면 user-A 
 *   디렉토리에 있는 cpu.shares 값을 1024 -> 2048로 변경하면 됩니다. 
 *   (디렉토리에 대한 load weight 개념입니다) 
 *
 *   nice 값은 동일 사용자(같은 cgroup 디렉토리에 있는 task들)가 동작중인 
 *   태스크들간의 시간 점유율을 바꿀 수 있고, cgroup 디렉토리간의 점유율은 
 *   cpu.shares로 지정하는 개념입니다. 
 *
 *   cfs에서 fair 라는 관점이 처음엔 태스크들 사이에서 사용된 개념입니다. 
 *   특별히 nice 값을 지정하지 않으면 모든 태스크가 같은 시간을 사용합니다.
 *   그런데 유저들간의 같은 시간을 사용하여야 fair 다 라는 개념으로 탄생한 것이 
 *   cgroup이고, 이를 사용자 로긴시마다 자동으로 부여하는 것이 autogroup입니다.
 *   즉 cgroup을 사용하면 각 그룹에 지정된 cpu.shares 값을 변경하지 않으면 
 *   모든 사용자간에 공평한 시간을 사용하게 합니다 
 */
/*
 * IAMROOT, 2022.11.26:
 * - autogrup 초기화.
 *   유저 로그인시 자동 그룹 할당
 */
void __init autogroup_init(struct task_struct *init_task)
{
	autogroup_default.tg = &root_task_group;
	kref_init(&autogroup_default.kref);
	init_rwsem(&autogroup_default.lock);
	init_task->signal->autogroup = &autogroup_default;
}

void autogroup_free(struct task_group *tg)
{
	kfree(tg->autogroup);
}

static inline void autogroup_destroy(struct kref *kref)
{
	struct autogroup *ag = container_of(kref, struct autogroup, kref);

#ifdef CONFIG_RT_GROUP_SCHED
	/* We've redirected RT tasks to the root task group... */
	ag->tg->rt_se = NULL;
	ag->tg->rt_rq = NULL;
#endif
	sched_offline_group(ag->tg);
	sched_destroy_group(ag->tg);
}

static inline void autogroup_kref_put(struct autogroup *ag)
{
	kref_put(&ag->kref, autogroup_destroy);
}

static inline struct autogroup *autogroup_kref_get(struct autogroup *ag)
{
	kref_get(&ag->kref);
	return ag;
}

static inline struct autogroup *autogroup_task_get(struct task_struct *p)
{
	struct autogroup *ag;
	unsigned long flags;

	if (!lock_task_sighand(p, &flags))
		return autogroup_kref_get(&autogroup_default);

	ag = autogroup_kref_get(p->signal->autogroup);
	unlock_task_sighand(p, &flags);

	return ag;
}

static inline struct autogroup *autogroup_create(void)
{
	struct autogroup *ag = kzalloc(sizeof(*ag), GFP_KERNEL);
	struct task_group *tg;

	if (!ag)
		goto out_fail;

	tg = sched_create_group(&root_task_group);
	if (IS_ERR(tg))
		goto out_free;

	kref_init(&ag->kref);
	init_rwsem(&ag->lock);
	ag->id = atomic_inc_return(&autogroup_seq_nr);
	ag->tg = tg;
#ifdef CONFIG_RT_GROUP_SCHED
	/*
	 * Autogroup RT tasks are redirected to the root task group
	 * so we don't have to move tasks around upon policy change,
	 * or flail around trying to allocate bandwidth on the fly.
	 * A bandwidth exception in __sched_setscheduler() allows
	 * the policy change to proceed.
	 */
	free_rt_sched_group(tg);
	tg->rt_se = root_task_group.rt_se;
	tg->rt_rq = root_task_group.rt_rq;
#endif
	tg->autogroup = ag;

	sched_online_group(tg, &root_task_group);
	return ag;

out_free:
	kfree(ag);
out_fail:
	if (printk_ratelimit()) {
		printk(KERN_WARNING "autogroup_create: %s failure.\n",
			ag ? "sched_create_group()" : "kzalloc()");
	}

	return autogroup_kref_get(&autogroup_default);
}

bool task_wants_autogroup(struct task_struct *p, struct task_group *tg)
{
	if (tg != &root_task_group)
		return false;
	/*
	 * If we race with autogroup_move_group() the caller can use the old
	 * value of signal->autogroup but in this case sched_move_task() will
	 * be called again before autogroup_kref_put().
	 *
	 * However, there is no way sched_autogroup_exit_task() could tell us
	 * to avoid autogroup->tg, so we abuse PF_EXITING flag for this case.
	 */
	if (p->flags & PF_EXITING)
		return false;

	return true;
}

void sched_autogroup_exit_task(struct task_struct *p)
{
	/*
	 * We are going to call exit_notify() and autogroup_move_group() can't
	 * see this thread after that: we can no longer use signal->autogroup.
	 * See the PF_EXITING check in task_wants_autogroup().
	 */
	sched_move_task(p);
}

static void
autogroup_move_group(struct task_struct *p, struct autogroup *ag)
{
	struct autogroup *prev;
	struct task_struct *t;
	unsigned long flags;

	BUG_ON(!lock_task_sighand(p, &flags));

	prev = p->signal->autogroup;
	if (prev == ag) {
		unlock_task_sighand(p, &flags);
		return;
	}

	p->signal->autogroup = autogroup_kref_get(ag);
	/*
	 * We can't avoid sched_move_task() after we changed signal->autogroup,
	 * this process can already run with task_group() == prev->tg or we can
	 * race with cgroup code which can read autogroup = prev under rq->lock.
	 * In the latter case for_each_thread() can not miss a migrating thread,
	 * cpu_cgroup_attach() must not be possible after cgroup_exit() and it
	 * can't be removed from thread list, we hold ->siglock.
	 *
	 * If an exiting thread was already removed from thread list we rely on
	 * sched_autogroup_exit_task().
	 */
	for_each_thread(p, t)
		sched_move_task(t);

	unlock_task_sighand(p, &flags);
	autogroup_kref_put(prev);
}

/* Allocates GFP_KERNEL, cannot be called under any spinlock: */
void sched_autogroup_create_attach(struct task_struct *p)
{
	struct autogroup *ag = autogroup_create();

	autogroup_move_group(p, ag);

	/* Drop extra reference added by autogroup_create(): */
	autogroup_kref_put(ag);
}
EXPORT_SYMBOL(sched_autogroup_create_attach);

/* Cannot be called under siglock. Currently has no users: */
void sched_autogroup_detach(struct task_struct *p)
{
	autogroup_move_group(p, &autogroup_default);
}
EXPORT_SYMBOL(sched_autogroup_detach);

void sched_autogroup_fork(struct signal_struct *sig)
{
	sig->autogroup = autogroup_task_get(current);
}

void sched_autogroup_exit(struct signal_struct *sig)
{
	autogroup_kref_put(sig->autogroup);
}

static int __init setup_autogroup(char *str)
{
	sysctl_sched_autogroup_enabled = 0;

	return 1;
}
__setup("noautogroup", setup_autogroup);

#ifdef CONFIG_PROC_FS

int proc_sched_autogroup_set_nice(struct task_struct *p, int nice)
{
	static unsigned long next = INITIAL_JIFFIES;
	struct autogroup *ag;
	unsigned long shares;
	int err, idx;

	if (nice < MIN_NICE || nice > MAX_NICE)
		return -EINVAL;

	err = security_task_setnice(current, nice);
	if (err)
		return err;

	if (nice < 0 && !can_nice(current, nice))
		return -EPERM;

	/* This is a heavy operation, taking global locks.. */
	if (!capable(CAP_SYS_ADMIN) && time_before(jiffies, next))
		return -EAGAIN;

	next = HZ / 10 + jiffies;
	ag = autogroup_task_get(p);

	idx = array_index_nospec(nice + 20, 40);
	shares = scale_load(sched_prio_to_weight[idx]);

	down_write(&ag->lock);
	err = sched_group_set_shares(ag->tg, shares);
	if (!err)
		ag->nice = nice;
	up_write(&ag->lock);

	autogroup_kref_put(ag);

	return err;
}

void proc_sched_autogroup_show_task(struct task_struct *p, struct seq_file *m)
{
	struct autogroup *ag = autogroup_task_get(p);

	if (!task_group_is_autogroup(ag->tg))
		goto out;

	down_read(&ag->lock);
	seq_printf(m, "/autogroup-%ld nice %d\n", ag->id, ag->nice);
	up_read(&ag->lock);

out:
	autogroup_kref_put(ag);
}
#endif /* CONFIG_PROC_FS */

int autogroup_path(struct task_group *tg, char *buf, int buflen)
{
	if (!task_group_is_autogroup(tg))
		return 0;

	return snprintf(buf, buflen, "%s-%ld", "/autogroup", tg->autogroup->id);
}
