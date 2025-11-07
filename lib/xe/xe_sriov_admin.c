// SPDX-License-Identifier: MIT
/*
 * Copyright(c) 2025 Intel Corporation. All rights reserved.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "igt.h"
#include "igt_sriov_device.h"
#include "igt_sysfs.h"
#include "igt_sysfs_choice.h"
#include "xe_sriov_admin.h"

static const char SRIOV_ADMIN[] = "device/sriov_admin";

static int fmt_profile_rel_path(char *buf, size_t sz, unsigned int vf_num,
				const char *attr)
{
	igt_assert(buf && attr && sz);

	return snprintf(buf, sz, "%s/%s/%s", SRIOV_ADMIN, igt_sriov_func_str(vf_num), attr);
}

static int fmt_bulk_rel_path(char *buf, size_t sz, const char *attr)
{
	igt_assert(buf && attr && sz);

	return snprintf(buf, sz, "%s/.bulk_profile/%s", SRIOV_ADMIN, attr);
}

static int ret_from_printf(int ret)
{
	return ret > 0 ? 0 : ret;
}

static int ret_from_scanf_items(int ret, int want_items)
{
	/* igt_sysfs_scanf: returns number of assigned items, or <0 on -errno */
	if (ret < 0)
		return ret;
	return (ret == want_items) ? 0 : -EIO;
}

/**
 * xe_sriov_admin_is_present - Check if SR-IOV admin sysfs interface is available
 * @pf_fd: PF device file descriptor.
 *
 * Returns: true if the PF exposes the SR-IOV admin tree, false otherwise.
 */
bool xe_sriov_admin_is_present(int pf_fd)
{
	int sysfs;
	bool ret;

	sysfs = igt_sysfs_open(pf_fd);
	if (sysfs < 0)
		return -1;

	ret = igt_sysfs_has_attr(sysfs, SRIOV_ADMIN);
	close(sysfs);
	return ret;
}

/**
 * __xe_sriov_admin_set_exec_quantum_ms - Set execution quantum for a VF
 * @pf_fd:   PF device file descriptor.
 * @vf_num:  VF index (0 for PF, >0 for VFs).
 * @eq_ms:   Execution quantum in milliseconds.
 *
 * Writes the new execution quantum to sysfs.
 *
 * Returns: 0 on success or negative errno on error.
 */
int __xe_sriov_admin_set_exec_quantum_ms(int pf_fd, unsigned int vf_num,
					 uint32_t eq_ms)
{
	char path[PATH_MAX];
	int sysfs;
	bool ret;

	sysfs = igt_sysfs_open(pf_fd);
	if (sysfs < 0)
		return sysfs;

	fmt_profile_rel_path(path, sizeof(path), vf_num, "profile/exec_quantum_ms");
	ret = igt_sysfs_printf(sysfs, path, "%u", eq_ms);
	close(sysfs);

	return ret_from_printf(ret);
}

/**
 * xe_sriov_admin_set_exec_quantum_ms - Assert wrapper for setting VF execution quantum
 * @pf_fd:   PF device file descriptor.
 * @vf_num:  VF index (0 for PF, >0 for VFs).
 * @eq_ms:   Execution quantum in milliseconds.
 *
 * Calls __xe_sriov_admin_set_exec_quantum_ms() and asserts on error.
 */
void xe_sriov_admin_set_exec_quantum_ms(int pf_fd, unsigned int vf_num, uint32_t eq_ms)
{
	igt_assert_eq(0, __xe_sriov_admin_set_exec_quantum_ms(pf_fd, vf_num, eq_ms));
}

/**
 * __xe_sriov_admin_get_exec_quantum_ms - Read execution quantum for a VF
 * @pf_fd:   PF device file descriptor.
 * @vf_num:  VF index (0 for PF, >0 for VFs).
 * @eq_ms:   Output pointer for the execution quantum (ms).
 *
 * Reads current VF execution quantum from sysfs.
 *
 * Returns: 0 on success or negative errno on error.
 */
int __xe_sriov_admin_get_exec_quantum_ms(int pf_fd, unsigned int vf_num, uint32_t *eq_ms)
{
	char path[PATH_MAX];
	unsigned int val = 0;
	int sysfs, ret;

	sysfs = igt_sysfs_open(pf_fd);
	if (sysfs < 0)
		return sysfs;

	fmt_profile_rel_path(path, sizeof(path), vf_num, "profile/exec_quantum_ms");
	ret = igt_sysfs_scanf(sysfs, path, "%u", &val);
	close(sysfs);

	ret = ret_from_scanf_items(ret, 1);
	if (ret)
		return ret;

	*eq_ms = val;
	return 0;
}

/**
 * xe_sriov_admin_get_exec_quantum_ms - Assert wrapper for reading VF execution quantum
 * @pf_fd:   PF device file descriptor.
 * @vf_num:  VF index (0 for PF, >0 for VFs).
 *
 * Returns: execution quantum (ms); asserts on error.
 */
uint32_t xe_sriov_admin_get_exec_quantum_ms(int pf_fd, unsigned int vf_num)
{
	uint32_t v = 0;

	igt_assert_eq(0, __xe_sriov_admin_get_exec_quantum_ms(pf_fd, vf_num, &v));

	return v;
}

/**
 * __xe_sriov_admin_set_preempt_timeout_us - Set preemption timeout for a VF
 * @pf_fd:   PF device file descriptor.
 * @vf_num:  VF index (0 for PF, >0 for VFs).
 * @pt_us:   Preemption timeout in microseconds.
 *
 * Returns: 0 on success or negative errno on error.
 */
int __xe_sriov_admin_set_preempt_timeout_us(int pf_fd, unsigned int vf_num, uint32_t pt_us)
{
	char path[PATH_MAX];
	int sysfs, ret;

	sysfs = igt_sysfs_open(pf_fd);
	if (sysfs < 0)
		return sysfs;

	fmt_profile_rel_path(path, sizeof(path), vf_num, "profile/preempt_timeout_us");
	ret = igt_sysfs_printf(sysfs, path, "%u", pt_us);
	close(sysfs);

	return ret_from_printf(ret);
}

/**
 * xe_sriov_admin_set_preempt_timeout_us - Assert wrapper for setting VF preemption timeout
 * @pf_fd:   PF device file descriptor.
 * @vf_num:  VF index (0 for PF, >0 for VFs).
 * @pt_us:   Preemption timeout in microseconds.
 */
void xe_sriov_admin_set_preempt_timeout_us(int pf_fd, unsigned int vf_num, uint32_t pt_us)
{
	igt_assert_eq(0, __xe_sriov_admin_set_preempt_timeout_us(pf_fd, vf_num, pt_us));
}

/**
 * __xe_sriov_admin_get_preempt_timeout_us - Read preemption timeout for a VF
 * @pf_fd:   PF device file descriptor.
 * @vf_num:  VF index (0 for PF, >0 for VFs).
 * @pt_us:   Output pointer for preemption timeout (µs).
 *
 * Returns: 0 on success or negative errno on error.
 */
int __xe_sriov_admin_get_preempt_timeout_us(int pf_fd, unsigned int vf_num,
					    uint32_t *pt_us)
{
	char path[PATH_MAX];
	unsigned int val = 0;
	int sysfs, ret;

	sysfs = igt_sysfs_open(pf_fd);
	if (sysfs < 0)
		return sysfs;

	fmt_profile_rel_path(path, sizeof(path), vf_num,
			     "profile/preempt_timeout_us");
	ret = igt_sysfs_scanf(sysfs, path, "%u", &val);
	close(sysfs);

	ret = ret_from_scanf_items(ret, 1);
	if (ret)
		return ret;
	*pt_us = val;
	return 0;
}

/**
 * xe_sriov_admin_get_preempt_timeout_us - Assert wrapper for reading VF preemption timeout
 * @pf_fd:   PF device file descriptor.
 * @vf_num:  VF index (0 for PF, >0 for VFs).
 *
 * Returns: preemption timeout (µs); asserts on error.
 */
uint32_t xe_sriov_admin_get_preempt_timeout_us(int pf_fd, unsigned int vf_num)
{
	uint32_t v = 0;

	igt_assert_eq(0, __xe_sriov_admin_get_preempt_timeout_us(pf_fd, vf_num, &v));
	return v;
}

/**
 * __xe_sriov_admin_set_sched_priority_string - Set VF priority from string
 * @pf_fd:   PF device file descriptor.
 * @vf_num:  VF index (0 for PF, >0 for VFs).
 * @prio:    String value ("low", "normal", "high").
 *
 * Returns: 0 on success or negative errno on error.
 */
int __xe_sriov_admin_set_sched_priority_string(int pf_fd, unsigned int vf_num,
					       const char *prio)
{
	char path[PATH_MAX];
	int sysfs, ret;

	if (!prio)
		return -EINVAL;

	sysfs = igt_sysfs_open(pf_fd);
	if (sysfs < 0)
		return sysfs;

	fmt_profile_rel_path(path, sizeof(path), vf_num,
			     "profile/sched_priority");
	ret = igt_sysfs_printf(sysfs, path, "%s", prio);
	close(sysfs);
	return ret_from_printf(ret);
}

/**
 * __xe_sriov_admin_set_sched_priority - Set VF scheduling priority
 * @pf_fd:   PF device file descriptor.
 * @vf_num:  VF index (0 for PF, >0 for VFs).
 * @prio:    Priority enum value.
 *
 * Returns: 0 on success or negative errno on error.
 */
int __xe_sriov_admin_set_sched_priority(int pf_fd, unsigned int vf_num,
					enum xe_sriov_sched_priority prio)
{
	const char *p = xe_sriov_sched_priority_to_string(prio);

	return __xe_sriov_admin_set_sched_priority_string(pf_fd, vf_num, p);
}

/**
 * xe_sriov_admin_set_sched_priority - Assert wrapper for setting VF priority
 * @pf_fd:   PF device file descriptor.
 * @vf_num:  VF index (0 for PF, >0 for VFs).
 * @prio:    Priority enum value.
 */
void xe_sriov_admin_set_sched_priority(int pf_fd, unsigned int vf_num,
				       enum xe_sriov_sched_priority prio)
{
	igt_assert_eq(0, __xe_sriov_admin_set_sched_priority(pf_fd, vf_num, prio));
}

/**
 * __xe_sriov_admin_get_sched_priority_choice - Read sched_priority tokens
 * @pf_fd:   PF device file descriptor
 * @vf_num:  VF index (0 for PF, >0 for VFs).
 * @choice:  Output choice structure with parsed tokens and selected index
 *
 * Reads the sched_priority sysfs attribute for the given PF/VF and parses it
 * into an igt_sysfs_choice.
 *
 * Returns: 0 on success or a negative errno code.
 */
int __xe_sriov_admin_get_sched_priority_choice(int pf_fd, unsigned int vf_num,
					       struct igt_sysfs_choice *choice)
{
	char path[PATH_MAX];
	int sysfs, ret;

	sysfs = igt_sysfs_open(pf_fd);
	if (sysfs < 0)
		return sysfs;

	fmt_profile_rel_path(path, sizeof(path), vf_num, "profile/sched_priority");
	ret = igt_sysfs_choice_read(sysfs, path, choice);
	close(sysfs);

	return ret;
}

/**
 * __xe_sriov_admin_get_sched_priority - Read VF scheduling priority + mask
 * @pf_fd:     PF device file descriptor.
 * @vf_num:    VF index (0 for PF, >0 for VFs).
 * @prio:      Output pointer for the effective priority.
 * @prio_mask: Output mask of allowed priorities.
 *
 * Returns: 0 on success or negative errno on error.
 */
int __xe_sriov_admin_get_sched_priority(int pf_fd, unsigned int vf_num,
					enum xe_sriov_sched_priority *prio,
					unsigned int *prio_mask)
{
	struct igt_sysfs_choice prio_ch = {};
	int ret;

	ret = __xe_sriov_admin_get_sched_priority_choice(pf_fd, vf_num, &prio_ch);
	if (ret)
		return ret;

	ret = xe_sriov_sched_priority_from_string(prio_ch.tokens[prio_ch.selected], prio);
	if (igt_debug_on_f(ret, "unknown selected value '%s' (err=%d)\n",
			   prio_ch.tokens[prio_ch.selected], ret))
		return ret;

	if (prio_mask) {
		ret = xe_sriov_sched_priority_choice_to_mask(&prio_ch, prio_mask, NULL);
		if (igt_debug_on_f(ret, "mask conversion failed (err=%d)\n", ret))
			return ret;
	}

	return 0;
}

/**
 * xe_sriov_admin_get_sched_priority - Assert wrapper for reading VF priority
 * @pf_fd:     PF device file descriptor.
 * @vf_num:    VF index (0 for PF, >0 for VFs).
 * @prio_mask: Output mask of supported priorities.
 *
 * Returns: effective priority; asserts on error.
 */
enum xe_sriov_sched_priority
xe_sriov_admin_get_sched_priority(int pf_fd, unsigned int vf_num,
				  unsigned int *prio_mask)
{
	enum xe_sriov_sched_priority cur_prio;

	igt_assert_eq(0,
		      __xe_sriov_admin_get_sched_priority(pf_fd, vf_num, &cur_prio, prio_mask));

	return cur_prio;
}

/**
 * __xe_sriov_admin_bulk_set_exec_quantum_ms - Set execution quantum for PF and all VFs
 * @pf_fd: PF device file descriptor.
 * @eq_ms: Execution quantum in milliseconds.
 *
 * Applies the value to PF and all VFs.
 *
 * Returns: 0 on success or negative errno on error.
 */
int __xe_sriov_admin_bulk_set_exec_quantum_ms(int pf_fd, uint32_t eq_ms)
{
	char path[PATH_MAX];
	int sysfs, ret;

	sysfs = igt_sysfs_open(pf_fd);
	if (sysfs < 0)
		return sysfs;

	fmt_bulk_rel_path(path, sizeof(path), "exec_quantum_ms");
	ret = igt_sysfs_printf(sysfs, path, "%u", eq_ms);
	close(sysfs);

	return ret_from_printf(ret);
}

/**
 * xe_sriov_admin_bulk_set_exec_quantum_ms - Assert wrapper for bulk execution quantum update
 * @pf_fd: PF device file descriptor.
 * @eq_ms: Execution quantum in milliseconds.
 */
void xe_sriov_admin_bulk_set_exec_quantum_ms(int pf_fd, uint32_t eq_ms)
{
	igt_assert_eq(0, __xe_sriov_admin_bulk_set_exec_quantum_ms(pf_fd, eq_ms));
}

/**
 * __xe_sriov_admin_bulk_set_preempt_timeout_us - Set preemption timeout for PF and all VFs
 * @pf_fd: PF device file descriptor.
 * @pt_us: Preemption timeout in microseconds.
 *
 * Applies the value to PF and all VFs.
 *
 * Returns: 0 on success or negative errno on error.
 */
int __xe_sriov_admin_bulk_set_preempt_timeout_us(int pf_fd, uint32_t pt_us)
{
	char path[PATH_MAX];
	int sysfs, ret;

	sysfs = igt_sysfs_open(pf_fd);
	if (sysfs < 0)
		return sysfs;

	fmt_bulk_rel_path(path, sizeof(path), "preempt_timeout_us");
	ret = igt_sysfs_printf(sysfs, path, "%u", pt_us);
	close(sysfs);

	return ret_from_printf(ret);
}

/**
 * xe_sriov_admin_bulk_set_preempt_timeout_us - Assert wrapper for bulk preemption timeout update
 * @pf_fd: PF device file descriptor.
 * @pt_us: Preemption timeout in microseconds.
 */
void xe_sriov_admin_bulk_set_preempt_timeout_us(int pf_fd, uint32_t pt_us)
{
	igt_assert_eq(0, __xe_sriov_admin_bulk_set_preempt_timeout_us(pf_fd, pt_us));
}

/**
 * __xe_sriov_admin_bulk_set_sched_priority_string - Set scheduling priority for PF and all VFs
 * @pf_fd: PF device file descriptor.
 * @prio:  String priority ("low", "normal", "high").
 *
 * Returns: 0 on success or negative errno on error.
 */
int __xe_sriov_admin_bulk_set_sched_priority_string(int pf_fd, const char *prio)
{
	char path[PATH_MAX];
	int sysfs, ret;

	sysfs = igt_sysfs_open(pf_fd);
	if (sysfs < 0)
		return sysfs;

	fmt_bulk_rel_path(path, sizeof(path), "sched_priority");
	ret = igt_sysfs_printf(sysfs, path, "%s", prio);
	close(sysfs);

	return ret_from_printf(ret);
}

/**
 * xe_sriov_admin_bulk_set_sched_priority_string - Assert wrapper for bulk priority update
 * @pf_fd: PF device file descriptor.
 * @prio:  String priority.
 */
void xe_sriov_admin_bulk_set_sched_priority_string(int pf_fd, const char *prio)
{
	igt_assert_eq(0, __xe_sriov_admin_bulk_set_sched_priority_string(pf_fd, prio));
}

/**
 * __xe_sriov_admin_bulk_set_sched_priority - Set numeric priority for PF and all VFs
 * @pf_fd: PF device file descriptor.
 * @prio:  Enum priority value.
 *
 * Returns: 0 on success or negative errno on error.
 */
int __xe_sriov_admin_bulk_set_sched_priority(int pf_fd,
					     enum xe_sriov_sched_priority prio)
{
	const char *s = xe_sriov_sched_priority_to_string(prio);

	if (!s)
		return -EINVAL;
	return __xe_sriov_admin_bulk_set_sched_priority_string(pf_fd, s);
}

/**
 * xe_sriov_admin_bulk_set_sched_priority - Assert wrapper for bulk priority update
 * @pf_fd: PF device file descriptor.
 * @prio:  Enum priority value.
 */
void xe_sriov_admin_bulk_set_sched_priority(int pf_fd,
					    enum xe_sriov_sched_priority prio)
{
	igt_assert_eq(0, __xe_sriov_admin_bulk_set_sched_priority(pf_fd, prio));
}

/**
 * __xe_sriov_admin_vf_stop - Issue stop command for a VF
 * @pf_fd:  PF device file descriptor.
 * @vf_num: VF index.
 *
 * Triggers VF stop via sysfs.
 *
 * Returns: 0 on success or negative errno on error.
 */
int __xe_sriov_admin_vf_stop(int pf_fd, unsigned int vf_num)
{
	char path[PATH_MAX];
	int sysfs, ret;

	sysfs = igt_sysfs_open(pf_fd);
	if (sysfs < 0)
		return sysfs;

	fmt_profile_rel_path(path, sizeof(path), vf_num, "stop");
	ret = igt_sysfs_printf(sysfs, path, "%u", 1u);
	close(sysfs);

	return ret_from_printf(ret);
}

/**
 * xe_sriov_admin_vf_stop - Assert wrapper for VF stop command
 * @pf_fd:  PF device file descriptor.
 * @vf_num: VF index.
 */
void xe_sriov_admin_vf_stop(int pf_fd, unsigned int vf_num)
{
	igt_assert_eq(0, __xe_sriov_admin_vf_stop(pf_fd, vf_num));
}

/**
 * __xe_sriov_admin_restore_defaults - Restore scheduling defaults for a VF
 * @pf_fd:  PF device file descriptor.
 * @vf_num: VF index (0 for PF, >0 for VFs).
 *
 * Resets execution quantum, preemption timeout, and priority to driver defaults.
 *
 * Returns: 0 on success or negative errno on error.
 */
int __xe_sriov_admin_restore_defaults(int pf_fd, unsigned int vf_num)
{
	int ret_eq, ret_pt, ret_prio;
	int ret = 0;

	ret_eq = __xe_sriov_admin_set_exec_quantum_ms(pf_fd, vf_num, 0);
	igt_warn_on(ret_eq);
	if (!ret)
		ret = ret_eq;

	ret_pt = __xe_sriov_admin_set_preempt_timeout_us(pf_fd, vf_num, 0);
	igt_warn_on(ret_pt);
	if (!ret)
		ret = ret_pt;

	ret_prio = __xe_sriov_admin_set_sched_priority(pf_fd, vf_num,
						       XE_SRIOV_SCHED_PRIORITY_LOW);
	igt_warn_on(ret_prio);
	if (!ret)
		ret = ret_prio;

	return ret;
}

/**
 * xe_sriov_admin_restore_defaults - Assert wrapper restoring VF defaults
 * @pf_fd:  PF device file descriptor.
 * @vf_num: VF index (0 for PF, >0 for VFs).
 */
void xe_sriov_admin_restore_defaults(int pf_fd, unsigned int vf_num)
{
	igt_assert_eq(0, __xe_sriov_admin_restore_defaults(pf_fd, vf_num));
}

/**
 * __xe_sriov_admin_bulk_restore_defaults - Restore scheduling defaults for PF and all VFs
 * @pf_fd: PF device file descriptor.
 *
 * Resets PF and all VFs to driver default scheduling parameters.
 *
 * Returns: 0 on success or negative errno on error.
 */
int __xe_sriov_admin_bulk_restore_defaults(int pf_fd)
{
	int ret_eq, ret_pt, ret_prio;
	int ret = 0;

	ret_eq = __xe_sriov_admin_bulk_set_exec_quantum_ms(pf_fd, 0);
	igt_warn_on(ret_eq);
	if (!ret)
		ret = ret_eq;

	ret_pt = __xe_sriov_admin_bulk_set_preempt_timeout_us(pf_fd, 0);
	igt_warn_on(ret_pt);
	if (!ret)
		ret = ret_pt;

	ret_prio = __xe_sriov_admin_bulk_set_sched_priority(pf_fd,
							    XE_SRIOV_SCHED_PRIORITY_LOW);
	igt_warn_on(ret_prio);
	if (!ret)
		ret = ret_prio;

	return ret;
}

/**
 * xe_sriov_admin_bulk_restore_defaults - Assert wrapper for restoring defaults on PF and all VFs
 * @pf_fd: PF device file descriptor.
 */
void xe_sriov_admin_bulk_restore_defaults(int pf_fd)
{
	igt_assert_eq(0, __xe_sriov_admin_bulk_restore_defaults(pf_fd));
}
