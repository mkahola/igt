/*
 * Copyright © 2018-2023 Intel Corporation
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
 * Displayport Display Stream Compression test
 * Until the CRC support is added this needs to be invoked with --interactive
 * to manually verify if the test pattern is seen without corruption for each
 * subtest.
 *
 * Authors:
 * 	Manasi Navare <manasi.d.navare@intel.com>
 * 	Swati Sharma <swati2.sharma@intel.com>
 */

/**
 * TEST: kms dsc
 * Category: Display
 * Description: Test to validate display stream compression
 * Driver requirement: i915, xe
 * Mega feature: VDSC
 */

#include "kms_dsc_helper.h"

/**
 * SUBTEST: dsc-%s
 * Description: Tests Display Stream Compression functionality if supported by a
 *              connector by forcing %arg[1] on all connectors that support it
 *
 * arg[1]:
 *
 * @basic:                        DSC with default parameters
 * @with-bpc:                     DSC with certain input BPC for the connector
 * @with-bpc-formats:             DSC with certain input BPC for the connector and diff formats
 * @with-formats:                 DSC with default parameters and creating fb with diff formats
 * @with-output-formats:          DSC with output formats
 * @with-output-formats-with-bpc: DSC with output formats with certain input BPC for the connector
 * @fractional-bpp:               DSC with fractional bpp with default parameters
 * @fractional-bpp-with-bpc:      DSC with fractional bpp with certain input BPC for the connector
 */

IGT_TEST_DESCRIPTION("Test to validate display stream compression");

#define LEN		20
#define DEFAULT_BPC	0
#define MIN_DSC_BPC	8

#define TEST_DSC_BASIC		(0<<0)
#define TEST_DSC_BPC		(1<<0)
#define TEST_DSC_FORMAT		(1<<1)
#define TEST_DSC_OUTPUT_FORMAT	(1<<2)
#define TEST_DSC_FRACTIONAL_BPP (1<<3)

typedef struct {
	int drm_fd;
	uint32_t devid;
	igt_display_t display;
	struct igt_fb fb_test_pattern;
	enum dsc_output_format output_format;
	unsigned int plane_format;
	igt_output_t *output;
	int input_bpc;
	int disp_ver;
	enum pipe pipe;
	bool limited;
} data_t;

static int output_format_list[] = {DSC_FORMAT_YCBCR420, DSC_FORMAT_YCBCR444};
static int format_list[] = {DRM_FORMAT_XYUV8888, DRM_FORMAT_XRGB2101010, DRM_FORMAT_XRGB16161616F, DRM_FORMAT_YUYV};
static uint32_t bpc_list[] = {8, 10, 12};

static inline void manual(const char *expected)
{
	igt_debug_interactive_mode_check("all", expected);
}

static drmModeModeInfo *get_next_mode(igt_output_t *output, int index)
{
	drmModeConnector *connector = output->config.connector;
	drmModeModeInfo *next_mode = NULL;

	if (index < connector->count_modes)
		next_mode = &connector->modes[index];

	return next_mode;
}

static void test_reset(data_t *data)
{
	igt_debug("Reset input BPC\n");
	data->input_bpc = 0;
	force_dsc_enable_bpc(data->drm_fd, data->output, data->input_bpc);

	igt_debug("Reset DSC output format\n");
	data->output_format = DSC_FORMAT_RGB;
	force_dsc_output_format(data->drm_fd, data->output, data->output_format);
}

static void test_cleanup(data_t *data)
{
	igt_output_t *output = data->output;
	igt_plane_t *primary;

	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_plane_set_fb(primary, NULL);

	igt_output_set_pipe(output, PIPE_NONE);
	igt_remove_fb(data->drm_fd, &data->fb_test_pattern);
}

/* re-probe connectors and do a modeset with DSC */
static void update_display(data_t *data, uint32_t test_type)
{
	int ret;
	bool enabled;
	int index = 0;
	int current_bpc = 0;
	igt_plane_t *primary;
	drmModeModeInfo *mode;
	igt_output_t *output = data->output;
	igt_display_t *display = &data->display;
	drmModeConnector *connector = output->config.connector;

	/* sanitize the state before starting the subtest */
	igt_display_reset(display);
	igt_display_commit(display);

	igt_debug("DSC is supported on %s\n", data->output->name);
	save_force_dsc_en(data->drm_fd, data->output);
	force_dsc_enable(data->drm_fd, data->output);

	if (test_type & TEST_DSC_BPC) {
		igt_debug("Trying to set input BPC to %d\n", data->input_bpc);
		force_dsc_enable_bpc(data->drm_fd, data->output, data->input_bpc);
	}

	if (test_type & TEST_DSC_OUTPUT_FORMAT) {
		igt_debug("Trying to set DSC %s output format\n",
			   kmstest_dsc_output_format_str(data->output_format));
		force_dsc_output_format(data->drm_fd, data->output, data->output_format);
	}

	if (test_type & TEST_DSC_FRACTIONAL_BPP) {
		igt_debug("DSC fractional bpp is supported on %s\n", data->output->name);
		save_force_dsc_fractional_bpp_en(data->drm_fd, data->output);
		force_dsc_fractional_bpp_enable(data->drm_fd, data->output);
	}

	igt_output_set_pipe(output, data->pipe);
	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);

	igt_skip_on(!igt_plane_has_format_mod(primary, data->plane_format,
		    DRM_FORMAT_MOD_LINEAR));

	mode = igt_output_get_highres_mode(output);

	do {
		if (data->output_format != DSC_FORMAT_RGB && index > 0)
			mode = get_next_mode(output, index++);

		if (mode == NULL)
			goto reset;

		igt_output_override_mode(output, mode);

		if (!intel_pipe_output_combo_valid(display)) {
			if (data->output_format == DSC_FORMAT_RGB) {
				igt_info("No valid pipe/output/mode found.\n");

				mode = NULL;
				goto reset;
			} else {
				continue;
			}
		}

		igt_create_pattern_fb(data->drm_fd,
				      mode->hdisplay,
				      mode->vdisplay,
				      data->plane_format,
				      DRM_FORMAT_MOD_LINEAR,
				      &data->fb_test_pattern);
		igt_plane_set_fb(primary, &data->fb_test_pattern);

		ret = igt_display_try_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
		if (data->output_format == DSC_FORMAT_RGB || ret == 0)
			break;

		igt_remove_fb(data->drm_fd, &data->fb_test_pattern);
	} while (index < connector->count_modes);

	if (ret != 0)
		goto reset;

	/* until we have CRC check support, manually check if RGB test
	 * pattern has no corruption.
	 */
	manual("RGB test pattern without corruption");

	enabled = igt_is_dsc_enabled(data->drm_fd, output->name);
	igt_info("Current mode is: %dx%d @%dHz -- DSC is: %s\n",
				mode->hdisplay,
				mode->vdisplay,
				mode->vrefresh,
				enabled ? "ON" : "OFF");

	restore_force_dsc_en();
	restore_force_dsc_fractional_bpp_en();

	if (test_type & TEST_DSC_BPC) {
		current_bpc = igt_get_pipe_current_bpc(data->drm_fd, data->pipe);
		igt_skip_on_f(data->input_bpc != current_bpc,
			      "Input bpc = %d is not equal to current bpc = %d\n",
			      data->input_bpc, current_bpc);
	}

	igt_assert_f(enabled,
		     "Default DSC enable failed on connector: %s pipe: %s\n",
		     output->name,
		     kmstest_pipe_name(data->pipe));
reset:
	test_reset(data);

	test_cleanup(data);
	igt_skip_on(mode == NULL);
	igt_assert_eq(ret, 0);
}

static void test_dsc(data_t *data, uint32_t test_type, int bpc,
		     unsigned int plane_format,
		     enum dsc_output_format output_format)
{
	igt_display_t *display = &data->display;
	igt_output_t *output;
	enum pipe pipe;
	char name[3][LEN] = {
				{0},
				{0},
				{0},
			    };

	igt_require(check_gen11_bpc_constraint(data->drm_fd, data->input_bpc));

	for_each_pipe_with_valid_output(display, pipe, output) {
		data->output_format = output_format;
		data->plane_format = plane_format;
		data->input_bpc = bpc;
		data->output = output;
		data->pipe = pipe;

		if (!is_dsc_supported_by_sink(data->drm_fd, data->output) ||
		    !check_gen11_dp_constraint(data->drm_fd, data->output, data->pipe))
			continue;

		if (igt_get_output_max_bpc(data->drm_fd, output->name) < MIN_DSC_BPC) {
			igt_info("Output %s doesn't support min %d-bpc\n", igt_output_name(data->output), MIN_DSC_BPC);
			continue;
		}

		if ((test_type & TEST_DSC_OUTPUT_FORMAT) &&
		    (!is_dsc_output_format_supported(data->drm_fd, data->disp_ver,
						     data->output, data->output_format)))
			continue;

		if ((test_type & TEST_DSC_FRACTIONAL_BPP) &&
		    (!is_dsc_fractional_bpp_supported(data->disp_ver,
						      data->drm_fd, data->output)))
			continue;

		if (test_type & TEST_DSC_OUTPUT_FORMAT)
			snprintf(&name[0][0], LEN, "-%s", kmstest_dsc_output_format_str(data->output_format));
		if (test_type & TEST_DSC_FORMAT)
			snprintf(&name[1][0], LEN, "-%s", igt_format_str(data->plane_format));
		if (test_type & TEST_DSC_BPC)
			snprintf(&name[2][0], LEN, "-%dbpc", data->input_bpc);

		igt_dynamic_f("pipe-%s-%s%s%s%s",  kmstest_pipe_name(data->pipe), data->output->name,
			      &name[0][0], &name[1][0], &name[2][0])
			update_display(data, test_type);

		if (data->limited)
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
	default:
		return IGT_OPT_HANDLER_ERROR;
	}

	return IGT_OPT_HANDLER_SUCCESS;
}

static const char help_str[] =
	"  --limited|-l\t\tLimit execution to 1 valid pipe-output combo\n";

data_t data = {};

igt_main_args("l", NULL, help_str, opt_handler, &data)
{
	igt_fixture() {
		data.drm_fd = drm_open_driver_master(DRIVER_INTEL | DRIVER_XE);
		data.devid = intel_get_drm_devid(data.drm_fd);
		data.disp_ver = intel_display_ver(data.devid);
		kmstest_set_vt_graphics_mode();
		igt_install_exit_handler(kms_dsc_exit_handler);
		igt_display_require(&data.display, data.drm_fd);
		igt_display_require_output(&data.display);
		igt_require(is_dsc_supported_by_source(data.drm_fd));
	}

	igt_describe("Tests basic display stream compression functionality if supported "
		     "by a connector by forcing DSC on all connectors that support it "
		     "with default parameters");
	igt_subtest_with_dynamic("dsc-basic")
			test_dsc(&data, TEST_DSC_BASIC, DEFAULT_BPC,
				 DRM_FORMAT_XRGB8888, DSC_FORMAT_RGB);

	igt_describe("Tests basic display stream compression functionality if supported "
		     "by a connector by forcing DSC on all connectors that support it "
		     "with default parameters and creating fb with diff formats");
	igt_subtest_with_dynamic("dsc-with-formats") {
		for (int k = 0; k < ARRAY_SIZE(format_list); k++)
			test_dsc(&data, TEST_DSC_FORMAT, DEFAULT_BPC,
				 format_list[k], DSC_FORMAT_RGB);
	}

	igt_describe("Tests basic display stream compression functionality if supported "
		     "by a connector by forcing DSC on all connectors that support it "
		     "with certain input BPC for the connector");
	igt_subtest_with_dynamic("dsc-with-bpc") {
		for (int j = 0; j < ARRAY_SIZE(bpc_list); j++)
			test_dsc(&data, TEST_DSC_BPC, bpc_list[j],
				 DRM_FORMAT_XRGB8888, DSC_FORMAT_RGB);
	}

	igt_describe("Tests basic display stream compression functionality if supported "
		     "by a connector by forcing DSC on all connectors that support it "
		     "with certain input BPC for the connector with diff formats");
	igt_subtest_with_dynamic("dsc-with-bpc-formats") {
		for (int j = 0; j < ARRAY_SIZE(bpc_list); j++) {
			for (int k = 0; k < ARRAY_SIZE(format_list); k++) {
				test_dsc(&data, TEST_DSC_BPC | TEST_DSC_FORMAT,
				bpc_list[j], format_list[k],
				DSC_FORMAT_RGB);
			}
		}
	}

	igt_describe("Tests basic display stream compression functionality if supported "
		     "by a connector by forcing DSC and output format on all connectors "
		     "that support it");
	igt_subtest_with_dynamic("dsc-with-output-formats") {
		for (int k = 0; k < ARRAY_SIZE(output_format_list); k++)
			test_dsc(&data, TEST_DSC_OUTPUT_FORMAT, DEFAULT_BPC,
				 DRM_FORMAT_XRGB8888,
				 output_format_list[k]);
	}

	igt_describe("Tests basic display stream compression functionality if supported "
		     "by a connector by forcing DSC and output format on all connectors "
		     "that support it with certain input BPC for the connector");
	igt_subtest_with_dynamic("dsc-with-output-formats-with-bpc") {
		for (int k = 0; k < ARRAY_SIZE(output_format_list); k++) {
			for (int j = 0; j < ARRAY_SIZE(bpc_list); j++) {
				test_dsc(&data, TEST_DSC_OUTPUT_FORMAT | TEST_DSC_BPC,
					 bpc_list[j], DRM_FORMAT_XRGB8888,
					 output_format_list[k]);
			}
		}
	}

	igt_describe("Tests fractional compressed bpp functionality if supported "
		     "by a connector by forcing fractional_bpp on all connectors that support it "
		     "with default parameter. While finding the optimum compressed bpp, driver will "
		     "skip over the compressed bpps with integer values. It will go ahead with DSC, "
		     "iff compressed bpp is fractional, failing in which, it will fail the commit.");
	igt_subtest_with_dynamic("dsc-fractional-bpp")
			test_dsc(&data, TEST_DSC_FRACTIONAL_BPP,
				 DEFAULT_BPC, DRM_FORMAT_XRGB8888,
				 DSC_FORMAT_RGB);

	igt_describe("Tests fractional compressed bpp functionality if supported "
		     "by a connector by forcing fractional_bpp on all connectors that support it "
		     "with certain input BPC for the connector.");
	igt_subtest_with_dynamic("dsc-fractional-bpp-with-bpc") {
		for (int j = 0; j < ARRAY_SIZE(bpc_list); j++)
			test_dsc(&data, TEST_DSC_FRACTIONAL_BPP | TEST_DSC_BPC,
				 bpc_list[j], DRM_FORMAT_XRGB8888,
				 DSC_FORMAT_RGB);
	}

	igt_fixture() {
		igt_display_fini(&data.display);
		drm_close_driver(data.drm_fd);
	}
}
