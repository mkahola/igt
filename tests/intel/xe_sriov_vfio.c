// SPDX-License-Identifier: MIT
/*
 * Copyright(c) 2026 Intel Corporation. All rights reserved.
 */
#include <linux/vfio.h>
#include <limits.h>
#include <sys/ioctl.h>

#include "drmtest.h"
#include "igt_core.h"
#include "igt_kmod.h"
#include "igt_pci.h"
#include "igt_sriov_device.h"

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
 *
 * SUBTEST: bind-unbind-vfs
 * Description: Enable VFs and bind/unbind each one to xe-vfio-pci via driver_override.
 *
 * SUBTEST: open-basic
 * Description: Bind VF to xe-vfio-pci and perform minimal VFIO group open and status ioctl.
 */

IGT_TEST_DESCRIPTION("Xe SR-IOV VFIO tests (xe-vfio-pci)");

#define DRIVER_OVERRIDE_TIMEOUT_MS 200

static const char *XE_VFIO_PCI_MOD = "xe_vfio_pci";
static const char *XE_VFIO_PCI_DRV = "xe-vfio-pci";

static int pf_fd = -1;
static bool autoprobe;
static bool xe_vfio_loaded_initially;

static void assert_xe_vfio_pci_is_bound(const char *pci_slot)
{
	char bound[64];
	int ret;

	ret = igt_pci_get_bound_driver_name(pci_slot, bound, sizeof(bound));
	igt_assert_f(ret >= 0, "Failed to read bound driver for %s (%d)\n", pci_slot, ret);
	igt_assert_f(ret > 0, "Expected %s to be bound to %s, but it is unbound\n",
		     pci_slot, XE_VFIO_PCI_DRV);
	igt_assert_f(!strcmp(bound, XE_VFIO_PCI_DRV),
		     "Expected %s to be bound to %s, got %s\n",
		     pci_slot, XE_VFIO_PCI_DRV, bound);
}

static void assert_driver_unbound(const char *pci_slot)
{
	char bound[64];
	int ret;

	ret = igt_pci_get_bound_driver_name(pci_slot, bound, sizeof(bound));
	igt_assert_f(ret >= 0, "Failed to read bound driver for %s (%d)\n", pci_slot, ret);
	igt_assert_f(ret == 0, "Expected %s to be unbound, but is bound to %s\n",
		     pci_slot, bound);
}

static char *vf_pci_slot_alloc(unsigned int vf_id)
{
	char *slot = igt_sriov_get_vf_pci_slot_alloc(pf_fd, vf_id);

	igt_assert_f(slot, "Failed to get VF%u PCI slot\n", vf_id);
	return slot;
}

static void vf_bind_override(unsigned int vf_id, const char *driver)
{
	char *slot = vf_pci_slot_alloc(vf_id);
	int ret;

	ret = igt_pci_bind_driver_override(slot, driver, DRIVER_OVERRIDE_TIMEOUT_MS);
	igt_assert_f(ret == 0, "bind %s (VF%u) to %s failed (%d)\n", slot, vf_id, driver, ret);
	assert_xe_vfio_pci_is_bound(slot);

	free(slot);
}

static void vf_unbind_override(unsigned int vf_id)
{
	char *slot = vf_pci_slot_alloc(vf_id);
	int ret;

	ret = igt_pci_unbind_driver_override(slot, DRIVER_OVERRIDE_TIMEOUT_MS);
	igt_assert_f(ret == 0, "unbind %s (VF%u) failed (%d)\n", slot, vf_id, ret);
	assert_driver_unbound(slot);

	free(slot);
}

static void bind_unbind_vfs(unsigned int num_vfs)
{
	igt_sriov_disable_driver_autoprobe(pf_fd);
	igt_sriov_enable_vfs(pf_fd, num_vfs);

	igt_require_f(!igt_pci_system_reinit(), "Failed to refresh PCI state\n");

	igt_sriov_enable_driver_autoprobe(pf_fd);

	for (unsigned int vf_id = 1; vf_id <= num_vfs; vf_id++) {
		vf_bind_override(vf_id, XE_VFIO_PCI_DRV);
		vf_unbind_override(vf_id);
	}

	igt_sriov_disable_vfs(pf_fd);
}

static bool vf_iommu_group_id_alloc(const char *pci_slot, char **out_group_id)
{
	char path[PATH_MAX];
	char link[PATH_MAX];
	ssize_t len;
	const char *base;

	igt_assert(out_group_id);
	*out_group_id = NULL;

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/iommu_group", pci_slot);
	len = readlink(path, link, sizeof(link) - 1);
	if (len < 0)
		return false;

	link[len] = '\0';
	base = strrchr(link, '/');
	if (!base || !base[1])
		return false;

	*out_group_id = strdup(base + 1);
	igt_assert(*out_group_id);

	return true;
}

struct vfio_dev_fds {
	int container_fd;
	int group_fd;
	int device_fd;
	char *group_id;
};

static void vfio_open_group(const char *pci_slot, struct vfio_dev_fds *out,
			    struct vfio_group_status *out_group_status)
{
	char group_path[PATH_MAX];
	int ret;

	igt_assert(out);
	*out = (struct vfio_dev_fds){
		.container_fd = -1,
		.group_fd = -1,
		.device_fd = -1,
		.group_id = NULL,
	};

	if (out_group_status)
		*out_group_status = (struct vfio_group_status){
					.argsz = sizeof(*out_group_status)
				    };

	if (!vf_iommu_group_id_alloc(pci_slot, &out->group_id))
		igt_skip("No IOMMU group (IOMMU disabled or not exposed)\n");

	out->container_fd = open("/dev/vfio/vfio", O_RDWR | O_CLOEXEC);
	if (out->container_fd < 0)
		igt_skip("/dev/vfio/vfio not available\n");

	snprintf(group_path, sizeof(group_path), "/dev/vfio/%s", out->group_id);
	out->group_fd = open(group_path, O_RDWR | O_CLOEXEC);
	igt_require_f(out->group_fd >= 0, "Failed to open %s (%d)\n", group_path, -errno);

	if (out_group_status) {
		ret = ioctl(out->group_fd, VFIO_GROUP_GET_STATUS, out_group_status);
		igt_require_f(ret == 0, "VFIO_GROUP_GET_STATUS failed (%d)\n", -errno);
	}
}

static void vfio_close_device(struct vfio_dev_fds *fds)
{
	if (!fds)
		return;

	if (fds->device_fd >= 0)
		close(fds->device_fd);

	if (fds->group_fd >= 0 && fds->container_fd >= 0)
		ioctl(fds->group_fd, VFIO_GROUP_UNSET_CONTAINER, &fds->container_fd);

	if (fds->group_fd >= 0)
		close(fds->group_fd);
	if (fds->container_fd >= 0)
		close(fds->container_fd);

	free(fds->group_id);

	fds->container_fd = -1;
	fds->group_fd = -1;
	fds->device_fd = -1;
	fds->group_id = NULL;
}

static void vfio_open_basic(const char *pci_slot)
{
	struct vfio_dev_fds fds;
	struct vfio_group_status group_status;

	vfio_open_group(pci_slot, &fds, &group_status);
	igt_info("VFIO group %s status flags=0x%x\n", fds.group_id, group_status.flags);
	vfio_close_device(&fds);
}

static void open_pf(void)
{
	int fd;

	if (pf_fd >= 0)
		return;

	fd = drm_open_driver(DRIVER_XE);
	igt_assert_fd(fd);

	if (!igt_sriov_is_pf(fd)) {
		drm_close_driver(fd);
		igt_skip("Xe device is not an SR-IOV PF\n");
	}

	if (igt_sriov_get_enabled_vfs(fd) != 0) {
		drm_close_driver(fd);
		igt_skip("VFs must be disabled before running this test\n");
	}

	autoprobe = igt_sriov_is_driver_autoprobe_enabled(fd);
	pf_fd = fd;
}

static void cleanup_pf(void)
{
	if (pf_fd < 0)
		return;

	igt_sriov_disable_vfs(pf_fd);
	igt_abort_on_f(igt_sriov_get_enabled_vfs(pf_fd) > 0, "Failed to disable VF(s)\n");
	autoprobe ? igt_sriov_enable_driver_autoprobe(pf_fd) :
		   igt_sriov_disable_driver_autoprobe(pf_fd);
	igt_abort_on_f(autoprobe != igt_sriov_is_driver_autoprobe_enabled(pf_fd),
		       "Failed to restore sriov_drivers_autoprobe value\n");
	drm_close_driver(pf_fd);
	pf_fd = -1;
}

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

	igt_describe("Enable VFs and bind/unbind each one to xe-vfio-pci via driver_override.");
	igt_subtest_with_dynamic("bind-unbind-vfs") {
		igt_skip_on_f(igt_kmod_load(XE_VFIO_PCI_MOD, NULL),
			      "Failed to load %s\n", XE_VFIO_PCI_MOD);

		open_pf();

		for_each_sriov_num_vfs(pf_fd, num_vfs) {
			igt_dynamic_f("numvfs-%u", num_vfs) {
				bind_unbind_vfs(num_vfs);
			}
		}
	}

	igt_describe("Bind VF to xe-vfio-pci and do minimal VFIO open and GET_STATUS.");
	igt_subtest("open-basic") {
		unsigned int vf_num = 1;
		char *slot = NULL;

		igt_skip_on_f(igt_kmod_load(XE_VFIO_PCI_MOD, NULL),
			      "Failed to load %s\n", XE_VFIO_PCI_MOD);

		open_pf();

		igt_sriov_disable_driver_autoprobe(pf_fd);
		igt_sriov_enable_vfs(pf_fd, vf_num);

		igt_require_f(!igt_pci_system_reinit(), "Failed to refresh PCI state\n");
		igt_sriov_enable_driver_autoprobe(pf_fd);

		slot = vf_pci_slot_alloc(vf_num);

		vf_bind_override(vf_num, XE_VFIO_PCI_DRV);

		vfio_open_basic(slot);

		vf_unbind_override(vf_num);
		free(slot);
		igt_sriov_disable_vfs(pf_fd);
	}

	igt_fixture() {
		cleanup_pf();
		restore_xe_vfio_module();
	}
}
