// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 Arthur Grillo
 */

#include "igt.h"

#define FRAME_COUNT 100
#define NUM_FBS 2

struct rect_t {
	int x, y;
	int width, height;
};

struct plane_t {
	igt_plane_t *base;
	struct rect_t rect;
	uint32_t format;
	struct igt_fb fbs[NUM_FBS];
};

struct kms_t {
	struct rect_t crtc;
	struct plane_t primary;
	struct plane_t overlay_a;
	struct plane_t overlay_b;
	struct plane_t writeback;
};

struct data_t {
	int fd;
	igt_display_t display;
	igt_output_t *wb_output;
	drmModeModeInfo *mode;
	struct kms_t kms;
};

static void plane_setup(struct plane_t *plane, int index)
{
	igt_plane_set_size(plane->base, plane->rect.width, plane->rect.height);
	igt_plane_set_position(plane->base, plane->rect.x, plane->rect.y);
	igt_plane_set_fb(plane->base, &plane->fbs[index]);
}

static void gen_fbs(struct data_t *data)
{
	struct kms_t *kms = &data->kms;
	drmModeModeInfo *mode = igt_output_get_mode(data->wb_output);

	for (int i = 0; i < NUM_FBS; i++) {
		igt_create_color_fb(data->fd, kms->primary.rect.width, kms->primary.rect.height,
				    kms->primary.format, DRM_FORMAT_MOD_LINEAR,
				    !i, i, i,
				    &kms->primary.fbs[i]);

		igt_create_color_fb(data->fd, kms->overlay_a.rect.width, kms->overlay_a.rect.height,
				    kms->overlay_a.format, DRM_FORMAT_MOD_LINEAR,
				    i, !i, i,
				    &kms->overlay_a.fbs[i]);

		igt_create_color_fb(data->fd, kms->overlay_b.rect.width, kms->overlay_b.rect.height,
				    kms->overlay_b.format, DRM_FORMAT_MOD_LINEAR,
				    i, i, !i,
				    &kms->overlay_b.fbs[i]);

		kms->writeback.rect.width = mode->hdisplay;
		kms->writeback.rect.height = mode->vdisplay;
		igt_create_fb(data->fd, kms->writeback.rect.width, kms->writeback.rect.height,
			      kms->writeback.format, DRM_FORMAT_MOD_LINEAR,
			      &kms->writeback.fbs[i]);
	}
}

static igt_output_t *find_wb_output(struct data_t *data)
{
	for (int i = 0; i < data->display.n_outputs; i++) {
		igt_output_t *output = &data->display.outputs[i];

		if (output->config.connector->connector_type != DRM_MODE_CONNECTOR_WRITEBACK)
			continue;

		return output;

	}

	return NULL;
}

static void set_crtc_size(struct data_t *data)
{
	drmModeModeInfo *mode;
	struct rect_t *crtc = &data->kms.crtc;

	for_each_connector_mode(data->wb_output) {
		mode = &data->wb_output->config.connector->modes[j__];
		if (mode->hdisplay == crtc->width && mode->vdisplay == crtc->height) {
			igt_output_override_mode(data->wb_output, mode);
			return;
		}
	}


	igt_assert_f(0, "CRTC size %dx%d not supported\n", crtc->width, crtc->height);
}

static struct kms_t default_kms = {
	.crtc = {
		.width = 4096, .height = 2160,
	},
	.primary = {
		.rect = {
			.x = 101, .y = 0,
			.width = 3639, .height = 2160,
		},
		.format = DRM_FORMAT_XRGB8888,
	},
	.overlay_a = {
		.rect = {
			.x = 201, .y = 199,
			.width = 3033, .height = 1777,
		},
		.format = DRM_FORMAT_XRGB16161616,
	},
	.overlay_b = {
		.rect = {
			.x = 1800, .y = 250,
			.width = 1507, .height = 1400,
		},
		.format = DRM_FORMAT_ARGB8888,
	},
	.writeback = {
		.rect = {
			.x = 0, .y = 0,
			// Size is to be determined at runtime
		},
		.format = DRM_FORMAT_XRGB8888,
	},
};


int igt_simple_main()
{
	struct data_t data = {0};
	igt_crtc_t *crtc;
	struct timespec then, now;
	double elapsed;

	data.kms = default_kms;

	data.fd = drm_open_driver_master(DRIVER_ANY);

	kmstest_set_vt_graphics_mode();

	igt_display_require(&data.display, data.fd);
	igt_require(data.display.is_atomic);

	igt_display_require_output(&data.display);

	igt_display_reset(&data.display);

	data.wb_output = find_wb_output(&data);
	igt_require(data.wb_output);

	for_each_crtc(&data.display, crtc) {
		igt_debug("Selecting pipe %s to %s\n",
			  igt_crtc_name(crtc),
			  igt_output_name(data.wb_output));
		igt_output_set_crtc(data.wb_output,
				    crtc);
		break;
	}

	set_crtc_size(&data);

	gen_fbs(&data);

	data.kms.primary.base = igt_output_get_plane_type(data.wb_output, DRM_PLANE_TYPE_PRIMARY);
	data.kms.overlay_a.base = igt_output_get_plane_type_index(data.wb_output,
								  DRM_PLANE_TYPE_OVERLAY, 0);
	data.kms.overlay_b.base = igt_output_get_plane_type_index(data.wb_output,
								  DRM_PLANE_TYPE_OVERLAY, 1);

	igt_assert_eq(igt_gettime(&then), 0);

	for (int i = 0; i < FRAME_COUNT; i++) {
		int fb_index = i % NUM_FBS;

		plane_setup(&data.kms.primary, fb_index);

		plane_setup(&data.kms.overlay_a, fb_index);

		plane_setup(&data.kms.overlay_b, fb_index);

		igt_output_set_writeback_fb(data.wb_output, &data.kms.writeback.fbs[fb_index]);

		igt_display_commit2(&data.display, COMMIT_ATOMIC);
	}

	igt_assert_eq(igt_gettime(&now), 0);
	elapsed = igt_time_elapsed(&then, &now);

	igt_info("Time spent in the loop with %d frames: %lfs.\n", FRAME_COUNT, elapsed);

	igt_display_fini(&data.display);
	drm_close_driver(data.fd);
}
