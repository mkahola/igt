// SPDX-License-Identifier: MIT
// Copyright 2025 Advanced Micro Devices, Inc.

#include "igt.h"
#include "igt_kms.h"
#include "amdgpu_drm.h"
#include "amdgpu.h"

/*
 * Only two ASICs of FAMILY_RV are DCN 2.1.
 * They can be determined by their external chip revision.
 *
 * This is necessary to determine if the NO_AVAILABLE_PLANES subtest is
 * applicable to the ASIC under test.
 *
 * NOTE: Copied from dal_asic_id.h in AMD's display driver on Linux.
 */
#define ASICREV_IS_RENOIR(eChipRev) ((eChipRev >= 0x91) && (eChipRev < 0xF0))
#define ASICREV_IS_GREEN_SARDINE(eChipRev) ((eChipRev >= 0xA1) && (eChipRev < 0xFF))


/**
 * TEST: amd_cursor_overlay
 * Category: Display
 * Description: Tests cursor fall back from native to overlay
 * Driver requirement: amdgpu
 */

/**
 * SUBTEST: rgb-to-yuv
 * Description: Tests native cursor fall back to overlay cursor when a top plane
 *				switches from RGB to YUV.
 * SUBTEST: non-full
 * Description: Tests native cursor fall back to overlay cursor when a top plane
 *				does not fill the crtc.
 * SUBTEST: scaling-%d
 * Description: Tests native cursor fall back to overlay cursor when a top plane
 *				is scaled.
 *
 * arg[1].values: 50, 75, 125, 150, 175, 200
 *
 * SUBTEST: max-planes
 * Description: Tests native cursor fall back to overlay cursor when a top plane
 *				is YUV and there are all but one overlay planes are used.
 *
 * SUBTEST: no-available-planes
 * Description: Tests native cursor attempt to fall back to overlay cursor,
 *				but fails atomic commit due to no available overlay planes.
 */

enum {
	TEST_YUV = 1,
	TEST_QUARTER_FB = 1 << 1,
	TEST_SCALING = 1 << 2,
	TEST_MAX_PLANES = 1 << 3,
	TEST_NO_AVAILABLE_PLANES = 1 << 4,
};

typedef struct {
	int x;
	int y;
} pos_t;

/* Common test data. */
typedef struct data {
	igt_display_t display;
	igt_plane_t *primary;
	igt_plane_t *cursor;
	igt_plane_t *overlays[6];
	igt_output_t *output;
	igt_crtc_t *crtc;
	igt_pipe_crc_t *pipe_crc;
	drmModeModeInfo *mode;
	igt_fb_t rgb_fb;
	igt_fb_t rgb_fb_o;
	igt_fb_t yuv_fb;
	igt_fb_t quarter_fb;
	igt_fb_t scale_fb;
	igt_fb_t cfb;
	enum pipe pipe_id;
	int drm_fd;
	int available_overlay_planes;
	uint64_t max_curw;
	uint64_t max_curh;
} data_t;

/* Retuns the number of available overlay planes. */
static int get_overlay_planes_count(igt_display_t *display, enum pipe pipe)
{
	int count = 0;
	igt_plane_t *plane;

	for_each_plane_on_pipe(display, pipe, plane)
		if (plane->type == DRM_PLANE_TYPE_OVERLAY)
			count++;

	return count;
}

/* Sets all overlay planes to the given fb and position, then commits. */
static void set_overlay_planes(data_t *data, int count, igt_fb_t *fb, int x, int y)
{
	for (int i = 0; i < count; i++) {
		igt_plane_set_fb(data->overlays[i], fb);
		igt_plane_set_position(data->overlays[i], x, y);
	}
	igt_display_commit_atomic(&data->display, 0, NULL);
}

/*
 * Checks the ASIC has enough overlay planes and from a supported family.
 *
 * Currently TEST_NO_AVAILABLE_PLANES subtest is only
 * applicable to DCN 2.1 & DCN 3.5+ APUs.
 */
static bool can_support_all_overlay_planes(int available_overlay_planes, int family_id, int chip_rev_id)
{
	/* For now we only support ASICs with 3 overlay planes. */
	if (available_overlay_planes != 3)
		return false;

	switch (family_id) {
	case AMDGPU_FAMILY_RV:
		return (ASICREV_IS_RENOIR(chip_rev_id) ||
			ASICREV_IS_GREEN_SARDINE(chip_rev_id));
	case AMDGPU_FAMILY_GC_11_5_0:
		return true;
	default:
		return false;
	}
}

/* Common test setup. */
static void test_init(data_t *data, enum pipe pipe_id, igt_output_t *output,
		      unsigned int flags, int available_overlay_planes)
{
	int i;

	data->pipe_id = pipe_id;
	data->available_overlay_planes = available_overlay_planes;
	data->crtc = &data->display.crtcs[data->pipe_id];
	data->output = output;
	data->mode = igt_output_get_mode(data->output);
	data->primary = igt_crtc_get_plane_type(data->crtc,
						DRM_PLANE_TYPE_PRIMARY);
	data->cursor = igt_crtc_get_plane_type(data->crtc,
					       DRM_PLANE_TYPE_CURSOR);

	if (flags & TEST_MAX_PLANES)
		for (i = 0; i < available_overlay_planes - 1; i++)
			data->overlays[i] = igt_crtc_get_plane_type_index(data->crtc,
									  DRM_PLANE_TYPE_OVERLAY,
									  i);
	if (flags & TEST_NO_AVAILABLE_PLANES)
		for (i = 0; i < available_overlay_planes; i++)
			data->overlays[i] = igt_crtc_get_plane_type_index(data->crtc,
									  DRM_PLANE_TYPE_OVERLAY,
									  i);

	igt_info("Using (pipe %s + %s) to run the subtest.\n",
		 kmstest_pipe_name(data->pipe_id), igt_output_name(data->output));

	igt_require_pipe_crc(data->drm_fd);
	data->pipe_crc = igt_crtc_crc_new(igt_crtc_for_pipe(&data->display, data->pipe_id),
					  IGT_PIPE_CRC_SOURCE_AUTO);
}

/* Common test finish. */
static void test_fini(data_t *data)
{
	/* Free CRC collector first */
	igt_pipe_crc_free(data->pipe_crc);

	/* Clear all planes */
	igt_plane_set_fb(data->primary, NULL);
	igt_plane_set_fb(data->cursor, NULL);

	for (int i = 0; i < data->available_overlay_planes; i++)
		if (data->overlays[i])
			igt_plane_set_fb(data->overlays[i], NULL);

	/* Commit the cleared plane state before resetting the graph */
	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	/* Reset the display graph after committing the null state */
	igt_display_reset(&data->display);
}

/* Common test cleanup. */
static void test_cleanup(data_t *data)
{
	igt_remove_fb(data->drm_fd, &data->cfb);
	igt_remove_fb(data->drm_fd, &data->rgb_fb);
	igt_remove_fb(data->drm_fd, &data->rgb_fb_o);
	igt_remove_fb(data->drm_fd, &data->yuv_fb);
	igt_remove_fb(data->drm_fd, &data->quarter_fb);
	igt_remove_fb(data->drm_fd, &data->scale_fb);
}


static void test_cursor_pos(data_t *data, int x, int y, unsigned int flags)
{
	igt_crc_t ref_crc, test_crc;
	cairo_t *cr;
	igt_fb_t *rgb_fb = &data->rgb_fb;
	igt_fb_t *rgb_fb_o = &data->rgb_fb_o;
	igt_fb_t *yuv_fb = &data->yuv_fb;
	igt_fb_t *quarter_fb = &data->quarter_fb;
	igt_fb_t *cfb = &data->cfb;
	igt_fb_t *scale_fb = &data->scale_fb;
	int cw = cfb->width;
	int ch = cfb->height;
	int available_overlay_planes = data->available_overlay_planes;
	int opp_x, opp_y, ret;

	cr = igt_get_cairo_ctx(rgb_fb->fd, rgb_fb);

	igt_plane_set_fb(data->primary, rgb_fb);
	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	igt_paint_color(cr, 0, 0, rgb_fb->width, rgb_fb->height, 0.0, 0.0, 0.0);

	/* Draw a magenta square where the cursor should be. */
	igt_paint_color(cr, x, y, cw, ch, 1.0, 0.0, 1.0);
	igt_put_cairo_ctx(cr);

	/* Display the cursor. */
	igt_plane_set_fb(data->cursor, cfb);
	igt_plane_set_position(data->cursor, x, y);
	igt_display_commit_atomic(&data->display, 0, NULL);

	/* Place the overlay plane on the opposite quarter of the screen from the cursor. */
	if (flags & TEST_MAX_PLANES ||
	    flags & TEST_NO_AVAILABLE_PLANES ||
	    flags & TEST_QUARTER_FB) {
		opp_x = x < (data->mode->hdisplay / 2) ? (data->mode->hdisplay / 2) : 0;
		opp_y = y < (data->mode->vdisplay / 2) ? (data->mode->vdisplay / 2) : 0;
	}

	if (flags & TEST_NO_AVAILABLE_PLANES) {

		/* Display the overlay planes. */
		set_overlay_planes(data, available_overlay_planes, rgb_fb_o, opp_x, opp_y);

		/*
		 * Trigger cursor fall back due to a YUV plane;
		 * expect the atomic commit to fail due to no
		 * available overlay planes.
		 */
		igt_plane_set_fb(data->primary, &data->yuv_fb);
		ret = igt_display_try_commit_atomic(&data->display,
			 DRM_MODE_ATOMIC_ALLOW_MODESET, 0);

		/* Expected atomic commit to fail due to no available overlay planes. */
		igt_assert_f(ret == -EINVAL,
			"Expected commit fail due to no available overlay planes.\n");

		/* Exit early. */
		return;
	}

	/* Display the overlay planes as a reference for TEST_MAX_PLANES. */
	if (flags & TEST_MAX_PLANES) {
		/* Display the overlay planes. */
		set_overlay_planes(data, available_overlay_planes - 1, rgb_fb_o, opp_x, opp_y);
	}

	/** Record a reference CRC. */
	igt_pipe_crc_start(data->pipe_crc);
	igt_pipe_crc_get_current(data->drm_fd, data->pipe_crc, &ref_crc);

	/* Switch primary plane to YUV FB for TEST_YUV and TEST_MAX_PLANES. */
	if (flags & TEST_YUV || flags & TEST_MAX_PLANES) {
		igt_plane_set_fb(data->primary, yuv_fb);
		igt_plane_set_position(data->primary, 0, 0);
		igt_plane_set_size(data->primary, yuv_fb->width, yuv_fb->height);
		igt_display_commit_atomic(&data->display, DRM_MODE_ATOMIC_ALLOW_MODESET, 0);

	/* Switch primary plane to use a quarter-sized FB, opposite from cursor. */
	} else if (flags & TEST_QUARTER_FB) {
		igt_plane_set_fb(data->primary, quarter_fb);
		igt_plane_set_position(data->primary, opp_x, opp_y);
		igt_display_commit_atomic(&data->display, 0, NULL);

	/* Switch primary plane to use a scaled FB. */
	} else if (flags & TEST_SCALING) {
		igt_plane_set_fb(data->primary, scale_fb);
		igt_plane_set_position(data->primary, 0, 0);
		igt_plane_set_size(data->primary, data->mode->hdisplay, data->mode->vdisplay);
		igt_display_commit_atomic(&data->display, 0, NULL);
	}

	/*
	 * Wait for one more vblank since cursor updates are not
	 * synchronized to the same frame on AMD hw.
	 */
	if(is_amdgpu_device(data->drm_fd))
		igt_wait_for_vblank_count(igt_crtc_for_pipe(&data->display, data->pipe_id), 1);

	/* Record the new CRC. */
	igt_pipe_crc_get_current(data->drm_fd, data->pipe_crc, &test_crc);
	igt_pipe_crc_stop(data->pipe_crc);

	/* CRC Check is sufficient for this test */
	igt_assert_crc_equal(&ref_crc, &test_crc);
}

/*
 * Tests the cursor on a variety of positions on the screen.
 * Specific edge cases that should be captured here are the negative edges
 * of each plane and the centers.
 */
static void test_cursor_spots(data_t *data, int size, unsigned int flags)
{
	int sw = data->mode->hdisplay;
	int sh = data->mode->vdisplay;
	int i;
		const pos_t pos[] = {
		/* Test diagonally from top left to bottom right. */
		{ -size / 3, -size / 3 },
		{ 0, 0 },
		{ sw / 4 - size, sh / 4 - size },
		{ sw / 4 - size / 3, sh / 4 - size / 3 },
		{ sw / 4, sh / 4 },
		{ sw / 4 + size, sh / 4 + size },
		{ sw / 2, sh / 2 },
		{ sw / 4 + sw / 2 - size, sh / 4 + sh / 2 - size },
		{ sw / 4 + sw / 2 - size / 3, sh / 4 + sh / 2 - size / 3 },
		{ sw / 4 + sw / 2 + size, sh / 4 + sh / 2 + size },
		{ sw - size, sh - size },
		{ sw - size / 3, sh - size / 3 },
		/* Test remaining corners. */
		{ sw - size, 0 },
		{ 0, sh - size },
		{ sw / 4 + sw / 2 - size, sh / 4 },
		{ sw / 4, sh / 4 + sh / 2 - size }
	};

	for (i = 0; i < ARRAY_SIZE(pos); ++i)
		test_cursor_pos(data, pos[i].x, pos[i].y, flags);
}

static void test_cursor(data_t *data, int size, unsigned int flags, unsigned int scaling_factor)
{
	int sw, sh;

	igt_skip_on(size > data->max_curw || size > data->max_curh);

	sw = data->mode->hdisplay;
	sh = data->mode->vdisplay;

	test_cleanup(data);

	/* Create primary FB. */
	igt_create_color_fb(data->drm_fd, sw, sh, DRM_FORMAT_XRGB8888,
			    DRM_FORMAT_MOD_LINEAR, 0.0, 0.0, 0.0, &data->rgb_fb);

	/* Create cursor FB. */
	igt_create_color_fb(data->drm_fd, size, size, DRM_FORMAT_ARGB8888,
				DRM_FORMAT_MOD_LINEAR, 1.0, 0.0, 1.0, &data->cfb);

	/* Create YUV FB for RGB-to-YUV, MAX_PLANES and NO_AVAILABLE_PLANES subtests */
	if (flags & TEST_YUV ||
	    flags & TEST_MAX_PLANES ||
	    flags & TEST_NO_AVAILABLE_PLANES)
		igt_create_fb(data->drm_fd, sw, sh, DRM_FORMAT_NV12,
					DRM_FORMAT_MOD_NONE, &data->yuv_fb);

	/* Create a quarter-sized FB. */
	if (flags & TEST_QUARTER_FB)
		igt_create_color_fb(data->drm_fd, sw / 2, sh / 2, DRM_FORMAT_XRGB8888,
					DRM_FORMAT_MOD_LINEAR, 0.0, 0.0, 0.0, &data->quarter_fb);

	/* Create a FB for scaling. */
	if (flags & TEST_SCALING)
		igt_create_color_fb(data->drm_fd, (sw * scaling_factor) / 100, (sh * scaling_factor) / 100, DRM_FORMAT_XRGB8888,
					DRM_FORMAT_MOD_LINEAR, 0.0, 0.0, 0.0, &data->scale_fb);

	/*
	 * Create RGB FB for overlay planes for MAX_PLANES and
	 * NO_AVAILABLE_PLANES subtests.
	 *
	 * The overlay FB size is quarter the screen size to ensure that
	 * the cursor can be placed on the primary plane to trigger fall back.
	 */
	if (flags & TEST_MAX_PLANES || flags & TEST_NO_AVAILABLE_PLANES) {
		/* Create RGB FB for overlay planes. */
		igt_create_color_fb(data->drm_fd, sw / 2, sh / 2, DRM_FORMAT_XRGB8888,
					DRM_FORMAT_MOD_LINEAR, 0.0, 1.0, 0.0, &data->rgb_fb_o);
	}

	igt_output_set_crtc(data->output,
		igt_crtc_for_pipe(&data->display, data->pipe_id));

	/* Run the test for different cursor spots. */
	test_cursor_spots(data, size, flags);
}

int igt_main()
{
	static const int cursor_sizes[] = { 64, 128, 256 };
	data_t data = { .max_curw = 64, .max_curh = 64 };
	igt_crtc_t *crtc;
	igt_output_t *output;
	igt_display_t *display;
	int i, j, available_overlay_planes;
	int ret, err, family_id, chip_rev_id;
	uint32_t major, minor;
	amdgpu_device_handle device;
	struct amdgpu_gpu_info gpu_info = {0};
	struct {
		const char *name;
		unsigned int flags;
		unsigned int scale_factor;
		const char *desc;
	} tests[] = {
		{ "rgb-to-yuv", TEST_YUV, 100,
		"Tests native cursor fall back to overlay cursor when a top plane switches from RGB to YUV" },
		{"non-full", TEST_QUARTER_FB, 100,
		"Tests native cursor fall back to overlay cursor when a top plane does not fill the crtc"},
		{"max-planes", TEST_MAX_PLANES, 100,
		"Tests native cursor fall back to overlay cursor when a top plane is YUV and there are all but one overlay planes used."},
		{"no-available-planes", TEST_NO_AVAILABLE_PLANES, 100,
		"Tests native cursor attempt to fall back to overlay cursor required, but fails atomic commit due to no available overlay planes."},
		{"scaling-50", TEST_SCALING, 50,
		"Tests native cursor fall back to overlay cursor when a top plane is scaled"},
		{"scaling-75", TEST_SCALING, 75,
		"Tests native cursor fall back to overlay cursor when a top plane is scaled"},
		{"scaling-125", TEST_SCALING, 125,
		"Tests native cursor fall back to overlay cursor when a top plane is scaled"},
		{"scaling-150", TEST_SCALING, 150,
		"Tests native cursor fall back to overlay cursor when a top plane is scaled"},
		{"scaling-175", TEST_SCALING, 175,
		"Tests native cursor fall back to overlay cursor when a top plane is scaled"},
		{"scaling-200", TEST_SCALING, 200,
		"Tests native cursor fall back to overlay cursor when a top plane is scaled"},
	};

	igt_fixture() {

		/* Initialize the driver and retrieve GPU info. */
		data.drm_fd = drm_open_driver_master(DRIVER_AMDGPU);
		err = amdgpu_device_initialize(data.drm_fd, &major, &minor, &device);
		igt_require(err == 0);

		err = amdgpu_query_gpu_info(device, &gpu_info);
		igt_require(err == 0);

		family_id = gpu_info.family_id;
		chip_rev_id = gpu_info.chip_external_rev;

		igt_display_require(&data.display, data.drm_fd);
		igt_require(data.display.is_atomic);
		igt_display_require_output(&data.display);
		display = &data.display;

		ret = drmGetCap(data.drm_fd, DRM_CAP_CURSOR_WIDTH, &data.max_curw);
		igt_assert(ret == 0 || errno == EINVAL);
		ret = drmGetCap(data.drm_fd, DRM_CAP_CURSOR_HEIGHT, &data.max_curh);
		igt_assert(ret == 0 || errno == EINVAL);

		kmstest_set_vt_graphics_mode();
	}


	for (i = 0; i < ARRAY_SIZE(tests); i++) {
		igt_describe_f("%s", tests[i].desc);
		igt_subtest_with_dynamic_f("%s", tests[i].name) {

			/*
			 * Skip YUV, MAX_PLANES and NO_AVAILABLE_PLANES subtests
			 * if YUV is not supported.
			 */
			if (tests[i].flags & TEST_YUV ||
			    tests[i].flags & TEST_MAX_PLANES ||
			    tests[i].flags & TEST_NO_AVAILABLE_PLANES)
				igt_require(igt_display_has_format_mod(display,
							DRM_FORMAT_NV12,
							DRM_FORMAT_MOD_LINEAR));

			for_each_crtc_with_single_output(&data.display, crtc,
							 output) {

				igt_display_reset(display);
				igt_output_set_crtc(output,
					crtc);

				available_overlay_planes = get_overlay_planes_count(display,
										    crtc->pipe);

				/* Require at least one overlay plane. */
				if (!available_overlay_planes)
					igt_skip("%s subtest requires at least 1 overlay plane.\n",
						 tests[i].name);

				/*
				 * For now, NO_AVAILABLE_PLANES substest is only appropriate for
				 * AMD ASICs with 3 overlay planes and with DCN 2.1 & 3.5+ APU's.
				 */
				if (tests[i].flags & TEST_NO_AVAILABLE_PLANES &&
				    !can_support_all_overlay_planes(available_overlay_planes, family_id, chip_rev_id))
					igt_skip("%s subtest requires 3 overlay planes with a supported DCN.\n",
						 tests[i].name);

				test_init(&data, crtc->pipe, output,
					  tests[i].flags,
					  available_overlay_planes);

				for (j = 0; j < ARRAY_SIZE(cursor_sizes); j++) {
					int size = cursor_sizes[j];

					igt_dynamic_f("pipe-%s-%s-size-%d",
						      igt_crtc_name(crtc),
						      igt_output_name(output),
						      size)
						test_cursor(&data, size, tests[i].flags, tests[i].scale_factor);

					test_cleanup(&data);
				}

				test_fini(&data);

				/* Detach output and commit a clean state before moving to the next subtest */
				igt_output_set_crtc(output, NULL);
				igt_display_commit2(&data.display, COMMIT_ATOMIC);
			}
		}
	}

	igt_fixture() {

		igt_display_reset(&data.display);
		igt_display_commit2(&data.display, COMMIT_ATOMIC);
		igt_display_fini(&data.display);
		drm_close_driver(data.drm_fd);
	}
}
