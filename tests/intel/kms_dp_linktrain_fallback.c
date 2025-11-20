/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2024 Intel Corporation
 */

/**
 * TEST: kms dp linktrain fallback
 * Category: Display
 * Description: Test link training fallback for eDP/DP connectors
 * Driver requirement: i915, xe
 * Mega feature: General Display Features
 */

#include <sys/types.h>
#include "igt_sysfs.h"
#include "igt.h"
#include "kms_mst_helper.h"
#include "kms_dsc_helper.h"

/**
 * SUBTEST: dp-fallback
 * Description: Test fallback on DP connectors
 *
 * SUBTEST: dsc-fallback
 * Description: Test fallback to DSC when BW isn't sufficient
 */

#define RETRAIN_COUNT 1
/*
 * Two consecutives link training failures
 * reduces link params (link rate, lane count)
 */
#define LT_FAILURE_REDUCED_CAPS 2
#define SPURIOUS_HPD_RETRY 3

static int traversed_mst_outputs[IGT_MAX_PIPES];
static int traversed_mst_output_count;
typedef struct {
	int drm_fd;
	igt_display_t display;
	drmModeModeInfo *mode;
	igt_output_t *output;
	enum pipe pipe;
	struct igt_fb fb;
	struct igt_plane *primary;
	int n_pipes;
} data_t;

typedef int (*condition_check_fn)(int drm_fd, igt_output_t *output);

IGT_TEST_DESCRIPTION("Test link-training / dsc fallback");

static bool setup_mst_outputs(data_t *data, igt_output_t *mst_output[],
			      int *output_count)
{
	int i;
	igt_output_t *output;

	/*
	 * Check if this is already traversed
	 */
	for (i = 0; i < traversed_mst_output_count; i++)
		if (i < IGT_MAX_PIPES &&
		    traversed_mst_outputs[i] == data->output->config.connector->connector_id)
			return false;

	igt_assert_f(igt_find_all_mst_output_in_topology(data->drm_fd, &data->display,
							 data->output, mst_output,
							 output_count) == 0,
							 "Unable to find mst outputs or given output is not mst\n");

	for (i = 0; i < *output_count; i++) {
		output = mst_output[i];
		if (traversed_mst_output_count < IGT_MAX_PIPES) {
			traversed_mst_outputs[traversed_mst_output_count++] = output->config.connector->connector_id;
			igt_info("Output %s is in same topology as %s\n",
				 igt_output_name(output),
				 igt_output_name(data->output));
		} else {
			igt_assert_f(false, "Unable to save traversed output\n");
			return false;
		}
	}
	return true;
}

static void setup_pipe_on_outputs(data_t *data,
				      igt_output_t *outputs[],
				      int *output_count)
{
	int i = 0;

	igt_require_f(data->n_pipes >= *output_count,
		      "Need %d pipes to assign to %d outputs\n",
		      data->n_pipes, *output_count);

	for_each_pipe(&data->display, data->pipe) {
		if (i >= *output_count)
			break;
		/*
		 * TODO: add support for modes requiring joined pipes
		 */
		igt_info("Setting pipe %s on output %s\n",
			 kmstest_pipe_name(data->pipe),
			 igt_output_name(outputs[i]));
		igt_output_set_pipe(outputs[i++], data->pipe);
	}
}

static void setup_modeset_on_outputs(data_t *data,
				     igt_output_t *outputs[],
				     int *output_count,
				     drmModeModeInfo *mode[],
				     struct igt_fb fb[],
				     struct igt_plane *primary[])
{
	int i;

	for (i = 0; i < *output_count; i++) {
		mode[i] = igt_output_get_mode(outputs[i]);
		igt_info("Mode %dx%d@%d on output %s\n",
			 mode[i]->hdisplay, mode[i]->vdisplay,
			 mode[i]->vrefresh,
			 igt_output_name(outputs[i]));
		primary[i] = igt_output_get_plane_type(outputs[i],
						       DRM_PLANE_TYPE_PRIMARY);
		igt_create_color_fb(data->drm_fd,
				    mode[i]->hdisplay,
				    mode[i]->vdisplay,
				    DRM_FORMAT_XRGB8888,
				    DRM_FORMAT_MOD_LINEAR, 0.0, 1.0, 0.0,
				    &fb[i]);
		igt_plane_set_fb(primary[i], &fb[i]);
	}
}

static void set_connector_link_status_good(data_t *data, igt_output_t *outputs[],
					   int *output_count)
{
	int i;
	igt_output_t *output;

        /*
         * update the link status to good for all outputs
         */
        for_each_connected_output(&data->display, output)
		for(i = 0; i < *output_count; i++)
			if (output->id == outputs[i]->id)
				igt_output_set_prop_value(output,
							  IGT_CONNECTOR_LINK_STATUS,
							  DRM_MODE_LINK_STATUS_GOOD);
}

static bool validate_modeset_for_outputs(data_t *data,
					igt_output_t *outputs[],
					int *output_count,
					drmModeModeInfo *mode[],
					struct igt_fb fb[],
					struct igt_plane *primary[])
{
	igt_require_f(*output_count > 0, "Require at least 1 output\n");
	setup_pipe_on_outputs(data, outputs, output_count);
	igt_assert_f(igt_fit_modes_in_bw(&data->display), "Unable to fit modes in bw\n");
	setup_modeset_on_outputs(data, outputs,
				 output_count,
				 mode, fb, primary);
	return true;
}

static bool setup_outputs(data_t *data, bool is_mst,
		      igt_output_t *outputs[],
		      int *output_count, drmModeModeInfo *mode[],
		      struct igt_fb fb[], struct igt_plane *primary[])
{
	bool ret;

	*output_count = 0;

	if (is_mst) {
		ret = setup_mst_outputs(data, outputs, output_count);
		if (!ret) {
			igt_info("Skipping MST output %s as already tested\n",
				 igt_output_name(data->output));
			return false;
		}
	} else
		if ((*output_count) < IGT_MAX_PIPES)
			outputs[(*output_count)++] = data->output;

	ret = validate_modeset_for_outputs(data, outputs,
					   output_count, mode,
					   fb, primary);

	if (!ret) {
		igt_info("Skipping output %s as valid pipe/output combo not found\n",
			 igt_output_name(data->output));
		return false;
	}

	igt_display_commit2(&data->display, COMMIT_ATOMIC);
	return true;
}

static int check_condition_with_timeout(int drm_fd, igt_output_t *output,
					condition_check_fn check_fn,
					double interval, double timeout)
{
	struct timespec start_time, current_time;
	double elapsed_time;

	clock_gettime(CLOCK_MONOTONIC, &start_time);

	while (1) {
		if (check_fn(drm_fd, output) == 0)
			return 0;

		clock_gettime(CLOCK_MONOTONIC, &current_time);
		elapsed_time = (current_time.tv_sec - start_time.tv_sec) +
			(current_time.tv_nsec - start_time.tv_nsec) / 1e9;

		if (elapsed_time >= timeout)
			return -1;

		usleep((useconds_t)(interval * 1000000));
	}
}

/*
 * Force a link training failure followed by link retrain, then
 * block until the driver has no further pending retrain/failure.
 * Returns false if we time out waiting.
 */
static bool force_failure_and_wait(data_t *data,
				   igt_output_t *output,
				   int failure_type,
				   int retrain_count,
				   double interval,
				   double timeout)
{
	igt_force_lt_failure(data->drm_fd, output, failure_type);
	igt_force_link_retrain(data->drm_fd, output, retrain_count);

	/* Wait until there's no pending retrain */
	if (check_condition_with_timeout(data->drm_fd, output,
					 igt_get_dp_pending_retrain,
					 interval, timeout)) {
		igt_info("Timed out waiting for pending retrain\n");
		return false;
	}

	/* Wait until there's no pending LT failures */
	if (check_condition_with_timeout(data->drm_fd, output,
					 igt_get_dp_pending_lt_failures,
					 interval, timeout)) {
		igt_info("Timed out waiting for pending LT failures\n");
		return false;
	}

	return true;
}

/*
 * Waits for a hotplug event, then checks that the link-status is BAD.
 * Returns false if the link-status isn't BAD or no hotplug arrives in time.
 */
static bool wait_for_hotplug_and_check_bad(int drm_fd,
					   data_t *data,
					   igt_output_t *output,
					   struct udev_monitor *mon,
					   double hotplug_timeout)
{
	uint32_t link_status_prop_id;
	uint64_t link_status_value;
	drmModePropertyPtr link_status_prop;

	if (!igt_hotplug_detected(mon, hotplug_timeout)) {
		igt_info("No hotplug event within %.2f seconds.\n", hotplug_timeout);
		return false;
	}

	kmstest_get_property(drm_fd,
			     output->config.connector->connector_id,
			     DRM_MODE_OBJECT_CONNECTOR,
			     "link-status",
			     &link_status_prop_id, &link_status_value,
			     &link_status_prop);

	if (link_status_value != DRM_MODE_LINK_STATUS_BAD) {
		igt_info("Expected link-status=BAD but got %" PRIu64 "\n",
			 link_status_value);
		return false;
	}

	return true;
}

/*
 * Sets link status=GOOD for the specified outputs, then calls
 * validate_modeset_for_outputs() to re-commit. Returns false
 * if the re-commit fails.
 */
static bool fix_link_status_and_recommit(data_t *data,
					 igt_output_t *outputs[],
					 int *output_count,
					 drmModeModeInfo * modes[],
					 struct igt_fb fbs[],
					 struct igt_plane *primaries[])
{
	int i;
	igt_output_t *out;

	/* Set link-status=GOOD on each tested output */
	for_each_connected_output(&data->display, out) {
		for (i = 0; i < *output_count; i++) {
			if (out->id == outputs[i]->id) {
				igt_output_set_prop_value(
					out, IGT_CONNECTOR_LINK_STATUS,
					DRM_MODE_LINK_STATUS_GOOD);
			}
		}
	}

	if (!validate_modeset_for_outputs(data, outputs, output_count,
					  modes, fbs, primaries)) {
		igt_info("Modeset validation failed after forcing link-status=GOOD\n");
		return false;
	}

	if (igt_display_try_commit_atomic(&data->display,
					  DRM_MODE_ATOMIC_ALLOW_MODESET,
					  NULL) != 0) {
		igt_info("Commit failed after restoring link-status=GOOD\n");
		return false;
	}

	return true;
}

static void test_fallback(data_t *data, bool is_mst)
{
	int output_count, retries;
	int max_link_rate, curr_link_rate, prev_link_rate;
	int max_lane_count, curr_lane_count, prev_lane_count;
	igt_output_t *outputs[IGT_MAX_PIPES];
	drmModeModeInfo * modes[IGT_MAX_PIPES];
	struct igt_fb fbs[IGT_MAX_PIPES];
	struct igt_plane *primaries[IGT_MAX_PIPES];
	struct udev_monitor *mon;

	retries = SPURIOUS_HPD_RETRY;

	igt_display_reset(&data->display);
	igt_reset_link_params(data->drm_fd, data->output);
	if (!setup_outputs(data, is_mst, outputs,
			   &output_count, modes, fbs,
			   primaries))
		return;

	igt_info("Testing link training fallback on %s\n",
		 igt_output_name(data->output));
	max_link_rate = igt_get_max_link_rate(data->drm_fd, data->output);
	max_lane_count = igt_get_max_lane_count(data->drm_fd, data->output);
	prev_link_rate = igt_get_current_link_rate(data->drm_fd, data->output);
	prev_lane_count = igt_get_current_lane_count(data->drm_fd, data->output);

	while (!igt_get_dp_link_retrain_disabled(data->drm_fd,
						 data->output)) {
		igt_info("Current link rate: %d, Current lane count: %d\n",
			 prev_link_rate,
			 prev_lane_count);
		mon = igt_watch_uevents();

		igt_assert_f(force_failure_and_wait(data, data->output,
						    LT_FAILURE_REDUCED_CAPS,
						    RETRAIN_COUNT,
						    1.0, 20.0),
						    "Link training failure steps timed out\n");

		if (igt_get_dp_link_retrain_disabled(data->drm_fd,
						     data->output)) {
			igt_reset_connectors();
			return;
		}

		igt_assert_f(wait_for_hotplug_and_check_bad(data->drm_fd,
							    data,
							    data->output,
							    mon,
							    20.0), "Didn't get hotplug or link status != BAD\n");

		igt_flush_uevents(mon);
		set_connector_link_status_good(data, outputs, &output_count);
		igt_assert_f(fix_link_status_and_recommit(data,
							  outputs,
							  &output_count,
							  modes,
							  fbs,
							  primaries), "modeset failed\n");
		igt_assert_eq(data->output->values[IGT_CONNECTOR_LINK_STATUS], DRM_MODE_LINK_STATUS_GOOD);
		curr_link_rate = igt_get_current_link_rate(data->drm_fd, data->output);
		curr_lane_count = igt_get_current_lane_count(data->drm_fd, data->output);

		igt_debug("Fallback state: prev %dx%d, curr %dx%d, max %dx%d, retries=%u\n",
			  prev_link_rate, prev_lane_count,
			  curr_link_rate, curr_lane_count,
			  max_link_rate,  max_lane_count,
			  retries);
		igt_assert_f((curr_link_rate < prev_link_rate ||
			     curr_lane_count < prev_lane_count) ||
			     ((curr_link_rate == max_link_rate && curr_lane_count == max_lane_count) && --retries),
			     "Fallback unsuccessful\n");

		prev_link_rate = curr_link_rate;
		prev_lane_count = curr_lane_count;
	}
}

static bool run_lt_fallback_test(data_t *data)
{
	bool ran = false;
	igt_output_t *output;

	for_each_connected_output(&data->display, output) {
		data->output = output;

		if (!igt_has_force_link_training_failure_debugfs(data->drm_fd,
								 data->output)) {
			igt_info("Output %s doesn't support forcing link training failure\n",
				 igt_output_name(data->output));
			continue;
		}

		if (output->config.connector->connector_type != DRM_MODE_CONNECTOR_DisplayPort) {
			igt_info("Skipping output %s as it's not DP\n", output->name);
				continue;
		}

		ran = true;

		/*
		 * Check output is MST
		 */
		if (igt_check_output_is_dp_mst(data->output)) {
			igt_info("Testing MST output %s\n",
				 igt_output_name(data->output));
			test_fallback(data, true);
		} else {
			igt_info("Testing DP output %s\n",
				 igt_output_name(data->output));
			test_fallback(data, false);
		}
	}
	return ran;
}

static void test_dsc_sst_fallback(data_t *data)
{
	bool non_dsc_mode_found = false;
	bool dsc_fallback_successful = false;
	int ret;
	struct udev_monitor *mon;
	drmModeModeInfo *mode_to_check;
	igt_output_t *outputs[IGT_MAX_PIPES];
	int output_count = 0;

	igt_info("Checking DSC fallback on %s\n", igt_output_name(data->output));
	data->pipe = PIPE_A;

	igt_display_reset(&data->display);
	igt_reset_link_params(data->drm_fd, data->output);
	igt_force_link_retrain(data->drm_fd, data->output, RETRAIN_COUNT);

	/* Find a mode that doesn't require DSC initially */
	for_each_connector_mode(data->output) {
		data->mode = &data->output->config.connector->modes[j__];
		igt_create_color_fb(data->drm_fd, data->mode->hdisplay,
				    data->mode->vdisplay, DRM_FORMAT_XRGB8888,
				    DRM_FORMAT_MOD_LINEAR, 0.0, 1.0, 0.0,
				    &data->fb);
		igt_output_override_mode(data->output, data->mode);
		igt_output_set_pipe(data->output, data->pipe);
		data->primary = igt_output_get_plane_type(data->output,
						DRM_PLANE_TYPE_PRIMARY);
		igt_plane_set_fb(data->primary, &data->fb);

		ret = igt_display_try_commit_atomic(&data->display,
						    DRM_MODE_ATOMIC_TEST_ONLY |
						    DRM_MODE_ATOMIC_ALLOW_MODESET,
						    NULL);
		if (ret != 0) {
			igt_debug("Skipping mode %dx%d@%d on %s\n",
				 data->mode->hdisplay, data->mode->vdisplay,
				 data->mode->vrefresh,
				 igt_output_name(data->output));
			continue;
		}
		igt_display_commit2(&data->display, COMMIT_ATOMIC);

		if (!igt_is_dsc_enabled(data->drm_fd,
					data->output->name)) {
			drmModeModeInfo *non_dsc_mode
				= igt_output_get_mode(data->output);
			igt_info("Found mode %dx%d@%d %s that doesn't need DSC with link rate %d and lane count %d\n",
				 non_dsc_mode->hdisplay, non_dsc_mode->vdisplay,
				 non_dsc_mode->vrefresh, non_dsc_mode->name,
				 igt_get_current_link_rate(data->drm_fd, data->output),
				 igt_get_current_lane_count(data->drm_fd, data->output));
			non_dsc_mode_found = true;
			break;
		}
	}
	igt_require_f(non_dsc_mode_found,
		      "No non-DSC mode found on %s\n",
		      igt_output_name(data->output));

	/* Repeatedly force link failure until DSC is required (or link is disabled) */
	while (!igt_get_dp_link_retrain_disabled(data->drm_fd, data->output)) {
		mon = igt_watch_uevents();

		igt_assert_f(force_failure_and_wait(data, data->output,
						    LT_FAILURE_REDUCED_CAPS,
						    RETRAIN_COUNT, 1.0, 20.0),
			     "Forcing DSC fallback timed out\n");

		if (igt_get_dp_link_retrain_disabled(data->drm_fd,
						     data->output)) {
			igt_reset_connectors();
			igt_flush_uevents(mon);
			return;
		}

		igt_assert_f(wait_for_hotplug_and_check_bad(data->drm_fd,
							    data,
							    data->output,
							    mon,
							    20.0),
			     "Didn't get hotplug or link-status=BAD for DSC\n");
		igt_flush_uevents(mon);

		outputs[output_count++] = data->output;
		set_connector_link_status_good(data, outputs, &output_count);
		igt_display_commit2(&data->display, COMMIT_ATOMIC);

		mode_to_check = igt_output_get_mode(data->output);

		if (igt_is_dsc_enabled(data->drm_fd, data->output->name)) {
			igt_info("mode %dx%d@%d now requires DSC with link rate %d and lane count %d\n",
				 mode_to_check->hdisplay, mode_to_check->vdisplay,
				 mode_to_check->vrefresh,
				 igt_get_current_link_rate(data->drm_fd, data->output),
				 igt_get_current_lane_count(data->drm_fd, data->output));
			igt_info("DSC fallback successful on %s\n",
				 igt_output_name(data->output));
			dsc_fallback_successful = true;
			break;
		} else {
			igt_info("mode %dx%d@%d still doesn't require DSC\n",
				 mode_to_check->hdisplay, mode_to_check->vdisplay,
				 mode_to_check->vrefresh);
		}
	}
	igt_assert_f(dsc_fallback_successful, "DSC fallback unsuccessful\n");
}

static bool run_dsc_sst_fallaback_test(data_t *data)
{
	bool ran = false;
	igt_output_t *output;

	if (!is_dsc_supported_by_source(data->drm_fd)) {
		igt_info("DSC not supported by source.\n");
		return ran;
	}

	for_each_connected_output(&data->display, output) {
		data->output = output;

		if (!igt_has_force_link_training_failure_debugfs(data->drm_fd,
								 data->output)) {
			igt_info("Output %s doesn't support forcing link training.\n",
				 igt_output_name(data->output));
			continue;
		}

		if (output->config.connector->connector_type != DRM_MODE_CONNECTOR_DisplayPort) {
			igt_info("Skipping output %s as it's not DP\n", output->name);
			continue;
		}

		if (!is_dsc_supported_by_sink(data->drm_fd, data->output))
			continue;

		ran = true;
		test_dsc_sst_fallback(data);
	}

	return ran;
}

int igt_main()
{
	data_t data = {};

	igt_fixture() {
		unsigned int debug_mask_if_ci = DRM_UT_KMS;
		data.drm_fd = drm_open_driver_master(DRIVER_INTEL |
						     DRIVER_XE);
		kmstest_set_vt_graphics_mode();
		igt_display_require(&data.display, data.drm_fd);
		igt_display_require_output(&data.display);
		for_each_pipe(&data.display, data.pipe)
			data.n_pipes++;
		igt_install_exit_handler(igt_drm_debug_mask_reset_exit_handler);
		update_debug_mask_if_ci(debug_mask_if_ci);

		/*
		 * Some environments may have environment
		 * variable set to ignore long hpd, disable it for this test
		 */
		igt_assert_f(igt_ignore_long_hpd(data.drm_fd, false),
			     "Unable to disable ignore long hpd\n");
	}

	igt_subtest("dp-fallback") {
		igt_require_f(run_lt_fallback_test(&data),
			      "Skipping test as no output found or none supports fallback\n");
	}

	igt_subtest("dsc-fallback") {
		igt_require_f(run_dsc_sst_fallaback_test(&data),
			      "Skipping test as DSC fallback conditions not met.\n");
	}

	igt_fixture() {
		igt_remove_fb(data.drm_fd, &data.fb);
		igt_display_fini(&data.display);
		close(data.drm_fd);
	}
}
