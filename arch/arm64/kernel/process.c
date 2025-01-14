// SPDX-License-Identifier: GPL-2.0-only
/*
 * Based on arch/arm/kernel/process.c
 *
 * Original Copyright (C) 1995  Linus Torvalds
 * Copyright (C) 1996-2000 Russell King - Converted to ARM.
 * Copyright (C) 2012 ARM Ltd.
 */
#include <linux/compat.h>
#include <linux/efi.h>
#include <linux/elf.h>
#include <linux/export.h>
#include <linux/sched.h>
#include <linux/sched/debug.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
#include <linux/kernel.h>
#include <linux/mman.h>
#include <linux/mm.h>
#include <linux/nospec.h>
#include <linux/stddef.h>
#include <linux/sysctl.h>
#include <linux/unistd.h>
#include <linux/user.h>
#include <linux/delay.h>
#include <linux/reboot.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/cpu.h>
#include <linux/elfcore.h>
#include <linux/pm.h>
#include <linux/tick.h>
#include <linux/utsname.h>
#include <linux/uaccess.h>
#include <linux/random.h>
#include <linux/hw_breakpoint.h>
#include <linux/personality.h>
#include <linux/notifier.h>
#include <trace/events/power.h>
#include <linux/percpu.h>
#include <linux/thread_info.h>
#include <linux/prctl.h>

#include <asm/alternative.h>
#include <asm/compat.h>
#include <asm/cpufeature.h>
#include <asm/cacheflush.h>
#include <asm/exec.h>
#include <asm/fpsimd.h>
#include <asm/mmu_context.h>
#include <asm/mte.h>
#include <asm/processor.h>
#include <asm/pointer_auth.h>
#include <asm/stacktrace.h>
#include <asm/switch_to.h>
#include <asm/system_misc.h>

#if defined(CONFIG_STACKPROTECTOR) && !defined(CONFIG_STACKPROTECTOR_PER_TASK)
#include <linux/stackprotector.h>
unsigned long __stack_chk_guard __ro_after_init;
EXPORT_SYMBOL(__stack_chk_guard);
#endif

/*
 * Function pointers to optional machine specific functions
 */
/*
 * IAMROOT, 2021.12.18:
 * - psci_0_2_set_functions과 같은 power management관련 함수들이 호출될때
 *   callback함수가 설정된다.
 * - ex) psci_sys_poweroff
 */
void (*pm_power_off)(void);
EXPORT_SYMBOL_GPL(pm_power_off);

#ifdef CONFIG_HOTPLUG_CPU
void arch_cpu_idle_dead(void)
{
       cpu_die();
}
#endif

/*
 * Called by kexec, immediately prior to machine_kexec().
 *
 * This must completely disable all secondary CPUs; simply causing those CPUs
 * to execute e.g. a RAM-based pin loop is not sufficient. This allows the
 * kexec'd kernel to use any and all RAM as it sees fit, without having to
 * avoid any code or data used by any SW CPU pin loop. The CPU hotplug
 * functionality embodied in smpt_shutdown_nonboot_cpus() to achieve this.
 */
void machine_shutdown(void)
{
	smp_shutdown_nonboot_cpus(reboot_cpu);
}

/*
 * Halting simply requires that the secondary CPUs stop performing any
 * activity (executing tasks, handling interrupts). smp_send_stop()
 * achieves this.
 */
void machine_halt(void)
{
	local_irq_disable();
	smp_send_stop();
	while (1);
}

/*
 * Power-off simply requires that the secondary CPUs stop performing any
 * activity (executing tasks, handling interrupts). smp_send_stop()
 * achieves this. When the system power is turned off, it will take all CPUs
 * with it.
 */
void machine_power_off(void)
{
	local_irq_disable();
	smp_send_stop();
	if (pm_power_off)
		pm_power_off();
}

/*
 * Restart requires that the secondary CPUs stop performing any activity
 * while the primary CPU resets the system. Systems with multiple CPUs must
 * provide a HW restart implementation, to ensure that all CPUs reset at once.
 * This is required so that any code running after reset on the primary CPU
 * doesn't have to co-ordinate with other CPUs to ensure they aren't still
 * executing pre-reset code, and using RAM that the primary CPU's code wishes
 * to use. Implementing such co-ordination would be essentially impossible.
 */
void machine_restart(char *cmd)
{
	/* Disable interrupts first */
	local_irq_disable();
	smp_send_stop();

	/*
	 * UpdateCapsule() depends on the system being reset via
	 * ResetSystem().
	 */
	if (efi_enabled(EFI_RUNTIME_SERVICES))
		efi_reboot(reboot_mode, NULL);

	/* Now call the architecture specific reboot code. */
	do_kernel_restart(cmd);

	/*
	 * Whoops - the architecture was unable to reboot.
	 */
	printk("Reboot failed -- System halted\n");
	while (1);
}

#define bstr(suffix, str) [PSR_BTYPE_ ## suffix >> PSR_BTYPE_SHIFT] = str
static const char *const btypes[] = {
	bstr(NONE, "--"),
	bstr(  JC, "jc"),
	bstr(   C, "-c"),
	bstr(  J , "j-")
};
#undef bstr

static void print_pstate(struct pt_regs *regs)
{
	u64 pstate = regs->pstate;

	if (compat_user_mode(regs)) {
		printk("pstate: %08llx (%c%c%c%c %c %s %s %c%c%c %cDIT %cSSBS)\n",
			pstate,
			pstate & PSR_AA32_N_BIT ? 'N' : 'n',
			pstate & PSR_AA32_Z_BIT ? 'Z' : 'z',
			pstate & PSR_AA32_C_BIT ? 'C' : 'c',
			pstate & PSR_AA32_V_BIT ? 'V' : 'v',
			pstate & PSR_AA32_Q_BIT ? 'Q' : 'q',
			pstate & PSR_AA32_T_BIT ? "T32" : "A32",
			pstate & PSR_AA32_E_BIT ? "BE" : "LE",
			pstate & PSR_AA32_A_BIT ? 'A' : 'a',
			pstate & PSR_AA32_I_BIT ? 'I' : 'i',
			pstate & PSR_AA32_F_BIT ? 'F' : 'f',
			pstate & PSR_AA32_DIT_BIT ? '+' : '-',
			pstate & PSR_AA32_SSBS_BIT ? '+' : '-');
	} else {
		const char *btype_str = btypes[(pstate & PSR_BTYPE_MASK) >>
					       PSR_BTYPE_SHIFT];

		printk("pstate: %08llx (%c%c%c%c %c%c%c%c %cPAN %cUAO %cTCO %cDIT %cSSBS BTYPE=%s)\n",
			pstate,
			pstate & PSR_N_BIT ? 'N' : 'n',
			pstate & PSR_Z_BIT ? 'Z' : 'z',
			pstate & PSR_C_BIT ? 'C' : 'c',
			pstate & PSR_V_BIT ? 'V' : 'v',
			pstate & PSR_D_BIT ? 'D' : 'd',
			pstate & PSR_A_BIT ? 'A' : 'a',
			pstate & PSR_I_BIT ? 'I' : 'i',
			pstate & PSR_F_BIT ? 'F' : 'f',
			pstate & PSR_PAN_BIT ? '+' : '-',
			pstate & PSR_UAO_BIT ? '+' : '-',
			pstate & PSR_TCO_BIT ? '+' : '-',
			pstate & PSR_DIT_BIT ? '+' : '-',
			pstate & PSR_SSBS_BIT ? '+' : '-',
			btype_str);
	}
}

void __show_regs(struct pt_regs *regs)
{
	int i, top_reg;
	u64 lr, sp;

	if (compat_user_mode(regs)) {
		lr = regs->compat_lr;
		sp = regs->compat_sp;
		top_reg = 12;
	} else {
		lr = regs->regs[30];
		sp = regs->sp;
		top_reg = 29;
	}

	show_regs_print_info(KERN_DEFAULT);
	print_pstate(regs);

	if (!user_mode(regs)) {
		printk("pc : %pS\n", (void *)regs->pc);
		printk("lr : %pS\n", (void *)ptrauth_strip_insn_pac(lr));
	} else {
		printk("pc : %016llx\n", regs->pc);
		printk("lr : %016llx\n", lr);
	}

	printk("sp : %016llx\n", sp);

	if (system_uses_irq_prio_masking())
		printk("pmr_save: %08llx\n", regs->pmr_save);

	i = top_reg;

	while (i >= 0) {
		printk("x%-2d: %016llx", i, regs->regs[i]);

		while (i-- % 3)
			pr_cont(" x%-2d: %016llx", i, regs->regs[i]);

		pr_cont("\n");
	}
}

void show_regs(struct pt_regs *regs)
{
	__show_regs(regs);
	dump_backtrace(regs, NULL, KERN_DEFAULT);
}

static void tls_thread_flush(void)
{
	write_sysreg(0, tpidr_el0);

	if (is_compat_task()) {
		current->thread.uw.tp_value = 0;

		/*
		 * We need to ensure ordering between the shadow state and the
		 * hardware state, so that we don't corrupt the hardware state
		 * with a stale shadow state during context switch.
		 */
		barrier();
		write_sysreg(0, tpidrro_el0);
	}
}

static void flush_tagged_addr_state(void)
{
	if (IS_ENABLED(CONFIG_ARM64_TAGGED_ADDR_ABI))
		clear_thread_flag(TIF_TAGGED_ADDR);
}

void flush_thread(void)
{
	fpsimd_flush_thread();
	tls_thread_flush();
	flush_ptrace_hw_breakpoint(current);
	flush_tagged_addr_state();
}

void release_thread(struct task_struct *dead_task)
{
}

void arch_release_task_struct(struct task_struct *tsk)
{
	fpsimd_release_task(tsk);
}

/*
 * IAMROOT, 2023.04.01:
 * - src를 dst에 copy한다.
 */
int arch_dup_task_struct(struct task_struct *dst, struct task_struct *src)
{
	if (current->mm)
		fpsimd_preserve_current_state();
	*dst = *src;

	/* We rely on the above assignment to initialize dst's thread_flags: */
	BUILD_BUG_ON(!IS_ENABLED(CONFIG_THREAD_INFO_IN_TASK));

	/*
	 * Detach src's sve_state (if any) from dst so that it does not
	 * get erroneously used or freed prematurely.  dst's sve_state
	 * will be allocated on demand later on if dst uses SVE.
	 * For consistency, also clear TIF_SVE here: this could be done
	 * later in copy_process(), but to avoid tripping up future
	 * maintainers it is best not to leave TIF_SVE and sve_state in
	 * an inconsistent state, even temporarily.
	 */
/*
 * IAMROOT, 2023.04.01:
 * - papago
 *   src의 sve_state(있는 경우)를 dst에서 분리하여 잘못 사용되거나 
 *   조기에 해제되지 않도록 합니다. dst가 SVE를 사용하는 경우 나중에 
 *   dst의 sve_state가 필요에 따라 할당됩니다.
 *   일관성을 위해 여기에서도 TIF_SVE를 지웁니다.
 *   이 작업은 나중에 copy_process()에서 수행할 수 있지만 향후 관리자를 
 *   방해하지 않으려면 일시적일지라도 TIF_SVE 및 sve_state를 일관성 없는 
 *   상태로 두지 않는 것이 가장 좋습니다.
 *
 * - copy후 sve에 대한것은 clear 해놓는다.
 */
	dst->thread.sve_state = NULL;
	clear_tsk_thread_flag(dst, TIF_SVE);

	/* clear any pending asynchronous tag fault raised by the parent */
	clear_tsk_thread_flag(dst, TIF_MTE_ASYNC_FAULT);

	return 0;
}

asmlinkage void ret_from_fork(void) asm("ret_from_fork");

/*
 * IAMROOT, 2023.04.14:
 * - thread_struct 초기화.
 */
int copy_thread(unsigned long clone_flags, unsigned long stack_start,
		unsigned long stk_sz, struct task_struct *p, unsigned long tls)
{
	struct pt_regs *childregs = task_pt_regs(p);

	memset(&p->thread.cpu_context, 0, sizeof(struct cpu_context));

	/*
	 * In case p was allocated the same task_struct pointer as some
	 * other recently-exited task, make sure p is disassociated from
	 * any cpu that may have run that now-exited task recently.
	 * Otherwise we could erroneously skip reloading the FPSIMD
	 * registers for p.
	 */
	/*
	 * IAMROOT. 2023.04.08:
	 * - google-translate
	 * p가 최근에 종료된 다른 작업과 동일한 task_struct 포인터를 할당받은 경우 최근에
	 * 종료된 해당 작업을 실행했을 수 있는 CPU에서 p가 연결 해제되었는지
	 * 확인합니다. 그렇지 않으면 p에 대한 FPSIMD 레지스터 다시 로드를 잘못 건너뛸 수
	 * 있습니다.
	 */
	fpsimd_flush_task_state(p);

	ptrauth_thread_init_kernel(p);

/*
 * IAMROOT, 2023.04.14:
 * - kernel thread도, io worker도 아닌 경우에 수행한다.
 *   current pt_regs을 copy에서 사용한다
 */
	if (likely(!(p->flags & (PF_KTHREAD | PF_IO_WORKER)))) {
		*childregs = *current_pt_regs();
/*
 * IAMROOT, 2023.04.08:
 * - 복사대상의 x0(반환값)을 굳이 child에서 사용할 필요가 없다.
 *   정돈하는 개념에서 0로 초기화해주는 거 같다.
 */
		childregs->regs[0] = 0;

		/*
		 * Read the current TLS pointer from tpidr_el0 as it may be
		 * out-of-sync with the saved value.
		 */
		*task_user_tls(p) = read_sysreg(tpidr_el0);

		if (stack_start) {
			if (is_compat_thread(task_thread_info(p)))
				childregs->compat_sp = stack_start;
			else
				childregs->sp = stack_start;
		}

		/*
		 * If a TLS pointer was passed to clone, use it for the new
		 * thread.
		 */
		if (clone_flags & CLONE_SETTLS)
			p->thread.uw.tp_value = tls;
	} else {
		/*
		 * A kthread has no context to ERET to, so ensure any buggy
		 * ERET is treated as an illegal exception return.
		 *
		 * When a user task is created from a kthread, childregs will
		 * be initialized by start_thread() or start_compat_thread().
		 */
/*
 * IAMROOT, 2023.04.14:
 * - papago
 *   kthread에는 ERET에 대한 컨텍스트가 없으므로 버그가 있는 ERET가 불법 예외 
 *   반환으로 처리되도록 합니다.
 *
 *   사용자 작업이 kthread에서 생성되면 childregs는 start_thread() 또는
 *   start_compat_thread()에 의해 초기화됩니다.
 */
		memset(childregs, 0, sizeof(struct pt_regs));
		childregs->pstate = PSR_MODE_EL1h | PSR_IL_BIT;

		p->thread.cpu_context.x19 = stack_start;
		p->thread.cpu_context.x20 = stk_sz;
	}
	p->thread.cpu_context.pc = (unsigned long)ret_from_fork;
	p->thread.cpu_context.sp = (unsigned long)childregs;
	/*
	 * For the benefit of the unwinder, set up childregs->stackframe
	 * as the final frame for the new task.
	 */
	p->thread.cpu_context.fp = (unsigned long)childregs->stackframe;

	ptrace_hw_copy_thread(p);

	return 0;
}

void tls_preserve_current_state(void)
{
	*task_user_tls(current) = read_sysreg(tpidr_el0);
}


/*
 * IAMROOT, 2023.01.28:
 * - PASS
 * - tls(thread local storage)
 */
static void tls_thread_switch(struct task_struct *next)
{
	tls_preserve_current_state();

	if (is_compat_thread(task_thread_info(next)))
		write_sysreg(next->thread.uw.tp_value, tpidrro_el0);
	else if (!arm64_kernel_unmapped_at_el0())
		write_sysreg(0, tpidrro_el0);

	write_sysreg(*task_user_tls(next), tpidr_el0);
}

/*
 * Force SSBS state on context-switch, since it may be lost after migrating
 * from a CPU which treats the bit as RES0 in a heterogeneous system.
 */
/*
 * IAMROOT, 2023.01.28:
 * - PASS
 * - SSBS(Speculative Store Bypass Safe)처리
 */
static void ssbs_thread_switch(struct task_struct *next)
{
	/*
	 * Nothing to do for kernel threads, but 'regs' may be junk
	 * (e.g. idle task) so check the flags and bail early.
	 */
	if (unlikely(next->flags & PF_KTHREAD))
		return;

	/*
	 * If all CPUs implement the SSBS extension, then we just need to
	 * context-switch the PSTATE field.
	 */
	if (cpus_have_const_cap(ARM64_SSBS))
		return;

	spectre_v4_enable_task_mitigation(next);
}

/*
 * We store our current task in sp_el0, which is clobbered by userspace. Keep a
 * shadow copy so that we can restore this upon entry from userspace.
 *
 * This is *only* for exception entry from EL0, and is not valid until we
 * __switch_to() a user task.
 */
/*
 * IAMROOT, 2022.11.08:
 * - papago
 *   current task를 sp_el0에 저장합니다. 사용자 공간에서 입력할 때 이를 복원할 수
 *   있도록 섀도 복사본을 유지합니다.
 *
 *   이것은 EL0의 예외 항목에 대한 *only*이며 사용자 작업을 __switch_to()할
 *   때까지 유효하지 않습니다.
 *
 * - context swtich 를 할때 next process가 저장된다.
 */
DEFINE_PER_CPU(struct task_struct *, __entry_task);

/*
 * IAMROOT, 2023.01.28:
 * - 동작할 task(next)를 __entry_task에 기록한다.
 */
static void entry_task_switch(struct task_struct *next)
{
	__this_cpu_write(__entry_task, next);
}

/*
 * ARM erratum 1418040 handling, affecting the 32bit view of CNTVCT.
 * Assuming the virtual counter is enabled at the beginning of times:
 *
 * - disable access when switching from a 64bit task to a 32bit task
 * - enable access when switching from a 32bit task to a 64bit task
 */
/*
 * IAMROOT, 2023.01.28:
 * - papago
 *   ARM 정오표 1418040 처리, CNTVCT의 32비트 보기에 영향을 미침.
 *   시작 시간에 가상 카운터가 활성화되었다고 가정합니다.
 *
 *   - 64비트 태스크에서 32비트 태스크로 전환 시 접근 금지
 *   - 32비트 태스크에서 64비트 태스크로 전환할 때 액세스를 활성화합니다. 
 *
 * - prev와 next가 같은경우
 *   return.
 * - 32bit -> 64bit
 *   ARCH_TIMER_USR_VCT_ACCESS_EN 추가
 * - 64bit -> 32bit
 *   ARCH_TIMER_USR_VCT_ACCESS_EN 제거
 */
static void erratum_1418040_thread_switch(struct task_struct *prev,
					  struct task_struct *next)
{
	bool prev32, next32;
	u64 val;

	if (!IS_ENABLED(CONFIG_ARM64_ERRATUM_1418040))
		return;

	prev32 = is_compat_thread(task_thread_info(prev));
	next32 = is_compat_thread(task_thread_info(next));

	if (prev32 == next32 || !this_cpu_has_cap(ARM64_WORKAROUND_1418040))
		return;

	val = read_sysreg(cntkctl_el1);

	if (!next32)
		val |= ARCH_TIMER_USR_VCT_ACCESS_EN;
	else
		val &= ~ARCH_TIMER_USR_VCT_ACCESS_EN;

	write_sysreg(val, cntkctl_el1);
}

/*
 * __switch_to() checks current->thread.sctlr_user as an optimisation. Therefore
 * this function must be called with preemption disabled and the update to
 * sctlr_user must be made in the same preemption disabled block so that
 * __switch_to() does not see the variable update before the SCTLR_EL1 one.
 */
/*
 * IAMROOT, 2023.01.28:
 * - papago
 *   __switch_to()는 current->thread.sctlr_user를 최적화로 확인합니다. 따라서 
 *   이 함수는 선점을 비활성화한 상태에서 호출해야 하며 sctlr_user에 대한 
 *   업데이트는 동일한 선점 비활성화 블록에서 수행해야 __switch_to()가 
 *   SCTLR_EL1 이전에 변수 업데이트를 볼 수 없습니다.
 *
 *  - sctlr_el1을 수정하면 isb를 무조건해줘야한다.
 */
void update_sctlr_el1(u64 sctlr)
{
	/*
	 * EnIA must not be cleared while in the kernel as this is necessary for
	 * in-kernel PAC. It will be cleared on kernel exit if needed.
	 */
	sysreg_clear_set(sctlr_el1, SCTLR_USER_MASK & ~SCTLR_ELx_ENIA, sctlr);

	/* ISB required for the kernel uaccess routines when setting TCF0. */
	isb();
}

/*
 * Thread switching.
 */
/*
 * IAMROOT, 2023.01.28:
 * - 1. __entry_task에 next기록한다.
 *   2. dsb(ish)수행한다.
 *   3. sctlr_el1이 변경됬으면 갱신한다.
 *   4. cpu_switch_to 실행.
 */
__notrace_funcgraph struct task_struct *__switch_to(struct task_struct *prev,
				struct task_struct *next)
{
	struct task_struct *last;

	fpsimd_thread_switch(next);
	tls_thread_switch(next);
	hw_breakpoint_thread_switch(next);
	contextidr_thread_switch(next);
	entry_task_switch(next);
	ssbs_thread_switch(next);
	erratum_1418040_thread_switch(prev, next);
	ptrauth_thread_switch_user(next);

	/*
	 * Complete any pending TLB or cache maintenance on this CPU in case
	 * the thread migrates to a different CPU.
	 * This full barrier is also required by the membarrier system
	 * call.
	 */
/*
 * IAMROOT, 2023.01.28:
 * - papago
 *   스레드가 다른 CPU로 마이그레이션되는 경우 이 CPU에서 보류 중인 TLB 또는 
 *   캐시 유지 관리를 완료하십시오. 
 *   이 전체 장벽은 membarrier 시스템 호출에도 필요합니다.
 * - tlb cache등을 flush 한 경우에 완료될때까지 기다린다.
 */
	dsb(ish);

	/*
	 * MTE thread switching must happen after the DSB above to ensure that
	 * any asynchronous tag check faults have been logged in the TFSR*_EL1
	 * registers.
	 */
	mte_thread_switch(next);
	/* avoid expensive SCTLR_EL1 accesses if no change */
/*
 * IAMROOT, 2023.01.28:
 * - sctlr_user가 다르다면 sctlr_el1을 변경되는 sctlr_user로 바꾼다.
 */
	if (prev->thread.sctlr_user != next->thread.sctlr_user)
		update_sctlr_el1(next->thread.sctlr_user);

	/* the actual thread switch */
	last = cpu_switch_to(prev, next);

	return last;
}

unsigned long get_wchan(struct task_struct *p)
{
	struct stackframe frame;
	unsigned long stack_page, ret = 0;
	int count = 0;
	if (!p || p == current || task_is_running(p))
		return 0;

	stack_page = (unsigned long)try_get_task_stack(p);
	if (!stack_page)
		return 0;

	start_backtrace(&frame, thread_saved_fp(p), thread_saved_pc(p));

	do {
		if (unwind_frame(p, &frame))
			goto out;
		if (!in_sched_functions(frame.pc)) {
			ret = frame.pc;
			goto out;
		}
	} while (count++ < 16);

out:
	put_task_stack(p);
	return ret;
}

unsigned long arch_align_stack(unsigned long sp)
{
	if (!(current->personality & ADDR_NO_RANDOMIZE) && randomize_va_space)
		sp -= get_random_int() & ~PAGE_MASK;
	return sp & ~0xf;
}

#ifdef CONFIG_COMPAT
int compat_elf_check_arch(const struct elf32_hdr *hdr)
{
	if (!system_supports_32bit_el0())
		return false;

	if ((hdr)->e_machine != EM_ARM)
		return false;

	if (!((hdr)->e_flags & EF_ARM_EABI_MASK))
		return false;

	/*
	 * Prevent execve() of a 32-bit program from a deadline task
	 * if the restricted affinity mask would be inadmissible on an
	 * asymmetric system.
	 */
	return !static_branch_unlikely(&arm64_mismatched_32bit_el0) ||
	       !dl_task_check_affinity(current, system_32bit_el0_cpumask());
}
#endif

/*
 * Called from setup_new_exec() after (COMPAT_)SET_PERSONALITY.
 */
void arch_setup_new_exec(void)
{
	unsigned long mmflags = 0;

	if (is_compat_task()) {
		mmflags = MMCF_AARCH32;

		/*
		 * Restrict the CPU affinity mask for a 32-bit task so that
		 * it contains only 32-bit-capable CPUs.
		 *
		 * From the perspective of the task, this looks similar to
		 * what would happen if the 64-bit-only CPUs were hot-unplugged
		 * at the point of execve(), although we try a bit harder to
		 * honour the cpuset hierarchy.
		 */
		if (static_branch_unlikely(&arm64_mismatched_32bit_el0))
			force_compatible_cpus_allowed_ptr(current);
	} else if (static_branch_unlikely(&arm64_mismatched_32bit_el0)) {
		relax_compatible_cpus_allowed_ptr(current);
	}

	current->mm->context.flags = mmflags;
	ptrauth_thread_init_user();
	mte_thread_init_user();

	if (task_spec_ssb_noexec(current)) {
		arch_prctl_spec_ctrl_set(current, PR_SPEC_STORE_BYPASS,
					 PR_SPEC_ENABLE);
	}
}

#ifdef CONFIG_ARM64_TAGGED_ADDR_ABI
/*
 * Control the relaxed ABI allowing tagged user addresses into the kernel.
 */
static unsigned int tagged_addr_disabled;

long set_tagged_addr_ctrl(struct task_struct *task, unsigned long arg)
{
	unsigned long valid_mask = PR_TAGGED_ADDR_ENABLE;
	struct thread_info *ti = task_thread_info(task);

	if (is_compat_thread(ti))
		return -EINVAL;

	if (system_supports_mte())
		valid_mask |= PR_MTE_TCF_MASK | PR_MTE_TAG_MASK;

	if (arg & ~valid_mask)
		return -EINVAL;

	/*
	 * Do not allow the enabling of the tagged address ABI if globally
	 * disabled via sysctl abi.tagged_addr_disabled.
	 */
	if (arg & PR_TAGGED_ADDR_ENABLE && tagged_addr_disabled)
		return -EINVAL;

	if (set_mte_ctrl(task, arg) != 0)
		return -EINVAL;

	update_ti_thread_flag(ti, TIF_TAGGED_ADDR, arg & PR_TAGGED_ADDR_ENABLE);

	return 0;
}

long get_tagged_addr_ctrl(struct task_struct *task)
{
	long ret = 0;
	struct thread_info *ti = task_thread_info(task);

	if (is_compat_thread(ti))
		return -EINVAL;

	if (test_ti_thread_flag(ti, TIF_TAGGED_ADDR))
		ret = PR_TAGGED_ADDR_ENABLE;

	ret |= get_mte_ctrl(task);

	return ret;
}

/*
 * Global sysctl to disable the tagged user addresses support. This control
 * only prevents the tagged address ABI enabling via prctl() and does not
 * disable it for tasks that already opted in to the relaxed ABI.
 */

static struct ctl_table tagged_addr_sysctl_table[] = {
	{
		.procname	= "tagged_addr_disabled",
		.mode		= 0644,
		.data		= &tagged_addr_disabled,
		.maxlen		= sizeof(int),
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_ONE,
	},
	{ }
};

static int __init tagged_addr_init(void)
{
	if (!register_sysctl("abi", tagged_addr_sysctl_table))
		return -EINVAL;
	return 0;
}

core_initcall(tagged_addr_init);
#endif	/* CONFIG_ARM64_TAGGED_ADDR_ABI */

#ifdef CONFIG_BINFMT_ELF
int arch_elf_adjust_prot(int prot, const struct arch_elf_state *state,
			 bool has_interp, bool is_interp)
{
	/*
	 * For dynamically linked executables the interpreter is
	 * responsible for setting PROT_BTI on everything except
	 * itself.
	 */
	if (is_interp != has_interp)
		return prot;

	if (!(state->flags & ARM64_ELF_BTI))
		return prot;

	if (prot & PROT_EXEC)
		prot |= PROT_BTI;

	return prot;
}
#endif
