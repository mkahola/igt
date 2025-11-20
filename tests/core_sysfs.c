// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */

#include "igt.h"
#include "igt_dir.h"
#include "igt_sysfs.h"

/**
 * TEST: sysfs test
 * Description: Read entries from sysfs path.
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: uapi
 * Functionality: sysfs
 * Feature: core
 * Test category: uapi
 *
 * SUBTEST: read-all-entries
 * Description: Read all entries from sysfs path
 *
 */

IGT_TEST_DESCRIPTION("Read entries from sysfs paths.");

int igt_main()
{
	int fd = -1;
	int sysfs = -1;

	igt_fixture() {
		fd = drm_open_driver_master(DRIVER_ANY);
		sysfs = igt_sysfs_open(fd);
		igt_require(sysfs >= 0);

		kmstest_set_vt_graphics_mode();
	}

	igt_describe("Read all entries from sysfs path.");
	igt_subtest("read-all-entries")
		igt_dir_process_files_simple(sysfs);

	igt_fixture() {
		close(sysfs);
		drm_close_driver(fd);
	}
}
