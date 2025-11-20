// SPDX-License-Identifier: MIT
/*
 * Copyright 2014 Advanced Micro Devices, Inc.
 * Copyright 2023 Advanced Micro Devices, Inc.
 */

#include <amdgpu.h>
#include "amdgpu_drm.h"

#include "igt.h"

#include "lib/amdgpu/amd_PM4.h"
#include "lib/amdgpu/amd_cp_dma.h"
#include "lib/amdgpu/amd_memory.h"

static bool
is_vm_tests_enable(uint32_t family_id)
{
	bool ret = true;

	if (family_id == AMDGPU_FAMILY_SI) {
		//TODO currently hangs the CP on this ASIC, VM  test is disabled
		ret = false;
	}

	return ret;
}

static void
amdgpu_vmid_reserve_test(amdgpu_device_handle device_handle,
		const struct amdgpu_gpu_info *gpu_info)
{
	amdgpu_context_handle context_handle;
	amdgpu_bo_handle ib_result_handle;
	void *ib_result_cpu;
	uint64_t ib_result_mc_address;
	struct amdgpu_cs_request ibs_request;
	struct amdgpu_cs_ib_info ib_info;
	struct amdgpu_cs_fence fence_status;
	uint32_t expired, flags;
	int i, r;
	amdgpu_bo_list_handle bo_list;
	amdgpu_va_handle va_handle;
	int32_t *ptr;
	unsigned int gc_ip_type;

	gc_ip_type = asic_is_gfx_pipe_removed(gpu_info) ? AMDGPU_HW_IP_COMPUTE :
							AMDGPU_HW_IP_GFX;

	r = amdgpu_cs_ctx_create(device_handle, &context_handle);
	igt_assert_eq(r, 0);

	flags = 0;
	r = amdgpu_vm_reserve_vmid(device_handle, flags);
	igt_assert_eq(r, 0);


	r = amdgpu_bo_alloc_and_map(device_handle, 4096, 4096,
			AMDGPU_GEM_DOMAIN_GTT, 0,
			&ib_result_handle, &ib_result_cpu,
			&ib_result_mc_address, &va_handle);
	igt_assert_eq(r, 0);

	r = amdgpu_get_bo_list(device_handle, ib_result_handle, NULL,
				&bo_list);
	igt_assert_eq(r, 0);

	ptr = ib_result_cpu;

	for (i = 0; i < 16; ++i)
		ptr[i] = GFX_COMPUTE_NOP;

	memset(&ib_info, 0, sizeof(struct amdgpu_cs_ib_info));
	ib_info.ib_mc_address = ib_result_mc_address;
	ib_info.size = 16;

	memset(&ibs_request, 0, sizeof(struct amdgpu_cs_request));
	ibs_request.ip_type = gc_ip_type;
	ibs_request.ring = 0;
	ibs_request.number_of_ibs = 1;
	ibs_request.ibs = &ib_info;
	ibs_request.resources = bo_list;
	ibs_request.fence_info.handle = NULL;

	r = amdgpu_cs_submit(context_handle, 0, &ibs_request, 1);
	igt_assert_eq(r, 0);


	memset(&fence_status, 0, sizeof(struct amdgpu_cs_fence));
	fence_status.context = context_handle;
	fence_status.ip_type = gc_ip_type;
	fence_status.ip_instance = 0;
	fence_status.ring = 0;
	fence_status.fence = ibs_request.seq_no;

	r = amdgpu_cs_query_fence_status(&fence_status,
			AMDGPU_TIMEOUT_INFINITE, 0, &expired);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_list_destroy(bo_list);
	igt_assert_eq(r, 0);

	amdgpu_bo_unmap_and_free(ib_result_handle, va_handle,
					ib_result_mc_address, 4096);

	flags = 0;
	r = amdgpu_vm_unreserve_vmid(device_handle, flags);
	igt_assert_eq(r, 0);


	r = amdgpu_cs_ctx_free(context_handle);
	igt_assert_eq(r, 0);
}

static void
amdgpu_vm_unaligned_map(amdgpu_device_handle device_handle)
{
	uint64_t map_size = (4ULL << 30) - (2 << 12);
	struct amdgpu_bo_alloc_request request = {};
	amdgpu_bo_handle buf_handle;
	amdgpu_va_handle handle;
	uint64_t vmc_addr, alignment = 1ULL << 30;
	int r;

	request.alloc_size = 4ULL << 30;
	request.phys_alignment = 4096;
	request.preferred_heap = AMDGPU_GEM_DOMAIN_VRAM;
	request.flags = AMDGPU_GEM_CREATE_NO_CPU_ACCESS;

	r = amdgpu_bo_alloc(device_handle, &request, &buf_handle);
	if (r == -ENOMEM) {
		/* Try allocate on the device of small memory */
		request.alloc_size = 8ULL << 20;
		map_size = (8ULL << 20) - (2 << 12);
		alignment = 2ULL << 20;
		r = amdgpu_bo_alloc(device_handle, &request, &buf_handle);
	}

	igt_assert_eq(r, 0);

	r = amdgpu_va_range_alloc(device_handle, amdgpu_gpu_va_range_general,
				request.alloc_size, alignment, 0, &vmc_addr,
				&handle, 0);
	igt_assert_eq(r, 0);

	vmc_addr += 1 << 12;

	r = amdgpu_bo_va_op(buf_handle, 0, map_size, vmc_addr, 0, AMDGPU_VA_OP_MAP);
	igt_assert_eq(r, 0);

	amdgpu_bo_va_op(buf_handle, 0, map_size, vmc_addr, 0,
			AMDGPU_VA_OP_UNMAP);

	amdgpu_bo_free(buf_handle);
}

static void
amdgpu_vm_mapping_test(amdgpu_device_handle device_handle)
{
	struct amdgpu_bo_alloc_request req = {0};
	struct drm_amdgpu_info_device dev_info;
	const uint64_t size = 4096;
	amdgpu_bo_handle buf;
	uint64_t addr;
	int r;

	req.alloc_size = size;
	req.phys_alignment = 0;
	req.preferred_heap = AMDGPU_GEM_DOMAIN_GTT;
	req.flags = 0;

	r = amdgpu_bo_alloc(device_handle, &req, &buf);
	igt_assert_eq(r, 0);

	r = amdgpu_query_info(device_handle, AMDGPU_INFO_DEV_INFO,
				sizeof(dev_info), &dev_info);
	igt_assert_eq(r, 0);

	addr = dev_info.virtual_address_offset;
	r = amdgpu_bo_va_op(buf, 0, size, addr, 0, AMDGPU_VA_OP_MAP);
	igt_assert_eq(r, 0);

	addr = dev_info.virtual_address_max - size;
	r = amdgpu_bo_va_op(buf, 0, size, addr, 0, AMDGPU_VA_OP_MAP);
	igt_assert_eq(r, 0);

	if (dev_info.high_va_offset) {
		addr = dev_info.high_va_offset;
		r = amdgpu_bo_va_op(buf, 0, size, addr, 0, AMDGPU_VA_OP_MAP);
		igt_assert_eq(r, 0);

		addr = dev_info.high_va_max - size;
		r = amdgpu_bo_va_op(buf, 0, size, addr, 0, AMDGPU_VA_OP_MAP);
		igt_assert_eq(r, 0);
	}

	amdgpu_bo_free(buf);
}

igt_main
{
	amdgpu_device_handle device;
	struct amdgpu_gpu_info gpu_info = {};
	int fd = -1;

	igt_fixture() {
		uint32_t major, minor;
		int err;

		fd = drm_open_driver(DRIVER_AMDGPU);
		err = amdgpu_device_initialize(fd, &major, &minor, &device);
		igt_require(err == 0);
		igt_info("Initialized amdgpu, driver version %d.%d\n",
			 major, minor);
		err = amdgpu_query_gpu_info(device, &gpu_info);
		igt_assert_eq(err, 0);

		igt_skip_on(!is_vm_tests_enable(gpu_info.family_id));
	}

	igt_describe("Test reserve vmid");
	igt_subtest("vmid-reserve-test")
	amdgpu_vmid_reserve_test(device, &gpu_info);

	igt_describe("Test unaligned map");
	igt_subtest("amdgpu-vm-unaligned-map")
	amdgpu_vm_unaligned_map(device);

	igt_describe("Test vm mapping");
	igt_subtest("amdgpu-vm-mapping-test")
	amdgpu_vm_mapping_test(device);

	igt_fixture() {
		amdgpu_device_deinitialize(device);
		drm_close_driver(fd);
	}
}
