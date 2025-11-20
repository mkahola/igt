/*
 * Copyright © 2020 Collabora, Ltd.
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
 *    Emil Velikov <emil.l.velikov@gmail.com>
 *
 */

/*
 * Testcase: Check that drop/setMaster behaves correctly wrt root/user access
 *
 * Test checks if the ioctls succeed or fail, depending if the applications was
 * run with root, user privileges or if we have separate privileged arbitrator.
 */

#include "igt.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
/**
 * TEST: core setmaster
 * Description: Check that Drop/SetMaster behaves correctly wrt root/user access
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: DRM
 * Functionality: permission management for clients
 * Feature: core
 * Test category: GEM_Legacy
 *
 * SUBTEST: master-drop-set-root
 * Description: Ensure that root can Set/DropMaster
 *
 * SUBTEST: master-drop-set-shared-fd
 * Description: Check the Set/DropMaster behaviour on shared fd
 *
 * SUBTEST: master-drop-set-user
 * Description: Ensure first normal user can Set/DropMaster
 */

IGT_TEST_DESCRIPTION("Check that Drop/SetMaster behaves correctly wrt root/user"
		     " access");

static bool is_master(int fd)
{
	/* FIXME: replace with drmIsMaster once we bumped libdrm version */
	return drmAuthMagic(fd, 0) != -EACCES;
}

static void check_drop_set(void)
{
	int master;

	master = __drm_open_driver(DRIVER_ANY);

	/* Ensure we have a valid device. This is _extremely_ unlikely to
	 * trigger as tweak_perm() aims to ensure we have the correct rights.
	 * Although:
	 * - igt_fork() + igt_skip() is broken, aka the igt_skip() is not
	 * propagated to the child and we FAIL with a misleading trace.
	 * - there is _no_ guarantee that we'll open a device handled by
	 * tweak_perm(), because __drm_open_driver() does a modprobe(8)
	 * - successfully opening a device is part of the test
	 */
	igt_assert_neq(master, -1);

	/* At this point we're master capable due to:
	 * - being root - always
	 * - normal user - as the only drm only drm client (on this VT)
	 */
	igt_assert_eq(is_master(master), true);

	/* If we have SYS_CAP_ADMIN we're in the textbook best-case scenario.
	 *
	 * Otherwise newer kernels allow the application to drop/revoke its
	 * master capability and request it again later.
	 *
	 * In this case, we address two types of issues:
	 * - the application no longer need suid-root (or equivalent) which
	 * was otherwise required _solely_ for these two ioctls
	 * - plenty of applications ignore (or discard) the result of the
	 * calls all together.
	 */
	igt_assert_eq(drmDropMaster(master), 0);
	igt_assert_eq(drmSetMaster(master), 0);

	drm_close_driver(master);
}

static void tweak_perm(uint8_t *saved_perm, char *path, bool save)
{
	struct stat st;
	int ret;

	if (path[0] == 0)
		return;

	ret = stat(path, &st);
	igt_assert_f(!ret, "stat failed with %d path=%s\n", errno, path);

	if (save) {
		/* Save and toggle */
		*saved_perm = st.st_mode & (S_IROTH | S_IWOTH);
		st.st_mode |= S_IROTH | S_IWOTH;
	} else {
		/* Clear and restore */
		st.st_mode &= ~(S_IROTH | S_IWOTH);
		st.st_mode |= *saved_perm;
	}

	/* There's only one way for chmod to fail - race vs rmmod. */
	ret = chmod(path, st.st_mode);
	igt_assert_f(!ret, "chmod failed with %d path=%s\n", errno, path);
}

igt_main
{
	igt_fixture() {
		/*
		 * We're operating on the device files themselves
		 * before opening them, make sure the drivers are
		 * loaded.
		 */
		drm_load_module(DRIVER_ANY);
	}

	igt_describe("Ensure that root can Set/DropMaster");
	igt_subtest("master-drop-set-root") {
		check_drop_set();
	}


	igt_subtest_group() {
		uint8_t saved_perm;
		char buf[255];

		/* Upon dropping root we end up as random user, which
		 * a) is not in the video group, and
		 * b) lacks ACL (set via logind or otherwise), thus
		 * any open() fill fail.
		 *
		 * As such, save the state of original other rw permissions
		 * and toggle them on.
		 */

		/* Note: we use a fixture to ensure the permissions are
		 * restored on skip or failure.
		 */
		igt_fixture() {
			char path[255];
			int len;
			int fd;

			memset(buf, 0, sizeof(buf));
			saved_perm = 0;

			fd = __drm_open_driver(DRIVER_ANY);
			igt_assert_fd(fd);

			snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);

			len = readlink(path, buf, sizeof(buf) - 1);
			igt_assert_f(len > 0, "readlink failed with %d path=%s\n", errno, path);

			buf[len] = '\0';
			if (!(strstr(buf, "/dev/dri/card") == buf ||
					strstr(buf, "/dev/dri/renderD") == buf))
				igt_assert_f(0, "Not a card nor render, path=%s\n", buf);

			igt_assert_eq(__drm_close_driver(fd), 0);

			tweak_perm(&saved_perm, buf, true);
		}

		igt_describe("Ensure first normal user can Set/DropMaster");
		igt_subtest("master-drop-set-user") {
			igt_fork(child, 1) {
				igt_drop_root();
				check_drop_set();
			}
			igt_waitchildren();
		}

		/* Restore the original permissions */
		igt_fixture() {
			tweak_perm(&saved_perm, buf, false);
		}
	}

	igt_describe("Check the Set/DropMaster behaviour on shared fd");
	igt_subtest("master-drop-set-shared-fd") {
		int master;

		master = __drm_open_driver(DRIVER_ANY);

		igt_require(master >= 0);

		igt_assert_eq(is_master(master), true);
		igt_fork(child, 1) {
			igt_drop_root();

			/* Dropping root privileges should not alter the
			 * master capability of the fd */
			igt_assert_eq(is_master(master), true);

			/* Even though we've got the master capable fd, we're
			 * a different process (kernel struct pid *) than the
			 * one which opened the device node.
			 *
			 * This ensures that existing workcases of separate
			 * (privileged) arbitrator still work. For example:
			 * - logind + X/Wayland compositor
			 * - weston-launch + weston
			 */
			igt_assert_eq(drmDropMaster(master), -1);
			igt_assert_eq(errno, EACCES);
			igt_assert_eq(drmSetMaster(master), -1);
			igt_assert_eq(errno, EACCES);

			drm_close_driver(master);
		}
		igt_waitchildren();

		drm_close_driver(master);
	}
}
