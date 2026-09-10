// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */

/**
 * TEST: kms sharpness filter
 * Category: Display
 * Description: Test to validate content adaptive sharpness filter
 * Driver requirement: xe
 * Mega feature: General Display Features
 */

#include "igt.h"
#include "igt_kms.h"

/**
 * SUBTEST: filter-basic
 * Description: Verify basic content adaptive sharpness filter.
 *
 * SUBTEST: filter-strength
 * Description: Verify that varying strength (0-255), affects the degree of sharpness applied.
 *
 * SUBTEST: filter-toggle
 * Description: Verify toggling between enabling and disabling content adaptive sharpness filter.
 *
 * SUBTEST: filter-tap
 * Description: Verify content adaptive sharpness filter with resolution change, resolution change
 * 		will lead to selection of distinct taps.
 *
 * SUBTEST: filter-dpms
 * Description: Verify content adaptive sharpness filter with DPMS.
 *
 * SUBTEST: filter-suspend
 * Description: Verify content adaptive sharpness filter with suspend.
 *
 * SUBTEST: filter-scaler-upscale
 * Description: verify content adaptive sharpness filter with 1 plane scaler enabled during upscaling.
 *
 * SUBTEST: filter-scaler-downscale
 * Description: verify content adaptive sharpness filter with 1 plane scaler enabled during downscaling.
 *
 * SUBTEST: invalid-filter-with-scaler
 * Description: Negative check for content adaptive sharpness filter
 * 		when 2 plane scalers have already been enabled and
 * 		attempt is made to enable sharpness filter.
 *
 * SUBTEST: invalid-filter-with-plane
 * Description: Negative check for content adaptive sharpness filter
 * 		when 2 NV12 planes have already been enabled and attempt is
 * 		made to enable the sharpness filter.
 *
 * SUBTEST: invalid-plane-with-filter
 * Description: Negative check for content adaptive sharpness filter
 * 		when 1 NV12 plane and sharpness filter have already been enabled
 * 		and attempt is made to enable the second NV12 plane.
 *
 * SUBTEST: invalid-filter-with-scaling-mode
 * Description: Negative check for content adaptive sharpness filter
 *              when scaling mode is already enabled and attempt is made to enable
 *              sharpness filter.
 *
 * SUBTEST: invalid-filter-with-nearest-neighbor
 * Description: Negative check for content adaptive sharpness filter when
 *              the pipe scaling filter is set to Nearest Neighbor and an
 *              attempt is made to enable the sharpness filter.
*/

IGT_TEST_DESCRIPTION("Test to validate content adaptive sharpness filter");

/*
 * Until the CRC support is added test needs to be invoked with
 * --interactive|--i to manually verify if "sharpened" image
 * is seen without corruption for each subtest.
 */

#define OUTPUT_LIMIT 			3

#define TAP_3				3
#define TAP_5				5
#define TAP_7				7
#define DISABLE_FILTER			0
#define MIN_FILTER_STRENGTH		1
#define MID_FILTER_STRENGTH		128
#define MAX_FILTER_STRENGTH		255
#define MAX_PIXELS_FOR_3_TAP_FILTER	(1920 * 1080)
#define MAX_PIXELS_FOR_5_TAP_FILTER	(3840 * 2160)
#define NROUNDS				10
#define DEFAULT_FILTER_STRENGTH		-1

enum test_type {
	TEST_FILTER_BASIC,
	TEST_FILTER_STRENGTH,
	TEST_FILTER_TOGGLE,
	TEST_FILTER_TAP,
	TEST_FILTER_DPMS,
	TEST_FILTER_SUSPEND,
	TEST_FILTER_UPSCALE,
	TEST_FILTER_DOWNSCALE,
	TEST_INVALID_FILTER_WITH_SCALER,
	TEST_INVALID_FILTER_WITH_PLANE,
	TEST_INVALID_PLANE_WITH_FILTER,
	TEST_INVALID_FILTER_WITH_SCALING_MODE,
	TEST_INVALID_FILTER_WITH_NEAREST_NEIGHBOR,
};

const int filter_strength_list[] = {
	MIN_FILTER_STRENGTH,
	(MIN_FILTER_STRENGTH + MID_FILTER_STRENGTH) / 2,
	MID_FILTER_STRENGTH,
	(MID_FILTER_STRENGTH + MAX_FILTER_STRENGTH) / 2,
	MAX_FILTER_STRENGTH,
};
const int filter_tap_list[] = {
	TAP_3,
	TAP_5,
	TAP_7,
};
static const uint32_t scaling_modes[] = {
	DRM_MODE_SCALE_FULLSCREEN,
	DRM_MODE_SCALE_ASPECT,
};

enum subtest_iter {
	ITER_NONE,
	ITER_STRENGTH,
	ITER_SCALING_MODE,
};

static const struct subtest_entry {
	const char *name;
	const char *describe;
	enum test_type type;
	int filter_strength;
	uint32_t format;
	enum subtest_iter iter;
} subtests[] = {
	{
		.name     = "filter-basic",
		.describe = "Verify basic content adaptive sharpness filter.",
		.type     = TEST_FILTER_BASIC,
		.filter_strength = DEFAULT_FILTER_STRENGTH,
	},
	{
		.name     = "filter-strength",
		.describe = "Verify that varying strength (0-255) affects the degree of sharpness applied.",
		.type     = TEST_FILTER_STRENGTH,
		.filter_strength = DEFAULT_FILTER_STRENGTH,
		.iter     = ITER_STRENGTH,
	},
	{
		.name     = "filter-toggle",
		.describe = "Verify toggling between enabling and disabling content adaptive sharpness filter.",
		.type     = TEST_FILTER_TOGGLE,
		.filter_strength = MAX_FILTER_STRENGTH,
	},
	{
		.name     = "filter-tap",
		.describe = "Verify content adaptive sharpness filter with resolution change; "
			    "resolution change will lead to selection of distinct taps.",
		.type     = TEST_FILTER_TAP,
		.filter_strength = DEFAULT_FILTER_STRENGTH,
	},
	{
		.name     = "filter-dpms",
		.describe = "Verify content adaptive sharpness filter with DPMS.",
		.type     = TEST_FILTER_DPMS,
		.filter_strength = DEFAULT_FILTER_STRENGTH,
	},
	{
		.name     = "filter-suspend",
		.describe = "Verify content adaptive sharpness filter with suspend.",
		.type     = TEST_FILTER_SUSPEND,
		.filter_strength = DEFAULT_FILTER_STRENGTH,
	},
	{
		.name     = "filter-scaler-upscale",
		.describe = "Verify content adaptive sharpness filter with 1 plane scaler enabled during upscaling.",
		.type     = TEST_FILTER_UPSCALE,
		.filter_strength = DEFAULT_FILTER_STRENGTH,
	},
	{
		.name     = "filter-scaler-downscale",
		.describe = "Verify content adaptive sharpness filter with 1 plane scaler enabled during downscaling.",
		.type     = TEST_FILTER_DOWNSCALE,
		.filter_strength = DEFAULT_FILTER_STRENGTH,
	},
	{
		.name     = "invalid-filter-with-scaler",
		.describe = "Negative check for content adaptive sharpness filter "
			    "when 2 plane scalers have already been enabled and "
			    "attempt is made to enable sharpness filter.",
		.type     = TEST_INVALID_FILTER_WITH_SCALER,
		.filter_strength = DEFAULT_FILTER_STRENGTH,
	},
	{
		.name     = "invalid-filter-with-plane",
		.describe = "Negative check for content adaptive sharpness filter "
			    "when 2 NV12 planes have already been enabled and "
			    "attempt is made to enable the sharpness filter.",
		.type     = TEST_INVALID_FILTER_WITH_PLANE,
		.filter_strength = DEFAULT_FILTER_STRENGTH,
		.format   = DRM_FORMAT_NV12,
	},
	{
		.name     = "invalid-plane-with-filter",
		.describe = "Negative check for content adaptive sharpness filter "
			    "when 1 NV12 plane and sharpness filter have already been enabled "
			    "and attempt is made to enable the second NV12 plane.",
		.type     = TEST_INVALID_PLANE_WITH_FILTER,
		.filter_strength = DEFAULT_FILTER_STRENGTH,
		.format   = DRM_FORMAT_NV12,
	},
	{
		.name     = "invalid-filter-with-scaling-mode",
		.describe = "Negative check for content adaptive sharpness filter "
			    "when scaling mode is already enabled and attempt is made "
			    "to enable sharpness filter.",
		.type     = TEST_INVALID_FILTER_WITH_SCALING_MODE,
		.filter_strength = DEFAULT_FILTER_STRENGTH,
		.iter     = ITER_SCALING_MODE,
	},
	{
		.name     = "invalid-filter-with-nearest-neighbor",
		.describe = "Negative check for content adaptive sharpness filter "
			    "when the pipe scaling filter is set to Nearest Neighbor "
			    "and an attempt is made to enable the sharpness filter.",
		.type     = TEST_INVALID_FILTER_WITH_NEAREST_NEIGHBOR,
		.filter_strength = DEFAULT_FILTER_STRENGTH,
	},
};

typedef struct {
	int drm_fd;
	bool limited;
	bool all_crtcs;
	bool all_outputs;
	struct igt_fb fb[4];
	igt_crtc_t *crtc;
	igt_display_t display;
	igt_output_t *output;
	igt_plane_t *plane[4];
	drmModeModeInfo *mode;
	int filter_strength;
	int filter_tap;
	uint64_t modifier;
	uint32_t format;
	uint32_t scaling_mode;
} data_t;

static void set_filter_strength_on_pipe(data_t *data)
{
	igt_crtc_set_prop_value(data->crtc,
				    IGT_CRTC_SHARPNESS_STRENGTH,
				    data->filter_strength);
}

static bool has_scaling_mode(igt_output_t *output)
{
	return igt_output_has_prop(output, IGT_CONNECTOR_SCALING_MODE) &&
	       igt_output_get_prop(output, IGT_CONNECTOR_SCALING_MODE);
}

static void paint_image(igt_fb_t *fb)
{
	cairo_t *cr = igt_get_cairo_ctx(fb->fd, fb);
	int img_x, img_y, img_w, img_h;
	const char *file = "1080p-left.png";

	img_x = img_y = 0;
	img_w = fb->width;
	img_h = fb->height;

	igt_paint_image(cr, file, img_x, img_y, img_w, img_h);

	igt_put_cairo_ctx(cr);
}

static void setup_fb(int fd, int width, int height, uint32_t format,
		     uint64_t modifier, struct igt_fb *fb)
{
	int fb_id;

	fb_id = igt_create_fb(fd, width, height, format, modifier, fb);
	igt_assert(fb_id);

	paint_image(fb);
}

static void cleanup_fbs(data_t *data)
{
	for (int i = 0; i < ARRAY_SIZE(data->fb); i++)
		igt_remove_fb(data->drm_fd, &data->fb[i]);
}

static void set_planes(data_t *data, enum test_type type)
{
	int ret;
	drmModeModeInfo *mode = data->mode;
	igt_output_t *output = data->output;

	data->plane[1] = igt_output_get_plane(output, 1);
	data->plane[2] = igt_output_get_plane(output, 2);

	if (type == TEST_FILTER_UPSCALE) {
		setup_fb(data->drm_fd, 20, 20, data->format, data->modifier, &data->fb[1]);
		igt_plane_set_fb(data->plane[1], &data->fb[1]);
		igt_plane_set_size(data->plane[1], mode->hdisplay, mode->vdisplay);
	}

	if (type == TEST_FILTER_DOWNSCALE) {
		setup_fb(data->drm_fd, mode->hdisplay, mode->vdisplay, data->format, data->modifier, &data->fb[1]);
		igt_plane_set_fb(data->plane[1], &data->fb[1]);
		igt_plane_set_size(data->plane[1], mode->hdisplay * 0.75, mode->vdisplay * 0.75);
	}

	if (type == TEST_INVALID_FILTER_WITH_SCALER) {
		setup_fb(data->drm_fd, 20, 20, data->format, data->modifier, &data->fb[1]);
		setup_fb(data->drm_fd, 20, 20, data->format, data->modifier, &data->fb[2]);
		igt_plane_set_fb(data->plane[1], &data->fb[1]);
		igt_plane_set_fb(data->plane[2], &data->fb[2]);
		igt_plane_set_size(data->plane[1], mode->hdisplay, mode->vdisplay);
		igt_plane_set_size(data->plane[2], mode->hdisplay, mode->vdisplay);
	}

	if (type == TEST_INVALID_FILTER_WITH_PLANE) {
		setup_fb(data->drm_fd, mode->hdisplay, mode->vdisplay, data->format, data->modifier, &data->fb[1]);
		setup_fb(data->drm_fd, mode->hdisplay, mode->vdisplay, data->format, data->modifier, &data->fb[2]);
		igt_plane_set_fb(data->plane[1], &data->fb[1]);
		igt_plane_set_fb(data->plane[2], &data->fb[2]);
	}

	if (type == TEST_INVALID_PLANE_WITH_FILTER) {
		setup_fb(data->drm_fd, mode->hdisplay, mode->vdisplay, data->format, data->modifier, &data->fb[1]);
		igt_plane_set_fb(data->plane[1], &data->fb[1]);
	}

	if (type == TEST_INVALID_FILTER_WITH_SCALING_MODE) {
		setup_fb(data->drm_fd, mode->hdisplay, mode->vdisplay, data->format, data->modifier, &data->fb[1]);
		setup_fb(data->drm_fd, 640, 480, data->format, data->modifier, &data->fb[2]);
		igt_plane_set_fb(data->plane[1], &data->fb[1]);
		igt_plane_set_fb(data->plane[2], &data->fb[2]);

		ret = igt_display_try_commit_atomic(&data->display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
		igt_assert_eq(ret, 0);

		mode->hdisplay = 640;
		mode->vdisplay = 480;

		igt_output_override_mode(data->output, mode);
		igt_plane_set_fb(data->plane[2], NULL);
		igt_plane_set_fb(data->plane[1], &data->fb[2]);

		igt_output_set_prop_value(data->output, IGT_CONNECTOR_SCALING_MODE, data->scaling_mode);
	}
}

static void cleanup(data_t *data)
{
	igt_display_reset(&data->display);

	cleanup_fbs(data);
}

static void get_modes_for_filter_taps(int drm_fd, igt_output_t *output,
				      drmModeModeInfo *mode[3])
{
	drmModeConnector *connector = output->config.connector;
	int max_dotclock = igt_get_max_dotclock(drm_fd);
	int total_pixels = 0;

	/*
	 * TAP 3: mode->hdisplay <= 1920 && mode->vdisplay <= 1080
	 * TAP 5: (mode->hdisplay > 1920 && mode->hdisplay < 3840) &&
	 * 	  (mode->vdisplay > 1080 && mode->vdisplay < 2160)
	 * TAP 7: mode->hdisplay >= 3840 && mode->vdisplay >= 2160
	 *
	 * Skip modes requiring joiner since joiner + CASF is unsupported.
	 */
	for (int i = 0; i < connector->count_modes; i++) {
		if (igt_bigjoiner_possible(drm_fd, &connector->modes[i],
					   max_dotclock))
			continue;

		total_pixels = connector->modes[i].hdisplay * connector->modes[i].vdisplay;

		if (total_pixels <= MAX_PIXELS_FOR_3_TAP_FILTER)
			mode[0] = &connector->modes[i];

		if (total_pixels > MAX_PIXELS_FOR_3_TAP_FILTER &&
		    total_pixels <= MAX_PIXELS_FOR_5_TAP_FILTER)
			mode[1] = &connector->modes[i];

		if (total_pixels > MAX_PIXELS_FOR_5_TAP_FILTER)
			mode[2] = &connector->modes[i];
	}
}

static int test_filter_toggle(data_t *data)
{
	int ret = 0;

	for (int i = 0; i < NROUNDS; i++) {
		if (i % 2 == 0)
			data->filter_strength = DISABLE_FILTER;
		else
			data->filter_strength = MAX_FILTER_STRENGTH;

		set_filter_strength_on_pipe(data);
		ret |= igt_display_try_commit2(&data->display, COMMIT_ATOMIC);
	}

	return ret;
}

static bool is_invalid_test(enum test_type type)
{
	switch (type) {
	case TEST_INVALID_FILTER_WITH_SCALER:
	case TEST_INVALID_FILTER_WITH_PLANE:
	case TEST_INVALID_PLANE_WITH_FILTER:
	case TEST_INVALID_FILTER_WITH_SCALING_MODE:
	case TEST_INVALID_FILTER_WITH_NEAREST_NEIGHBOR:
		return true;
	default:
		return false;
	}
}

static bool needs_extra_planes(enum test_type type)
{
	switch (type) {
	case TEST_FILTER_UPSCALE:
	case TEST_FILTER_DOWNSCALE:
	case TEST_INVALID_FILTER_WITH_SCALER:
	case TEST_INVALID_FILTER_WITH_PLANE:
	case TEST_INVALID_FILTER_WITH_SCALING_MODE:
		return true;
	default:
		return false;
	}
}

static void test_sharpness_filter(data_t *data, enum test_type type)
{
	igt_output_t *output = data->output;
	drmModeModeInfo *mode = data->mode;
	igt_crc_t crc, no_sharp_crc, sharp_crc;
	igt_pipe_crc_t *pipe_crc = NULL;
	int ret;

	data->plane[0] = igt_crtc_get_plane_type(data->crtc,
						 DRM_PLANE_TYPE_PRIMARY);
	igt_skip_on_f(!igt_plane_has_format_mod(data->plane[0], data->format, data->modifier),
		      "No requested format/modifier on pipe %s\n",
		      igt_crtc_name(data->crtc));

	setup_fb(data->drm_fd, mode->hdisplay, mode->vdisplay,
		 data->format, data->modifier, &data->fb[0]);
	igt_plane_set_fb(data->plane[0], &data->fb[0]);

	if (type == TEST_INVALID_FILTER_WITH_SCALING_MODE)
		igt_require_f(has_scaling_mode(output), "No connector scaling mode found on %s\n", output->name);

	if (type == TEST_INVALID_FILTER_WITH_NEAREST_NEIGHBOR)
		igt_require_f(igt_crtc_has_prop(data->crtc, IGT_CRTC_SCALING_FILTER),
			      "No CRTC SCALING_FILTER property on pipe %s\n",
			      igt_crtc_name(data->crtc));

	if (needs_extra_planes(type))
		set_planes(data, type);

	/*
	 * For positive tests, capture a reference CRC with the sharpness
	 * filter still disabled before applying the filter. After the filter
	 * is enabled and committed, the resulting CRC must differ.
	 */
	if (!is_invalid_test(type)) {
		if (type == TEST_FILTER_DOWNSCALE) {
			ret = igt_display_try_commit2(&data->display, COMMIT_ATOMIC);
			igt_skip_on_f(ret == -ERANGE || ret == -EINVAL,
				      "Scaling op not supported, cdclk limits might be exceeded.\n");
			igt_assert_eq(ret, 0);
		} else {
			igt_display_commit2(&data->display, COMMIT_ATOMIC);
		}
		pipe_crc = igt_crtc_crc_new(data->crtc,
					    IGT_PIPE_CRC_SOURCE_AUTO);
		igt_pipe_crc_collect_crc(pipe_crc, &no_sharp_crc);
	}

	set_filter_strength_on_pipe(data);

	if (type == TEST_INVALID_FILTER_WITH_NEAREST_NEIGHBOR)
		igt_crtc_set_prop_enum(data->crtc, IGT_CRTC_SCALING_FILTER,
				       "Nearest Neighbor");

	if (type == TEST_INVALID_FILTER_WITH_SCALING_MODE)
		ret = igt_display_try_commit_atomic(&data->display, 0, NULL);
	else
		ret = igt_display_try_commit2(&data->display, COMMIT_ATOMIC);

	if (type == TEST_FILTER_TOGGLE)
		ret |= test_filter_toggle(data);

	if (type == TEST_FILTER_DOWNSCALE)
		igt_skip_on_f(ret == -ERANGE || ret == -EINVAL,
			      "Scaling op not supported, cdclk limits might be exceeded.\n");

	if (type == TEST_INVALID_PLANE_WITH_FILTER) {
		data->plane[3] = igt_output_get_plane(data->output, 3);
		setup_fb(data->drm_fd, mode->hdisplay, mode->vdisplay, data->format, data->modifier, &data->fb[3]);
		igt_plane_set_fb(data->plane[3], &data->fb[3]);

		ret = igt_display_try_commit2(&data->display, COMMIT_ATOMIC);
	}

	if (!is_invalid_test(type)) {
		igt_pipe_crc_collect_crc(pipe_crc, &sharp_crc);
	}

	if (type == TEST_FILTER_DPMS || type == TEST_FILTER_SUSPEND) {
		if (type == TEST_FILTER_DPMS) {
			kmstest_set_connector_dpms(data->drm_fd,
						   output->config.connector,
						   DRM_MODE_DPMS_OFF);
			kmstest_set_connector_dpms(data->drm_fd,
						   output->config.connector,
						   DRM_MODE_DPMS_ON);
		} else {
			igt_system_suspend_autoresume(SUSPEND_STATE_MEM,
						      SUSPEND_TEST_NONE);
		}

		igt_pipe_crc_collect_crc(pipe_crc, &crc);
		igt_assert_crc_equal(&crc, &sharp_crc);
	}

	if (!is_invalid_test(type)) {
		igt_assert_f(!igt_check_crc_equal(&sharp_crc, &no_sharp_crc),
			     "Sharpness CRC matches reference CRC; filter had no effect on pipe %s\n",
			     igt_crtc_name(data->crtc));
	}

	if (pipe_crc)
		igt_pipe_crc_free(pipe_crc);

	if (is_invalid_test(type))
		igt_assert_eq(ret, -EINVAL);
	else
		igt_assert_eq(ret, 0);

	cleanup(data);
}

static bool has_sharpness_filter(igt_crtc_t *crtc)
{
	return igt_crtc_has_prop(crtc, IGT_CRTC_SHARPNESS_STRENGTH);
}

static const char * const test_type_names[] = {
	[TEST_FILTER_BASIC]                     = "basic",
	[TEST_FILTER_STRENGTH]                  = NULL,
	[TEST_FILTER_TOGGLE]                    = "toggle",
	[TEST_FILTER_TAP]                       = NULL,
	[TEST_FILTER_DPMS]                      = "dpms",
	[TEST_FILTER_SUSPEND]                   = "suspend",
	[TEST_FILTER_UPSCALE]                   = "upscale",
	[TEST_FILTER_DOWNSCALE]                 = "downscale",
	[TEST_INVALID_FILTER_WITH_SCALER]       = "invalid-filter-with-scaler",
	[TEST_INVALID_FILTER_WITH_PLANE]        = "invalid-filter-with-plane",
	[TEST_INVALID_PLANE_WITH_FILTER]        = "invalid-plane-with-filter",
	[TEST_INVALID_FILTER_WITH_SCALING_MODE] = NULL,
	[TEST_INVALID_FILTER_WITH_NEAREST_NEIGHBOR] = "invalid-filter-with-nearest-neighbor",
};

static void build_test_suffix(data_t *data, enum test_type type,
			      char *name, size_t len)
{
	if (test_type_names[type]) {
		snprintf(name, len, "-%s", test_type_names[type]);
		return;
	}

	/* suffix depends on the current test parameters */
	switch (type) {
	case TEST_FILTER_STRENGTH:
		snprintf(name, len, "-strength-%d", data->filter_strength);
		break;
	case TEST_INVALID_FILTER_WITH_SCALING_MODE:
		snprintf(name, len, "-invalid-filter-with-scaling-mode-%s",
			 kmstest_scaling_mode_str(data->scaling_mode));
		break;
	default:
		igt_assert_f(false, "Unhandled test type %d\n", type);
	}
}

static drmModeModeInfo *find_lowest_mode(igt_output_t *output)
{
	drmModeModeInfo *low = NULL, *mode;

	for_each_connector_mode(output, mode) {
		int pixels = mode->hdisplay * mode->vdisplay;
		int low_pixels = low ? low->hdisplay * low->vdisplay : INT_MAX;

		if (pixels < low_pixels ||
		    (pixels == low_pixels && mode->vrefresh < low->vrefresh))
			low = mode;
	}

	return low;
}

static void
run_sharpness_filter_test(data_t *data, enum test_type type)
{
	igt_display_t *display = &data->display;
	igt_output_t *output;
	igt_crtc_t *crtc;
	int output_counter = 0;
	char name[40];

	for_each_connected_output(display, output) {
		if (type == TEST_FILTER_SUSPEND && !data->all_outputs &&
		    output_counter > OUTPUT_LIMIT)
			continue;

		output_counter++;

		for_each_crtc(display, crtc) {
			igt_display_reset(display);

			data->output = output;
			data->crtc = crtc;

			igt_output_set_crtc(data->output, data->crtc);

			if (type == TEST_FILTER_SUSPEND && !data->all_crtcs && crtc->crtc_index != 0 &&
						crtc->crtc_index != display->n_crtcs-1)
					continue;

			/*
			 * FIXME: Joiner + CASF currently unsupported.
			 * Remove this check once support is implemented.
			 * Until then, run on non-joiner mode in joiner configuration.
			 */
			if (is_joiner_mode(data->drm_fd, data->output)) {
				data->mode = igt_get_non_joiner_mode(data->drm_fd,
								     data->output);
				if (!data->mode) {
					igt_info("No non-joiner mode found on output %s\n",
						 igt_output_name(data->output));
					continue;
				}

				igt_output_override_mode(data->output, data->mode);

				igt_info("Executing on non-joiner mode %dx%d@%d\n",
					 data->mode->hdisplay,
					 data->mode->vdisplay,
					 data->mode->vrefresh);
			} else {
				if (is_invalid_test(type)) {
					data->mode = igt_output_get_mode(data->output);
				} else {
					data->mode = find_lowest_mode(data->output);
					if (!data->mode) {
						igt_info("No mode found on output %s\n",
							 igt_output_name(data->output));
						continue;
					}

					igt_info("Executing on lowest mode %dx%d@%d@%s\n",
						 data->mode->hdisplay,
						 data->mode->vdisplay,
						 data->mode->vrefresh,
						 data->output->name);
				}
			}

			if (!has_sharpness_filter(data->crtc)) {
				igt_info("%s: Doesn't support IGT_CRTC_SHARPNESS_STRENGTH.\n",
				igt_crtc_name(data->crtc));
				continue;
			}

			if (!intel_pipe_output_combo_valid(display)) {
				igt_output_set_crtc(data->output, NULL);
				continue;
			}

			if (type == TEST_FILTER_TAP) {
				drmModeModeInfo *modes[3] = { NULL, NULL, NULL };

				get_modes_for_filter_taps(data->drm_fd, output, modes);
				for (int i = 0; i < 3; i++) {
					data->filter_tap = filter_tap_list[i];
					if (!modes[i]) {
						igt_info("No valid non-joiner mode for tap-%d on %s\n",
							 data->filter_tap,
							 igt_output_name(data->output));
						continue;
					}
					data->mode = modes[i];
					igt_info("Mode %dx%d@%d on output %s\n",
						 data->mode->hdisplay,
						 data->mode->vdisplay,
						 data->mode->vrefresh,
						 igt_output_name(data->output));
					igt_output_override_mode(data->output,
								 data->mode);
					if (!data->output->pending_crtc)
						igt_output_set_crtc(data->output,
								    data->crtc);

					snprintf(name, sizeof(name), "-tap-%d",
						 data->filter_tap);
					igt_dynamic_f("pipe-%s-%s%s",
						       igt_crtc_name(data->crtc),
						       data->output->name, name)
						test_sharpness_filter(data, type);
				}

				if (data->limited)
					break;
				continue;
			}

			build_test_suffix(data, type, name, sizeof(name));

			igt_dynamic_f("pipe-%s-%s%s",
				      igt_crtc_name(data->crtc),
				      data->output->name, name)
				test_sharpness_filter(data, type);

			if (data->limited)
				break;
		}
	}
}

static void set_data_defaults(data_t *data)
{
	data->modifier        = DRM_FORMAT_MOD_LINEAR;
	data->format          = DRM_FORMAT_XRGB8888;
	data->filter_strength = MID_FILTER_STRENGTH;
}

static int iter_count(enum subtest_iter iter)
{
	switch (iter) {
	case ITER_STRENGTH:
		return ARRAY_SIZE(filter_strength_list);
	case ITER_SCALING_MODE:
		return ARRAY_SIZE(scaling_modes);
	default:
		return 1;
	}
}

static void apply_iter_param(data_t *data, const struct subtest_entry *st, int idx)
{
	switch (st->iter) {
	case ITER_STRENGTH:
		data->filter_strength = filter_strength_list[idx];
		break;
	case ITER_SCALING_MODE:
		data->scaling_mode = scaling_modes[idx];
		break;
	default:
		break;
	}
}

static int opt_handler(int opt, int opt_index, void *_data)
{
	data_t *data = _data;

	switch (opt) {
	case 'l':
		data->limited = true;
		break;
	case 'c':
		data->all_crtcs = true;
		break;
	case 'o':
		data->all_outputs = true;
		break;
	default:
		return IGT_OPT_HANDLER_ERROR;
	}

	return IGT_OPT_HANDLER_SUCCESS;
}

static const char help_str[] =
	"  --limited|-l\t\tLimit execution to 1 valid pipe-output combo\n"
	"  --all-outputs|-o\t\tExtend suspend tests for all outputs\n"
	"  --all-crtcs|-c\t\tExtend suspend tests for all crtcs\n";

data_t data = {};

int igt_main_args("loc", NULL, help_str, opt_handler, &data)
{
	igt_fixture() {
		data.drm_fd = drm_open_driver_master(DRIVER_ANY);

		kmstest_set_vt_graphics_mode();

		igt_display_require(&data.display, data.drm_fd);
		igt_require(data.display.is_atomic);
		igt_display_require_output(&data.display);
	}

	for (int s = 0; s < ARRAY_SIZE(subtests); s++) {
		const struct subtest_entry *p = &subtests[s];

		igt_describe(p->describe);
		igt_subtest_with_dynamic(p->name) {
			set_data_defaults(&data);

			if (p->filter_strength != DEFAULT_FILTER_STRENGTH)
				data.filter_strength = p->filter_strength;

			if (p->format != 0)
				data.format = p->format;

			for (int i = 0; i < iter_count(p->iter); i++) {
				apply_iter_param(&data, p, i);
				run_sharpness_filter_test(&data, p->type);
			}
		}
	}

	igt_fixture() {
		igt_display_fini(&data.display);
		drm_close_driver(data.drm_fd);
	}
}
