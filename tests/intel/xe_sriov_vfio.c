// SPDX-License-Identifier: MIT
/*
 * Copyright(c) 2026 Intel Corporation. All rights reserved.
 */

#include "igt_core.h"
#include "igt_kmod.h"

/**
 * TEST: xe_sriov_vfio
 * Category: Core
 * Mega feature: SR-IOV
 * Sub-category: SR-IOV VFIO
 * Functionality: VFIO module
 * Description: Verify basic xe-vfio-pci module operations.
 *
 * SUBTEST: load-unload-xe-vfio-pci
 * Description: Attempt to load xe_vfio_pci module and then unload it.
 */

IGT_TEST_DESCRIPTION("Xe SR-IOV VFIO tests (xe-vfio-pci)");

static const char *XE_VFIO_PCI_MOD = "xe_vfio_pci";

static bool xe_vfio_loaded_initially;

static void restore_xe_vfio_module(void)
{
	bool loaded;
	int ret;

	loaded = igt_kmod_is_loaded(XE_VFIO_PCI_MOD);

	if (loaded != xe_vfio_loaded_initially) {
		ret = xe_vfio_loaded_initially ?
			igt_kmod_load(XE_VFIO_PCI_MOD, NULL) :
			igt_kmod_unload(XE_VFIO_PCI_MOD);
		igt_abort_on_f(ret,
			       "Failed to %s %s during cleanup\n",
			       xe_vfio_loaded_initially ? "load" : "unload",
			       XE_VFIO_PCI_MOD);
		loaded = igt_kmod_is_loaded(XE_VFIO_PCI_MOD);
	}

	igt_abort_on_f(loaded != xe_vfio_loaded_initially,
		       "%s should be %s after cleanup\n",
		       XE_VFIO_PCI_MOD,
		       xe_vfio_loaded_initially ? "loaded" : "unloaded");
}

int igt_main()
{
	igt_fixture() {
		xe_vfio_loaded_initially = igt_kmod_is_loaded(XE_VFIO_PCI_MOD);
	}

	igt_describe("Attempt to load xe_vfio_pci module and then unload it.");
	igt_subtest("load-unload-xe-vfio-pci") {
		int ret;

		igt_skip_on(xe_vfio_loaded_initially);

		ret = igt_kmod_load(XE_VFIO_PCI_MOD, NULL);
		igt_assert_f(ret == 0, "Failed to load %s (%d)\n", XE_VFIO_PCI_MOD, ret);
		igt_assert(igt_kmod_is_loaded(XE_VFIO_PCI_MOD));

		ret = igt_kmod_unload(XE_VFIO_PCI_MOD);
		igt_assert_f(ret == 0, "Failed to unload %s (%d)\n", XE_VFIO_PCI_MOD, ret);
		igt_assert(igt_kmod_is_loaded(XE_VFIO_PCI_MOD) == false);
	}

	igt_fixture() {
		restore_xe_vfio_module();
	}
}
