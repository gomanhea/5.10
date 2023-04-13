/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Written by Mark Hemment, 1996 (markhe@nextd.demon.co.uk).
 *
 * (C) SGI 2006, Christoph Lameter
 * 	Cleaned up and restructured to ease the addition of alternative
 * 	implementations of SLAB allocators.
 * (C) Linux Foundation 2008-2013
 *      Unified interface for all slab allocators
 */

#ifndef _LINUX_SLAB_H
#define	_LINUX_SLAB_H

#include <linux/gfp.h>
#include <linux/overflow.h>
#include <linux/types.h>
#include <linux/workqueue.h>
#include <linux/percpu-refcount.h>


/*
 * Flags to pass to kmem_cache_create().
 * The ones marked DEBUG are only valid if CONFIG_DEBUG_SLAB is set.
 */
/*
 * IAMROOT, 2022.06.11: 
 * 슬랩 캐시 생성에 사용되는 플래그들
 * - SLAB_CONSISTENCY_CHECKS: saniny check를 수행
 * - SLAB_RED_ZONE: 디버그용 red-zone을 구성한다.
 * - SLAB_POISON: 디버그용 poison을 사용한다.
 * - SLAB_HWCACHE_ALIGN: 성능을 높이기 위해 하드웨어 캐시 정렬을 요청한다.
 * - SLAB_CACHE_DMA: DMA 버퍼로 사용할 캐시
 * - SLAB_CACHE_DMA32: DMA32 버퍼로 사용할 캐시
 * - SLAB_STORE_USER: 유저 트래킹을 구성한다.
 * - SLAB_PANIC: 메모리 부족 시 slab panic 요청
 */
/* DEBUG: Perform (expensive) checks on alloc/free */
#define SLAB_CONSISTENCY_CHECKS	((slab_flags_t __force)0x00000100U)
/* DEBUG: Red zone objs in a cache */
#define SLAB_RED_ZONE		((slab_flags_t __force)0x00000400U)
/* DEBUG: Poison objects */
#define SLAB_POISON		((slab_flags_t __force)0x00000800U)
/* Align objs on cache lines */
#define SLAB_HWCACHE_ALIGN	((slab_flags_t __force)0x00002000U)
/* Use GFP_DMA memory */
#define SLAB_CACHE_DMA		((slab_flags_t __force)0x00004000U)
/* Use GFP_DMA32 memory */
#define SLAB_CACHE_DMA32	((slab_flags_t __force)0x00008000U)
/* DEBUG: Store the last owner for bug hunting */
#define SLAB_STORE_USER		((slab_flags_t __force)0x00010000U)
/* Panic if kmem_cache_create() fails */
#define SLAB_PANIC		((slab_flags_t __force)0x00040000U)
/*
 * SLAB_TYPESAFE_BY_RCU - **WARNING** READ THIS!
 *
 * This delays freeing the SLAB page by a grace period, it does _NOT_
 * delay object freeing. This means that if you do kmem_cache_free()
 * that memory location is free to be reused at any time. Thus it may
 * be possible to see another object there in the same RCU grace period.
 *
 * This feature only ensures the memory location backing the object
 * stays valid, the trick to using this is relying on an independent
 * object validation pass. Something like:
 *
 *  rcu_read_lock()
 * again:
 *  obj = lockless_lookup(key);
 *  if (obj) {
 *    if (!try_get_ref(obj)) // might fail for free objects
 *      goto again;
 *
 *    if (obj->key != key) { // not the object we expected
 *      put_ref(obj);
 *      goto again;
 *    }
 *  }
 *  rcu_read_unlock();
 *
 * This is useful if we need to approach a kernel structure obliquely,
 * from its address obtained without the usual locking. We can lock
 * the structure to stabilize it and check it's still at the given address,
 * only if we can be sure that the memory has not been meanwhile reused
 * for some other kind of object (which our subsystem's lock might corrupt).
 *
 * rcu_read_lock before reading the address, then rcu_read_unlock after
 * taking the spinlock within the structure expected at that address.
 *
 * Note that SLAB_TYPESAFE_BY_RCU was originally named SLAB_DESTROY_BY_RCU.
 */
/*
 * IAMROOT, 2022.06.11: 
 * - SLAB_TYPESAFE_BY_RCU:
 *   캐시가 rcu를 사용한 슬랩 오브젝트의 소멸 방법을 사용하는 경우 사용
 * - SLAB_NOLEAKTRACE:
 *   메모리 누수 트레이스를 하지 않도록 할 때 요청할 때 사용
 */
/* Defer freeing slabs to RCU */
#define SLAB_TYPESAFE_BY_RCU	((slab_flags_t __force)0x00080000U)
/* Spread some memory over cpuset */
#define SLAB_MEM_SPREAD		((slab_flags_t __force)0x00100000U)
/* Trace allocations and frees */
#define SLAB_TRACE		((slab_flags_t __force)0x00200000U)

/* Flag to prevent checks on free */
#ifdef CONFIG_DEBUG_OBJECTS
# define SLAB_DEBUG_OBJECTS	((slab_flags_t __force)0x00400000U)
#else
# define SLAB_DEBUG_OBJECTS	0
#endif

/* Avoid kmemleak tracing */
#define SLAB_NOLEAKTRACE	((slab_flags_t __force)0x00800000U)

/* Fault injection mark */
#ifdef CONFIG_FAILSLAB
# define SLAB_FAILSLAB		((slab_flags_t __force)0x02000000U)
#else
# define SLAB_FAILSLAB		0
#endif
/* Account to memcg */
/*
 * IAMROOT, 2022.06.25:
 * - memcg용 account
 */
#ifdef CONFIG_MEMCG_KMEM
# define SLAB_ACCOUNT		((slab_flags_t __force)0x04000000U)
#else
# define SLAB_ACCOUNT		0
#endif

#ifdef CONFIG_KASAN
#define SLAB_KASAN		((slab_flags_t __force)0x08000000U)
#else
#define SLAB_KASAN		0
#endif

/* The following flags affect the page allocator grouping pages by mobility */
/* Objects are reclaimable */
/*
 * IAMROOT, 2022.06.18:
 * - slab을 reclaim가능한 cache로 만든다.
 *   ex) inode
 */
#define SLAB_RECLAIM_ACCOUNT	((slab_flags_t __force)0x00020000U)
#define SLAB_TEMPORARY		SLAB_RECLAIM_ACCOUNT	/* Objects are short-lived */

/* Slab deactivation flag */
#define SLAB_DEACTIVATED	((slab_flags_t __force)0x10000000U)

/*
 * ZERO_SIZE_PTR will be returned for zero sized kmalloc requests.
 *
 * Dereferencing ZERO_SIZE_PTR will lead to a distinct access fault.
 *
 * ZERO_SIZE_PTR can be passed to kfree though in the same way that NULL can.
 * Both make kfree a no-op.
 */
/*
 * IAMROOT, 2022.06.25:
 * - papago
 *   ZERO_SIZE_PTR은 크기가 0인 kmalloc 요청에 대해 반환됩니다. 
 *
 *   ZERO_SIZE_PTR을 역참조하면 고유한 액세스 오류가 발생합니다.
 *
 *   ZERO_SIZE_PTR은 NULL과 같은 방식으로 kfree에 전달할 수 있습니다.
 *   둘 다 kfree를 no-op으로 만듭니다.
 */
#define ZERO_SIZE_PTR ((void *)16)

/*
 * IAMROOT, 2022.06.25:
 * - 0 ~ ZERO_SIZE_PTR까지의 값을 NULL로 취급한다.
 */
#define ZERO_OR_NULL_PTR(x) ((unsigned long)(x) <= \
				(unsigned long)ZERO_SIZE_PTR)

#include <linux/kasan.h>

struct mem_cgroup;
/*
 * struct kmem_cache related prototypes
 */
void __init kmem_cache_init(void);
bool slab_is_available(void);

extern bool usercopy_fallback;

struct kmem_cache *kmem_cache_create(const char *name, unsigned int size,
			unsigned int align, slab_flags_t flags,
			void (*ctor)(void *));
struct kmem_cache *kmem_cache_create_usercopy(const char *name,
			unsigned int size, unsigned int align,
			slab_flags_t flags,
			unsigned int useroffset, unsigned int usersize,
			void (*ctor)(void *));
void kmem_cache_destroy(struct kmem_cache *);
int kmem_cache_shrink(struct kmem_cache *);

/*
 * Please use this macro to create slab caches. Simply specify the
 * name of the structure and maybe some flags that are listed above.
 *
 * The alignment of the struct determines object alignment. If you
 * f.e. add ____cacheline_aligned_in_smp to the struct declaration
 * then the objects will be properly aligned in SMP configurations.
 */
/*
 * IAMROOT, 2023.04.08:
 * - papago
 *   슬랩 캐시를 생성하려면 이 매크로를 사용하십시오. 구조의 이름과 위에 
 *   나열된 일부 플래그를 지정하기만 하면 됩니다.
 *
 *   구조체의 정렬에 따라 객체 정렬이 결정됩니다. 당신이 f.e.
 *   ____cacheline_aligned_in_smp를 구조체 선언에 추가하면 개체가 SMP 
 *   구성에서 적절하게 정렬됩니다.
 */
#define KMEM_CACHE(__struct, __flags)					\
		kmem_cache_create(#__struct, sizeof(struct __struct),	\
			__alignof__(struct __struct), (__flags), NULL)

/*
 * To whitelist a single field for copying to/from usercopy, use this
 * macro instead for KMEM_CACHE() above.
 */
#define KMEM_CACHE_USERCOPY(__struct, __flags, __field)			\
		kmem_cache_create_usercopy(#__struct,			\
			sizeof(struct __struct),			\
			__alignof__(struct __struct), (__flags),	\
			offsetof(struct __struct, __field),		\
			sizeof_field(struct __struct, __field), NULL)

/*
 * Common kmalloc functions provided by all allocators
 */
void * __must_check krealloc(const void *, size_t, gfp_t);
void kfree(const void *);
void kfree_sensitive(const void *);
size_t __ksize(const void *);
size_t ksize(const void *);
#ifdef CONFIG_PRINTK
bool kmem_valid_obj(void *object);
void kmem_dump_obj(void *object);
#endif

#ifdef CONFIG_HAVE_HARDENED_USERCOPY_ALLOCATOR
void __check_heap_object(const void *ptr, unsigned long n, struct page *page,
			bool to_user);
#else
static inline void __check_heap_object(const void *ptr, unsigned long n,
				       struct page *page, bool to_user) { }
#endif

/*
 * Some archs want to perform DMA into kmalloc caches and need a guaranteed
 * alignment larger than the alignment of a 64-bit integer.
 * Setting ARCH_KMALLOC_MINALIGN in arch headers allows that.
 */
/*
 * IAMROOT, 2022.06.11: 
 * 아키텍처마다 DMA에 사용할 align 바이트 수를 지정할 수 있다.
 * 이 값이 8보다 큰 경우 kmalloc에도 동일하게 반영하고, 없으면 8로 지정한다.
 * ARM64의 경우 현재 128 바이트의 정렬을 사용한다.
 * 즉) ARCH_DMA_MINALIGN=128
 *     ARCH_KMALLOC_MINALIGN=128
 *     KMALLOC_MIN_SIZE=128
 *     KMALLOC_SHIFT_LOW=7
 */
#if defined(ARCH_DMA_MINALIGN) && ARCH_DMA_MINALIGN > 8
#define ARCH_KMALLOC_MINALIGN ARCH_DMA_MINALIGN
#define KMALLOC_MIN_SIZE ARCH_DMA_MINALIGN
#define KMALLOC_SHIFT_LOW ilog2(ARCH_DMA_MINALIGN)
#else
#define ARCH_KMALLOC_MINALIGN __alignof__(unsigned long long)
#endif

/*
 * Setting ARCH_SLAB_MINALIGN in arch headers allows a different alignment.
 * Intended for arches that get misalignment faults even for 64 bit integer
 * aligned buffers.
 */
/*
 * IAMROOT, 2022.06.11: 
 * ARM64: 최소 슬랩 align 값은 8바이트이다.
 */
#ifndef ARCH_SLAB_MINALIGN
#define ARCH_SLAB_MINALIGN __alignof__(unsigned long long)
#endif

/*
 * kmalloc and friends return ARCH_KMALLOC_MINALIGN aligned
 * pointers. kmem_cache_alloc and friends return ARCH_SLAB_MINALIGN
 * aligned pointers.
 */
#define __assume_kmalloc_alignment __assume_aligned(ARCH_KMALLOC_MINALIGN)
#define __assume_slab_alignment __assume_aligned(ARCH_SLAB_MINALIGN)
#define __assume_page_alignment __assume_aligned(PAGE_SIZE)

/*
 * Kmalloc array related definitions
 */

#ifdef CONFIG_SLAB
/*
 * The largest kmalloc size supported by the SLAB allocators is
 * 32 megabyte (2^25) or the maximum allocatable page order if that is
 * less than 32 MB.
 *
 * WARNING: Its not easy to increase this value since the allocators have
 * to do various tricks to work around compiler limitations in order to
 * ensure proper constant folding.
 */
#define KMALLOC_SHIFT_HIGH	((MAX_ORDER + PAGE_SHIFT - 1) <= 25 ? \
				(MAX_ORDER + PAGE_SHIFT - 1) : 25)
#define KMALLOC_SHIFT_MAX	KMALLOC_SHIFT_HIGH
#ifndef KMALLOC_SHIFT_LOW
#define KMALLOC_SHIFT_LOW	5
#endif
#endif

#ifdef CONFIG_SLUB
/*
 * SLUB directly allocates requests fitting in to an order-1 page
 * (PAGE_SIZE*2).  Larger requests are passed to the page allocator.
 */
/*
 * IAMROOT, 2022.06.11: 
 * KMALLOC_SHIFT_HIGH=13(4K 페이지인 경우)
 * KMALLOC_SHIFT_MAX=22
 * KMALLOC_SHIFT_LOW=3 (지정되지 않은 경우)
 */

#define KMALLOC_SHIFT_HIGH	(PAGE_SHIFT + 1)
#define KMALLOC_SHIFT_MAX	(MAX_ORDER + PAGE_SHIFT - 1)
#ifndef KMALLOC_SHIFT_LOW
#define KMALLOC_SHIFT_LOW	3
#endif
#endif

#ifdef CONFIG_SLOB
/*
 * SLOB passes all requests larger than one page to the page allocator.
 * No kmalloc array is necessary since objects of different sizes can
 * be allocated from the same page.
 */
#define KMALLOC_SHIFT_HIGH	PAGE_SHIFT
#define KMALLOC_SHIFT_MAX	(MAX_ORDER + PAGE_SHIFT - 1)
#ifndef KMALLOC_SHIFT_LOW
#define KMALLOC_SHIFT_LOW	3
#endif
#endif

/*
 * IAMROOT, 2022.06.11: 
 * kmalloc()을 통해 할당할 수 있는 최소 사이즈 및 최대 사이즈(바이트)
 * KMALLOC_MAX_SIZE=2^22=4M
 * KMALLOC_MAX_CHCHE_SIZE=2^13=8K
 * KMALLOC_MAX_ORDER=11
 * KMALLOC_MIN_SIZE=8
 */
/* Maximum allocatable size */
#define KMALLOC_MAX_SIZE	(1UL << KMALLOC_SHIFT_MAX)
/* Maximum size for which we actually use a slab cache */
#define KMALLOC_MAX_CACHE_SIZE	(1UL << KMALLOC_SHIFT_HIGH)
/* Maximum order allocatable via the slab allocator */
#define KMALLOC_MAX_ORDER	(KMALLOC_SHIFT_MAX - PAGE_SHIFT)

/*
 * Kmalloc subsystem.
 */
#ifndef KMALLOC_MIN_SIZE
#define KMALLOC_MIN_SIZE (1 << KMALLOC_SHIFT_LOW)
#endif

/*
 * This restriction comes from byte sized index implementation.
 * Page size is normally 2^12 bytes and, in this case, if we want to use
 * byte sized index which can represent 2^8 entries, the size of the object
 * should be equal or greater to 2^12 / 2^8 = 2^4 = 16.
 * If minimum size of kmalloc is less than 16, we use it as minimum object
 * size and give up to use byte sized index.
 */
/*
 * IAMROOT, 2022.06.11: 
 * 슬랩 object의 최소 사이즈는 arm64: 16
 */
#define SLAB_OBJ_MIN_SIZE      (KMALLOC_MIN_SIZE < 16 ? \
                               (KMALLOC_MIN_SIZE) : 16)

/*
 * Whenever changing this, take care of that kmalloc_type() and
 * create_kmalloc_caches() still work as intended.
 *
 * KMALLOC_NORMAL can contain only unaccounted objects whereas KMALLOC_CGROUP
 * is for accounted but unreclaimable and non-dma objects. All the other
 * kmem caches can have both accounted and unaccounted objects.
 */

/*
 * IAMROOT, 2022.06.25:
 * - 전부다 있는 경우
 * enum kmalloc_cache_type {
 *	KMALLOC_NORMAL = 0,
 *	KMALLOC_CGROUP,
 *	KMALLOC_RECLAIM,
 *	KMALLOC_DMA,
 *	NR_KMALLOC_TYPES
 * };
 *
 * - 다 없는 경우
 *   enum kmalloc_cache_type {
 *	KMALLOC_NORMAL = 0,
 *	KMALLOC_DMA = KMALLOC_NORMAL,
 *	KMALLOC_CGROUP = KMALLOC_NORMAL,
 *	KMALLOC_RECLAIM,
 *	NR_KMALLOC_TYPES
 * };
 * KMALLOC_NORMAL과 KMALLOC_RECLAIM만 남게 될것이다.
 */
enum kmalloc_cache_type {
	KMALLOC_NORMAL = 0,
#ifndef CONFIG_ZONE_DMA
	KMALLOC_DMA = KMALLOC_NORMAL,
#endif
#ifndef CONFIG_MEMCG_KMEM
	KMALLOC_CGROUP = KMALLOC_NORMAL,
#else
	KMALLOC_CGROUP,
#endif
	KMALLOC_RECLAIM,
#ifdef CONFIG_ZONE_DMA
	KMALLOC_DMA,
#endif
	NR_KMALLOC_TYPES
};

#ifndef CONFIG_SLOB
extern struct kmem_cache *
kmalloc_caches[NR_KMALLOC_TYPES][KMALLOC_SHIFT_HIGH + 1];

/*
 * Define gfp bits that should not be set for KMALLOC_NORMAL.
 */
#define KMALLOC_NOT_NORMAL_BITS					\
	(__GFP_RECLAIMABLE |					\
	(IS_ENABLED(CONFIG_ZONE_DMA)   ? __GFP_DMA : 0) |	\
	(IS_ENABLED(CONFIG_MEMCG_KMEM) ? __GFP_ACCOUNT : 0))

/*
 * IAMROOT, 2022.06.25:
 * - 기본적으로 normal이며, 만약 normal을 사용안한다면 KMALLOC_NOT_NORMAL_BITS가
 *   있어야된다.
 */
static __always_inline enum kmalloc_cache_type kmalloc_type(gfp_t flags)
{
	/*
	 * The most common case is KMALLOC_NORMAL, so test for it
	 * with a single branch for all the relevant flags.
	 */
	if (likely((flags & KMALLOC_NOT_NORMAL_BITS) == 0))
		return KMALLOC_NORMAL;

	/*
	 * At least one of the flags has to be set. Their priorities in
	 * decreasing order are:
	 *  1) __GFP_DMA
	 *  2) __GFP_RECLAIMABLE
	 *  3) __GFP_ACCOUNT
	 */
/*
 * IAMROOT, 2022.06.25:
 * - DMA -> reclaim -> memcg 순의 우선순위로 판별한다.
 */
	if (IS_ENABLED(CONFIG_ZONE_DMA) && (flags & __GFP_DMA))
		return KMALLOC_DMA;
	if (!IS_ENABLED(CONFIG_MEMCG_KMEM) || (flags & __GFP_RECLAIMABLE))
		return KMALLOC_RECLAIM;
	else
		return KMALLOC_CGROUP;
}

/*
 * Figure out which kmalloc slab an allocation of a certain size
 * belongs to.
 * 0 = zero alloc
 * 1 =  65 .. 96 bytes
 * 2 = 129 .. 192 bytes
 * n = 2^(n-1)+1 .. 2^n
 *
 * Note: __kmalloc_index() is compile-time optimized, and not runtime optimized;
 * typical usage is via kmalloc_index() and therefore evaluated at compile-time.
 * Callers where !size_is_constant should only be test modules, where runtime
 * overheads of __kmalloc_index() can be tolerated.  Also see kmalloc_slab().
 */
static __always_inline unsigned int __kmalloc_index(size_t size,
						    bool size_is_constant)
{
	if (!size)
		return 0;

	if (size <= KMALLOC_MIN_SIZE)
		return KMALLOC_SHIFT_LOW;

	if (KMALLOC_MIN_SIZE <= 32 && size > 64 && size <= 96)
		return 1;
	if (KMALLOC_MIN_SIZE <= 64 && size > 128 && size <= 192)
		return 2;
	if (size <=          8) return 3;
	if (size <=         16) return 4;
	if (size <=         32) return 5;
	if (size <=         64) return 6;
	if (size <=        128) return 7;
	if (size <=        256) return 8;
	if (size <=        512) return 9;
	if (size <=       1024) return 10;
	if (size <=   2 * 1024) return 11;
	if (size <=   4 * 1024) return 12;
	if (size <=   8 * 1024) return 13;
	if (size <=  16 * 1024) return 14;
	if (size <=  32 * 1024) return 15;
	if (size <=  64 * 1024) return 16;
	if (size <= 128 * 1024) return 17;
	if (size <= 256 * 1024) return 18;
	if (size <= 512 * 1024) return 19;
	if (size <= 1024 * 1024) return 20;
	if (size <=  2 * 1024 * 1024) return 21;
	if (size <=  4 * 1024 * 1024) return 22;
	if (size <=  8 * 1024 * 1024) return 23;
	if (size <=  16 * 1024 * 1024) return 24;
	if (size <=  32 * 1024 * 1024) return 25;

	if ((IS_ENABLED(CONFIG_CC_IS_GCC) || CONFIG_CLANG_VERSION >= 110000)
	    && !IS_ENABLED(CONFIG_PROFILE_ALL_BRANCHES) && size_is_constant)
		BUILD_BUG_ON_MSG(1, "unexpected size in kmalloc_index()");
	else
		BUG();

	/* Will never be reached. Needed because the compiler may complain */
	return -1;
}
#define kmalloc_index(s) __kmalloc_index(s, true)
#endif /* !CONFIG_SLOB */

void *__kmalloc(size_t size, gfp_t flags) __assume_kmalloc_alignment __malloc;
void *kmem_cache_alloc(struct kmem_cache *, gfp_t flags) __assume_slab_alignment __malloc;
void kmem_cache_free(struct kmem_cache *, void *);

/*
 * Bulk allocation and freeing operations. These are accelerated in an
 * allocator specific way to avoid taking locks repeatedly or building
 * metadata structures unnecessarily.
 *
 * Note that interrupts must be enabled when calling these functions.
 */
void kmem_cache_free_bulk(struct kmem_cache *, size_t, void **);
int kmem_cache_alloc_bulk(struct kmem_cache *, gfp_t, size_t, void **);

/*
 * Caller must not use kfree_bulk() on memory not originally allocated
 * by kmalloc(), because the SLOB allocator cannot handle this.
 */
static __always_inline void kfree_bulk(size_t size, void **p)
{
	kmem_cache_free_bulk(NULL, size, p);
}

#ifdef CONFIG_NUMA
void *__kmalloc_node(size_t size, gfp_t flags, int node) __assume_kmalloc_alignment __malloc;
void *kmem_cache_alloc_node(struct kmem_cache *, gfp_t flags, int node) __assume_slab_alignment __malloc;
#else
static __always_inline void *__kmalloc_node(size_t size, gfp_t flags, int node)
{
	return __kmalloc(size, flags);
}

static __always_inline void *kmem_cache_alloc_node(struct kmem_cache *s, gfp_t flags, int node)
{
	return kmem_cache_alloc(s, flags);
}
#endif

#ifdef CONFIG_TRACING
extern void *kmem_cache_alloc_trace(struct kmem_cache *, gfp_t, size_t) __assume_slab_alignment __malloc;

#ifdef CONFIG_NUMA
extern void *kmem_cache_alloc_node_trace(struct kmem_cache *s,
					   gfp_t gfpflags,
					   int node, size_t size) __assume_slab_alignment __malloc;
#else
static __always_inline void *
kmem_cache_alloc_node_trace(struct kmem_cache *s,
			      gfp_t gfpflags,
			      int node, size_t size)
{
	return kmem_cache_alloc_trace(s, gfpflags, size);
}
#endif /* CONFIG_NUMA */

#else /* CONFIG_TRACING */
static __always_inline void *kmem_cache_alloc_trace(struct kmem_cache *s,
		gfp_t flags, size_t size)
{
	void *ret = kmem_cache_alloc(s, flags);

	ret = kasan_kmalloc(s, ret, size, flags);
	return ret;
}

static __always_inline void *
kmem_cache_alloc_node_trace(struct kmem_cache *s,
			      gfp_t gfpflags,
			      int node, size_t size)
{
	void *ret = kmem_cache_alloc_node(s, gfpflags, node);

	ret = kasan_kmalloc(s, ret, size, gfpflags);
	return ret;
}
#endif /* CONFIG_TRACING */

extern void *kmalloc_order(size_t size, gfp_t flags, unsigned int order) __assume_page_alignment __malloc;

#ifdef CONFIG_TRACING
extern void *kmalloc_order_trace(size_t size, gfp_t flags, unsigned int order) __assume_page_alignment __malloc;
#else
static __always_inline void *
kmalloc_order_trace(size_t size, gfp_t flags, unsigned int order)
{
	return kmalloc_order(size, flags, order);
}
#endif

/*
 * IAMROOT, 2022.06.25:
 * - KMALLOC_MAX_CACHE_SIZE보다 큰 size에 대한 kmalloc 요청은 buddy에서
 *   compound page로 할당받아온다.
 */
static __always_inline void *kmalloc_large(size_t size, gfp_t flags)
{
	unsigned int order = get_order(size);
	return kmalloc_order_trace(size, flags, order);
}

/**
 * kmalloc - allocate memory
 * @size: how many bytes of memory are required.
 * @flags: the type of memory to allocate.
 *
 * kmalloc is the normal method of allocating memory
 * for objects smaller than page size in the kernel.
 *
 * The allocated object address is aligned to at least ARCH_KMALLOC_MINALIGN
 * bytes. For @size of power of two bytes, the alignment is also guaranteed
 * to be at least to the size.
 *
 * The @flags argument may be one of the GFP flags defined at
 * include/linux/gfp.h and described at
 * :ref:`Documentation/core-api/mm-api.rst <mm-api-gfp-flags>`
 *
 * The recommended usage of the @flags is described at
 * :ref:`Documentation/core-api/memory-allocation.rst <memory_allocation>`
 *
 * Below is a brief outline of the most useful GFP flags
 *
 * %GFP_KERNEL
 *	Allocate normal kernel ram. May sleep.
 *
 * %GFP_NOWAIT
 *	Allocation will not sleep.
 *
 * %GFP_ATOMIC
 *	Allocation will not sleep.  May use emergency pools.
 *
 * %GFP_HIGHUSER
 *	Allocate memory from high memory on behalf of user.
 *
 * Also it is possible to set different flags by OR'ing
 * in one or more of the following additional @flags:
 *
 * %__GFP_HIGH
 *	This allocation has high priority and may use emergency pools.
 *
 * %__GFP_NOFAIL
 *	Indicate that this allocation is in no way allowed to fail
 *	(think twice before using).
 *
 * %__GFP_NORETRY
 *	If memory is not immediately available,
 *	then give up at once.
 *
 * %__GFP_NOWARN
 *	If allocation fails, don't issue any warnings.
 *
 * %__GFP_RETRY_MAYFAIL
 *	Try really hard to succeed the allocation but fail
 *	eventually.
 */
/*
 * IAMROOT, 2022.06.25:
 * - @flags, @size에 따라 object를 할당받아온다.
 */
static __always_inline void *kmalloc(size_t size, gfp_t flags)
{
/*
 * IAMROOT, 2022.06.25:
 * - @size가 const값이면 compile 타임에 판단해서 함수를 호출한다.
 */
	if (__builtin_constant_p(size)) {
#ifndef CONFIG_SLOB
		unsigned int index;
#endif
		if (size > KMALLOC_MAX_CACHE_SIZE)
			return kmalloc_large(size, flags);
#ifndef CONFIG_SLOB
		index = kmalloc_index(size);

		if (!index)
			return ZERO_SIZE_PTR;

		return kmem_cache_alloc_trace(
				kmalloc_caches[kmalloc_type(flags)][index],
				flags, size);
#endif
	}
	return __kmalloc(size, flags);
}

static __always_inline void *kmalloc_node(size_t size, gfp_t flags, int node)
{
#ifndef CONFIG_SLOB
	if (__builtin_constant_p(size) &&
		size <= KMALLOC_MAX_CACHE_SIZE) {
		unsigned int i = kmalloc_index(size);

		if (!i)
			return ZERO_SIZE_PTR;

		return kmem_cache_alloc_node_trace(
				kmalloc_caches[kmalloc_type(flags)][i],
						flags, node, size);
	}
#endif
	return __kmalloc_node(size, flags, node);
}

/**
 * kmalloc_array - allocate memory for an array.
 * @n: number of elements.
 * @size: element size.
 * @flags: the type of memory to allocate (see kmalloc).
 */
static inline void *kmalloc_array(size_t n, size_t size, gfp_t flags)
{
	size_t bytes;

	if (unlikely(check_mul_overflow(n, size, &bytes)))
		return NULL;
	if (__builtin_constant_p(n) && __builtin_constant_p(size))
		return kmalloc(bytes, flags);
	return __kmalloc(bytes, flags);
}

/**
 * krealloc_array - reallocate memory for an array.
 * @p: pointer to the memory chunk to reallocate
 * @new_n: new number of elements to alloc
 * @new_size: new size of a single member of the array
 * @flags: the type of memory to allocate (see kmalloc)
 */
static __must_check inline void *
krealloc_array(void *p, size_t new_n, size_t new_size, gfp_t flags)
{
	size_t bytes;

	if (unlikely(check_mul_overflow(new_n, new_size, &bytes)))
		return NULL;

	return krealloc(p, bytes, flags);
}

/**
 * kcalloc - allocate memory for an array. The memory is set to zero.
 * @n: number of elements.
 * @size: element size.
 * @flags: the type of memory to allocate (see kmalloc).
 */
static inline void *kcalloc(size_t n, size_t size, gfp_t flags)
{
	return kmalloc_array(n, size, flags | __GFP_ZERO);
}

/*
 * kmalloc_track_caller is a special version of kmalloc that records the
 * calling function of the routine calling it for slab leak tracking instead
 * of just the calling function (confusing, eh?).
 * It's useful when the call to kmalloc comes from a widely-used standard
 * allocator where we care about the real place the memory allocation
 * request comes from.
 */
extern void *__kmalloc_track_caller(size_t, gfp_t, unsigned long);
#define kmalloc_track_caller(size, flags) \
	__kmalloc_track_caller(size, flags, _RET_IP_)

static inline void *kmalloc_array_node(size_t n, size_t size, gfp_t flags,
				       int node)
{
	size_t bytes;

	if (unlikely(check_mul_overflow(n, size, &bytes)))
		return NULL;
	if (__builtin_constant_p(n) && __builtin_constant_p(size))
		return kmalloc_node(bytes, flags, node);
	return __kmalloc_node(bytes, flags, node);
}

static inline void *kcalloc_node(size_t n, size_t size, gfp_t flags, int node)
{
	return kmalloc_array_node(n, size, flags | __GFP_ZERO, node);
}


#ifdef CONFIG_NUMA
extern void *__kmalloc_node_track_caller(size_t, gfp_t, int, unsigned long);
#define kmalloc_node_track_caller(size, flags, node) \
	__kmalloc_node_track_caller(size, flags, node, \
			_RET_IP_)

#else /* CONFIG_NUMA */

#define kmalloc_node_track_caller(size, flags, node) \
	kmalloc_track_caller(size, flags)

#endif /* CONFIG_NUMA */

/*
 * Shortcuts
 */
/*
 * IAMROOT, 2022.06.18:
 * - flags + __GFP_ZERO, all node.
 *   slab object를 memset하여 할당받는다.
 */
static inline void *kmem_cache_zalloc(struct kmem_cache *k, gfp_t flags)
{
	return kmem_cache_alloc(k, flags | __GFP_ZERO);
}

/**
 * kzalloc - allocate memory. The memory is set to zero.
 * @size: how many bytes of memory are required.
 * @flags: the type of memory to allocate (see kmalloc).
 */
static inline void *kzalloc(size_t size, gfp_t flags)
{
	return kmalloc(size, flags | __GFP_ZERO);
}

/**
 * kzalloc_node - allocate zeroed memory from a particular memory node.
 * @size: how many bytes of memory are required.
 * @flags: the type of memory to allocate (see kmalloc).
 * @node: memory node from which to allocate
 */
static inline void *kzalloc_node(size_t size, gfp_t flags, int node)
{
	return kmalloc_node(size, flags | __GFP_ZERO, node);
}

unsigned int kmem_cache_size(struct kmem_cache *s);
void __init kmem_cache_init_late(void);

#if defined(CONFIG_SMP) && defined(CONFIG_SLAB)
int slab_prepare_cpu(unsigned int cpu);
int slab_dead_cpu(unsigned int cpu);
#else
#define slab_prepare_cpu	NULL
#define slab_dead_cpu		NULL
#endif

#endif	/* _LINUX_SLAB_H */
