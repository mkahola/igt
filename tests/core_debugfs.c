// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */

#include "igt.h"
#include "igt_debugfs.h"
#include "igt_dir.h"

/**
 * TEST: debugfs test
 * Description: Read entries from debugfs
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: uapi
 * Functionality: debugfs
 * Feature: core
 * Test category: uapi
 *
 * SUBTEST: read-all-entries
 * Description: Read all entries from debugfs path validating debugfs entries
 */

IGT_TEST_DESCRIPTION("Read entries from debugfs");

igt_main
{
	int debugfs = -1;
	int fd = -1;

	igt_fixture() {
		fd = drm_open_driver_master(DRIVER_ANY);
		debugfs = igt_debugfs_dir(fd);
		igt_require(debugfs >= 0);

		kmstest_set_vt_graphics_mode();
	}

	igt_describe("Read all entries from debugfs path.");
	igt_subtest("read-all-entries") {
		igt_dir_process_files_simple(debugfs);
	}

	igt_fixture() {
		close(debugfs);
		drm_close_driver(fd);
	}
}
