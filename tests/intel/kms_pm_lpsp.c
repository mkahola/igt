/*
 * Copyright © 2013 Intel Corporation
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
 * Author: Paulo Zanoni <paulo.r.zanoni@intel.com>
 *
 */

/**
 * TEST: kms pm lpsp
 * Category: Display
 * Description: These tests validates display Low Power Single Pipe configurations
 * Driver requirement: i915, xe
 * Mega feature: Display Power Management
 */

#include "igt.h"
#include "igt_kmod.h"
#include "igt_pm.h"
#include "igt_sysfs.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

/**
 * SUBTEST: kms-lpsp
 * Description: This test validates lpsp on all connected outputs on low power pipes
 *
 * SUBTEST: screens-disabled
 * Description: This test validates lpsp while all crtc are disabled
 * Driver requirement: i915
 */

#define MAX_SINK_LPSP_INFO_BUF_LEN	4096

#define PWR_DOMAIN_INFO "i915_power_domain_info"

typedef struct {
	int drm_fd;
	int debugfs_fd;
	uint32_t devid;
	char *pwr_dmn_info;
	igt_display_t display;
	struct igt_fb fb;
	igt_output_t *output;
	enum pipe pipe;
} data_t;

static int max_dotclock;

static bool lpsp_is_enabled(data_t *data)
{
	char buf[MAX_SINK_LPSP_INFO_BUF_LEN];
	int len;

	len = igt_debugfs_simple_read(data->debugfs_fd, "i915_lpsp_status",
				      buf, sizeof(buf));
	if (len < 0)
		igt_assert_eq(len, -ENODEV);

	igt_skip_on(strstr(buf, "LPSP: not supported"));

	return strstr(buf, "LPSP: enabled");
}

static bool dmc_supported(int debugfs)
{
	char buf[15];
	int len;

	len = igt_sysfs_read(debugfs, "i915_dmc_info", buf, sizeof(buf) - 1);

	if (len < 0)
		return false;
	else
		return true;
}

/*
 * The LPSP mode is all about an enabled pipe, but we expect to also be in the
 * low power mode when no pipes are enabled, so do this check anyway.
 */
static void screens_disabled_subtest(data_t *data)
{
	int valid_output = 0;

	for (int i = 0; i < data->display.n_outputs; i++) {
		data->output = &data->display.outputs[i];
		igt_output_set_pipe(data->output, PIPE_NONE);
		igt_display_commit(&data->display);
		valid_output++;
	}

	igt_require_f(valid_output, "No connected output found\n");
	/* eDP panel may have power_cycle_delay of 600ms, 1sec delay is safer */
	igt_assert_f(igt_wait(lpsp_is_enabled(data), 1000, 100),
		     "lpsp is not enabled\n%s:\n%s\n",
		     PWR_DOMAIN_INFO, data->pwr_dmn_info =
		     igt_sysfs_get(data->debugfs_fd, PWR_DOMAIN_INFO));
}

static void setup_lpsp_output(data_t *data)
{
	igt_plane_t *primary;
	drmModeModeInfo *mode = igt_output_get_mode(data->output);

	primary = igt_output_get_plane_type(data->output,
					    DRM_PLANE_TYPE_PRIMARY);
	igt_plane_set_fb(primary, NULL);
	igt_create_pattern_fb(data->drm_fd,
			      mode->hdisplay, mode->vdisplay,
			      DRM_FORMAT_XRGB8888,
			      DRM_FORMAT_MOD_LINEAR,
			      &data->fb);
	igt_plane_set_fb(primary, &data->fb);
	igt_display_commit(&data->display);
}

static void test_cleanup(data_t *data)
{
	igt_plane_t *primary;

	if (!data->output || data->output->pending_pipe == PIPE_NONE)
		return;

	primary = igt_output_get_plane_type(data->output,
					    DRM_PLANE_TYPE_PRIMARY);
	igt_plane_set_fb(primary, NULL);
	igt_output_set_pipe(data->output, PIPE_NONE);
	igt_display_commit(&data->display);
	igt_remove_fb(data->drm_fd, &data->fb);
	data->output = NULL;
}

static bool test_constraint(data_t *data)
{
	drmModeModeInfo *mode;

	igt_display_reset(&data->display);
	igt_output_set_pipe(data->output, data->pipe);

	mode = igt_output_get_mode(data->output);

	/* For LPSP avoid Bigjoiner. */
	if (igt_check_force_joiner_status(data->drm_fd, data->output->name))
		return false;

	if (igt_bigjoiner_possible(data->drm_fd, mode, max_dotclock)) {
		for_each_connector_mode(data->output) {
			mode = &data->output->config.connector->modes[j__];

			if (igt_bigjoiner_possible(data->drm_fd, mode, max_dotclock))
				continue;

			igt_output_override_mode(data->output, mode);

			return true;
		}

		return false;
	}

	return true;
}

static void test_lpsp(data_t *data)
{
	setup_lpsp_output(data);
	igt_assert_f(igt_wait(lpsp_is_enabled(data), 1000, 100),
		     "%s: lpsp is not enabled\n%s:\n%s\n",
		     data->output->name, PWR_DOMAIN_INFO, data->pwr_dmn_info =
		     igt_sysfs_get(data->debugfs_fd, PWR_DOMAIN_INFO));
}

IGT_TEST_DESCRIPTION("These tests validates display Low Power Single Pipe configurations");
igt_main()
{
	data_t data = {};

	igt_fixture() {
		data.drm_fd = drm_open_driver_master(DRIVER_INTEL | DRIVER_XE);
		igt_require(data.drm_fd >= 0);
		data.debugfs_fd = igt_debugfs_dir(data.drm_fd);
		igt_require(data.debugfs_fd >= 0);
		igt_pm_enable_audio_runtime_pm();
		kmstest_set_vt_graphics_mode();
		data.devid = intel_get_drm_devid(data.drm_fd);
		igt_display_require(&data.display, data.drm_fd);
		igt_require(igt_pm_dmc_loaded(data.debugfs_fd));

		max_dotclock = igt_get_max_dotclock(data.drm_fd);
	}

	igt_describe("This test validates lpsp while all crtc are disabled");
	igt_subtest("screens-disabled") {
		igt_require_i915(data.drm_fd);
		igt_require_f(!dmc_supported(data.debugfs_fd),
			      "DC states supported platform don't have ROI for this subtest\n");
		screens_disabled_subtest(&data);
	}

	igt_describe("This test validates lpsp on all connected outputs on low power pipes");
	igt_subtest_with_dynamic_f("kms-lpsp") {
		igt_display_t *display = &data.display;
		igt_output_t *output;
		enum pipe pipe;

		for_each_connected_output(display, output) {
			drmModeConnectorPtr connector = output->config.connector;

			if (!i915_output_is_lpsp_capable(data.drm_fd, output))
				continue;

			for_each_pipe(display, pipe) {
				if (!igt_pipe_connector_valid(pipe, output))
					continue;

				/* LPSP is low power single pipe usages i.e. PIPE_A */
				if (pipe != PIPE_A)
					continue;

				if (connector->connector_type != DRM_MODE_CONNECTOR_eDP)
					igt_require_f(intel_display_ver(data.devid) >= 13,
						     "LPSP support on external panel from Gen13+ platform\n");

				data.output = output;
				data.pipe = pipe;

				if (!test_constraint(&data))
					continue;

				igt_dynamic_f("pipe-%s-%s", kmstest_pipe_name(pipe), igt_output_name(output))
					test_lpsp(&data);

				test_cleanup(&data);
			}
		}
	}

	igt_fixture() {
		free(data.pwr_dmn_info);
		igt_display_fini(&data.display);
		drm_close_driver(data.drm_fd);
	}
}
