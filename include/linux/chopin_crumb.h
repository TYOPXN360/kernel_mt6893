/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_CHOPIN_CRUMB_H
#define _LINUX_CHOPIN_CRUMB_H

#include <linux/types.h>

/*
 * Early boot stages, recorded into the pstore block at 0x48090000 that
 * LK re-exports through the expdb partition on the next start.
 *
 * Stage numbering follows the channel that can reach that block at the
 * moment, because TTBR0 changes underneath us inside setup_arch():
 *
 *	parse_early_param();
 *	local_daif_restore(DAIF_PROCCTX_NOIRQ);
 *	cpu_uninstall_idmap();	<- TTBR0 -> zero page; identity map gone
 *	arm64_memblock_init();
 *	paging_init();		<- linear map live again
 *
 *  stage <= CRUMB_IDMAP_LAST
 *	identity map installed by head.S, which covers
 *	0x40000000..0x48200000.
 *
 *  CRUMB_IDMAP_LAST < stage < STAGE_SA_PAGING
 *	neither the identity map nor the linear map reaches the block.
 *	The fixmap is used instead, via early_memremap(), which resolves
 *	in TTBR1 and stays live until early_ioremap_reset().
 *
 *  stage >= STAGE_SA_PAGING
 *	the linear map built by paging_init() is live, so __va() works.
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

/* inside setup_arch(), after the idmap is gone -- fixmap */
#define STAGE_SA_POST_UNINSTALL	8
#define STAGE_SA_MEMBLOCK_ENTER	9
#define STAGE_MB_FDT_ENFORCE	10
#define STAGE_MB_MEMSTART	11
#define STAGE_MB_PRE_RESERVED	12
#define STAGE_MB_POST_RESERVED	13
#define STAGE_MB_PRE_CMA	14
#define STAGE_MB_POST_CMA	15
#define STAGE_SA_MEMBLOCK_DONE	16

/* inside setup_arch(), linear map live again */
#define STAGE_SA_PAGING		17
#define STAGE_SA_DEVTREE	18
#define STAGE_SA_BOOTMEM	19
#define STAGE_SA_STD_RESOURCES	20
#define STAGE_SA_SMP_CPUS	21
#define STAGE_POST_SETUP_ARCH	22

/* the rest of start_kernel() -- linear map */
#define STAGE_POST_ZONELIST	23
#define STAGE_POST_MM_INIT	24
#define STAGE_POST_CONSOLE	25
#define STAGE_REST_INIT		26

#define STAGE_BASIC_SETUP	27
#define STAGE_INITCALLS_ENTER	28

/* stage = STAGE_INITCALL_LEVEL + initcall level (0..7) */
#define STAGE_INITCALL_LEVEL	30

#ifdef CONFIG_CHOPIN_CRUMB
void chopin_crumb_mark(u32 stage);
#else
static inline void chopin_crumb_mark(u32 stage) { }
#endif

#endif /* _LINUX_CHOPIN_CRUMB_H */
