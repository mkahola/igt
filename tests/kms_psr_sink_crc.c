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

#include "igt.h"
#include "igt_sysfs.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "intel_bufmgr.h"

bool running_with_psr_disabled;

#define CRC_BLACK "000000000000"
#define CRC_LEN 12

enum operations {
	PAGE_FLIP,
	MMAP_GTT,
	MMAP_GTT_WAITING,
	MMAP_CPU,
	BLT,
	RENDER,
	PLANE_MOVE,
	PLANE_ONOFF,
};

static const char *op_str(enum operations op)
{
	static const char * const name[] = {
		[PAGE_FLIP] = "page_flip",
		[MMAP_GTT] = "mmap_gtt",
		[MMAP_GTT_WAITING] = "mmap_gtt_waiting",
		[MMAP_CPU] = "mmap_cpu",
		[BLT] = "blt",
		[RENDER] = "render",
		[PLANE_MOVE] = "plane_move",
		[PLANE_ONOFF] = "plane_onoff",
	};

	return name[op];
}

typedef struct {
	int drm_fd;
	int test_plane;
	enum operations op;
	uint32_t devid;
	uint32_t crtc_id;
	igt_display_t display;
	drm_intel_bufmgr *bufmgr;
	struct igt_fb fb_green, fb_white;
	igt_plane_t *primary, *sprite, *cursor;
	int mod_size;
	int mod_stride;
	drmModeModeInfo *mode;
	igt_output_t *output;
} data_t;

static void create_cursor_fb(data_t *data)
{
	cairo_t *cr;
	uint32_t fb_id;

	fb_id = igt_create_fb(data->drm_fd, 64, 64,
			      DRM_FORMAT_ARGB8888, LOCAL_DRM_FORMAT_MOD_NONE,
			      &data->fb_white);
	igt_assert(fb_id);

	cr = igt_get_cairo_ctx(data->drm_fd, &data->fb_white);
	igt_paint_color_alpha(cr, 0, 0, 64, 64, 1.0, 1.0, 1.0, 1.0);
	igt_put_cairo_ctx(data->drm_fd, &data->fb_white, cr);
}


static void setup_output(data_t *data)
{
	igt_display_t *display = &data->display;
	igt_output_t *output;
	enum pipe pipe;

	for_each_pipe_with_valid_output(display, pipe, output) {
		drmModeConnectorPtr c = output->config.connector;

		if (c->connector_type != DRM_MODE_CONNECTOR_eDP)
			continue;

		igt_output_set_pipe(output, pipe);
		data->crtc_id = output->config.crtc->crtc_id;
		data->output = output;
		data->mode = igt_output_get_mode(output);

		return;
	}
}

static void display_init(data_t *data)
{
	igt_display_init(&data->display, data->drm_fd);
	setup_output(data);
}

static void display_fini(data_t *data)
{
	igt_display_fini(&data->display);
}

static void fill_blt(data_t *data, uint32_t handle, unsigned char color)
{
	drm_intel_bo *dst = gem_handle_to_libdrm_bo(data->bufmgr,
						    data->drm_fd,
						    "", handle);
	struct intel_batchbuffer *batch;

	batch = intel_batchbuffer_alloc(data->bufmgr, data->devid);
	igt_assert(batch);

	COLOR_BLIT_COPY_BATCH_START(0);
	OUT_BATCH((1 << 24) | (0xf0 << 16) | 0);
	OUT_BATCH(0);
	OUT_BATCH(0xfff << 16 | 0xfff);
	OUT_RELOC(dst, I915_GEM_DOMAIN_RENDER, I915_GEM_DOMAIN_RENDER, 0);
	OUT_BATCH(color);
	ADVANCE_BATCH();

	intel_batchbuffer_flush(batch);
	intel_batchbuffer_free(batch);

	gem_bo_busy(data->drm_fd, handle);
}

static void scratch_buf_init(struct igt_buf *buf, drm_intel_bo *bo,
			     int size, int stride)
{
	buf->bo = bo;
	buf->stride = stride;
	buf->tiling = I915_TILING_X;
	buf->size = size;
}

static void fill_render(data_t *data, uint32_t handle, unsigned char color)
{
	drm_intel_bo *src, *dst;
	struct intel_batchbuffer *batch;
	struct igt_buf src_buf, dst_buf;
	const uint8_t buf[4] = { color, color, color, color };
	igt_render_copyfunc_t rendercopy = igt_get_render_copyfunc(data->devid);

	igt_skip_on(!rendercopy);

	dst = gem_handle_to_libdrm_bo(data->bufmgr, data->drm_fd, "", handle);
	igt_assert(dst);

	src = drm_intel_bo_alloc(data->bufmgr, "", data->mod_size, 4096);
	igt_assert(src);

	gem_write(data->drm_fd, src->handle, 0, buf, 4);

	scratch_buf_init(&src_buf, src, data->mod_size, data->mod_stride);
	scratch_buf_init(&dst_buf, dst, data->mod_size, data->mod_stride);

	batch = intel_batchbuffer_alloc(data->bufmgr, data->devid);
	igt_assert(batch);

	rendercopy(batch, NULL,
		   &src_buf, 0, 0, 0xff, 0xff,
		   &dst_buf, 0, 0);

	intel_batchbuffer_free(batch);

	gem_bo_busy(data->drm_fd, handle);
}

static bool psr_possible(data_t *data)
{
	char buf[512];

	igt_debugfs_read(data->drm_fd, "i915_edp_psr_status", buf);

	return running_with_psr_disabled ||
		strstr(buf, "Sink_Support: yes\n");
}

static bool psr_active(data_t *data)
{
	char buf[512];

	igt_debugfs_read(data->drm_fd, "i915_edp_psr_status", buf);

	return running_with_psr_disabled ||
		strstr(buf, "HW Enabled & Active bit: yes\n");
}

static bool wait_psr_entry(data_t *data)
{
	int timeout = 5;
	while (timeout--) {
		if (psr_active(data))
			return true;
		sleep(1);
	}
	return false;
}

static void get_sink_crc(data_t *data, char *crc) {
	int dir;

	if (igt_interactive_debug)
		return;

	dir = igt_debugfs_dir(data->drm_fd);
	igt_require_f(igt_sysfs_scanf(dir, "i915_sink_crc_eDP1", "%s\n", crc),
		      "Sink CRC is unreliable on this machine. Try manual debug with --interactive-debug=no-crc\n");
	close(dir);

	igt_debug("%s\n", crc);
	igt_debug_wait_for_keypress("crc");

	/* The important value was already taken.
	 * Now give a time for human eyes
	 */
	usleep(300000);

	/* Black screen is always invalid */
	igt_assert(strncmp(crc, CRC_BLACK, CRC_LEN) != 0);
}

static bool is_green(char *crc)
{
	char color_mask[5] = "FFFF\0";
	char rs[5], gs[5], bs[5];
	unsigned int rh, gh, bh, mask;
	int ret;

	if (igt_interactive_debug)
		return false;

	sscanf(color_mask, "%4x", &mask);

	memcpy(rs, &crc[0], 4);
	rs[4] = '\0';
	ret = sscanf(rs, "%4x", &rh);
	igt_require(ret > 0);

	memcpy(gs, &crc[4], 4);
	gs[4] = '\0';
	ret = sscanf(gs, "%4x", &gh);
	igt_require(ret > 0);

	memcpy(bs, &crc[8], 4);
	bs[4] = '\0';
	ret = sscanf(bs, "%4x", &bh);
	igt_require(ret > 0);

	return ((rh & mask) == 0 &&
		(gh & mask) != 0 &&
		(bh & mask) == 0);
}

static void assert_or_manual(bool condition, const char *expected)
{
	igt_debug_manual_check("no-crc", expected);
	igt_assert(igt_interactive_debug || condition);
}

static bool drrs_disabled(data_t *data)
{
	char buf[512];

	igt_debugfs_read(data->drm_fd, "i915_drrs_status", buf);

	return !strstr(buf, "DRRS Supported: Yes\n");
}

static void run_test(data_t *data)
{
	uint32_t handle = data->fb_white.gem_handle;
	igt_plane_t *test_plane;
	void *ptr;
	char ref_crc[CRC_LEN];
	char crc[CRC_LEN];
	const char *expected = "";

	/* Confirm that screen became Green */
	get_sink_crc(data, ref_crc);
	assert_or_manual(is_green(ref_crc), "screen GREEN");

	/* Confirm screen stays Green after PSR got active */
	igt_assert(wait_psr_entry(data));
	get_sink_crc(data, ref_crc);
	assert_or_manual(is_green(ref_crc), "screen GREEN");

	/* Setting a secondary fb/plane */
	switch (data->test_plane) {
	case DRM_PLANE_TYPE_PRIMARY: default: test_plane = data->primary; break;
	case DRM_PLANE_TYPE_OVERLAY: test_plane = data->sprite; break;
	case DRM_PLANE_TYPE_CURSOR: test_plane = data->cursor; break;
	}
	igt_plane_set_fb(test_plane, &data->fb_white);
	igt_display_commit(&data->display);

	/* Confirm it is not Green anymore */
	igt_assert(wait_psr_entry(data));
	get_sink_crc(data, ref_crc);
	if (data->test_plane == DRM_PLANE_TYPE_PRIMARY)
		assert_or_manual(!is_green(ref_crc), "screen WHITE");
	else
		assert_or_manual(!is_green(ref_crc), "GREEN background with WHITE box");

	switch (data->op) {
	case PAGE_FLIP:
		/* Only in use when testing primary plane */
		igt_assert(drmModePageFlip(data->drm_fd, data->crtc_id,
					   data->fb_green.fb_id, 0, NULL) == 0);
		get_sink_crc(data, crc);
		assert_or_manual(is_green(crc), "screen GREEN");
		expected = "still GREEN";
		break;
	case MMAP_GTT:
		ptr = gem_mmap__gtt(data->drm_fd, handle, data->mod_size,
				    PROT_WRITE);
		gem_set_domain(data->drm_fd, handle,
			       I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
		memset(ptr, 0xcc, data->mod_size);
		munmap(ptr, data->mod_size);
		expected = "BLACK or TRANSPARENT mark on top of plane in test";
		break;
	case MMAP_GTT_WAITING:
		ptr = gem_mmap__gtt(data->drm_fd, handle, data->mod_size,
				    PROT_WRITE);
		gem_set_domain(data->drm_fd, handle,
			       I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);

		/* Printing white on white so the screen shouldn't change */
		memset(ptr, 0xff, data->mod_size);
		get_sink_crc(data, crc);
		if (data->test_plane == DRM_PLANE_TYPE_PRIMARY)
			assert_or_manual(strncmp(ref_crc, crc, CRC_LEN) == 0, "screen WHITE");
		else
			assert_or_manual(strncmp(ref_crc, crc, CRC_LEN) == 0,
			       "GREEN background with WHITE box");

		igt_info("Waiting 10s...\n");
		sleep(10);

		/* Now lets print black to change the screen */
		memset(ptr, 0, data->mod_size);
		munmap(ptr, data->mod_size);
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
		expected = "BLACK or TRANSPARENT mark on top of plane in test";
		break;
	case BLT:
		fill_blt(data, handle, 0);
		expected = "BLACK or TRANSPARENT mark on top of plane in test";
		break;
	case RENDER:
		fill_render(data, handle, 0);
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
	get_sink_crc(data, crc);
	assert_or_manual(strncmp(ref_crc, crc, CRC_LEN) != 0, expected);
}

static void test_cleanup(data_t *data) {
	igt_plane_set_fb(data->primary, NULL);
	if (data->test_plane == DRM_PLANE_TYPE_OVERLAY)
		igt_plane_set_fb(data->sprite, NULL);
	if (data->test_plane == DRM_PLANE_TYPE_CURSOR)
		igt_plane_set_fb(data->cursor, NULL);

	igt_display_commit(&data->display);

	igt_remove_fb(data->drm_fd, &data->fb_green);
	igt_remove_fb(data->drm_fd, &data->fb_white);
}

static void setup_test_plane(data_t *data)
{
	uint32_t white_h, white_v;

	igt_create_color_fb(data->drm_fd,
			    data->mode->hdisplay, data->mode->vdisplay,
			    DRM_FORMAT_XRGB8888,
			    LOCAL_I915_FORMAT_MOD_X_TILED,
			    0.0, 1.0, 0.0,
			    &data->fb_green);

	data->primary = igt_output_get_plane_type(data->output, DRM_PLANE_TYPE_PRIMARY);
	igt_plane_set_fb(data->primary, NULL);

	white_h = data->mode->hdisplay;
	white_v = data->mode->vdisplay;

	/* Ignoring pitch and bpp to avoid changing full screen */
	data->mod_size = white_h * white_v;
	data->mod_stride = white_h * 4;

	switch (data->test_plane) {
	case DRM_PLANE_TYPE_OVERLAY:
		data->sprite = igt_output_get_plane_type(data->output,
						    DRM_PLANE_TYPE_OVERLAY);
		igt_plane_set_fb(data->sprite, NULL);
		/* To make it different for human eyes let's make
		 * sprite visible in only one quarter of the primary
		 */
		white_h = white_h/2;
		white_v = white_v/2;
	case DRM_PLANE_TYPE_PRIMARY:
		igt_create_color_fb(data->drm_fd,
				    white_h, white_v,
				    DRM_FORMAT_XRGB8888,
				    LOCAL_I915_FORMAT_MOD_X_TILED,
				    1.0, 1.0, 1.0,
				    &data->fb_white);
		break;
	case DRM_PLANE_TYPE_CURSOR:
		data->cursor = igt_output_get_plane_type(data->output,
						    DRM_PLANE_TYPE_CURSOR);
		igt_plane_set_fb(data->cursor, NULL);
		create_cursor_fb(data);
		igt_plane_set_position(data->cursor, 0, 0);

		/* Cursor is 64 x 64, ignoring pitch and bbp again */
		data->mod_size = 64 * 64;
		break;
	}

	igt_display_commit(&data->display);

	igt_plane_set_fb(data->primary, &data->fb_green);
	igt_display_commit(&data->display);
}

static void dpms_off_on(data_t data)
{
	kmstest_set_connector_dpms(data.drm_fd, data.output->config.connector,
				   DRM_MODE_DPMS_OFF);
	sleep(1);
	kmstest_set_connector_dpms(data.drm_fd, data.output->config.connector,
				   DRM_MODE_DPMS_ON);
}

static int opt_handler(int opt, int opt_index, void *data)
{
	switch (opt) {
	case 'n':
		running_with_psr_disabled = true;
		break;
	default:
		igt_assert(0);
	}

	return 0;
}

int main(int argc, char *argv[])
{
	const char *help_str =
	       "  --no-psr\tRun test without PSR to check the CRC test logic.";
	static struct option long_options[] = {
		{"no-psr", 0, 0, 'n'},
		{ 0, 0, 0, 0 }
	};
	data_t data = {};
	enum operations op;

	igt_subtest_init_parse_opts(&argc, argv, "", long_options,
				    help_str, opt_handler, NULL);
	igt_skip_on_simulation();

	igt_fixture {
		data.drm_fd = drm_open_driver_master(DRIVER_INTEL);
		kmstest_set_vt_graphics_mode();
		data.devid = intel_get_drm_devid(data.drm_fd);

		igt_set_module_param_int("enable_psr", running_with_psr_disabled ?
					 0 : 1);

		igt_skip_on(!psr_possible(&data));

		data.bufmgr = drm_intel_bufmgr_gem_init(data.drm_fd, 4096);
		igt_assert(data.bufmgr);
		drm_intel_bufmgr_gem_enable_reuse(data.bufmgr);

		display_init(&data);
	}

	igt_subtest("psr_basic") {
		setup_test_plane(&data);
		igt_assert(wait_psr_entry(&data));
		test_cleanup(&data);
	}

	igt_subtest("psr_drrs") {
		setup_test_plane(&data);
		igt_assert(drrs_disabled(&data));
		test_cleanup(&data);
	}

	for (op = PAGE_FLIP; op <= RENDER; op++) {
		igt_subtest_f("primary_%s", op_str(op)) {
			data.test_plane = DRM_PLANE_TYPE_PRIMARY;
			data.op = op;
			setup_test_plane(&data);
			igt_assert(wait_psr_entry(&data));
			run_test(&data);
			test_cleanup(&data);
		}
	}

	for (op = MMAP_GTT; op <= PLANE_ONOFF; op++) {
		igt_subtest_f("sprite_%s", op_str(op)) {
			data.test_plane = DRM_PLANE_TYPE_OVERLAY;
			data.op = op;
			setup_test_plane(&data);
			igt_assert(wait_psr_entry(&data));
			run_test(&data);
			test_cleanup(&data);
		}
	}

	for (op = MMAP_GTT; op <= PLANE_ONOFF; op++) {
		igt_subtest_f("cursor_%s", op_str(op)) {
			data.test_plane = DRM_PLANE_TYPE_CURSOR;
			data.op = op;
			setup_test_plane(&data);
			igt_assert(wait_psr_entry(&data));
			run_test(&data);
			test_cleanup(&data);
		}
	}

	igt_subtest_f("dpms_off_psr_active") {
		data.test_plane = DRM_PLANE_TYPE_PRIMARY;
		data.op = RENDER;
		setup_test_plane(&data);
		igt_assert(wait_psr_entry(&data));

		dpms_off_on(data);

		run_test(&data);
		test_cleanup(&data);
	}

	igt_subtest_f("dpms_off_psr_exit") {
		data.test_plane = DRM_PLANE_TYPE_OVERLAY;
		data.op = PLANE_ONOFF;
		setup_test_plane(&data);

		dpms_off_on(data);

		igt_assert(wait_psr_entry(&data));
		run_test(&data);
		test_cleanup(&data);
	}

	igt_subtest_f("suspend_psr_active") {
		data.test_plane = DRM_PLANE_TYPE_PRIMARY;
		data.op = PAGE_FLIP;
		setup_test_plane(&data);
		igt_assert(wait_psr_entry(&data));

		igt_system_suspend_autoresume(SUSPEND_STATE_MEM,
					      SUSPEND_TEST_NONE);

		run_test(&data);
		test_cleanup(&data);
	}

	igt_subtest_f("suspend_psr_exit") {
		data.test_plane = DRM_PLANE_TYPE_CURSOR;
		data.op = PLANE_ONOFF;
		setup_test_plane(&data);

		igt_system_suspend_autoresume(SUSPEND_STATE_MEM,
					      SUSPEND_TEST_NONE);

		igt_assert(wait_psr_entry(&data));
		run_test(&data);
		test_cleanup(&data);
	}

	igt_fixture {
		drm_intel_bufmgr_destroy(data.bufmgr);
		display_fini(&data);
	}

	igt_exit();
}
