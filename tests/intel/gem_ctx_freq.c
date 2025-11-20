/*
 * Copyright © 2018 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>

#include "i915/gem.h"
#include "i915/gem_engine_topology.h"
#include "i915_drm.h"
#include "igt.h"
#include "igt_perf.h"
#include "igt_sysfs.h"
#include "sw_sync.h"
/**
 * TEST: gem ctx freq
 * Category: Core
 * Mega feature: Power management
 * Sub-category: Power management tests
 * Functionality: frequency management
 * Feature: context feature
 * Test category: GEM_Legacy
 *
 * SUBTEST: sysfs
 */

#define SAMPLE_PERIOD (USEC_PER_SEC / 10)
#define PMU_TOLERANCE 100

static int i915 = -1;
static int sysfs = -1;

static void kick_rps_worker(void)
{
	sched_yield();
	usleep(SAMPLE_PERIOD);
}

static double measure_frequency(int pmu, int period_us)
{
	uint64_t data[2];
	uint64_t d_t, d_v;

	kick_rps_worker(); /* let the kthreads (intel_rps_work) run */

	igt_assert_eq(read(pmu, data, sizeof(data)), sizeof(data));
	d_v = -data[0];
	d_t = -data[1];

	usleep(period_us);

	igt_assert_eq(read(pmu, data, sizeof(data)), sizeof(data));
	d_v += data[0];
	d_t += data[1];

	return d_v * 1e9 / d_t;
}

static bool __pmu_within_tolerance(double actual, double target)
{
	return (actual > target - PMU_TOLERANCE &&
		actual < target + PMU_TOLERANCE);
}

static void pmu_assert(double actual, double target)
{
	igt_assert_f(__pmu_within_tolerance(actual, target),
		     "Measured frequency %.2fMHz, is beyond target %.0f±%dMhz\n",
		     actual, target, PMU_TOLERANCE);
}

static void busy_wait_until_idle(igt_spin_t *spin)
{
	igt_spin_end(spin);
	do {
		usleep(10000);
	} while (gem_bo_busy(i915, spin->handle));
}

static void __igt_spin_free_idle(igt_spin_t *spin)
{
	busy_wait_until_idle(spin);

	igt_spin_free(i915, spin);
}

#define TRIANGLE_SIZE(x) (2 * (x) + 1)
static void triangle_fill(uint32_t *t, unsigned int nstep,
			  uint32_t min, uint32_t max)
{
	for (unsigned int step = 0; step <= 2*nstep; step++) {
		int frac = step > nstep ? 2*nstep - step : step;
		t[step] = min + (max - min) * frac / nstep;
	}
}

static void set_sysfs_freq(int dirfd, uint32_t min, uint32_t max)
{
	igt_sysfs_rps_printf(dirfd, RPS_MIN_FREQ_MHZ, "%u", min);
	igt_sysfs_rps_printf(dirfd, RPS_MAX_FREQ_MHZ, "%u", max);
}

static bool get_sysfs_freq(int dirfd, uint32_t *min, uint32_t *max)
{
	return (igt_sysfs_rps_scanf(dirfd, RPS_MIN_FREQ_MHZ, "%u", min) == 1 &&
		igt_sysfs_rps_scanf(dirfd, RPS_MAX_FREQ_MHZ, "%u", max) == 1);
}

static void sysfs_range(int dirfd, int gt)
{
#define N_STEPS 10
	uint32_t frequencies[TRIANGLE_SIZE(N_STEPS)];
	struct i915_engine_class_instance *ci;
	uint32_t sys_min, sys_max, ctx;
	unsigned int count;
	igt_spin_t *spin;
	double measured;
	int pmu;
	uint64_t ahnd = get_reloc_ahnd(i915, 0);

	/*
	 * The sysfs interface sets the global limits and overrides the
	 * user's request. So we can to check that if the user requests
	 * a range outside of the sysfs, the requests are only run at the
	 * constrained sysfs range. With GuC SLPC this requires disabling
	 * efficient freq.
	 */

	igt_pm_ignore_slpc_efficient_freq(i915, dirfd, true);
	igt_require(get_sysfs_freq(dirfd, &sys_min, &sys_max));
	igt_info("System min freq: %dMHz; max freq: %dMHz\n", sys_min, sys_max);

	triangle_fill(frequencies, N_STEPS, sys_min, sys_max);

	ci = gem_list_engines(i915, 1 << gt, ~0U, &count);
	igt_require(ci);
	ctx = gem_context_create_for_engine(i915,
					    ci[0].engine_class,
					    ci[0].engine_instance);
	free(ci);

	pmu = perf_i915_open(i915, __I915_PMU_REQUESTED_FREQUENCY(gt));
	igt_require(pmu >= 0);

	for (int outer = 0; outer <= 2*N_STEPS; outer++) {
		uint32_t sys_freq = frequencies[outer];
		uint32_t cur, discard;

		gem_quiescent_gpu(i915);
		spin = igt_spin_new(i915, .ahnd = ahnd, .ctx_id = ctx);
		usleep(10000);

		set_sysfs_freq(dirfd, sys_freq, sys_freq);
		get_sysfs_freq(dirfd, &cur, &discard);

		measured = measure_frequency(pmu, SAMPLE_PERIOD);
		igt_debugfs_dump(i915, "i915_rps_boost_info");

		set_sysfs_freq(dirfd, sys_min, sys_max);
		__igt_spin_free_idle(spin);

		igt_info("sysfs: Measured %.1fMHz, expected %dMhz\n",
			 measured, cur);
		pmu_assert(measured, cur);
	}
	gem_quiescent_gpu(i915);

	gem_context_destroy(i915, ctx);
	close(pmu);
	put_ahnd(ahnd);

#undef N_STEPS
}

static void __restore_sysfs_freq(int dirfd, int sysfs_fd, int fd)
{
	char buf[256];
	int len;

	igt_pm_ignore_slpc_efficient_freq(fd, dirfd, false);

	len = igt_sysfs_read(sysfs_fd, "gt_RPn_freq_mhz", buf, sizeof(buf) - 1);
	if (len > 0) {
		buf[len] = '\0';
		igt_sysfs_rps_set(dirfd, RPS_MIN_FREQ_MHZ, buf);
	}

	len = igt_sysfs_rps_read(dirfd, RPS_RP0_FREQ_MHZ, buf, sizeof(buf) - 1);
	if (len > 0) {
		buf[len] = '\0';
		igt_sysfs_rps_set(dirfd, RPS_MAX_FREQ_MHZ, buf);
		igt_sysfs_rps_set(dirfd, RPS_BOOST_FREQ_MHZ, buf);
	}
}

static void restore_sysfs_freq(int sig)
{
	int sysfs_fd, dirfd, gt, fd;

	fd = drm_open_driver(DRIVER_INTEL);
	sysfs_fd = igt_sysfs_open(fd);
	igt_assert(sysfs_fd != -1);

	for_each_sysfs_gt_dirfd(fd, dirfd, gt)
		__restore_sysfs_freq(dirfd, sysfs_fd, fd);

	close(sysfs_fd);
	drm_close_driver(fd);
}

static void __disable_boost(int dirfd)
{
	char buf[256];
	int len;

	len = igt_sysfs_rps_read(dirfd, RPS_RPn_FREQ_MHZ, buf, sizeof(buf) - 1);
	if (len > 0) {
		buf[len] = '\0';
		igt_sysfs_rps_set(dirfd, RPS_MIN_FREQ_MHZ, buf);
		igt_sysfs_rps_set(dirfd, RPS_BOOST_FREQ_MHZ, buf);
	}

	len = igt_sysfs_rps_read(dirfd, RPS_RP0_FREQ_MHZ, buf, sizeof(buf) - 1);
	if (len > 0) {
		buf[len] = '\0';
		igt_sysfs_rps_set(dirfd, RPS_MAX_FREQ_MHZ, buf);
	}
}

static void disable_boost(void)
{
	int dirfd, gt;

	for_each_sysfs_gt_dirfd(i915, dirfd, gt)
		__disable_boost(dirfd);
}

int igt_main()
{
	igt_fixture() {
		i915 = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(i915);

		sysfs = igt_sysfs_open(i915);
		igt_assert(sysfs != -1);
		igt_install_exit_handler(restore_sysfs_freq);

		disable_boost();
	}

	igt_subtest_with_dynamic_f("sysfs") {
		int dirfd, gt;

		for_each_sysfs_gt_dirfd(i915, dirfd, gt)
			igt_dynamic_f("gt%u", gt)
				sysfs_range(dirfd, gt);
	}

	igt_fixture() {
		close(sysfs);
		drm_close_driver(i915);
	}
}
