// SPDX-License-Identifier: MIT
/*
 * Copyright(c) 2024 Intel Corporation. All rights reserved.
 */

#include <errno.h>

#include "drmtest.h"
#include "igt_core.h"
#include "igt_debugfs.h"
#include "igt_sriov_device.h"
#include "igt_sysfs.h"
#include "igt_sysfs_choice.h"
#include "intel_chipset.h"
#include "linux_scaffold.h"
#include "xe/xe_query.h"
#include "xe/xe_mmio.h"
#include "xe/xe_sriov_debugfs.h"
#include "xe/xe_sriov_provisioning.h"

/**
 * xe_sriov_shared_res_to_string:
 * @key: The shared resource of type enum xe_sriov_shared_res
 *
 * Converts a shared resource enum to its corresponding string
 * representation. It is useful for logging and debugging purposes.
 *
 * Return: A string representing the shared resource key.
 */
const char *xe_sriov_shared_res_to_string(enum xe_sriov_shared_res res)
{
	switch (res) {
	case XE_SRIOV_SHARED_RES_CONTEXTS:
		return "contexts";
	case XE_SRIOV_SHARED_RES_DOORBELLS:
		return "doorbells";
	case XE_SRIOV_SHARED_RES_GGTT:
		return "ggtt";
	case XE_SRIOV_SHARED_RES_LMEM:
		return "lmem";
	}

	return NULL;
}

#define PRE_1250_IP_VER_GGTT_PTE_VFID_MASK	GENMASK_ULL(4, 2)
#define GGTT_PTE_VFID_MASK			GENMASK_ULL(11, 2)
#define GGTT_PTE_VFID_SHIFT			2
#define GUC_GGTT_TOP				0xFEE00000
#define MAX_WOPCM_SIZE				SZ_8M
#define START_PTE_OFFSET			(MAX_WOPCM_SIZE / SZ_4K * sizeof(xe_ggtt_pte_t))
#define MAX_PTE_OFFSET				(GUC_GGTT_TOP / SZ_4K * sizeof(xe_ggtt_pte_t))

static uint64_t get_vfid_mask(int fd)
{
	uint16_t dev_id = intel_get_drm_devid(fd);

	return (intel_graphics_ver(dev_id) >= IP_VER(12, 50)) ?
		GGTT_PTE_VFID_MASK : PRE_1250_IP_VER_GGTT_PTE_VFID_MASK;
}

#define MAX_DEBUG_ENTRIES 70

static int append_range(struct xe_sriov_provisioned_range **ranges,
			unsigned int *nr_ranges, unsigned int vf_id,
			uint32_t start, uint32_t end)
{
	struct xe_sriov_provisioned_range *new_ranges;

	new_ranges = realloc(*ranges,
			     (*nr_ranges + 1) * sizeof(struct xe_sriov_provisioned_range));
	if (!new_ranges) {
		free(*ranges);
		*ranges = NULL;
		*nr_ranges = 0;
		return -ENOMEM;
	}

	*ranges = new_ranges;
	if (*nr_ranges < MAX_DEBUG_ENTRIES)
		igt_debug("Found VF%u GGTT range [%#x-%#x] num_ptes=%zu\n",
			  vf_id, start, end,
			  (end - start + sizeof(xe_ggtt_pte_t)) /
			  sizeof(xe_ggtt_pte_t));
	(*ranges)[*nr_ranges].vf_id = vf_id;
	(*ranges)[*nr_ranges].start = start;
	(*ranges)[*nr_ranges].end = end;
	(*nr_ranges)++;

	return 0;
}

/**
 * xe_sriov_find_ggtt_provisioned_pte_offsets - Find GGTT provisioned PTE offsets
 * @pf_fd: File descriptor for the Physical Function
 * @tile: Tile id
 * @mmio: Pointer to the MMIO structure
 * @ranges: Pointer to the array of provisioned ranges
 * @nr_ranges: Pointer to the number of provisioned ranges
 *
 * Searches for GGTT provisioned PTE ranges for each VF and populates
 * the provided ranges array with the start and end offsets of each range.
 * The number of ranges found is stored in nr_ranges.
 *
 * Reads the GGTT PTEs and identifies the VF ID associated with each PTE.
 * It then groups contiguous PTEs with the same VF ID into ranges.
 * The ranges are dynamically allocated and must be freed by the caller.
 * The start and end offsets in each range are inclusive.
 *
 * Returns 0 on success, or a negative error code on failure.
 */
int xe_sriov_find_ggtt_provisioned_pte_offsets(int pf_fd, uint8_t tile, struct xe_mmio *mmio,
					       struct xe_sriov_provisioned_range **ranges,
					       unsigned int *nr_ranges)
{
	uint64_t vfid_mask = get_vfid_mask(pf_fd);
	unsigned int vf_id, current_vf_id = -1;
	uint32_t current_start = 0;
	uint32_t current_end = 0;
	xe_ggtt_pte_t pte;
	int ret;

	*ranges = NULL;
	*nr_ranges = 0;

	for (uint32_t offset = START_PTE_OFFSET; offset < MAX_PTE_OFFSET;
	     offset += sizeof(xe_ggtt_pte_t)) {
		pte = xe_mmio_ggtt_read(mmio, tile, offset);
		vf_id = (pte & vfid_mask) >> GGTT_PTE_VFID_SHIFT;

		if (vf_id != current_vf_id) {
			if (current_vf_id != -1) {
				/* End the current range and append it */
				ret = append_range(ranges, nr_ranges, current_vf_id,
						   current_start, current_end);
				if (ret < 0)
					return ret;
			}
			/* Start a new range */
			current_vf_id = vf_id;
			current_start = offset;
		}
		current_end = offset;
	}

	if (current_vf_id != -1) {
		/* Append the last range */
		ret = append_range(ranges, nr_ranges, current_vf_id,
				   current_start, current_end);
		if (ret < 0)
			return ret;
	}

	if (*nr_ranges > MAX_DEBUG_ENTRIES)
		igt_debug("Ranges output trimmed to first %u entries out of %u\n",
			  MAX_DEBUG_ENTRIES, *nr_ranges);

	return 0;
}

/**
 * xe_sriov_shared_res_attr_name - Retrieve the attribute name for a shared resource
 * @res: Shared resource type (see enum xe_sriov_shared_res)
 * @vf_num: VF number (1-based) or 0 for PF
 *
 * Returns the attribute name corresponding to the specified
 * shared resource type and VF number. For VF (vf_num > 0), the "quota"
 * attribute name is returned (e.g., "contexts_quota"). For PF (vf_num == 0),
 * the "spare" attribute name is returned (e.g., "contexts_spare").
 *
 * Return:
 * The attribute name as a string if the resource type is valid.
 * NULL if the resource type is invalid.
 */
const char *xe_sriov_shared_res_attr_name(enum xe_sriov_shared_res res,
					  unsigned int vf_num)
{
	switch (res) {
	case XE_SRIOV_SHARED_RES_CONTEXTS:
		return vf_num ? "contexts_quota" : "contexts_spare";
	case XE_SRIOV_SHARED_RES_DOORBELLS:
		return vf_num ? "doorbells_quota" : "doorbells_spare";
	case XE_SRIOV_SHARED_RES_GGTT:
		return vf_num ? "ggtt_quota" : "ggtt_spare";
	case XE_SRIOV_SHARED_RES_LMEM:
		return vf_num ? "lmem_quota" : "lmem_spare";
	}

	return NULL;
}

/**
 * __xe_sriov_pf_get_shared_res_attr - Read shared resource attribute
 * @pf: PF device file descriptor
 * @res: Shared resource type (see enum xe_sriov_shared_res)
 * @vf_num: VF number (1-based) or 0 for PF
 * @gt_num: GT number
 * @value: Pointer to store the read attribute value
 *
 * Reads the specified shared resource attribute for the given PF device @pf,
 * VF number @vf_num, and GT @gt_num. The attribute depends on @vf_num:
 * - For VF (vf_num > 0), reads the "quota" attribute.
 * - For PF (vf_num == 0), reads the "spare" attribute.
 *
 *
 * Return: 0 on success, negative error code on failure.
 */
int __xe_sriov_pf_get_shared_res_attr(int pf, enum xe_sriov_shared_res res,
				      unsigned int vf_num, unsigned int gt_num,
				      uint64_t *value)
{
	return __xe_sriov_pf_debugfs_get_u64(pf, vf_num, gt_num,
					     xe_sriov_shared_res_attr_name(res, vf_num),
					     value);
}

/**
 * xe_sriov_pf_get_shared_res_attr - Read shared resource attribute
 * @pf: PF device file descriptor
 * @res: Shared resource type (see enum xe_sriov_shared_res)
 * @vf_num: VF number (1-based) or 0 for PF
 * @gt_num: GT number
 *
 * A throwing version of __xe_sriov_pf_get_shared_res_attr().
 * Instead of returning an error code, it returns the quota value and asserts
 * in case of an error.
 *
 * Return: The value for the given shared resource attribute.
 *         Asserts in case of failure.
 */
uint64_t xe_sriov_pf_get_shared_res_attr(int pf, enum xe_sriov_shared_res res,
					 unsigned int vf_num,
					 unsigned int gt_num)
{
	uint64_t value;

	igt_fail_on(__xe_sriov_pf_get_shared_res_attr(pf, res, vf_num, gt_num, &value));

	return value;
}

/**
 * __xe_sriov_pf_set_shared_res_attr - Set a shared resource attribute
 * @pf: PF device file descriptor
 * @res: Shared resource type (see enum xe_sriov_shared_res)
 * @vf_num: VF number (1-based) or 0 for PF
 * @gt_num: GT number
 * @value: Value to set for the shared resource attribute
 *
 * Sets the specified shared resource attribute for the given PF device @pf,
 * VF number @vf_num, and GT @gt_num. The attribute depends on @vf_num:
 * - For VF (vf_num > 0), reads the "quota" attribute.
 * - For PF (vf_num == 0), reads the "spare" attribute.
 *
 * Return: 0 on success, negative error code on failure.
 */
int __xe_sriov_pf_set_shared_res_attr(int pf, enum xe_sriov_shared_res res,
				      unsigned int vf_num, unsigned int gt_num,
				      uint64_t value)
{
	return __xe_sriov_pf_debugfs_set_u64(pf, vf_num, gt_num,
					     xe_sriov_shared_res_attr_name(res, vf_num),
					     value);
}

/**
 * xe_sriov_pf_set_shared_res_attr - Set the shared resource attribute value
 * @pf: PF device file descriptor
 * @res: Shared resource type (see enum xe_sriov_shared_res)
 * @vf_num: VF number (1-based) or 0 for PF
 * @gt_num: GT number
 * @value: Value to set
 *
 * A throwing version of __xe_sriov_pf_set_shared_res_attr().
 * Instead of returning an error code, it asserts in case of an error.
 */
void xe_sriov_pf_set_shared_res_attr(int pf, enum xe_sriov_shared_res res,
				     unsigned int vf_num, unsigned int gt_num,
				     uint64_t value)
{
	igt_fail_on(__xe_sriov_pf_set_shared_res_attr(pf, res, vf_num, gt_num, value));
}

/**
 * xe_sriov_is_shared_res_provisionable - Check if a shared resource is provisionable
 * @pf: PF device file descriptor
 * @res: Shared resource type (see enum xe_sriov_shared_res)
 * @gt_num: GT number
 *
 * Determines whether a specified shared resource can be provisioned.
 *
 * Return: true if the shared resource is provisionable, false otherwise.
 */
bool xe_sriov_is_shared_res_provisionable(int pf, enum xe_sriov_shared_res res,
					  unsigned int gt_num)
{
	if (res == XE_SRIOV_SHARED_RES_LMEM)
		return xe_has_vram(pf) && !xe_is_media_gt(pf, gt_num);
	else if (res == XE_SRIOV_SHARED_RES_GGTT)
		return !xe_is_media_gt(pf, gt_num);

	return true;
}

/**
 * __xe_sriov_pf_get_provisioned_quota - Get VF's provisioned quota.
 * @pf: PF device file descriptor
 * @res: Shared resource type (see enum xe_sriov_shared_res)
 * @vf_num: VF number (1-based)
 * @gt_num: GT number
 * @value: Pointer to store the read value
 *
 * Gets VF's provisioning value for the specified shared resource @res,
 * VF number @vf_num and GT number @gt_num.
 *
 * Return: 0 on success, negative error code on failure.
 */
int __xe_sriov_pf_get_provisioned_quota(int pf, enum xe_sriov_shared_res res,
					unsigned int vf_num, unsigned int gt_num,
					uint64_t *value)
{
	struct xe_sriov_provisioned_range *ranges;
	int ret;

	ret = xe_sriov_pf_debugfs_read_check_ranges(pf, res, gt_num, &ranges,
						    igt_sriov_get_enabled_vfs(pf));
	if (igt_debug_on_f(ret, "%s: Failed ranges check on GT%u (%d)\n",
			   xe_sriov_debugfs_provisioned_attr_name(res), gt_num, ret))
		return ret;

	*value = ranges[vf_num - 1].end - ranges[vf_num - 1].start + 1;

	free(ranges);

	return 0;
}

/**
 * xe_sriov_pf_get_provisioned_quota - Get VF's provisioned quota.
 * @pf: PF device file descriptor
 * @res: Shared resource type (see enum xe_sriov_shared_res)
 * @vf_num: VF number (1-based)
 * @gt_num: GT number
 *
 * A throwing version of __xe_sriov_pf_get_provisioned_quota().
 * Instead of returning an error code, it returns the quota value and asserts
 * in case of an error.
 *
 * Return: The provisioned quota for the given shared resource.
 *         Asserts in case of failure.
 */
uint64_t xe_sriov_pf_get_provisioned_quota(int pf, enum xe_sriov_shared_res res,
					   unsigned int vf_num, unsigned int gt_num)
{
	uint64_t value;

	igt_fail_on(__xe_sriov_pf_get_provisioned_quota(pf, res, vf_num, gt_num, &value));

	return value;
}

/**
 * __xe_sriov_get_exec_quantum_ms - Read the execution quantum in milliseconds for a given VF
 * @pf: PF device file descriptor
 * @vf_num: VF number (1-based) or 0 for PF
 * @gt_num: GT number
 * @value: Pointer to store the read value
 *
 * Reads the execution quantum in milliseconds for the given PF device @pf,
 * VF number @vf_num on GT @gt_num.
 *
 * Return: 0 on success, negative error code on failure.
 */
int __xe_sriov_get_exec_quantum_ms(int pf, unsigned int vf_num,
				   unsigned int gt_num, uint32_t *value)
{
	return __xe_sriov_pf_debugfs_get_u32(pf, vf_num, gt_num, "exec_quantum_ms", value);
}

/**
 * xe_sriov_get_exec_quantum_ms - Get the execution quantum in milliseconds for a given VF
 * @pf: PF device file descriptor
 * @vf_num: VF number (1-based) or 0 for PF
 * @gt_num: GT number
 *
 * A throwing version of __xe_sriov_get_exec_quantum_ms().
 * Instead of returning an error code, it returns the value read and
 * asserts in case of an error.
 *
 * Return: Execution quantum in milliseconds assigned to a given VF. Asserts in case of failure.
 */
uint32_t xe_sriov_get_exec_quantum_ms(int pf, unsigned int vf_num,
				      unsigned int gt_num)
{
	uint32_t value;

	igt_fail_on(__xe_sriov_get_exec_quantum_ms(pf, vf_num, gt_num, &value));

	return value;
}

/**
 * __xe_sriov_set_exec_quantum_ms - Set the execution quantum in milliseconds for a given VF
 * @pf: PF device file descriptor
 * @vf_num: VF number (1-based) or 0 for PF
 * @gt_num: GT number
 * @value: Value to set
 *
 * Sets the execution quantum in milliseconds for the given PF device @pf,
 * VF number @vf_num on GT @gt_num.
 *
 * Return: 0 on success, negative error code on failure.
 */
int __xe_sriov_set_exec_quantum_ms(int pf, unsigned int vf_num,
				   unsigned int gt_num, uint32_t value)
{
	return __xe_sriov_pf_debugfs_set_u32(pf, vf_num, gt_num, "exec_quantum_ms", value);
}

/**
 * xe_sriov_set_exec_quantum_ms - Set the execution quantum in milliseconds for a given VF
 * @pf: PF device file descriptor
 * @vf_num: VF number (1-based) or 0 for PF
 * @gt_num: GT number
 * @value: Value to set
 *
 * A throwing version of __xe_sriov_set_exec_quantum_ms().
 * Instead of returning an error code, it asserts in case of an error.
 */
void xe_sriov_set_exec_quantum_ms(int pf, unsigned int vf_num,
				  unsigned int gt_num, uint32_t value)
{
	igt_fail_on(__xe_sriov_set_exec_quantum_ms(pf, vf_num, gt_num, value));
}

/**
 * __xe_sriov_get_preempt_timeout_us - Get the preemption timeout in microseconds for a given VF
 * @pf: PF device file descriptor
 * @vf_num: VF number (1-based) or 0 for PF
 * @gt_num: GT number
 * @value: Pointer to store the read value
 *
 * Reads the preemption timeout in microseconds for the given PF device @pf,
 * VF number @vf_num on GT @gt_num.
 *
 * Return: 0 on success, negative error code on failure.
 */
int __xe_sriov_get_preempt_timeout_us(int pf, unsigned int vf_num,
				      unsigned int gt_num, uint32_t *value)
{
	return __xe_sriov_pf_debugfs_get_u32(pf, vf_num, gt_num, "preempt_timeout_us", value);
}

/**
 * xe_sriov_get_preempt_timeout_us - Get the preemption timeout in microseconds for a given VF
 * @pf: PF device file descriptor
 * @vf_num: VF number (1-based) or 0 for PF
 * @gt_num: GT number
 *
 * A throwing version of __xe_sriov_get_preempt_timeout_us().
 * Instead of returning an error code, it returns the value read and
 * asserts in case of an error.
 *
 * Return: Preemption timeout in microseconds assigned to a given VF.
 * Asserts in case of failure.
 */
uint32_t xe_sriov_get_preempt_timeout_us(int pf, unsigned int vf_num,
					 unsigned int gt_num)
{
	uint32_t value;

	igt_fail_on(__xe_sriov_get_preempt_timeout_us(pf, vf_num, gt_num, &value));

	return value;
}

/**
 * __xe_sriov_set_preempt_timeout_us - Set the preemption timeout in microseconds for a given VF
 * @pf: PF device file descriptor
 * @vf_num: VF number (1-based) or 0 for PF
 * @gt_num: GT number
 * @value: Value to set
 *
 * Sets the preemption timeout in microseconds for the given PF device @pf,
 * VF number @vf_num on GT @gt_num.
 *
 * Return: 0 on success, negative error code on failure.
 */
int __xe_sriov_set_preempt_timeout_us(int pf, unsigned int vf_num,
				      unsigned int gt_num, uint32_t value)
{
	return __xe_sriov_pf_debugfs_set_u32(pf, vf_num, gt_num, "preempt_timeout_us", value);
}

/**
 * xe_sriov_set_preempt_timeout_us - Set the preemption timeout in microseconds for a given VF
 * @pf: PF device file descriptor
 * @vf_num: VF number (1-based) or 0 for PF
 * @gt_num: GT number
 * @value: Value to set
 *
 * A throwing version of __xe_sriov_set_preempt_timeout_us().
 * Instead of returning an error code, it asserts in case of an error.
 */
void xe_sriov_set_preempt_timeout_us(int pf, unsigned int vf_num,
				     unsigned int gt_num, uint32_t value)
{
	igt_fail_on(__xe_sriov_set_preempt_timeout_us(pf, vf_num, gt_num, value));
}

/**
 * __xe_sriov_get_engine_reset - Get the engine reset policy status for a given GT
 * @pf: PF device file descriptor
 * @gt_num: GT number
 * @value: Pointer to store the read engine reset policy status
 *
 * Reads the engine reset status for the given PF device @pf on GT @gt_num.
 *
 * Return: 0 on success, negative error code on failure.
 */
int __xe_sriov_get_engine_reset(int pf, unsigned int gt_num, bool *value)
{
	return __xe_sriov_pf_debugfs_get_boolean(pf, 0, gt_num, "reset_engine", value);
}

/**
 * xe_sriov_get_engine_reset - Get the engine reset policy status for a given GT
 * @pf: PF device file descriptor
 * @gt_num: GT number
 *
 * A throwing version of __xe_sriov_get_engine_reset().
 * Instead of returning an error code, it returns the engine reset status
 * and asserts in case of an error.
 *
 * Return: The engine reset status for the given GT.
 *         Asserts in case of failure.
 */
bool xe_sriov_get_engine_reset(int pf, unsigned int gt_num)
{
	bool value;

	igt_fail_on(__xe_sriov_get_engine_reset(pf, gt_num, &value));

	return value;
}

/**
 * __xe_sriov_set_engine_reset - Set the engine reset policy for a given GT
 * @pf: PF device file descriptor
 * @gt_num: GT number
 * @value: Engine reset policy status to set
 *
 * Sets the engine reset policy for the given PF device @pf on GT @gt_num.
 *
 * Return: 0 on success, negative error code on failure.
 */
int __xe_sriov_set_engine_reset(int pf, unsigned int gt_num, bool value)
{
	return __xe_sriov_pf_debugfs_set_boolean(pf, 0, gt_num, "reset_engine", value);
}

/**
 * xe_sriov_set_engine_reset - Set the engine reset policy for a given GT
 * @pf: PF device file descriptor
 * @gt_num: GT number
 * @value: Engine reset policy status to set
 *
 * A throwing version of __xe_sriov_set_engine_reset().
 * Instead of returning an error code, it asserts in case of an error.
 */
void xe_sriov_set_engine_reset(int pf, unsigned int gt_num, bool value)
{
	igt_fail_on(__xe_sriov_set_engine_reset(pf, gt_num, value));
}

/**
 * __xe_sriov_set_sched_if_idle - Set the scheduling if idle policy status for a given GT
 * @pf: PF device file descriptor
 * @gt_num: GT number
 * @value: Scheduling if idle policy status to set
 *
 * Sets the scheduling if idle policy status for the given PF device @pf on GT @gt_num.
 *
 * Return: 0 on success, negative error code on failure.
 */
int __xe_sriov_set_sched_if_idle(int pf, unsigned int gt_num, bool value)
{
	return __xe_sriov_pf_debugfs_set_boolean(pf, 0, gt_num, "sched_if_idle", value);
}

/**
 * xe_sriov_set_sched_if_idle - Set the scheduling if idle status policy for a given GT
 * @pf: PF device file descriptor
 * @gt_num: GT number
 * @value: Scheduling if idle policy status to set
 *
 * A throwing version of __xe_sriov_set_sched_if_idle().
 * Instead of returning an error code, it asserts in case of an error.
 */
void xe_sriov_set_sched_if_idle(int pf, unsigned int gt_num, bool value)
{
	igt_fail_on(__xe_sriov_set_sched_if_idle(pf, gt_num, value));
}

static const char * const xe_sriov_sched_priority_str[] = {
	[XE_SRIOV_SCHED_PRIORITY_LOW]    = "low",
	[XE_SRIOV_SCHED_PRIORITY_NORMAL] = "normal",
	[XE_SRIOV_SCHED_PRIORITY_HIGH]   = "high",
};

_Static_assert(ARRAY_SIZE(xe_sriov_sched_priority_str) == (XE_SRIOV_SCHED_PRIORITY_HIGH + 1),
	       "sched priority table must cover 0..HIGH");

/**
 * xe_sriov_sched_priority_to_string - Convert scheduling priority enum to string
 * @prio: SR-IOV scheduling priority value
 *
 * Converts an enumeration value of type &enum xe_sriov_sched_priority
 * into its corresponding string representation.
 *
 * Return: A pointer to a constant string literal ("low", "normal", or "high"),
 * or %NULL if the value is invalid or unrecognized.
 */
const char *xe_sriov_sched_priority_to_string(enum xe_sriov_sched_priority prio)
{
	switch (prio) {
	case XE_SRIOV_SCHED_PRIORITY_LOW:
	case XE_SRIOV_SCHED_PRIORITY_NORMAL:
	case XE_SRIOV_SCHED_PRIORITY_HIGH:
		return xe_sriov_sched_priority_str[prio];
	}

	return NULL;
}

/**
 * xe_sriov_sched_priority_from_string - Parse scheduling priority from string
 * @s: NUL-terminated string to parse
 * @prio: Output pointer to store parsed enum value
 *
 * Parses a string representing a scheduling priority ("low", "normal", "high")
 * into the corresponding &enum xe_sriov_sched_priority value.
 *
 * Return: 0 on success, -EINVAL if the string is invalid or unrecognized.
 */
int xe_sriov_sched_priority_from_string(const char *s,
					enum xe_sriov_sched_priority *prio)
{
	igt_assert(s && prio);

	for (size_t i = 0; i < ARRAY_SIZE(xe_sriov_sched_priority_str); i++) {
		const char *name = xe_sriov_sched_priority_str[i];

		if (name && !strcmp(s, name)) {
			*prio = (enum xe_sriov_sched_priority)i;
			return 0;
		}
	}
	return -EINVAL;
}

/**
 * xe_sriov_sched_priority_choice_to_mask - Map parsed sysfs choice to mask + selection
 * @choice: Parsed choice (tokens + selected index)
 * @mask: Output bitmask of known priorities present in @choice
 * @selected_idx: Output selected priority index in the known-name table, or -1
 *
 * Converts an &struct igt_sysfs_choice representing the sched_priority sysfs
 * attribute into a bitmask and an optional selected index.
 *
 * The bit positions in @mask correspond to &enum xe_sriov_sched_priority values
 * (LOW/NORMAL/HIGH). Unknown tokens in @choice are ignored (best-effort), so
 * tests can tolerate kernels that add extra choices.
 *
 * Return: 0 on success, -EINVAL on invalid arguments.
 */
int xe_sriov_sched_priority_choice_to_mask(const struct igt_sysfs_choice *choice,
					   unsigned int *mask, int *selected_idx)
{
	return igt_sysfs_choice_to_mask(choice, xe_sriov_sched_priority_str,
					ARRAY_SIZE(xe_sriov_sched_priority_str),
					mask, selected_idx);
}

/**
 * xe_sriov_sched_priority_mask_to_string - Format priority mask as text
 * @buf: Output buffer.
 * @buf_sz: Size of @buf.
 * @mask: Priority bitmask.
 * @selected_idx: Index to highlight with brackets, or <0 for none.
 *
 * Converts @mask to a space-separated string of priority names. If @selected_idx
 * is >= 0 and present in @mask, that priority is wrapped in brackets, e.g.
 * "low [normal] high". An empty @mask results in an empty string.
 *
 * Return: 0 on success, -EINVAL on invalid args, -E2BIG if @buf_sz is too small.
 */
int xe_sriov_sched_priority_mask_to_string(char *buf, size_t buf_sz,
					   unsigned int mask, int selected_idx)
{
	return igt_sysfs_choice_format_mask(buf, buf_sz,
					 xe_sriov_sched_priority_str,
					 ARRAY_SIZE(xe_sriov_sched_priority_str),
					 mask, selected_idx);
}

/**
 * __xe_sriov_get_sched_priority - Get the scheduling priority for a given VF
 * @pf: PF device file descriptor
 * @vf_num: VF number (1-based) or 0 for PF
 * @gt_num: GT number
 * @value: Pointer to store the read scheduling priority
 *
 * Reads the scheduling priority for the given PF device @pf,
 * VF number @vf_num on GT @gt_num.
 *
 * Return: 0 on success, negative error code on failure.
 */
int __xe_sriov_get_sched_priority(int pf, unsigned int vf_num,
				  unsigned int gt_num,
				  enum xe_sriov_sched_priority *value)
{
	uint32_t priority;
	int ret;

	ret = __xe_sriov_pf_debugfs_get_u32(pf, vf_num, gt_num, "sched_priority", &priority);
	if (ret)
		return ret;

	if (priority > XE_SRIOV_SCHED_PRIORITY_HIGH)
		return -ERANGE;

	*value = priority;

	return 0;
}

/**
 * xe_sriov_get_sched_priority - Get the scheduling priority for a given VF
 * @pf: PF device file descriptor
 * @vf_num: VF number (1-based) or 0 for PF
 * @gt_num: GT number
 *
 * A throwing version of __xe_sriov_get_sched_priority().
 * Instead of returning an error code, it returns the scheduling priority
 * and asserts in case of an error.
 *
 * Return: The scheduling priority for the given VF and GT.
 *         Asserts in case of failure.
 */
enum xe_sriov_sched_priority
xe_sriov_get_sched_priority(int pf, unsigned int vf_num, unsigned int gt_num)
{
	enum xe_sriov_sched_priority priority;

	igt_fail_on(__xe_sriov_get_sched_priority(pf, vf_num, gt_num, &priority));

	return priority;
}

/**
 * __xe_sriov_set_sched_priority - Set the scheduling priority for a given VF
 * @pf: PF device file descriptor
 * @vf_num: VF number (1-based) or 0 for PF
 * @gt_num: GT number
 * @value: Scheduling priority to set (enum xe_sriov_sched_priority)
 *
 * Sets the scheduling priority for the given PF device @pf, VF number @vf_num on GT @gt_num.
 *
 * Return: 0 on success, negative error code on failure.
 */
int __xe_sriov_set_sched_priority(int pf, unsigned int vf_num, unsigned int gt_num,
				  enum xe_sriov_sched_priority value)
{
	return __xe_sriov_pf_debugfs_set_u32(pf, vf_num, gt_num, "sched_priority", value);
}

/**
 * xe_sriov_set_sched_priority - Set the scheduling priority for a given VF
 * @pf: PF device file descriptor
 * @vf_num: VF number (1-based) or 0 for PF
 * @gt_num: GT number
 * @value: Scheduling priority to set (enum xe_sriov_sched_priority)
 *
 * A throwing version of __xe_sriov_set_sched_priority().
 * Instead of returning an error code, it asserts in case of an error.
 */
void xe_sriov_set_sched_priority(int pf, unsigned int vf_num, unsigned int gt_num,
				 enum xe_sriov_sched_priority value)
{
	igt_fail_on(__xe_sriov_set_sched_priority(pf, vf_num, gt_num, value));
}

/**
 * xe_sriov_require_default_scheduling_attributes - Ensure default SR-IOV scheduling attributes
 * @pf_fd: PF device file descriptor
 *
 * Skips the current test if non-default SR-IOV scheduling attributes are set.
 *
 * Default scheduling attributes are as follows for each VF and PF:
 * - exec_quantum_ms equals zero (meaning infinity)
 * - preempt_timeout_us equals zero (meaning infinity)
 * - reset_engine equals false
 * - sched_priority equals XE_SRIOV_SCHED_PRIORITY_LOW
 */
void xe_sriov_require_default_scheduling_attributes(int pf)
{
	unsigned int totalvfs = igt_sriov_get_total_vfs(pf);
	enum xe_sriov_sched_priority sched_priority;
	bool reset_engine;
	uint32_t eq, pt;
	unsigned int gt;

	xe_for_each_gt(pf, gt) {
		igt_skip_on(__xe_sriov_get_engine_reset(pf, gt, &reset_engine));
		igt_require_f(!reset_engine, "reset_engine != false on gt%u\n", gt);

		for (unsigned int vf_num = 0; vf_num <= totalvfs; ++vf_num) {
			igt_skip_on(__xe_sriov_get_exec_quantum_ms(pf, vf_num, gt, &eq));
			igt_require_f(eq == 0, "exec_quantum_ms != 0 on gt%u/VF%u\n", gt, vf_num);

			igt_skip_on(__xe_sriov_get_preempt_timeout_us(pf, vf_num, gt, &pt));
			igt_require_f(pt == 0, "preempt_timeout_us != 0 on gt%u/VF%u\n",
				      gt, vf_num);

			igt_skip_on(__xe_sriov_get_sched_priority(pf, vf_num, gt, &sched_priority));
			igt_require_f(sched_priority == XE_SRIOV_SCHED_PRIORITY_LOW,
				      "sched_priority != LOW on gt%u/VF%u\n", gt, vf_num);
		}
	}
}

/**
 * xe_sriov_disable_vfs_restore_auto_provisioning - Disable all VFs and
 * request PF to restore its default auto-provisioning state.
 * @pf: PF device file descriptor.
 *
 * Convenience wrapper combining igt_sriov_disable_vfs() and
 * xe_sriov_pf_debugfs_restore_auto_provisioning(). Ensures that after
 * VF teardown the PF is reset to a clean provisioning state.
 */
void xe_sriov_disable_vfs_restore_auto_provisioning(int pf)
{
	igt_sriov_disable_vfs(pf);
	if (xe_sriov_pf_debugfs_supports_restore_auto_provisioning(pf))
		xe_sriov_pf_debugfs_restore_auto_provisioning(pf);
}
