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
 *  CRUMB_IDMAP_LAST < stage < CRUMB_LINEAR_FIRST
 *	neither the identity map nor the linear map reaches the block, so
 *	it is mapped through the fixmap with early_memremap(), which
 *	resolves in TTBR1 and stays live until early_ioremap_reset().
 *
 *  stage >= CRUMB_LINEAR_FIRST
 *	the linear map exists, so __va() works.
 *
 * CRUMB_LINEAR_FIRST is the first stage that runs after map_mem() has
 * built the linear map. Marking anything earlier that way would fault,
 * which is why the paging_init() internals that run before it are
 * numbered below it: the marks are emitted in execution order, so a
 * dump can be read top to bottom.
 */
#define CRUMB_IDMAP_LAST	7
#define CRUMB_LINEAR_FIRST	22

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

/* paging_init() internals -- linear map exists from map_mem() onward */
#define STAGE_PI_TTBR1_TEMP	22
#define STAGE_PI_COPY_PGD	23
#define STAGE_PI_TTBR1_SWAPPER	24
#define STAGE_PI_CLEAR_FIXMAP	25
#define STAGE_PI_DONE		26

/* the rest of setup_arch() -- linear map */
#define STAGE_SA_PAGING		27
#define STAGE_SA_DEVTREE	28
#define STAGE_SA_BOOTMEM	29
#define STAGE_SA_STD_RESOURCES	30
#define STAGE_SA_SMP_CPUS	31
#define STAGE_POST_SETUP_ARCH	32

/* the rest of start_kernel() -- linear map */
#define STAGE_POST_ZONELIST	33
#define STAGE_POST_MM_INIT	34
#define STAGE_POST_CONSOLE	35
#define STAGE_REST_INIT		36

#define STAGE_BASIC_SETUP	37
#define STAGE_INITCALLS_ENTER	38

/* stage = STAGE_INITCALL_LEVEL + initcall level (0..7) */
#define STAGE_INITCALL_LEVEL	40

#ifdef CONFIG_CHOPIN_CRUMB
void chopin_crumb_mark(u32 stage);
#else
static inline void chopin_crumb_mark(u32 stage) { }
#endif

#endif /* _LINUX_CHOPIN_CRUMB_H */
