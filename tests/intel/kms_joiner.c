/*
 * Copyright © 2020 Intel Corporation
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
 * Author:
 *  Karthik B S <karthik.b.s@intel.com>
 */

/**
 * TEST: kms big joiner
 * Category: Display
 * Description: Test big joiner
 * Driver requirement: i915, xe
 * Mega feature: Pipe Joiner
 */

#include "igt.h"
#include "xe/xe_query.h"
#include "kms_dsc_helper.c"
#include "kms_joiner_helper.h"

/**
 * SUBTEST: invalid-modeset-big-joiner
 * Description: Verify if the modeset on the adjoining pipe is rejected when
 *              the pipe is active with a big joiner modeset
 *
 * SUBTEST: invalid-modeset-ultra-joiner
 * Description: Verify if the modeset on the other pipes are rejected when
 *              the pipe A is active with ultra joiner modeset
 *
 * SUBTEST: basic-big-joiner
 * Description: Verify the basic modeset on big joiner mode on all pipes
 *
 * SUBTEST: basic-ultra-joiner
 * Description: Verify the basic modeset on ultra joiner mode on all pipes
 *
 * SUBTEST: invalid-modeset-force-big-joiner
 * Description: Verify if modeset on adjacent pipe is declined when force big joiner modeset is active.
 *		Force big joiner applies bigjoiner functionality to non-bigjoiner outputs,
 *		so test exclusively targets non-bigjoiner outputs.
 *
 * SUBTEST: basic-force-big-joiner
 * Description: Verify basic big joiner modeset in force joiner mode across all pipes.
 *		Force joiner applies bigjoiner functionality to non-bigjoiner outputs thus,
 *		the test exclusively targets non-bigjoiner outputs.
 *
 * SUBTEST: basic-force-ultra-joiner
 * Description: Verify basic ultra joiner modeset in force joiner mode across all pipes.
 *		Force joiner applies bigjoiner functionality to non-bigjoiner outputs thus,
 *		the test exclusively targets non-bigjoiner outputs.
 *
 * SUBTEST: invalid-modeset-force-ultra-joiner
 * Description: Verify if the modeset on the other pipes are rejected when
 *              the pipe A is active with force ultra joiner modeset.
 *
 * SUBTEST: switch-modeset-ultra-joiner-big-joiner
 * Description: Verify switching between ultra joiner and big joiner modeset.
 *
 * SUBTEST: basic-max-non-joiner
 * Description: Validate basic max non-joiner modeset by selecting the max mode
 *		supported on single pipe.
 */
IGT_TEST_DESCRIPTION("Test joiner / force joiner");

#define INVALID_TEST_OUTPUT 2

typedef struct {
	int drm_fd;
	int big_joiner_output_count;
	int ultra_joiner_output_count;
	int non_big_joiner_output_count;
	int non_ultra_joiner_output_count;
	int non_joiner_output_count;
	int mixed_output_count;
	int output_count;
	int n_pipes;
	uint32_t master_pipes;
	igt_output_t *big_joiner_output[IGT_MAX_PIPES];
	igt_output_t *ultra_joiner_output[IGT_MAX_PIPES];
	igt_output_t *non_big_joiner_output[IGT_MAX_PIPES];
	igt_output_t *non_ultra_joiner_output[IGT_MAX_PIPES];
	igt_output_t *mixed_output[IGT_MAX_PIPES];
	igt_output_t *non_joiner_output[IGT_MAX_PIPES];
	enum pipe pipe_seq[IGT_MAX_PIPES];
	igt_display_t display;
	bool ultra_joiner_supported;
} data_t;

static int max_dotclock;

static void enable_force_joiner_on_all_non_big_joiner_outputs(data_t *data)
{
	bool status;
	igt_output_t *output;
	int i;

	for (i = 0; i < data->non_big_joiner_output_count; i++) {
		output = data->non_big_joiner_output[i];
		status = kmstest_force_connector_joiner(data->drm_fd, output->config.connector, JOINED_PIPES_BIG_JOINER);
		igt_assert_f(status, "Failed to toggle force joiner\n");
	}
}

static void enable_force_joiner_on_all_non_ultra_joiner_outputs(data_t *data)
{
	bool status;
	igt_output_t *output;
	int i;

	for (i = 0; i < data->non_ultra_joiner_output_count; i++) {
		output = data->non_ultra_joiner_output[i];
		status = kmstest_force_connector_joiner(data->drm_fd, output->config.connector, JOINED_PIPES_ULTRA_JOINER);
		igt_assert_f(status, "Failed to toggle force joiner\n");
	}
}

static enum pipe get_next_master_pipe(data_t *data, uint32_t available_pipe_mask)
{
	if ((data->master_pipes & available_pipe_mask) == 0)
		return PIPE_NONE;

	return ffs(data->master_pipes & available_pipe_mask) - 1;
}

static enum pipe setup_pipe(data_t *data, igt_output_t *output, enum pipe pipe, uint32_t available_pipe_mask)
{
	enum pipe master_pipe;
	uint32_t attempt_mask;

	attempt_mask = BIT(pipe);
	master_pipe = get_next_master_pipe(data, available_pipe_mask & attempt_mask);

	if (master_pipe == PIPE_NONE)
		return PIPE_NONE;

	igt_info("Using pipe %s as master and %s slave for %s\n", kmstest_pipe_name(pipe),
		 kmstest_pipe_name(pipe + 1), output->name);
	igt_output_set_pipe(output, pipe);

	return master_pipe;
}

static void set_joiner_mode(data_t *data, igt_output_t *output, drmModeModeInfo *mode)
{
	igt_plane_t *primary;
	igt_fb_t fb;

	igt_info("Committing joiner mode for output %s with mode %dx%d@%d\n",
		  output->name, mode->hdisplay, mode->vdisplay, mode->vrefresh);

	igt_output_set_pipe(output, PIPE_A);
	igt_output_override_mode(output, mode);
	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_create_pattern_fb(data->drm_fd, mode->hdisplay, mode->vdisplay, DRM_FORMAT_XRGB8888,
			      DRM_FORMAT_MOD_LINEAR, &fb);
	igt_plane_set_fb(primary, &fb);
	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	igt_display_reset(&data->display);
	igt_reset_connectors();
	igt_plane_set_fb(primary, NULL);
	igt_remove_fb(data->drm_fd, &fb);
}

static void require_ultra_joiner(data_t *data, enum force_joiner_mode mode)
{
	igt_require_f(data->ultra_joiner_supported,
		      "Ultra joiner not supported on this platform\n");
	igt_require_f(data->n_pipes >= 4, "Minimum 4 pipes required\n");

	if (mode == FORCE_JOINER_ENABLE)
		igt_require_f(data->non_ultra_joiner_output_count > 0,
			      "No non ultra joiner output found\n");
	else
		igt_require_f(data->ultra_joiner_output_count > 0,
			      "No ultra joiner output found\n");
}

static void require_big_joiner(data_t *data, enum force_joiner_mode mode)
{
	igt_require_f(data->n_pipes >= 2, "Minimum 2 pipes required\n");

	if (mode == FORCE_JOINER_ENABLE)
		igt_require_f(data->non_big_joiner_output_count > 0,
			      "No non big joiner output found\n");
	else
		igt_require_f(data->big_joiner_output_count > 0,
			      "No big joiner output found\n");
}

static void switch_modeset_ultra_joiner_big_joiner(data_t *data, igt_output_t *output)
{
	drmModeModeInfo bj_mode;
	drmModeModeInfo uj_mode;
	int status;
	bool ultrajoiner_found;
	enum pipe pipe;
	bool force_joiner_supported;

	drmModeConnector *connector = output->config.connector;

	ultrajoiner_found = ultrajoiner_mode_found(data->drm_fd, connector, max_dotclock, &uj_mode);
	force_joiner_supported = igt_has_force_joiner_debugfs(data->drm_fd, output->name) &&
				 is_dsc_supported_by_sink(data->drm_fd, output);

	if (ultrajoiner_found) {
		igt_output_override_mode(output, &uj_mode);
	} else if (force_joiner_supported &&
		   igt_get_dsc_sink_max_slice_count(data->drm_fd, output->name) >= 8) {
		status = kmstest_force_connector_joiner(data->drm_fd, output->config.connector,
							JOINED_PIPES_ULTRA_JOINER);
		igt_assert_f(status, "Failed to toggle force joiner\n");
		uj_mode = *igt_output_get_mode(output);
	} else {
		igt_info("No ultra joiner mode found on output %s\n", output->name);
		return;
	}

	igt_dynamic_f("pipe-%s-%s", kmstest_pipe_name(pipe), output->name) {
		set_joiner_mode(data, output, &uj_mode);
		/* Switch to big joiner mode */
		if (bigjoiner_mode_found(data->drm_fd, output->config.connector,
					 max_dotclock, &bj_mode)) {
			igt_output_override_mode(output, &bj_mode);
		} else {
			status = kmstest_force_connector_joiner(data->drm_fd,
								output->config.connector,
								JOINED_PIPES_BIG_JOINER);
			igt_assert_f(status, "Failed to toggle force joiner\n");
			bj_mode = *igt_output_get_mode(output);
		}

		set_joiner_mode(data, output, &bj_mode);

		/* Switch back to ultra joiner*/
		if (ultrajoiner_found) {
			igt_output_override_mode(output, &uj_mode);
		} else {
			status = kmstest_force_connector_joiner(data->drm_fd,
								output->config.connector,
								JOINED_PIPES_ULTRA_JOINER);
			igt_assert_f(status, "Failed to toggle force joiner\n");
		}

		set_joiner_mode(data, output, &uj_mode);
	}
}

static void test_single_joiner(data_t *data, int output_count, bool force_joiner)
{
	int i;
	enum pipe pipe, master_pipe;
	uint32_t available_pipe_mask = BIT(data->n_pipes) - 1;
	igt_output_t *output;
	igt_plane_t *primary;
	igt_output_t **outputs;
	igt_fb_t fb;
	drmModeModeInfo *mode;

	outputs = force_joiner ? data->non_big_joiner_output : data->big_joiner_output;
	igt_display_reset(&data->display);
	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	for (i = 0; i < output_count; i++) {
		output = outputs[i];
		for (pipe = 0; pipe < data->n_pipes - 1; pipe++) {
			master_pipe = setup_pipe(data, output, pipe, available_pipe_mask);
			if (master_pipe == PIPE_NONE)
				continue;
			mode = igt_output_get_mode(output);
			primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
			igt_create_pattern_fb(data->drm_fd, mode->hdisplay, mode->vdisplay, DRM_FORMAT_XRGB8888,
					      DRM_FORMAT_MOD_LINEAR, &fb);
			igt_plane_set_fb(primary, &fb);
			igt_display_commit2(&data->display, COMMIT_ATOMIC);
			igt_display_reset(&data->display);
			igt_plane_set_fb(primary, NULL);
			igt_remove_fb(data->drm_fd, &fb);
		}
	}
}

static void test_multi_joiner(data_t *data, int output_count, bool force_joiner)
{
	int i, cleanup;
	uint32_t available_pipe_mask;
	enum pipe pipe, master_pipe;
	igt_output_t **outputs;
	igt_output_t *output;
	igt_plane_t *primary[output_count];
	igt_fb_t fb[output_count];
	drmModeModeInfo *mode;

	available_pipe_mask = BIT(data->n_pipes) - 1;
	outputs = force_joiner ? data->non_big_joiner_output : data->big_joiner_output;
	cleanup = 0;

	igt_display_reset(&data->display);
	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	for (i = 0; i < output_count; i++) {
		output = outputs[i];
		for (pipe = 0; pipe < data->n_pipes; pipe++) {
			master_pipe = setup_pipe(data, output, pipe, available_pipe_mask);
			if (master_pipe == PIPE_NONE)
				continue;
			cleanup++;
			mode = igt_output_get_mode(output);
			primary[i] = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
			igt_create_pattern_fb(data->drm_fd, mode->hdisplay, mode->vdisplay, DRM_FORMAT_XRGB8888,
					      DRM_FORMAT_MOD_LINEAR, &fb[i]);
			igt_plane_set_fb(primary[i], &fb[i]);

			available_pipe_mask &= ~BIT(master_pipe);
			available_pipe_mask &= ~BIT(master_pipe + 1);
			break;
		}
	}
	igt_display_commit2(&data->display, COMMIT_ATOMIC);
	for (i = 0; i < cleanup; i++) {
		igt_plane_set_fb(primary[i], NULL);
		igt_remove_fb(data->drm_fd, &fb[i]);
	}
}

static void test_invalid_modeset_two_joiner(data_t *data,
					    bool mixed, bool force_joiner)
{
	int i, j, ret;
	uint32_t available_pipe_mask;
	uint32_t attempt_mask;
	enum pipe master_pipe;
	igt_output_t **outputs;
	igt_output_t *output;
	igt_plane_t *primary[INVALID_TEST_OUTPUT];
	igt_fb_t fb[INVALID_TEST_OUTPUT];
	drmModeModeInfo mode;

	available_pipe_mask = BIT(data->n_pipes) - 1;
	outputs = force_joiner ? data->non_big_joiner_output :
		  mixed ? data->mixed_output : data->big_joiner_output;
	igt_display_reset(&data->display);
	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	for (i = 0; i < data->n_pipes - 1; i++) {
		attempt_mask = BIT(data->pipe_seq[i]);
		master_pipe = get_next_master_pipe(data, available_pipe_mask & attempt_mask);

		if (master_pipe == PIPE_NONE)
			continue;

		for (j = 0; j < INVALID_TEST_OUTPUT; j++) {
			output = outputs[j];

			if (!force_joiner) {
				igt_require_f(bigjoiner_mode_found(data->drm_fd, output->config.connector, max_dotclock, &mode),
							  "No big joiner mode found on output %s\n", output->name);
				igt_output_override_mode(output, &mode);
			} else {
				mode = *igt_output_get_mode(output);
			}

			igt_output_set_pipe(output, data->pipe_seq[i + j]);
			igt_info("Assigning pipe %s to %s with mode %dx%d@%d%s",
				 kmstest_pipe_name(data->pipe_seq[i + j]),
				 igt_output_name(output), mode.hdisplay,
				 mode.vdisplay, mode.vrefresh,
				 j == INVALID_TEST_OUTPUT - 1 ? "\n" : ", ");
			primary[j] = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
			igt_create_pattern_fb(data->drm_fd, mode.hdisplay, mode.vdisplay,
					      DRM_FORMAT_XRGB8888,
					      DRM_FORMAT_MOD_LINEAR, &fb[j]);
			igt_plane_set_fb(primary[j], &fb[j]);
		}
		ret = igt_display_try_commit2(&data->display, COMMIT_ATOMIC);
		igt_display_reset(&data->display);
		for (j = 0; j < INVALID_TEST_OUTPUT; j++) {
			igt_plane_set_fb(primary[j], NULL);
			igt_remove_fb(data->drm_fd, &fb[j]);
		}
		igt_assert_f(ret != 0, "Commit shouldn't have passed\n");
	}
}

static void test_joiner_on_last_pipe(data_t *data, bool force_joiner)
{
	int i, len, ret;
	igt_output_t **outputs;
	igt_output_t *output;
	igt_plane_t *primary;
	igt_fb_t fb;
	drmModeModeInfo mode;

	len = force_joiner ? data->non_big_joiner_output_count : data->big_joiner_output_count;
	outputs = force_joiner ? data->non_big_joiner_output : data->big_joiner_output;

	for (i = 0; i < len; i++) {
		igt_display_reset(&data->display);
		igt_display_commit2(&data->display, COMMIT_ATOMIC);
		output = outputs[i];

		if (!force_joiner) {
			igt_require_f(bigjoiner_mode_found(data->drm_fd, output->config.connector, max_dotclock, &mode),
						  "No big joiner mode found on output %s\n", output->name);
			igt_output_override_mode(output, &mode);
		} else {
			mode = *igt_output_get_mode(output);
		}

		igt_output_set_pipe(output, data->pipe_seq[data->n_pipes - 1]);
		igt_info(" Assigning pipe %s to %s with mode %dx%d@%d\n",
				 kmstest_pipe_name(data->pipe_seq[data->n_pipes - 1]),
				 igt_output_name(output), mode.hdisplay,
				 mode.vdisplay, mode.vrefresh);
		primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
		igt_create_pattern_fb(data->drm_fd, mode.hdisplay, mode.vdisplay,
				      DRM_FORMAT_XRGB8888,
				      DRM_FORMAT_MOD_LINEAR, &fb);
		igt_plane_set_fb(primary, &fb);
		ret = igt_display_try_commit2(&data->display, COMMIT_ATOMIC);
		igt_plane_set_fb(primary, NULL);
		igt_remove_fb(data->drm_fd, &fb);
		igt_assert_f(ret != 0, "Commit shouldn't have passed\n");
	}
}

static void test_ultra_joiner(data_t *data, bool invalid_pipe, bool two_display, bool force_joiner)
{
	int i, j, k, ret, count;
	igt_output_t *output, *non_ultra_joiner_output;
	igt_plane_t *primary;
	igt_output_t **outputs;
	igt_fb_t fb;
	drmModeModeInfo mode;

	if (force_joiner) {
		outputs = data->non_ultra_joiner_output;
		count = data->non_ultra_joiner_output_count;
	} else {
		outputs = data->ultra_joiner_output;
		count = data->ultra_joiner_output_count;
	}

	igt_display_reset(&data->display);
	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	for (i = 0; i < count; i++) {
		output = outputs[i];

		if (!force_joiner) {
			igt_require(ultrajoiner_mode_found(data->drm_fd, output->config.connector, max_dotclock, &mode));
			igt_output_override_mode(output, &mode);
		} else {
			mode = *igt_output_get_mode(output);
		}

		for (j = 0; j < data->n_pipes; j++) {
			/* Ultra joiner is only valid on PIPE_A */
			if (invalid_pipe && j == PIPE_A)
				continue;
			if (!invalid_pipe && j != PIPE_A)
				continue;
			if (two_display && j != PIPE_A)
				continue;

			igt_output_set_pipe(output, data->pipe_seq[j]);

			primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
			igt_create_pattern_fb(data->drm_fd, mode.hdisplay, mode.vdisplay, DRM_FORMAT_XRGB8888,
					      DRM_FORMAT_MOD_LINEAR, &fb);
			igt_plane_set_fb(primary, &fb);

			if (invalid_pipe)
				ret = igt_display_try_commit2(&data->display, COMMIT_ATOMIC);
			else
				igt_display_commit2(&data->display, COMMIT_ATOMIC);

			if (two_display) {
				for_each_connected_output(&data->display, non_ultra_joiner_output) {
					if (output->id != non_ultra_joiner_output->id) {
						for (k = 1; k < data->n_pipes; k++) {
							igt_plane_t *plane;
							drmModeModeInfo *mode1;

							mode1 = igt_output_get_mode(non_ultra_joiner_output);

							igt_output_set_pipe(non_ultra_joiner_output, data->pipe_seq[k]);
							plane = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);

							igt_plane_set_fb(plane, &fb);
							igt_fb_set_size(&fb, plane, mode1->hdisplay, mode1->vdisplay);
							igt_plane_set_size(plane, mode1->hdisplay, mode1->vdisplay);

							ret = igt_display_try_commit2(&data->display, COMMIT_ATOMIC);

							igt_plane_set_fb(plane, NULL);
							igt_assert_f(ret != 0, "Commit expected to fail on second display\n");
						}
						/* Validation with one output is sufficient */
						break;
					}
				}
			}

			igt_display_reset(&data->display);
			igt_plane_set_fb(primary, NULL);
			igt_remove_fb(data->drm_fd, &fb);

			if (invalid_pipe)
				igt_assert_f(ret != 0, "Commit shouldn't have passed\n");
		}
	}
}

static void test_basic_max_non_joiner(data_t *data)
{
	int count;
	enum pipe pipe;
	igt_output_t **outputs, *output;
	igt_fb_t fb;
	igt_plane_t *primary;
	drmModeModeInfo mode;

	count = data->non_joiner_output_count;
	outputs = data->non_joiner_output;
	igt_display_reset(&data->display);

	for (int i = 0; i < count; i++) {
		output = outputs[i];

		for (pipe = 0; pipe < data->n_pipes; pipe++) {
			igt_dynamic_f("pipe-%s-%s", kmstest_pipe_name(pipe), output->name) {
				igt_output_set_pipe(output, pipe);
				igt_require(max_non_joiner_mode_found(data->drm_fd,
								      output->config.connector,
								      max_dotclock, &mode));
				igt_output_override_mode(output, &mode);
				igt_info("Appplying mode = %dx%d@%d\n", mode.hdisplay,
					 mode.vdisplay, mode.vrefresh);
				primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
				igt_create_pattern_fb(data->drm_fd, mode.hdisplay, mode.vdisplay,
						      DRM_FORMAT_XRGB8888,
						      DRM_FORMAT_MOD_LINEAR, &fb);
				igt_plane_set_fb(primary, &fb);
				igt_display_commit2(&data->display, COMMIT_ATOMIC);
				igt_assert_f(!igt_is_joiner_enabled_for_pipe(data->drm_fd, pipe),
					     "Joiner enabled on pipe %c", 'A' + pipe);

				igt_display_reset(&data->display);
				igt_plane_set_fb(primary, NULL);
				igt_remove_fb(data->drm_fd, &fb);
			}
		}
	}
}

int igt_main()
{
	bool ultra_joiner_supported, is_dgfx;
	int i, j, display_ver;
	igt_output_t *output;
	drmModeModeInfo mode;
	data_t data;

	igt_fixture() {
		data.ultra_joiner_supported = false;
		data.big_joiner_output_count = 0;
		data.ultra_joiner_output_count = 0;
		data.non_big_joiner_output_count = 0;
		data.non_ultra_joiner_output_count = 0;
		data.non_joiner_output_count = 0;
		data.mixed_output_count = 0;
		data.output_count = 0;
		j = 0;

		data.drm_fd = drm_open_driver_master(DRIVER_INTEL | DRIVER_XE);
		kmstest_set_vt_graphics_mode();
		igt_display_require(&data.display, data.drm_fd);
		igt_set_all_master_pipes_for_platform(&data.display, &data.master_pipes);
		igt_require(data.display.is_atomic);
		max_dotclock = igt_get_max_dotclock(data.drm_fd);

		is_dgfx = is_xe_device(data.drm_fd) ? xe_has_vram(data.drm_fd) : gem_has_lmem(data.drm_fd);
		display_ver = intel_display_ver(intel_get_drm_devid(data.drm_fd));
		if (is_dgfx && display_ver == 14)
			data.ultra_joiner_supported = true;

		for_each_connected_output(&data.display, output) {
			bool ultrajoiner_found = false, bigjoiner_found = false, force_joiner_supported = false;
			bool non_joiner_found = false;
			drmModeConnector *connector = output->config.connector;

			/*
			 * Bigjoiner will come in to the picture when the
			 * resolution > 5K or clock > max-dot-clock.
			 * Ultrajoiner will come in to the picture when the
			 * resolution > 10K or clock > 2 * max-dot-clock.
			 */
			bigjoiner_found = bigjoiner_mode_found(data.drm_fd, connector, max_dotclock, &mode);
			ultrajoiner_found = ultrajoiner_mode_found(data.drm_fd, connector, max_dotclock, &mode);
			non_joiner_found = max_non_joiner_mode_found(data.drm_fd, connector,
								     max_dotclock, &mode);

			if (igt_has_force_joiner_debugfs(data.drm_fd, output->name))
				force_joiner_supported = true;

			if (ultrajoiner_found)
				data.ultra_joiner_output[data.ultra_joiner_output_count++] = output;
			else if (force_joiner_supported && is_dsc_supported_by_sink(data.drm_fd, output) &&
				 igt_get_dsc_sink_max_slice_count(data.drm_fd, output->name) >= 8)
				data.non_ultra_joiner_output[data.non_ultra_joiner_output_count++] = output;

			if (bigjoiner_found)
				data.big_joiner_output[data.big_joiner_output_count++] = output;
			else if (force_joiner_supported)
				data.non_big_joiner_output[data.non_big_joiner_output_count++] = output;

			if (non_joiner_found)
				data.non_joiner_output[data.non_joiner_output_count++] = output;

			data.output_count++;
		}
		if (data.big_joiner_output_count == 1 && data.non_big_joiner_output_count >= 1) {
			/*
			 * Mixed output consists of 1 bigjoiner output and 1 non bigjoiner output
			 */
			data.mixed_output[data.mixed_output_count++] = data.big_joiner_output[0];
			data.mixed_output[data.mixed_output_count++] = data.non_big_joiner_output[0];
		}

		data.n_pipes = 0;
		for_each_pipe(&data.display, i) {
			data.n_pipes++;
			data.pipe_seq[j] = i;
			j++;
		}
	}


	igt_describe("Verify the basic modeset on big joiner mode on all pipes");
	igt_subtest_with_dynamic("basic-big-joiner") {
		require_big_joiner(&data, FORCE_JOINER_DISABLE);
		igt_dynamic_f("single-joiner")
			test_single_joiner(&data, data.big_joiner_output_count, false);
		if (data.big_joiner_output_count > 1)
			igt_dynamic_f("multi-joiner")
				test_multi_joiner(&data, data.big_joiner_output_count, false);
	}

	igt_describe("Verify the basic modeset on ultra joiner mode on all pipes");
	igt_subtest_with_dynamic("basic-ultra-joiner") {
		require_ultra_joiner(&data, FORCE_JOINER_DISABLE);
		igt_dynamic_f("single-joiner")
			test_ultra_joiner(&data, false, false, false);
	}

	igt_describe("Verify if the modeset on the adjoining pipe is rejected "
		     "when the pipe is active with a big joiner modeset");
	igt_subtest_with_dynamic("invalid-modeset-big-joiner") {
		require_big_joiner(&data, FORCE_JOINER_DISABLE);
		if (data.big_joiner_output_count >= 1)
			igt_dynamic_f("big_joiner_on_last_pipe")
				test_joiner_on_last_pipe(&data, false);
		if (data.big_joiner_output_count > 1)
			igt_dynamic_f("invalid_combinations")
				test_invalid_modeset_two_joiner(&data, false, false);
		if (data.mixed_output_count)
			igt_dynamic_f("mixed_output")
				test_invalid_modeset_two_joiner(&data, true, false);
	}

	igt_describe("Verify if the modeset on the other pipes are rejected "
		     "when the pipe A is active with a ultra joiner modeset");
	igt_subtest_with_dynamic("invalid-modeset-ultra-joiner") {
		require_ultra_joiner(&data, FORCE_JOINER_DISABLE);
		igt_dynamic_f("ultra_joiner_on_invalid_pipe")
			test_ultra_joiner(&data, true, false, false);
		if (data.non_ultra_joiner_output_count > 0) {
			igt_dynamic_f("2x")
				test_ultra_joiner(&data, false, true, false);
		}
	}

	igt_describe("Verify the basic modeset on force big joiner mode on all pipes");
	igt_subtest_with_dynamic("basic-force-big-joiner") {
		require_big_joiner(&data, FORCE_JOINER_ENABLE);
		igt_dynamic_f("single") {
			enable_force_joiner_on_all_non_big_joiner_outputs(&data);
			test_single_joiner(&data, data.non_big_joiner_output_count, true);
			igt_reset_connectors();
		}

		if (data.non_big_joiner_output_count > 1) {
			igt_dynamic_f("multi") {
				enable_force_joiner_on_all_non_big_joiner_outputs(&data);
				test_multi_joiner(&data, data.non_big_joiner_output_count, true);
				igt_reset_connectors();
			}
		}
	}

	igt_subtest_with_dynamic("invalid-modeset-force-big-joiner") {
		require_big_joiner(&data, FORCE_JOINER_ENABLE);
		if (data.non_big_joiner_output_count >= 1) {
			igt_dynamic_f("big_joiner_on_last_pipe") {
				enable_force_joiner_on_all_non_big_joiner_outputs(&data);
				test_joiner_on_last_pipe(&data, true);
				igt_reset_connectors();
			}
		}

		if (data.non_big_joiner_output_count > 1) {
			igt_dynamic_f("invalid_combinations") {
				enable_force_joiner_on_all_non_big_joiner_outputs(&data);
				test_invalid_modeset_two_joiner(&data, false, true);
				igt_reset_connectors();
			}
		}
	}

	igt_describe("Verify the basic modeset on force ultra joiner mode on all pipes");
	igt_subtest_with_dynamic("basic-force-ultra-joiner") {
		require_ultra_joiner(&data, FORCE_JOINER_ENABLE);
		igt_dynamic_f("single") {
			enable_force_joiner_on_all_non_ultra_joiner_outputs(&data);
			test_ultra_joiner(&data, false, false, true);
			igt_reset_connectors();
		}
	}

	igt_describe("Verify modeset switch between ultra joiner and big joiner");
	igt_subtest_with_dynamic("switch-modeset-ultra-joiner-big-joiner") {
		igt_require_f(ultra_joiner_supported,
			      "Ultra joiner not supported on this platform\n");
		igt_require_f(data.ultra_joiner_output_count > 0 ||
			      data.non_ultra_joiner_output_count > 0,
			      "No ultra joiner or force ultra joiner output found\n");
		igt_require_f(data.n_pipes > 3,
			      "Minimum 4 pipes required\n");

		for_each_connected_output(&data.display, output)
			switch_modeset_ultra_joiner_big_joiner(&data, output);
	}

	igt_subtest_with_dynamic("invalid-modeset-force-ultra-joiner") {
		require_ultra_joiner(&data, FORCE_JOINER_ENABLE);
		igt_dynamic_f("ultra_joiner_on_invalid_pipe") {
			enable_force_joiner_on_all_non_ultra_joiner_outputs(&data);
			test_ultra_joiner(&data, true, false, true);
			igt_reset_connectors();
		}

		if (data.non_ultra_joiner_output_count >= 1 && data.output_count > 1) {
			igt_dynamic_f("2x") {
				enable_force_joiner_on_all_non_ultra_joiner_outputs(&data);
				test_ultra_joiner(&data, false, true, true);
				igt_reset_connectors();
			}
		}
	}

	igt_describe("Verify the basic modeset on the maximum non-joiner mode across "
		     "all pipes");
	igt_subtest_with_dynamic("basic-max-non-joiner") {
		igt_require_f(data.n_pipes >= 1,
			      "At least one pipe is required.\n");
		igt_require_f(data.non_joiner_output_count  > 0,
			      "No suitable non-joiner mode found\n");
			test_basic_max_non_joiner(&data);
	}

	igt_fixture() {
		igt_display_fini(&data.display);
		drm_close_driver(data.drm_fd);
		igt_reset_connectors();
	}
}
