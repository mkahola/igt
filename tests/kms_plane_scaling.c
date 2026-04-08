/*
 * Copyright © 2013,2014 Intel Corporation
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
 */

/**
 * TEST: kms plane scaling
 * Category: Display
 * Description: Test display plane scaling
 * Driver requirement: i915, xe
 * Mega feature: General Display Features
 */

#include "igt.h"
#include "igt_vec.h"
#include <math.h>

/**
 * SUBTEST: plane-scaler-unity-scaling-with-modifiers
 * Description: Tests scaling with modifiers, unity scaling.
 *
 * SUBTEST: plane-scaler-with-clipping-clamping-modifiers
 * Description: Tests scaling with clipping and clamping, modifiers.
 *
 * SUBTEST: plane-upscale-%s-with-modifiers
 * Description: Tests upscaling with modifiers %arg[1].
 *
 * arg[1]:
 *
 * @20x20:          from 20x20 fb
 * @factor-0-25:    for 0.25 scaling factor
 */

/**
 * SUBTEST: plane-downscale-factor-%s-with-modifiers
 * Description: Tests downscaling with modifiers for %arg[1] scaling factor.
 *
 * arg[1]:
 *
 * @0-25:   0.25
 * @0-5:    0.5
 * @0-75:   0.75
 */

/**
 * SUBTEST: plane-scaler-unity-scaling-with-rotation
 * Description: Tests scaling with rotation, unity scaling.
 *
 * SUBTEST: plane-scaler-with-clipping-clamping-rotation
 * Description: Tests scaling with clipping and clamping, rotation.
 *
 * SUBTEST: plane-upscale-%s-with-rotation
 * Description: Tests upscaling with rotation %arg[1].
 *
 * arg[1]:
 *
 * @20x20:          from 20x20 fb
 * @factor-0-25:    for 0.25 scaling factor
 */

/**
 * SUBTEST: plane-downscale-factor-%s-with-rotation
 * Description: Tests downscaling with rotation for %arg[1] scaling factor.
 *
 * arg[1]:
 *
 * @0-25:   0.25
 * @0-5:    0.5
 * @0-75:   0.75
 */

/**
 * SUBTEST: plane-scaler-unity-scaling-with-pixel-format
 * Description: Tests scaling with pixel formats, unity scaling.
 *
 * SUBTEST: plane-scaler-with-clipping-clamping-pixel-formats
 * Description: Tests scaling with clipping and clamping, pixel formats.
 *
 * SUBTEST: plane-upscale-%s-with-pixel-format
 * Description: Tests upscaling with pixel formats %arg[1].
 *
 * arg[1]:
 *
 * @20x20:          from 20x20 fb
 * @factor-0-25:    for 0.25 scaling factor
 */

/**
 * SUBTEST: plane-downscale-factor-%s-with-pixel-format
 * Description: Tests downscaling with pixel formats for %arg[1] scaling factor.
 *
 * arg[1]:
 *
 * @0-25:   0.25
 * @0-5:    0.5
 * @0-75:   0.75
 */

/**
 * SUBTEST: planes-downscale-factor-%s
 * Description: Tests downscaling of 2 planes for %arg[1] scaling factor.
 *
 * arg[1]:
 *
 * @0-25:  0.25
 * @0-5:   0.5
 * @0-75:  0.75
 */

/**
 * SUBTEST: planes-downscale-factor-%s-%s
 * Description: Tests downscaling (scaling factor %arg[1]) and upscaling (%arg[2])
 *              of 2 planes.
 *
 * arg[1]:
 *
 * @0-25:  0.25
 * @0-5:   0.5
 * @0-75:  0.75
 *
 * arg[2]:
 *
 * @upscale-20x20:           upscale 20x20
 * @upscale-factor-0-25:     scaling factor 0.25
 * @unity-scaling:           Unity
 */

/**
 * SUBTEST: planes-scaler-unity-scaling
 * Description: Tests scaling of 2 planes, unity scaling.
 *
 * SUBTEST: planes-upscale-%s
 * Description: Tests upscaling of 2 planes %arg[1].
 *
 * arg[1]:
 *
 * @20x20:          from 20x20 fb
 * @factor-0-25:    for 0.25 scaling factor
 */

/**
 * SUBTEST: planes-%s-downscale-factor-%s
 * Description: Tests scaling (%arg[1]) and downscaling (scaling factor %arg[2])
 *              of 2 planes.
 *
 * arg[1]:
 *
 * @unity-scaling:           Unity
 * @upscale-factor-0-25:     scaling factor 0.25
 * @upscale-20x20:           upscale 20x20
 *
 * arg[2]:
 *
 * @0-25:  0.25
 * @0-5:   0.5
 * @0-75:  0.75
 */

/**
 * SUBTEST: invalid-num-scalers
 * Description: Negative test for number of scalers per pipe.
 *
 * SUBTEST: 2x-scaler-multi-pipe
 * Description: Tests scaling with multi-pipe.
 *
 * SUBTEST: invalid-parameters
 * Description: Test parameters which should not be accepted
 *
 * SUBTEST: intel-max-src-size
 * Description: Test for validating max source size.
 */

IGT_TEST_DESCRIPTION("Test display plane scaling");

enum scaler_combo_test_type {
	TEST_PLANES_UPSCALE = 0,
	TEST_PLANES_DOWNSCALE,
	TEST_PLANES_UPSCALE_DOWNSCALE,
	TEST_PLANES_DOWNSCALE_UPSCALE,
};

typedef struct {
	uint32_t devid;
	int drm_fd;
	igt_display_t display;
	struct igt_fb fb[4];
	bool extended;
} data_t;

struct invalid_paramtests {
	const char *testname;
	uint32_t planesize[2];
	uint32_t vrefresh;
	struct {
		enum igt_atomic_plane_properties prop;
		uint32_t value;
	} params[8];
};

static const struct invalid_paramtests intel_paramtests[] = {
	{
		.testname = "intel-max-src-size",
		.planesize = {3840, 2160},
		.vrefresh = 30,
	},
};

const struct {
	const char * const describe;
	const char * const name;
	const double sf;
	const bool is_upscale;
} scaler_with_pixel_format_tests[] = {
	{
		"Tests upscaling with pixel formats, from 20x20 fb.",
		"plane-upscale-20x20-with-pixel-format",
		0.0,
		true,
	},
	{
		"Tests upscaling with pixel formats for 0.25 scaling factor.",
		"plane-upscale-factor-0-25-with-pixel-format",
		0.25,
		true,
	},
	{
		"Tests downscaling with pixel formats for 0.25 scaling factor.",
		"plane-downscale-factor-0-25-with-pixel-format",
		0.25,
		false,
	},
	{
		"Tests downscaling with pixel formats for 0.5 scaling factor.",
		"plane-downscale-factor-0-5-with-pixel-format",
		0.5,
		false,
	},
	{
		"Tests downscaling with pixel formats for 0.75 scaling factor.",
		"plane-downscale-factor-0-75-with-pixel-format",
		0.75,
		false,
	},
	{
		"Tests scaling with pixel formats, unity scaling.",
		"plane-scaler-unity-scaling-with-pixel-format",
		1.0,
		true,
	},
};

const struct {
	const char * const describe;
	const char * const name;
	const double sf;
	const bool is_upscale;
} scaler_with_rotation_tests[] = {
	{
		"Tests upscaling with rotation, from 20x20 fb.",
		"plane-upscale-20x20-with-rotation",
		0.0,
		true,
	},
	{
		"Tests upscaling with rotation for 0.25 scaling factor.",
		"plane-upscale-factor-0-25-with-rotation",
		0.25,
		true,
	},
	{
		"Tests downscaling with rotation for 0.25 scaling factor.",
		"plane-downscale-factor-0-25-with-rotation",
		0.25,
		false,
	},
	{
		"Tests downscaling with rotation for 0.5 scaling factor.",
		"plane-downscale-factor-0-5-with-rotation",
		0.5,
		false,
	},
	{
		"Tests downscaling with rotation for 0.75 scaling factor.",
		"plane-downscale-factor-0-75-with-rotation",
		0.75,
		false,
	},
	{
		"Tests scaling with rotation, unity scaling.",
		"plane-scaler-unity-scaling-with-rotation",
		1.0,
		true,
	},
};

const struct {
	const char * const describe;
	const char * const name;
	const double sf;
	const bool is_upscale;
} scaler_with_modifiers_tests[] = {
	{
		"Tests upscaling with modifiers, from 20x20 fb.",
		"plane-upscale-20x20-with-modifiers",
		0.0,
		true,
	},
	{
		"Tests upscaling with modifiers for 0.25 scaling factor.",
		"plane-upscale-factor-0-25-with-modifiers",
		0.25,
		true,
	},
	{
		"Tests downscaling with modifiers for 0.25 scaling factor.",
		"plane-downscale-factor-0-25-with-modifiers",
		0.25,
		false,
	},
	{
		"Tests downscaling with modifiers for 0.5 scaling factor.",
		"plane-downscale-factor-0-5-with-modifiers",
		0.5,
		false,
	},
	{
		"Tests downscaling with modifiers for 0.75 scaling factor.",
		"plane-downscale-factor-0-75-with-modifiers",
		0.75,
		false,
	},
	{
		"Tests scaling with modifiers, unity scaling.",
		"plane-scaler-unity-scaling-with-modifiers",
		1.0,
		true,
	},
};

const struct {
	const char * const describe;
	const char * const name;
	const double sf_plane1;
	const double sf_plane2;
	const enum scaler_combo_test_type test_type;
} scaler_with_2_planes_tests[] = {
	{
		"Tests upscaling of 2 planes, from 20x20 fb.",
		"planes-upscale-20x20",
		0.0,
		0.0,
		TEST_PLANES_UPSCALE,
	},
	{
		"Tests upscaling of 2 planes for 0.25 scaling factor.",
		"planes-upscale-factor-0-25",
		0.25,
		0.25,
		TEST_PLANES_UPSCALE,
	},
	{
		"Tests scaling of 2 planes, unity scaling.",
		"planes-scaler-unity-scaling",
		1.0,
		1.0,
		TEST_PLANES_UPSCALE,
	},
	{
		"Tests downscaling of 2 planes for 0.25 scaling factor.",
		"planes-downscale-factor-0-25",
		0.25,
		0.25,
		TEST_PLANES_DOWNSCALE,
	},
	{
		"Tests downscaling of 2 planes for 0.5 scaling factor.",
		"planes-downscale-factor-0-5",
		0.5,
		0.5,
		TEST_PLANES_DOWNSCALE,
	},
	{
		"Tests downscaling of 2 planes for 0.75 scaling factor.",
		"planes-downscale-factor-0-75",
		0.75,
		0.75,
		TEST_PLANES_DOWNSCALE,
	},
	{
		"Tests upscaling (20x20) and downscaling (scaling factor 0.25) of 2 planes.",
		"planes-upscale-20x20-downscale-factor-0-25",
		0.0,
		0.25,
		TEST_PLANES_UPSCALE_DOWNSCALE,
	},
	{
		"Tests upscaling (20x20) and downscaling (scaling factor 0.5) of 2 planes.",
		"planes-upscale-20x20-downscale-factor-0-5",
		0.0,
		0.5,
		TEST_PLANES_UPSCALE_DOWNSCALE,
	},
	{
		"Tests upscaling (20x20) and downscaling (scaling factor 0.75) of 2 planes.",
		"planes-upscale-20x20-downscale-factor-0-75",
		0.0,
		0.75,
		TEST_PLANES_UPSCALE_DOWNSCALE,
	},
	{
		"Tests upscaling (scaling factor 0.25) and downscaling (scaling factor 0.25) of 2 planes.",
		"planes-upscale-factor-0-25-downscale-factor-0-25",
		0.25,
		0.25,
		TEST_PLANES_UPSCALE_DOWNSCALE,
	},
	{
		"Tests upscaling (scaling factor 0.25) and downscaling (scaling factor 0.5) of 2 planes.",
		"planes-upscale-factor-0-25-downscale-factor-0-5",
		0.25,
		0.5,
		TEST_PLANES_UPSCALE_DOWNSCALE,
	},
	{
		"Tests upscaling (scaling factor 0.25) and downscaling (scaling factor 0.75) of 2 planes.",
		"planes-upscale-factor-0-25-downscale-factor-0-75",
		0.25,
		0.75,
		TEST_PLANES_UPSCALE_DOWNSCALE,
	},
	{
		"Tests scaling (unity) and downscaling (scaling factor 0.25) of 2 planes.",
		"planes-unity-scaling-downscale-factor-0-25",
		1.0,
		0.25,
		TEST_PLANES_UPSCALE_DOWNSCALE,
	},
	{
		"Tests scaling (unity) and downscaling (scaling factor 0.5) of 2 planes.",
		"planes-unity-scaling-downscale-factor-0-5",
		1.0,
		0.5,
		TEST_PLANES_UPSCALE_DOWNSCALE,
	},
	{
		"Tests scaling (unity) and downscaling (scaling factor 0.75) of 2 planes.",
		"planes-unity-scaling-downscale-factor-0-75",
		1.0,
		0.75,
		TEST_PLANES_UPSCALE_DOWNSCALE,
	},
	{
		"Tests downscaling (scaling factor 0.25) and upscaling (20x20) of 2 planes.",
		"planes-downscale-factor-0-25-upscale-20x20",
		0.25,
		0.0,
		TEST_PLANES_DOWNSCALE_UPSCALE,
	},
	{
		"Tests downscaling (scaling factor 0.25) and upscaling (scaling factor 0.25) of 2 planes.",
		"planes-downscale-factor-0-25-upscale-factor-0-25",
		0.25,
		0.25,
		TEST_PLANES_DOWNSCALE_UPSCALE,
	},
	{
		"Tests downscaling (scaling factor 0.25) and scaling (unity) of 2 planes.",
		"planes-downscale-factor-0-25-unity-scaling",
		0.25,
		1.0,
		TEST_PLANES_DOWNSCALE_UPSCALE,
	},
	{
		"Tests downscaling (scaling factor 0.5) and upscaling (20x20) of 2 planes.",
		"planes-downscale-factor-0-5-upscale-20x20",
		0.5,
		0.0,
		TEST_PLANES_DOWNSCALE_UPSCALE,
	},
	{
		"Tests downscaling (scaling factor 0.5) and upscaling (scaling factor 0.25) of 2 planes.",
		"planes-downscale-factor-0-5-upscale-factor-0-25",
		0.5,
		0.25,
		TEST_PLANES_DOWNSCALE_UPSCALE,
	},
	{
		"Tests downscaling (scaling factor 0.5) and scaling (unity) of 2 planes.",
		"planes-downscale-factor-0-5-unity-scaling",
		0.5,
		1.0,
		TEST_PLANES_DOWNSCALE_UPSCALE,
	},
	{
		"Tests downscaling (scaling factor 0.75) and upscaling (20x20) of 2 planes.",
		"planes-downscale-factor-0-75-upscale-20x20",
		0.75,
		0.0,
		TEST_PLANES_DOWNSCALE_UPSCALE,
	},
	{
		"Tests downscaling (scaling factor 0.75) and upscaling (scaling factor 0.25) of 2 planes.",
		"planes-downscale-factor-0-75-upscale-factor-0-25",
		0.75,
		0.25,
		TEST_PLANES_DOWNSCALE_UPSCALE,
	},
	{
		"Tests downscaling (scaling factor 0.75) and scaling (unity) of 2 planes.",
		"planes-downscale-factor-0-75-unity-scaling",
		0.75,
		1.0,
		TEST_PLANES_DOWNSCALE_UPSCALE,
	},
};

static int get_width(drmModeModeInfo *mode, double scaling_factor)
{
	if (scaling_factor == 0.0)
		return 20;
	else
		return mode->hdisplay * scaling_factor;
}

static int get_height(drmModeModeInfo *mode, double scaling_factor)
{
	if (scaling_factor == 0.0)
		return 20;
	else
		return mode->vdisplay * scaling_factor;
}

static void cleanup_fbs(data_t *data)
{
	for (int i = 0; i < ARRAY_SIZE(data->fb); i++)
		igt_remove_fb(data->drm_fd, &data->fb[i]);
}

static void cleanup_crtc(data_t *data)
{
	igt_display_reset(&data->display);

	cleanup_fbs(data);
}

static void skip_if_erange_or_einval(int ret)
{
	igt_skip_on_f(ret == -ERANGE || ret == -EINVAL,
		      "Unsupported scaling operation (ret = %s)\n",
		      ret == -EINVAL ? "-EINVAL" : "-ERANGE");
}

static uint32_t
check_scaling_pipe_plane_rot(data_t *d, igt_plane_t *plane,
			     uint32_t pixel_format,
			     uint64_t modifier,
			     double sf_plane,
			     bool is_clip_clamp,
			     bool is_upscale,
			     igt_output_t *output,
			     igt_rotation_t rot)
{
	igt_display_t *display = &d->display;
	drmModeModeInfo *mode;
	int commit_ret;
	int w, h;
	int width, height;

	for_each_connector_mode(output, mode) {
		igt_output_override_mode(output, mode);
		igt_debug("Trying mode %dx%d\n",
			  mode->hdisplay, mode->vdisplay);

		if (is_clip_clamp == true) {
			width = mode->hdisplay + 100;
			height = mode->vdisplay + 100;
		} else {
			width = get_width(mode, sf_plane);
			height = get_height(mode, sf_plane);
		}

		if (is_upscale) {
			w = width;
			h = height;
		} else {
			w = mode->hdisplay;
			h = mode->vdisplay;
		}

		/*
		 * guarantee even value width/height to avoid fractional
		 * uv component in chroma subsampling for yuv 4:2:0 formats
		 */
		w = ALIGN(w, 2);
		h = ALIGN(h, 2);

		igt_create_fb(display->drm_fd, w, h, pixel_format, modifier, &d->fb[0]);

		igt_plane_set_fb(plane, &d->fb[0]);
		igt_fb_set_position(&d->fb[0], plane, 0, 0);
		igt_fb_set_size(&d->fb[0], plane, w, h);
		igt_plane_set_position(plane, 0, 0);

		if (is_upscale)
			igt_plane_set_size(plane, mode->hdisplay, mode->vdisplay);
		else
			igt_plane_set_size(plane, width, height);

		if (rot != IGT_ROTATION_0)
			igt_plane_set_rotation(plane, rot);
		commit_ret = igt_display_try_commit2(display, COMMIT_ATOMIC);
		if (commit_ret == -ERANGE || commit_ret == -EINVAL)
			igt_debug("Unsupported scaling factor with fb size %dx%d with return value %s\n",
				  w, h, (commit_ret == -EINVAL) ? "-EINVAL" : "-ERANGE");
		igt_plane_set_fb(plane, NULL);
		igt_plane_set_position(plane, 0, 0);
		cleanup_fbs(d);
		if (commit_ret == 0)
			break;
	}
	return commit_ret;
}

static const igt_rotation_t rotations[] = {
	IGT_ROTATION_0,
	IGT_ROTATION_90,
	IGT_ROTATION_180,
	IGT_ROTATION_270,
};

static bool can_scale(data_t *d, unsigned format)
{
	if (!is_intel_device(d->drm_fd))
		return true;

	switch (format) {
	case DRM_FORMAT_XRGB16161616F:
	case DRM_FORMAT_XBGR16161616F:
	case DRM_FORMAT_ARGB16161616F:
	case DRM_FORMAT_ABGR16161616F:
		if (intel_display_ver(d->devid) >= 11)
			return true;
		/* fall through */
	case DRM_FORMAT_C8:
		return false;
	default:
		return true;
	}
}

static bool test_format(data_t *data,
			struct igt_vec *tested_formats,
			uint32_t format)
{
	if (!igt_fb_supported_format(format))
		return false;

	if (!is_intel_device(data->drm_fd) ||
	    data->extended)
		return true;

	format = igt_reduce_format(format);

	/* only test each format "class" once */
	if (igt_vec_index(tested_formats, &format) >= 0)
		return false;

	igt_vec_push(tested_formats, &format);

	return true;
}

static bool test_crtc_iteration(data_t *data, igt_crtc_t *crtc, int iteration)
{
	if (!is_intel_device(data->drm_fd) ||
	    data->extended)
		return true;

	if ((crtc->pipe > PIPE_B) && (iteration >= 2))
		return false;

	return true;
}

static const uint64_t modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	I915_FORMAT_MOD_X_TILED,
	I915_FORMAT_MOD_Y_TILED,
	I915_FORMAT_MOD_Yf_TILED,
	I915_FORMAT_MOD_4_TILED
};

static uint32_t
test_scaler_with_modifier_crtc(data_t *d,
			       double sf_plane,
			       bool is_clip_clamp,
			       bool is_upscale, igt_crtc_t *crtc,
			       igt_output_t *output)
{
	unsigned format = DRM_FORMAT_XRGB8888;
	igt_plane_t *plane;
	uint32_t ret;

	cleanup_crtc(d);

	igt_output_set_crtc(output, crtc);

	for_each_plane_on_crtc(crtc, plane) {
		if (plane->type == DRM_PLANE_TYPE_CURSOR)
			continue;

		for (int i = 0; i < ARRAY_SIZE(modifiers); i++) {
			uint64_t modifier = modifiers[i];

			if (igt_plane_has_format_mod(plane, format, modifier))
				ret = check_scaling_pipe_plane_rot(d, plane,
								   format, modifier,
								   sf_plane,
								   is_clip_clamp,
								   is_upscale,
								   output,
								   IGT_ROTATION_0);
			if (ret != 0)
				return ret;
		}
	}
	return ret;
}

static uint32_t
test_scaler_with_rotation_crtc(data_t *d,
			       double sf_plane,
			       bool is_clip_clamp,
			       bool is_upscale, igt_crtc_t *crtc,
			       igt_output_t *output)
{
	unsigned format = DRM_FORMAT_XRGB8888;
	uint64_t modifier = DRM_FORMAT_MOD_LINEAR;
	igt_plane_t *plane;
	uint32_t ret;

	cleanup_crtc(d);

	igt_output_set_crtc(output, crtc);

	for_each_plane_on_crtc(crtc, plane) {
		if (plane->type == DRM_PLANE_TYPE_CURSOR)
			continue;

		for (int i = 0; i < ARRAY_SIZE(rotations); i++) {
			igt_rotation_t rot = rotations[i];

			if (igt_plane_has_rotation(plane, rot))
				ret = check_scaling_pipe_plane_rot(d, plane,
								   format, modifier,
								   sf_plane,
								   is_clip_clamp,
								   is_upscale,
								   output,
								   rot);
			if (ret != 0)
				return ret;
		}
	}
	return ret;
}

static uint32_t
test_scaler_with_pixel_format_crtc(data_t *d, double sf_plane,
				   bool is_clip_clamp,
				   bool is_upscale, igt_crtc_t *crtc,
				   igt_output_t *output)
{
	uint64_t modifier = DRM_FORMAT_MOD_LINEAR;
	igt_plane_t *plane;
	uint32_t ret;

	cleanup_crtc(d);

	igt_output_set_crtc(output, crtc);

	for_each_plane_on_crtc(crtc, plane) {
		struct igt_vec tested_formats;

		if (plane->type == DRM_PLANE_TYPE_CURSOR)
			continue;

		igt_vec_init(&tested_formats, sizeof(uint32_t));

		for (int j = 0; j < plane->drm_plane->count_formats; j++) {
			uint32_t format = plane->drm_plane->formats[j];

			if (!test_crtc_iteration(d, crtc, j))
				continue;

			if (test_format(d, &tested_formats, format) &&
			    igt_plane_has_format_mod(plane, format, modifier) &&
			    can_scale(d, format))
				ret = check_scaling_pipe_plane_rot(d, plane,
								   format, modifier,
								   sf_plane,
								   is_clip_clamp,
								   is_upscale,
								   output,
								   IGT_ROTATION_0);
			if (ret != 0) {
				igt_vec_fini(&tested_formats);
				return ret;
			}
		}
		igt_vec_fini(&tested_formats);
	}
	return ret;
}

static igt_crtc_t *
find_connected_pipe(igt_display_t *display, bool second, igt_output_t **output)
{
	igt_crtc_t *crtc;
	bool first_output = false;
	bool found = false;

	igt_display_reset(display);

	for_each_crtc(display, crtc) {
		for_each_valid_output_on_crtc(display,
					      crtc,
					      *output) {
			if (igt_output_get_driving_crtc(*output) != NULL)
				continue;

			igt_output_set_crtc(*output, crtc);
			if (intel_pipe_output_combo_valid(display)) {
				found = true;

				if (second) {
					first_output = true;
					second = false;
					found = false;
				}
				break;
			}
			igt_output_set_crtc(*output, NULL);
		}
		if (found)
			break;
	}

	igt_display_reset(display);

	if (first_output)
		igt_require_f(found, "No second valid output found\n");
	else
		igt_require_f(found, "No valid outputs found\n");

	return crtc;
}

static int
__test_planes_scaling_combo(data_t *d, int w1, int h1, int w2, int h2, igt_output_t *output,
			    igt_plane_t *p1, igt_plane_t *p2,
			    struct igt_fb *fb1, struct igt_fb *fb2,
			    enum scaler_combo_test_type test_type)
{
	igt_display_t *display = &d->display;
	drmModeModeInfo *mode;
	int ret;

	mode = igt_output_get_mode(output);

	igt_plane_set_fb(p1, fb1);
	igt_plane_set_fb(p2, fb2);

	switch (test_type) {
	case TEST_PLANES_UPSCALE:
		igt_plane_set_size(p1, mode->hdisplay, mode->vdisplay);
		igt_plane_set_size(p2, mode->hdisplay - 20, mode->vdisplay - 20);
		break;
	case TEST_PLANES_DOWNSCALE:
		igt_plane_set_size(p1, w1, h1);
		igt_plane_set_size(p2, w2, h2);
		break;
	case TEST_PLANES_UPSCALE_DOWNSCALE:
		igt_plane_set_size(p1, mode->hdisplay, mode->vdisplay);
		igt_plane_set_size(p2, w2, h2);
		break;
	case TEST_PLANES_DOWNSCALE_UPSCALE:
		igt_plane_set_size(p1, w1, h1);
		igt_plane_set_size(p2, mode->hdisplay, mode->vdisplay);
		break;
	default:
		igt_assert(0);
	}

	ret = igt_display_try_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

	igt_plane_set_fb(p1, NULL);
	igt_plane_set_fb(p2, NULL);

	if (ret == -EINVAL || ret == -ERANGE)
		igt_debug("Scaling op not supported by driver with %s\n",
			 (ret == -EINVAL) ? "-EINVAL" : "-ERANGE");
	return ret;
}

static void setup_fb(int fd, int width, int height, struct igt_fb *fb)
{
	igt_create_fb(fd, width, height,
		      DRM_FORMAT_XRGB8888,
		      DRM_FORMAT_MOD_LINEAR,
		      fb);
}

static uint32_t
test_planes_scaling_combo(data_t *d, double sf_plane1,
			  double sf_plane2, igt_crtc_t *crtc,
			  igt_output_t *output,
			  enum scaler_combo_test_type test_type)
{
	igt_display_t *display = &d->display;
	drmModeModeInfo *mode;
	int n_planes;
	int w1, h1, w2, h2;
	int ret;

	cleanup_crtc(d);

	igt_output_set_crtc(output, crtc);
	for_each_connector_mode(output, mode) {
		igt_output_override_mode(output, mode);
		igt_debug("Trying mode %dx%d\n",
			  mode->hdisplay, mode->vdisplay);
		w1 = get_width(mode, sf_plane1);
		h1 = get_height(mode, sf_plane1);
		w2 = get_width(mode, sf_plane2);
		h2 = get_height(mode, sf_plane2);

		n_planes = crtc->n_planes;
		igt_require(n_planes >= 2);

		switch (test_type) {
		case TEST_PLANES_UPSCALE:
			setup_fb(display->drm_fd, w1, h1, &d->fb[1]);
			setup_fb(display->drm_fd, w2, h2, &d->fb[2]);
			break;
		case TEST_PLANES_DOWNSCALE:
			setup_fb(display->drm_fd, mode->hdisplay, mode->vdisplay, &d->fb[1]);
			setup_fb(display->drm_fd, mode->hdisplay, mode->vdisplay, &d->fb[2]);
			break;
		case TEST_PLANES_UPSCALE_DOWNSCALE:
			setup_fb(display->drm_fd, w1, h1, &d->fb[1]);
			setup_fb(display->drm_fd, mode->hdisplay, mode->vdisplay, &d->fb[2]);
			break;
		case TEST_PLANES_DOWNSCALE_UPSCALE:
			setup_fb(display->drm_fd, mode->hdisplay, mode->vdisplay, &d->fb[1]);
			setup_fb(display->drm_fd, w2, h2, &d->fb[2]);
			break;
		default:
			igt_assert(0);
		}

		for (int k = 0; k < n_planes - 1; k += 2) {
			igt_plane_t *p1, *p2;

			p1 = &crtc->planes[k];
			igt_require(p1);
			p2 = &crtc->planes[k+1];
			igt_require(p2);

			if (p1->type == DRM_PLANE_TYPE_CURSOR || p2->type == DRM_PLANE_TYPE_CURSOR)
				continue;
			ret = __test_planes_scaling_combo(d, w1, h1, w2, h2, output,
							  p1, p2,
							  &d->fb[1], &d->fb[2],
							  test_type);
			if (ret != 0)
				break;
		}
		cleanup_fbs(d);
		if (ret == 0)
			break;
	}
	return ret;
}

static void
test_invalid_num_scalers(data_t *d, igt_crtc_t *crtc, igt_output_t *output)
{
	igt_display_t *display = &d->display;
	int width, height;
	igt_plane_t *plane[3];
	drmModeModeInfo *mode;
	int ret;

	cleanup_crtc(d);

	igt_output_set_crtc(output, crtc);

	width = height = 20;
	mode = igt_output_get_mode(output);

	plane[0] = igt_crtc_get_plane_type_index(crtc, DRM_PLANE_TYPE_OVERLAY,
						 0);
	igt_require(plane[0]);
	plane[1] = igt_crtc_get_plane_type_index(crtc, DRM_PLANE_TYPE_OVERLAY,
						 1);
	igt_require(plane[1]);
	plane[2] = igt_crtc_get_plane_type_index(crtc, DRM_PLANE_TYPE_OVERLAY,
						 2);
	igt_require(plane[2]);

	igt_create_fb(display->drm_fd,
		      width, height,
		      DRM_FORMAT_XRGB8888,
		      DRM_FORMAT_MOD_LINEAR,
		      &d->fb[0]);
	igt_create_fb(display->drm_fd,
		      width, height,
		      DRM_FORMAT_XRGB8888,
		      DRM_FORMAT_MOD_LINEAR,
		      &d->fb[1]);
	igt_create_fb(display->drm_fd,
		      width, height,
		      DRM_FORMAT_XRGB8888,
		      DRM_FORMAT_MOD_LINEAR,
		      &d->fb[2]);

	igt_plane_set_fb(plane[0], &d->fb[0]);
	igt_plane_set_fb(plane[1], &d->fb[1]);
	igt_plane_set_fb(plane[2], &d->fb[2]);

	igt_plane_set_size(plane[0], mode->hdisplay, mode->vdisplay);
	igt_plane_set_size(plane[1], mode->hdisplay, mode->vdisplay);
	igt_plane_set_size(plane[2], mode->hdisplay, mode->vdisplay);

	/*
	 * This commit is expected to fail for intel devices. intel devices support
	 * max 2 scalers/pipe. In dmesg we can find: Too many scaling requests 3 > 2.
	 * For devices (non-intel, or possible future intel) that are able to perform
	 * this amount of scaling; handle that case aswell.
	 */
	ret = igt_display_try_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	igt_skip_on_f(ret == 0, "Cannot test handling of too many scaling ops, the device supports a large amount.\n");
	igt_assert(ret == -EINVAL || ret == -ERANGE);

	/* cleanup */
	igt_plane_set_fb(plane[0], NULL);
	igt_plane_set_fb(plane[1], NULL);
	igt_plane_set_fb(plane[2], NULL);
	cleanup_fbs(d);
}

static void test_scaler_with_multi_pipe_plane(data_t *d)
{
	igt_display_t *display = &d->display;
	igt_crtc_t *crtc1;
	igt_crtc_t *crtc2;
	igt_output_t *output1, *output2;
	drmModeModeInfo *mode1, *mode2;
	igt_plane_t *plane[4];
	int ret1, ret2;

	cleanup_fbs(d);

	crtc1 = find_connected_pipe(display, false, &output1);
	crtc2 = find_connected_pipe(display, true, &output2);
	igt_skip_on(!output1 || !output2);

	igt_info("Using (pipe %s + %s) and (pipe %s + %s) to run the subtest.\n",
		 igt_crtc_name(crtc1), igt_output_name(output1),
		 igt_crtc_name(crtc2), igt_output_name(output2));

	igt_output_set_crtc(output1,
			    crtc1);
	igt_output_set_crtc(output2,
			    crtc2);

	igt_require(igt_crtc_num_scalers(crtc1) >= 2);
	igt_require(igt_crtc_num_scalers(crtc2) >= 2);

	plane[0] = igt_output_get_plane(output1, 0);
	igt_require(plane[0]);
	plane[1] = igt_output_get_plane(output1, 0);
	igt_require(plane[1]);
	plane[2] = igt_output_get_plane(output2, 1);
	igt_require(plane[2]);
	plane[3] = igt_output_get_plane(output2, 1);
	igt_require(plane[3]);

	igt_create_fb(d->drm_fd, 600, 600, DRM_FORMAT_XRGB8888,
		      DRM_FORMAT_MOD_LINEAR, &d->fb[0]);
	igt_create_fb(d->drm_fd, 500, 500, DRM_FORMAT_XRGB8888,
		      DRM_FORMAT_MOD_LINEAR, &d->fb[1]);
	igt_create_fb(d->drm_fd, 700, 700, DRM_FORMAT_XRGB8888,
		      DRM_FORMAT_MOD_LINEAR, &d->fb[2]);
	igt_create_fb(d->drm_fd, 400, 400, DRM_FORMAT_XRGB8888,
		      DRM_FORMAT_MOD_LINEAR, &d->fb[3]);

	igt_plane_set_fb(plane[0], &d->fb[0]);
	igt_plane_set_fb(plane[1], &d->fb[1]);
	igt_plane_set_fb(plane[2], &d->fb[2]);
	igt_plane_set_fb(plane[3], &d->fb[3]);

	if (igt_display_try_commit_atomic(display,
				DRM_MODE_ATOMIC_TEST_ONLY |
				DRM_MODE_ATOMIC_ALLOW_MODESET,
				NULL) != 0) {
		bool found = igt_override_all_active_output_modes_to_fit_bw(display);
		igt_require_f(found, "No valid mode combo found.\n");
	}

	igt_display_commit2(display, COMMIT_ATOMIC);

	mode1 = igt_output_get_mode(output1);
	mode2 = igt_output_get_mode(output2);

	/* upscaling primary */
	igt_plane_set_size(plane[0], mode1->hdisplay, mode1->vdisplay);
	igt_plane_set_size(plane[2], mode2->hdisplay, mode2->vdisplay);
	ret1 = igt_display_try_commit2(display, COMMIT_ATOMIC);

	/* upscaling sprites */
	igt_plane_set_size(plane[1], mode1->hdisplay, mode1->vdisplay);
	igt_plane_set_size(plane[3], mode2->hdisplay, mode2->vdisplay);
	ret2 = igt_display_try_commit2(display, COMMIT_ATOMIC);

	igt_plane_set_fb(plane[0], NULL);
	igt_plane_set_fb(plane[1], NULL);
	igt_plane_set_fb(plane[2], NULL);
	igt_plane_set_fb(plane[3], NULL);
	cleanup_fbs(d);

	igt_skip_on_f(ret1 == -ERANGE || ret1 == -EINVAL ||
		      ret2 == -ERANGE || ret2 == -EINVAL,
		      "Scaling op is not supported by driver\n");
	igt_assert_eq(ret1 && ret2, 0);
}

static void invalid_parameter_tests(data_t *d)
{
	igt_display_t *display = &d->display;
	igt_crtc_t *crtc;
	igt_output_t *output;
	igt_fb_t fb;
	igt_plane_t *plane;
	int rval;

	static const struct invalid_paramtests paramtests[] = {
		{
			.testname = "less-than-1-height-src",
			.planesize = {256, 8},
			.params = {{IGT_PLANE_SRC_H, IGT_FIXED(0, 30) }, {~0}}
		},
		{
			.testname = "less-than-1-width-src",
			.planesize = {8, 256},
			.params = {{IGT_PLANE_SRC_W, IGT_FIXED(0, 30) }, {~0}}
		},
	};

	igt_fixture() {
		crtc = igt_first_crtc_with_single_output(display, &output);

		igt_output_set_crtc(output, crtc);
		plane = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);

		igt_require(igt_crtc_num_scalers(crtc) >= 1);

		igt_create_fb(d->drm_fd, 256, 256,
			      DRM_FORMAT_XRGB8888,
			      DRM_FORMAT_MOD_LINEAR,
			      &fb);
	}

	igt_describe("test parameters which should not be accepted");
	igt_subtest_with_dynamic("invalid-parameters") {
		for (uint32_t i = 0; i < ARRAY_SIZE(paramtests); i++) {
			igt_dynamic(paramtests[i].testname) {
				igt_plane_set_position(plane, 0, 0);
				igt_plane_set_fb(plane, &fb);
				igt_plane_set_size(plane,
						   paramtests[i].planesize[0],
						   paramtests[i].planesize[1]);

				for (uint32_t j = 0; paramtests[i].params[j].prop != ~0; j++)
					igt_plane_set_prop_value(plane,
								 paramtests[i].params[j].prop,
								 paramtests[i].params[j].value);

				rval = igt_display_try_commit2(&d->display, COMMIT_ATOMIC);

				igt_assert(rval == -EINVAL || rval == -ERANGE);
			}
		}
	}

	igt_fixture() {
		igt_remove_fb(d->drm_fd, &fb);
		igt_output_set_crtc(output, NULL);
	}
}

static drmModeModeInfo *find_mode(data_t *data, igt_output_t *output, const struct invalid_paramtests *test)
{
	drmModeModeInfo *found = NULL, *mode;

	for_each_connector_mode(output, mode) {
		if (mode->hdisplay == test->planesize[0] &&
		    mode->vdisplay == test->planesize[1] &&
		    mode->vrefresh >= test->vrefresh) {
			if (found && found->vrefresh < mode->vrefresh)
				continue;

			found = mode;
		}
	}

	return found;
}

/*
 *	Max source/destination width/height for intel driver.
 *	These numbers are coming from
 *	drivers/gpu/drm/i915/display/skl_scaler.c in kernel sources.
 *
 *	DISPLAY_VER < 11
 *		max_src_w = 4096
 *		max_src_h = 4096
 *		max_dst_w = 4096
 *		max_dst_h = 4096
 *
 *	DISPLAY_VER = 11
 *		max_src_w = 5120
 *		max_src_h = 4096
 *		max_dst_w = 5120
 *		max_dst_h = 4096
 *
 *	DISPLAY_VER = 12-13
 *		max_src_w = 5120
 *		max_src_h = 8192
 *		max_dst_w = 8192
 *		max_dst_h = 8192
 *
 *	DISPLAY_VER = 14
 *		max_src_w = 4096
 *		max_src_h = 8192
 *		max_dst_w = 8192
 *		max_dst_h = 8192
 */
static void intel_max_source_size_test(data_t *d, igt_crtc_t *crtc,
				       igt_output_t *output,
				       const struct invalid_paramtests *param)
{
	igt_fb_t fb;
	igt_plane_t *plane;
	int rval;
	drmModeModeInfo *mode = NULL;

	cleanup_crtc(d);

	igt_output_set_crtc(output, crtc);

	/*
	 * Need to get the mode again, because it may have changed
	 * after setting the pipe.
	 */
	mode = find_mode(d, output, param);
	igt_assert(mode);
	if (!mode)
		return;

	igt_output_override_mode(output, mode);
	plane = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);

	igt_create_fb(d->drm_fd, 5120, 4320,
		      DRM_FORMAT_XRGB8888,
		      DRM_FORMAT_MOD_LINEAR,
		      &fb);

	igt_plane_set_position(plane, 0, 0);
	igt_plane_set_fb(plane, &fb);
	igt_plane_set_size(plane, param->planesize[0], param->planesize[1]);

	rval = igt_display_try_commit2(&d->display, COMMIT_ATOMIC);

	if (intel_display_ver(d->devid) < 11 || intel_display_ver(d->devid) >= 14)
		igt_assert_eq(rval, -EINVAL);
	else
		igt_assert_eq(rval, 0);

	igt_plane_set_fb(plane, NULL);
	cleanup_fbs(d);
}

static bool
crtc_output_combo_valid(igt_display_t *display, igt_crtc_t *crtc,
			igt_output_t *output)
{
	bool ret = true;

	igt_display_reset(display);

	igt_output_set_crtc(output, crtc);
	if (!intel_pipe_output_combo_valid(display))
		ret = false;
	igt_output_set_crtc(output, NULL);

	return ret;
}

static int opt_handler(int opt, int opt_index, void *_data)
{
	data_t *data = _data;

	switch (opt) {
	case 'e':
		data->extended = true;
		break;
	}

	return IGT_OPT_HANDLER_SUCCESS;
}

static const struct option long_opts[] = {
	{ .name = "extended", .has_arg = false, .val = 'e', },
	{}
};

static const char help_str[] =
	"  --extended\t\tRun the extended tests\n";

static data_t data;

int igt_main_args("", long_opts, help_str, opt_handler, &data)
{
	igt_crtc_t *crtc;
	uint32_t ret = -EINVAL;

	igt_fixture() {
		data.drm_fd = drm_open_driver_master(DRIVER_ANY);
		igt_display_require(&data.display, data.drm_fd);
		data.devid = is_intel_device(data.drm_fd) ?
			intel_get_drm_devid(data.drm_fd) : 0;
		igt_require(data.display.is_atomic);
	}

	igt_subtest_group() {
		igt_output_t *output;

		for (int index = 0; index < ARRAY_SIZE(scaler_with_pixel_format_tests); index++) {
			igt_describe(scaler_with_pixel_format_tests[index].describe);
			igt_subtest_with_dynamic(scaler_with_pixel_format_tests[index].name) {
				for_each_crtc(&data.display, crtc) {
					igt_dynamic_f("pipe-%s", igt_crtc_name(crtc)) {
						for_each_valid_output_on_crtc(&data.display,
									      crtc,
									      output) {
							igt_info("Trying on %s\n", igt_output_name(output));
							if (!crtc_output_combo_valid(&data.display, crtc, output))
								continue;
							if (igt_crtc_num_scalers(crtc) < 1)
								continue;

							ret = test_scaler_with_pixel_format_crtc(&data,
									scaler_with_pixel_format_tests[index].sf,
									false,
									scaler_with_pixel_format_tests[index].is_upscale,
									crtc,
									output);
							if (ret == 0)
								break;
							igt_info("Required scaling operation not supported on %s trying on next output\n",
								 igt_output_name(output));
						}
						skip_if_erange_or_einval(ret);
					}
				}
			}
		}

		for (int index = 0; index < ARRAY_SIZE(scaler_with_rotation_tests); index++) {
			igt_describe(scaler_with_rotation_tests[index].describe);
			igt_subtest_with_dynamic(scaler_with_rotation_tests[index].name) {
				for_each_crtc(&data.display, crtc) {
					igt_dynamic_f("pipe-%s", igt_crtc_name(crtc)) {
						for_each_valid_output_on_crtc(&data.display,
									      crtc,
									      output) {
							igt_info("Trying on %s\n", igt_output_name(output));
							if (!crtc_output_combo_valid(&data.display, crtc, output))
								continue;
							if (igt_crtc_num_scalers(crtc) < 1)
								continue;

							ret = test_scaler_with_rotation_crtc(&data,
									scaler_with_rotation_tests[index].sf,
									false,
									scaler_with_rotation_tests[index].is_upscale,
									crtc,
									output);
							if (ret == 0)
								break;
							igt_info("Required scaling operation not supported on %s trying on next output\n",
								 igt_output_name(output));
						}
						skip_if_erange_or_einval(ret);
					}
				}
			}
		}

		for (int index = 0; index < ARRAY_SIZE(scaler_with_modifiers_tests); index++) {
			igt_describe(scaler_with_modifiers_tests[index].describe);
			igt_subtest_with_dynamic(scaler_with_modifiers_tests[index].name) {
				for_each_crtc(&data.display, crtc) {
					igt_dynamic_f("pipe-%s", igt_crtc_name(crtc)) {
						for_each_valid_output_on_crtc(&data.display,
									      crtc,
									      output) {
							igt_info("Trying on %s\n", igt_output_name(output));
							if (!crtc_output_combo_valid(&data.display, crtc, output))
								continue;
							if (igt_crtc_num_scalers(crtc) < 1)
								continue;

							ret = test_scaler_with_modifier_crtc(&data,
									scaler_with_modifiers_tests[index].sf,
									false,
									scaler_with_modifiers_tests[index].is_upscale,
									crtc,
									output);
							if (ret == 0)
								break;
							igt_info("Required scaling operation not supported on %s trying on next output\n",
								 igt_output_name(output));
						}
						skip_if_erange_or_einval(ret);
					}
				}
			}
		}

		igt_describe("Tests scaling with clipping and clamping, pixel formats.");
		igt_subtest_with_dynamic("plane-scaler-with-clipping-clamping-pixel-formats") {
			for_each_crtc(&data.display, crtc) {
				igt_dynamic_f("pipe-%s", igt_crtc_name(crtc)) {
					for_each_valid_output_on_crtc(&data.display,
								      crtc,
								      output) {
						igt_info("Trying on %s\n", igt_output_name(output));
						if (!crtc_output_combo_valid(&data.display, crtc, output))
							continue;
						if (igt_crtc_num_scalers(crtc) < 1)
							continue;

						ret = test_scaler_with_pixel_format_crtc(&data, 0.0, true,
											 false,
											 crtc,
											 output);
						if (ret == 0)
							break;
						igt_info("Required scaling operation not supported on %s trying on next output\n",
							 igt_output_name(output));
					}
					skip_if_erange_or_einval(ret);
				}
			}
		}


		igt_describe("Tests scaling with clipping and clamping, rotation.");
		igt_subtest_with_dynamic("plane-scaler-with-clipping-clamping-rotation") {
			for_each_crtc(&data.display, crtc) {
				igt_dynamic_f("pipe-%s", igt_crtc_name(crtc)) {
					for_each_valid_output_on_crtc(&data.display,
								      crtc,
								      output) {
						igt_info("Trying on %s\n", igt_output_name(output));
						if (!crtc_output_combo_valid(&data.display, crtc, output))
							continue;
						if (igt_crtc_num_scalers(crtc) < 1)
							continue;

						ret = test_scaler_with_rotation_crtc(&data, 0.0, true,
										     false,
										     crtc,
										     output);
						if (ret == 0)
							break;
						igt_info("Required scaling operation not supported on %s trying on next output\n",
							 igt_output_name(output));
					}
					skip_if_erange_or_einval(ret);
				}
			}
		}

		igt_describe("Tests scaling with clipping and clamping, modifiers.");
		igt_subtest_with_dynamic("plane-scaler-with-clipping-clamping-modifiers") {
			for_each_crtc(&data.display, crtc) {
				igt_dynamic_f("pipe-%s", igt_crtc_name(crtc)) {
					for_each_valid_output_on_crtc(&data.display,
								      crtc,
								      output) {
						igt_info("Trying on %s\n", igt_output_name(output));
						if (!crtc_output_combo_valid(&data.display, crtc, output))
							continue;
						if (igt_crtc_num_scalers(crtc) < 1)
							continue;

						ret = test_scaler_with_modifier_crtc(&data, 0.0, true,
										     false,
										     crtc,
										     output);
						if (ret == 0)
							break;
						igt_info("Required scaling operation not supported on %s trying on next output\n",
							 igt_output_name(output));
					}
					skip_if_erange_or_einval(ret);
				}
			}
		}

		for (int index = 0; index < ARRAY_SIZE(scaler_with_2_planes_tests); index++) {
			igt_describe(scaler_with_2_planes_tests[index].describe);
			igt_subtest_with_dynamic(scaler_with_2_planes_tests[index].name) {
				for_each_crtc(&data.display, crtc) {
					igt_dynamic_f("pipe-%s", igt_crtc_name(crtc)) {
						for_each_valid_output_on_crtc(&data.display,
									      crtc,
									      output) {
							igt_info("Trying on %s\n",
								 igt_output_name(output));
							if (!crtc_output_combo_valid(&data.display, crtc, output))
								continue;
							if (igt_crtc_num_scalers(crtc) < 2)
								continue;
							ret = test_planes_scaling_combo(&data,
								scaler_with_2_planes_tests[index].sf_plane1,
								scaler_with_2_planes_tests[index].sf_plane2,
								crtc,
								output,
								scaler_with_2_planes_tests[index].test_type);
							if (ret == 0)
								break;
							igt_info("Required scaling operation not supported on %s trying on next output\n",
								 igt_output_name(output));
						}
						skip_if_erange_or_einval(ret);
					}
				}
			}
		}

		for (int index = 0; index < ARRAY_SIZE(intel_paramtests); index++) {
			igt_describe("Test for validating max source size.");
			igt_subtest_with_dynamic(intel_paramtests[index].testname) {
				igt_require_intel(data.drm_fd);
				for_each_crtc(&data.display, crtc) {
					for_each_valid_output_on_crtc(&data.display,
								      crtc,
								      output) {
						if (igt_crtc_num_scalers(crtc) < 1)
							continue;
						/*
						 * Need to find mode with lowest vrefresh else
						 * we can exceed cdclk limits.
						 */
						if (find_mode(&data, output, &intel_paramtests[index]))
							igt_dynamic_f("pipe-%s-%s",
								      igt_crtc_name(crtc), igt_output_name(output))
								intel_max_source_size_test(&data,
											   crtc,
											   output,
											   &intel_paramtests[index]);
						else
							igt_info("Unable to find the lowest " \
								 "refresh rate mode on output " \
								 "%s pipe %s\n",
								 igt_output_name(output),
								 igt_crtc_name(crtc));
						continue;
					}
					break;
				}
			}
		}

		igt_describe("Negative test for number of scalers per pipe.");
		igt_subtest_with_dynamic("invalid-num-scalers") {
			for_each_crtc_with_valid_output(&data.display, crtc, output) {
				if (!crtc_output_combo_valid(&data.display, crtc, output))
					continue;
				if (igt_crtc_num_scalers(crtc) < 1)
						continue;

				igt_dynamic_f("pipe-%s-%s-invalid-num-scalers",
					      igt_crtc_name(crtc), igt_output_name(output))
					test_invalid_num_scalers(&data,
								 crtc,
								 output);
			}
		}
	}

	igt_subtest_group()
		invalid_parameter_tests(&data);

	igt_describe("Tests scaling with multi-pipe.");
	igt_subtest_f("2x-scaler-multi-pipe")
		test_scaler_with_multi_pipe_plane(&data);

	igt_fixture() {
		igt_display_fini(&data.display);
		drm_close_driver(data.drm_fd);
	}
}
