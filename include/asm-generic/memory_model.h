/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_MEMORY_MODEL_H
#define __ASM_MEMORY_MODEL_H

#include <linux/pfn.h>

#ifndef __ASSEMBLY__

/*
 * supports 3 memory models.
 */
#if defined(CONFIG_FLATMEM)

#ifndef ARCH_PFN_OFFSET
#define ARCH_PFN_OFFSET		(0UL)
#endif

#define __pfn_to_page(pfn)	(mem_map + ((pfn) - ARCH_PFN_OFFSET))
#define __page_to_pfn(page)	((unsigned long)((page) - mem_map) + \
				 ARCH_PFN_OFFSET)

/*
 * IAMROOT, 2021.12.11:
 * - vmemmap을 쓸경우 vmemmap을 시작으로 pfn을 구하면 되기때문에
 *   flat과 동일한 성능이 나온다.
 */
#elif defined(CONFIG_SPARSEMEM_VMEMMAP)

/* memmap is virtually contiguous.  */
#define __pfn_to_page(pfn)	(vmemmap + (pfn))
#define __page_to_pfn(page)	(unsigned long)((page) - vmemmap)

/*
 * IAMROOT, 2021.12.11:
 * - vmemmap을 사용안할경우 직접 memmap 구조체를 가져와 구해야된다.
 */
#elif defined(CONFIG_SPARSEMEM)
/*
 * Note: section's mem_map is encoded to reflect its start_pfn.
 * section[i].section_mem_map == mem_map's address - start_pfn;
 */
#define __page_to_pfn(pg)					\
({	const struct page *__pg = (pg);				\
	int __sec = page_to_section(__pg);			\
	(unsigned long)(__pg - __section_mem_map_addr(__nr_to_section(__sec)));	\
})

#define __pfn_to_page(pfn)				\
({	unsigned long __pfn = (pfn);			\
	struct mem_section *__sec = __pfn_to_section(__pfn);	\
	__section_mem_map_addr(__sec) + __pfn;		\
})
#endif /* CONFIG_FLATMEM/SPARSEMEM */

/*
 * IAMROOT, 2021.10.09: 
 * __phys_to_pfn():
 *   물리 주소를 pfn으로 변환한다.
 *   4K 페이지 예) 0x1234_5678 -> 0x12345
 *
 * __pfn_to_phys():
 *   pfn을 물리 주소로 변환한다.
 *   4K 페이지 예) 0x12345 -> 0x1234_5000
 */
/*
 * Convert a physical address to a Page Frame Number and back
 */
#define	__phys_to_pfn(paddr)	PHYS_PFN(paddr)
#define	__pfn_to_phys(pfn)	PFN_PHYS(pfn)

/*
 * IAMROOT, 2021.12.11:
 * - memmap으로 접근해 struct page 주소를 가져오거나
 *   반대로 struct page주소로 pfn을 구해온다.
 */
#define page_to_pfn __page_to_pfn
#define pfn_to_page __pfn_to_page

#endif /* __ASSEMBLY__ */

#endif
