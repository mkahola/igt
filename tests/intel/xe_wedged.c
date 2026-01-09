// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 Intel Corporation
 */

/**
 * TEST: cause fake gt reset failure which put Xe device in wedged state
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: driver
 * Functionality: wedged
 * Test category: functionality test
 */

#include <limits.h>
#include <dirent.h>

#include "igt.h"
#include "igt_device.h"
#include "igt_kmod.h"
#include "igt_sriov_device.h"
#include "igt_syncobj.h"
#include "igt_sysfs.h"

#include "xe_drm.h"
#include "xe/xe_gt.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"
#include "xe/xe_spin.h"

static void ignore_wedged_in_dmesg(void)
{
	/* this is needed for igt_runner so it will ignore it */
	igt_emit_ignore_dmesg_regex("CRITICAL: Xe has declared device [0-9A-Fa-f:.]* as wedged"
				    "|GT[0-9A-Fa-f]*: reset failed .-ECANCELED"
				    "|GT[0-9A-Fa-f]*: Failed to submit"
				    "|Modules linked in:"
				    "|__pfx___drm_");
}

static void force_wedged(int fd)
{
	igt_debugfs_write(fd, "fail_gt_reset/probability", "100");
	igt_debugfs_write(fd, "fail_gt_reset/times", "2");

	xe_force_gt_reset_async(fd, 0);
	sleep(1);
}

static int simple_ioctl(int fd)
{
	int ret;

	struct drm_xe_vm_create create = {
		.extensions = 0,
		.flags = 0,
	};

	ret = igt_ioctl(fd, DRM_IOCTL_XE_VM_CREATE, &create);

	if (ret == 0)
		xe_vm_destroy(fd, create.vm_id);

	return ret;
}

static void
simple_exec(int fd, struct drm_xe_engine_class_instance *eci)
{
	uint32_t vm;
	uint64_t addr = 0x1a0000;
	struct drm_xe_sync sync[2] = {
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, },
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, },
	};
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 2,
		.syncs = to_user_pointer(sync),
	};
	uint64_t batch_offset, batch_addr, sdi_offset, sdi_addr;
	uint32_t exec_queue;
	uint32_t syncobjs;
	size_t bo_size;
	uint32_t bo = 0;
	struct {
		uint32_t batch[16];
		uint64_t pad;
		uint32_t data;
	} *data;
	int b;

	vm = xe_vm_create(fd, 0, 0);

	bo_size = sizeof(*data) * 2;
	bo_size = xe_bb_size(fd, bo_size);
	bo = xe_bo_create(fd, vm, bo_size,
			  vram_if_possible(fd, eci->gt_id),
			  DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	data = xe_bo_map(fd, bo, bo_size);

	exec_queue = xe_exec_queue_create(fd, vm, eci, 0);

	syncobjs = syncobj_create(fd, 0);
	sync[0].handle = syncobj_create(fd, 0);

	xe_vm_bind_async(fd, vm, 0, bo, 0, addr,
			 bo_size, sync, 1);

	batch_offset = (char *)&data[0].batch - (char *)data;
	batch_addr = addr + batch_offset;
	sdi_offset = (char *)&data[0].data - (char *)data;
	sdi_addr = addr + sdi_offset;

	b = 0;
	data[0].batch[b++] = MI_STORE_DWORD_IMM_GEN4;
	data[0].batch[b++] = sdi_addr;
	data[0].batch[b++] = sdi_addr >> 32;
	data[0].batch[b++] = 0xc0ffee;
	data[0].batch[b++] = MI_BATCH_BUFFER_END;
	igt_assert(b <= ARRAY_SIZE(data[0].batch));

	sync[0].flags &= ~DRM_XE_SYNC_FLAG_SIGNAL;
	sync[1].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
	sync[1].handle = syncobjs;

	exec.exec_queue_id = exec_queue;
	exec.address = batch_addr;

	syncobj_reset(fd, &syncobjs, 1);

	xe_exec(fd, &exec);

	igt_assert(syncobj_wait(fd, &syncobjs, 1, INT64_MAX, 0, NULL));
	igt_assert_eq(data[0].data, 0xc0ffee);
	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));
	sync[0].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
	xe_vm_unbind_async(fd, vm, 0, 0, addr, bo_size, sync, 1);
	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));
	igt_assert_eq(data[0].data, 0xc0ffee);

	syncobj_destroy(fd, sync[0].handle);
	syncobj_destroy(fd, syncobjs);
	xe_exec_queue_destroy(fd, exec_queue);
	munmap(data, bo_size);
	gem_close(fd, bo);
	xe_vm_destroy(fd, vm);
}

static void
simple_hang(int fd, struct drm_xe_sync *sync)
{
	struct drm_xe_engine_class_instance *eci = &xe_engine(fd, 0)->instance;
	uint32_t vm;
	uint64_t addr = 0x1a0000;
	struct drm_xe_exec exec_hang = {
		.num_batch_buffer = 1,
	};
	uint64_t spin_offset;
	uint32_t hang_exec_queue;
	size_t bo_size;
	uint32_t bo = 0;
	struct {
		struct xe_spin spin;
		uint32_t batch[16];
		uint64_t pad;
		uint32_t data;
	} *data;
	struct xe_spin_opts spin_opts = { .preempt = false };
	int err;

	if (sync) {
		exec_hang.syncs = to_user_pointer(sync);
		exec_hang.num_syncs = 1;
	}

	vm = xe_vm_create(fd, 0, 0);
	bo_size = xe_bb_size(fd, sizeof(*data));
	bo = xe_bo_create(fd, vm, bo_size,
			  vram_if_possible(fd, eci->gt_id),
			  DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	data = xe_bo_map(fd, bo, bo_size);
	hang_exec_queue = xe_exec_queue_create(fd, vm, eci, 0);

	spin_offset = (char *)&data[0].spin - (char *)data;
	spin_opts.addr = addr + spin_offset;
	xe_spin_init(&data[0].spin, &spin_opts);
	exec_hang.exec_queue_id = hang_exec_queue;
	exec_hang.address = spin_opts.addr;

	do {
		err =  igt_ioctl(fd, DRM_IOCTL_XE_EXEC, &exec_hang);
	} while (err && errno == ENOMEM);
}

/**
 * SUBTEST: basic-wedged
 * Description: Force Xe device wedged after injecting a failure in GT reset
 */
/**
 * SUBTEST: wedged-at-any-timeout
 * Description: Force Xe device wedged after a simple guc timeout
 */
/**
 * SUBTEST: wedged-mode-toggle
 * Description: Test wedged.mode=1 after testing wedged.mode=2
 */
/**
 * SUBTEST: basic-wedged-read
 * Description: Read wedged_mode debugfs
 */
int igt_main()
{
	struct drm_xe_engine_class_instance *hwe;
	int fd;
	char pci_slot[NAME_MAX];

	igt_fixture() {
		fd = drm_open_driver(DRIVER_XE);
		igt_device_get_pci_slot_name(fd, pci_slot);
	}

	igt_subtest("basic-wedged") {
		igt_require(igt_debugfs_exists(fd, "fail_gt_reset/probability",
					       O_RDWR));
		igt_debugfs_write(fd, "fail_gt_reset/verbose", "1");

		igt_assert_eq(simple_ioctl(fd), 0);
		ignore_wedged_in_dmesg();

		force_wedged(fd);
		igt_assert_neq(simple_ioctl(fd), 0);

		drm_close_driver(fd);
		igt_kmod_rebind("xe", pci_slot);
		fd = drm_open_driver(DRIVER_XE);

		igt_assert_eq(simple_ioctl(fd), 0);
		xe_for_each_engine(fd, hwe)
			simple_exec(fd, hwe);
	}

	igt_subtest_f("wedged-at-any-timeout") {
		struct drm_xe_sync hang_sync = {
			.type = DRM_XE_SYNC_TYPE_SYNCOBJ,
			.flags = DRM_XE_SYNC_FLAG_SIGNAL,
		};
		int err;

		igt_require(igt_debugfs_exists(fd, "wedged_mode", O_RDWR));
		igt_require(!intel_is_vf_device(fd));

		igt_debugfs_write(fd, "wedged_mode", "2");
		igt_skip_on_f(igt_sriov_is_pf(fd) && errno == EPERM,
			      "On PF, upon-any-hang-no-reset (2) wedged mode is restricted to debug builds only\n");

		ignore_wedged_in_dmesg();

		hang_sync.handle = syncobj_create(fd, 0);

		simple_hang(fd, &hang_sync);

		/*
		 * Wait for the hang to be detected.  If the hang has already
		 * taken place, this will return ECANCELED and we can just move
		 * on immediately.
		 */
		err = syncobj_wait_err(fd, &hang_sync.handle, 1, INT64_MAX, 0);
		if (err)
			igt_assert_eq(err, -ECANCELED);

		/* Other ioctls should also be returning ECANCELED now */
		igt_assert_neq(simple_ioctl(fd), 0);
		igt_assert_eq(errno, ECANCELED);

		/*
		 * Rebind the device and ensure proper operation is restored
		 * for all engines.
		 */
		drm_close_driver(fd);
		igt_kmod_rebind("xe", pci_slot);
		fd = drm_open_driver(DRIVER_XE);

		igt_assert_eq(simple_ioctl(fd), 0);
		xe_for_each_engine(fd, hwe)
			simple_exec(fd, hwe);
	}

	igt_subtest_f("wedged-mode-toggle") {
		igt_require(igt_debugfs_exists(fd, "wedged_mode", O_RDWR));
		igt_require(!intel_is_vf_device(fd));

		igt_debugfs_write(fd, "wedged_mode", "2");
		igt_skip_on_f(igt_sriov_is_pf(fd) && errno == EPERM,
			      "On PF, upon-any-hang-no-reset (2) wedged mode is restricted to debug builds only\n");

		igt_assert_eq(simple_ioctl(fd), 0);
		igt_debugfs_write(fd, "wedged_mode", "1");
		ignore_wedged_in_dmesg();
		simple_hang(fd, NULL);
		igt_assert_eq(simple_ioctl(fd), 0);
	}

	igt_subtest_f("basic-wedged-read") {
		char str[150] = {0};

		igt_require(igt_debugfs_exists(fd, "wedged_mode", O_RDONLY));

		igt_debugfs_read(fd, "wedged_mode", str);
		igt_assert_f(str[0] != '\0', "Failed to read wedged_mode from debugfs!\n");
	}

	igt_fixture() {
		if (igt_debugfs_exists(fd, "fail_gt_reset/probability", O_RDWR)) {
			igt_debugfs_write(fd, "fail_gt_reset/probability", "0");
			igt_debugfs_write(fd, "fail_gt_reset/times", "1");
		}

		/* Tests might have failed, force a rebind before exiting */
		drm_close_driver(fd);
		igt_kmod_rebind("xe", pci_slot);
	}
}
