// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (C) 2025 Collabora Ltd.

#include "igt.h"
#include "igt_core.h"
#include "igt_panthor.h"
#include "panthor_drm.h"
#include <stdint.h>

int igt_main() {
	int fd = -1;

	igt_fixture() {
		igt_panthor_skip_on_big_endian();
		fd = drm_open_driver(DRIVER_PANTHOR);
	}

	igt_describe("Query GPU information from ROM.");
	igt_subtest("query") {
		struct drm_panthor_gpu_info gpu = {};

		igt_panthor_query(fd, DRM_PANTHOR_DEV_QUERY_GPU_INFO, &gpu, sizeof(gpu), 0);

		igt_assert_neq(gpu.gpu_id, 0);
	}

	igt_fixture() {
		drm_close_driver(fd);
	}
}
