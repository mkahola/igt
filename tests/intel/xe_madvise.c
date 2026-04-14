// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */

/**
 * TEST: Validate purgeable BO madvise functionality
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: Memory management tests
 * Functionality: madvise, purgeable
 */

#include "igt.h"
#include "xe_drm.h"

#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"

/* Purgeable test constants */
#define PURGEABLE_ADDR		0x1a0000
#define PURGEABLE_BO_SIZE	4096

static bool xe_has_purgeable_support(int fd)
{
	struct drm_xe_query_config *config = xe_config(fd);

	return config->info[DRM_XE_QUERY_CONFIG_FLAGS] &
		DRM_XE_QUERY_CONFIG_FLAG_HAS_PURGING_SUPPORT;
}

/**
 * purgeable_setup_simple_bo - Setup VM and bind a single BO
 * @fd: DRM file descriptor
 * @vm: Output VM handle
 * @bo: Output BO handle
 * @addr: Virtual address to bind at
 * @size: Size of the BO
 * @use_scratch: Whether to use scratch page flag
 *
 * Helper to create VM, BO, and bind it at the specified address.
 */
static void purgeable_setup_simple_bo(int fd, uint32_t *vm, uint32_t *bo,
				      uint64_t addr, size_t size, bool use_scratch)
{
	struct drm_xe_sync sync = {
		.type = DRM_XE_SYNC_TYPE_USER_FENCE,
		.flags = DRM_XE_SYNC_FLAG_SIGNAL,
		.timeline_value = 1,
	};
	uint64_t sync_val = 0;

	*vm = xe_vm_create(fd, use_scratch ? DRM_XE_VM_CREATE_FLAG_SCRATCH_PAGE : 0, 0);
	*bo = xe_bo_create(fd, *vm, size, vram_if_possible(fd, 0),
			   DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);

	sync.addr = to_user_pointer(&sync_val);
	xe_vm_bind_async(fd, *vm, 0, *bo, 0, addr, size, &sync, 1);
	xe_wait_ufence(fd, &sync_val, 1, 0, NSEC_PER_SEC);
}

/**
 * SUBTEST: dontneed-before-mmap
 * Description: Mark BO as DONTNEED before mmap, verify mmap() fails with -EBUSY
 * Test category: functionality test
 */
static void test_dontneed_before_mmap(int fd)
{
	uint32_t bo, vm;
	uint64_t addr = PURGEABLE_ADDR;
	size_t bo_size = PURGEABLE_BO_SIZE;
	struct drm_xe_gem_mmap_offset mmo = {};
	uint32_t retained;
	void *ptr;

	purgeable_setup_simple_bo(fd, &vm, &bo, addr, bo_size, false);

	/* Mark BO as DONTNEED - new mmap operations must be blocked */
	retained = xe_vm_madvise_purgeable(fd, vm, addr, bo_size,
					   DRM_XE_VMA_PURGEABLE_STATE_DONTNEED);
	igt_assert_eq(retained, 1);

	/* Ioctl succeeds even for DONTNEED BO; blocking happens at mmap() time. */
	mmo.handle = bo;
	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_GEM_MMAP_OFFSET, &mmo), 0);

	/* mmap() on a DONTNEED BO must fail with EBUSY. */
	ptr = mmap(NULL, bo_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mmo.offset);
	igt_assert_eq_u64((uint64_t)ptr, (uint64_t)MAP_FAILED);
	igt_assert_eq(errno, EBUSY);

	/* Restore to WILLNEED before cleanup */
	xe_vm_madvise_purgeable(fd, vm, addr, bo_size,
				DRM_XE_VMA_PURGEABLE_STATE_WILLNEED);

	gem_close(fd, bo);
	xe_vm_destroy(fd, vm);
}

int igt_main()
{
	struct drm_xe_engine_class_instance *hwe;
	int fd;

	igt_fixture() {
		fd = drm_open_driver(DRIVER_XE);
		xe_device_get(fd);
		igt_require_f(xe_has_purgeable_support(fd),
			      "Kernel does not support purgeable buffer objects\n");
	}

	igt_subtest("dontneed-before-mmap")
		xe_for_each_engine(fd, hwe) {
			test_dontneed_before_mmap(fd);
			break;
		}

	igt_fixture() {
		xe_device_put(fd);
		drm_close_driver(fd);
	}
}
