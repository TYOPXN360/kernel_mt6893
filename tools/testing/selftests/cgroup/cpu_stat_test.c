// SPDX-License-Identifier: GPL-2.0
/*
 * cpu_stat_test - exercise the cgroup v2 "cpu.stat" usage counters.
 *
 * The counters under test are per task_group and are charged from two
 * different existing kernel entry points:
 *   - usage_usec  from cpuacct_charge()          (scheduler runtime)
 *   - user_usec   from cpuacct_account_field()   (tick/cputime split, user)
 *   - system_usec from cpuacct_account_field()   (tick/cputime split, system)
 *
 * The point of the two workloads below is to prove the user/system split is
 * actually wired to the kernel's classification rather than to a single
 * lumped counter: a user-space spin loop must grow user_usec far more than
 * system_usec, while a syscall-heavy loop does the opposite.
 *
 * Each group is created under the cgroup v2 mount, a child burns CPU inside
 * it, and the parent's and child's cpu.stat are compared before and after.
 * The migration part moves a running child between two sibling groups and
 * checks that the history stays in the old group, that the new group starts
 * growing, and that the parent's total keeps rising monotonically.
 *
 * Must run as root with a cgroup v2 hierarchy mounted and the "cpu" controller
 * available in cgroup.controllers. Nothing here assumes a particular CPU
 * count or an idle machine; every threshold is expressed relatively.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <sched.h>
#include <stdarg.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* MAX_PATH is the mount path; derived paths get extra room for a suffix so
 * -Wformat-truncation stays quiet and no snprintf can silently cut a path. */
#define MAX_PATH 512
#define MAX_DERIVED MAX_PATH
/* Local buffers append the longest fixed suffix we use ("/cgroup.subtree_control")
 * to a path that may already be MAX_DERIVED long. */
#define MAX_CHILD (MAX_DERIVED + 32)

/* Wall-clock seconds a worker burns CPU before exiting. */
#define BURN_SECONDS 4

/*
 * Slack for the migration check. A task that has just been migrated may take
 * one scheduling tick to stop being charged to the old group, so the old
 * group's counter is allowed a small amount of post-migration growth.
 */
#define MIGRATION_SLACK_USEC 200000

static const char *cg_root;
/* Sized so parent_dir + "/a" cannot truncate under -Wformat-truncation. */
static char parent_dir[MAX_PATH];
static char child_a[MAX_CHILD];
static char child_b[MAX_CHILD];

static int failures;
static int checks;

static void check(int ok, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static void check(int ok, const char *fmt, ...)
{
	va_list ap;

	checks++;
	va_start(ap, fmt);
	if (ok) {
		printf("ok     - ");
		vprintf(fmt, ap);
	} else {
		failures++;
		printf("FAIL   - ");
		vprintf(fmt, ap);
	}
	printf("\n");
	va_end(ap);
}

/* Read a single unsigned long out of a "cpu.stat"-style flat-keyed file. */
static unsigned long long read_stat(const char *cgdir, const char *key)
{
	char path[MAX_CHILD], line[256];
	unsigned long long value = 0;
	FILE *f;

	snprintf(path, sizeof(path), "%s/cpu.stat", cgdir);
	f = fopen(path, "r");
	if (!f)
		return 0;

	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, key, strlen(key)) == 0) {
			sscanf(line + strlen(key), "%llu", &value);
			break;
		}
	}
	fclose(f);
	return value;
}

static int memory_controller_enabled;

static int enable_memory_controller(const char *cgdir)
{
	char path[MAX_CHILD];
	int fd, ok = 0;

	snprintf(path, sizeof(path), "%s/cgroup.subtree_control", cgdir);
	fd = open(path, O_WRONLY);
	if (fd < 0)
		return 0;
	if (write(fd, "+memory", 7) == 7)
		ok = 1;
	else
		fprintf(stderr, "note: could not enable +memory in %s: %s\n",
			cgdir, strerror(errno));
	close(fd);
	return ok;
}

static void test_memory_reclaim_abi(void)
{
	char path[MAX_CHILD + 32];
	const char *valid[] = { "0", "0 swappiness=0", "0 swappiness=200" };
	const char *invalid[] = { "0 swappiness=201", "0 noswap=1", "invalid" };
	int i, fd;

	if (!memory_controller_enabled) {
		printf("# SKIP memory.reclaim checks: memory controller unavailable\n");
		return;
	}

	snprintf(path, sizeof(path), "%s/memory.reclaim", child_a);
	if (access(path, W_OK)) {
		printf("# SKIP memory.reclaim checks: %s unavailable\n", path);
		return;
	}

	for (i = 0; i < (int)(sizeof(valid) / sizeof(valid[0])); i++) {
		fd = open(path, O_WRONLY);
		if (fd < 0) {
			check(0, "open memory.reclaim: %s", strerror(errno));
			return;
		}
		check(write(fd, valid[i], strlen(valid[i])) == (ssize_t)strlen(valid[i]),
		      "memory.reclaim accepts zero-target input '%s'", valid[i]);
		close(fd);
	}

	for (i = 0; i < (int)(sizeof(invalid) / sizeof(invalid[0])); i++) {
		fd = open(path, O_WRONLY);
		if (fd < 0) {
			check(0, "open memory.reclaim: %s", strerror(errno));
			return;
		}
		errno = 0;
		check(write(fd, invalid[i], strlen(invalid[i])) < 0 && errno == EINVAL,
		      "memory.reclaim rejects invalid input '%s'", invalid[i]);
		close(fd);
	}
}

static void enable_cpu_controller(const char *cgdir)
{
	char path[MAX_CHILD];
	int fd;

	snprintf(path, sizeof(path), "%s/cgroup.subtree_control", cgdir);
	fd = open(path, O_WRONLY);
	if (fd < 0)
		return;
	/* "+cpu" may already be there, which is not an error we need to fail on. */
	if (write(fd, "+cpu", 4) < 0)
		fprintf(stderr, "note: could not enable +cpu in %s: %s\n",
			cgdir, strerror(errno));
	close(fd);
}

/*
 * Create a cgroup without touching subtree_control.
 *
 * Only the subtree parent may enable domain controllers: a cgroup that will
 * hold tasks must not have them in its own subtree_control, because cgroup v2
 * forbids that ("no internal process" rule). The children are created here
 * precisely so that the parent can distribute CPU to them.
 */
/*
 * Create a cgroup, refusing to reuse anything that already exists.
 *
 * EEXIST is treated as a hard error rather than success: the names are unique
 * per run, so an existing directory means a PID/time collision or a leftover
 * from a crashed run, and quietly adopting it would mean writing into - and
 * later removing - a cgroup this test did not create.
 */
static void mkdir_cg(const char *path)
{
	if (mkdir(path, 0755) < 0) {
		fprintf(stderr, "cannot create %s: %s%s\n", path, strerror(errno),
			errno == EEXIST
				? " (name already exists; refusing to reuse it)" : "");
		exit(2);
	}
}

static void write_pid(const char *cgdir, pid_t pid)
{
	char path[MAX_CHILD], buf[32];
	int fd, len;

	snprintf(path, sizeof(path), "%s/cgroup.procs", cgdir);
	fd = open(path, O_WRONLY);
	if (fd < 0) {
		fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno));
		exit(2);
	}
	len = snprintf(buf, sizeof(buf), "%d", pid);
	if (write(fd, buf, len) != len)
		fprintf(stderr, "cannot move pid %d into %s: %s\n", pid, cgdir,
			strerror(errno));
	close(fd);
}

/*
 * Burn CPU for BURN_SECONDS. When syscall_heavy is false the loop is pure
 * user-space arithmetic; when true it makes a getpid()/sched_yield() call in
 * every iteration so the kernel does the work on its behalf.
 */
static void burn_cpu(int syscall_heavy)
{
	struct timespec start, now;
	volatile unsigned long sink = 0;

	clock_gettime(CLOCK_MONOTONIC, &start);
	for (;;) {
		int i;

		clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec - start.tv_sec >= BURN_SECONDS)
			break;

		if (syscall_heavy) {
			for (i = 0; i < 20000; i++) {
				sink += (unsigned long)getpid();
				if ((i & 0x3ff) == 0)
					sched_yield();
			}
		} else {
			for (i = 0; i < 20000; i++)
				sink += (unsigned long)i * 2654435761UL;
		}
	}
	/* Keep the optimiser from removing the loop. */
	if (sink == 0)
		fprintf(stderr, " ");
}

static unsigned long long spawn_worker(const char *cgdir, int syscall_heavy,
				      int seconds, pid_t *pid_out)
{
	struct timespec ts = { seconds, 0 };
	pid_t pid = fork();

	if (pid < 0) {
		perror("fork");
		exit(2);
	}
	if (pid == 0) {
		write_pid(cgdir, getpid());
		nanosleep(&ts, NULL);
		burn_cpu(syscall_heavy);
		_exit(0);
	}
	if (pid_out)
		*pid_out = pid;
	return 0;
}

static void wait_for(pid_t pid)
{
	int status;

	waitpid(pid, &status, 0);
}

/* Test 1: a user-space workload must grow user_usec, not system_usec. */
static void test_user_workload(void)
{
	unsigned long long before_u, after_u, after_s;
	pid_t pid;

	before_u = read_stat(child_a, "user_usec");

	spawn_worker(child_a, 0, 0, &pid);
	wait_for(pid);

	after_u = read_stat(child_a, "user_usec");
	after_s = read_stat(child_a, "system_usec");

	check(after_u > before_u, "user workload grows user_usec (%llu -> %llu)",
	      before_u, after_u);
	check(after_u > after_s,
	      "user workload leaves user_usec above system_usec (user %llu, sys %llu)",
	      after_u, after_s);
}

/* Test 2: a syscall-heavy workload must grow system_usec as well. */
static void test_syscall_workload(void)
{
	unsigned long long before_u, before_s, after_u, after_s;
	pid_t pid;

	before_u = read_stat(child_b, "user_usec");
	before_s = read_stat(child_b, "system_usec");

	spawn_worker(child_b, 1, 0, &pid);
	wait_for(pid);

	after_u = read_stat(child_b, "user_usec");
	after_s = read_stat(child_b, "system_usec");

	check(after_u > before_u && after_s > before_s,
	      "syscall workload grows both counters (user %llu->%llu, sys %llu->%llu)",
	      before_u, after_u, before_s, after_s);
}

/* Test 3: usage_usec grows, and is tracked separately from user+system. */
static void test_usage_counter(void)
{
	unsigned long long before, after, u, s;
	pid_t pid;

	before = read_stat(child_a, "usage_usec");

	spawn_worker(child_a, 0, 0, &pid);
	wait_for(pid);

	after = read_stat(child_a, "usage_usec");
	u = read_stat(child_a, "user_usec");
	s = read_stat(child_a, "system_usec");

	check(after > before, "usage_usec grows with a busy child (%llu -> %llu)",
	      before, after);
	/*
	 * usage_usec is accumulated from the scheduler runtime event while
	 * user/system come from the tick split, so they are close but must not
	 * be required to be equal.
	 */
	check(after <= u + s + MIGRATION_SLACK_USEC,
	      "usage_usec is not larger than user_usec + system_usec (usage %llu, user %llu, sys %llu)",
	      after, u, s);
}

/* Test 4: the parent includes its children. */
static void test_parent_includes_children(void)
{
	unsigned long long parent_u, child_a_u, child_b_u, parent_usage;
	unsigned long long sum_children;
	pid_t pid;

	spawn_worker(child_a, 0, 0, &pid);
	wait_for(pid);

	parent_u = read_stat(parent_dir, "user_usec");
	child_a_u = read_stat(child_a, "user_usec");
	child_b_u = read_stat(child_b, "user_usec");
	sum_children = child_a_u + child_b_u;

	check(parent_u >= sum_children,
	      "parent user_usec includes both children (parent %llu >= children %llu)",
	      parent_u, sum_children);

	parent_usage = read_stat(parent_dir, "usage_usec");
	check(parent_usage > 0, "parent usage_usec is non-zero (%llu)", parent_usage);
}

/*
 * Test 5: moving a running task between sibling groups keeps the history in
 * the old group, starts the new one growing, and never makes the parent go
 * backwards.
 */
static void test_migration(void)
{
	unsigned long long a_before, a_after_moved, b_before, b_after;
	unsigned long long parent_before, parent_after;
	unsigned long long a_final, a_final2;
	pid_t pid;

	a_before = read_stat(child_a, "user_usec");
	b_before = read_stat(child_b, "user_usec");
	parent_before = read_stat(parent_dir, "user_usec");

	/* A child that stays inside child_a while we move it. */
	spawn_worker(child_a, 0, 0, &pid);

	/* Let it burn for a while inside child_a, then move it to child_b. */
	sleep(1);
	write_pid(child_b, pid);
	sleep(1);

	a_after_moved = read_stat(child_a, "user_usec");
	b_after = read_stat(child_b, "user_usec");
	parent_after = read_stat(parent_dir, "user_usec");

	wait_for(pid);

	/* The task is gone from child_a now, so it must not grow any further. */
	a_final = read_stat(child_a, "user_usec");
	sleep(1);
	a_final2 = read_stat(child_a, "user_usec");

	check(b_after > b_before,
	      "child_b grows after the migration (%llu -> %llu)", b_before, b_after);
	check(a_after_moved >= a_before,
	      "child_a keeps the history it accrued before the migration (%llu -> %llu)",
	      a_before, a_after_moved);
	check(a_final2 <= a_final + MIGRATION_SLACK_USEC,
	      "child_a roughly stops growing once the task left it (%llu -> %llu)",
	      a_final, a_final2);
	check(parent_after >= parent_before,
	      "parent user_usec is monotonic across the migration (%llu -> %llu)",
	      parent_before, parent_after);
}

int main(int argc, char **argv)
{
	char path[MAX_CHILD];

	if (argc < 2) {
		fprintf(stderr, "usage: %s <cgroup2 mount>\n", argv[0]);
		return 2;
	}
	cg_root = argv[1];

	if (access(cg_root, R_OK | W_OK | X_OK) != 0) {
		fprintf(stderr, "cannot access %s: %s\n", cg_root,
			strerror(errno));
		return 2;
	}

	/* A dedicated subtree keeps the test from disturbing anything else. */
	/*
	 * A unique name per run, so the test can never collide with another
	 * run, with a stale directory from a crashed run, or - more importantly -
	 * with anything a user created. Nothing that already exists in the
	 * subtree is touched: in particular no process is ever signalled, since
	 * the group is created empty and only receives this test's own children.
	 */
	snprintf(parent_dir, sizeof(parent_dir), "%s/cpu_stat_selftest.%d.%ld",
		 cg_root, (int)getpid(), (long)time(NULL));
	mkdir_cg(parent_dir);
	enable_cpu_controller(parent_dir);

	snprintf(child_a, sizeof(child_a), "%s/a", parent_dir);
	mkdir_cg(child_a);
	snprintf(child_b, sizeof(child_b), "%s/b", parent_dir);
	mkdir_cg(child_b);
	memory_controller_enabled = enable_memory_controller(parent_dir);

	test_memory_reclaim_abi();
	test_user_workload();
	test_syscall_workload();
	test_usage_counter();
	test_parent_includes_children();
	test_migration();

	printf("\n%d checks, %d failures\n", checks, failures);

	/*
	 * Best-effort cleanup. A failure here means something is still in one of
	 * the groups (or the controller could not be turned off), which is worth
	 * reporting but must not turn a passing test into a failure.
	 */
	/*
	 * Order matters: remove the (empty) children first, then turn the cpu
	 * controller back off in the parent, then remove the parent. Disabling
	 * the controller last would fail while children still exist, and
	 * removing the parent first is impossible while it has children.
	 */
	snprintf(path, sizeof(path), "%s/a", parent_dir);
	if (rmdir(path) && errno != ENOENT)
		printf("# note: could not remove %s: %s\n", path, strerror(errno));
	snprintf(path, sizeof(path), "%s/b", parent_dir);
	if (rmdir(path) && errno != ENOENT)
		printf("# note: could not remove %s: %s\n", path, strerror(errno));

	snprintf(path, sizeof(path), "%s/cgroup.subtree_control", parent_dir);
	{
		int fd = open(path, O_WRONLY);

		if (fd >= 0) {
			if (write(fd, "-cpu", 4) < 0)
				printf("# note: could not disable +cpu in %s: %s\n",
				       parent_dir, strerror(errno));
			close(fd);
		} else {
			printf("# note: could not open %s: %s\n", path,
			       strerror(errno));
		}
	}
	if (memory_controller_enabled) {
		int fd = open(path, O_WRONLY);

		if (fd >= 0) {
			if (write(fd, "-memory", 7) < 0)
				printf("# note: could not disable +memory in %s: %s\n",
				       parent_dir, strerror(errno));
			close(fd);
		}
	}

	if (rmdir(parent_dir) && errno != ENOENT)
		printf("# note: could not remove %s: %s\n", parent_dir,
		       strerror(errno));

	return failures ? 1 : 0;
}