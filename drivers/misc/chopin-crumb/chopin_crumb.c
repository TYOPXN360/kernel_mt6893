// SPDX-License-Identifier: GPL-2.0
/*
 * Chopin early-boot breadcrumbs.
 *
 * GenieZone hands the 4.19 kernel control and then nothing else is
 * observable: LK appends mtk_printk_ctrl.disable_uart=1 to the command
 * line, this tree has no minirdump driver so the oops partition stays
 * empty, and GenieZone only logs registered TA service calls.
 *
 * What does survive is the pstore block. LK exports it verbatim into the
 * expdb partition on every boot:
 *
 *	kedump: read SYS_PSTORE_RAW from offset 0x2800 size 0xe0000
 *
 * so anything the kernel leaves at 0x48090000 comes back out on the next
 * start. head.S owns +0x800 through +0x91f; this file starts at +0x920.
 *
 * Earlier attempts at this produced nothing, and the reasons are worth
 * keeping:
 *
 *  1. Writing to LK's own log_store buffer at 0x7ffbf000. That region is
 *     not part of the exported pstore image, so it is never visible.
 *
 *  2. Using memremap(MEMREMAP_WB). The first call site is
 *     STAGE_START_KERNEL at the very top of start_kernel(), long before
 *     setup_arch() populates the paging structures and long before the
 *     vmalloc and slab allocators exist. try_ram_remap() therefore fails
 *     its pfn_valid() check and the ioremap() fallback cannot allocate,
 *     so the mapping stayed NULL through every stage and only the
 *     pr_err() fallback ran -- into a console nobody can read.
 *
 *  3. Storing without cleaning the cache. dsb orders stores, it does not
 *     push them to the point of coherency; with caches on, the values
 *     can still be dirty in L1/L2 when LK reads the block back. The
 *     head.S probes needed an explicit dc cvac for exactly this reason,
 *     and the two dumps show it: without the clean, probe 17 and every
 *     probe after it disappeared.
 *
 * Reachability of the pstore block changes twice, both times inside
 * setup_arch():
 *
 *	parse_early_param();
 *	local_daif_restore(DAIF_PROCCTX_NOIRQ);
 *	cpu_uninstall_idmap();	<- TTBR0 -> zero page; identity map gone
 *	arm64_memblock_init();
 *	paging_init();		<- linear map live again
 *
 * and the kernel currently stops inside that second interval, so all
 * three regimes have to be writable or the mechanism cannot see the
 * failure it was built for. A record travels by whichever channel is
 * available:
 *
 *	stage <= CRUMB_IDMAP_LAST
 *		identity map installed by head.S, which covers
 *		0x40000000..0x48200000. head.S probes 6-9 store to this very
 *		block that way and land in DRAM, which proves the path.
 *
 *	stage > CRUMB_IDMAP_LAST
 *		the fixmap, via early_memremap(). That resolves in TTBR1,
 *		which is live throughout, and draws on the slots
 *		early_fixmap_init() and early_ioremap_init() set aside near
 *		the top of setup_arch(). early_ioremap_reset() runs later,
 *		but early_memremap() keeps working past it because it falls
 *		back to __late_set_fixmap internally, so one channel covers
 *		everything after the identity map is gone.
 *
 * __va() is deliberately not used anywhere in this file. It depends on
 * the linear map covering the block, which in turn depends on memblock
 * not having the block marked no-map, and an observer must not be built
 * on an assumption whose failure mode is a fault at the exact spot being
 * investigated. Four dumps in a row stopped on the very first stage that
 * would have used __va(), which is what a bad assumption there looks
 * like: indistinguishable from the hang being chased.
 *
 * Nothing here may fault: the whole point is to observe a boot that dies
 * silently, and a fault in the observer would be indistinguishable from
 * the failure being chased. The fixmap path therefore falls back to a
 * plain .bss stash if early_memremap() refuses, and the .bss stash is
 * replayed into the block by the first stage that has a proper mapping
 * again. A stage that only ever reached .bss is invisible in the dump,
 * which is a poor outcome, but still far better than a fault that cannot
 * report its own cause.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/chopin_crumb.h>
#include <linux/string.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <asm/barrier.h>
#include <asm/cacheflush.h>
#include <asm/memory.h>

/*
 * Deliberately unlike the head.S probes, which use 0x44424b41 through
 * 0x44424b51. A shared value would make a C-side record
 * indistinguishable from an assembly probe in the dump.
 */
#define CRUMB_MAGIC		0x44424343u	/* "CCBD" */

/*
 * From the running 4.14 kernel, and confirmed in the LK command line
 * captured in expdb:
 *	ramoops.mem_address=0x48090000 ramoops.mem_size=0xe0000
 * MTK carves the first 0x800 bytes into a partition table and head.S
 * then owns +0x800..+0x91f (its last probe is a single word at +0x910),
 * so the C-side table begins at +0x920.
 */
#define CRUMB_TABLE		(0x48090000UL + 0x920UL)

/*
 * A record is 24 bytes, not 16: four u32 fields occupy the first 16 and
 * sp needs 8-byte alignment, so sizeof() rounds the struct up. Indexing
 * goes through tbl[stage] so the compiler applies the real size.
 */
#define CRUMB_MAX		48

/* Which channel a record went through. */
#define CRUMB_FLAG_STASH	0x10	/* .bss only, never reached DRAM */
#define CRUMB_FLAG_IDMAP	1
#define CRUMB_FLAG_FIXMAP	3
#define CRUMB_FLAG_LINEAR	2

struct chopin_crumb {
	u32 magic;
	u32 stage;
	u32 reached;	/* highest stage seen so far */
	u32 flags;	/* which mapping the record went through */
	u64 sp;
};

/* Staging area, in .bss, mapped by TTBR1 for the whole of early boot. */
static struct chopin_crumb crumb_stash[CRUMB_MAX];
static u32 crumb_stash_cnt;
static u32 crumb_reached;

/*
 * dsb orders the stores; only an explicit clean pushes them to the point
 * of coherency that LK reads from. Clean the whole table rather than the
 * single record being written, because the table is written with a
 * scatter of stores and a line-at-a-time clean would leave the
 * neighbours dirty in cache.
 */
static void crumb_flush(void *addr, size_t len)
{
	__clean_dcache_area_poc(addr, len);
	dsb(sy);
}

static void crumb_stash_one(struct chopin_crumb *c)
{
	if (crumb_stash_cnt < CRUMB_MAX)
		crumb_stash[crumb_stash_cnt++] = *c;
}

/* Replay everything staged so far, plus the current record. */
static void crumb_replay(struct chopin_crumb *tbl)
{
	u32 i;

	for (i = 0; i < crumb_stash_cnt; i++)
		tbl[i] = crumb_stash[i];

	crumb_flush(tbl, CRUMB_MAX * sizeof(struct chopin_crumb));
}

void chopin_crumb_mark(u32 stage)
{
	struct chopin_crumb c;
	void *tbl = NULL;
	void *fix = NULL;
	unsigned long len = 0;
	u64 sp = (u64)(unsigned long)__builtin_frame_address(0);

	if (stage > crumb_reached)
		crumb_reached = stage;

	c.magic = CRUMB_MAGIC;
	c.stage = stage;
	c.reached = crumb_reached;
	c.sp = sp;

	if (stage <= CRUMB_IDMAP_LAST) {
		c.flags = CRUMB_FLAG_IDMAP;
		tbl = (void *)(unsigned long)CRUMB_TABLE;
	} else {
		/*
		 * Past the identity map, reach the block through the fixmap.
		 * It resolves in TTBR1 and does not depend on whether the
		 * linear map happens to cover this block.
		 *
		 * __va() is deliberately not used. Whether it works depends
		 * on memblock not having the block marked no-map, and that
		 * assumption must not live in the observer: if it is wrong
		 * the probe faults and the boot stops right there, which
		 * looks exactly like the hang that was being chased. Four
		 * dumps in a row stopped on the step where this function
		 * first called __va(), which is what that failure mode looks
		 * like from the outside.
		 *
		 * The mapping is taken and released per call. early_memremap()
		 * keeps working after early_ioremap_reset() -- it switches to
		 * __late_set_fixmap internally -- but the slots it hands out
		 * are reused, so a cached pointer would go stale.
		 */
		unsigned long start = CRUMB_TABLE & PAGE_MASK;

		len = PAGE_ALIGN(CRUMB_TABLE + CRUMB_MAX *
				 sizeof(struct chopin_crumb) - start);
		fix = early_memremap(start, len);
		if (fix) {
			c.flags = CRUMB_FLAG_FIXMAP;
			tbl = (char *)fix + (CRUMB_TABLE - start);
		}
	}

	if (!tbl) {
		/*
		 * No channel is usable right now. Record it in .bss so the
		 * next stage that can write replays it, and return without
		 * touching anything that might fault.
		 */
		c.flags = CRUMB_FLAG_STASH;
		crumb_stash_one(&c);
		return;
	}

	crumb_stash_one(&c);
	crumb_replay(tbl);

	/*
	 * Give the slot back so the pool is not exhausted over the many
	 * stages that run after this point.
	 */
	if (fix)
		early_iounmap(fix, len);
}
