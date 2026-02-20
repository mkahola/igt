// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */

/**
 * TEST: kms chamelium sharpness filter
 * Category: Display
 * Description: Test to validate content adaptive sharpness filter using Chamelium
 * Driver requirement: xe
 * Mega feature: General Display Features
 */

#include "igt.h"
#include "igt_kms.h"

/**
 * SUBTEST: filter-basic
 * Description: Verify basic content adaptive sharpness filter.
 */

IGT_TEST_DESCRIPTION("Test to validate content adaptive sharpness filter using Chamelium");

#define MID_FILTER_STRENGTH		128
#define MAX_FRAMES			4

typedef struct {
	int drm_fd;
	igt_crtc_t *crtc;
	struct igt_fb fb;
	igt_display_t display;
	igt_output_t *output;
	igt_plane_t *primary;
	drmModeModeInfo *mode;
	int filter_strength;
	struct chamelium *chamelium;
	struct chamelium_port **ports;
	int port_count;
} data_t;

static bool pipe_output_combo_valid(data_t *data, enum pipe pipe)
{
	igt_display_t *display = &data->display;
	igt_crtc_t *crtc = igt_crtc_for_pipe(display, pipe);
	bool ret = true;

	igt_output_set_crtc(data->output,
			    crtc);
	if (!intel_pipe_output_combo_valid(&data->display))
		ret = false;
	igt_output_set_crtc(data->output, NULL);

	return ret;
}

static void set_filter_strength_on_pipe(data_t *data)
{
	igt_crtc_set_prop_value(data->crtc,
				    IGT_CRTC_SHARPNESS_STRENGTH,
				    data->filter_strength);
}

static void reset_filter_strength_on_pipe(data_t *data)
{
	igt_crtc_set_prop_value(data->crtc,
				    IGT_CRTC_SHARPNESS_STRENGTH, 0);
}

static void paint_image(igt_fb_t *fb)
{
	cairo_t *cr = igt_get_cairo_ctx(fb->fd, fb);
	int img_x, img_y, img_w, img_h;
	const char *file = "1080p-left.png";

	img_x = img_y = 0;
	img_w = fb->width;
	img_h = fb->height;

	igt_paint_image(cr, file, img_x, img_y, img_w, img_h);

	igt_put_cairo_ctx(cr);
}

static void setup_fb(int fd, int width, int height, uint32_t format,
		     uint64_t modifier, struct igt_fb *fb)
{
	int fb_id;

	fb_id = igt_create_fb(fd, width, height, format, modifier, fb);
	igt_assert(fb_id);

	paint_image(fb);
}

static void destroy_frame_dumps(struct chamelium_frame_dump *frames[], int count)
{
	for (int i = 0; i < count; i++) {
		if (frames[i]) {
			chamelium_destroy_frame_dump(frames[i]);
			frames[i] = NULL;
		}
	}
}

static void cleanup(data_t *data)
{
	igt_remove_fb(data->drm_fd, &data->fb);
	igt_output_set_crtc(data->output, NULL);
	igt_output_override_mode(data->output, NULL);
	igt_display_commit2(&data->display, COMMIT_ATOMIC);
}

static void test_t(data_t *data, igt_plane_t *primary,
		   struct chamelium_port *port)
{
	struct chamelium_frame_dump *frame[4];
	drmModeModeInfo *mode;
	int height, width;
	bool match[4], match_ok = false;

	igt_output_set_crtc(data->output,
			    data->crtc);

	mode = igt_output_get_mode(data->output);
	height = mode->hdisplay;
	width = mode->vdisplay;

	setup_fb(data->drm_fd, height, width, DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR, &data->fb);

	igt_plane_set_fb(data->primary, &data->fb);
	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	chamelium_capture(data->chamelium, port, 0, 0, 0, 0, 1);
	frame[0] =
		chamelium_read_captured_frame(data->chamelium, 0);

	set_filter_strength_on_pipe(data);
	igt_display_commit_atomic(&data->display, 0, NULL);

	chamelium_capture(data->chamelium, port, 0, 0, 0, 0, 1);
	frame[1] =
		chamelium_read_captured_frame(data->chamelium, 0);

	reset_filter_strength_on_pipe(data);
	igt_display_commit_atomic(&data->display, 0, NULL);

	chamelium_capture(data->chamelium, port, 0, 0, 0, 0, 1);
	frame[2] =
		chamelium_read_captured_frame(data->chamelium, 0);

	set_filter_strength_on_pipe(data);
	igt_display_commit_atomic(&data->display, 0, NULL);

	chamelium_capture(data->chamelium, port, 0, 0, 0, 0, 1);
	frame[3] =
		chamelium_read_captured_frame(data->chamelium, 0);

	match[0] = chamelium_frame_eq_or_dump_frame_pair(data->chamelium,
						       frame[0], frame[1]);

	match[1] = chamelium_frame_eq_or_dump_frame_pair(data->chamelium,
						       frame[1], frame[2]);

	match[2] = chamelium_frame_eq_or_dump_frame_pair(data->chamelium,
						       frame[0], frame[2]);

	match[3] = chamelium_frame_eq_or_dump_frame_pair(data->chamelium,
						       frame[1], frame[3]);

	match_ok = (match[0] == false) && (match[1] == false) && (match[2] == true) && (match[3] == true);

	destroy_frame_dumps(frame, MAX_FRAMES);
	cleanup(data);

	igt_assert_f(match_ok, "Sharpness filter test failed:\n"
	             "Expected: Frame[0]==Frame[1]: 0, Frame[1]==Frame[2]: 0, Frame[0]==Frame[2]: 1, Frame[1]==Frame[3]: 1\n"
		     "Observed: Frame[0]==Frame[1]: %d, Frame[1]==Frame[2]: %d, Frame[0]==Frame[2]: %d, Frame[1]==Frame[3]: %d\n", match[0], match[1], match[2], match[3]);
}

static int test_setup(data_t *data, enum pipe p)
{
	igt_display_t *display = &data->display;
	igt_crtc_t *crtc = igt_crtc_for_pipe(display, p);
	int i = 0;

	igt_display_reset(&data->display);
	igt_require(crtc->n_planes >= 0);

	data->primary = igt_crtc_get_plane_type(crtc, DRM_PLANE_TYPE_PRIMARY);
	igt_assert(data->primary);

	/*
	 * Prefer to run this test on HDMI connector if its connected, since on DP we
	 * sometimes face DP FSM issue
	 */
        for_each_valid_output_on_pipe(&data->display, crtc->pipe,
				      data->output) {
		data->crtc = crtc;
		for (i = 0; i < data->port_count; i++) {
			if ((data->output->config.connector->connector_type == DRM_MODE_CONNECTOR_HDMIA ||
			     data->output->config.connector->connector_type == DRM_MODE_CONNECTOR_HDMIB) &&
			     strcmp(data->output->name, chamelium_port_get_name(data->ports[i])) == 0)
				return i;
		}
	}

	for_each_valid_output_on_pipe(&data->display, crtc->pipe,
				      data->output) {
		data->crtc = crtc;
		for (i = 0; i < data->port_count; i++) {
			if (strcmp(data->output->name,
				   chamelium_port_get_name(data->ports[i])) == 0)
				return i;
		}
	}

	return -1;
}

static void test_sharpness_filter(data_t *data,  enum pipe p)
{
	igt_display_t *display = &data->display;
	igt_crtc_t *crtc = igt_crtc_for_pipe(display, p);
	int port_idx = test_setup(data, crtc->pipe);

	igt_require(port_idx >= 0);
	igt_require(igt_crtc_has_prop(crtc, IGT_CRTC_SHARPNESS_STRENGTH));

	if (!pipe_output_combo_valid(data, crtc->pipe))
		return;

	igt_dynamic_f("pipe-%s-%s", igt_crtc_name(crtc), data->output->name)
		(test_t(data, data->primary, data->ports[port_idx]));
}

static void
run_sharpness_filter_test(data_t *data)
{
	igt_display_t *display = &data->display;
	igt_crtc_t *crtc;

	igt_describe("Verify basic content adaptive sharpness filter.");
	igt_subtest_with_dynamic("filter-basic") {
		for_each_crtc(display, crtc) {
			data->filter_strength = MID_FILTER_STRENGTH;
			test_sharpness_filter(data, crtc->pipe);
		}
	}
}

int igt_main()
{
	data_t data = {};

	igt_fixture() {
		data.drm_fd = drm_open_driver_master(DRIVER_XE);

		igt_display_require(&data.display, data.drm_fd);
		igt_require(data.display.is_atomic);

		igt_chamelium_allow_fsm_handling = false;

		/* we need to initalize chamelium after igt_display_require */
		data.chamelium = chamelium_init(data.drm_fd, &data.display);
		igt_require(data.chamelium);

		data.ports = chamelium_get_ports(data.chamelium,
						 &data.port_count);

		if (!data.port_count)
			igt_skip("No ports connected\n");
		/*
		 * The behavior differs based on the availability of port mappings:
		 * - When using port mappings (chamelium_read_port_mappings),
		 *   ports are not plugged
		 * - During autodiscovery, all ports are plugged at the end.
		 *
		 * This quick workaround (unplug, plug, and re-probe the connectors)
		 * prevents any ports from being unintentionally skipped in test_setup.
		 */
		for(int i = 0; i < data.port_count; i++) {
			chamelium_unplug(data.chamelium, data.ports[i]);
			chamelium_plug(data.chamelium, data.ports[i]);
			chamelium_wait_for_conn_status_change(&data.display,
							      data.chamelium,
							      data.ports[i],
							      DRM_MODE_CONNECTED);
			igt_assert_f(chamelium_reprobe_connector(&data.display,
								 data.chamelium,
								 data.ports[i]) == DRM_MODE_CONNECTED,
								 "Output not connected\n");
		}

		kmstest_set_vt_graphics_mode();
	}

	run_sharpness_filter_test(&data);

	igt_fixture() {
		igt_display_fini(&data.display);
		drm_close_driver(data.drm_fd);
	}
}
