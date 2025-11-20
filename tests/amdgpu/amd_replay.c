// SPDX-License-Identifier: MIT
/*
 * Copyright 2024 Advanced Micro Devices, Inc.
 */

#include <dirent.h>
#include <fcntl.h>

#include "igt_amd.h"

/* hardware requirements:
 * eDP panel that supports Panel Replay
 */
IGT_TEST_DESCRIPTION("Basic test for enabling Panel Replay for eDP displays");

#define REPLAY_SETTLE_DELAY 10
#define FLIP_FRAME_BEFORE_TEST 60

/* Common test data. */
struct test_data {
	igt_display_t display;
	igt_plane_t *primary;
	igt_output_t *output;
	igt_pipe_t *pipe;
	drmModeModeInfo *mode;
	igt_fb_t ref_fb;
	igt_fb_t ref_fb2;
	igt_fb_t *flip_fb;
	enum pipe pipe_id;
	int fd;
	int debugfs_fd;
	int w, h;
};

struct {
	bool visual_confirm;
} opt = {
	.visual_confirm = false,	/* visual confirm debug option */
};

const char *help_str =
"  --visual-confirm           Panel Replay visual confirm debug option enable\n";

struct option long_options[] = {
	{"visual-confirm",	required_argument, NULL, 'v'},
	{ 0, 0, 0, 0 }
};

enum test_mode {
	TEST_MODE_STATIC_SCREEN = 0,
	TEST_MODE_INTERMITTENT_LIVE,
	TEST_MODE_CONSTANT_LIVE,
	TEST_MODE_SUSPEND,
	TEST_MODE_FLIP_ONLY,
	TEST_MODE_COUNT
};

/* Common test setup. */
static void test_init(struct test_data *data)
{
	igt_display_t *display = &data->display;

	/* It doesn't matter which pipe we choose on amdpgu. */
	data->pipe_id = PIPE_A;
	data->pipe = &data->display.pipes[data->pipe_id];

	igt_display_reset(display);

	data->output = igt_get_single_output_for_pipe(display, data->pipe_id);
	igt_require(data->output);
	igt_info("output %s\n", data->output->name);

	data->mode = igt_output_get_mode(data->output);
	igt_assert(data->mode);
	kmstest_dump_mode(data->mode);

	data->primary =
		 igt_pipe_get_plane_type(data->pipe, DRM_PLANE_TYPE_PRIMARY);

	igt_output_set_pipe(data->output, data->pipe_id);

	data->w = data->mode->hdisplay;
	data->h = data->mode->vdisplay;

	data->ref_fb.fb_id = 0;
	data->ref_fb2.fb_id = 0;

	if (opt.visual_confirm) {
		/**
		 * if visual confirm option is enabled, we'd trigger a full modeset before test run
		 * to have Panel Replay visual confirm enable take effect. DPMS off -> ON transition
		 * is one of many approaches.
		 */
		kmstest_set_connector_dpms(data->fd, data->output->config.connector,
			 DRM_MODE_DPMS_OFF);
		kmstest_set_connector_dpms(data->fd, data->output->config.connector,
			 DRM_MODE_DPMS_ON);
	}
}

/* Common test cleanup. */
static void test_fini(struct test_data *data)
{
		igt_display_t *display = &data->display;

		igt_display_reset(display);
		igt_display_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET, 0);
}

static int check_conn_type(struct test_data *data, uint32_t type)
{
	int i;

	for (i = 0; i < data->display.n_outputs; i++) {
		uint32_t conn_type = data->display.outputs[i].config.connector->connector_type;

		if (conn_type == type)
			return i;
	}

	return -1;
}

static bool replay_mode_supported(struct test_data *data)
{
	/* run Panel Replay test if eDP panel support Panel Replay */
	if (!igt_amd_output_has_replay_cap(data->fd, data->output->name)) {
		igt_warn(" driver does not have %s debugfs interface\n", DEBUGFS_EDP_REPLAY_CAP);

		return false;
	}

	if (!igt_amd_output_has_replay_state(data->fd, data->output->name)) {
		igt_warn(" driver does not have %s debugfs interface\n", DEBUGFS_EDP_REPLAY_STATE);

		return false;
	}

	if (!igt_amd_replay_support_sink(data->fd, data->output->name)) {
		igt_warn(" output %s not support Panel Replay mode\n", data->output->name);

		return false;
	}

	if (!igt_amd_replay_support_drv(data->fd, data->output->name)) {
		igt_warn(" kernel driver not support Panel Replay mode\n");

		return false;
	}

	return true;
}

/* Read from /dev/drm_dp_aux
 * addr: DPCD offset
 * val:  Read value of DPCD register
 */
static bool dpcd_read_byte(int drm_fd,
	drmModeConnector *connector, uint32_t addr, uint8_t *val)
{
	DIR *dir;
	int dir_fd;
	uint8_t buf[16] = {0};
	*val = 0;

	dir_fd = igt_connector_sysfs_open(drm_fd, connector);
	igt_assert(dir_fd >= 0);

	dir = fdopendir(dir_fd);
	igt_assert(dir);

	for (;;) {
		struct dirent *ent;
		char path[5 + sizeof(ent->d_name)];
		int fd, ret, i, j, k;

		ent = readdir(dir);
		if (!ent)
			break;

		if (strncmp(ent->d_name, "drm_dp_aux", 10))
			continue;

		snprintf(path, sizeof(path), "/dev/%s", ent->d_name);

		fd = open(path, O_RDONLY);
		igt_assert(fd >= 0);

		k = (addr / 16) + 1;
		j = addr % 16;

		/* read 16 bytes each loop */
		for (i = 0; i < k; i++) {
			ret = read(fd, buf, sizeof(buf));
			if (ret < 0)
				break;
			if (ret != sizeof(buf))
				break;
		}

		close(fd);

		closedir(dir);
		close(dir_fd);

		if (ret > 0)
			*val = buf[j];

		return (ret > 0);
	}

	closedir(dir);
	close(dir_fd);

	igt_skip("Missing /dev/drm_dp_aux*, check DRM_DISPLAY_DP_AUX_CHARDEV in kernel config\n");

	return false;
}

static void page_flip_test(struct test_data *data, igt_output_t *output,
						 enum test_mode test_mode, uint32_t frame_num)
{
	int ret, frame_count;
	enum replay_state replay_state;
	uint8_t panel_dpcd = 0;

	if (!data || data->ref_fb.fb_id == 0 || data->ref_fb2.fb_id == 0
	    || frame_num <= 5) {
		igt_skip("Page flip failed.\n");
	}

	data->flip_fb = &data->ref_fb;

	for (frame_count = 0; frame_count <= frame_num; frame_count++) {
		ret = drmModePageFlip(data->fd, output->config.crtc->crtc_id,
				data->flip_fb->fb_id, DRM_MODE_PAGE_FLIP_EVENT, NULL);
		igt_require(ret == 0);
		kmstest_wait_for_pageflip(data->fd);

		if (test_mode == (TEST_MODE_CONSTANT_LIVE || TEST_MODE_INTERMITTENT_LIVE)
				&& frame_count > 5) {
			/* Panel Replay state needs few frame to enter the live mode */
			replay_state = igt_amd_read_replay_state(data->fd, output->name);
			dpcd_read_byte(data->fd, output->config.connector, 0x378, &panel_dpcd);
			igt_debug("replay_state live mode = 0x%X\n", replay_state);
			igt_fail_on_f(replay_state < REPLAY_STATE_4 && replay_state >= REPLAY_STATE_5,
					"State should be REPLAY_STATE_4 (Active with single frame update)\n");
			igt_fail_on_f(panel_dpcd == 0, "Panel is not in replay mode\n");
		}

		if (frame_count % 2 == 0)
			data->flip_fb = &data->ref_fb2;
		else
			data->flip_fb = &data->ref_fb;
	}
}

static void run_check_replay(struct test_data *data, enum test_mode test_mode)
{
	int edp_idx;
	enum replay_state replay_state;
	igt_output_t *output;
	uint8_t panel_dpcd = 0;

	test_init(data);

	edp_idx = check_conn_type(data, DRM_MODE_CONNECTOR_eDP);
	igt_skip_on_f(edp_idx == -1, "no eDP connector found\n");

	/* check if eDP support Panel Replay. */
	igt_skip_on(!replay_mode_supported(data));

	for_each_connected_output(&data->display, output) {
		if (output->config.connector->connector_type != DRM_MODE_CONNECTOR_eDP)
			continue;

		igt_create_color_fb(data->fd, data->mode->hdisplay,
			 data->mode->vdisplay, DRM_FORMAT_XRGB8888, 0, 0.6, 0.6, 0.6, &data->ref_fb);
		igt_create_color_fb(data->fd, data->mode->hdisplay,
			 data->mode->vdisplay, DRM_FORMAT_XRGB8888, 0, 0.0, 0.4, 0.14, &data->ref_fb2);

		igt_plane_set_fb(data->primary, &data->ref_fb);
		igt_display_commit_atomic(&data->display, DRM_MODE_ATOMIC_ALLOW_MODESET, 0);
		data->flip_fb = &data->ref_fb;
		drmModePageFlip(data->fd, output->config.crtc->crtc_id,
			 data->flip_fb->fb_id, DRM_MODE_PAGE_FLIP_EVENT, NULL);
		kmstest_wait_for_pageflip(data->fd);

		/* Do some page flips and let the replay enable */
		page_flip_test(data, output, TEST_MODE_FLIP_ONLY, FLIP_FRAME_BEFORE_TEST);

		/* Panel Replay state takes some time to settle its value on static screen */
		sleep(REPLAY_SETTLE_DELAY);

		/* Check Panel Replay state */
		replay_state = igt_amd_read_replay_state(data->fd, output->name);
		igt_debug("replay_state static mode before flip = 0x%X\n", replay_state);
		igt_fail_on_f(replay_state < 0, "Open Panel Replay state debugfs failed\n");
		igt_fail_on_f(replay_state < REPLAY_STATE_2,
			 "Panel Replay was not enabled for connector %s\n", output->name);

		/* Do some page flip and let the replay go into live mode */
		page_flip_test(data, output, test_mode, 20);

		/* Check Panel Replay state in static screen */
		if (test_mode == TEST_MODE_STATIC_SCREEN || TEST_MODE_INTERMITTENT_LIVE) {
			/* Panel Replay state takes some time to settle its value on static screen */
			sleep(1);

			replay_state = igt_amd_read_replay_state(data->fd, output->name);
			dpcd_read_byte(data->fd, output->config.connector, 0x378, &panel_dpcd);
			igt_debug("replay_state static mode = 0x%X\n", replay_state);
			igt_fail_on_f(replay_state < REPLAY_STATE_3 && replay_state >= REPLAY_STATE_4,
				 "State should be REPLAY_STATE_3 (Active)\n");
			igt_fail_on_f(panel_dpcd == 0, "Panel is not in replay mode\n");
		}

		/* Do another page flip if we do the replay_intermittent_live test */
		if (test_mode == TEST_MODE_INTERMITTENT_LIVE) {
			page_flip_test(data, output, test_mode, 30);

			/* Panel Replay state takes some time to settle its value on static screen */
			sleep(1);

			replay_state = igt_amd_read_replay_state(data->fd, output->name);
			dpcd_read_byte(data->fd, output->config.connector, 0x378, &panel_dpcd);
			igt_debug("replay_state TEST_MODE_INTERMITTENT_LIVE after flip = 0x%X\n",
				 replay_state);
			igt_fail_on_f(replay_state < REPLAY_STATE_3 && replay_state >= REPLAY_STATE_4,
				 "State should be REPLAY_STATE_3 (Active)\n");
			igt_fail_on_f(panel_dpcd == 0, "Panel is not in replay mode\n");
		}

		igt_remove_fb(data->fd, &data->ref_fb);
		igt_remove_fb(data->fd, &data->ref_fb2);
	}

	test_fini(data);
}

static void run_check_replay_suspend(struct test_data *data)
{
	int edp_idx;
	enum replay_state replay_state;
	igt_output_t *output;
	uint8_t panel_dpcd = 0;

	test_init(data);

	edp_idx = check_conn_type(data, DRM_MODE_CONNECTOR_eDP);
	igt_skip_on_f(edp_idx == -1, "no eDP connector found\n");

	/* check if eDP support Panel Replay. */
	igt_skip_on(!replay_mode_supported(data));

	for_each_connected_output(&data->display, output) {
		if (output->config.connector->connector_type != DRM_MODE_CONNECTOR_eDP)
			continue;

		igt_create_color_fb(data->fd, data->mode->hdisplay,
			 data->mode->vdisplay, DRM_FORMAT_XRGB8888, 0, 0.6, 0.6, 0.6, &data->ref_fb);
		igt_create_color_fb(data->fd, data->mode->hdisplay,
			 data->mode->vdisplay, DRM_FORMAT_XRGB8888, 0, 0.0, 0.4, 0.14, &data->ref_fb2);

		igt_plane_set_fb(data->primary, &data->ref_fb);
		igt_display_commit_atomic(&data->display, DRM_MODE_ATOMIC_ALLOW_MODESET, 0);
		data->flip_fb = &data->ref_fb;
		drmModePageFlip(data->fd, output->config.crtc->crtc_id,
			 data->flip_fb->fb_id, DRM_MODE_PAGE_FLIP_EVENT, NULL);
		kmstest_wait_for_pageflip(data->fd);

		/* Suspend and Resume */
		igt_system_suspend_autoresume(SUSPEND_STATE_MEM, SUSPEND_TEST_NONE);

		/* Do some page flip and let the replay go into live mode */
		page_flip_test(data, output, TEST_MODE_SUSPEND, FLIP_FRAME_BEFORE_TEST);

		/* Panel Replay state takes some time to settle its value on static screen */
		sleep(REPLAY_SETTLE_DELAY);

		replay_state = igt_amd_read_replay_state(data->fd, output->name);
		dpcd_read_byte(data->fd, output->config.connector, 0x378, &panel_dpcd);
		igt_debug("replay_state static mode = 0x%X\n", replay_state);
		igt_fail_on_f(replay_state < REPLAY_STATE_3 && replay_state >= REPLAY_STATE_4,
			 "State should be REPLAY_STATE_3 (Active)\n");
		igt_fail_on_f(panel_dpcd == 0, "Panel is not in replay mode\n");

		igt_remove_fb(data->fd, &data->ref_fb);
		igt_remove_fb(data->fd, &data->ref_fb2);
	}

	test_fini(data);
}

static int opt_handler(int option, int option_index, void *data)
{
	switch (option) {
	case 'v':
		opt.visual_confirm = strtol(optarg, NULL, 0);
		igt_info("Panel Replay Visual Confirm %s\n",
			 opt.visual_confirm ? "enabled" : "disabled");
		break;
	default:
		return IGT_OPT_HANDLER_ERROR;
	}

	return IGT_OPT_HANDLER_SUCCESS;
}

igt_main_args("", long_options, help_str, opt_handler, NULL)
{
	struct test_data data;

	igt_skip_on_simulation();
	memset(&data, 0, sizeof(data));

	igt_fixture()
	{
		data.fd = drm_open_driver_master(DRIVER_AMDGPU);

		if (data.fd == -1)
			igt_skip("Not an amdgpu driver.\n");

		data.debugfs_fd = igt_debugfs_dir(data.fd);

		kmstest_set_vt_graphics_mode();

		igt_display_require(&data.display, data.fd);
		igt_require(&data.display.is_atomic);
		igt_display_require_output(&data.display);

		/* check if visual confirm option available */
		if (opt.visual_confirm) {
			igt_skip_on(!igt_amd_has_visual_confirm(data.fd));
			igt_skip_on_f(!igt_amd_set_visual_confirm(data.fd, VISUAL_CONFIRM_REPLAY),
				 "set Panel Replay visual confirm failed\n");
		}
	}

	igt_describe("Test whether Panel Replay can be enabled with static screen");
	igt_subtest("replay_static_screen") run_check_replay(&data, TEST_MODE_STATIC_SCREEN);

	igt_describe("Test whether Panel Replay can be enabled with intermittent live mdoe");
	igt_subtest("replay_intermittent_live") run_check_replay(&data, TEST_MODE_INTERMITTENT_LIVE);

	igt_describe("Test whether Panel Replay can be enabled with constant live mdoe");
	igt_subtest("replay_constant_live") run_check_replay(&data, TEST_MODE_CONSTANT_LIVE);

	igt_describe("Test whether Panel Replay can be enabled after resume from suspend");
	igt_subtest("replay_suspend") run_check_replay_suspend(&data);

	igt_fixture()
	{
		if (opt.visual_confirm) {
			igt_skip_on(!igt_amd_has_visual_confirm(data.fd));
			igt_require_f(igt_amd_set_visual_confirm(data.fd, VISUAL_CONFIRM_DISABLE),
				 "reset Panel Replay visual confirm failed\n");
		}
		close(data.debugfs_fd);
		igt_display_fini(&data.display);
		drm_close_driver(data.fd);
	}
}
