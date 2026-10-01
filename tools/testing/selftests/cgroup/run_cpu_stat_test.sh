#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Run the cgroup v2 cpu.stat usage-counter test against the unified hierarchy.
#
# Requires root and a cgroup2 mount. The "cpu" controller must be available in
# the root's cgroup.controllers, otherwise the test cannot create groups that
# expose cpu.stat and reports SKIP rather than a false PASS.

set -u

# The harness exports TEST_BINARY when it runs the test; fall back to the
# program built next to this script when it is invoked by hand.
TEST_BINARY="${TEST_BINARY:-$(dirname "$(readlink -f "$0")")/cpu_stat_test}"

if [ ! -x "$TEST_BINARY" ]; then
	echo "SKIP: test binary not found at $TEST_BINARY (build the selftest first)"
	exit 4
fi

CG2_MOUNT=""

while [ $# -gt 0 ]; do
	case "$1" in
	--cg2-mount)
		CG2_MOUNT="$2"
		shift 2
		;;
	*)
		echo "usage: $0 [--cg2-mount DIR]" >&2
		exit 2
		;;
	esac
done

if [ "$(id -u)" -ne 0 ]; then
	echo "SKIP: needs root (cpu.stat is only readable for cgroup v2 groups)"
	exit 4
fi

# Locate a cgroup2 mount if one was not given.
if [ -z "$CG2_MOUNT" ]; then
	CG2_MOUNT="$(awk '$3 == "cgroup2" { print $2; exit }' /proc/mounts)"
fi

if [ -z "$CG2_MOUNT" ] || [ ! -d "$CG2_MOUNT" ]; then
	echo "SKIP: no cgroup2 hierarchy mounted"
	exit 4
fi

if [ ! -r "$CG2_MOUNT/cgroup.controllers" ]; then
	echo "SKIP: $CG2_MOUNT is not a usable cgroup2 mount"
	exit 4
fi

if ! grep -qw cpu "$CG2_MOUNT/cgroup.controllers"; then
	echo "SKIP: the cpu controller is not available at $CG2_MOUNT"
	echo "      (available: $(cat "$CG2_MOUNT/cgroup.controllers"))"
	exit 4
fi

echo "# using cgroup2 mount: $CG2_MOUNT"
"$TEST_BINARY" "$CG2_MOUNT"
rc=$?

if [ "$rc" -eq 4 ]; then
	exit 4
fi

exit $rc