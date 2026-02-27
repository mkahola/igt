// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

/**
 * TEST: kms color pipeline
 * Category: Display
 * Description: Test to validate DRM colorops at plane level
 * Driver requirement: i915, xe
 * Mega feature: Color Management
 */

#include "kms_color_helper.h"
#include "kms_colorop_helper.h"

#define MAX_COLOROPS	5

/**
 * SUBTEST: plane-%s
 * Description: Test plane color pipeline with colorops: %arg[1].
 *
 * arg[1]:
 *
 * @lut1d:			1D LUT
 * @lut1d-pre-ctm3x4:		1D LUT PRE CTM 3x4
 * @lut1d-post-ctm3x4:		1D LUT POST CTM 3x4
 * @ctm3x4:			3X4 CTM
 * @lut1d-ctm3x4:		1D LUT --> 3X4 CTM
 * @ctm3x4-lut1d:		3X4 CTM --> 1D LUT
 * @lut1d-lut1d:		1D LUT --> 1D LUT
 * @lut1d-ctm3x4-lut1d:		1D LUT --> 3X4 CTM --> 1D LUT
 * @lut3d-green-only:		3D LUT
 */

IGT_TEST_DESCRIPTION("Test DRM colorops at plane level");

static void test_cleanup(data_t *data)
{
	if (data->pipe_crc) {
		igt_pipe_crc_free(data->pipe_crc);
		data->pipe_crc = NULL;
	}

	igt_output_set_crtc(data->output, NULL);
	igt_display_commit_atomic(&data->display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
}

static void test_setup(data_t *data, igt_crtc_t *crtc)
{
	igt_require_pipe_crc(data->drm_fd);
	igt_require(crtc);
	igt_require(crtc->n_planes > 0);

	igt_output_set_crtc(data->output, crtc);

	data->primary = igt_crtc_get_plane_type(crtc, DRM_PLANE_TYPE_PRIMARY);
	data->mode = igt_output_get_mode(data->output);
	igt_require(data->mode);

	data->pipe_crc = igt_crtc_crc_new(crtc,
					  IGT_PIPE_CRC_SOURCE_AUTO);

	/* Disable Pipe color props. */
	disable_ctm(crtc);
	disable_degamma(crtc);
	disable_gamma(crtc);

	igt_display_commit_atomic(&data->display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
}

static bool ctm_colorop_only(kms_colorop_t *colorops[])
{
	int i;

	if (!colorops[0])
		return false;

	for (i = 0; colorops[i]; i++) {
		if (colorops[i]->type != KMS_COLOROP_CTM_3X4)
			return false;
	}

	return true;
}

static void
capture_ref_crc(data_t *data, igt_output_t *output,
		const color_t *fb_color, igt_crc_t *crc /* out */)
{
	struct igt_fb fb;
	drmModeModeInfo *mode;
	igt_plane_t *primary;
	char *crc_str;
	int ret;

	/*
	 * Since CRC is collected at Pipe level,
	 * collecting crc for primary plane
	 * is enough for reference.
	 */
	primary = igt_output_get_plane(output, DRM_PLANE_TYPE_PRIMARY);

	mode = igt_output_get_mode(output);
	igt_assert_fd(igt_create_fb(data->drm_fd,
				    mode->hdisplay, mode->vdisplay,
				    DRM_FORMAT_XRGB8888,
				    DRM_FORMAT_MOD_LINEAR,
				    &fb));

	paint_rectangles(data, mode, fb_color, &fb);
	igt_plane_set_fb(primary, &fb);
	ret = igt_display_try_commit_atomic(&data->display, 0, NULL);
	igt_assert(!ret);

	igt_wait_for_vblank(primary->crtc);
	igt_pipe_crc_collect_crc(data->pipe_crc, crc);

	igt_plane_set_fb(primary, NULL);
	igt_display_commit_atomic(&data->display, 0, NULL);

	igt_remove_fb(data->drm_fd, &fb);

	crc_str = igt_crc_to_string(crc);
	igt_debug("CRC for reference fb: %s\n", crc_str);
	free(crc_str);
}

static void _test_plane_colorops(data_t *data,
				 igt_plane_t *plane,
				 const color_t *fb_colors,
				 igt_crc_t *crc_ref,
				 kms_colorop_t *colorops[])
{
	igt_display_t *display = &data->display;
	drmModeModeInfo *mode = data->mode;
	igt_colorop_t *color_pipeline;
	igt_crc_t crc_pipe;
	struct igt_fb fb;

	color_pipeline = get_color_pipeline(display, plane, colorops);
	igt_skip_on(!color_pipeline);

	/* Create a framebuffer at the size of the output. */
	igt_assert(igt_create_fb(data->drm_fd,
				 mode->hdisplay,
				 mode->vdisplay,
				 DRM_FORMAT_XRGB8888,
				 DRM_FORMAT_MOD_LINEAR,
				 &fb));

	/* Hardware pipeline CRC */
	set_color_pipeline(display, plane, colorops, color_pipeline);
	/*
	 * Use flat colors only when the pipeline
	 * contains CTM colorops exclusively.
	 */
	if (ctm_colorop_only(colorops))
		paint_rectangles(data, mode, fb_colors, &fb);
	else
		paint_gradient_rectangles(data, mode, fb_colors, &fb);

	igt_plane_set_fb(plane, &fb);
	igt_display_commit_atomic(&data->display, 0, NULL);
	igt_wait_for_vblank(plane->crtc);
	igt_pipe_crc_collect_crc(data->pipe_crc, &crc_pipe);

	igt_assert_crc_equal(crc_ref, &crc_pipe);

	/* Cleanup per-test state */
	set_color_pipeline_bypass(plane);
	reset_colorops(colorops);
	igt_plane_set_fb(plane, NULL);
	igt_display_commit_atomic(&data->display, 0, NULL);

	igt_remove_fb(data->drm_fd, &fb);
}

static void test_plane_colorops(data_t *data, igt_crtc_t *crtc,
				const color_t *fb_colors,
				const color_t *exp_colors,
				kms_colorop_t *colorops[])
{
	int n_planes = crtc->n_planes;
	igt_output_t *output = data->output;
	igt_plane_t *plane;
	igt_crc_t ref_crc;

	capture_ref_crc(data, output, exp_colors, &ref_crc);

	for (int plane_id = 0; plane_id < n_planes; plane_id++) {
		plane = igt_output_get_plane(output, plane_id);

		if (!igt_plane_has_prop(plane, IGT_PLANE_COLOR_PIPELINE))
			continue;

		igt_dynamic_f("pipe-%s-plane-%u", kmstest_pipe_name(crtc->pipe), plane_id)
			_test_plane_colorops(data, plane, fb_colors,
					     &ref_crc, colorops);
	}
}

static void
run_tests_for_plane(data_t *data)
{
	igt_crtc_t *crtc;
	igt_output_t *output = NULL;
	static const color_t colors_rgb[] = {
	        { 1.0, 0.0, 0.0 },
	        { 0.0, 1.0, 0.0 },
	        { 0.0, 0.0, 1.0 },
	};
	static const color_t colors_red_to_blue[] = {
		{ 0.0, 0.0, 1.0 },
		{ 0.0, 1.0, 0.0 },
		{ 0.0, 0.0, 1.0 },
	};
	static const color_t colors_red_and_green[] = {
		{ 1.0, 1.0, 0.0 },
		{ 1.0, 1.0, 0.0 },
		{ 1.0, 1.0, 0.0 }
	};
	static const color_t colors_only_green[] = {
		{ 0.0, 1.0, 0.0 },
		{ 0.0, 1.0, 0.0 },
		{ 0.0, 1.0, 0.0 }
	};
	const igt_matrix_3x4_t ctm_red_to_blue = { {
		0.0, 0.0, 0.0, 0.0,
		0.0, 1.0, 0.0, 0.0,
		1.0, 0.0, 1.0, 0.0,
	} };
	const igt_matrix_3x4_t ctm_linear = { {
		1.0, 0.0, 0.0, 0.0,
		0.0, 1.0, 0.0, 0.0,
		0.0, 0.0, 1.0, 0.0,
	} };
	kms_colorop_t lut1d_linear = {
		.type = KMS_COLOROP_CUSTOM_LUT1D,
		.name = "Pre/Post CSC GAMMA (linear LUT)",
		.lut1d = &igt_1dlut_linear,
		.transform = &igt_color_linear,
	};
	kms_colorop_t lut1d_max = {
		.type = KMS_COLOROP_CUSTOM_LUT1D,
		.lut1d = &igt_1dlut_max,
		.name = "Pre/Post CSC GAMMA (max LUT)",
		.transform = &igt_color_max,
	};
	kms_colorop_t lut3d = {
		.type = KMS_COLOROP_LUT3D,
		.lut3d = &igt_3dlut_17_green_only,
		.lut3d_info = {
			.size = 17,
			.interpolation = DRM_COLOROP_LUT3D_INTERPOLATION_TETRAHEDRAL,
		},
		.name = "3dlut passing only green channel (RGB order)",
		.transform = NULL,
	};
	kms_colorop_t ctm_3x4 = {
		.type = KMS_COLOROP_CTM_3X4,
		.name = "CTM 3X4 (red to blue)",
		.matrix_3x4 = &ctm_red_to_blue,
	};
	kms_colorop_t ctm_3x4_linear = {
		.type = KMS_COLOROP_CTM_3X4,
		.name = "CTM 3X4 (linear)",
		.matrix_3x4 = &ctm_linear,
	};

	struct {
		const char *name;
		const color_t *fb_colors;
		const color_t *exp_colors;
		kms_colorop_t *colorops[MAX_COLOROPS];
	} plane_colorops_tests[] = {
		{ .name = "lut1d",
		  .fb_colors = colors_rgb,
		  .exp_colors = colors_rgb,
		  .colorops = { &lut1d_max, NULL },
		},
		{ .name = "lut1d-pre-ctm3x4",
		  .fb_colors = colors_rgb,
		  .exp_colors = colors_rgb,
		  .colorops = { &lut1d_max, &ctm_3x4_linear, NULL },
		},
		{ .name = "lut1d-post-ctm3x4",
		  .fb_colors = colors_rgb,
		  .exp_colors = colors_rgb,
		  .colorops = { &ctm_3x4_linear, &lut1d_max, NULL },
		},
		{ .name = "ctm3x4",
		  .fb_colors = colors_rgb,
		  .exp_colors = colors_red_to_blue,
		  .colorops = { &ctm_3x4, NULL },
		},
		{ .name = "lut1d-ctm3x4",
		  .fb_colors = colors_rgb,
		  .exp_colors = colors_red_to_blue,
		  .colorops = { &lut1d_max, &ctm_3x4, NULL },
		},
		{ .name = "ctm3x4-lut1d",
		  .fb_colors = colors_rgb,
		  .exp_colors = colors_red_to_blue,
		  .colorops = { &ctm_3x4, &lut1d_max, NULL },
		},
		{ .name = "lut1d-lut1d",
		  .fb_colors = colors_rgb,
		  .exp_colors = colors_rgb,
		  .colorops = { &lut1d_linear, &lut1d_max, NULL },
		},
		{ .name = "lut1d-ctm3x4-lut1d",
		  .fb_colors = colors_rgb,
		  .exp_colors = colors_red_to_blue,
		  .colorops = { &lut1d_linear, &ctm_3x4, &lut1d_max, NULL },
		},
		{ .name = "lut3d-green-only",
		  .fb_colors = colors_red_and_green,
		  .exp_colors = colors_only_green,
		  .colorops = { &lut3d, NULL },
		},
	};

	for (int i = 0; i < ARRAY_SIZE(plane_colorops_tests); i++) {
		igt_describe_f("Test plane color pipeline with colorops: %s", plane_colorops_tests[i].name);
		igt_subtest_with_dynamic_f("plane-%s", plane_colorops_tests[i].name) {
			for_each_crtc_with_single_output(&data->display, crtc,
							 output) {
				data->output = output;

				if (!crtc_output_combo_valid(data, crtc))
					continue;

				test_setup(data,
					   crtc);

				data->color_depth = 8;
				data->drm_format = DRM_FORMAT_XRGB8888;

				test_plane_colorops(data, crtc,
						    plane_colorops_tests[i].fb_colors,
						    plane_colorops_tests[i].exp_colors,
						    plane_colorops_tests[i].colorops);
				test_cleanup(data);
			}
		}
	}
}

int igt_main()
{
	int has_plane_color_pipeline = 0;
	data_t data = {};

	igt_fixture() {
		data.drm_fd = drm_open_driver_master(DRIVER_ANY);

		/*
		 * Plane color pipeline support requires atomic modesetting.
		 * The atomic capability is enabled here as a prerequisite;
		 * igt_display_require() will reinitialize the display state.
		 */

		if (drmSetClientCap(data.drm_fd, DRM_CLIENT_CAP_ATOMIC, 1) == 0)
			data.display.is_atomic = 1;

		if (drmSetClientCap(data.drm_fd, DRM_CLIENT_CAP_PLANE_COLOR_PIPELINE, 1) == 0)
			has_plane_color_pipeline = 1;

		kmstest_set_vt_graphics_mode();

		igt_display_require(&data.display, data.drm_fd);
		data.display.has_plane_color_pipeline = has_plane_color_pipeline;
		igt_require(data.display.is_atomic);
	}

	igt_subtest_group()
		run_tests_for_plane(&data);

	igt_fixture() {
		igt_display_fini(&data.display);
		drm_close_driver(data.drm_fd);
	}
}
