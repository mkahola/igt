/*
 * Copyright © 2021 Google, Inc.
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
 */

#include <fcntl.h>
#include <glob.h>

#include "igt.h"
#include "igt_fs.h"
#include "igt_msm.h"

static struct msm_device *dev;
static struct msm_bo *scratch_bo;
static uint32_t *scratch;

/*
 * Helper to read and clear devcore.  We want to read it completely to ensure
 * we catch any kernel side regressions like:
 * https://gitlab.freedesktop.org/drm/msm/-/issues/20
 */

static void
read_and_clear_devcore(void)
{
	glob_t glob_buf = {0};
	int ret, fd;

	/* The devcore shows up asynchronously, so it might not be
	 * immediately available:
	 */
	if (!igt_wait(glob("/sys/class/devcoredump/devcd*/data",
			   GLOB_NOSORT, NULL, &glob_buf) != GLOB_NOMATCH,
		      1000, 100))
		return;

	fd = open(glob_buf.gl_pathv[0], O_RDWR);

	if (fd >= 0) {
		char buf[0x1000];

		/*
		 * We want to read the entire file but we can throw away the
		 * contents.. we just want to make sure that we exercise the
		 * kernel side codepaths hit when reading the devcore from
		 * sysfs
		 */
		do {
			ret = igt_readn(fd, buf, sizeof(buf));
		} while (ret > 0);

		/* Clear the devcore: */
		igt_writen(fd, "1", 1);

		close(fd);
	}

	globfree(&glob_buf);
}

/*
 * Helpers for cmdstream packet building:
 */

static void
wait_mem_gte(struct msm_cmd *cmd, uint32_t offset_dwords, uint32_t ref)
{
	msm_cmd_pkt7(cmd, CP_WAIT_MEM_GTE, 4);
	msm_cmd_emit(cmd, 0);                              /* RESERVED */
	msm_cmd_bo  (cmd, scratch_bo, offset_dwords * 4);  /* POLL_ADDR_LO/HI */
	msm_cmd_emit(cmd, ref);                            /* REF */
}

static void
mem_write(struct msm_cmd *cmd, uint32_t offset_dwords, uint32_t val)
{
	msm_cmd_pkt7(cmd, CP_MEM_WRITE, 3);
	msm_cmd_bo  (cmd, scratch_bo, offset_dwords * 4);  /* ADDR_LO/HI */
	msm_cmd_emit(cmd, val);                            /* VAL */
}

/*
 * Helper for hang tests.  Emits multiple submits, with one in the middle
 * that triggers a fault, and confirms that the submits before and after
 * the faulting one execute properly, ie. that the driver properly manages
 * to recover and re-queue the submits after the faulting submit;
 */
static void
do_hang_test(struct msm_pipe *pipe)
{
	struct msm_cmd *cmds[16];
	int fence_fds[ARRAY_SIZE(cmds)];

	memset(scratch, 0, 0x1000);

	for (unsigned i = 0; i < ARRAY_SIZE(cmds); i++) {
		struct msm_cmd *cmd = igt_msm_cmd_new(pipe, 0x1000);

		cmds[i] = cmd;

		/*
		 * Emit a packet to wait for scratch[0] to be >= 1
		 *
		 * This lets us force the GPU to wait until all the cmdstream is
		 * queued up.
		 */
		wait_mem_gte(cmd, 0, 1);

		if (i == 10) {
			msm_cmd_emit(cmd, 0xdeaddead);
		}

		/* Emit a packet to write scratch[1+i] = 2+i: */
		mem_write(cmd, 1+i, 2+i);
	}

	for (unsigned i = 0; i < ARRAY_SIZE(cmds); i++) {
		fence_fds[i] = igt_msm_cmd_submit(cmds[i]);
	}

	usleep(10000);

	/* Let the WAIT_MEM_GTE complete: */
	scratch[0] = 1;

	for (unsigned i = 0; i < ARRAY_SIZE(cmds); i++) {
		igt_wait_and_close(fence_fds[i]);
		igt_msm_cmd_free(cmds[i]);
		if (i == 10)
			continue;
		igt_assert_eq(scratch[1+i], 2+i);
	}

	read_and_clear_devcore();
}

/**
 * SUBTEST: gpu-fault-parallel
 *
 * Description: does a bunch of submits in parallel threads, a subset of
 * which trigger GPU hangs.  For the submits which do not trigger hangs,
 * validate that they executed properly by checking that they were able
 * to write to the scratch buffer, so that we can see that the kernel
 * properly re-plays the non-faulting submits.
 */
static void
do_parallel_test(struct msm_pipe *pipe, int child)
{
	struct msm_cmd *cmd = igt_msm_cmd_new(pipe, 0x1000);
	bool hang = child == 5;
	int fence_fd;

	msm_cmd_pkt7(cmd, CP_NOP, 0);

	if (hang) {
		msm_cmd_emit(cmd, 0xdeaddead);
	} else {
		/* Each forked thread writes/reads offset of child idx dwords: */
		msm_cmd_pkt7(cmd, CP_MEM_WRITE, 3);
		msm_cmd_bo  (cmd, scratch_bo, child * 4); /* ADDR_LO/HI */
		msm_cmd_emit(cmd, child + 1);             /* VAL */
	}

	igt_until_timeout(15) {
		scratch[child] = 0;
		fence_fd = igt_msm_cmd_submit(cmd);
		igt_wait_and_close(fence_fd);

		if (hang) {
			read_and_clear_devcore();
		} else {
			/* verify that non-crashing submits succeeded: */
			igt_assert_eq(scratch[child], child + 1);
		}
	}

	igt_msm_cmd_free(cmd);
}

static void
do_fault_test(struct msm_pipe *pipe, bool stress)
{
	struct msm_cmd *cmd = igt_msm_cmd_new(pipe, 0x10000);
	unsigned int cnt = stress ? 0x10000 / 16 : 1;

	for (unsigned int i = 0; i < cnt; i++) {
		msm_cmd_pkt7(cmd, CP_MEM_WRITE, 3);
		msm_cmd_emit(cmd, 0xdeaddead);           /* ADDR_LO */
		msm_cmd_emit(cmd, 0x1);                  /* ADDR_HI */
		msm_cmd_emit(cmd, 0x123);                /* VAL */
	}

	igt_wait_and_close(igt_msm_cmd_submit(cmd));
}

/*
 * Tests for drm/msm hangcheck, recovery, and fault handling
 */

igt_main
{
	static struct msm_pipe *pipe = NULL;

	igt_fixture() {
		dev = igt_msm_dev_open();
		pipe = igt_msm_pipe_open(dev, 0);
		scratch_bo = igt_msm_bo_new(dev, 0x1000, MSM_BO_WC);
		scratch = igt_msm_bo_map(scratch_bo);
	}

	igt_describe("Test sw hangcheck handling");
	igt_subtest("hangcheck") {
		igt_require(dev->gen >= 6);
		igt_require(igt_debugfs_exists(dev->fd, "disable_err_irq", O_WRONLY));

		/* Disable hw hang detection to force fallback to sw hangcheck: */
		igt_debugfs_write(dev->fd, "disable_err_irq", "Y");

		do_hang_test(pipe);

		igt_debugfs_write(dev->fd, "disable_err_irq", "N");
	}

	igt_describe("Test hw fault handling");
	igt_subtest("gpu-fault") {
		igt_require(dev->gen >= 6);

		do_hang_test(pipe);
	}

	igt_describe("Parallel fault handling");
	igt_subtest("gpu-fault-parallel") {
		igt_require(dev->gen >= 6);

		igt_fork(child, 20) {
			do_parallel_test(pipe, child);
		}
		igt_waitchildren();
	}

	igt_describe("Test iova fault handling");
	igt_subtest("iova-fault") {
		igt_require(dev->gen >= 6);

		do_fault_test(pipe, false);
	}

	igt_describe("Test iova fault handling (stress)");
	igt_subtest("iova-fault-stress") {
		igt_require(dev->gen >= 6);

		do_fault_test(pipe, true);
	}

	igt_fixture() {
		igt_msm_bo_free(scratch_bo);
		igt_msm_pipe_close(pipe);
		igt_msm_dev_close(dev);
	}
}
