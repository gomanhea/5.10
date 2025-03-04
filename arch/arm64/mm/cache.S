/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Cache maintenance
 *
 * Copyright (C) 2001 Deep Blue Solutions Ltd.
 * Copyright (C) 2012 ARM Ltd.
 */

#include <linux/errno.h>
#include <linux/linkage.h>
#include <linux/init.h>
#include <asm/assembler.h>
#include <asm/cpufeature.h>
#include <asm/alternative.h>
#include <asm/asm-uaccess.h>

/*
 *	caches_clean_inval_pou_macro(start,end) [fixup]
 *
 *	Ensure that the I and D caches are coherent within specified region.
 *	This is typically used when code has been written to a memory region,
 *	and will be executed.
 *
 *	- start   - virtual start address of region
 *	- end     - virtual end address of region
 *	- fixup   - optional label to branch to on user fault
 */
.macro	caches_clean_inval_pou_macro, fixup
alternative_if ARM64_HAS_CACHE_IDC
	dsb     ishst
	b       .Ldc_skip_\@
alternative_else_nop_endif
	mov     x2, x0
	mov     x3, x1
	dcache_by_line_op cvau, ish, x2, x3, x4, x5, \fixup
.Ldc_skip_\@:
alternative_if ARM64_HAS_CACHE_DIC
	isb
	b	.Lic_skip_\@
alternative_else_nop_endif
	invalidate_icache_by_line x0, x1, x2, x3, \fixup
.Lic_skip_\@:
.endm

/*
 *	caches_clean_inval_pou(start,end)
 *
 *	Ensure that the I and D caches are coherent within specified region.
 *	This is typically used when code has been written to a memory region,
 *	and will be executed.
 *
 *	- start   - virtual start address of region
 *	- end     - virtual end address of region
 */
SYM_FUNC_START(caches_clean_inval_pou)
	caches_clean_inval_pou_macro
	ret
SYM_FUNC_END(caches_clean_inval_pou)

/*
 *	caches_clean_inval_user_pou(start,end)
 *
 *	Ensure that the I and D caches are coherent within specified region.
 *	This is typically used when code has been written to a memory region,
 *	and will be executed.
 *
 *	- start   - virtual start address of region
 *	- end     - virtual end address of region
 */
SYM_FUNC_START(caches_clean_inval_user_pou)
	uaccess_ttbr0_enable x2, x3, x4

	caches_clean_inval_pou_macro 2f
	mov	x0, xzr
1:
	uaccess_ttbr0_disable x1, x2
	ret
2:
	mov	x0, #-EFAULT
	b	1b
SYM_FUNC_END(caches_clean_inval_user_pou)

/*
 *	icache_inval_pou(start,end)
 *
 *	Ensure that the I cache is invalid within specified region.
 *
 *	- start   - virtual start address of region
 *	- end     - virtual end address of region
 */
SYM_FUNC_START(icache_inval_pou)
alternative_if ARM64_HAS_CACHE_DIC
	isb
	ret
alternative_else_nop_endif

	invalidate_icache_by_line x0, x1, x2, x3
	ret
SYM_FUNC_END(icache_inval_pou)

/*
 *	dcache_clean_inval_poc(start, end)
 *
 *	Ensure that any D-cache lines for the interval [start, end)
 *	are cleaned and invalidated to the PoC.
 *
 *	- start   - virtual start address of region
 *	- end     - virtual end address of region
 */
SYM_FUNC_START_PI(dcache_clean_inval_poc)
	dcache_by_line_op civac, sy, x0, x1, x2, x3
	ret
SYM_FUNC_END_PI(dcache_clean_inval_poc)

/*
 *	dcache_clean_pou(start, end)
 *
 * 	Ensure that any D-cache lines for the interval [start, end)
 * 	are cleaned to the PoU.
 *
 *	- start   - virtual start address of region
 *	- end     - virtual end address of region
 */
SYM_FUNC_START(dcache_clean_pou)
alternative_if ARM64_HAS_CACHE_IDC
	dsb	ishst
	ret
alternative_else_nop_endif
	dcache_by_line_op cvau, ish, x0, x1, x2, x3
	ret
SYM_FUNC_END(dcache_clean_pou)

/*
 *	dcache_inval_poc(start, end)
 *
 * 	Ensure that any D-cache lines for the interval [start, end)
 * 	are invalidated. Any partial lines at the ends of the interval are
 *	also cleaned to PoC to prevent data loss.
 *
 *	- start   - kernel start address of region
 *	- end     - kernel end address of region
 */
SYM_FUNC_START_LOCAL(__dma_inv_area)
/*
 * IAMROOT, 2021.07.17: 
 *
 * - SYM_FUNC_START_PI(x) 해석:
 *   - SYM_FUNC_START_ALIAS(__pi_##x): 하나의 FUNC에 1개 이상의 이름을
 *                                     부여하고 싶을때 사용하는 매크로
 *          .globl __pi_dcache_inval_poc
 *          .align 2;
 *          __pi_dcache_inval_poc:
 *
 *   - SYM_FUNC_START()
 *          .globl dcache_inval_poc
 *          .align 2;
 *          dcache_inval_poc:
 *
 *   - BTI_C
 *          hint 34;
 */
SYM_FUNC_START_PI(dcache_inval_poc)
	/* FALLTHROUGH */

/*
 *	__dma_inv_area(start, end)
 *	- start   - virtual start address of region
 *	- end     - virtual end address of region
 */
 /*
  * IAMROOT, 2021.07.18: 
  * dcache_inval_poc(char * start, int end)
  * {
  *		int dcache_min_line = dcache_line_size();	// 64로가정 0x40
  *		int mask = dcache_min_line - 1;		// 0x40 - 1 = 0x3f
  *
  *		if(end & mask) {
  *			end &= ~(mask);
  *      	clean_invalidated_cache(end); // dc civac, x1
  *     }
  *        
  *     if(start & mask) {
  *			start &= ~(mask);
  *			clean_invalidated_cache(start);
  *			start += dcache_min_line;
  *		}
  *
  *		while(start < end) {
  *			invalidated_cache(start);
  *			start += dcache_min_line;
  *		}
  * }
  */
	dcache_line_size x2, x3
/* IAMROOT, 2021.07.17: 
 * - x0: start address
 * - x1: end address
 * - x2: cache line size
 *
 * End line aligned를 체크하기 위해서 수행하는 인스트럭션
 * 인스트럭션 결과로 하위 모든 Bit가 Set
 *
 * 예) x3 = 0x3F (0x40 - 1) -> (x2가 64인 경우)
 */
	sub	x3, x2, #1
/* IAMROOT, 2021.07.17: 
 * tst r0, r1 : r0와 r1을 AND 연산한 것처럼 
 * -> CPSR 의 NZCV Flag가 결정 
 * -> Z가 영향을 받음
 */
	tst	x1, x3				// end cache line aligned?
/* IAMROOT, 2021.07.17: 
 * bic x0, x1, x2 : x1 & ~x2 의 결과에서 1을 다 Clear한 후 x0에 저장
 *
 * 예) cache line size = 64 bytes
 *   x1 = 0x0402_0321 (unaligned)
 *   x3 = 0xFFFF_FFC0 (~0x3F)
 *   -------- AND -------------
 *   x1 = 0x0402_0300 (aligned)
 */
	bic	x1, x1, x3
/* IAMROOT, 2021.07.17: 
 * b.eq : 위의 tst 명령어로 인해서 Z flag = 1 이라면 1f 실행 
 * 1f: 밑의 Label 1을 의미
 */	
	b.eq	1f
/* IAMROOT, 2021.07.17: 
 * D : Data cache
 * U : Unified cache
 * dc : 통합 캐시까지 모두 Clear
 * Clean : Dirty bit가 marking된 cache line에 한하여
 *         next level cache (unified L2) or main memory에
 *         writing 후 dirty bit clear 수행.
 *         (write-back policy 일때 적용되는 규칙)
 * Invalidate : Cache line의 V Status를 Clear
 *
 * - Point of Coherency 의미
 *   For a particular address, the PoC is the point at which all
 *   observers, for example, cores, DSPs, or DMA engines,
 *   that can access memory, are guaranteed to see the same copy of a memory
 *   location. Typically, this(PoC point) is the main external system memory.
 *   - D-Cache와 관련이 있다.
 *   - Main memory가 PoC point 이고 L1, L2, L3 모든 cache line에 대하여
 *     clean 수행.
 *
 * - Point of Unification 의미
 *   The PoU for a core is the point at which the instruction and
 *   data caches and translation table walks of the core are guaranteed to
 *   see the same copy of a memory location. For example, a unified level 2 cache
 *   would be the point of unification in a system with Harvard level 1 caches and
 *   a TLB for caching translation table entries.
 *   If no external cache is present, main memory would be the Point of Unification.
 *   - I-Cache와 관련이 있다.
 *   - Unified (L2/L3) cache 까지만 적용
 *
 * - 시작/끝 주소가 cache line size에 맞춰 align이 필요한 경우
 *   - boot_args[0], [3]: civac 적용
 *              [1], [2]: ivac 적용
 *   1. [0]과 [1]의 address를 align.
 *   2. 시작과 끝 주소에 대해서만 civac 적용.
 *     - cache controller는 clean & invalidate 작업에 대해 cache line size
 *       만큼 진행하나 align 되어 있지 않다면 해당 주소와 연관되어 있는
 *       main memory의 다른 부분이 함께 cache linefill된 경우이며 해당 부분의
 *       cache coherency를 위하여 civac 명령어로 처리함.
 *
 * - dc 명령어와 cache controller의 관계
 *   dc 명령어가 수행되면 cache controller의 queue에 push 되고 controller는
 *   background에서 queue를 계속 처리한다. 또한, queue pool은 10개인데 100개의
 *   요청이 들어오면 10개는 바로 push하고 90개는 대기하게 된다.
 */
	dc	civac, x1			// clean & invalidate D / U line
/*
 * IAMROOT, 2021.07.24: 
 * data cache 정렬이 되지 않은 시작과 끝지점에 대해서는 
 * 데이터 보호를 위해 clean 작업을 수행한다.
 */
1:	tst	x0, x3				// start cache line aligned?
	bic	x0, x0, x3
	b.eq	2f
	dc	civac, x0			// clean & invalidate D / U line
	b	3f
2:	dc	ivac, x0			// invalidate D / U line
3:	add	x0, x0, x2
	cmp	x0, x1
	b.lo	2b
/*
 * IAMROOT, 2021.07.24: 
 *
 * dsb: Data Synchronization Barrier
 * sy : ALL cores
 * - Pending load/store, cache maintenance insts, TLB maintenance insts의
 *   수행 결과를 다음 명령어가 실행되기 전에 완료(보장)하기 위하여 사용.
 * - dsb 명령어를 수행하게 되면 cache controller는 queue에 대기중인 명렁어
 *   처리가 끝날때까지 input을 hold하고 queue pool을 처리한다.
 */
	dsb	sy
	ret
SYM_FUNC_END_PI(dcache_inval_poc)
SYM_FUNC_END(__dma_inv_area)

/*
 *	dcache_clean_poc(start, end)
 *
 * 	Ensure that any D-cache lines for the interval [start, end)
 * 	are cleaned to the PoC.
 *
 *	- start   - virtual start address of region
 *	- end     - virtual end address of region
 */
SYM_FUNC_START_LOCAL(__dma_clean_area)
SYM_FUNC_START_PI(dcache_clean_poc)
	/* FALLTHROUGH */

/*
 *	__dma_clean_area(start, end)
 *	- start   - virtual start address of region
 *	- end     - virtual end address of region
 */
	dcache_by_line_op cvac, sy, x0, x1, x2, x3
	ret
SYM_FUNC_END_PI(dcache_clean_poc)
SYM_FUNC_END(__dma_clean_area)

/*
 *	dcache_clean_pop(start, end)
 *
 * 	Ensure that any D-cache lines for the interval [start, end)
 * 	are cleaned to the PoP.
 *
 *	- start   - virtual start address of region
 *	- end     - virtual end address of region
 */
SYM_FUNC_START_PI(dcache_clean_pop)
	alternative_if_not ARM64_HAS_DCPOP
	b	dcache_clean_poc
	alternative_else_nop_endif
	dcache_by_line_op cvap, sy, x0, x1, x2, x3
	ret
SYM_FUNC_END_PI(dcache_clean_pop)

/*
 *	__dma_flush_area(start, size)
 *
 *	clean & invalidate D / U line
 *
 *	- start   - virtual start address of region
 *	- size    - size in question
 */
SYM_FUNC_START_PI(__dma_flush_area)
	add	x1, x0, x1
	dcache_by_line_op civac, sy, x0, x1, x2, x3
	ret
SYM_FUNC_END_PI(__dma_flush_area)

/*
 *	__dma_map_area(start, size, dir)
 *	- start	- kernel virtual start address
 *	- size	- size of region
 *	- dir	- DMA direction
 */
SYM_FUNC_START_PI(__dma_map_area)
	add	x1, x0, x1
	cmp	w2, #DMA_FROM_DEVICE
	b.eq	__dma_inv_area
	b	__dma_clean_area
SYM_FUNC_END_PI(__dma_map_area)

/*
 *	__dma_unmap_area(start, size, dir)
 *	- start	- kernel virtual start address
 *	- size	- size of region
 *	- dir	- DMA direction
 */
SYM_FUNC_START_PI(__dma_unmap_area)
	add	x1, x0, x1
	cmp	w2, #DMA_TO_DEVICE
	b.ne	__dma_inv_area
	ret
SYM_FUNC_END_PI(__dma_unmap_area)
