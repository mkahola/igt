/*
 * Copyright © 2014 Intel Corporation
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
 *   Ander Conselvan de Oliveira <ander.conselvan.de.oliveira@intel.com>
 */

/**
 * TEST: kms flip tiling
 * Category: Display
 * Description: Test page flips and tiling scenarios
 * Driver requirement: i915, xe
 * Mega feature: General Display Features
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "igt.h"

/**
 * SUBTEST: flip-change-tiling
 * Description: Check pageflip between modifiers
 */

IGT_TEST_DESCRIPTION("Test page flips and tiling scenarios");

typedef struct {
	int drm_fd;
	igt_display_t display;
	int gen;
	uint32_t testformat;
	struct igt_fb fb[2];
	struct igt_fb old_fb[2];
	igt_pipe_crc_t *pipe_crc;
	bool flipevent_in_queue; // if test fails may need to handle rogue event
} data_t;

static void pipe_crc_free(data_t *data)
{
	if (!data->pipe_crc)
		return;

	igt_pipe_crc_stop(data->pipe_crc);
	igt_pipe_crc_free(data->pipe_crc);
	data->pipe_crc = NULL;
}

static void pipe_crc_new(data_t *data, igt_crtc_t *crtc)
{
	if (data->pipe_crc)
		return;

	data->pipe_crc = igt_crtc_crc_new(crtc,
					  IGT_PIPE_CRC_SOURCE_AUTO);
	igt_assert(data->pipe_crc);
	igt_pipe_crc_start(data->pipe_crc);
}

static int try_commit(igt_display_t *display)
{
	return igt_display_try_commit2(display, display->is_atomic ?
				       COMMIT_ATOMIC : COMMIT_LEGACY);
}

/* This helper is used to restrict tests to commonly supported tiling modes
 * across platforms for simulation.
 */
static bool is_basic_tiling_modifier(uint64_t mod)
{
	return mod == DRM_FORMAT_MOD_LINEAR ||
	       mod == I915_FORMAT_MOD_4_TILED ||
	       mod == I915_FORMAT_MOD_X_TILED;
}

static uint64_t pageflip_timeout_us(drmModeModeInfo *mode)
{
	uint64_t timeout_ns;

	/* 1 frame for flip + 1 frame for vblank wait due to FBC. */
	timeout_ns = igt_kms_frame_time_from_vrefresh(mode->vrefresh) * 2;
	/* 20 msec scheduling overhead. */
	timeout_ns += 20000000;

	return DIV_ROUND_UP(timeout_ns, 1000);
}

static void
test_flip_tiling(data_t *data, igt_crtc_t *crtc, igt_output_t *output,
		 uint64_t modifier[2])
{
	drmModeModeInfo *mode;
	igt_plane_t *primary;
	igt_crc_t reference_crc, crc;
	int fb_id, ret;

	memcpy(&data->old_fb, &data->fb, sizeof(data->fb));

	mode = igt_output_get_mode(output);

	primary = igt_output_get_plane(output, 0);

	fb_id = igt_create_pattern_fb(data->drm_fd,
				      mode->hdisplay, mode->vdisplay,
				      data->testformat, modifier[0],
				      &data->fb[0]);
	igt_assert(fb_id);

	/* Second fb has different background so CRC does not match. */
	fb_id = igt_create_color_pattern_fb(data->drm_fd,
					    mode->hdisplay, mode->vdisplay,
					    data->testformat, modifier[1],
					    0.5, 0.5, 0.5, &data->fb[1]);
	igt_assert(fb_id);

	/* Set the crtc and generate a reference CRC. */
	igt_plane_set_fb(primary, &data->fb[1]);
	igt_require_f(try_commit(&data->display) == 0,
		      "commit failed with " IGT_MODIFIER_FMT "\n",
		      IGT_MODIFIER_ARGS(modifier[1]));
	pipe_crc_new(data, crtc);
	igt_pipe_crc_get_current(data->drm_fd, data->pipe_crc, &reference_crc);

	/* Commit the first fb. */
	igt_plane_set_fb(primary, &data->fb[0]);
	igt_require_f(try_commit(&data->display) == 0,
		      "commit failed with " IGT_MODIFIER_FMT "\n",
		      IGT_MODIFIER_ARGS(modifier[0]));

	/* Flip to the second fb. */
	ret = drmModePageFlip(data->drm_fd, output->config.crtc->crtc_id,
			      data->fb[1].fb_id, DRM_MODE_PAGE_FLIP_EVENT, NULL);
	/*
	 * Page flip should work but some transitions may be temporarily
	 * on some kernels.
	 */
	igt_require(ret == 0);

	data->flipevent_in_queue = true;
	kmstest_wait_for_pageflip_timeout(data->drm_fd, pageflip_timeout_us(mode));
	data->flipevent_in_queue = false;

	/* Get a crc and compare with the reference. */
	igt_pipe_crc_get_current(data->drm_fd, data->pipe_crc, &crc);
	igt_assert_crc_equal(&reference_crc, &crc);

	igt_remove_fb(data->drm_fd, &data->old_fb[0]);
	igt_remove_fb(data->drm_fd, &data->old_fb[1]);
}

static void test_cleanup(data_t *data, igt_output_t *output)
{
	igt_plane_t *primary;
	primary = igt_output_get_plane(output, 0);

	/* Clean up. */
	igt_plane_set_fb(primary, NULL);
	pipe_crc_free(data);
	igt_output_set_crtc(output, NULL);

	igt_remove_fb(data->drm_fd, &data->fb[0]);
	igt_remove_fb(data->drm_fd, &data->fb[1]);
}

static void handle_lost_event(data_t *data) {
	// wait for max 5 seconds in case hit swapping or similar in progress.
	drmEventContext evctx = { .version = 2 };
	struct timeval timeout = { .tv_sec = 5};
	fd_set fds;
	int ret;

	FD_ZERO(&fds);
	FD_SET(data->drm_fd, &fds);
	do {
		errno = 0;
		ret = select(data->drm_fd + 1, &fds, NULL, NULL, &timeout);
	} while (ret < 0 && errno == EINTR);

	// TODO: if still failed may need to reset/restart everything to
	// avoid consecutive tests failing.

	igt_assert(drmHandleEvent(data->drm_fd, &evctx) == 0);

	data->flipevent_in_queue = false;
	igt_remove_fb(data->drm_fd, &data->old_fb[0]);
	igt_remove_fb(data->drm_fd, &data->old_fb[1]);
}

static data_t data = {};
igt_output_t *output;

int igt_main()
{
	igt_fixture() {
		data.drm_fd = drm_open_driver_master(DRIVER_INTEL | DRIVER_XE);
		data.gen = intel_display_ver(intel_get_drm_devid(data.drm_fd));

		data.testformat = DRM_FORMAT_XRGB8888;

		kmstest_set_vt_graphics_mode();

		igt_require_pipe_crc(data.drm_fd);
		igt_display_require(&data.display, data.drm_fd);
	}

	igt_describe("Check pageflip between modifiers");
	igt_subtest_with_dynamic("flip-change-tiling") {
		igt_crtc_t *crtc;
		bool run_in_simulation = igt_run_in_simulation();

		for_each_crtc_with_valid_output(&data.display, crtc, output) {
			igt_plane_t *plane;

			igt_display_reset(&data.display);
			pipe_crc_free(&data);

			igt_output_set_crtc(output,
					    crtc);
			if (!intel_pipe_output_combo_valid(&data.display))
				continue;

			plane = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);

			for (int i = 0; i < plane->format_mod_count; i++) {
				if (plane->formats[i] != data.testformat)
					continue;

				for (int j = 0; j < plane->format_mod_count; j++) {
					uint64_t modifier[2] = {
						plane->modifiers[i],
						plane->modifiers[j],
					};

					if (plane->formats[j] != data.testformat)
						continue;

					if (run_in_simulation &&
					    (!is_basic_tiling_modifier(plane->modifiers[i]) ||
					     !is_basic_tiling_modifier(plane->modifiers[j])))
						continue;

					igt_dynamic_f("pipe-%s-%s-%s-to-%s",
						      igt_crtc_name(crtc),
						      igt_output_name(output),
						      igt_fb_modifier_name(modifier[0]),
						      igt_fb_modifier_name(modifier[1]))
						test_flip_tiling(&data,
								 crtc,
								 output,
								 modifier);

					if (data.flipevent_in_queue)
						handle_lost_event(&data);
				}
			}
			test_cleanup(&data, output);
		}
	}

	igt_fixture() {
		igt_display_fini(&data.display);
		drm_close_driver(data.drm_fd);
	}
}
