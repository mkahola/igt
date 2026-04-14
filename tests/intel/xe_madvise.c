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
 * trigger_memory_pressure - Force kernel to reclaim DONTNEED BOs
 * @fd: DRM file descriptor
 *
 * dGPU: over-fill VRAM by ~50 % so TTM evicts purgeable BOs.
 * iGPU: poke the shrinker via igt_purge_vm_caches() (avoids OOM).
 */
static void trigger_memory_pressure(int fd)
{
	uint64_t mem_size, overpressure;
	const uint64_t chunk = 8ull << 20; /* 8 MiB */
	int max_objs, n = 0;
	uint32_t *handles;
	uint64_t total;
	void *p;
	uint32_t handle, vm;

	/* iGPU: use the shrinker, no need to flood system RAM */
	if (!xe_has_vram(fd)) {
		igt_purge_vm_caches(fd);
		return;
	}

	/* dGPU: fill VRAM + 50 % to force TTM eviction */
	mem_size = xe_visible_vram_size(fd, 0);
	overpressure = mem_size / 2;
	if (overpressure < (64 << 20))
		overpressure = 64 << 20;

	/* Separate VM so pressure BOs don't interfere with the test */
	vm = xe_vm_create(fd, 0, 0);

	max_objs = (mem_size + overpressure) / chunk + 1;
	handles = malloc(max_objs * sizeof(*handles));
	igt_assert(handles);

	total = 0;
	while (total < mem_size + overpressure && n < max_objs) {
		uint32_t err;

		err = __xe_bo_create(fd, vm, chunk,
				     vram_if_possible(fd, 0),
				     DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM,
				     NULL, &handle);
		if (err) /* Out of VRAM — sufficient pressure achieved */
			break;

		handles[n++] = handle;
		total += chunk;

		p = xe_bo_map(fd, handle, chunk);
		igt_assert(p != MAP_FAILED);

		/* Fault in all pages so they actually consume VRAM */
		memset(p, 0xCD, chunk);
		munmap(p, chunk);
	}

	/* Allow shrinker time to process pressure */
	usleep(100000);

	for (int i = 0; i < n; i++)
		gem_close(fd, handles[i]);

	free(handles);

	xe_vm_destroy(fd, vm);
}

/**
 * purgeable_mark_and_verify_purged - Mark DONTNEED, pressure, check purged
 * @fd: DRM file descriptor
 * @vm: VM handle
 * @addr: Virtual address of the BO
 * @size: Size of the BO
 *
 * Returns true if the BO was purged under memory pressure.
 */
static bool purgeable_mark_and_verify_purged(int fd, uint32_t vm, uint64_t addr, size_t size)
{
	uint32_t retained;

	/* Mark as DONTNEED */
	retained = xe_vm_madvise_purgeable(fd, vm, addr, size,
					   DRM_XE_VMA_PURGEABLE_STATE_DONTNEED);
	if (retained == 0)
		return true; /* Already purged */

	/* Trigger memory pressure */
	trigger_memory_pressure(fd);

	/* Verify purged */
	retained = xe_vm_madvise_purgeable(fd, vm, addr, size,
					   DRM_XE_VMA_PURGEABLE_STATE_WILLNEED);
	return retained == 0;
}

static jmp_buf jmp;

__noreturn static void sigtrap(int sig)
{
	siglongjmp(jmp, sig);
}

/**
 * SUBTEST: purged-mmap-blocked
 * Description: After BO is purged, verify mmap() fails with -EINVAL
 * Test category: functionality test
 */
static void test_purged_mmap_blocked(int fd)
{
	uint32_t bo, vm;
	uint64_t addr = PURGEABLE_ADDR;
	size_t bo_size = PURGEABLE_BO_SIZE;
	struct drm_xe_gem_mmap_offset mmo = {};
	void *ptr;

	purgeable_setup_simple_bo(fd, &vm, &bo, addr, bo_size, false);
	if (!purgeable_mark_and_verify_purged(fd, vm, addr, bo_size)) {
		gem_close(fd, bo);
		xe_vm_destroy(fd, vm);
		igt_skip("Unable to induce purge on this platform/config");
	}

	/*
	 * Getting the mmap offset is always allowed regardless of purgeable
	 * state - the blocking happens at mmap() time (xe_gem_object_mmap).
	 * For a purged BO, mmap() must fail with -EINVAL (no backing store).
	 */
	mmo.handle = bo;
	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_GEM_MMAP_OFFSET, &mmo), 0);

	ptr = mmap(NULL, bo_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mmo.offset);
	igt_assert_eq_u64((uint64_t)ptr, (uint64_t)MAP_FAILED);
	igt_assert_eq(errno, EINVAL);

	gem_close(fd, bo);
	xe_vm_destroy(fd, vm);
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

/**
 * SUBTEST: dontneed-after-mmap
 * Description: Mark BO as DONTNEED after mmap, verify SIGBUS on accessing purged mapping
 * Test category: functionality test
 */
static void test_dontneed_after_mmap(int fd)
{
	uint32_t bo, vm;
	uint64_t addr = PURGEABLE_ADDR;
	size_t bo_size = PURGEABLE_BO_SIZE;
	void *map;

	purgeable_setup_simple_bo(fd, &vm, &bo, addr, bo_size, true);

	map = xe_bo_map(fd, bo, bo_size);
	igt_assert(map != MAP_FAILED);
	memset(map, 0xAB, bo_size);

	if (!purgeable_mark_and_verify_purged(fd, vm, addr, bo_size)) {
		munmap(map, bo_size);
		gem_close(fd, bo);
		xe_vm_destroy(fd, vm);
		igt_skip("Unable to induce purge on this platform/config");
	}

	/* Access purged mapping - should trigger SIGBUS/SIGSEGV */
	{
		sighandler_t old_sigsegv, old_sigbus;
		char *ptr = (char *)map;
		int sig;

		old_sigsegv = signal(SIGSEGV, (__sighandler_t)sigtrap);
		old_sigbus = signal(SIGBUS, (__sighandler_t)sigtrap);

		sig = sigsetjmp(jmp, 1); /* savemask=1: save/restore signal mask */
		switch (sig) {
		case SIGBUS:
		case SIGSEGV:
			/* Expected - purged mapping access failed */
			break;
		case 0:
			*ptr = 0;
		default:
			igt_assert_f(false,
				     "Access to purged mapping should trigger SIGBUS, got sig=%d\n",
				     sig);
			break;
		}

		signal(SIGBUS, old_sigbus);
		signal(SIGSEGV, old_sigsegv);
	}

	munmap(map, bo_size);
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

	igt_subtest("purged-mmap-blocked")
		xe_for_each_engine(fd, hwe) {
			test_purged_mmap_blocked(fd);
			break;
		}

	igt_subtest("dontneed-after-mmap")
		xe_for_each_engine(fd, hwe) {
			test_dontneed_after_mmap(fd);
			break;
		}

	igt_fixture() {
		xe_device_put(fd);
		drm_close_driver(fd);
	}
}
