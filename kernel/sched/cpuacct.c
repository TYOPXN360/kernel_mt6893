// SPDX-License-Identifier: GPL-2.0
#include <linux/cgroup.h>
#include <linux/slab.h>
#include <linux/percpu.h>
#include <linux/spinlock.h>
#include <linux/cpumask.h>
#include <linux/seq_file.h>
#include <linux/rcupdate.h>
#include <linux/kernel_stat.h>
#include <linux/err.h>

#include "sched.h"

/*
 * CPU accounting code for task groups.
 *
 * Based on the work by Paul Menage (menage@google.com) and Balbir Singh
 * (balbir@in.ibm.com).
 */

/* Time spent by the tasks of the cpu accounting group executing in ... */
enum cpuacct_stat_index {
	CPUACCT_STAT_USER,	/* ... user mode */
	CPUACCT_STAT_SYSTEM,	/* ... kernel mode */

	CPUACCT_STAT_NSTATS,
};

static const char * const cpuacct_stat_desc[] = {
	[CPUACCT_STAT_USER] = "user",
	[CPUACCT_STAT_SYSTEM] = "system",
};

struct cpuacct_usage {
	u64	usages[CPUACCT_STAT_NSTATS];
};

/* track cpu usage of a group of tasks and its child groups */
struct cpuacct {
	struct cgroup_subsys_state css;
	/* cpuusage holds pointer to a u64-type object on every cpu */
	struct cpuacct_usage __percpu *cpuusage;
	struct kernel_cpustat __percpu *cpustat;
};

static inline struct cpuacct *css_ca(struct cgroup_subsys_state *css)
{
	return css ? container_of(css, struct cpuacct, css) : NULL;
}

/* return cpu accounting group to which this task belongs */
static inline struct cpuacct *task_ca(struct task_struct *tsk)
{
	return css_ca(task_css(tsk, cpuacct_cgrp_id));
}

static inline struct cpuacct *parent_ca(struct cpuacct *ca)
{
	return css_ca(ca->css.parent);
}

static DEFINE_PER_CPU(struct cpuacct_usage, root_cpuacct_cpuusage);
static struct cpuacct root_cpuacct = {
	.cpustat	= &kernel_cpustat,
	.cpuusage	= &root_cpuacct_cpuusage,
};

/* create a new cpu accounting group */
static struct cgroup_subsys_state *
cpuacct_css_alloc(struct cgroup_subsys_state *parent_css)
{
	struct cpuacct *ca;

	if (!parent_css)
		return &root_cpuacct.css;

	ca = kzalloc(sizeof(*ca), GFP_KERNEL);
	if (!ca)
		goto out;

	ca->cpuusage = alloc_percpu(struct cpuacct_usage);
	if (!ca->cpuusage)
		goto out_free_ca;

	ca->cpustat = alloc_percpu(struct kernel_cpustat);
	if (!ca->cpustat)
		goto out_free_cpuusage;

	return &ca->css;

out_free_cpuusage:
	free_percpu(ca->cpuusage);
out_free_ca:
	kfree(ca);
out:
	return ERR_PTR(-ENOMEM);
}

/* destroy an existing cpu accounting group */
static void cpuacct_css_free(struct cgroup_subsys_state *css)
{
	struct cpuacct *ca = css_ca(css);

	free_percpu(ca->cpustat);
	free_percpu(ca->cpuusage);
	kfree(ca);
}

static u64 cpuacct_cpuusage_read(struct cpuacct *ca, int cpu,
				 enum cpuacct_stat_index index)
{
	struct cpuacct_usage *cpuusage = per_cpu_ptr(ca->cpuusage, cpu);
	u64 data;

	/*
	 * We allow index == CPUACCT_STAT_NSTATS here to read
	 * the sum of suages.
	 */
	BUG_ON(index > CPUACCT_STAT_NSTATS);

#ifndef CONFIG_64BIT
	/*
	 * Take rq->lock to make 64-bit read safe on 32-bit platforms.
	 */
	raw_spin_lock_irq(&cpu_rq(cpu)->lock);
#endif

	if (index == CPUACCT_STAT_NSTATS) {
		int i = 0;

		data = 0;
		for (i = 0; i < CPUACCT_STAT_NSTATS; i++)
			data += cpuusage->usages[i];
	} else {
		data = cpuusage->usages[index];
	}

#ifndef CONFIG_64BIT
	raw_spin_unlock_irq(&cpu_rq(cpu)->lock);
#endif

	return data;
}

static void cpuacct_cpuusage_write(struct cpuacct *ca, int cpu, u64 val)
{
	struct cpuacct_usage *cpuusage = per_cpu_ptr(ca->cpuusage, cpu);
	int i;

#ifndef CONFIG_64BIT
	/*
	 * Take rq->lock to make 64-bit write safe on 32-bit platforms.
	 */
	raw_spin_lock_irq(&cpu_rq(cpu)->lock);
#endif

	for (i = 0; i < CPUACCT_STAT_NSTATS; i++)
		cpuusage->usages[i] = val;

#ifndef CONFIG_64BIT
	raw_spin_unlock_irq(&cpu_rq(cpu)->lock);
#endif
}

/* return total cpu usage (in nanoseconds) of a group */
static u64 __cpuusage_read(struct cgroup_subsys_state *css,
			   enum cpuacct_stat_index index)
{
	struct cpuacct *ca = css_ca(css);
	u64 totalcpuusage = 0;
	int i;

	for_each_possible_cpu(i)
		totalcpuusage += cpuacct_cpuusage_read(ca, i, index);

	return totalcpuusage;
}

static u64 cpuusage_user_read(struct cgroup_subsys_state *css,
			      struct cftype *cft)
{
	return __cpuusage_read(css, CPUACCT_STAT_USER);
}

static u64 cpuusage_sys_read(struct cgroup_subsys_state *css,
			     struct cftype *cft)
{
	return __cpuusage_read(css, CPUACCT_STAT_SYSTEM);
}

static u64 cpuusage_read(struct cgroup_subsys_state *css, struct cftype *cft)
{
	return __cpuusage_read(css, CPUACCT_STAT_NSTATS);
}

static int cpuusage_write(struct cgroup_subsys_state *css, struct cftype *cft,
			  u64 val)
{
	struct cpuacct *ca = css_ca(css);
	int cpu;

	/*
	 * Only allow '0' here to do a reset.
	 */
	if (val)
		return -EINVAL;

	for_each_possible_cpu(cpu)
		cpuacct_cpuusage_write(ca, cpu, 0);

	return 0;
}

static int __cpuacct_percpu_seq_show(struct seq_file *m,
				     enum cpuacct_stat_index index)
{
	struct cpuacct *ca = css_ca(seq_css(m));
	u64 percpu;
	int i;

	for_each_possible_cpu(i) {
		percpu = cpuacct_cpuusage_read(ca, i, index);
		seq_printf(m, "%llu ", (unsigned long long) percpu);
	}
	seq_printf(m, "\n");
	return 0;
}

static int cpuacct_percpu_user_seq_show(struct seq_file *m, void *V)
{
	return __cpuacct_percpu_seq_show(m, CPUACCT_STAT_USER);
}

static int cpuacct_percpu_sys_seq_show(struct seq_file *m, void *V)
{
	return __cpuacct_percpu_seq_show(m, CPUACCT_STAT_SYSTEM);
}

static int cpuacct_percpu_seq_show(struct seq_file *m, void *V)
{
	return __cpuacct_percpu_seq_show(m, CPUACCT_STAT_NSTATS);
}

static int cpuacct_all_seq_show(struct seq_file *m, void *V)
{
	struct cpuacct *ca = css_ca(seq_css(m));
	int index;
	int cpu;

	seq_puts(m, "cpu");
	for (index = 0; index < CPUACCT_STAT_NSTATS; index++)
		seq_printf(m, " %s", cpuacct_stat_desc[index]);
	seq_puts(m, "\n");

	for_each_possible_cpu(cpu) {
		struct cpuacct_usage *cpuusage = per_cpu_ptr(ca->cpuusage, cpu);

		seq_printf(m, "%d", cpu);

		for (index = 0; index < CPUACCT_STAT_NSTATS; index++) {
#ifndef CONFIG_64BIT
			/*
			 * Take rq->lock to make 64-bit read safe on 32-bit
			 * platforms.
			 */
			raw_spin_lock_irq(&cpu_rq(cpu)->lock);
#endif

			seq_printf(m, " %llu", cpuusage->usages[index]);

#ifndef CONFIG_64BIT
			raw_spin_unlock_irq(&cpu_rq(cpu)->lock);
#endif
		}
		seq_puts(m, "\n");
	}
	return 0;
}

static int cpuacct_stats_show(struct seq_file *sf, void *v)
{
	struct cpuacct *ca = css_ca(seq_css(sf));
	s64 val[CPUACCT_STAT_NSTATS];
	int cpu;
	int stat;

	memset(val, 0, sizeof(val));
	for_each_possible_cpu(cpu) {
		u64 *cpustat = per_cpu_ptr(ca->cpustat, cpu)->cpustat;

		val[CPUACCT_STAT_USER]   += cpustat[CPUTIME_USER];
		val[CPUACCT_STAT_USER]   += cpustat[CPUTIME_NICE];
		val[CPUACCT_STAT_SYSTEM] += cpustat[CPUTIME_SYSTEM];
		val[CPUACCT_STAT_SYSTEM] += cpustat[CPUTIME_IRQ];
		val[CPUACCT_STAT_SYSTEM] += cpustat[CPUTIME_SOFTIRQ];
	}

	for (stat = 0; stat < CPUACCT_STAT_NSTATS; stat++) {
		seq_printf(sf, "%s %lld\n",
			   cpuacct_stat_desc[stat],
			   (long long)nsec_to_clock_t(val[stat]));
	}

	return 0;
}

static struct cftype files[] = {
	{
		.name = "usage",
		.read_u64 = cpuusage_read,
		.write_u64 = cpuusage_write,
	},
	{
		.name = "usage_user",
		.read_u64 = cpuusage_user_read,
	},
	{
		.name = "usage_sys",
		.read_u64 = cpuusage_sys_read,
	},
	{
		.name = "usage_percpu",
		.seq_show = cpuacct_percpu_seq_show,
	},
	{
		.name = "usage_percpu_user",
		.seq_show = cpuacct_percpu_user_seq_show,
	},
	{
		.name = "usage_percpu_sys",
		.seq_show = cpuacct_percpu_sys_seq_show,
	},
	{
		.name = "usage_all",
		.seq_show = cpuacct_all_seq_show,
	},
	{
		.name = "stat",
		.seq_show = cpuacct_stats_show,
	},
	{ }	/* terminate */
};

/*
 * charge this task's execution time to its accounting group.
 *
 * called with rq->lock held.
 */
#if defined(CONFIG_CGROUP_CPUACCT) && defined(CONFIG_CGROUP_SCHED)
/* Defined below, next to its sibling task_group_account_cputime(). */
static void task_group_account_usage(struct task_struct *tsk, u64 val);
#endif

void cpuacct_charge(struct task_struct *tsk, u64 cputime)
{
	struct cpuacct *ca;
	int index = CPUACCT_STAT_SYSTEM;
	struct pt_regs *regs = task_pt_regs(tsk);

	if (regs && user_mode(regs))
		index = CPUACCT_STAT_USER;

	rcu_read_lock();

	for (ca = task_ca(tsk); ca; ca = parent_ca(ca))
		this_cpu_ptr(ca->cpuusage)->usages[index] += cputime;

	rcu_read_unlock();

#if defined(CONFIG_CGROUP_CPUACCT) && defined(CONFIG_CGROUP_SCHED)
	/*
	 * This is the scheduler runtime event, and it is the only source for
	 * cpu.stat usage_usec. It is accounted separately from
	 * cpuacct_account_field() (the tick/cputime split that feeds
	 * user_usec/system_usec) on purpose: the two are different
	 * measurements, so usage_usec is not synthesised as user + system.
	 */
	task_group_account_usage(tsk, cputime);
#endif /* CONFIG_CGROUP_CPUACCT && CONFIG_CGROUP_SCHED */
}

/*
 * Add user/system time to the cpu controller's per-group counters.
 *
 * task_group(tsk) is the task's cpu controller task_group - the very
 * task_group that backs the v2 hierarchy, because sched_change_group()
 * derives sched_task_group from task_css_check(tsk, cpu_cgrp_id) rather than
 * from a v1-only mapping. Charging along that chain gives real per-v2-cgroup
 * numbers, and it is done here, at the single point where cputime.c already
 * classifies the charge, so no additional accounting call site is introduced.
 *
 * The walk is bounded by the root task_group and runs under rcu_read_lock(),
 * matching the lifetime rule sched_free_group() already relies on: a
 * task_group stays valid for an RCU grace period after it is unlinked, so a
 * charge racing with a group's removal either lands in the group or is
 * dropped, never dereferences freed memory. Each task_group's counters are
 * per-CPU, so concurrent charges on different CPUs need no lock; the reader
 * sums them under rcu_read_lock() as well.
 *
 * This feeds user_usec/system_usec only. usage_usec is accumulated separately
 * from cpuacct_charge(), the scheduler runtime event, because the runtime and
 * the tick-based split are different measurements.
 */
/*
 * struct task_group and css_tg() only exist under CONFIG_CGROUP_SCHED, and the
 * per-group counters only under CONFIG_CGROUP_CPUACCT, so every use of them
 * here needs both. cpuacct.o is built from CONFIG_CGROUP_CPUACCT alone, hence
 * the explicit pair rather than relying on an outer guard.
 */
#if defined(CONFIG_CGROUP_CPUACCT) && defined(CONFIG_CGROUP_SCHED)
static void task_group_account_cputime(struct task_struct *tsk, int index, u64 val)
{
	struct task_group *tg;
	bool user;

	if (!val)
		return;

	/*
	 * user/nice both count as user time; system/irq/softirq as system time.
	 */
	switch (index) {
	case CPUTIME_USER:
	case CPUTIME_NICE:
		user = true;
		break;
	case CPUTIME_SYSTEM:
	case CPUTIME_IRQ:
	case CPUTIME_SOFTIRQ:
		user = false;
		break;
	default:
		return;
	}

	/*
	 * Take one snapshot of sched_task_group rather than dereferencing the
	 * task repeatedly: rcu_read_lock() below pins the task_group objects
	 * against css_free(), which is what makes walking the parent chain safe,
	 * but sched_task_group itself is an ordinary pointer that
	 * sched_change_group() may update, so it is read once up front.
	 */
	rcu_read_lock();
	tg = READ_ONCE(tsk->sched_task_group);
	for (; tg; tg = tg->parent) {
		if (!tg->cpustat)
			break;

		/*
		 * __this_cpu_add() is given the __percpu member itself: it
		 * expands to raw_cpu_ptr(&(pcp)), which is what performs the
		 * current-CPU offset. Handing it an already-offset
		 * this_cpu_ptr() result would offset twice. __this_cpu_* (as
		 * opposed to this_cpu_*) also keeps the CONFIG_DEBUG_PREEMPT
		 * check, which matters because these charges can come from
		 * interrupt context.
		 */
		if (user)
			__this_cpu_add(tg->cpustat->user_ns, val);
		else
			__this_cpu_add(tg->cpustat->sys_ns, val);

		/* The root task_group has no parent; the walk ends there. */
		if (!tg->parent)
			break;
	}
	rcu_read_unlock();
}

/*
 * Add scheduler runtime to the cpu controller's per-group usage counter.
 *
 * Same parent-chain walk and same RCU rule as
 * task_group_account_cputime() above. Kept distinct from it because cpu.stat
 * usage_usec is documented as total time, and deriving it as user+system
 * would silently mix two different measurements (tick/cputime split versus
 * scheduler runtime) that are not guaranteed to agree.
 */
static void task_group_account_usage(struct task_struct *tsk, u64 val)
{
	struct task_group *tg;

	if (!val)
		return;

	/* Same snapshot-then-walk rule as task_group_account_cputime(). */
	rcu_read_lock();
	tg = READ_ONCE(tsk->sched_task_group);
	for (; tg; tg = tg->parent) {
		if (!tg->cpustat)
			break;

		__this_cpu_add(tg->cpustat->usage_ns, val);

		if (!tg->parent)
			break;
	}
	rcu_read_unlock();
}
#endif /* CONFIG_CGROUP_CPUACCT && CONFIG_CGROUP_SCHED */

void cpuacct_account_field(struct task_struct *tsk, int index, u64 val)
{
	struct cpuacct *ca;

	rcu_read_lock();
	for (ca = task_ca(tsk); ca != &root_cpuacct; ca = parent_ca(ca))
		this_cpu_ptr(ca->cpustat)->cpustat[index] += val;
	rcu_read_unlock();

#if defined(CONFIG_CGROUP_CPUACCT) && defined(CONFIG_CGROUP_SCHED)
	task_group_account_cputime(tsk, index, val);
#endif
}

/*
 * Read the per-task_group CPU time counters, in nanoseconds.
 *
 * Used by kernel/sched/core.c to fill the v2 cpu.stat usage fields. The
 * counters are charged to a group and to every ancestor, so a parent already
 * includes its children; that is exactly what the cgroup v2 cpu.stat
 * usage_usec is defined to report (it accounts for the processes in the
 * cgroup *and* its descendants), so no child subtraction is needed.
 */
void cpuacct_get_task_group_usage(struct cgroup_subsys_state *css,
				  u64 *usage_ns, u64 *user_ns, u64 *sys_ns)
{
#if defined(CONFIG_CGROUP_CPUACCT) && defined(CONFIG_CGROUP_SCHED)
	/*
	 * css must be the css of the cpu controller: that is the only one whose
	 * owning struct is a task_group, and it is what the cpu cftypes hand to
	 * seq_css(). css_tg() lives in sched.h for exactly this purpose.
	 */
	struct task_group *tg = css_tg(css);
	u64 usage = 0, user = 0, sys = 0;
	int cpu;

	if (!tg || !tg->cpustat) {
		*usage_ns = 0;
		*user_ns = 0;
		*sys_ns = 0;
		return;
	}

	rcu_read_lock();
	for_each_possible_cpu(cpu) {
		struct task_group_cputat *cputat = per_cpu_ptr(tg->cpustat, cpu);

		usage += cputat->usage_ns;
		user += cputat->user_ns;
		sys += cputat->sys_ns;
	}
	rcu_read_unlock();

	*usage_ns = usage;
	*user_ns = user;
	*sys_ns = sys;
#else
	/*
	 * Without both CONFIG_CGROUP_CPUACCT and CONFIG_CGROUP_SCHED there is
	 * no task_group (and no counters on it) to read.
	 */
	*usage_ns = 0;
	*user_ns = 0;
	*sys_ns = 0;
#endif
}

struct cgroup_subsys cpuacct_cgrp_subsys = {
	.css_alloc	= cpuacct_css_alloc,
	.css_free	= cpuacct_css_free,
	.legacy_cftypes	= files,
	.early_init	= true,
};
