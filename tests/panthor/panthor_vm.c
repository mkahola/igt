// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (C) 2025 Collabora Ltd.

#include "igt.h"
#include "igt_core.h"
#include "igt_panthor.h"
#include "panthor_drm.h"

int igt_main() {
	int fd;

	igt_fixture() {
		fd = drm_open_driver(DRIVER_PANTHOR);
	}

	igt_describe("Create and destroy a VM");
	igt_subtest("vm_create_destroy") {
		uint32_t vm_id;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert_neq(vm_id, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_subtest("vm_destroy_invalid") {
		igt_panthor_vm_destroy(fd, 0xdeadbeef, EINVAL);
	}

	igt_describe("Test the VM_BIND API synchronously");
	igt_subtest("vm_bind") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t bo_size = 0x1000;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert_neq(vm_id, 0);

		igt_panthor_bo_create(fd, &bo, bo_size, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle,
				    0x1000, 0x1000, DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Test unbinding a previously bound range");
	igt_subtest("vm_unbind") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t bo_size = 0x1000;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert_neq(vm_id, 0);

		igt_panthor_bo_create(fd, &bo, bo_size, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle,
				    0x1000, 0x1000, DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);
		igt_panthor_vm_bind(fd, vm_id, 0,
				    0x1000, 0x1000, DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Test unbinding an address range that was not previously bound");
	igt_subtest("vm_unbind_invalid_address") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t bo_size = 0x1000;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert_neq(vm_id, 0);

		igt_panthor_bo_create(fd, &bo, bo_size, 0, 0);

		/* This was not bound previously*/
		igt_panthor_vm_bind(fd, vm_id, bo.handle,
				    0x1000, 0x1000, DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, EINVAL);
		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Requested unmapping is identical to existing huge page mapping");
	igt_subtest("vm_unbind_identical_hugepage_single") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t bo_size = SZ_2M;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_bo_create(fd, &bo, bo_size, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle, 0x200000, bo_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		igt_panthor_vm_bind(fd, vm_id, 0, 0x200000, bo_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Requested unmapping is identical to existing huge page mapping, but only a subset of the object's pages are mapped in the VM");
	igt_subtest("vm_unbind_identical_hugepage_single_partial") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t bo_size = SZ_4M;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_bo_create(fd, &bo, bo_size, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle, 0x200000, bo_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		igt_panthor_vm_bind(fd, vm_id, 0, 0x200000, bo_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Requested unmapping is identical to existing multiple huge page mapping");
	igt_subtest("vm_unbind_identical_hugepage_multiple") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t bo_size = SZ_1M * 6;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_bo_create(fd, &bo, bo_size, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle, 0x200000, bo_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		igt_panthor_vm_bind(fd, vm_id, 0, 0x200000, bo_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Requested unmapping is identical to existing huge page mapping, but only part of the BO is mapped");
	igt_subtest("vm_unbind_identical_hugepage_offset") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t bo_size = SZ_1M * 6;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_bo_create(fd, &bo, bo_size, 0, 0);
		igt_panthor_vm_bind_offset(fd, vm_id, bo.handle, 0x200000,
					   SZ_4M, SZ_2M, DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		igt_panthor_vm_bind(fd, vm_id, 0, 0x200000, bo_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Requested unmapping is left aligned subset of an existing huge page mapping");
	igt_subtest("vm_unbind_hugepage_leftaligned") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t bo_size = SZ_4M;
		uint64_t unmap_size = SZ_8K;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_bo_create(fd, &bo, bo_size, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle, 0x200000, bo_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		igt_panthor_vm_bind(fd, vm_id, 0, 0x200000, unmap_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Requested unmapping is right aligned subset of an existing huge page mapping");
	igt_subtest("vm_unbind_hugepage_rightaligned") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t bo_size = SZ_4M;
		uint64_t unmap_size = SZ_8K;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_bo_create(fd, &bo, bo_size, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle, 0x200000, bo_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		igt_panthor_vm_bind(fd, vm_id, 0, 0x200000 + bo_size - unmap_size, unmap_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Requested unmapping is a superset of existing huge page mapping");
	igt_subtest("vm_unbind_hugepage_superset") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t bo_size = SZ_2M;
		uint64_t unmap_size = SZ_1M * 5;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_bo_create(fd, &bo, bo_size, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle, 0x200000, bo_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		igt_panthor_vm_bind(fd, vm_id, 0, 0x100000, unmap_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Requested unmapping is a subset of an existing mapping");
	igt_subtest("vm_unbind_hugepage_subset") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t bo_size = SZ_4M;
		uint64_t unmap_size = SZ_8K;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_bo_create(fd, &bo, bo_size, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle, 0x200000, bo_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		igt_panthor_vm_bind(fd, vm_id, 0, 0x200000 + SZ_1M, unmap_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);
		igt_panthor_vm_bind(fd, vm_id, 0, 0x200000 + SZ_2M + SZ_4K, unmap_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_describe("Perform successive unmaps over the remnants of an original multi huge page mapping");
	igt_subtest("vm_unbind_hugepage_successive") {
		uint32_t vm_id;
		struct panthor_bo bo;
		uint64_t bo_size = SZ_1M * 6;

		igt_panthor_vm_create(fd, &vm_id, 0);
		igt_assert(vm_id != 0);

		igt_panthor_bo_create(fd, &bo, bo_size, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, bo.handle, 0x200000, bo_size,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);

		igt_panthor_vm_bind(fd, vm_id, 0, 0x3fc000, 0x208000,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);
		igt_panthor_vm_bind(fd, vm_id, 0, 0x3fc000, 0x208000,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);
		igt_panthor_vm_bind(fd, vm_id, 0, 0x200000, 0x4000,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);
		igt_panthor_vm_bind(fd, vm_id, 0, 0x401000, 0x4000,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);
		igt_panthor_vm_bind(fd, vm_id, 0, 0x4fb000, 0xA000,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);
		igt_panthor_vm_bind(fd, vm_id, 0, 0x3fb000, 0xA000,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);
		igt_panthor_vm_bind(fd, vm_id, 0, 0x3fc000, 0x4000,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);
		igt_panthor_vm_bind(fd, vm_id, 0, 0x200000, 0x1000,
				    DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);
	}

	igt_fixture() {
		drm_close_driver(fd);
	}
}
