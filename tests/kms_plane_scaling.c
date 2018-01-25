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

#include "igt.h"
#include <math.h>


IGT_TEST_DESCRIPTION("Test display plane scaling");

typedef struct {
	uint32_t devid;
	int drm_fd;
	igt_display_t display;
	igt_crc_t ref_crc;
	igt_pipe_crc_t *pipe_crc;

	int image_w;
	int image_h;

	struct igt_fb fb[4];

	igt_plane_t *plane1;
	igt_plane_t *plane2;
	igt_plane_t *plane3;
	igt_plane_t *plane4;
} data_t;

static int get_num_scalers(uint32_t devid, enum pipe pipe)
{
	igt_require(intel_gen(devid) >= 9);

	if (intel_gen(devid) >= 10)
		return 2;
	else if (pipe != PIPE_C)
		return 2;
	else
		return 1;
}

static void cleanup_crtc(data_t *data)
{
	int i;

	igt_display_reset(&data->display);
	igt_pipe_crc_free(data->pipe_crc);
	data->pipe_crc = NULL;

	for (i = 0; i < ARRAY_SIZE(data->fb); i++) {
		if (!data->fb[i].fb_id)
			continue;

		igt_remove_fb(data->drm_fd, &data->fb[i]);
		data->fb[i].fb_id = 0;
	}
}

static void prepare_crtc(data_t *data, igt_output_t *output, enum pipe pipe,
			igt_plane_t *plane, drmModeModeInfo *mode)
{
	igt_display_t *display = &data->display;

	cleanup_crtc(data);

	igt_output_set_pipe(output, pipe);

	/* create the pipe_crc object for this pipe */
	data->pipe_crc = igt_pipe_crc_new(data->drm_fd, pipe, INTEL_PIPE_CRC_SOURCE_AUTO);

	/* allocate fb for plane 1 */
	igt_create_pattern_fb(data->drm_fd, mode->hdisplay, mode->vdisplay,
			      DRM_FORMAT_XRGB8888,
			      LOCAL_I915_FORMAT_MOD_X_TILED, /* tiled */
			      &data->fb[0]);

	igt_plane_set_fb(plane, &data->fb[0]);

	if (plane->type != DRM_PLANE_TYPE_PRIMARY) {
		igt_plane_t *primary;
		int ret;

		/* Do we succeed without enabling the primary plane? */
		ret = igt_display_try_commit2(display, COMMIT_ATOMIC);
		if (!ret)
			return;

		/*
		 * Fallback: set the primary plane to actually enable the pipe.
		 * Some drivers always require the primary plane to be enabled.
		 */
		primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
		igt_plane_set_fb(primary, &data->fb[0]);
	}
	igt_display_commit2(display, COMMIT_ATOMIC);
}

static void paint_fb(data_t *d, struct igt_fb *fb)
{
	cairo_t *cr;

	cr = igt_get_cairo_ctx(d->drm_fd, fb);
	igt_paint_color(cr, 0, 0, fb->width, fb->height, 0.0, 1.0, 0.0);
	igt_put_cairo_ctx(d->drm_fd, fb, cr);
}

static void check_scaling_pipe_plane_rot(data_t *d, igt_plane_t *plane,
					 uint32_t pixel_format,
					 uint64_t tiling, enum pipe pipe,
					 igt_output_t *output,
					 igt_rotation_t rot)
{
	igt_display_t *display = &d->display;
	int width, height;
	drmModeModeInfo *mode;

	cleanup_crtc(d);

	igt_output_set_pipe(output, pipe);
	mode = igt_output_get_mode(output);

	/* create buffer in the range of  min and max source side limit.*/
	width = height = 9;
	igt_create_fb(display->drm_fd, width, height,
		      pixel_format, tiling, &d->fb[0]);
	paint_fb(d, &d->fb[0]);
	igt_plane_set_fb(plane, &d->fb[0]);

	/* Check min to full resolution upscaling */
	igt_fb_set_position(&d->fb[0], plane, 0, 0);
	igt_fb_set_size(&d->fb[0], plane, width, height);
	igt_plane_set_position(plane, 0, 0);
	igt_plane_set_size(plane, mode->hdisplay, mode->vdisplay);
	igt_plane_set_rotation(plane, rot);
	igt_display_commit2(display, COMMIT_ATOMIC);

	igt_plane_set_fb(plane, NULL);
	igt_plane_set_position(plane, 0, 0);
}

static const igt_rotation_t rotations[] = {
	IGT_ROTATION_0,
	IGT_ROTATION_90,
	IGT_ROTATION_180,
	IGT_ROTATION_270,
};

static void test_scaler_with_rotation_pipe(data_t *d, enum pipe pipe,
					   igt_output_t *output)
{
	igt_display_t *display = &d->display;
	igt_plane_t *plane;

	igt_output_set_pipe(output, pipe);
	for_each_plane_on_pipe(display, pipe, plane) {
		if (plane->type == DRM_PLANE_TYPE_CURSOR)
			continue;

		for (int i = 0; i < ARRAY_SIZE(rotations); i++) {
			igt_rotation_t rot = rotations[i];

			check_scaling_pipe_plane_rot(d, plane, DRM_FORMAT_XRGB8888,
						     LOCAL_I915_FORMAT_MOD_Y_TILED,
						     pipe, output, rot);
		}
	}
}

static bool can_draw(uint32_t drm_format)
{
	const uint32_t *drm_formats;
	int format_count, i;

	igt_get_all_cairo_formats(&drm_formats, &format_count);

	for (i = 0; i < format_count; i++)
		if (drm_formats[i] == drm_format)
			return true;

	return false;
}

static const uint64_t tilings[] = {
	LOCAL_DRM_FORMAT_MOD_NONE,
	LOCAL_I915_FORMAT_MOD_X_TILED,
	LOCAL_I915_FORMAT_MOD_Y_TILED,
	LOCAL_I915_FORMAT_MOD_Yf_TILED
};

static void test_scaler_with_pixel_format_pipe(data_t *d, enum pipe pipe, igt_output_t *output)
{
	igt_display_t *display = &d->display;
	igt_plane_t *plane;

	igt_output_set_pipe(output, pipe);

	for_each_plane_on_pipe(display, pipe, plane) {
		if (plane->type == DRM_PLANE_TYPE_CURSOR)
			continue;

		for (int i = 0; i < ARRAY_SIZE(tilings); i++) {
			uint64_t tiling = tilings[i];

			for (int j = 0; j < plane->drm_plane->count_formats; j++) {
				uint32_t format = plane->drm_plane->formats[j];

				if (can_draw(format))
					check_scaling_pipe_plane_rot(d, plane,
								     format, tiling,
								     pipe, output, IGT_ROTATION_0);
			}
		}
	}
}

/* does iterative scaling on plane2 */
static void iterate_plane_scaling(data_t *d, drmModeModeInfo *mode)
{
	igt_display_t *display = &d->display;

	if (mode->hdisplay >= d->fb[1].width) {
		int w, h;
		/* fixed fb */
		igt_fb_set_position(&d->fb[1], d->plane2, 0, 0);
		igt_fb_set_size(&d->fb[1], d->plane2, d->fb[1].width, d->fb[1].height);
		igt_plane_set_position(d->plane2, 0, 0);

		/* adjust plane size */
		for (w = d->fb[1].width; w <= mode->hdisplay; w+=10) {
			h = w * d->fb[1].height / d->fb[1].width;
			igt_plane_set_size(d->plane2, w, h);
			igt_display_commit2(display, COMMIT_ATOMIC);
		}
	} else {
		int w, h;
		/* fixed plane */
		igt_plane_set_position(d->plane2, 0, 0);
		igt_plane_set_size(d->plane2, mode->hdisplay, mode->vdisplay);
		igt_fb_set_position(&d->fb[1], d->plane2, 0, 0);

		/* adjust fb size */
		for (w = mode->hdisplay; w <= d->fb[1].width; w+=10) {
			/* Source coordinates must not be clipped. */
			h = min(w * mode->hdisplay / mode->vdisplay, d->fb[1].height);
			igt_fb_set_size(&d->fb[1], d->plane2, w, h);
			igt_display_commit2(display, COMMIT_ATOMIC);
		}
	}
}

static void
test_plane_scaling_on_pipe(data_t *d, enum pipe pipe, igt_output_t *output)
{
	igt_display_t *display = &d->display;
	drmModeModeInfo *mode;
	int primary_plane_scaling = 0; /* For now */

	mode = igt_output_get_mode(output);

	/* Set up display with plane 1 */
	d->plane1 = &display->pipes[pipe].planes[0];
	prepare_crtc(d, output, pipe, d->plane1, mode);

	igt_create_color_pattern_fb(display->drm_fd, 600, 600,
				    DRM_FORMAT_XRGB8888,
				    LOCAL_I915_FORMAT_MOD_X_TILED, /* tiled */
				    .5, .5, .5, &d->fb[1]);

	igt_create_pattern_fb(d->drm_fd,
			      mode->hdisplay, mode->vdisplay,
			      DRM_FORMAT_XRGB8888,
			      LOCAL_I915_FORMAT_MOD_X_TILED, /* tiled */
			      &d->fb[2]);

	if (primary_plane_scaling) {
		/* Primary plane upscaling */
		igt_fb_set_position(&d->fb[0], d->plane1, 100, 100);
		igt_fb_set_size(&d->fb[0], d->plane1, 500, 500);
		igt_plane_set_position(d->plane1, 0, 0);
		igt_plane_set_size(d->plane1, mode->hdisplay, mode->vdisplay);
		igt_display_commit2(display, COMMIT_ATOMIC);

		/* Primary plane 1:1 no scaling */
		igt_fb_set_position(&d->fb[0], d->plane1, 0, 0);
		igt_fb_set_size(&d->fb[0], d->plane1, d->fb[0].width, d->fb[0].height);
		igt_plane_set_position(d->plane1, 0, 0);
		igt_plane_set_size(d->plane1, mode->hdisplay, mode->vdisplay);
		igt_display_commit2(display, COMMIT_ATOMIC);
	}

	/* Set up fb[1]->plane2 mapping. */
	d->plane2 = igt_output_get_plane(output, 1);
	igt_plane_set_fb(d->plane2, &d->fb[1]);

	/* 2nd plane windowed */
	igt_fb_set_position(&d->fb[1], d->plane2, 100, 100);
	igt_fb_set_size(&d->fb[1], d->plane2, d->fb[1].width-200, d->fb[1].height-200);
	igt_plane_set_position(d->plane2, 100, 100);
	igt_plane_set_size(d->plane2, mode->hdisplay-200, mode->vdisplay-200);
	igt_display_commit2(display, COMMIT_ATOMIC);

	iterate_plane_scaling(d, mode);

	/* 2nd plane up scaling */
	igt_fb_set_position(&d->fb[1], d->plane2, 100, 100);
	igt_fb_set_size(&d->fb[1], d->plane2, 500, 500);
	igt_plane_set_position(d->plane2, 10, 10);
	igt_plane_set_size(d->plane2, mode->hdisplay-20, mode->vdisplay-20);
	igt_display_commit2(display, COMMIT_ATOMIC);

	/* 2nd plane downscaling */
	igt_fb_set_position(&d->fb[1], d->plane2, 0, 0);
	igt_fb_set_size(&d->fb[1], d->plane2, d->fb[1].width, d->fb[1].height);
	igt_plane_set_position(d->plane2, 10, 10);

	/* Downscale (10/9)x of original image */
	igt_plane_set_size(d->plane2, (d->fb[1].width * 10)/9, (d->fb[1].height * 10)/9);
	igt_display_commit2(display, COMMIT_ATOMIC);

	if (primary_plane_scaling) {
		/* Primary plane up scaling */
		igt_fb_set_position(&d->fb[0], d->plane1, 100, 100);
		igt_fb_set_size(&d->fb[0], d->plane1, 500, 500);
		igt_plane_set_position(d->plane1, 0, 0);
		igt_plane_set_size(d->plane1, mode->hdisplay, mode->vdisplay);
		igt_display_commit2(display, COMMIT_ATOMIC);
	}

	/* Set up fb[2]->plane3 mapping. */
	d->plane3 = igt_output_get_plane(output, 2);
	igt_plane_set_fb(d->plane3, &d->fb[2]);

	if(d->plane3->type == DRM_PLANE_TYPE_CURSOR) {
		igt_debug("Plane-3 doesnt exist on pipe %s\n", kmstest_pipe_name(pipe));
		return;
	}

	/* 3rd plane windowed - no scaling */
	igt_fb_set_position(&d->fb[2], d->plane3, 100, 100);
	igt_fb_set_size(&d->fb[2], d->plane3, d->fb[2].width-300, d->fb[2].height-300);
	igt_plane_set_position(d->plane3, 100, 100);
	igt_plane_set_size(d->plane3, mode->hdisplay-300, mode->vdisplay-300);
	igt_display_commit2(display, COMMIT_ATOMIC);

	/* Switch scaler from plane 2 to plane 3 */
	igt_fb_set_position(&d->fb[1], d->plane2, 100, 100);
	igt_fb_set_size(&d->fb[1], d->plane2, d->fb[1].width-200, d->fb[1].height-200);
	igt_plane_set_position(d->plane2, 100, 100);
	igt_plane_set_size(d->plane2, d->fb[1].width-200, d->fb[1].height-200);

	igt_fb_set_position(&d->fb[2], d->plane3, 100, 100);
	igt_fb_set_size(&d->fb[2], d->plane3, d->fb[2].width-400, d->fb[2].height-400);
	igt_plane_set_position(d->plane3, 10, 10);
	igt_plane_set_size(d->plane3, mode->hdisplay-300, mode->vdisplay-300);
	igt_display_commit2(display, COMMIT_ATOMIC);

	if (primary_plane_scaling) {
		/* Switch scaler from plane 1 to plane 2 */
		igt_fb_set_position(&d->fb[0], d->plane1, 0, 0);
		igt_fb_set_size(&d->fb[0], d->plane1, d->fb[0].width, d->fb[0].height);
		igt_plane_set_position(d->plane1, 0, 0);
		igt_plane_set_size(d->plane1, mode->hdisplay, mode->vdisplay);

		igt_fb_set_position(&d->fb[1], d->plane2, 100, 100);
		igt_fb_set_size(&d->fb[1], d->plane2, d->fb[1].width-500,d->fb[1].height-500);
		igt_plane_set_position(d->plane2, 100, 100);
		igt_plane_set_size(d->plane2, mode->hdisplay-200, mode->vdisplay-200);
		igt_display_commit2(display, COMMIT_ATOMIC);
	}
}

static void
test_scaler_with_clipping_clamping_scenario(data_t *d, enum pipe pipe, igt_output_t *output)
{
	drmModeModeInfo *mode;

	igt_require(get_num_scalers(d->devid, pipe) >= 2);

	mode = igt_output_get_mode(output);
	d->plane1 = &d->display.pipes[pipe].planes[0];
	prepare_crtc(d, output, pipe, d->plane1, mode);

	igt_create_pattern_fb(d->drm_fd,
			      mode->hdisplay, mode->vdisplay,
			      DRM_FORMAT_XRGB8888,
			      LOCAL_I915_FORMAT_MOD_X_TILED, &d->fb[1]);

	igt_create_pattern_fb(d->drm_fd,
			      mode->hdisplay, mode->vdisplay,
			      DRM_FORMAT_XRGB8888,
			      LOCAL_I915_FORMAT_MOD_Y_TILED, &d->fb[2]);

	igt_plane_set_fb(d->plane1, &d->fb[1]);
	d->plane2 = igt_output_get_plane(output, 1);
	igt_plane_set_fb(d->plane2, &d->fb[2]);

	igt_fb_set_position(&d->fb[1], d->plane1, 0, 0);
	igt_fb_set_size(&d->fb[1], d->plane1, 300, 300);
	igt_plane_set_position(d->plane1, 100, 400);
	igt_fb_set_position(&d->fb[2], d->plane2, 0, 0);
	igt_fb_set_size(&d->fb[2], d->plane2, 400, 400);
	igt_plane_set_position(d->plane2, 100, 100);

	/* scaled window size is outside the modeset area.*/
	igt_plane_set_size(d->plane1, mode->hdisplay + 200,
					    mode->vdisplay + 200);
	igt_plane_set_size(d->plane2, mode->hdisplay + 100,
					    mode->vdisplay + 100);
	igt_display_commit2(&d->display, COMMIT_ATOMIC);
}

static void find_connected_pipe(igt_display_t *display, bool second, enum pipe *pipe, igt_output_t **output)
{
	enum pipe first = PIPE_NONE;
	igt_output_t *first_output = NULL;
	bool found = false;

	for_each_pipe_with_valid_output(display, *pipe, *output) {
		if (first == *pipe || *output == first_output)
			continue;

		if (second) {
			first = *pipe;
			first_output = *output;
			second = false;
			continue;
		}

		return;
	}

	if (first_output)
		igt_require_f(found, "No second valid output found\n");
	else
		igt_require_f(found, "No valid outputs found\n");
}

static void test_scaler_with_multi_pipe_plane(data_t *d)
{
	igt_display_t *display = &d->display;
	igt_output_t *output1, *output2;
	drmModeModeInfo *mode1, *mode2;
	enum pipe pipe1, pipe2;

	cleanup_crtc(d);

	find_connected_pipe(display, false, &pipe1, &output1);
	find_connected_pipe(display, true, &pipe2, &output2);

	igt_skip_on(!output1 || !output2);

	igt_output_set_pipe(output1, pipe1);
	igt_output_set_pipe(output2, pipe2);

	d->plane1 = igt_output_get_plane(output1, 0);
	d->plane2 = get_num_scalers(d->devid, pipe1) >= 2 ? igt_output_get_plane(output1, 1) : NULL;
	d->plane3 = igt_output_get_plane(output2, 0);
	d->plane4 = get_num_scalers(d->devid, pipe2) >= 2 ? igt_output_get_plane(output2, 1) : NULL;

	mode1 = igt_output_get_mode(output1);
	mode2 = igt_output_get_mode(output2);

	igt_create_pattern_fb(d->drm_fd, 600, 600,
			      DRM_FORMAT_XRGB8888,
			      LOCAL_I915_FORMAT_MOD_Y_TILED, &d->fb[0]);

	igt_create_pattern_fb(d->drm_fd, 500, 500,
			      DRM_FORMAT_XRGB8888,
			      LOCAL_I915_FORMAT_MOD_Y_TILED, &d->fb[1]);

	igt_create_pattern_fb(d->drm_fd, 700, 700,
			      DRM_FORMAT_XRGB8888,
			      LOCAL_I915_FORMAT_MOD_Y_TILED, &d->fb[2]);

	igt_create_pattern_fb(d->drm_fd, 400, 400,
			      DRM_FORMAT_XRGB8888,
			      LOCAL_I915_FORMAT_MOD_Y_TILED, &d->fb[3]);

	igt_plane_set_fb(d->plane1, &d->fb[0]);
	if (d->plane2)
		igt_plane_set_fb(d->plane2, &d->fb[1]);
	igt_plane_set_fb(d->plane3, &d->fb[2]);
	if (d->plane4)
		igt_plane_set_fb(d->plane4, &d->fb[3]);
	igt_display_commit2(display, COMMIT_ATOMIC);

	/* Upscaling Primary */
	igt_plane_set_size(d->plane1, mode1->hdisplay, mode1->vdisplay);
	igt_plane_set_size(d->plane3, mode2->hdisplay, mode2->vdisplay);
	igt_display_commit2(display, COMMIT_ATOMIC);

	/* Upscaling Sprites */
	igt_plane_set_size(d->plane2 ?: d->plane1, mode1->hdisplay, mode1->vdisplay);
	igt_plane_set_size(d->plane4 ?: d->plane3, mode2->hdisplay, mode2->vdisplay);
	igt_display_commit2(display, COMMIT_ATOMIC);
}

igt_main
{
	data_t data = {};
	enum pipe pipe;

	igt_skip_on_simulation();

	igt_fixture {
		data.drm_fd = drm_open_driver(DRIVER_INTEL);
		igt_require_pipe_crc(data.drm_fd);
		igt_display_init(&data.display, data.drm_fd);
		data.devid = intel_get_drm_devid(data.drm_fd);
		igt_require(data.display.is_atomic);
	}

	for_each_pipe_static(pipe) igt_subtest_group {
		igt_output_t *output;

		igt_fixture {
			igt_display_require_output_on_pipe(&data.display, pipe);

			igt_require(get_num_scalers(data.devid, pipe) > 0);
		}

		igt_subtest_f("pipe-%s-plane-scaling", kmstest_pipe_name(pipe))
			for_each_valid_output_on_pipe(&data.display, pipe, output)
				test_plane_scaling_on_pipe(&data, pipe, output);

		igt_subtest_f("pipe-%s-scaler-with-pixel-format", kmstest_pipe_name(pipe))
			for_each_valid_output_on_pipe(&data.display, pipe, output)
				test_scaler_with_pixel_format_pipe(&data, pipe, output);

		igt_subtest_f("pipe-%s-scaler-with-rotation", kmstest_pipe_name(pipe))
			for_each_valid_output_on_pipe(&data.display, pipe, output)
				test_scaler_with_rotation_pipe(&data, pipe, output);

		igt_subtest_f("pipe-%s-scaler-with-clipping-clamping", kmstest_pipe_name(pipe))
			for_each_valid_output_on_pipe(&data.display, pipe, output)
				test_scaler_with_clipping_clamping_scenario(&data, pipe, output);
	}

	igt_subtest_f("2x-scaler-multi-pipe")
		test_scaler_with_multi_pipe_plane(&data);

	igt_fixture
		igt_display_fini(&data.display);
}
