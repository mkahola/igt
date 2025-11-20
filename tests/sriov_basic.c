// SPDX-License-Identifier: MIT
/*
 * Copyright(c) 2023 Intel Corporation. All rights reserved.
 */

#include "drmtest.h"
#include "igt_core.h"
#include "igt_sriov_device.h"

IGT_TEST_DESCRIPTION("Basic tests for enabling SR-IOV Virtual Functions");

/**
 * TEST: sriov_basic
 * Category: Core
 * Mega feature: SR-IOV
 * Sub-category: VFs enabling
 * Functionality: configure / enable VFs
 * Description: Validate SR-IOV VFs enabling
 */

/**
 * SUBTEST: enable-vfs-autoprobe-off
 * Description:
 *   Verify VFs enabling without probing VF driver
 */
static void enable_vfs_autoprobe_off(int pf_fd, unsigned int num_vfs)
{
	igt_debug("Testing %u VFs\n", num_vfs);

	igt_require(igt_sriov_get_enabled_vfs(pf_fd) == 0);
	igt_sriov_disable_driver_autoprobe(pf_fd);
	igt_sriov_enable_vfs(pf_fd, num_vfs);
	igt_assert_eq(num_vfs, igt_sriov_get_enabled_vfs(pf_fd));
	igt_sriov_disable_vfs(pf_fd);
}

/**
 * SUBTEST: enable-vfs-autoprobe-on
 * Description:
 *   Verify VFs enabling and auto-probing VF driver
 */
static void enable_vfs_autoprobe_on(int pf_fd, unsigned int num_vfs)
{
	bool err = false;

	igt_debug("Testing %u VFs\n", num_vfs);

	igt_require(igt_sriov_get_enabled_vfs(pf_fd) == 0);
	igt_sriov_enable_driver_autoprobe(pf_fd);
	igt_sriov_enable_vfs(pf_fd, num_vfs);
	igt_assert_eq(num_vfs, igt_sriov_get_enabled_vfs(pf_fd));
	for (int vf_num = 1; vf_num <= num_vfs; ++vf_num) {
		if (!igt_sriov_is_vf_drm_driver_probed(pf_fd, vf_num)) {
			igt_debug("VF%u probe failed\n", vf_num);
			err = true;
		}
	}
	igt_sriov_disable_vfs(pf_fd);
	igt_assert(!err);
}

/**
 * SUBTEST: enable-vfs-bind-unbind-each
 * Description:
 *   Verify VFs enabling with binding and unbinding the driver one by one to each of them.
 *   Version includes dynamic subtests that allow specifying the number of enabled VFs as numvfs-N.
 *
 * SUBTEST: enable-vfs-bind-unbind-each-numvfs-all
 * Description:
 *   Verify the enabling of all VFs and the sequential binding and unbinding of the driver to each one.
 */
static void enable_vfs_bind_unbind_each(int pf_fd, unsigned int num_vfs)
{
	igt_debug("Testing %u VFs\n", num_vfs);

	igt_require(igt_sriov_get_enabled_vfs(pf_fd) == 0);

	igt_sriov_disable_driver_autoprobe(pf_fd);
	igt_sriov_enable_vfs(pf_fd, num_vfs);
	igt_sriov_enable_driver_autoprobe(pf_fd);

	for (int i = 1; i <= num_vfs; i++) {
		igt_assert(!igt_sriov_is_vf_drm_driver_probed(pf_fd, i));

		igt_sriov_bind_vf_drm_driver(pf_fd, i);
		igt_assert(igt_sriov_is_vf_drm_driver_probed(pf_fd, i));

		igt_sriov_unbind_vf_drm_driver(pf_fd, i);
		igt_assert(!igt_sriov_is_vf_drm_driver_probed(pf_fd, i));
	}

	igt_sriov_disable_vfs(pf_fd);
}

/**
 * SUBTEST: bind-unbind-vf
 * Description:
 *   Verify binding and unbinding the driver to specific VF
 */
static void bind_unbind_vf(int pf_fd, unsigned int vf_num)
{
	igt_debug("Testing VF%u\n", vf_num);

	igt_require(igt_sriov_get_enabled_vfs(pf_fd) == 0);

	igt_sriov_disable_driver_autoprobe(pf_fd);
	igt_sriov_enable_vfs(pf_fd, vf_num);
	igt_sriov_enable_driver_autoprobe(pf_fd);

	igt_assert(!igt_sriov_is_vf_drm_driver_probed(pf_fd, vf_num));

	igt_sriov_bind_vf_drm_driver(pf_fd, vf_num);
	igt_assert(igt_sriov_is_vf_drm_driver_probed(pf_fd, vf_num));

	igt_sriov_unbind_vf_drm_driver(pf_fd, vf_num);
	igt_assert(!igt_sriov_is_vf_drm_driver_probed(pf_fd, vf_num));

	igt_sriov_disable_vfs(pf_fd);
}

int igt_main()
{
	int pf_fd;
	bool autoprobe;

	igt_fixture() {
		pf_fd = drm_open_driver(DRIVER_ANY);
		igt_require(igt_sriov_is_pf(pf_fd));
		igt_require(igt_sriov_vfs_supported(pf_fd));
		igt_require(igt_sriov_get_enabled_vfs(pf_fd) == 0);
		autoprobe = igt_sriov_is_driver_autoprobe_enabled(pf_fd);
	}

	igt_describe("Verify VFs enabling without probing VF driver");
	igt_subtest_with_dynamic("enable-vfs-autoprobe-off") {
		for_each_sriov_num_vfs(pf_fd, num_vfs) {
			igt_dynamic_f("numvfs-%u", num_vfs) {
				enable_vfs_autoprobe_off(pf_fd, num_vfs);
			}
		}
		for_random_sriov_num_vfs(pf_fd, num_vfs) {
			igt_dynamic_f("numvfs-random") {
				enable_vfs_autoprobe_off(pf_fd, num_vfs);
			}
		}
		for_max_sriov_num_vfs(pf_fd, num_vfs) {
			igt_dynamic_f("numvfs-all") {
				enable_vfs_autoprobe_off(pf_fd, num_vfs);
			}
		}
	}

	igt_describe("Verify VFs enabling and auto-probing VF driver");
	igt_subtest_with_dynamic("enable-vfs-autoprobe-on") {
		for_each_sriov_num_vfs(pf_fd, num_vfs) {
			igt_dynamic_f("numvfs-%u", num_vfs) {
				enable_vfs_autoprobe_on(pf_fd, num_vfs);
			}
		}
		for_random_sriov_num_vfs(pf_fd, num_vfs) {
			igt_dynamic_f("numvfs-random") {
				enable_vfs_autoprobe_on(pf_fd, num_vfs);
			}
		}
		for_max_sriov_num_vfs(pf_fd, num_vfs) {
			igt_dynamic_f("numvfs-all") {
				enable_vfs_autoprobe_on(pf_fd, num_vfs);
			}
		}
	}

	igt_describe("Verify VFs enabling with binding and unbinding the driver one be one to each of them");
	igt_subtest_with_dynamic("enable-vfs-bind-unbind-each") {
		for_each_sriov_num_vfs(pf_fd, num_vfs) {
			igt_dynamic_f("numvfs-%u", num_vfs) {
				enable_vfs_bind_unbind_each(pf_fd, num_vfs);
			}
		}
		for_random_sriov_num_vfs(pf_fd, num_vfs) {
			igt_dynamic_f("numvfs-random") {
				enable_vfs_bind_unbind_each(pf_fd, num_vfs);
			}
		}
	}

	igt_describe("Verify the enabling of all VFs and the sequential binding and unbinding of the driver to each one");
	igt_subtest("enable-vfs-bind-unbind-each-numvfs-all") {
		for_max_sriov_num_vfs(pf_fd, num_vfs) {
			enable_vfs_bind_unbind_each(pf_fd, num_vfs);
		}
	}

	igt_describe("Test binds and unbinds the driver to specific VF");
	igt_subtest_with_dynamic("bind-unbind-vf") {
		for_each_sriov_vf(pf_fd, vf) {
			igt_dynamic_f("vf-%u", vf) {
				bind_unbind_vf(pf_fd, vf);
			}
		}
		for_random_sriov_vf(pf_fd, vf) {
			igt_dynamic_f("vf-random") {
				bind_unbind_vf(pf_fd, vf);
			}
		}
		for_last_sriov_vf(pf_fd, vf) {
			igt_dynamic_f("vf-last") {
				bind_unbind_vf(pf_fd, vf);
			}
		}
	}

	igt_fixture() {
		igt_sriov_disable_vfs(pf_fd);
		/* abort to avoid execution of next tests with enabled VFs */
		igt_abort_on_f(igt_sriov_get_enabled_vfs(pf_fd) > 0, "Failed to disable VF(s)");
		autoprobe ? igt_sriov_enable_driver_autoprobe(pf_fd) :
			    igt_sriov_disable_driver_autoprobe(pf_fd);
		igt_abort_on_f(autoprobe != igt_sriov_is_driver_autoprobe_enabled(pf_fd),
			       "Failed to restore sriov_drivers_autoprobe value\n");
		close(pf_fd);
	}
}
