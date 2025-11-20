// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

#include <fcntl.h>

#include "i915/gem.h"
#include "igt_sysfs.h"
#include "igt.h"
/**
 * TEST: i915 pm freq api
 * Description: Test SLPC freq API
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: uapi
 * Functionality: slpc
 *
 * SUBTEST: freq-basic-api
 * Description: Test basic API for controlling min/max GT frequency
 *
 * SUBTEST: freq-reset
 * Description: Test basic freq API works after a reset
 *
 * SUBTEST: freq-reset-multiple
 * Description: Test basic freq API works after multiple resets
 *
 * SUBTEST: freq-suspend
 * Description: Test basic freq API works after a runtime suspend
 */

IGT_TEST_DESCRIPTION("Test SLPC freq API");
/*
 * Too many intermediate components and steps before freq is adjusted
 * Specially if workload is under execution, so let's wait 100 ms.
 */
#define ACT_FREQ_LATENCY_US 100000

static uint32_t get_freq(int dirfd, uint8_t id)
{
	uint32_t val;

	igt_assert(igt_sysfs_rps_scanf(dirfd, id, "%u", &val) == 1);

	return val;
}

static int set_freq(int dirfd, uint8_t id, uint32_t val)
{
	return igt_sysfs_rps_printf(dirfd, id, "%u", val);
}

static void test_freq_basic_api(int dirfd, int gt)
{
	uint32_t rpn, rp0, rpe;

	/* Save frequencies */
	rpn = get_freq(dirfd, RPS_RPn_FREQ_MHZ);
	rp0 = get_freq(dirfd, RPS_RP0_FREQ_MHZ);
	rpe = get_freq(dirfd, RPS_RP1_FREQ_MHZ);
	igt_debug("GT: %d, RPn: %d, RPe: %d, RP0: %d\n", gt, rpn, rpe, rp0);

	/* Set min/max to RPn, RP0 for baseline behavior */
	igt_assert_lt(0, set_freq(dirfd, RPS_MIN_FREQ_MHZ, rpn));
	igt_assert_lt(0, set_freq(dirfd, RPS_MAX_FREQ_MHZ, rp0));

	/*
	 * Negative bound tests
	 * RPn is the floor
	 * RP0 is the ceiling
	 */
	igt_assert_lt(set_freq(dirfd, RPS_MIN_FREQ_MHZ, rpn - 1), 0);
	igt_assert_lt(set_freq(dirfd, RPS_MIN_FREQ_MHZ, rp0 + 1), 0);
	igt_assert_lt(set_freq(dirfd, RPS_MAX_FREQ_MHZ, rpn - 1), 0);
	igt_assert_lt(set_freq(dirfd, RPS_MAX_FREQ_MHZ, rp0 + 1), 0);

	/* Assert min requests are respected from rp0 to rpn */
	igt_assert_lt(0, set_freq(dirfd, RPS_MIN_FREQ_MHZ, rp0));
	igt_assert_eq_u32(get_freq(dirfd, RPS_MIN_FREQ_MHZ), rp0);
	igt_assert_lt(0, set_freq(dirfd, RPS_MIN_FREQ_MHZ, rpe));
	igt_assert_eq_u32(get_freq(dirfd, RPS_MIN_FREQ_MHZ), rpe);
	igt_assert_lt(0, set_freq(dirfd, RPS_MIN_FREQ_MHZ, rpn));
	igt_assert_eq_u32(get_freq(dirfd, RPS_MIN_FREQ_MHZ), rpn);

	/* Assert max requests are respected from rpn to rp0 */
	igt_assert_lt(0, set_freq(dirfd, RPS_MAX_FREQ_MHZ, rpn));
	igt_assert_eq_u32(get_freq(dirfd, RPS_MAX_FREQ_MHZ), rpn);
	igt_assert_lt(0, set_freq(dirfd, RPS_MAX_FREQ_MHZ, rpe));
	igt_assert_eq_u32(get_freq(dirfd, RPS_MAX_FREQ_MHZ), rpe);
	igt_assert_lt(0, set_freq(dirfd, RPS_MAX_FREQ_MHZ, rp0));
	igt_assert_eq_u32(get_freq(dirfd, RPS_MAX_FREQ_MHZ), rp0);

}

static void test_reset(int i915, int dirfd, int gt, int count)
{
	uint32_t rpn = get_freq(dirfd, RPS_RPn_FREQ_MHZ);
	uint32_t req_freq;
	int fd;

	for (int i = 0; i < count; i++) {
		igt_debug("Running cycle: %d", i);
		igt_assert_lt(0, set_freq(dirfd, RPS_MIN_FREQ_MHZ, rpn));
		igt_assert_lt(0, set_freq(dirfd, RPS_MAX_FREQ_MHZ, rpn));
		usleep(ACT_FREQ_LATENCY_US);
		req_freq = get_freq(dirfd, RPS_CUR_FREQ_MHZ);
		if (req_freq)
			igt_assert_eq(req_freq, rpn);

		/* Manually trigger a GT reset */
		fd = igt_debugfs_gt_open(i915, gt, "reset", O_WRONLY);
		igt_require(fd >= 0);
		igt_ignore_warn(write(fd, "1\n", 2));

		req_freq = get_freq(dirfd, RPS_CUR_FREQ_MHZ);
		if (req_freq)
			igt_assert_eq(req_freq, rpn);
	}
	close(fd);
}

static void test_suspend(int i915, int dirfd, int gt)
{
	uint32_t rpn = get_freq(dirfd, RPS_RPn_FREQ_MHZ);
	uint32_t req_freq;

	igt_assert_lt(0, set_freq(dirfd, RPS_MIN_FREQ_MHZ, rpn));
	igt_assert_lt(0, set_freq(dirfd, RPS_MAX_FREQ_MHZ, rpn));
	usleep(ACT_FREQ_LATENCY_US);
	req_freq = get_freq(dirfd, RPS_CUR_FREQ_MHZ);
	if (req_freq)
		igt_assert_eq(req_freq, rpn);

	/* Manually trigger a suspend */
	igt_system_suspend_autoresume(SUSPEND_STATE_FREEZE,
				      SUSPEND_TEST_NONE);

	req_freq = get_freq(dirfd, RPS_CUR_FREQ_MHZ);
	if (req_freq)
		igt_assert_eq(req_freq, rpn);
}

int i915 = -1;
uint32_t *stash_min, *stash_max;

static void restore_sysfs_freq(int sig)
{
	int dirfd, gt;
	/* Restore frequencies */
	for_each_sysfs_gt_dirfd(i915, dirfd, gt) {
		igt_pm_ignore_slpc_efficient_freq(i915, dirfd, false);
		igt_assert_lt(0,
			      set_freq(dirfd, RPS_MAX_FREQ_MHZ, stash_max[gt]));
		igt_assert_lt(0,
			      set_freq(dirfd, RPS_MIN_FREQ_MHZ, stash_min[gt]));
	}
	free(stash_min);
	free(stash_max);
	drm_close_driver(i915);
}

igt_main()
{
	igt_fixture() {
		int num_gts, dirfd, gt;

		i915 = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(i915);
		/* i915_pm_rps already covers execlist path */
		igt_require_f(gem_using_guc_submission(i915) &&
			      i915_is_slpc_enabled(i915),
			      "This test is supported only with SLPC enabled\n");

		num_gts = igt_sysfs_get_num_gt(i915);
		stash_min = (uint32_t*)malloc(sizeof(uint32_t) * num_gts);
		stash_max = (uint32_t*)malloc(sizeof(uint32_t) * num_gts);

		/* Save curr min and max across GTs */
		for_each_sysfs_gt_dirfd(i915, dirfd, gt) {
			stash_min[gt] = get_freq(dirfd, RPS_MIN_FREQ_MHZ);
			stash_max[gt] = get_freq(dirfd, RPS_MAX_FREQ_MHZ);
			igt_debug("GT: %d, min: %d, max: %d\n", gt, stash_min[gt], stash_max[gt]);
			igt_pm_ignore_slpc_efficient_freq(i915, dirfd, true);
		}
		igt_install_exit_handler(restore_sysfs_freq);
	}

	igt_describe("Test basic API for controlling min/max GT frequency");
	igt_subtest_with_dynamic_f("freq-basic-api") {
		int dirfd, gt;

		for_each_sysfs_gt_dirfd(i915, dirfd, gt)
			igt_dynamic_f("gt%u", gt)
				test_freq_basic_api(dirfd, gt);
	}

	igt_describe("Test basic freq API works after a reset");
	igt_subtest_with_dynamic_f("freq-reset") {
		int dirfd, gt;

		for_each_sysfs_gt_dirfd(i915, dirfd, gt)
			igt_dynamic_f("gt%u", gt)
				test_reset(i915, dirfd, gt, 1);
	}

	igt_describe("Test basic freq API works after multiple resets");
	igt_subtest_with_dynamic_f("freq-reset-multiple") {
		int dirfd, gt;

		for_each_sysfs_gt_dirfd(i915, dirfd, gt)
			igt_dynamic_f("gt%u", gt)
				test_reset(i915, dirfd, gt, 50);
	}

	igt_describe("Test basic freq API works after suspend");
	igt_subtest_with_dynamic_f("freq-suspend") {
		int dirfd, gt;

		for_each_sysfs_gt_dirfd(i915, dirfd, gt)
			igt_dynamic_f("gt%u", gt)
				test_suspend(i915, dirfd, gt);
	}
}
