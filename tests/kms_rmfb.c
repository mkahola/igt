/*
 * Copyright © 2016 Intel Corporation
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
 */

/**
 * TEST: kms rmfb
 * Category: Display
 * Description: This tests rmfb and close-fd behavior. In these cases the
 *              framebuffers should be removed from the crtc.
 * Driver requirement: i915, xe
 * Mega feature: General Display Features
 */

#include "igt.h"
#include "drmtest.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/**
 * SUBTEST: close-fd
 * Description: Kernel driver is supposed to free the framebuffers from any and all planes
 *		when the fd is closed. Ensure that is the case by closing and re-opening
 *		it.
 *
 * SUBTEST: rmfb-ioctl
 * Description: Kernel driver is supposed to free the framebuffers from any and all planes
 *              when DRM_IOCTL_MODE_RMFB ioctl is called. Ensure that is the case.
 */

#ifndef DRM_CAP_CURSOR_WIDTH
#define DRM_CAP_CURSOR_WIDTH 0x8
#endif
#ifndef DRM_CAP_CURSOR_HEIGHT
#define DRM_CAP_CURSOR_HEIGHT 0x9
#endif

IGT_TEST_DESCRIPTION("This tests rmfb and close-fd behavior. In these cases "
		     "the framebuffers should be removed from the crtc.");

struct rmfb_data {
	int drm_fd;
	igt_display_t display;
};

/*
 * 1. Set primary plane to a known fb.
 * 2. Make sure getcrtc returns the correct fb id.
 * 3. Call rmfb on the fb.
 * 4. Make sure getcrtc returns 0 fb id.
 *
 * RMFB is supposed to free the framebuffers from any and all planes,
 * so test this and make sure it works.
 */
static void
test_rmfb(struct rmfb_data *data, igt_output_t *output, enum pipe pipe, bool reopen)
{
	struct igt_fb fb, argb_fb;
	igt_display_t *display = &data->display;
	drmModeModeInfo *mode;
	igt_plane_t *plane;
	drmModeCrtc *drm_crtc;
	uint64_t cursor_width, cursor_height;
	int num_active_planes = 0;

	igt_display_reset(display);
	igt_output_set_crtc(output, igt_crtc_for_pipe(output->display, pipe));

	mode = igt_output_get_mode(output);

	igt_create_fb(data->drm_fd, mode->hdisplay, mode->vdisplay,
		      DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR, &fb);

	do_or_die(drmGetCap(data->drm_fd, DRM_CAP_CURSOR_WIDTH, &cursor_width));
	do_or_die(drmGetCap(data->drm_fd, DRM_CAP_CURSOR_HEIGHT, &cursor_height));

	igt_create_fb(data->drm_fd, cursor_width, cursor_height,
		      DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR, &argb_fb);

	/*
	 * Make sure these buffers are suited for display use
	 * because most of the modeset operations must be fast
	 * later on.
	 */
	for_each_plane_on_pipe(&data->display, pipe, plane) {
		if (plane->type == DRM_PLANE_TYPE_CURSOR) {
			igt_plane_set_fb(plane, &argb_fb);
			igt_fb_set_size(&argb_fb, plane, cursor_width, cursor_height);
			igt_plane_set_size(plane, cursor_width, cursor_height);
		} else {
			igt_plane_set_fb(plane, &fb);
		}

		if (igt_display_try_commit2(&data->display,
					    data->display.is_atomic ? COMMIT_ATOMIC :
					    plane->type == DRM_PLANE_TYPE_PRIMARY ?
					    COMMIT_LEGACY : COMMIT_UNIVERSAL)) {
			/*
			 * Disable any plane that fails (presumably
			 * due to exceeding some hardware limit).
			 */
			igt_plane_set_fb(plane, NULL);
		} else {
			num_active_planes++;
		}
	}

	/*
	 * Make sure we were able to enable at least one
	 * plane so that we actually test something.
	 */
	igt_assert_lt(0, num_active_planes);

	igt_display_commit2(&data->display, data->display.is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

	drm_crtc = drmModeGetCrtc(data->drm_fd, output->config.crtc->crtc_id);

	igt_assert_eq(drm_crtc->buffer_id, fb.fb_id);

	drmModeFreeCrtc(drm_crtc);

	if (reopen) {
		drm_close_driver(data->drm_fd);

		data->drm_fd = drm_open_driver_master(DRIVER_ANY);
		drmSetClientCap(data->drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
		drmSetClientCap(data->drm_fd, DRM_CLIENT_CAP_ATOMIC, 1);

		igt_crtc_refresh(igt_crtc_for_pipe(&data->display, pipe),
				 true);
	} else {
		igt_remove_fb(data->drm_fd, &fb);
		igt_remove_fb(data->drm_fd, &argb_fb);
	}

	drm_crtc = drmModeGetCrtc(data->drm_fd, output->config.crtc->crtc_id);

	igt_assert_eq(drm_crtc->buffer_id, 0);

	drmModeFreeCrtc(drm_crtc);

	for_each_plane_on_pipe(&data->display, pipe, plane) {
		drmModePlanePtr planeres = drmModeGetPlane(data->drm_fd, plane->drm_plane->plane_id);

		igt_assert_eq(planeres->fb_id, 0);

		drmModeFreePlane(planeres);
	}

	igt_output_set_crtc(output, NULL);
}

static void
run_rmfb_test(struct rmfb_data *data, bool reopen)
{
	igt_output_t *output;
	igt_crtc_t *crtc;
	igt_display_t *display = &data->display;

	for_each_crtc_with_single_output(display, crtc, output) {
		igt_display_reset(display);

		igt_output_set_crtc(output,
				    crtc);
		if (!intel_pipe_output_combo_valid(display))
			continue;

		igt_dynamic_f("pipe-%s-%s", igt_crtc_name(crtc),
			      igt_output_name(output))
			test_rmfb(data, output, crtc->pipe, reopen);
	}
}

int igt_main()
{
	const struct {
		bool reopen;
		const char *name;
		const char *description;
	} tests[] = {
		{ false, "rmfb-ioctl", "Kernel driver is supposed to free the framebuffers from any and all planes "
				       "when DRM_IOCTL_MODE_RMFB ioctl is called. Ensure that is the case." },
		{ true, "close-fd", "Kernel driver is supposed to free the framebuffers from any and all planes "
				    "when the fd is closed. Ensure that is the case by closing and re-opening "
				    "it" },
	};
	struct rmfb_data data = {};
	int i, other_fd;

	igt_fixture() {
		data.drm_fd = drm_open_driver_master(DRIVER_ANY);
		/*
		 * Prevent fb from changing underneath so we can check by
		 * fb_id == 0 after removing the fb
		 */
		other_fd = drm_reopen_driver(data.drm_fd);

		kmstest_set_vt_graphics_mode();

		igt_display_require(&data.display, data.drm_fd);
		igt_display_require_output(&data.display);
	}

	for (i = 0; i < ARRAY_SIZE(tests); i++) {
		igt_describe(tests[i].description);
		igt_subtest_with_dynamic(tests[i].name) {
			run_rmfb_test(&data, tests[i].reopen);
		}
	}

	igt_fixture() {
		igt_display_fini(&data.display);
		drm_close_driver(data.drm_fd);
		drm_close_driver(other_fd);
	}
}
