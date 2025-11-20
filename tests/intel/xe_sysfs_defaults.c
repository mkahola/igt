// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

/**
 * TEST: xe sysfs defaults
 * Description: check if the sysfs engine .defaults node has all values.
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: SysMan tests
 * Functionality: sysman defaults
 * Test category: functionality test
 *
 * SUBTEST: engine-defaults
 * Description: check each engine defaults in sysfs.
 */

#include <dirent.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "igt.h"
#include "igt_sysfs.h"

#include "xe_drm.h"
#include "xe/xe_query.h"

static void test_defaults(int xe, int engine, const char **property,
			  uint16_t class, int gt)
{
	struct dirent *de;
	uint64_t property_value;
	int defaults;
	DIR *dir;

	defaults = openat(engine, ".defaults", O_DIRECTORY);
	igt_require(defaults != -1);

	dir = fdopendir(engine);
	while ((de = readdir(dir))) {
		if (*de->d_name == '.')
			continue;

		igt_debug("Checking attr '%s'\n", de->d_name);

		igt_assert_f(__igt_sysfs_get_u64(defaults, de->d_name, &property_value),
			     "Default value %s is not present!\n", de->d_name);

		igt_debug("Default property:%s, value:0x%" PRId64 "\n", de->d_name, property_value);

		igt_assert_f(!igt_sysfs_set(defaults, de->d_name, "garbage"),
					    "write into default value of %s succeeded!\n",
					    de->d_name);
	}
	closedir(dir);
}

int igt_main()
{
	int xe, sys_fd;
	int gt;

	igt_fixture() {
		xe = drm_open_driver(DRIVER_XE);
		xe_device_get(xe);

		sys_fd = igt_sysfs_open(xe);
		igt_require(sys_fd != -1);
		close(sys_fd);
	}

	igt_subtest_with_dynamic("engine-defaults") {
		xe_for_each_gt(xe, gt) {
			int engines_fd = -1;
			int gt_fd = -1;

			gt_fd = xe_sysfs_gt_open(xe, gt);
			igt_require(gt_fd != -1);
			engines_fd = openat(gt_fd, "engines", O_RDONLY);
			igt_require(engines_fd != -1);

			igt_sysfs_engines(xe, engines_fd, 0, 0, NULL, test_defaults);

			close(engines_fd);
			 close(gt_fd);
		}
	}

	igt_fixture() {
		xe_device_put(xe);
		close(xe);
	}
}

