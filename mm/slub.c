// SPDX-License-Identifier: GPL-2.0
/*
 * SLUB: A slab allocator that limits cache line use instead of queuing
 * objects in per cpu and per node lists.
 *
 * The allocator synchronizes using per slab locks or atomic operations
 * and only uses a centralized lock to manage a pool of partial slabs.
 *
 * (C) 2007 SGI, Christoph Lameter
 * (C) 2011 Linux Foundation, Christoph Lameter
 */

#include <linux/mm.h>
#include <linux/swap.h> /* struct reclaim_state */
#include <linux/module.h>
#include <linux/bit_spinlock.h>
#include <linux/interrupt.h>
#include <linux/swab.h>
#include <linux/bitops.h>
#include <linux/slab.h>
#include "slab.h"
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/kasan.h>
#include <linux/cpu.h>
#include <linux/cpuset.h>
#include <linux/mempolicy.h>
#include <linux/ctype.h>
#include <linux/debugobjects.h>
#include <linux/kallsyms.h>
#include <linux/kfence.h>
#include <linux/memory.h>
#include <linux/math64.h>
#include <linux/fault-inject.h>
#include <linux/stacktrace.h>
#include <linux/prefetch.h>
#include <linux/memcontrol.h>
#include <linux/random.h>
#include <kunit/test.h>

#include <linux/debugfs.h>
#include <trace/events/kmem.h>

#include "internal.h"

/*
 * Lock order:
 *   1. slab_mutex (Global Mutex)
 *   2. node->list_lock (Spinlock)
 *   3. kmem_cache->cpu_slab->lock (Local lock)
 *   4. slab_lock(page) (Only on some arches or for debugging)
 *   5. object_map_lock (Only for debugging)
 *
 *   slab_mutex
 *
 *   The role of the slab_mutex is to protect the list of all the slabs
 *   and to synchronize major metadata changes to slab cache structures.
 *   Also synchronizes memory hotplug callbacks.
 *
 *   slab_lock
 *
 *   The slab_lock is a wrapper around the page lock, thus it is a bit
 *   spinlock.
 *
 *   The slab_lock is only used for debugging and on arches that do not
 *   have the ability to do a cmpxchg_double. It only protects:
 *	A. page->freelist	-> List of object free in a page
 *	B. page->inuse		-> Number of objects in use
 *	C. page->objects	-> Number of objects in page
 *	D. page->frozen		-> frozen state
 *
 *   Frozen slabs
 *
 *   If a slab is frozen then it is exempt from list management. It is not
 *   on any list except per cpu partial list. The processor that froze the
 *   slab is the one who can perform list operations on the page. Other
 *   processors may put objects onto the freelist but the processor that
 *   froze the slab is the only one that can retrieve the objects from the
 *   page's freelist.
 *
 *   list_lock
 *
 *   The list_lock protects the partial and full list on each node and
 *   the partial slab counter. If taken then no new slabs may be added or
 *   removed from the lists nor make the number of partial slabs be modified.
 *   (Note that the total number of slabs is an atomic value that may be
 *   modified without taking the list lock).
 *
 *   The list_lock is a centralized lock and thus we avoid taking it as
 *   much as possible. As long as SLUB does not have to handle partial
 *   slabs, operations can continue without any centralized lock. F.e.
 *   allocating a long series of objects that fill up slabs does not require
 *   the list lock.
 *
 *   cpu_slab->lock local lock
 *
 *   This locks protect slowpath manipulation of all kmem_cache_cpu fields
 *   except the stat counters. This is a percpu structure manipulated only by
 *   the local cpu, so the lock protects against being preempted or interrupted
 *   by an irq. Fast path operations rely on lockless operations instead.
 *   On PREEMPT_RT, the local lock does not actually disable irqs (and thus
 *   prevent the lockless operations), so fastpath operations also need to take
 *   the lock and are no longer lockless.
 *
 *   lockless fastpaths
 *
 *   The fast path allocation (slab_alloc_node()) and freeing (do_slab_free())
 *   are fully lockless when satisfied from the percpu slab (and when
 *   cmpxchg_double is possible to use, otherwise slab_lock is taken).
 *   They also don't disable preemption or migration or irqs. They rely on
 *   the transaction id (tid) field to detect being preempted or moved to
 *   another cpu.
 *
 *   irq, preemption, migration considerations
 *
 *   Interrupts are disabled as part of list_lock or local_lock operations, or
 *   around the slab_lock operation, in order to make the slab allocator safe
 *   to use in the context of an irq.
 *
 *   In addition, preemption (or migration on PREEMPT_RT) is disabled in the
 *   allocation slowpath, bulk allocation, and put_cpu_partial(), so that the
 *   local cpu doesn't change in the process and e.g. the kmem_cache_cpu pointer
 *   doesn't have to be revalidated in each section protected by the local lock.
 *
 * SLUB assigns one slab for allocation to each processor.
 * Allocations only occur from these slabs called cpu slabs.
 *
 * Slabs with free elements are kept on a partial list and during regular
 * operations no list for full slabs is used. If an object in a full slab is
 * freed then the slab will show up again on the partial lists.
 * We track full slabs for debugging purposes though because otherwise we
 * cannot scan all objects.
 *
 * Slabs are freed when they become empty. Teardown and setup is
 * minimal so we rely on the page allocators per cpu caches for
 * fast frees and allocs.
 *
 * page->frozen		The slab is frozen and exempt from list processing.
 * 			This means that the slab is dedicated to a purpose
 * 			such as satisfying allocations for a specific
 * 			processor. Objects may be freed in the slab while
 * 			it is frozen but slab_free will then skip the usual
 * 			list operations. It is up to the processor holding
 * 			the slab to integrate the slab into the slab lists
 * 			when the slab is no longer needed.
 *
 * 			One use of this flag is to mark slabs that are
 * 			used for allocations. Then such a slab becomes a cpu
 * 			slab. The cpu slab may be equipped with an additional
 * 			freelist that allows lockless access to
 * 			free objects in addition to the regular freelist
 * 			that requires the slab lock.
 *
 * SLAB_DEBUG_FLAGS	Slab requires special handling due to debug
 * 			options set. This moves	slab handling out of
 * 			the fast path and disables lockless freelists.
 */

/*
 * We could simply use migrate_disable()/enable() but as long as it's a
 * function call even on !PREEMPT_RT, use inline preempt_disable() there.
 */
#ifndef CONFIG_PREEMPT_RT
#define slub_get_cpu_ptr(var)	get_cpu_ptr(var)
#define slub_put_cpu_ptr(var)	put_cpu_ptr(var)
#else
#define slub_get_cpu_ptr(var)		\
({					\
	migrate_disable();		\
	this_cpu_ptr(var);		\
})
#define slub_put_cpu_ptr(var)		\
do {					\
	(void)(var);			\
	migrate_enable();		\
} while (0)
#endif

#ifdef CONFIG_SLUB_DEBUG
#ifdef CONFIG_SLUB_DEBUG_ON
DEFINE_STATIC_KEY_TRUE(slub_debug_enabled);
#else
DEFINE_STATIC_KEY_FALSE(slub_debug_enabled);
#endif
#endif		/* CONFIG_SLUB_DEBUG */

static inline bool kmem_cache_debug(struct kmem_cache *s)
{
	return kmem_cache_debug_flags(s, SLAB_DEBUG_FLAGS);
}

/*
 * IAMROOT, 2022.06.18:
 * - redzone을 사용할경우 fp는 red_left_pad만큼 이동된 위치에 있다.
 */
void *fixup_red_left(struct kmem_cache *s, void *p)
{
	if (kmem_cache_debug_flags(s, SLAB_RED_ZONE))
		p += s->red_left_pad;

	return p;
}

static inline bool kmem_cache_has_cpu_partial(struct kmem_cache *s)
{
#ifdef CONFIG_SLUB_CPU_PARTIAL
	return !kmem_cache_debug(s);
#else
	return false;
#endif
}

/*
 * Issues still to be resolved:
 *
 * - Support PAGE_ALLOC_DEBUG. Should be easy to do.
 *
 * - Variable sizing of the per node arrays
 */

/* Enable to log cmpxchg failures */
#undef SLUB_DEBUG_CMPXCHG

/*
 * Minimum number of partial slabs. These will be left on the partial
 * lists even if they are empty. kmem_cache_shrink may reclaim them.
 */
#define MIN_PARTIAL 5

/*
 * Maximum number of desirable partial slabs.
 * The existence of more partial slabs makes kmem_cache_shrink
 * sort the partial list by the number of objects in use.
 */
#define MAX_PARTIAL 10

#define DEBUG_DEFAULT_FLAGS (SLAB_CONSISTENCY_CHECKS | SLAB_RED_ZONE | \
				SLAB_POISON | SLAB_STORE_USER)

/*
 * These debug flags cannot use CMPXCHG because there might be consistency
 * issues when checking or reading debug information
 */
#define SLAB_NO_CMPXCHG (SLAB_CONSISTENCY_CHECKS | SLAB_STORE_USER | \
				SLAB_TRACE)


/*
 * Debugging flags that require metadata to be stored in the slab.  These get
 * disabled when slub_debug=O is used and a cache's min order increases with
 * metadata.
 */
#define DEBUG_METADATA_FLAGS (SLAB_RED_ZONE | SLAB_POISON | SLAB_STORE_USER)

#define OO_SHIFT	16
#define OO_MASK		((1 << OO_SHIFT) - 1)
#define MAX_OBJS_PER_PAGE	32767 /* since page.objects is u15 */

/* Internal SLUB flags */
/* Poison object */
#define __OBJECT_POISON		((slab_flags_t __force)0x80000000U)
/* Use cmpxchg_double */
#define __CMPXCHG_DOUBLE	((slab_flags_t __force)0x40000000U)

/*
 * Tracking user of a slab.
 */
#define TRACK_ADDRS_COUNT 16

/*
 * IAMROOT, 2022.06.18:
 * - object를 누가 호출해서 할당/해제 했는지를 추적한다.
 *   할당용 1개, 해제용 1개를 쓴다.
 */
struct track {
	unsigned long addr;	/* Called from address */
#ifdef CONFIG_STACKTRACE
	unsigned long addrs[TRACK_ADDRS_COUNT];	/* Called from address */
#endif
	int cpu;		/* Was running on cpu */
	int pid;		/* Pid context */
	unsigned long when;	/* When did the operation occur */
};

enum track_item { TRACK_ALLOC, TRACK_FREE };

#ifdef CONFIG_SYSFS
static int sysfs_slab_add(struct kmem_cache *);
static int sysfs_slab_alias(struct kmem_cache *, const char *);
#else
static inline int sysfs_slab_add(struct kmem_cache *s) { return 0; }
static inline int sysfs_slab_alias(struct kmem_cache *s, const char *p)
							{ return 0; }
#endif

#if defined(CONFIG_DEBUG_FS) && defined(CONFIG_SLUB_DEBUG)
static void debugfs_slab_add(struct kmem_cache *);
#else
static inline void debugfs_slab_add(struct kmem_cache *s) { }
#endif

static inline void stat(const struct kmem_cache *s, enum stat_item si)
{
#ifdef CONFIG_SLUB_STATS
	/*
	 * The rmw is racy on a preemptible kernel but this is acceptable, so
	 * avoid this_cpu_add()'s irq-disable overhead.
	 */
	raw_cpu_inc(s->cpu_slab->stat[si]);
#endif
}

/*
 * Tracks for which NUMA nodes we have kmem_cache_nodes allocated.
 * Corresponds to node_state[N_NORMAL_MEMORY], but can temporarily
 * differ during memory hotplug/hotremove operations.
 * Protected by slab_mutex.
 */
/*
 * IAMROOT, 2022.06.18:
 * - memory에 있는 node.
 */
static nodemask_t slab_nodes;

/********************************************************************
 * 			Core slab cache functions
 *******************************************************************/

/*
 * Returns freelist pointer (ptr). With hardening, this is obfuscated
 * with an XOR of the address where the pointer is held and a per-cache
 * random number.
 */
/*
 * IAMROOT, 2022.06.18:
 * - CONFIG_SLAB_FREELIST_HARDENED가 on 일경우
 *   a@s->random값을 이용해 FP(@ptr)을 숨긴다.
 */
static inline void *freelist_ptr(const struct kmem_cache *s, void *ptr,
				 unsigned long ptr_addr)
{
#ifdef CONFIG_SLAB_FREELIST_HARDENED
	/*
	 * When CONFIG_KASAN_SW/HW_TAGS is enabled, ptr_addr might be tagged.
	 * Normally, this doesn't cause any issues, as both set_freepointer()
	 * and get_freepointer() are called with a pointer with the same tag.
	 * However, there are some issues with CONFIG_SLUB_DEBUG code. For
	 * example, when __free_slub() iterates over objects in a cache, it
	 * passes untagged pointers to check_object(). check_object() in turns
	 * calls get_freepointer() with an untagged pointer, which causes the
	 * freepointer to be restored incorrectly.
	 */
	return (void *)((unsigned long)ptr ^ s->random ^
			swab((unsigned long)kasan_reset_tag((void *)ptr_addr)));
#else
	return ptr;
#endif
}

/* Returns the freelist pointer recorded at location ptr_addr. */
/*
 * IAMROOT, 2022.06.18:
 * @ptr_addr FP.
 * hardened를 사용했을경우 관련 처리를 한다.
 */
static inline void *freelist_dereference(const struct kmem_cache *s,
					 void *ptr_addr)
{
	return freelist_ptr(s, (void *)*(unsigned long *)(ptr_addr),
			    (unsigned long)ptr_addr);
}

/*
 * IAMROOT, 2022.06.18:
 * - @object의 FP를 가져온다. hardened를 사용했을경우 관련 처리를 한다.
 */
static inline void *get_freepointer(struct kmem_cache *s, void *object)
{
	object = kasan_reset_tag(object);
	return freelist_dereference(s, object + s->offset);
}

/*
 * IAMROOT, 2022.06.25:
 * - @object의 fp를 prefetch한다.
 */
static void prefetch_freepointer(const struct kmem_cache *s, void *object)
{
	prefetch(object + s->offset);
}

/*
 * IAMROOT, 2022.06.25:
 * - debug가 꺼져있으면 그냥 get_freepointer().
 */
static inline void *get_freepointer_safe(struct kmem_cache *s, void *object)
{
	unsigned long freepointer_addr;
	void *p;

	if (!debug_pagealloc_enabled_static())
		return get_freepointer(s, object);

	object = kasan_reset_tag(object);
	freepointer_addr = (unsigned long)object + s->offset;
	copy_from_kernel_nofault(&p, (void **)freepointer_addr, sizeof(p));
	return freelist_ptr(s, p, freepointer_addr);
}

/*
 * IAMROOT, 2022.06.18:
 * - @object의 FP에 next object (@fp)를 저장한다.
 */
static inline void set_freepointer(struct kmem_cache *s, void *object, void *fp)
{
	unsigned long freeptr_addr = (unsigned long)object + s->offset;

#ifdef CONFIG_SLAB_FREELIST_HARDENED
	BUG_ON(object == fp); /* naive detection of double free or corruption */
#endif

	freeptr_addr = (unsigned long)kasan_reset_tag((void *)freeptr_addr);
	*(void **)freeptr_addr = freelist_ptr(s, fp, freeptr_addr);
}

/* Loop over all objects in a slab */
#define for_each_object(__p, __s, __addr, __objects) \
	for (__p = fixup_red_left(__s, __addr); \
		__p < (__addr) + (__objects) * (__s)->size; \
		__p += (__s)->size)

/*
 * IAMROOT, 2022.06.18:
 * - PAGE_SIZE = 4K, order == PAGE_ALLOC_COSTLY_ORDER 일때
 *   4k * 2^3 / size = return 32k / size
 */
static inline unsigned int order_objects(unsigned int order, unsigned int size)
{
	return ((unsigned int)PAGE_SIZE << order) / size;
}

/*
 * IAMROOT, 2022.06.18:
 * - @order, @size로 oo초기값을 구해온다.
 * - order제한이 MAX_OBJS_PER_PAGE(OO_SHIFT order)값이므로
 *   
 *   0          OO_SHIFT(16)      31
 *   +-------------+---------------+
 *   | object수    | order         |
 *   +-------------+---------------+
 *     oo_objects()  oo_order()
 */
static inline struct kmem_cache_order_objects oo_make(unsigned int order,
		unsigned int size)
{
	struct kmem_cache_order_objects x = {
		(order << OO_SHIFT) + order_objects(order, size)
	};

	return x;
}

static inline unsigned int oo_order(struct kmem_cache_order_objects x)
{
	return x.x >> OO_SHIFT;
}

static inline unsigned int oo_objects(struct kmem_cache_order_objects x)
{
	return x.x & OO_MASK;
}

/*
 * Per slab locking using the pagelock
 */
static __always_inline void __slab_lock(struct page *page)
{
	VM_BUG_ON_PAGE(PageTail(page), page);
	bit_spin_lock(PG_locked, &page->flags);
}

static __always_inline void __slab_unlock(struct page *page)
{
	VM_BUG_ON_PAGE(PageTail(page), page);
	__bit_spin_unlock(PG_locked, &page->flags);
}

static __always_inline void slab_lock(struct page *page, unsigned long *flags)
{
	if (IS_ENABLED(CONFIG_PREEMPT_RT))
		local_irq_save(*flags);
	__slab_lock(page);
}

static __always_inline void slab_unlock(struct page *page, unsigned long *flags)
{
	__slab_unlock(page);
	if (IS_ENABLED(CONFIG_PREEMPT_RT))
		local_irq_restore(*flags);
}

/*
 * Interrupts must be disabled (for the fallback code to work right), typically
 * by an _irqsave() lock variant. Except on PREEMPT_RT where locks are different
 * so we disable interrupts as part of slab_[un]lock().
 */

/*
 * IAMROOT, 2022.06.25:
 * - new 2개를 old 2개로 동시에 교체한다. 성공했으면 true.
 */
static inline bool __cmpxchg_double_slab(struct kmem_cache *s, struct page *page,
		void *freelist_old, unsigned long counters_old,
		void *freelist_new, unsigned long counters_new,
		const char *n)
{
	if (!IS_ENABLED(CONFIG_PREEMPT_RT))
		lockdep_assert_irqs_disabled();
#if defined(CONFIG_HAVE_CMPXCHG_DOUBLE) && \
    defined(CONFIG_HAVE_ALIGNED_STRUCT_PAGE)
/*
 * IAMROOT, 2022.06.25:
 * - arm64는 cmpxchg_double을 지원한다.
 */
	if (s->flags & __CMPXCHG_DOUBLE) {
		if (cmpxchg_double(&page->freelist, &page->counters,
				   freelist_old, counters_old,
				   freelist_new, counters_new))
			return true;
	} else
#endif
	{

/*
 * IAMROOT, 2022.06.25:
 * - debug를 위한 루틴.
 */
		/* init to 0 to prevent spurious warnings */
		unsigned long flags = 0;

		slab_lock(page, &flags);
		if (page->freelist == freelist_old &&
					page->counters == counters_old) {
			page->freelist = freelist_new;
			page->counters = counters_new;
			slab_unlock(page, &flags);
			return true;
		}
		slab_unlock(page, &flags);
	}

	cpu_relax();
	stat(s, CMPXCHG_DOUBLE_FAIL);

#ifdef SLUB_DEBUG_CMPXCHG
	pr_info("%s %s: cmpxchg double redo ", n, s->name);
#endif

	return false;
}

static inline bool cmpxchg_double_slab(struct kmem_cache *s, struct page *page,
		void *freelist_old, unsigned long counters_old,
		void *freelist_new, unsigned long counters_new,
		const char *n)
{
#if defined(CONFIG_HAVE_CMPXCHG_DOUBLE) && \
    defined(CONFIG_HAVE_ALIGNED_STRUCT_PAGE)
	if (s->flags & __CMPXCHG_DOUBLE) {
		if (cmpxchg_double(&page->freelist, &page->counters,
				   freelist_old, counters_old,
				   freelist_new, counters_new))
			return true;
	} else
#endif
	{
		unsigned long flags;

		local_irq_save(flags);
		__slab_lock(page);
		if (page->freelist == freelist_old &&
					page->counters == counters_old) {
			page->freelist = freelist_new;
			page->counters = counters_new;
			__slab_unlock(page);
			local_irq_restore(flags);
			return true;
		}
		__slab_unlock(page);
		local_irq_restore(flags);
	}

	cpu_relax();
	stat(s, CMPXCHG_DOUBLE_FAIL);

#ifdef SLUB_DEBUG_CMPXCHG
	pr_info("%s %s: cmpxchg double redo ", n, s->name);
#endif

	return false;
}

#ifdef CONFIG_SLUB_DEBUG
static unsigned long object_map[BITS_TO_LONGS(MAX_OBJS_PER_PAGE)];
static DEFINE_RAW_SPINLOCK(object_map_lock);

static void __fill_map(unsigned long *obj_map, struct kmem_cache *s,
		       struct page *page)
{
	void *addr = page_address(page);
	void *p;

	bitmap_zero(obj_map, page->objects);

	for (p = page->freelist; p; p = get_freepointer(s, p))
		set_bit(__obj_to_index(s, addr, p), obj_map);
}

#if IS_ENABLED(CONFIG_KUNIT)
static bool slab_add_kunit_errors(void)
{
	struct kunit_resource *resource;

	if (likely(!current->kunit_test))
		return false;

	resource = kunit_find_named_resource(current->kunit_test, "slab_errors");
	if (!resource)
		return false;

	(*(int *)resource->data)++;
	kunit_put_resource(resource);
	return true;
}
#else
static inline bool slab_add_kunit_errors(void) { return false; }
#endif

/*
 * Determine a map of object in use on a page.
 *
 * Node listlock must be held to guarantee that the page does
 * not vanish from under us.
 */
static unsigned long *get_map(struct kmem_cache *s, struct page *page)
	__acquires(&object_map_lock)
{
	VM_BUG_ON(!irqs_disabled());

	raw_spin_lock(&object_map_lock);

	__fill_map(object_map, s, page);

	return object_map;
}

static void put_map(unsigned long *map) __releases(&object_map_lock)
{
	VM_BUG_ON(map != object_map);
	raw_spin_unlock(&object_map_lock);
}

static inline unsigned int size_from_object(struct kmem_cache *s)
{
	if (s->flags & SLAB_RED_ZONE)
		return s->size - s->red_left_pad;

	return s->size;
}

static inline void *restore_red_left(struct kmem_cache *s, void *p)
{
	if (s->flags & SLAB_RED_ZONE)
		p -= s->red_left_pad;

	return p;
}

/*
 * Debug settings:
 */
#if defined(CONFIG_SLUB_DEBUG_ON)
static slab_flags_t slub_debug = DEBUG_DEFAULT_FLAGS;
#else
static slab_flags_t slub_debug;
#endif

static char *slub_debug_string;

/*
 * IAMROOT, 2022.06.25:
 * - high order debug 정보는 용량을 많이 차지 하므로 거기에 대한
 *   disable 처리(o option)
 */
static int disable_higher_order_debug;

/*
 * slub is about to manipulate internal object metadata.  This memory lies
 * outside the range of the allocated object, so accessing it would normally
 * be reported by kasan as a bounds error.  metadata_access_enable() is used
 * to tell kasan that these accesses are OK.
 */
static inline void metadata_access_enable(void)
{
	kasan_disable_current();
}

static inline void metadata_access_disable(void)
{
	kasan_enable_current();
}

/*
 * Object debugging
 */

/* Verify that a pointer has an address that is valid within a slab page */
/*
 * IAMROOT, 2022.06.18:
 * - 주소값들이 정상적으로 있는지 검사.
 *   @object가 slab page안에 들어가는지 확인다.
 */
static inline int check_valid_pointer(struct kmem_cache *s,
				struct page *page, void *object)
{
	void *base;

	if (!object)
		return 1;

	base = page_address(page);
	object = kasan_reset_tag(object);
	object = restore_red_left(s, object);
	if (object < base || object >= base + page->objects * s->size ||
		(object - base) % s->size) {
		return 0;
	}

	return 1;
}

static void print_section(char *level, char *text, u8 *addr,
			  unsigned int length)
{
	metadata_access_enable();
	print_hex_dump(level, text, DUMP_PREFIX_ADDRESS,
			16, 1, kasan_reset_tag((void *)addr), length, 1);
	metadata_access_disable();
}

/*
 * See comment in calculate_sizes().
 */
/*
 * IAMROOT, 2022.06.18:
 * - FP가 object 밖에 있는지 확인한다.(rcu, posion사용시 FP를 바깥에 빼놓는데
 *   그 여부를 확인하는것).
 */
static inline bool freeptr_outside_object(struct kmem_cache *s)
{
	return s->offset >= s->inuse;
}

/*
 * Return offset of the end of info block which is inuse + free pointer if
 * not overlapping with object.
 */
static inline unsigned int get_info_end(struct kmem_cache *s)
{
	if (freeptr_outside_object(s))
		return s->inuse + sizeof(void *);
	else
		return s->inuse;
}

static struct track *get_track(struct kmem_cache *s, void *object,
	enum track_item alloc)
{
	struct track *p;

	p = object + get_info_end(s);

	return kasan_reset_tag(p + alloc);
}

/*
 * IAMROOT, 2022.06.18:
 * - 초기화때는 @addr = 0이여서 memset될것이다.
 */
static void set_track(struct kmem_cache *s, void *object,
			enum track_item alloc, unsigned long addr)
{
	struct track *p = get_track(s, object, alloc);

	if (addr) {
#ifdef CONFIG_STACKTRACE
		unsigned int nr_entries;

		metadata_access_enable();
		nr_entries = stack_trace_save(kasan_reset_tag(p->addrs),
					      TRACK_ADDRS_COUNT, 3);
		metadata_access_disable();

		if (nr_entries < TRACK_ADDRS_COUNT)
			p->addrs[nr_entries] = 0;
#endif
		p->addr = addr;
		p->cpu = smp_processor_id();
		p->pid = current->pid;
		p->when = jiffies;
	} else {
		memset(p, 0, sizeof(struct track));
	}
}

/*
 * IAMROOT, 2022.06.18:
 * - alloc, free track을 초기화한다.
 */
static void init_tracking(struct kmem_cache *s, void *object)
{
	if (!(s->flags & SLAB_STORE_USER))
		return;

	set_track(s, object, TRACK_FREE, 0UL);
	set_track(s, object, TRACK_ALLOC, 0UL);
}

static void print_track(const char *s, struct track *t, unsigned long pr_time)
{
	if (!t->addr)
		return;

	pr_err("%s in %pS age=%lu cpu=%u pid=%d\n",
	       s, (void *)t->addr, pr_time - t->when, t->cpu, t->pid);
#ifdef CONFIG_STACKTRACE
	{
		int i;
		for (i = 0; i < TRACK_ADDRS_COUNT; i++)
			if (t->addrs[i])
				pr_err("\t%pS\n", (void *)t->addrs[i]);
			else
				break;
	}
#endif
}

void print_tracking(struct kmem_cache *s, void *object)
{
	unsigned long pr_time = jiffies;
	if (!(s->flags & SLAB_STORE_USER))
		return;

	print_track("Allocated", get_track(s, object, TRACK_ALLOC), pr_time);
	print_track("Freed", get_track(s, object, TRACK_FREE), pr_time);
}

static void print_page_info(struct page *page)
{
	pr_err("Slab 0x%p objects=%u used=%u fp=0x%p flags=%#lx(%pGp)\n",
	       page, page->objects, page->inuse, page->freelist,
	       page->flags, &page->flags);

}

static void slab_bug(struct kmem_cache *s, char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	pr_err("=============================================================================\n");
	pr_err("BUG %s (%s): %pV\n", s->name, print_tainted(), &vaf);
	pr_err("-----------------------------------------------------------------------------\n\n");
	va_end(args);
}

__printf(2, 3)
static void slab_fix(struct kmem_cache *s, char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	if (slab_add_kunit_errors())
		return;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	pr_err("FIX %s: %pV\n", s->name, &vaf);
	va_end(args);
}

static bool freelist_corrupted(struct kmem_cache *s, struct page *page,
			       void **freelist, void *nextfree)
{
	if ((s->flags & SLAB_CONSISTENCY_CHECKS) &&
	    !check_valid_pointer(s, page, nextfree) && freelist) {
		object_err(s, page, *freelist, "Freechain corrupt");
		*freelist = NULL;
		slab_fix(s, "Isolate corrupted freechain");
		return true;
	}

	return false;
}

static void print_trailer(struct kmem_cache *s, struct page *page, u8 *p)
{
	unsigned int off;	/* Offset of last byte */
	u8 *addr = page_address(page);

	print_tracking(s, p);

	print_page_info(page);

	pr_err("Object 0x%p @offset=%tu fp=0x%p\n\n",
	       p, p - addr, get_freepointer(s, p));

	if (s->flags & SLAB_RED_ZONE)
		print_section(KERN_ERR, "Redzone  ", p - s->red_left_pad,
			      s->red_left_pad);
	else if (p > addr + 16)
		print_section(KERN_ERR, "Bytes b4 ", p - 16, 16);

	print_section(KERN_ERR,         "Object   ", p,
		      min_t(unsigned int, s->object_size, PAGE_SIZE));
	if (s->flags & SLAB_RED_ZONE)
		print_section(KERN_ERR, "Redzone  ", p + s->object_size,
			s->inuse - s->object_size);

	off = get_info_end(s);

	if (s->flags & SLAB_STORE_USER)
		off += 2 * sizeof(struct track);

	off += kasan_metadata_size(s);

	if (off != size_from_object(s))
		/* Beginning of the filler is the free pointer */
		print_section(KERN_ERR, "Padding  ", p + off,
			      size_from_object(s) - off);

	dump_stack();
}

void object_err(struct kmem_cache *s, struct page *page,
			u8 *object, char *reason)
{
	if (slab_add_kunit_errors())
		return;

	slab_bug(s, "%s", reason);
	print_trailer(s, page, object);
	add_taint(TAINT_BAD_PAGE, LOCKDEP_NOW_UNRELIABLE);
}

static __printf(3, 4) void slab_err(struct kmem_cache *s, struct page *page,
			const char *fmt, ...)
{
	va_list args;
	char buf[100];

	if (slab_add_kunit_errors())
		return;

	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	slab_bug(s, "%s", buf);
	print_page_info(page);
	dump_stack();
	add_taint(TAINT_BAD_PAGE, LOCKDEP_NOW_UNRELIABLE);
}

/*
 * IAMROOT, 2022.06.18:
 * - redzone, poison 값을 기록한다.
 */
static void init_object(struct kmem_cache *s, void *object, u8 val)
{
	u8 *p = kasan_reset_tag(object);

/*
 * IAMROOT, 2022.06.18:
 *
 *            <----- inuse ----------->
 * +----------+-----------------+-----+
 * |    Z     | object          |  Z  |
 * +----------+-----------------+-----+
 * <--val---->p                  <-val->
 * SLUB_RED_INACTIVE, SLUB_RED_ACTIVE
 */
	if (s->flags & SLAB_RED_ZONE)
		memset(p - s->red_left_pad, val, s->red_left_pad);

/*
 * IAMROOT, 2022.06.18:
 *
 * +-----------------++
 * | object          ||
 * +-----------------++
 * p<--POISON_FREE-->^POISON_END(마지막 byte만)
 */
	if (s->flags & __OBJECT_POISON) {
		memset(p, POISON_FREE, s->object_size - 1);
		p[s->object_size - 1] = POISON_END;
	}

	if (s->flags & SLAB_RED_ZONE)
		memset(p + s->object_size, val, s->inuse - s->object_size);
}

static void restore_bytes(struct kmem_cache *s, char *message, u8 data,
						void *from, void *to)
{
	slab_fix(s, "Restoring %s 0x%p-0x%p=0x%x", message, from, to - 1, data);
	memset(from, data, to - from);
}

static int check_bytes_and_report(struct kmem_cache *s, struct page *page,
			u8 *object, char *what,
			u8 *start, unsigned int value, unsigned int bytes)
{
	u8 *fault;
	u8 *end;
	u8 *addr = page_address(page);

	metadata_access_enable();
	fault = memchr_inv(kasan_reset_tag(start), value, bytes);
	metadata_access_disable();
	if (!fault)
		return 1;

	end = start + bytes;
	while (end > fault && end[-1] == value)
		end--;

	if (slab_add_kunit_errors())
		goto skip_bug_print;

	slab_bug(s, "%s overwritten", what);
	pr_err("0x%p-0x%p @offset=%tu. First byte 0x%x instead of 0x%x\n",
					fault, end - 1, fault - addr,
					fault[0], value);
	print_trailer(s, page, object);
	add_taint(TAINT_BAD_PAGE, LOCKDEP_NOW_UNRELIABLE);

skip_bug_print:
	restore_bytes(s, what, value, fault, end);
	return 0;
}

/*
 * Object layout:
 *
 * object address
 * 	Bytes of the object to be managed.
 * 	If the freepointer may overlay the object then the free
 *	pointer is at the middle of the object.
 *
 * 	Poisoning uses 0x6b (POISON_FREE) and the last byte is
 * 	0xa5 (POISON_END)
 *
 * object + s->object_size
 * 	Padding to reach word boundary. This is also used for Redzoning.
 * 	Padding is extended by another word if Redzoning is enabled and
 * 	object_size == inuse.
 *
 * 	We fill with 0xbb (RED_INACTIVE) for inactive objects and with
 * 	0xcc (RED_ACTIVE) for objects in use.
 *
 * object + s->inuse
 * 	Meta data starts here.
 *
 * 	A. Free pointer (if we cannot overwrite object on free)
 * 	B. Tracking data for SLAB_STORE_USER
 *	C. Padding to reach required alignment boundary or at minimum
 * 		one word if debugging is on to be able to detect writes
 * 		before the word boundary.
 *
 *	Padding is done using 0x5a (POISON_INUSE)
 *
 * object + s->size
 * 	Nothing is used beyond s->size.
 *
 * If slabcaches are merged then the object_size and inuse boundaries are mostly
 * ignored. And therefore no slab options that rely on these boundaries
 * may be used with merged slabcaches.
 */
/*
 * IAMROOT, 2022.06.18:
 * - 남아있는 영역에 POISON_INUSE값이 있는지 검사.
 */
static int check_pad_bytes(struct kmem_cache *s, struct page *page, u8 *p)
{
	unsigned long off = get_info_end(s);	/* The end of info */

	if (s->flags & SLAB_STORE_USER)
		/* We also have user information there */
		off += 2 * sizeof(struct track);

	off += kasan_metadata_size(s);

	if (size_from_object(s) == off)
		return 1;

	return check_bytes_and_report(s, page, p, "Object padding",
			p + off, POISON_INUSE, size_from_object(s) - off);
}

/* Check the pad bytes at the end of a slab page */
/*
 * IAMROOT, 2022.06.18:
 * - pad공간의 침해여부 검사.(poison check.) error 출력 및 pad값 복구만 해준다.
 */
static int slab_pad_check(struct kmem_cache *s, struct page *page)
{
	u8 *start;
	u8 *fault;
	u8 *end;
	u8 *pad;
	int length;
	int remainder;

	if (!(s->flags & SLAB_POISON))
		return 1;

	start = page_address(page);
	length = page_size(page);
	end = start + length;
	remainder = length % s->size;
	if (!remainder)
		return 1;

	pad = end - remainder;
	metadata_access_enable();
	fault = memchr_inv(kasan_reset_tag(pad), POISON_INUSE, remainder);
	metadata_access_disable();
	if (!fault)
		return 1;
	while (end > fault && end[-1] == POISON_INUSE)
		end--;

	slab_err(s, page, "Padding overwritten. 0x%p-0x%p @offset=%tu",
			fault, end - 1, fault - start);
	print_section(KERN_ERR, "Padding ", pad, remainder);

	restore_bytes(s, "slab padding", POISON_INUSE, fault, end);
	return 0;
}

/*
 * IAMROOT, 2022.06.18:
 * - object에 대해서 검사.
 */
static int check_object(struct kmem_cache *s, struct page *page,
					void *object, u8 val)
{
	u8 *p = object;
	u8 *endobject = object + s->object_size;

	if (s->flags & SLAB_RED_ZONE) {
/*
 * IAMROOT, 2022.06.18:
 * - 각각 redzone영역에 val값이 있는지 검사.
 */
		if (!check_bytes_and_report(s, page, object, "Left Redzone",
			object - s->red_left_pad, val, s->red_left_pad))
			return 0;

		if (!check_bytes_and_report(s, page, object, "Right Redzone",
			endobject, val, s->inuse - s->object_size))
			return 0;
	} else {
/*
 * IAMROOT, 2022.06.18:
 * POISON_INUSE이 padding 자리에 있는지 검사.
 */
		if ((s->flags & SLAB_POISON) && s->object_size < s->inuse) {
			check_bytes_and_report(s, page, p, "Alignment padding",
				endobject, POISON_INUSE,
				s->inuse - s->object_size);
		}
	}

	if (s->flags & SLAB_POISON) {
		if (val != SLUB_RED_ACTIVE && (s->flags & __OBJECT_POISON) &&
/*
 * IAMROOT, 2022.06.18:
 * - object에 POISON_FREE, 마지막 byte에 POISON_END값이 있는지 검사.
 */
			(!check_bytes_and_report(s, page, p, "Poison", p,
					POISON_FREE, s->object_size - 1) ||
			 !check_bytes_and_report(s, page, p, "End Poison",
				p + s->object_size - 1, POISON_END, 1)))
			return 0;
		/*
		 * check_pad_bytes cleans up on its own.
		 */
		check_pad_bytes(s, page, p);
	}

/*
 * IAMROOT, 2022.06.18:
 * - 사용중인 object는 FP가 없다. FP check를 안한다.
 */
	if (!freeptr_outside_object(s) && val == SLUB_RED_ACTIVE)
		/*
		 * Object and freepointer overlap. Cannot check
		 * freepointer while object is allocated.
		 */
		return 1;

	/* Check free pointer validity */
/*
 * IAMROOT, 2022.06.18:
 * - FP가 slab page 영역내에 있는지 검사한다.
 */
	if (!check_valid_pointer(s, page, get_freepointer(s, p))) {
		object_err(s, page, p, "Freepointer corrupt");
		/*
		 * No choice but to zap it and thus lose the remainder
		 * of the free objects in this slab. May cause
		 * another error because the object count is now wrong.
		 */
		set_freepointer(s, p, NULL);
		return 0;
	}
	return 1;
}

/*
 * IAMROOT, 2022.06.18:
 * - 1. Slab flag가 있는지 검사.
 *   2. page->object가 계산된 object보다 큰지 검사.
 *   3. page->inuse가 계산된 object보다 큰지 검사.
 *   4. slab pad공간(remainder)에 대한 posion 검사.
 */
static int check_slab(struct kmem_cache *s, struct page *page)
{
	int maxobj;

	if (!PageSlab(page)) {
		slab_err(s, page, "Not a valid slab page");
		return 0;
	}

	maxobj = order_objects(compound_order(page), s->size);
	if (page->objects > maxobj) {
		slab_err(s, page, "objects %u > max %u",
			page->objects, maxobj);
		return 0;
	}
	if (page->inuse > page->objects) {
		slab_err(s, page, "inuse %u > max %u",
			page->inuse, page->objects);
		return 0;
	}
	/* Slab_pad_check fixes things up after itself */
	slab_pad_check(s, page);
	return 1;
}

/*
 * Determine if a certain object on a page is on the freelist. Must hold the
 * slab lock to guarantee that the chains are in a consistent state.
 */
static int on_freelist(struct kmem_cache *s, struct page *page, void *search)
{
	int nr = 0;
	void *fp;
	void *object = NULL;
	int max_objects;

	fp = page->freelist;
	while (fp && nr <= page->objects) {
		if (fp == search)
			return 1;
		if (!check_valid_pointer(s, page, fp)) {
			if (object) {
				object_err(s, page, object,
					"Freechain corrupt");
				set_freepointer(s, object, NULL);
			} else {
				slab_err(s, page, "Freepointer corrupt");
				page->freelist = NULL;
				page->inuse = page->objects;
				slab_fix(s, "Freelist cleared");
				return 0;
			}
			break;
		}
		object = fp;
		fp = get_freepointer(s, object);
		nr++;
	}

	max_objects = order_objects(compound_order(page), s->size);
	if (max_objects > MAX_OBJS_PER_PAGE)
		max_objects = MAX_OBJS_PER_PAGE;

	if (page->objects != max_objects) {
		slab_err(s, page, "Wrong number of objects. Found %d but should be %d",
			 page->objects, max_objects);
		page->objects = max_objects;
		slab_fix(s, "Number of objects adjusted");
	}
	if (page->inuse != page->objects - nr) {
		slab_err(s, page, "Wrong object count. Counter is %d but counted were %d",
			 page->inuse, page->objects - nr);
		page->inuse = page->objects - nr;
		slab_fix(s, "Object count adjusted");
	}
	return search == NULL;
}

/*
 * IAMROOT, 2022.06.18:
 * - SLAB_TRACE가 있을경우 alloc시 pr_info 출력.
 */
static void trace(struct kmem_cache *s, struct page *page, void *object,
								int alloc)
{
	if (s->flags & SLAB_TRACE) {
		pr_info("TRACE %s %s 0x%p inuse=%d fp=0x%p\n",
			s->name,
			alloc ? "alloc" : "free",
			object, page->inuse,
			page->freelist);

		if (!alloc)
			print_section(KERN_INFO, "Object ", (void *)object,
					s->object_size);

		dump_stack();
	}
}

/*
 * Tracking of fully allocated slabs for debugging purposes.
 */
static void add_full(struct kmem_cache *s,
	struct kmem_cache_node *n, struct page *page)
{
	if (!(s->flags & SLAB_STORE_USER))
		return;

	lockdep_assert_held(&n->list_lock);
	list_add(&page->slab_list, &n->full);
}

static void remove_full(struct kmem_cache *s, struct kmem_cache_node *n, struct page *page)
{
	if (!(s->flags & SLAB_STORE_USER))
		return;

	lockdep_assert_held(&n->list_lock);
	list_del(&page->slab_list);
}

/* Tracking of the number of slabs for debugging purposes */
static inline unsigned long slabs_node(struct kmem_cache *s, int node)
{
	struct kmem_cache_node *n = get_node(s, node);

	return atomic_long_read(&n->nr_slabs);
}

static inline unsigned long node_nr_slabs(struct kmem_cache_node *n)
{
	return atomic_long_read(&n->nr_slabs);
}

static inline void inc_slabs_node(struct kmem_cache *s, int node, int objects)
{
	struct kmem_cache_node *n = get_node(s, node);

	/*
	 * May be called early in order to allocate a slab for the
	 * kmem_cache_node structure. Solve the chicken-egg
	 * dilemma by deferring the increment of the count during
	 * bootstrap (see early_kmem_cache_node_alloc).
	 */
	if (likely(n)) {
		atomic_long_inc(&n->nr_slabs);
		atomic_long_add(objects, &n->total_objects);
	}
}

/*
 * IAMROOT, 2022.06.25:
 * - dec slabs 통계
 */
static inline void dec_slabs_node(struct kmem_cache *s, int node, int objects)
{
	struct kmem_cache_node *n = get_node(s, node);

	atomic_long_dec(&n->nr_slabs);
	atomic_long_sub(objects, &n->total_objects);
}

/* Object debug checks for alloc/free paths */
/*
 * IAMROOT, 2022.06.18:
 * - redzone, poison, user track을 초기화한다.
 */
static void setup_object_debug(struct kmem_cache *s, struct page *page,
								void *object)
{
	if (!kmem_cache_debug_flags(s, SLAB_STORE_USER|SLAB_RED_ZONE|__OBJECT_POISON))
		return;

	init_object(s, object, SLUB_RED_INACTIVE);
	init_tracking(s, object);
}

static
void setup_page_debug(struct kmem_cache *s, struct page *page, void *addr)
{
	if (!kmem_cache_debug_flags(s, SLAB_POISON))
		return;

	metadata_access_enable();
	memset(kasan_reset_tag(addr), POISON_INUSE, page_size(page));
	metadata_access_disable();
}

/*
 * IAMROOT, 2022.06.18:
 * - 1. Slab flag가 있는지 검사.
 *   2. page->object가 계산된 object보다 큰지 검사.
 *   3. page->inuse가 계산된 object보다 큰지 검사.
 *   4. slab pad공간(remainder)에 대한 posion 검사.
 *   5. object(FP)가 slab page 안에 있는지 검사.
 *   6. object에 대해서 검사.
 *
 * - alloc일때 여러 검사를 수행한다.
 */
static inline int alloc_consistency_checks(struct kmem_cache *s,
					struct page *page, void *object)
{
	if (!check_slab(s, page))
		return 0;

	if (!check_valid_pointer(s, page, object)) {
		object_err(s, page, object, "Freelist Pointer check fails");
		return 0;
	}

	if (!check_object(s, page, object, SLUB_RED_INACTIVE))
		return 0;

	return 1;
}

/*
 * IAMROOT, 2022.06.18:
 * - 1. alloc시 검사를 해야되는 사항 수행
 *   2. alloc user track 기록
 *   3. object의 redzone에 SLUB_RED_ACTIVE값으로 기록하고 posion정보도 기록한다.
 */
static noinline int alloc_debug_processing(struct kmem_cache *s,
					struct page *page,
					void *object, unsigned long addr)
{
	if (s->flags & SLAB_CONSISTENCY_CHECKS) {
		if (!alloc_consistency_checks(s, page, object))
			goto bad;
	}

	/* Success perform special debug activities for allocs */
	if (s->flags & SLAB_STORE_USER)
		set_track(s, object, TRACK_ALLOC, addr);
	trace(s, page, object, 1);
	init_object(s, object, SLUB_RED_ACTIVE);
	return 1;

bad:
	if (PageSlab(page)) {
		/*
		 * If this is a slab page then lets do the best we can
		 * to avoid issues in the future. Marking all objects
		 * as used avoids touching the remaining objects.
		 */
		slab_fix(s, "Marking all objects used");
		page->inuse = page->objects;
		page->freelist = NULL;
	}
	return 0;
}

/*
 * IAMROOT, 2022.06.18:
 * - alloc_consistency_checks과 비슷한 방식으로 free할때 검사를 수행한다.
 */
static inline int free_consistency_checks(struct kmem_cache *s,
		struct page *page, void *object, unsigned long addr)
{
	if (!check_valid_pointer(s, page, object)) {
		slab_err(s, page, "Invalid object pointer 0x%p", object);
		return 0;
	}

	if (on_freelist(s, page, object)) {
		object_err(s, page, object, "Object already free");
		return 0;
	}

	if (!check_object(s, page, object, SLUB_RED_ACTIVE))
		return 0;

	if (unlikely(s != page->slab_cache)) {
		if (!PageSlab(page)) {
			slab_err(s, page, "Attempt to free object(0x%p) outside of slab",
				 object);
		} else if (!page->slab_cache) {
			pr_err("SLUB <none>: no slab for object 0x%p.\n",
			       object);
			dump_stack();
		} else
			object_err(s, page, object,
					"page slab pointer corrupt.");
		return 0;
	}
	return 1;
}

/* Supports checking bulk free of a constructed freelist */
/*
 * IAMROOT, 2022.06.25:
 * @return 0 : invalid. 1 : valid
 * - page, object등을 검사한다.
 */
static noinline int free_debug_processing(
	struct kmem_cache *s, struct page *page,
	void *head, void *tail, int bulk_cnt,
	unsigned long addr)
{
	struct kmem_cache_node *n = get_node(s, page_to_nid(page));
	void *object = head;
	int cnt = 0;
	unsigned long flags, flags2;
	int ret = 0;

	spin_lock_irqsave(&n->list_lock, flags);
	slab_lock(page, &flags2);

/*
 * IAMROOT, 2022.06.25:
 * - page check
 */
	if (s->flags & SLAB_CONSISTENCY_CHECKS) {
		if (!check_slab(s, page))
			goto out;
	}

next_object:
	cnt++;

/*
 * IAMROOT, 2022.06.25:
 * - object check
 */
	if (s->flags & SLAB_CONSISTENCY_CHECKS) {
		if (!free_consistency_checks(s, page, object, addr))
			goto out;
	}

	if (s->flags & SLAB_STORE_USER)
		set_track(s, object, TRACK_FREE, addr);
	trace(s, page, object, 0);
	/* Freepointer not overwritten by init_object(), SLAB_POISON moved it */
	init_object(s, object, SLUB_RED_INACTIVE);

	/* Reached end of constructed freelist yet? */
	if (object != tail) {
		object = get_freepointer(s, object);
		goto next_object;
	}
	ret = 1;

out:
	if (cnt != bulk_cnt)
		slab_err(s, page, "Bulk freelist count(%d) invalid(%d)\n",
			 bulk_cnt, cnt);

	slab_unlock(page, &flags2);
	spin_unlock_irqrestore(&n->list_lock, flags);
	if (!ret)
		slab_fix(s, "Object at 0x%p not freed", object);
	return ret;
}

/*
 * Parse a block of slub_debug options. Blocks are delimited by ';'
 *
 * @str:    start of block
 * @flags:  returns parsed flags, or DEBUG_DEFAULT_FLAGS if none specified
 * @slabs:  return start of list of slabs, or NULL when there's no list
 * @init:   assume this is initial parsing and not per-kmem-create parsing
 *
 * returns the start of next block if there's any, or NULL
 */
static char *
parse_slub_debug_flags(char *str, slab_flags_t *flags, char **slabs, bool init)
{
	bool higher_order_disable = false;

	/* Skip any completely empty blocks */
	while (*str && *str == ';')
		str++;

/*
 * IAMROOT, 2022.06.11: 
 * 처음 부터 플래그 없이 ","가 선택되는 경우 "FZPT"가 추가된다.
 */
	if (*str == ',') {
		/*
		 * No options but restriction on slabs. This means full
		 * debugging for slabs matching a pattern.
		 */
		*flags = DEBUG_DEFAULT_FLAGS;
		goto check_slabs;
	}
	*flags = 0;

	/* Determine which debug features should be switched on */
	for (; *str && *str != ',' && *str != ';'; str++) {
		switch (tolower(*str)) {
		case '-':
			*flags = 0;
			break;
		case 'f':
			*flags |= SLAB_CONSISTENCY_CHECKS;
			break;
		case 'z':
			*flags |= SLAB_RED_ZONE;
			break;
		case 'p':
			*flags |= SLAB_POISON;
			break;
		case 'u':
			*flags |= SLAB_STORE_USER;
			break;
		case 't':
			*flags |= SLAB_TRACE;
			break;
		case 'a':
			*flags |= SLAB_FAILSLAB;
			break;
		case 'o':
			/*
			 * Avoid enabling debugging on caches if its minimum
			 * order would increase as a result.
			 */
			higher_order_disable = true;
			break;
		default:
			if (init)
				pr_err("slub_debug option '%c' unknown. skipped\n", *str);
		}
	}
check_slabs:
/*
 * IAMROOT, 2022.06.11: 
 * 플래그 다음의 "," 뒤의 문자열을 슬랩 캐시명으로 모두 대입시킨다.
 * *slabs: "dentry,abc;....."
 */
	if (*str == ',')
		*slabs = ++str;
	else
		*slabs = NULL;

	/* Skip over the slab list */
	while (*str && *str != ';')
		str++;

	/* Skip any completely empty blocks */
	while (*str && *str == ';')
		str++;

	if (init && higher_order_disable)
		disable_higher_order_debug = 1;

/*
 * IAMROOT, 2022.06.11: 
 * ";" 문자열 다음 문자열을 반환한다.
 */
	if (*str)
		return str;
	else
		return NULL;
}

/*
 * IAMROOT, 2022.06.11: 
 * 필요한 슬랩캐시를 런타임 디버그를 켤수 있다.
 * 
 * slub_debug=<Debug-Options>,<slab name1>,<slab name2>,...
 * 옵션들:
 *
 *      F               Sanity checks on (enables SLAB_DEBUG_CONSISTENCY_CHECKS
 *                      Sorry SLAB legacy issues)
 *      Z               Red zoning
 *      P               Poisoning (object and padding)
 *      U               User tracking (free and alloc)
 *      T               Trace (please only use on single slabs)
 *      A               Enable failslab filter mark for the cache
 *      O               Switch debugging off for caches that would have
 *                      caused higher minimum slab orders
 *      -               Switch all debugging off (useful if the kernel is
 *                      configured with CONFIG_SLUB_DEBUG_ON)
 * 
 * 예) slub_debug=FZ
 *     slub_debug=,dentry
 *     slub_debug=P,kmalloc-*,dentry
 *     slub_debug=F,dentry
 *
 * 예) slub_debug
 *     전체 캐시를 생성할 때 FZPT가 추가된다.
 *
 * 예) slub_debug=F;Z,dentry,abc
 *     전체 캐시를 생성할 때엔 F를 적용하고, dentry와 abc에는 Z를 추가 적용한다.
 * 
 * slub_debug 전역 변수에는 global 플래그들이 담긴다.
 * slub_debug_string에는 전체 문자열이 담긴다.
 */
static int __init setup_slub_debug(char *str)
{
	slab_flags_t flags;
	slab_flags_t global_flags;
	char *saved_str;
	char *slab_list;
	bool global_slub_debug_changed = false;
	bool slab_list_specified = false;

/*
 * IAMROOT, 2022.06.11: 
 * DEBUG_DEFAULT_FLAGS:
 *  SLAB_CONSISTENCY_CHECKS | SLAB_RED_ZONE | SLAB_POISON | SLAB_STORE_USER
 *            F                    Z               P              T
 *
 * "slub_debug" or "slub_debug="라고만 지정하면 위의 4개 플래그가 자동으로 추가된다.
 */
	global_flags = DEBUG_DEFAULT_FLAGS;
	if (*str++ != '=' || !*str)
		/*
		 * No options specified. Switch on full debugging.
		 */
		goto out;

/*
 * IAMROOT, 2022.06.11: 
 * "slub_debug="을 제외한 나머지 문자열을 saved_str에 임시로 보관한다. 
 */
	saved_str = str;
	while (str) {
		str = parse_slub_debug_flags(str, &flags, &slab_list, true);

/*
 * IAMROOT, 2022.06.11: 
 * 글로벌 플래그가 지정된 경우 global_slub_debug_changed=true로 변경된다.
 * 또한 슬랩 캐시 이름이 지정된 경우에는 slab_list_specified도 true로 변경된다.
 */

		if (!slab_list) {
			global_flags = flags;
			global_slub_debug_changed = true;
		} else {
			slab_list_specified = true;
		}
	}

	/*
	 * For backwards compatibility, a single list of flags with list of
	 * slabs means debugging is only changed for those slabs, so the global
	 * slub_debug should be unchanged (0 or DEBUG_DEFAULT_FLAGS, depending
	 * on CONFIG_SLUB_DEBUG_ON). We can extended that to multiple lists as
	 * long as there is no option specifying flags without a slab list.
	 */
/*
 * IAMROOT, 2022.06.11: 
 * 슬랩 캐시 이름이 지정된 경우 slub_debug_string에는 보관해뒀던 문자열들이
 * 지정된다. 또한 글로벌로 지정될 플래그 설정은 global_flags에 담는다.
 */
	if (slab_list_specified) {
		if (!global_slub_debug_changed)
			global_flags = slub_debug;
		slub_debug_string = saved_str;
	}
out:
	slub_debug = global_flags;
	if (slub_debug != 0 || slub_debug_string)
		static_branch_enable(&slub_debug_enabled);
	else
		static_branch_disable(&slub_debug_enabled);
	if ((static_branch_unlikely(&init_on_alloc) ||
	     static_branch_unlikely(&init_on_free)) &&
	    (slub_debug & SLAB_POISON))
		pr_info("mem auto-init: SLAB_POISON will take precedence over init_on_alloc/init_on_free\n");
	return 1;
}

__setup("slub_debug", setup_slub_debug);

/*
 * kmem_cache_flags - apply debugging options to the cache
 * @object_size:	the size of an object without meta data
 * @flags:		flags to set
 * @name:		name of the cache
 *
 * Debug option(s) are applied to @flags. In addition to the debug
 * option(s), if a slab name (or multiple) is specified i.e.
 * slub_debug=<Debug-Options>,<slab name1>,<slab name2> ...
 * then only the select slabs will receive the debug option(s).
 */
/*
 * IAMROOT, 2022.06.11: 
 * SLUB_DEBUG 커널 옵션이 사용되는 경우 플래그가 추가된다.
 */
slab_flags_t kmem_cache_flags(unsigned int object_size,
	slab_flags_t flags, const char *name)
{
	char *iter;
	size_t len;
	char *next_block;
	slab_flags_t block_flags;
/*
 * IAMROOT, 2022.06.11: 
 * "slub_debug="에서 지정된 글로벌 플래그들을 알아온다.
 */
	slab_flags_t slub_debug_local = slub_debug;

	/*
	 * If the slab cache is for debugging (e.g. kmemleak) then
	 * don't store user (stack trace) information by default,
	 * but let the user enable it via the command line below.
	 */
/*
 * IAMROOT, 2022.06.11: 
 * 누수 트래킹을 하지 않도록 요청한 경우 유저 트래킹 플래그를 제외시킨다.
 */
	if (flags & SLAB_NOLEAKTRACE)
		slub_debug_local &= ~SLAB_STORE_USER;

	len = strlen(name);
	next_block = slub_debug_string;
	/* Go through all blocks of debug options, see if any matches our slab's name */
	while (next_block) {
/*
 * IAMROOT, 2022.06.11: 
 * 한 블럭을 파싱하여 block_flags에 알아온다. 반환되는 next_block에는 
 * 다음 블럭이 지정되고, iter에는 지정된 캐시명이 담긴다.
 */
		next_block = parse_slub_debug_flags(next_block, &block_flags, &iter, false);
/*
 * IAMROOT, 2022.06.11: 
 * 지정된 캐시가 없는 경우는 해당 블럭을 skip 한다.
 */
		if (!iter)
			continue;
		/* Found a block that has a slab list, search it */
/*
 * IAMROOT, 2022.06.11: 
 * 슬랩 캐시명이 지정된 경우 ";"으로 끝날 때까지 요청한 캐시명과 생성할 캐시명이
 * 같은 경우 해당 플래그를 지정한다.
 * 
 * 블럭내 슬랩 캐시 명이 ","로 구분되어서 올 수 있고, "*"를 포함하여 다수의 
 * 슬랩 캐시를 선택할 수 있다. 
 */
		while (*iter) {
			char *end, *glob;
			size_t cmplen;

			end = strchrnul(iter, ',');
			if (next_block && next_block < end)
				end = next_block - 1;

			glob = strnchr(iter, end - iter, '*');
			if (glob)
				cmplen = glob - iter;
			else
				cmplen = max_t(size_t, len, (end - iter));

			if (!strncmp(name, iter, cmplen)) {
				flags |= block_flags;
				return flags;
			}

			if (!*end || *end == ';')
				break;
			iter = end + 1;
		}
	}

	return flags | slub_debug_local;
}
#else /* !CONFIG_SLUB_DEBUG */
static inline void setup_object_debug(struct kmem_cache *s,
			struct page *page, void *object) {}
static inline
void setup_page_debug(struct kmem_cache *s, struct page *page, void *addr) {}

static inline int alloc_debug_processing(struct kmem_cache *s,
	struct page *page, void *object, unsigned long addr) { return 0; }

static inline int free_debug_processing(
	struct kmem_cache *s, struct page *page,
	void *head, void *tail, int bulk_cnt,
	unsigned long addr) { return 0; }

static inline int slab_pad_check(struct kmem_cache *s, struct page *page)
			{ return 1; }
static inline int check_object(struct kmem_cache *s, struct page *page,
			void *object, u8 val) { return 1; }
static inline void add_full(struct kmem_cache *s, struct kmem_cache_node *n,
					struct page *page) {}
static inline void remove_full(struct kmem_cache *s, struct kmem_cache_node *n,
					struct page *page) {}
/*
 * IAMROOT, 2022.06.11: 
 * SLUB_DEBUG 커널 옵션이 적용되지 않은 경우 추가되는 플래그가 없다.
 */
slab_flags_t kmem_cache_flags(unsigned int object_size,
	slab_flags_t flags, const char *name)
{
	return flags;
}
#define slub_debug 0

#define disable_higher_order_debug 0

static inline unsigned long slabs_node(struct kmem_cache *s, int node)
							{ return 0; }
static inline unsigned long node_nr_slabs(struct kmem_cache_node *n)
							{ return 0; }
static inline void inc_slabs_node(struct kmem_cache *s, int node,
							int objects) {}
static inline void dec_slabs_node(struct kmem_cache *s, int node,
							int objects) {}

static bool freelist_corrupted(struct kmem_cache *s, struct page *page,
			       void **freelist, void *nextfree)
{
	return false;
}
#endif /* CONFIG_SLUB_DEBUG */

/*
 * Hooks for other subsystems that check memory allocations. In a typical
 * production configuration these hooks all should produce no code at all.
 */
static inline void *kmalloc_large_node_hook(void *ptr, size_t size, gfp_t flags)
{
	ptr = kasan_kmalloc_large(ptr, size, flags);
	/* As ptr might get tagged, call kmemleak hook after KASAN. */
	kmemleak_alloc(ptr, size, 1, flags);
	return ptr;
}

static __always_inline void kfree_hook(void *x)
{
	kmemleak_free(x);
	kasan_kfree_large(x);
}

/*
 * IAMROOT, 2022.06.25:
 * @return 기본값 false.
 * - @init이 true이면 memset한다.
 */
static __always_inline bool slab_free_hook(struct kmem_cache *s,
						void *x, bool init)
{
	kmemleak_free_recursive(x, s->flags);

	debug_check_no_locks_freed(x, s->object_size);

	if (!(s->flags & SLAB_DEBUG_OBJECTS))
		debug_check_no_obj_freed(x, s->object_size);

	/* Use KCSAN to help debug racy use-after-free. */
	if (!(s->flags & SLAB_TYPESAFE_BY_RCU))
		__kcsan_check_access(x, s->object_size,
				     KCSAN_ACCESS_WRITE | KCSAN_ACCESS_ASSERT);

	/*
	 * As memory initialization might be integrated into KASAN,
	 * kasan_slab_free and initialization memset's must be
	 * kept together to avoid discrepancies in behavior.
	 *
	 * The initialization memset's clear the object and the metadata,
	 * but don't touch the SLAB redzone.
	 */
	if (init) {
		int rsize;

		if (!kasan_has_integrated_init())
			memset(kasan_reset_tag(x), 0, s->object_size);
		rsize = (s->flags & SLAB_RED_ZONE) ? s->red_left_pad : 0;
		memset((char *)kasan_reset_tag(x) + s->inuse, 0,
		       s->size - s->inuse - rsize);
	}
	/* KASAN might put x into memory quarantine, delaying its reuse. */
	return kasan_slab_free(s, x, init);
}

/*
 * IAMROOT, 2022.06.25:
 * - @head n0(head) -> n1 -> n2.. 로 cnt만큼 연결된 slab object들. fp로 연결되있다.
 */
static inline bool slab_free_freelist_hook(struct kmem_cache *s,
					   void **head, void **tail,
					   int *cnt)
{

	void *object;
	void *next = *head;
	void *old_tail = *tail ? *tail : *head;

	if (is_kfence_address(next)) {
		slab_free_hook(s, next, false);
		return true;
	}

	/* Head and tail of the reconstructed freelist */
	*head = NULL;
	*tail = NULL;

	do {
		object = next;
		next = get_freepointer(s, object);

		/* If object's reuse doesn't have to be delayed */
		if (!slab_free_hook(s, object, slab_want_init_on_free(s))) {
			/* Move object to the new freelist */
/*
 * IAMROOT, 2022.06.25:
 * - object의 fp에 *head로 갱신되고, *head는 object로 갱신된다.
 *   즉 head를 free하고 head->fp로 head를 갱신
 * --- ex) o1->o2->o3 로 연결되있다고 가정
 * - 첫루틴
 *   전)
 *   *head = NULL, *tail = NULL
 *
 *   후)
 *   o1.fp = NULL
 *   head = o1, tail = o1
 *
 * - 두번째 루틴
 *   o2.fp = o1
 *           o1.fp = NULL
 *   head = o2, tail = o1
 * - 세번째 루틴
 *   o3.fp = o2
 *           o2.fp = o1
 *		     o1.fp = NULL
 *   head = o3, tail = o1
 */
			set_freepointer(s, object, *head);
			*head = object;
			if (!*tail)
				*tail = object;
		} else {
			/*
			 * Adjust the reconstructed freelist depth
			 * accordingly if object's reuse is delayed.
			 */
			--(*cnt);
		}
	} while (object != old_tail);

	if (*head == *tail)
		*tail = NULL;

	return *head != NULL;
}

static void *setup_object(struct kmem_cache *s, struct page *page,
				void *object)
{
	setup_object_debug(s, page, object);
	object = kasan_init_slab_obj(s, object);
	if (unlikely(s->ctor)) {
		kasan_unpoison_object_data(s, object);
		s->ctor(object);
		kasan_poison_object_data(s, object);
	}
	return object;
}

/*
 * Slab allocation and freeing
 */
/*
 * IAMROOT, 2022.06.18:
 * - cache의 order로 page를 할당해온다.
 */
static inline struct page *alloc_slab_page(struct kmem_cache *s,
		gfp_t flags, int node, struct kmem_cache_order_objects oo)
{
	struct page *page;
	unsigned int order = oo_order(oo);

	if (node == NUMA_NO_NODE)
		page = alloc_pages(flags, order);
	else
		page = __alloc_pages_node(node, flags, order);

	return page;
}

#ifdef CONFIG_SLAB_FREELIST_RANDOM
/* Pre-initialize the random sequence cache */
static int init_cache_random_seq(struct kmem_cache *s)
{
	unsigned int count = oo_objects(s->oo);
	int err;

	/* Bailout if already initialised */
	if (s->random_seq)
		return 0;

	err = cache_random_seq_create(s, count, GFP_KERNEL);
	if (err) {
		pr_err("SLUB: Unable to initialize free list for %s\n",
			s->name);
		return err;
	}

	/* Transform to an offset on the set of pages */
	if (s->random_seq) {
		unsigned int i;

		for (i = 0; i < count; i++)
			s->random_seq[i] *= s->size;
	}
	return 0;
}

/* Initialize each random sequence freelist per cache */

/*
 * IAMROOT, 2022.06.25:
 * - fp 연결에 random sequence를 사용하기 위한 값을 초기화한다.
 */
static void __init init_freelist_randomization(void)
{
	struct kmem_cache *s;

	mutex_lock(&slab_mutex);

/*
 * IAMROOT, 2022.06.18:
 * - 모든 slab cache를 interate하며 random seq를 초기화한다.
 */
	list_for_each_entry(s, &slab_caches, list)
		init_cache_random_seq(s);

	mutex_unlock(&slab_mutex);
}

/* Get the next entry on the pre-computed freelist randomized */
/*
 * IAMROOT, 2022.06.18:
 * - @pos를 index로 하여 cache random_seq에서 next 주소를 가져온다.
 *   만약 limit를 넘는 index가 있다면 재시도.
 */
static void *next_freelist_entry(struct kmem_cache *s, struct page *page,
				unsigned long *pos, void *start,
				unsigned long page_limit,
				unsigned long freelist_count)
{
	unsigned int idx;

	/*
	 * If the target page allocation failed, the number of objects on the
	 * page might be smaller than the usual size defined by the cache.
	 */
	do {
		idx = s->random_seq[*pos];
		*pos += 1;
		if (*pos >= freelist_count)
			*pos = 0;
	} while (unlikely(idx >= page_limit));

	return (char *)start + idx;
}

/* Shuffle the single linked freelist based on a random pre-computed sequence */
/*
 * IAMROOT, 2022.06.18:
 * - freelist를 random으로 시작점을 정하고, 순서를 섞는다. debug 정보를 초기화한다.
 */
static bool shuffle_freelist(struct kmem_cache *s, struct page *page)
{
	void *start;
	void *cur;
	void *next;
	unsigned long idx, pos, page_limit, freelist_count;

	if (page->objects < 2 || !s->random_seq)
		return false;

	freelist_count = oo_objects(s->oo);
/*
 * IAMROOT, 2022.06.18:
 * - freelist_count이내의 숫자에서 random number를 가져온다.
 */
	pos = get_random_int() % freelist_count;

	page_limit = page->objects * s->size;
	start = fixup_red_left(s, page_address(page));

	/* First entry is used as the base of the freelist */
/*
 * IAMROOT, 2022.06.18:
 * - pos를 기준으로 random next를 가져온다. pos는 + 1 된다.
 */
	cur = next_freelist_entry(s, page, &pos, start, page_limit,
				freelist_count);
	cur = setup_object(s, page, cur);

/*
 * IAMROOT, 2022.06.18:
 * - 시작지점을 위에서 random으로 구한 cur로 설정한다.
 */
	page->freelist = cur;

/*
 * IAMROOT, 2022.06.18:
 * - cur의 FP에 next를 단방향 list 식으로 연결한다. 맨마지막 object FP엔 null을
 *   넣는다.
 *
 * |     object1             |    object2              |
 * +---+-----------+---+-----+---+-----------+---+-----+
 * | Z |   |FP|    | Z | PAD | Z |   |FP|    | Z | PAD |
 * +---+----v------+---+-----+---^----v------+---+-----+
 *          \____________________/    NULL
 *        object의 시작점을 가리킨다.
 *
 */
	for (idx = 1; idx < page->objects; idx++) {
		next = next_freelist_entry(s, page, &pos, start, page_limit,
			freelist_count);
		next = setup_object(s, page, next);
		set_freepointer(s, cur, next);
		cur = next;
	}
	set_freepointer(s, cur, NULL);

	return true;
}
#else
static inline int init_cache_random_seq(struct kmem_cache *s)
{
	return 0;
}
static inline void init_freelist_randomization(void) { }
static inline bool shuffle_freelist(struct kmem_cache *s, struct page *page)
{
	return false;
}
#endif /* CONFIG_SLAB_FREELIST_RANDOM */

/*
 * IAMROOT, 2022.06.18:
 * - slab page를 할당받고 debug정보 초기화, fp를 전부 이어 놓는다. 그리고 frozen
 *   상태를 해놓는다.
 */
static struct page *allocate_slab(struct kmem_cache *s, gfp_t flags, int node)
{
	struct page *page;
	struct kmem_cache_order_objects oo = s->oo;
	gfp_t alloc_gfp;
	void *start, *p, *next;
	int idx;
	bool shuffle;

	flags &= gfp_allowed_mask;

	flags |= s->allocflags;

	/*
	 * Let the initial higher-order allocation fail under memory pressure
	 * so we fall-back to the minimum order allocation.
	 */
	alloc_gfp = (flags | __GFP_NOWARN | __GFP_NORETRY) & ~__GFP_NOFAIL;
/*
 * IAMROOT, 2022.06.18:
 * - direct recalim 요청, order가 min보다 크면 nofail을 지우고 nomemalloc을 추가한다.
 */
	if ((alloc_gfp & __GFP_DIRECT_RECLAIM) && oo_order(oo) > oo_order(s->min))
		alloc_gfp = (alloc_gfp | __GFP_NOMEMALLOC) & ~(__GFP_RECLAIM|__GFP_NOFAIL);

	page = alloc_slab_page(s, alloc_gfp, node, oo);
	if (unlikely(!page)) {
/*
 * IAMROOT, 2022.06.18:
 * - 실패하면 order를 줄이고, flags를 완화하여 한번더 시도한다.
 */
		oo = s->min;
		alloc_gfp = flags;
		/*
		 * Allocation may have failed due to fragmentation.
		 * Try a lower order alloc if possible
		 */
		page = alloc_slab_page(s, alloc_gfp, node, oo);
		if (unlikely(!page))
			goto out;
		stat(s, ORDER_FALLBACK);
	}

	page->objects = oo_objects(oo);

	account_slab_page(page, oo_order(oo), s, flags);

	page->slab_cache = s;

/*
 * IAMROOT, 2022.06.18:
 * - slab flag를 set한다. 만약 비상메모리로 할당받아왔다면 SlabPfmemalloc flag도
 *   붙인다.
 */
	__SetPageSlab(page);
	if (page_is_pfmemalloc(page))
		SetPageSlabPfmemalloc(page);

	kasan_poison_slab(page);

	start = page_address(page);

	setup_page_debug(s, page, start);

	shuffle = shuffle_freelist(s, page);

	if (!shuffle) {
/*
 * IAMROOT, 2022.06.18:
 * - shuffle이 아니거나 기능을 안쓰는 경우등일때 순서대로 연결한다.
 */
		start = fixup_red_left(s, start);
		start = setup_object(s, page, start);
		page->freelist = start;
		for (idx = 0, p = start; idx < page->objects - 1; idx++) {
			next = p + s->size;
			next = setup_object(s, page, next);
			set_freepointer(s, p, next);
			p = next;
		}
		set_freepointer(s, p, NULL);
	}

	page->inuse = page->objects;

/*
 * IAMROOT, 2022.06.18:
 * - frozen은 cpu에 해당 page가 있을 경우 set된다.
 *   미리 cpu에 바로 연결될 준비를 해놓는 개념.
 */
	page->frozen = 1;

out:
	if (!page)
		return NULL;

	inc_slabs_node(s, page_to_nid(page), page->objects);

	return page;
}

/*
 * IAMROOT, 2022.06.18:
 * - slab page를 할당받고 debug정보 초기화, fp를 전부 이어 놓는다. 그리고 frozen
 *   상태를 해놓는다.
 */
static struct page *new_slab(struct kmem_cache *s, gfp_t flags, int node)
{
	if (unlikely(flags & GFP_SLAB_BUG_MASK))
		flags = kmalloc_fix_flags(flags);

	WARN_ON_ONCE(s->ctor && (flags & __GFP_ZERO));

	return allocate_slab(s,
		flags & (GFP_RECLAIM_MASK | GFP_CONSTRAINT_MASK), node);
}

/*
 * IAMROOT, 2022.06.25:
 * - TODO
 */
static void __free_slab(struct kmem_cache *s, struct page *page)
{
	int order = compound_order(page);
	int pages = 1 << order;

	if (kmem_cache_debug_flags(s, SLAB_CONSISTENCY_CHECKS)) {
		void *p;

		slab_pad_check(s, page);
		for_each_object(p, s, page_address(page),
						page->objects)
			check_object(s, page, p, SLUB_RED_INACTIVE);
	}

	__ClearPageSlabPfmemalloc(page);
	__ClearPageSlab(page);
	/* In union with page->mapping where page allocator expects NULL */
	page->slab_cache = NULL;
	if (current->reclaim_state)
		current->reclaim_state->reclaimed_slab += pages;
	unaccount_slab_page(page, order, s);
	__free_pages(page, order);
}

static void rcu_free_slab(struct rcu_head *h)
{
	struct page *page = container_of(h, struct page, rcu_head);

	__free_slab(page->slab_cache, page);
}

static void free_slab(struct kmem_cache *s, struct page *page)
{
	if (unlikely(s->flags & SLAB_TYPESAFE_BY_RCU)) {
		call_rcu(&page->rcu_head, rcu_free_slab);
	} else
		__free_slab(s, page);
}

static void discard_slab(struct kmem_cache *s, struct page *page)
{
	dec_slabs_node(s, page_to_nid(page), page->objects);
	free_slab(s, page);
}

/*
 * Management of partially allocated slabs.
 */
/*
 * IAMROOT, 2022.06.25:
 * - partial list에 slab page를 추가한다.
 */
static inline void
__add_partial(struct kmem_cache_node *n, struct page *page, int tail)
{
	n->nr_partial++;
	if (tail == DEACTIVATE_TO_TAIL)
		list_add_tail(&page->slab_list, &n->partial);
	else
		list_add(&page->slab_list, &n->partial);
}

static inline void add_partial(struct kmem_cache_node *n,
				struct page *page, int tail)
{
	lockdep_assert_held(&n->list_lock);
	__add_partial(n, page, tail);
}

/*
 * IAMROOT, 2022.06.25:
 * - parital list에서 한개 뺀다.
 */
static inline void remove_partial(struct kmem_cache_node *n,
					struct page *page)
{
	lockdep_assert_held(&n->list_lock);
	list_del(&page->slab_list);
	n->nr_partial--;
}

/*
 * Remove slab from the partial list, freeze it and
 * return the pointer to the freelist.
 *
 * Returns a list of objects or NULL if it fails.
 */
/*
 * IAMROOT, 2022.06.25:
 * - mode == true
 *   node partial list로부터 page를 얻어와 cpu로 옮기는 상태.
 *   전부 inuse로 사용한다.
 * - mode == false
 */
static inline void *acquire_slab(struct kmem_cache *s,
		struct kmem_cache_node *n, struct page *page,
		int mode, int *objects)
{
	void *freelist;
	unsigned long counters;
	struct page new;

	lockdep_assert_held(&n->list_lock);

	/*
	 * Zap the freelist and set the frozen bit.
	 * The old freelist is the list of objects for the
	 * per cpu allocation list.
	 */
	freelist = page->freelist;
	counters = page->counters;

/*
 * IAMROOT, 2022.06.25:
 * - counters와 objects, inuse, frozen는 union.
 */
	new.counters = counters;
/*
 * IAMROOT, 2022.06.25:
 * - *objects = slab page내 (object수 - 사용중)
 */
	*objects = new.objects - new.inuse;
/*
 * IAMROOT, 2022.06.25:
 * - mode == true
 *   node parital list부터 page를 얻는 상태. 즉 cpu partial 추가해야되는 상태고
 *   이는 전부 cpu에서 사용중인거다.
 */
	if (mode) {
		new.inuse = page->objects;
/*
 * IAMROOT, 2022.06.25:
 * - 회수된게 없는 상태. (page의 frelist는 회수 될때 or 사용가능한 object들을
 *   연결 할때 사용한다.)
 */
		new.freelist = NULL;
	} else {
/*
 * IAMROOT, 2022.06.25:
 * - 가지고 있던것을 그대로 넣는다(복사 개념)
 */
		new.freelist = freelist;
	}

/*
 * IAMROOT, 2022.06.25:
 * - node parital list에서 막 꺼내온상태이므로 frozen(cpu on)은 false여야한다.
 */
	VM_BUG_ON(new.frozen);

/*
 * IAMROOT, 2022.06.25:
 * - cpu쪽으로 옮긴다.
 */
	new.frozen = 1;

/*
 * IAMROOT, 2022.06.25:
 * - freelist, countes 를 동시에 교체한다. 실패한 경우 return NULL.
 */
	if (!__cmpxchg_double_slab(s, page,
			freelist, counters,
			new.freelist, new.counters,
			"acquire_slab"))
		return NULL;

	remove_partial(n, page);
	WARN_ON(!freelist);
	return freelist;
}

#ifdef CONFIG_SLUB_CPU_PARTIAL
static void put_cpu_partial(struct kmem_cache *s, struct page *page, int drain);
#else
static inline void put_cpu_partial(struct kmem_cache *s, struct page *page,
				   int drain) { }
#endif
static inline bool pfmemalloc_match(struct page *page, gfp_t gfpflags);

/*
 * Try to allocate a partial slab from a specific node.
 */
/*
 * IAMROOT, 2022.06.25:
 * @return 최초의 object.
 * @ret_pages 최초의 slab page
 * - @n에 node partial list가 존재하는 경우 cpu parital list로
 *   slabs_cpu_partial(s) / 2 개수만큼 옮기는걸 시도한다.
 */
static void *get_partial_node(struct kmem_cache *s, struct kmem_cache_node *n,
			      struct page **ret_page, gfp_t gfpflags)
{
	struct page *page, *page2;
	void *object = NULL;
	unsigned int available = 0;
	unsigned long flags;
	int objects;

	/*
	 * Racy check. If we mistakenly see no partial slabs then we
	 * just allocate an empty slab. If we mistakenly try to get a
	 * partial slab and there is none available then get_partial()
	 * will return NULL.
	 */
/*
 * IAMROOT, 2022.06.18:
 * - kmem_cache_node가 없거나 parital도 없으면 return null.
 */
	if (!n || !n->nr_partial)
		return NULL;

	spin_lock_irqsave(&n->list_lock, flags);

/*
 * IAMROOT, 2022.06.25:
 * - slub_cpu_partial(s)의 절반 이상을 node partial list에서 cpu paritial로 옮긴다.
 */
	list_for_each_entry_safe(page, page2, &n->partial, slab_list) {
		void *t;

/*
 * IAMROOT, 2022.06.25:
 * - pfmemalloc 매칭이 안맞다면 continue
 */
		if (!pfmemalloc_match(page, gfpflags))
			continue;

		t = acquire_slab(s, n, page, object == NULL, &objects);
		if (!t)
			break;

/*
 * IAMROOT, 2022.06.25:
 * - 옮겨온 object수를 합산한다.
 */
		available += objects;
		if (!object) {
/*
 * IAMROOT, 2022.06.25:
 * - node parital list부터 page를 얻음.
 */
			*ret_page = page;
			stat(s, ALLOC_FROM_PARTIAL);
			object = t;
		} else {
/*
 * IAMROOT, 2022.06.25:
 * - page를 cpu parital로 추가한다.
 */
			put_cpu_partial(s, page, 0);
			stat(s, CPU_PARTIAL_NODE);
		}

/*
 * IAMROOT, 2022.06.25:
 * - available이 현재 cpu_partial의 절반보다 크다면 중단.
 */
		if (!kmem_cache_has_cpu_partial(s)
			|| available > slub_cpu_partial(s) / 2)
			break;

	}
	spin_unlock_irqrestore(&n->list_lock, flags);
	return object;
}

/*
 * Get a page from somewhere. Search in increasing NUMA distances.
 */
/*
 * IAMROOT, 2022.06.25:
 * - remote node parital list에서 slab page를 가져온다.
 */
static void *get_any_partial(struct kmem_cache *s, gfp_t flags,
			     struct page **ret_page)
{
#ifdef CONFIG_NUMA
	struct zonelist *zonelist;
	struct zoneref *z;
	struct zone *zone;
	enum zone_type highest_zoneidx = gfp_zone(flags);
	void *object;
	unsigned int cpuset_mems_cookie;

	/*
	 * The defrag ratio allows a configuration of the tradeoffs between
	 * inter node defragmentation and node local allocations. A lower
	 * defrag_ratio increases the tendency to do local allocations
	 * instead of attempting to obtain partial slabs from other nodes.
	 *
	 * If the defrag_ratio is set to 0 then kmalloc() always
	 * returns node local objects. If the ratio is higher then kmalloc()
	 * may return off node objects because partial slabs are obtained
	 * from other nodes and filled up.
	 *
	 * If /sys/kernel/slab/xx/remote_node_defrag_ratio is set to 100
	 * (which makes defrag_ratio = 1000) then every (well almost)
	 * allocation will first attempt to defrag slab caches on other nodes.
	 * This means scanning over all nodes to look for partial slabs which
	 * may be expensive if we do it every time we are trying to find a slab
	 * with available objects.
	 */
/*
 * IAMROOT, 2022.06.18:
 * - papago
 *   조각 모음 비율을 사용하면 노드 간 조각 모음과 노드 로컬 할당 간의 균형을
 *   구성할 수 있습니다. defrag_ratio가 낮을수록 다른 노드에서 부분 슬래브를
 *   얻으려고 시도하는 대신 로컬 할당을 수행하는 경향이 높아집니다.
 *
 *   defrag_ratio가 0으로 설정되면 kmalloc()은 항상 노드 로컬 객체를 반환합니다.
 *   비율이 더 높으면 kmalloc()은 다른 노드에서 부분 슬래브를 가져와 채워지기
 *   때문에 노드 개체를 반환할 수 있습니다.
 *
 *   /sys/kernel/slab/xx/remote_node_defrag_ratio가 100으로 설정되면
 *   (defrag_ratio = 1000이 됨) 모든(거의 거의) 할당이 먼저 다른 노드에서 슬랩
 *   캐시 조각 모음을 시도합니다.
 *   이것은 사용 가능한 개체가 있는 슬래브를 찾으려고 할 때마다 비용이 많이 들 수
 *   있는 부분 슬래브를 찾기 위해 모든 노드를 스캔하는 것을 의미합니다.
 *
 * - /sys/kernel/slab/xx/remote_node_defrag_ratio
 *   유저에서 보면 100(default 100.). kernel에서는 1000로 환산해서 사용한다.
 *   remote node에서도 가져오는 비율.
 */
	if (!s->remote_node_defrag_ratio ||
			get_cycles() % 1024 > s->remote_node_defrag_ratio)
		return NULL;

/*
 * IAMROOT, 2022.06.18:
 * - task mempolicy에 따라 node를 구해오고 해당 node로 시작하는 zonelist를 구해와
 *   zone을 iterate한다.
 *   cache node가 어느 zone에서도 없다면 iterate가 종료되고 return null.
 */
	do {
		cpuset_mems_cookie = read_mems_allowed_begin();
		zonelist = node_zonelist(mempolicy_slab_node(), flags);
		for_each_zone_zonelist(zone, z, zonelist, highest_zoneidx) {
			struct kmem_cache_node *n;

			n = get_node(s, zone_to_nid(zone));

			if (n && cpuset_zone_allowed(zone, flags) &&
					n->nr_partial > s->min_partial) {
				object = get_partial_node(s, n, ret_page, flags);
				if (object) {
					/*
					 * Don't check read_mems_allowed_retry()
					 * here - if mems_allowed was updated in
					 * parallel, that was a harmless race
					 * between allocation and the cpuset
					 * update
					 */
					return object;
				}
			}
		}
	} while (read_mems_allowed_retry(cpuset_mems_cookie));
#endif	/* CONFIG_NUMA */
	return NULL;
}

/*
 * Get a partial page, lock it and return it.
 */
/*
 * IAMROOT, 2022.06.25:
 * - @node에서 먼저 slab page얻어오는걸 시도하고, 실패하면 remote node에서 시도한다.
 */
static void *get_partial(struct kmem_cache *s, gfp_t flags, int node,
			 struct page **ret_page)
{
	void *object;
	int searchnode = node;

	if (node == NUMA_NO_NODE)
		searchnode = numa_mem_id();

	object = get_partial_node(s, get_node(s, searchnode), ret_page, flags);
	if (object || node != NUMA_NO_NODE)
		return object;

	return get_any_partial(s, flags, ret_page);
}

#ifdef CONFIG_PREEMPTION
/*
 * Calculate the next globally unique transaction for disambiguation
 * during cmpxchg. The transactions start with the cpu number and are then
 * incremented by CONFIG_NR_CPUS.
 */
#define TID_STEP  roundup_pow_of_two(CONFIG_NR_CPUS)
#else
/*
 * No preemption supported therefore also no need to check for
 * different cpus.
 */
#define TID_STEP 1
#endif

static inline unsigned long next_tid(unsigned long tid)
{
	return tid + TID_STEP;
}

#ifdef SLUB_DEBUG_CMPXCHG
static inline unsigned int tid_to_cpu(unsigned long tid)
{
	return tid % TID_STEP;
}

static inline unsigned long tid_to_event(unsigned long tid)
{
	return tid / TID_STEP;
}
#endif

static inline unsigned int init_tid(int cpu)
{
	return cpu;
}

static inline void note_cmpxchg_failure(const char *n,
		const struct kmem_cache *s, unsigned long tid)
{
#ifdef SLUB_DEBUG_CMPXCHG
	unsigned long actual_tid = __this_cpu_read(s->cpu_slab->tid);

	pr_info("%s %s: cmpxchg redo ", n, s->name);

#ifdef CONFIG_PREEMPTION
	if (tid_to_cpu(tid) != tid_to_cpu(actual_tid))
		pr_warn("due to cpu change %d -> %d\n",
			tid_to_cpu(tid), tid_to_cpu(actual_tid));
	else
#endif
	if (tid_to_event(tid) != tid_to_event(actual_tid))
		pr_warn("due to cpu running other code. Event %ld->%ld\n",
			tid_to_event(tid), tid_to_event(actual_tid));
	else
		pr_warn("for unknown reason: actual=%lx was=%lx target=%lx\n",
			actual_tid, tid, next_tid(tid));
#endif
	stat(s, CMPXCHG_DOUBLE_CPU_FAIL);
}

/*
 * IAMROOT, 2022.06.25:
 * - cpu_slab을 초기화한다.
 */
static void init_kmem_cache_cpus(struct kmem_cache *s)
{
	int cpu;
	struct kmem_cache_cpu *c;

	for_each_possible_cpu(cpu) {
		c = per_cpu_ptr(s->cpu_slab, cpu);
		local_lock_init(&c->lock);
		c->tid = init_tid(cpu);
	}
}

/*
 * Finishes removing the cpu slab. Merges cpu's freelist with page's freelist,
 * unfreezes the slabs and puts it on the proper list.
 * Assumes the slab has been already safely taken away from kmem_cache_cpu
 * by the caller.
 */
/*
 * IAMROOT, 2022.06.18:
 * - TODO
 */
static void deactivate_slab(struct kmem_cache *s, struct page *page,
			    void *freelist)
{
	enum slab_modes { M_NONE, M_PARTIAL, M_FULL, M_FREE };
	struct kmem_cache_node *n = get_node(s, page_to_nid(page));
	int lock = 0, free_delta = 0;
	enum slab_modes l = M_NONE, m = M_NONE;
	void *nextfree, *freelist_iter, *freelist_tail;
	int tail = DEACTIVATE_TO_HEAD;
	unsigned long flags = 0;
	struct page new;
	struct page old;

	if (page->freelist) {
		stat(s, DEACTIVATE_REMOTE_FREES);
		tail = DEACTIVATE_TO_TAIL;
	}

	/*
	 * Stage one: Count the objects on cpu's freelist as free_delta and
	 * remember the last object in freelist_tail for later splicing.
	 */
	freelist_tail = NULL;
	freelist_iter = freelist;
	while (freelist_iter) {
		nextfree = get_freepointer(s, freelist_iter);

		/*
		 * If 'nextfree' is invalid, it is possible that the object at
		 * 'freelist_iter' is already corrupted.  So isolate all objects
		 * starting at 'freelist_iter' by skipping them.
		 */
		if (freelist_corrupted(s, page, &freelist_iter, nextfree))
			break;

		freelist_tail = freelist_iter;
		free_delta++;

		freelist_iter = nextfree;
	}

	/*
	 * Stage two: Unfreeze the page while splicing the per-cpu
	 * freelist to the head of page's freelist.
	 *
	 * Ensure that the page is unfrozen while the list presence
	 * reflects the actual number of objects during unfreeze.
	 *
	 * We setup the list membership and then perform a cmpxchg
	 * with the count. If there is a mismatch then the page
	 * is not unfrozen but the page is on the wrong list.
	 *
	 * Then we restart the process which may have to remove
	 * the page from the list that we just put it on again
	 * because the number of objects in the slab may have
	 * changed.
	 */
redo:

	old.freelist = READ_ONCE(page->freelist);
	old.counters = READ_ONCE(page->counters);
	VM_BUG_ON(!old.frozen);

	/* Determine target state of the slab */
	new.counters = old.counters;
	if (freelist_tail) {
		new.inuse -= free_delta;
		set_freepointer(s, freelist_tail, old.freelist);
		new.freelist = freelist;
	} else
		new.freelist = old.freelist;

	new.frozen = 0;

	if (!new.inuse && n->nr_partial >= s->min_partial)
		m = M_FREE;
	else if (new.freelist) {
		m = M_PARTIAL;
		if (!lock) {
			lock = 1;
			/*
			 * Taking the spinlock removes the possibility
			 * that acquire_slab() will see a slab page that
			 * is frozen
			 */
			spin_lock_irqsave(&n->list_lock, flags);
		}
	} else {
		m = M_FULL;
		if (kmem_cache_debug_flags(s, SLAB_STORE_USER) && !lock) {
			lock = 1;
			/*
			 * This also ensures that the scanning of full
			 * slabs from diagnostic functions will not see
			 * any frozen slabs.
			 */
			spin_lock_irqsave(&n->list_lock, flags);
		}
	}

	if (l != m) {
		if (l == M_PARTIAL)
			remove_partial(n, page);
		else if (l == M_FULL)
			remove_full(s, n, page);

		if (m == M_PARTIAL)
			add_partial(n, page, tail);
		else if (m == M_FULL)
			add_full(s, n, page);
	}

	l = m;
	if (!cmpxchg_double_slab(s, page,
				old.freelist, old.counters,
				new.freelist, new.counters,
				"unfreezing slab"))
		goto redo;

	if (lock)
		spin_unlock_irqrestore(&n->list_lock, flags);

	if (m == M_PARTIAL)
		stat(s, tail);
	else if (m == M_FULL)
		stat(s, DEACTIVATE_FULL);
	else if (m == M_FREE) {
		stat(s, DEACTIVATE_EMPTY);
		discard_slab(s, page);
		stat(s, FREE_SLAB);
	}
}

#ifdef CONFIG_SLUB_CPU_PARTIAL

/*
 * IAMROOT, 2022.06.25:
 * - partial_pages를 unfrozen하고 node partial list로 되돌려보내는데 이미 충분히
 *   많다면 min_partial이상이면 buddy로 돌려보낸다.
 */
static void __unfreeze_partials(struct kmem_cache *s, struct page *partial_page)
{
	struct kmem_cache_node *n = NULL, *n2 = NULL;
	struct page *page, *discard_page = NULL;
	unsigned long flags = 0;

/*
 * IAMROOT, 2022.06.25:
 * - page->next로 linked list iteration
 */
	while (partial_page) {
		struct page new;
		struct page old;

		page = partial_page;
		partial_page = page->next;

/*
 * IAMROOT, 2022.06.25:
 * - lock 갱신(이전 node와 다르다면 이전 node를 unlock하고 현재 node를 lock한다.)
 */
		n2 = get_node(s, page_to_nid(page));
		if (n != n2) {
			if (n)
				spin_unlock_irqrestore(&n->list_lock, flags);

			n = n2;
			spin_lock_irqsave(&n->list_lock, flags);
		}

/*
 * IAMROOT, 2022.06.25:
 * - frozen 값을 0으로 변경하는 작업(unfrozen). 성공할때까지 cmpxchg를 시도한다.
 */
		do {

			old.freelist = page->freelist;
			old.counters = page->counters;
			VM_BUG_ON(!old.frozen);

			new.counters = old.counters;
			new.freelist = old.freelist;

			new.frozen = 0;

		} while (!__cmpxchg_double_slab(s, page,
				old.freelist, old.counters,
				new.freelist, new.counters,
				"unfreezing slab"));

/*
 * IAMROOT, 2022.06.25:
 * - node에 min_partial보다 많으면 buddy로 page를 discard_page로 연결한다.
 *   아니면 node parital list로 넣는다.
 */
		if (unlikely(!new.inuse && n->nr_partial >= s->min_partial)) {
			page->next = discard_page;
			discard_page = page;
		} else {
			add_partial(n, page, DEACTIVATE_TO_TAIL);
			stat(s, FREE_ADD_PARTIAL);
		}
	}

	if (n)
		spin_unlock_irqrestore(&n->list_lock, flags);

/*
 * IAMROOT, 2022.06.25:
 * - discard_page가 존재하면 buddy로 되돌린다.
 */
	while (discard_page) {
		page = discard_page;
		discard_page = discard_page->next;

		stat(s, DEACTIVATE_EMPTY);
		discard_slab(s, page);
		stat(s, FREE_SLAB);
	}
}

/*
 * Unfreeze all the cpu partial slabs.
 */
static void unfreeze_partials(struct kmem_cache *s)
{
	struct page *partial_page;
	unsigned long flags;

	local_lock_irqsave(&s->cpu_slab->lock, flags);
	partial_page = this_cpu_read(s->cpu_slab->partial);
	this_cpu_write(s->cpu_slab->partial, NULL);
	local_unlock_irqrestore(&s->cpu_slab->lock, flags);

	if (partial_page)
		__unfreeze_partials(s, partial_page);
}

static void unfreeze_partials_cpu(struct kmem_cache *s,
				  struct kmem_cache_cpu *c)
{
	struct page *partial_page;

	partial_page = slub_percpu_partial(c);
	c->partial = NULL;

	if (partial_page)
		__unfreeze_partials(s, partial_page);
}

/*
 * Put a page that was just frozen (in __slab_free|get_partial_node) into a
 * partial page slot if available.
 *
 * If we did not find a slot then simply move all the partials to the
 * per node partial list.
 */
/*
 * IAMROOT, 2022.06.25:
 * - papago
 *   (__slab_free|get_partial_node에서) 방금 고정된 페이지를 가능한 경우
 *   parital 페이지 슬롯에 넣습니다.
 *
 *   슬롯을 찾지 못한 경우 모든 부분을 노드당 부분 목록으로 이동하기만 하면 됩니다.
 *
 * - cpu parital에 page를 넣는다. @drain 요청이 있다면 node로 보내는 기준으로
 *   drain을 시도한다.
 */
static void put_cpu_partial(struct kmem_cache *s, struct page *page, int drain)
{
	struct page *oldpage;
	struct page *page_to_unfreeze = NULL;
	unsigned long flags;
	int pages = 0;
	int pobjects = 0;

	local_lock_irqsave(&s->cpu_slab->lock, flags);

	oldpage = this_cpu_read(s->cpu_slab->partial);

/*
 * IAMROOT, 2022.06.25:
 * - 이미 parial page가 있는 상태에서, drain요청이 있고 object가 충분히 많다면
 *   page를 unfreeze(node partial로 이동) 시킨다.
 */
	if (oldpage) {
		if (drain && oldpage->pobjects > slub_cpu_partial(s)) {
			/*
			 * Partial array is full. Move the existing set to the
			 * per node partial list. Postpone the actual unfreezing
			 * outside of the critical section.
			 */
/*
 * IAMROOT, 2022.06.25:
 * - papago
 *   partial 배열이 가득 찼습니다. 기존 집합을 노드별 parital list으로 이동합니다.
 *   임계 영역 외부에서 실제 동결 해제를 연기합니다.
 */
			page_to_unfreeze = oldpage;
			oldpage = NULL;
		} else {
			pobjects = oldpage->pobjects;
			pages = oldpage->pages;
		}
	}

/*
 * IAMROOT, 2022.06.25:
 * - object수를 갱신한다 (이전에 있는 object + @page의 남아있는 object 수)
 * - next page를 oldpage로 한다.
 *   [new page] -> [old page] -> ...
 * - page가 1개 더 들어오는 개념이므로 pages++을 해준다.
 */
	pages++;
	pobjects += page->objects - page->inuse;

	page->pages = pages;
	page->pobjects = pobjects;
	page->next = oldpage;

/*
 * IAMROOT, 2022.06.25:
 * - new page를 head로 갱신한다.
 */
	this_cpu_write(s->cpu_slab->partial, page);

	local_unlock_irqrestore(&s->cpu_slab->lock, flags);

/*
 * IAMROOT, 2022.06.25:
 * - page_to_unfreeze들을 node parital list나 buddy로 되돌려보낸다.
 */
	if (page_to_unfreeze) {
		__unfreeze_partials(s, page_to_unfreeze);
		stat(s, CPU_PARTIAL_DRAIN);
	}
}

#else	/* CONFIG_SLUB_CPU_PARTIAL */

static inline void unfreeze_partials(struct kmem_cache *s) { }
static inline void unfreeze_partials_cpu(struct kmem_cache *s,
				  struct kmem_cache_cpu *c) { }

#endif	/* CONFIG_SLUB_CPU_PARTIAL */

static inline void flush_slab(struct kmem_cache *s, struct kmem_cache_cpu *c)
{
	unsigned long flags;
	struct page *page;
	void *freelist;

	local_lock_irqsave(&s->cpu_slab->lock, flags);

	page = c->page;
	freelist = c->freelist;

	c->page = NULL;
	c->freelist = NULL;
	c->tid = next_tid(c->tid);

	local_unlock_irqrestore(&s->cpu_slab->lock, flags);

	if (page) {
		deactivate_slab(s, page, freelist);
		stat(s, CPUSLAB_FLUSH);
	}
}

/*
 * IAMROOT, 2022.06.25:
 * - cpu slab에 있는 page와 partial page들을 buddy, node partial로 되돌려 보낸다.
 */
static inline void __flush_cpu_slab(struct kmem_cache *s, int cpu)
{
	struct kmem_cache_cpu *c = per_cpu_ptr(s->cpu_slab, cpu);
	void *freelist = c->freelist;
	struct page *page = c->page;

	c->page = NULL;
	c->freelist = NULL;
	c->tid = next_tid(c->tid);

	if (page) {
		deactivate_slab(s, page, freelist);
		stat(s, CPUSLAB_FLUSH);
	}

	unfreeze_partials_cpu(s, c);
}

struct slub_flush_work {
	struct work_struct work;
	struct kmem_cache *s;
	bool skip;
};

/*
 * Flush cpu slab.
 *
 * Called from CPU work handler with migration disabled.
 */
static void flush_cpu_slab(struct work_struct *w)
{
	struct kmem_cache *s;
	struct kmem_cache_cpu *c;
	struct slub_flush_work *sfw;

	sfw = container_of(w, struct slub_flush_work, work);

	s = sfw->s;
	c = this_cpu_ptr(s->cpu_slab);

	if (c->page)
		flush_slab(s, c);

	unfreeze_partials(s);
}

static bool has_cpu_slab(int cpu, struct kmem_cache *s)
{
	struct kmem_cache_cpu *c = per_cpu_ptr(s->cpu_slab, cpu);

	return c->page || slub_percpu_partial(c);
}

static DEFINE_MUTEX(flush_lock);
static DEFINE_PER_CPU(struct slub_flush_work, slub_flush);

static void flush_all_cpus_locked(struct kmem_cache *s)
{
	struct slub_flush_work *sfw;
	unsigned int cpu;

	lockdep_assert_cpus_held();
	mutex_lock(&flush_lock);

	for_each_online_cpu(cpu) {
		sfw = &per_cpu(slub_flush, cpu);
		if (!has_cpu_slab(cpu, s)) {
			sfw->skip = true;
			continue;
		}
		INIT_WORK(&sfw->work, flush_cpu_slab);
		sfw->skip = false;
		sfw->s = s;
		schedule_work_on(cpu, &sfw->work);
	}

	for_each_online_cpu(cpu) {
		sfw = &per_cpu(slub_flush, cpu);
		if (sfw->skip)
			continue;
		flush_work(&sfw->work);
	}

	mutex_unlock(&flush_lock);
}

static void flush_all(struct kmem_cache *s)
{
	cpus_read_lock();
	flush_all_cpus_locked(s);
	cpus_read_unlock();
}

/*
 * Use the cpu notifier to insure that the cpu slabs are flushed when
 * necessary.
 */
static int slub_cpu_dead(unsigned int cpu)
{
	struct kmem_cache *s;

	mutex_lock(&slab_mutex);
	list_for_each_entry(s, &slab_caches, list)
		__flush_cpu_slab(s, cpu);
	mutex_unlock(&slab_mutex);
	return 0;
}

/*
 * Check if the objects in a per cpu structure fit numa
 * locality expectations.
 */
static inline int node_match(struct page *page, int node)
{
#ifdef CONFIG_NUMA
	if (node != NUMA_NO_NODE && page_to_nid(page) != node)
		return 0;
#endif
	return 1;
}

#ifdef CONFIG_SLUB_DEBUG
static int count_free(struct page *page)
{
	return page->objects - page->inuse;
}

static inline unsigned long node_nr_objs(struct kmem_cache_node *n)
{
	return atomic_long_read(&n->total_objects);
}
#endif /* CONFIG_SLUB_DEBUG */

#if defined(CONFIG_SLUB_DEBUG) || defined(CONFIG_SYSFS)
static unsigned long count_partial(struct kmem_cache_node *n,
					int (*get_count)(struct page *))
{
	unsigned long flags;
	unsigned long x = 0;
	struct page *page;

	spin_lock_irqsave(&n->list_lock, flags);
	list_for_each_entry(page, &n->partial, slab_list)
		x += get_count(page);
	spin_unlock_irqrestore(&n->list_lock, flags);
	return x;
}
#endif /* CONFIG_SLUB_DEBUG || CONFIG_SYSFS */

static noinline void
slab_out_of_memory(struct kmem_cache *s, gfp_t gfpflags, int nid)
{
#ifdef CONFIG_SLUB_DEBUG
	static DEFINE_RATELIMIT_STATE(slub_oom_rs, DEFAULT_RATELIMIT_INTERVAL,
				      DEFAULT_RATELIMIT_BURST);
	int node;
	struct kmem_cache_node *n;

	if ((gfpflags & __GFP_NOWARN) || !__ratelimit(&slub_oom_rs))
		return;

	pr_warn("SLUB: Unable to allocate memory on node %d, gfp=%#x(%pGg)\n",
		nid, gfpflags, &gfpflags);
	pr_warn("  cache: %s, object size: %u, buffer size: %u, default order: %u, min order: %u\n",
		s->name, s->object_size, s->size, oo_order(s->oo),
		oo_order(s->min));

	if (oo_order(s->min) > get_order(s->object_size))
		pr_warn("  %s debugging increased min order, use slub_debug=O to disable.\n",
			s->name);

	for_each_kmem_cache_node(s, node, n) {
		unsigned long nr_slabs;
		unsigned long nr_objs;
		unsigned long nr_free;

		nr_free  = count_partial(n, count_free);
		nr_slabs = node_nr_slabs(n);
		nr_objs  = node_nr_objs(n);

		pr_warn("  node %d: slabs: %ld, objs: %ld, free: %ld\n",
			node, nr_slabs, nr_objs, nr_free);
	}
#endif
}

/*
 * IAMROOT, 2022.06.18:
 * - 긴급메모리에서 가져온 page인지 확인하고 맞다면 @gfpflags가 긴급메모리에서
 *   실제로 요청한 flag인지 확인한다.
 */
static inline bool pfmemalloc_match(struct page *page, gfp_t gfpflags)
{
	if (unlikely(PageSlabPfmemalloc(page)))
		return gfp_pfmemalloc_allowed(gfpflags);

	return true;
}

/*
 * A variant of pfmemalloc_match() that tests page flags without asserting
 * PageSlab. Intended for opportunistic checks before taking a lock and
 * rechecking that nobody else freed the page under us.
 */
static inline bool pfmemalloc_match_unsafe(struct page *page, gfp_t gfpflags)
{
	if (unlikely(__PageSlabPfmemalloc(page)))
		return gfp_pfmemalloc_allowed(gfpflags);

	return true;
}

/*
 * Check the page->freelist of a page and either transfer the freelist to the
 * per cpu freelist or deactivate the page.
 *
 * The page is still frozen if the return value is not NULL.
 *
 * If this function returns NULL then the page has been unfrozen.
 */
static inline void *get_freelist(struct kmem_cache *s, struct page *page)
{
	struct page new;
	unsigned long counters;
	void *freelist;

	lockdep_assert_held(this_cpu_ptr(&s->cpu_slab->lock));

	do {
		freelist = page->freelist;
		counters = page->counters;

		new.counters = counters;
		VM_BUG_ON(!new.frozen);

		new.inuse = page->objects;
		new.frozen = freelist != NULL;

	} while (!__cmpxchg_double_slab(s, page,
		freelist, counters,
		NULL, new.counters,
		"get_freelist"));

	return freelist;
}

/*
 * Slow path. The lockless freelist is empty or we need to perform
 * debugging duties.
 *
 * Processing is still very fast if new objects have been freed to the
 * regular freelist. In that case we simply take over the regular freelist
 * as the lockless freelist and zap the regular freelist.
 *
 * If that is not working then we fall back to the partial lists. We take the
 * first element of the freelist as the object to allocate now and move the
 * rest of the freelist to the lockless freelist.
 *
 * And if we were unable to get a new slab from the partial slab lists then
 * we need to allocate a new slab. This is the slowest path since it involves
 * a call to the page allocator and the setup of a new slab.
 *
 * Version of __slab_alloc to use when we know that preemption is
 * already disabled (which is the case for bulk allocation).
 */

/*
 * IAMROOT, 2022.06.25:
 * - c에 page가 없는 경우.
 *   1. cpu parital에서 가져온다.
 *   2. cpu partial에서 없다면 node partial list에서 얻어온다.
 *   3. node parital list에도 없다면 buddy에서 slab page를 할당후 재시도.
 * - c에 page가 있는 경우
 *   c freelist에서 꺼내온다.
 */
static void *___slab_alloc(struct kmem_cache *s, gfp_t gfpflags, int node,
			  unsigned long addr, struct kmem_cache_cpu *c)
{
	void *freelist;
	struct page *page;
	unsigned long flags;

	stat(s, ALLOC_SLOWPATH);

reread_page:

/*
 * IAMROOT, 2022.06.18:
 * - @c에 page가 없는경우 바로 new_slab
 */
	page = READ_ONCE(c->page);
	if (!page) {
		/*
		 * if the node is not online or has no normal memory, just
		 * ignore the node constraint
		 */
		if (unlikely(node != NUMA_NO_NODE &&
			     !node_isset(node, slab_nodes)))
			node = NUMA_NO_NODE;
		goto new_slab;
	}
redo:

	if (unlikely(!node_match(page, node))) {
		/*
		 * same as above but node_match() being false already
		 * implies node != NUMA_NO_NODE
		 */
		if (!node_isset(node, slab_nodes)) {
			node = NUMA_NO_NODE;
			goto redo;
		} else {
			stat(s, ALLOC_NODE_MISMATCH);
			goto deactivate_slab;
		}
	}

	/*
	 * By rights, we should be searching for a slab page that was
	 * PFMEMALLOC but right now, we are losing the pfmemalloc
	 * information when the page leaves the per-cpu allocator
	 */
	if (unlikely(!pfmemalloc_match_unsafe(page, gfpflags)))
		goto deactivate_slab;

	/* must check again c->page in case we got preempted and it changed */
	local_lock_irqsave(&s->cpu_slab->lock, flags);
	if (unlikely(page != c->page)) {
		local_unlock_irqrestore(&s->cpu_slab->lock, flags);
		goto reread_page;
	}
	freelist = c->freelist;
	if (freelist)
		goto load_freelist;

	freelist = get_freelist(s, page);

	if (!freelist) {
		c->page = NULL;
		local_unlock_irqrestore(&s->cpu_slab->lock, flags);
		stat(s, DEACTIVATE_BYPASS);
		goto new_slab;
	}

	stat(s, ALLOC_REFILL);

load_freelist:

	lockdep_assert_held(this_cpu_ptr(&s->cpu_slab->lock));

	/*
	 * freelist is pointing to the list of objects to be used.
	 * page is pointing to the page from which the objects are obtained.
	 * That page must be frozen for per cpu allocations to work.
	 */
	VM_BUG_ON(!c->page->frozen);
/*
 * IAMROOT, 2022.06.18:
 * - c->freelist가 다음 object를 가리키게 한다.
 * - tid += TID_STEP
 */
	c->freelist = get_freepointer(s, freelist);
	c->tid = next_tid(c->tid);
	local_unlock_irqrestore(&s->cpu_slab->lock, flags);
	return freelist;

deactivate_slab:

	local_lock_irqsave(&s->cpu_slab->lock, flags);
	if (page != c->page) {
		local_unlock_irqrestore(&s->cpu_slab->lock, flags);
		goto reread_page;
	}
	freelist = c->freelist;
	c->page = NULL;
	c->freelist = NULL;
	local_unlock_irqrestore(&s->cpu_slab->lock, flags);
	deactivate_slab(s, page, freelist);

new_slab:

/*
 * IAMROOT, 2022.06.18:
 * - c->paritial page가 있는지 검사. 없다면 new_objects로 진행.
 */
	if (slub_percpu_partial(c)) {
		local_lock_irqsave(&s->cpu_slab->lock, flags);
		if (unlikely(c->page)) {
			local_unlock_irqrestore(&s->cpu_slab->lock, flags);
			goto reread_page;
		}
		if (unlikely(!slub_percpu_partial(c))) {
			local_unlock_irqrestore(&s->cpu_slab->lock, flags);
			/* we were preempted and partial list got empty */
			goto new_objects;
		}

		page = c->page = slub_percpu_partial(c);
		slub_set_percpu_partial(c, page);
		local_unlock_irqrestore(&s->cpu_slab->lock, flags);
		stat(s, CPU_PARTIAL_ALLOC);
		goto redo;
	}

new_objects:

/*
 * IAMROOT, 2022.06.18:
 * - 요청 @node의 parital에서 slab page를 가져와본다.
 *   n->paritial의 slab page가 없으면 freelist는 null.
 */
	freelist = get_partial(s, gfpflags, node, &page);
	if (freelist)
		goto check_new_page;

	slub_put_cpu_ptr(s->cpu_slab);
/*
 * IAMROOT, 2022.06.18:
 * - n->partial도 없었으므로 새로운 frozen상태의 slab page를 구해온다.
 */
	page = new_slab(s, gfpflags, node);
	c = slub_get_cpu_ptr(s->cpu_slab);

	if (unlikely(!page)) {
		slab_out_of_memory(s, gfpflags, node);
		return NULL;
	}

	/*
	 * No other reference to the page yet so we can
	 * muck around with it freely without cmpxchg
	 */

/*
 * IAMROOT, 2022.06.18:
 * - page->freelist를 null로 끊고 c freelist에 넣을 준비를 한다.
 */
	freelist = page->freelist;
	page->freelist = NULL;

	stat(s, ALLOC_SLAB);

check_new_page:

/*
 * IAMROOT, 2022.06.18:
 * - debug라면, alloc_debug_processing을 통해 잘못된 slab page라면 새로운 slab을
 *   할당받도록 new_slab으로 이동한다.
 */
	if (kmem_cache_debug(s)) {
		if (!alloc_debug_processing(s, page, freelist, addr)) {
			/* Slab failed checks. Next slab needed */
			goto new_slab;
		} else {
			/*
			 * For debug case, we don't load freelist so that all
			 * allocations go through alloc_debug_processing()
			 */
			goto return_single;
		}
	}

/*
 * IAMROOT, 2022.06.18:
 * - page가 긴급메모리에서 가져왔는데 @gfpflags에 긴급요청이 없었으면. 즉
 *   flag와 맞지 않게 메모리를 가져왔으면 return_single.
 */
	if (unlikely(!pfmemalloc_match(page, gfpflags)))
		/*
		 * For !pfmemalloc_match() case we don't load freelist so that
		 * we don't make further mismatched allocations easier.
		 */
		goto return_single;

retry_load_page:

	local_lock_irqsave(&s->cpu_slab->lock, flags);
	if (unlikely(c->page)) {
		void *flush_freelist = c->freelist;
		struct page *flush_page = c->page;

		c->page = NULL;
		c->freelist = NULL;
		c->tid = next_tid(c->tid);

		local_unlock_irqrestore(&s->cpu_slab->lock, flags);

		deactivate_slab(s, flush_page, flush_freelist);

		stat(s, CPUSLAB_FLUSH);

		goto retry_load_page;
	}
	c->page = page;

	goto load_freelist;

return_single:

	deactivate_slab(s, page, get_freepointer(s, freelist));
	return freelist;
}

/*
 * A wrapper for ___slab_alloc() for contexts where preemption is not yet
 * disabled. Compensates for possible cpu changes by refetching the per cpu area
 * pointer.
 */
/*
 * IAMROOT, 2022.06.25:
 * - slab object 하나를 할당해온다.
 */
static void *__slab_alloc(struct kmem_cache *s, gfp_t gfpflags, int node,
			  unsigned long addr, struct kmem_cache_cpu *c)
{
	void *p;

#ifdef CONFIG_PREEMPT_COUNT
	/*
	 * We may have been preempted and rescheduled on a different
	 * cpu before disabling preemption. Need to reload cpu area
	 * pointer.
	 */
	c = slub_get_cpu_ptr(s->cpu_slab);
#endif

	p = ___slab_alloc(s, gfpflags, node, addr, c);
#ifdef CONFIG_PREEMPT_COUNT
	slub_put_cpu_ptr(s->cpu_slab);
#endif
	return p;
}

/*
 * If the object has been wiped upon free, make sure it's fully initialized by
 * zeroing out freelist pointer.
 */
/*
 * IAMROOT, 2022.06.25:
 * - init on free 설정이 존재하면 fp를 0로 초기화한다.
 */
static __always_inline void maybe_wipe_obj_freeptr(struct kmem_cache *s,
						   void *obj)
{
	if (unlikely(slab_want_init_on_free(s)) && obj)
		memset((void *)((char *)kasan_reset_tag(obj) + s->offset),
			0, sizeof(void *));
}

/*
 * Inlined fastpath so that allocation functions (kmalloc, kmem_cache_alloc)
 * have the fastpath folded into their functions. So no function call
 * overhead for requests that can be satisfied on the fastpath.
 *
 * The fastpath works by first checking if the lockless freelist can be used.
 * If not then __slab_alloc is called for slow processing.
 *
 * Otherwise we can simply pick the next object from the lockless free list.
 */
/*
 * IAMROOT, 2022.06.18:
 * - papago
 *   할당 기능(kmalloc, kmem_cache_alloc)이 해당 기능으로 접힌 fastpath를 갖도록
 *   인라인된 fastpath. 따라서 빠른 경로에서 충족될 수 있는 요청에 대한 함수
 *   호출 오버헤드가 없습니다.
 *
 *   fastpath는 먼저 lockless freelist를 사용할 수 있는지 확인하여 작동합니다.
 *   그렇지 않은 경우 느린 처리를 위해 __slab_alloc이 호출됩니다.
 *
 *   그렇지 않으면 잠금 없는 자유 목록에서 다음 개체를 간단히 선택할 수 있습니다.*
 *
 * - object를 요청한 cache의 node에서 할당을 한다.
 */
static __always_inline void *slab_alloc_node(struct kmem_cache *s,
		gfp_t gfpflags, int node, unsigned long addr, size_t orig_size)
{
	void *object;
	struct kmem_cache_cpu *c;
	struct page *page;
	unsigned long tid;
	struct obj_cgroup *objcg = NULL;
	bool init = false;

	s = slab_pre_alloc_hook(s, &objcg, 1, gfpflags);
	if (!s)
		return NULL;

	object = kfence_alloc(s, orig_size, gfpflags);
	if (unlikely(object))
		goto out;

redo:
	/*
	 * Must read kmem_cache cpu data via this cpu ptr. Preemption is
	 * enabled. We may switch back and forth between cpus while
	 * reading from one cpu area. That does not matter as long
	 * as we end up on the original cpu again when doing the cmpxchg.
	 *
	 * We must guarantee that tid and kmem_cache_cpu are retrieved on the
	 * same cpu. We read first the kmem_cache_cpu pointer and use it to read
	 * the tid. If we are preempted and switched to another cpu between the
	 * two reads, it's OK as the two are still associated with the same cpu
	 * and cmpxchg later will validate the cpu.
	 */
/*
 * IAMROOT, 2022.06.18:
 * - papago
 *   이 cpu ptr을 통해 kmem_cache cpu 데이터를 읽어야 합니다. 선점이
 *   활성화되었습니다. 한 CPU 영역에서 읽는 동안 CPU 간에 앞뒤로 전환할 수 있습니다.
 *   cmpxchg를 수행할 때 원래 CPU에서 다시 종료되는 한 문제가 되지 않습니다.
 *
 *   tid 및 kmem_cache_cpu가 동일한 CPU에서 검색되도록 보장해야 합니다.
 *   먼저 kmem_cache_cpu 포인터를 읽고 tid를 읽는 데 사용합니다.
 *   우리가 선점되어 두 읽기 사이에 다른 CPU로 전환되는 경우 두 읽기가
 *   여전히 동일한 CPU와 연결되어 있고 나중에 cmpxchg에서 CPU의 유효성을
 *   검사하므로 괜찮습니다.
 */
	c = raw_cpu_ptr(s->cpu_slab);
	tid = READ_ONCE(c->tid);

	/*
	 * Irqless object alloc/free algorithm used here depends on sequence
	 * of fetching cpu_slab's data. tid should be fetched before anything
	 * on c to guarantee that object and page associated with previous tid
	 * won't be used with current tid. If we fetch tid first, object and
	 * page could be one associated with next tid and our alloc/free
	 * request will be failed. In this case, we will retry. So, no problem.
	 */
/*
 * IAMROOT, 2022.06.18:
 * papago
 * 여기서 사용되는 Irqless 객체 할당/해제 알고리즘은 cpu_slab의 데이터를 가져오는
 * 순서에 따라 다릅니다. tid는 이전 tid와 연결된 개체 및 페이지가 현재 tid와 함께
 * 사용되지 않도록 보장하기 위해 c에서 무엇보다 먼저 가져와야 합니다. tid를 먼저
 * 가져오면 객체와 페이지가 다음 tid와 연결될 수 있으며 할당/해제 요청이
 * 실패합니다. 이 경우 다시 시도합니다. 문제 없습니다. 
 */
	barrier();

	/*
	 * The transaction ids are globally unique per cpu and per operation on
	 * a per cpu queue. Thus they can be guarantee that the cmpxchg_double
	 * occurs on the right processor and that there was no operation on the
	 * linked list in between.
	 */
/*
 * IAMROOT, 2022.06.18:
 * - 트랜잭션 ID는 CPU별 및 CPU별 대기열의 작업별로 global적으로 고유합니다.
 *   따라서 cmpxchg_double이 올바른 프로세서에서 발생하고 그 사이에 연결된 목록에
 *   대한 작업이 없음을 보장할 수 있습니다.
 */

	object = c->freelist;
	page = c->page;
	/*
	 * We cannot use the lockless fastpath on PREEMPT_RT because if a
	 * slowpath has taken the local_lock_irqsave(), it is not protected
	 * against a fast path operation in an irq handler. So we need to take
	 * the slow path which uses local_lock. It is still relatively fast if
	 * there is a suitable cpu freelist.
	 */
/*
 * IAMROOT, 2022.06.18:
 * - slowpath가 local_lock_irqsave()를 사용하는 경우 irq 처리기의 fastpath 작업에
 *   대해 보호되지 않기 때문에 PREEMPT_RT에서 잠금 없는 빠른 경로를 사용할 수
 *   없습니다. 따라서 local_lock을 사용하는 느린 경로를 선택해야 합니다. 적절한
 *   CPU 여유 목록이 있으면 여전히 상대적으로 빠릅니다.
 *
 * - object == NULL : freelist가 하나도 없다.
 *   page == 0      : page가 없다.
 *   !node_match    : 요청 node와 cache의 node가 다른 경우. 요청한 node에서 가져온다.
 *
 * - CONFIG_PREEMPT_RT
 *   선점형인 경우 irq disable을 해도 irq가 들어오는 상황이 발생한다. 즉 lock없이
 *   fastpath 처리를 할수없다.
 *
 * - slowpath로 할지 fastpath로 할지 결정한다.
 */
	if (IS_ENABLED(CONFIG_PREEMPT_RT) ||
	    unlikely(!object || !page || !node_match(page, node))) {
		object = __slab_alloc(s, gfpflags, node, addr, c);
	} else {
		void *next_object = get_freepointer_safe(s, object);

		/*
		 * The cmpxchg will only match if there was no additional
		 * operation and if we are on the right processor.
		 *
		 * The cmpxchg does the following atomically (without lock
		 * semantics!)
		 * 1. Relocate first pointer to the current per cpu area.
		 * 2. Verify that tid and freelist have not been changed
		 * 3. If they were not changed replace tid and freelist
		 *
		 * Since this is without lock semantics the protection is only
		 * against code executing on this cpu *not* from access by
		 * other cpus.
		 */
/*
 * IAMROOT, 2022.06.18:
 * - papgo
 *   cmpxchg는 추가 작업이 없고 올바른 프로세서에 있는 경우에만 일치합니다.
 *
 *   cmpxchg는 원자적으로 다음을 수행합니다(잠금 의미 없이!).
 *   1. 첫 번째 포인터를 현재 CPU당 영역으로 재배치합니다.
 *   2. tid 및 freelist가 변경되지 않았는지 확인합니다.
 *   3. 변경되지 않은 경우 tid 및 freelist를 교체합니다.
 *   이것은 잠금 의미가 없기 때문에 보호는 이 CPU에서 실행되는 코드에 대해서만
 *   다른 CPU에 의해 액세스되지 않습니다.
 *
 * - s->cpu_slab->freelist, s->cpu_slab->tid 값이 object, tid와 맞는지 확인하고,
 *   맞다면 next_object, next_tid(tid)으로 바뀔것이다.
 *
 * - 실패 상황판단 방법의 이유
 *   -- tid 비교 이유
 *	preemption 발생으로 인해 cpu가 변경됬을 경우 tid가 바뀔수있다.
 *   -- freelist, object 비교 이유
 *	다른 cpu가 먼저 할당한지 여부 판단.
 */
		if (unlikely(!this_cpu_cmpxchg_double(
				s->cpu_slab->freelist, s->cpu_slab->tid,
				object, tid,
				next_object, next_tid(tid)))) {

			note_cmpxchg_failure("slab_alloc", s, tid);
			goto redo;
		}
/*
 * IAMROOT, 2022.06.25:
 * - next_object를 prefetch한다.
 */
		prefetch_freepointer(s, next_object);
		stat(s, ALLOC_FASTPATH);
	}

	maybe_wipe_obj_freeptr(s, object);
	init = slab_want_init_on_alloc(gfpflags, s);

out:
	slab_post_alloc_hook(s, objcg, gfpflags, 1, &object, init);

	return object;
}

/*
 * IAMROOT, 2022.06.18:
 * - object를 요청한 cache에서 모든 node를 대상으로 할당을 시도한다.
 *   pcpu -> local node -> remote node 순으로 시도를 할것이다.
 */
static __always_inline void *slab_alloc(struct kmem_cache *s,
		gfp_t gfpflags, unsigned long addr, size_t orig_size)
{
	return slab_alloc_node(s, gfpflags, NUMA_NO_NODE, addr, orig_size);
}

/*
 * IAMROOT, 2022.06.25:
 * - object를 요청한 cache에서 모든 node를 대상으로 할당을 시도한다.
 */
void *kmem_cache_alloc(struct kmem_cache *s, gfp_t gfpflags)
{
	void *ret = slab_alloc(s, gfpflags, _RET_IP_, s->object_size);

	trace_kmem_cache_alloc(_RET_IP_, ret, s->object_size,
				s->size, gfpflags);

	return ret;
}
EXPORT_SYMBOL(kmem_cache_alloc);

#ifdef CONFIG_TRACING
void *kmem_cache_alloc_trace(struct kmem_cache *s, gfp_t gfpflags, size_t size)
{
	void *ret = slab_alloc(s, gfpflags, _RET_IP_, size);
	trace_kmalloc(_RET_IP_, ret, size, s->size, gfpflags);
	ret = kasan_kmalloc(s, ret, size, gfpflags);
	return ret;
}
EXPORT_SYMBOL(kmem_cache_alloc_trace);
#endif

#ifdef CONFIG_NUMA
void *kmem_cache_alloc_node(struct kmem_cache *s, gfp_t gfpflags, int node)
{
	void *ret = slab_alloc_node(s, gfpflags, node, _RET_IP_, s->object_size);

	trace_kmem_cache_alloc_node(_RET_IP_, ret,
				    s->object_size, s->size, gfpflags, node);

	return ret;
}
EXPORT_SYMBOL(kmem_cache_alloc_node);

#ifdef CONFIG_TRACING
void *kmem_cache_alloc_node_trace(struct kmem_cache *s,
				    gfp_t gfpflags,
				    int node, size_t size)
{
	void *ret = slab_alloc_node(s, gfpflags, node, _RET_IP_, size);

	trace_kmalloc_node(_RET_IP_, ret,
			   size, s->size, gfpflags, node);

	ret = kasan_kmalloc(s, ret, size, gfpflags);
	return ret;
}
EXPORT_SYMBOL(kmem_cache_alloc_node_trace);
#endif
#endif	/* CONFIG_NUMA */

/*
 * Slow path handling. This may still be called frequently since objects
 * have a longer lifetime than the cpu slabs in most processing loads.
 *
 * So we still attempt to reduce cache line usage. Just take the slab
 * lock and free the item. If there is no additional partial page
 * handling required then we can return immediately.
 */
/*
 * IAMROOT, 2022.06.25:
 * - papago
 *   느린 경로 처리. 대부분의 처리 부하에서 객체가 CPU slab보다 수명이 더 길기
 *   때문에 여전히 자주 호출될 수 있습니다.
 *
 *   그래서 우리는 여전히 캐시 라인 사용량을 줄이려고 합니다. slab lock를 잡고
 *   item을 free하십시오. 추가 부분 페이지 처리가 필요하지 않으면 즉시 반환할 수
 *   있습니다.
 */
static void __slab_free(struct kmem_cache *s, struct page *page,
			void *head, void *tail, int cnt,
			unsigned long addr)

{
	void *prior;
	int was_frozen;
	struct page new;
	unsigned long counters;
	struct kmem_cache_node *n = NULL;
	unsigned long flags;

	stat(s, FREE_SLOWPATH);

	if (kfence_free(head))
		return;

	if (kmem_cache_debug(s) &&
	    !free_debug_processing(s, page, head, tail, cnt, addr))
		return;

	do {
		if (unlikely(n)) {
			spin_unlock_irqrestore(&n->list_lock, flags);
			n = NULL;
		}
		prior = page->freelist;
		counters = page->counters;
		set_freepointer(s, tail, prior);
		new.counters = counters;
		was_frozen = new.frozen;

/*
 * IAMROOT, 2022.06.25:
 * - cnt만큼 해제를 하므로 inuse를 빼준다.
 */
		new.inuse -= cnt;

/*
 * IAMROOT, 2022.06.25:
 * - !new.inuse  = 사용중인 object가 없는 상황. 전부 free object인 상황.
 *                 node freelist에 있다고 생각됨.
 *   !prior      = 해당 page->freelist가 없는상태.
 *   !was_frozen = cpu parital이나 cpu list에 없는 상황.
 */
		if ((!new.inuse || !prior) && !was_frozen) {

			if (kmem_cache_has_cpu_partial(s) && !prior) {

/*
 * IAMROOT, 2022.06.25:
 * - !prior
 *   전부 사용중인 page였다가 하나가 free가 된상황.
 */
				/*
				 * Slab was on no list before and will be
				 * partially empty
				 * We can defer the list move and instead
				 * freeze it.
				 */
				new.frozen = 1;

			} else { /* Needs to be taken off a list */

				n = get_node(s, page_to_nid(page));
				/*
				 * Speculatively acquire the list_lock.
				 * If the cmpxchg does not succeed then we may
				 * drop the list_lock without any processing.
				 *
				 * Otherwise the list_lock will synchronize with
				 * other processors updating the list of slabs.
				 */

/*
 * IAMROOT, 2022.06.25:
 * - papgo
 *   list_lock을 추측하여 획득합니다.
 *   cmpxchg가 성공하지 못하면 처리 없이 list_lock을 삭제할 수 있습니다.
 *
 *   그렇지 않으면 list_lock이 슬래브 목록을 업데이트하는 다른 프로세서와
 *   동기화됩니다.
 */
				spin_lock_irqsave(&n->list_lock, flags);

			}
		}

	} while (!cmpxchg_double_slab(s, page,
		prior, counters,
		head, new.counters,
		"__slab_free"));

	if (likely(!n)) {

		if (likely(was_frozen)) {
			/*
			 * The list lock was not taken therefore no list
			 * activity can be necessary.
			 */
			stat(s, FREE_FROZEN);
		} else if (new.frozen) {
			/*
			 * If we just froze the page then put it onto the
			 * per cpu partial list.
			 */
			put_cpu_partial(s, page, 1);
			stat(s, CPU_PARTIAL_FREE);
		}

		return;
	}


/*
 * IAMROOT, 2022.06.25:
 * - node partial list
 * - slab page 내의 모든 object가 free 상태이고, 이미 node parital에 많이 있다면
 *   slab page를 되돌려보낸다.
 */
	if (unlikely(!new.inuse && n->nr_partial >= s->min_partial))
		goto slab_empty;

	/*
	 * Objects left in the slab. If it was not on the partial list before
	 * then add it.
	 */
	if (!kmem_cache_has_cpu_partial(s) && unlikely(!prior)) {
		remove_full(s, n, page);
		add_partial(n, page, DEACTIVATE_TO_TAIL);
		stat(s, FREE_ADD_PARTIAL);
	}
	spin_unlock_irqrestore(&n->list_lock, flags);
	return;

slab_empty:
	if (prior) {
/*
 * IAMROOT, 2022.06.25:
 * - node partial list로부터 해당 slab page를 제거한다.
 */
		/*
		 * Slab on the partial list.
		 */
		remove_partial(n, page);
		stat(s, FREE_REMOVE_PARTIAL);
	} else {
		/* Slab must be on the full list */
		remove_full(s, n, page);
	}

	spin_unlock_irqrestore(&n->list_lock, flags);
	stat(s, FREE_SLAB);
	discard_slab(s, page);
}

/*
 * Fastpath with forced inlining to produce a kfree and kmem_cache_free that
 * can perform fastpath freeing without additional function calls.
 *
 * The fastpath is only possible if we are freeing to the current cpu slab
 * of this processor. This typically the case if we have just allocated
 * the item before.
 *
 * If fastpath is not possible then fall back to __slab_free where we deal
 * with all sorts of special processing.
 *
 * Bulk free of a freelist with several objects (all pointing to the
 * same page) possible by specifying head and tail ptr, plus objects
 * count (cnt). Bulk free indicated by tail pointer being set.
 */
static __always_inline void do_slab_free(struct kmem_cache *s,
				struct page *page, void *head, void *tail,
				int cnt, unsigned long addr)
{
	void *tail_obj = tail ? : head;
	struct kmem_cache_cpu *c;
	unsigned long tid;

	/* memcg_slab_free_hook() is already called for bulk free. */
/*
 * IAMROOT, 2022.06.25:
 * - tail == NULL이면 1개라는 의미.
 */
	if (!tail)
		memcg_slab_free_hook(s, &head, 1);
redo:
	/*
	 * Determine the currently cpus per cpu slab.
	 * The cpu may change afterward. However that does not matter since
	 * data is retrieved via this pointer. If we are on the same cpu
	 * during the cmpxchg then the free will succeed.
	 */
/*
 * IAMROOT, 2022.06.25:
 * - papago
 *   CPU slab당 현재 CPU를 확인합니다.
 *   cpu는 나중에 변경될 수 있습니다. 그러나 이 포인터를 통해 데이터를 검색하므로
 *   이는 중요하지 않습니다. cmpxchg 동안 동일한 CPU에 있으면 free가 성공합니다. 
 */
	c = raw_cpu_ptr(s->cpu_slab);
	tid = READ_ONCE(c->tid);

	/* Same with comment on barrier() in slab_alloc_node() */
	barrier();

/*
 * IAMROOT, 2022.06.25:
 * - free 할 object가 c에서 관리되고 있는 경우 fast-path를 사용하여 
 *   object를 반환처리를 수행한다..
 *   즉 cpu 슬랩에서 할당 관리되고 있는 page의 slab object가 할당해제요청이 온것.
 *   -> 할당한 지 얼마 안된 시점에서 반납이 이루어진 case이다.
 */
	if (likely(page == c->page)) {
#ifndef CONFIG_PREEMPT_RT
		void **freelist = READ_ONCE(c->freelist);

/*
 * IAMROOT, 2022.06.25:
 * - c->freelist = new_head -> ... -> new_tail -> old 와 같이 연결된다.
 */
		set_freepointer(s, tail_obj, freelist);

		if (unlikely(!this_cpu_cmpxchg_double(
				s->cpu_slab->freelist, s->cpu_slab->tid,
				freelist, tid,
				head, next_tid(tid)))) {

			note_cmpxchg_failure("slab_free", s, tid);
			goto redo;
		}
#else /* CONFIG_PREEMPT_RT */
		/*
		 * We cannot use the lockless fastpath on PREEMPT_RT because if
		 * a slowpath has taken the local_lock_irqsave(), it is not
		 * protected against a fast path operation in an irq handler. So
		 * we need to take the local_lock. We shouldn't simply defer to
		 * __slab_free() as that wouldn't use the cpu freelist at all.
		 */
		void **freelist;

/*
 * IAMROOT, 2022.06.25:
 * - preemption은 lock이 필요하다. lock을 걸었으므로 atomic연산이 불필요하다.
 */
		local_lock(&s->cpu_slab->lock);
		c = this_cpu_ptr(s->cpu_slab);
		if (unlikely(page != c->page)) {
			local_unlock(&s->cpu_slab->lock);
			goto redo;
		}
		tid = c->tid;
		freelist = c->freelist;

		set_freepointer(s, tail_obj, freelist);
		c->freelist = head;
		c->tid = next_tid(tid);

		local_unlock(&s->cpu_slab->lock);
#endif
		stat(s, FREE_FASTPATH);
	} else
/*
 * IAMROOT, 2022.07.02: 
 * cpu 슬랩이 처리하고 있는 page가 아닌 경우에는 slow-path 할당 해제 루틴을 
 * 사용한다.
 */
		__slab_free(s, page, head, tail_obj, cnt, addr);

}

static __always_inline void slab_free(struct kmem_cache *s, struct page *page,
				      void *head, void *tail, int cnt,
				      unsigned long addr)
{
	/*
	 * With KASAN enabled slab_free_freelist_hook modifies the freelist
	 * to remove objects, whose reuse must be delayed.
	 */
	if (slab_free_freelist_hook(s, &head, &tail, &cnt))
		do_slab_free(s, page, head, tail, cnt, addr);
}

#ifdef CONFIG_KASAN_GENERIC
void ___cache_free(struct kmem_cache *cache, void *x, unsigned long addr)
{
	do_slab_free(cache, virt_to_head_page(x), x, NULL, 1, addr);
}
#endif

void kmem_cache_free(struct kmem_cache *s, void *x)
{
	s = cache_from_obj(s, x);
	if (!s)
		return;
	slab_free(s, virt_to_head_page(x), x, NULL, 1, _RET_IP_);
	trace_kmem_cache_free(_RET_IP_, x, s->name);
}
EXPORT_SYMBOL(kmem_cache_free);

struct detached_freelist {
	struct page *page;
	void *tail;
	void *freelist;
	int cnt;
	struct kmem_cache *s;
};

static inline void free_nonslab_page(struct page *page, void *object)
{
	unsigned int order = compound_order(page);

	VM_BUG_ON_PAGE(!PageCompound(page), page);
	kfree_hook(object);
	mod_lruvec_page_state(page, NR_SLAB_UNRECLAIMABLE_B, -(PAGE_SIZE << order));
	__free_pages(page, order);
}

/*
 * This function progressively scans the array with free objects (with
 * a limited look ahead) and extract objects belonging to the same
 * page.  It builds a detached freelist directly within the given
 * page/objects.  This can happen without any need for
 * synchronization, because the objects are owned by running process.
 * The freelist is build up as a single linked list in the objects.
 * The idea is, that this detached freelist can then be bulk
 * transferred to the real freelist(s), but only requiring a single
 * synchronization primitive.  Look ahead in the array is limited due
 * to performance reasons.
 */
static inline
int build_detached_freelist(struct kmem_cache *s, size_t size,
			    void **p, struct detached_freelist *df)
{
	size_t first_skipped_index = 0;
	int lookahead = 3;
	void *object;
	struct page *page;

	/* Always re-init detached_freelist */
	df->page = NULL;

	do {
		object = p[--size];
		/* Do we need !ZERO_OR_NULL_PTR(object) here? (for kfree) */
	} while (!object && size);

	if (!object)
		return 0;

	page = virt_to_head_page(object);
	if (!s) {
		/* Handle kalloc'ed objects */
		if (unlikely(!PageSlab(page))) {
			free_nonslab_page(page, object);
			p[size] = NULL; /* mark object processed */
			return size;
		}
		/* Derive kmem_cache from object */
		df->s = page->slab_cache;
	} else {
		df->s = cache_from_obj(s, object); /* Support for memcg */
	}

	if (is_kfence_address(object)) {
		slab_free_hook(df->s, object, false);
		__kfence_free(object);
		p[size] = NULL; /* mark object processed */
		return size;
	}

	/* Start new detached freelist */
	df->page = page;
	set_freepointer(df->s, object, NULL);
	df->tail = object;
	df->freelist = object;
	p[size] = NULL; /* mark object processed */
	df->cnt = 1;

	while (size) {
		object = p[--size];
		if (!object)
			continue; /* Skip processed objects */

		/* df->page is always set at this point */
		if (df->page == virt_to_head_page(object)) {
			/* Opportunity build freelist */
			set_freepointer(df->s, object, df->freelist);
			df->freelist = object;
			df->cnt++;
			p[size] = NULL; /* mark object processed */

			continue;
		}

		/* Limit look ahead search */
		if (!--lookahead)
			break;

		if (!first_skipped_index)
			first_skipped_index = size + 1;
	}

	return first_skipped_index;
}

/* Note that interrupts must be enabled when calling this function. */
void kmem_cache_free_bulk(struct kmem_cache *s, size_t size, void **p)
{
	if (WARN_ON(!size))
		return;

	memcg_slab_free_hook(s, p, size);
	do {
		struct detached_freelist df;

		size = build_detached_freelist(s, size, p, &df);
		if (!df.page)
			continue;

		slab_free(df.s, df.page, df.freelist, df.tail, df.cnt, _RET_IP_);
	} while (likely(size));
}
EXPORT_SYMBOL(kmem_cache_free_bulk);

/* Note that interrupts must be enabled when calling this function. */
int kmem_cache_alloc_bulk(struct kmem_cache *s, gfp_t flags, size_t size,
			  void **p)
{
	struct kmem_cache_cpu *c;
	int i;
	struct obj_cgroup *objcg = NULL;

	/* memcg and kmem_cache debug support */
	s = slab_pre_alloc_hook(s, &objcg, size, flags);
	if (unlikely(!s))
		return false;
	/*
	 * Drain objects in the per cpu slab, while disabling local
	 * IRQs, which protects against PREEMPT and interrupts
	 * handlers invoking normal fastpath.
	 */
	c = slub_get_cpu_ptr(s->cpu_slab);
	local_lock_irq(&s->cpu_slab->lock);

	for (i = 0; i < size; i++) {
		void *object = kfence_alloc(s, s->object_size, flags);

		if (unlikely(object)) {
			p[i] = object;
			continue;
		}

		object = c->freelist;
		if (unlikely(!object)) {
			/*
			 * We may have removed an object from c->freelist using
			 * the fastpath in the previous iteration; in that case,
			 * c->tid has not been bumped yet.
			 * Since ___slab_alloc() may reenable interrupts while
			 * allocating memory, we should bump c->tid now.
			 */
			c->tid = next_tid(c->tid);

			local_unlock_irq(&s->cpu_slab->lock);

			/*
			 * Invoking slow path likely have side-effect
			 * of re-populating per CPU c->freelist
			 */
			p[i] = ___slab_alloc(s, flags, NUMA_NO_NODE,
					    _RET_IP_, c);
			if (unlikely(!p[i]))
				goto error;

			c = this_cpu_ptr(s->cpu_slab);
			maybe_wipe_obj_freeptr(s, p[i]);

			local_lock_irq(&s->cpu_slab->lock);

			continue; /* goto for-loop */
		}
		c->freelist = get_freepointer(s, object);
		p[i] = object;
		maybe_wipe_obj_freeptr(s, p[i]);
	}
	c->tid = next_tid(c->tid);
	local_unlock_irq(&s->cpu_slab->lock);
	slub_put_cpu_ptr(s->cpu_slab);

	/*
	 * memcg and kmem_cache debug support and memory initialization.
	 * Done outside of the IRQ disabled fastpath loop.
	 */
	slab_post_alloc_hook(s, objcg, flags, size, p,
				slab_want_init_on_alloc(flags, s));
	return i;
error:
	slub_put_cpu_ptr(s->cpu_slab);
	slab_post_alloc_hook(s, objcg, flags, i, p, false);
	__kmem_cache_free_bulk(s, i, p);
	return 0;
}
EXPORT_SYMBOL(kmem_cache_alloc_bulk);


/*
 * Object placement in a slab is made very easy because we always start at
 * offset 0. If we tune the size of the object to the alignment then we can
 * get the required alignment by putting one properly sized object after
 * another.
 *
 * Notice that the allocation order determines the sizes of the per cpu
 * caches. Each processor has always one slab available for allocations.
 * Increasing the allocation order reduces the number of times that slabs
 * must be moved on and off the partial lists and is therefore a factor in
 * locking overhead.
 */

/*
 * Minimum / Maximum order of slab pages. This influences locking overhead
 * and slab fragmentation. A higher order reduces the number of partial slabs
 * and increases the number of allocations possible without having to
 * take the list_lock.
 */
static unsigned int slub_min_order;
static unsigned int slub_max_order = PAGE_ALLOC_COSTLY_ORDER;
static unsigned int slub_min_objects;

/*
 * Calculate the order of allocation given an slab object size.
 *
 * The order of allocation has significant impact on performance and other
 * system components. Generally order 0 allocations should be preferred since
 * order 0 does not cause fragmentation in the page allocator. Larger objects
 * be problematic to put into order 0 slabs because there may be too much
 * unused space left. We go to a higher order if more than 1/16th of the slab
 * would be wasted.
 *
 * In order to reach satisfactory performance we must ensure that a minimum
 * number of objects is in one slab. Otherwise we may generate too much
 * activity on the partial lists which requires taking the list_lock. This is
 * less a concern for large slabs though which are rarely used.
 *
 * slub_max_order specifies the order where we begin to stop considering the
 * number of objects in a slab as critical. If we reach slub_max_order then
 * we try to keep the page order as low as possible. So we accept more waste
 * of space in favor of a small page order.
 *
 * Higher order allocations also allow the placement of more objects in a
 * slab and thereby reduce object handling overhead. If the user has
 * requested a higher minimum order then we start with that one instead of
 * the smallest order which will fit the object.
 */

/*
 * IAMROOT, 2022.06.18:
 * - papgo
 *  slab object 크기가 주어지면 할당 순서를 계산합니다.
 *  
 *  할당 순서는 성능 및 기타 시스템 구성 요소에 상당한 영향을 미칩니다.
 *  일반적으로 0차 할당은 페이지 할당자에서 조각화를 일으키지 않으므로 0차 할당이
 *  선호되어야 합니다. 더 큰 개체는 사용하지 않은 공간이 너무 많이 남아 있을 수
 *  있으므로 순서 0 슬래브에 넣는 데 문제가 있습니다. 슬래브의 1/16 이상이
 *  낭비되면 더 높은 순서로 이동합니다.
 *  
 *  만족스러운 성능에 도달하려면 하나의 슬래브에 최소한의 개체 수를 보장해야 합니다.
 *  그렇지 않으면 list_lock을 취해야 하는 부분 목록에서 너무 많은 활동을 생성할 수
 *  있습니다. 이것은 거의 사용되지 않는 대형 슬래브의 경우 덜 우려됩니다.
 *  
 *  slub_max_order는 슬래브에 있는 객체의 수를 중요한 것으로 간주하는 것을 중단하기
 *  시작하는 순서를 지정합니다. slub_max_order에 도달하면 페이지 순서를 가능한 한
 *  낮게 유지하려고 합니다. 그래서 우리는 작은 페이지 주문에 찬성하여 더 많은 공간
 *  낭비를 받아들입니다.
 *
 *  고차 할당은 또한 슬래브에 더 많은 개체를 배치할 수 있게 하여 개체 처리
 *  오버헤드를 줄입니다. 사용자가 더 높은 최소 주문을 요청한 경우 개체에 맞는
 *  최소 주문 대신 해당 주문부터 시작합니다.
 *
 * - slab_size / fract의 크기가 unused 보다 큰지를 확인한다. 낭비공간이 적은
 *   order값을 얻기 위한 작업이다.
 */
static inline unsigned int slab_order(unsigned int size,
		unsigned int min_objects, unsigned int max_order,
		unsigned int fract_leftover)
{
	unsigned int min_order = slub_min_order;
	unsigned int order;

/*
 * IAMROOT, 2022.06.18:
 * - min_order에 대해서 object개수가 너무 많으면(min order가 너무 큰 상황)
 *   MAX_OBJS_PER_PAGE의 기준으로 맞춰서 order를 구한다.
 *
 * - ex) size = 16, min_order = 7
 *   4k * 2^x / 16 >= 32k => 2^x >= 2^7 => x >= 7
 *   order_objects값이 32k가 나와 MAX_OBJS_PER_PAGE값이 초과.
 *   즉 min_order값이 너무 크다.
 *   get_order(16 * 32767) - 1 => 6
 */
	if (order_objects(min_order, size) > MAX_OBJS_PER_PAGE)
		return get_order(size * MAX_OBJS_PER_PAGE) - 1;

/*
 * IAMROOT, 2022.06.18:
 * - min_order ~ max_order
 */
	for (order = max(min_order, (unsigned int)get_order(min_objects * size));
			order <= max_order; order++) {

		unsigned int slab_size = (unsigned int)PAGE_SIZE << order;
		unsigned int rem;

		rem = slab_size % size;

/*
 * IAMROOT, 2022.06.18:
 * - slab_size / fract_leftover보다 적으면. 즉 낭비가 적으면 break.
 */
		if (rem <= slab_size / fract_leftover)
			break;
	}

	return order;
}

/*
 * IAMROOT, 2022.06.18:
 * - @size를 사용해 min_objects를 기준으로 min_order ~ max_order사이의 값으로
 *   가장 낭비가 적은 order를 구한다. 단 범위를 벗어날수도 있다.
 */
static inline int calculate_order(unsigned int size)
{
	unsigned int order;
	unsigned int min_objects;
	unsigned int max_objects;
	unsigned int nr_cpus;

	/*
	 * Attempt to find best configuration for a slab. This
	 * works by first attempting to generate a layout with
	 * the best configuration and backing off gradually.
	 *
	 * First we increase the acceptable waste in a slab. Then
	 * we reduce the minimum objects required in a slab.
	 */
	min_objects = slub_min_objects;
	if (!min_objects) {
		/*
		 * Some architectures will only update present cpus when
		 * onlining them, so don't trust the number if it's just 1. But
		 * we also don't want to use nr_cpu_ids always, as on some other
		 * architectures, there can be many possible cpus, but never
		 * onlined. Here we compromise between trying to avoid too high
		 * order on systems that appear larger than they are, and too
		 * low order on systems that appear smaller than they are.
		 */
		nr_cpus = num_present_cpus();
		if (nr_cpus <= 1)
			nr_cpus = nr_cpu_ids;
/*
 * IAMROOT, 2022.06.18:
 * - min_objects = 4 * 2log(nr_cpus)
 * - ex)
 *   nr_cpus == 4 일때 4 * (3 + 1) = 16
 */
		min_objects = 4 * (fls(nr_cpus) + 1);
	}
	max_objects = order_objects(slub_max_order, size);
	min_objects = min(min_objects, max_objects);

/*
 * IAMROOT, 2022.06.18:
 * - 1. slab order 범위 내 적절한 order 산출
 *   2. 1개 object가 들어갈 수 있는 slab order 범위 내 산출
 *   3. 1개 object가 들어갈수 있는 order 산출 (order 무제한)
 */
	while (min_objects > 1) {
		unsigned int fraction;

		fraction = 16;
/*
 * IAMROOT, 2022.06.18:
 * - 1/16(6.25%) -> 1/8(12.5%) -> 1/4(25%) 로 fraction을 확인한다.
 *   최대 25%낭비까지만 고려한다.
 */
		while (fraction >= 4) {
			order = slab_order(size, min_objects,
					slub_max_order, fraction);
			if (order <= slub_max_order)
				return order;
			fraction /= 2;
		}
		min_objects--;
	}

	/*
	 * We were unable to place multiple objects in a slab. Now
	 * lets see if we can place a single object there.
	 */

/*
 * IAMROOT, 2022.06.18:
 * - 2.
 *   min_objects를 무시하고 1개 이상. slub_max_orde는 제한사항 그대로 사용.
 *   fraction 무시(낭비 무시).
 *
 * - 낭비가 1 /4 보다 큰경우 진입하게 된다. 낭비가 얼마되든 상관이 없이 order만을
 *   고려한다.
 *
 * - ex) slub_max_order = 3, size = 20k인경우 50%가까이 낭비되는 상태로 들어올것.
 */
	order = slab_order(size, 1, slub_max_order, 1);
	if (order <= slub_max_order)
		return order;

	/*
	 * Doh this slab cannot be placed using slub_max_order.
	 */
/*
 * IAMROOT, 2022.06.18:
 * - 3.
 *   MAX_ORDER로 계산. fraction 무시.
 *
 * - ex) slub_max_order = 3, size = 40k일때.
 *   max order만으로는 32k까지만 되므로, max order를 4로 키워야된다.
 */
	order = slab_order(size, 1, MAX_ORDER, 1);
	if (order < MAX_ORDER)
		return order;
/*
 * IAMROOT, 2022.06.18:
 * - MAX_ORDER로도 order를 못구함. system config가 안맞는다.
 *
 * - ex) size가 4k * 2^11 보다 큰 경우. MAX_ORDER로도 할당이 안되는 경우.
 */
	return -ENOSYS;
}

static void
init_kmem_cache_node(struct kmem_cache_node *n)
{
	n->nr_partial = 0;
	spin_lock_init(&n->list_lock);
	INIT_LIST_HEAD(&n->partial);
#ifdef CONFIG_SLUB_DEBUG
	atomic_long_set(&n->nr_slabs, 0);
	atomic_long_set(&n->total_objects, 0);
	INIT_LIST_HEAD(&n->full);
#endif
}

/*
 * IAMROOT, 2022.06.25:
 * - @s에 cpu_slab을 할당하고 초기화한다.
 */
static inline int alloc_kmem_cache_cpus(struct kmem_cache *s)
{
	BUILD_BUG_ON(PERCPU_DYNAMIC_EARLY_SIZE <
			KMALLOC_SHIFT_HIGH * sizeof(struct kmem_cache_cpu));

	/*
	 * Must align to double word boundary for the double cmpxchg
	 * instructions to work; see __pcpu_double_call_return_bool().
	 */
/*
 * IAMROOT, 2022.06.25:
 * - 이중 cmpxchg 명령이 작동하려면 이중 단어 경계에 맞춰야 합니다.
 *   __pcpu_double_call_return_bool()을 참조하십시오.
 * - freelist, tid membert를 동시에 제어하는데 두개의 크기가 2word 이므로
 *   아래와 같이 align을 맞춰준다.
 */
	s->cpu_slab = __alloc_percpu(sizeof(struct kmem_cache_cpu),
				     2 * sizeof(void *));

	if (!s->cpu_slab)
		return 0;

	init_kmem_cache_cpus(s);

	return 1;
}

static struct kmem_cache *kmem_cache_node;

/*
 * No kmalloc_node yet so do it by hand. We know that this is the first
 * slab on the node for this slabcache. There are no concurrent accesses
 * possible.
 *
 * Note that this function only works on the kmem_cache_node
 * when allocating for the kmem_cache_node. This is used for bootstrapping
 * memory on a fresh node that has no slab structures yet.
 */
/*
 * IAMROOT, 2022.06.25:
 * - slab page를 kmem_cache_node의 order로 할당받아온후,
 *   할당 받아온 page의 0번째(n) object를 kmem_cache_node->n[node]로 사용하고,
 *   parital list에 page를 넣는다. 1번째 object부터 freelist가 된다.
 */
static void early_kmem_cache_node_alloc(int node)
{
	struct page *page;
	struct kmem_cache_node *n;

	BUG_ON(kmem_cache_node->size < sizeof(struct kmem_cache_node));

/*
 * IAMROOT, 2022.06.25:
 * - booting단계에서 slab_state == DOWN일때 이 함수를 진입하게되고,
 *   kmem_cache_node는 kmem_cache_init()의 boot_kmem_cache_node을 가리키고
 *   있을것이다.
 */
	page = new_slab(kmem_cache_node, GFP_NOWAIT, node);

	BUG_ON(!page);
	if (page_to_nid(page) != node) {
		pr_err("SLUB: Unable to allocate memory from node %d\n", node);
		pr_err("SLUB: Allocating a useless per node structure in order to be able to continue\n");
	}

	n = page->freelist;
	BUG_ON(!n);
#ifdef CONFIG_SLUB_DEBUG
	init_object(kmem_cache_node, n, SLUB_RED_ACTIVE);
	init_tracking(kmem_cache_node, n);
#endif
	n = kasan_slab_alloc(kmem_cache_node, n, GFP_KERNEL, false);

/*
 * IAMROOT, 2022.06.25:
 * - 다음 object를 page->freelist로 넣는다.
 */
	page->freelist = get_freepointer(kmem_cache_node, n);
	page->inuse = 1;
	page->frozen = 0;
	kmem_cache_node->node[node] = n;
	init_kmem_cache_node(n);
	inc_slabs_node(kmem_cache_node, node, page->objects);

	/*
	 * No locks need to be taken here as it has just been
	 * initialized and there is no concurrent access.
	 */
	__add_partial(n, page, DEACTIVATE_TO_HEAD);
}

static void free_kmem_cache_nodes(struct kmem_cache *s)
{
	int node;
	struct kmem_cache_node *n;

	for_each_kmem_cache_node(s, node, n) {
		s->node[node] = NULL;
		kmem_cache_free(kmem_cache_node, n);
	}
}

void __kmem_cache_release(struct kmem_cache *s)
{
	cache_random_seq_destroy(s);
	free_percpu(s->cpu_slab);
	free_kmem_cache_nodes(s);
}

static int init_kmem_cache_nodes(struct kmem_cache *s)
{
	int node;

	for_each_node_mask(node, slab_nodes) {
		struct kmem_cache_node *n;

/*
 * IAMROOT, 2022.06.25:
 * - kmem_cache_node를 못만든 상태(slab_state == DOWN) 이면 
 *   early_kmem_cache_node_alloc()를 수행하고, 아니면 kmem_cache_alloc_node를
 *   호출한다.
 * - kmem_cache_node->node[0..node] 까지 생성되고 각 parital list에 page가 등록된다.
 */
		if (slab_state == DOWN) {
			early_kmem_cache_node_alloc(node);
			continue;
		}
		n = kmem_cache_alloc_node(kmem_cache_node,
						GFP_KERNEL, node);

		if (!n) {
			free_kmem_cache_nodes(s);
			return 0;
		}

		init_kmem_cache_node(n);
		s->node[node] = n;
	}
	return 1;
}

static void set_min_partial(struct kmem_cache *s, unsigned long min)
{
	if (min < MIN_PARTIAL)
		min = MIN_PARTIAL;
	else if (min > MAX_PARTIAL)
		min = MAX_PARTIAL;
	s->min_partial = min;
}

/*
 * IAMROOT, 2022.06.25:
 * - cpu parital 기능을 사용한다면 2 ~ 30개의 slab page를 가지고 있게 된다.
 */
static void set_cpu_partial(struct kmem_cache *s)
{
#ifdef CONFIG_SLUB_CPU_PARTIAL
	/*
	 * cpu_partial determined the maximum number of objects kept in the
	 * per cpu partial lists of a processor.
	 *
	 * Per cpu partial lists mainly contain slabs that just have one
	 * object freed. If they are used for allocation then they can be
	 * filled up again with minimal effort. The slab will never hit the
	 * per node partial lists and therefore no locking will be required.
	 *
	 * This setting also determines
	 *
	 * A) The number of objects from per cpu partial slabs dumped to the
	 *    per node list when we reach the limit.
	 * B) The number of objects in cpu partial slabs to extract from the
	 *    per node list when we run out of per cpu objects. We only fetch
	 *    50% to keep some capacity around for frees.
	 */
/*
 * IAMROOT, 2022.06.25:
 * - papago
 *   cpu_partial은 프로세서의 CPU당 부분 목록에 보관되는 최대 개체 수를 결정했습니다.
 *
 *   CPU당 부분 목록에는 주로 하나의 객체만 해제된 슬랩이 포함됩니다.
 *   할당에 사용되는 경우 최소한의 노력으로 다시 채울 수 있습니다. 슬랩은 노드당
 *   부분 목록에 절대 도달하지 않으므로 잠금이 필요하지 않습니다.
 *
 *   이 설정은 또한
 *   A) 제한에 도달할 때 노드당 목록에 덤프되는 CPU당 부분 슬랩의 개체 수를
 *   결정합니다.
 *   B) CPU당 개체가 부족할 때 노드당 목록에서 추출할 CPU 부분 슬랩의 개체 수입니다.
 *   일부 용량을 무료로 유지하기 위해 50%만 가져옵니다.*
 */
	if (!kmem_cache_has_cpu_partial(s))
		slub_set_cpu_partial(s, 0);
	else if (s->size >= PAGE_SIZE)
		slub_set_cpu_partial(s, 2);
	else if (s->size >= 1024)
		slub_set_cpu_partial(s, 6);
	else if (s->size >= 256)
		slub_set_cpu_partial(s, 13);
	else
		slub_set_cpu_partial(s, 30);
#endif
}

/*
 * calculate_sizes() determines the order and the distribution of data within
 * a slab object.
 */
/*
 * IAMROOT, 2022.06.18:
 * - size (debug flag, align등을 고려), flags, allocflags, red_left_pad,
 *   offset, oo(min_order, max_order, @forced_order 고려)등을 결정한다.
 */
static int calculate_sizes(struct kmem_cache *s, int forced_order)
{
	slab_flags_t flags = s->flags;
	unsigned int size = s->object_size;
	unsigned int order;

	/*
	 * Round up object size to the next word boundary. We can only
	 * place the free pointer at word boundaries and this determines
	 * the possible location of the free pointer.
	 */
/*
 * IAMROOT, 2022.06.11: 
 * 슬랩 object들은 각 object에 FP(Free Pointer)가 사용되므로 모든 객체 간의
 * 주소 지정은 포인터 사이즈 단위로 정렬하여 사용한다.
 */
	size = ALIGN(size, sizeof(void *));

#ifdef CONFIG_SLUB_DEBUG
	/*
	 * Determine if we can poison the object itself. If the user of
	 * the slab may touch the object after free or before allocation
	 * then we should never poison the object itself.
	 */
/*
 * IAMROOT, 2022.06.11: 
 * 디버그 옵션을 위해 poison 옵션을 사용했더라도, 
 * rcu free 옵션 또는 슬랩 캐시 생성자를 사용한 경우 poison 옵션을 제거한다.
 */
	if ((flags & SLAB_POISON) && !(flags & SLAB_TYPESAFE_BY_RCU) &&
			!s->ctor)
		s->flags |= __OBJECT_POISON;
	else
		s->flags &= ~__OBJECT_POISON;


/*
 * IAMROOT, 2022.06.11: 
 * redzone이 사용되었고, object_size가 정렬 문제로 인해 size와 다른 경우 
 * redzone을 위해 8바이트를 추가한다.
 *
 * 예) 정렬되지 않고 남은 공간을 사용하여 redzone 데이터를 기록할 수 있는데,
 *     정렬되어 오브젝트 내에 빈 공간이 없음녀 별도의 redzone을 추가해야 한다.
 *     +------------------+
 *     |  obj_size=16 |XXX|
 *     +__________________+
 *     <-----size=24------>

 *     +------------------+--------+
 *     |  obj_size=24     |XXXXXXXX|
 *     +__________________+--------+
 *     <-----size=24------>
 */
	/*
	 * If we are Redzoning then check if there is some space between the
	 * end of the object and the free pointer. If not then add an
	 * additional word to have some bytes to store Redzone information.
	 */
	if ((flags & SLAB_RED_ZONE) && size == s->object_size)
		size += sizeof(void *);
#endif

	/*
	 * With that we have determined the number of bytes in actual use
	 * by the object and redzoning.
	 */
	s->inuse = size;

	if ((flags & (SLAB_TYPESAFE_BY_RCU | SLAB_POISON)) ||
	    ((flags & SLAB_RED_ZONE) && s->object_size < sizeof(void *)) ||
	    s->ctor) {
		/*
		 * Relocate free pointer after the object if it is not
		 * permitted to overwrite the first word of the object on
		 * kmem_cache_free.
		 *
		 * This is the case if we do RCU, have a constructor or
		 * destructor, are poisoning the objects, or are
		 * redzoning an object smaller than sizeof(void *).
		 *
		 * The assumption that s->offset >= s->inuse means free
		 * pointer is outside of the object is used in the
		 * freeptr_outside_object() function. If that is no
		 * longer true, the function needs to be modified.
		 */
/*
 * IAMROOT, 2022.06.17:
 * - papago
 *   kmem_cache_free에 있는 개체의 첫 번째 단어를 덮어쓰는 것이 허용되지 않는
 *   경우 개체 뒤에 free pointer를 재배치합니다.
 *   
 *   RCU를 수행하거나 생성자 또는 소멸자가 있거나 개체를 감염시키거나
 *   sizeof(void *)보다 작은 개체를 redzoning하는 경우입니다.
 *   
 *   s->offset >= s->inuse라는 가정은 freeptr_outside_object() 함수에서 사용되는
 *   free pointer가 객체 외부에 있다는 것을 의미합니다. 그것이 더 이상 사실이
 *   아니라면 함수를 수정해야 합니다.
 *
 * - if문의 조건에 해당하면 시작주소(offset)을 조정하고 뒤에 long byte를 추가한다.
 *
 * - object_size = 20이라고 가정. size는 align되어 24라고 했을대.
 *     +----------------+-+--+       
 *     |  obj_size=20   | |fp|       
 *     +________________+_+--+       
 *                        ^offset = 24
 *     <---new_size=32------->
 */
		s->offset = size;
		size += sizeof(void *);
	} else {
		/*
		 * Store freelist pointer near middle of object to keep
		 * it away from the edges of the object to avoid small
		 * sized over/underflows from neighboring allocations.
		 */
/*
 * IAMROOT, 2022.06.17:
 * - papgo
 *   인접한 할당에서 작은 크기의 오버/언더플로를 방지하기 위해 개체의
 *   가장자리에서 멀리 유지하기 위해 개체의 중간 근처에 freelist 포인터를
 *   저장합니다.
 *
 * - over/under flow 방지를 위해 fp를 중앙으로 이동한다.(offset 추가.)
 *
 * - object_size = 20이라고 가정. size는 align되어 24라고 했을대.
 *               obj_size=20   
 *           +-------+--+-----+-+          
 *           |       |fp|     | |          
 *           +_______+__+_____+_+          
 *                   ^offset = 8
 *           <--- size=24------->
 */
		s->offset = ALIGN_DOWN(s->object_size / 2, sizeof(void *));
	}

#ifdef CONFIG_SLUB_DEBUG
	if (flags & SLAB_STORE_USER)
		/*
		 * Need to store information about allocs and frees after
		 * the object.
		 */
/*
 * IAMROOT, 2022.06.18:
 * - 할당, 해제용으로 총 2개.
 */
		size += 2 * sizeof(struct track);
#endif

	kasan_cache_create(s, &size, &s->flags);
#ifdef CONFIG_SLUB_DEBUG
	if (flags & SLAB_RED_ZONE) {
		/*
		 * Add some empty padding so that we can catch
		 * overwrites from earlier objects rather than let
		 * tracking information or the free pointer be
		 * corrupted if a user writes before the start
		 * of the object.
		 */
/*
 * IAMROOT, 2022.06.18:
 * - long + red_left_pad
 *  
 * - s->align을 64라고 가정했을때.
 *   <- red zone(s->align) -> <-----old size ---------->
 *   +------------------------+------------------------+---+
 *   | (8) | (54)             | object                 |(8)|
 *   +________________________+________________________+___+   
 *     Z      PAD             ^red_left_pads            Z
 *   <--------- new size = red_left_pad + old_size + 8  --->
 *
 *
 */
		size += sizeof(void *);

		s->red_left_pad = sizeof(void *);
		s->red_left_pad = ALIGN(s->red_left_pad, s->align);
		size += s->red_left_pad;
	}
#endif

	/*
	 * SLUB stores one object immediately after another beginning from
	 * offset 0. In order to align the objects we have to simply size
	 * each object to conform to the alignment.
	 */
/*
 * IAMROOT, 2022.06.18:
 * - 최종적으로 s->align으로 size를 다시 align해준다.
 */
	size = ALIGN(size, s->align);
	s->size = size;
/*
 * IAMROOT, 2022.06.18:
 * - Git blame 참고
 *   mm: slub: implement SLUB version of obj_to_index()
 */
	s->reciprocal_size = reciprocal_value(size);
	if (forced_order >= 0)
		order = forced_order;
	else
		order = calculate_order(size);

	if ((int)order < 0)
		return 0;

	s->allocflags = 0;
/*
 * IAMROOT, 2022.06.18:
 * - order가 1이상이면 compound_order 처리.
 */
	if (order)
		s->allocflags |= __GFP_COMP;

/*
 * IAMROOT, 2022.06.18:
 * - dma 영역내 할당.
 */
	if (s->flags & SLAB_CACHE_DMA)
		s->allocflags |= GFP_DMA;

/*
 * IAMROOT, 2022.06.18:
 * - dma32 영역내 할당.
 */
	if (s->flags & SLAB_CACHE_DMA32)
		s->allocflags |= GFP_DMA32;

/*
 * IAMROOT, 2022.06.18:
 * - reclaim 가능한 slab을 만들도록 한다.
 */
	if (s->flags & SLAB_RECLAIM_ACCOUNT)
		s->allocflags |= __GFP_RECLAIMABLE;

	/*
	 * Determine the number of objects per slab
	 */
	s->oo = oo_make(order, size);
/*
 * IAMROOT, 2022.06.18:
 * - size로 재계산을하여 min oo를 구해놓는다.
 * - max oo도 s->oo와 비교하여 갱신한다.
 *   alias cache(size와 flag가 비슷해서 원래 있던 cache를 다른 곳에서 사용하는경우)
 *   를 open할때 상황에따라 order가 다를 수 잇으므로 max를 갱신해주는것이다.
 */
	s->min = oo_make(get_order(size), size);
	if (oo_objects(s->oo) > oo_objects(s->max))
		s->max = s->oo;

	return !!oo_objects(s->oo);
}

static int kmem_cache_open(struct kmem_cache *s, slab_flags_t flags)
{
/*
 * IAMROOT, 2022.06.11: 
 * "slub_debug" 커널 옵션에서 지정된 캐시에 대해 디버그 플래그를 추가해 온다.
 */
	s->flags = kmem_cache_flags(s->size, flags, s->name);
/*
 * IAMROOT, 2022.06.11: 
 * 보안 향상을 위해 CONFIG_SLAB_FREELIST_HARDENED 커널 옵션을 사용하여 
 * free 포인터 값들을 encaptualation하여 숨길 수 있다.
 */
#ifdef CONFIG_SLAB_FREELIST_HARDENED
	s->random = get_random_long();
#endif

	if (!calculate_sizes(s, -1))
		goto error;
	if (disable_higher_order_debug) {
		/*
		 * Disable debugging flags that store metadata if the min slab
		 * order increased.
		 */
		if (get_order(s->size) > get_order(s->object_size)) {
			s->flags &= ~DEBUG_METADATA_FLAGS;
			s->offset = 0;
			if (!calculate_sizes(s, -1))
				goto error;
		}
	}

#if defined(CONFIG_HAVE_CMPXCHG_DOUBLE) && \
    defined(CONFIG_HAVE_ALIGNED_STRUCT_PAGE)
	if (system_has_cmpxchg_double() && (s->flags & SLAB_NO_CMPXCHG) == 0)
		/* Enable fast mode */
		s->flags |= __CMPXCHG_DOUBLE;
#endif

	/*
	 * The larger the object size is, the more pages we want on the partial
	 * list to avoid pounding the page allocator excessively.
	 */
	set_min_partial(s, ilog2(s->size) / 2);

	set_cpu_partial(s);

#ifdef CONFIG_NUMA
	s->remote_node_defrag_ratio = 1000;
#endif

	/* Initialize the pre-computed randomized freelist if slab is up */
	if (slab_state >= UP) {
		if (init_cache_random_seq(s))
			goto error;
	}

	if (!init_kmem_cache_nodes(s))
		goto error;

	if (alloc_kmem_cache_cpus(s))
		return 0;

error:
	__kmem_cache_release(s);
	return -EINVAL;
}

static void list_slab_objects(struct kmem_cache *s, struct page *page,
			      const char *text)
{
#ifdef CONFIG_SLUB_DEBUG
	void *addr = page_address(page);
	unsigned long flags;
	unsigned long *map;
	void *p;

	slab_err(s, page, text, s->name);
	slab_lock(page, &flags);

	map = get_map(s, page);
	for_each_object(p, s, addr, page->objects) {

		if (!test_bit(__obj_to_index(s, addr, p), map)) {
			pr_err("Object 0x%p @offset=%tu\n", p, p - addr);
			print_tracking(s, p);
		}
	}
	put_map(map);
	slab_unlock(page, &flags);
#endif
}

/*
 * Attempt to free all partial slabs on a node.
 * This is called from __kmem_cache_shutdown(). We must take list_lock
 * because sysfs file might still access partial list after the shutdowning.
 */
static void free_partial(struct kmem_cache *s, struct kmem_cache_node *n)
{
	LIST_HEAD(discard);
	struct page *page, *h;

	BUG_ON(irqs_disabled());
	spin_lock_irq(&n->list_lock);
	list_for_each_entry_safe(page, h, &n->partial, slab_list) {
		if (!page->inuse) {
			remove_partial(n, page);
			list_add(&page->slab_list, &discard);
		} else {
			list_slab_objects(s, page,
			  "Objects remaining in %s on __kmem_cache_shutdown()");
		}
	}
	spin_unlock_irq(&n->list_lock);

	list_for_each_entry_safe(page, h, &discard, slab_list)
		discard_slab(s, page);
}

bool __kmem_cache_empty(struct kmem_cache *s)
{
	int node;
	struct kmem_cache_node *n;

	for_each_kmem_cache_node(s, node, n)
		if (n->nr_partial || slabs_node(s, node))
			return false;
	return true;
}

/*
 * Release all resources used by a slab cache.
 */
int __kmem_cache_shutdown(struct kmem_cache *s)
{
	int node;
	struct kmem_cache_node *n;

	flush_all_cpus_locked(s);
	/* Attempt to free all objects */
	for_each_kmem_cache_node(s, node, n) {
		free_partial(s, n);
		if (n->nr_partial || slabs_node(s, node))
			return 1;
	}
	return 0;
}

#ifdef CONFIG_PRINTK
void kmem_obj_info(struct kmem_obj_info *kpp, void *object, struct page *page)
{
	void *base;
	int __maybe_unused i;
	unsigned int objnr;
	void *objp;
	void *objp0;
	struct kmem_cache *s = page->slab_cache;
	struct track __maybe_unused *trackp;

	kpp->kp_ptr = object;
	kpp->kp_page = page;
	kpp->kp_slab_cache = s;
	base = page_address(page);
	objp0 = kasan_reset_tag(object);
#ifdef CONFIG_SLUB_DEBUG
	objp = restore_red_left(s, objp0);
#else
	objp = objp0;
#endif
	objnr = obj_to_index(s, page, objp);
	kpp->kp_data_offset = (unsigned long)((char *)objp0 - (char *)objp);
	objp = base + s->size * objnr;
	kpp->kp_objp = objp;
	if (WARN_ON_ONCE(objp < base || objp >= base + page->objects * s->size || (objp - base) % s->size) ||
	    !(s->flags & SLAB_STORE_USER))
		return;
#ifdef CONFIG_SLUB_DEBUG
	objp = fixup_red_left(s, objp);
	trackp = get_track(s, objp, TRACK_ALLOC);
	kpp->kp_ret = (void *)trackp->addr;
#ifdef CONFIG_STACKTRACE
	for (i = 0; i < KS_ADDRS_COUNT && i < TRACK_ADDRS_COUNT; i++) {
		kpp->kp_stack[i] = (void *)trackp->addrs[i];
		if (!kpp->kp_stack[i])
			break;
	}

	trackp = get_track(s, objp, TRACK_FREE);
	for (i = 0; i < KS_ADDRS_COUNT && i < TRACK_ADDRS_COUNT; i++) {
		kpp->kp_free_stack[i] = (void *)trackp->addrs[i];
		if (!kpp->kp_free_stack[i])
			break;
	}
#endif
#endif
}
#endif

/********************************************************************
 *		Kmalloc subsystem
 *******************************************************************/

static int __init setup_slub_min_order(char *str)
{
	get_option(&str, (int *)&slub_min_order);

	return 1;
}

__setup("slub_min_order=", setup_slub_min_order);

static int __init setup_slub_max_order(char *str)
{
	get_option(&str, (int *)&slub_max_order);
	slub_max_order = min(slub_max_order, (unsigned int)MAX_ORDER - 1);

	return 1;
}

__setup("slub_max_order=", setup_slub_max_order);

static int __init setup_slub_min_objects(char *str)
{
	get_option(&str, (int *)&slub_min_objects);

	return 1;
}

__setup("slub_min_objects=", setup_slub_min_objects);

/*
 * IAMROOT, 2022.06.25:
 * - @flags에 따라서 type을 정하고 type과 @size에 해당하는 kmem_cache에서
 *   할당 받아온다.
 */
void *__kmalloc(size_t size, gfp_t flags)
{
	struct kmem_cache *s;
	void *ret;

	if (unlikely(size > KMALLOC_MAX_CACHE_SIZE))
		return kmalloc_large(size, flags);

	s = kmalloc_slab(size, flags);

	if (unlikely(ZERO_OR_NULL_PTR(s)))
		return s;

	ret = slab_alloc(s, flags, _RET_IP_, size);

	trace_kmalloc(_RET_IP_, ret, size, s->size, flags);

	ret = kasan_kmalloc(s, ret, size, flags);

	return ret;
}
EXPORT_SYMBOL(__kmalloc);

#ifdef CONFIG_NUMA
static void *kmalloc_large_node(size_t size, gfp_t flags, int node)
{
	struct page *page;
	void *ptr = NULL;
	unsigned int order = get_order(size);

	flags |= __GFP_COMP;
	page = alloc_pages_node(node, flags, order);
	if (page) {
		ptr = page_address(page);
		mod_lruvec_page_state(page, NR_SLAB_UNRECLAIMABLE_B,
				      PAGE_SIZE << order);
	}

	return kmalloc_large_node_hook(ptr, size, flags);
}

void *__kmalloc_node(size_t size, gfp_t flags, int node)
{
	struct kmem_cache *s;
	void *ret;

	if (unlikely(size > KMALLOC_MAX_CACHE_SIZE)) {
		ret = kmalloc_large_node(size, flags, node);

		trace_kmalloc_node(_RET_IP_, ret,
				   size, PAGE_SIZE << get_order(size),
				   flags, node);

		return ret;
	}

	s = kmalloc_slab(size, flags);

	if (unlikely(ZERO_OR_NULL_PTR(s)))
		return s;

	ret = slab_alloc_node(s, flags, node, _RET_IP_, size);

	trace_kmalloc_node(_RET_IP_, ret, size, s->size, flags, node);

	ret = kasan_kmalloc(s, ret, size, flags);

	return ret;
}
EXPORT_SYMBOL(__kmalloc_node);
#endif	/* CONFIG_NUMA */

#ifdef CONFIG_HARDENED_USERCOPY
/*
 * Rejects incorrectly sized objects and objects that are to be copied
 * to/from userspace but do not fall entirely within the containing slab
 * cache's usercopy region.
 *
 * Returns NULL if check passes, otherwise const char * to name of cache
 * to indicate an error.
 */
void __check_heap_object(const void *ptr, unsigned long n, struct page *page,
			 bool to_user)
{
	struct kmem_cache *s;
	unsigned int offset;
	size_t object_size;
	bool is_kfence = is_kfence_address(ptr);

	ptr = kasan_reset_tag(ptr);

	/* Find object and usable object size. */
	s = page->slab_cache;

	/* Reject impossible pointers. */
	if (ptr < page_address(page))
		usercopy_abort("SLUB object not in SLUB page?!", NULL,
			       to_user, 0, n);

	/* Find offset within object. */
	if (is_kfence)
		offset = ptr - kfence_object_start(ptr);
	else
		offset = (ptr - page_address(page)) % s->size;

	/* Adjust for redzone and reject if within the redzone. */
	if (!is_kfence && kmem_cache_debug_flags(s, SLAB_RED_ZONE)) {
		if (offset < s->red_left_pad)
			usercopy_abort("SLUB object in left red zone",
				       s->name, to_user, offset, n);
		offset -= s->red_left_pad;
	}

	/* Allow address range falling entirely within usercopy region. */
	if (offset >= s->useroffset &&
	    offset - s->useroffset <= s->usersize &&
	    n <= s->useroffset - offset + s->usersize)
		return;

	/*
	 * If the copy is still within the allocated object, produce
	 * a warning instead of rejecting the copy. This is intended
	 * to be a temporary method to find any missing usercopy
	 * whitelists.
	 */
	object_size = slab_ksize(s);
	if (usercopy_fallback &&
	    offset <= object_size && n <= object_size - offset) {
		usercopy_warn("SLUB object", s->name, to_user, offset, n);
		return;
	}

	usercopy_abort("SLUB object", s->name, to_user, offset, n);
}
#endif /* CONFIG_HARDENED_USERCOPY */

size_t __ksize(const void *object)
{
	struct page *page;

	if (unlikely(object == ZERO_SIZE_PTR))
		return 0;

	page = virt_to_head_page(object);

	if (unlikely(!PageSlab(page))) {
		WARN_ON(!PageCompound(page));
		return page_size(page);
	}

	return slab_ksize(page->slab_cache);
}
EXPORT_SYMBOL(__ksize);

/*
 * IAMROOT, 2022.07.02: 
 * object의 가상 주소 @x를 인자로 object의 반환(할당 해제)을 요청한다.
 * -> 가상 주소 @x는 사용자 영역에 있는 가상 주소가 아니라 
 *                   커널 lm 매핑을 한 커널 가상 주소 영역 중에 하나인 주소이므로
 *                   이러한 lm 가상 주소는 virt_to_phys, virt_to_page와 같은 
 *                   API의 사용이 가능하다.
 */
void kfree(const void *x)
{
	struct page *page;
	void *object = (void *)x;

	trace_kfree(_RET_IP_, x);

	if (unlikely(ZERO_OR_NULL_PTR(x)))
		return;

	page = virt_to_head_page(x);
/*
 * IAMROOT, 2022.06.25:
 * - buddy에서 직접가져온경우등
 *   ex) kmalloc_large()등
 */
	if (unlikely(!PageSlab(page))) {
		free_nonslab_page(page, object);
		return;
	}
	slab_free(page->slab_cache, page, object, NULL, 1, _RET_IP_)
}
EXPORT_SYMBOL(kfree);

#define SHRINK_PROMOTE_MAX 32

/*
 * kmem_cache_shrink discards empty slabs and promotes the slabs filled
 * up most to the head of the partial lists. New allocations will then
 * fill those up and thus they can be removed from the partial lists.
 *
 * The slabs with the least items are placed last. This results in them
 * being allocated from last increasing the chance that the last objects
 * are freed in them.
 */
static int __kmem_cache_do_shrink(struct kmem_cache *s)
{
	int node;
	int i;
	struct kmem_cache_node *n;
	struct page *page;
	struct page *t;
	struct list_head discard;
	struct list_head promote[SHRINK_PROMOTE_MAX];
	unsigned long flags;
	int ret = 0;

	for_each_kmem_cache_node(s, node, n) {
		INIT_LIST_HEAD(&discard);
		for (i = 0; i < SHRINK_PROMOTE_MAX; i++)
			INIT_LIST_HEAD(promote + i);

		spin_lock_irqsave(&n->list_lock, flags);

		/*
		 * Build lists of slabs to discard or promote.
		 *
		 * Note that concurrent frees may occur while we hold the
		 * list_lock. page->inuse here is the upper limit.
		 */
		list_for_each_entry_safe(page, t, &n->partial, slab_list) {
			int free = page->objects - page->inuse;

			/* Do not reread page->inuse */
			barrier();

			/* We do not keep full slabs on the list */
			BUG_ON(free <= 0);

			if (free == page->objects) {
				list_move(&page->slab_list, &discard);
				n->nr_partial--;
			} else if (free <= SHRINK_PROMOTE_MAX)
				list_move(&page->slab_list, promote + free - 1);
		}

		/*
		 * Promote the slabs filled up most to the head of the
		 * partial list.
		 */
		for (i = SHRINK_PROMOTE_MAX - 1; i >= 0; i--)
			list_splice(promote + i, &n->partial);

		spin_unlock_irqrestore(&n->list_lock, flags);

		/* Release empty slabs */
		list_for_each_entry_safe(page, t, &discard, slab_list)
			discard_slab(s, page);

		if (slabs_node(s, node))
			ret = 1;
	}

	return ret;
}

int __kmem_cache_shrink(struct kmem_cache *s)
{
	flush_all(s);
	return __kmem_cache_do_shrink(s);
}

static int slab_mem_going_offline_callback(void *arg)
{
	struct kmem_cache *s;

	mutex_lock(&slab_mutex);
	list_for_each_entry(s, &slab_caches, list) {
		flush_all_cpus_locked(s);
		__kmem_cache_do_shrink(s);
	}
	mutex_unlock(&slab_mutex);

	return 0;
}

static void slab_mem_offline_callback(void *arg)
{
	struct memory_notify *marg = arg;
	int offline_node;

	offline_node = marg->status_change_nid_normal;

	/*
	 * If the node still has available memory. we need kmem_cache_node
	 * for it yet.
	 */
	if (offline_node < 0)
		return;

	mutex_lock(&slab_mutex);
	node_clear(offline_node, slab_nodes);
	/*
	 * We no longer free kmem_cache_node structures here, as it would be
	 * racy with all get_node() users, and infeasible to protect them with
	 * slab_mutex.
	 */
	mutex_unlock(&slab_mutex);
}

static int slab_mem_going_online_callback(void *arg)
{
	struct kmem_cache_node *n;
	struct kmem_cache *s;
	struct memory_notify *marg = arg;
	int nid = marg->status_change_nid_normal;
	int ret = 0;

	/*
	 * If the node's memory is already available, then kmem_cache_node is
	 * already created. Nothing to do.
	 */
	if (nid < 0)
		return 0;

	/*
	 * We are bringing a node online. No memory is available yet. We must
	 * allocate a kmem_cache_node structure in order to bring the node
	 * online.
	 */
	mutex_lock(&slab_mutex);
	list_for_each_entry(s, &slab_caches, list) {
		/*
		 * The structure may already exist if the node was previously
		 * onlined and offlined.
		 */
		if (get_node(s, nid))
			continue;
		/*
		 * XXX: kmem_cache_alloc_node will fallback to other nodes
		 *      since memory is not yet available from the node that
		 *      is brought up.
		 */
		n = kmem_cache_alloc(kmem_cache_node, GFP_KERNEL);
		if (!n) {
			ret = -ENOMEM;
			goto out;
		}
		init_kmem_cache_node(n);
		s->node[nid] = n;
	}
	/*
	 * Any cache created after this point will also have kmem_cache_node
	 * initialized for the new node.
	 */
	node_set(nid, slab_nodes);
out:
	mutex_unlock(&slab_mutex);
	return ret;
}

/*
 * IAMROOT, 2022.06.18:
 * - memory node가 추가되거나 삭제 되거나 할때 callback으로 호출된다.
 */
static int slab_memory_callback(struct notifier_block *self,
				unsigned long action, void *arg)
{
	int ret = 0;

	switch (action) {
	case MEM_GOING_ONLINE:
		ret = slab_mem_going_online_callback(arg);
		break;
	case MEM_GOING_OFFLINE:
		ret = slab_mem_going_offline_callback(arg);
		break;
	case MEM_OFFLINE:
	case MEM_CANCEL_ONLINE:
		slab_mem_offline_callback(arg);
		break;
	case MEM_ONLINE:
	case MEM_CANCEL_OFFLINE:
		break;
	}
	if (ret)
		ret = notifier_from_errno(ret);
	else
		ret = NOTIFY_OK;
	return ret;
}

static struct notifier_block slab_memory_callback_nb = {
	.notifier_call = slab_memory_callback,
	.priority = SLAB_CALLBACK_PRI,
};

/********************************************************************
 *			Basic setup of slabs
 *******************************************************************/

/*
 * Used for early kmem_cache structures that were allocated using
 * the page allocator. Allocate them properly then fix up the pointers
 * that may be pointing to the wrong kmem_cache structure.
 */

/*
 * IAMROOT, 2022.06.18:
 * - static영역에 만들어진 cache를 동적영역(s)으로 옮긴다.
 *   slab_caches에 새로 만들어진 s를 추가한다.
 */
static struct kmem_cache * __init bootstrap(struct kmem_cache *static_cache)
{
	int node;
	struct kmem_cache *s = kmem_cache_zalloc(kmem_cache, GFP_NOWAIT);
	struct kmem_cache_node *n;

	memcpy(s, static_cache, kmem_cache->object_size);

	/*
	 * This runs very early, and only the boot processor is supposed to be
	 * up.  Even if it weren't true, IRQs are not up so we couldn't fire
	 * IPIs around.
	 */
/*
 * IAMROOT, 2022.06.25:
 * - papgo
 *   이것은 매우 일찍 실행되며 부트 프로세서만 작동해야 합니다.
 *   사실이 아니더라도 IRQ가 작동하지 않아 IPI를 실행할 수 없습니다.
 * - cpu slab에 있는 모든 page들을 flush 한다.
 */
	__flush_cpu_slab(s, smp_processor_id());

/*
 * IAMROOT, 2022.06.25:
 * - node partial에 있는 모든 page들을 iteration해서 @s로 등록한다.
 */
	for_each_kmem_cache_node(s, node, n) {
		struct page *p;

		list_for_each_entry(p, &n->partial, slab_list)
			p->slab_cache = s;

#ifdef CONFIG_SLUB_DEBUG
		list_for_each_entry(p, &n->full, slab_list)
			p->slab_cache = s;
#endif
	}

/*
 * IAMROOT, 2022.06.25:
 * - slab_caches에 slab cache s를 추가한다.
 */
	list_add(&s->list, &slab_caches);
	return s;
}

/*
 * IAMROOT, 2022.06.25:
 * - slab cache를 사용할 준비를 한다.
 */
void __init kmem_cache_init(void)
{
	static __initdata struct kmem_cache boot_kmem_cache,
		boot_kmem_cache_node;
	int node;

/*
 * IAMROOT, 2022.06.11: 
 * 커널이 디버그로 빌드된 경우에는 슬랩 페이지 할당을 single 페이지에서만 
 * 가능하게 order를 0으로 제한한다.
 */
	if (debug_guardpage_minorder())
		slub_max_order = 0;

	/* Print slub debugging pointers without hashing */
/*
 * IAMROOT, 2022.06.11: 
 * 런타임 slub_debug= 파라미터를 사용한 경우 강제로 no_hash_pointers=true를 
 * 설정한다. 
 */
	if (__slub_debug_enabled())
		no_hash_pointers_enable(NULL);

/*
 * IAMROOT, 2022.06.11: 
 * kmem_cache_node 및 kmem_cache 슬래캐시는 전역에 설정되어 있으며, 
 * 초기화 중 일시적으로 boot_* 구조체를 사용한다.
 */
	kmem_cache_node = &boot_kmem_cache_node;
	kmem_cache = &boot_kmem_cache;

	/*
	 * Initialize the nodemask for which we will allocate per node
	 * structures. Here we don't need taking slab_mutex yet.
	 */
/*
 * IAMROOT, 2022.06.11: 
 * 슬랩캐시에서 관리할 노드를 식별하기 위해 메모리가 있는 NUMA 노드들에 대해
 * 비트마스크를 전역 slab_nodes에 설정한다.
 */
	for_each_node_state(node, N_NORMAL_MEMORY)
		node_set(node, slab_nodes);

/*
 * IAMROOT, 2022.06.11: 
 * kemem_cache_node는 모든 슬랩캐시내에서 많은 액세스가 필요한 구조체이다.
 * 따라서 hwcache에 맞춰 정렬 요청을 한다.
 * kmem_cache_node라는 전역 슬랩 캐시에 &boot_kmem_cache_node가 대입되어 있다.
 */
	create_boot_cache(kmem_cache_node, "kmem_cache_node",
		sizeof(struct kmem_cache_node), SLAB_HWCACHE_ALIGN, 0, 0);

/*
 * IAMROOT, 2022.06.18:
 * - notifier에 slab_memory_callback_nb을 등록한다.
 */
	register_hotmemory_notifier(&slab_memory_callback_nb);

	/* Able to allocate the per node structures */

/*
 * IAMROOT, 2022.06.18:
 * - kmem_cache_node를 할당할수 있는 상태가 되서 state를 변경한다.
 */
	slab_state = PARTIAL;

/*
 * IAMROOT, 2022.06.11: 
 * kmem_cache 구조체를 할당할 때 구조체 내부의 node[]에 사용할 포인터 수를
 * MAX_NUMNODES(default=16)만큼 만들지 않고, 런타임에 확인된 노드 수 만큼 만든다.
 */
	create_boot_cache(kmem_cache, "kmem_cache",
			offsetof(struct kmem_cache, node) +
				nr_node_ids * sizeof(struct kmem_cache_node *),
		       SLAB_HWCACHE_ALIGN, 0, 0);

/*
 * IAMROOT, 2022.06.25:
 * - static cache를 동적 영역으로 복사하고 slab_caches에 넣는다.
 */
	kmem_cache = bootstrap(&boot_kmem_cache);
	kmem_cache_node = bootstrap(&boot_kmem_cache_node);

	/* Now we can use the kmem_cache to allocate kmalloc slabs */
	setup_kmalloc_cache_index_table();

/*
 * IAMROOT, 2022.06.25:
 * - type과 size별 kmalloc_cache를 만들고 slab_state를 UP으로 갱신한다
 */
	create_kmalloc_caches(0);

	/* Setup random freelists for each cache */
	init_freelist_randomization();

	cpuhp_setup_state_nocalls(CPUHP_SLUB_DEAD, "slub:dead", NULL,
				  slub_cpu_dead);

/*
 * IAMROOT, 2022.06.25:
 * - ex)
 *   SLUB: HWalign=64, Order=0-3, MinObjects=0, CPUs=8, Nodes=1 
 * - cache_line_size()는 실제 register값을 잃어 온다.
 *   kmem_cache에서 사용하는 hwalign은 armv8 arch중에서 제일 큰 cache를 기준으로
 *   생각해서 128로 사용하고 있다.
 */
	pr_info("SLUB: HWalign=%d, Order=%u-%u, MinObjects=%u, CPUs=%u, Nodes=%u\n",
		cache_line_size(),
		slub_min_order, slub_max_order, slub_min_objects,
		nr_cpu_ids, nr_node_ids);
}

void __init kmem_cache_init_late(void)
{
}

struct kmem_cache *
__kmem_cache_alias(const char *name, unsigned int size, unsigned int align,
		   slab_flags_t flags, void (*ctor)(void *))
{
	struct kmem_cache *s;

	s = find_mergeable(size, align, flags, name, ctor);
	if (s) {
		s->refcount++;

		/*
		 * Adjust the object sizes so that we clear
		 * the complete object on kzalloc.
		 */
		s->object_size = max(s->object_size, size);
		s->inuse = max(s->inuse, ALIGN(size, sizeof(void *)));

		if (sysfs_slab_alias(s, name)) {
			s->refcount--;
			s = NULL;
		}
	}

	return s;
}

/*
 * IAMROOT, 2022.06.11: 
 * 슬럽(slub) 캐시를 생성한다.
 * - config가 존재할 경우 sys/kernel/slab, debugfs에 경로가 생긴다.
 */
int __kmem_cache_create(struct kmem_cache *s, slab_flags_t flags)
{
	int err;

	err = kmem_cache_open(s, flags);
	if (err)
		return err;

	/* Mutex is not taken during early boot */
/*
 * IAMROOT, 2022.06.18:
 * - 부팅중엔(아직 slab이 안만들어지는 상황) 빠져나간다.
 */
	if (slab_state <= UP)
		return 0;

	err = sysfs_slab_add(s);
	if (err) {
		__kmem_cache_release(s);
		return err;
	}

	if (s->flags & SLAB_STORE_USER)
		debugfs_slab_add(s);

	return 0;
}

void *__kmalloc_track_caller(size_t size, gfp_t gfpflags, unsigned long caller)
{
	struct kmem_cache *s;
	void *ret;

	if (unlikely(size > KMALLOC_MAX_CACHE_SIZE))
		return kmalloc_large(size, gfpflags);

	s = kmalloc_slab(size, gfpflags);

	if (unlikely(ZERO_OR_NULL_PTR(s)))
		return s;

	ret = slab_alloc(s, gfpflags, caller, size);

	/* Honor the call site pointer we received. */
	trace_kmalloc(caller, ret, size, s->size, gfpflags);

	return ret;
}
EXPORT_SYMBOL(__kmalloc_track_caller);

#ifdef CONFIG_NUMA
void *__kmalloc_node_track_caller(size_t size, gfp_t gfpflags,
					int node, unsigned long caller)
{
	struct kmem_cache *s;
	void *ret;

	if (unlikely(size > KMALLOC_MAX_CACHE_SIZE)) {
		ret = kmalloc_large_node(size, gfpflags, node);

		trace_kmalloc_node(caller, ret,
				   size, PAGE_SIZE << get_order(size),
				   gfpflags, node);

		return ret;
	}

	s = kmalloc_slab(size, gfpflags);

	if (unlikely(ZERO_OR_NULL_PTR(s)))
		return s;

	ret = slab_alloc_node(s, gfpflags, node, caller, size);

	/* Honor the call site pointer we received. */
	trace_kmalloc_node(caller, ret, size, s->size, gfpflags, node);

	return ret;
}
EXPORT_SYMBOL(__kmalloc_node_track_caller);
#endif

#ifdef CONFIG_SYSFS
static int count_inuse(struct page *page)
{
	return page->inuse;
}

static int count_total(struct page *page)
{
	return page->objects;
}
#endif

#ifdef CONFIG_SLUB_DEBUG
static void validate_slab(struct kmem_cache *s, struct page *page,
			  unsigned long *obj_map)
{
	void *p;
	void *addr = page_address(page);
	unsigned long flags;

	slab_lock(page, &flags);

	if (!check_slab(s, page) || !on_freelist(s, page, NULL))
		goto unlock;

	/* Now we know that a valid freelist exists */
	__fill_map(obj_map, s, page);
	for_each_object(p, s, addr, page->objects) {
		u8 val = test_bit(__obj_to_index(s, addr, p), obj_map) ?
			 SLUB_RED_INACTIVE : SLUB_RED_ACTIVE;

		if (!check_object(s, page, p, val))
			break;
	}
unlock:
	slab_unlock(page, &flags);
}

static int validate_slab_node(struct kmem_cache *s,
		struct kmem_cache_node *n, unsigned long *obj_map)
{
	unsigned long count = 0;
	struct page *page;
	unsigned long flags;

	spin_lock_irqsave(&n->list_lock, flags);

	list_for_each_entry(page, &n->partial, slab_list) {
		validate_slab(s, page, obj_map);
		count++;
	}
	if (count != n->nr_partial) {
		pr_err("SLUB %s: %ld partial slabs counted but counter=%ld\n",
		       s->name, count, n->nr_partial);
		slab_add_kunit_errors();
	}

	if (!(s->flags & SLAB_STORE_USER))
		goto out;

	list_for_each_entry(page, &n->full, slab_list) {
		validate_slab(s, page, obj_map);
		count++;
	}
	if (count != atomic_long_read(&n->nr_slabs)) {
		pr_err("SLUB: %s %ld slabs counted but counter=%ld\n",
		       s->name, count, atomic_long_read(&n->nr_slabs));
		slab_add_kunit_errors();
	}

out:
	spin_unlock_irqrestore(&n->list_lock, flags);
	return count;
}

long validate_slab_cache(struct kmem_cache *s)
{
	int node;
	unsigned long count = 0;
	struct kmem_cache_node *n;
	unsigned long *obj_map;

	obj_map = bitmap_alloc(oo_objects(s->oo), GFP_KERNEL);
	if (!obj_map)
		return -ENOMEM;

	flush_all(s);
	for_each_kmem_cache_node(s, node, n)
		count += validate_slab_node(s, n, obj_map);

	bitmap_free(obj_map);

	return count;
}
EXPORT_SYMBOL(validate_slab_cache);

#ifdef CONFIG_DEBUG_FS
/*
 * Generate lists of code addresses where slabcache objects are allocated
 * and freed.
 */

struct location {
	unsigned long count;
	unsigned long addr;
	long long sum_time;
	long min_time;
	long max_time;
	long min_pid;
	long max_pid;
	DECLARE_BITMAP(cpus, NR_CPUS);
	nodemask_t nodes;
};

struct loc_track {
	unsigned long max;
	unsigned long count;
	struct location *loc;
};

static struct dentry *slab_debugfs_root;

static void free_loc_track(struct loc_track *t)
{
	if (t->max)
		free_pages((unsigned long)t->loc,
			get_order(sizeof(struct location) * t->max));
}

static int alloc_loc_track(struct loc_track *t, unsigned long max, gfp_t flags)
{
	struct location *l;
	int order;

	order = get_order(sizeof(struct location) * max);

	l = (void *)__get_free_pages(flags, order);
	if (!l)
		return 0;

	if (t->count) {
		memcpy(l, t->loc, sizeof(struct location) * t->count);
		free_loc_track(t);
	}
	t->max = max;
	t->loc = l;
	return 1;
}

static int add_location(struct loc_track *t, struct kmem_cache *s,
				const struct track *track)
{
	long start, end, pos;
	struct location *l;
	unsigned long caddr;
	unsigned long age = jiffies - track->when;

	start = -1;
	end = t->count;

	for ( ; ; ) {
		pos = start + (end - start + 1) / 2;

		/*
		 * There is nothing at "end". If we end up there
		 * we need to add something to before end.
		 */
		if (pos == end)
			break;

		caddr = t->loc[pos].addr;
		if (track->addr == caddr) {

			l = &t->loc[pos];
			l->count++;
			if (track->when) {
				l->sum_time += age;
				if (age < l->min_time)
					l->min_time = age;
				if (age > l->max_time)
					l->max_time = age;

				if (track->pid < l->min_pid)
					l->min_pid = track->pid;
				if (track->pid > l->max_pid)
					l->max_pid = track->pid;

				cpumask_set_cpu(track->cpu,
						to_cpumask(l->cpus));
			}
			node_set(page_to_nid(virt_to_page(track)), l->nodes);
			return 1;
		}

		if (track->addr < caddr)
			end = pos;
		else
			start = pos;
	}

	/*
	 * Not found. Insert new tracking element.
	 */
	if (t->count >= t->max && !alloc_loc_track(t, 2 * t->max, GFP_ATOMIC))
		return 0;

	l = t->loc + pos;
	if (pos < t->count)
		memmove(l + 1, l,
			(t->count - pos) * sizeof(struct location));
	t->count++;
	l->count = 1;
	l->addr = track->addr;
	l->sum_time = age;
	l->min_time = age;
	l->max_time = age;
	l->min_pid = track->pid;
	l->max_pid = track->pid;
	cpumask_clear(to_cpumask(l->cpus));
	cpumask_set_cpu(track->cpu, to_cpumask(l->cpus));
	nodes_clear(l->nodes);
	node_set(page_to_nid(virt_to_page(track)), l->nodes);
	return 1;
}

static void process_slab(struct loc_track *t, struct kmem_cache *s,
		struct page *page, enum track_item alloc,
		unsigned long *obj_map)
{
	void *addr = page_address(page);
	void *p;

	__fill_map(obj_map, s, page);

	for_each_object(p, s, addr, page->objects)
		if (!test_bit(__obj_to_index(s, addr, p), obj_map))
			add_location(t, s, get_track(s, p, alloc));
}
#endif  /* CONFIG_DEBUG_FS   */
#endif	/* CONFIG_SLUB_DEBUG */

#ifdef CONFIG_SYSFS
enum slab_stat_type {
	SL_ALL,			/* All slabs */
	SL_PARTIAL,		/* Only partially allocated slabs */
	SL_CPU,			/* Only slabs used for cpu caches */
	SL_OBJECTS,		/* Determine allocated objects not slabs */
	SL_TOTAL		/* Determine object capacity not slabs */
};

#define SO_ALL		(1 << SL_ALL)
#define SO_PARTIAL	(1 << SL_PARTIAL)
#define SO_CPU		(1 << SL_CPU)
#define SO_OBJECTS	(1 << SL_OBJECTS)
#define SO_TOTAL	(1 << SL_TOTAL)

static ssize_t show_slab_objects(struct kmem_cache *s,
				 char *buf, unsigned long flags)
{
	unsigned long total = 0;
	int node;
	int x;
	unsigned long *nodes;
	int len = 0;

	nodes = kcalloc(nr_node_ids, sizeof(unsigned long), GFP_KERNEL);
	if (!nodes)
		return -ENOMEM;

	if (flags & SO_CPU) {
		int cpu;

		for_each_possible_cpu(cpu) {
			struct kmem_cache_cpu *c = per_cpu_ptr(s->cpu_slab,
							       cpu);
			int node;
			struct page *page;

			page = READ_ONCE(c->page);
			if (!page)
				continue;

			node = page_to_nid(page);
			if (flags & SO_TOTAL)
				x = page->objects;
			else if (flags & SO_OBJECTS)
				x = page->inuse;
			else
				x = 1;

			total += x;
			nodes[node] += x;

			page = slub_percpu_partial_read_once(c);
			if (page) {
				node = page_to_nid(page);
				if (flags & SO_TOTAL)
					WARN_ON_ONCE(1);
				else if (flags & SO_OBJECTS)
					WARN_ON_ONCE(1);
				else
					x = page->pages;
				total += x;
				nodes[node] += x;
			}
		}
	}

	/*
	 * It is impossible to take "mem_hotplug_lock" here with "kernfs_mutex"
	 * already held which will conflict with an existing lock order:
	 *
	 * mem_hotplug_lock->slab_mutex->kernfs_mutex
	 *
	 * We don't really need mem_hotplug_lock (to hold off
	 * slab_mem_going_offline_callback) here because slab's memory hot
	 * unplug code doesn't destroy the kmem_cache->node[] data.
	 */

#ifdef CONFIG_SLUB_DEBUG
	if (flags & SO_ALL) {
		struct kmem_cache_node *n;

		for_each_kmem_cache_node(s, node, n) {

			if (flags & SO_TOTAL)
				x = atomic_long_read(&n->total_objects);
			else if (flags & SO_OBJECTS)
				x = atomic_long_read(&n->total_objects) -
					count_partial(n, count_free);
			else
				x = atomic_long_read(&n->nr_slabs);
			total += x;
			nodes[node] += x;
		}

	} else
#endif
	if (flags & SO_PARTIAL) {
		struct kmem_cache_node *n;

		for_each_kmem_cache_node(s, node, n) {
			if (flags & SO_TOTAL)
				x = count_partial(n, count_total);
			else if (flags & SO_OBJECTS)
				x = count_partial(n, count_inuse);
			else
				x = n->nr_partial;
			total += x;
			nodes[node] += x;
		}
	}

	len += sysfs_emit_at(buf, len, "%lu", total);
#ifdef CONFIG_NUMA
	for (node = 0; node < nr_node_ids; node++) {
		if (nodes[node])
			len += sysfs_emit_at(buf, len, " N%d=%lu",
					     node, nodes[node]);
	}
#endif
	len += sysfs_emit_at(buf, len, "\n");
	kfree(nodes);

	return len;
}

#define to_slab_attr(n) container_of(n, struct slab_attribute, attr)
#define to_slab(n) container_of(n, struct kmem_cache, kobj)

struct slab_attribute {
	struct attribute attr;
	ssize_t (*show)(struct kmem_cache *s, char *buf);
	ssize_t (*store)(struct kmem_cache *s, const char *x, size_t count);
};

#define SLAB_ATTR_RO(_name) \
	static struct slab_attribute _name##_attr = \
	__ATTR(_name, 0400, _name##_show, NULL)

#define SLAB_ATTR(_name) \
	static struct slab_attribute _name##_attr =  \
	__ATTR(_name, 0600, _name##_show, _name##_store)

static ssize_t slab_size_show(struct kmem_cache *s, char *buf)
{
	return sysfs_emit(buf, "%u\n", s->size);
}
SLAB_ATTR_RO(slab_size);

static ssize_t align_show(struct kmem_cache *s, char *buf)
{
	return sysfs_emit(buf, "%u\n", s->align);
}
SLAB_ATTR_RO(align);

static ssize_t object_size_show(struct kmem_cache *s, char *buf)
{
	return sysfs_emit(buf, "%u\n", s->object_size);
}
SLAB_ATTR_RO(object_size);

static ssize_t objs_per_slab_show(struct kmem_cache *s, char *buf)
{
	return sysfs_emit(buf, "%u\n", oo_objects(s->oo));
}
SLAB_ATTR_RO(objs_per_slab);

static ssize_t order_show(struct kmem_cache *s, char *buf)
{
	return sysfs_emit(buf, "%u\n", oo_order(s->oo));
}
SLAB_ATTR_RO(order);

static ssize_t min_partial_show(struct kmem_cache *s, char *buf)
{
	return sysfs_emit(buf, "%lu\n", s->min_partial);
}

static ssize_t min_partial_store(struct kmem_cache *s, const char *buf,
				 size_t length)
{
	unsigned long min;
	int err;

	err = kstrtoul(buf, 10, &min);
	if (err)
		return err;

	set_min_partial(s, min);
	return length;
}
SLAB_ATTR(min_partial);

static ssize_t cpu_partial_show(struct kmem_cache *s, char *buf)
{
	return sysfs_emit(buf, "%u\n", slub_cpu_partial(s));
}

static ssize_t cpu_partial_store(struct kmem_cache *s, const char *buf,
				 size_t length)
{
	unsigned int objects;
	int err;

	err = kstrtouint(buf, 10, &objects);
	if (err)
		return err;
	if (objects && !kmem_cache_has_cpu_partial(s))
		return -EINVAL;

	slub_set_cpu_partial(s, objects);
	flush_all(s);
	return length;
}
SLAB_ATTR(cpu_partial);

static ssize_t ctor_show(struct kmem_cache *s, char *buf)
{
	if (!s->ctor)
		return 0;
	return sysfs_emit(buf, "%pS\n", s->ctor);
}
SLAB_ATTR_RO(ctor);

static ssize_t aliases_show(struct kmem_cache *s, char *buf)
{
	return sysfs_emit(buf, "%d\n", s->refcount < 0 ? 0 : s->refcount - 1);
}
SLAB_ATTR_RO(aliases);

static ssize_t partial_show(struct kmem_cache *s, char *buf)
{
	return show_slab_objects(s, buf, SO_PARTIAL);
}
SLAB_ATTR_RO(partial);

static ssize_t cpu_slabs_show(struct kmem_cache *s, char *buf)
{
	return show_slab_objects(s, buf, SO_CPU);
}
SLAB_ATTR_RO(cpu_slabs);

static ssize_t objects_show(struct kmem_cache *s, char *buf)
{
	return show_slab_objects(s, buf, SO_ALL|SO_OBJECTS);
}
SLAB_ATTR_RO(objects);

static ssize_t objects_partial_show(struct kmem_cache *s, char *buf)
{
	return show_slab_objects(s, buf, SO_PARTIAL|SO_OBJECTS);
}
SLAB_ATTR_RO(objects_partial);

static ssize_t slabs_cpu_partial_show(struct kmem_cache *s, char *buf)
{
	int objects = 0;
	int pages = 0;
	int cpu;
	int len = 0;

	for_each_online_cpu(cpu) {
		struct page *page;

		page = slub_percpu_partial(per_cpu_ptr(s->cpu_slab, cpu));

		if (page) {
			pages += page->pages;
			objects += page->pobjects;
		}
	}

	len += sysfs_emit_at(buf, len, "%d(%d)", objects, pages);

#ifdef CONFIG_SMP
	for_each_online_cpu(cpu) {
		struct page *page;

		page = slub_percpu_partial(per_cpu_ptr(s->cpu_slab, cpu));
		if (page)
			len += sysfs_emit_at(buf, len, " C%d=%d(%d)",
					     cpu, page->pobjects, page->pages);
	}
#endif
	len += sysfs_emit_at(buf, len, "\n");

	return len;
}
SLAB_ATTR_RO(slabs_cpu_partial);

static ssize_t reclaim_account_show(struct kmem_cache *s, char *buf)
{
	return sysfs_emit(buf, "%d\n", !!(s->flags & SLAB_RECLAIM_ACCOUNT));
}
SLAB_ATTR_RO(reclaim_account);

static ssize_t hwcache_align_show(struct kmem_cache *s, char *buf)
{
	return sysfs_emit(buf, "%d\n", !!(s->flags & SLAB_HWCACHE_ALIGN));
}
SLAB_ATTR_RO(hwcache_align);

#ifdef CONFIG_ZONE_DMA
static ssize_t cache_dma_show(struct kmem_cache *s, char *buf)
{
	return sysfs_emit(buf, "%d\n", !!(s->flags & SLAB_CACHE_DMA));
}
SLAB_ATTR_RO(cache_dma);
#endif

static ssize_t usersize_show(struct kmem_cache *s, char *buf)
{
	return sysfs_emit(buf, "%u\n", s->usersize);
}
SLAB_ATTR_RO(usersize);

static ssize_t destroy_by_rcu_show(struct kmem_cache *s, char *buf)
{
	return sysfs_emit(buf, "%d\n", !!(s->flags & SLAB_TYPESAFE_BY_RCU));
}
SLAB_ATTR_RO(destroy_by_rcu);

#ifdef CONFIG_SLUB_DEBUG
static ssize_t slabs_show(struct kmem_cache *s, char *buf)
{
	return show_slab_objects(s, buf, SO_ALL);
}
SLAB_ATTR_RO(slabs);

static ssize_t total_objects_show(struct kmem_cache *s, char *buf)
{
	return show_slab_objects(s, buf, SO_ALL|SO_TOTAL);
}
SLAB_ATTR_RO(total_objects);

static ssize_t sanity_checks_show(struct kmem_cache *s, char *buf)
{
	return sysfs_emit(buf, "%d\n", !!(s->flags & SLAB_CONSISTENCY_CHECKS));
}
SLAB_ATTR_RO(sanity_checks);

static ssize_t trace_show(struct kmem_cache *s, char *buf)
{
	return sysfs_emit(buf, "%d\n", !!(s->flags & SLAB_TRACE));
}
SLAB_ATTR_RO(trace);

static ssize_t red_zone_show(struct kmem_cache *s, char *buf)
{
	return sysfs_emit(buf, "%d\n", !!(s->flags & SLAB_RED_ZONE));
}

SLAB_ATTR_RO(red_zone);

static ssize_t poison_show(struct kmem_cache *s, char *buf)
{
	return sysfs_emit(buf, "%d\n", !!(s->flags & SLAB_POISON));
}

SLAB_ATTR_RO(poison);

static ssize_t store_user_show(struct kmem_cache *s, char *buf)
{
	return sysfs_emit(buf, "%d\n", !!(s->flags & SLAB_STORE_USER));
}

SLAB_ATTR_RO(store_user);

static ssize_t validate_show(struct kmem_cache *s, char *buf)
{
	return 0;
}

static ssize_t validate_store(struct kmem_cache *s,
			const char *buf, size_t length)
{
	int ret = -EINVAL;

	if (buf[0] == '1') {
		ret = validate_slab_cache(s);
		if (ret >= 0)
			ret = length;
	}
	return ret;
}
SLAB_ATTR(validate);

#endif /* CONFIG_SLUB_DEBUG */

#ifdef CONFIG_FAILSLAB
static ssize_t failslab_show(struct kmem_cache *s, char *buf)
{
	return sysfs_emit(buf, "%d\n", !!(s->flags & SLAB_FAILSLAB));
}
SLAB_ATTR_RO(failslab);
#endif

static ssize_t shrink_show(struct kmem_cache *s, char *buf)
{
	return 0;
}

static ssize_t shrink_store(struct kmem_cache *s,
			const char *buf, size_t length)
{
	if (buf[0] == '1')
		kmem_cache_shrink(s);
	else
		return -EINVAL;
	return length;
}
SLAB_ATTR(shrink);

#ifdef CONFIG_NUMA
static ssize_t remote_node_defrag_ratio_show(struct kmem_cache *s, char *buf)
{
	return sysfs_emit(buf, "%u\n", s->remote_node_defrag_ratio / 10);
}

static ssize_t remote_node_defrag_ratio_store(struct kmem_cache *s,
				const char *buf, size_t length)
{
	unsigned int ratio;
	int err;

	err = kstrtouint(buf, 10, &ratio);
	if (err)
		return err;
	if (ratio > 100)
		return -ERANGE;

	s->remote_node_defrag_ratio = ratio * 10;

	return length;
}
SLAB_ATTR(remote_node_defrag_ratio);
#endif

#ifdef CONFIG_SLUB_STATS
static int show_stat(struct kmem_cache *s, char *buf, enum stat_item si)
{
	unsigned long sum  = 0;
	int cpu;
	int len = 0;
	int *data = kmalloc_array(nr_cpu_ids, sizeof(int), GFP_KERNEL);

	if (!data)
		return -ENOMEM;

	for_each_online_cpu(cpu) {
		unsigned x = per_cpu_ptr(s->cpu_slab, cpu)->stat[si];

		data[cpu] = x;
		sum += x;
	}

	len += sysfs_emit_at(buf, len, "%lu", sum);

#ifdef CONFIG_SMP
	for_each_online_cpu(cpu) {
		if (data[cpu])
			len += sysfs_emit_at(buf, len, " C%d=%u",
					     cpu, data[cpu]);
	}
#endif
	kfree(data);
	len += sysfs_emit_at(buf, len, "\n");

	return len;
}

static void clear_stat(struct kmem_cache *s, enum stat_item si)
{
	int cpu;

	for_each_online_cpu(cpu)
		per_cpu_ptr(s->cpu_slab, cpu)->stat[si] = 0;
}

#define STAT_ATTR(si, text) 					\
static ssize_t text##_show(struct kmem_cache *s, char *buf)	\
{								\
	return show_stat(s, buf, si);				\
}								\
static ssize_t text##_store(struct kmem_cache *s,		\
				const char *buf, size_t length)	\
{								\
	if (buf[0] != '0')					\
		return -EINVAL;					\
	clear_stat(s, si);					\
	return length;						\
}								\
SLAB_ATTR(text);						\

STAT_ATTR(ALLOC_FASTPATH, alloc_fastpath);
STAT_ATTR(ALLOC_SLOWPATH, alloc_slowpath);
STAT_ATTR(FREE_FASTPATH, free_fastpath);
STAT_ATTR(FREE_SLOWPATH, free_slowpath);
STAT_ATTR(FREE_FROZEN, free_frozen);
STAT_ATTR(FREE_ADD_PARTIAL, free_add_partial);
STAT_ATTR(FREE_REMOVE_PARTIAL, free_remove_partial);
STAT_ATTR(ALLOC_FROM_PARTIAL, alloc_from_partial);
STAT_ATTR(ALLOC_SLAB, alloc_slab);
STAT_ATTR(ALLOC_REFILL, alloc_refill);
STAT_ATTR(ALLOC_NODE_MISMATCH, alloc_node_mismatch);
STAT_ATTR(FREE_SLAB, free_slab);
STAT_ATTR(CPUSLAB_FLUSH, cpuslab_flush);
STAT_ATTR(DEACTIVATE_FULL, deactivate_full);
STAT_ATTR(DEACTIVATE_EMPTY, deactivate_empty);
STAT_ATTR(DEACTIVATE_TO_HEAD, deactivate_to_head);
STAT_ATTR(DEACTIVATE_TO_TAIL, deactivate_to_tail);
STAT_ATTR(DEACTIVATE_REMOTE_FREES, deactivate_remote_frees);
STAT_ATTR(DEACTIVATE_BYPASS, deactivate_bypass);
STAT_ATTR(ORDER_FALLBACK, order_fallback);
STAT_ATTR(CMPXCHG_DOUBLE_CPU_FAIL, cmpxchg_double_cpu_fail);
STAT_ATTR(CMPXCHG_DOUBLE_FAIL, cmpxchg_double_fail);
STAT_ATTR(CPU_PARTIAL_ALLOC, cpu_partial_alloc);
STAT_ATTR(CPU_PARTIAL_FREE, cpu_partial_free);
STAT_ATTR(CPU_PARTIAL_NODE, cpu_partial_node);
STAT_ATTR(CPU_PARTIAL_DRAIN, cpu_partial_drain);
#endif	/* CONFIG_SLUB_STATS */

static struct attribute *slab_attrs[] = {
	&slab_size_attr.attr,
	&object_size_attr.attr,
	&objs_per_slab_attr.attr,
	&order_attr.attr,
	&min_partial_attr.attr,
	&cpu_partial_attr.attr,
	&objects_attr.attr,
	&objects_partial_attr.attr,
	&partial_attr.attr,
	&cpu_slabs_attr.attr,
	&ctor_attr.attr,
	&aliases_attr.attr,
	&align_attr.attr,
	&hwcache_align_attr.attr,
	&reclaim_account_attr.attr,
	&destroy_by_rcu_attr.attr,
	&shrink_attr.attr,
	&slabs_cpu_partial_attr.attr,
#ifdef CONFIG_SLUB_DEBUG
	&total_objects_attr.attr,
	&slabs_attr.attr,
	&sanity_checks_attr.attr,
	&trace_attr.attr,
	&red_zone_attr.attr,
	&poison_attr.attr,
	&store_user_attr.attr,
	&validate_attr.attr,
#endif
#ifdef CONFIG_ZONE_DMA
	&cache_dma_attr.attr,
#endif
#ifdef CONFIG_NUMA
	&remote_node_defrag_ratio_attr.attr,
#endif
#ifdef CONFIG_SLUB_STATS
	&alloc_fastpath_attr.attr,
	&alloc_slowpath_attr.attr,
	&free_fastpath_attr.attr,
	&free_slowpath_attr.attr,
	&free_frozen_attr.attr,
	&free_add_partial_attr.attr,
	&free_remove_partial_attr.attr,
	&alloc_from_partial_attr.attr,
	&alloc_slab_attr.attr,
	&alloc_refill_attr.attr,
	&alloc_node_mismatch_attr.attr,
	&free_slab_attr.attr,
	&cpuslab_flush_attr.attr,
	&deactivate_full_attr.attr,
	&deactivate_empty_attr.attr,
	&deactivate_to_head_attr.attr,
	&deactivate_to_tail_attr.attr,
	&deactivate_remote_frees_attr.attr,
	&deactivate_bypass_attr.attr,
	&order_fallback_attr.attr,
	&cmpxchg_double_fail_attr.attr,
	&cmpxchg_double_cpu_fail_attr.attr,
	&cpu_partial_alloc_attr.attr,
	&cpu_partial_free_attr.attr,
	&cpu_partial_node_attr.attr,
	&cpu_partial_drain_attr.attr,
#endif
#ifdef CONFIG_FAILSLAB
	&failslab_attr.attr,
#endif
	&usersize_attr.attr,

	NULL
};

static const struct attribute_group slab_attr_group = {
	.attrs = slab_attrs,
};

static ssize_t slab_attr_show(struct kobject *kobj,
				struct attribute *attr,
				char *buf)
{
	struct slab_attribute *attribute;
	struct kmem_cache *s;
	int err;

	attribute = to_slab_attr(attr);
	s = to_slab(kobj);

	if (!attribute->show)
		return -EIO;

	err = attribute->show(s, buf);

	return err;
}

static ssize_t slab_attr_store(struct kobject *kobj,
				struct attribute *attr,
				const char *buf, size_t len)
{
	struct slab_attribute *attribute;
	struct kmem_cache *s;
	int err;

	attribute = to_slab_attr(attr);
	s = to_slab(kobj);

	if (!attribute->store)
		return -EIO;

	err = attribute->store(s, buf, len);
	return err;
}

static void kmem_cache_release(struct kobject *k)
{
	slab_kmem_cache_release(to_slab(k));
}

static const struct sysfs_ops slab_sysfs_ops = {
	.show = slab_attr_show,
	.store = slab_attr_store,
};

static struct kobj_type slab_ktype = {
	.sysfs_ops = &slab_sysfs_ops,
	.release = kmem_cache_release,
};

static struct kset *slab_kset;

static inline struct kset *cache_kset(struct kmem_cache *s)
{
	return slab_kset;
}

#define ID_STR_LENGTH 64

/* Create a unique string id for a slab cache:
 *
 * Format	:[flags-]size
 */
static char *create_unique_id(struct kmem_cache *s)
{
	char *name = kmalloc(ID_STR_LENGTH, GFP_KERNEL);
	char *p = name;

	BUG_ON(!name);

	*p++ = ':';
	/*
	 * First flags affecting slabcache operations. We will only
	 * get here for aliasable slabs so we do not need to support
	 * too many flags. The flags here must cover all flags that
	 * are matched during merging to guarantee that the id is
	 * unique.
	 */
	if (s->flags & SLAB_CACHE_DMA)
		*p++ = 'd';
	if (s->flags & SLAB_CACHE_DMA32)
		*p++ = 'D';
	if (s->flags & SLAB_RECLAIM_ACCOUNT)
		*p++ = 'a';
	if (s->flags & SLAB_CONSISTENCY_CHECKS)
		*p++ = 'F';
	if (s->flags & SLAB_ACCOUNT)
		*p++ = 'A';
	if (p != name + 1)
		*p++ = '-';
	p += sprintf(p, "%07u", s->size);

	BUG_ON(p > name + ID_STR_LENGTH - 1);
	return name;
}

static int sysfs_slab_add(struct kmem_cache *s)
{
	int err;
	const char *name;
	struct kset *kset = cache_kset(s);
	int unmergeable = slab_unmergeable(s);

	if (!kset) {
		kobject_init(&s->kobj, &slab_ktype);
		return 0;
	}

	if (!unmergeable && disable_higher_order_debug &&
			(slub_debug & DEBUG_METADATA_FLAGS))
		unmergeable = 1;

	if (unmergeable) {
		/*
		 * Slabcache can never be merged so we can use the name proper.
		 * This is typically the case for debug situations. In that
		 * case we can catch duplicate names easily.
		 */
		sysfs_remove_link(&slab_kset->kobj, s->name);
		name = s->name;
	} else {
		/*
		 * Create a unique name for the slab as a target
		 * for the symlinks.
		 */
		name = create_unique_id(s);
	}

	s->kobj.kset = kset;
	err = kobject_init_and_add(&s->kobj, &slab_ktype, NULL, "%s", name);
	if (err)
		goto out;

	err = sysfs_create_group(&s->kobj, &slab_attr_group);
	if (err)
		goto out_del_kobj;

	if (!unmergeable) {
		/* Setup first alias */
		sysfs_slab_alias(s, s->name);
	}
out:
	if (!unmergeable)
		kfree(name);
	return err;
out_del_kobj:
	kobject_del(&s->kobj);
	goto out;
}

void sysfs_slab_unlink(struct kmem_cache *s)
{
	if (slab_state >= FULL)
		kobject_del(&s->kobj);
}

void sysfs_slab_release(struct kmem_cache *s)
{
	if (slab_state >= FULL)
		kobject_put(&s->kobj);
}

/*
 * Need to buffer aliases during bootup until sysfs becomes
 * available lest we lose that information.
 */
struct saved_alias {
	struct kmem_cache *s;
	const char *name;
	struct saved_alias *next;
};

static struct saved_alias *alias_list;

static int sysfs_slab_alias(struct kmem_cache *s, const char *name)
{
	struct saved_alias *al;

	if (slab_state == FULL) {
		/*
		 * If we have a leftover link then remove it.
		 */
		sysfs_remove_link(&slab_kset->kobj, name);
		return sysfs_create_link(&slab_kset->kobj, &s->kobj, name);
	}

	al = kmalloc(sizeof(struct saved_alias), GFP_KERNEL);
	if (!al)
		return -ENOMEM;

	al->s = s;
	al->name = name;
	al->next = alias_list;
	alias_list = al;
	return 0;
}

/*
 * IAMROOT, 2022.06.18:
 * - sysfs의 slab
 */
static int __init slab_sysfs_init(void)
{
	struct kmem_cache *s;
	int err;

	mutex_lock(&slab_mutex);

	slab_kset = kset_create_and_add("slab", NULL, kernel_kobj);
	if (!slab_kset) {
		mutex_unlock(&slab_mutex);
		pr_err("Cannot register slab subsystem.\n");
		return -ENOSYS;
	}

	slab_state = FULL;

	list_for_each_entry(s, &slab_caches, list) {
		err = sysfs_slab_add(s);
		if (err)
			pr_err("SLUB: Unable to add boot slab %s to sysfs\n",
			       s->name);
	}

	while (alias_list) {
		struct saved_alias *al = alias_list;

		alias_list = alias_list->next;
		err = sysfs_slab_alias(al->s, al->name);
		if (err)
			pr_err("SLUB: Unable to add boot slab alias %s to sysfs\n",
			       al->name);
		kfree(al);
	}

	mutex_unlock(&slab_mutex);
	return 0;
}

__initcall(slab_sysfs_init);
#endif /* CONFIG_SYSFS */

#if defined(CONFIG_SLUB_DEBUG) && defined(CONFIG_DEBUG_FS)
static int slab_debugfs_show(struct seq_file *seq, void *v)
{

	struct location *l;
	unsigned int idx = *(unsigned int *)v;
	struct loc_track *t = seq->private;

	if (idx < t->count) {
		l = &t->loc[idx];

		seq_printf(seq, "%7ld ", l->count);

		if (l->addr)
			seq_printf(seq, "%pS", (void *)l->addr);
		else
			seq_puts(seq, "<not-available>");

		if (l->sum_time != l->min_time) {
			seq_printf(seq, " age=%ld/%llu/%ld",
				l->min_time, div_u64(l->sum_time, l->count),
				l->max_time);
		} else
			seq_printf(seq, " age=%ld", l->min_time);

		if (l->min_pid != l->max_pid)
			seq_printf(seq, " pid=%ld-%ld", l->min_pid, l->max_pid);
		else
			seq_printf(seq, " pid=%ld",
				l->min_pid);

		if (num_online_cpus() > 1 && !cpumask_empty(to_cpumask(l->cpus)))
			seq_printf(seq, " cpus=%*pbl",
				 cpumask_pr_args(to_cpumask(l->cpus)));

		if (nr_online_nodes > 1 && !nodes_empty(l->nodes))
			seq_printf(seq, " nodes=%*pbl",
				 nodemask_pr_args(&l->nodes));

		seq_puts(seq, "\n");
	}

	if (!idx && !t->count)
		seq_puts(seq, "No data\n");

	return 0;
}

static void slab_debugfs_stop(struct seq_file *seq, void *v)
{
}

static void *slab_debugfs_next(struct seq_file *seq, void *v, loff_t *ppos)
{
	struct loc_track *t = seq->private;

	v = ppos;
	++*ppos;
	if (*ppos <= t->count)
		return v;

	return NULL;
}

static void *slab_debugfs_start(struct seq_file *seq, loff_t *ppos)
{
	return ppos;
}

static const struct seq_operations slab_debugfs_sops = {
	.start  = slab_debugfs_start,
	.next   = slab_debugfs_next,
	.stop   = slab_debugfs_stop,
	.show   = slab_debugfs_show,
};

static int slab_debug_trace_open(struct inode *inode, struct file *filep)
{

	struct kmem_cache_node *n;
	enum track_item alloc;
	int node;
	struct loc_track *t = __seq_open_private(filep, &slab_debugfs_sops,
						sizeof(struct loc_track));
	struct kmem_cache *s = file_inode(filep)->i_private;
	unsigned long *obj_map;

	if (!t)
		return -ENOMEM;

	obj_map = bitmap_alloc(oo_objects(s->oo), GFP_KERNEL);
	if (!obj_map) {
		seq_release_private(inode, filep);
		return -ENOMEM;
	}

	if (strcmp(filep->f_path.dentry->d_name.name, "alloc_traces") == 0)
		alloc = TRACK_ALLOC;
	else
		alloc = TRACK_FREE;

	if (!alloc_loc_track(t, PAGE_SIZE / sizeof(struct location), GFP_KERNEL)) {
		bitmap_free(obj_map);
		seq_release_private(inode, filep);
		return -ENOMEM;
	}

	for_each_kmem_cache_node(s, node, n) {
		unsigned long flags;
		struct page *page;

		if (!atomic_long_read(&n->nr_slabs))
			continue;

		spin_lock_irqsave(&n->list_lock, flags);
		list_for_each_entry(page, &n->partial, slab_list)
			process_slab(t, s, page, alloc, obj_map);
		list_for_each_entry(page, &n->full, slab_list)
			process_slab(t, s, page, alloc, obj_map);
		spin_unlock_irqrestore(&n->list_lock, flags);
	}

	bitmap_free(obj_map);
	return 0;
}

static int slab_debug_trace_release(struct inode *inode, struct file *file)
{
	struct seq_file *seq = file->private_data;
	struct loc_track *t = seq->private;

	free_loc_track(t);
	return seq_release_private(inode, file);
}

static const struct file_operations slab_debugfs_fops = {
	.open    = slab_debug_trace_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = slab_debug_trace_release,
};

static void debugfs_slab_add(struct kmem_cache *s)
{
	struct dentry *slab_cache_dir;

	if (unlikely(!slab_debugfs_root))
		return;

	slab_cache_dir = debugfs_create_dir(s->name, slab_debugfs_root);

	debugfs_create_file("alloc_traces", 0400,
		slab_cache_dir, s, &slab_debugfs_fops);

	debugfs_create_file("free_traces", 0400,
		slab_cache_dir, s, &slab_debugfs_fops);
}

void debugfs_slab_release(struct kmem_cache *s)
{
	debugfs_remove_recursive(debugfs_lookup(s->name, slab_debugfs_root));
}

static int __init slab_debugfs_init(void)
{
	struct kmem_cache *s;

	slab_debugfs_root = debugfs_create_dir("slab", NULL);

	list_for_each_entry(s, &slab_caches, list)
		if (s->flags & SLAB_STORE_USER)
			debugfs_slab_add(s);

	return 0;

}
__initcall(slab_debugfs_init);
#endif
/*
 * The /proc/slabinfo ABI
 */
#ifdef CONFIG_SLUB_DEBUG
void get_slabinfo(struct kmem_cache *s, struct slabinfo *sinfo)
{
	unsigned long nr_slabs = 0;
	unsigned long nr_objs = 0;
	unsigned long nr_free = 0;
	int node;
	struct kmem_cache_node *n;

	for_each_kmem_cache_node(s, node, n) {
		nr_slabs += node_nr_slabs(n);
		nr_objs += node_nr_objs(n);
		nr_free += count_partial(n, count_free);
	}

	sinfo->active_objs = nr_objs - nr_free;
	sinfo->num_objs = nr_objs;
	sinfo->active_slabs = nr_slabs;
	sinfo->num_slabs = nr_slabs;
	sinfo->objects_per_slab = oo_objects(s->oo);
	sinfo->cache_order = oo_order(s->oo);
}

void slabinfo_show_stats(struct seq_file *m, struct kmem_cache *s)
{
}

ssize_t slabinfo_write(struct file *file, const char __user *buffer,
		       size_t count, loff_t *ppos)
{
	return -EIO;
}
#endif /* CONFIG_SLUB_DEBUG */
