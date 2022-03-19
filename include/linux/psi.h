#ifndef _LINUX_PSI_H
#define _LINUX_PSI_H

#include <linux/jump_label.h>
#include <linux/psi_types.h>
#include <linux/sched.h>
#include <linux/poll.h>

struct seq_file;
struct css_set;

/*
 * IAMROOT, 2022.03.19:
 * - memory가 압박을 받는 상황에서의 정보를 저장하여 /proc/pressure/memory 를
 *   통해 user등에 제공한다.
 *
 * - ex) https://www.kernel.org/doc/html/latest/accounting/psi.html
 */
#ifdef CONFIG_PSI

extern struct static_key_false psi_disabled;
extern struct psi_group psi_system;

void psi_init(void);

void psi_task_change(struct task_struct *task, int clear, int set);
void psi_task_switch(struct task_struct *prev, struct task_struct *next,
		     bool sleep);

void psi_memstall_enter(unsigned long *flags);
void psi_memstall_leave(unsigned long *flags);

int psi_show(struct seq_file *s, struct psi_group *group, enum psi_res res);

#ifdef CONFIG_CGROUPS
int psi_cgroup_alloc(struct cgroup *cgrp);
void psi_cgroup_free(struct cgroup *cgrp);
void cgroup_move_task(struct task_struct *p, struct css_set *to);

struct psi_trigger *psi_trigger_create(struct psi_group *group,
			char *buf, size_t nbytes, enum psi_res res);
void psi_trigger_replace(void **trigger_ptr, struct psi_trigger *t);

__poll_t psi_trigger_poll(void **trigger_ptr, struct file *file,
			poll_table *wait);
#endif

#else /* CONFIG_PSI */

static inline void psi_init(void) {}

static inline void psi_memstall_enter(unsigned long *flags) {}
static inline void psi_memstall_leave(unsigned long *flags) {}

#ifdef CONFIG_CGROUPS
static inline int psi_cgroup_alloc(struct cgroup *cgrp)
{
	return 0;
}
static inline void psi_cgroup_free(struct cgroup *cgrp)
{
}
static inline void cgroup_move_task(struct task_struct *p, struct css_set *to)
{
	rcu_assign_pointer(p->cgroups, to);
}
#endif

#endif /* CONFIG_PSI */

#endif /* _LINUX_PSI_H */
