/*
 * Copyright © 2012 Intel Corporation
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
 * Authors:
 *    Ben Widawsky <ben@bwidawsk.net>
 *
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

#include "i915/gem.h"
#include "i915/gem_create.h"
#include "igt.h"
#include "igt_debugfs.h"
#include "igt_perf.h"
#include "igt_power.h"
#include "igt_sysfs.h"
#include "sw_sync.h"
/**
 * TEST: i915 pm rc6 residency
 * Category: Core
 * Mega feature: Power management
 * Sub-category: Power management tests
 * Functionality: rc6
 * Feature: GuCRC, pm_rc6
 *
 * SUBTEST: media-rc6-accuracy
 * Feature: pm_rc6
 *
 * SUBTEST: rc6-accuracy
 *
 * SUBTEST: rc6-fence
 *
 * SUBTEST: rc6-idle
 */

#define SLEEP_DURATION 3 /* in seconds */

#define RC6_ENABLED	1
#define RC6P_ENABLED	2
#define RC6PP_ENABLED	4

char *drpc;

static int sysfs;

struct residencies {
	int rc6;
	int media_rc6;
	int rc6p;
	int rc6pp;
	int duration;
};

static unsigned long get_rc6_enabled_mask(int dirfd)
{
	unsigned long enabled;

	enabled = 0;
	igt_sysfs_rps_scanf(dirfd, RC6_ENABLE, "%lu", &enabled);
	return enabled;
}

static bool has_rc6_residency(int dirfd, enum i915_attr_id id)
{
	unsigned long residency;

	return igt_sysfs_rps_scanf(dirfd, id, "%lu", &residency) == 1;
}

static unsigned long read_rc6_residency(int dirfd, enum i915_attr_id id)
{
	unsigned long residency;

	residency = 0;
	igt_assert(igt_sysfs_rps_scanf(dirfd, id, "%lu", &residency) == 1);
	return residency;
}

static void residency_accuracy(unsigned int diff,
			       unsigned int duration,
			       const char *name_of_rc6_residency)
{
	double ratio;

	ratio = (double)diff / duration;

	igt_info("Residency in %s or deeper state: %u ms (sleep duration %u ms) (%.1f%% of expected duration)\n",
		 name_of_rc6_residency, diff, duration, 100*ratio);
	igt_assert_f(ratio > 0.9 && ratio < 1.05,
		     "Sysfs RC6 residency counter is inaccurate.\n");
}

static unsigned long gettime_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void read_residencies(int devid, int dirfd, unsigned int mask,
			     struct residencies *res)
{
	res->duration = gettime_ms();

	if (mask & RC6_ENABLED)
		res->rc6 = read_rc6_residency(dirfd, RC6_RESIDENCY_MS);

	if ((mask & RC6_ENABLED) &&
	    (IS_VALLEYVIEW(devid) || IS_CHERRYVIEW(devid)))
		res->media_rc6 = read_rc6_residency(dirfd, MEDIA_RC6_RESIDENCY_MS);

	if (mask & RC6P_ENABLED)
		res->rc6p = read_rc6_residency(dirfd, RC6P_RESIDENCY_MS);

	if (mask & RC6PP_ENABLED)
		res->rc6pp = read_rc6_residency(dirfd, RC6PP_RESIDENCY_MS);

	res->duration += (gettime_ms() - res->duration) / 2;
}

static void measure_residencies(int devid, int dirfd, unsigned int mask,
				struct residencies *res)
{
	struct residencies start = { };
	struct residencies end = { };
	int retry;

	/*
	 * Retry in case of counter wrap-around. We simply re-run the
	 * measurement, since the valid counter range is different on
	 * different platforms and so fixing it up would be non-trivial.
	 */
	read_residencies(devid, dirfd, mask, &end);
	igt_debug("time=%d: rc6=(%d, %d), rc6p=%d, rc6pp=%d\n",
		  end.duration, end.rc6, end.media_rc6, end.rc6p, end.rc6pp);
	for (retry = 0; retry < 2; retry++) {
		start = end;
		sleep(SLEEP_DURATION);
		read_residencies(devid, dirfd, mask, &end);

		igt_debug("time=%d: rc6=(%d, %d), rc6p=%d, rc6pp=%d\n",
			  end.duration,
			  end.rc6, end.media_rc6, end.rc6p, end.rc6pp);

		if (end.rc6 >= start.rc6 &&
		    end.media_rc6 >= start.media_rc6 &&
		    end.rc6p >= start.rc6p &&
		    end.rc6pp >= start.rc6pp)
			break;
	}
	igt_assert_f(retry < 2, "residency values are not consistent\n");

	res->rc6 = end.rc6 - start.rc6;
	res->rc6p = end.rc6p - start.rc6p;
	res->rc6pp = end.rc6pp - start.rc6pp;
	res->media_rc6 = end.media_rc6 - start.media_rc6;
	res->duration = end.duration - start.duration;

	/*
	 * For the purposes of this test case we want a given residency value
	 * to include the time spent in the corresponding RC state _and_ also
	 * the time spent in any enabled deeper states. So for example if any
	 * of RC6P or RC6PP is enabled we want the time spent in these states
	 * to be also included in the RC6 residency value. The kernel reported
	 * residency values are exclusive, so add up things here.
	 */
	res->rc6p += res->rc6pp;
	res->rc6 += res->rc6p;
}

static bool wait_for_rc6(int dirfd)
{
	struct timespec tv = {};
	unsigned long start, now;

	/* First wait for roughly an RC6 Evaluation Interval */
	usleep(160 * 1000);

	/* Then poll for RC6 to start ticking */
	now = read_rc6_residency(dirfd, RC6_RESIDENCY_MS);
	do {
		start = now;
		usleep(5000);
		now = read_rc6_residency(dirfd, RC6_RESIDENCY_MS);
		if (now - start > 1)
			return true;
	} while (!igt_seconds_elapsed(&tv));

	return false;
}

static uint64_t __pmu_read_single(int fd, uint64_t *ts)
{
	uint64_t data[2];

	igt_assert_eq(read(fd, data, sizeof(data)), sizeof(data));

	if (ts)
		*ts = data[1];

	return data[0];
}

static uint64_t pmu_read_single(int fd)
{
	return __pmu_read_single(fd, NULL);
}

static char *get_drpc(int i915, int gt_id)
{
	int gt_dir;

	gt_dir = igt_debugfs_gt_dir(i915, gt_id);
	igt_assert_neq(gt_dir, -1);
	return igt_sysfs_get(gt_dir, "drpc");
}

static bool __pmu_wait_for_rc6(int fd)
{
	struct timespec tv = {};
	uint64_t start, now;

	/* First wait for roughly an RC6 Evaluation Interval */
	usleep(160 * 1000);

	/* Then poll for RC6 to start ticking */
	now = pmu_read_single(fd);
	do {
		start = now;
		usleep(5000);
		now = pmu_read_single(fd);
		if (now - start > 1e6)
			return true;
	} while (!igt_seconds_elapsed(&tv));

	return false;
}

static int open_pmu(int i915, uint64_t config)
{
	int fd;

	fd = perf_i915_open(i915, config);
	igt_skip_on(fd < 0 && errno == ENODEV);
	igt_assert_lte(0, fd);

	return fd;
}

#define FREQUENT_BOOST 0x1
#define ONCE 0x2

static void sighandler(int sig)
{
}

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

static uint32_t stash_min;
static int s_dirfd = -1;

static void restore_freq(int sig)
{
	if (s_dirfd == -1)
		return;

	set_freq(s_dirfd, RPS_MIN_FREQ_MHZ, stash_min);
	close(s_dirfd);
}

static void bg_load(int i915, const intel_ctx_t *ctx, uint64_t engine_flags,
		    unsigned int flags, unsigned long *ctl, unsigned int gt)
{
	const bool has_execlists = intel_gen(intel_get_drm_devid(i915)) >= 8;
	struct sigaction act = {
		.sa_handler = sighandler
	};
	int64_t timeout = 1;
	uint64_t ahnd;
	int rp0;

	ahnd = get_reloc_ahnd(i915, ctx->id);
	rp0 = get_freq(s_dirfd, RPS_RP0_FREQ_MHZ);
	sigaction(SIGINT, &act, NULL);
	do {
		uint64_t submit, wait, elapsed;
		struct timespec tv = {};
		igt_spin_t *spin;

		igt_nsec_elapsed(&tv);
		spin = igt_spin_new(i915,
				    .ahnd = ahnd,
				    .ctx = ctx,
				    .engine = engine_flags);
		submit = igt_nsec_elapsed(&tv);
		if (flags & FREQUENT_BOOST) {
			/* Set MIN freq to RP0 to achieve the peak freq */
			igt_assert_lt(0, set_freq(s_dirfd, RPS_MIN_FREQ_MHZ, rp0));
			igt_assert(gem_bo_busy(i915, spin->handle));
			gem_wait(i915, spin->handle, &timeout);

			/* Restore the MIN freq back to default */
			igt_assert_lt(0, set_freq(s_dirfd, RPS_MIN_FREQ_MHZ, stash_min));
			igt_spin_end(spin);
			igt_spin_free(i915, spin);
			gem_quiescent_gpu(i915);
			if (flags & ONCE)
				flags &= ~FREQUENT_BOOST;
		} else  {
			igt_assert(gem_bo_busy(i915, spin->handle));
			igt_spin_end(spin);
			igt_spin_free(i915, spin);
			gem_quiescent_gpu(i915);
		}
		wait = igt_nsec_elapsed(&tv);

		/*
		 * The legacy ringbuffer submission lacks a fast soft-rc6
		 * mechanism as we have no interrupt for an idle ring. As such
		 * we are at the mercy of HW RC6... which is not quite as
		 * precise as we need to pass this test. Oh well.
		 *
		 * Fake it until we make it.
		 */
		if (!has_execlists)
			igt_drop_caches_set(i915, DROP_IDLE);

		elapsed = igt_nsec_elapsed(&tv);
		igt_debug("Pulse took %.3fms (submit %.1fus, wait %.1fus, idle %.1fus)\n",
			  1e-6 * elapsed,
			  1e-3 * submit,
			  1e-3 * (wait - submit),
			  1e-3 * (elapsed - wait));
		ctl[1]++;

		/* aim for ~1% busy */
		usleep(min_t(elapsed, elapsed / 10, 50 * 1000));
	} while (!READ_ONCE(*ctl));
	put_ahnd(ahnd);
}

static void kill_children(int sig)
{
	void (*old)(int);

	old = signal(sig, SIG_IGN);
	kill(-getpgrp(), sig);
	signal(sig, old);
}

static void rc6_idle(int i915, const intel_ctx_t *ctx, uint64_t flags, unsigned int gt)
{
	const int64_t duration_ns = 2 * SLEEP_DURATION * (int64_t)NSEC_PER_SEC;
	const int tolerance = 20; /* Some RC6 is better than none! */
	const unsigned int gen = intel_gen(intel_get_drm_devid(i915));
	struct {
		const char *name;
		unsigned int flags;
		double power;
	} phases[] = {
		{ "once", FREQUENT_BOOST | ONCE },
		{ "normal", 0 },
		{ "boost", FREQUENT_BOOST }
	};
	struct power_sample sample[2];
	unsigned long slept, cycles;
	unsigned long *done;
	uint64_t rc6, ts[2];
	struct igt_power gpu;
	int fd;

	fd = open_pmu(i915, __I915_PMU_RC6_RESIDENCY(gt));
	igt_drop_caches_set(i915, DROP_IDLE);
	igt_require(__pmu_wait_for_rc6(fd));
	igt_power_open(i915, &gpu, "gpu");

	/* While idle check full RC6. */
	igt_power_get_energy(&gpu, &sample[0]);
	rc6 = -__pmu_read_single(fd, &ts[0]);
	slept = igt_measured_usleep(duration_ns / 1000) * NSEC_PER_USEC;
	rc6 += __pmu_read_single(fd, &ts[1]);
	igt_debug("slept=%lu perf=%"PRIu64", rc6=%"PRIu64"\n",
		  slept, ts[1] - ts[0], rc6);
	igt_power_get_energy(&gpu, &sample[1]);
	if (sample[1].energy) {
		double idle = igt_power_get_mJ(&gpu, &sample[0], &sample[1]);

		igt_log(IGT_LOG_DOMAIN,
			!gem_has_lmem(i915) && idle > 1e-3 && gen > 6 ? IGT_LOG_WARN : IGT_LOG_INFO,
			"Total energy used while idle: %.1fmJ (%.1fmW)\n",
			idle, (idle * 1e9) / slept);
	}
	drpc = get_drpc(i915, gt);

	assert_within_epsilon_debug(rc6, ts[1] - ts[0], 5, drpc);

	if (gt) {
		close(fd);
		igt_power_close(&gpu);
		return;
	}

	done = mmap(0, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);

	for (int p = 0; p < ARRAY_SIZE(phases); p++) {
		memset(done, 0, 2 * sizeof(*done));
		igt_fork(child, 1) /* Setup up a very light load */
			bg_load(i915, ctx, flags, phases[p].flags, done, gt);

		igt_power_get_energy(&gpu, &sample[0]);
		cycles = -READ_ONCE(done[1]);
		rc6 = -__pmu_read_single(fd, &ts[0]);
		slept = igt_measured_usleep(duration_ns / 1000) * NSEC_PER_USEC;
		rc6 += __pmu_read_single(fd, &ts[1]);
		cycles += READ_ONCE(done[1]);
		igt_debug("%s: slept=%lu perf=%"PRIu64", cycles=%lu, rc6=%"PRIu64"\n",
			  phases[p].name, slept, ts[1] - ts[0], cycles, rc6);
		igt_power_get_energy(&gpu, &sample[1]);
		if (sample[1].energy) {
			phases[p].power = igt_power_get_mJ(&gpu, &sample[0], &sample[1]);
			igt_info("Total energy used for %s: %.1fmJ (%.1fmW)\n",
				 phases[p].name,
				 phases[p].power,
				 phases[p].power * 1e9 / slept);
			phases[p].power /= slept; /* normalize */
			phases[p].power *= 1e9; /* => mW */
		}

		*done = 1;
		kill_children(SIGINT);
		igt_waitchildren();

		/* At least one wakeup/s needed for a reasonable test */
		igt_assert(cycles >= SLEEP_DURATION);

		/* While very nearly idle, expect full RC6 */
		drpc = get_drpc(i915, gt);

		assert_within_epsilon_debug(rc6, ts[1] - ts[0], tolerance, drpc);

		free(drpc);
		drpc = NULL;
	}

	munmap(done, 4096);
	close(fd);

	igt_power_close(&gpu);

	if (phases[2].power - phases[1].power > 20 && !gem_has_lmem(i915)) {
		igt_assert_f(2 * phases[0].power - phases[1].power <= phases[2].power,
			     "Exceeded energy expectations for single busy wait load\n"
			     "Used %.1fmW, min %.1fmW, max %.1fmW, expected less than %.1fmW\n",
			     phases[0].power, phases[1].power, phases[2].power,
			     phases[1].power + (phases[2].power - phases[1].power) / 2);
	}
}

static void rc6_fence(int i915, unsigned int gt)
{
	const int64_t duration_ns = SLEEP_DURATION * (int64_t)NSEC_PER_SEC;
	const int tolerance = 20; /* Some RC6 is better than none! */
	const unsigned int gen = intel_gen(intel_get_drm_devid(i915));
	const struct intel_execution_engine2 *e;
	const intel_ctx_t *ctx;
	struct power_sample sample[2];
	unsigned long slept;
	uint64_t rc6, ts[2], ahnd;
	struct igt_power gpu;
	int fd;

	igt_require_sw_sync();

	fd = open_pmu(i915, __I915_PMU_RC6_RESIDENCY(gt));
	igt_drop_caches_set(i915, DROP_IDLE);
	igt_require(__pmu_wait_for_rc6(fd));
	igt_power_open(i915, &gpu, "gpu");

	/* While idle check full RC6. */
	igt_power_get_energy(&gpu, &sample[0]);
	rc6 = -__pmu_read_single(fd, &ts[0]);
	slept = igt_measured_usleep(duration_ns / 1000) * NSEC_PER_USEC;
	rc6 += __pmu_read_single(fd, &ts[1]);
	igt_debug("slept=%lu perf=%"PRIu64", rc6=%"PRIu64"\n",
		  slept, ts[1] - ts[0], rc6);

	igt_power_get_energy(&gpu, &sample[1]);
	if (sample[1].energy) {
		double idle = igt_power_get_mJ(&gpu, &sample[0], &sample[1]);
		igt_log(IGT_LOG_DOMAIN,
			!gem_has_lmem(i915) && idle > 1e-3 && gen > 6 ? IGT_LOG_WARN : IGT_LOG_INFO,
			"Total energy used while idle: %.1fmJ (%.1fmW)\n",
			idle, (idle * 1e9) / slept);
	}
	drpc = get_drpc(i915, gt);

	assert_within_epsilon_debug(rc6, ts[1] - ts[0], 5, drpc);

	/* Submit but delay execution, we should be idle and conserving power */
	ctx = intel_ctx_create_for_gt(i915, gt);
	ahnd = get_reloc_ahnd(i915, ctx->id);
	for_each_ctx_engine(i915, ctx, e) {
		igt_spin_t *spin;
		int timeline;
		int fence;

		timeline = sw_sync_timeline_create();
		fence = sw_sync_timeline_create_fence(timeline, 1);
		spin = igt_spin_new(i915,
				    .ahnd = ahnd,
				    .ctx = ctx,
				    .engine = e->flags,
				    .fence = fence,
				    .flags = IGT_SPIN_FENCE_IN);
		close(fence);

		igt_power_get_energy(&gpu, &sample[0]);
		rc6 = -__pmu_read_single(fd, &ts[0]);
		slept = igt_measured_usleep(duration_ns / 1000) * NSEC_PER_USEC;
		rc6 += __pmu_read_single(fd, &ts[1]);
		igt_debug("%s: slept=%lu perf=%"PRIu64", rc6=%"PRIu64"\n",
			  e->name, slept, ts[1] - ts[0], rc6);

		igt_power_get_energy(&gpu, &sample[1]);
		if (sample[1].energy) {
			double power = igt_power_get_mJ(&gpu, &sample[0], &sample[1]);
			igt_info("Total energy used for %s: %.1fmJ (%.1fmW)\n",
				 e->name,
				 power,
				 power * 1e9 / slept);
		}

		igt_assert(gem_bo_busy(i915, spin->handle));
		igt_spin_free(i915, spin);

		close(timeline);

		drpc = get_drpc(i915, gt);

		assert_within_epsilon_debug(rc6, ts[1] - ts[0], tolerance, drpc);
		gem_quiescent_gpu(i915);

		free(drpc);
		drpc = NULL;
	}
	put_ahnd(ahnd);
	intel_ctx_destroy(i915, ctx);

	igt_power_close(&gpu);
	close(fd);
}

static unsigned int rc6_enabled_mask(int i915, int dirfd)
{
	igt_require(has_rc6_residency(dirfd, RC6_RESIDENCY_MS));

	/* Make sure rc6 counters are running */
	igt_drop_caches_set(i915, DROP_IDLE);
	igt_require(wait_for_rc6(dirfd));

	return get_rc6_enabled_mask(dirfd);
}

int igt_main()
{
	int i915 = -1;
	unsigned int dirfd, gt;
	const intel_ctx_t *ctx;

	/* Use drm_open_driver to verify device existence */
	igt_fixture() {
		i915 = drm_open_driver(DRIVER_INTEL);
	}

	igt_subtest_with_dynamic("rc6-idle") {
		const struct intel_execution_engine2 *e;

		igt_require_gem(i915);
		gem_quiescent_gpu(i915);
		igt_require_f(i915_is_slpc_enabled(i915),
			      "This test can only be conducted if SLPC is enabled\n");

		s_dirfd = igt_sysfs_gt_open(i915, 0);
		stash_min = get_freq(s_dirfd, RPS_MIN_FREQ_MHZ);
		igt_install_exit_handler(restore_freq);
		intel_allocator_multiprocess_start();

		i915_for_each_gt(i915, dirfd, gt) {
			ctx = intel_ctx_create_for_gt(i915, gt);
			for_each_ctx_engine(i915, ctx, e) {
				if (e->instance == 0) {
					igt_dynamic_f("gt%u-%s", gt, e->name)
						rc6_idle(i915, ctx, e->flags, gt);
				}
			}
			intel_ctx_destroy(i915, ctx);
		}
		intel_allocator_multiprocess_stop();
	}

	igt_subtest_with_dynamic("rc6-fence") {
		igt_require_gem(i915);
		gem_quiescent_gpu(i915);

		i915_for_each_gt(i915, dirfd, gt)
			igt_dynamic_f("gt%u", gt)
				rc6_fence(i915, gt);
	}

	igt_subtest_group() {
		unsigned int rc6_enabled = 0;
		unsigned int devid = 0;

		igt_fixture() {
			devid = intel_get_drm_devid(i915);
			sysfs = igt_sysfs_open(i915);
			igt_assert(sysfs != 1);
		}

		igt_subtest_with_dynamic("rc6-accuracy") {
			i915_for_each_gt(i915, dirfd, gt) {
				igt_dynamic_f("gt%u", gt) {
					struct residencies res;

					rc6_enabled = rc6_enabled_mask(i915, dirfd);
					igt_require(rc6_enabled & RC6_ENABLED);

					measure_residencies(devid, dirfd, rc6_enabled, &res);
					residency_accuracy(res.rc6, res.duration, "rc6");
				}
			}
		}

		igt_subtest("media-rc6-accuracy") {
			struct residencies res;

			igt_require(IS_VALLEYVIEW(devid) || IS_CHERRYVIEW(devid));

			rc6_enabled = rc6_enabled_mask(i915, sysfs);
			igt_require(rc6_enabled & RC6_ENABLED);

			measure_residencies(devid, sysfs, rc6_enabled, &res);
			residency_accuracy(res.media_rc6, res.duration, "media_rc6");
		}

		igt_fixture()
			close(sysfs);
	}

	igt_fixture() {
		free(drpc);
		drm_close_driver(i915);
	}
}
