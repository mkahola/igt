/*
 * Copyright © 2017 Intel Corporation
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

IGT_TEST_DESCRIPTION("Test that kernel rejects atomic modeset if ALLOW_MODESET flag is not set");

static uint64_t get_broadcast_rgb_mode(uint64_t mode)
{
	switch (mode) {
	case BROADCAST_RGB_AUTO:
		return BROADCAST_RGB_FULL;
	case BROADCAST_RGB_FULL:
		return BROADCAST_RGB_16_235;
	case BROADCAST_RGB_16_235:
		return BROADCAST_RGB_AUTO;
	default:
		return BROADCAST_RGB_AUTO;
	}
}

static void
test_init(igt_display_t *display, struct igt_fb *fb, igt_output_t *output)
{
	drmModeModeInfo *mode;
	igt_plane_t *primary;
	int id;

	mode = igt_output_get_mode(output);

	id = igt_create_pattern_fb(display->drm_fd,
				   mode->hdisplay, mode->vdisplay,
				   DRM_FORMAT_XRGB8888,
				   LOCAL_I915_FORMAT_MOD_X_TILED,
				   fb);
	igt_assert(id);

	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_assert(primary);

	igt_plane_set_fb(primary, fb);
}

static void
test_finish(igt_display_t *display, struct igt_fb *fb, igt_output_t *output)
{
	igt_plane_t *primary;

	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_assert(primary);

	igt_remove_fb(display->drm_fd, fb);

	igt_plane_set_fb(primary, NULL);
	igt_output_set_pipe(output, PIPE_ANY);

	igt_display_commit2(display, COMMIT_ATOMIC);
}

static void
test_allow_modeset(igt_display_t *display, enum pipe pipe, igt_output_t *output)
{
	struct igt_fb fb;
	int flags = DRM_MODE_ATOMIC_NONBLOCK;
	int ret;

	igt_output_set_pipe(output, pipe);

	test_init(display, &fb, output);

	/*
	 * Try to do atomic commit without DRM_MODE_ATOMIC_ALLOW_MODESET flag.
	 * Kernel should reject this request.
	 */
	ret = igt_display_try_commit_atomic(display, flags, NULL);
	igt_assert_eq(ret, -EINVAL);

	/* do modeset */
	igt_output_set_pipe(output, pipe);

	/*
	 * Try to do atomic commit with DRM_MODE_ATOMIC_ALLOW_MODESET flag set.
	 * The kernel should now accept this request.
	 */
	flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
	ret = igt_display_try_commit_atomic(display, flags, NULL);
	igt_assert_eq(ret, 0);

	test_finish(display, &fb, output);
}

static void
test_active_property(igt_display_t *display, enum pipe pipe, igt_output_t *output)
{
	struct igt_fb fb;
	int flags = DRM_MODE_ATOMIC_NONBLOCK;
	int ret;
	bool found;
	uint32_t id;
	uint64_t val1, val2;
	uint64_t mode;

	found = kmstest_get_property(display->drm_fd,
				     output->config.connector->connector_id,
				     DRM_MODE_OBJECT_CONNECTOR,
				     "Broadcast RGB", &id, &val1, NULL);
	igt_assert(found);

	mode = get_broadcast_rgb_mode(val1);

	igt_output_set_pipe(output, pipe);

	test_init(display, &fb, output);

	/*
	 * Try to do atomic commit without DRM_MODE_ATOMIC_ALLOW_MODESET flag.
	 * Kernel should reject this request.
	 */
	ret = igt_display_try_commit_atomic(display, flags, NULL);
	igt_assert_eq(ret, -EINVAL);

	/* change property */
	ret = kmstest_set_connector_broadcast_rgb(display->drm_fd,
						  output->config.connector,
						  mode);
	igt_assert(ret);

	/*
	 * Try to do atomic commit with DRM_MODE_ATOMIC_ALLOW_MODESET flag set.
	 * The kernel should now accept this request.
	 */
	flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
	ret = igt_display_try_commit_atomic(display, flags, NULL);
	igt_assert_eq(ret, 0);

	found = kmstest_get_property(display->drm_fd,
				     output->config.connector->connector_id,
				     DRM_MODE_OBJECT_CONNECTOR,
				     "Broadcast RGB", &id, &val2, NULL);
	igt_assert(found);
	igt_assert(val1 != val2);


	/* switch back to RGB auto mode */
	ret = kmstest_set_connector_broadcast_rgb(display->drm_fd,
						  output->config.connector,
						  BROADCAST_RGB_AUTO);
	igt_assert(ret);

	test_finish(display, &fb, output);
}

igt_main
{
	igt_output_t *output;
	igt_display_t display;
	drmModeResPtr res;

	igt_skip_on_simulation();

	igt_fixture {
		display.drm_fd = drm_open_driver_master(DRIVER_ANY);
		igt_enable_connectors();
		kmstest_set_vt_graphics_mode();
		igt_display_init(&display, display.drm_fd);
		igt_require(display.is_atomic);

		res = drmModeGetResources(display.drm_fd);
		kmstest_unset_all_crtcs(display.drm_fd, res);
	}

	igt_subtest_f("allow-modeset") {
		enum pipe pipe;
		int valid_tests = 0;

		for_each_pipe_with_valid_output(&display, pipe, output) {
			test_allow_modeset(&display, pipe, output);

			valid_tests++;
			break;
		}

		igt_require_f(valid_tests, "no valid crtc/connector combinations found\n");
	}

	igt_subtest_f("active-property") {
		enum pipe pipe;
		int valid_tests = 0;

		for_each_pipe_with_valid_output(&display, pipe, output) {
			test_active_property(&display, pipe, output);

			valid_tests++;
			break;
		}

		igt_require_f(valid_tests, "no valid crtc/connector combinations found\n");
	}

	igt_fixture {
		igt_display_fini(&display);
		igt_reset_connectors();
		drmModeFreeResources(res);
		close(display.drm_fd);
	}

	igt_exit();
}
