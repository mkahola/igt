/*
 * Copyright © 2013 Intel Corporation
 * Copyright © 2018 Collabora, Ltd.
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
 * Authors:
 *    Daniel Vetter <daniel.vetter@ffwll.ch>
 *    Daniel Stone <daniels@collabora.com>
 *
 */

/**
 * TEST: kms getfb
 * Category: Display
 * Description: Tests GETFB and GETFB2 ioctls.
 * Driver requirement: i915, xe
 * Mega feature: General Display Features
 */

#include "igt.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#include "drm.h"
#include "drm_fourcc.h"
#include "i915/gem_create.h"
#include "igt_device.h"
#include "xe/xe_query.h"
#include "xe/xe_ioctl.h"

/**
 * SUBTEST: getfb-handle-%s
 * Description: Tests error handling %arg[1]
 *
 * arg[1]:
 *
 * @closed:       when passing a handle that has been closed.
 * @not-fb:       when passing an invalid handle.
 * @valid:        when passing an valid handle.
 * @zero:         for a zero'd input.
 */

/**
 * SUBTEST: getfb-reject-nv12
 * Description: Tests error handling while requesting NV12 buffers it should
 *              refuse because getfb supports returning a single buffer handle.
 *
 * SUBTEST: getfb-%s-different-handles
 * Description: Tests error handling while requesting for two different handles
 *              from %arg[1].
 *
 * arg[1]:
 *
 * @addfb:           same fd
 * @repeated:        different fd
 */

/**
 * SUBTEST: getfb2-accept-nv12
 * Description: Tests outputs are correct when retrieving a NV12 framebuffer.
 *
 * SUBTEST: getfb2-into-addfb2
 * Description: Output check by passing the output of GETFB2 into ADDFB2.
 *
 * SUBTEST: getfb2-handle-%s
 * Description: Tests error handling %arg[1].
 *
 * arg[1]:
 *
 * @closed:                  when passing a handle that has been closed
 * @not-fb:                  when passing an invalid handle
 * @zero:                    for a zero'd input
 */

/**
 * SUBTEST: %s-handle-protection
 * Description: Make sure %arg[1] return handles if caller is non-root or non-master.
 *
 * arg[1]:
 *
 * @getfb:      GETFB ioctl
 * @getfb2:     GETFB2 ioctl
 */

IGT_TEST_DESCRIPTION("Tests GETFB and GETFB2 ioctls.");

static bool has_getfb_iface(int fd)
{
	struct drm_mode_fb_cmd arg = { };
	int err;

	err = 0;
	if (drmIoctl(fd, DRM_IOCTL_MODE_GETFB, &arg))
		err = -errno;
	switch (err) {
	case -ENOTTY: /* ioctl unrecognised (kernel too old) */
	case -ENOTSUP: /* driver doesn't support KMS */
		return false;
	default:
		return true;
	}
}

/**
 * Find and return an arbitrary valid property ID.
 */
static uint32_t get_any_prop_id(struct igt_display *display)
{
	for (int i = 0; i < display->n_outputs; i++) {
		igt_output_t *output = &display->outputs[i];
		if (output->props[IGT_CONNECTOR_DPMS] != 0)
			return output->props[IGT_CONNECTOR_DPMS];
	}

	return 0;
}

static void test_handle_input(struct igt_display *display)
{
	struct igt_fb fb;

	igt_fixture() {
		igt_create_fb(display->drm_fd, 1024, 1024,
			      DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR,
			      &fb);
	}

	igt_describe("Tests error handling for a zero'd input.");
	igt_subtest("getfb-handle-zero") {
		struct drm_mode_fb_cmd get = { .fb_id = 0 };
		do_ioctl_err(display->drm_fd, DRM_IOCTL_MODE_GETFB, &get,
			     ENOENT);
	}

	igt_describe("Tests error handling when passing an valid "
		     "handle.");
	igt_subtest("getfb-handle-valid") {
		struct drm_mode_fb_cmd get = { .fb_id = fb.fb_id };
		do_ioctl(display->drm_fd, DRM_IOCTL_MODE_GETFB, &get);
		igt_assert_neq_u32(get.handle, 0);
		igt_assert_eq_u32(get.width, fb.width);
		igt_assert_eq_u32(get.height, fb.height);
		igt_assert_eq_u32(get.pitch, fb.strides[0]);
		igt_assert_eq_u32(get.depth, 24);
		igt_assert_eq_u32(get.bpp, 32);
		gem_close(display->drm_fd, get.handle);
	}

	igt_describe("Tests error handling when passing a handle that "
		     "has been closed.");
	igt_subtest("getfb-handle-closed") {
		struct drm_mode_fb_cmd get = { .fb_id = fb.fb_id };
		igt_remove_fb(display->drm_fd, &fb);
		do_ioctl_err(display->drm_fd, DRM_IOCTL_MODE_GETFB, &get,
			     ENOENT);
	}

	igt_describe("Tests error handling when passing an invalid "
		     "handle.");
	igt_subtest("getfb-handle-not-fb") {
		struct drm_mode_fb_cmd get = {
			.fb_id = get_any_prop_id(display)
		};

		igt_require(get.fb_id > 0);
		do_ioctl_err(display->drm_fd, DRM_IOCTL_MODE_GETFB, &get,
			     ENOENT);
	}

	igt_fixture()
		igt_remove_fb(display->drm_fd, &fb);
}

static void test_duplicate_handles(struct igt_display *display)
{
	struct igt_fb fb;

	igt_fixture() {
		igt_create_fb(display->drm_fd, 1024, 1024,
			      DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR,
			      &fb);
	}

	igt_describe("Tests error handling while requesting for two different "
		     "handles from same fd.");
	igt_subtest("getfb-addfb-different-handles") {
		struct drm_mode_fb_cmd get = { .fb_id = fb.fb_id };

		do_ioctl(display->drm_fd, DRM_IOCTL_MODE_GETFB, &get);
		igt_assert_neq_u32(get.handle, fb.gem_handle);
		gem_close(display->drm_fd, get.handle);
	}

	igt_describe("Tests error handling while requesting for two different "
		     "handles from different fd.");
	igt_subtest("getfb-repeated-different-handles") {
		struct drm_mode_fb_cmd get1 = { .fb_id = fb.fb_id };
		struct drm_mode_fb_cmd get2 = { .fb_id = fb.fb_id };

		do_ioctl(display->drm_fd, DRM_IOCTL_MODE_GETFB, &get1);
		do_ioctl(display->drm_fd, DRM_IOCTL_MODE_GETFB, &get2);
		igt_assert_neq_u32(get1.handle, get2.handle);

		gem_close(display->drm_fd, get1.handle);
		gem_close(display->drm_fd, get2.handle);
	}

	igt_describe("Tests error handling while requesting NV12 buffers "
		     "it should refuse because getfb supports returning "
		     "a single buffer handle.");
	igt_subtest("getfb-reject-nv12") {
		struct drm_mode_fb_cmd get = { };
		struct igt_fb nv12_fb;

		igt_require(igt_display_has_format_mod(display,
						       DRM_FORMAT_NV12,
						       DRM_FORMAT_MOD_LINEAR));

		igt_create_fb(display->drm_fd, 1024, 1024,
			      DRM_FORMAT_NV12, DRM_FORMAT_MOD_LINEAR,
			      &nv12_fb);

		get.fb_id = nv12_fb.fb_id;
		do_ioctl_err(display->drm_fd, DRM_IOCTL_MODE_GETFB, &get,
			     EINVAL);

		igt_remove_fb(display->drm_fd, &nv12_fb);
	}

	igt_fixture()
		igt_remove_fb(display->drm_fd, &fb);
}

static void test_getfb2(struct igt_display *display)
{
	struct igt_fb fb;

	igt_fixture() {
		struct drm_mode_fb_cmd2 get = {};

		igt_create_fb(display->drm_fd, 1024, 1024,
			      DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR,
			      &fb);

		get.fb_id = fb.fb_id;
		do_ioctl(display->drm_fd, DRM_IOCTL_MODE_GETFB2, &get);
		igt_assert_neq_u32(get.handles[0], 0);
		gem_close(display->drm_fd, get.handles[0]);
	}

	igt_describe("Tests error handling for a zero'd input.");
	igt_subtest("getfb2-handle-zero") {
		struct drm_mode_fb_cmd2 get = {};
		do_ioctl_err(display->drm_fd, DRM_IOCTL_MODE_GETFB2, &get,
			     ENOENT);
	}

	igt_describe("Tests error handling when passing a handle that "
		     "has been closed.");
	igt_subtest("getfb2-handle-closed") {
		struct igt_fb test_fb;
		struct drm_mode_fb_cmd2 get = { };

		igt_create_fb(display->drm_fd, fb.width, fb.height,
			      fb.drm_format, fb.modifier, &test_fb);

		get.fb_id = test_fb.fb_id;
		igt_remove_fb(display->drm_fd, &test_fb);

		do_ioctl_err(display->drm_fd, DRM_IOCTL_MODE_GETFB2, &get,
			     ENOENT);
	}

	igt_describe("Tests error handling when passing an invalid "
		     "handle.");
	igt_subtest("getfb2-handle-not-fb") {
		struct drm_mode_fb_cmd2 get = {
			.fb_id = get_any_prop_id(display)
		};
		igt_require(get.fb_id > 0);
		do_ioctl_err(display->drm_fd, DRM_IOCTL_MODE_GETFB2, &get,
			     ENOENT);
	}

	igt_describe("Tests outputs are correct when retrieving a "
		     "NV12 framebuffer.");
	igt_subtest("getfb2-accept-nv12") {
		struct igt_fb nv12_fb;
		struct drm_mode_fb_cmd2 get = { };
		int i;

		igt_require(igt_display_has_format_mod(display,
						       DRM_FORMAT_NV12,
						       DRM_FORMAT_MOD_LINEAR));

		igt_create_fb(display->drm_fd, 1024, 1024,
			      DRM_FORMAT_NV12, DRM_FORMAT_MOD_LINEAR,
			      &nv12_fb);

		get.fb_id = nv12_fb.fb_id;
		do_ioctl(display->drm_fd, DRM_IOCTL_MODE_GETFB2, &get);

		igt_assert_eq_u32(get.width, nv12_fb.width);
		igt_assert_eq_u32(get.height, nv12_fb.height);
		igt_assert(get.flags & DRM_MODE_FB_MODIFIERS);

		for (i = 0; i < ARRAY_SIZE(get.handles); i++) {
			igt_assert_eq_u32(get.pitches[i], nv12_fb.strides[i]);
			igt_assert_eq_u32(get.offsets[i], nv12_fb.offsets[i]);
			if (i < 2) {
				igt_assert_neq_u32(get.handles[i], 0);
				igt_assert_neq_u32(get.handles[i],
						   nv12_fb.gem_handle);
				igt_assert_eq_u64(get.modifier[i],
						  fb.modifier);
			} else {
				igt_assert_eq_u32(get.handles[i], 0);
				igt_assert_eq_u64(get.modifier[i], 0);
			}
		}

		if (is_intel_device(display->drm_fd))
			igt_assert_eq_u32(get.handles[0], get.handles[1]);

		igt_remove_fb(display->drm_fd, &nv12_fb);
	}

	igt_describe("Output check by passing the output of GETFB2 "
		     "into ADDFB2.");
	igt_subtest("getfb2-into-addfb2") {
		struct drm_mode_fb_cmd2 cmd = { };

		cmd.fb_id = fb.fb_id;
		do_ioctl(display->drm_fd, DRM_IOCTL_MODE_GETFB2, &cmd);
		do_ioctl(display->drm_fd, DRM_IOCTL_MODE_ADDFB2, &cmd);

		do_ioctl(display->drm_fd, DRM_IOCTL_MODE_RMFB, &cmd.fb_id);
		gem_close(display->drm_fd, cmd.handles[0]);
	}

	igt_fixture()
		igt_remove_fb(display->drm_fd, &fb);
}

static void test_handle_protection(void) {
	int non_master_fd;
	struct drm_mode_fb_cmd2 non_master_add = {};

	igt_fixture() {
		non_master_fd = drm_open_driver(DRIVER_ANY);

		non_master_add.width = 1024;
		non_master_add.height = 1024;
		non_master_add.pixel_format = DRM_FORMAT_XRGB8888;
		non_master_add.pitches[0] = 1024*4;
		non_master_add.handles[0] = igt_create_bo_with_dimensions(non_master_fd, 1024, 1024,
			DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR, 0, NULL, NULL, NULL);
		igt_require(non_master_add.handles[0] != 0);
		do_ioctl(non_master_fd, DRM_IOCTL_MODE_ADDFB2, &non_master_add);
	}

	igt_describe("Make sure GETFB doesn't return handles if caller "
		     "is non-root or non-master.");
	igt_subtest("getfb-handle-protection") {
		struct drm_mode_fb_cmd get = { .fb_id = non_master_add.fb_id};

		igt_fork(child, 1) {
			igt_drop_root();

			do_ioctl(non_master_fd, DRM_IOCTL_MODE_GETFB, &get);
			/* ioctl succeeds but handle should be 0 */
			igt_assert_eq_u32(get.handle, 0);
		}
		igt_waitchildren();
	}

	igt_describe("Make sure GETFB2 doesn't return handles if caller "
		     "is non-root or non-master.");
	igt_subtest("getfb2-handle-protection") {
		struct drm_mode_fb_cmd2 get = { .fb_id = non_master_add.fb_id};
		int i;

		igt_fork(child, 1) {
			igt_drop_root();

			do_ioctl(non_master_fd, DRM_IOCTL_MODE_GETFB2, &get);
			/* ioctl succeeds but handles should be 0 */
			for (i = 0; i < ARRAY_SIZE(get.handles); i++) {
				igt_assert_eq_u32(get.handles[i], 0);
			}
		}
		igt_waitchildren();
	}

	igt_fixture() {
		do_ioctl(non_master_fd, DRM_IOCTL_MODE_RMFB, &non_master_add.fb_id);
		gem_close(non_master_fd, non_master_add.handles[0]);

		drm_close_driver(non_master_fd);
	}
}

int igt_main()
{
	int fd;
	igt_display_t display;

	igt_fixture() {
		fd = drm_open_driver_master(DRIVER_ANY);
		igt_require(has_getfb_iface(fd));
		igt_display_require(&display, fd);
	}

	igt_subtest_group()
		test_handle_input(&display);

	igt_subtest_group()
		test_duplicate_handles(&display);

	igt_subtest_group()
		test_getfb2(&display);

	igt_subtest_group()
		test_handle_protection();

	igt_fixture() {
		igt_display_fini(&display);
		drm_close_driver(fd);
	}
}
