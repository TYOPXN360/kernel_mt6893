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
 * Three earlier attempts at this produced nothing, and the reasons are
 * worth keeping:
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
 * The channel is now: store through a mapping that is known to exist at
 * that moment, then clean to PoC. Which mapping that is depends on
 * setup_arch(), which tears the identity map down partway through:
 *
 *	parse_early_param();
 *	local_daif_restore(DAIF_PROCCTX_NOIRQ);
 *	cpu_uninstall_idmap();		<- TTBR0 goes to the zero page
 *	arm64_memblock_init();
 *	paging_init();			<- linear map becomes live
 *
 *  - Before that (stages 1 and 2, both inside start_kernel before
 *    setup_arch()) TTBR0 still holds the identity map head.S installed,
 *    which covers 0x40000000..0x48200000. The block is written at its own
 *    physical address; head.S probes 6-9 store to this very block that
 *    way and land in DRAM, which is what proves the path.
 *
 *  - From STAGE_POST_SETUP_ARCH onward the linear map is the one to use.
 *    The block sits inside the /memory node (0x40000000..0x7e605000) and
 *    no DTB declares it reserved or no-map -- it is LK that carves it out
 *    and passes the address on the command line -- so map_mem() covers it
 *    with PAGE_KERNEL and __va() resolves.
 *
 * Nothing here may fault: the whole point is to observe a boot that dies
 * silently, and a fault in the observer would be indistinguishable from
 * the failure being chased. Hence one boundary, chosen once.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/chopin_crumb.h>
#include <asm/barrier.h>
#include <asm/cacheflush.h>
#include <asm/memory.h>

#define CRUMB_MAGIC		0x44424343u	/* "CCBD" */
/*
 * Deliberately unlike the head.S probes, which use 0x44424b41
 * through 0x44424b51. A shared value would make a C-side record
 * indistinguishable from an assembly probe in the dump.
 */

/*
 * From the running 4.14 kernel, and confirmed in the LK command line
 * captured in expdb:
 *	ramoops.mem_address=0x48090000 ramoops.mem_size=0xe0000
 * MTK carves the first 0x800 bytes into a partition table and head.S
 * then owns +0x800..+0x91f (its last probe is a single word at +0x910),
 * so the C-side table begins at +0x920 and runs for 0x300 bytes.
 */
#define CRUMB_RAMOOPS		0x48090000UL
#define CRUMB_TABLE		(CRUMB_RAMOOPS + 0x920UL)
#define CRUMB_MAX		32

#define CRUMB_FLAG_IDMAP	1
#define CRUMB_FLAG_LINEAR	2

struct chopin_crumb {
	u32 magic;
	u32 stage;
	u32 reached;	/* highest stage seen so far */
	u32 flags;	/* which mapping the record went through */
	u64 sp;
};

static u32 crumb_reached;

/*
 * dsb orders the stores; only an explicit clean pushes them to the point
 * of coherency that LK reads from. Clean the whole table rather than the
 * single record being written, because the table is written with a
 * scatter of stores and a line-at-a-time clean would leave the
 * neighbours dirty in cache.
 *
 * A record is 24 bytes, not 16: four u32 fields occupy the first 16 and
 * sp needs 8-byte alignment, so sizeof() rounds the struct up. The table
 * therefore spans 32 * 24 = 768 = 0x300 bytes, from 0x48090920 to
 * 0x48090c20. Indexing goes through tbl[stage] so the compiler applies
 * the real size rather than the number written in a comment.
 */
static void crumb_flush(void *addr, size_t len)
{
	__clean_dcache_area_poc(addr, len);
	dsb(sy);
}

void chopin_crumb_mark(u32 stage)
{
	struct chopin_crumb *tbl;
	struct chopin_crumb c;
	bool linear = stage >= STAGE_POST_SETUP_ARCH;
	u64 sp = (u64)(unsigned long)__builtin_frame_address(0);

	tbl = linear ? __va(CRUMB_TABLE)
		     : (void *)(unsigned long)CRUMB_TABLE;

	if (stage > crumb_reached)
		crumb_reached = stage;

	c.magic = CRUMB_MAGIC;
	c.stage = stage;
	c.reached = crumb_reached;
	c.flags = linear ? CRUMB_FLAG_LINEAR : CRUMB_FLAG_IDMAP;
	c.sp = sp;

	/* entry 0 always carries the most recent mark */
	tbl[0] = c;
	if (stage < CRUMB_MAX)
		tbl[stage] = c;

	crumb_flush(tbl, CRUMB_MAX * sizeof(struct chopin_crumb));
}
