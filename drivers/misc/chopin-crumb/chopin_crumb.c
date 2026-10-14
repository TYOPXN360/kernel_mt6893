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
 * Reaching that block depends on a mapping that covers it, and the
 * mappings come and go, so each record is written twice:
 *
 *  1. Always into a .bss array. That array lives inside the kernel image,
 *     which TTBR1 maps from _text to _end for the whole of early boot --
 *     through swapper_pg_dir at first, then through the paging_init()
 *     tables which carry the same image mappings over. Writing it cannot
 *     fault, whatever else is going on.
 *
 *  2. Into the pstore block, but only when a mapping for it is known to
 *     be available. Failures here are silent by design: the .bss copy
 *     still holds the record and a later flusher puts it in the block.
 *
 * The flusher runs from an initcall, which is late enough that every
 * mapping is settled, and it replays the entire .bss array. It reaches
 * the block with ioremap(), which builds its own page tables and so does
 * not depend on the linear map covering the block -- the same way
 * ramoops reaches it. A dump only exists after a reset, so the block is
 * the only place that survives.
 *
 * History, kept because each entry cost a boot cycle:
 *
 *  1. Writing to LK's own log_store buffer at 0x7ffbf000 produced
 *     nothing: that region is not part of the exported pstore image.
 *
 *  2. Using memremap(MEMREMAP_WB) produced nothing either. The first
 *     call site is STAGE_START_KERNEL at the top of start_kernel(),
 *     before setup_arch() has populated the paging structures and before
 *     the vmalloc and slab allocators exist, so try_ram_remap() fails
 *     its pfn_valid() check and the ioremap() fallback cannot allocate.
 *
 *  3. Storing without cleaning the cache lost everything after the MMU
 *     came on. dsb orders stores, it does not push them to the point of
 *     coherency, and the head.S probes show it plainly: without dc cvac,
 *     probe 17 and every probe after it disappeared.
 *
 *  4. Using __va() for the later stages stopped the boot at the first
 *     stage that used it. The kernel only reached the vibrator again once
 *     __va() was removed, so that path was provably fatal rather than
 *     merely unreliable.
 *
 * Lesson from 4: an observer must not assume a mapping exists. Where the
 * assumption is wrong, the probe faults and the boot stops at the probe,
 * which is indistinguishable in the dump from the hang being chased.
 * Hence the unconditional .bss write plus best-effort block writes.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/chopin_crumb.h>
#include <linux/string.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <asm/barrier.h>
#include <asm/cacheflush.h>

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
 * The boot counter, deliberately outside the table so that wiping the
 * table cannot wipe it too. It is the one field that is written by a
 * boot and read by the next one, which is what makes a stale table
 * distinguishable from a fresh one: a dump whose records all carry an
 * older seq than the counter means this boot never reached stage 1 and
 * every record in the block is left over from the previous boot.
 *
 * Five dumps in a row produced a byte-identical table across three
 * different kernel versions, which is exactly what stale data looks
 * like -- so until this counter existed, "the kernel dies at stage 21"
 * and "the kernel never wrote anything this time" were the same
 * observation.
 */
#define CRUMB_SEQ		(0x48090000UL + 0x1600UL)

/*
 * A record is 32 bytes: six u32 fields then sp, which needs 8-byte
 * alignment, so one u32 of padding. Indexing goes through tbl[i] rather
 * than pointer arithmetic on a byte count, so the compiler applies the
 * real size everywhere.
 */
#define CRUMB_MAX		48

/* Which channel a record was written through. */
#define CRUMB_FLAG_IDMAP	1
#define CRUMB_FLAG_FIXMAP	3
#define CRUMB_FLAG_BSS		0x20	/* only the .bss copy exists */

struct chopin_crumb {
	u32 magic;
	u32 seq;	/* which boot wrote this record */
	u32 stage;
	u32 reached;	/* highest stage seen so far */
	u32 flags;	/* which mapping the record went through */
	u32 pad;
	u64 sp;
};

/*
 * The unconditional copy. In .bss, inside the kernel image, mapped by
 * TTBR1 for the whole of early boot.
 */
static struct chopin_crumb crumb_stash[CRUMB_MAX];
static u32 crumb_stash_cnt;
static u32 crumb_reached;
static u32 crumb_boot_seq;

/*
 * dsb orders the stores; only an explicit clean pushes them to the point
 * of coherency that LK reads from. Clean the whole block rather than the
 * single record being written, because the table is written with a
 * scatter of stores and a line-at-a-time clean would leave the
 * neighbours dirty in cache.
 */
static void crumb_flush(void *addr, size_t len)
{
	__clean_dcache_area_poc(addr, len);
	dsb(sy);
}

/*
 * Bump the boot counter and wipe the table. Called once, from stage 1,
 * which runs at the top of start_kernel() while the identity map that
 * head.S installed still covers the block -- so this touches the block
 * with plain stores, no fixmap and no allocator.
 *
 * The wipe is what makes the next dump readable: a boot that dies
 * between here and the first record leaves an all-zero table, which
 * cannot be confused with the previous boot's leftovers. The counter is
 * kept outside the wiped range for the same reason.
 *
 * A counter that reads as 0 (fresh RAM, cold boot) or as something
 * implausible (random RAM contents) is reset rather than incremented, so
 * the value stays a small monotonic number instead of drifting to
 * noise.
 */
static void crumb_boot_begin(void)
{
	u32 *seq = (u32 *)(unsigned long)CRUMB_SEQ;
	void *tbl = (void *)(unsigned long)CRUMB_TABLE;
	u32 v = *seq;

	*seq = (v == 0 || v > 100000u) ? 1u : v + 1u;
	crumb_boot_seq = *seq;

	memset(tbl, 0, CRUMB_MAX * sizeof(struct chopin_crumb));

	crumb_flush(tbl, CRUMB_MAX * sizeof(struct chopin_crumb));
	crumb_flush(seq, sizeof(*seq));
}

static void crumb_stash_one(const struct chopin_crumb *c)
{
	if (crumb_stash_cnt < CRUMB_MAX)
		crumb_stash[crumb_stash_cnt++] = *c;
}

/*
 * Best-effort copy of the accumulated records into the pstore block.
 * Returns true if the block was written.
 *
 * The identity map covers the block until setup_arch() removes it, and
 * CRUMB_IDMAP_LAST is the last stage that runs before that. Past it, the
 * fixmap is used, which resolves in TTBR1 and does not care whether the
 * linear map covers the block. __va() is not used at all: whether it
 * works depends on the block not being marked no-map, and that is not
 * something this code is in a position to guarantee.
 */
static bool crumb_write_block(bool force_idmap)
{
	struct chopin_crumb *tbl;
	void *fix = NULL;
	unsigned long len = 0;
	u32 i;

	if (force_idmap || crumb_reached <= CRUMB_IDMAP_LAST) {
		tbl = (void *)(unsigned long)CRUMB_TABLE;
	} else {
		unsigned long start = CRUMB_TABLE & PAGE_MASK;

		len = PAGE_ALIGN(CRUMB_TABLE + CRUMB_MAX *
				 sizeof(struct chopin_crumb) - start);
		fix = early_memremap(start, len);
		if (!fix)
			return false;
		tbl = (struct chopin_crumb *)((char *)fix +
					      (CRUMB_TABLE - start));
	}

	for (i = 0; i < crumb_stash_cnt; i++)
		tbl[i] = crumb_stash[i];

	crumb_flush(tbl, CRUMB_MAX * sizeof(struct chopin_crumb));

	if (fix)
		early_iounmap(fix, len);

	return true;
}

/*
 * Mark a stage, choosing the channel from the numeric range as usual.
 */
void chopin_crumb_mark(u32 stage)
{
	chopin_crumb_mark_idmap(stage, stage <= CRUMB_IDMAP_LAST);
}

/*
 * Mark a stage, overriding the channel.
 *
 * force_idmap exists for the inside of cpu_replace_ttbr1(), where the
 * two mappings swap over and the usual range rule gets it wrong: past
 * STAGE_PI_MAP_MEM the fixmap is the correct default, but between
 * cpu_install_idmap() and cpu_uninstall_idmap() TTBR1 holds the new pgd
 * and has no fixmap entries, while TTBR0 holds the identity map that
 * head.S extended to cover the block. Only the caller knows which of the
 * two is live, so it says so.
 */
void chopin_crumb_mark_idmap(u32 stage, bool force_idmap)
{
	struct chopin_crumb c;
	u64 sp = (u64)(unsigned long)__builtin_frame_address(0);

	if (stage == STAGE_START_KERNEL)
		crumb_boot_begin();

	if (stage > crumb_reached)
		crumb_reached = stage;

	c.magic = CRUMB_MAGIC;
	c.seq = crumb_boot_seq;
	c.stage = stage;
	c.reached = crumb_reached;
	c.sp = sp;
	c.flags = force_idmap ? CRUMB_FLAG_IDMAP : CRUMB_FLAG_FIXMAP;

	/*
	 * Unconditional and fault-free. If everything else fails, this is
	 * what the dump will be built from.
	 */
	crumb_stash_one(&c);

	if (!crumb_write_block(force_idmap))
		crumb_stash[crumb_stash_cnt - 1].flags = CRUMB_FLAG_BSS;
}

/*
 * Late flush, for a boot that survives all the way and still needs to
 * leave a complete table behind on reset. By now the allocator is up, so
 * this does not depend on the linear map covering the block at all:
 * ioremap() builds its own page tables for the range, which is exactly
 * how ramoops reaches the same memory (ram_core.c does ioremap() on the
 * address LK passes on the command line). That makes this the one path
 * that works regardless of how map_mem() treated the block.
 */
static int __init chopin_crumb_flush_init(void)
{
	struct chopin_crumb *tbl;
	u32 i;

	/* ioremap() builds its own tables, so no channel choice is needed. */
	tbl = ioremap(CRUMB_TABLE, CRUMB_MAX * sizeof(struct chopin_crumb));
	if (!tbl)
		return 0;

	for (i = 0; i < crumb_stash_cnt; i++)
		tbl[i] = crumb_stash[i];

	crumb_flush(tbl, CRUMB_MAX * sizeof(struct chopin_crumb));
	iounmap(tbl);

	return 0;
}
late_initcall(chopin_crumb_flush_init);

/*
 * Capture trigger on five short presses of volume-down.
 *
 * A boot that stops halfway leaves no way to capture it. Holding the
 * power key is the only way out on this phone, and that cuts power: the
 * next power-up clears DRAM, which is where the kernel log and the
 * pstore block live. Every dump taken after such a press came back
 * empty, which is why several rounds of USB work were done without ever
 * reading a message the kernel printed.
 *
 * The stall being chased is not a wedged task either -- by the time the
 * screen sits on the logo, init is alive and looping on a failing
 * command, so a hung-task or soft-lockup watchshdog never fires. What is
 * needed is a trigger that only requires the kernel to be running,
 * which is what a key handler provides: the keypad keeps generating
 * events as long as its interrupt is served, and that holds through a
 * userspace stall.
 *
 * So: five short presses, then dump the log and hand control back to the
 * bootloader in software. A software re-entry keeps power up, so DRAM
 * survives and kmsg_dump() copies the log into pstore on the way out.
 * The log lands in the block LK already exports to the expdb partition,
 * so the next boot reads it back without the phone ever losing power.
 *
 * Volume-down rather than volume-up or power: the keypad reports all of
 * them, but volume-down is the one that is not otherwise load-bearing
 * during boot. Power is held for a hardware reset and volume-up is held
 * for the bootloader menu, so neither can be used for this.
 *
 * A plain input_handler rather than a hook in the keypad driver: it
 * attaches to whatever device reports the key, needs no knowledge of the
 * keypad hardware, and leaves a working driver untouched.
 */

#include <linux/slab.h>
#include <linux/input.h>
#include <linux/kmsg_dump.h>
#include <linux/reboot.h>
#include <linux/delay.h>
#include <linux/workqueue.h>

#define CRUMB_KEY		KEY_VOLUMEDOWN
#define CRUMB_PRESSES		5
/* Long enough to be deliberate, short enough to stay convenient. */
#define CRUMB_WINDOW_MS		3000

static unsigned int crumb_presses;
static unsigned long crumb_first_ms;

/*
 * Runs off the input event path. Both the dump and the reboot do things
 * that must not happen with the keypad spinlock held, and the input core
 * calls the event callback from the interrupt handler.
 */
static void crumb_trigger(struct work_struct *w)
{
	pr_emerg("mediatek-crumb: five presses, dumping log before re-entry\n");

	/*
	 * KMSG_DUMP_OOPS is the reason pstore records as dmesg-ramoops-N,
	 * the record that reads back as a whole log rather than as a crash
	 * report. It is not a panic, so nothing else fires from it.
	 */
	kmsg_dump(KMSG_DUMP_OOPS);

	/* Give the store a moment to drain before control leaves. */
	mdelay(200);

	emergency_restart();
}

static DECLARE_WORK(crumb_work, crumb_trigger);

static void crumb_event(struct input_handle *handle, unsigned int type,
			unsigned int code, int value)
{
	unsigned long now;

	if (type != EV_KEY || code != CRUMB_KEY)
		return;

	if (!value)
		return;

	now = jiffies;

	/* A press after the window closed starts a fresh count. */
	if (!crumb_presses ||
	    time_after(now,
		       crumb_first_ms + msecs_to_jiffies(CRUMB_WINDOW_MS))) {
		crumb_first_ms = now;
		crumb_presses = 0;
	}

	crumb_presses++;

	if (crumb_presses >= CRUMB_PRESSES) {
		crumb_presses = 0;
		schedule_work(&crumb_work);
	}
}

static bool crumb_filter(struct input_handle *handle, unsigned int type,
			 unsigned int code, int value)
{
	if (type != EV_KEY || code != CRUMB_KEY)
		return false;

	/*
	 * Swallow the key once the trigger is armed so userspace does not
	 * also act on it. Until then it passes through: volume-down is
	 * still an ordinary key while the count is building.
	 */
	return crumb_presses >= CRUMB_PRESSES;
}

static int crumb_connect(struct input_handler *handler, struct input_dev *dev,
			 const struct input_device_id *id)
{
	struct input_handle *handle;
	int error;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "chopin-crumb";

	error = input_register_handle(handle);
	if (error) {
		kfree(handle);
		return error;
	}

	error = input_open_device(handle);
	if (error) {
		input_unregister_handle(handle);
		kfree(handle);
		return error;
	}

	return 0;
}

static void crumb_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id crumb_ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) },
	},
	{ },
};

static struct input_handler crumb_handler = {
	.event		= crumb_event,
	.filter		= crumb_filter,
	.connect	= crumb_connect,
	.disconnect	= crumb_disconnect,
	.name		=	"chopin-crumb",
	.id_table	= crumb_ids,
};

static int __init chopin_crumb_key_init(void)
{
	return input_register_handler(&crumb_handler);
}
device_initcall(chopin_crumb_key_init);
