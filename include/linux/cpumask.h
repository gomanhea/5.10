/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_CPUMASK_H
#define __LINUX_CPUMASK_H

/*
 * Cpumasks provide a bitmap suitable for representing the
 * set of CPU's in a system, one bit position per CPU number.  In general,
 * only nr_cpu_ids (<= NR_CPUS) bits are valid.
 */
#include <linux/kernel.h>
#include <linux/threads.h>
#include <linux/bitmap.h>
#include <linux/atomic.h>
#include <linux/bug.h>

/* IAMROOT, 2021.09.30:
 * CONFIG_NR_CPUS에 따라 compile 시점에 cpumask 크기가 정해짐이 보인다.
 */
/* Don't assign or return these: may not be this big! */
typedef struct cpumask { DECLARE_BITMAP(bits, NR_CPUS); } cpumask_t;

/**
 * cpumask_bits - get the bits in a cpumask
 * @maskp: the struct cpumask *
 *
 * You should only assume nr_cpu_ids bits of this mask are valid.  This is
 * a macro so it's const-correct.
 */
/*
 * IAMROOT, 2023.04.13:
 * - papago
 *   cpumask_bits - cpumask의 비트 가져오기
 *   @maskp: 구조체 cpumask
 *
 *   이 마스크의 nr_cpu_ids 비트만 유효하다고 가정해야 합니다. 이것은
 *   매크로이므로 const가 정확합니다.
 * - return cpumask->bits
 */
#define cpumask_bits(maskp) ((maskp)->bits)

/**
 * cpumask_pr_args - printf args to output a cpumask
 * @maskp: cpumask to be printed
 *
 * Can be used to provide arguments for '%*pb[l]' when printing a cpumask.
 */
#define cpumask_pr_args(maskp)		nr_cpu_ids, cpumask_bits(maskp)

/* IAMROOT, 2021.09.30:
 * NR_CPUS 는 compile time에 정해지고, nr_cpus_ids는 runtime에 정해진다.
 * kernel/smp.c 에 정의. possible cpu 수
 */
#if NR_CPUS == 1
#define nr_cpu_ids		1U
#else
extern unsigned int nr_cpu_ids;
#endif

/* IAMROOT, 2021.09.30:
 * cpumask를 stack을 쓰지 않고 alloc하여 사용한다는 의미이다. cpu가 많아지면
 * copy보다는 pointer로 넘겨주는게 효율이 좋기때문이다.
 * (cpumask_var_t 참고)
 */
#ifdef CONFIG_CPUMASK_OFFSTACK
/* Assuming NR_CPUS is huge, a runtime limit is more efficient.  Also,
 * not all bits may be allocated. */
#define nr_cpumask_bits	nr_cpu_ids
#else
#define nr_cpumask_bits	((unsigned int)NR_CPUS)
#endif

/*
 * The following particular system cpumasks and operations manage
 * possible, present, active and online cpus.
 *
 *     cpu_possible_mask- has bit 'cpu' set iff cpu is populatable
 *     cpu_present_mask - has bit 'cpu' set iff cpu is populated
 *     cpu_online_mask  - has bit 'cpu' set iff cpu available to scheduler
 *     cpu_active_mask  - has bit 'cpu' set iff cpu available to migration
 *
 *  If !CONFIG_HOTPLUG_CPU, present == possible, and active == online.
 *
 *  The cpu_possible_mask is fixed at boot time, as the set of CPU id's
 *  that it is possible might ever be plugged in at anytime during the
 *  life of that system boot.  The cpu_present_mask is dynamic(*),
 *  representing which CPUs are currently plugged in.  And
 *  cpu_online_mask is the dynamic subset of cpu_present_mask,
 *  indicating those CPUs available for scheduling.
 *
 *  If HOTPLUG is enabled, then cpu_possible_mask is forced to have
 *  all NR_CPUS bits set, otherwise it is just the set of CPUs that
 *  ACPI reports present at boot.
 *
 *  If HOTPLUG is enabled, then cpu_present_mask varies dynamically,
 *  depending on what ACPI reports as currently plugged in, otherwise
 *  cpu_present_mask is just a copy of cpu_possible_mask.
 *
 *  (*) Well, cpu_present_mask is dynamic in the hotplug case.  If not
 *      hotplug, it's a copy of cpu_possible_mask, hence fixed at boot.
 *
 * Subtleties:
 * 1) UP arch's (NR_CPUS == 1, CONFIG_SMP not defined) hardcode
 *    assumption that their single CPU is online.  The UP
 *    cpu_{online,possible,present}_masks are placebos.  Changing them
 *    will have no useful affect on the following num_*_cpus()
 *    and cpu_*() macros in the UP case.  This ugliness is a UP
 *    optimization - don't waste any instructions or memory references
 *    asking if you're online or how many CPUs there are if there is
 *    only one CPU.
 */

/*
 * IAMROOT, 2023.04.13:
 * - set_cpu_online() 주석 참고
 */
extern struct cpumask __cpu_possible_mask;
extern struct cpumask __cpu_online_mask;
extern struct cpumask __cpu_present_mask;
extern struct cpumask __cpu_active_mask;
extern struct cpumask __cpu_dying_mask;
#define cpu_possible_mask ((const struct cpumask *)&__cpu_possible_mask)
#define cpu_online_mask   ((const struct cpumask *)&__cpu_online_mask)
#define cpu_present_mask  ((const struct cpumask *)&__cpu_present_mask)
#define cpu_active_mask   ((const struct cpumask *)&__cpu_active_mask)
#define cpu_dying_mask    ((const struct cpumask *)&__cpu_dying_mask)

extern atomic_t __num_online_cpus;

extern cpumask_t cpus_booted_once_mask;

static inline void cpu_max_bits_warn(unsigned int cpu, unsigned int bits)
{
#ifdef CONFIG_DEBUG_PER_CPU_MAPS
	WARN_ON_ONCE(cpu >= bits);
#endif /* CONFIG_DEBUG_PER_CPU_MAPS */
}

/* verify cpu argument to cpumask_* operators */
/*
 * IAMROOT, 2023.04.13:
 * - @cpu가 nr_cpumask_bits를 넘는지 검사하는 루틴이 있다.
 *   return cpu.
 */
static inline unsigned int cpumask_check(unsigned int cpu)
{
	cpu_max_bits_warn(cpu, nr_cpumask_bits);
	return cpu;
}

#if NR_CPUS == 1
/* Uniprocessor.  Assume all masks are "1". */
static inline unsigned int cpumask_first(const struct cpumask *srcp)
{
	return 0;
}

static inline unsigned int cpumask_last(const struct cpumask *srcp)
{
	return 0;
}

/* Valid inputs for n are -1 and 0. */
static inline unsigned int cpumask_next(int n, const struct cpumask *srcp)
{
	return n+1;
}

static inline unsigned int cpumask_next_zero(int n, const struct cpumask *srcp)
{
	return n+1;
}

static inline unsigned int cpumask_next_and(int n,
					    const struct cpumask *srcp,
					    const struct cpumask *andp)
{
	return n+1;
}

static inline unsigned int cpumask_next_wrap(int n, const struct cpumask *mask,
					     int start, bool wrap)
{
	/* cpu0 unless stop condition, wrap and at cpu0, then nr_cpumask_bits */
	return (wrap && n == 0);
}

/* cpu must be a valid cpu, ie 0, so there's no other choice. */
static inline unsigned int cpumask_any_but(const struct cpumask *mask,
					   unsigned int cpu)
{
	return 1;
}

static inline unsigned int cpumask_local_spread(unsigned int i, int node)
{
	return 0;
}

static inline int cpumask_any_and_distribute(const struct cpumask *src1p,
					     const struct cpumask *src2p) {
	return cpumask_next_and(-1, src1p, src2p);
}

static inline int cpumask_any_distribute(const struct cpumask *srcp)
{
	return cpumask_first(srcp);
}

#define for_each_cpu(cpu, mask)			\
	for ((cpu) = 0; (cpu) < 1; (cpu)++, (void)mask)
#define for_each_cpu_not(cpu, mask)		\
	for ((cpu) = 0; (cpu) < 1; (cpu)++, (void)mask)
#define for_each_cpu_wrap(cpu, mask, start)	\
	for ((cpu) = 0; (cpu) < 1; (cpu)++, (void)mask, (void)(start))
#define for_each_cpu_and(cpu, mask1, mask2)	\
	for ((cpu) = 0; (cpu) < 1; (cpu)++, (void)mask1, (void)mask2)
#else
/**
 * cpumask_first - get the first cpu in a cpumask
 * @srcp: the cpumask pointer
 *
 * Returns >= nr_cpu_ids if no cpus set.
 */
static inline unsigned int cpumask_first(const struct cpumask *srcp)
{
	return find_first_bit(cpumask_bits(srcp), nr_cpumask_bits);
}

/**
 * cpumask_last - get the last CPU in a cpumask
 * @srcp:	- the cpumask pointer
 *
 * Returns	>= nr_cpumask_bits if no CPUs set.
 */
static inline unsigned int cpumask_last(const struct cpumask *srcp)
{
	return find_last_bit(cpumask_bits(srcp), nr_cpumask_bits);
}

unsigned int __pure cpumask_next(int n, const struct cpumask *srcp);

/**
 * cpumask_next_zero - get the next unset cpu in a cpumask
 * @n: the cpu prior to the place to search (ie. return will be > @n)
 * @srcp: the cpumask pointer
 *
 * Returns >= nr_cpu_ids if no further cpus unset.
 */
static inline unsigned int cpumask_next_zero(int n, const struct cpumask *srcp)
{
	/* -1 is a legal arg here. */
	if (n != -1)
		cpumask_check(n);
	return find_next_zero_bit(cpumask_bits(srcp), nr_cpumask_bits, n+1);
}

int __pure cpumask_next_and(int n, const struct cpumask *, const struct cpumask *);
int __pure cpumask_any_but(const struct cpumask *mask, unsigned int cpu);
unsigned int cpumask_local_spread(unsigned int i, int node);
int cpumask_any_and_distribute(const struct cpumask *src1p,
			       const struct cpumask *src2p);
int cpumask_any_distribute(const struct cpumask *srcp);

/**
 * for_each_cpu - iterate over every cpu in a mask
 * @cpu: the (optionally unsigned) integer iterator
 * @mask: the cpumask pointer
 *
 * After the loop, cpu is >= nr_cpu_ids.
 */
#define for_each_cpu(cpu, mask)				\
	for ((cpu) = -1;				\
		(cpu) = cpumask_next((cpu), (mask)),	\
		(cpu) < nr_cpu_ids;)

/**
 * for_each_cpu_not - iterate over every cpu in a complemented mask
 * @cpu: the (optionally unsigned) integer iterator
 * @mask: the cpumask pointer
 *
 * After the loop, cpu is >= nr_cpu_ids.
 */
#define for_each_cpu_not(cpu, mask)				\
	for ((cpu) = -1;					\
		(cpu) = cpumask_next_zero((cpu), (mask)),	\
		(cpu) < nr_cpu_ids;)

extern int cpumask_next_wrap(int n, const struct cpumask *mask, int start, bool wrap);

/**
 * for_each_cpu_wrap - iterate over every cpu in a mask, starting at a specified location
 * @cpu: the (optionally unsigned) integer iterator
 * @mask: the cpumask pointer
 * @start: the start location
 *
 * The implementation does not assume any bit in @mask is set (including @start).
 *
 * After the loop, cpu is >= nr_cpu_ids.
 */
/*
 * IAMROOT, 2023.04.22:
 * - wrap이 붙은 함수 특징 : 지정된 cpu부터 그 전까지 한바퀴를 iterate한다.
 */
#define for_each_cpu_wrap(cpu, mask, start)					\
	for ((cpu) = cpumask_next_wrap((start)-1, (mask), (start), false);	\
	     (cpu) < nr_cpumask_bits;						\
	     (cpu) = cpumask_next_wrap((cpu), (mask), (start), true))

/**
 * for_each_cpu_and - iterate over every cpu in both masks
 * @cpu: the (optionally unsigned) integer iterator
 * @mask1: the first cpumask pointer
 * @mask2: the second cpumask pointer
 *
 * This saves a temporary CPU mask in many places.  It is equivalent to:
 *	struct cpumask tmp;
 *	cpumask_and(&tmp, &mask1, &mask2);
 *	for_each_cpu(cpu, &tmp)
 *		...
 *
 * After the loop, cpu is >= nr_cpu_ids.
 */
/*
 * IAMROOT, 2023.04.15:
 * - mask1 & mask2를 한후 iterate
 */
#define for_each_cpu_and(cpu, mask1, mask2)				\
	for ((cpu) = -1;						\
		(cpu) = cpumask_next_and((cpu), (mask1), (mask2)),	\
		(cpu) < nr_cpu_ids;)
#endif /* SMP */

#define CPU_BITS_NONE						\
{								\
	[0 ... BITS_TO_LONGS(NR_CPUS)-1] = 0UL			\
}

#define CPU_BITS_CPU0						\
{								\
	[0] =  1UL						\
}

/**
 * cpumask_set_cpu - set a cpu in a cpumask
 * @cpu: cpu number (< nr_cpu_ids)
 * @dstp: the cpumask pointer
 */
/*
 * IAMROOT, 2023.01.03:
 * - @dstp에 @cpu bit를 추가한다.
 */
static inline void cpumask_set_cpu(unsigned int cpu, struct cpumask *dstp)
{
	set_bit(cpumask_check(cpu), cpumask_bits(dstp));
}

static inline void __cpumask_set_cpu(unsigned int cpu, struct cpumask *dstp)
{
	__set_bit(cpumask_check(cpu), cpumask_bits(dstp));
}


/**
 * cpumask_clear_cpu - clear a cpu in a cpumask
 * @cpu: cpu number (< nr_cpu_ids)
 * @dstp: the cpumask pointer
 */
static inline void cpumask_clear_cpu(int cpu, struct cpumask *dstp)
{
	clear_bit(cpumask_check(cpu), cpumask_bits(dstp));
}

static inline void __cpumask_clear_cpu(int cpu, struct cpumask *dstp)
{
	__clear_bit(cpumask_check(cpu), cpumask_bits(dstp));
}

/**
 * cpumask_test_cpu - test for a cpu in a cpumask
 * @cpu: cpu number (< nr_cpu_ids)
 * @cpumask: the cpumask pointer
 *
 * Returns 1 if @cpu is set in @cpumask, else returns 0
 */
/*
 * IAMROOT, 2023.04.13:
 * - papago
 *   cpumask_test_cpu - cpumask에서 cpu를 테스트합니다.
 *
 *   @cpu: CPU 번호(< nr_cpu_ids)
 *   @cpumask: cpumask 포인터.
 *
 *   @cpumask에 @cpu가 설정되어 있으면 1을 반환하고 그렇지 않으면 0을 
 *   반환합니다.
 * - @cpumask에 cpu가 있으면 return 1. 아니면 return 0.
 */
static inline int cpumask_test_cpu(int cpu, const struct cpumask *cpumask)
{
	return test_bit(cpumask_check(cpu), cpumask_bits((cpumask)));
}

/**
 * cpumask_test_and_set_cpu - atomically test and set a cpu in a cpumask
 * @cpu: cpu number (< nr_cpu_ids)
 * @cpumask: the cpumask pointer
 *
 * Returns 1 if @cpu is set in old bitmap of @cpumask, else returns 0
 *
 * test_and_set_bit wrapper for cpumasks.
 */
static inline int cpumask_test_and_set_cpu(int cpu, struct cpumask *cpumask)
{
	return test_and_set_bit(cpumask_check(cpu), cpumask_bits(cpumask));
}

/**
 * cpumask_test_and_clear_cpu - atomically test and clear a cpu in a cpumask
 * @cpu: cpu number (< nr_cpu_ids)
 * @cpumask: the cpumask pointer
 *
 * Returns 1 if @cpu is set in old bitmap of @cpumask, else returns 0
 *
 * test_and_clear_bit wrapper for cpumasks.
 */
static inline int cpumask_test_and_clear_cpu(int cpu, struct cpumask *cpumask)
{
	return test_and_clear_bit(cpumask_check(cpu), cpumask_bits(cpumask));
}

/**
 * cpumask_setall - set all cpus (< nr_cpu_ids) in a cpumask
 * @dstp: the cpumask pointer
 */
static inline void cpumask_setall(struct cpumask *dstp)
{
	bitmap_fill(cpumask_bits(dstp), nr_cpumask_bits);
}

/**
 * cpumask_clear - clear all cpus (< nr_cpu_ids) in a cpumask
 * @dstp: the cpumask pointer
 */
static inline void cpumask_clear(struct cpumask *dstp)
{
	bitmap_zero(cpumask_bits(dstp), nr_cpumask_bits);
}

/**
 * cpumask_and - *dstp = *src1p & *src2p
 * @dstp: the cpumask result
 * @src1p: the first input
 * @src2p: the second input
 *
 * If *@dstp is empty, returns 0, else returns 1
 */
static inline int cpumask_and(struct cpumask *dstp,
			       const struct cpumask *src1p,
			       const struct cpumask *src2p)
{
	return bitmap_and(cpumask_bits(dstp), cpumask_bits(src1p),
				       cpumask_bits(src2p), nr_cpumask_bits);
}

/**
 * cpumask_or - *dstp = *src1p | *src2p
 * @dstp: the cpumask result
 * @src1p: the first input
 * @src2p: the second input
 */
static inline void cpumask_or(struct cpumask *dstp, const struct cpumask *src1p,
			      const struct cpumask *src2p)
{
	bitmap_or(cpumask_bits(dstp), cpumask_bits(src1p),
				      cpumask_bits(src2p), nr_cpumask_bits);
}

/**
 * cpumask_xor - *dstp = *src1p ^ *src2p
 * @dstp: the cpumask result
 * @src1p: the first input
 * @src2p: the second input
 */
static inline void cpumask_xor(struct cpumask *dstp,
			       const struct cpumask *src1p,
			       const struct cpumask *src2p)
{
	bitmap_xor(cpumask_bits(dstp), cpumask_bits(src1p),
				       cpumask_bits(src2p), nr_cpumask_bits);
}

/**
 * cpumask_andnot - *dstp = *src1p & ~*src2p
 * @dstp: the cpumask result
 * @src1p: the first input
 * @src2p: the second input
 *
 * If *@dstp is empty, returns 0, else returns 1
 */
/*
 * IAMROOT, 2023.04.15:
 * - @src2p를 먼저 not을 한후 @src1p와 and한다.
 *   *src1p & ~*src2p
 */
static inline int cpumask_andnot(struct cpumask *dstp,
				  const struct cpumask *src1p,
				  const struct cpumask *src2p)
{
	return bitmap_andnot(cpumask_bits(dstp), cpumask_bits(src1p),
					  cpumask_bits(src2p), nr_cpumask_bits);
}

/**
 * cpumask_complement - *dstp = ~*srcp
 * @dstp: the cpumask result
 * @srcp: the input to invert
 */
static inline void cpumask_complement(struct cpumask *dstp,
				      const struct cpumask *srcp)
{
	bitmap_complement(cpumask_bits(dstp), cpumask_bits(srcp),
					      nr_cpumask_bits);
}

/**
 * cpumask_equal - *src1p == *src2p
 * @src1p: the first input
 * @src2p: the second input
 */

/*
 * IAMROOT, 2022.12.30:
 * - return true : @*src1p == @*src2p
 */
static inline bool cpumask_equal(const struct cpumask *src1p,
				const struct cpumask *src2p)
{
	return bitmap_equal(cpumask_bits(src1p), cpumask_bits(src2p),
						 nr_cpumask_bits);
}

/**
 * cpumask_or_equal - *src1p | *src2p == *src3p
 * @src1p: the first input
 * @src2p: the second input
 * @src3p: the third input
 */
static inline bool cpumask_or_equal(const struct cpumask *src1p,
				    const struct cpumask *src2p,
				    const struct cpumask *src3p)
{
	return bitmap_or_equal(cpumask_bits(src1p), cpumask_bits(src2p),
			       cpumask_bits(src3p), nr_cpumask_bits);
}

/**
 * cpumask_intersects - (*src1p & *src2p) != 0
 * @src1p: the first input
 * @src2p: the second input
 */
static inline bool cpumask_intersects(const struct cpumask *src1p,
				     const struct cpumask *src2p)
{
	return bitmap_intersects(cpumask_bits(src1p), cpumask_bits(src2p),
						      nr_cpumask_bits);
}

/**
 * cpumask_subset - (*src1p & ~*src2p) == 0
 * @src1p: the first input
 * @src2p: the second input
 *
 * Returns 1 if *@src1p is a subset of *@src2p, else returns 0
 */
/*
 * IAMROOT. 2022.12.10:
 * - google-translate
 *   cpumask_subset - (*src1p & ~*src2p) == 0
 *   @src1p: 첫 번째 입력
 *   @src2p: 두 번째 입력
 *   @src1p가 *@src2p의 하위 집합이면 1을 반환하고 그렇지 않으면 0을 반환합니다.
 *
 * - src1p=0b0011, scr2p=0b1111 return 1
 *   src1p=0b0011, scr2p=0b0011 return 1
 *   src1p=0b0111, scr2p=0b0011 return 0
 */
static inline int cpumask_subset(const struct cpumask *src1p,
				 const struct cpumask *src2p)
{
	return bitmap_subset(cpumask_bits(src1p), cpumask_bits(src2p),
						  nr_cpumask_bits);
}

/**
 * cpumask_empty - *srcp == 0
 * @srcp: the cpumask to that all cpus < nr_cpu_ids are clear.
 */
static inline bool cpumask_empty(const struct cpumask *srcp)
{
	return bitmap_empty(cpumask_bits(srcp), nr_cpumask_bits);
}

/**
 * cpumask_full - *srcp == 0xFFFFFFFF...
 * @srcp: the cpumask to that all cpus < nr_cpu_ids are set.
 */
static inline bool cpumask_full(const struct cpumask *srcp)
{
	return bitmap_full(cpumask_bits(srcp), nr_cpumask_bits);
}

/**
 * cpumask_weight - Count of bits in *srcp
 * @srcp: the cpumask to count bits (< nr_cpu_ids) in.
 */
static inline unsigned int cpumask_weight(const struct cpumask *srcp)
{
	return bitmap_weight(cpumask_bits(srcp), nr_cpumask_bits);
}

/**
 * cpumask_shift_right - *dstp = *srcp >> n
 * @dstp: the cpumask result
 * @srcp: the input to shift
 * @n: the number of bits to shift by
 */
static inline void cpumask_shift_right(struct cpumask *dstp,
				       const struct cpumask *srcp, int n)
{
	bitmap_shift_right(cpumask_bits(dstp), cpumask_bits(srcp), n,
					       nr_cpumask_bits);
}

/**
 * cpumask_shift_left - *dstp = *srcp << n
 * @dstp: the cpumask result
 * @srcp: the input to shift
 * @n: the number of bits to shift by
 */
static inline void cpumask_shift_left(struct cpumask *dstp,
				      const struct cpumask *srcp, int n)
{
	bitmap_shift_left(cpumask_bits(dstp), cpumask_bits(srcp), n,
					      nr_cpumask_bits);
}

/**
 * cpumask_copy - *dstp = *srcp
 * @dstp: the result
 * @srcp: the input cpumask
 */
static inline void cpumask_copy(struct cpumask *dstp,
				const struct cpumask *srcp)
{
	bitmap_copy(cpumask_bits(dstp), cpumask_bits(srcp), nr_cpumask_bits);
}

/**
 * cpumask_any - pick a "random" cpu from *srcp
 * @srcp: the input cpumask
 *
 * Returns >= nr_cpu_ids if no cpus set.
 */
/*
 * IAMROOT, 2023.04.13:
 * - 첫번째꺼 그냥 선택
 */
#define cpumask_any(srcp) cpumask_first(srcp)

/**
 * cpumask_first_and - return the first cpu from *srcp1 & *srcp2
 * @src1p: the first input
 * @src2p: the second input
 *
 * Returns >= nr_cpu_ids if no cpus set in both.  See also cpumask_next_and().
 */
#define cpumask_first_and(src1p, src2p) cpumask_next_and(-1, (src1p), (src2p))

/**
 * cpumask_any_and - pick a "random" cpu from *mask1 & *mask2
 * @mask1: the first input cpumask
 * @mask2: the second input cpumask
 *
 * Returns >= nr_cpu_ids if no cpus set.
 */
#define cpumask_any_and(mask1, mask2) cpumask_first_and((mask1), (mask2))

/**
 * cpumask_of - the cpumask containing just a given cpu
 * @cpu: the cpu (<= nr_cpu_ids)
 */

/*
 * IAMROOT, 2022.12.30:
 * - @cpu에 대한 cpumask를 얻어온다.
 */
#define cpumask_of(cpu) (get_cpu_mask(cpu))

/**
 * cpumask_parse_user - extract a cpumask from a user string
 * @buf: the buffer to extract from
 * @len: the length of the buffer
 * @dstp: the cpumask to set.
 *
 * Returns -errno, or 0 for success.
 */
static inline int cpumask_parse_user(const char __user *buf, int len,
				     struct cpumask *dstp)
{
	return bitmap_parse_user(buf, len, cpumask_bits(dstp), nr_cpumask_bits);
}

/**
 * cpumask_parselist_user - extract a cpumask from a user string
 * @buf: the buffer to extract from
 * @len: the length of the buffer
 * @dstp: the cpumask to set.
 *
 * Returns -errno, or 0 for success.
 */
static inline int cpumask_parselist_user(const char __user *buf, int len,
				     struct cpumask *dstp)
{
	return bitmap_parselist_user(buf, len, cpumask_bits(dstp),
				     nr_cpumask_bits);
}

/**
 * cpumask_parse - extract a cpumask from a string
 * @buf: the buffer to extract from
 * @dstp: the cpumask to set.
 *
 * Returns -errno, or 0 for success.
 */
static inline int cpumask_parse(const char *buf, struct cpumask *dstp)
{
	return bitmap_parse(buf, UINT_MAX, cpumask_bits(dstp), nr_cpumask_bits);
}

/**
 * cpulist_parse - extract a cpumask from a user string of ranges
 * @buf: the buffer to extract from
 * @dstp: the cpumask to set.
 *
 * Returns -errno, or 0 for success.
 */
static inline int cpulist_parse(const char *buf, struct cpumask *dstp)
{
	return bitmap_parselist(buf, cpumask_bits(dstp), nr_cpumask_bits);
}

/**
 * cpumask_size - size to allocate for a 'struct cpumask' in bytes
 */
static inline unsigned int cpumask_size(void)
{
	return BITS_TO_LONGS(nr_cpumask_bits) * sizeof(long);
}

/*
 * cpumask_var_t: struct cpumask for stack usage.
 *
 * Oh, the wicked games we play!  In order to make kernel coding a
 * little more difficult, we typedef cpumask_var_t to an array or a
 * pointer: doing &mask on an array is a noop, so it still works.
 *
 * ie.
 *	cpumask_var_t tmpmask;
 *	if (!alloc_cpumask_var(&tmpmask, GFP_KERNEL))
 *		return -ENOMEM;
 *
 *	  ... use 'tmpmask' like a normal struct cpumask * ...
 *
 *	free_cpumask_var(tmpmask);
 *
 *
 * However, one notable exception is there. alloc_cpumask_var() allocates
 * only nr_cpumask_bits bits (in the other hand, real cpumask_t always has
 * NR_CPUS bits). Therefore you don't have to dereference cpumask_var_t.
 *
 *	cpumask_var_t tmpmask;
 *	if (!alloc_cpumask_var(&tmpmask, GFP_KERNEL))
 *		return -ENOMEM;
 *
 *	var = *tmpmask;
 *
 * This code makes NR_CPUS length memcopy and brings to a memory corruption.
 * cpumask_copy() provide safe copy functionality.
 *
 * Note that there is another evil here: If you define a cpumask_var_t
 * as a percpu variable then the way to obtain the address of the cpumask
 * structure differently influences what this_cpu_* operation needs to be
 * used. Please use this_cpu_cpumask_var_t in those cases. The direct use
 * of this_cpu_ptr() or this_cpu_read() will lead to failures when the
 * other type of cpumask_var_t implementation is configured.
 *
 * Please also note that __cpumask_var_read_mostly can be used to declare
 * a cpumask_var_t variable itself (not its content) as read mostly.
 */
#ifdef CONFIG_CPUMASK_OFFSTACK
/* IAMROOT, 2021.09.30:
 * 동적할당을 하는것이 보인다. alloc 및 free함수가 존재한다.
 */
typedef struct cpumask *cpumask_var_t;

/*
 * IAMROOT, 2023.05.06:
 * - @x(pcpu cpumasks)에서 this cpu에 대한 cpumask를 가져온다.
 */
#define this_cpu_cpumask_var_ptr(x)	this_cpu_read(x)
#define __cpumask_var_read_mostly	__read_mostly

bool alloc_cpumask_var_node(cpumask_var_t *mask, gfp_t flags, int node);
bool alloc_cpumask_var(cpumask_var_t *mask, gfp_t flags);
bool zalloc_cpumask_var_node(cpumask_var_t *mask, gfp_t flags, int node);
bool zalloc_cpumask_var(cpumask_var_t *mask, gfp_t flags);
void alloc_bootmem_cpumask_var(cpumask_var_t *mask);
void free_cpumask_var(cpumask_var_t mask);
void free_bootmem_cpumask_var(cpumask_var_t mask);

static inline bool cpumask_available(cpumask_var_t mask)
{
	return mask != NULL;
}

#else
/* IAMROOT, 2021.09.30:
 * 정적할당을 하는것이 보인다. alloc 및 free함수가 실제 code가 없는것이 보인다.
 */
typedef struct cpumask cpumask_var_t[1];

#define this_cpu_cpumask_var_ptr(x) this_cpu_ptr(x)
#define __cpumask_var_read_mostly

static inline bool alloc_cpumask_var(cpumask_var_t *mask, gfp_t flags)
{
	return true;
}

static inline bool alloc_cpumask_var_node(cpumask_var_t *mask, gfp_t flags,
					  int node)
{
	return true;
}

static inline bool zalloc_cpumask_var(cpumask_var_t *mask, gfp_t flags)
{
	cpumask_clear(*mask);
	return true;
}

static inline bool zalloc_cpumask_var_node(cpumask_var_t *mask, gfp_t flags,
					  int node)
{
	cpumask_clear(*mask);
	return true;
}

/*
 * IAMROOT, 2021.11.13:
 * - cpumask를 static하게 사용하면 alloc할 필요없다.
 */
static inline void alloc_bootmem_cpumask_var(cpumask_var_t *mask)
{
}

static inline void free_cpumask_var(cpumask_var_t mask)
{
}

static inline void free_bootmem_cpumask_var(cpumask_var_t mask)
{
}

static inline bool cpumask_available(cpumask_var_t mask)
{
	return true;
}
#endif /* CONFIG_CPUMASK_OFFSTACK */

/* It's common to want to use cpu_all_mask in struct member initializers,
 * so it has to refer to an address rather than a pointer. */
extern const DECLARE_BITMAP(cpu_all_bits, NR_CPUS);
#define cpu_all_mask to_cpumask(cpu_all_bits)

/* First bits of cpu_bit_bitmap are in fact unset. */
/*
 * IAMROOT, 2023.04.13:
 * - cpu가 하나도 set이 안되있는 bitmap을 return한다.
 */
#define cpu_none_mask to_cpumask(cpu_bit_bitmap[0])

#define for_each_possible_cpu(cpu) for_each_cpu((cpu), cpu_possible_mask)
#define for_each_online_cpu(cpu)   for_each_cpu((cpu), cpu_online_mask)
#define for_each_present_cpu(cpu)  for_each_cpu((cpu), cpu_present_mask)

/* Wrappers for arch boot code to manipulate normally-constant masks */
void init_cpu_present(const struct cpumask *src);
void init_cpu_possible(const struct cpumask *src);
void init_cpu_online(const struct cpumask *src);

static inline void reset_cpu_possible_mask(void)
{
	bitmap_zero(cpumask_bits(&__cpu_possible_mask), NR_CPUS);
}

static inline void
set_cpu_possible(unsigned int cpu, bool possible)
{
	if (possible)
		cpumask_set_cpu(cpu, &__cpu_possible_mask);
	else
		cpumask_clear_cpu(cpu, &__cpu_possible_mask);
}

static inline void
set_cpu_present(unsigned int cpu, bool present)
{
	if (present)
		cpumask_set_cpu(cpu, &__cpu_present_mask);
	else
		cpumask_clear_cpu(cpu, &__cpu_present_mask);
}

void set_cpu_online(unsigned int cpu, bool online);

static inline void
set_cpu_active(unsigned int cpu, bool active)
{
	if (active)
		cpumask_set_cpu(cpu, &__cpu_active_mask);
	else
		cpumask_clear_cpu(cpu, &__cpu_active_mask);
}

static inline void
set_cpu_dying(unsigned int cpu, bool dying)
{
	if (dying)
		cpumask_set_cpu(cpu, &__cpu_dying_mask);
	else
		cpumask_clear_cpu(cpu, &__cpu_dying_mask);
}

/**
 * to_cpumask - convert an NR_CPUS bitmap to a struct cpumask *
 * @bitmap: the bitmap
 *
 * There are a few places where cpumask_var_t isn't appropriate and
 * static cpumasks must be used (eg. very early boot), yet we don't
 * expose the definition of 'struct cpumask'.
 *
 * This does the conversion, and can be used as a constant initializer.
 */

/*
 * IAMROOT, 2022.12.30:
 * - @bitmap이 unsigned long * type인지 compile time에 확인한다.
 */
#define to_cpumask(bitmap)						\
	((struct cpumask *)(1 ? (bitmap)				\
			    : (void *)sizeof(__check_is_bitmap(bitmap))))


/*
 * IAMROOT, 2022.12.30:
 * - to_cpumask 매크로에서 @bitmap이 unsigned long * type인지 확인하는 용도
 */
static inline int __check_is_bitmap(const unsigned long *bitmap)
{
	return 1;
}

/*
 * Special-case data structure for "single bit set only" constant CPU masks.
 *
 * We pre-generate all the 64 (or 32) possible bit positions, with enough
 * padding to the left and the right, and return the constant pointer
 * appropriately offset.
 */

extern const unsigned long
	cpu_bit_bitmap[BITS_PER_LONG+1][BITS_TO_LONGS(NR_CPUS)];


/*
 * IAMROOT, 2022.12.30:
 * --- 작동원리.
 * -- NR_CPUS가 1 ~ 64개까지는 일반 배열진입과 다를바 없다.
 *  [0]  = {0x0000_0000_0000_0000}
 *  [1]  = {0x0000_0000_0000_0001}
 *  ..
 *  [63] = {0x4000_0000_0000_0000}
 *  [64] = {0x8000_0000_0000_0000}
 *
 *   cpu_bit_bitmap[1 + cpu % BITS_TO_LONGS]에 의해서 cpu + 1번호에
 *   대한 배열이 선택되고 p -= cpu / BITS_PER_LONG 은 아무동작
 *   안하기 때문이다.
 *
 * -- NR_CPUS가 65 ~ 128개
 *  [0]  = {0x0000_0000_0000_0000, 0x0}
 *  [1]  = {0x0000_0000_0000_0001, 0x0}
 *  ..
 *  [63] = {0x4000_0000_0000_0000, 0x0}
 *  [64] = {0x8000_0000_0000_0000, 0x0}
 *
 *  cpu 0 ~ 63을 구할땐 p -= cpu / BITS_PER_LONG이 동작을 안하므로
 *  일반 배열 접근과 같다.
 *  하지만 65 ~ 128을 접근할땐
 *
 *  p -= cpu / BITS_PER_LONG => 로 인해서 -1 long size만큼 시작주소가
 *   감소한다
 *
 *  65번을 예로 했을때.
 *  [0]  = {0x0000_0000_0000_0000, 0x0}
 *                                 ^여기가 시작주소가 된다.
 *  [1]  = {0x0000_0000_0000_0001, 0x0}
 *
 *  여기서 cpumask는 2 long size만큼이므로 return되는 값은 address
 *  기운을 봤을때
 *  
 *  {0x0000_0000_0000_0000, 0x0} {0x0000_0000_0000_0001, 0x0}
 *                          ^^^^^^^^^^^^^^^^^^^^^^^^^^^
 *                                 이 위치가 되게되고 return값은
 *
 *  {0x0000_0000_0000_0000}, {0x0000_0000_0000_0001}
 *
 *  가 된다.
 *
 *  즉 long size단위로 -를 시킴으로써 long size단위로 건너 뜀으로
 *  long bits개의 행개수 만큼으로 모든 NR_CPUS개의 각각 set 된
 *  cpumask bit를 표현할수있다.
 *
 * -- NR_CPUS가 129 ~ 192개
 *  [0]  = {0x0000_0000_0000_0000, 0x0, 0x0}
 *  [1]  = {0x0000_0000_0000_0001, 0x0, 0x0}
 *  ..
 *  [63] = {0x4000_0000_0000_0000, 0x0, 0x0}
 *  [64] = {0x8000_0000_0000_0000, 0x0, 0x0}
 *
 *  cpu 128을 접근한다고 할때.
 *  p -= cpu / BITS_PER_LONG => 로 인해서 -2 long size만큼 시작주소가
 *   감소한다
 *
 *  [0]  = {0x0000_0000_0000_0000, 0x0, 0x0}
 *                                 ^여기서 시작되고
 *  [1]  = {0x0000_0000_0000_0001, 0x0, 0x0}
 *
 *  연속된주소로 봤을때.
 *  {0x0000_0000_0000_0000, 0x0, 0x0} {0x0000_0000_0000_0001, 0x0, 0x0}
 *                          ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
 *                           
 *  {0x0}, {0x0}, {0x0000_0000_0000_0001}
 *  가 return이 된다.
 *   
 * ---
 * - @cpu에 대한 bit만 set된 const cpumask를 얻어온다.
 * - long bits개 단위로 long size 단위로 시작주소를 -로 감소함으로써
 *   가장 기본이 되는 64개의 행을 가진 배열 한개로 NR_CPUS만큼의
 *   bit가 각 set된 cpumask를 가져온다.
 *   ex) NR_CPU 1 ~ 64개
 *       cpu = 1  > {0x0000_0000_0000_0001}
 *       cpu = 2  > {0x0000_0000_0000_0002}
 *       cpu = 64 > {0x8000_0000_0000_0000}
 *   ex) NR_CPU 65 ~ 128개
 *       cpu = 1  > {0x0}, {0x0000_0000_0000_0001}
 *       cpu = 2  > {0x0}, {0x0000_0000_0000_0002}
 *       cpu = 64 > {0x0}, {0x8000_0000_0000_0000}
 *       cpu = 65 > {0x0000_0000_0000_0001}, {0x0}
 *   ex) NR_CPU 129 ~ 192개
 *       cpu =  1  > {0x0}, {0x0}, {0x0000_0000_0000_0001}
 *       cpu =  2  > {0x0}, {0x0}, {0x0000_0000_0000_0002}
 *       cpu =  64 > {0x0}, {0x0}, {0x8000_0000_0000_0000}
 *       cpu =  65 > {0x0}, {0x0000_0000_0000_0001}, {0x0}
 *       cpu = 129 > {0x0000_0000_0000_0001}, {0x0}, {0x0}
 */
static inline const struct cpumask *get_cpu_mask(unsigned int cpu)
{
	const unsigned long *p = cpu_bit_bitmap[1 + cpu % BITS_PER_LONG];
	p -= cpu / BITS_PER_LONG;
	return to_cpumask(p);
}

#if NR_CPUS > 1
/**
 * num_online_cpus() - Read the number of online CPUs
 *
 * Despite the fact that __num_online_cpus is of type atomic_t, this
 * interface gives only a momentary snapshot and is not protected against
 * concurrent CPU hotplug operations unless invoked from a cpuhp_lock held
 * region.
 */
static inline unsigned int num_online_cpus(void)
{
	return atomic_read(&__num_online_cpus);
}
#define num_possible_cpus()	cpumask_weight(cpu_possible_mask)
#define num_present_cpus()	cpumask_weight(cpu_present_mask)
#define num_active_cpus()	cpumask_weight(cpu_active_mask)

static inline bool cpu_online(unsigned int cpu)
{
	return cpumask_test_cpu(cpu, cpu_online_mask);
}

static inline bool cpu_possible(unsigned int cpu)
{
	return cpumask_test_cpu(cpu, cpu_possible_mask);
}

static inline bool cpu_present(unsigned int cpu)
{
	return cpumask_test_cpu(cpu, cpu_present_mask);
}

/*
 * IAMROOT, 2023.04.13:
 * - cpu가 active인지(스케쥴링 가능한 상태) 확인한다.
 */
static inline bool cpu_active(unsigned int cpu)
{
	return cpumask_test_cpu(cpu, cpu_active_mask);
}

static inline bool cpu_dying(unsigned int cpu)
{
	return cpumask_test_cpu(cpu, cpu_dying_mask);
}

#else

#define num_online_cpus()	1U
#define num_possible_cpus()	1U
#define num_present_cpus()	1U
#define num_active_cpus()	1U

static inline bool cpu_online(unsigned int cpu)
{
	return cpu == 0;
}

static inline bool cpu_possible(unsigned int cpu)
{
	return cpu == 0;
}

static inline bool cpu_present(unsigned int cpu)
{
	return cpu == 0;
}

static inline bool cpu_active(unsigned int cpu)
{
	return cpu == 0;
}

static inline bool cpu_dying(unsigned int cpu)
{
	return false;
}

#endif /* NR_CPUS > 1 */

/*
 * IAMROOT, 2023.03.11:
 * - 스케쥴러가 동작하지 않음(cpu_online_mask 에 설정되지 않았다)
 */
#define cpu_is_offline(cpu)	unlikely(!cpu_online(cpu))

#if NR_CPUS <= BITS_PER_LONG
#define CPU_BITS_ALL						\
{								\
	[BITS_TO_LONGS(NR_CPUS)-1] = BITMAP_LAST_WORD_MASK(NR_CPUS)	\
}

#else /* NR_CPUS > BITS_PER_LONG */

#define CPU_BITS_ALL						\
{								\
	[0 ... BITS_TO_LONGS(NR_CPUS)-2] = ~0UL,		\
	[BITS_TO_LONGS(NR_CPUS)-1] = BITMAP_LAST_WORD_MASK(NR_CPUS)	\
}
#endif /* NR_CPUS > BITS_PER_LONG */

/**
 * cpumap_print_to_pagebuf  - copies the cpumask into the buffer either
 *	as comma-separated list of cpus or hex values of cpumask
 * @list: indicates whether the cpumap must be list
 * @mask: the cpumask to copy
 * @buf: the buffer to copy into
 *
 * Returns the length of the (null-terminated) @buf string, zero if
 * nothing is copied.
 */
static inline ssize_t
cpumap_print_to_pagebuf(bool list, char *buf, const struct cpumask *mask)
{
	return bitmap_print_to_pagebuf(list, buf, cpumask_bits(mask),
				      nr_cpu_ids);
}

/**
 * cpumap_print_bitmask_to_buf  - copies the cpumask into the buffer as
 *	hex values of cpumask
 *
 * @buf: the buffer to copy into
 * @mask: the cpumask to copy
 * @off: in the string from which we are copying, we copy to @buf
 * @count: the maximum number of bytes to print
 *
 * The function prints the cpumask into the buffer as hex values of
 * cpumask; Typically used by bin_attribute to export cpumask bitmask
 * ABI.
 *
 * Returns the length of how many bytes have been copied, excluding
 * terminating '\0'.
 */
static inline ssize_t
cpumap_print_bitmask_to_buf(char *buf, const struct cpumask *mask,
		loff_t off, size_t count)
{
	return bitmap_print_bitmask_to_buf(buf, cpumask_bits(mask),
				   nr_cpu_ids, off, count) - 1;
}

/**
 * cpumap_print_list_to_buf  - copies the cpumask into the buffer as
 *	comma-separated list of cpus
 *
 * Everything is same with the above cpumap_print_bitmask_to_buf()
 * except the print format.
 */
static inline ssize_t
cpumap_print_list_to_buf(char *buf, const struct cpumask *mask,
		loff_t off, size_t count)
{
	return bitmap_print_list_to_buf(buf, cpumask_bits(mask),
				   nr_cpu_ids, off, count) - 1;
}

#if NR_CPUS <= BITS_PER_LONG
#define CPU_MASK_ALL							\
(cpumask_t) { {								\
	[BITS_TO_LONGS(NR_CPUS)-1] = BITMAP_LAST_WORD_MASK(NR_CPUS)	\
} }
#else
#define CPU_MASK_ALL							\
(cpumask_t) { {								\
	[0 ... BITS_TO_LONGS(NR_CPUS)-2] = ~0UL,			\
	[BITS_TO_LONGS(NR_CPUS)-1] = BITMAP_LAST_WORD_MASK(NR_CPUS)	\
} }
#endif /* NR_CPUS > BITS_PER_LONG */

#define CPU_MASK_NONE							\
(cpumask_t) { {								\
	[0 ... BITS_TO_LONGS(NR_CPUS)-1] =  0UL				\
} }

#define CPU_MASK_CPU0							\
(cpumask_t) { {								\
	[0] =  1UL							\
} }

#endif /* __LINUX_CPUMASK_H */
