// SPDX-License-Identifier: MIT
/*
 * Copyright 2025 Advanced Micro Devices, Inc.
 */

#include "sw_sync.h"
#include "kms_colorop_helper.h"

#include <glib.h>

/**
 * TEST: kms colorop
 * Category: Display
 * Description: Test to validate the retrieving and setting of DRM colorops
 *
 * SUBTEST: check_plane_colorop_ids
 * Description: Verify that all igt_colorop_t IDs are unique across planes
 *
 * SUBTEST: plane-%s-%s
 * Description: Tests DRM colorop properties on a plane
 * Driver requirement: amdgpu
 * Functionality: kms_core
 * Mega feature: General Display Features
 * Test category: functionality test
 *
 * arg[1]:
 *
 * @XR24-XR24:				XRGB8888 framebuffer and writeback buffer
 * @XR30-XR30:				XRGB2101010 framebuffer and writeback buffer
 *
 * arg[2]:
 *
 * @bypass:				Bypass Color Pipeline
 * @srgb_eotf:				sRGB EOTF
 * @srgb_inv_eotf:			sRGB Inverse EOTF
 * @srgb_eotf-srgb_inv_eotf:		sRGB EOTF -> sRGB Inverse EOTF
 * @srgb_eotf-srgb_inv_eotf-srgb_eotf:  sRGB EOTF -> sRGB Inverse EOTF -> sRGB EOTF
 * @srgb_inv_eotf_lut:			sRGB Inverse EOTF Custom LUT
 * @srgb_inv_eotf_lut-srgb_eotf_lut:	sRGB Inverse EOTF Custom LUT -> sRGB EOTF Custom LUT
 * @bt2020_inv_oetf:			BT.2020 Inverse OETF
 * @bt2020_oetf:			BT.2020 OETF
 * @bt2020_inv_oetf-bt2020_oetf:	BT.2020 Inverse OETF > BT.2020 OETF
 * @pq_eotf:				PQ EOTF
 * @pq_inv_eotf:			PQ Inverse EOTF
 * @pq_eotf-pq_inv_eotf:		PQ EOTF -> PQ Inverse EOTF
 * @pq_125_eotf:			PQ EOTF for [0.0, 125.0] optical range
 * @pq_125_inv_eotf:			PQ Inverse EOTF for [0.0, 125.0] optical range
 * @pq_125_eotf-pq_125_inv_eotf:	PQ EOTF -> PQ Inverse EOTF with [0.0, 125.0] optical range
 * @pq_125_eotf-pq_125_inv_eotf-pq_125_eotf: PQ EOTF -> PQ Inverse EOTF -> PQ EOTF with [0.0, 125.0] optical range
 * @gamma_2_2_inv_oetf:			Gamma 2.2 Inverse OETF
 * @gamma_2_2_inv_oetf-gamma_2_2_oetf:	Gamma 2.2 Inverse OETF -> Gamma 2.2 OETF
 * @gamma_2_2_inv_oetf-gamma_2_2_oetf-gamma_2_2_inv_oetf: Gamma 2.2 Inverse OETF -> Gamma 2.2 OETF -> Gamma 2.2 Inverse OETF
 * @ctm_3x4_50_desat:			3x4 matrix doing a 50% desaturation
 * @ctm_3x4_overdrive:			3x4 matrix overdring all values by 50%
 * @ctm_3x4_oversaturate:		3x4 matrix oversaturating values
 * @ctm_3x4_bt709_enc:			BT709 encoding matrix
 * @ctm_3x4_bt709_dec:			BT709 decoding matrix
 * @ctm_3x4_bt709_enc_dec:		BT709 encoding matrix, followed by decoding matrix
 * @ctm_3x4_bt709_dec_enc:		BT709 decoding matrix, followed by encoding matrix
 * @multiply_125:			Multiplier by 125
 * @multiply_inv_125:			Multiplier by inverse of 125
 * @3dlut_17_12_rgb:			3D LUT with length 17, color depth 12, and traversal order = RGB
 *
 */

static bool check_writeback_config(igt_display_t *display, igt_output_t *output,
				    drmModeModeInfo override_mode, __u32 fourcc_in,
				    __u32 fourcc_out)
{
	igt_fb_t input_fb, output_fb;
	igt_plane_t *plane;
	uint32_t writeback_format = fourcc_out;
	uint64_t modifier = DRM_FORMAT_MOD_LINEAR;
	int width, height, ret;
	drmModePropertyBlobRes *wb_formats_blob;
	int i;
	__u32 *format;
	bool found_format = false;

	igt_output_override_mode(output, &override_mode);

	width = override_mode.hdisplay;
	height = override_mode.vdisplay;

	plane = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_skip_on_f(!igt_plane_has_format_mod(plane, fourcc_in, DRM_FORMAT_MOD_LINEAR),
		      "plane doesn't support fourcc format %x\n", fourcc_in);

	ret = igt_create_fb(display->drm_fd, width, height,
			    fourcc_in, modifier, &input_fb);
	igt_assert(ret >= 0);

	/* check writeback formats */
	wb_formats_blob = igt_get_writeback_formats_blob(output);
	format = wb_formats_blob->data;

	for (i = 0; i < wb_formats_blob->length / 4; i++)
		if (fourcc_out == format[i])
			found_format = true;

	igt_skip_on_f(!found_format,
		      "writeback doesn't support fourcc format %x\n", fourcc_out);

	ret = igt_create_fb(display->drm_fd, width, height,
			    writeback_format, modifier, &output_fb);
	igt_assert(ret >= 0);

	igt_plane_set_fb(plane, &input_fb);
	igt_output_set_writeback_fb(output, &output_fb);

	ret = igt_display_try_commit_atomic(display, DRM_MODE_ATOMIC_TEST_ONLY |
					    DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	igt_plane_set_fb(plane, NULL);
	igt_remove_fb(display->drm_fd, &input_fb);
	igt_remove_fb(display->drm_fd, &output_fb);

	return !ret;
}

typedef struct {
	bool dump_check;
} data_t;

static data_t data;

static igt_output_t *kms_writeback_get_output(igt_display_t *display, __u32 fourcc_in, __u32 fourcc_out)
{
	int i;
	igt_crtc_t *crtc;

	drmModeModeInfo override_mode = {
		.clock = 25175,
		.hdisplay = 640,
		.hsync_start = 656,
		.hsync_end = 752,
		.htotal = 800,
		.hskew = 0,
		.vdisplay = 480,
		.vsync_start = 490,
		.vsync_end = 492,
		.vtotal = 525,
		.vscan = 0,
		.vrefresh = 60,
		.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
		.name = {"640x480-60"},
	};

	for (i = 0; i < display->n_outputs; i++) {
		igt_output_t *output = &display->outputs[i];

		if (output->config.connector->connector_type != DRM_MODE_CONNECTOR_WRITEBACK)
			continue;

		for_each_crtc(display, crtc) {
			igt_output_set_crtc(output,
					    crtc);

			if (check_writeback_config(display, output, override_mode, fourcc_in, fourcc_out)) {
				igt_debug("Using connector %u:%s on pipe %d\n",
					  output->config.connector->connector_id,
					  output->name, crtc->pipe);
				return output;
			}
		}

		igt_debug("We found %u:%s, but this test will not be able to use it.\n",
			  output->config.connector->connector_id, output->name);

		/* Restore any connectors we don't use, so we don't trip on them later */
		kmstest_force_connector(display->drm_fd, output->config.connector, FORCE_CONNECTOR_UNSPECIFIED);
	}

	return NULL;
}

static bool compare_with_bracket(igt_fb_t *in, igt_fb_t *out)
{
	/* Each driver is expected to have its own bracket, i.e., by trial and error */
	if (is_vkms_device(in->fd))
		return igt_cmp_fb_pixels(in, out, 1, 1);

	if (is_amdgpu_device(in->fd))
		return igt_cmp_fb_pixels(in, out, 13, 13);

	/*
	 * By default we'll look for a [0, 0] bracket. We can then
	 * define it for each driver that implements support for this
	 * test. That way we can understand the precision of each
	 * driver better.
	 */
	return igt_cmp_fb_pixels(in, out, 0, 0);
}

#define MAX_COLOROPS 5

static void apply_transforms(kms_colorop_t *colorops[], igt_fb_t *sw_transform_fb)
{
	int i;
	igt_pixel_transform transforms[MAX_COLOROPS];

	for (i = 0; colorops[i]; i++)
		transforms[i] = colorops[i]->transform;

	igt_color_transform_pixels(sw_transform_fb, transforms, i);
}

static void colorop_plane_test(igt_display_t *display,
			       igt_output_t *output,
			       igt_plane_t *plane,
			       igt_fb_t *input_fb,
			       igt_fb_t *output_fb,
			       __u32 fourcc_in,
			       __u32 fourcc_out,
			       kms_colorop_t *colorops[])
{
	igt_colorop_t *color_pipeline = NULL;
	igt_fb_t sw_transform_fb;
	igt_crc_t input_crc, output_crc;
	int res;

	igt_fb_get_fnv1a_crc(input_fb, &input_crc);

	/* reset color pipeline*/

	set_color_pipeline_bypass(plane);

	/* Commit */
	igt_plane_set_fb(plane, input_fb);
	igt_output_set_writeback_fb(output, output_fb);

	igt_display_commit_atomic(output->display,
				DRM_MODE_ATOMIC_ALLOW_MODESET,
				NULL);
	igt_get_and_wait_out_fence(output);

	/* Compare input and output buffers. They should be equal here. */
	igt_fb_get_fnv1a_crc(output_fb, &output_crc);

	igt_assert_crc_equal(&input_crc, &output_crc);

	/* create sw transformed buffer */
	res = igt_copy_fb(display->drm_fd, input_fb, &sw_transform_fb);
	igt_assert_lte(0, res);

	igt_assert(igt_cmp_fb_pixels(input_fb, &sw_transform_fb, 0, 0));

	apply_transforms(colorops, &sw_transform_fb);

	if (data.dump_check)
		igt_dump_fb(display, &sw_transform_fb, ".", "sw_transform");

	/* discover and set COLOR PIPELINE */

	if (!colorops[0]) {
		/* bypass test */
		set_color_pipeline_bypass(plane);
	} else {
		/* get COLOR_PIPELINE enum */
		color_pipeline = get_color_pipeline(display, plane, colorops);

		/* skip test if we can't find applicable pipeline */
		igt_skip_on(!color_pipeline);

		set_color_pipeline(display, plane, colorops, color_pipeline);
	}

	igt_output_set_writeback_fb(output, output_fb);

	/* commit COLOR_PIPELINE */
	igt_display_commit_atomic(display,
				DRM_MODE_ATOMIC_ALLOW_MODESET,
				NULL);
	igt_get_and_wait_out_fence(output);

	if (data.dump_check)
		igt_dump_fb(display, output_fb, ".", "output");

	/* compare sw transformed and KMS transformed FBs */
	igt_assert(compare_with_bracket(&sw_transform_fb, output_fb));

	/* reset color pipeline*/
	set_color_pipeline_bypass(plane);

	/* Commit */
	igt_plane_set_fb(plane, input_fb);
	igt_output_set_writeback_fb(output, output_fb);

	igt_display_commit_atomic(output->display,
				DRM_MODE_ATOMIC_ALLOW_MODESET,
				NULL);
	igt_get_and_wait_out_fence(output);
}

static void check_plane_colorop_ids(igt_display_t *display)
{
	igt_plane_t *plane;
	int colorop_idx;
	igt_colorop_t *next;
	igt_crtc_t *crtc;
	int prop_val = 0;

	/* Use hash tables to track drm_planes and unique IDs */
	GHashTable *plane_set = g_hash_table_new(g_direct_hash, g_direct_equal);
	GHashTable *id_set = g_hash_table_new(g_direct_hash, g_direct_equal);

	for_each_crtc(display, crtc) {
		for_each_plane_on_pipe(display, crtc->pipe, plane) {
			/* Skip when a drm_plane is already scanned */
			if (g_hash_table_contains(plane_set, GINT_TO_POINTER(plane->drm_plane->plane_id)))
				continue;

			g_hash_table_add(plane_set, GINT_TO_POINTER(plane->drm_plane->plane_id));

			for (colorop_idx = 0; colorop_idx < plane->num_color_pipelines; colorop_idx++) {
				next = plane->color_pipelines[colorop_idx];
				while (next) {
					/* Check if the ID already exists in the set */
					if (g_hash_table_contains(id_set, GINT_TO_POINTER(next->id))) {
						igt_fail_on_f(true, "Duplicate colorop ID %u found on plane %d\n",
						next->id, plane->drm_plane->plane_id);
					}

					g_hash_table_add(id_set, GINT_TO_POINTER(next->id));
					prop_val = igt_colorop_get_prop(display, next, IGT_COLOROP_NEXT);
					next = igt_find_colorop(display, prop_val);
				}
			}
		}
	}

	g_hash_table_destroy(id_set);
	g_hash_table_destroy(plane_set);
	igt_info("All igt_colorop_t IDs are unique across planes\n");
}

static int opt_handler(int option, int option_index, void *_data)
{
	switch (option) {
	case 'd':
		data.dump_check = true;
		break;
	default:
		return IGT_OPT_HANDLER_ERROR;
	}
	return IGT_OPT_HANDLER_SUCCESS;
}

const char *help_str =
	" --dump | -d Prints buffer to files.\n";

static const struct option long_options[] = {
	{ .name = "dump", .has_arg = false, .val = 'd', },
	{}
};

int igt_main_args("d", long_options, help_str, opt_handler, NULL)
{

	struct {
		kms_colorop_t *colorops[MAX_COLOROPS];
		const char *name;
	} tests[] = {
		{ { NULL }, "bypass" },
		{ { &kms_colorop_srgb_eotf, NULL }, "srgb_eotf" },
		{ { &kms_colorop_srgb_inv_eotf, NULL }, "srgb_inv_eotf" },
		{ { &kms_colorop_srgb_eotf, &kms_colorop_srgb_inv_eotf, NULL }, "srgb_eotf-srgb_inv_eotf" },
		{ { &kms_colorop_srgb_eotf, &kms_colorop_srgb_inv_eotf, &kms_colorop_srgb_eotf_2, NULL }, "srgb_eotf-srgb_inv_eotf-srgb_eotf" },
		{ { &kms_colorop_srgb_inv_eotf_lut, NULL }, "srgb_inv_eotf_lut" },
		{ { &kms_colorop_srgb_inv_eotf_lut, &kms_colorop_srgb_eotf_lut, NULL }, "srgb_inv_eotf_lut-srgb_eotf_lut" },
		{ { &kms_colorop_bt2020_inv_oetf, NULL }, "bt2020_inv_oetf" },
		{ { &kms_colorop_bt2020_oetf, NULL }, "bt2020_oetf" },
		{ { &kms_colorop_bt2020_inv_oetf, &kms_colorop_bt2020_oetf, NULL }, "bt2020_inv_oetf-bt2020_oetf" },
		{ { &kms_colorop_pq_eotf, NULL }, "pq_eotf" },
		{ { &kms_colorop_pq_inv_eotf, NULL }, "pq_inv_eotf" },
		{ { &kms_colorop_pq_eotf, &kms_colorop_pq_inv_eotf, NULL }, "pq_eotf-pq_inv_eotf" },
		{ { &kms_colorop_pq_125_eotf, NULL }, "pq_125_eotf" },
		{ { &kms_colorop_pq_125_inv_eotf, NULL }, "pq_125_inv_eotf" },
		{ { &kms_colorop_pq_125_eotf, &kms_colorop_pq_125_inv_eotf, NULL }, "pq_125_eotf-pq_125_inv_eotf" },
		{ { &kms_colorop_pq_125_eotf, &kms_colorop_pq_125_inv_eotf, &kms_colorop_pq_125_eotf_2, NULL }, "pq_125_eotf-pq_125_inv_eotf-pq_125_eotf" },
		{ { &kms_colorop_gamma_22_inv_oetf, NULL }, "gamma_2_2_inv_oetf" },
		{ { &kms_colorop_gamma_22_inv_oetf, &kms_colorop_gamma_22_oetf, NULL }, "gamma_2_2_inv_oetf-gamma_2_2_oetf" },
		{ { &kms_colorop_gamma_22_inv_oetf, &kms_colorop_gamma_22_oetf, &kms_colorop_gamma_22_inv_oetf, NULL }, "gamma_2_2_inv_oetf-gamma_2_2_oetf-gamma_2_2_inv_oetf" },
		{ { &kms_colorop_ctm_3x4_50_desat, NULL }, "ctm_3x4_50_desat" },
		{ { &kms_colorop_ctm_3x4_overdrive, NULL }, "ctm_3x4_overdrive" },
		{ { &kms_colorop_ctm_3x4_oversaturate, NULL }, "ctm_3x4_oversaturate" },
		{ { &kms_colorop_ctm_3x4_bt709_enc, NULL }, "ctm_3x4_bt709_enc" },
		{ { &kms_colorop_ctm_3x4_bt709_dec, NULL }, "ctm_3x4_bt709_dec" },
		{ { &kms_colorop_ctm_3x4_bt709_enc, &kms_colorop_ctm_3x4_bt709_dec, NULL }, "ctm_3x4_bt709_enc_dec" },
		{ { &kms_colorop_ctm_3x4_bt709_dec, &kms_colorop_ctm_3x4_bt709_enc, NULL }, "ctm_3x4_bt709_dec_enc" },
		{ { &kms_colorop_multiply_125, NULL }, "multiply_125" },
		{ { &kms_colorop_multiply_inv_125, NULL }, "multiply_inv_125" },
		{ { &kms_colorop_3dlut_17_12_rgb, NULL }, "3dlut_17_12_rgb" },
	};

	struct {
		__u32 fourcc_in;
		__u32 fourcc_out;
		const char *name;
	} formats[] = {
		{ DRM_FORMAT_XRGB8888, DRM_FORMAT_XRGB8888, "XR24-XR24" },
		{ DRM_FORMAT_XRGB2101010, DRM_FORMAT_XRGB2101010, "XR30-XR30" },
	};

	igt_display_t display;
	int i, j, ret;

	igt_fixture() {
		display.drm_fd = drm_open_driver_master(DRIVER_ANY);

		if (drmSetClientCap(display.drm_fd, DRM_CLIENT_CAP_ATOMIC, 1) == 0)
			display.is_atomic = 1;

		ret = drmSetClientCap(display.drm_fd, DRM_CLIENT_CAP_WRITEBACK_CONNECTORS, 1);

		igt_require_f(!ret, "error setting DRM_CLIENT_CAP_WRITEBACK_CONNECTORS\n");

		igt_display_require(&display, display.drm_fd);
		if (drmSetClientCap(display.drm_fd, DRM_CLIENT_CAP_PLANE_COLOR_PIPELINE, 1) == 0)
			display.has_plane_color_pipeline = 1;

		kmstest_set_vt_graphics_mode();

		igt_display_require(&display, display.drm_fd);
		if (drmSetClientCap(display.drm_fd, DRM_CLIENT_CAP_PLANE_COLOR_PIPELINE, 1) == 0)
			display.has_plane_color_pipeline = 1;

		igt_require(display.is_atomic);
	}

	igt_subtest_f("check_plane_colorop_ids") {
		check_plane_colorop_ids(&display);
	}

	for (j = 0; j < ARRAY_SIZE(formats); j++) {
		igt_output_t *output;
		igt_plane_t *plane;
		igt_fb_t input_fb, output_fb;
		unsigned int fb_id;
		drmModeModeInfo mode;

		igt_subtest_group() {
			igt_fixture() {
				output = kms_writeback_get_output(&display,
								  formats[j].fourcc_in,
								  formats[j].fourcc_out);
				igt_require(output);

				if (output->use_override_mode)
					memcpy(&mode, &output->override_mode, sizeof(mode));
				else
					memcpy(&mode, &output->config.default_mode, sizeof(mode));

				/* create input fb */
				plane = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
				igt_assert(plane);
				igt_require(igt_plane_has_prop(plane, IGT_PLANE_COLOR_PIPELINE));

				fb_id = igt_create_color_pattern_fb(display.drm_fd,
								mode.hdisplay, mode.vdisplay,
								formats[j].fourcc_in, DRM_FORMAT_MOD_LINEAR,
								0.2, 0.2, 0.2, &input_fb);
				igt_assert(fb_id >= 0);
				igt_plane_set_fb(plane, &input_fb);

				if (data.dump_check)
					igt_dump_fb(&display, &input_fb, ".", "input");

				/* create output fb */
				fb_id = igt_create_fb(display.drm_fd, mode.hdisplay, mode.vdisplay,
							formats[j].fourcc_in,
							igt_fb_mod_to_tiling(0),
							&output_fb);
				igt_require(fb_id > 0);
			}

			for (i = 0; i < ARRAY_SIZE(tests); i++) {
				igt_describe("Check color ops on a plane");
				igt_subtest_f("plane-%s-%s", formats[j].name, tests[i].name)
					colorop_plane_test(&display,
							output,
							plane,
							&input_fb,
							&output_fb,
							formats[j].fourcc_in,
							formats[j].fourcc_out,
							tests[i].colorops);
			}

			igt_fixture() {
				igt_detach_crtc(&display, output);
				igt_remove_fb(display.drm_fd, &input_fb);
				igt_remove_fb(display.drm_fd, &output_fb);

			}
		}
	}

	igt_fixture() {
		igt_display_fini(&display);
		drm_close_driver(display.drm_fd);
	}
}
