/*
 * Copyright © 2016 Intel Corporation
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
 */

/**
 * TEST: kms atomic transition
 * Category: Display
 * Description: This is a stress test, to ensure that all combinations of
 * 		atomic transitions work correctly. For i915/xe this will mainly be a
 * 		stress test on watermark calculations.
 * Driver requirement: i915, xe
 * Mega feature: General Display Features
 */

#include "igt.h"
#include "igt_rand.h"
#include "drmtest.h"
#include "sw_sync.h"
#include "igt_sysfs.h"
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <poll.h>

/**
 * SUBTEST: plane-primary-toggle-with-vblank-wait
 * Description: Check toggling of primary plane with vblank
 *
 * SUBTEST: plane-all-modeset-%s
 * Description: Modeset test for all plane combinations %arg[1]
 *
 * arg[1]:
 *
 * @transition:                           default
 * @transition-fencing:                   with fencing commit
 */

/**
 * SUBTEST: plane-all-modeset-%s
 * Description: Modeset test for all plane combinations %arg[1]
 *
 * arg[1]:
 *
 * @transition-fencing-internal-panels:   on internal panels with fencing commit
 * @transition-internal-panels:           on internal panels
 */

/**
 * SUBTEST: plane-all-%s
 * Description: Transition test for all plane combinations %arg[1]
 *
 * arg[1]:
 *
 * @transition:                           default
 * @transition-fencing:                   with fencing commit
 * @transition-nonblocking:               with non-blocking commit
 * @transition-nonblocking-fencing:       with non-blocking & fencing commit
 */

/**
 * SUBTEST: plane-toggle-modeset-transition
 * Description: Check toggling and modeset transition on plane
 *
 * SUBTEST: plane-use-after-nonblocking-%s
 * Description: Transition test with non %arg[1] and make sure commit of disabled
 *              plane has to complete before atomic commit on that plane
 *
 * arg[1]:
 *
 * @unbind:           blocking commit
 * @unbind-fencing:   blocking commit with fencing
 */

/**
 * SUBTEST: modeset-%s
 * Description: Modeset transition tests for combinations of %arg[1]
 *
 * arg[1]:
 *
 * @transition:                     crtc enabled
 * @transition-fencing:             crtc enabled with fencing commit
 * @transition-nonblocking:         crtc enabled with nonblocking commit
 * @transition-nonblocking-fencing: crtc enabled with nonblocking & fencing commit
 */

#ifndef DRM_CAP_CURSOR_WIDTH
#define DRM_CAP_CURSOR_WIDTH 0x8
#endif
#ifndef DRM_CAP_CURSOR_HEIGHT
#define DRM_CAP_CURSOR_HEIGHT 0x9
#endif

struct plane_parms {
	struct igt_fb *fb;
	uint32_t width, height, mask;
};

typedef struct {
	int drm_fd;
	struct igt_fb fbs[2], argb_fb, sprite_fb;
	igt_display_t display;
	bool extended;
	igt_pipe_crc_t *pipe_crcs[IGT_MAX_PIPES];
} data_t;

/* globals for fence support */
int *timeline;
pthread_t *thread;
int *seqno;

static void
run_primary_test(data_t *data, igt_crtc_t *crtc, igt_output_t *output)
{
	drmModeModeInfo *mode;
	igt_plane_t *primary;
	igt_fb_t *fb = &data->fbs[0];
	int i, ret;
	unsigned flags = DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET;

	igt_display_reset(&data->display);

	igt_info("Using (pipe %s + %s) to run the subtest.\n",
		 igt_crtc_name(crtc), igt_output_name(output));

	igt_output_set_crtc(output, crtc);
	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);

	mode = igt_output_get_mode(output);

	igt_plane_set_fb(primary, NULL);
	ret = igt_display_try_commit_atomic(&data->display, flags, NULL);
	igt_skip_on_f(ret == -EINVAL, "Primary plane cannot be disabled separately from output\n");

	igt_create_fb(data->drm_fd, mode->hdisplay, mode->vdisplay,
		      DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR, fb);

	igt_plane_set_fb(primary, fb);

	for (i = 0; i < 4; i++) {
		igt_display_commit2(&data->display, COMMIT_ATOMIC);

		if (!(i & 1))
			igt_wait_for_vblank(crtc);

		igt_plane_set_fb(primary, (i & 1) ? fb : NULL);
		igt_display_commit2(&data->display, COMMIT_ATOMIC);

		if (i & 1)
			igt_wait_for_vblank(crtc);

		igt_plane_set_fb(primary, (i & 1) ? NULL : fb);
	}
}

static void *fence_inc_thread(void *arg)
{
	int t = *((int *) arg);

	pthread_detach(pthread_self());

	usleep(5000);
	sw_sync_timeline_inc(t, 1);
	return NULL;
}

static void configure_fencing(igt_plane_t *plane)
{
	int i, fd, ret;

	i = plane->index;

	seqno[i]++;
	fd = sw_sync_timeline_create_fence(timeline[i], seqno[i]);
	igt_plane_set_fence_fd(plane, fd);
	close(fd);
	ret = pthread_create(&thread[i], NULL, fence_inc_thread, &timeline[i]);
	igt_assert_eq(ret, 0);
}

static bool skip_plane(data_t *data, igt_plane_t *plane)
{
	int index = plane->index;

	if (data->extended)
		return false;

	if (!is_intel_device(data->drm_fd))
		return false;

	if (plane->type == DRM_PLANE_TYPE_CURSOR)
		return false;

	if (intel_display_ver(intel_get_drm_devid(data->drm_fd)) < 11)
		return false;

	/*
	 * Test 1 HDR plane, 1 SDR UV plane, 1 SDR Y plane.
	 *
	 * Kernel registers planes in the hardware Z order:
	 * 0,1,2 HDR planes
	 * 3,4 SDR UV planes
	 * 5,6 SDR Y planes
	 */
	return index != 0 && index != 3 && index != 5;
}

static int
wm_setup_plane(data_t *data, igt_crtc_t *crtc,
	       uint32_t mask, struct plane_parms *parms, bool fencing)
{
	igt_plane_t *plane;
	int planes_set_up = 0;

	/*
	* Make sure these buffers are suited for display use
	* because most of the modeset operations must be fast
	* later on.
	*/
	for_each_plane_on_crtc(crtc,
			       plane) {
		int i = plane->index;

		if (skip_plane(data, plane))
			continue;

		if (!mask || !(parms[i].mask & mask)) {
			if (plane->values[IGT_PLANE_FB_ID] && plane->type != DRM_PLANE_TYPE_PRIMARY) {
				igt_plane_set_fb(plane, NULL);
				planes_set_up++;
			}
			continue;
		}

		if (fencing)
			configure_fencing(plane);

		igt_plane_set_fb(plane, parms[i].fb);
		igt_fb_set_size(parms[i].fb, plane, parms[i].width, parms[i].height);
		igt_plane_set_size(plane, parms[i].width, parms[i].height);

		planes_set_up++;
	}
	return planes_set_up;
}

static void ev_page_flip(int fd, unsigned seq, unsigned tv_sec, unsigned tv_usec, void *user_data)
{
	igt_debug("Retrieved vblank seq: %u on unk\n", seq);
}

static drmEventContext drm_events = {
	.version = 2,
	.page_flip_handler = ev_page_flip
};

enum transition_type {
	TRANSITION_PLANES,
	TRANSITION_AFTER_FREE,
	TRANSITION_MODESET,
	TRANSITION_MODESET_FAST,
	TRANSITION_MODESET_DISABLE,
};

static void set_sprite_wh(data_t *data, igt_crtc_t *crtc,
			  struct plane_parms *parms, struct igt_fb *sprite_fb,
			  bool alpha, unsigned w, unsigned h)
{
	igt_plane_t *plane;

	for_each_plane_on_crtc(crtc,
			       plane) {
		int i = plane->index;

		if (plane->type == DRM_PLANE_TYPE_PRIMARY ||
		    plane->type == DRM_PLANE_TYPE_CURSOR)
			continue;

		if (!parms[i].mask)
			continue;

		parms[i].width = w;
		parms[i].height = h;
	}

	igt_remove_fb(data->drm_fd, sprite_fb);
	igt_create_fb(data->drm_fd, w, h,
		      alpha ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888,
		      DRM_FORMAT_MOD_LINEAR, sprite_fb);
}

#define is_atomic_check_failure_errno(errno) \
		(errno != -EINVAL && errno != 0)

#define is_atomic_check_plane_size_errno(errno) \
		(errno == -EINVAL)

static void setup_parms(data_t *data, igt_crtc_t *crtc,
			const drmModeModeInfo *mode,
			struct igt_fb *primary_fb,
			struct igt_fb *argb_fb,
			struct igt_fb *sprite_fb,
			struct plane_parms *parms,
			unsigned *iter_max)
{
	uint64_t cursor_width, cursor_height;
	unsigned sprite_width, sprite_height, prev_w, prev_h;
	bool max_sprite_width, max_sprite_height, alpha = true;
	uint32_t n_planes = crtc->n_planes;
	uint32_t n_overlays = 0, overlays[n_planes];
	igt_plane_t *plane;
	uint32_t iter_mask = 3;

	do_or_die(drmGetCap(data->drm_fd, DRM_CAP_CURSOR_WIDTH, &cursor_width));
	if (cursor_width >= mode->hdisplay)
		cursor_width = mode->hdisplay;

	do_or_die(drmGetCap(data->drm_fd, DRM_CAP_CURSOR_HEIGHT, &cursor_height));
	if (cursor_height >= mode->vdisplay)
		cursor_height = mode->vdisplay;

	for_each_plane_on_crtc(crtc,
			       plane) {
		int i = plane->index;

		if (plane->type == DRM_PLANE_TYPE_PRIMARY) {
			parms[i].fb = primary_fb;
			parms[i].width = mode->hdisplay;
			parms[i].height = mode->vdisplay;
			parms[i].mask = 1 << 0;
		} else if (plane->type == DRM_PLANE_TYPE_CURSOR) {
			parms[i].fb = argb_fb;
			parms[i].width = cursor_width;
			parms[i].height = cursor_height;
			parms[i].mask = 1 << 1;
		} else {
			if (!n_overlays)
				alpha = igt_plane_has_format_mod(plane,
					DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR);
			parms[i].fb = sprite_fb;
			parms[i].mask = 1 << 2;

			iter_mask |= 1 << 2;

			overlays[n_overlays++] = i;
		}
	}

	if (n_overlays >= 2) {
		uint32_t i;

		/*
		 * Create 2 groups for overlays, make sure 1 plane is put
		 * in each then spread the rest out.
		 */
		iter_mask |= 1 << 3;
		parms[overlays[n_overlays - 1]].mask = 1 << 3;

		for (i = 1; i < n_overlays - 1; i++) {
			int val = hars_petruska_f54_1_random_unsafe_max(2);

			parms[overlays[i]].mask = 1 << (2 + val);
		}
	}

	igt_create_fb(data->drm_fd, cursor_width, cursor_height,
		      DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR, argb_fb);

	igt_create_fb(data->drm_fd, cursor_width, cursor_height,
		      DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR, sprite_fb);

	*iter_max = iter_mask + 1;
	if (!n_overlays)
		return;

	/*
	 * Pre gen9 not all sizes are supported, find the biggest possible
	 * size that can be enabled on all sprite planes.
	 */
	prev_w = sprite_width = cursor_width;
	prev_h = sprite_height = cursor_height;

	max_sprite_width = (sprite_width == mode->hdisplay);
	max_sprite_height = (sprite_height == mode->vdisplay);

	while (!max_sprite_width && !max_sprite_height) {
		int ret;

		set_sprite_wh(data, crtc,
			      parms, sprite_fb,
			      alpha, sprite_width, sprite_height);

		wm_setup_plane(data, crtc,
			       (1 << n_planes) - 1, parms,
			       false);
		ret = igt_display_try_commit_atomic(&data->display, DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
		igt_assert(!is_atomic_check_failure_errno(ret));

		if (!is_atomic_check_plane_size_errno(ret)) {
			prev_w = sprite_width;
			prev_h = sprite_height;
			sprite_width *= max_sprite_width ? 1 : 2;
			if (sprite_width >= mode->hdisplay) {
				max_sprite_width = true;

				sprite_width = mode->hdisplay;
			}

			sprite_height *= max_sprite_height ? 1 : 2;
			if (sprite_height >= mode->vdisplay) {
				max_sprite_height = true;

				sprite_height = mode->vdisplay;
			}
			continue;
		}

		if (cursor_width == sprite_width &&
		    cursor_height == sprite_height) {
			igt_plane_t *removed_plane = NULL;
			igt_assert_f(n_planes >= 3, "No planes left to proceed with!");
			if (n_overlays > 0) {
				uint32_t plane_to_remove = hars_petruska_f54_1_random_unsafe_max(n_overlays);
				removed_plane = &crtc->planes[overlays[plane_to_remove]];
				igt_plane_set_fb(removed_plane, NULL);
				while (plane_to_remove < (n_overlays - 1)) {
					overlays[plane_to_remove] = overlays[plane_to_remove + 1];
					plane_to_remove++;
				}
				n_overlays--;
			}
			if (removed_plane) {
				parms[removed_plane->index].mask = 0;
				igt_info("Removed plane %d\n", removed_plane->index);
			}
			n_planes--;
			igt_info("Reduced available planes to %d\n", n_planes);
			continue;
		}

		sprite_width = prev_w;
		sprite_height = prev_h;

		if (!max_sprite_width)
			max_sprite_width = true;
		else
			max_sprite_height = true;
	}

	set_sprite_wh(data, crtc, parms,
			sprite_fb,
			alpha, sprite_width, sprite_height);

	igt_info("Running test on pipe %s with resolution %dx%d and sprite size %dx%d alpha %i\n",
		 igt_crtc_name(crtc), mode->hdisplay, mode->vdisplay,
		 sprite_width, sprite_height, alpha);
}

static void prepare_fencing(data_t *data, igt_crtc_t *crtc)
{
	igt_plane_t *plane;
	int n_planes;

	igt_require_sw_sync();

	n_planes = crtc->n_planes;
	timeline = calloc(n_planes, sizeof(*timeline));
	igt_assert_f(timeline != NULL, "Failed to allocate memory for timelines\n");
	thread = calloc(n_planes, sizeof(*thread));
	igt_assert_f(thread != NULL, "Failed to allocate memory for thread\n");
	seqno = calloc(n_planes, sizeof(*seqno));
	igt_assert_f(seqno != NULL, "Failed to allocate memory for seqno\n");

	for_each_plane_on_crtc(crtc,
			       plane)
		timeline[plane->index] = sw_sync_timeline_create();
}

static void unprepare_fencing(data_t *data, igt_crtc_t *crtc)
{
	igt_plane_t *plane;

	/* Make sure these got allocated in the first place */
	if (!timeline)
		return;

	for_each_plane_on_crtc(crtc,
			       plane)
		close(timeline[plane->index]);

	free(timeline);
	free(thread);
	free(seqno);
}

static void atomic_commit(data_t *data_v, igt_crtc_t *crtc,
			  unsigned int flags, void *data, bool fencing)
{
	if (fencing)
		igt_crtc_request_out_fence(crtc);

	igt_display_commit_atomic(&data_v->display, flags, data);
}

static int fd_completed(int fd)
{
	struct pollfd pfd = { fd, POLLIN };
	int ret;

	ret = poll(&pfd, 1, 0);
	igt_assert_lte(0, ret);
	return ret;
}

static void wait_for_transition(data_t *data, igt_crtc_t *crtc,
				bool nonblocking, bool fencing)
{
	if (fencing) {
		int fence_fd = crtc->out_fence_fd;

		if (!nonblocking)
			igt_assert(fd_completed(fence_fd));

		igt_assert(sync_fence_wait(fence_fd, 30000) == 0);
	} else {
		if (!nonblocking)
			igt_assert(fd_completed(data->drm_fd));

		drmHandleEvent(data->drm_fd, &drm_events);
	}
}

/*
 * 1. Set primary plane to a known fb.
 * 2. Make sure getcrtc returns the correct fb id.
 * 3. Call rmfb on the fb.
 * 4. Make sure getcrtc returns 0 fb id.
 *
 * RMFB is supposed to free the framebuffers from any and all planes,
 * so test this and make sure it works.
 */
static void
run_transition_test(data_t *data, igt_crtc_t *crtc, igt_output_t *output,
		    enum transition_type type, bool nonblocking, bool fencing)
{
	drmModeModeInfo *mode, override_mode;
	igt_plane_t *plane;
	uint32_t iter_max, i;
	struct plane_parms parms[crtc->n_planes];
	unsigned flags = 0;
	int ret;

	igt_info("Using (pipe %s + %s) to run the subtest.\n",
		 igt_crtc_name(crtc), igt_output_name(output));

	if (fencing)
		prepare_fencing(data, crtc);
	else
		flags |= DRM_MODE_PAGE_FLIP_EVENT;

	if (nonblocking)
		flags |= DRM_MODE_ATOMIC_NONBLOCK;

	if (type >= TRANSITION_MODESET)
		flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;

	mode = igt_output_get_mode(output);
	override_mode = *mode;
	/* try to force a modeset */
	override_mode.flags ^= DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NHSYNC;

	igt_create_fb(data->drm_fd, mode->hdisplay, mode->vdisplay,
		      DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR, &data->fbs[0]);

	igt_output_set_crtc(output, crtc);

	wm_setup_plane(data, crtc, 0, NULL,
		       false);

	if (flags & DRM_MODE_ATOMIC_ALLOW_MODESET) {
		igt_output_set_crtc(output, NULL);

		igt_display_commit2(&data->display, COMMIT_ATOMIC);

		igt_output_set_crtc(output,
				    crtc);
	}

	setup_parms(data, crtc, mode,
		    &data->fbs[0], &data->argb_fb,
		    &data->sprite_fb, parms, &iter_max);

	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	/*
	 * In some configurations the tests may not run to completion with all
	 * sprite planes lit up at 4k resolution, try decreasing width/size of secondary
	 * planes to fix this
	 */
	while (1) {
		wm_setup_plane(data, crtc,
			       iter_max - 1, parms, false);

		if (fencing)
			igt_crtc_request_out_fence(crtc);

		ret = igt_display_try_commit_atomic(&data->display, DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
		igt_assert(!is_atomic_check_failure_errno(ret));

		if (!is_atomic_check_plane_size_errno(ret) || crtc->n_planes < 3)
			break;

		ret = 0;
		for_each_plane_on_crtc(crtc,
				       plane) {
			i = plane->index;

			if (plane->type == DRM_PLANE_TYPE_PRIMARY ||
			    plane->type == DRM_PLANE_TYPE_CURSOR)
				continue;

			parms[i].width /= 2;
			ret = 1;
			igt_info("Reducing sprite %i to %ux%u\n", i - 1, parms[i].width, parms[i].height);
			break;
		}

		igt_skip_on_f(!ret,
			      "Cannot run tests without proper size sprite planes\n");
	}

	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	if (type == TRANSITION_AFTER_FREE) {
		int fence_fd = -1;

		wm_setup_plane(data, crtc,
			       0, parms, fencing);

		atomic_commit(data, crtc,
			      flags,
			      (void *)(unsigned long)0, fencing);
		if (fencing) {
			fence_fd = crtc->out_fence_fd;
			crtc->out_fence_fd = -1;
		}

		/* force planes to be part of commit */
		for_each_plane_on_crtc(crtc,
				       plane) {
			if (parms[plane->index].mask)
				igt_plane_set_position(plane, 0, 0);
		}

		igt_display_commit2(&data->display, COMMIT_ATOMIC);

		if (fence_fd != -1) {
			igt_assert(fd_completed(fence_fd));
			close(fence_fd);
		} else {
			igt_assert(fd_completed(data->drm_fd));
			wait_for_transition(data,
					    crtc,
					    false, fencing);
		}
		return;
	}

	for (i = 0; i < iter_max; i++) {
		int n_enable_planes = igt_hweight(i);

		if (type == TRANSITION_MODESET_FAST &&
		    n_enable_planes > 1 &&
		    n_enable_planes < crtc->n_planes)
			continue;

		igt_output_set_crtc(output,
				    crtc);

		if (!wm_setup_plane(data, crtc, i, parms, fencing))
			continue;

		atomic_commit(data, crtc,
			      flags,
			      (void *)(unsigned long)i, fencing);
		wait_for_transition(data,
				    crtc,
				    nonblocking, fencing);

		if (type == TRANSITION_MODESET_DISABLE) {
			igt_output_set_crtc(output, NULL);

			if (!wm_setup_plane(data, crtc, 0, parms, fencing))
				continue;

			atomic_commit(data,
				      crtc,
				      flags, (void *) 0UL,
				      fencing);
			wait_for_transition(data,
					    crtc,
					    nonblocking,
					    fencing);
		} else {
			uint32_t j;

			/* i -> i+1 will be done when i increases, can be skipped here */
			for (j = iter_max - 1; j > i + 1; j--) {
				n_enable_planes = igt_hweight(j);

				if (type == TRANSITION_MODESET_FAST &&
				    n_enable_planes > 1 &&
				    n_enable_planes < crtc->n_planes)
					continue;

				if (!wm_setup_plane(data, crtc, j, parms, fencing))
					continue;

				if (type >= TRANSITION_MODESET)
					igt_output_override_mode(output, &override_mode);

				atomic_commit(data,
					      crtc,
					      flags,
					      (void *)(unsigned long) j,
					      fencing);
				wait_for_transition(data,
						    crtc,
						    nonblocking, fencing);

				if (!wm_setup_plane(data, crtc, i, parms, fencing))
					continue;

				if (type >= TRANSITION_MODESET)
					igt_output_override_mode(output, NULL);

				atomic_commit(data,
					      crtc,
					      flags,
					      (void *)(unsigned long) i,
					      fencing);
				wait_for_transition(data,
						    crtc,
						    nonblocking, fencing);
			}
		}
	}
}

static void test_cleanup(data_t *data, igt_crtc_t *crtc, igt_output_t *output,
			 bool fencing)
{
	igt_plane_t *plane;

	if (fencing)
		unprepare_fencing(data,
				  crtc);

	igt_output_set_crtc(output, NULL);

	for_each_plane_on_crtc(crtc,
			       plane)
		igt_plane_set_fb(plane, NULL);

	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	igt_remove_fb(data->drm_fd, &data->fbs[0]);
	igt_remove_fb(data->drm_fd, &data->fbs[1]);
	igt_remove_fb(data->drm_fd, &data->argb_fb);
	igt_remove_fb(data->drm_fd, &data->sprite_fb);
}

static void commit_display(data_t *data, unsigned event_mask, bool nonblocking)
{
	unsigned flags;
	int num_events = igt_hweight(event_mask);
	ssize_t ret;

	flags = DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_PAGE_FLIP_EVENT;
	if (nonblocking)
		flags |= DRM_MODE_ATOMIC_NONBLOCK;

	do {
		ret = igt_display_try_commit_atomic(&data->display, flags, NULL);
	} while (ret == -EBUSY);

	igt_assert_eq(ret, 0);

	igt_debug("Event mask: %x, waiting for %i events\n", event_mask, num_events);

	igt_set_timeout(30, "Waiting for events timed out\n");

	while (num_events) {
		char buf[32];
		struct drm_event *e = (void *)buf;
		struct drm_event_vblank *vblank = (void *)buf;

		igt_set_timeout(3, "Timed out while reading drm_fd\n");
		ret = read(data->drm_fd, buf, sizeof(buf));
		igt_reset_timeout();
		if (ret < 0 && (errno == EINTR || errno == EAGAIN))
			continue;

		igt_assert(ret >= 0);
		igt_assert_eq(e->type, DRM_EVENT_FLIP_COMPLETE);

		igt_debug("Retrieved vblank seq: %u on unk/unk\n", vblank->sequence);

		num_events--;
	}

	igt_reset_timeout();
}

static void unset_output_pipe(igt_display_t *display)
{
	int i;

	for (i = 0; i < display->n_outputs; i++)
		igt_output_set_crtc(&display->outputs[i], NULL);
}

static unsigned set_combinations(data_t *data, unsigned mask, struct igt_fb *fb)
{
	igt_output_t *output;
	igt_crtc_t *crtc;
	unsigned event_mask = 0;

	unset_output_pipe(&data->display);

	for_each_crtc(&data->display, crtc) {
		igt_plane_t *plane = igt_crtc_get_plane_type(crtc,
							     DRM_PLANE_TYPE_PRIMARY);

		igt_crtc_t *old_crtc = plane->ref->crtc;

		/*
		 * If a plane is being shared by multiple pipes, we must disable the pipe that
		 * currently is holding the plane
		 */
		if (old_crtc != crtc) {
			igt_plane_t *old_plane = igt_crtc_get_plane_type(old_crtc,
									 DRM_PLANE_TYPE_PRIMARY);

			igt_plane_set_fb(old_plane, NULL);
			igt_display_commit2(&data->display, COMMIT_ATOMIC);
		}
	}

	for_each_crtc(&data->display, crtc) {
		igt_plane_t *plane = igt_crtc_get_plane_type(crtc,
							     DRM_PLANE_TYPE_PRIMARY);
		drmModeModeInfo *mode = NULL;

		if (!(mask & (1 << crtc->pipe))) {
			if (igt_crtc_is_prop_changed(crtc, IGT_CRTC_ACTIVE)) {
				event_mask |= 1 << crtc->pipe;
				igt_plane_set_fb(plane, NULL);
			}

			continue;
		}

		event_mask |= 1 << crtc->pipe;

		for_each_valid_output_on_pipe(&data->display, crtc->pipe,
					      output) {
			if (igt_output_get_driving_crtc(output) != NULL)
				continue;

			igt_output_set_crtc(output,
					    crtc);
			if (intel_pipe_output_combo_valid(&data->display)) {
				mode = igt_output_get_mode(output);
				break;
			} else {
				igt_output_set_crtc(output, NULL);
			}
		}

		if (!mode)
			return 0;

		igt_output_set_crtc(output,
				    crtc);
		igt_plane_set_fb(plane, fb);
		igt_fb_set_size(fb, plane, mode->hdisplay, mode->vdisplay);
		igt_plane_set_size(plane, mode->hdisplay, mode->vdisplay);
	}

	return event_mask;
}

static void refresh_primaries(data_t  *data, int mask)
{
	igt_crtc_t *crtc;
	igt_plane_t *plane;

	for_each_crtc(&data->display, crtc) {
		if (!((1 << crtc->pipe) & mask))
			continue;

		for_each_plane_on_crtc(crtc,
				       plane)
			if (plane->type == DRM_PLANE_TYPE_PRIMARY)
				igt_plane_set_position(plane, 0, 0);
	}
}

static void collect_crcs_mask(igt_pipe_crc_t **pipe_crcs, unsigned mask, igt_crc_t *crcs)
{
	int i;

	for (i = 0; i < IGT_MAX_PIPES; i++) {
		if (!((1 << i) & mask))
			continue;

		if (!pipe_crcs[i])
			continue;

		igt_pipe_crc_collect_crc(pipe_crcs[i], &crcs[i]);
	}
}

static void run_modeset_tests(data_t *data, int howmany, bool nonblocking, bool fencing)
{
	igt_crtc_t *crtc;
	int i, j;
	unsigned iter_max;
	igt_output_t *output;
	uint16_t width = 0, height = 0;

retry:
	unset_output_pipe(&data->display);

	j = 0;
	for_each_connected_output(&data->display, output) {
		drmModeModeInfo *mode = igt_output_get_mode(output);

		width = max(width, mode->hdisplay);
		height = max(height, mode->vdisplay);
	}

	igt_create_pattern_fb(data->drm_fd, width, height,
				   DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR, &data->fbs[0]);
	igt_create_color_pattern_fb(data->drm_fd, width, height,
				    DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR, .5, .5, .5, &data->fbs[1]);

	for_each_crtc(&data->display, crtc) {
		igt_plane_t *plane = igt_crtc_get_plane_type(crtc,
							     DRM_PLANE_TYPE_PRIMARY);
		drmModeModeInfo *mode = NULL;

		/* count enable pipes to set max iteration */
		j += 1;

		if (is_intel_device(data->drm_fd))
			data->pipe_crcs[crtc->pipe] = igt_crtc_crc_new(crtc,
							      IGT_PIPE_CRC_SOURCE_AUTO);

		for_each_valid_output_on_pipe(&data->display, crtc->pipe,
					      output) {
			if (igt_output_get_driving_crtc(output) != NULL)
				continue;

			igt_output_set_crtc(output,
					    crtc);
			if (intel_pipe_output_combo_valid(&data->display)) {
				mode = igt_output_get_mode(output);

				igt_info("(pipe %s + %s), mode:",
					 igt_crtc_name(crtc),
					 igt_output_name(output));
				kmstest_dump_mode(mode);

				break;
			} else {
				igt_output_set_crtc(output, NULL);
			}
		}

		if (mode) {
			igt_plane_set_fb(plane, &data->fbs[1]);
			igt_fb_set_size(&data->fbs[1], plane, mode->hdisplay, mode->vdisplay);
			igt_plane_set_size(plane, mode->hdisplay, mode->vdisplay);

			if (fencing)
				igt_crtc_request_out_fence(crtc);
		} else {
			igt_plane_set_fb(plane, NULL);
		}
	}

	iter_max = (j > 0) ? (j << 1) : 1;

	if (igt_run_in_simulation() && iter_max > 1)
		iter_max = iter_max >> 2;

	if (igt_display_try_commit_atomic(&data->display,
				DRM_MODE_ATOMIC_TEST_ONLY |
				DRM_MODE_ATOMIC_ALLOW_MODESET,
				NULL) != 0) {
		bool found = igt_override_all_active_output_modes_to_fit_bw(&data->display);
		igt_require_f(found, "No valid mode combo found.\n");

		goto retry;
	}
	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	for (i = 0; i < iter_max; i++) {
		igt_crc_t crcs[5][IGT_MAX_PIPES];
		unsigned event_mask;

		if (igt_hweight(i) > howmany)
			continue;

		event_mask = set_combinations(data, i, &data->fbs[0]);
		if (!event_mask && i)
			continue;

		commit_display(data, event_mask, nonblocking);

		collect_crcs_mask(data->pipe_crcs, i, crcs[0]);

		for (j = iter_max - 1; j > i + 1; j--) {
			if (igt_hweight(j) > howmany)
				continue;

			if (igt_hweight(i) < howmany && igt_hweight(j) < howmany)
				continue;

			event_mask = set_combinations(data, j, &data->fbs[1]);
			if (!event_mask)
				continue;

			commit_display(data, event_mask, nonblocking);

			collect_crcs_mask(data->pipe_crcs, j, crcs[1]);

			refresh_primaries(data, j);
			commit_display(data, j, nonblocking);
			collect_crcs_mask(data->pipe_crcs, j, crcs[2]);

			event_mask = set_combinations(data, i, &data->fbs[0]);
			if (!event_mask)
				continue;

			commit_display(data, event_mask, nonblocking);
			collect_crcs_mask(data->pipe_crcs, i, crcs[3]);

			refresh_primaries(data, i);
			commit_display(data, i, nonblocking);
			collect_crcs_mask(data->pipe_crcs, i, crcs[4]);

			if (!is_intel_device(data->drm_fd))
				continue;

			for (int k = 0; k < IGT_MAX_PIPES; k++) {
				if (i & (1 << k)) {
					igt_assert_crc_equal(&crcs[0][k], &crcs[3][k]);
					igt_assert_crc_equal(&crcs[0][k], &crcs[4][k]);
				}

				if (j & (1 << k))
					igt_assert_crc_equal(&crcs[1][k], &crcs[2][k]);
			}
		}
	}

	/* Cleanup */
	unset_output_pipe(&data->display);
	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	if (is_intel_device(data->drm_fd)) {
		for_each_crtc(&data->display, crtc)
			igt_pipe_crc_free(data->pipe_crcs[crtc->pipe]);
	}

	igt_remove_fb(data->drm_fd, &data->fbs[0]);
	igt_remove_fb(data->drm_fd, &data->fbs[1]);
}

static void run_modeset_transition(data_t *data, int requested_outputs, bool nonblocking, bool fencing)
{
	igt_output_t *outputs[IGT_MAX_PIPES] = {};
	int num_outputs = 0;
	igt_crtc_t *crtc;

	for_each_crtc(&data->display, crtc) {
		igt_output_t *output;

		for_each_valid_output_on_pipe(&data->display, crtc->pipe,
					      output) {
			int i;

			for (i = crtc->pipe - 1; i >= 0; i--)
				if (outputs[i] == output)
					break;

			if (i < 0) {
				outputs[crtc->pipe] = output;
				num_outputs++;
				break;
			}
		}
	}

	if (num_outputs < requested_outputs) {
		igt_debug("Should have at least %i outputs, found %i\n",
			  requested_outputs, num_outputs);
		return;
	}

	igt_dynamic_f("%ix-outputs", requested_outputs)
		run_modeset_tests(data, requested_outputs, nonblocking, fencing);
}

static bool pipe_output_combo_valid(igt_display_t *display, igt_crtc_t *crtc,
				    igt_output_t *output)
{
	bool ret = true;

	igt_display_reset(display);

	igt_output_set_crtc(output, crtc);
	if (!intel_pipe_output_combo_valid(display))
		ret = false;
	igt_output_set_crtc(output, NULL);

	return ret;
}

static int opt_handler(int opt, int opt_index, void *_data)
{
	data_t *data = _data;

	switch (opt) {
	case 'e':
		data->extended = true;
		break;
	}

	return IGT_OPT_HANDLER_SUCCESS;
}

static const struct option long_opts[] = {
	{ .name = "extended", .has_arg = false, .val = 'e', },
	{}
};

static const char help_str[] =
	"  --extended\t\tRun the extended tests\n";

static data_t data;

int igt_main_args("", long_opts, help_str, opt_handler, &data)
{
	igt_output_t *output;
	igt_crtc_t *crtc;
	struct {
		const char *name;
		enum transition_type type;
		bool nonblocking;
		bool fencing;
		const char *desc;
	} transition_tests[] = {
		{ "plane-all-transition", TRANSITION_PLANES, false, false,
		  "Transition test for all plane combinations" },
		{ "plane-all-transition-fencing", TRANSITION_PLANES, false, true,
		  "Transition test for all plane combinations with fencing commit" },
		{ "plane-all-transition-nonblocking", TRANSITION_PLANES, true, false,
		  "Transition test for all plane combinations with nonblocking commit" },
		{ "plane-all-transition-nonblocking-fencing", TRANSITION_PLANES, true, true,
		  "Transition test for all plane combinations with nonblocking and fencing commit" },
		{ "plane-use-after-nonblocking-unbind", TRANSITION_AFTER_FREE, true, false,
		  "Transition test with non blocking commit and make sure commit of disabled plane has "
		       "to complete before atomic commit on that plane" },
		{ "plane-use-after-nonblocking-unbind-fencing", TRANSITION_AFTER_FREE, true, true,
		  "Transition test with non blocking and fencing commit and make sure commit of "
		       "disabled plane has to complete before atomic commit on that plane" },
		{ "plane-all-modeset-transition", TRANSITION_MODESET, false, false,
		  "Modeset test for all plane combinations" },
		{ "plane-all-modeset-transition-fencing", TRANSITION_MODESET, false, true,
		  "Modeset test for all plane combinations with fencing commit" },
		{ "plane-all-modeset-transition-internal-panels", TRANSITION_MODESET_FAST, false, false,
		  "Modeset test for all plane combinations on internal panels" },
		{ "plane-all-modeset-transition-fencing-internal-panels", TRANSITION_MODESET_FAST, false, true,
		  "Modeset test for all plane combinations on internal panels with fencing commit" },
		{ "plane-toggle-modeset-transition", TRANSITION_MODESET_DISABLE, false, false,
		  "Check toggling and modeset transition on plane" },
	};
	struct {
		const char *name;
		bool nonblocking;
		bool fencing;
		const char *desc;
	} modeset_tests[] = {
		{ "modeset-transition", false, false,
		  "Modeset transition tests for combinations of crtc enabled" },
		{ "modeset-transition-fencing", false, true,
		  "Modeset transition tests for combinations of crtc enabled with fencing commit" },
		{ "modeset-transition-nonblocking", true, false,
		  "Modeset transition tests for combinations of crtc enabled with nonblocking commit" },
		{ "modeset-transition-nonblocking-fencing", true, true,
		  "Modeset transition tests for combinations of crtc enabled with nonblocking and fencing commit" },
	};
	int i, j, count = 0;
	int pipe_count = 0;

	igt_fixture() {
		unsigned int debug_mask_if_ci = DRM_UT_KMS;
		data.drm_fd = drm_open_driver_master(DRIVER_ANY);

		kmstest_set_vt_graphics_mode();

		igt_display_require(&data.display, data.drm_fd);
		igt_require(data.display.is_atomic);

		igt_display_require_output(&data.display);

		for_each_connected_output(&data.display, output)
			count++;

		igt_install_exit_handler(igt_drm_debug_mask_reset_exit_handler);
		update_debug_mask_if_ci(debug_mask_if_ci);
	}

	igt_describe("Check toggling of primary plane with vblank");
	igt_subtest_with_dynamic("plane-primary-toggle-with-vblank-wait") {
		pipe_count = 0;

		for_each_crtc_with_valid_output(&data.display, crtc, output) {
			if (pipe_count == 2 * count && !data.extended)
				break;

			if (!pipe_output_combo_valid(&data.display, crtc, output))
				continue;

			pipe_count++;
			igt_dynamic_f("pipe-%s-%s", igt_crtc_name(crtc),
				      igt_output_name(output))
				run_primary_test(&data,
						 crtc,
						 output);
			test_cleanup(&data,
				     crtc,
				     output, false);
		}
	}

	for (i = 0; i < ARRAY_SIZE(transition_tests); i++) {
		if (strstr(transition_tests[i].name, "modeset"))
			update_debug_mask_if_ci(DRM_UT_DRIVER);

		igt_describe(transition_tests[i].desc);
		igt_subtest_with_dynamic_f("%s", transition_tests[i].name) {
			pipe_count = 0;

			for_each_crtc_with_valid_output(&data.display, crtc,
							output) {
				/*
				 * Test modeset cases on internal panels separately with a reduced
				 * number of combinations, to avoid long runtimes due to modesets on
				 * panels with long power cycle delays.
				 */
				if ((transition_tests[i].type == TRANSITION_MODESET) &&
				    output_is_internal_panel(output))
					continue;

				if ((transition_tests[i].type == TRANSITION_MODESET_FAST) &&
				    !output_is_internal_panel(output))
					continue;

				if (pipe_count == 2 * count && !data.extended)
					break;

				if (!pipe_output_combo_valid(&data.display, crtc, output))
					continue;

				pipe_count++;
				igt_dynamic_f("pipe-%s-%s",
					      igt_crtc_name(crtc),
					      igt_output_name(output))
					run_transition_test(&data,
							    crtc,
							    output,
							    transition_tests[i].type,
							    transition_tests[i].nonblocking,
							    transition_tests[i].fencing);

				test_cleanup(&data,
					     crtc,
					     output,
					     transition_tests[i].fencing);
			}
		}
	}

	for (i = 0; i < ARRAY_SIZE(modeset_tests); i++) {
		if (igt_get_connected_output_count(&data.display) > 2)
			update_debug_mask_if_ci(DRM_UT_DRIVER);

		igt_describe_f("%s", modeset_tests[i].desc);
		igt_subtest_with_dynamic_f("%s", modeset_tests[i].name) {
			for (j = 1; j <= count; j++) {
				run_modeset_transition(&data, j,
						       modeset_tests[i].nonblocking,
						       modeset_tests[i].fencing);
			}
		}
	}

	igt_fixture() {
		igt_display_fini(&data.display);
		drm_close_driver(data.drm_fd);
	}
}
