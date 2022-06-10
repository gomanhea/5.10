/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_RMAP_H
#define _LINUX_RMAP_H
/*
 * Declarations for Reverse Mapping functions in mm/rmap.c
 */

#include <linux/list.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/rwsem.h>
#include <linux/memcontrol.h>
#include <linux/highmem.h>

/*
 * The anon_vma heads a list of private "related" vmas, to scan if
 * an anonymous page pointing to this anon_vma needs to be unmapped:
 * the vmas on the list will be related by forking, or by splitting.
 *
 * Since vmas come and go as they are split and merged (particularly
 * in mprotect), the mapping field of an anonymous page cannot point
 * directly to a vma: instead it points to an anon_vma, on whose list
 * the related vmas can be easily linked or unlinked.
 *
 * After unlinking the last vma on the list, we must garbage collect
 * the anon_vma object itself: we're guaranteed no page can be
 * pointing to this anon_vma once its vma list is empty.
 */
struct anon_vma {
/*
 * IAMROOT, 2022.06.04:
 * - 최초생성시 root는 자기자신.
 */
	struct anon_vma *root;		/* Root of this anon_vma tree */
	struct rw_semaphore rwsem;	/* W: modification, R: walking the list */
	/*
	 * The refcount is taken on an anon_vma when there is no
	 * guarantee that the vma of page tables will exist for
	 * the duration of the operation. A caller that takes
	 * the reference is responsible for clearing up the
	 * anon_vma if they are the last user on release
	 */
	atomic_t refcount;

	/*
	 * Count of child anon_vmas and VMAs which points to this anon_vma.
	 *
	 * This counter is used for making decision about reusing anon_vma
	 * instead of forking new one. See comments in function anon_vma_clone.
	 */
/*
 * IAMROOT, 2022.06.04:
 * -papago
 *  이 anon_vma를 가리키는 자식 anon_vmas 및 VMA의 수입니다.
 *
 *  이 카운터는 새로운 것을 분기하는 대신 anon_vma를 재사용하는 것에 대한
 *  결정을 내리는 데 사용됩니다. anon_vma_clone 함수의 주석을 참조하십시오.
 *
 * - 최초 할당시 1로 시작한다.(anon_vma_alloc() 참고)
 * - degree = child anon_vma 개수 + vma 개수
 *
 * -----
 *
 * 1. 자기참조. child 0. vma 1
 * anon_vma == anon_vma->parent. degree = 2
 * (parent 자기참조 1 + vma)
 *
 * 2. canon_vma(child anon_vma) = 3개. vma 1
 *
 * canon_vma1 -> anon_vma
 * canon_vma2 -/
 * canon_vma3 -/
 *
 * degree = 4. (anon_vma를 parent로보는 것이 3개 + vma)
 */
	unsigned degree;

/*
 * IAMROOT, 2022.06.04:
 * - 최초 할당시 parent는 자기자신.
 */
	struct anon_vma *parent;	/* Parent of this anon_vma */

	/*
	 * NOTE: the LSB of the rb_root.rb_node is set by
	 * mm_take_all_locks() _after_ taking the above lock. So the
	 * rb_root must only be read/written after taking the above lock
	 * to be sure to see a valid next pointer. The LSB bit itself
	 * is serialized by a system wide lock only visible to
	 * mm_take_all_locks() (mm_all_locks_mutex).
	 */

	/* Interval tree of private "related" vmas */
/*
 * IAMROOT, 2022.05.28:
 * - av와 연결된 avc들은 rb tree로 관리한다.
 *   avc는 vma와 1:1로 연결되있으므로, av는 rb tree를 통해 vma를 관리하게된다.
 */
	struct rb_root_cached rb_root;
};

/*
 * The copy-on-write semantics of fork mean that an anon_vma
 * can become associated with multiple processes. Furthermore,
 * each child process will have its own anon_vma, where new
 * pages for that process are instantiated.
 *
 * This structure allows us to find the anon_vmas associated
 * with a VMA, or the VMAs associated with an anon_vma.
 * The "same_vma" list contains the anon_vma_chains linking
 * all the anon_vmas associated with this VMA.
 * The "rb" field indexes on an interval tree the anon_vma_chains
 * which link all the VMAs associated with this anon_vma.
 */
/*
 * IAMROOT, 2022.05.28:
 * - avc --> vma 
 *   vma 로 직접 연결
 *
 * - avc <-- vma
 *   같은 vma를 가리키는 avc끼리 vma의 anon_vma_chain을 통해 list로 연결.
 *
 * - avc --> av
 *   anon_vma로 직접 연결
 *
 * - avc <-- av
 *   av에 속한 avc를 rb_root를 통해 rb tree로 관리
 */
struct anon_vma_chain {
/*
 * IAMROOT, 2022.05.28:
 * - avc와 연결되는 vma.
 */
	struct vm_area_struct *vma;
/*
 * IAMROOT, 2022.05.28:
 * - avc와 연결되는 av
 */
	struct anon_vma *anon_vma;
/*
 * IAMROOT, 2022.05.28:
 * - same_vma를 통해서 같은 vma를 가리키고있는 avc를 list로 연결한다.
 *       
 *                                                            
 * vma                              avc1       avc2           avc3
 *  ^   anon_vma_chain =========> same_vma <-> same_vma <-> same_vma
 *  \--------------------------------vma           vma         vma
 *   \----------------------------------------------+          |
 *    \--------------------------------------------------------+
 */
	struct list_head same_vma;   /* locked by mmap_lock & page_table_lock */
/*
 * IAMROOT, 2022.05.28:
 * - avc가 속한 av의 rb root에 들어갈 node.
 *   av는 여러개의 avc를 rb로 관리한다.
 */
	struct rb_node rb;			/* locked by anon_vma->rwsem */
	unsigned long rb_subtree_last;
#ifdef CONFIG_DEBUG_VM_RB
	unsigned long cached_vma_start, cached_vma_last;
#endif
};

/*
 * IAMROOT, 2022.04.09:
 * - Try To Unmap(TTU)
 */
enum ttu_flags {
	TTU_SPLIT_HUGE_PMD	= 0x4,	/* split huge PMD if any */
	TTU_IGNORE_MLOCK	= 0x8,	/* ignore mlock */
	TTU_SYNC		= 0x10,	/* avoid racy checks with PVMW_SYNC */
	TTU_IGNORE_HWPOISON	= 0x20,	/* corrupted page is recoverable */
	TTU_BATCH_FLUSH		= 0x40,	/* Batch TLB flushes where possible
					 * and caller guarantees they will
					 * do a final flush if necessary */
	TTU_RMAP_LOCKED		= 0x80,	/* do not grab rmap lock:
					 * caller holds it */
};

#ifdef CONFIG_MMU
static inline void get_anon_vma(struct anon_vma *anon_vma)
{
	atomic_inc(&anon_vma->refcount);
}

void __put_anon_vma(struct anon_vma *anon_vma);

/*
 * IAMROOT, 2022.05.28:
 * - @anon_vma ref drop
 */
static inline void put_anon_vma(struct anon_vma *anon_vma)
{
	if (atomic_dec_and_test(&anon_vma->refcount))
		__put_anon_vma(anon_vma);
}

static inline void anon_vma_lock_write(struct anon_vma *anon_vma)
{
	down_write(&anon_vma->root->rwsem);
}

static inline void anon_vma_unlock_write(struct anon_vma *anon_vma)
{
	up_write(&anon_vma->root->rwsem);
}

static inline void anon_vma_lock_read(struct anon_vma *anon_vma)
{
	down_read(&anon_vma->root->rwsem);
}

static inline void anon_vma_unlock_read(struct anon_vma *anon_vma)
{
	up_read(&anon_vma->root->rwsem);
}


/*
 * anon_vma helper functions.
 */
void anon_vma_init(void);	/* create anon_vma_cachep */
int  __anon_vma_prepare(struct vm_area_struct *);
void unlink_anon_vmas(struct vm_area_struct *);
int anon_vma_clone(struct vm_area_struct *, struct vm_area_struct *);
int anon_vma_fork(struct vm_area_struct *, struct vm_area_struct *);

/*
 * IAMROOT, 2022.06.04:
 * - @vma가 av이면 prepare가 필요없다.
 *   reuse가능한 av를 찾거나 av를 할당하고 vma에 연결한다.
 */
static inline int anon_vma_prepare(struct vm_area_struct *vma)
{
	if (likely(vma->anon_vma))
		return 0;

	return __anon_vma_prepare(vma);
}

/*
 * IAMROOT, 2022.05.28:
 * - @next와 link되있는 anon_vma에서 unlink한다.
 */
static inline void anon_vma_merge(struct vm_area_struct *vma,
				  struct vm_area_struct *next)
{
	VM_BUG_ON_VMA(vma->anon_vma != next->anon_vma, vma);
	unlink_anon_vmas(next);
}

struct anon_vma *page_get_anon_vma(struct page *page);

/* bitflags for do_page_add_anon_rmap() */
#define RMAP_EXCLUSIVE 0x01
#define RMAP_COMPOUND 0x02

/*
 * rmap interfaces called when adding or removing pte of page
 */
void page_move_anon_rmap(struct page *, struct vm_area_struct *);
void page_add_anon_rmap(struct page *, struct vm_area_struct *,
		unsigned long, bool);
void do_page_add_anon_rmap(struct page *, struct vm_area_struct *,
			   unsigned long, int);
void page_add_new_anon_rmap(struct page *, struct vm_area_struct *,
		unsigned long, bool);
void page_add_file_rmap(struct page *, bool);
void page_remove_rmap(struct page *, bool);

void hugepage_add_anon_rmap(struct page *, struct vm_area_struct *,
			    unsigned long);
void hugepage_add_new_anon_rmap(struct page *, struct vm_area_struct *,
				unsigned long);

static inline void page_dup_rmap(struct page *page, bool compound)
{
	atomic_inc(compound ? compound_mapcount_ptr(page) : &page->_mapcount);
}

/*
 * Called from mm/vmscan.c to handle paging out
 */
int page_referenced(struct page *, int is_locked,
			struct mem_cgroup *memcg, unsigned long *vm_flags);

void try_to_migrate(struct page *page, enum ttu_flags flags);
void try_to_unmap(struct page *, enum ttu_flags flags);

int make_device_exclusive_range(struct mm_struct *mm, unsigned long start,
				unsigned long end, struct page **pages,
				void *arg);

/* Avoid racy checks */
#define PVMW_SYNC		(1 << 0)
/* Look for migarion entries rather than present PTEs */
#define PVMW_MIGRATION		(1 << 1)

struct page_vma_mapped_walk {
	struct page *page;
	struct vm_area_struct *vma;
	unsigned long address;
	pmd_t *pmd;
	pte_t *pte;
	spinlock_t *ptl;
	unsigned int flags;
};

static inline void page_vma_mapped_walk_done(struct page_vma_mapped_walk *pvmw)
{
	/* HugeTLB pte is set to the relevant page table entry without pte_mapped. */
	if (pvmw->pte && !PageHuge(pvmw->page))
		pte_unmap(pvmw->pte);
	if (pvmw->ptl)
		spin_unlock(pvmw->ptl);
}

bool page_vma_mapped_walk(struct page_vma_mapped_walk *pvmw);

/*
 * Used by swapoff to help locate where page is expected in vma.
 */
unsigned long page_address_in_vma(struct page *, struct vm_area_struct *);

/*
 * Cleans the PTEs of shared mappings.
 * (and since clean PTEs should also be readonly, write protects them too)
 *
 * returns the number of cleaned PTEs.
 */
int page_mkclean(struct page *);

/*
 * called in munlock()/munmap() path to check for other vmas holding
 * the page mlocked.
 */
void page_mlock(struct page *page);

void remove_migration_ptes(struct page *old, struct page *new, bool locked);

/*
 * Called by memory-failure.c to kill processes.
 */
struct anon_vma *page_lock_anon_vma_read(struct page *page);
void page_unlock_anon_vma_read(struct anon_vma *anon_vma);
int page_mapped_in_vma(struct page *page, struct vm_area_struct *vma);

/*
 * rmap_walk_control: To control rmap traversing for specific needs
 *
 * arg: passed to rmap_one() and invalid_vma()
 * rmap_one: executed on each vma where page is mapped
 * done: for checking traversing termination condition
 * anon_lock: for getting anon_lock by optimized way rather than default
 * invalid_vma: for skipping uninterested vma
 */
struct rmap_walk_control {
	void *arg;
	/*
	 * Return false if page table scanning in rmap_walk should be stopped.
	 * Otherwise, return true.
	 */
	bool (*rmap_one)(struct page *page, struct vm_area_struct *vma,
					unsigned long addr, void *arg);
	int (*done)(struct page *page);
	struct anon_vma *(*anon_lock)(struct page *page);
	bool (*invalid_vma)(struct vm_area_struct *vma, void *arg);
};

void rmap_walk(struct page *page, struct rmap_walk_control *rwc);
void rmap_walk_locked(struct page *page, struct rmap_walk_control *rwc);

#else	/* !CONFIG_MMU */

#define anon_vma_init()		do {} while (0)
#define anon_vma_prepare(vma)	(0)
#define anon_vma_link(vma)	do {} while (0)

static inline int page_referenced(struct page *page, int is_locked,
				  struct mem_cgroup *memcg,
				  unsigned long *vm_flags)
{
	*vm_flags = 0;
	return 0;
}

static inline void try_to_unmap(struct page *page, enum ttu_flags flags)
{
}

static inline int page_mkclean(struct page *page)
{
	return 0;
}


#endif	/* CONFIG_MMU */

#endif	/* _LINUX_RMAP_H */
