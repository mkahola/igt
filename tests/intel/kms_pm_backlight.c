/*
 * Copyright © 2015 Intel Corporation
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
 * Author:
 *    Antti Koskipaa <antti.koskipaa@linux.intel.com>
 *
 */

/**
 * TEST: kms pm backlight
 * Category: Display
 * Description: Basic backlight sysfs test
 * Driver requirement: i915, xe
 * Mega feature: Display Power Management
 */

#include <errno.h>
#include <fcntl.h>
#include <libgen.h> /* for POSIX basename */
#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "igt.h"
#include "igt_device.h"
#include "igt_device_scan.h"

/**
 * SUBTEST: bad-brightness
 * Description: Test the bad brightness.
 *
 * SUBTEST: basic-brightness
 * Description: Test the basic brightness.
 *
 * SUBTEST: fade
 * Description: Test basic fade.
 *
 * SUBTEST: fade-with-dpms
 * Description: Test the fade with DPMS.
 *
 * SUBTEST: fade-with-suspend
 * Description: Test the fade with suspend.
 *
 * SUBTEST: brightness-with-dpms
 * Description: test brightness with dpms on and off cycle.
 */

enum {
	TEST_NONE = 0,
	TEST_DPMS,
	TEST_DPMS_CYCLE,
	TEST_SUSPEND,
};

#define TOLERANCE 5 /* percent */
#define BACKLIGHT_PATH "/sys/class/backlight"

#define FADESTEPS 10
#define FADESPEED 100 /* milliseconds between steps */

#define NUM_EDP_OUTPUTS 2

IGT_TEST_DESCRIPTION("Basic backlight sysfs test");

static void test_and_verify(igt_backlight_context_t *context, int val)
{
	const int tolerance = val * TOLERANCE / 100;
	int result;

	igt_assert_eq(igt_backlight_write(val, "brightness", context), 0);
	igt_assert_eq(igt_backlight_read(&result, "brightness", context), 0);
	/* Check that the exact value sticks */
	igt_assert_eq(result, val);

	igt_assert_eq(igt_backlight_read(&result, "actual_brightness", context), 0);
	/* Some rounding may happen depending on hw */
	igt_assert_f(result >= max(0, val - tolerance) &&
		     result <= min(context->max, val + tolerance),
		     "actual_brightness [%d] did not match expected brightness [%d +- %d]\n",
		     result, val, tolerance);
}

static void test_brightness(igt_backlight_context_t *context)
{
	test_and_verify(context, 0);
	test_and_verify(context, context->max);
	test_and_verify(context, context->max / 2);
}

static void test_bad_brightness(igt_backlight_context_t *context)
{
	int val;
	/* First write some sane value */
	igt_backlight_write(context->max / 2, "brightness", context);
	/* Writing invalid values should fail and not change the value */
	igt_assert_lt(igt_backlight_write(-1, "brightness", context), 0);
	igt_backlight_read(&val, "brightness", context);
	igt_assert_eq(val, context->max / 2);
	igt_assert_lt(igt_backlight_write(context->max + 1, "brightness", context), 0);
	igt_backlight_read(&val, "brightness", context);
	igt_assert_eq(val, context->max / 2);
	igt_assert_lt(igt_backlight_write(INT_MAX, "brightness", context), 0);
	igt_backlight_read(&val, "brightness", context);
	igt_assert_eq(val, context->max / 2);
}

static void test_fade(igt_backlight_context_t *context)
{
	int i;
	static const struct timespec ts = { .tv_sec = 0, .tv_nsec = FADESPEED*1000000 };

	/* Fade out, then in */
	for (i = context->max; i > 0; i -= context->max / FADESTEPS) {
		test_and_verify(context, i);
		nanosleep(&ts, NULL);
	}
	for (i = 0; i <= context->max; i += context->max / FADESTEPS) {
		test_and_verify(context, i);
		nanosleep(&ts, NULL);
	}
}

static void check_dpms_cycle(igt_backlight_context_t *context)
{
	int max, val_1, val_2;

	igt_backlight_read(&max, "max_brightness", context);
	igt_assert(max);

	igt_backlight_write(max / 2, "brightness", context);
	igt_backlight_read(&val_1, "actual_brightness", context);

	igt_pm_dpms_toggle(context->output);

	igt_backlight_read(&val_2, "actual_brightness", context);
	igt_assert_eq(val_1, val_2);
}

static void
check_suspend(igt_output_t *output)
{
	igt_system_suspend_autoresume(SUSPEND_STATE_MEM, SUSPEND_TEST_NONE);
}

static void test_cleanup(igt_display_t *display, igt_output_t *output)
{
	igt_output_set_crtc(output, NULL);
	igt_display_commit2(display, display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);
	igt_pm_restore_sata_link_power_management();
}

static void test_setup(igt_display_t display, igt_output_t *output)
{
	igt_plane_t *primary;
	drmModeModeInfo *mode;
	struct igt_fb fb;
	enum pipe pipe;

	igt_display_reset(&display);

	for_each_pipe(&display, pipe) {
		igt_output_set_crtc(output,
				    igt_crtc_for_pipe(output->display, pipe));
		if (!intel_pipe_output_combo_valid(&display)) {
			igt_output_set_crtc(output, NULL);
			continue;
		}
		mode = igt_output_get_mode(output);

		igt_create_pattern_fb(display.drm_fd,
				      mode->hdisplay, mode->vdisplay,
				      DRM_FORMAT_XRGB8888,
				      DRM_FORMAT_MOD_LINEAR, &fb);
		primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
		igt_plane_set_fb(primary, &fb);

		igt_display_commit2(&display, display.is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);
		igt_pm_enable_sata_link_power_management();

		break;
	}
}

int igt_main()
{
	int fd;
	int i = 0;
	igt_display_t display;
	igt_output_t *output;
	char file_path_n[PATH_MAX] = "";
	bool dual_edp = false;
	igt_backlight_context_t contexts[NUM_EDP_OUTPUTS];
	struct {
		const char *name;
		const char *desc;
		void (*test_t)(igt_backlight_context_t *context);
		int flags;
	} tests[] = {
		{ "basic-brightness", "test the basic brightness.", test_brightness, TEST_NONE },
		{ "bad-brightness", "test the bad brightness.", test_bad_brightness, TEST_NONE },
		{ "fade", "test basic fade.", test_fade, TEST_NONE },
		{ "fade-with-dpms", "test the fade with DPMS.", test_fade, TEST_DPMS },
		{ "brightness-with-dpms", "test brightness with dpms on and off cycle.",
		   check_dpms_cycle, TEST_DPMS_CYCLE},
		{ "fade-with-suspend", "test the fade with suspend.", test_fade, TEST_SUSPEND },
	};

	igt_fixture() {
		bool found = false;
		char full_name[32] = {};
		char *name;

		/*
		 * Backlight tests requires the output to be enabled,
		 * try to enable all.
		 */
		kmstest_set_vt_graphics_mode();
		igt_display_require(&display, drm_open_driver(DRIVER_INTEL | DRIVER_XE));

		for_each_connected_output(&display, output) {
			if (!output_is_internal_panel(output))
				continue;

			if (found)
				snprintf(file_path_n, PATH_MAX, "%s/card%i-%s-backlight/brightness",
					 BACKLIGHT_PATH, igt_device_get_card_index(display.drm_fd),
					 igt_output_name(output));
			else
				snprintf(file_path_n, PATH_MAX, "%s/intel_backlight/brightness",
					 BACKLIGHT_PATH);

			fd = open(file_path_n, O_RDONLY);
			if (fd == -1)
				continue;

			if (found)
				snprintf(contexts[i].path, PATH_MAX, "card%i-%s-backlight",
					 igt_device_get_card_index(display.drm_fd),
					 igt_output_name(output));
			else
				snprintf(contexts[i].path, PATH_MAX, "intel_backlight");

			close(fd);

			snprintf(contexts[i].backlight_dir_path, PATH_MAX, "%s", BACKLIGHT_PATH);

			/* should be ../../cardX-$output */
			snprintf(file_path_n, PATH_MAX, "%s/%s/device", BACKLIGHT_PATH,
				 contexts[i].path);
			igt_assert_lt(16, readlink(file_path_n, full_name, sizeof(full_name) - 1));
			name = basename(full_name);
			igt_assert(igt_backlight_read(&contexts[i].max,
						      "max_brightness", &contexts[i]) > -1);
			igt_skip_on(igt_backlight_read(&contexts[i].old,
						       "brightness", &contexts[i]));

			if (!strcmp(name + 6, output->name)) {
				contexts[i++].output = output;

				if (found)
					dual_edp = true;
				else
					found = true;
			}
		}
		igt_require_f(found, "No valid output found.\n");
	}

	for (i = 0; i < ARRAY_SIZE(tests); i++) {
		igt_describe(tests[i].desc);
		igt_subtest_with_dynamic(tests[i].name) {
			for (int j = 0; j < (dual_edp ? 2 : 1); j++) {
				test_setup(display, &contexts->output[j]);

				if (tests[i].flags == TEST_DPMS)
					igt_pm_dpms_toggle(contexts[j].output);

				if (tests[i].flags == TEST_SUSPEND)
					check_suspend(contexts[j].output);

				igt_dynamic_f("%s", igt_output_name(contexts[j].output)) {
					tests[i].test_t(&contexts[j]);
					test_cleanup(&display, contexts[j].output);
				}
			}
		}
	}

	igt_fixture() {
		/* Restore old brightness */
		for (i = 0; i < (dual_edp ? 2 : 1); i++)
			igt_backlight_write(contexts[i].old, "brightness", &contexts[i]);

		igt_display_fini(&display);
		igt_pm_restore_sata_link_power_management();
		drm_close_driver(display.drm_fd);
	}
}
