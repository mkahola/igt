/* SPDX-License-Identifier: MIT */
/*
 * Copyright(c) 2023 Intel Corporation. All rights reserved.
 */

#ifndef __IGT_SRIOV_DEVICE_H__
#define __IGT_SRIOV_DEVICE_H__

#include <stdbool.h>
#include <stddef.h>

/* Library for managing SR-IOV (Single Root I/O Virtualization)
 * devices.
 *
 * SR-IOV is a specification that allows a single PCIe physical
 * device to appear as a physical function (PF) and multiple virtual
 * functions (VFs) to the operating system.
 */

bool igt_sriov_is_pf(int device);
bool igt_sriov_vfs_supported(int pf);
unsigned int igt_sriov_get_total_vfs(int pf);
unsigned int igt_sriov_get_enabled_vfs(int pf);
void igt_sriov_enable_vfs(int pf, unsigned int num_vfs);
void igt_sriov_disable_vfs(int pf);
bool igt_sriov_is_driver_autoprobe_enabled(int pf);
void igt_sriov_enable_driver_autoprobe(int pf);
void igt_sriov_disable_driver_autoprobe(int pf);
int igt_sriov_open_vf_drm_device(int pf, unsigned int vf_num);
bool igt_sriov_is_vf_drm_driver_probed(int pf, unsigned int vf_num);
void igt_sriov_bind_vf_drm_driver(int pf, unsigned int vf_num);
void igt_sriov_unbind_vf_drm_driver(int pf, unsigned int vf_num);
int igt_sriov_device_sysfs_open(int pf, unsigned int vf_num);
bool igt_sriov_device_reset_exists(int pf, unsigned int vf_num);
bool igt_sriov_device_reset(int pf, unsigned int vf_num);
bool intel_is_vf_device(int device);
const char *igt_sriov_func_str(unsigned int vf_num);

/**
 * __is_valid_range - Helper to check VF range is valid
 * @start_vf: Starting VF number
 * @end_vf: Ending VF number
 * @total_vfs: Total number of VFs
 *
 * Return: true if the range is valid, false otherwise.
 */
static inline bool __is_valid_range(unsigned int start_vf, unsigned int end_vf,
				    unsigned int total_vfs)
{
	return !igt_warn_on_f(start_vf > end_vf || end_vf > total_vfs || start_vf == 0,
			      "start_vf=%u, end_vf=%u, total_vfs=%u\n",
			      start_vf, end_vf, total_vfs);
}

/**
 * igt_sriov_random_vf_in_range - Get a random VF number within a specified range
 * @pf_fd: PF device file descriptor
 * @start: Starting VF number in the range
 * @end: Ending VF number in the range
 *
 * Returns a random VF number within the specified range [start, end].
 * If the range is invalid (start > end, end > total VFs,
 * or start == 0), the function returns 0.
 *
 * Return: A random VF number within the range, or 0 if the range is invalid.
 */
static inline unsigned int
igt_sriov_random_vf_in_range(int pf_fd, unsigned int start, unsigned int end)
{
	unsigned int total_vfs = igt_sriov_get_total_vfs(pf_fd);

	if (!__is_valid_range(start, end, total_vfs))
		return 0;

	return start + random() % (end - start + 1);
}

/**
 * for_each_sriov_vf - Helper for running code on each VF
 * @__pf_fd: PF device file descriptor
 * @__vf_num: VFs iterator
 *
 * For loop that iterates over all VFs associated with given PF @__pf_fd.
 */
#define for_each_sriov_vf(__pf_fd, __vf_num) \
	for (unsigned int __vf_num = 1, __total_vfs = igt_sriov_get_total_vfs(__pf_fd); \
	     __vf_num <= __total_vfs; \
	     ++__vf_num)
#define for_each_sriov_num_vfs for_each_sriov_vf

/**
 * for_each_sriov_enabled_vf - Helper for running code on each enabled VF
 * @__pf_fd: PF device file descriptor
 * @__vf_num: VFs iterator
 *
 * For loop that iterates over all enabled VFs associated with given PF @__pf_fd.
 */
#define for_each_sriov_enabled_vf(__pf_fd, __vf_num) \
	for (unsigned int __vf_num = 1, __enabled_vfs = igt_sriov_get_enabled_vfs(__pf_fd); \
	     __vf_num <= __enabled_vfs; \
	     ++__vf_num)

/**
 * for_each_sriov_vf_in_range - Iterate over VFs in a specified range
 * @__pf_fd: PF device file descriptor
 * @__start: Starting VF number in the range
 * @__end: Ending VF number in the range
 * @__vf_num: Variable to store the random VF number
 *
 * For loop that iterates over VFs associated with given PF @__pf_fd,
 * within the specified range [__start, __end]. The loop runs only if
 * the range is valid.
 */
#define for_each_sriov_vf_in_range(__pf_fd, __start, __end, __vf_num) \
	for (unsigned int __vf_num = __is_valid_range((__start), (__end), \
						      igt_sriov_get_total_vfs(__pf_fd)) ? \
						      (__start) : 0; \
	     __vf_num && __vf_num <= (__end); \
	     ++__vf_num)
#define for_each_sriov_num_vfs_in_range for_each_sriov_vf_in_range

/**
 * for_random_sriov_vf_in_range - Iterate over a random VF in a specified range
 * @__pf_fd: PF device file descriptor
 * @__start: Starting VF number in the range
 * @__end: Ending VF number in the range
 * @__vf_num: Variable to store the random VF number
 *
 * Iterates over a random VF number within the specified range [__start, __end].
 * The loop runs only if the range is valid and a random
 * VF number is successfully selected.
 */
#define for_random_sriov_vf_in_range(__pf_fd, __start, __end, __vf_num) \
	for (unsigned int __vf_num = igt_sriov_random_vf_in_range(__pf_fd, __start, __end); \
	     __vf_num != 0; __vf_num = 0)

/**
 * for_random_sriov_vf_starting_from - Iterate over a random VF starting from a specified VF
 * @__pf_fd: PF device file descriptor
 * @__start: Starting VF number
 * @__vf_num: Variable to store the random VF number
 *
 * This macro iterates over a random VF number starting from the specified
 * VF number @__start to the total number of VFs associated with the given
 * PF @__pf_fd.
 */
#define for_random_sriov_vf_starting_from(__pf_fd, __start, __vf_num) \
	for_random_sriov_vf_in_range(__pf_fd, __start, igt_sriov_get_total_vfs(__pf_fd), __vf_num)

/**
 * for_random_sriov_vf - Iterate over a random VF for a given PF
 * @__pf_fd: PF device file descriptor
 * @__vf_num: Variable to store the random VF number
 *
 * Iterates over a random VF number selected from the range
 * of all VFs associated with the given PF @__pf_fd. The loop runs only
 * if a random VF number is successfully selected.
 */
#define for_random_sriov_vf(__pf_fd, __vf_num) \
	for_random_sriov_vf_in_range(__pf_fd, 1, igt_sriov_get_total_vfs(__pf_fd), __vf_num)

/* for_random_sriov_num_vfs - Alias for for_random_sriov_vf */
#define for_random_sriov_num_vfs for_random_sriov_vf

/**
 * for_last_sriov_vf - Helper for running code on last VF
 * @__pf_fd: PF device file descriptor
 * @__vf_num: stores last VF number
 *
 * Helper allows to run code using last VF number (stored in @__vf_num)
 * associated with given PF @__pf_fd.
 */
#define for_last_sriov_vf(__pf_fd, __vf_num) \
	for (unsigned int __vf_num = igt_sriov_get_total_vfs(__pf_fd), __tmp = 0; \
	     __tmp < 1; \
	     ++__tmp)
#define for_max_sriov_num_vfs for_last_sriov_vf

#endif /* __IGT_SRIOV_DEVICE_H__ */
