/*
 * Copyright © 2007 Intel Corporation
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
 *    Eric Anholt <eric@anholt.net>
 *
 */

#include <drm.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <sys/types.h>

#include "igt.h"

/**
 * TEST: core getstats
 * Description: Tests the DRM_IOCTL_GET_STATS ioctl.
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: uapi
 * Functionality: DRM memory management
 * Feature: core
 * Test category: GEM_Legacy
 *
 * SUBTEST: basic
 * Description: Tests the DRM_IOCTL_GET_STATS ioctl.
 */

/**
 * Checks DRM_IOCTL_GET_STATS.
 *
 * I don't care too much about the actual contents, just that the kernel
 * doesn't crash.
 */

IGT_TEST_DESCRIPTION("Tests the DRM_IOCTL_GET_STATS ioctl.");

igt_main()
{
	int fd, ret;
	drm_stats_t stats;

	igt_fixture()
		fd = drm_open_driver(DRIVER_ANY);

	igt_describe("Check DRM_IOCTL_GET_STATS ioctl of the first drm device.");
	igt_subtest("basic") {
		ret = ioctl(fd, DRM_IOCTL_GET_STATS, &stats);
		igt_assert(ret == 0);
	}

	igt_fixture()
		drm_close_driver(fd);
}
