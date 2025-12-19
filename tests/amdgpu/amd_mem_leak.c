/*
 * Copyright 2020 Advanced Micro Devices, Inc.
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
#include <fcntl.h>
#include "lib/amdgpu/amd_mem_leak.h"

IGT_TEST_DESCRIPTION("Test checking memory leaks with suspend-resume and connector hotplug");

/* Common test data. */
typedef struct data {
	igt_display_t display;
	igt_plane_t *primary;
	igt_output_t *output;
	igt_crtc_t *pipe;
	drmModeModeInfo *mode;
	enum pipe pipe_id;
	int fd;
	int w;
	int h;
} data_t;

/* Common test setup. */
static void test_init(data_t *data)
{
	igt_display_t *display = &data->display;

	/* It doesn't matter which pipe we choose on amdpgu. */
	data->pipe_id = PIPE_A;
	data->pipe = igt_crtc_for_pipe(&data->display, data->pipe_id);

	igt_display_reset(display);

	/* find a connected output */
	data->output = NULL;
	for (int i=0; i < data->display.n_outputs; ++i) {
		drmModeConnector *connector = data->display.outputs[i].config.connector;
		if (connector->connection == DRM_MODE_CONNECTED) {
			data->output = &data->display.outputs[i];
		}
	}
	igt_assert_f(data->output, "Requires connected output\n");

	data->mode = igt_output_get_mode(data->output);
	igt_assert(data->mode);

	data->primary =
		igt_crtc_get_plane_type(data->pipe, DRM_PLANE_TYPE_PRIMARY);

	igt_output_set_crtc(data->output,
			    data->pipe);

	data->w = data->mode->hdisplay;
	data->h = data->mode->vdisplay;
}

/* Common test cleanup. */
static void test_fini(data_t *data)
{
	igt_display_reset(&data->display);
}

static void test_suspend_resume(data_t *data)
{
	igt_display_t *display = &data->display;
	igt_fb_t rfb;

	test_init(data);

	if(!clear_memleak(true)) {
		igt_skip("kmemleak is not enabled for this kernel\n");
	}

	igt_create_pattern_fb(data->fd, data->w, data->h, DRM_FORMAT_XRGB8888, 0, &rfb);
	igt_plane_set_fb(data->primary, &rfb);
	igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

	igt_system_suspend_autoresume(SUSPEND_STATE_MEM, SUSPEND_TEST_NONE);

	igt_assert_f(is_no_memleak(), "memory leak detected\n");

	igt_remove_fb(data->fd, &rfb);
	test_fini(data);
}

static void test_hotplug(data_t *data)
{
	igt_display_t *display = &data->display;
	igt_fb_t rfb;

	test_init(data);

	igt_amd_require_hpd(&data->display, data->fd);

	if(!clear_memleak(true)) {
		igt_skip("kmemleak is not enabled for this kernel\n");
	}

	igt_create_pattern_fb(data->fd, data->w, data->h, DRM_FORMAT_XRGB8888, 0, &rfb);
	igt_plane_set_fb(data->primary, &rfb);
	igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

	igt_amd_trigger_hotplug(data->fd, data->output->name);

	igt_assert_f(is_no_memleak(), "memory leak detected\n");

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

	igt_describe("Test memory leaks after resume from suspend");
	igt_subtest("connector-suspend-resume") test_suspend_resume(&data);
	igt_describe("Test memroy leaks after connector hotplug");
	igt_subtest("connector-hotplug") test_hotplug(&data);

	igt_fixture()
	{
		igt_display_fini(&data.display);
		drm_close_driver(data.fd);
	}
}
