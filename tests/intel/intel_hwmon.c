// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include <dirent.h>
#include <sys/stat.h>
#include "igt.h"
#include "igt_hwmon.h"
#include "igt_sysfs.h"
/**
 * TEST: intel hwmon
 * Description: Tests for intel hwmon
 * Category: Core
 * Mega feature: RAS
 * Sub-category: RAS tests
 * Functionality: hwmon
 * Test category: functionality
 *
 * SUBTEST: hwmon-read
 * Description: Verify we can read all hwmon attributes
 *
 * SUBTEST: hwmon-write
 * Description: Verify writable hwmon attributes
 */

IGT_TEST_DESCRIPTION("Tests for intel hwmon");

static void check_if_temp_valid(int hwm, char *sysfs_name)
{
	int32_t cur_temp = 0, limit = 0;
	char str[32] = {0};
	uint8_t ch = 0;

	/* Get the channel number and sysfs entry suffix. */
	igt_assert(sscanf(sysfs_name, "temp%hhu_%s", &ch, str) == 2);

	/* If entry is tempX_input, check if it exceeds tempX_crit. */
	if (!strncmp("input", str, 5)) {
		sprintf(str, "temp%hhu_crit", ch);
		if (!faccessat(hwm, str, R_OK, 0)) {
			igt_assert_lt(0, igt_sysfs_scanf(hwm, sysfs_name, "%d", &cur_temp));
			igt_assert_lt(0, igt_sysfs_scanf(hwm, str, "%d", &limit));
			igt_debug("current temp = %d limit = %d\n", cur_temp, limit);
			igt_assert_f(cur_temp <= limit, "current temperature exceeds limit!\n");
		}
	}
}

static void hwmon_read(int hwm)
{
	struct dirent *de;
	char val[128];
	DIR *dir;

	dir = fdopendir(dup(hwm));
	igt_assert(dir);
	rewinddir(dir);

	while ((de = readdir(dir))) {
		if (de->d_type != DT_REG || !strcmp(de->d_name, "uevent"))
			continue;

		igt_assert(igt_sysfs_scanf(hwm, de->d_name, "%127s", val) == 1);
		igt_debug("'%s': %s\n", de->d_name, val);

		if (!strncmp(de->d_name, "temp", 4))
			check_if_temp_valid(hwm, de->d_name);

	}
	closedir(dir);
}

static void hwmon_write(int hwm)
{
	igt_sysfs_rw_attr_t rw;
	struct dirent *de;
	struct stat st;
	DIR *dir;

	dir = fdopendir(dup(hwm));
	igt_assert(dir);
	rewinddir(dir);

	rw.dir = hwm;
	rw.start = 1;
	rw.tol = 0.1;

	while ((de = readdir(dir))) {
		if (de->d_type != DT_REG || !strcmp(de->d_name, "uevent"))
			continue;

		igt_assert(!fstatat(hwm, de->d_name, &st, 0));
		if (!(st.st_mode & 0222))
			continue;

		rw.attr = de->d_name;
		igt_sysfs_rw_attr_verify(&rw);
	}
	closedir(dir);
}

int igt_main()
{
	int fd, hwm;

	igt_fixture() {
		fd = drm_open_driver_master(DRIVER_INTEL | DRIVER_XE);
		hwm = igt_hwmon_open(fd);
		igt_require(hwm >= 0);
	}

	igt_describe("Verify we can read all hwmon attributes");
	igt_subtest("hwmon-read") {
		hwmon_read(hwm);
	}

	igt_describe("Verify writable hwmon attributes");
	igt_subtest("hwmon-write") {
		hwmon_write(hwm);
	}

	igt_fixture() {
		close(hwm);
		drm_close_driver(fd);
	}
}
