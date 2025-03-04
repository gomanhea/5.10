// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2016 Linaro Ltd <ard.biesheuvel@linaro.org>
 */

#include <linux/cache.h>
#include <linux/crc32.h>
#include <linux/init.h>
#include <linux/libfdt.h>
#include <linux/mm_types.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/pgtable.h>
#include <linux/random.h>

#include <asm/cacheflush.h>
#include <asm/fixmap.h>
#include <asm/kernel-pgtable.h>
#include <asm/memory.h>
#include <asm/mmu.h>
#include <asm/sections.h>
#include <asm/setup.h>

enum kaslr_status {
	KASLR_ENABLED,
	KASLR_DISABLED_CMDLINE,
	KASLR_DISABLED_NO_SEED,
	KASLR_DISABLED_FDT_REMAP,
};

static enum kaslr_status __initdata kaslr_status;
u64 __ro_after_init module_alloc_base;
u16 __initdata memstart_offset_seed;

/*
 * IAMROOT, 2021.09.04:
 * - devicetree/bindings/chosen.txt
 *
 * {
 *	chosen {
 *		kaslr-seed = <0xfeedbeef 0xc0def00d>;
 *	};
 * };
 *
 * dtb에 위와 비슷하게 정의되있는 값이 존재하고, 해당 값을 추출하여
 * random값을 정의 하는데 사용한다. 없으면 0로 return 시킨다.
 */
static __init u64 get_kaslr_seed(void *fdt)
{
	int node, len;
	fdt64_t *prop;
	u64 ret;

	node = fdt_path_offset(fdt, "/chosen");
	if (node < 0)
		return 0;

	prop = fdt_getprop_w(fdt, node, "kaslr-seed", &len);
	if (!prop || len != sizeof(u64))
		return 0;

	ret = fdt64_to_cpu(*prop);
	*prop = 0;
	return ret;
}

struct arm64_ftr_override kaslr_feature_override __initdata;

/*
 * This routine will be executed with the kernel mapped at its default virtual
 * address, and if it returns successfully, the kernel will be remapped, and
 * start_kernel() will be executed from a randomized virtual offset. The
 * relocation will result in all absolute references (e.g., static variables
 * containing function pointers) to be reinitialized, and zero-initialized
 * .bss variables will be reset to 0.
 */
u64 __init kaslr_early_init(void)
{
	void *fdt;
	u64 seed, offset, mask, module_range;
	unsigned long raw;

	/*
	 * Set a reasonable default for module_alloc_base in case
	 * we end up running with module randomization disabled.
	 */
	module_alloc_base = (u64)_etext - MODULES_VSIZE;
	dcache_clean_inval_poc((unsigned long)&module_alloc_base,
			    (unsigned long)&module_alloc_base +
				    sizeof(module_alloc_base));

	/*
	 * Try to map the FDT early. If this fails, we simply bail,
	 * and proceed with KASLR disabled. We will make another
	 * attempt at mapping the FDT in setup_machine()
	 */
	fdt = get_early_fdt_ptr();
	if (!fdt) {
		kaslr_status = KASLR_DISABLED_FDT_REMAP;
		return 0;
	}

	/*
	 * Retrieve (and wipe) the seed from the FDT
	 */
	seed = get_kaslr_seed(fdt);

	/*
	 * Check if 'nokaslr' appears on the command line, and
	 * return 0 if that is the case.
	 */
/*
 * IAMROOT, 2021.11.15:
 * init_feature_override 함수가 미리 불러와져 해당 함수에서
 * dtb에서 /choosen/boot_args를 가져와 parsing 했을것이고
 * 그 과정에서 kaslr_feature_override 이 설정됬을것이다. 
 * 해당 arg는 nokaslr이 들어오면 val에 값이 set될것이다.
 */
	if (kaslr_feature_override.val & kaslr_feature_override.mask & 0xf) {
		kaslr_status = KASLR_DISABLED_CMDLINE;
		return 0;
	}

	/*
	 * Mix in any entropy obtainable architecturally if enabled
	 * and supported.
	 */

	if (arch_get_random_seed_long_early(&raw))
		seed ^= raw;

	if (!seed) {
		kaslr_status = KASLR_DISABLED_NO_SEED;
		return 0;
	}

/*
 * IAMROOT, 2021.09.04:
 * - mask (VA 48bit)
 *   (1 << (48 - 2)) - 1 -> kernel 공간의 반에 반절. 64TB를 범위로 하겠다는것.
 *
 *   mask = (64TB 범위) & (2MB algin) = 0x0000_3fff_ffe0_0000
 *        => max 값은 64TB - 2MB. 
 *   offset = 0x0000_2000_0000_0000(32TB) + (seed & 0x0000_3fff_ffe0_0000)
 *   min offset(seed & 0x0000_3fff_ffe0_0000 = 0)
 *              = 32TB + 1
 *   max offset(seed & 0x0000_3fff_ffe0_0000 = 0x0000_3fff_ffe0_0000)
 *              = 32TB + 64TB - 2MB
 *
 *   32TB부터 96TB - 2MB까지의 범위 안에서  2MB 단위의 random offset을
 *   구하겠다는 의미이다.
 */
	/*
	 * OK, so we are proceeding with KASLR enabled. Calculate a suitable
	 * kernel image offset from the seed. Let's place the kernel in the
	 * middle half of the VMALLOC area (VA_BITS_MIN - 2), and stay clear of
	 * the lower and upper quarters to avoid colliding with other
	 * allocations.
	 * Even if we could randomize at page granularity for 16k and 64k pages,
	 * let's always round to 2 MB so we don't interfere with the ability to
	 * map using contiguous PTEs
	 */
	mask = ((1UL << (VA_BITS_MIN - 2)) - 1) & ~(SZ_2M - 1);
	offset = BIT(VA_BITS_MIN - 3) + (seed & mask);

	/* use the top 16 bits to randomize the linear region */
	memstart_offset_seed = seed >> 48;

	if (!IS_ENABLED(CONFIG_KASAN_VMALLOC) &&
	    (IS_ENABLED(CONFIG_KASAN_GENERIC) ||
	     IS_ENABLED(CONFIG_KASAN_SW_TAGS)))
		/*
		 * KASAN without KASAN_VMALLOC does not expect the module region
		 * to intersect the vmalloc region, since shadow memory is
		 * allocated for each module at load time, whereas the vmalloc
		 * region is shadowed by KASAN zero pages. So keep modules
		 * out of the vmalloc region if KASAN is enabled without
		 * KASAN_VMALLOC, and put the kernel well within 4 GB of the
		 * module region.
		 */
		return offset % SZ_2G;

/*
 * IAMROOT, 2021.09.11:
 * - CONFIG_RANDOMIZE_MODULE_REGION_FULL 이 on이면 2G 범위에서 module의 랜덤
 *   위치를 정하고, off라면 MODULES_VSIZE(128MB) 내에서 module의 랜덤위치를
 *   정한다.
 *  */
	if (IS_ENABLED(CONFIG_RANDOMIZE_MODULE_REGION_FULL)) {
		/*
		 * Randomize the module region over a 2 GB window covering the
		 * kernel. This reduces the risk of modules leaking information
		 * about the address of the kernel itself, but results in
		 * branches between modules and the core kernel that are
		 * resolved via PLTs. (Branches between modules will be
		 * resolved normally.)
		 */
		module_range = SZ_2G - (u64)(_end - _stext);
		module_alloc_base = max((u64)_end + offset - SZ_2G,
					(u64)MODULES_VADDR);
	} else {
		/*
		 * Randomize the module region by setting module_alloc_base to
		 * a PAGE_SIZE multiple in the range [_etext - MODULES_VSIZE,
		 * _stext) . This guarantees that the resulting region still
		 * covers [_stext, _etext], and that all relative branches can
		 * be resolved without veneers unless this region is exhausted
		 * and we fall back to a larger 2GB window in module_alloc()
		 * when ARM64_MODULE_PLTS is enabled.
		 */
		module_range = MODULES_VSIZE - (u64)(_etext - _stext);
		module_alloc_base = (u64)_etext + offset - MODULES_VSIZE;
	}

/*
 * IAMROOT, 2021.09.11:
 * - seed값의 2MB이하 값 추출하여 module_range을 곱해서 2MB align을 한 값을
 *   2MB로 나눈다. 그 후 module_alloc_base에 적용하고 PAGE_MASK 시킨다.
 */
	/* use the lower 21 bits to randomize the base of the module region */
	module_alloc_base += (module_range * (seed & ((1 << 21) - 1))) >> 21;
	module_alloc_base &= PAGE_MASK;

	dcache_clean_inval_poc((unsigned long)&module_alloc_base,
			    (unsigned long)&module_alloc_base +
				    sizeof(module_alloc_base));
	dcache_clean_inval_poc((unsigned long)&memstart_offset_seed,
			    (unsigned long)&memstart_offset_seed +
				    sizeof(memstart_offset_seed));

	return offset;
}

static int __init kaslr_init(void)
{
	switch (kaslr_status) {
	case KASLR_ENABLED:
		pr_info("KASLR enabled\n");
		break;
	case KASLR_DISABLED_CMDLINE:
		pr_info("KASLR disabled on command line\n");
		break;
	case KASLR_DISABLED_NO_SEED:
		pr_warn("KASLR disabled due to lack of seed\n");
		break;
	case KASLR_DISABLED_FDT_REMAP:
		pr_warn("KASLR disabled due to FDT remapping failure\n");
		break;
	}

	return 0;
}
core_initcall(kaslr_init)
