/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2025 Intel Corporation
 */

/**
 * TEST: kms dirty fbc
 * Category: Display
 * Description: Test DIRTYFB ioctl functionality with FBC enabled.
 * Driver requirement: xe
 * Mega feature: General Display Features
 */

#include <sys/types.h>

#include "igt.h"

#include "igt_sysfs.h"
#include "i915/gem.h"
#include "i915/gem_create.h"
#include "i915/intel_drrs.h"
#include "igt_psr.h"

#include "i915/intel_fbc.h"
#include "intel_mocs.h"
#include "intel_pat.h"

#include "xe/xe_query.h"

/**
 *
 * SUBTEST: fbc-dirty-rectangle-out-visible-area
 * Description: Sanity test to verify FBC DR by sending multiple damaged areas with non psr modes
 *
 * SUBTEST: fbc-dirty-rectangle-dirtyfb-tests
 * Description: Sanity test to verify FBC DR by sending multiple damaged areas with non psr modes
 *
 * SUBTEST: fbc-dirty-rectangle-different-formats
 * Description: Sanity test to verify FBC DR by sending multiple
 *              damaged areas with different formats.
 *
 */

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

#define SQUARE_SIZE 100
#define SQUARE_OFFSET 100
#define SQUARE_OFFSET_2 600

#define N_FBS				4
#define MAIN_FB_IDX			0
#define DIRTY_RECT_FBS_START_IDX	1

typedef struct {
	int drm_fd;
	int debugfs_fd;
	igt_display_t display;
	drmModeModeInfo *mode;
	igt_output_t *output;
	igt_pipe_crc_t *pipe_crc;
	enum pipe pipe;
	u32 format;

	struct igt_fb fb[N_FBS];

	enum {
		FEATURE_NONE  = 0,
		FEATURE_PSR   = 1,
		FEATURE_FBC   = 2,
		FEATURE_DRRS  = 4,
		FEATURE_COUNT = 8,
		FEATURE_DEFAULT = 8,
	} feature;
	bool is_simulation;
} data_t;

static void set_damage_clip_w(struct drm_mode_rect *damage, int x1, int y1, int width, int height)
{
	damage->x1 = x1;
	damage->y1 = y1;
	damage->x2 = x1 + width;
	damage->y2 = y1 + height;
}

static void dirty_rect_draw_white_rects(data_t *data, struct igt_fb *fb,
					int nrects, struct drm_mode_rect *rect)
{
	cairo_t *cr;

	if (!nrects || !rect)
		return;

	cr = igt_get_cairo_ctx(data->drm_fd, fb);

	for (int i = 0; i < nrects; i++) {
		igt_paint_color_alpha(cr, rect[i].x1, rect[i].y1,
				      rect[i].x2 - rect[i].x1,
				      rect[i].y2 - rect[i].y1,
				      1.0, 1.0, 1.0, 1.0);
	}

	igt_put_cairo_ctx(cr);
}


static void
set_damage_area(igt_plane_t *plane,  struct drm_mode_rect *rects,
		size_t length)
{
	igt_plane_replace_prop_blob(plane, IGT_PLANE_FB_DAMAGE_CLIPS, rects, length);
}

static void
set_fb_and_collect_crc(data_t *data, igt_plane_t *plane, struct igt_fb *fb,
		       igt_crc_t *crc)
{
	igt_plane_set_fb(plane, fb);
	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	igt_pipe_crc_start(data->pipe_crc);
	igt_pipe_crc_get_current(data->drm_fd, data->pipe_crc, crc);
	igt_pipe_crc_stop(data->pipe_crc);
	igt_assert_f(intel_fbc_is_enabled(data->drm_fd, data->pipe,
					  IGT_LOG_INFO),
					  "FBC is not enabled\n");
}

static void
update_rect_with_dirtyfb(data_t *data, struct igt_fb *fb1, struct igt_fb *fb2,
			 struct drm_mode_rect *rect)
{
	struct intel_buf *src, *dst;
	struct intel_bb *ibb;
	igt_spin_t *spin;
	int r;
	struct buf_ops *bops;
	igt_render_copyfunc_t rendercopy;

	bops = buf_ops_create(data->drm_fd);
	rendercopy = igt_get_render_copyfunc(data->drm_fd);

	src = intel_buf_create_full(bops, fb1->gem_handle, fb1->width,
				    fb1->height,
				    igt_drm_format_to_bpp(fb1->drm_format),
				    0,
				    igt_fb_mod_to_tiling(fb1->modifier),
				    0, fb1->size, 0, system_memory(data->drm_fd),
				    intel_get_pat_idx_uc(data->drm_fd),
				    DEFAULT_MOCS_INDEX);
	dst = intel_buf_create_full(bops, fb2->gem_handle,
				    fb2->width, fb2->height,
				    igt_drm_format_to_bpp(fb2->drm_format),
				    0, igt_fb_mod_to_tiling(fb2->modifier),
				    0, fb2->size, 0, system_memory(data->drm_fd),
				    intel_get_pat_idx_uc(data->drm_fd),
				    DEFAULT_MOCS_INDEX);
	ibb = intel_bb_create(data->drm_fd, PAGE_SIZE);

	spin = igt_spin_new(data->drm_fd, .ahnd = ibb->allocator_handle);
	igt_spin_set_timeout(spin, NSEC_PER_SEC);

	rendercopy(ibb, src, rect->x1, rect->y1, rect->x2 - rect->x1,
		   rect->y2 - rect->y1, dst, rect->x1, rect->y1);

	/* Perfom dirtyfb right after initiating rendercopy/blitter */
	r = drmModeDirtyFB(data->drm_fd, fb2->fb_id, NULL, 0);
	igt_assert(r == 0 || r == -ENOSYS);

	/* Ensure rendercopy/blitter is complete */
	intel_bb_sync(ibb);

	igt_spin_free(data->drm_fd, spin);
	intel_bb_destroy(ibb);
	intel_buf_destroy(src);
	intel_buf_destroy(dst);
}

static void fbc_dirty_rectangle_dirtyfb(data_t *data)
{
	igt_plane_t *primary;
	struct drm_mode_rect full_rect, rect1, rect2;
	igt_crc_t main_crc, fb2_crc, fb3_crc, crc;

	primary = igt_output_get_plane_type(data->output, DRM_PLANE_TYPE_PRIMARY);

	set_damage_clip_w(&full_rect, 0, 0, data->mode->hdisplay, data->mode->vdisplay);
	set_damage_clip_w(&rect1, SQUARE_OFFSET, SQUARE_OFFSET, SQUARE_SIZE, SQUARE_SIZE);
	set_damage_clip_w(&rect2, SQUARE_OFFSET_2, SQUARE_OFFSET_2, SQUARE_SIZE, SQUARE_SIZE);

	igt_create_color_fb(data->drm_fd, data->mode->hdisplay, data->mode->vdisplay,
			    data->format, DRM_FORMAT_MOD_LINEAR, 0.0, 0.0, 1.0, &data->fb[MAIN_FB_IDX]);

	igt_create_color_fb(data->drm_fd, data->mode->hdisplay, data->mode->vdisplay,
			    data->format, DRM_FORMAT_MOD_LINEAR, 0.0, 0.0, 1.0, &data->fb[1]);
	dirty_rect_draw_white_rects(data, &data->fb[1], 1, &rect1);

	igt_create_color_fb(data->drm_fd, data->mode->hdisplay, data->mode->vdisplay,
			    data->format, DRM_FORMAT_MOD_LINEAR, 0.0, 0.0, 1.0, &data->fb[2]);
	dirty_rect_draw_white_rects(data, &data->fb[2], 1, &rect2);

	/* 1st screen - Empty blue screen */
	set_damage_area(primary, &full_rect, sizeof(full_rect));
	set_fb_and_collect_crc(data, primary, &data->fb[MAIN_FB_IDX], &main_crc);

	/* 2nd screen - 1st white rect at 100, 100 - using damage area */
	set_damage_area(primary, &rect1, sizeof(rect1));
	set_fb_and_collect_crc(data, primary, &data->fb[1], &fb2_crc);

	/* 3rd screen - 2nd white rect at 600, 600 - using damage area.
	 * Now two white rects on screen
	 */
	set_damage_area(primary, &rect2, sizeof(rect2));
	set_fb_and_collect_crc(data, primary, &data->fb[2], &fb3_crc);

	/* 4th screen - clear the 2nd white rect at 600,600 with dirtyfb.
	 * Copy rect2 area from main_fb to fb3.
	 */
	update_rect_with_dirtyfb(data, &data->fb[MAIN_FB_IDX], &data->fb[2], &rect2);
	/* Now the screen must match 1st screen - with whole blue */
	igt_pipe_crc_collect_crc(data->pipe_crc, &crc);
	igt_assert_crc_equal(&crc, &main_crc);

	/* 5th screen - Copy the first rect at 100,100 with dirtyfb.
	 * Copy rect1 area from fb2 to fb3.
	 */
	update_rect_with_dirtyfb(data, &data->fb[1], &data->fb[2], &rect1);
	/* Now the screen must match 2nd screen - with one rect at 100,100 */
	igt_pipe_crc_collect_crc(data->pipe_crc, &crc);
	igt_assert_crc_equal(&crc, &fb2_crc);
}

/**
 * fbc_dirty_rectangle_outside_visible_region - Test dirty rectangle outside visible region
 * @data: Pointer to the test data structure
 *
 * This test verifies the behavior of the Frame Buffer Compression (FBC) when
 * dirty rectangles are set outside the visible region of the display. It creates
 * a main framebuffer and three additional framebuffers with dirty rectangles
 * positioned horizontally, vertically, and both horizontally and vertically
 * outside the visible region. The test then sets the damage area to these
 * rectangles and collects CRCs to ensure that the content outside the visible
 * region does not affect the main framebuffer's CRC.
 */
static void fbc_dirty_rectangle_outside_visible_region(data_t *data)
{
	igt_plane_t *primary;
	struct drm_mode_rect rect[N_FBS];
	igt_crc_t rect_crc[N_FBS];

	primary = igt_output_get_plane_type(data->output, DRM_PLANE_TYPE_PRIMARY);

	set_damage_clip_w(&rect[0], 0, 0, data->mode->hdisplay + 200, data->mode->vdisplay + 200);

	/* Rect Horizontally outside visible region */
	set_damage_clip_w(&rect[1], data->mode->hdisplay + 10, 100, SQUARE_SIZE, SQUARE_SIZE);

	/* Rect vertically outside visible region */
	set_damage_clip_w(&rect[2], 10, data->mode->vdisplay + 50, SQUARE_SIZE, SQUARE_SIZE);

	/* Rect Horizontally and vertically outside visible region */
	set_damage_clip_w(&rect[3], data->mode->hdisplay + 10, data->mode->vdisplay + 50,
			  SQUARE_SIZE, SQUARE_SIZE);

	igt_create_color_fb(data->drm_fd, data->mode->hdisplay + 200,
			    data->mode->vdisplay + 200, data->format,
			    DRM_FORMAT_MOD_LINEAR, 0.0, 1.0, 0.0, &data->fb[MAIN_FB_IDX]);

	for (int i = DIRTY_RECT_FBS_START_IDX; i < N_FBS; i++) {
		igt_create_color_fb(data->drm_fd, data->mode->hdisplay + 200,
				    data->mode->vdisplay + 200, data->format,
				    DRM_FORMAT_MOD_LINEAR, 0.0, 1.0, 0.0, &data->fb[i]);
		dirty_rect_draw_white_rects(data, &data->fb[i], 1, &rect[i]);
	}

	/* Main rect */
	set_damage_area(primary, &rect[MAIN_FB_IDX], sizeof(rect[MAIN_FB_IDX]));
	set_fb_and_collect_crc(data, primary, &data->fb[MAIN_FB_IDX], &rect_crc[MAIN_FB_IDX]);

	for (int i = DIRTY_RECT_FBS_START_IDX; i < N_FBS; i++) {
		set_damage_area(primary, &rect[i], sizeof(rect[i]));
		set_fb_and_collect_crc(data, primary, &data->fb[i], &rect_crc[i]);
		igt_assert_crc_equal(&rect_crc[i], &rect_crc[MAIN_FB_IDX]);
	}
}

/*
 * fbc_dirty_rectangle_basic
 * @data: data_t
 *
 * This test draws screens as full-screen updates and collects their CRCs
 * as reference values. Screens are then updated using the FBC
 * dirty rect feature and compared with the reference CRCs.
 * Matching CRCs indicate success.
 *
 * Steps to Collect Reference CRCs:
 * 1. Full Blue Screen
 *    - Frame Buffer: main_fb
 *    - CRC: main_fb_crc
 * 2. White Square on Upper Left
 *    - Frame Buffer: rect1_fb
 *    - CRC: rect1_fb_crc
 * 3. Second White Square Below First
 *    - Frame Buffer: rect2_fb
 *    - CRC: rect2_fb_crc
 * 4. Both Rectangles
 *    - Frame Buffer: rect_combined_fb
 *    - CRC: rect_combined_fb_crc
 *
 * Steps to Update Screen with FBC Dirty Rect:
 * 1. Full Blue Screen
 *    - Set rect_combined_fb with Damage Area Update
 *    - CRC should match rect_combined_fb_crc
 * 2. Clear First Rectangle Area
 *    - Use main_fb and damage area as rect1 coordinates
 *    - CRC should match rect2_fb_crc
 * 3. Clear Second Rectangle Area
 *    - Use main_fb and damage area as rect2 coordinates
 *    - CRC should match main_fb_crc
 */
static void fbc_dirty_rectangle_basic(data_t *data)
{
	igt_plane_t *primary;
	struct drm_mode_rect rect1;
	struct drm_mode_rect rect2;
	struct drm_mode_rect rect_combined[2];
	struct drm_mode_rect full_rect;
	igt_crc_t main_fb_crc, rect_1_fb_crc, rect_2_fb_crc, rect_combined_fb_crc, crc;

	primary = igt_output_get_plane_type(data->output, DRM_PLANE_TYPE_PRIMARY);

	set_damage_clip_w(&full_rect, 0, 0, data->mode->hdisplay, data->mode->vdisplay);
	set_damage_clip_w(&rect1, SQUARE_OFFSET, SQUARE_OFFSET, SQUARE_SIZE, SQUARE_SIZE);
	set_damage_clip_w(&rect2, SQUARE_OFFSET_2, SQUARE_OFFSET_2, SQUARE_SIZE, SQUARE_SIZE);
	rect_combined[0] = rect1;
	rect_combined[1] = rect2;

	igt_create_color_fb(data->drm_fd, data->mode->hdisplay, data->mode->vdisplay, data->format,
			    DRM_FORMAT_MOD_LINEAR, 0.0, 0.0, 1.0, &data->fb[MAIN_FB_IDX]);

	igt_create_color_fb(data->drm_fd, data->mode->hdisplay, data->mode->vdisplay, data->format,
			    DRM_FORMAT_MOD_LINEAR, 0.0, 0.0, 1.0, &data->fb[1]);
	dirty_rect_draw_white_rects(data, &data->fb[1], 1, &rect1);

	igt_create_color_fb(data->drm_fd, data->mode->hdisplay, data->mode->vdisplay, data->format,
			    DRM_FORMAT_MOD_LINEAR, 0.0, 0.0, 1.0, &data->fb[2]);
	dirty_rect_draw_white_rects(data, &data->fb[2], 1, &rect2);

	igt_create_color_fb(data->drm_fd, data->mode->hdisplay, data->mode->vdisplay, data->format,
			    DRM_FORMAT_MOD_LINEAR, 0.0, 0.0, 1.0, &data->fb[3]);
	dirty_rect_draw_white_rects(data, &data->fb[3], ARRAY_SIZE(rect_combined),
				    rect_combined);

	/* main_fb blank blue screen - get and store crc */
	set_fb_and_collect_crc(data, primary, &data->fb[MAIN_FB_IDX], &main_fb_crc);

	/* Whole blue screen with one white rect and collect crc */
	set_damage_area(primary, &full_rect, sizeof(full_rect));
	set_fb_and_collect_crc(data, primary, &data->fb[1], &rect_1_fb_crc);

	/* Second white rect and collect crc */
	set_damage_area(primary, &full_rect, sizeof(full_rect));
	set_fb_and_collect_crc(data, primary, &data->fb[2], &rect_2_fb_crc);

	/* Both rects and collect crc */
	set_damage_area(primary, &full_rect, sizeof(full_rect));
	set_fb_and_collect_crc(data, primary, &data->fb[3], &rect_combined_fb_crc);

	/* Put full blank screen back */
	set_damage_area(primary, &full_rect, sizeof(full_rect));
	set_fb_and_collect_crc(data, primary, &data->fb[MAIN_FB_IDX], &crc);
	igt_assert_crc_equal(&crc, &main_fb_crc);

	/* Set combined rect - draw two white rects using damage area */
	set_damage_area(primary, rect_combined, sizeof(rect_combined));
	set_fb_and_collect_crc(data, primary, &data->fb[3], &crc);
	igt_assert_crc_equal(&crc, &rect_combined_fb_crc);

	/* Clear first rect using damage area. Only the second rect should be visible here! */
	set_damage_area(primary, &rect1, sizeof(rect1));
	set_fb_and_collect_crc(data, primary, &data->fb[MAIN_FB_IDX], &crc);
	igt_assert_crc_equal(&crc, &rect_2_fb_crc);

	/* Clear the second rect as well. Now back to original blank screen */
	set_damage_area(primary, &rect2, sizeof(rect2));
	set_fb_and_collect_crc(data, primary, &data->fb[MAIN_FB_IDX], &crc);
	igt_assert_crc_equal(&crc, &main_fb_crc);
}

static void cleanup(data_t *data)
{
	igt_remove_fb(data->drm_fd, &data->fb[0]);
	igt_remove_fb(data->drm_fd, &data->fb[1]);
	igt_remove_fb(data->drm_fd, &data->fb[2]);
	igt_remove_fb(data->drm_fd, &data->fb[3]);

	igt_pipe_crc_free(data->pipe_crc);

	igt_output_set_crtc(data->output, NULL);

	igt_display_commit2(&data->display, COMMIT_ATOMIC);
}

static bool prepare_test(data_t *data)
{
	igt_display_t *display = &data->display;
	igt_display_reset(&data->display);

	data->mode = igt_output_get_mode(data->output);
	igt_output_set_crtc(data->output,
			    igt_crtc_for_pipe(display, data->pipe));
	data->pipe_crc = igt_crtc_crc_new(igt_crtc_for_pipe(display, data->pipe),
					  IGT_PIPE_CRC_SOURCE_AUTO);

	igt_require_f(intel_fbc_supported_on_chipset(data->drm_fd, data->pipe),
		      "FBC not supported by the chipset on pipe\n");

	if (psr_sink_support(data->drm_fd, data->debugfs_fd, PSR_MODE_1, NULL) ||
	    psr_sink_support(data->drm_fd, data->debugfs_fd, PSR_MODE_2, NULL) ||
	    psr_sink_support(data->drm_fd, data->debugfs_fd, PR_MODE, NULL)) {
		igt_info("PSR is supported by the sink. Disabling PSR to test Dirty FBC functionality.\n");
		psr_disable(data->drm_fd, data->debugfs_fd, data->output);
	}

	if (data->feature & FEATURE_FBC)
		intel_fbc_enable(data->drm_fd);

	return intel_pipe_output_combo_valid(&data->display);
}

static void fbc_dirty_rectangle_test(data_t *data, void (*test_func)(data_t *))
{
	if (!prepare_test(data))
		return;

	test_func(data);
	cleanup(data);
}

int igt_main()
{
	igt_crtc_t *crtc;
	data_t data = {0};

	igt_fixture() {
		data.drm_fd = drm_open_driver_master(DRIVER_XE);
		igt_require(data.drm_fd >= 0);
		data.debugfs_fd = igt_debugfs_dir(data.drm_fd);
		kmstest_set_vt_graphics_mode();
		igt_require_pipe_crc(data.drm_fd);
		igt_display_require(&data.display, data.drm_fd);
		igt_display_require_output(&data.display);
		igt_require_f(intel_display_ver(intel_get_drm_devid(data.drm_fd)) >= 30,
			      "FBC with dirty region is not supported\n");
		data.is_simulation = igt_run_in_simulation();
	}

	igt_subtest_with_dynamic("fbc-dirty-rectangle-out-visible-area") {
		bool single_pipe = false;
		data.feature = FEATURE_FBC;

		for_each_crtc(&data.display, crtc) {
			data.pipe = crtc->pipe;
			if (single_pipe)
				break;
			for_each_valid_output_on_pipe(&data.display,
						      crtc->pipe, data.output) {
				data.format = DRM_FORMAT_XRGB8888;

				igt_dynamic_f("pipe-%s-%s",
					       igt_crtc_name(crtc),
					       igt_output_name(data.output)) {
					fbc_dirty_rectangle_test(&data,
						fbc_dirty_rectangle_outside_visible_region);
				}
				single_pipe = data.is_simulation;
				if (single_pipe)
					break;
			}
		}
	}

	igt_subtest_with_dynamic("fbc-dirty-rectangle-dirtyfb-tests") {
		bool single_pipe = false;
		data.feature = FEATURE_FBC;

		for_each_crtc(&data.display, crtc) {
			data.pipe = crtc->pipe;
			if (single_pipe)
				break;
			for_each_valid_output_on_pipe(&data.display,
						      crtc->pipe, data.output) {
				data.format = DRM_FORMAT_XRGB8888;

				igt_dynamic_f("pipe-%s-%s",
					       igt_crtc_name(crtc),
					       igt_output_name(data.output)) {
					fbc_dirty_rectangle_test(&data,
							fbc_dirty_rectangle_dirtyfb);
				}
				single_pipe = data.is_simulation;
				if (single_pipe)
					break;
			}
		}
	}

	igt_subtest_with_dynamic("fbc-dirty-rectangle-different-formats") {
		uint32_t formats[] = {DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888, DRM_FORMAT_RGB565};
		int num_formats = ARRAY_SIZE(formats);
		bool single_pipe = false;
		data.feature = FEATURE_FBC;

		for_each_crtc(&data.display, crtc) {
			data.pipe = crtc->pipe;
			if (single_pipe)
				break;
			for_each_valid_output_on_pipe(&data.display,
						      crtc->pipe, data.output) {
				for (int i = 0; i < num_formats; i++) {
					/* on simulation platforms , limit to single format */
					if (data.is_simulation && i > 0)
						break;

					igt_dynamic_f("pipe-%s-%s-format-%s",
						       igt_crtc_name(crtc),
						       igt_output_name(data.output),
						       igt_format_str(formats[i])) {
						data.format = formats[i];
						fbc_dirty_rectangle_test(&data,
							fbc_dirty_rectangle_basic);
					}
				}
				single_pipe = data.is_simulation;
				if (single_pipe)
					break;
			}
		}
	}

	igt_fixture() {
		igt_display_fini(&data.display);
		close(data.drm_fd);
	}
}
