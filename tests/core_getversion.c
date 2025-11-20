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

#include "igt.h"
#include <string.h>
#include <sys/ioctl.h>
/**
 * TEST: core getversion
 * Description: Tests the DRM_IOCTL_GET_VERSION ioctl and libdrm's drmGetVersion() interface to it.
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: DRM
 * Functionality: permission management for clients
 * Feature: core
 * Test category: GEM_Legacy
 *
 * SUBTEST: basic
 * Description: Tests GET_VERSION ioctl of the first device.
 *
 * SUBTEST: all-cards
 * Description: Tests GET_VERSION ioctl for all drm devices.
 */

IGT_TEST_DESCRIPTION("Tests the DRM_IOCTL_GET_VERSION ioctl and libdrm's "
		     "drmGetVersion() interface to it.");

static void check(int fd, char *dst, int len)
{
	drmVersionPtr v;

	v = drmGetVersion(fd);
	igt_assert_neq(strlen(v->name), 0);
	igt_assert_neq(strlen(v->date), 0);
	igt_assert_neq(strlen(v->desc), 0);
	if (is_i915_device(fd))
		igt_assert_lte(1, v->version_major);

	snprintf(dst, len, "%s v%d.%d %s %s", v->name, v->version_major,
		v->version_minor, v->date, v->desc);
	dst[len - 1] = 0;
	drmFree(v);
}

static void check_all_drm(void)
{
	char info[256];
	int fd2;

	for (int i = 0; ; i++) {
		fd2 = __drm_open_driver_another(i, DRIVER_ANY);
		if (fd2 == -1)
			break;

		check(fd2, info, sizeof(info));
		igt_info("%d: %s\n", i, info);
		drm_close_driver(fd2);
	}
}

igt_main()
{
	char info[256];
	int fd;

	igt_fixture() {
		fd = __drm_open_driver(DRIVER_ANY);
		igt_assert_fd(fd);
	}

	igt_describe("Check GET_VERSION ioctl of the first drm device.");
	igt_subtest("basic") {
		check(fd, info, sizeof(info));
		igt_info("0: %s\n", info);
	}

	igt_describe("Check GET_VERSION ioctl for all drm devices.");
	igt_subtest("all-cards")
		check_all_drm();

	igt_fixture()
		drm_close_driver(fd);
}
