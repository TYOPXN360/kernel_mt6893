/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_CHOPIN_CRUMB_H
#define _LINUX_CHOPIN_CRUMB_H

#include <linux/types.h>

/*
 * Early boot stages, recorded into the pstore block at 0x48090000 that
 * LK re-exports through the expdb partition on the next start.
 *
 * Stage numbers are in execution order and the value also selects the
 * channel used to reach the block, because what is reachable changes
 * under us twice, both inside setup_arch():
 *
 *	parse_early_param();
 *	local_daif_restore(DAIF_PROCCTX_NOIRQ);
 *	cpu_uninstall_idmap();	<- TTBR0 -> zero page; identity map gone
 *	arm64_memblock_init();
 *	paging_init();		<- linear map appears part-way through
 *
 *  stage <= CRUMB_IDMAP_LAST
 *	identity map installed by head.S, covering 0x40000000..0x48200000.
 *
 *  stage > CRUMB_IDMAP_LAST
 *	the fixmap, via early_memremap(). That resolves in TTBR1 and does
 *	not depend on the linear map covering the block.
 *
 * __va() is deliberately not used: whether the linear map covers the
 * block depends on memblock not having it marked no-map, and a probe
 * built on that assumption fails by faulting at the very spot being
 * investigated. Stage numbers run in execution order, so a dump reads
 * top to bottom.
 *
 * The one exception is cpu_replace_ttbr1(), which is bracketed by two
 * mappings changing under it. Its marks select the channel explicitly;
 * see STAGE_TTBR1_* below.
 */
#define CRUMB_IDMAP_LAST	7

/* start_kernel(), before setup_arch() -- identity map */
#define STAGE_START_KERNEL	1
#define STAGE_PRE_SETUP_ARCH	2

/* inside setup_arch(), up to cpu_uninstall_idmap() -- identity map */
#define STAGE_SA_ENTER		3
#define STAGE_SA_IOREMAP	4
#define STAGE_SA_MACHINE_FDT	5
#define STAGE_SA_EARLY_PARAM	6
#define STAGE_SA_PRE_UNINSTALL	7

/* arm64_memblock_init() and friends -- fixmap */
#define STAGE_SA_POST_UNINSTALL	8
#define STAGE_SA_MEMBLOCK_ENTER	9
#define STAGE_MB_FDT_ENFORCE	10
#define STAGE_MB_MEMSTART	11
#define STAGE_MB_PRE_RESERVED	12
#define STAGE_MB_POST_RESERVED	13
#define STAGE_MB_PRE_CMA	14
#define STAGE_MB_POST_CMA	15
#define STAGE_SA_MEMBLOCK_DONE	16

/* paging_init() internals -- still fixmap, the linear map is not up yet */
#define STAGE_PI_ENTER		17
#define STAGE_PI_ALLOC		18
#define STAGE_PI_SET_FIXMAP	19
#define STAGE_PI_MAP_KERNEL	20
#define STAGE_PI_MAP_MEM	21

/*
 * Inside cpu_replace_ttbr1(). Every dump so far stopped between 21 and
 * 22, and this is the only code in that gap, so it is opened up step by
 * step. The helper is expanded in paging_init() rather than called.
 *
 * From STAGE_TTBR1_IDMAP_IN onward these are written through the
 * identity map explicitly, not through whatever the generic mark would
 * pick. cpu_install_idmap() has just pointed TTBR0 at idmap_pg_dir,
 * which head.S extends to cover 0x48090000, so that path is known live.
 * TTBR1 simultaneously points at the new pgd, which has no fixmap
 * entries yet, so the fixmap is exactly what cannot be trusted there.
 */
#define STAGE_TTBR1_ENTER	22	/* before cpu_install_idmap() */
#define STAGE_TTBR1_IDMAP_IN	23	/* identity map live from here */
#define STAGE_TTBR1_REPLACED	24	/* TTBR1 now the new pgd */
#define STAGE_TTBR1_IDMAP_OUT	25	/* idmap gone again */

/* paging_init() internals, back on the fixmap */
#define STAGE_PI_TTBR1_TEMP	26
#define STAGE_PI_COPY_PGD	27
#define STAGE_PI_TTBR1_SWAPPER	28
#define STAGE_PI_CLEAR_FIXMAP	29
#define STAGE_PI_DONE		30

/* the rest of setup_arch() */
#define STAGE_SA_PAGING		31
#define STAGE_SA_DEVTREE	32
#define STAGE_SA_BOOTMEM	33
#define STAGE_SA_STD_RESOURCES	34
#define STAGE_SA_SMP_CPUS	35
#define STAGE_POST_SETUP_ARCH	36

/* the rest of start_kernel() */
#define STAGE_POST_ZONELIST	37
#define STAGE_POST_MM_INIT	38
#define STAGE_POST_CONSOLE	39
#define STAGE_REST_INIT		40

#define STAGE_BASIC_SETUP	41
#define STAGE_INITCALLS_ENTER	42

/* stage = STAGE_INITCALL_LEVEL + initcall level (0..7) */
#define STAGE_INITCALL_LEVEL	44

#ifdef CONFIG_CHOPIN_CRUMB
void chopin_crumb_mark(u32 stage);
/*
 * Same, but the caller states whether the identity map is live. Needed
 * only inside cpu_replace_ttbr1(); see chopin_crumb.c.
 */
void chopin_crumb_mark_idmap(u32 stage, bool force_idmap);
#else
static inline void chopin_crumb_mark(u32 stage) { }
static inline void chopin_crumb_mark_idmap(u32 stage, bool force_idmap) { }
#endif

#endif /* _LINUX_CHOPIN_CRUMB_H */
