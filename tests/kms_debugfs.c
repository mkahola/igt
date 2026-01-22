// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */

#include "igt.h"
#include "igt_debugfs.h"
#include "igt_dir.h"

/**
 * TEST: kms debugfs test
 * Description: Read entries from debugfs with all displays on and with
 *		all displays off.
 *
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: uapi
 * Functionality: debugfs
 * Feature: core
 * Test category: uapi
 *
 * SUBTEST: display-off-read-all
 * Description: Read all debugfs entries with display off.
 *
 * SUBTEST: display-on-read-all
 * Description: Read all debugfs entries with display on.
 */

/**
 * igt_display_all_on: Try to turn on all displays
 * @display: pointer to the igt_display structure
 *
 * Returns: void
 */
static void igt_display_all_on(igt_display_t *display)
{
	struct igt_fb fb[IGT_MAX_PIPES];
	enum pipe pipe;

	/* try to light all pipes */
	for_each_pipe(display, pipe) {
		igt_output_t *output;

		for_each_valid_output_on_pipe(display, pipe, output) {
			igt_plane_t *primary;
			drmModeModeInfo *mode;

			if (igt_output_get_driving_crtc(output) != NULL)
				continue;

			igt_output_set_crtc(output,
					    igt_crtc_for_pipe(output->display, pipe));
			primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
			mode = igt_output_get_mode(output);
			igt_create_pattern_fb(display->drm_fd,
					      mode->hdisplay, mode->vdisplay,
					      DRM_FORMAT_XRGB8888,
					      DRM_FORMAT_MOD_LINEAR, &fb[pipe]);

			/* Set a valid fb as some debugfs like to
			 * inspect it on a active pipe
			 */
			igt_plane_set_fb(primary, &fb[pipe]);
			break;
		}
	}

	/* Skip if bandwidth is insufficient for all simultaneous displays */
	igt_require(igt_fit_modes_in_bw(display));

	igt_display_commit2(display, display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);
}

/**
 * igt_display_all_off: Try to turn off all displays
 * @display: pointer to the igt_display structure
 *
 * Returns: void
 */
static void igt_display_all_off(igt_display_t *display)
{
	enum pipe pipe;
	igt_output_t *output;
	igt_plane_t *plane;

	for_each_connected_output(display, output)
		igt_output_set_crtc(output, NULL);

	for_each_pipe(display, pipe)
		for_each_plane_on_pipe(display, pipe, plane)
			igt_plane_set_fb(plane, NULL);

	igt_display_commit2(display, display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);
}

IGT_TEST_DESCRIPTION("Read entries from debugfs with display on/off.");

int igt_main()
{
	int debugfs = -1;
	igt_display_t display;
	int fd = -1;

	igt_fixture() {
		fd = drm_open_driver_master(DRIVER_ANY);
		debugfs = igt_debugfs_dir(fd);
		igt_require(debugfs >= 0);

		kmstest_set_vt_graphics_mode();

		igt_display_require(&display, fd);

		/* Make sure we have at least one output connected */
		igt_display_require_output(&display);
	}

	igt_subtest("display-off-read-all") {
		igt_display_all_off(&display);

		igt_dir_process_files_simple(debugfs);
	}

	igt_subtest("display-on-read-all") {
		/* try to light all pipes */
		igt_display_all_on(&display);

		igt_dir_process_files_simple(debugfs);
	}

	igt_fixture() {
		igt_display_fini(&display);
		close(debugfs);
		drm_close_driver(fd);
	}
}
