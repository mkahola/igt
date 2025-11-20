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
 */

/**
 * TEST: kms psr2 sf
 * Category: Display
 * Description: Tests to verify PSR2 selective fetch by sending multiple damaged
 *              areas with and without fbc
 * Driver requirement: i915, xe
 * Mega feature: PSR
 */

#include "igt.h"
#include "igt_sysfs.h"
#include "igt_psr.h"
#include "kms_dsc_helper.h"
#include "i915/intel_fbc.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/**
 * SUBTEST: psr2-%s-plane-move-continuous-%s
 * Description: Test that selective fetch works on moving %arg[1] plane %arg[2]
 *              visible area (no update)
 *
 * SUBTEST: pr-%s-plane-move-continuous-%s
 * Mega feature: Panel Replay
 * Description: Test that selective fetch works on moving %arg[1] plane %arg[2]
 *              visible area (no update)
 *
 * SUBTEST: fbc-psr2-%s-plane-move-continuous-%s
 * Description: Test that fbc with selective fetch works on moving %arg[1] plane %arg[2]
 *              visible area (no update)
 *
 * SUBTEST: fbc-pr-%s-plane-move-continuous-%s
 * Mega feature: Panel Replay
 * Description: Test that fbc with selective fetch works on moving %arg[1] plane %arg[2]
 *              visible area (no update)
 *
 * arg[1]:
 *
 * @cursor:               Cursor
 * @overlay:              Overlay
 *
 * arg[2]:
 *
 * @exceed-fully-sf:      exceeding fully
 * @exceed-sf:            exceeding paritally
 * @sf:                   default
 */

/**
 * SUBTEST: psr2-cursor-plane-update-sf
 * Description: Test that selective fetch works on cursor plane
 *
 * SUBTEST: pr-cursor-plane-update-sf
 * Mega feature: Panel Replay
 * Description: Test that selective fetch works on cursor plane
 *
 * SUBTEST: fbc-psr2-cursor-plane-update-sf
 * Description: Test that fbc with selective fetch works on cursor plane
 *
 * SUBTEST: fbc-pr-cursor-plane-update-sf
 * Mega feature: Panel Replay
 * Description: Test that fbc with selective fetch works on cursor plane
 *
 * SUBTEST: psr2-overlay-plane-update-continuous-sf
 * Description: Test that selective fetch works on overlay plane
 *
 * SUBTEST: pr-overlay-plane-update-continuous-sf
 * Mega feature: Panel Replay
 * Description: Test that selective fetch works on overlay plane
 *
 * SUBTEST: fbc-psr2-overlay-plane-update-sf-dmg-area
 * Description: Test that fbc with selective fetch works on overlay plane
 *
 * SUBTEST: fbc-pr-overlay-plane-update-sf-dmg-area
 * Mega feature: Panel Replay
 * Description: Test that fbc with selective fetch works on overlay plane
 *
 * SUBTEST: psr2-overlay-plane-update-sf-dmg-area
 * Description: Test that selective fetch works on overlay plane
 *
 * SUBTEST: pr-overlay-plane-update-sf-dmg-area
 * Mega feature: Panel Replay
 * Description: Test that selective fetch works on overlay plane
 *
 * SUBTEST: fbc-psr2-overlay-plane-update-continuous-sf
 * Description: Test that fbc with selective fetch works on overlay plane
 *
 * SUBTEST: fbc-pr-overlay-plane-update-continuous-sf
 * Mega feature: Panel Replay
 * Description: Test that fbc with selective fetch works on overlay plane
 *
 * SUBTEST: psr2-overlay-primary-update-sf-dmg-area
 * Description: Test that selective fetch works on primary plane with blended
 *              overlay plane
 *
 * SUBTEST: pr-overlay-primary-update-sf-dmg-area
 * Mega feature: Panel Replay
 * Description: Test that selective fetch works on primary plane with blended
 *              overlay plane
 *
 * SUBTEST: fbc-psr2-overlay-primary-update-sf-dmg-area
 * Description: Test that fbc with selective fetch works on primary plane with blended
 *              overlay plane
 *
 * SUBTEST: fbc-pr-overlay-primary-update-sf-dmg-area
 * Mega feature: Panel Replay
 * Description: Test that fbc with selective fetch works on primary plane with blended
 *              overlay plane
 *
 * SUBTEST: psr2-plane-move-sf-dmg-area
 * Description: Test that selective fetch works on moving overlay plane
 *
 * SUBTEST: pr-plane-move-sf-dmg-area
 * Mega feature: Panel Replay
 * Description: Test that selective fetch works on moving overlay plane
 *
 * SUBTEST: fbc-psr2-plane-move-sf-dmg-area
 * Description: Test that fbc with selective fetch works on moving overlay plane
 *
 * SUBTEST: fbc-pr-plane-move-sf-dmg-area
 * Mega feature: Panel Replay
 * Description: Test that fbc with selective fetch works on moving overlay plane
 *
 * SUBTEST: psr2-primary-plane-update-sf-dmg-area
 * Description: Test that selective fetch works on primary plane
 *
 * SUBTEST: pr-primary-plane-update-sf-dmg-area
 * Mega feature: Panel Replay
 * Description: Test that selective fetch works on primary plane
 *
 * SUBTEST: fbc-psr2-primary-plane-update-sf-dmg-area
 * Description: Test that fbc with selective fetch works on primary plane
 *
 * SUBTEST: fbc-pr-primary-plane-update-sf-dmg-area
 * Mega feature: Panel Replay
 * Description: Test that fbc with selective fetch works on primary plane
 *
 * SUBTEST: psr2-primary-plane-update-sf-dmg-area-big-fb
 * Description: Test that selective fetch works on primary plane with big fb
 *
 * SUBTEST: pr-primary-plane-update-sf-dmg-area-big-fb
 * Mega feature: Panel Replay
 * Description: Test that selective fetch works on primary plane with big fb
 */

IGT_TEST_DESCRIPTION("Tests to verify PSR2 selective fetch by sending multiple"
		     " damaged areas with and without fbc");

#define SQUARE_SIZE 100

#define CUR_SIZE 64
#define MAX_DAMAGE_AREAS 5

#define MAX_SCREEN_CHANGES 5

enum operations {
	PLANE_UPDATE,
	PLANE_UPDATE_CONTINUOUS,
	PLANE_MOVE,
	PLANE_MOVE_CONTINUOUS,
	PLANE_MOVE_CONTINUOUS_EXCEED,
	PLANE_MOVE_CONTINUOUS_EXCEED_FULLY,
	OVERLAY_PRIM_UPDATE
};

enum plane_move_postion {
	POS_TOP_LEFT,
	POS_TOP_RIGHT,
	POS_BOTTOM_LEFT,
	POS_BOTTOM_RIGHT,
	POS_CENTER,
	POS_TOP,
	POS_BOTTOM,
	POS_LEFT,
	POS_RIGHT,
};

typedef struct {
	int drm_fd;
	uint32_t devid;
	int debugfs_fd;
	igt_display_t display;
	drmModeModeInfo *mode;
	igt_output_t *output;
	struct igt_fb fb_primary, fb_overlay, fb_cursor;
	struct igt_fb fb_test;
	struct igt_fb *fb_continuous;
	uint32_t primary_format;
	int damage_area_count;
	int big_fb_width, big_fb_height;
	struct drm_mode_rect plane_update_clip[MAX_DAMAGE_AREAS];
	struct drm_mode_rect plane_move_clip;
	struct drm_mode_rect cursor_clip;
	enum operations op;
	enum fbc_mode op_fbc_mode;
	enum plane_move_postion pos;
	int test_plane_id;
	igt_plane_t *test_plane;
	bool big_fb_test;
	bool fbc_flag;
	bool et_flag;
	cairo_t *cr;
	uint32_t screen_changes;
	int cur_x, cur_y;
	enum pipe pipe;
	enum psr_mode psr_mode;
	enum {
		FEATURE_NONE  = 0,
		FEATURE_DSC   = 1,
		FEATURE_COUNT = 2,
	} coexist_feature;
} data_t;

static bool set_sel_fetch_mode_for_output(data_t *data)
{
	bool supported = false;

	data->et_flag = false;

	if (psr_sink_support(data->drm_fd, data->debugfs_fd,
						 PR_MODE_SEL_FETCH_ET, data->output)) {
		supported = true;
		data->psr_mode = PR_MODE_SEL_FETCH;
		data->et_flag = true;
	} else if (psr_sink_support(data->drm_fd, data->debugfs_fd,
							PR_MODE_SEL_FETCH, data->output)) {
		supported = true;
		data->psr_mode = PR_MODE_SEL_FETCH;
	} else if (psr_sink_support(data->drm_fd, data->debugfs_fd,
							PSR_MODE_2_ET, data->output)) {
		supported = true;
		data->psr_mode = PSR_MODE_2;
		data->et_flag = true;
	} else	if (psr_sink_support(data->drm_fd, data->debugfs_fd,
							  PSR_MODE_2, data->output)) {
		supported = true;
		data->psr_mode = PSR_MODE_2;
	} else
		igt_info("selective fetch not supported on output %s\n", data->output->name);

	if (supported)
		supported = psr_enable(data->drm_fd, data->debugfs_fd, data->psr_mode,
				       data->output);

	return supported;
}

static const char *op_str(enum operations op)
{
	static const char * const name[] = {
		[PLANE_UPDATE] = "plane-update",
		[PLANE_UPDATE_CONTINUOUS] = "plane-update-continuous",
		[PLANE_MOVE_CONTINUOUS] = "plane-move-continuous",
		[PLANE_MOVE_CONTINUOUS_EXCEED] = "plane-move-continuous-exceed",
		[PLANE_MOVE_CONTINUOUS_EXCEED_FULLY] =
		"plane-move-continuous-exceed-fully",
		[PLANE_MOVE] = "plane-move",
		[OVERLAY_PRIM_UPDATE] = "overlay-primary-update",
	};

	return name[op];
}

static const char *coexist_feature_str(int coexist_feature)
{
	switch (coexist_feature) {
	case FEATURE_NONE:
		return "";
	case FEATURE_DSC:
		return "-dsc";
	default:
		igt_assert(false);
	}
}

static void display_init(data_t *data)
{
	igt_display_require(&data->display, data->drm_fd);
	igt_display_reset(&data->display);
}

static void display_fini(data_t *data)
{
	igt_display_fini(&data->display);
}

static void draw_rect(data_t *data, igt_fb_t *fb, int x, int y, int w, int h,
			double r, double g, double b, double a)
{
	cairo_t *cr;

	cr = igt_get_cairo_ctx(data->drm_fd, fb);
	igt_paint_color_alpha(cr, x, y, w, h, r, g, b, a);
	igt_put_cairo_ctx(cr);
}

static void set_clip(struct drm_mode_rect *clip, int x, int y, int width,
		     int height)
{
	clip->x1 = x;
	clip->y1 = y;
	clip->x2 = x + width;
	clip->y2 = y + height;
}

static void plane_update_setup_squares(data_t *data, igt_fb_t *fb, uint32_t h,
				       uint32_t v, int pos_x, int pos_y)
{
	int x, y;
	int width = SQUARE_SIZE;
	int height = SQUARE_SIZE;

	switch (data->damage_area_count) {
	case 5:
		/*Bottom right corner*/
		x = pos_x + h - SQUARE_SIZE;
		y = pos_y + v - SQUARE_SIZE;

		draw_rect(data, fb, x, y, width, height, 1.0, 1.0, 1.0, 1.0);
		set_clip(&data->plane_update_clip[4], x, y, width, height);
	case 4:
		/*Bottom left corner*/
		x = pos_x;
		y = pos_y + v - SQUARE_SIZE;

		draw_rect(data, fb, x, y, width, height, 1.0, 1.0, 1.0, 1.0);
		set_clip(&data->plane_update_clip[3], x, y, width, height);
	case 3:
		/*Top right corner*/
		x = pos_x + h - SQUARE_SIZE;
		y = pos_y;

		draw_rect(data, fb, x, y, width, height, 1.0, 1.0, 1.0, 1.0);
		set_clip(&data->plane_update_clip[2], x, y, width, height);
	case 2:
		/*Top left corner*/
		x = pos_x;
		y = pos_y;

		draw_rect(data, fb, x, y, width, height, 1.0, 1.0, 1.0, 1.0);
		set_clip(&data->plane_update_clip[1], x, y, width, height);
	case 1:
		/*Center*/
		x = pos_x + h / 2 - SQUARE_SIZE / 2;
		y = pos_y + v / 2 - SQUARE_SIZE / 2;

		draw_rect(data, fb, x, y, width, height, 1.0, 1.0, 1.0, 1.0);
		set_clip(&data->plane_update_clip[0], x, y, width, height);
		break;
	default:
		igt_assert(false);
	}
}

static void plane_move_setup_square(data_t *data, igt_fb_t *fb, uint32_t h,
				    uint32_t v, int pos_x, int pos_y)
{
	int x = 0, y = 0;

	switch (data->pos) {
	case POS_TOP_LEFT:
		/*Bottom right corner*/
		x = pos_x + h - SQUARE_SIZE;
		y = pos_y + v - SQUARE_SIZE;
		break;
	case POS_TOP_RIGHT:
		/*Bottom left corner*/
		x = pos_x;
		y = pos_y + v - SQUARE_SIZE;
		break;
	case POS_BOTTOM_LEFT:
		/*Top right corner*/
		x = pos_x + h - SQUARE_SIZE;
		y = pos_y + 0;
		break;
	case POS_BOTTOM_RIGHT:
		/*Top left corner*/
		x = pos_x;
		y = pos_y;
		break;
	default:
		igt_assert(false);
	}

	draw_rect(data, fb, x, y,
		  SQUARE_SIZE, SQUARE_SIZE, 1.0, 1.0, 1.0, 1.0);
	set_clip(&data->plane_move_clip, x, y, SQUARE_SIZE, SQUARE_SIZE);
}

static void prepare(data_t *data)
{
	igt_output_t *output = data->output;
	igt_plane_t *primary, *sprite = NULL, *cursor = NULL;
	int fb_w, fb_h, x, y, view_w, view_h;

	data->mode = igt_output_get_mode(output);

	if (data->coexist_feature & FEATURE_DSC) {
		save_force_dsc_en(data->drm_fd, output);
		force_dsc_enable(data->drm_fd, output);
		igt_output_set_pipe(output, PIPE_NONE);
		igt_display_commit2(&data->display, COMMIT_ATOMIC);
	}

	igt_output_set_pipe(output, data->pipe);

	if (data->big_fb_test) {
		fb_w = data->big_fb_width;
		fb_h = data->big_fb_height;
		x = fb_w / 2;
		y = fb_h / 2;
		view_w = data->mode->hdisplay;
		view_h = data->mode->vdisplay;
	} else {
		fb_w = view_w = data->mode->hdisplay;
		fb_h = view_h = data->mode->vdisplay;
		x = y = 0;
	}

	/* all green frame */
	igt_create_color_fb(data->drm_fd, fb_w, fb_h,
			    data->primary_format,
			    DRM_FORMAT_MOD_LINEAR,
			    0.0, 1.0, 0.0,
			    &data->fb_primary);

	primary = igt_output_get_plane_type(output,
			DRM_PLANE_TYPE_PRIMARY);

	switch (data->test_plane_id) {
	case DRM_PLANE_TYPE_OVERLAY:
		sprite = igt_output_get_plane_type(output,
						   DRM_PLANE_TYPE_OVERLAY);
		/*All blue plane*/
		igt_create_color_fb(data->drm_fd,
				    fb_w / 2, fb_h / 2,
				    DRM_FORMAT_XRGB8888,
				    DRM_FORMAT_MOD_LINEAR,
				    0.0, 0.0, 1.0,
				    &data->fb_overlay);

		igt_create_color_fb(data->drm_fd,
				    fb_w / 2, fb_h / 2,
				    DRM_FORMAT_XRGB8888,
				    DRM_FORMAT_MOD_LINEAR,
				    0.0, 0.0, 1.0,
				    &data->fb_test);

		data->fb_continuous = &data->fb_overlay;

		if (data->op == PLANE_MOVE) {
			plane_move_setup_square(data, &data->fb_test,
						view_w / 2, view_h / 2,
						x, y);

		} else {
			plane_update_setup_squares(data, &data->fb_test,
						   view_w / 2, view_h / 2,
						   x, y);
		}

		igt_plane_set_fb(sprite, &data->fb_overlay);
		igt_fb_set_position(&data->fb_overlay, sprite, x, y);
		igt_fb_set_size(&data->fb_overlay, primary, view_w / 2,
				view_h / 2);
		igt_plane_set_size(sprite, view_w / 2, view_h / 2);
		data->test_plane = sprite;
		break;

	case DRM_PLANE_TYPE_PRIMARY:
		igt_create_color_fb(data->drm_fd, fb_w, fb_h,
				    DRM_FORMAT_XRGB8888,
				    DRM_FORMAT_MOD_LINEAR,
				    0.0, 1.0, 0.0,
				    &data->fb_test);

		plane_update_setup_squares(data, &data->fb_test,
					   view_w, view_h, x, y);
		data->fb_continuous = &data->fb_primary;
		data->test_plane = primary;

		if (data->op == OVERLAY_PRIM_UPDATE) {
			sprite = igt_output_get_plane_type(output,
						   DRM_PLANE_TYPE_OVERLAY);

			igt_create_color_fb(data->drm_fd, fb_w, fb_h,
					    DRM_FORMAT_XRGB8888,
					    DRM_FORMAT_MOD_LINEAR,
					    0.0, 0.0, 1.0,
					    &data->fb_overlay);

			igt_plane_set_fb(sprite, &data->fb_overlay);
			igt_fb_set_position(&data->fb_overlay, sprite, x, y);
			igt_fb_set_size(&data->fb_overlay, primary, view_w,
					view_h);
			igt_plane_set_size(sprite, view_w, view_h);
			igt_plane_set_prop_value(sprite, IGT_PLANE_ALPHA,
						 0x6060);
		}
		break;

	case DRM_PLANE_TYPE_CURSOR:
		cursor = igt_output_get_plane_type(output,
						   DRM_PLANE_TYPE_CURSOR);
		igt_plane_set_position(cursor, 0, 0);

		igt_create_fb(data->drm_fd, CUR_SIZE, CUR_SIZE,
			      DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR,
			      &data->fb_cursor);

		draw_rect(data, &data->fb_cursor, 0, 0, CUR_SIZE, CUR_SIZE,
			    0.0, 0.0, 1.0, 1.0);

		igt_create_fb(data->drm_fd, CUR_SIZE, CUR_SIZE,
			      DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR,
			      &data->fb_test);
		data->fb_continuous = &data->fb_cursor;

		draw_rect(data, &data->fb_test, 0, 0, CUR_SIZE, CUR_SIZE,
			    1.0, 1.0, 1.0, 1.0);

		set_clip(&data->cursor_clip, 0, 0, CUR_SIZE, CUR_SIZE);
		igt_plane_set_fb(cursor, &data->fb_cursor);
		data->test_plane = cursor;
		break;
	default:
		igt_assert(false);
	}

	igt_plane_set_fb(primary, &data->fb_primary);
	igt_fb_set_position(&data->fb_primary, primary, x, y);
	igt_fb_set_size(&data->fb_primary, primary, view_w,
			view_h);
	igt_plane_set_size(primary, view_w, view_h);
	igt_plane_set_position(primary, 0, 0);
	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	igt_skip_on_f(IS_BATTLEMAGE(data->devid) && data->op_fbc_mode == FBC_ENABLED,
		      "FBC isn't supported on BMG\n");

	if (data->coexist_feature & FEATURE_DSC)
		igt_require_f(igt_is_dsc_enabled(data->drm_fd, output->name),
			      "DSC is not enabled\n");
	if (data->op_fbc_mode == FBC_ENABLED)
		igt_require_f(data->fbc_flag,
			      "Can't test FBC with PSR\n");
}

static inline void manual(const char *expected)
{
	igt_debug_interactive_mode_check("all", expected);
}

static void plane_update_expected_output(int plane_type, int box_count,
					 int screen_changes)
{
	char expected[64] = {};

	switch (plane_type) {
	case DRM_PLANE_TYPE_PRIMARY:
		sprintf(expected, "screen Green with %d White box(es)",
			box_count);
		break;
	case DRM_PLANE_TYPE_OVERLAY:
		/*
		 * Continuous updates only for DRM_PLANE_TYPE_OVERLAY
		 * for now.
		 */
		if (screen_changes & 1)
			sprintf(expected, "screen Green with Blue box");
		else
			sprintf(expected,
				"screen Green with Blue box and %d White box(es)",
				box_count);
		break;
	case DRM_PLANE_TYPE_CURSOR:
		sprintf(expected, "screen Green with %d White box(es)",
			box_count);
		break;
	default:
		igt_assert(false);
	}

	manual(expected);
}

static void plane_move_expected_output(enum plane_move_postion pos)
{
	char expected[64] = {};

	switch (pos) {
	case POS_TOP_LEFT:
		sprintf(expected,
			"screen Green with Blue box on top left corner and White box");
		break;
	case POS_TOP_RIGHT:
		sprintf(expected,
			"screen Green with Blue box on top right corner and White box");
		break;
	case POS_BOTTOM_LEFT:
		sprintf(expected,
			"screen Green with Blue box on bottom left corner and White box");
		break;
	case POS_BOTTOM_RIGHT:
		sprintf(expected,
			"screen Green with Blue box on bottom right corner and White box");
		break;
	default:
		igt_assert(false);
	}

	manual(expected);
}

static void plane_move_continuous_expected_output(data_t *data)
{
	char expected[128] = {};
	int ret = 0;

	switch (data->pos) {
	case POS_TOP_LEFT:
		ret = sprintf(expected,
			      "screen Green with Blue box on top left corner");
		break;
	case POS_TOP_RIGHT:
		ret = sprintf(expected,
			      "screen Green with Blue box on top right corner");
		break;
	case POS_BOTTOM_LEFT:
		ret = sprintf(expected,
			      "screen Green with Blue box on bottom left corner");
		break;
	case POS_BOTTOM_RIGHT:
		ret = sprintf(expected,
			      "screen Green with Blue box on bottom right corner");
		break;
	case POS_CENTER:
		ret = sprintf(expected, "screen Green with Blue box on center");
		break;
	case POS_TOP:
		ret = sprintf(expected, "screen Green with Blue box on top");
		break;
	case POS_BOTTOM:
		ret = sprintf(expected, "screen Green with Blue box on bottom");
		break;
	case POS_LEFT:
		ret = sprintf(expected, "screen Green with Blue box on left");
		break;
	case POS_RIGHT:
		ret = sprintf(expected, "screen Green with Blue box on right");
		break;
	default:
		igt_assert(false);
	}

	if (ret) {
		if (data->op == PLANE_MOVE_CONTINUOUS_EXCEED)
			sprintf(expected + ret, "(partly exceeding area)");
		else if (data->op == PLANE_MOVE_CONTINUOUS_EXCEED_FULLY)
			sprintf(expected + ret, "(fully exceeding area)");
	}

	manual(expected);
}

static void overlay_prim_update_expected_output(int box_count)
{
	char expected[64] = {};

	sprintf(expected,
		"screen Green with Blue overlay, %d light Blue box(es)",
		box_count);

	manual(expected);

}

static void expected_output(data_t *data)
{
	switch (data->op) {
	case PLANE_MOVE:
		plane_move_expected_output(data->pos);
		break;
	case PLANE_MOVE_CONTINUOUS:
	case PLANE_MOVE_CONTINUOUS_EXCEED:
	case PLANE_MOVE_CONTINUOUS_EXCEED_FULLY:
		plane_move_continuous_expected_output(data);
		break;
	case PLANE_UPDATE:
		plane_update_expected_output(data->test_plane_id,
					     data->damage_area_count,
					     data->screen_changes);
		break;
	case PLANE_UPDATE_CONTINUOUS:
		plane_update_expected_output(data->test_plane_id,
					     data->damage_area_count,
					     data->screen_changes);
		break;
	case OVERLAY_PRIM_UPDATE:
		overlay_prim_update_expected_output(data->damage_area_count);
		break;
	default:
		igt_assert(false);
	}
}

static void damaged_plane_move(data_t *data)
{
	igt_plane_t *test_plane = data->test_plane;
	uint32_t h = data->mode->hdisplay;
	uint32_t v = data->mode->vdisplay;
	int x, y;

	if (data->big_fb_test) {
		x = data->big_fb_width / 2;
		y = data->big_fb_height / 2;
	} else {
		x = y = 0;
	}

	if (data->test_plane_id == DRM_PLANE_TYPE_OVERLAY) {
		h = h/2;
		v = v/2;
	}

	igt_plane_set_fb(test_plane, &data->fb_test);

	igt_fb_set_position(&data->fb_test, test_plane, x,
			    y);
	igt_fb_set_size(&data->fb_test, test_plane, h, v);
	igt_plane_set_size(test_plane, h, v);

	igt_plane_replace_prop_blob(test_plane, IGT_PLANE_FB_DAMAGE_CLIPS,
				    &data->plane_move_clip,
				    sizeof(struct drm_mode_rect));

	switch (data->pos) {
	case POS_TOP_LEFT:
		igt_plane_set_position(data->test_plane, 0, 0);
		break;
	case POS_TOP_RIGHT:
		igt_plane_set_position(data->test_plane,
				       data->mode->hdisplay/2, 0);
		break;
	case POS_BOTTOM_LEFT:
		igt_plane_set_position(data->test_plane, 0,
				       data->mode->vdisplay/2);
		break;
	case POS_BOTTOM_RIGHT:
		igt_plane_set_position(data->test_plane,
				       data->mode->hdisplay/2,
				       data->mode->vdisplay/2);
		break;
	default:
		igt_assert(false);
	}

	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	igt_assert(psr_wait_entry(data->debugfs_fd, data->psr_mode, data->output));

	expected_output(data);
}
static void get_target_coords(data_t *data, int *x, int *y)
{
	int target_x, target_y, exceed_x, exceed_y;

	switch (data->pos) {
	case POS_TOP_LEFT:
		target_x = 0;
		target_y = 0;
		break;
	case POS_TOP_RIGHT:
		target_x = data->mode->hdisplay - data->fb_test.width;
		target_y = 0;
		break;
	case POS_BOTTOM_LEFT:
		target_x = 0;
		target_y = data->mode->vdisplay - data->fb_test.height;
		break;
	case POS_BOTTOM_RIGHT:
		target_x = data->mode->hdisplay - data->fb_test.width;
		target_y = data->mode->vdisplay - data->fb_test.height;
		break;
	case POS_CENTER:
		target_x = data->mode->hdisplay / 2;
		target_y = data->mode->vdisplay / 2;
		break;
	case POS_BOTTOM:
		target_x = data->mode->hdisplay / 2;
		target_y = data->mode->vdisplay - data->fb_test.height;
		break;
	case POS_TOP:
		target_x = data->mode->hdisplay / 2;
		target_y = 0;
		break;
	case POS_RIGHT:
		target_x = data->mode->hdisplay - data->fb_test.width;
		target_y = data->mode->vdisplay / 2;
		break;
	case POS_LEFT:
		target_x = 0;
		target_y = data->mode->vdisplay / 2;
		break;
	default:
		igt_assert(false);
	}

	if (data->op == PLANE_MOVE_CONTINUOUS_EXCEED) {
		exceed_x  = data->fb_test.width / 2;
		exceed_y  = data->fb_test.height / 2;
	} else if (data->op == PLANE_MOVE_CONTINUOUS_EXCEED_FULLY) {
		exceed_x  = data->fb_test.width;
		exceed_y  = data->fb_test.height;
	}

	if (data->op != PLANE_MOVE_CONTINUOUS) {
		switch (data->pos) {
		case POS_TOP_LEFT:
			target_x -= exceed_x;
			target_y -= exceed_y;
			break;
		case POS_TOP_RIGHT:
			target_x += exceed_x;
			target_y -= exceed_y;
			break;
		case POS_BOTTOM_LEFT:
			target_x -= exceed_x;
			target_y += exceed_y;
			break;
		case POS_BOTTOM_RIGHT:
			target_x += exceed_x;
			target_y += exceed_y;
			break;
		case POS_BOTTOM:
			target_y += exceed_y;
			break;
		case POS_TOP:
			target_y -= exceed_y;
			break;
		case POS_RIGHT:
			target_x += exceed_x;
			break;
		case POS_LEFT:
			target_x -= exceed_x;
			break;
		case POS_CENTER:
			break;
		}
	}

	*x = target_x;
	*y = target_y;
}

static void plane_move_continuous(data_t *data)
{
	int target_x, target_y;

	igt_assert(psr_wait_entry(data->debugfs_fd, data->psr_mode, data->output));

	get_target_coords(data, &target_x, &target_y);

	while (data->cur_x != target_x || data->cur_y != target_y) {
		if (data->cur_x < target_x)
			data->cur_x += min(target_x - data->cur_x, 20);
		else if (data->cur_x > target_x)
			data->cur_x -= min(data->cur_x - target_x, 20);

		if (data->cur_y < target_y)
			data->cur_y += min(target_y - data->cur_y, 20);
		else if (data->cur_y > target_y)
			data->cur_y -= min(data->cur_y - target_y, 20);

		igt_plane_set_position(data->test_plane, data->cur_x, data->cur_y);
		igt_display_commit2(&data->display, COMMIT_ATOMIC);
	}

	expected_output(data);
}

static void damaged_plane_update(data_t *data)
{
	igt_plane_t *test_plane = data->test_plane;
	struct igt_fb *fb_test;
	uint32_t h, v;
	int x, y;

	if (data->big_fb_test) {
		x = data->big_fb_width / 2;
		y = data->big_fb_height / 2;
	} else {
		x = y = 0;
	}

	switch (data->test_plane_id) {
	case DRM_PLANE_TYPE_OVERLAY:
		h = data->mode->hdisplay / 2;
		v = data->mode->vdisplay / 2;
		break;
	case DRM_PLANE_TYPE_PRIMARY:
		h = data->mode->hdisplay;
		v = data->mode->vdisplay;
		break;
	case DRM_PLANE_TYPE_CURSOR:
		h = v = CUR_SIZE;
		break;
	default:
		igt_assert(false);
	}

	if (data->screen_changes & 1)
		fb_test = data->fb_continuous;
	else
		fb_test = &data->fb_test;

	igt_plane_set_fb(test_plane, fb_test);

	if (data->test_plane_id == DRM_PLANE_TYPE_CURSOR)
		igt_plane_replace_prop_blob(test_plane,
					    IGT_PLANE_FB_DAMAGE_CLIPS,
					    &data->cursor_clip,
					    sizeof(struct drm_mode_rect));
	else
		igt_plane_replace_prop_blob(test_plane,
					    IGT_PLANE_FB_DAMAGE_CLIPS,
					    &data->plane_update_clip,
					    sizeof(struct drm_mode_rect)*
					    data->damage_area_count);

	igt_fb_set_position(fb_test, test_plane, x, y);
	igt_fb_set_size(fb_test, test_plane, h, v);
	igt_plane_set_size(test_plane, h, v);
	igt_plane_set_position(data->test_plane, 0, 0);
	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	igt_assert(psr_wait_entry(data->debugfs_fd, data->psr_mode, data->output));

	expected_output(data);
}

static void run(data_t *data)
{
	int i;

	igt_assert(psr_wait_entry(data->debugfs_fd, data->psr_mode, data->output));

	if (data->fbc_flag == true && data->op_fbc_mode == FBC_ENABLED)
		igt_assert_f(intel_fbc_wait_until_enabled(data->drm_fd,
							  data->pipe),
							  "FBC still disabled\n");

	/* TODO: Enable this check if other connectors support Early Transport */
	if (data->et_flag && data->output != NULL &&
	    data->output->config.connector->connector_type == DRM_MODE_CONNECTOR_eDP)
		igt_assert_f(early_transport_check(data->debugfs_fd),
			     "Early Transport Disabled\n");

	data->screen_changes = 0;

	switch (data->op) {
	case PLANE_UPDATE:
	case OVERLAY_PRIM_UPDATE:
		damaged_plane_update(data);
		break;
	case PLANE_UPDATE_CONTINUOUS:
		for (data->screen_changes = 0;
		     data->screen_changes < MAX_SCREEN_CHANGES;
		     data->screen_changes++) {
			damaged_plane_update(data);
		}
		break;
	case PLANE_MOVE:
		damaged_plane_move(data);
		break;
	case PLANE_MOVE_CONTINUOUS:
	case PLANE_MOVE_CONTINUOUS_EXCEED:
	case PLANE_MOVE_CONTINUOUS_EXCEED_FULLY:
		/*
		 * Start from top left corner and keep plane position
		 * over iterations.
		 */
		data->cur_x = data->cur_y = 0;
		for (i = POS_TOP_LEFT; i <= POS_RIGHT; i++) {
			data->pos = i;
			plane_move_continuous(data);
		}
		break;
	default:
		igt_assert(false);
	}

	psr_sink_error_check(data->debugfs_fd, data->psr_mode, data->output);
}

static void cleanup(data_t *data)
{
	igt_output_t *output = data->output;
	igt_plane_t *primary;
	igt_plane_t *sprite;

	primary = igt_output_get_plane_type(output,
					    DRM_PLANE_TYPE_PRIMARY);

	igt_plane_set_fb(primary, NULL);

	if (data->test_plane_id != DRM_PLANE_TYPE_PRIMARY) {
		igt_plane_set_position(data->test_plane, 0, 0);
		igt_plane_set_fb(data->test_plane, NULL);
	}

	if (data->op == OVERLAY_PRIM_UPDATE) {
		sprite = igt_output_get_plane_type(output,
				DRM_PLANE_TYPE_OVERLAY);
		igt_plane_set_position(sprite, 0, 0);
		igt_plane_set_fb(sprite, NULL);
	}

	if (data->coexist_feature & FEATURE_DSC)
		restore_force_dsc_en();

	igt_output_set_pipe(output, PIPE_NONE);

	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	igt_remove_fb(data->drm_fd, &data->fb_primary);
	igt_remove_fb(data->drm_fd, &data->fb_overlay);
	igt_remove_fb(data->drm_fd, &data->fb_cursor);
	igt_remove_fb(data->drm_fd, &data->fb_test);
}

static bool check_pr_psr2_sel_fetch_support(data_t *data)
{
	bool status = false;

	/* Check sink supports PR/PSR2 selective fetch */
	if (!set_sel_fetch_mode_for_output(data))
		return false;

	/* Check if selective fetch can be enabled */
	if (!selective_fetch_check(data->debugfs_fd, data->output))
		igt_assert("Selective fetch is not enabled even though panel should support it\n");

	prepare(data);
	/* We enter into DEEP_SLEEP for both PSR2 and PR sel fetch */
	status = psr_wait_entry(data->debugfs_fd, data->psr_mode, data->output);
	cleanup(data);
	return status;
}

static bool
pipe_output_combo_valid(igt_display_t *display,
			enum pipe pipe, igt_output_t *output)
{
	bool ret = true;

	igt_display_reset(display);

	igt_output_set_pipe(output, pipe);
	if (!intel_pipe_output_combo_valid(display))
		ret = false;
	igt_output_set_pipe(output, PIPE_NONE);

	return ret;
}

static bool check_psr_mode_supported(data_t *data, int psr_stat)
{
	if (data->psr_mode == psr_stat)
		return true;
	else
		return false;
}

static void run_dynamic_test_damage_areas(data_t data, int i, int coexist_features[])
{
	for (int j = FEATURE_NONE; j < FEATURE_COUNT; j++) {
		if (j != FEATURE_NONE && !(coexist_features[i] & j))
			continue;
		igt_dynamic_f("pipe-%s-%s%s", kmstest_pipe_name(data.pipe),
			      igt_output_name(data.output), coexist_feature_str(j)) {
			data.coexist_feature = j;
			for (int k = 1; k <= MAX_DAMAGE_AREAS; k++) {
				data.damage_area_count = k;
				prepare(&data);
				run(&data);
				cleanup(&data);
			}
		}
	}
}

static void run_dynamic_test(data_t data, int i, int coexist_features[])
{
	for (int j = FEATURE_NONE; j < FEATURE_COUNT; j++) {
		if (j != FEATURE_NONE && !(coexist_features[i] & j))
			continue;
		igt_dynamic_f("pipe-%s-%s%s", kmstest_pipe_name(data.pipe),
			      igt_output_name(data.output), coexist_feature_str(j)) {
			data.coexist_feature = j;
			prepare(&data);
			run(&data);
			cleanup(&data);
		}
	}
}

static void run_plane_move(data_t data, int i, int coexist_features[])
{
	for (int j = FEATURE_NONE; j < FEATURE_COUNT; j++) {
		if (j != FEATURE_NONE && !(coexist_features[i] & j))
			continue;
		igt_dynamic_f("pipe-%s-%s%s", kmstest_pipe_name(data.pipe),
			      igt_output_name(data.output), coexist_feature_str(j)) {
			data.test_plane_id = DRM_PLANE_TYPE_OVERLAY;
			data.coexist_feature = j;
			for (int k = POS_TOP_LEFT; k <= POS_BOTTOM_RIGHT; k++) {
				data.pos = k;
				prepare(&data);
				run(&data);
				cleanup(&data);
			}
		}
	}
}

static void run_plane_update_continuous(data_t data, int i, int coexist_features[])
{
	for (int j = FEATURE_NONE; j < FEATURE_COUNT; j++) {
		if (j != FEATURE_NONE && !(coexist_features[i] & j))
			continue;
		igt_dynamic_f("pipe-%s-%s%s", kmstest_pipe_name(data.pipe),
			      igt_output_name(data.output), coexist_feature_str(j)) {
			data.damage_area_count = 1;
			if (data.op_fbc_mode == FBC_ENABLED)
				data.primary_format = DRM_FORMAT_XRGB8888;
			else
				data.primary_format = DRM_FORMAT_NV12;

			data.test_plane_id = DRM_PLANE_TYPE_OVERLAY;
			data.coexist_feature = j;
			prepare(&data);
			run(&data);
			cleanup(&data);
		}
	}
}

igt_main()
{
	bool output_supports_pr_psr2_sel_fetch = false;
	bool pr_psr2_sel_fetch_supported = false;
	data_t data = {};
	igt_output_t *outputs[IGT_MAX_PIPES * IGT_MAX_PIPES];
	int i, y, z;
	int pipes[IGT_MAX_PIPES * IGT_MAX_PIPES];
	int n_pipes = 0;
	int coexist_features[IGT_MAX_PIPES * IGT_MAX_PIPES];
	const char *append_fbc_subtest[2] = {
		"",
		"fbc-"
	};
	int fbc_status[] = {FBC_DISABLED, FBC_ENABLED};

	const char *append_psr_subtest[2] = {
		"psr2-",
		"pr-"
	};
	int psr_status[] = {PSR_MODE_2, PR_MODE_SEL_FETCH};
	bool fbc_chipset_support;
	int disp_ver;

	igt_fixture() {
		drmModeResPtr res;

		data.drm_fd = drm_open_driver_master(DRIVER_INTEL | DRIVER_XE);
		data.debugfs_fd = igt_debugfs_dir(data.drm_fd);
		kmstest_set_vt_graphics_mode();

		display_init(&data);

		data.devid = intel_get_drm_devid(data.drm_fd);
		disp_ver = intel_display_ver(data.devid);
		fbc_chipset_support = intel_fbc_supported_on_chipset(data.drm_fd, data.pipe);

		data.damage_area_count = MAX_DAMAGE_AREAS;
		data.primary_format = DRM_FORMAT_XRGB8888;

		res = drmModeGetResources(data.drm_fd);
		data.big_fb_width = res->max_width;
		data.big_fb_height = res->max_height;
		igt_info("Big framebuffer size %dx%d\n",
			 data.big_fb_width, data.big_fb_height);

		for_each_pipe_with_valid_output(&data.display, data.pipe, data.output) {
			coexist_features[n_pipes] = 0;
			output_supports_pr_psr2_sel_fetch = check_pr_psr2_sel_fetch_support(&data);
			if (output_supports_pr_psr2_sel_fetch) {
				pipes[n_pipes] = data.pipe;
				outputs[n_pipes] = data.output;

				if (is_dsc_supported_by_sink(data.drm_fd, data.output))
					coexist_features[n_pipes] |= FEATURE_DSC;

				n_pipes++;
			}
			pr_psr2_sel_fetch_supported |= output_supports_pr_psr2_sel_fetch;
		}
		igt_require_f(pr_psr2_sel_fetch_supported,
					  "No output supports selective fetch\n");
	}

	for (y = 0; y < ARRAY_SIZE(fbc_status); y++) {
		for (z = 0; z < ARRAY_SIZE(psr_status); z++) {
			data.op = PLANE_UPDATE;
			data.test_plane_id = DRM_PLANE_TYPE_PRIMARY;
			data.primary_format = DRM_FORMAT_XRGB8888;
			data.big_fb_test = 0;

			data.op_fbc_mode = fbc_status[y];
			data.psr_mode = psr_status[z];
			data.fbc_flag = fbc_chipset_support &&
					intel_fbc_supported_for_psr_mode(disp_ver, data.psr_mode);

			/* Verify primary plane selective fetch */
			igt_describe("Test that selective fetch works on primary plane");
			igt_subtest_with_dynamic_f("%s%sprimary-%s-sf-dmg-area",
						   append_fbc_subtest[y],
						   append_psr_subtest[z],
						   op_str(data.op)) {
				for (i = 0; i < n_pipes; i++) {
					if (!pipe_output_combo_valid(&data.display,
								     pipes[i], outputs[i]))
						continue;
					data.pipe = pipes[i];
					data.output = outputs[i];
					igt_assert_f(set_sel_fetch_mode_for_output(&data),
						     "Selective fetch is not supported\n");

					if (!check_psr_mode_supported(&data, psr_status[z]))
						continue;

					data.test_plane_id = DRM_PLANE_TYPE_PRIMARY;
					run_dynamic_test_damage_areas(data, i, coexist_features);
				}
			}

			/* Verify primary plane selective fetch with big fb */
			if (data.op_fbc_mode == FBC_DISABLED) {
				data.big_fb_test = 1;
				igt_describe("Test that selective fetch works on primary plane "
					     "with big fb");
				igt_subtest_with_dynamic_f("%s%sprimary-%s-sf-dmg-area-big-fb",
							   append_fbc_subtest[y],
							   append_psr_subtest[z],
							   op_str(data.op)) {
					for (i = 0; i < n_pipes; i++) {
						if (!pipe_output_combo_valid(&data.display,
									     pipes[i], outputs[i]))
							continue;
						data.pipe = pipes[i];
						data.output = outputs[i];
						igt_assert_f(set_sel_fetch_mode_for_output(&data),
							     "Selective fetch is not supported\n");
						if (!check_psr_mode_supported(&data, psr_status[z]))
							continue;

						data.test_plane_id = DRM_PLANE_TYPE_PRIMARY;
						run_dynamic_test_damage_areas(data, i,
									      coexist_features);
					}
				}
			}

			data.big_fb_test = 0;
			/* Verify overlay plane selective fetch */
			igt_describe("Test that selective fetch works on overlay plane");
			igt_subtest_with_dynamic_f("%s%soverlay-%s-sf-dmg-area",
						   append_fbc_subtest[y],
						   append_psr_subtest[z],
						   op_str(data.op)) {
				for (i = 0; i < n_pipes; i++) {
					if (!pipe_output_combo_valid(&data.display,
								     pipes[i],
								     outputs[i]))
						continue;
					data.pipe = pipes[i];
					data.output = outputs[i];
					igt_assert_f(set_sel_fetch_mode_for_output(&data),
						     "Selective fetch is not supported\n");
					if (!check_psr_mode_supported(&data, psr_status[z]))
						continue;

					data.test_plane_id = DRM_PLANE_TYPE_OVERLAY;
					run_dynamic_test_damage_areas(data, i, coexist_features);
				}
			}

			data.damage_area_count = 1;
			/* Verify cursor plane selective fetch */
			igt_describe("Test that selective fetch works on cursor plane");
			igt_subtest_with_dynamic_f("%s%scursor-%s-sf", append_fbc_subtest[y],
						   append_psr_subtest[z], op_str(data.op)) {
				for (i = 0; i < n_pipes; i++) {
					if (!pipe_output_combo_valid(&data.display,
								     pipes[i],
								     outputs[i]))
						continue;
					data.pipe = pipes[i];
					data.output = outputs[i];
					igt_assert_f(set_sel_fetch_mode_for_output(&data),
						     "Selective fetch is not supported\n");
					if (!check_psr_mode_supported(&data, psr_status[z]))
						continue;

					data.test_plane_id = DRM_PLANE_TYPE_CURSOR;
					run_dynamic_test(data, i, coexist_features);
				}
			}

			data.op = PLANE_MOVE_CONTINUOUS;
			igt_describe("Test that selective fetch works on "
				     "moving cursor plane (no update)");
			igt_subtest_with_dynamic_f("%s%scursor-%s-sf", append_fbc_subtest[y],
						   append_psr_subtest[z], op_str(data.op)) {
				for (i = 0; i < n_pipes; i++) {
					if (!pipe_output_combo_valid(&data.display,
								     pipes[i],
								     outputs[i]))
						continue;
					data.pipe = pipes[i];
					data.output = outputs[i];
					igt_assert_f(set_sel_fetch_mode_for_output(&data),
						     "Selective fetch is not supported\n");
					if (!check_psr_mode_supported(&data, psr_status[z]))
						continue;

					data.test_plane_id = DRM_PLANE_TYPE_CURSOR;
					run_dynamic_test(data, i, coexist_features);
				}
			}

			data.op = PLANE_MOVE_CONTINUOUS_EXCEED;
			igt_describe("Test that selective fetch works on moving cursor "
				     "plane exceeding partially visible area (no update)");
			igt_subtest_with_dynamic_f("%s%scursor-%s-sf", append_fbc_subtest[y],
						   append_psr_subtest[z], op_str(data.op)) {
				for (i = 0; i < n_pipes; i++) {
					if (!pipe_output_combo_valid(&data.display,
								     pipes[i],
								     outputs[i]))
						continue;
					data.pipe = pipes[i];
					data.output = outputs[i];
					igt_assert_f(set_sel_fetch_mode_for_output(&data),
						     "Selective fetch is not supported\n");
					if (!check_psr_mode_supported(&data, psr_status[z]))
						continue;

					data.test_plane_id = DRM_PLANE_TYPE_CURSOR;
					run_dynamic_test(data, i, coexist_features);
				}
			}

			data.op = PLANE_MOVE_CONTINUOUS_EXCEED_FULLY;
			igt_describe("Test that selective fetch works on moving cursor plane "
				     "exceeding fully visible area (no update)");
			igt_subtest_with_dynamic_f("%s%scursor-%s-sf", append_fbc_subtest[y],
						   append_psr_subtest[z], op_str(data.op)) {
				for (i = 0; i < n_pipes; i++) {
					if (!pipe_output_combo_valid(&data.display,
								     pipes[i],
								     outputs[i]))
						continue;
					data.pipe = pipes[i];
					data.output = outputs[i];
					igt_assert_f(set_sel_fetch_mode_for_output(&data),
						     "Selective fetch is not supported\n");
					if (!check_psr_mode_supported(&data, psr_status[z]))
						continue;

					data.test_plane_id = DRM_PLANE_TYPE_CURSOR;
					run_dynamic_test(data, i, coexist_features);
				}
			}

			/* Only for overlay plane */
			data.op = PLANE_MOVE;
			/* Verify overlay plane move selective fetch */
			igt_describe("Test that selective fetch works on moving overlay plane");
			igt_subtest_with_dynamic_f("%s%s%s-sf-dmg-area", append_fbc_subtest[y],
						   append_psr_subtest[z], op_str(data.op)) {
				for (i = 0; i < n_pipes; i++) {
					if (!pipe_output_combo_valid(&data.display,
								     pipes[i],
								     outputs[i]))
						continue;
					data.pipe = pipes[i];
					data.output = outputs[i];
					igt_assert_f(set_sel_fetch_mode_for_output(&data),
						     "Selective fetch is not supported\n");
					if (!check_psr_mode_supported(&data, psr_status[z]))
						continue;

					run_plane_move(data, i, coexist_features);
				}
			}

			data.op = PLANE_MOVE_CONTINUOUS;
			igt_describe("Test that selective fetch works on moving overlay "
				     "plane (no update)");
			igt_subtest_with_dynamic_f("%s%soverlay-%s-sf", append_fbc_subtest[y],
						   append_psr_subtest[z], op_str(data.op)) {
				for (i = 0; i < n_pipes; i++) {
					if (!pipe_output_combo_valid(&data.display,
								     pipes[i],
								     outputs[i]))
						continue;
					data.pipe = pipes[i];
					data.output = outputs[i];
					igt_assert_f(set_sel_fetch_mode_for_output(&data),
						     "Selective fetch is not supported\n");
					if (!check_psr_mode_supported(&data, psr_status[z]))
						continue;

					data.test_plane_id = DRM_PLANE_TYPE_OVERLAY;
					run_dynamic_test(data, i, coexist_features);
				}
			}

			data.op = PLANE_MOVE_CONTINUOUS_EXCEED;
			igt_describe("Test that selective fetch works on moving overlay "
				     "plane partially exceeding visible area (no update)");
			igt_subtest_with_dynamic_f("%s%soverlay-%s-sf", append_fbc_subtest[y],
						   append_psr_subtest[z], op_str(data.op)) {
				for (i = 0; i < n_pipes; i++) {
					if (!pipe_output_combo_valid(&data.display,
								     pipes[i],
								     outputs[i]))
						continue;
					data.pipe = pipes[i];
					data.output = outputs[i];
					igt_assert_f(set_sel_fetch_mode_for_output(&data),
						     "Selective fetch is not supported\n");
					if (!check_psr_mode_supported(&data, psr_status[z]))
						continue;

					data.test_plane_id = DRM_PLANE_TYPE_OVERLAY;
					run_dynamic_test(data, i, coexist_features);
				}
			}

			data.op = PLANE_MOVE_CONTINUOUS_EXCEED_FULLY;
			igt_describe("Test that selective fetch works on moving overlay plane "
				     "fully exceeding visible area (no update)");
			igt_subtest_with_dynamic_f("%s%soverlay-%s-sf", append_fbc_subtest[y],
						   append_psr_subtest[z], op_str(data.op)) {
				for (i = 0; i < n_pipes; i++) {
					if (!pipe_output_combo_valid(&data.display, pipes[i],
								     outputs[i]))
						continue;
					data.pipe = pipes[i];
					data.output = outputs[i];
					igt_assert_f(set_sel_fetch_mode_for_output(&data),
						     "Selective fetch is not supported\n");
					if (!check_psr_mode_supported(&data, psr_status[z]))
						continue;

					data.test_plane_id = DRM_PLANE_TYPE_OVERLAY;
					run_dynamic_test(data, i, coexist_features);
				}
			}

			/* Verify primary plane selective fetch with overplay plane blended */
			data.op = OVERLAY_PRIM_UPDATE;
			igt_describe("Test that selective fetch works on primary plane "
				     "with blended overlay plane");
			igt_subtest_with_dynamic_f("%s%s%s-sf-dmg-area", append_fbc_subtest[y],
						   append_psr_subtest[z], op_str(data.op)) {
				for (i = 0; i < n_pipes; i++) {
					if (!pipe_output_combo_valid(&data.display,
								     pipes[i],
								     outputs[i]))
						continue;
					data.pipe = pipes[i];
					data.output = outputs[i];
					igt_assert_f(set_sel_fetch_mode_for_output(&data),
						     "Selective fetch is not supported\n");
					if (!check_psr_mode_supported(&data, psr_status[z]))
						continue;

					data.test_plane_id = DRM_PLANE_TYPE_PRIMARY;
					run_dynamic_test_damage_areas(data, i, coexist_features);
				}
			}

			/*
			 * Verify overlay plane selective fetch using NV12 primary
			 * plane and continuous updates.
			 */
			data.op = PLANE_UPDATE_CONTINUOUS;
			igt_describe("Test that selective fetch works on overlay plane");
			igt_subtest_with_dynamic_f("%s%soverlay-%s-sf", append_fbc_subtest[y],
						   append_psr_subtest[z], op_str(data.op)) {
				for (i = 0; i < n_pipes; i++) {
					if (!pipe_output_combo_valid(&data.display,
								     pipes[i],
								     outputs[i]))
						continue;
					data.pipe = pipes[i];
					data.output = outputs[i];
					igt_assert_f(set_sel_fetch_mode_for_output(&data),
						     "Selective fetch is not supported\n");
					if (!check_psr_mode_supported(&data, psr_status[z]))
						continue;

					run_plane_update_continuous(data, i, coexist_features);
				}
			}
		}
	}

	igt_fixture() {
		close(data.debugfs_fd);
		display_fini(&data);
		drm_close_driver(data.drm_fd);
	}
}
