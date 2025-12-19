/*
 * Copyright 2023 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "igt.h"
#include "igt_amd.h"
#include "igt_core.h"
#include <fcntl.h>

IGT_TEST_DESCRIPTION("Test display refresh from MALL cache");

/*
 * Time needed in seconds for vblank irq count to reach 0.
 * Typically about 5 seconds.
 */

#define MALL_SETTLE_DELAY 10

/* Common test data. */
typedef struct data {
	igt_display_t display;
	igt_plane_t *primary;
	igt_output_t *output;
	igt_crtc_t *pipe;
	igt_pipe_crc_t *pipe_crc;
	drmModeModeInfo *mode;
	enum pipe pipe_id;
	int fd;
	int w;
	int h;
} data_t;

struct line_check {
	int found;
	const char *substr;
};

/* Common test setup. */
static void test_init(data_t *data)
{
	igt_display_t *display = &data->display;
	bool mall_capable = false;
	bool mall_en = false;

	/* It doesn't matter which pipe we choose on amdpgu. */
	data->pipe_id = PIPE_A;
	data->pipe = igt_crtc_for_pipe(&data->display, data->pipe_id);

	igt_display_reset(display);

	igt_amd_get_mall_status(data->fd, &mall_capable, &mall_en);
	igt_require_f(mall_capable, "Requires hardware that supports MALL cache\n");

	/* find a connected output */
	data->output = NULL;
	for (int i=0; i < data->display.n_outputs; ++i) {
		drmModeConnector *connector = data->display.outputs[i].config.connector;
		if (connector->connection == DRM_MODE_CONNECTED) {
			data->output = &data->display.outputs[i];
		}
	}
	igt_require_f(data->output, "Requires a connected display\n");

	data->mode = igt_output_get_mode(data->output);
	igt_assert(data->mode);

	data->primary =
		igt_crtc_get_plane_type(data->pipe, DRM_PLANE_TYPE_PRIMARY);

	data->pipe_crc = igt_pipe_crc_new(data->fd, data->pipe_id,
					  IGT_PIPE_CRC_SOURCE_AUTO);

	igt_output_set_crtc(data->output,
			    data->pipe);

	data->w = data->mode->hdisplay;
	data->h = data->mode->vdisplay;
}

/* Common test cleanup. */
static void test_fini(data_t *data)
{
	igt_pipe_crc_free(data->pipe_crc);
	igt_display_reset(&data->display);
	igt_display_commit_atomic(&data->display, DRM_MODE_ATOMIC_ALLOW_MODESET, 0);
}

static void test_mall_ss(data_t *data)
{
	igt_display_t *display = &data->display;
	igt_fb_t rfb;
	igt_crc_t test_crc, ref_crc;
	bool mall_supp, mall_en;

	test_init(data);

	igt_create_pattern_fb(data->fd, data->w, data->h, DRM_FORMAT_XRGB8888, 0, &rfb);
	igt_plane_set_fb(data->primary, &rfb);
	igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	igt_pipe_crc_collect_crc(data->pipe_crc, &ref_crc);

	sleep(MALL_SETTLE_DELAY);

	igt_amd_get_mall_status(data->fd, &mall_supp, &mall_en);
	igt_fail_on_f(!(mall_supp && mall_en), "MALL did not get enabled\n");

	igt_pipe_crc_collect_crc(data->pipe_crc, &test_crc);
	igt_assert_crc_equal(&ref_crc, &test_crc);

	igt_remove_fb(data->fd, &rfb);
	test_fini(data);
}

int igt_main()
{
	data_t data;

	igt_skip_on_simulation();

	memset(&data, 0, sizeof(data));

	igt_fixture()
	{
		data.fd = drm_open_driver_master(DRIVER_AMDGPU);

		kmstest_set_vt_graphics_mode();

		igt_display_require(&data.display, data.fd);
		igt_require(data.display.is_atomic);
		igt_display_require_output(&data.display);
	}

	igt_describe("Tests whether display scanout is triggered from MALL cache instead "
		     "of GPU VRAM when screen contents are idle");
	igt_subtest("static-screen") test_mall_ss(&data);

	igt_fixture()
	{
		igt_display_fini(&data.display);
	}
}
