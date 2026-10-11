/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_CHOPIN_CRUMB_H
#define _LINUX_CHOPIN_CRUMB_H

#include <linux/types.h>

/*
 * Early boot stages, recorded into the pstore block at 0x48090000 that
 * LK re-exports through the expdb partition on the next start.
 *
 * Stages 1 and 2 are marked from start_kernel() before setup_arch(),
 * while the identity map head.S installed is still live on TTBR0.
 * setup_arch() calls cpu_uninstall_idmap() and then paging_init(), so
 * STAGE_POST_SETUP_ARCH and everything after it goes through the linear
 * map instead. See chopin_crumb.c.
 */
#define STAGE_START_KERNEL	1
#define STAGE_PRE_SETUP_ARCH	2
#define STAGE_POST_SETUP_ARCH	3
#define STAGE_POST_ZONELIST	4
#define STAGE_POST_MM_INIT	5
#define STAGE_POST_CONSOLE	6
#define STAGE_REST_INIT		7

#define STAGE_BASIC_SETUP	8
#define STAGE_INITCALLS_ENTER	9

/* stage = STAGE_INITCALL_LEVEL + initcall level (0..7) */
#define STAGE_INITCALL_LEVEL	20

#ifdef CONFIG_CHOPIN_CRUMB
void chopin_crumb_mark(u32 stage);
#else
static inline void chopin_crumb_mark(u32 stage) { }
#endif

#endif /* _LINUX_CHOPIN_CRUMB_H */
