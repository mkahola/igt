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
 *    Jeff McGee <jeff.mcgee@intel.com>
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/wait.h>

#include "i915/gem.h"
#include "i915/gem_create.h"
#include "igt.h"
#include "igt_aux.h"
#include "igt_dummyload.h"
#include "igt_perf.h"
#include "igt_rand.h"
#include "igt_sysfs.h"
/**
 * TEST: i915 pm rps
 * Description: Render P-States tests - verify GPU frequency changes
 * Category: Core
 * Mega feature: Power management
 * Sub-category: Power management tests
 * Functionality: pm_rps
 * Feature: pm_rps
 * Test category: pm_rps
 *
 * SUBTEST: basic-api
 *
 * SUBTEST: engine-order
 * Description:
 *   Check if context reuse does not affect waitboosting.
 *   Render P-States tests - verify GPU frequency changes
 *
 * SUBTEST: fence-order
 * Description:
 *   Check if the order of fences does not affect waitboosting.
 *   Render P-States tests - verify GPU frequency changes
 * Feature: pm_rps, synchronization feature
 *
 * SUBTEST: min-max-config-idle
 * SUBTEST: min-max-config-loaded
 * SUBTEST: reset
 * SUBTEST: thresholds
 * SUBTEST: thresholds-idle
 * SUBTEST: thresholds-idle-park
 * SUBTEST: thresholds-park
 * SUBTEST: waitboost
 */

#define NUMFREQ		8
#define MAX_GTS		8

IGT_TEST_DESCRIPTION("Render P-States tests - verify GPU frequency changes");
static void dump(const int *freqs, int gt_id);

static int drm_fd;

enum {
	ACT,
	CUR,
	MIN,
	MAX,
	RP0,
	RP1,
	RPn,
	BOOST
};

static int origfreqs[MAX_GTS][NUMFREQ];

struct sysfs_file {
	const char *name;
	const char *mode;
	FILE *filp;
} sysfs_files[MAX_GTS][NUMFREQ];

static const char *freq_names[NUMFREQ] = {
	[ACT]   = "act",
	[CUR]   = "cur",
	[MIN]   = "min",
	[MAX]   = "max",
	[RP0]   = "RP0",
	[RP1]   = "RP1",
	[RPn]   = "RPn",
	[BOOST] = "boost"
};

static const char *freq_modes[NUMFREQ] = {
	[ACT]   = "r",
	[CUR]   = "r",
	[MIN]   = "rb+",
	[MAX]   = "rb+",
	[RP0]   = "r",
	[RP1]   = "r",
	[RPn]   = "r",
	[BOOST] = "rb+"
};

static void init_sysfs_files(void)
{
	int tmp, gt;
	int actual_gt_count = 0;

	/* Count actual GTs and initialize sysfs_files dynamically */
	i915_for_each_gt(drm_fd, tmp, gt) {
		if (gt >= MAX_GTS) {
			igt_warn("GT index %d exceeds MAX_GTS (%d), skipping\n", gt, MAX_GTS);
			continue;
		}

		actual_gt_count = gt + 1;

		for (int j = 0; j < NUMFREQ; j++) {
			sysfs_files[gt][j].name = freq_names[j];
			sysfs_files[gt][j].mode = freq_modes[j];
			sysfs_files[gt][j].filp = NULL;
		}
	}

	igt_debug("Detected %d GT(s)\n", actual_gt_count);
}

static int readval(FILE *filp)
{
	int val;
	int scanned;

	rewind(filp);
	scanned = fscanf(filp, "%d", &val);
	igt_assert_eq(scanned, 1);

	return val;
}

static void read_freqs(int *freqs, int gt_id)
{
	int i;

	for (i = 0; i < NUMFREQ; i++)
		freqs[i] = readval(sysfs_files[gt_id][i].filp);
}

static void nsleep(unsigned long ns)
{
	struct timespec ts;
	int ret;

	ts.tv_sec = 0;
	ts.tv_nsec = ns;
	do {
		struct timespec rem;

		ret = nanosleep(&ts, &rem);
		igt_assert(ret == 0 || errno == EINTR);
		ts = rem;
	} while (ret && errno == EINTR);
}

static void wait_freq_settle(int gt_id)
{
	int timeout = 10;

	while (1) {
		int freqs[NUMFREQ];
		read_freqs(freqs, gt_id);
		if (freqs[CUR] >= freqs[MIN] && freqs[CUR] <= freqs[MAX])
			break;
		nsleep(1000000);
		if (!timeout--)
			break;
	}
}

static int do_writeval(FILE *filp, int val, int lerrno, bool readback_check, int gt_id)
{
	int ret, orig;

	orig = readval(filp);
	rewind(filp);
	ret = fprintf(filp, "%d", val);

	if (lerrno) {
		/* Expecting specific error */
		igt_assert(ret == EOF && errno == lerrno);
		if (readback_check)
			igt_assert_eq(readval(filp), orig);
	} else {
		if (ret < 0) {
			int freqs[NUMFREQ];

			read_freqs(freqs, gt_id);
			dump(freqs, gt_id);
		}
		/* Expecting no error */
		igt_assert_lt(0, ret);
		wait_freq_settle(gt_id);
		if (readback_check)
			igt_assert_eq(readval(filp), val);
	}

	return ret;
}

#define writeval(filp, val, gt_id) do_writeval(filp, val, 0, true, gt_id)
#define writeval_inval(filp, val, gt_id) do_writeval(filp, val, EINVAL, true, gt_id)
#define writeval_nocheck(filp, val, gt_id) do_writeval(filp, val, 0, false, gt_id)

static void check_freq_constraints(const int *freqs)
{
	igt_assert_lte(freqs[MIN], freqs[MAX]);
	igt_assert_lte(freqs[CUR], freqs[MAX]);
	igt_assert_lte(freqs[RPn], freqs[MIN]);
	igt_assert_lte(freqs[MAX], freqs[RP0]);
	igt_assert_lte(freqs[RP1], freqs[RP0]);
	igt_assert_lte(freqs[RPn], freqs[RP1]);
	igt_assert_neq(freqs[RP0], 0);
	igt_assert_neq(freqs[RP1], 0);
}

static void dump(const int *freqs, int gt_id)
{
	int i;

	igt_debug("gt%d freq (MHz):", gt_id);
	for (i = 0; i < NUMFREQ; i++)
		igt_debug("  %s=%d", sysfs_files[gt_id][i].name, freqs[i]);
	igt_debug("\n");
}

enum load {
	LOW = 0,
	HIGH
};

static struct load_helper {
	int link;
	enum load load;
	bool exit;
	bool signal;
	int target_gt;
	struct igt_helper_process igt_proc;
} lh;

static void load_helper_signal_handler(int sig)
{
	if (sig == SIGUSR2) {
		lh.load = !lh.load;
		lh.signal = true;
		igt_debug("Switching background load to %s\n", lh.load ? "high" : "low");
	} else
		lh.exit = true;
}

static void load_helper_sync(void)
{
	bool dummy;

	igt_assert_eq(read(lh.link, &dummy, sizeof(dummy)), sizeof(dummy));
}

#define LOAD_HELPER_PAUSE_USEC 500
#define LOAD_HELPER_BO_SIZE (16*1024*1024)
static void load_helper_set_load(enum load load)
{
	igt_assert(lh.igt_proc.running);

	if (lh.load == load)
		return;

	lh.load = load;
	kill(lh.igt_proc.pid, SIGUSR2);

	/* wait for load-helper to switch */
	load_helper_sync();
}

static void load_helper_run(enum load load, int target_gt)
{
	int link[2];

	/*
	 * FIXME fork helpers won't get cleaned up when started from within a
	 * subtest, so handle the case where it sticks around a bit too long.
	 */
	if (lh.igt_proc.running) {
		load_helper_set_load(load);
		return;
	}

	igt_require_gem(drm_fd);

	lh.exit = false;
	lh.load = load;
	lh.signal = true;
	lh.target_gt = target_gt;

	pipe(link);
	lh.link = link[1];

	igt_fork_helper(&lh.igt_proc) {
		igt_spin_t *spin[2] = {};
		bool prev_load;
		uint32_t handle;
		uint64_t ahnd;
		const intel_ctx_t *ctx;

		intel_allocator_init();
		ctx = intel_ctx_create_for_gt(drm_fd, lh.target_gt);
		ahnd = get_reloc_ahnd(drm_fd, ctx->id);

		signal(SIGTERM, load_helper_signal_handler);
		signal(SIGUSR2, load_helper_signal_handler);

		igt_debug("Applying %s load on GT%d...\n", lh.load ? "high" : "low", lh.target_gt);

		prev_load = lh.load == HIGH;
		spin[0] = __igt_spin_new(drm_fd, .ahnd = ahnd, .ctx = ctx);
		if (prev_load)
			spin[1] = __igt_spin_new(drm_fd, .ahnd = ahnd, .ctx = ctx);
		prev_load = !prev_load; /* send the initial signal */
		while (!lh.exit) {
			bool high_load;

			handle = spin[0]->handle;
			igt_spin_end(spin[0]);
			while (gem_bo_busy(drm_fd, handle))
				usleep(100);

			igt_spin_free(drm_fd, spin[0]);
			usleep(100);

			high_load = lh.load == HIGH;
			if (!high_load && spin[1]) {
				igt_spin_free(drm_fd, spin[1]);
				spin[1] = NULL;
			} else {
				spin[0] = spin[1];
			}
			spin[high_load] = __igt_spin_new(drm_fd, .ahnd = ahnd, .ctx = ctx);

			if (lh.signal && high_load != prev_load) {
				igt_assert_eq(write(lh.link, &lh.signal, sizeof(lh.signal)),
					      sizeof(lh.signal));
				lh.signal = false;
			}
			prev_load = high_load;
		}

		handle = spin[0]->handle;
		igt_spin_end(spin[0]);

		if (spin[1]) {
			handle = spin[1]->handle;
			igt_spin_end(spin[1]);
		}

		/* Wait for completion without boosting */
		usleep(1000);
		while (gem_bo_busy(drm_fd, handle))
			usleep(1000);

		/*
		 * Idle/boost logic is tied with request retirement.
		 * Speed up detection of idle state and ensure deboost
		 * after removing load.
		 */
		igt_drop_caches_set(drm_fd, DROP_RETIRE);

		igt_spin_free(drm_fd, spin[1]);
		igt_spin_free(drm_fd, spin[0]);
		put_ahnd(ahnd);
		intel_ctx_destroy(drm_fd, ctx);
	}

	close(lh.link);
	lh.link = link[0];

	/* wait for our helper to complete its first round */
	load_helper_sync();
}

static void load_helper_stop(void)
{
	kill(lh.igt_proc.pid, SIGTERM);
	igt_assert(igt_wait_helper(&lh.igt_proc) == 0);
}

static void do_load_gpu(int target_gt)
{
	load_helper_run(LOW, target_gt);
	nsleep(10000000);
	load_helper_stop();
}

/* Return a frequency rounded by HW to the nearest supported value */
static int get_hw_rounded_freq(int target, int gt_id)
{
	int freqs[NUMFREQ];
	int old_freq;
	int idx;
	int ret;

	read_freqs(freqs, gt_id);

	if (freqs[MIN] > target)
		idx = MIN;
	else
		idx = MAX;

	old_freq = freqs[idx];
	writeval_nocheck(sysfs_files[gt_id][idx].filp, target, gt_id);
	read_freqs(freqs, gt_id);
	ret = freqs[idx];
	writeval_nocheck(sysfs_files[gt_id][idx].filp, old_freq, gt_id);

	return ret;
}

/*
 * Modify softlimit MIN and MAX freqs to valid and invalid levels. Depending
 * on subtest run different check after each modification.
 */
static void min_max_config(void (*check)(int), bool load_gpu, int gt_id)
{
	int fmid = (origfreqs[gt_id][RPn] + origfreqs[gt_id][RP0]) / 2;

	/*
	 * hw (and so kernel) rounds to the nearest value supported by
	 * the given platform.
	 */
	fmid = get_hw_rounded_freq(fmid, gt_id);

	igt_debug("\nCheck original min and max...\n");
	if (load_gpu)
		do_load_gpu(gt_id);
	check(gt_id);

	igt_debug("\nSet min=RPn and max=RP0...\n");
	writeval(sysfs_files[gt_id][MIN].filp, origfreqs[gt_id][RPn], gt_id);
	writeval(sysfs_files[gt_id][MAX].filp, origfreqs[gt_id][RP0], gt_id);
	if (load_gpu)
		do_load_gpu(gt_id);
	check(gt_id);

	igt_debug("\nIncrease min to midpoint...\n");
	writeval(sysfs_files[gt_id][MIN].filp, fmid, gt_id);
	if (load_gpu)
		do_load_gpu(gt_id);
	check(gt_id);

	igt_debug("\nIncrease min to RP0...\n");
	writeval(sysfs_files[gt_id][MIN].filp, origfreqs[gt_id][RP0], gt_id);
	if (load_gpu)
		do_load_gpu(gt_id);
	check(gt_id);

	igt_debug("\nIncrease min above RP0 (invalid)...\n");
	writeval_inval(sysfs_files[gt_id][MIN].filp, origfreqs[gt_id][RP0] + 1000, gt_id);
	check(gt_id);

	if (origfreqs[gt_id][RPn] < origfreqs[gt_id][RP0]) {
		igt_debug("\nDecrease max to RPn (invalid)...\n");
		writeval_inval(sysfs_files[gt_id][MAX].filp, origfreqs[gt_id][RPn], gt_id);
		check(gt_id);
	}

	igt_debug("\nDecrease min to midpoint...\n");
	writeval(sysfs_files[gt_id][MIN].filp, fmid, gt_id);
	if (load_gpu)
		do_load_gpu(gt_id);
	check(gt_id);

	igt_debug("\nDecrease min to RPn...\n");
	writeval(sysfs_files[gt_id][MIN].filp, origfreqs[gt_id][RPn], gt_id);
	if (load_gpu)
		do_load_gpu(gt_id);
	check(gt_id);

	igt_debug("\nDecrease min below RPn (invalid)...\n");
	writeval_inval(sysfs_files[gt_id][MIN].filp, 0, gt_id);
	check(gt_id);

	igt_debug("\nDecrease max to midpoint...\n");
	writeval(sysfs_files[gt_id][MAX].filp, fmid, gt_id);
	check(gt_id);

	igt_debug("\nDecrease max to RPn...\n");
	writeval(sysfs_files[gt_id][MAX].filp, origfreqs[gt_id][RPn], gt_id);
	check(gt_id);

	igt_debug("\nDecrease max below RPn (invalid)...\n");
	writeval_inval(sysfs_files[gt_id][MAX].filp, 0, gt_id);
	check(gt_id);

	if (origfreqs[gt_id][RP0] > origfreqs[gt_id][RPn]) {
		igt_debug("\nIncrease min to RP0 (invalid)...\n");
		writeval_inval(sysfs_files[gt_id][MIN].filp, origfreqs[gt_id][RP0], gt_id);
		check(gt_id);
	}

	igt_debug("\nIncrease max to midpoint...\n");
	writeval(sysfs_files[gt_id][MAX].filp, fmid, gt_id);
	check(gt_id);

	igt_debug("\nIncrease max to RP0...\n");
	writeval(sysfs_files[gt_id][MAX].filp, origfreqs[gt_id][RP0], gt_id);
	check(gt_id);

	igt_debug("\nIncrease max above RP0 (invalid)...\n");
	writeval_inval(sysfs_files[gt_id][MAX].filp, origfreqs[gt_id][RP0] + 1000, gt_id);
	check(gt_id);

	writeval(sysfs_files[gt_id][MIN].filp, origfreqs[gt_id][MIN], gt_id);
	writeval(sysfs_files[gt_id][MAX].filp, origfreqs[gt_id][MAX], gt_id);
}

static void basic_check(int gt_id)
{
	int freqs[NUMFREQ];

	read_freqs(freqs, gt_id);
	dump(freqs, gt_id);
	check_freq_constraints(freqs);
}

#define IDLE_WAIT_TIMESTEP_MSEC 250
#define IDLE_WAIT_TIMEOUT_MSEC 2500
static void idle_check(int gt_id)
{
	int freqs[NUMFREQ];
	int wait = 0;

	/* Monitor frequencies until cur settles down to min, which should
	 * happen within the allotted time */
	do {
		read_freqs(freqs, gt_id);
		dump(freqs, gt_id);
		check_freq_constraints(freqs);
		if (freqs[ACT] <= freqs[RPn])
			break;
		usleep(1000 * IDLE_WAIT_TIMESTEP_MSEC);
		wait += IDLE_WAIT_TIMESTEP_MSEC;
	} while (wait < IDLE_WAIT_TIMEOUT_MSEC);

	igt_debugfs_dump(drm_fd, "i915_rps_boost_info");
	/* Actual freq may be 0 when idle or in RC6 */
	igt_assert_lte(freqs[ACT], freqs[RPn]);
	igt_debug("Required %d msec to reach cur=idle\n", wait);
}

#define LOADED_WAIT_TIMESTEP_MSEC 100
#define LOADED_WAIT_TIMEOUT_MSEC 3000
static void loaded_check(int gt_id)
{
	int freqs[NUMFREQ];
	int wait = 0;

	/* Monitor frequencies until cur increases to max, which should
	 * happen within the allotted time */
	do {
		read_freqs(freqs, gt_id);
		dump(freqs, gt_id);
		check_freq_constraints(freqs);
		if (freqs[CUR] >= freqs[MAX])
			break;
		usleep(1000 * LOADED_WAIT_TIMESTEP_MSEC);
		wait += LOADED_WAIT_TIMESTEP_MSEC;
	} while (wait < LOADED_WAIT_TIMEOUT_MSEC);

	igt_debugfs_dump(drm_fd, "i915_rps_boost_info");
	igt_assert_lte(freqs[MAX], freqs[CUR]);
	igt_debug("Required %d msec to reach cur=max\n", wait);
}

#define STABILIZE_WAIT_TIMESTEP_MSEC 250
#define STABILIZE_WAIT_TIMEOUT_MSEC 15000
static void stabilize_check(int *out, int gt_id)
{
	int freqs[NUMFREQ];
	int wait = 0;

	/* Monitor frequencies until HW will stabilize cur frequency.
	 * It should happen within allotted time */
	read_freqs(freqs, gt_id);
	dump(freqs, gt_id);
	usleep(1000 * STABILIZE_WAIT_TIMESTEP_MSEC);
	do {
		read_freqs(out, gt_id);
		dump(out, gt_id);

		if (memcmp(freqs, out, sizeof(freqs)) == 0)
			break;

		memcpy(freqs, out, sizeof(freqs));
		wait += STABILIZE_WAIT_TIMESTEP_MSEC;
	} while (wait < STABILIZE_WAIT_TIMEOUT_MSEC);

	igt_debugfs_dump(drm_fd, "i915_rps_boost_info");
	igt_debug("Waited %d msec to stabilize cur\n", wait);
}

static void verify_sysfs_values(int gt)
{
	int current_val;

	/* Verify MIN frequency */
	current_val = readval(sysfs_files[gt][MIN].filp);
	if (current_val != origfreqs[gt][MIN]) {
		igt_warn("GT%d MIN freq verification failed: expected %d, got %d\n",
			 gt, origfreqs[gt][MIN], current_val);
	}

	/* Verify MAX frequency */
	current_val = readval(sysfs_files[gt][MAX].filp);
	if (current_val != origfreqs[gt][MAX]) {
		igt_warn("GT%d MAX freq verification failed: expected %d, got %d\n",
			 gt, origfreqs[gt][MAX], current_val);
	}

	/* Verify BOOST frequency */
	current_val = readval(sysfs_files[gt][BOOST].filp);
	if (current_val != origfreqs[gt][BOOST]) {
		igt_warn("GT%d BOOST freq verification failed: expected %d, got %d\n",
			 gt, origfreqs[gt][BOOST], current_val);
	}
}

static void restore_sysfs_on_each_gt(void)
{
	int tmp, gt;

	i915_for_each_gt(drm_fd, tmp, gt) {
		writeval(sysfs_files[gt][MIN].filp, origfreqs[gt][MIN], gt);
		writeval(sysfs_files[gt][MAX].filp, origfreqs[gt][MAX], gt);
		writeval(sysfs_files[gt][BOOST].filp, origfreqs[gt][BOOST], gt);
		verify_sysfs_values(gt);
	}
}

static void boost_freq(int fd, int *boost_freqs, int gt_id)
{
	const intel_ctx_t *ctx;
	int64_t timeout = 1;
	igt_spin_t *load;
	uint64_t ahnd;

	ctx = intel_ctx_create_for_gt(fd, gt_id);
	ahnd = get_reloc_ahnd(fd, ctx->id);

	load = igt_spin_new(fd, .ahnd = ahnd, .ctx = ctx);

	/* Strip off extra fences from the object, and keep it from starting */
	igt_spin_free(fd, igt_spin_new(fd, .ahnd = ahnd, .ctx = ctx, .dependency = load->handle));

	/* Waiting will grant us a boost to maximum */
	gem_wait(fd, load->handle, &timeout);

	read_freqs(boost_freqs, gt_id);
	dump(boost_freqs, gt_id);

	/* Avoid downlocking till boost request is pending */
	igt_spin_end(load);
	gem_sync(fd, load->handle);
	igt_spin_free(fd, load);
	put_ahnd(ahnd);
	intel_ctx_destroy(fd, ctx);
}

static void waitboost(int fd, bool reset, int gt_id)
{
	int pre_freqs[NUMFREQ];
	int boost_freqs[NUMFREQ];
	int post_freqs[NUMFREQ];
	int fmid = (origfreqs[gt_id][RPn] + origfreqs[gt_id][RP0]) / 2;

	fmid = get_hw_rounded_freq(fmid, gt_id);

	igt_require(origfreqs[gt_id][RP0] > origfreqs[gt_id][RPn]);

	load_helper_run(LOW, gt_id);

	igt_debug("Apply low load...\n");
	sleep(1);
	stabilize_check(pre_freqs, gt_id);

	if (reset) {
		igt_debug("Reset gpu...\n");
		igt_force_gpu_reset(fd);
		sleep(1);
	}

	/* Set max freq to less than boost freq */
	writeval(sysfs_files[gt_id][MAX].filp, fmid, gt_id);

	/* When we wait upon the GPU, we want to temporarily boost it
	 * to maximum.
	 */
	boost_freq(fd, boost_freqs, gt_id);

	/* Set max freq to original softmax */
	writeval(sysfs_files[gt_id][MAX].filp, origfreqs[gt_id][MAX], gt_id);

	igt_debug("Apply low load again...\n");
	sleep(1);
	stabilize_check(post_freqs, gt_id);

	igt_debug("Removing load...\n");
	load_helper_stop();
	idle_check(gt_id);

	igt_assert_lte(pre_freqs[CUR], pre_freqs[MAX]);
	igt_assert_eq(boost_freqs[CUR], boost_freqs[BOOST]);
	igt_assert_lte(post_freqs[CUR], post_freqs[MAX]);
}

static uint32_t batch_create(int i915, uint64_t sz)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	const uint32_t chk = MI_ARB_CHECK;
	uint32_t handle = gem_create(i915, sz);
	uint32_t *map;

	map = gem_mmap__device_coherent(i915, handle, 0, sz, PROT_WRITE);

	for (uint64_t pg = 1; pg * 4096 < sz; pg++)
		map[(pg * 4096) / sizeof(*map)] = chk;

	map[sz / sizeof(*map) - 1] = bbe;
	munmap(map, sz);

	return handle;
}

static uint64_t __fence_order(int i915,
			      struct drm_i915_gem_exec_object2 *obj,
			      struct drm_i915_gem_execbuffer2 *eb,
			      uint64_t flags0, uint64_t flags1,
			      double *outf)
{
	uint64_t before[2], after[2];
	struct timespec tv;
	int fd;

	gem_quiescent_gpu(i915);
	fd = perf_i915_open(i915, I915_PMU_ACTUAL_FREQUENCY);

	igt_gettime(&tv);

	obj->flags = flags0;
	gem_execbuf(i915, eb);

	obj->flags = flags1;
	gem_execbuf(i915, eb);

	igt_assert_eq(read(fd, before, sizeof(before)), sizeof(before));
	gem_sync(i915, obj->handle);
	igt_assert_eq(read(fd, after, sizeof(after)), sizeof(after));
	close(fd);

	after[0] -= before[0];
	after[1] -= before[1];

	*outf = 1e9 * after[0] / after[1];
	return igt_nsec_elapsed(&tv);
}

static void fence_order(int i915)
{
	const uint64_t sz = 512ull << 20;
	struct drm_i915_gem_exec_object2 obj[2] = {
		{ .handle = gem_create(i915, 4096) },
		{ .handle = batch_create(i915, sz) },
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(obj),
		.buffer_count = ARRAY_SIZE(obj),
	};
	uint64_t wr, rw;
	uint32_t min = 0, max = 0;
	double freq;
	int sysfs;

	/*
	 * Check the order of fences found during GEM_WAIT does not affect
	 * waitboosting.
	 *
	 * Internally, implicit fences are tracked within a dma-resv which
	 * imposes no order on the individually fences tracked within. Since
	 * there is no defined order, the sequence of waits (and the associated
	 * waitboosts) is also undefined, undermining the consistency of the
	 * waitboost heuristic.
	 *
	 * In particular, we can influence the sequence of fence storage
	 * within dma-resv by mixing read/write semantics for implicit fences.
	 * We can exploit this property of dma-resv to exercise that no matter
	 * the stored order, the heuristic is applied consistently for the
	 * user's GEM_WAIT ioctl.
	 */

	sysfs = igt_sysfs_open(i915);
	__igt_sysfs_get_u32(sysfs, "gt_RPn_freq_mhz", &min);
	__igt_sysfs_get_u32(sysfs, "gt_RP0_freq_mhz", &max);
	igt_require(max > min);

	/* Only allow ourselves to upclock via waitboosting */
	igt_sysfs_printf(sysfs, "gt_min_freq_mhz", "%u", min);
	igt_sysfs_printf(sysfs, "gt_max_freq_mhz", "%u", min);
	igt_sysfs_printf(sysfs, "gt_boost_freq_mhz", "%u", max);

	/* Warm up to bind the vma */
	__fence_order(i915, &obj[0], &execbuf, 0, 0, &freq);

	wr = __fence_order(i915, &obj[0], &execbuf, EXEC_OBJECT_WRITE, 0, &freq);
	igt_info("Write-then-read: %.2fms @ %.3fMHz\n", wr * 1e-6, freq);

	rw = __fence_order(i915, &obj[0], &execbuf, 0, EXEC_OBJECT_WRITE, &freq);
	igt_info("Read-then-write: %.2fms @ %.3fMHz\n", rw * 1e-6, freq);

	gem_close(i915, obj[0].handle);
	gem_close(i915, obj[1].handle);

	igt_sysfs_printf(sysfs, "gt_min_freq_mhz", "%u", min);
	igt_sysfs_printf(sysfs, "gt_max_freq_mhz", "%u", max);

	close(sysfs);

	igt_assert(4 * rw > 3 * wr && 4 * wr > 3 * rw);
}

static uint64_t __engine_order(int i915,
			       struct drm_i915_gem_exec_object2 *obj,
			       struct drm_i915_gem_execbuffer2 *eb,
			       unsigned int *engines0,
			       unsigned int *engines1,
			       unsigned int num_engines,
			       double *outf)
{
	uint64_t before[2], after[2];
	struct timespec tv;
	int fd;

	gem_quiescent_gpu(i915);
	fd = perf_i915_open(i915, I915_PMU_ACTUAL_FREQUENCY);

	igt_gettime(&tv);

	obj->flags = EXEC_OBJECT_WRITE;
	for (unsigned int n = 0; n < num_engines; n++) {
		eb->flags &= ~63ull;
		eb->flags |= engines0[n];
		gem_execbuf_wr(i915, eb);
	}

	obj->flags = 0;
	for (unsigned int n = 0; n < num_engines; n++) {
		eb->flags &= ~63ull;
		eb->flags |= engines1[n];
		gem_execbuf(i915, eb);
	}

	igt_assert_eq(read(fd, before, sizeof(before)), sizeof(before));
	gem_sync(i915, obj->handle);
	igt_assert_eq(read(fd, after, sizeof(after)), sizeof(after));
	close(fd);

	after[0] -= before[0];
	after[1] -= before[1];

	*outf = 1e9 * after[0] / after[1];
	return igt_nsec_elapsed(&tv);
}

static void engine_order(int i915)
{
	const uint64_t sz = 512ull << 20;
	struct drm_i915_gem_exec_object2 obj[2] = {
		{ .handle = gem_create(i915, 4096) },
		{ .handle = batch_create(i915, sz) },
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(obj),
		.buffer_count = ARRAY_SIZE(obj),
	};
	const struct intel_execution_engine2 *e;
	unsigned int engines[2], reverse[2];
	uint64_t forward, backward, both;
	unsigned int num_engines;
	const intel_ctx_t *ctx;
	uint32_t min = 0, max = 0;
	double freq;
	int sysfs;

	/*
	 * Check the order of fences found during GEM_WAIT does not affect
	 * waitboosting. (See fence_order())
	 *
	 * Another way we can manipulate the order of fences within the
	 * dma-resv is through repeated use of the same contexts.
	 */

	num_engines = 0;
	ctx = intel_ctx_create_all_physical(i915);
	for_each_ctx_engine(i915, ctx, e) {
		/*
		 * Avoid using the cmdparser as it will try to allocate
		 * a new shadow batch for each submission -> oom
		 */
		if (!gem_engine_has_mutable_submission(i915, e->class))
			continue;

		engines[num_engines++] = e->flags;
		if (num_engines == ARRAY_SIZE(engines))
			break;
	}
	igt_require(num_engines > 1);
	for (unsigned int n = 0; n < num_engines; n++)
		reverse[n] = engines[num_engines - n - 1];
	execbuf.rsvd1 = ctx->id;

	sysfs = igt_sysfs_open(i915);
	__igt_sysfs_get_u32(sysfs, "gt_RPn_freq_mhz", &min);
	__igt_sysfs_get_u32(sysfs, "gt_RP0_freq_mhz", &max);
	igt_require(max > min);

	/* Only allow ourselves to upclock via waitboosting */
	igt_sysfs_printf(sysfs, "gt_min_freq_mhz", "%u", min);
	igt_sysfs_printf(sysfs, "gt_max_freq_mhz", "%u", min);
	igt_sysfs_printf(sysfs, "gt_boost_freq_mhz", "%u", max);

	/* Warm up to bind the vma */
	gem_execbuf(i915, &execbuf);

	forward = __engine_order(i915, &obj[0], &execbuf,
				 engines, engines, num_engines,
				 &freq);
	igt_info("Forwards: %.2fms @ %.3fMhz\n", forward * 1e-6, freq);

	backward = __engine_order(i915, &obj[0], &execbuf,
				  reverse, reverse, num_engines,
				  &freq);
	igt_info("Backwards: %.2fms @ %.3fMhz\n", backward * 1e-6, freq);

	both = __engine_order(i915, &obj[0], &execbuf,
			      engines, reverse, num_engines,
			      &freq);
	igt_info("Bidirectional: %.2fms @ %.3fMhz\n", both * 1e-6, freq);

	gem_close(i915, obj[0].handle);
	gem_close(i915, obj[1].handle);
	intel_ctx_destroy(i915, ctx);

	igt_sysfs_printf(sysfs, "gt_min_freq_mhz", "%u", min);
	igt_sysfs_printf(sysfs, "gt_max_freq_mhz", "%u", max);

	close(sysfs);

	igt_assert(4 * forward > 3 * backward && 4 * backward > 3 * forward);
	igt_assert(4 * forward > 3 * both && 4 * both > 3 * forward);
}

static void pm_rps_exit_handler(int sig)
{
	int tmp, gt;

	i915_for_each_gt(drm_fd, tmp, gt) {
		if (sysfs_files[gt][MAX].filp) {
			if (origfreqs[gt][MIN] > readval(sysfs_files[gt][MAX].filp)) {
				writeval(sysfs_files[gt][MAX].filp, origfreqs[gt][MAX], gt);
				writeval(sysfs_files[gt][MIN].filp, origfreqs[gt][MIN], gt);
			} else {
				writeval(sysfs_files[gt][MIN].filp, origfreqs[gt][MIN], gt);
				writeval(sysfs_files[gt][MAX].filp, origfreqs[gt][MAX], gt);
			}
		}
	}

	if (lh.igt_proc.running)
		load_helper_stop();

	drm_close_driver(drm_fd);
}

static struct i915_engine_class_instance
find_dword_engine(int i915, const unsigned int gt)
{
	struct i915_engine_class_instance *engines, ci = { -1, -1 };
	unsigned int i, count;

	engines = gem_list_engines(i915, 1u << gt, ~0u, &count);
	igt_assert(engines);

	for (i = 0; i < count; i++) {
		if (!gem_class_can_store_dword(i915, engines[i].engine_class))
			continue;

		ci = engines[i];
		break;
	}

	free(engines);

	return ci;
}

static igt_spin_t *spin_sync_gt(int i915, uint64_t ahnd, unsigned int gt,
				const intel_ctx_t **ctx)
{
	struct i915_engine_class_instance ci = { -1, -1 };
	struct intel_execution_engine2 e = { };

	ci = find_dword_engine(i915, gt);

	igt_require(ci.engine_class != (uint16_t)I915_ENGINE_CLASS_INVALID);

	if (gem_has_contexts(i915)) {
		e.class = ci.engine_class;
		e.instance = ci.engine_instance;
		e.flags = 0;
		*ctx = intel_ctx_create_for_engine(i915, e.class, e.instance);
	} else {
		igt_require(gt == 0); /* Impossible anyway. */
		e.class = gem_execbuf_flags_to_engine_class(I915_EXEC_DEFAULT);
		e.instance = 0;
		e.flags = I915_EXEC_DEFAULT;
		*ctx = intel_ctx_0(i915);
	}

	igt_debug("Using engine %u:%u\n", e.class, e.instance);

	return __igt_sync_spin(i915, ahnd, *ctx, &e);
}

static void sysfs_fail_set_u32(int dir, const char *attr, uint32_t set)
{
	u32 old, new;
	bool ret;

	old = igt_sysfs_get_u32(dir, attr);
	ret = __igt_sysfs_set_u32(dir, attr, set);
	igt_assert_eq(ret, false);
	new = igt_sysfs_get_u32(dir, attr);
	igt_assert_eq(old, new);
}

static void sysfs_set_u32(int dir, const char *attr, uint32_t set)
{
	u32 new;

	igt_sysfs_set_u32(dir, attr, set);

	new = igt_sysfs_get_u32(dir, attr);
	igt_assert_eq(set, new);
}

#define TEST_IDLE 0x1
#define TEST_PARK 0x2
static void test_thresholds(int i915, unsigned int gt, unsigned int flags)
{
	uint64_t ahnd = get_reloc_ahnd(i915, 0);
	unsigned int def_up = 0, def_down = 0;
	const unsigned int points = 10;
	igt_spin_t *spin = NULL;
	const intel_ctx_t *ctx;
	unsigned int *ta, *tb;
	unsigned int i;
	int sysfs;
	bool ret;

	sysfs = igt_sysfs_gt_open(i915, gt);
	igt_require(sysfs >= 0);

	/* Feature test */
	ret = __igt_sysfs_get_u32(sysfs, "rps_up_threshold_pct", &def_up);
	igt_require(ret);
	ret = __igt_sysfs_get_u32(sysfs, "rps_down_threshold_pct", &def_down);
	igt_require(ret);
	igt_require(def_up && def_down);

	/* Check invalid percentages are rejected */
	sysfs_fail_set_u32(sysfs, "rps_up_threshold_pct", 101);
	sysfs_fail_set_u32(sysfs, "rps_down_threshold_pct", 101);

	/*
	 * Invent some random up-down thresholds, but always include 0 and 100
	 * just to have some wild edge cases.
	 */
	ta = calloc(points, sizeof(unsigned int));
	tb = calloc(points, sizeof(unsigned int));
	igt_require(ta && tb);

	ta[0] = tb[0] = 0;
	ta[1] = tb[1] = 100;
	hars_petruska_f54_1_random_seed(time(NULL));
	for (i = 2; i < points; i++) {
		ta[i] = hars_petruska_f54_1_random_unsafe_max(100);
		tb[i] = hars_petruska_f54_1_random_unsafe_max(100);
	}
	igt_permute_array(ta, points, igt_exchange_int);
	igt_permute_array(tb, points, igt_exchange_int);

	/* Exercise the thresholds with a GPU load to trigger park/unpark etc */
	for (i = 0; i < points; i++) {
		igt_info("Testing thresholds up %u%% and down %u%%...\n", ta[i], tb[i]);
		sysfs_set_u32(sysfs, "rps_up_threshold_pct", ta[i]);
		sysfs_set_u32(sysfs, "rps_down_threshold_pct", tb[i]);

		if (flags & TEST_IDLE) {
			gem_quiescent_gpu(i915);
		} else if (spin) {
			intel_ctx_destroy(i915, ctx);
			igt_spin_free(i915, spin);
			spin = NULL;
			if (flags & TEST_PARK) {
				gem_quiescent_gpu(i915);
				usleep(500000);
			}
		}
		spin = spin_sync_gt(i915, ahnd, gt, &ctx);
		usleep(1000000);
		if (flags & TEST_IDLE) {
			intel_ctx_destroy(i915, ctx);
			igt_spin_free(i915, spin);
			if (flags & TEST_PARK) {
				gem_quiescent_gpu(i915);
				usleep(500000);
			}
			spin = NULL;
		}
	}

	if (spin) {
		intel_ctx_destroy(i915, ctx);
		igt_spin_free(i915, spin);
	}

	gem_quiescent_gpu(i915);

	/* Restore defaults */
	sysfs_set_u32(sysfs, "rps_up_threshold_pct", def_up);
	sysfs_set_u32(sysfs, "rps_down_threshold_pct", def_down);

	free(ta);
	free(tb);
	close(sysfs);
	put_ahnd(ahnd);
}

static void read_sysfs_freq(char *sysfs_path)
{
	int tmp, gt;

	i915_for_each_gt(drm_fd, tmp, gt) {
		for (int j = 0; j < NUMFREQ; j++) {
			struct sysfs_file *sysfs_file = &sysfs_files[gt][j];
			int val = -1;
			char *path;
			int ret;

			ret = asprintf(&path, "%s/gt/gt%d/rps_%s_freq_mhz",
				       sysfs_path, gt, sysfs_file->name);
			igt_assert(ret != -1);
			sysfs_file->filp = fopen(path, sysfs_file->mode);
			igt_require(sysfs_file->filp);
			setbuf(sysfs_file->filp, NULL);

			val = readval(sysfs_file->filp);
			igt_assert(val >= 0);
			origfreqs[gt][j] = val;
			free(path);
		}
	}
}

int igt_main()
{
	int tmp, gt;

	igt_fixture() {
		char sysfs_path[80];

		/* Use drm_open_driver to verify device existence */
		drm_fd = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(drm_fd);
		igt_require(gem_can_store_dword(drm_fd, 0));
		igt_assert(igt_sysfs_path(drm_fd, sysfs_path,
					  sizeof(sysfs_path)));
		/* Initialize sysfs_files str dynamically based on actual GTs */
		init_sysfs_files();
		read_sysfs_freq(sysfs_path);
		igt_install_exit_handler(pm_rps_exit_handler);
	}

	igt_subtest("basic-api") {
		igt_skip_on_f(i915_is_slpc_enabled(drm_fd),
			      "This subtest is not supported when SLPC is enabled\n");
		i915_for_each_gt(drm_fd, tmp, gt) {
			min_max_config(basic_check, false, gt);
		}
	}

	/* Verify the constraints, check if we can reach idle */
	igt_subtest("min-max-config-idle") {
		igt_skip_on_f(i915_is_slpc_enabled(drm_fd),
			      "This subtest is not supported when SLPC is enabled\n");
			i915_for_each_gt(drm_fd, tmp, gt) {
				min_max_config(idle_check, true, gt);
			}
	}

	/* Verify the constraints with high load, check if we can reach max */
	igt_subtest("min-max-config-loaded") {
		igt_skip_on_f(i915_is_slpc_enabled(drm_fd),
			      "This subtest is not supported when SLPC is enabled\n");
		i915_for_each_gt(drm_fd, tmp, gt) {
			load_helper_run(HIGH, gt);
			min_max_config(loaded_check, false, gt);
			load_helper_stop();
		}
	}

	/* Checks if we achieve boost using gem_wait */
	igt_subtest("waitboost") {
		restore_sysfs_on_each_gt();
		sleep(1);
		i915_for_each_gt(drm_fd, tmp, gt) {
			igt_info("gt%u\n", gt);
			waitboost(drm_fd, false, gt);
		}
	}

	igt_describe("Check if the order of fences does not affect waitboosting");
	igt_subtest("fence-order") {
		fence_order(drm_fd);
	}

	igt_describe("Check if context reuse does not affect waitboosting");
	igt_subtest("engine-order") {
		engine_order(drm_fd);
	}

	/* Test boost frequency after GPU reset */
	igt_subtest("reset") {
		igt_hang_t hang;

		restore_sysfs_on_each_gt();
		sleep(1);
		hang = igt_allow_hang(drm_fd, 0, 0);
		i915_for_each_gt(drm_fd, tmp, gt) {
			igt_info("gt%u\n", gt);
			waitboost(drm_fd, true, gt);
		}
		igt_disallow_hang(drm_fd, hang);
	}

	igt_subtest_with_dynamic("thresholds-idle") {
		igt_skip_on_f(i915_is_slpc_enabled(drm_fd),
			      "This subtest is not supported when SLPC is enabled\n");
		i915_for_each_gt(drm_fd, tmp, gt) {
			igt_dynamic_f("gt%u", gt)
				test_thresholds(drm_fd, gt, TEST_IDLE);
		}
	}

	igt_subtest_with_dynamic("thresholds") {
		igt_skip_on_f(i915_is_slpc_enabled(drm_fd),
			      "This subtest is not supported when SLPC is enabled\n");
		i915_for_each_gt(drm_fd, tmp, gt) {
			igt_dynamic_f("gt%u", gt)
				test_thresholds(drm_fd, gt, 0);
		}
	}

	igt_subtest_with_dynamic("thresholds-park") {
		igt_skip_on_f(i915_is_slpc_enabled(drm_fd),
			      "This subtest is not supported when SLPC is enabled\n");
		i915_for_each_gt(drm_fd, tmp, gt) {
			igt_dynamic_f("gt%u", gt)
				test_thresholds(drm_fd, gt, TEST_PARK);
		}
	}

	igt_subtest_with_dynamic("thresholds-idle-park") {
		igt_skip_on_f(i915_is_slpc_enabled(drm_fd),
			      "This subtest is not supported when SLPC is enabled\n");

		i915_for_each_gt(drm_fd, tmp, gt) {
			igt_dynamic_f("gt%u", gt)
				test_thresholds(drm_fd, gt, TEST_IDLE | TEST_PARK);
		}
	}

	igt_fixture()
		drm_close_driver(drm_fd);
}
