/* SPDX-License-Identifier: MIT
 * Copyright 2026 Advanced Micro Devices, Inc.
 */
#ifndef AMD_UTILS_H
#define AMD_UTILS_H

#include <stdbool.h>

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

#endif
