// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (C) 2025 Collabora Ltd.

#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

#include "igt.h"
#include "igt_panthor.h"
#include "igt_syncobj.h"
#include "panthor_drm.h"

static size_t
issue_store_multiple(uint8_t *cs, uint64_t kernel_va, uint32_t constant)
{
	const uint8_t kernel_va_reg = 68;
	const uint8_t constant_reg = 70;
	uint64_t instrs[] = {
		/* MOV48: Load the source register ([r68; r69]) with the kernel address */
		cs_mov48(kernel_va_reg, kernel_va),
		/* MOV32: Load a known constant into r70 */
		cs_mov32(constant_reg, constant),
		/* STORE_MULTIPLE: Store the first register to the address pointed
		 * to by [r68; r69]
		 */
		cs_stm32(kernel_va_reg, constant_reg, 0),
		/* FLUSH all Wait for all cores */
		cs_wait(0xff, false),
		/* MOV32: Clear r70 to 0 */
		cs_mov32(constant_reg, 0),
		/* FLUSH_CACHE: Clean and invalidate all caches */
		cs_flush(CS_FLUSH_MODE_CLEAN_AND_INVALIDATE,
			 CS_FLUSH_MODE_CLEAN_AND_INVALIDATE,
			 CS_FLUSH_MODE_INVALIDATE,
			 0, constant_reg, 1),
		cs_wait(0xff, false),
	};

	memcpy(cs, instrs, sizeof(instrs));
	return sizeof(instrs);
}

igt_main() {
	int fd;

	igt_fixture() {
		fd = drm_open_driver(DRIVER_PANTHOR);
	}

	igt_describe("Create and destroy a CSF group.");
	igt_subtest("group_create") {
		struct drm_panthor_vm_create vm_create = {};
		struct drm_panthor_vm_destroy vm_destroy = {};
		uint32_t group_handle;

		vm_create.flags = 0;
		do_ioctl(fd, DRM_IOCTL_PANTHOR_VM_CREATE, &vm_create);
		igt_assert_neq(vm_create.id, 0);

		group_handle = igt_panthor_group_create_simple(fd, vm_create.id, 0);
		igt_assert_neq(group_handle, 0);

		igt_panthor_group_destroy(fd, group_handle, 0);

		vm_destroy = (struct drm_panthor_vm_destroy) { .id = vm_create.id };
		do_ioctl(fd, DRM_IOCTL_PANTHOR_VM_DESTROY, &vm_destroy);
	}

	igt_describe("Submit a job to a group and wait for completion. "
		     "The job writes a known value to a buffer object that is then "
		     "mmaped and checked.");
	igt_subtest("group_submit") {
		uint32_t vm_id;
		uint32_t group_handle;
		struct panthor_bo cmd_buf_bo = {};
		struct panthor_bo result_bo = {};
		uint64_t command_stream_gpu_addr;
		uint32_t command_stream_size;
		uint64_t result_gpu_addr;
		uint32_t syncobj_handle;
		const int INITIAL_VA = 0x1000000;

		igt_panthor_vm_create(fd, &vm_id, 0);

		igt_panthor_bo_create_mapped(fd, &cmd_buf_bo, 4096, 0, 0);
		igt_panthor_vm_bind(fd, vm_id, cmd_buf_bo.handle, INITIAL_VA,
				    cmd_buf_bo.size, DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);
		command_stream_gpu_addr = INITIAL_VA;

		/* Create the BO to receive the result of the store. */
		igt_panthor_bo_create_mapped(fd, &result_bo, 4096, 0, 0);
		/* Also bind the result BO. */
		igt_panthor_vm_bind(fd, vm_id, result_bo.handle, INITIAL_VA + 4096,
				    result_bo.size, DRM_PANTHOR_VM_BIND_OP_TYPE_MAP, 0);
		result_gpu_addr = INITIAL_VA + 4096;

		command_stream_size = issue_store_multiple(cmd_buf_bo.map, result_gpu_addr, 0xdeadbeef);

		group_handle = igt_panthor_group_create_simple(fd, vm_id, 0);
		igt_assert_neq(group_handle, 0);

		syncobj_handle = syncobj_create(fd, 0);

		igt_panthor_group_submit_simple(fd, group_handle, 0, command_stream_gpu_addr, command_stream_size, syncobj_handle, 0);

		igt_assert(syncobj_wait(fd, &syncobj_handle, 1, INT64_MAX, 0, NULL));

		igt_assert_eq(*(uint32_t *)result_bo.map, 0xdeadbeef);

		syncobj_destroy(fd, syncobj_handle);

		igt_panthor_group_destroy(fd, group_handle, 0);

		igt_panthor_vm_destroy(fd, vm_id, 0);

		igt_panthor_free_bo(fd, &cmd_buf_bo);
		igt_panthor_free_bo(fd, &result_bo);
	}

	igt_fixture() {
		drm_close_driver(fd);
	}
}
