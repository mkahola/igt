/* SPDX-License-Identifier: MIT
 * Copyright 2026 Advanced Micro Devices, Inc.
 */
#ifndef AMD_UTILS_H
#define AMD_UTILS_H

#include <stdbool.h>

struct amd_lockdep_state {
	bool enabled;
	unsigned long taint_before;
	int kmsg_fd;
};

/**
* log_total_time - record start time and print total runtime
* @enter: true to start timing, false to print and stop
* @binary: binary name to include in the message (e.g., igt_test_name())
*
* Helper to print a minutes/seconds summary of an entire binary run.
*
* Intended usage:
*   amdgpu_log_total_time(true, igt_test_name());
*   ...
*   amdgpu_log_total_time(false, igt_test_name());
*/
void log_total_time(bool enter, const char *binary);

bool amd_is_lockdep_enabled(void);
int amd_lockdep_kmsg_open(void);
bool amd_lockdep_kmsg_has_violation(int kmsg_fd);
void amd_assert_no_lockdep_violations(int kmsg_fd,
				      unsigned long taint_before);
void amd_lockdep_begin(struct amd_lockdep_state *lockdep);
void amd_lockdep_end(struct amd_lockdep_state *lockdep);

#endif
