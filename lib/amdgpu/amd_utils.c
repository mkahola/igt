/* SPDX-License-Identifier: MIT
 * Copyright 2026 Advanced Micro Devices, Inc.
 */

#include <inttypes.h>
#include "igt.h"
#include "amd_utils.h"

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
