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

	igt_fixture() {
		drm_close_driver(fd);
	}
}
