/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Chopin early-boot breadcrumbs.
 *
 * GenieZone hands the 4.19 kernel control with "boot linux-64 via EL2"
 * and then nothing else is observable. LK appends
 * mtk_printk_ctrl.disable_uart=1 to the command line after whatever the
 * boot image supplies, this tree has no minirdump driver so oops stays
 * empty, and pstore/ramoops lands in RAM that nothing can read back.
 * GenieZone only logs registered TA service calls, so a bare SMC from
 * head.S is dropped silently as well.
 *
 * What does survive is LK's log_store. LK maps the expdb partition at
 * 0x7ffbf000 in DRAM and copies the leading ~127KB of that buffer back
 * into the expdb partition on every boot, so anything the kernel leaves
 * inside it comes back out on the next start. The working 4.14 kernel
 * reports MemTotal 5598444 kB while the DTB only declares 998 MB, which
 * proves LK widens /memory before the jump, so 0x7ffbf000 is mappable.
 *
 * Each stage is a 16-byte record. The whole table is written at every
 * stage rather than appended, so a later stage overwriting an earlier
 * one still leaves the highest stage reached intact.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/slab.h>

#define CRUMB_MAGIC		0x44424b43u	/* "CBKD" */
#define CRUMB_LOG_STORE		0x7ffbf000UL
#define CRUMB_LEN		0x40000UL
/* Stay well inside the ~127KB LK actually copies back. */
#define CRUMB_TABLE		(CRUMB_LOG_STORE + 0x1000UL)
#define CRUMB_MAX		32

#define STAGE_START_KERNEL	1
#define STAGE_PRE_SETUP_ARCH	2
#define STAGE_POST_SETUP_ARCH	3
#define STAGE_BANNER		4
#define STAGE_POST_ZONELIST	5
#define STAGE_PRE_INITCALL	6
#define STAGE_POST_INITCALL	7
#define STAGE_REST_INIT		8
#define STAGE_SMP_INIT		9

struct chopin_crumb {
	u32 magic;
	u32 stage;
	u32 reached;	/* highest stage seen so far */
	u32 flags;
	u64 sp;
};

static struct chopin_crumb *crumb_base;
static u32 crumb_reached;

void __init chopin_crumb_init(void)
{
	void *p;

	if (crumb_base)
		return;

	/*
	 * MEMREMAP_WB gives a cached writable mapping. If the address is
	 * not system memory this returns NULL and every later stage just
	 * records "no breadcrumb" rather than faulting.
	 */
	p = memremap(CRUMB_TABLE, CRUMB_LEN, MEMREMAP_WB);
	if (!p)
		p = memremap(CRUMB_TABLE, CRUMB_LEN, MEMREMAP_WT);
	if (!p) {
		pr_err("chopin-crumb: memremap(0x%lx) failed\n",
		       (unsigned long)CRUMB_TABLE);
		return;
	}
	crumb_base = p;
	pr_err("chopin-crumb: mapped 0x%lx\n", (unsigned long)CRUMB_TABLE);
}

void __init chopin_crumb_mark(u32 stage)
{
	struct chopin_crumb c;
	u64 sp = (u64)(unsigned long)__builtin_frame_address(0);

	/* memremap() needs the allocator, which is up by then. */
	if (!crumb_base && stage != STAGE_START_KERNEL)
		chopin_crumb_init();

	if (stage > crumb_reached)
		crumb_reached = stage;

	if (!crumb_base) {
		/* Fall back to the one channel we can still reach. */
		pr_err("chopin-crumb: reached stage %u sp=%016llx\n",
		       stage, sp);
		return;
	}

	c.magic = CRUMB_MAGIC;
	c.stage = stage;
	c.reached = crumb_reached;
	c.flags = 1;
	c.sp = sp;

	memcpy(crumb_base, &c, sizeof(c));

	/* Leave a trail of the last few stages for the dump. */
	if (stage < CRUMB_MAX)
		memcpy((char *)crumb_base + sizeof(c) * (stage + 1),
		       &c, sizeof(c));
}
