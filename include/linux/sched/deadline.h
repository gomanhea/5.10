/* SPDX-License-Identifier: GPL-2.0 */

/*
 * SCHED_DEADLINE tasks has negative priorities, reflecting
 * the fact that any of them has higher prio than RT and
 * NORMAL/BATCH tasks.
 */

#define MAX_DL_PRIO		0

/*
 * IAMROOT, 2023.02.27:
 * @return 1 : dl prio
 * @prio가 dl인지 판단한다. dl 의 경우 prio = -1 이다.
 */
static inline int dl_prio(int prio)
{
	if (unlikely(prio < MAX_DL_PRIO))
		return 1;
	return 0;
}

/*
 * IAMROOT, 2023.02.25:
 * @return 1 : is dl task.
 *         0 : dl task아님.
 * - dl 의 경우 prio = -1
 */
static inline int dl_task(struct task_struct *p)
{
	return dl_prio(p->prio);
}

/*
 * IAMROOT, 2023.03.04:
 * @return true  ------+------+---->
 *                     a      b
 * @return false 그외
 */
static inline bool dl_time_before(u64 a, u64 b)
{
	return (s64)(a - b) < 0;
}

#ifdef CONFIG_SMP

struct root_domain;
extern void dl_add_task_root_domain(struct task_struct *p);
extern void dl_clear_root_domain(struct root_domain *rd);

#endif /* CONFIG_SMP */
