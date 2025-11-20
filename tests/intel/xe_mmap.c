// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

/**
 * TEST: Test if the driver is capable of doing mmap on different memory regions
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: Memory management tests
 * Functionality: mmap
 */

#include <fcntl.h>

#include "igt.h"

#include "xe_drm.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"

#include <setjmp.h>
#include <signal.h>
#include <string.h>

/**
 * SUBTEST: system
 * Test category: functionality test
 * Description: Test mmap on system memory
 */

/**
 * SUBTEST: small-bar
 * Description: Sanity check mmap behaviour on small-bar systems
 * GPU requirements: GPU needs to have dedicated VRAM and using small-bar
 * Test category: functionality test
 */

/**
 * SUBTEST: %s
 * Description: Test mmap on %arg[1] memory
 * GPU requirements: GPU needs to have dedicated VRAM
 * Test category: functionality test
 *
 * arg[1]:
 *
 * @vram:		vram
 * @vram-system:	system vram
 */
static void
test_mmap(int fd, uint32_t placement, uint32_t flags)
{
	size_t bo_size = xe_get_default_alignment(fd);
	uint32_t bo;
	void *map;

	igt_require_f(placement, "Device doesn't support such memory region\n");

	bo = xe_bo_create(fd, 0, bo_size, placement, flags);

	map = xe_bo_map(fd, bo, bo_size);
	strcpy(map, "Write some data to the BO!");

	munmap(map, bo_size);

	gem_close(fd, bo);
}

#define PAGE_SIZE 4096

/**
 * SUBTEST: pci-membarrier
 * Description: create pci memory barrier with write on defined mmap offset.
 * Test category: functionality test
 *
 */
static void test_pci_membarrier(int xe)
{
	uint64_t flags = MAP_SHARED;
	unsigned int prot = PROT_WRITE;
	uint32_t *ptr;
	uint64_t size = PAGE_SIZE;
	struct timespec tv;
	struct drm_xe_gem_mmap_offset mmo = {
		.handle = 0,
		.flags = DRM_XE_MMAP_OFFSET_FLAG_PCI_BARRIER,
	};

	do_ioctl(xe, DRM_IOCTL_XE_GEM_MMAP_OFFSET, &mmo);
	ptr = mmap(NULL, size, prot, flags, xe, mmo.offset);
	igt_assert(ptr != MAP_FAILED);

	/* Check whole page for any errors, also check as
	 * we should not read written values back
	 */
	for (int i = 0; i < size / sizeof(*ptr); i++) {
		/* It is expected unconfigured doorbell space
		 * will return read value 0xdeadbeef
		 */
		igt_assert_eq_u32(READ_ONCE(ptr[i]), 0xdeadbeef);

		igt_gettime(&tv);
		ptr[i] = i;
		if (READ_ONCE(ptr[i]) == i) {
			while (READ_ONCE(ptr[i]) == i)
				;
			igt_info("fd:%d value retained for %"PRId64"ns pos:%d\n",
					xe, igt_nsec_elapsed(&tv), i);
		}
		igt_assert_neq(READ_ONCE(ptr[i]), i);
	}

	munmap(ptr, size);
}

/**
 * SUBTEST: pci-membarrier-parallel
 * Description: create parallel pci memory barrier with write on defined mmap offset.
 * Test category: functionality test
 *
 */
static void test_pci_membarrier_parallel(int xe, int child, unsigned int i)
{
	uint64_t flags = MAP_SHARED;
	unsigned int prot = PROT_WRITE;
	uint32_t *ptr;
	uint64_t size = PAGE_SIZE;
	struct drm_xe_gem_mmap_offset mmo = {
		.handle = 0,
		.flags = DRM_XE_MMAP_OFFSET_FLAG_PCI_BARRIER,
	};
	int value;

	do_ioctl(xe, DRM_IOCTL_XE_GEM_MMAP_OFFSET, &mmo);
	ptr = mmap(NULL, size, prot, flags, xe, mmo.offset);
	igt_assert(ptr != MAP_FAILED);

	/* It is expected unconfigured doorbell space
	 * will return read value 0xdeadbeef
	 */
	igt_assert_eq_u32(READ_ONCE(ptr[i]), 0xdeadbeef);

	igt_until_timeout(5) {
		/* Check clients should not be able to see each other */
		if (child != -1) {
			value = i + 1;
			igt_assert_neq(READ_ONCE(ptr[i]), i);
		} else {
			value = i;
			igt_assert_neq(READ_ONCE(ptr[i]), i + 1);
		}

		WRITE_ONCE(ptr[i], value);
	}
	igt_assert_eq_u32(READ_ONCE(ptr[i]), 0xdeadbeef);

	munmap(ptr, size);
}

/**
 * SUBTEST: pci-membarrier-bad-pagesize
 * Description: Test mmap offset with bad pagesize for pci membarrier.
 * Test category: negative test
 *
 */
static void test_bad_pagesize_for_pcimem(int fd)
{
	uint32_t *map;
	uint64_t page_size = PAGE_SIZE * 2;
	struct drm_xe_gem_mmap_offset mmo = {
		.handle = 0,
		.flags = DRM_XE_MMAP_OFFSET_FLAG_PCI_BARRIER,
	};

	do_ioctl(fd, DRM_IOCTL_XE_GEM_MMAP_OFFSET, &mmo);
	map = mmap(NULL, page_size, PROT_WRITE, MAP_SHARED, fd, mmo.offset);
	igt_assert(map == MAP_FAILED);
}

/**
 * SUBTEST: bad-flags
 * Description: Test mmap offset with bad flags.
 * Test category: negative test
 *
 */
static void test_bad_flags(int fd)
{
	uint64_t size = xe_get_default_alignment(fd);
	struct drm_xe_gem_mmap_offset mmo = {
		.handle = xe_bo_create(fd, 0, size,
				       vram_if_possible(fd, 0),
				       DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM),
		.flags = -1u,
	};

	do_ioctl_err(fd, DRM_IOCTL_XE_GEM_MMAP_OFFSET, &mmo, EINVAL);
	gem_close(fd, mmo.handle);
}

/**
 * SUBTEST: bad-extensions
 * Description: Test mmap offset with bad extensions.
 * Test category: negative test
 *
 */
static void test_bad_extensions(int fd)
{
	uint64_t size = xe_get_default_alignment(fd);
	struct drm_xe_user_extension ext;
	struct drm_xe_gem_mmap_offset mmo = {
		.handle = xe_bo_create(fd, 0, size,
				       vram_if_possible(fd, 0),
				       DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM),
	};

	mmo.extensions = to_user_pointer(&ext);
	ext.name = -1;

	do_ioctl_err(fd, DRM_IOCTL_XE_GEM_MMAP_OFFSET, &mmo, EINVAL);
	gem_close(fd, mmo.handle);
}

/**
 * SUBTEST: bad-object
 * Description: Test mmap offset with bad object.
 * Test category: negative test
 *
 */
static void test_bad_object(int fd)
{
	uint64_t size = xe_get_default_alignment(fd);
	struct drm_xe_gem_mmap_offset mmo = {
		.handle = xe_bo_create(fd, 0, size,
				       vram_if_possible(fd, 0),
				       DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM),
	};

	mmo.handle = 0xdeadbeef;
	do_ioctl_err(fd, DRM_IOCTL_XE_GEM_MMAP_OFFSET, &mmo, ENOENT);
}

/**
 * SUBTEST: pci-membarrier-bad-object
 * Description: Test mmap offset with bad object for pci mem barrier.
 * Test category: negative test
 *
 */
static void test_bad_object_for_pcimem(int fd)
{
	uint64_t size = xe_get_default_alignment(fd);
	struct drm_xe_gem_mmap_offset mmo = {
		.handle = xe_bo_create(fd, 0, size,
				       vram_if_possible(fd, 0),
				       DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM),
		.flags = DRM_XE_MMAP_OFFSET_FLAG_PCI_BARRIER,
	};

	do_ioctl_err(fd, DRM_IOCTL_XE_GEM_MMAP_OFFSET, &mmo, EINVAL);
}

static jmp_buf jmp;

__noreturn static void sigtrap(int sig)
{
	siglongjmp(jmp, sig);
}

static void trap_sigbus(uint32_t *ptr)
{
	sighandler_t old_sigbus;

	old_sigbus = signal(SIGBUS, sigtrap);
	switch (sigsetjmp(jmp, SIGBUS)) {
	case SIGBUS:
		break;
	case 0:
		*ptr = 0xdeadbeaf;
	default:
		igt_assert(!"reached");
		break;
	}
	signal(SIGBUS, old_sigbus);
}

/**
 * SUBTEST: small-bar
 * Description: Test mmap behaviour on small-bar systems.
 * Test category: functionality test
 *
 */
static void test_small_bar(int fd)
{
	size_t page_size = xe_get_default_alignment(fd);
	uint64_t visible_size = xe_visible_vram_size(fd, 0);
	uint32_t bo;
	uint64_t mmo;
	uint32_t *map;

	/* 2BIG invalid case */
	igt_assert_neq(__xe_bo_create(fd, 0, visible_size + page_size,
				      vram_memory(fd, 0),
				      DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM,
				      NULL,
				      &bo),
		       0);

	/* Normal operation */
	bo = xe_bo_create(fd, 0, visible_size / 4, vram_memory(fd, 0),
			  DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	mmo = xe_bo_mmap_offset(fd, bo);
	map = mmap(NULL, page_size, PROT_WRITE, MAP_SHARED, fd, mmo);
	igt_assert(map != MAP_FAILED);

	map[0] = 0xdeadbeaf;

	munmap(map, page_size);
	gem_close(fd, bo);

	/* Normal operation with system memory spilling */
	bo = xe_bo_create(fd, 0, visible_size,
			  vram_memory(fd, 0) |
			  system_memory(fd),
			  DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	mmo = xe_bo_mmap_offset(fd, bo);
	map = mmap(NULL, page_size, PROT_WRITE, MAP_SHARED, fd, mmo);
	igt_assert(map != MAP_FAILED);

	map[0] = 0xdeadbeaf;

	munmap(map, page_size);
	gem_close(fd, bo);

	/* Bogus operation with SIGBUS */
	bo = xe_bo_create(fd, 0, visible_size + page_size, vram_memory(fd, 0), 0);
	mmo = xe_bo_mmap_offset(fd, bo);
	map = mmap(NULL, page_size, PROT_WRITE, MAP_SHARED, fd, mmo);
	igt_assert(map != MAP_FAILED);

	trap_sigbus(map);
	gem_close(fd, bo);
}

static int
__xe_query(int fd, struct drm_xe_device_query *q)
{
	if (igt_ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, q))
		return -errno;
	return 0;
}

static int
__xe_query_items(int fd, uint32_t *items, uint32_t n_items)
{
	struct drm_xe_device_query q = {
		.size = n_items,
		.data = to_user_pointer(items),
	};
	return __xe_query(fd, &q);
}

static void assert_caching(int fd, uint64_t placement, uint32_t flags,
			   uint16_t cpu_caching, bool fail)
{
	uint64_t size = xe_get_default_alignment(fd);
	uint64_t mmo;
	uint32_t handle;
	uint32_t *map;
	bool ret;

	ret = __xe_bo_create_caching(fd, 0, size, placement, flags, cpu_caching, &handle);
	igt_assert(ret == fail);

	if (fail)
		return;

	mmo = xe_bo_mmap_offset(fd, handle);
	map = mmap(NULL, size, PROT_WRITE, MAP_SHARED, fd, mmo);
	igt_assert(map != MAP_FAILED);
	map[0] = 0xdeadbeaf;
	munmap(map, size);
	gem_close(fd, handle);

	/* Adding a Read-Only page check exercise */
	map = mmap(0, size, PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	igt_assert(map != MAP_FAILED);
	igt_assert_eq(0, mprotect(map, size, PROT_READ));
	igt_assert_lt(__xe_query_items(fd, map, 1), 0);
	munmap(map, size);

}

/**
 * SUBTEST: cpu-caching
 * Description: Test explicit cpu_caching, including mmap behaviour.
 * Test category: functionality test
 */
static void test_cpu_caching(int fd)
{
	if (vram_memory(fd, 0)) {
		assert_caching(fd, vram_memory(fd, 0),
			       DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM,
			       DRM_XE_GEM_CPU_CACHING_WC, false);
		assert_caching(fd, vram_memory(fd, 0) | system_memory(fd),
			       DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM,
			       DRM_XE_GEM_CPU_CACHING_WC, false);

		assert_caching(fd, vram_memory(fd, 0),
			       DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM,
			       DRM_XE_GEM_CPU_CACHING_WB, true);
		assert_caching(fd, vram_memory(fd, 0) | system_memory(fd),
			       DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM,
			       DRM_XE_GEM_CPU_CACHING_WB, true);
	}

	assert_caching(fd, system_memory(fd), 0, DRM_XE_GEM_CPU_CACHING_WB, false);
	assert_caching(fd, system_memory(fd), 0, DRM_XE_GEM_CPU_CACHING_WC, false);

	assert_caching(fd, system_memory(fd), 0, -1, true);
	assert_caching(fd, system_memory(fd), 0, 0, true);
	assert_caching(fd, system_memory(fd), 0, DRM_XE_GEM_CPU_CACHING_WC + 1, true);
}

static bool is_pci_membarrier_supported(int fd)
{
	struct drm_xe_gem_mmap_offset mmo = {
		.handle = 0,
		.flags = DRM_XE_MMAP_OFFSET_FLAG_PCI_BARRIER,
	};

	return (igt_ioctl(fd, DRM_IOCTL_XE_GEM_MMAP_OFFSET, &mmo) == 0);
}

static void prepare_pci_membarrier_test(int fd, int *fw_ptr)
{
	if (*fw_ptr > 0)
		return;

	igt_require(is_pci_membarrier_supported(fd));
	*fw_ptr = igt_debugfs_open(fd, "forcewake_all", O_RDONLY);
	igt_assert_lte(0, *fw_ptr);
}

igt_main
{
	int fd;

	igt_fixture()
		fd = drm_open_driver(DRIVER_XE);

	igt_subtest("system")
		test_mmap(fd, system_memory(fd), 0);

	igt_subtest("vram")
		test_mmap(fd, vram_memory(fd, 0),
			  DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);

	igt_subtest("vram-system")
		test_mmap(fd, vram_memory(fd, 0) | system_memory(fd),
			  DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);

	igt_subtest_group {
		int fw_handle = -1;

		igt_subtest("pci-membarrier") {
			prepare_pci_membarrier_test(fd, &fw_handle);
			test_pci_membarrier(fd);
		}

		igt_subtest("pci-membarrier-parallel") {
			int xe;
			unsigned int i;
			uint32_t *ptr;

			xe = drm_reopen_driver(fd);
			i = rand() % (PAGE_SIZE / sizeof(*ptr));
			prepare_pci_membarrier_test(fd, &fw_handle);
			igt_fork(child, 1)
				test_pci_membarrier_parallel(xe, child, i);
			test_pci_membarrier_parallel(fd, -1, i);
			igt_waitchildren();

			drm_close_driver(xe);
		}

		igt_subtest("pci-membarrier-bad-pagesize") {
			prepare_pci_membarrier_test(fd, &fw_handle);
			test_bad_pagesize_for_pcimem(fd);
		}

		igt_subtest("pci-membarrier-bad-object") {
			prepare_pci_membarrier_test(fd, &fw_handle);
			test_bad_object_for_pcimem(fd);
		}

		igt_fixture()
			close(fw_handle);
	}

	igt_subtest("bad-flags")
		test_bad_flags(fd);

	igt_subtest("bad-extensions")
		test_bad_extensions(fd);

	igt_subtest("bad-object")
		test_bad_object(fd);


	igt_subtest("small-bar") {
		igt_require(xe_visible_vram_size(fd, 0));
		igt_require(xe_visible_vram_size(fd, 0) < xe_vram_size(fd, 0));
		test_small_bar(fd);
	}

	igt_subtest("cpu-caching")
		test_cpu_caching(fd);

	igt_fixture()
		drm_close_driver(fd);
}
