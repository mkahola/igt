// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

/**
 * TEST: xe sysfs scheduler
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: SysMan tests
 * Functionality: scheduler control interface
 *
 * SUBTEST: %s-invalid
 * Description: Test to check if %s arg[1] schedule parameter rejects any
 *		unrepresentable intervals.
 * Test category: negative test
 *
 * SUBTEST: %s-invalid-string
 * Description: Test to check if %s arg[1] schedule parameter checks for
 *		invalid string values.
 * Test category: negative test
 *
 * SUBTEST: %s-invalid-large-string
 * Description: Test to check if %s arg[1] schedule parameter checks for
 *		large invalid strings (4k).
 * Test category: negative test
 *
 * SUBTEST: %s-min-max
 * Description: Test to check if %s arg[1] schedule parameter checks for
 *		min max values.
 * Test category: functionality test
 *
 * arg[1]:
 *
 * @preempt_timeout_us:		preempt timeout us
 * @timeslice_duration_us:	timeslice duration us
 * @job_timeout_ms:		job timeout ms
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

#define STR_LENGTH 4096

/**
 * generate_random_string:
 * @str: pointer to string buffer that will be having random string generated.
 * @length: length of string to generate.
 *
 * Generates random string that will always contain non-numerical characters.
 */
static void generate_random_string(char *str, size_t length)
{
	int type = 0;
	int digit_count = 0;
	int max_digits = length / 4;
	int loop_count = length - 1;

	for (size_t i = 0; i < loop_count; i++) {
		if (digit_count >= max_digits)
			type = rand() % 2;
		else
			type = rand() % 3;

		switch (type) {
		case 0:
			str[i] = 'A' + (rand() % 26);
			break;
		case 1:
			str[i] = 'a' + (rand() % 26);
			break;
		case 2:
			str[i] = '0' + (rand() % 10);
			digit_count++;
			break;
		default:
			str[i] = '_';
			break;
		}
	}
	str[length - 1] = '\0';
}

static void test_invalid(int xe, int engine, const char **property,
			 uint16_t class, int gt)
{
	unsigned int saved, set;
	unsigned int min, max;

	igt_sysfs_scanf(engine, property[2], "%u", &max);
	igt_sysfs_scanf(engine, property[1], "%u", &min);

	igt_assert(igt_sysfs_scanf(engine, property[0], "%u", &saved) == 1);
	igt_debug("Initial %s:%u\n", property[0], saved);

	igt_sysfs_printf(engine, property[0], "%d", max+100);
	igt_sysfs_scanf(engine, property[0], "%u", &set);
	igt_assert_eq(set, saved);

	igt_sysfs_printf(engine, property[0], "%d", min-100);
	igt_sysfs_scanf(engine, property[0], "%u", &set);
	igt_assert_eq(set, saved);
}

static void test_min_max(int xe, int engine, const char **property,
			 uint16_t class, int gt)
{
	unsigned int default_max, max;
	unsigned int default_min, min;
	unsigned int set, store;
	int defaults;

	defaults = openat(engine, ".defaults", O_DIRECTORY);
	igt_require(defaults != -1);

	igt_sysfs_scanf(defaults, property[2], "%u", &default_max);
	igt_sysfs_scanf(defaults, property[1], "%u", &default_min);
	igt_sysfs_scanf(engine, property[0], "%u", &store);

	igt_sysfs_printf(engine, property[2], "%d", default_max-10);
	igt_sysfs_scanf(engine, property[2], "%u", &max);
	igt_assert_eq(max, (default_max-10));

	igt_sysfs_printf(engine, property[2], "%d", default_max+1);
	igt_sysfs_scanf(engine, property[2], "%u", &max);
	igt_assert_neq(max, (default_max+1));

	igt_sysfs_printf(engine, property[1], "%d", default_min+1);
	igt_sysfs_scanf(engine, property[1], "%u", &min);
	igt_assert_eq(min, (default_min+1));

	igt_sysfs_printf(engine, property[1], "%d", default_min-10);
	igt_sysfs_scanf(engine, property[1], "%u", &min);
	igt_assert_neq(min, (default_min-10));

	igt_sysfs_printf(engine, property[0], "%d", min);
	igt_sysfs_scanf(engine, property[0], "%u", &set);
	igt_assert_eq(set, min);

	igt_sysfs_printf(engine, property[0], "%d", max);
	igt_sysfs_scanf(engine, property[0], "%u", &set);
	igt_assert_eq(set, max);

	igt_sysfs_printf(engine, property[0], "%d", default_min);
	igt_sysfs_scanf(engine, property[0], "%u", &set);
	igt_assert_neq(set, default_min);

	igt_sysfs_printf(engine, property[0], "%d", min);
	igt_sysfs_scanf(engine, property[0], "%u", &set);
	igt_assert_eq(set, min);

	/* Reset property, max, min to original values */
	igt_sysfs_printf(engine, property[0], "%d", store);
	igt_sysfs_scanf(engine, property[0], "%u", &set);
	igt_assert_eq(set, store);

	igt_sysfs_printf(engine, property[1], "%d", default_min);
	igt_sysfs_scanf(engine, property[1], "%u", &set);
	igt_assert_eq(set, default_min);

	igt_sysfs_printf(engine, property[2], "%d", default_max);
	igt_sysfs_scanf(engine, property[2], "%u", &set);
	igt_assert_eq(set, default_max);
}

static void test_invalid_string(int xe, int engine, const char **property,
				 uint16_t class, int gt)
{
	unsigned int saved, set;
	static const char invalid_input[] = "999abc";

	for (int i = 0; i < 3; i++) {
		igt_assert_eq(igt_sysfs_scanf(engine, property[i], "%u", &saved), 1);
		igt_info("Initial %s: %u\n", property[i], saved);
		/* Assert if the invalid write is returning negative error */
		igt_assert_lt(igt_sysfs_printf(engine, property[i], "%s",
			      invalid_input), 0);

		igt_assert_eq(igt_sysfs_scanf(engine, property[i], "%u", &set), 1);
		/* Check if the values are unchanged. */
		igt_assert_eq(set, saved);
	}
}

static void test_invalid_large_string(int xe, int engine, const char **property,
				       uint16_t class, int gt)
{
	unsigned int saved, set;
	char *random_str;

	random_str = (char *)malloc(sizeof(char) * (STR_LENGTH));
	igt_assert_f(random_str, "Memory allocation failed\n");

	generate_random_string(random_str, STR_LENGTH);
	igt_debug("Generated random string: %.10s...\n", random_str);

	for (int i = 0; i < 3; i++) {
		igt_assert_eq(igt_sysfs_scanf(engine, property[i], "%u", &saved), 1);
		igt_info("Initial %s: %u\n", property[i], saved);

		/* Assert if the invalid write is returning negative error */
		igt_assert_lt(igt_sysfs_printf(engine, property[i], "%s",
			      random_str), 0);

		igt_assert_eq(igt_sysfs_scanf(engine, property[i], "%u", &set), 1);
		/* Check if the values are unchanged. */
		igt_assert_eq(set, saved);
	}
	free(random_str);
}

#define MAX_GTS 8
igt_main
{
	static const struct {
		const char *name;
		void (*fn)(int, int, const char **, uint16_t, int);
	} tests[] = {
		{ "invalid", test_invalid },
		{ "invalid-string", test_invalid_string },
		{ "invalid-large-string", test_invalid_large_string },
		{ "min-max", test_min_max },
		{ }
	};

	const char *property[][3] = { {"preempt_timeout_us", "preempt_timeout_min", "preempt_timeout_max"},
				      {"timeslice_duration_us", "timeslice_duration_min", "timeslice_duration_max"},
				      {"job_timeout_ms", "job_timeout_min", "job_timeout_max"},
	};

	unsigned int store[MAX_GTS][3][3];
	int count = sizeof(property) / sizeof(property[0]);
	int gt_count = 0;
	int xe = -1;
	int sys_fd;
	int gt;
	int engines_fd[MAX_GTS], gt_fd[MAX_GTS];
	int *engine_list[MAX_GTS];

	igt_fixture() {
		xe = drm_open_driver(DRIVER_XE);
		xe_device_get(xe);

		sys_fd = igt_sysfs_open(xe);
		igt_require(sys_fd != -1);
		close(sys_fd);

		xe_for_each_gt(xe, gt) {
			int *list, i = 0;

			igt_require(gt_count < MAX_GTS);

			gt_fd[gt_count] = xe_sysfs_gt_open(xe, gt);
			igt_require(gt_fd[gt_count] != -1);
			engines_fd[gt_count] = openat(gt_fd[gt_count], "engines", O_RDONLY);
			igt_require(engines_fd[gt_count] != -1);

			list = igt_sysfs_get_engine_list(engines_fd[gt_count]);

			while (list[i] != -1) {
				for (int j = 0; j < count; j++) {
					const char **pl = property[j];

					for (int k = 0; k < 3; k++) {
						unsigned int *loc = &store[i][j][k];

						igt_require(igt_sysfs_scanf(list[i], pl[k],
									    "%u", loc) == 1);
					}
				}
				i++;
			}

			igt_require(i > 0);
			engine_list[gt_count] = list;
			gt_count++;
		}
	}

	for (int i = 0; i < count; i++) {
		for (typeof(*tests) *t = tests; t->name; t++) {
			igt_subtest_with_dynamic_f("%s-%s", property[i][0], t->name) {
				int j = 0;
				xe_for_each_gt(xe, gt) {
					int e = engines_fd[j];

					igt_sysfs_engines(xe, e, 0, 0, property[i], t->fn);
					j++;
				}
			}
		}
	}

	igt_fixture() {
		for (int gtn = gt_count - 1; gtn >= 0; gtn--) {
			int *list, i = 0;

			list = engine_list[gtn];

			while (list[i] != -1) {
				int e = list[i];

				for (int j = count - 1; j >= 0; j--) {
					const char **pl = property[j];

					for (int k = 2; k >= 0; k--) {
						unsigned int read = UINT_MAX;
						unsigned int val = store[i][j][k];

						igt_sysfs_printf(e, pl[k], "%u", val);
						igt_sysfs_scanf(e, pl[k], "%u", &read);
						igt_abort_on_f(read != val,
							       "%s not restored!\n", pl[k]);
					}
				}
				i++;
			}

			igt_sysfs_free_engine_list(list);
			close(engines_fd[gtn]);
			close(gt_fd[gtn]);
		}

		xe_device_put(xe);
		close(xe);
	}
}

