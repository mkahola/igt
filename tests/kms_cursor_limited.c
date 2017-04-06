#include "igt.h"

typedef struct {
	int drm_fd;
	igt_display_t display;
	struct igt_fb primary_fb;
	struct igt_fb fb;
	int fb_w, fb_h;
	igt_output_t *output;
	enum pipe pipe;
	igt_pipe_crc_t *pipe_crc;
} data_t;

static void cursor_enable(data_t *data)
{
	igt_output_t *output = data->output;
	igt_plane_t *cursor;

	cursor = igt_output_get_plane_type(output, DRM_PLANE_TYPE_CURSOR);
	igt_plane_set_fb(cursor, &data->fb);
	igt_plane_set_size(cursor, data->fb_w, data->fb_h);
}

static void cursor_disable(data_t *data)
{
	igt_output_t *output = data->output;
	igt_plane_t *cursor;

	cursor = igt_output_get_plane_type(output, DRM_PLANE_TYPE_CURSOR);
	igt_plane_set_fb(cursor, NULL);
}

static void overlay_enable(data_t *data)
{
	igt_output_t *output = data->output;
	igt_plane_t *overlay;

	overlay = igt_output_get_plane_type(output, DRM_PLANE_TYPE_OVERLAY);
	igt_plane_set_fb(overlay, &data->fb);
	igt_plane_set_size(overlay, data->fb_w, data->fb_h);
}

static void overlay_disable(data_t *data)
{
	igt_output_t *output = data->output;
	igt_plane_t *overlay;

	overlay = igt_output_get_plane_type(output, DRM_PLANE_TYPE_OVERLAY);
	igt_plane_set_fb(overlay, NULL);
}

static void create_fb(data_t *data)
{
	uint32_t fb_id;

	fb_id = igt_create_color_fb(data->drm_fd, data->fb_w, data->fb_h,
				    DRM_FORMAT_ARGB8888,
				    LOCAL_DRM_FORMAT_MOD_NONE,
				    0.5, 0.5, 0.5,
				    &data->fb);
	igt_assert(fb_id);
}

static void prepare_crtc(data_t *data, igt_output_t *output, int w, int h)
{
	drmModeModeInfo *mode;
	igt_display_t *display = &data->display;
	igt_plane_t *primary;

	/* select the pipe we want to use */
	igt_output_set_pipe(output, data->pipe);
	cursor_disable(data);

	/* create and set the primary plane fb */
	mode = igt_output_get_mode(output);
	igt_create_color_fb(data->drm_fd, mode->hdisplay, mode->vdisplay,
			    DRM_FORMAT_XRGB8888,
			    LOCAL_DRM_FORMAT_MOD_NONE,
			    0.0, 0.0, 0.0,
			    &data->primary_fb);

	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_plane_set_fb(primary, &data->primary_fb);

	igt_display_commit(display);

	/* create the pipe_crc object for this pipe */
	if (data->pipe_crc)
		igt_pipe_crc_free(data->pipe_crc);

	data->pipe_crc = igt_pipe_crc_new(data->drm_fd, data->pipe,
					  INTEL_PIPE_CRC_SOURCE_AUTO);

	data->fb_w = w;
	data->fb_h = h;
	create_fb(data);

	/* make sure cursor is disabled */
	cursor_disable(data);
	igt_wait_for_vblank(data->drm_fd, data->pipe);
}

static void do_single_test(data_t *data)
{
	igt_display_t *display = &data->display;
	igt_pipe_crc_t *pipe_crc = data->pipe_crc;
	igt_crc_t cursor_crc, overlay_crc;
	igt_plane_t *cursor, *overlay;

	cursor_enable(data);
	cursor = igt_output_get_plane_type(data->output, DRM_PLANE_TYPE_CURSOR);
	igt_plane_set_position(cursor, 0, 0);
	igt_display_commit(display);
	igt_wait_for_vblank(data->drm_fd, data->pipe);
	igt_pipe_crc_collect_crc(pipe_crc, &cursor_crc);

	cursor_disable(data);
	overlay_enable(data);
	overlay = igt_output_get_plane_type(data->output, DRM_PLANE_TYPE_OVERLAY);
	igt_plane_set_position(overlay, 0, 0);
	igt_display_commit(display);
	igt_wait_for_vblank(data->drm_fd, data->pipe);
	igt_pipe_crc_collect_crc(pipe_crc, &overlay_crc);

	igt_assert_crc_equal(&cursor_crc, &overlay_crc);

	overlay_disable(data);
	igt_display_commit(display);
}

static void run_tests(data_t *data)
{
	enum pipe p;
	igt_output_t *output;

	struct {
		const char *name;
		enum kmstest_broadcast_rgb_mode mode;
	} tests[] = {
		{ .name = "full", .mode = BROADCAST_RGB_FULL },
		{ .name = "limited", .mode = BROADCAST_RGB_16_235 },
	};


	for (int i = 0; i < ARRAY_SIZE(tests); i++) {
		igt_subtest(tests[i].name) {
			for_each_pipe_with_valid_output(&data->display, p, output) {
				data->output = output;
				data->pipe = p;

				prepare_crtc(data, output, 64, 64);

				kmstest_set_connector_broadcast_rgb(data->drm_fd,
								    output->config.connector,
								    tests[i].mode);

				do_single_test(data);
			}
		}
	}
}

static data_t data;

igt_main
{
	igt_fixture {
		data.drm_fd = drm_open_driver_master(DRIVER_INTEL);
		kmstest_set_vt_graphics_mode();

		igt_display_init(&data.display, data.drm_fd);
	}

	run_tests(&data);
}
