/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_CHOPIN_CRUMB_H
#define _LINUX_CHOPIN_CRUMB_H

#include <linux/types.h>

/*
 * Early boot stages, recorded into LK's log_store buffer in DRAM so they
 * survive into the expdb partition on the next start. The working 4.14
 * kernel reports MemTotal 5598444 kB while the DTB only declares 998 MB,
 * which shows LK widens /memory before handing over.
 */
#define STAGE_START_KERNEL	1
#define STAGE_PRE_SETUP_ARCH	2
#define STAGE_POST_SETUP_ARCH	3
#define STAGE_BANNER		4
#define STAGE_POST_ZONELIST	5
#define STAGE_PRE_INITCALL	6
#define STAGE_POST_INITCALL	7
#define STAGE_REST_INIT		8
#define STAGE_SMP_INIT		9

#ifdef CONFIG_CHOPIN_CRUMB
void __init chopin_crumb_init(void);
void __init chopin_crumb_mark(u32 stage);
#else
static inline void chopin_crumb_init(void) { }
static inline void chopin_crumb_mark(u32 stage) { }
#endif

#endif /* _LINUX_CHOPIN_CRUMB_H */
