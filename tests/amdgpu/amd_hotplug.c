/*
 * Copyright 2021 Advanced Micro Devices, Inc.
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

#include <fcntl.h>

#include "igt.h"
#include "igt_amd.h"

IGT_TEST_DESCRIPTION("Test simulated hotplugging on connectors");

/* Maximum pipes on any AMD ASIC. */
#define MAX_PIPES 6
#define LAST_HW_SLEEP_PATH "/sys/power/suspend_stats/last_hw_sleep"
#define MEM_SLEEP_PATH "/sys/power/mem_sleep"

/* Common test data. */
typedef struct data {
	igt_display_t display;
	igt_plane_t *primary[MAX_PIPES];
	igt_plane_t *overlay[MAX_PIPES];
	igt_plane_t *cursor[MAX_PIPES];
	igt_output_t *output[MAX_PIPES];
	igt_crtc_t *crtc[MAX_PIPES];
	igt_pipe_crc_t *pipe_crc[MAX_PIPES];
	drmModeModeInfo mode[MAX_PIPES];
	int w[MAX_PIPES];
	int h[MAX_PIPES];
	int fd;
} data_t;

static void test_init(data_t *data)
{
	igt_display_t *display = &data->display;
	int i, n, max_pipes = igt_display_n_crtcs(display);
	igt_crtc_t *crtc;

	for_each_crtc(display, crtc) {
		data->crtc[crtc->crtc_index] = crtc;
		data->primary[crtc->crtc_index] =
			igt_crtc_get_plane_type(crtc, DRM_PLANE_TYPE_PRIMARY);
		data->overlay[crtc->crtc_index] =
			igt_crtc_get_plane_type_index(crtc, DRM_PLANE_TYPE_OVERLAY, 0);
		data->cursor[crtc->crtc_index] =
			igt_crtc_get_plane_type(crtc, DRM_PLANE_TYPE_CURSOR);
		data->pipe_crc[crtc->crtc_index] =
			igt_crtc_crc_new(crtc, IGT_PIPE_CRC_SOURCE_AUTO);
	}

	for (i = 0, n = 0; i < display->n_outputs && n < max_pipes; ++i) {
		igt_output_t *output = &display->outputs[i];

		data->output[n] = output;

		/* Only allow physically connected displays for the tests. */
		if (!igt_output_is_connected(output))
			continue;

		igt_assert(kmstest_get_connector_default_mode(
			data->fd, output->config.connector, &data->mode[n]));

		data->w[n] = data->mode[n].hdisplay;
		data->h[n] = data->mode[n].vdisplay;

		n += 1;
	}

	igt_require(data->output[0]);
	igt_display_reset(display);
}

static void test_fini(data_t *data)
{
	igt_display_t *display = &data->display;
	igt_crtc_t *crtc;

	for_each_crtc(display, crtc) {
		igt_pipe_crc_free(data->pipe_crc[crtc->crtc_index]);
	}

	igt_display_reset(display);
	igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, 0);
}

/* Check if mem_sleep is s2idle */
static bool is_system_s2idle(void)
{
	int fd;
	char dst[64];
	int read_byte;

	fd = open(MEM_SLEEP_PATH, O_RDONLY);
	if (fd == -1)
		igt_skip("Open %s file error\n", MEM_SLEEP_PATH);

	read_byte = read(fd, dst, sizeof(dst));
	close(fd);

	if (read_byte <= 0)
		igt_skip("Read %s file error\n", MEM_SLEEP_PATH);

	return strstr(dst, "[s2idle]");
}

/* return the last hw_sleep duration time */
static int get_last_hw_sleep_time(void)
{
	int fd;
	char dst[64];
	int read_byte;

	fd = open(LAST_HW_SLEEP_PATH, O_RDONLY);
	if (fd == -1)
		igt_skip("Open HW sleep statistics file error\n");

	read_byte = read(fd, dst, sizeof(dst));
	close(fd);

	if (read_byte <= 0)
		igt_skip("Read HW sleep statistics file error\n");

	return strtol(dst, NULL, 10);
}

static void test_hotplug_basic(data_t *data, bool suspend)
{
	igt_output_t *output;
	igt_fb_t ref_fb[MAX_PIPES];
	igt_crc_t ref_crc[MAX_PIPES], new_crc[MAX_PIPES];
	igt_display_t *display = &data->display;
	igt_crtc_t *crtc;

	test_init(data);

	/* Setup all outputs */
	for_each_crtc(&data->display, crtc) {
		output = data->output[crtc->crtc_index];
		if (!output || !igt_output_is_connected(output))
			continue;

		igt_create_pattern_fb(data->fd, data->w[crtc->crtc_index],
				      data->h[crtc->crtc_index],
				      DRM_FORMAT_XRGB8888, 0,
				      &ref_fb[crtc->crtc_index]);
		igt_output_set_crtc(output,
				    crtc);
		igt_plane_set_fb(data->primary[crtc->crtc_index],
				 &ref_fb[crtc->crtc_index]);
	}
	igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, 0);

	/* Collect reference CRCs */
	for_each_crtc(&data->display, crtc) {
		output = data->output[crtc->crtc_index];
		if (!output || !igt_output_is_connected(output))
			continue;

		igt_pipe_crc_collect_crc(data->pipe_crc[crtc->crtc_index],
					 &ref_crc[crtc->crtc_index]);
	}

	if (suspend) {
		if (!is_system_s2idle())
			igt_skip("System is not configured for s2idle\n");

		igt_system_suspend_autoresume(SUSPEND_STATE_MEM,
					      SUSPEND_TEST_NONE);
		igt_assert_f(get_last_hw_sleep_time() > 0,
					  "Suspend did not reach hardware sleep state\n");
	}

	/* Trigger hotplug and confirm reference image is the same. */
	for_each_crtc(&data->display, crtc) {
		output = data->output[crtc->crtc_index];
		if (!output || !igt_output_is_connected(output))
			continue;

		igt_amd_trigger_hotplug(data->fd, output->name);

		igt_pipe_crc_collect_crc(data->pipe_crc[crtc->crtc_index],
					 &new_crc[crtc->crtc_index]);
		igt_assert_crc_equal(&ref_crc[crtc->crtc_index],
				     &new_crc[crtc->crtc_index]);
		igt_remove_fb(data->fd, &ref_fb[crtc->crtc_index]);
	}

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

		igt_amd_require_hpd(&data.display, data.fd);
	}

	igt_describe("Tests HPD on each connected output");
	igt_subtest("basic") test_hotplug_basic(&data, false);

	igt_describe("Tests HPD on each connected output after a suspend sequence");
	igt_subtest("basic-suspend") test_hotplug_basic(&data, true);

	igt_fixture()
	{
		igt_display_fini(&data.display);
	}
}
