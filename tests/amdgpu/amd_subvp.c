// SPDX-License-Identifier: MIT
/*
 * Copyright 2023 Advanced Micro Devices, Inc.
 */

#include <fcntl.h>

#include "igt.h"
#include "igt_amd.h"
#include "igt_core.h"

IGT_TEST_DESCRIPTION("Test enabling sub-viewport feature");

/* Common test data. */
struct data {
	igt_display_t display;
	igt_plane_t *primary[IGT_MAX_PIPES];
	igt_output_t *output[IGT_MAX_PIPES];
	igt_crtc_t *crtc[IGT_MAX_PIPES];
	igt_pipe_crc_t *pipe_crc[IGT_MAX_PIPES];
	drmModeModeInfo mode[IGT_MAX_PIPES];
	enum pipe pipe_id[IGT_MAX_PIPES];
	int fd;
};

struct line_check {
	int found;
	const char *substr;
};

static const drmModeModeInfo test_mode[] = {
	{ 533250,
	3840, 3888, 3920, 4000, 0,
	2160, 2214, 2219, 2222, 0,
	60,
	DRM_MODE_FLAG_NHSYNC,
	0x48,
	"4k60\0",
	}, /* from LG Ultra HD, product_id = 5B09, serial_number = 1010101 */
};

/* Forces a mode for a connector. */
static void force_output_mode(struct data *d, igt_output_t *output,
			      const drmModeModeInfo *mode)
{
	/* This allows us to create a virtual sink. */
	if (!igt_output_is_connected(output)) {
		kmstest_force_edid(d->fd, output->config.connector,
				   igt_kms_get_4k_edid());

		kmstest_force_connector(d->fd, output->config.connector,
					FORCE_CONNECTOR_DIGITAL);
	}

	igt_output_override_mode(output, mode);
}

/* Common test setup. */
static void test_init(struct data *data)
{
	igt_display_t *display = &data->display;
	igt_crtc_t *crtc;
	int i, n;
	bool subvp_capable = false;
	bool subvp_en = false;

	for_each_crtc(display, crtc) {
		data->pipe_id[crtc->pipe] = crtc->pipe;
		data->crtc[crtc->pipe] = crtc;
		data->primary[crtc->pipe] = igt_crtc_get_plane_type(crtc,
								    DRM_PLANE_TYPE_PRIMARY);
		data->pipe_crc[crtc->pipe] = igt_crtc_crc_new(crtc,
							      IGT_PIPE_CRC_SOURCE_AUTO);
	}

	for (i = 0,
	     n = 0; i < display->n_outputs && n < igt_display_n_crtcs(display); ++i) {
		igt_output_t *output = &display->outputs[i];

		data->output[n] = output;
		/* Only allow physically connected displays for the tests. */
		if (!igt_output_is_connected(output))
			continue;
		/* SubVP is only enabled on DP */
		if (output->config.connector->connector_type !=
			DRM_MODE_CONNECTOR_DisplayPort)
			continue;

		igt_assert(kmstest_get_connector_default_mode(
				data->fd, output->config.connector, &data->mode[n]));

		force_output_mode(data, data->output[n], &test_mode[0]);

		n += 1;
	}

	igt_require_f(n >= 2, "Requires at least two connected display\n");

	igt_amd_get_subvp_status(data->fd, &subvp_capable, &subvp_en);
	igt_require_f(subvp_capable, "Requires hardware that supports Sub-viewport\n");

	igt_display_reset(display);
}

/* Common test cleanup. */
static void test_fini(struct data *data)
{
	igt_display_t *display = &data->display;
	igt_crtc_t *crtc;

	for_each_crtc(display, crtc) {
		igt_pipe_crc_free(data->pipe_crc[crtc->pipe]);
	}

	igt_display_reset(display);
	igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, 0);
}

static void test_subvp(struct data *data)
{
	igt_display_t *display = &data->display;
	igt_fb_t rfb;
	bool subvp_supp, subvp_en;
	igt_output_t *output;
	igt_crtc_t *crtc;

	test_init(data);
	igt_enable_connectors(data->fd);

	for_each_crtc(&data->display, crtc) {
		/* Setup the output */
		output = data->output[crtc->pipe];
		if (!output || !igt_output_is_connected(output))
			continue;

		igt_create_pattern_fb(data->fd,
					test_mode[0].hdisplay,
					test_mode[0].vdisplay,
					DRM_FORMAT_XRGB8888,
					0,
					&rfb);

		igt_output_set_crtc(output,
				    igt_crtc_for_pipe(display, data->pipe_id[crtc->pipe]));
		igt_plane_set_fb(data->primary[crtc->pipe], &rfb);
		igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, 0);
	}


	igt_amd_get_subvp_status(data->fd, &subvp_supp, &subvp_en);
	igt_fail_on_f(!(subvp_supp && subvp_en), "SUBVP did not get enabled\n");

	igt_remove_fb(data->fd, &rfb);
	test_fini(data);
}

int igt_main()
{
	struct data data;

	igt_skip_on_simulation();

	memset(&data, 0, sizeof(data));

	igt_fixture()
	{
		data.fd = drm_open_driver_master(DRIVER_AMDGPU);
		igt_display_require(&data.display, data.fd);
		igt_display_require_output(&data.display);
		igt_require(data.display.is_atomic);

		kmstest_set_vt_graphics_mode();
	}

	igt_describe("Tests whether system enables sub-viewport when a specific mode is committed");
	igt_subtest("dual-4k60") test_subvp(&data);

	igt_fixture()
	{
		igt_display_fini(&data.display);
	}
}
