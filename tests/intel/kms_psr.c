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
 */

/**
 * TEST: kms psr
 * Category: Display
 * Description: Tests behaviour of PSR & PSR2 & PR
 * Driver requirement: i915, xe
 * Mega feature: PSR
 */

#include "i915/intel_fbc.h"
#include "igt.h"
#include "igt_sysfs.h"
#include "igt_psr.h"
#include "intel_mocs.h"
#include "intel_pat.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"

/**
 * SUBTEST: %s-basic
 * Description: Basic check for %arg[1] if it is detecting changes made in planes
 *
 * SUBTEST: fbc-%s-basic
 * Description: Basic check for fbc with %arg[1] if it is detecting changes made in planes
 *
 * SUBTEST: %s-dpms
 * Description: Check if %arg[1] is detecting changes when rendering operation
 *              is performed with dpms enabled or disabled
 *
 * SUBTEST: fbc-%s-dpms
 * Description: Check if fbc with %arg[1] is detecting changes when rendering operation
 *              is performed with dpms enabled or disabled
 *
 * SUBTEST: %s-no-drrs
 * Description: Check if %arg[1] is detecting changes when drrs is disabled
 *
 * SUBTEST: fbc-%s-no-drrs
 * Description: Check if fbc with %arg[1] is detecting changes when drrs is disabled
 *
 * SUBTEST: %s-suspend
 * Description: Check if %arg[1] is detecting changes when plane operation is
 *              performed with suspend resume cycles
 *
 * SUBTEST: fbc-%s-suspend
 * Description: Check if fbc with %arg[1] is detecting changes when plane operation is
 *              performed with suspend resume cycles
 *
 * arg[1]:
 *
 * @psr:		psr1
 * @psr2:		psr2
 */

/**
 * SUBTEST: %s-%s-%s
 * Description: Check if %arg[1] is detecting memory mapping %arg[3] operations
 * 		performed on %arg[2] planes
 * Driver requirement: i915
 *
 * SUBTEST: fbc-%s-%s-%s
 * Description: Check if fbc with %arg[1] is detecting memory mapping %arg[3] operations
 *              performed on %arg[2] planes
 * Driver requirement: i915
 *
 * arg[1]:
 *
 * @psr:		psr1
 * @psr2:		psr2
 *
 * arg[2]:
 *
 * @cursor:             Cursor plane
 * @primary:            Primary plane
 * @sprite:             Sprite plane
 *
 * arg[3]:
 *
 * @mmap-cpu:           MMAP CPU
 * @mmap-gtt:           MMAP GTT
 */

/**
 * SUBTEST: %s-primary-page-flip
 * Description: Check if %arg[1] is detecting page-flipping operation
 * 		performed on primary plane
 *
 * SUBTEST: fbc-%s-primary-page-flip
 * Description: Check if fbc with %arg[1] is detecting page-flipping operation
 *              performed on primary plane
 *
 * SUBTEST: %s-primary-%s
 * Description: Check if %arg[1] is detecting rendering operations %arg[2]
 * 		when performed on primary plane
 *
 * SUBTEST: fbc-%s-primary-%s
 * Description: Check if %arg[1] is detecting rendering operations %arg[2]
 *              when performed on primary plane
 *
 * arg[1]:
 *
 * @psr:		psr1
 * @psr2:		psr2
 *
 * arg[2]:
 *
 * @blt:                Blitter
 * @render:             Render
 */

/**
 * SUBTEST: %s-%s-%s
 * Description: Check if %arg[1] is detecting rendering and plane
 *              operations %arg[3] performed on %arg[2] planes
 *
 * SUBTEST: fbc-%s-%s-%s
 * Description: Check if fbc with %arg[1] is detecting rendering and plane
 *              operations %arg[3] performed on %arg[2] planes
 *
 * arg[1]:
 *
 * @psr:		psr1
 * @psr2:		psr2
 *
 * arg[2]:
 *
 * @cursor:             Cursor plane
 * @sprite:             Sprite plane
 *
 * arg[3]:
 *
 * @blt:                Blitter
 * @render:             Render
 * @plane-onoff:        Plane On off
 * @plane-move:         Move plane position
 */

/**
 * SUBTEST: pr-basic
 * Description: Basic check for pr if it is detecting changes made in planes
 * Mega feature: Panel Replay
 *
 * SUBTEST: fbc-pr-basic
 * Description: Basic check for fbc with pr if it is detecting changes made in planes
 * Mega feature: Panel Replay
 *
 * SUBTEST: pr-dpms
 * Description: Check if pr is detecting changes when rendering operation
 *              is performed with dpms enabled or disabled
 * Mega feature: Panel Replay
 *
 * SUBTEST: fbc-pr-dpms
 * Description: Check if fbc with pr is detecting changes when rendering operation
 *              is performed with dpms enabled or disabled
 * Mega feature: Panel Replay
 *
 * SUBTEST: pr-no-drrs
 * Description: Check if pr is detecting changes when drrs is disabled
 * Mega feature: Panel Replay
 *
 * SUBTEST: fbc-pr-no-drrs
 * Description: Check if fbc with pr is detecting changes when drrs is disabled
 * Mega feature: Panel Replay
 *
 * SUBTEST: pr-suspend
 * Description: Check if pr is detecting changes when plane operation is
 *              performed with suspend resume cycles
 * Mega feature: Panel Replay
 *
 * SUBTEST: fbc-pr-suspend
 * Description: Check if fbc with pr is detecting changes when plane operation is
 *              performed with suspend resume cycles
 * Mega feature: Panel Replay
 */

/**
 * SUBTEST: pr-%s-%s
 * Description: Check if pr is detecting memory mapping %arg[2] operations
 * 		performed on %arg[1] planes
 * Driver requirement: i915
 * Mega feature: Panel Replay
 *
 * SUBTEST: fbc-pr-%s-%s
 * Description: Check if fbc with pr is detecting memory mapping %arg[2] operations
 *              performed on %arg[1] planes
 * Driver requirement: i915
 * Mega feature: Panel Replay
 *
 * arg[1]:
 *
 * @cursor:             Cursor plane
 * @primary:            Primary plane
 * @sprite:             Sprite plane
 *
 * arg[2]:
 *
 * @mmap-cpu:           MMAP CPU
 * @mmap-gtt:           MMAP GTT
 */

/**
 * SUBTEST: pr-primary-page-flip
 * Description: Check if pr is detecting page-flipping operation
 * 		performed on primary plane
 * Mega feature: Panel Replay
 *
 * SUBTEST: fbc-pr-primary-page-flip
 * Description: Check if fbc with pr is detecting page-flipping operation
 *              performed on primary plane
 * Mega feature: Panel Replay
 *
 * SUBTEST: pr-primary-%s
 * Description: Check if pr is detecting rendering operations %arg[1]
 * 		when performed on primary plane
 * Mega feature: Panel Replay
 *
 * SUBTEST: fbc-pr-primary-%s
 * Description: Check if fbc with pr is detecting rendering operations %arg[1]
 *              when performed on primary plane
 * Mega feature: Panel Replay
 *
 * arg[1]:
 *
 * @blt:                Blitter
 * @render:             Render
 */

/**
 * SUBTEST: pr-%s-%s
 * Description: Check if pr is detecting rendering and plane
 *              operations %arg[2] performed on %arg[1] planes
 * Mega feature: Panel Replay
 *
 * SUBTEST: fbc-pr-%s-%s
 * Description: Check if fbc with pr is detecting rendering and plane
 *              operations %arg[2] performed on %arg[1] planes
 * Mega feature: Panel Replay
 *
 * arg[1]:
 *
 * @cursor:             Cursor plane
 * @sprite:             Sprite plane
 *
 * arg[2]:
 *
 * @blt:                Blitter
 * @render:             Render
 * @plane-onoff:        Plane On off
 * @plane-move:         Move plane position
 */

enum operations {
	PAGE_FLIP,
	MMAP_GTT,
	MMAP_CPU,
	BLT,
	RENDER,
	PLANE_MOVE,
	PLANE_ONOFF,
};

static const char *op_str(enum operations op)
{
	static const char * const name[] = {
		[PAGE_FLIP] = "page-flip",
		[MMAP_GTT] = "mmap-gtt",
		[MMAP_CPU] = "mmap-cpu",
		[BLT] = "blt",
		[RENDER] = "render",
		[PLANE_MOVE] = "plane-move",
		[PLANE_ONOFF] = "plane-onoff",
	};

	return name[op];
}

typedef struct {
	int drm_fd;
	int debugfs_fd;
	enum operations op;
	int test_plane_id;
	enum psr_mode op_psr_mode;
	enum fbc_mode op_fbc_mode;
	uint32_t devid;
	uint32_t crtc_id;
	igt_display_t display;
	struct buf_ops *bops;
	struct igt_fb fb_green, fb_white;
	igt_plane_t *test_plane;
	int mod_size;
	int mod_stride;
	drmModeModeInfo *mode;
	igt_output_t *output;
	bool fbc_flag;
} data_t;

static void create_cursor_fb(data_t *data)
{
	cairo_t *cr;
	uint32_t fb_id;

	fb_id = igt_create_fb(data->drm_fd, 64, 64,
			      DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR,
			      &data->fb_white);
	igt_assert(fb_id);

	cr = igt_get_cairo_ctx(data->drm_fd, &data->fb_white);
	igt_paint_color_alpha(cr, 0, 0, 64, 64, 1.0, 1.0, 1.0, 1.0);
	igt_put_cairo_ctx(cr);
}

static bool output_supports_psr(data_t *data)
{
	igt_output_t *output;

	for_each_connected_output(&data->display, output) {
		if (psr_sink_support(data->drm_fd, data->debugfs_fd,
				    PSR_MODE_2, output) ||
		   psr_sink_support(data->drm_fd, data->debugfs_fd,
				    PSR_MODE_1, output) ||
		   psr_sink_support(data->drm_fd, data->debugfs_fd,
				    PR_MODE, output))
			return true;
	}
	return false;
}

static void display_fini(data_t *data)
{
	igt_display_fini(&data->display);
}

static void color_blit_start(struct intel_bb *ibb)
{
	intel_bb_out(ibb, XY_COLOR_BLT_CMD_NOLEN |
		     COLOR_BLT_WRITE_ALPHA |
		     XY_COLOR_BLT_WRITE_RGB |
		     (4 + (ibb->gen >= 8)));
}

static struct intel_buf *create_buf_from_fb(data_t *data,
					    const struct igt_fb *fb)
{
	uint32_t name, handle, tiling, stride, width, height, bpp, size;
	struct intel_buf *buf;
	enum intel_driver driver = buf_ops_get_driver(data->bops);
	uint64_t region = (driver == INTEL_DRIVER_XE) ?
				vram_if_possible(data->drm_fd, 0) : -1;

	igt_assert_eq(fb->offsets[0], 0);

	tiling = igt_fb_mod_to_tiling(fb->modifier);
	stride = fb->strides[0];
	bpp = fb->plane_bpp[0];
	size = fb->size;
	width = stride / (bpp / 8);
	height = size / stride;

	buf = calloc(1, sizeof(*buf));
	igt_assert(buf);

	name = gem_flink(data->drm_fd, fb->gem_handle);
	handle = gem_open(data->drm_fd, name);
	buf = intel_buf_create_full(data->bops, handle, width, height,
				    bpp, 0, tiling, 0, size, stride, region,
				    intel_get_pat_idx_uc(data->drm_fd),
				    DEFAULT_MOCS_INDEX);
	intel_buf_set_ownership(buf, true);

	return buf;
}

static void fill_blt(data_t *data, const struct igt_fb *fb, unsigned char color)
{
	struct intel_bb *ibb;
	struct intel_buf *dst;

	ibb = intel_bb_create(data->drm_fd, 4096);
	dst = create_buf_from_fb(data, fb);
	intel_bb_add_intel_buf(ibb, dst, true);

	color_blit_start(ibb);
	intel_bb_out(ibb, (1 << 24) | (0xf0 << 16) | 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0xfff << 16 | 0xfff);
	intel_bb_emit_reloc(ibb, dst->handle, I915_GEM_DOMAIN_RENDER,
			    I915_GEM_DOMAIN_RENDER, 0, dst->addr.offset);
	intel_bb_out(ibb, color);

	intel_bb_flush_blit(ibb);
	intel_bb_destroy(ibb);
	intel_buf_destroy(dst);

	if (is_i915_device(data->drm_fd))
		gem_bo_busy(data->drm_fd, fb->gem_handle);
}

static void fill_render(data_t *data, const struct igt_fb *fb,
			unsigned char color)
{
	struct intel_buf *src, *dst;
	struct intel_bb *ibb;
	const uint8_t buf[4] = { color, color, color, color };
	igt_render_copyfunc_t rendercopy = igt_get_render_copyfunc(data->drm_fd);
	int height, width, tiling;

	igt_skip_on(!rendercopy);

	ibb = intel_bb_create(data->drm_fd, 4096);
	dst = create_buf_from_fb(data, fb);

	width = fb->strides[0] / (fb->plane_bpp[0] / 8);
	height = fb->size / fb->strides[0];
	tiling = igt_fb_mod_to_tiling(fb->modifier);

	src = intel_buf_create(data->bops, width, height, fb->plane_bpp[0],
			       0, tiling, 0);

	if (is_i915_device(data->drm_fd)) {
		gem_write(data->drm_fd, src->handle, 0, buf, 4);
	} else {
		void *map = xe_bo_mmap_ext(data->drm_fd, src->handle, 4,
					   PROT_READ | PROT_WRITE);

		memcpy(map, buf, 4);
		gem_munmap(map, 4);
	}

	rendercopy(ibb,
		   src, 0, 0, 0xff, 0xff,
		   dst, 0, 0);

	intel_bb_destroy(ibb);
	intel_buf_destroy(src);
	intel_buf_destroy(dst);

	if (is_i915_device(data->drm_fd))
		gem_bo_busy(data->drm_fd, fb->gem_handle);
}

static bool psr_wait_entry_if_enabled(data_t *data)
{
	igt_skip_on_f(!is_psr_enable_possible(data->drm_fd, data->op_psr_mode),
		      "enable_psr modparam doesn't allow psr mode %d\n",
		      data->op_psr_mode);

	return psr_wait_entry(data->debugfs_fd, data->op_psr_mode, data->output);
}

static bool psr_wait_update_if_enabled(data_t *data)
{
	igt_skip_on_f(!is_psr_enable_possible(data->drm_fd, data->op_psr_mode),
		      "enable_psr modparam doesn't allow psr mode %d\n",
		      data->op_psr_mode);

	return psr_wait_update(data->debugfs_fd, data->op_psr_mode, data->output);
}

static bool psr_enable_if_enabled(data_t *data)
{
	igt_skip_on_f(!is_psr_enable_possible(data->drm_fd, data->op_psr_mode),
		      "enable_psr modparam doesn't allow psr mode %d\n",
		      data->op_psr_mode);

	return psr_enable(data->drm_fd, data->debugfs_fd, data->op_psr_mode, data->output);
}

static inline void manual(const char *expected)
{
	igt_debug_interactive_mode_check("all", expected);
}

static bool drrs_disabled(data_t *data)
{
	char buf[512];

	/*
	 * FIXME: As of now, XE's debugfs is using i915 namespace, once Kernel
	 * changes got landed, please update this logic to use XE specific
	 * debugfs.
	 */
	igt_debugfs_simple_read(data->debugfs_fd, "i915_drrs_status",
			 buf, sizeof(buf));

	return !strstr(buf, "DRRS Enabled : Yes\n");
}

static void fb_dirty_fb_ioctl(data_t *data, struct igt_fb *fb)
{
	int ret;
	drmModeClip clip = {
		.x1 = 0,
		.x2 = fb->width,
		.y1 = 0,
		.y2 = fb->height
	};

	/* Cursor planes don't support frontbuffer rendering */
	if (data->test_plane->type == DRM_PLANE_TYPE_CURSOR) {
		igt_plane_set_fb(data->test_plane, &data->fb_white);
		igt_display_commit(&data->display);
		return;
	}

	ret = drmModeDirtyFB(data->drm_fd, fb->fb_id, &clip, 1);
	igt_assert(ret == 0 || ret == -ENOSYS);
}

static void run_test(data_t *data)
{
	uint32_t handle = data->fb_white.gem_handle;
	igt_plane_t *test_plane = data->test_plane;
	void *ptr;
	const char *expected = "";

	/* Confirm that screen became Green */
	manual("screen GREEN");

	/* Confirm screen stays Green after PSR got active */
	igt_assert(psr_wait_entry_if_enabled(data));
	manual("screen GREEN");

	/* Setting a secondary fb/plane */
	igt_plane_set_fb(test_plane, &data->fb_white);
	igt_display_commit(&data->display);

	/* Confirm it is not Green anymore */
	if (test_plane->type == DRM_PLANE_TYPE_PRIMARY)
		manual("screen WHITE");
	else
		manual("GREEN background with WHITE box");

	igt_assert(psr_wait_entry_if_enabled(data));
	switch (data->op) {
	case PAGE_FLIP:
		/* Only in use when testing primary plane */
		igt_assert(drmModePageFlip(data->drm_fd, data->crtc_id,
					   data->fb_green.fb_id, 0, NULL) == 0);
		expected = "GREEN";
		break;
	case MMAP_GTT:
		gem_require_mappable_ggtt(data->drm_fd);
		ptr = gem_mmap__gtt(data->drm_fd, handle, data->mod_size,
				    PROT_WRITE);
		gem_set_domain(data->drm_fd, handle,
			       I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
		memset(ptr, 0xcc, data->mod_size);
		munmap(ptr, data->mod_size);
		fb_dirty_fb_ioctl(data, &data->fb_white);
		expected = "BLACK or TRANSPARENT mark on top of plane in test";
		break;
	case MMAP_CPU:
		ptr = gem_mmap__cpu(data->drm_fd, handle, 0, data->mod_size,
				    PROT_WRITE);
		gem_set_domain(data->drm_fd, handle,
			       I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);
		memset(ptr, 0, data->mod_size);
		munmap(ptr, data->mod_size);
		gem_sw_finish(data->drm_fd, handle);
		fb_dirty_fb_ioctl(data, &data->fb_white);
		expected = "BLACK or TRANSPARENT mark on top of plane in test";
		break;
	case BLT:
		fill_blt(data, &data->fb_white, 0);
		fb_dirty_fb_ioctl(data, &data->fb_white);
		expected = "BLACK or TRANSPARENT mark on top of plane in test";
		break;
	case RENDER:
		fill_render(data, &data->fb_white, 0);
		fb_dirty_fb_ioctl(data, &data->fb_white);
		expected = "BLACK or TRANSPARENT mark on top of plane in test";
		break;
	case PLANE_MOVE:
		/* Only in use when testing Sprite and Cursor */
		igt_plane_set_position(test_plane, 500, 500);
		igt_display_commit(&data->display);
		expected = "White box moved to 500x500";
		break;
	case PLANE_ONOFF:
		/* Only in use when testing Sprite and Cursor */
		igt_plane_set_fb(test_plane, NULL);
		igt_display_commit(&data->display);
		expected = "screen GREEN";
		break;
	}
	igt_assert(psr_wait_update_if_enabled(data));
	manual(expected);
}

static void test_cleanup(data_t *data)
{
	igt_plane_t *primary;

	psr_sink_error_check(data->debugfs_fd, data->op_psr_mode, data->output);

	igt_output_override_mode(data->output, NULL);

	primary = igt_output_get_plane_type(data->output,
					    DRM_PLANE_TYPE_PRIMARY);
	igt_plane_set_fb(primary, NULL);
	igt_plane_set_fb(data->test_plane, NULL);
	igt_display_commit(&data->display);

	igt_remove_fb(data->drm_fd, &data->fb_green);
	igt_remove_fb(data->drm_fd, &data->fb_white);
	psr_disable(data->drm_fd, data->debugfs_fd, data->output);
}

static void setup_test_plane(data_t *data, int test_plane)
{
	uint32_t white_h, white_v;
	igt_plane_t *primary, *sprite, *cursor;

	igt_create_color_fb(data->drm_fd,
			    data->mode->hdisplay, data->mode->vdisplay,
			    DRM_FORMAT_XRGB8888,
			    DRM_FORMAT_MOD_LINEAR,
			    0.0, 1.0, 0.0,
			    &data->fb_green);

	primary = igt_output_get_plane_type(data->output,
					    DRM_PLANE_TYPE_PRIMARY);
	igt_plane_set_fb(primary, NULL);
	data->test_plane = primary;

	white_h = data->mode->hdisplay;
	white_v = data->mode->vdisplay;

	/* Ignoring pitch and bpp to avoid changing full screen */
	data->mod_size = white_h * white_v;
	data->mod_stride = white_h * 4;

	switch (test_plane) {
	case DRM_PLANE_TYPE_OVERLAY:
		sprite = igt_output_get_plane_type(data->output,
						   DRM_PLANE_TYPE_OVERLAY);
		igt_plane_set_fb(sprite, NULL);
		white_h = white_h/2;
		white_v = white_v/2;
		data->test_plane = sprite;
	case DRM_PLANE_TYPE_PRIMARY:
		igt_create_color_fb(data->drm_fd,
				    white_h, white_v,
				    DRM_FORMAT_XRGB8888,
				    DRM_FORMAT_MOD_LINEAR,
				    1.0, 1.0, 1.0,
				    &data->fb_white);
		break;
	case DRM_PLANE_TYPE_CURSOR:
		cursor = igt_output_get_plane_type(data->output,
						   DRM_PLANE_TYPE_CURSOR);
		igt_plane_set_fb(cursor, NULL);
		create_cursor_fb(data);
		igt_plane_set_position(cursor, 0, 0);

		/* Cursor is 64 x 64, ignoring pitch and bbp again */
		data->mod_size = 64 * 64;
		data->test_plane = cursor;
		break;
	}

	igt_display_commit(&data->display);

	igt_plane_set_fb(primary, &data->fb_green);
	igt_display_commit(&data->display);
}

static enum pipe get_pipe_for_output(igt_display_t *display,
				     igt_output_t *output)
{
	enum pipe pipe;

	for_each_pipe(display, pipe) {
		igt_output_set_pipe(output, pipe);

		if (!intel_pipe_output_combo_valid(display)) {
			igt_output_set_pipe(output, PIPE_NONE);
			continue;
		}

		return pipe;
	}

	igt_assert_f(false, "No pipe found for output %s\n",
		     igt_output_name(output));
}

static void test_setup(data_t *data)
{
	enum pipe pipe;
	drmModeConnectorPtr connector;
	bool psr_entered = false;

	igt_require_f(data->output,
		      "No available output found\n");


	igt_skip_on_f(IS_BATTLEMAGE(data->devid) && data->op_fbc_mode == FBC_ENABLED,
		      "FBC isn't supported on BMG\n");

	if (data->op_fbc_mode == FBC_ENABLED)
		igt_require_f(data->fbc_flag,
			      "Can't test FBC with PSR\n");

	pipe = get_pipe_for_output(&data->display, data->output);
	data->crtc_id = data->output->config.crtc->crtc_id;
	connector = data->output->config.connector;

	for (int i = 0; i < connector->count_modes; i++) {
		data->mode = &connector->modes[i];
		igt_info("Testing mode:\n");
		kmstest_dump_mode(data->mode);

		igt_output_override_mode(data->output, data->mode);

		if (!intel_pipe_output_combo_valid(&data->display)) {
			igt_info("Skipping mode, not compatible with selected pipe/output\n");
			continue;
		}

		psr_enable_if_enabled(data);
		setup_test_plane(data, data->test_plane_id);
		if (psr_wait_entry_if_enabled(data)) {
			if (data->fbc_flag == true && data->op_fbc_mode == FBC_ENABLED)
				igt_assert_f(intel_fbc_wait_until_enabled(data->drm_fd,
									  pipe),
									  "FBC still disabled\n");
			psr_entered = true;
			break;
		}
	}

	igt_assert(psr_entered);
}

static void dpms_off_on(data_t *data)
{
	kmstest_set_connector_dpms(data->drm_fd, data->output->config.connector,
				   DRM_MODE_DPMS_OFF);
	kmstest_set_connector_dpms(data->drm_fd, data->output->config.connector,
				   DRM_MODE_DPMS_ON);
}

data_t data = {};

igt_main
{
	int z, y;
	enum operations op;
	enum pipe pipe;
	const char *append_subtest_name[3] = {
		"psr-",
		"psr2-",
		"pr-"
	};
	const char *append_fbc_subtest[2] = {
		"",
		"fbc-"
	};
	int modes[] = {PSR_MODE_1, PSR_MODE_2, PR_MODE};
	int fbc_status[] = {FBC_DISABLED, FBC_ENABLED};
	igt_output_t *output;
	bool fbc_chipset_support;
	int disp_ver;

	igt_fixture() {
		data.drm_fd = drm_open_driver_master(DRIVER_INTEL | DRIVER_XE);
		data.debugfs_fd = igt_debugfs_dir(data.drm_fd);
		kmstest_set_vt_graphics_mode();
		data.devid = intel_get_drm_devid(data.drm_fd);
		data.bops = buf_ops_create(data.drm_fd);
		igt_display_require(&data.display, data.drm_fd);
		igt_require_f(output_supports_psr(&data), "Sink does not support PSR/PSR2/PR\n");
		disp_ver = intel_display_ver(data.devid);
		fbc_chipset_support = intel_fbc_supported_on_chipset(data.drm_fd, pipe);
	}

	for (y = 0; y < ARRAY_SIZE(fbc_status); y++) {
		data.op_fbc_mode = fbc_status[y];
		for (z = 0; z < ARRAY_SIZE(modes); z++) {
			data.op_psr_mode = modes[z];
			data.fbc_flag = fbc_chipset_support &&
					intel_fbc_supported_for_psr_mode(disp_ver,
									 data.op_psr_mode);

			igt_describe("Basic check for psr if it is detecting changes made "
				     "in planes");
			igt_subtest_with_dynamic_f("%s%sbasic", append_fbc_subtest[y],
						   append_subtest_name[z]) {
				for_each_connected_output(&data.display, output) {
					if (!psr_sink_support(data.drm_fd, data.debugfs_fd,
							      data.op_psr_mode, output))
						continue;
					igt_display_reset(&data.display);
					data.output = output;
					igt_dynamic_f("%s", data.output->name) {
						data.test_plane_id = DRM_PLANE_TYPE_PRIMARY;
						test_setup(&data);
						test_cleanup(&data);
					}
				}
			}

			igt_describe("Check if psr is detecting changes when drrs is disabled");
			igt_subtest_with_dynamic_f("%s%sno-drrs", append_fbc_subtest[y],
						   append_subtest_name[z]) {
				for_each_connected_output(&data.display, output) {
					if (!psr_sink_support(data.drm_fd, data.debugfs_fd,
							      data.op_psr_mode, output))
						continue;
					igt_display_reset(&data.display);
					data.output = output;
					igt_dynamic_f("%s", data.output->name) {
						data.test_plane_id = DRM_PLANE_TYPE_PRIMARY;
						test_setup(&data);
						igt_assert(drrs_disabled(&data));
						test_cleanup(&data);
					}
				}
			}

			for (op = PAGE_FLIP; op <= RENDER; op++) {
				igt_describe("Check if psr is detecting page-flipping,memory "
					     "mapping and rendering operations performed on "
					     "primary planes");
				igt_subtest_with_dynamic_f("%s%sprimary-%s", append_fbc_subtest[y],
						      append_subtest_name[z], op_str(op)) {
					igt_skip_on(is_xe_device(data.drm_fd) &&
						    (op == MMAP_CPU || op == MMAP_GTT));
					for_each_connected_output(&data.display, output) {
						if (!psr_sink_support(data.drm_fd, data.debugfs_fd,
								      data.op_psr_mode, output))
							continue;
						igt_display_reset(&data.display);
						data.output = output;
						igt_dynamic_f("%s", data.output->name) {
							data.op = op;
							data.test_plane_id = DRM_PLANE_TYPE_PRIMARY;
							test_setup(&data);
							run_test(&data);
							test_cleanup(&data);
						}
					}
				}
			}

			for (op = MMAP_GTT; op <= PLANE_ONOFF; op++) {
				igt_describe("Check if psr is detecting memory mapping,rendering "
						"and plane operations performed on sprite planes");
				igt_subtest_with_dynamic_f("%s%ssprite-%s", append_fbc_subtest[y],
							   append_subtest_name[z], op_str(op)) {
					igt_skip_on(is_xe_device(data.drm_fd) &&
						    (op == MMAP_CPU || op == MMAP_GTT));
					for_each_connected_output(&data.display, output) {
						if (!psr_sink_support(data.drm_fd, data.debugfs_fd,
								      data.op_psr_mode, output))
							continue;
						igt_display_reset(&data.display);
						data.output = output;
						igt_dynamic_f("%s", data.output->name) {
							data.op = op;
							data.test_plane_id = DRM_PLANE_TYPE_OVERLAY;
							test_setup(&data);
							run_test(&data);
							test_cleanup(&data);
						}
					}
				}

				igt_describe("Check if psr is detecting memory mapping, rendering "
						"and plane operations performed on cursor planes");
				igt_subtest_with_dynamic_f("%s%scursor-%s", append_fbc_subtest[y],
					      append_subtest_name[z], op_str(op)) {
					igt_skip_on(is_xe_device(data.drm_fd) &&
						    (op == MMAP_CPU || op == MMAP_GTT ||
						     op == BLT || op == RENDER));
					for_each_connected_output(&data.display, output) {
						if (!psr_sink_support(data.drm_fd, data.debugfs_fd,
								      data.op_psr_mode, output))
							continue;
						igt_display_reset(&data.display);
						data.output = output;
						igt_dynamic_f("%s", data.output->name) {
							data.test_plane_id = DRM_PLANE_TYPE_CURSOR;
							test_setup(&data);
							run_test(&data);
							test_cleanup(&data);
						}
					}
				}
		}

			igt_describe("Check if psr is detecting changes when rendering operation "
				     "is performed with dpms enabled or disabled");
			igt_subtest_with_dynamic_f("%s%sdpms", append_fbc_subtest[y],
						   append_subtest_name[z]) {
				for_each_connected_output(&data.display, output) {
					if (!psr_sink_support(data.drm_fd, data.debugfs_fd,
							      data.op_psr_mode, output))
						continue;
					igt_display_reset(&data.display);
					data.output = output;
					igt_dynamic_f("%s", data.output->name) {
						data.op = igt_get_render_copyfunc(data.drm_fd) ?
										  RENDER : BLT;
						data.test_plane_id = DRM_PLANE_TYPE_PRIMARY;
						test_setup(&data);
						dpms_off_on(&data);
						run_test(&data);
						test_cleanup(&data);
					}
				}
			}

			igt_describe("Check if psr is detecting changes when plane operation is "
				     "performed with suspend resume cycles");
			igt_subtest_with_dynamic_f("%s%ssuspend", append_fbc_subtest[y],
						   append_subtest_name[z]) {
				for_each_connected_output(&data.display, output) {
					if (!psr_sink_support(data.drm_fd, data.debugfs_fd,
							      data.op_psr_mode, output))
						continue;
					igt_display_reset(&data.display);
					data.output = output;
					igt_dynamic_f("%s", data.output->name) {
						data.op = PLANE_ONOFF;
						data.test_plane_id = DRM_PLANE_TYPE_CURSOR;
						test_setup(&data);
						igt_system_suspend_autoresume(SUSPEND_STATE_MEM,
									      SUSPEND_TEST_NONE);
						igt_assert(psr_wait_entry_if_enabled(&data));
						run_test(&data);
						test_cleanup(&data);
					}
				}
			}
		}
	}

	igt_fixture() {

		close(data.debugfs_fd);
		buf_ops_destroy(data.bops);
		display_fini(&data);
		drm_close_driver(data.drm_fd);
	}
}
