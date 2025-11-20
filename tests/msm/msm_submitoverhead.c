// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Google, Inc.
 */

#include "igt.h"
#include "igt_msm.h"

/*
 * Not as much a test, as a kernel submit overhead benchmark.  Generates lots
 * of submit ioctls with various size #s of buffers attached for measuring
 * and profiling kernel submit CPU overhead.
 */

#define MAX_BOS 1000

igt_main
{
	struct msm_device *dev = NULL;
	struct msm_pipe *pipe = NULL;
	struct msm_bo *bos[MAX_BOS];
	struct drm_msm_gem_submit_bo bos_table[MAX_BOS];
	static const int sizes[] = {
		10, 100, 250, 500, 1000,
	};

	igt_fixture() {
		struct drm_msm_gem_submit req;

		dev = igt_msm_dev_open();
		pipe = igt_msm_pipe_open(dev, 0);
		for (int i = 0; i < MAX_BOS; i++) {
			bos[i] = igt_msm_bo_new(dev, 0x1000, MSM_BO_WC);
			bos_table[i] = (struct drm_msm_gem_submit_bo) {
				.handle = bos[i]->handle,
				/*
				 * We don't bother testing BO_READ since
				 * mesa doesn't use that anymore
				 */
				.flags  = MSM_SUBMIT_BO_WRITE,
			};
		}

		/*
		 * Prime the pump, so first submit doesn't take the overhead
		 * of allocating backing pages:
		 */
		req = (struct drm_msm_gem_submit) {
			.flags   = pipe->pipe | MSM_SUBMIT_FENCE_FD_OUT,
			.queueid = pipe->submitqueue_id,
			.nr_bos  = ARRAY_SIZE(bos_table),
			.bos     = VOID2U64(bos_table),
		};
		do_ioctl(dev->fd, DRM_IOCTL_MSM_GEM_SUBMIT, &req);
		igt_wait_and_close(req.fence_fd);
	}

	for (int i = 0; i < ARRAY_SIZE(sizes); i++) {
		for (int mode = 0; mode < 2; mode++) {
			const char *modestr = mode ? "-no-implicit-sync" : "";
			const uint32_t modeflags = mode ? MSM_SUBMIT_NO_IMPLICIT : 0;

			igt_subtest_f("submitbench-%u-bos%s", sizes[i], modestr) {
				struct drm_msm_gem_submit req = {
					.flags   = pipe->pipe | modeflags,
					.queueid = pipe->submitqueue_id,
					.nr_bos  = sizes[i],
					.bos     = VOID2U64(bos_table),
				};
				unsigned int iterations = 0;

				igt_for_milliseconds(2000) {
					do_ioctl(dev->fd, DRM_IOCTL_MSM_GEM_SUBMIT, &req);
					iterations++;
				}
				igt_info("%u-bos: %u iterations\n", sizes[i], iterations);
			}
		}
	}

	igt_fixture() {
		for (int i = 0; i < MAX_BOS; i++)
			igt_msm_bo_free(bos[i]);
		igt_msm_pipe_close(pipe);
		igt_msm_dev_close(dev);
	}
}
