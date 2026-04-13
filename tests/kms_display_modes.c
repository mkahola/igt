/*
 * Copyright © 2022 Intel Corporation
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
 *  Jeevan B <jeevan.b@intel.com>
 */

/**
 * TEST: kms display modes
 * Category: Display
 * Description: Test Display Modes
 * Driver requirement: i915, xe
 * Mega feature: General Display Features
 */

#include "igt.h"

/**
 * SUBTEST: extended-mode-basic
 * Description: Test for validating display extended mode with a pair of connected
 *              displays
 */

IGT_TEST_DESCRIPTION("Test Display Modes");

typedef struct {
	int drm_fd;
	igt_display_t display;
	igt_output_t *mst_output[2];
	int n_pipes;
} data_t;

static bool output_is_dp_mst(data_t *data, igt_output_t *output, int i)
{
	int connector_id;
	static int prev_connector_id;

	connector_id = igt_get_dp_mst_connector_id(output);
	if (connector_id < 0)
		return false;

	/*
	 * Discarding outputs of other DP MST topology.
	 * Testing only on outputs on the topology we got previously
	*/
	if (i == 0) {
		prev_connector_id = connector_id;
	} else {
		if (connector_id != prev_connector_id)
			return false;
	}

	return true;
}

static void run_extendedmode_basic(data_t *data, igt_crtc_t *crtc1,
				   igt_output_t *output1, igt_crtc_t *crtc2,
				   igt_output_t *output2)
{
	struct igt_fb fb, fbs[2];
	drmModeModeInfo *mode[2];
	igt_display_t *display = &data->display;
	igt_plane_t *plane[2];
	igt_pipe_crc_t *pipe_crc[2] = { 0 };
	igt_crc_t ref_crc[2], crc[2];
	int width, height;
	cairo_t *cr;

	igt_display_reset(display);

	igt_output_set_crtc(output1,
			    crtc1);
	igt_output_set_crtc(output2,
			    crtc2);

	mode[0] = igt_output_get_mode(output1);
	mode[1] = igt_output_get_mode(output2);

	igt_assert_f(igt_fit_modes_in_bw(display), "Unable to fit modes in bw\n");

	pipe_crc[0] = igt_crtc_crc_new(crtc1,
				       IGT_PIPE_CRC_SOURCE_AUTO);
	pipe_crc[1] = igt_crtc_crc_new(crtc2,
				       IGT_PIPE_CRC_SOURCE_AUTO);

	igt_create_color_fb(data->drm_fd, mode[0]->hdisplay, mode[0]->vdisplay,
			    DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR, 1, 0, 0, &fbs[0]);
	igt_create_color_fb(data->drm_fd, mode[1]->hdisplay, mode[1]->vdisplay,
			    DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR, 0, 0, 1, &fbs[1]);

	plane[0] = igt_crtc_get_plane_type(crtc1,
					   DRM_PLANE_TYPE_PRIMARY);
	plane[1] = igt_crtc_get_plane_type(crtc2,
					   DRM_PLANE_TYPE_PRIMARY);

	igt_plane_set_fb(plane[0], &fbs[0]);
	igt_fb_set_size(&fbs[0], plane[0], mode[0]->hdisplay, mode[0]->vdisplay);
	igt_plane_set_size(plane[0], mode[0]->hdisplay, mode[0]->vdisplay);

	igt_plane_set_fb(plane[1], &fbs[1]);
	igt_fb_set_size(&fbs[1], plane[1], mode[1]->hdisplay, mode[1]->vdisplay);
	igt_plane_set_size(plane[1], mode[1]->hdisplay, mode[1]->vdisplay);

	igt_display_commit2(display, COMMIT_ATOMIC);

	igt_pipe_crc_collect_crc(pipe_crc[0], &ref_crc[0]);
	igt_pipe_crc_collect_crc(pipe_crc[1], &ref_crc[1]);

	/*Create a big framebuffer and display it on 2 monitors*/
	width = mode[0]->hdisplay + mode[1]->hdisplay;
	height = max(mode[0]->vdisplay, mode[1]->vdisplay);

	igt_create_fb(data->drm_fd, width, height,
		      DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR, &fb);
	cr = igt_get_cairo_ctx(data->drm_fd, &fb);
	igt_paint_color(cr, 0, 0, mode[0]->hdisplay, mode[0]->vdisplay, 1, 0, 0);
	igt_paint_color(cr, mode[0]->hdisplay, 0, mode[1]->hdisplay, mode[1]->vdisplay, 0, 0, 1);
	igt_put_cairo_ctx(cr);

	igt_plane_set_fb(plane[0], &fb);
	igt_fb_set_position(&fb, plane[0], 0, 0);
	igt_fb_set_size(&fb, plane[0], mode[0]->hdisplay, mode[0]->vdisplay);
	igt_plane_set_size(plane[0], mode[0]->hdisplay, mode[0]->vdisplay);

	igt_plane_set_fb(plane[1], &fb);
	igt_fb_set_position(&fb, plane[1], mode[0]->hdisplay, 0);
	igt_fb_set_size(&fb, plane[1], mode[1]->hdisplay, mode[1]->vdisplay);
	igt_plane_set_size(plane[1], mode[1]->hdisplay, mode[1]->vdisplay);

	igt_assert_f(igt_fit_modes_in_bw(display), "Unable to fit modes in bw\n");
	igt_display_commit2(display, COMMIT_ATOMIC);

	igt_pipe_crc_collect_crc(pipe_crc[0], &crc[0]);
	igt_pipe_crc_collect_crc(pipe_crc[1], &crc[1]);

	/*Clean up*/
	igt_remove_fb(data->drm_fd, &fbs[0]);
	igt_remove_fb(data->drm_fd, &fbs[1]);
	igt_remove_fb(data->drm_fd, &fb);

	igt_pipe_crc_free(pipe_crc[0]);
	igt_pipe_crc_free(pipe_crc[1]);

	igt_output_set_crtc(output1, NULL);
	igt_output_set_crtc(output2, NULL);

	igt_plane_set_fb(igt_crtc_get_plane_type(crtc1,
						 DRM_PLANE_TYPE_PRIMARY),
			 NULL);
	igt_plane_set_fb(igt_crtc_get_plane_type(crtc2,
						 DRM_PLANE_TYPE_PRIMARY),
			 NULL);
	igt_assert_f(igt_fit_modes_in_bw(display), "Unable to fit modes in bw\n");
	igt_display_commit2(display, COMMIT_ATOMIC);

	/*Compare CRC*/
	igt_assert_crc_equal(&crc[0], &ref_crc[0]);
	igt_assert_crc_equal(&crc[1], &ref_crc[1]);
}

static void run_extendedmode_test(data_t *data) {
	bool sim_flag = igt_run_in_simulation();
	igt_crtc_t *crtc1, *crtc2;
	igt_output_t *output1, *output2;
	igt_display_t *display = &data->display;

	igt_display_reset(display);

	for_each_crtc_with_valid_output(display, crtc1, output1) {
		for_each_crtc_with_valid_output(display, crtc2, output2) {
			if (crtc1 == crtc2)
				continue;

			if (output1 == output2)
				continue;

			igt_display_reset(display);

			igt_output_set_crtc(output1, crtc1);
			igt_output_set_crtc(output2, crtc2);

			if (!intel_pipe_output_combo_valid(display))
				continue;

			igt_dynamic_f("pipe-%s-%s-pipe-%s-%s",
				      igt_crtc_name(crtc1), igt_output_name(output1),
				      igt_crtc_name(crtc2), igt_output_name(output2))
				run_extendedmode_basic(data,
						       crtc1, output1,
						       crtc2, output2);

			/*
			 * In simulation env, only run the test once with a
			 * single valid pipe/output pair instead of all combos.
			 */
			if (sim_flag)
				break;
		}
		if (sim_flag)
			break;
	}
}

int igt_main()
{
	int dp_mst_outputs = 0, count = 0;
	igt_output_t *output;
	data_t data;

	igt_fixture() {
		data.drm_fd = drm_open_driver_master(DRIVER_ANY);
		kmstest_set_vt_graphics_mode();
		igt_display_require(&data.display, data.drm_fd);
		igt_display_require_output(&data.display);

		for_each_connected_output(&data.display, output) {
			data.mst_output[count++] = output;
			if (output_is_dp_mst(&data, output, dp_mst_outputs))
				dp_mst_outputs++;
		}
	}

	igt_describe("Test for validating display extended mode with a pair of connected displays");
	igt_subtest_with_dynamic("extended-mode-basic") {
		igt_require_f(count > 1, "Minimum 2 outputs are required\n");
		run_extendedmode_test(&data);
	}

	igt_fixture() {
		igt_display_fini(&data.display);
	}
}
