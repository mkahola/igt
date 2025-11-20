// SPDX-License-Identifier: MIT
// Copyright © 2023 Simon Ser

#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <sys/eventfd.h>

#include "drm.h"
#include "igt.h"
#include "igt_syncobj.h"
#include "sw_sync.h"

/**
 * TEST: syncobj eventfd
 * Description: Tests for the drm sync object eventfd API
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: DRM
 * Functionality: semaphore
 * Feature: synchronization
 * Test category: GEM_Legacy
 */

IGT_TEST_DESCRIPTION("Tests for the drm sync object eventfd API");

static bool
has_syncobj_eventfd(int fd)
{
	uint64_t value;
	int ret;

	if (drmGetCap(fd, DRM_CAP_SYNCOBJ_TIMELINE, &value))
		return false;
	if (!value)
		return false;

	/* Try waiting with invalid syncobj should fail with ENOENT */
	ret = __syncobj_eventfd(fd, 0, 0, 0, -1);
	return ret == -ENOENT;
}

static int
syncobj_attach_sw_sync(int fd, uint32_t handle, uint64_t point)
{
	int timeline, fence;
	uint32_t syncobj;

	timeline = sw_sync_timeline_create();
	fence = sw_sync_timeline_create_fence(timeline, 1);

	if (point == 0) {
		syncobj_import_sync_file(fd, handle, fence);
	} else {
		syncobj = syncobj_create(fd, 0);

		syncobj_import_sync_file(fd, syncobj, fence);
		syncobj_binary_to_timeline(fd, handle, point, syncobj);
		syncobj_destroy(fd, syncobj);
	}

	close(fence);

	return timeline;
}

static int
ev_fd_read(int ev_fd)
{
	uint64_t ev_fd_value;
	int ret;

	ret = read(ev_fd, &ev_fd_value, sizeof(ev_fd_value));
	if (ret == -1)
		return -errno;
	igt_assert_eq(ret, sizeof(ev_fd_value));
	return 0;
}

static void
ev_fd_poll_in(int ev_fd, bool avail)
{
	struct pollfd pollfd;
	int ret;
	int timeout_ms;

	/* Wait 5s if we're expecting data, 10ms otherwise */
	timeout_ms = avail ? 5000 : 10;
	pollfd.fd = ev_fd;
	pollfd.events = POLLIN;
	pollfd.revents = 0;
	ret = poll(&pollfd, 1, timeout_ms);
	if (avail) {
		igt_assert(ret >= 0);
		igt_assert(pollfd.revents & POLLIN);
	} else {
		igt_assert_eq(ret, 0);
	}
}

static void
ev_fd_assert_unsignaled(int ev_fd)
{
	/* Poll the eventfd to give the kernel time to signal it, error out if
	 * that happens */
	ev_fd_poll_in(ev_fd, false);
	igt_assert_eq(ev_fd_read(ev_fd), -EAGAIN);
}

static void
ev_fd_assert_signaled(int ev_fd)
{
	ev_fd_poll_in(ev_fd, true);
	igt_assert_eq(ev_fd_read(ev_fd), 0);
}

static const char test_bad_flags_desc[] =
	"Verifies that passing bad flags is rejected";
static void
test_bad_flags(int fd)
{
	uint32_t flags;
	uint32_t syncobj;
	int ev_fd;

	syncobj = syncobj_create(fd, DRM_SYNCOBJ_CREATE_SIGNALED);
	flags = 0xdeadbeef;
	ev_fd = eventfd(0, EFD_NONBLOCK);
	igt_assert_eq(__syncobj_eventfd(fd, syncobj, 0, flags, ev_fd), -EINVAL);

	close(ev_fd);
	syncobj_destroy(fd, syncobj);
}

static const char test_illegal_handle_desc[] =
	"Verifies that passing an invalid syncobj handle is rejected";
static void
test_illegal_handle(int fd)
{
	int ev_fd;

	ev_fd = eventfd(0, EFD_NONBLOCK);
	igt_assert_eq(__syncobj_eventfd(fd, 0, 0, 0, ev_fd), -ENOENT);

	close(ev_fd);
}

static const char test_illegal_eventfd_desc[] =
	"Verifies that passing an invalid eventfd is rejected";
static void
test_illegal_eventfd(int fd)
{
	int dev_null;
	uint32_t syncobj;

	syncobj = syncobj_create(fd, DRM_SYNCOBJ_CREATE_SIGNALED);

	dev_null = open("/dev/null", O_RDWR);
	igt_assert(dev_null >= 0);

	igt_assert_eq(__syncobj_eventfd(fd, syncobj, 0, 0, dev_null), -EINVAL);

	close(dev_null);
	syncobj_destroy(fd, syncobj);
}

static const char test_bad_pad_desc[] =
	"Verifies that passing a non-zero padding is rejected";
static void
test_bad_pad(int fd)
{
	struct drm_syncobj_eventfd args;
	int ret;

	args.handle = syncobj_create(fd, DRM_SYNCOBJ_CREATE_SIGNALED);
	args.flags = 0;
	args.point = 0;
	args.fd = eventfd(0, EFD_NONBLOCK);
	args.pad = 0xdeadbeef;

	ret = drmIoctl(fd, DRM_IOCTL_SYNCOBJ_EVENTFD, &args);
	igt_assert(ret == -1 && errno == EINVAL);
}

static const char test_wait_desc[] =
	"Verifies waiting an already-materialized fence";
static void
test_wait(int fd, bool use_timeline)
{
	uint32_t syncobj;
	int timeline, ev_fd_wait, ev_fd_avail;
	uint64_t point = use_timeline ? 1 : 0;

	syncobj = syncobj_create(fd, 0);
	timeline = syncobj_attach_sw_sync(fd, syncobj, point);
	ev_fd_wait = eventfd(0, EFD_NONBLOCK);
	ev_fd_avail = eventfd(0, EFD_NONBLOCK);

	syncobj_eventfd(fd, syncobj, point, 0, ev_fd_wait);
	syncobj_eventfd(fd, syncobj, point, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE,
			ev_fd_avail);

	ev_fd_assert_unsignaled(ev_fd_wait);
	ev_fd_assert_signaled(ev_fd_avail);

	sw_sync_timeline_inc(timeline, 1);

	ev_fd_assert_signaled(ev_fd_wait);

	close(ev_fd_wait);
	close(ev_fd_avail);
	close(timeline);
	syncobj_destroy(fd, syncobj);
}

static const char test_wait_before_signal_desc[] =
	"Verifies waiting a fence not yet materialized";
static void
test_wait_before_signal(int fd, bool use_timeline)
{
	uint32_t syncobj;
	int timeline, ev_fd_wait, ev_fd_avail;
	uint64_t point = use_timeline ? 1 : 0;

	syncobj = syncobj_create(fd, 0);
	ev_fd_wait = eventfd(0, EFD_NONBLOCK);
	ev_fd_avail = eventfd(0, EFD_NONBLOCK);

	syncobj_eventfd(fd, syncobj, point, 0, ev_fd_wait);
	syncobj_eventfd(fd, syncobj, point, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE,
			ev_fd_avail);

	ev_fd_assert_unsignaled(ev_fd_wait);
	ev_fd_assert_unsignaled(ev_fd_avail);

	timeline = syncobj_attach_sw_sync(fd, syncobj, point);

	ev_fd_assert_unsignaled(ev_fd_wait);
	ev_fd_assert_signaled(ev_fd_avail);

	sw_sync_timeline_inc(timeline, 1);

	ev_fd_assert_signaled(ev_fd_wait);

	close(ev_fd_wait);
	close(ev_fd_avail);
	close(timeline);
	syncobj_destroy(fd, syncobj);
}

static const char test_wait_signaled_desc[] =
	"Verifies waiting an already-signaled fence";
static void
test_wait_signaled(int fd, bool use_timeline)
{
	uint32_t syncobj;
	int timeline, ev_fd_wait, ev_fd_avail;
	uint64_t point = use_timeline ? 1 : 0;

	syncobj = syncobj_create(fd, 0);
	ev_fd_wait = eventfd(0, EFD_NONBLOCK);
	ev_fd_avail = eventfd(0, EFD_NONBLOCK);

	timeline = syncobj_attach_sw_sync(fd, syncobj, point);
	sw_sync_timeline_inc(timeline, 1);

	syncobj_eventfd(fd, syncobj, point, 0, ev_fd_wait);
	syncobj_eventfd(fd, syncobj, point, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE,
			ev_fd_avail);

	ev_fd_assert_signaled(ev_fd_wait);
	ev_fd_assert_signaled(ev_fd_avail);

	close(ev_fd_wait);
	close(ev_fd_avail);
	close(timeline);
	syncobj_destroy(fd, syncobj);
}

igt_main()
{
	int fd = -1, i;

	igt_fixture() {
		fd = drm_open_driver(DRIVER_ANY);
		igt_require(has_syncobj_eventfd(fd));
		igt_require_sw_sync();
	}

	igt_describe(test_bad_flags_desc);
	igt_subtest("invalid-bad-flags")
		test_bad_flags(fd);

	igt_describe(test_illegal_handle_desc);
	igt_subtest("invalid-illegal-handle")
		test_illegal_handle(fd);

	igt_describe(test_illegal_eventfd_desc);
	igt_subtest("invalid-illegal-eventfd")
		test_illegal_eventfd(fd);

	igt_describe(test_bad_pad_desc);
	igt_subtest("invalid-bad-pad")
		test_bad_pad(fd);

	for (i = 0; i < 2; i++) {
		bool use_timeline = i == 1;
		const char *kind = use_timeline ? "timeline" : "binary";

		igt_describe(test_wait_desc);
		igt_subtest_f("%s-wait", kind)
			test_wait(fd, use_timeline);

		igt_describe(test_wait_before_signal_desc);
		igt_subtest_f("%s-wait-before-signal", kind)
			test_wait_before_signal(fd, use_timeline);

		igt_describe(test_wait_signaled_desc);
		igt_subtest_f("%s-wait-signaled", kind)
			test_wait_signaled(fd, use_timeline);
	}

	igt_fixture() {
		drm_close_driver(fd);
	}
}
