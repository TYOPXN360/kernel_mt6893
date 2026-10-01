/* SPDX-License-Identifier: GPL-2.0 */
#ifdef CONFIG_CGROUP_CPUACCT

extern void cpuacct_charge(struct task_struct *tsk, u64 cputime);
extern void cpuacct_account_field(struct task_struct *tsk, int index, u64 val);

#ifdef CONFIG_CGROUP_SCHED
/*
 * Reads task_group::cpustat, and struct task_group only exists under
 * CONFIG_CGROUP_SCHED, so this one needs both knobs; the two APIs above stay
 * governed by CONFIG_CGROUP_CPUACCT alone because they drive the v1 cpuacct
 * subsystem, which has no task_group dependency.
 */
extern void cpuacct_get_task_group_usage(struct cgroup_subsys_state *css,
					  u64 *usage_ns, u64 *user_ns, u64 *sys_ns);
#endif /* CONFIG_CGROUP_SCHED */

#else /* !CONFIG_CGROUP_CPUACCT */

static inline void cpuacct_charge(struct task_struct *tsk, u64 cputime)
{
}

static inline void
cpuacct_account_field(struct task_struct *tsk, int index, u64 val)
{
}

#ifdef CONFIG_CGROUP_SCHED
static inline void
cpuacct_get_task_group_usage(struct cgroup_subsys_state *css, u64 *usage_ns,
			      u64 *user_ns, u64 *sys_ns)
{
	*usage_ns = 0;
	*user_ns = 0;
	*sys_ns = 0;
}
#endif /* CONFIG_CGROUP_SCHED */

#endif /* !CONFIG_CGROUP_CPUACCT */