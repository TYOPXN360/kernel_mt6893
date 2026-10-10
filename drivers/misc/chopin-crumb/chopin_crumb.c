/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Chopin early-boot breadcrumbs.
 *
 * GenieZone hands the 4.19 kernel control with "boot linux-64 via EL2"
 * and then nothing else is observable. LK appends
 * mtk_printk_ctrl.disable_uart=1 to the command line after whatever the
 * boot image supplies, and this tree has no minirdump driver so the oops
 * partition stays empty even on panic.
 * GenieZone only logs registered TA service calls, so a bare SMC from
 * head.S is dropped silently as well.
 *
 * What does survive is ramoops. LK exports it verbatim into the expdb
 * partition on every boot, visible as
 *   kedump add: SYS_PSTORE_RAW[1] e0000/e0000@2800
 * so anything the kernel leaves in that RAM comes back out on the next
 * start. A first attempt wrote to LK's own log_store buffer at 0x7ffbf000
 * instead and produced nothing, which is what this rewrite fixes.
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
/*
 * ramoops. LK exports it verbatim into the expdb partition as
 * "kedump add: SYS_PSTORE_RAW", which is a far better channel than LK's
 * own log_store buffer: 0x48090000 sits inside /memory as declared by
 * the DTB, so memremap() cannot refuse it, and the export does not depend
 * on how much of log_store LK happens to copy back.
 *
 * From the running 4.14 kernel:
 *   ramoops.mem_address=0x48090000  ramoops.mem_size=0xe0000
 *
 * MTK carves the first 0x800 bytes into a partition table
 * (SYS_RAMCONSOLE / SYS_SSPM / SYS_MCUP / ZAAE_LOG, seen in expdb), so
 * start past that. console_size is 0x40000 and pmsg_size 0x10000, both
 * also inside this range, so stay in the gap below them.
 *
 * The C-side table sits at +0x8a0. head.S writes its own single-word
 * probes at +0x800 through +0x880 and they must not overlap, otherwise
 * reaching start_kernel would erase the record of having got there.
 */
#define CRUMB_RAMOOPS		0x48090000UL
#define CRUMB_LEN		0x1000UL
#define CRUMB_TABLE		(CRUMB_RAMOOPS + 0x8a0UL)
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
	 * MEMREMAP_WB gives a cached writable mapping. ramoops is ordinary
	 * RAM inside /memory, so this should always succeed; if it does
	 * not, every later stage records "no breadcrumb" rather than
	 * faulting, since a fault here would hide the failure we are
	 * trying to observe.
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
