/* SPDX-License-Identifier: MIT
 * Copyright 2026 Advanced Micro Devices, Inc.
 */

#include <fcntl.h>
#include <inttypes.h>
#include <string.h>
#include <unistd.h>

#include "igt.h"
#include "igt_taints.h"
#include "amd_utils.h"

static const char * const lockdep_violation_patterns[] = {
	"circular locking dependency",
	"possible recursive locking detected",
	"inconsistent lock state",
	"possible circular locking dependency",
	"WARNING: lock held when returning to user space",
	NULL
};

/**
 * log_total_time - record start time and print total runtime
 * @enter: true to start timing, false to print and stop
 * @binary: binary name to include in the message (e.g., igt_test_name())
 *
 * This helper is intended for summarizing the total runtime of an IGT binary.
 * Call it once with @enter=true near the start of the program, then call it
 * again with @enter=false just before exiting.
 *
 * Not thread-safe: uses static state.
 */
void log_total_time(bool enter, const char *binary)
{
	static struct timespec t0;
	static bool started;
	static uint64_t total_ns;

	uint64_t total_s, mins, secs;

	if (enter) {
		igt_gettime(&t0);
		started = true;
		return;
 	}

	if (!started) {
		igt_warn("log_total_time(false) called before start\n");
		return;
	}

	total_ns = igt_nsec_elapsed(&t0);

	total_s = total_ns / 1000000000ULL;
	mins = total_s / 60;
	secs = total_s % 60;

	igt_kmsg("=== TOTAL (all subtests in %s binary): %" PRIu64 " min %" PRIu64 " sec ===\n",
			binary, mins, secs);
}

bool amd_is_lockdep_enabled(void)
{
	return access("/proc/lockdep_stats", F_OK) == 0;
}

int amd_lockdep_kmsg_open(void)
{
	int fd;

	fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
	if (fd < 0)
		return -1;

	lseek(fd, 0, SEEK_END);
	return fd;
}

bool amd_lockdep_kmsg_has_violation(int kmsg_fd)
{
	char buf[4096];
	ssize_t len;
	int i;

	if (kmsg_fd < 0)
		return false;

	while ((len = read(kmsg_fd, buf, sizeof(buf) - 1)) > 0) {
		buf[len] = '\0';
		for (i = 0; lockdep_violation_patterns[i]; i++) {
			if (strstr(buf, lockdep_violation_patterns[i])) {
				igt_warn("LOCKDEP VIOLATION: %s\n",
					 lockdep_violation_patterns[i]);
				igt_warn("  kmsg: %.200s\n", buf);
				return true;
			}
		}
	}

	return false;
}

void amd_assert_no_lockdep_violations(int kmsg_fd,
				      unsigned long taint_before)
{
	unsigned long taints = 0;
	bool violation;

	violation = amd_lockdep_kmsg_has_violation(kmsg_fd);

	igt_kernel_tainted(&taints);

	if ((taints & ~taint_before) & (1ul << TAINT_WARN)) {
		igt_warn("TAINT_WARN set during test - possible lockdep splat\n");
		violation = true;
	}

	igt_assert_f(!violation,
		     "Lockdep violation detected! Check dmesg for details.\n");
}

void amd_lockdep_begin(struct amd_lockdep_state *lockdep)
{
	lockdep->enabled = amd_is_lockdep_enabled();
	lockdep->taint_before = 0;
	lockdep->kmsg_fd = -1;

	if (!lockdep->enabled)
		return;

	igt_kernel_tainted(&lockdep->taint_before);
	lockdep->kmsg_fd = amd_lockdep_kmsg_open();
	igt_assert_f(lockdep->kmsg_fd >= 0,
		     "Failed to open /dev/kmsg for lockdep checking\n");
}

void amd_lockdep_end(struct amd_lockdep_state *lockdep)
{
	if (!lockdep->enabled)
		return;

	amd_assert_no_lockdep_violations(lockdep->kmsg_fd,
					 lockdep->taint_before);
	close(lockdep->kmsg_fd);
	lockdep->kmsg_fd = -1;
}
