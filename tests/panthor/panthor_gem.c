// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (C) 2025 Collabora Ltd.

#include <unistd.h>

#include "igt.h"
#include "igt_core.h"
#include "igt_panthor.h"

int igt_main() {
	int fd = -1;

	igt_fixture() {
		igt_panthor_skip_on_big_endian();
		fd = drm_open_driver(DRIVER_PANTHOR);
	}

	igt_describe("Create a buffer object");
	igt_subtest("bo_create") {
		struct panthor_bo bo;

		igt_panthor_bo_create(fd, &bo, 4096, 0, 0);
		igt_assert_neq(bo.handle, 0);

		igt_panthor_free_bo(fd, &bo);
	}

	igt_describe("Create a fake mmap offset for a buffer object");
	igt_subtest("bo_mmap_offset") {
		struct panthor_bo bo;
		uint64_t mmap_offset;

		igt_panthor_bo_create(fd, &bo, 4096, 0, 0);
		igt_assert_neq(bo.handle, 0);

		mmap_offset = igt_panthor_bo_mmap_offset(fd, bo.handle, 0);
		igt_assert_neq(mmap_offset, 0);

		igt_panthor_free_bo(fd, &bo);
	}

	igt_describe("Same as bo_mmap_offset but with an invalid handle");
	igt_subtest("bo_mmap_offset_invalid_handle") {
		struct panthor_bo bo;
		uint64_t mmap_offset;

		igt_panthor_bo_create(fd, &bo, 4096, 0, 0);
		igt_assert_neq(bo.handle, 0);

		mmap_offset = igt_panthor_bo_mmap_offset(fd, 0xdeadbeef, ENOENT);
		igt_assert_eq(mmap_offset, 0);

		igt_panthor_free_bo(fd, &bo);
	}

	igt_describe_f("Create a buffer object whose size is not page-aligned, and check "
		       "that the allocated size is rounded up to the next page size (%" PRIu64 ").",
		       (uint64_t)getpagesize() * 2);
	igt_subtest("bo_create_round_size") {
		struct panthor_bo bo;
		uint64_t expected_size = getpagesize() * 2;

		igt_panthor_bo_create(fd, &bo, 5000, 0, 0);
		igt_assert_neq(bo.handle, 0);
		igt_assert_eq(bo.size, expected_size);

		igt_panthor_free_bo(fd, &bo);
	}

	igt_describe_f("Check zero-ing of buffer at creation time");
	igt_subtest("bo_zeroed") {
		struct panthor_bo bo;

		igt_panthor_bo_create_mapped(fd, &bo, getpagesize(), 0, 0);
		igt_assert(bo.map);
		igt_assert_eq(*((uint32_t *)bo.map), 0);

		igt_panthor_free_bo(fd, &bo);
	}

	igt_fixture() {
		drm_close_driver(fd);
	}
}
