/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_CHOPIN_CRUMB_H
#define _LINUX_CHOPIN_CRUMB_H

#include <linux/types.h>

/*
 * Early boot stages, recorded into the pstore block at 0x48090000 that
 * LK re-exports through the expdb partition on the next start.
 *
 * Stage numbering follows the mapping that is usable at that moment,
 * because TTBR0 changes underneath us inside setup_arch():
 *
 *	parse_early_param();
 *	local_daif_restore(DAIF_PROCCTX_NOIRQ);
 *	cpu_uninstall_idmap();		<- TTBR0 -> zero page, idmap gone
 *	arm64_memblock_init();
 *	paging_init();			<- linear map live again
 *
 * So stages up to CRUMB_IDMAP_LAST store at the physical address through
 * the identity map head.S installed, and everything above it goes
 * through __va(). Nothing may be marked in between: that short window
 * has neither mapping, and a fault there would be silent.
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

/* still inside setup_arch(), from paging_init() on -- linear map */
#define STAGE_SA_PAGING		8
#define STAGE_SA_DEVTREE	9
#define STAGE_SA_BOOTMEM	10
#define STAGE_SA_STD_RESOURCES	11
#define STAGE_SA_SMP_CPUS	12
#define STAGE_POST_SETUP_ARCH	13

/* the rest of start_kernel() -- linear map */
#define STAGE_POST_ZONELIST	14
#define STAGE_POST_MM_INIT	15
#define STAGE_POST_CONSOLE	16
#define STAGE_REST_INIT		17

#define STAGE_BASIC_SETUP	18
#define STAGE_INITCALLS_ENTER	19

/* stage = STAGE_INITCALL_LEVEL + initcall level (0..7) */
#define STAGE_INITCALL_LEVEL	20

#ifdef CONFIG_CHOPIN_CRUMB
void chopin_crumb_mark(u32 stage);
#else
static inline void chopin_crumb_mark(u32 stage) { }
#endif

#endif /* _LINUX_CHOPIN_CRUMB_H */
