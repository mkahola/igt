// SPDX-License-Identifier: MIT
// Copyright 2023 Advanced Micro Devices, Inc.

#include "igt.h"
#include "drmtest.h"
#include <amdgpu.h>
#include <amdgpu_drm.h>
#include "lib/amdgpu/amd_PM4.h"
#include "lib/amdgpu/amd_ip_blocks.h"
#include "lib/amdgpu/amd_memory.h"
#include "lib/amdgpu/amd_cs_radv.h"
#include "lib/amdgpu/amd_family.h"

#define IB_SIZE	4096

/**
 * Check if gang tests are enabled based on GPU information.
 *
 * Gang tests are supported starting with Vega10.
 * For generations Navi10 and Navi14, gang submit + reserved VMID doesn't work.
 * This function mirrors the logic in the following amdgpu code:
 *
 * void amdgpu_vm_manager_init(struct amdgpu_device *adev) {
 *     adev->vm_manager.concurrent_flush = !(adev->asic_type < CHIP_VEGA10 ||
 *                                           adev->asic_type == CHIP_NAVI10 ||
 *                                           adev->asic_type == CHIP_NAVI14);
 * }
 *
 * @param gpu_info: Pointer to the structure containing GPU information.
 * @return: True if gang tests are enabled, false otherwise.
 */
static bool is_gang_tests_enable(const struct chip_info *pChip)
{
	/* Concurrent flushes are supported only on Vega10 and newer,
	 * excluding Navi10 and Navi14 due to known issues.
	 */
	if (pChip->family < CHIP_VEGA10 ||
		pChip->family == CHIP_NAVI10 ||
		pChip->family == CHIP_NAVI14) {
		return false;
	}

	return true;
}

static void
prepare_compute_cp_packet(amdgpu_device_handle device,
		struct amdgpu_ring_context *ring_context,
		const struct amdgpu_ip_block_version *ip_block)
{
	int r;

	/* allocate buffer for compute  ring*/
	r = amdgpu_bo_alloc_and_map(device,
					ring_context->write_length * sizeof(uint32_t),
					IB_SIZE, AMDGPU_GEM_DOMAIN_GTT, 0,
					&ring_context->bo, (void **)&ring_context->bo_cpu,
					&ring_context->bo_mc, &ring_context->va_handle);
	igt_assert_eq(r, 0);
	memset((void *)ring_context->bo_cpu, 0,
			ring_context->write_length * sizeof(uint32_t));

	/* allocate buffer for pm4 packet for compute ring*/
	r = amdgpu_bo_alloc_and_map(device, IB_SIZE + ring_context->write_length *
					sizeof(uint32_t),
					IB_SIZE, AMDGPU_GEM_DOMAIN_GTT, 0,
					&ring_context->bo2, (void **)&ring_context->bo2_cpu,
					&ring_context->bo_mc2, &ring_context->va_handle2);
	igt_assert_eq(r, 0);

	memset((void *)ring_context->bo2_cpu, 0,
				ring_context->write_length * sizeof(uint32_t));
	/* assign fields used by ASIC dependent function */
	ring_context->pm4 = (uint32_t *)ring_context->bo2_cpu;
	ip_block->funcs->write_linear(ip_block->funcs, ring_context,
				&ring_context->pm4_dw);
}

static void
prepare_gfx_cp_mem_packet(amdgpu_device_handle device,
		struct amdgpu_ring_context *ring_context,
		const struct amdgpu_ip_block_version *ip_block)
{
	int r;
	uint32_t write_length;
	uint64_t bo_mc;

	/* allocate buffer for gfx  */
	r = amdgpu_bo_alloc_and_map(device,
					ring_context->write_length2 * sizeof(uint32_t),
					IB_SIZE, AMDGPU_GEM_DOMAIN_GTT, 0,
					&ring_context->bo3, (void **)&ring_context->bo3_cpu,
					&ring_context->bo_mc3, &ring_context->va_handle3);
	igt_assert_eq(r, 0);
	memset((void *)ring_context->bo3_cpu, 0,
			ring_context->write_length2 * sizeof(uint32_t));

	/* allocate buffer for pm4 packet gfx*/
	r = amdgpu_bo_alloc_and_map(device,  IB_SIZE + ring_context->write_length2 *
					sizeof(uint32_t),
					IB_SIZE, AMDGPU_GEM_DOMAIN_GTT, 0,
					&ring_context->bo4, (void **)&ring_context->bo4_cpu,
					&ring_context->bo_mc4, &ring_context->va_handle4);
	igt_assert_eq(r, 0);
	memset((void *)ring_context->bo4_cpu, 0,
			ring_context->write_length2 * sizeof(uint32_t));
	/* assign fields used by ASIC dependent functions */
	ring_context->pm4 = (uint32_t *)ring_context->bo4_cpu;
	bo_mc = ring_context->bo_mc;
	ring_context->bo_mc = ring_context->bo_mc3;
	write_length = ring_context->write_length;
	ring_context->write_length = ring_context->write_length2;

	ip_block->funcs->write_linear(ip_block->funcs, ring_context,
				&ring_context->pm4_dw2);
	/* addr -1 of compute buf*/
	ring_context->bo_mc = bo_mc + (write_length - 1) * 4;
	ip_block->funcs->wait_reg_mem(ip_block->funcs, ring_context,
				&ring_context->pm4_dw2);
	ring_context->bo_mc = bo_mc;
}

static void
wait_for_fence(amdgpu_context_handle context_handle, uint32_t seq_no)
{
	int r;
	uint32_t expired;
	struct amdgpu_cs_fence fence_status = {0};

	/* wait for fence */
	fence_status.context = context_handle;
	fence_status.ip_type = AMDGPU_HW_IP_GFX;
	fence_status.fence = seq_no;

	r = amdgpu_cs_wait_fences(&fence_status, 1, 1,
				  AMDGPU_TIMEOUT_INFINITE,
				  &expired, NULL);
	igt_assert_eq(r, 0);
}

static void
amdgpu_cs_gang(amdgpu_device_handle device, uint32_t ring, bool is_vmid)
{
	/* keep as big as ib can hold for compute write data packet so that even
	 * for powerful gpu, wait_data packet in gfx queue will have need to wait.
	 */
	const int sdma_write_length_compute = IB_SIZE * 3;
	/* keep it small for gfx write data packet so that gfx need to wait for compute */
	const int sdma_write_length_gfx = 4;

	struct amdgpu_cs_request_radv request;
	struct drm_amdgpu_bo_list_entry bo_handles[2] = {0};
	struct amdgpu_ring_context *ring_context = NULL;
	struct amdgpu_ctx_radv *ctx_radv = NULL;
	int r;
	uint32_t flags;

	const struct amdgpu_ip_block_version *gfx_ip_block =
			get_ip_block(device, AMD_IP_GFX);
	const struct amdgpu_ip_block_version *compute_ip_block =
			get_ip_block(device, AMD_IP_COMPUTE);

	memset(&request, 0, sizeof(request));
	ring_context = malloc(sizeof(*ring_context));
	memset(ring_context, 0, sizeof(*ring_context));
	ring_context->write_length = sdma_write_length_compute;
	ring_context->write_length2 = sdma_write_length_gfx;

	r = amdgpu_ctx_radv_create(device, AMDGPU_IGT_CTX_PRIORITY_MEDIUM,  &ctx_radv);
	igt_assert_eq(r, 0);

	if (is_vmid) {
		flags = 0;
		r = amdgpu_vm_reserve_vmid(device, flags);
		igt_assert_eq(r, 0);
	}

	prepare_compute_cp_packet(device, ring_context, compute_ip_block);
	prepare_gfx_cp_mem_packet(device, ring_context, gfx_ip_block);

	request.number_of_ibs = 2;
	request.ring = ring;

	request.ibs[0].ib_mc_address = ring_context->bo_mc2; /* pm4 packet addr compute */
	request.ibs[0].size = ring_context->pm4_dw; /* size p4 compute */
	request.ibs[0].ip_type = AMDGPU_HW_IP_COMPUTE;

	request.ibs[1].ib_mc_address =  ring_context->bo_mc4; /* p4 packet addr gfx */
	request.ibs[1].size = ring_context->pm4_dw2;	/* size p4 gfx */
	request.ibs[1].ip_type = AMDGPU_HW_IP_GFX;

	bo_handles[0].bo_handle = amdgpu_get_bo_handle(ring_context->bo4);
	bo_handles[0].bo_priority = 0;
	bo_handles[1].bo_handle = amdgpu_get_bo_handle(ring_context->bo2);
	bo_handles[1].bo_priority = 0;
	request.handles = bo_handles;
	request.num_handles = 2;

	/* submit pm4 packets for gfx and compute as gang */
	r = amdgpu_cs_submit_radv(device, ring_context, &request, ctx_radv);
	if (is_vmid == true)
		igt_assert_eq(r, 0);
	else
		igt_assert_eq(r, 0);

	wait_for_fence(ctx_radv->ctx, request.seq_no);

	if (is_vmid == false) {
		/* verify compute test result meets with expected */
		ring_context->bo_cpu = ring_context->bo_cpu;
		ring_context->write_length = sdma_write_length_compute;
		r = compute_ip_block->funcs->compare(compute_ip_block->funcs, ring_context, 1);
		igt_assert_eq(r, 0);

		ring_context->bo_cpu = ring_context->bo3_cpu;
		ring_context->write_length = sdma_write_length_gfx;
		r = gfx_ip_block->funcs->compare(gfx_ip_block->funcs, ring_context, 1);
		igt_assert_eq(r, 0);
	}

	amdgpu_bo_unmap_and_free(ring_context->bo, ring_context->va_handle,
			ring_context->bo_mc, sdma_write_length_gfx * sizeof(uint32_t));

	amdgpu_bo_unmap_and_free(ring_context->bo2, ring_context->va_handle2,
			ring_context->bo_mc2, IB_SIZE);

	amdgpu_bo_unmap_and_free(ring_context->bo3, ring_context->va_handle3,
			ring_context->bo_mc3, sdma_write_length_compute * sizeof(uint32_t));

	amdgpu_bo_unmap_and_free(ring_context->bo4, ring_context->va_handle4,
			ring_context->bo_mc4, IB_SIZE);
	amdgpu_ctx_radv_destroy(device, ctx_radv);

	if (is_vmid) {
		r = amdgpu_vm_unreserve_vmid(device, flags);
		igt_assert_eq(r, 0);
	}
	free(ring_context);
}

int igt_main()
{
	amdgpu_device_handle device;
	struct amdgpu_gpu_info gpu_info = {0};
	int fd = -1;
	int r;
	bool arr_cap[AMD_IP_MAX] = {0};

	igt_fixture() {
		uint32_t major, minor;
		int err;

		fd = drm_open_driver(DRIVER_AMDGPU);

		err = amdgpu_device_initialize(fd, &major, &minor, &device);
		igt_require(err == 0);

		igt_info("Initialized amdgpu, driver version %d.%d\n",
			 major, minor);

		r = amdgpu_query_gpu_info(device, &gpu_info);
		igt_assert_eq(r, 0);
		r = setup_amdgpu_ip_blocks(major, minor, &gpu_info, device);
		igt_assert_eq(r, 0);
		igt_skip_on(!is_gang_tests_enable(g_pChip));
		asic_rings_readness(device, 1, arr_cap);

	}

	igt_describe("Test GPU gang cs for gfx and compute rings");
	igt_subtest_with_dynamic("amdgpu-cs-gang") {
		if (arr_cap[AMD_IP_GFX] && arr_cap[AMD_IP_COMPUTE]) {
			igt_dynamic_f("amdgpu-cs-gang-AMD_IP_GFX-AMD_IP_COMPUTE")
				amdgpu_cs_gang(device, 0, false);
		}
	}
	igt_describe("Test GPU gang cs for gfx and compute rings vmid");
	igt_subtest_with_dynamic("amdgpu-cs-gang-vmid") {
		if (arr_cap[AMD_IP_GFX] && arr_cap[AMD_IP_COMPUTE]) {
			igt_dynamic_f("amdgpu-cs-gang-vmid-AMD_IP_GFX-AMD_IP_COMPUTE")
				amdgpu_cs_gang(device, 0, true);
		}
	}

	igt_fixture() {
		amdgpu_device_deinitialize(device);
		drm_close_driver(fd);
	}
}
