/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Google LLC.
 */
#ifndef __ASM_RWONCE_H
#define __ASM_RWONCE_H

#ifdef CONFIG_LTO

#include <linux/compiler_types.h>
#include <asm/alternative-macros.h>

#ifndef BUILD_VDSO

#ifdef CONFIG_AS_HAS_LDAPR
/*
 * IAMROOT, 2022.11.11:
 * - consistency
 *   https://www.youtube.com/watch?v=Fm8iUFM2iWU
 *   https://www.cs.colostate.edu/~cs551/CourseNotes/Consistency/TypesConsistency.html
 *   https://www.inf.ed.ac.uk/teaching/courses/pa/Notes/lecture07-sc.pdf
 *   https://www.youtube.com/watch?v=6OLnU27wDb0
 *   https://en.wikipedia.org/wiki/Release_consistency
 *
 * - processor consistency
 *   https://en.wikipedia.org/wiki/Processor_consistency
 *   process에 실행된 write의 순서가 다른 process에서 동일하게 read된다는것.
 *
 * - RCpc(release consistent processor consisten),
 *   RCsc (release consistent sequential consistent)
 *   https://stackoverflow.com/questions/68676666/armv8-3-meaning-of-rcpc
 *   https://www.hpl.hp.com/techreports/Compaq-DEC/WRL-95-7.pdf
 *
 * - ldapr(load acquire RCpc Register)
 *   https://www.dpdk.org/wp-content/uploads/sites/35/2019/10/StateofC11Code.pdf
 */
#define __LOAD_RCPC(sfx, regs...)					\
	ALTERNATIVE(							\
		"ldar"	#sfx "\t" #regs,				\
		".arch_extension rcpc\n"				\
		"ldapr"	#sfx "\t" #regs,				\
	ARM64_HAS_LDAPR)
#else
#define __LOAD_RCPC(sfx, regs...)	"ldar" #sfx "\t" #regs
#endif /* CONFIG_AS_HAS_LDAPR */

/*
 * When building with LTO, there is an increased risk of the compiler
 * converting an address dependency headed by a READ_ONCE() invocation
 * into a control dependency and consequently allowing for harmful
 * reordering by the CPU.
 *
 * Ensure that such transformations are harmless by overriding the generic
 * READ_ONCE() definition with one that provides RCpc acquire semantics
 * when building with LTO.
 */
/*
 * IAMROOT, 2022.11.11:
 * - papago
 *   LTO로 빌드할 때 컴파일러가 READ_ONCE() 호출이 이끄는 주소 종속성을 제어
 *   종속성으로 변환하고 결과적으로 CPU에 의한 유해한 재정렬을 허용할 위험이
 *   증가합니다.
 *
 *   LTO로 구축할 때 RCpc 획득 의미 체계를 제공하는 일반 READ_ONCE() 정의를
 *   재정의하여 이러한 변환이 무해한지 확인합니다.
 *
 * - LTO(Link Time Optimization)
 *   https://gcc.gnu.org/wiki/LinkTimeOptimization
 */
#define __READ_ONCE(x)							\
({									\
	typeof(&(x)) __x = &(x);					\
	int atomic = 1;							\
	union { __unqual_scalar_typeof(*__x) __val; char __c[1]; } __u;	\
	switch (sizeof(x)) {						\
	case 1:								\
		asm volatile(__LOAD_RCPC(b, %w0, %1)			\
			: "=r" (*(__u8 *)__u.__c)			\
			: "Q" (*__x) : "memory");			\
		break;							\
	case 2:								\
		asm volatile(__LOAD_RCPC(h, %w0, %1)			\
			: "=r" (*(__u16 *)__u.__c)			\
			: "Q" (*__x) : "memory");			\
		break;							\
	case 4:								\
		asm volatile(__LOAD_RCPC(, %w0, %1)			\
			: "=r" (*(__u32 *)__u.__c)			\
			: "Q" (*__x) : "memory");			\
		break;							\
	case 8:								\
		asm volatile(__LOAD_RCPC(, %0, %1)			\
			: "=r" (*(__u64 *)__u.__c)			\
			: "Q" (*__x) : "memory");			\
		break;							\
	default:							\
		atomic = 0;						\
	}								\
	atomic ? (typeof(*__x))__u.__val : (*(volatile typeof(__x))__x);\
})

#endif	/* !BUILD_VDSO */
#endif	/* CONFIG_LTO */

#include <asm-generic/rwonce.h>

#endif	/* __ASM_RWONCE_H */
