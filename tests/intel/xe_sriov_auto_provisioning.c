// SPDX-License-Identifier: MIT
/*
 * Copyright(c) 2023 Intel Corporation. All rights reserved.
 */

#include <stdbool.h>

#include "drmtest.h"
#include "igt_core.h"
#include "igt_sriov_device.h"
#include "igt_sysfs.h"
#include "xe/xe_sriov_debugfs.h"
#include "xe/xe_sriov_provisioning.h"
#include "xe/xe_query.h"

/**
 * TEST: xe_sriov_auto_provisioning
 * Category: Core
 * Mega feature: SR-IOV
 * Sub-category: provisioning
 * Functionality: auto-provisioning
 * Description: Examine behavior of SR-IOV auto-provisioning
 * Run type: FULL
 *
 * SUBTEST: fair-allocation
 * Description: Verify that auto-provisioned resources are allocated by
 *		PF driver in fairly manner
 *
 * SUBTEST: resources-released-on-vfs-disabling
 * Description: Verify that auto-provisioned resources are released
 *		once VFs are disabled
 *
 * SUBTEST: exclusive-ranges
 * Description: Verify that ranges of auto-provisioned resources are exclusive
 *
 * SUBTEST: selfconfig-basic
 * Description: Check if VF configuration data is the same as provisioned
 *
 * SUBTEST: selfconfig-reprovision-increase-numvfs
 * Description: Check if VF configuration data is updated properly after
 *		increasing number of VFs
 *
 * SUBTEST: selfconfig-reprovision-reduce-numvfs
 * Description: Check if VF configuration data is updated properly after
 *		decreasing number of VFs
 */

IGT_TEST_DESCRIPTION("Xe tests for SR-IOV auto-provisioning");

/* Expects ranges sorted by VF IDs */
static int ranges_fair_allocation(enum xe_sriov_shared_res res,
				  struct xe_sriov_provisioned_range *ranges,
				  unsigned int nr_ranges)
{
	uint64_t expected_allocation = ranges[0].end - ranges[0].start + 1;

	for (unsigned int i = 1; i < nr_ranges; i++) {
		uint64_t current_allocation = ranges[i].end - ranges[i].start + 1;

		if (igt_debug_on_f(current_allocation != expected_allocation,
				   "%s: Allocation mismatch, expected=%" PRIu64 " VF%u=%" PRIu64 "\n",
				   xe_sriov_debugfs_provisioned_attr_name(res),
				   expected_allocation, ranges[i].vf_id,
				   current_allocation)) {
			return -1;
		}
	}

	return 0;
}

static int check_fair_allocation(int pf_fd, unsigned int num_vfs, unsigned int gt_id,
				 enum xe_sriov_shared_res res)
{
	struct xe_sriov_provisioned_range *ranges;
	int ret;

	ret = xe_sriov_pf_debugfs_read_check_ranges(pf_fd, res, gt_id, &ranges, num_vfs);
	if (igt_debug_on_f(ret, "%s: Failed ranges check on GT%u (%d)\n",
			   xe_sriov_debugfs_provisioned_attr_name(res), gt_id, ret))
		return ret;

	ret = ranges_fair_allocation(res, ranges, num_vfs);
	if (ret) {
		free(ranges);
		return ret;
	}

	free(ranges);

	return 0;
}

static void fair_allocation(int pf_fd, unsigned int num_vfs)
{
	enum xe_sriov_shared_res res;
	unsigned int gt;
	int fails = 0;

	igt_sriov_disable_driver_autoprobe(pf_fd);
	igt_sriov_enable_vfs(pf_fd, num_vfs);

	xe_for_each_gt(pf_fd, gt) {
		xe_sriov_for_each_provisionable_shared_res(res, pf_fd, gt) {
			if (igt_debug_on_f(check_fair_allocation(pf_fd, num_vfs, gt, res),
					   "%s fair allocation failed on gt%u\n",
					   xe_sriov_shared_res_to_string(res), gt))
				fails++;
		}
	}

	igt_sriov_disable_vfs(pf_fd);

	igt_fail_on_f(fails, "fair allocation failed\n");
}

static void resources_released_on_vfs_disabling(int pf_fd, unsigned int num_vfs)
{
	struct xe_sriov_provisioned_range *ranges;
	enum xe_sriov_shared_res res;
	unsigned int gt;
	int fails = 0;

	igt_sriov_disable_driver_autoprobe(pf_fd);
	igt_sriov_enable_vfs(pf_fd, num_vfs);

	xe_for_each_gt(pf_fd, gt) {
		xe_sriov_for_each_provisionable_shared_res(res, pf_fd, gt) {
			if (igt_warn_on_f(xe_sriov_pf_debugfs_read_check_ranges(pf_fd, res,
										gt,
										&ranges,
										num_vfs),
					  "%s: Failed ranges check on gt%u\n",
					  xe_sriov_debugfs_provisioned_attr_name(res), gt))
				continue;

			free(ranges);
		}
	}

	igt_sriov_disable_vfs(pf_fd);

	xe_for_each_gt(pf_fd, gt) {
		xe_sriov_for_each_provisionable_shared_res(res, pf_fd, gt) {
			if (igt_debug_on_f(xe_sriov_pf_debugfs_read_check_ranges(pf_fd, res,
										 gt,
										 &ranges,
										 0),
					   "%s: Failed ranges check on gt%u\n",
					   xe_sriov_debugfs_provisioned_attr_name(res), gt))
				fails++;
		}
	}

	igt_fail_on_f(fails, "shared resource release check failed\n");
}

static int compare_ranges_by_start(const void *a, const void *b)
{
	const struct xe_sriov_provisioned_range *range_a = a;
	const struct xe_sriov_provisioned_range *range_b = b;

	if (range_a->start < range_b->start)
		return -1;
	if (range_a->start > range_b->start)
		return 1;
	return 0;
}

static int check_no_overlap(int pf_fd, unsigned int num_vfs, unsigned int gt_id,
			    enum xe_sriov_shared_res res)
{
	struct xe_sriov_provisioned_range *ranges;
	int ret;

	ret = xe_sriov_pf_debugfs_read_check_ranges(pf_fd, res, gt_id, &ranges, num_vfs);
	if (ret)
		return ret;

	igt_assert(ranges);
	qsort(ranges, num_vfs, sizeof(ranges[0]), compare_ranges_by_start);

	for (unsigned int i = 0; i < num_vfs - 1; i++)
		if (ranges[i].end >= ranges[i + 1].start) {
			igt_debug((res == XE_SRIOV_SHARED_RES_GGTT) ?
				  "Overlapping ranges: VF%u [%" PRIx64 "-%" PRIx64 "] and VF%u [%" PRIx64 "-%" PRIx64 "]\n" :
				  "Overlapping ranges: VF%u [%" PRIu64 "-%" PRIu64 "] and VF%u [%" PRIu64 "-%" PRIu64 "]\n",
				  ranges[i].vf_id, ranges[i].start, ranges[i].end,
				  ranges[i + 1].vf_id, ranges[i + 1].start, ranges[i + 1].end);
			free(ranges);
			return -1;
		}

	free(ranges);

	return 0;
}

static void exclusive_ranges(int pf_fd, unsigned int num_vfs)
{
	enum xe_sriov_shared_res res;
	unsigned int gt;
	int fails = 0;

	igt_sriov_disable_driver_autoprobe(pf_fd);
	igt_sriov_enable_vfs(pf_fd, num_vfs);

	xe_for_each_gt(pf_fd, gt) {
		xe_sriov_for_each_provisionable_shared_res(res, pf_fd, gt) {
			if (res == XE_SRIOV_SHARED_RES_LMEM)
				/*
				 * lmem_provisioned is not applicable for this test,
				 * as it does not expose ranges
				 */
				continue;

			if (igt_debug_on_f(check_no_overlap(pf_fd, num_vfs, gt, res),
					   "%s overlap check failed on gt%u\n",
					   xe_sriov_shared_res_to_string(res), gt))
				fails++;
		}
	}

	igt_sriov_disable_vfs(pf_fd);

	igt_fail_on_f(fails, "exclusive ranges check failed\n");
}

#define REPROVISION_INCREASE_NUMVFS	(0x1 << 0)
#define REPROVISION_REDUCE_NUMVFS	(0x1 << 1)

static void check_selfconfig(int pf_fd, unsigned int vf_num, unsigned int flags)
{
	unsigned int gt_num, total_vfs = igt_sriov_get_total_vfs(pf_fd);
	uint64_t provisioned, queried;
	enum xe_sriov_shared_res res;
	int vf_fd, fails = 0;

	igt_sriov_disable_driver_autoprobe(pf_fd);
	igt_sriov_enable_vfs(pf_fd, (flags & REPROVISION_REDUCE_NUMVFS) ? total_vfs : vf_num);
	igt_sriov_enable_driver_autoprobe(pf_fd);

	igt_sriov_bind_vf_drm_driver(pf_fd, vf_num);
	vf_fd = igt_sriov_open_vf_drm_device(pf_fd, vf_num);
	igt_assert_fd(vf_fd);

	xe_for_each_gt(pf_fd, gt_num) {
		xe_sriov_for_each_provisionable_shared_res(res, pf_fd, gt_num) {
			provisioned = xe_sriov_pf_get_provisioned_quota(pf_fd, res, vf_num,
									gt_num);
			queried = xe_sriov_vf_debugfs_get_selfconfig(vf_fd, res, gt_num);

			if (igt_debug_on_f(provisioned != queried,
					   "%s selfconfig check failed on gt%u\n",
					   xe_sriov_shared_res_to_string(res), gt_num))
				fails++;
		}
	}

	close(vf_fd);
	igt_sriov_disable_vfs(pf_fd);

	if (flags && !fails) {
		igt_sriov_disable_driver_autoprobe(pf_fd);
		igt_sriov_enable_vfs(pf_fd, (flags & REPROVISION_INCREASE_NUMVFS) ? total_vfs :
										    vf_num);
		igt_sriov_enable_driver_autoprobe(pf_fd);

		igt_sriov_bind_vf_drm_driver(pf_fd, vf_num);
		vf_fd = igt_sriov_open_vf_drm_device(pf_fd, vf_num);
		igt_assert_fd(vf_fd);

		xe_for_each_gt(pf_fd, gt_num) {
			xe_sriov_for_each_provisionable_shared_res(res, pf_fd, gt_num) {
				provisioned = xe_sriov_pf_get_provisioned_quota(pf_fd, res, vf_num,
										gt_num);
				queried = xe_sriov_vf_debugfs_get_selfconfig(vf_fd, res, gt_num);

				if (igt_debug_on_f(provisioned != queried,
						   "%s selfconfig check after reprovisioning failed on gt%u\n",
						   xe_sriov_shared_res_to_string(res), gt_num))
					fails++;
			}
		}

		close(vf_fd);
		igt_sriov_disable_vfs(pf_fd);
	}

	igt_fail_on_f(fails, "selfconfig check failed\n");
}

static bool extended_scope;

static int opts_handler(int opt, int opt_index, void *data)
{
	switch (opt) {
	case 'e':
		extended_scope = true;
		break;
	default:
		return IGT_OPT_HANDLER_ERROR;
	}

	return IGT_OPT_HANDLER_SUCCESS;
}

static const struct option long_opts[] = {
	{ .name = "extended", .has_arg = false, .val = 'e', },
	{}
};

static const char help_str[] =
	"  --extended\tRun the extended test scope\n";

int igt_main_args("", long_opts, help_str, opts_handler, NULL)
{
	enum xe_sriov_shared_res res;
	unsigned int gt, total_vfs;
	bool autoprobe;
	int pf_fd;
	static struct subtest_variants {
		const char *name;
		unsigned int flags;
	} reprovisioning_variant[] = {
		{ "increase", REPROVISION_INCREASE_NUMVFS },
		{ "reduce", REPROVISION_REDUCE_NUMVFS },
		{ NULL },
	};

	igt_fixture() {
		struct xe_sriov_provisioned_range *ranges;
		int ret;

		pf_fd = drm_open_driver(DRIVER_XE);
		igt_require(igt_sriov_is_pf(pf_fd));
		igt_require(igt_sriov_get_enabled_vfs(pf_fd) == 0);

		xe_for_each_gt(pf_fd, gt) {
			xe_sriov_for_each_provisionable_shared_res(res, pf_fd, gt) {
				ret = xe_sriov_pf_debugfs_read_check_ranges(pf_fd, res, gt,
									    &ranges, 0);
				igt_skip_on_f(ret, "%s: Failed ranges check on gt%u (%d)\n",
					      xe_sriov_debugfs_provisioned_attr_name(res),
					      gt, ret);
			}
		}
		autoprobe = igt_sriov_is_driver_autoprobe_enabled(pf_fd);
		total_vfs = igt_sriov_get_total_vfs(pf_fd);
	}

	igt_describe("Verify that auto-provisioned resources are allocated by PF driver in fairly manner");
	igt_subtest_with_dynamic("fair-allocation") {
		if (extended_scope)
			for_each_sriov_num_vfs(pf_fd, num_vfs)
				igt_dynamic_f("numvfs-%d", num_vfs)
					fair_allocation(pf_fd, num_vfs);

		for_random_sriov_num_vfs(pf_fd, num_vfs) {
			igt_dynamic_f("numvfs-random") {
				igt_debug("numvfs=%u\n", num_vfs);
				fair_allocation(pf_fd, num_vfs);
			}
		}
	}

	igt_describe("Verify that auto-provisioned resources are released once VFs are disabled");
	igt_subtest_with_dynamic("resources-released-on-vfs-disabling") {
		if (extended_scope)
			for_each_sriov_num_vfs(pf_fd, num_vfs)
				igt_dynamic_f("numvfs-%d", num_vfs)
					resources_released_on_vfs_disabling(pf_fd, num_vfs);

		for_random_sriov_num_vfs(pf_fd, num_vfs) {
			igt_dynamic_f("numvfs-random") {
				igt_debug("numvfs=%u\n", num_vfs);
				resources_released_on_vfs_disabling(pf_fd, num_vfs);
			}
		}
	}

	igt_describe("Verify that ranges of auto-provisioned resources are exclusive");
	igt_subtest_with_dynamic_f("exclusive-ranges") {

		igt_skip_on(total_vfs < 2);

		if (extended_scope)
			for_each_sriov_num_vfs(pf_fd, num_vfs)
				igt_dynamic_f("numvfs-%d", num_vfs)
					exclusive_ranges(pf_fd, num_vfs);

		for_random_sriov_vf_in_range(pf_fd, 2, total_vfs, num_vfs) {
			igt_dynamic_f("numvfs-random") {
				igt_debug("numvfs=%u\n", num_vfs);
				exclusive_ranges(pf_fd, num_vfs);
			}
		}
	}

	igt_describe("Check if VF configuration data is the same as provisioned");
	igt_subtest_with_dynamic("selfconfig-basic") {
		if (extended_scope)
			for_each_sriov_vf(pf_fd, vf)
				igt_dynamic_f("vf-%u", vf)
					check_selfconfig(pf_fd, vf, 0);

		for_random_sriov_vf(pf_fd, vf) {
			igt_dynamic_f("vf-random") {
				igt_debug("vf=%u\n", vf);
				check_selfconfig(pf_fd, vf, 0);
			}
		}
	}

	for (const struct subtest_variants *s = reprovisioning_variant; s->name; s++) {
		igt_describe("Check if VF configuration data is the same as reprovisioned");
		igt_subtest_with_dynamic_f("selfconfig-reprovision-%s-numvfs", s->name) {

			igt_require(total_vfs > 1);

			if (extended_scope)
				for_each_sriov_vf_in_range(pf_fd, 1, total_vfs - 1, vf)
					igt_dynamic_f("vf-%u", vf)
						check_selfconfig(pf_fd, vf, s->flags);

			for_random_sriov_vf_in_range(pf_fd, 1, total_vfs - 1, vf) {
				igt_dynamic_f("vf-random") {
					igt_debug("vf=%u\n", vf);
					check_selfconfig(pf_fd, vf, s->flags);
				}
			}
		}
	}

	igt_fixture() {
		igt_sriov_disable_vfs(pf_fd);
		/* abort to avoid execution of next tests with enabled VFs */
		igt_abort_on_f(igt_sriov_get_enabled_vfs(pf_fd) > 0, "Failed to disable VF(s)");
		autoprobe ? igt_sriov_enable_driver_autoprobe(pf_fd) :
			    igt_sriov_disable_driver_autoprobe(pf_fd);
		igt_abort_on_f(autoprobe != igt_sriov_is_driver_autoprobe_enabled(pf_fd),
			       "Failed to restore sriov_drivers_autoprobe value\n");
		drm_close_driver(pf_fd);
	}
}
