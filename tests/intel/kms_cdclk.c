/*
 * Copyright © 2020 Intel Corporation
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
 *  Swati Sharma <swati2.sharma@intel.com>
 */

/**
 * TEST: kms cdclk
 * Category: Display
 * Description: Test cdclk features : crawling and squashing
 * Driver requirement: i915, xe
 * Mega feature: General Display Features
 */

#include "igt.h"

/**
 * SUBTEST: mode-transition-all-outputs
 * Description: Mode transition (low to high) test to validate cdclk frequency
 *              change by simultaneous modesets on all pipes with valid outputs.
 *
 * SUBTEST: mode-transition
 * Description: Mode transition (low to high) test to validate cdclk frequency change.
 *
 * SUBTEST: plane-scaling
 * Description: Plane scaling test to validate cdclk frequency change.
 */

IGT_TEST_DESCRIPTION("Test cdclk features : crawling and squashing");

#define HDISPLAY_4K	3840
#define VDISPLAY_4K	2160
#define VREFRESH	60

/* Test flags */
enum {
	TEST_PLANESCALING = 1 << 0,
	TEST_MODETRANSITION = 1 << 1,
};

typedef struct {
	int drm_fd;
	uint32_t devid;
	igt_display_t display;
} data_t;

static bool hardware_supported(data_t *data)
{
        if (intel_display_ver(data->devid) >= 13)
		return true;

	return false;
}

static __u64 get_mode_data_rate(drmModeModeInfo *mode)
{
	__u64 data_rate = (__u64)mode->hdisplay * (__u64)mode->vdisplay * (__u64)mode->vrefresh;
	return data_rate;
}

static bool is_4k(drmModeModeInfo mode)
{
	return (mode.hdisplay >= HDISPLAY_4K && mode.vdisplay >= VDISPLAY_4K &&
	        mode.vrefresh >= VREFRESH);
}

static bool is_equal(drmModeModeInfo mode_hi, drmModeModeInfo mode_lo)
{
	return (mode_hi.hdisplay == mode_lo.hdisplay &&
		mode_hi.vdisplay == mode_lo.vdisplay &&
		mode_hi.vrefresh == mode_lo.vrefresh);
}

static drmModeModeInfo *get_lowres_mode(igt_output_t *output)
{
	drmModeModeInfo *lowest_mode = NULL;
	drmModeConnector *connector = output->config.connector;
	int j;

	for (j = 0; j < connector->count_modes; j++) {
		if (!lowest_mode) {
			lowest_mode = &connector->modes[j];
		} else if (connector->modes[j].vdisplay && connector->modes[j].hdisplay) {
			__u64 lowest_data_rate = get_mode_data_rate(lowest_mode);
			__u64 data_rate = get_mode_data_rate(&output->config.connector->modes[j]);

			if (lowest_data_rate > data_rate)
				lowest_mode = &connector->modes[j];
		}
	}

	return lowest_mode;
}

static void do_cleanup_display(igt_display_t *dpy)
{
	igt_crtc_t *crtc;
	igt_output_t *output;
	igt_plane_t *plane;

	for_each_crtc(dpy, crtc)
		for_each_plane_on_pipe(dpy, crtc->pipe, plane)
			igt_plane_set_fb(plane, NULL);

	for_each_connected_output(dpy, output)
		igt_output_set_crtc(output, NULL);

	igt_display_commit2(dpy, dpy->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);
}

static void test_plane_scaling(data_t *data, igt_crtc_t *crtc,
			       igt_output_t *output)
{
	igt_display_t *display = &data->display;
	int cdclk_ref, cdclk_new;
	struct igt_fb fb;
	igt_plane_t *primary;
	drmModeModeInfo mode;
	int scaling = 50;
	int ret;
	bool test_complete = false;

	while (!test_complete) {
		do_cleanup_display(display);
		igt_display_reset(display);

		igt_output_set_crtc(output,
				    crtc);
		mode = *igt_output_get_highres_mode(output);
		igt_require_f(is_4k(mode), "Mode >= 4K not found on output %s\n",
			      igt_output_name(output));

		igt_output_override_mode(output, &mode);

		primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);

		igt_create_color_pattern_fb(display->drm_fd,
					    mode.hdisplay, mode.vdisplay,
					    DRM_FORMAT_XRGB8888,
					    DRM_FORMAT_MOD_LINEAR,
					    0.0, 0.0, 0.0, &fb);
		igt_plane_set_fb(primary, &fb);

		/* downscaling */
		igt_plane_set_size(primary, ((fb.width * scaling) / 100), ((fb.height * scaling) / 100));
		cdclk_ref = igt_get_current_cdclk(data->drm_fd);
		ret = igt_display_try_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
		if (ret != -EINVAL) {
			igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
			cdclk_new = igt_get_current_cdclk(data->drm_fd);
			igt_info("CD clock frequency %d -> %d\n", cdclk_ref, cdclk_new);

			/* cdclk should bump */
			igt_assert_lt(cdclk_ref, cdclk_new);

			test_complete = true;
		}

		scaling += 5;

		/* cleanup */
		do_cleanup_display(display);
		igt_remove_fb(display->drm_fd, &fb);
	}
}

static void test_mode_transition(data_t *data, igt_crtc_t *crtc,
				 igt_output_t *output)
{
	igt_display_t *display = &data->display;
	int cdclk_ref, cdclk_new;
	struct igt_fb fb;
	igt_plane_t *primary;
	drmModeModeInfo mode_hi, mode_lo, *mode;

	do_cleanup_display(display);
	igt_display_reset(display);

	igt_output_set_crtc(output, crtc);
	mode = igt_output_get_mode(output);
	mode_lo = *get_lowres_mode(output);
	mode_hi = *igt_output_get_highres_mode(output);
	igt_require_f(is_4k(mode_hi), "Mode >= 4K not found on output %s\n",
	              igt_output_name(output));

	igt_skip_on_f(is_equal(mode_hi, mode_lo), "Highest and lowest mode resolutions are same; no transition\n");

	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);

	igt_create_color_pattern_fb(display->drm_fd,
				    mode->hdisplay, mode->vdisplay,
				    DRM_FORMAT_XRGB8888,
				    DRM_FORMAT_MOD_LINEAR,
				    0.0, 0.0, 0.0, &fb);

	/* switch to lower resolution */
	igt_output_override_mode(output, &mode_lo);
	igt_plane_set_fb(primary, &fb);
	igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	cdclk_ref = igt_get_current_cdclk(data->drm_fd);

	/* switch to higher resolution */
	igt_output_override_mode(output, &mode_hi);
	igt_plane_set_fb(primary, &fb);
	igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	cdclk_new = igt_get_current_cdclk(data->drm_fd);
	igt_info("CD clock frequency %d -> %d\n", cdclk_ref, cdclk_new);

	/* cdclk should bump */
	if (cdclk_new != igt_get_max_cdclk(data->drm_fd))
		igt_assert_lt(cdclk_ref, cdclk_new);

	/* cleanup */
	do_cleanup_display(display);
	igt_remove_fb(display->drm_fd, &fb);
}

static void set_mode(data_t *data, int count, drmModeModeInfo *mode,
		     igt_output_t **valid_outputs, struct igt_fb fb)
{
	igt_display_t *display = &data->display;

	for (int i = 0; i < count; i++) {
		igt_crtc_t *crtc = igt_crtc_for_pipe(display, i);
		igt_plane_t *plane = igt_crtc_get_plane_type(crtc, DRM_PLANE_TYPE_PRIMARY);

		igt_output_override_mode(valid_outputs[i], &mode[i]);

		igt_plane_set_fb(plane, &fb);
		igt_fb_set_size(&fb, plane, mode[i].hdisplay, mode[i].vdisplay);
		igt_plane_set_size(plane, mode[i].hdisplay, mode[i].vdisplay);
	}
}

static void test_mode_transition_on_all_outputs(data_t *data)
{
	igt_display_t *display = &data->display;
	drmModeModeInfo *mode, mode_highres[IGT_MAX_PIPES] = {0}, mode_lowres[IGT_MAX_PIPES] = {0};
	igt_output_t *valid_outputs[IGT_MAX_PIPES] = {NULL};
	igt_output_t *output;
	int count = 0;
	int cdclk_ref, cdclk_new;
	uint16_t width = 0, height = 0;
	struct igt_fb fb;

	do_cleanup_display(display);
	igt_display_reset(display);

	for_each_connected_output(display, output) {
		mode_highres[count] = *igt_output_get_highres_mode(output);
		igt_require_f(is_4k(mode_highres[count]), "Mode >= 4K not found on output %s.\n",
			      igt_output_name(output));

		mode_lowres[count] = *get_lowres_mode(output);

		if (is_equal(mode_highres[count], mode_lowres[count])) {
			igt_info("Highest and lowest mode resolutions are same on output %s; no transition will occur, skipping\n",
				  igt_output_name(output));
			continue;
		}

		valid_outputs[count] = output;
		count++;
	}

	igt_skip_on_f(count < 2,
		      "Number of valid outputs (%d) must be greater than or equal to 2\n", count);

	for (int i = 0; i < count; i++) {
		igt_crtc_t *crtc = igt_crtc_for_pipe(display, i);

		mode = igt_output_get_mode(valid_outputs[i]);
		igt_assert(mode);

		width = max(width, mode->hdisplay);
		height = max(height, mode->vdisplay);

		igt_output_set_crtc(valid_outputs[i], crtc);
		igt_output_override_mode(valid_outputs[i], &mode_highres[i]);
	}

	igt_require(intel_pipe_output_combo_valid(display));

	igt_create_pattern_fb(data->drm_fd, width, height, DRM_FORMAT_XRGB8888,
			      DRM_FORMAT_MOD_LINEAR, &fb);

	set_mode(data, count, mode_lowres, valid_outputs, fb);
	igt_display_commit2(display, COMMIT_ATOMIC);
	cdclk_ref = igt_get_current_cdclk(data->drm_fd);

	set_mode(data, count, mode_highres, valid_outputs, fb);
	igt_display_commit2(display, COMMIT_ATOMIC);
	cdclk_new = igt_get_current_cdclk(data->drm_fd);
	igt_info("CD clock frequency %d -> %d\n", cdclk_ref, cdclk_new);

	/* cdclk should bump */
	if (cdclk_new != igt_get_max_cdclk(data->drm_fd))
		igt_assert_lt(cdclk_ref, cdclk_new);

	do_cleanup_display(display);
	igt_remove_fb(data->drm_fd, &fb);
}

static void run_cdclk_test(data_t *data, uint32_t flags)
{
	igt_display_t *display = &data->display;
	igt_output_t *output;
	igt_crtc_t *crtc;

	for_each_crtc_with_valid_output(display, crtc, output) {
		igt_output_set_crtc(output,
				    crtc);
		if (!intel_pipe_output_combo_valid(display)) {
			igt_output_set_crtc(output, NULL);
			continue;
		}

		igt_dynamic_f("pipe-%s-%s", igt_crtc_name(crtc), output->name) {
			if (flags & TEST_PLANESCALING)
				test_plane_scaling(data,
						   crtc,
						   output);
			if (flags & TEST_MODETRANSITION)
				test_mode_transition(data,
						     crtc,
						     output);
		}
	}
}

int igt_main()
{
	data_t data = {};

	igt_fixture() {
		data.drm_fd = drm_open_driver_master(DRIVER_INTEL | DRIVER_XE);
		igt_require(data.drm_fd >= 0);
		kmstest_set_vt_graphics_mode();
		data.devid = intel_get_drm_devid(data.drm_fd);
		igt_require_f(hardware_supported(&data),
			      "Hardware doesn't support crawling/squashing.\n");
		igt_display_require(&data.display, data.drm_fd);
		igt_display_require_output(&data.display);

		/* Wa_15015413771: Only single possible cdclk value in XE2_HPD */
		igt_require(!IS_BATTLEMAGE(data.devid));
	}

	igt_describe("Plane scaling test to validate cdclk frequency change.");
	igt_subtest_with_dynamic("plane-scaling")
		run_cdclk_test(&data, TEST_PLANESCALING);
	igt_describe("Mode transition (low to high) test to validate cdclk frequency change.");
	igt_subtest_with_dynamic("mode-transition")
		run_cdclk_test(&data, TEST_MODETRANSITION);

	igt_describe("Mode transition (low to high) test to validate cdclk frequency change "
		     "by simultaneous modesets on all pipes with valid outputs.");
	igt_subtest("mode-transition-all-outputs")
		test_mode_transition_on_all_outputs(&data);

	igt_fixture() {
		igt_display_fini(&data.display);
		drm_close_driver(data.drm_fd);
	}
}
