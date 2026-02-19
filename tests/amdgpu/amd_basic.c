// SPDX-License-Identifier: MIT
/*
 * Copyright 2014 Advanced Micro Devices, Inc.
 * Copyright 2022 Advanced Micro Devices, Inc.
 * Copyright 2023 Advanced Micro Devices, Inc.
 */

#include <pthread.h>
#include "lib/amdgpu/amd_memory.h"
#include "lib/amdgpu/amd_sdma.h"
#include "lib/amdgpu/amd_PM4.h"
#include "lib/amdgpu/amd_command_submission.h"
#include "lib/amdgpu/amd_compute.h"
#include "lib/amdgpu/amd_gfx.h"
#include "lib/amdgpu/shaders/amd_shaders.h"
#include "lib/amdgpu/compute_utils/amd_dispatch.h"
#include "lib/amdgpu/amdgpu_asic_addr.h"
#include "ioctl_wrappers.h"

#define BUFFER_SIZE (8 * 1024)

/**
 * MEM ALLOC TEST
 * @param device
 */
static void amdgpu_memory_alloc(amdgpu_device_handle device)
{
	amdgpu_bo_handle bo;
	amdgpu_va_handle va_handle;
	uint64_t bo_mc;

	/* Test visible VRAM */
	bo = gpu_mem_alloc(device,
			   4096, 4096,
			   AMDGPU_GEM_DOMAIN_VRAM,
			   AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED,
			   &bo_mc, &va_handle);

	gpu_mem_free(bo, va_handle, bo_mc, 4096);

	/* Test invisible VRAM */
	bo = gpu_mem_alloc(device,
			   4096, 4096,
			   AMDGPU_GEM_DOMAIN_VRAM,
			   AMDGPU_GEM_CREATE_NO_CPU_ACCESS,
			   &bo_mc, &va_handle);

	gpu_mem_free(bo, va_handle, bo_mc, 4096);

	/* Test GART Cacheable */
	bo = gpu_mem_alloc(device,
			   4096, 4096,
			   AMDGPU_GEM_DOMAIN_GTT,
			   0, &bo_mc, &va_handle);

	gpu_mem_free(bo, va_handle, bo_mc, 4096);

	/* Test GART USWC */
	bo = gpu_mem_alloc(device,
			   4096, 4096,
			   AMDGPU_GEM_DOMAIN_GTT,
			   AMDGPU_GEM_CREATE_CPU_GTT_USWC,
			   &bo_mc, &va_handle);

	gpu_mem_free(bo, va_handle, bo_mc, 4096);
}


/**
 * AMDGPU_HW_IP_GFX
 * @param device
 */
static void amdgpu_command_submission_gfx(amdgpu_device_handle device,
					  bool ce_avails,
					  bool user_queue)
{

	/* write data using the CP */
	amdgpu_command_submission_write_linear_helper(device,
						      get_ip_block(device, AMDGPU_HW_IP_GFX),
						      false, user_queue);

	/* const fill using the CP */
	amdgpu_command_submission_const_fill_helper(device,
						    get_ip_block(device, AMDGPU_HW_IP_GFX),
						    user_queue);

	/* copy data using the CP */
	amdgpu_command_submission_copy_linear_helper(device,
						     get_ip_block(device, AMDGPU_HW_IP_GFX),
						     user_queue);
	if (ce_avails) {
		/* separate IB buffers for multi-IB submission */
		amdgpu_command_submission_gfx_separate_ibs(device);
		/* shared IB buffer for multi-IB submission */
		amdgpu_command_submission_gfx_shared_ib(device);
	} else {
		igt_info("separate and shared IB buffers for multi IB submisison testes are skipped due to GFX11\n");
	}
}

/**
 * AMDGPU_HW_IP_COMPUTE
 * @param device
 */
static void amdgpu_command_submission_compute(amdgpu_device_handle device, bool user_queue)
{
	/* write data using the CP */
	amdgpu_command_submission_write_linear_helper(device,
						      get_ip_block(device, AMDGPU_HW_IP_COMPUTE),
						      false, user_queue);
	/* const fill using the CP */
	amdgpu_command_submission_const_fill_helper(device,
						    get_ip_block(device, AMDGPU_HW_IP_COMPUTE),
						    user_queue);
	/* copy data using the CP */
	amdgpu_command_submission_copy_linear_helper(device,
						     get_ip_block(device, AMDGPU_HW_IP_COMPUTE),
						     user_queue);
	/* nop test */
	amdgpu_command_submission_nop(device, AMDGPU_HW_IP_COMPUTE, user_queue);
}

/**
 * AMDGPU_HW_IP_DMA
 * @param device
 */
static void amdgpu_command_submission_sdma(amdgpu_device_handle device, bool user_queue)
{
	amdgpu_command_submission_write_linear_helper(device,
						      get_ip_block(device, AMDGPU_HW_IP_DMA),
						      false, user_queue);

	amdgpu_command_submission_const_fill_helper(device,
						    get_ip_block(device, AMDGPU_HW_IP_DMA),
						    user_queue);

	amdgpu_command_submission_copy_linear_helper(device,
						     get_ip_block(device, AMDGPU_HW_IP_DMA),
						     user_queue);
	/* nop test */
	amdgpu_command_submission_nop(device, AMDGPU_HW_IP_DMA, user_queue);
}

static void amdgpu_test_all_queues(amdgpu_device_handle device, bool user_queue)
{
	amdgpu_command_submission_write_linear_helper2(device, AMDGPU_HW_IP_GFX, false, user_queue);
	amdgpu_command_submission_write_linear_helper2(device, AMDGPU_HW_IP_COMPUTE, false, user_queue);
	amdgpu_command_submission_write_linear_helper2(device, AMDGPU_HW_IP_DMA, false, user_queue);
	amdgpu_command_submission_write_linear_helper2(device, AMDGPU_HW_IP_GFX |AMDGPU_HW_IP_COMPUTE |
							AMDGPU_HW_IP_DMA, false, user_queue);
}
/**
 * SEMAPHORE
 * @param device
 */
static void amdgpu_semaphore_test(amdgpu_device_handle device)
{
	amdgpu_context_handle context_handle[2];
	amdgpu_semaphore_handle sem;
	amdgpu_bo_handle ib_result_handle[2];
	void *ib_result_cpu[2];
	uint64_t ib_result_mc_address[2];
	struct amdgpu_cs_request ibs_request[2] = {};
	struct amdgpu_cs_ib_info ib_info[2] = {};
	struct amdgpu_cs_fence fence_status = {};
	uint32_t *ptr;
	uint32_t expired;
	amdgpu_bo_list_handle bo_list[2];
	amdgpu_va_handle va_handle[2];
	int r, i;

	r = amdgpu_cs_create_semaphore(&sem);
	igt_assert_eq(r, 0);
	for (i = 0; i < 2; i++) {
		r = amdgpu_cs_ctx_create(device, &context_handle[i]);
		igt_assert_eq(r, 0);

		r = amdgpu_bo_alloc_and_map(device, 4096, 4096,
					    AMDGPU_GEM_DOMAIN_GTT, 0,
					    &ib_result_handle[i], &ib_result_cpu[i],
					    &ib_result_mc_address[i], &va_handle[i]);
		igt_assert_eq(r, 0);

		r = amdgpu_get_bo_list(device, ib_result_handle[i],
				       NULL, &bo_list[i]);
		igt_assert_eq(r, 0);
	}

	/* 1. same context different engine */
	ptr = ib_result_cpu[0];
	ptr[0] = SDMA_NOP;
	ib_info[0].ib_mc_address = ib_result_mc_address[0];
	ib_info[0].size = 1;

	ibs_request[0].ip_type = AMDGPU_HW_IP_DMA;
	ibs_request[0].number_of_ibs = 1;
	ibs_request[0].ibs = &ib_info[0];
	ibs_request[0].resources = bo_list[0];
	ibs_request[0].fence_info.handle = NULL;
	r = amdgpu_cs_submit(context_handle[0], 0, &ibs_request[0], 1);
	igt_assert_eq(r, 0);
	r = amdgpu_cs_signal_semaphore(context_handle[0], AMDGPU_HW_IP_DMA, 0, 0, sem);
	igt_assert_eq(r, 0);

	r = amdgpu_cs_wait_semaphore(context_handle[0], AMDGPU_HW_IP_GFX, 0, 0, sem);
	igt_assert_eq(r, 0);
	ptr = ib_result_cpu[1];
	ptr[0] = GFX_COMPUTE_NOP;
	ib_info[1].ib_mc_address = ib_result_mc_address[1];
	ib_info[1].size = 1;

	ibs_request[1].ip_type = AMDGPU_HW_IP_GFX;
	ibs_request[1].number_of_ibs = 1;
	ibs_request[1].ibs = &ib_info[1];
	ibs_request[1].resources = bo_list[1];
	ibs_request[1].fence_info.handle = NULL;

	r = amdgpu_cs_submit(context_handle[0], 0, &ibs_request[1], 1);
	igt_assert_eq(r, 0);

	fence_status.context = context_handle[0];
	fence_status.ip_type = AMDGPU_HW_IP_GFX;
	fence_status.ip_instance = 0;
	fence_status.fence = ibs_request[1].seq_no;
	r = amdgpu_cs_query_fence_status(&fence_status,
					 500000000, 0, &expired);
	igt_assert_eq(r, 0);
	igt_assert_eq(expired, true);

	/* 2. same engine different context */
	ptr = ib_result_cpu[0];
	ptr[0] = GFX_COMPUTE_NOP;
	ib_info[0].ib_mc_address = ib_result_mc_address[0];
	ib_info[0].size = 1;

	ibs_request[0].ip_type = AMDGPU_HW_IP_GFX;
	ibs_request[0].number_of_ibs = 1;
	ibs_request[0].ibs = &ib_info[0];
	ibs_request[0].resources = bo_list[0];
	ibs_request[0].fence_info.handle = NULL;
	r = amdgpu_cs_submit(context_handle[0], 0, &ibs_request[0], 1);
	igt_assert_eq(r, 0);
	r = amdgpu_cs_signal_semaphore(context_handle[0], AMDGPU_HW_IP_GFX, 0, 0, sem);
	igt_assert_eq(r, 0);

	r = amdgpu_cs_wait_semaphore(context_handle[1], AMDGPU_HW_IP_GFX, 0, 0, sem);
	igt_assert_eq(r, 0);
	ptr = ib_result_cpu[1];
	ptr[0] = GFX_COMPUTE_NOP;
	ib_info[1].ib_mc_address = ib_result_mc_address[1];
	ib_info[1].size = 1;

	ibs_request[1].ip_type = AMDGPU_HW_IP_GFX;
	ibs_request[1].number_of_ibs = 1;
	ibs_request[1].ibs = &ib_info[1];
	ibs_request[1].resources = bo_list[1];
	ibs_request[1].fence_info.handle = NULL;
	r = amdgpu_cs_submit(context_handle[1], 0, &ibs_request[1], 1);

	igt_assert_eq(r, 0);

	fence_status.context = context_handle[1];
	fence_status.ip_type = AMDGPU_HW_IP_GFX;
	fence_status.ip_instance = 0;
	fence_status.fence = ibs_request[1].seq_no;
	r = amdgpu_cs_query_fence_status(&fence_status,
					 500000000, 0, &expired);
	igt_assert_eq(r, 0);
	igt_assert_eq(expired, true);
	for (i = 0; i < 2; i++) {
		amdgpu_bo_unmap_and_free(ib_result_handle[i], va_handle[i],
					 ib_result_mc_address[i], 4096);

		r = amdgpu_bo_list_destroy(bo_list[i]);
		igt_assert_eq(r, 0);

		r = amdgpu_cs_ctx_free(context_handle[i]);
		igt_assert_eq(r, 0);
	}

	r = amdgpu_cs_destroy_semaphore(sem);
	igt_assert_eq(r, 0);
}


/**
 * MULTI FENCE
 * @param device
 */
static void amdgpu_command_submission_multi_fence(amdgpu_device_handle device)
{
	amdgpu_command_submission_multi_fence_wait_all(device, true);
	amdgpu_command_submission_multi_fence_wait_all(device, false);
}

static void amdgpu_userptr_test(amdgpu_device_handle device)
{
	const int pm4_dw = 256;
	const int sdma_write_length = 16;	/* 4 DWORDs in bytes */

	struct amdgpu_ring_context *ring_context;
	int r;

	const struct amdgpu_ip_block_version *ip_block = get_ip_block(device, AMDGPU_HW_IP_DMA);

	igt_assert(ip_block);
	ring_context = calloc(1, sizeof(*ring_context));
	igt_assert(ring_context);

	/* setup parameters */

	ring_context->write_length =  sdma_write_length;
	ring_context->pm4 = calloc(pm4_dw, sizeof(*ring_context->pm4));
	ring_context->secure = false;
	ring_context->pm4_size = pm4_dw;
	ring_context->res_cnt = 1;
	igt_assert(ring_context->pm4);

	r = amdgpu_cs_ctx_create(device, &ring_context->context_handle);
	igt_assert_eq(r, 0);

	posix_memalign((void **)&ring_context->bo_cpu, sysconf(_SC_PAGE_SIZE), BUFFER_SIZE);
	igt_assert(ring_context->bo_cpu);
	memset((void *)ring_context->bo_cpu, 0, BUFFER_SIZE);

	r = amdgpu_create_bo_from_user_mem(device,
					   (void *)ring_context->bo_cpu,
					   BUFFER_SIZE, &ring_context->bo);
	igt_assert_eq(r, 0);

	ring_context->resources[0] = ring_context->bo;


	r = amdgpu_va_range_alloc(device,
				  amdgpu_gpu_va_range_general,
				  BUFFER_SIZE, 1, 0, &ring_context->bo_mc,
				  &ring_context->va_handle, 0);

	igt_assert_eq(r, 0);

	r = amdgpu_bo_va_op(ring_context->bo, 0, BUFFER_SIZE, ring_context->bo_mc, 0, AMDGPU_VA_OP_MAP);

	igt_assert_eq(r, 0);

	ip_block->funcs->write_linear(ip_block->funcs, ring_context, &ring_context->pm4_dw);

	 amdgpu_test_exec_cs_helper(device, ip_block->type, ring_context, 0);

	/* div=4: write_length is bytes, bo_cpu is uint32_t* */
	r = ip_block->funcs->compare(ip_block->funcs, ring_context, 4);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_va_op(ring_context->bo, 0, BUFFER_SIZE, ring_context->bo_mc, 0, AMDGPU_VA_OP_UNMAP);
	igt_assert_eq(r, 0);
	r = amdgpu_va_range_free(ring_context->va_handle);
	igt_assert_eq(r, 0);
	r = amdgpu_bo_free(ring_context->bo);
	igt_assert_eq(r, 0);

	r = amdgpu_cs_ctx_free(ring_context->context_handle);
	igt_assert_eq(r, 0);

	free(ring_context->pm4);
	free(ring_context);
}

static void
amdgpu_bo_eviction_test(amdgpu_device_handle device_handle)
{
	const int sdma_write_length = 1024;
	const int pm4_dw = 256;

	struct amdgpu_ring_context *ring_context;
	struct amdgpu_heap_info vram_info, gtt_info;
	int r, loop1, loop2;


	uint64_t gtt_flags[2] = {0, AMDGPU_GEM_CREATE_CPU_GTT_USWC};

	const struct amdgpu_ip_block_version *ip_block = get_ip_block(device_handle, AMDGPU_HW_IP_DMA);

	igt_assert(ip_block);

	ring_context = calloc(1, sizeof(*ring_context));
	ring_context->write_length =  sdma_write_length;
	ring_context->pm4 = calloc(pm4_dw, sizeof(*ring_context->pm4));
	ring_context->secure = false;
	ring_context->pm4_size = pm4_dw;
	ring_context->res_cnt = 4;
	igt_assert(ring_context->pm4);

	r = amdgpu_cs_ctx_create(device_handle, &ring_context->context_handle);
	igt_assert_eq(r, 0);

	r = amdgpu_query_heap_info(device_handle, AMDGPU_GEM_DOMAIN_VRAM,
				   0, &vram_info);
	igt_assert_eq(r, 0);

	r = amdgpu_query_heap_info(device_handle, AMDGPU_GEM_DOMAIN_GTT,
				   0, &gtt_info);
	igt_assert_eq(r, 0);

	/* For smaller gtt memory sizes, reduce gtt usage on initialization
	 * to satisfy eviction vram requirements. Example:
	 * gtt_info.heap_size 3036569600, gtt_info.max_allocation 2114244608  gtt_info.heap_usage 12845056
	 * gtt_info.heap_size 2895 mb, gtt_info.max_allocation 2016 mb gtt_info.heap_usage 12 mb
	 * vram_info.heap_size 2114244608, vram_info.max_allocation 2114244608 vram_info.heap_usage 26951680
	 * vram_info.heap_size 2016 mb, vram_info.max_allocation 2016 mb vram_info.heap_usage 25 mb
	 */
	if (gtt_info.heap_size - gtt_info.max_allocation < vram_info.max_allocation)
		gtt_info.max_allocation /=3;

	r = amdgpu_bo_alloc_wrap(device_handle, vram_info.max_allocation, 4096,
				 AMDGPU_GEM_DOMAIN_VRAM, 0, &ring_context->boa_vram[0]);
	igt_assert_eq(r, 0);
	r = amdgpu_bo_alloc_wrap(device_handle, vram_info.max_allocation, 4096,
				 AMDGPU_GEM_DOMAIN_VRAM, 0, &ring_context->boa_vram[1]);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_alloc_wrap(device_handle, gtt_info.max_allocation, 4096,
				 AMDGPU_GEM_DOMAIN_GTT, 0, &ring_context->boa_gtt[0]);
	igt_assert_eq(r, 0);
	r = amdgpu_bo_alloc_wrap(device_handle, gtt_info.max_allocation, 4096,
				 AMDGPU_GEM_DOMAIN_GTT, 0, &ring_context->boa_gtt[1]);
	igt_assert_eq(r, 0);



	loop1 = loop2 = 0;
	/* run 9 circle to test all mapping combination */
	while (loop1 < 2) {
		while (loop2 < 2) {
			/* allocate UC bo1for sDMA use */
			r = amdgpu_bo_alloc_and_map(device_handle,
						    sdma_write_length, 4096,
						    AMDGPU_GEM_DOMAIN_GTT,
						    gtt_flags[loop1],  &ring_context->bo,
						    (void **)&ring_context->bo_cpu, &ring_context->bo_mc,
						    &ring_context->va_handle);
			igt_assert_eq(r, 0);

			/* set bo1 */
			memset((void *)ring_context->bo_cpu, ip_block->funcs->pattern, ring_context->write_length);

			/* allocate UC bo2 for sDMA use */
			r = amdgpu_bo_alloc_and_map(device_handle,
						    sdma_write_length, 4096,
						    AMDGPU_GEM_DOMAIN_GTT,
						    gtt_flags[loop2], &ring_context->bo2,
						    (void **)&ring_context->bo2_cpu, &ring_context->bo_mc2,
						    &ring_context->va_handle2);
			igt_assert_eq(r, 0);

			/* clear bo2 */
			memset((void *)ring_context->bo2_cpu, 0, ring_context->write_length);

			ring_context->resources[0] = ring_context->bo;
			ring_context->resources[1] = ring_context->bo2;

			ring_context->resources[2] = ring_context->boa_vram[loop2];
			ring_context->resources[3] = ring_context->boa_gtt[loop2];
			ip_block->funcs->copy_linear(ip_block->funcs, ring_context, &ring_context->pm4_dw);
			amdgpu_test_exec_cs_helper(device_handle, ip_block->type, ring_context, 0);
			/* fulfill PM4: test DMA copy linear */
			r = ip_block->funcs->compare_pattern(ip_block->funcs, ring_context, sdma_write_length);
			igt_assert_eq(r, 0);
			amdgpu_bo_unmap_and_free(ring_context->bo, ring_context->va_handle, ring_context->bo_mc,
						 ring_context->write_length);
			amdgpu_bo_unmap_and_free(ring_context->bo2, ring_context->va_handle2, ring_context->bo_mc2,
						 ring_context->write_length);

			loop2++;
		}
		loop2 = 0;
		loop1++;
	}
	amdgpu_bo_free(ring_context->boa_vram[0]);
	amdgpu_bo_free(ring_context->boa_vram[1]);
	amdgpu_bo_free(ring_context->boa_gtt[0]);
	amdgpu_bo_free(ring_context->boa_gtt[1]);
	/* clean resources */

	/* end of test */
	r = amdgpu_cs_ctx_free(ring_context->context_handle);
	igt_assert_eq(r, 0);
	free(ring_context);
}

static void
amdgpu_sync_dependency_test(amdgpu_device_handle device_handle, bool user_queue)
{
	const unsigned const_size = 8192;
	const unsigned const_alignment = 4096;

	amdgpu_context_handle context_handle[2];

	amdgpu_bo_handle ib_result_handle;
	void *ib_result_cpu;
	uint64_t ib_result_mc_address;
	struct amdgpu_cs_request ibs_request;
	struct amdgpu_cs_ib_info ib_info;
	struct amdgpu_cs_fence fence_status;
	uint32_t expired;
	int r;
	amdgpu_bo_list_handle bo_list;
	amdgpu_va_handle va_handle;
	uint64_t seq_no;

	uint32_t cdw_old;

	uint32_t size_bytes, code_offset, data_offset;
	const uint32_t *shader;
	struct amdgpu_ring_context *ring_context;

	struct amdgpu_cmd_base *base = get_cmd_base();
	const struct amdgpu_ip_block_version *ip_block = get_ip_block(device_handle, AMD_IP_GFX);

	ring_context = calloc(1, sizeof(*ring_context));
	igt_assert(ring_context);

	if (user_queue) {
		ip_block->funcs->userq_create(device_handle, ring_context, ip_block->type);
	} else {
		r = amdgpu_cs_ctx_create(device_handle, &context_handle[0]);
		igt_assert_eq(r, 0);

		r = amdgpu_cs_ctx_create(device_handle, &context_handle[1]);
		igt_assert_eq(r, 0);
	}

	r = amdgpu_bo_alloc_and_map_sync(device_handle, const_size,
					 const_alignment, AMDGPU_GEM_DOMAIN_GTT, 0,
					 AMDGPU_VM_MTYPE_UC,
					 &ib_result_handle, &ib_result_cpu,
					 &ib_result_mc_address, &va_handle,
					 ring_context->timeline_syncobj_handle,
					 ++ring_context->point, user_queue);

	igt_assert_eq(r, 0);

	if (user_queue) {
		r = amdgpu_timeline_syncobj_wait(device_handle,
						 ring_context->timeline_syncobj_handle,
						 ring_context->point);
		igt_assert_eq(r, 0);
	} else {
		r = amdgpu_get_bo_list(device_handle, ib_result_handle, NULL,
			       &bo_list);
		igt_assert_eq(r, 0);
	}

	shader = get_shader_bin(&size_bytes, &code_offset, &data_offset);

	/* assign cmd buffer */
	base->attach_buf(base, ib_result_cpu, const_size);

	/* program compute */
	ip_block->funcs->gfx_program_compute(
		ip_block->funcs,
		base,
		ib_result_mc_address + (uint64_t)code_offset * 4,
		ib_result_mc_address + (uint64_t)data_offset * 4,
		0x002c0040,
		0x00000010,
		1, 1, 1);

	/* dispatch (flags chosen per-family in overrides) */
	ip_block->funcs->gfx_dispatch_direct( ip_block->funcs, base, 1, 1, 1, 0);


	memcpy(base->buf + code_offset, shader, size_bytes);

	memset(&ib_info, 0, sizeof(struct amdgpu_cs_ib_info));
	ib_info.ib_mc_address = ib_result_mc_address;
	ib_info.size = base->cdw;

	memset(&ibs_request, 0, sizeof(struct amdgpu_cs_request));
	ibs_request.ip_type = AMDGPU_HW_IP_GFX;
	ibs_request.ring = 0;
	ibs_request.number_of_ibs = 1;
	ibs_request.ibs = &ib_info;
	ibs_request.resources = bo_list;
	ibs_request.fence_info.handle = NULL;

	if (user_queue) {
		ring_context->pm4_dw = ib_info.size;
		ip_block->funcs->userq_submit(device_handle, ring_context, ip_block->type,
					 ib_result_mc_address);
	} else {
		r = amdgpu_cs_submit(context_handle[1], 0, &ibs_request, 1);
	}

	igt_assert_eq(r, 0);
	seq_no = ibs_request.seq_no;

	cdw_old = base->cdw;

	/* confirmed write */
	ip_block->funcs->gfx_write_confirm(
		ip_block->funcs, base, ib_result_mc_address + (uint64_t)data_offset * 4,
		99);

	memset(&ib_info, 0, sizeof(struct amdgpu_cs_ib_info));
	ib_info.ib_mc_address = ib_result_mc_address + cdw_old * 4;
	ib_info.size = base->cdw - cdw_old;

	memset(&ibs_request, 0, sizeof(struct amdgpu_cs_request));
	ibs_request.ip_type = AMDGPU_HW_IP_GFX;
	ibs_request.ring = 0;
	ibs_request.number_of_ibs = 1;
	ibs_request.ibs = &ib_info;
	ibs_request.resources = bo_list;
	ibs_request.fence_info.handle = NULL;

	ibs_request.number_of_dependencies = 1;

	ibs_request.dependencies = calloc(1, sizeof(*ibs_request.dependencies));
	ibs_request.dependencies[0].context = context_handle[1];
	ibs_request.dependencies[0].ip_instance = 0;
	ibs_request.dependencies[0].ring = 0;
	ibs_request.dependencies[0].fence = seq_no;

	if (user_queue) {
		ring_context->pm4_dw = ib_info.size;
		ip_block->funcs->userq_submit(device_handle, ring_context, ip_block->type,
					ib_info.ib_mc_address);
	} else {
		r = amdgpu_cs_submit(context_handle[0], 0, &ibs_request, 1);
		igt_assert_eq(r, 0);
	}

	memset(&fence_status, 0, sizeof(struct amdgpu_cs_fence));
	fence_status.context = context_handle[0];
	fence_status.ip_type = AMDGPU_HW_IP_GFX;
	fence_status.ip_instance = 0;
	fence_status.ring = 0;
	fence_status.fence = ibs_request.seq_no;

	if (!user_queue) {
		r = amdgpu_cs_query_fence_status(&fence_status,
		       AMDGPU_TIMEOUT_INFINITE, 0, &expired);
		igt_assert_eq(r, 0);
	}

	/* Expect the second command to wait for shader to complete */
	igt_assert_eq(base->buf[data_offset], 99);

	if (!user_queue) {
		r = amdgpu_bo_list_destroy(bo_list);
		igt_assert_eq(r, 0);
	}

	amdgpu_bo_unmap_and_free(ib_result_handle, va_handle,
				 ib_result_mc_address, const_alignment);

	if (user_queue) {
		ip_block->funcs->userq_destroy(device_handle, ring_context, ip_block->type);
	} else {
		amdgpu_cs_ctx_free(context_handle[0]);
		amdgpu_cs_ctx_free(context_handle[1]);
	}

	free(ibs_request.dependencies);
	free_cmd_base(base);
	free(ring_context);
}

static int wait_for_value64(volatile uint64_t *ptr, uint64_t expected,
			    uint64_t timeout_ns, uint64_t check_interval_ns)
{
	struct timespec start, now, sleep_time;
	uint64_t elapsed_ns;

	if (!ptr)
		return -EINVAL;

	clock_gettime(CLOCK_MONOTONIC, &start);

	if (check_interval_ns > 0) {
		sleep_time.tv_sec = check_interval_ns / 1000000000ULL;
		sleep_time.tv_nsec = check_interval_ns % 1000000000ULL;
	}

	while (1) {
		/* Check current value */
		if (*ptr == expected)
			return 0;

		/* Check timeout if specified */
		if (timeout_ns > 0) {
			clock_gettime(CLOCK_MONOTONIC, &now);
			elapsed_ns = (now.tv_sec - start.tv_sec) * 1000000000ULL +
				     (now.tv_nsec - start.tv_nsec);

			if (elapsed_ns >= timeout_ns)
				return -ETIMEDOUT;
		}

		/* Sleep if interval specified, otherwise tight loop */
		if (check_interval_ns > 0)
			nanosleep(&sleep_time, NULL);
		else
			__asm__ __volatile__("pause" ::: "memory"); /* CPU hint for spin-wait */
	}
}

static void* amdgpu_fwm_test_thread(void *data)
{
	struct amdgpu_ring_context *ring_context = data;
	uint64_t *job_start_write_data_ptr = (uint64_t*)ring_context->bo3_cpu;
	uint64_t *fwm_fence_ptr = (uint64_t*)ring_context->bo3_cpu + 1;
	int *ret = calloc(1, sizeof(int));

	/* Poll to confirm if job has started. This is done to have low timeout for fences
	 * wrongly satsified check.
	 */
	*ret = wait_for_value64((uint64_t*)job_start_write_data_ptr, 0x1, 2 * 1000000, 1000);
	if (*ret)
		return ret;

	/* Poll if job completed without fence satisfied */
	*ret = wait_for_value64((uint64_t*)ring_context->bo_cpu, 0xdeadbeef, 100 * 1000, 2000);
	if (*ret == 0) {
		*ret = -EINVAL;
		return ret;
	}

	for (unsigned i = 0; i < 32; i++)
		fwm_fence_ptr[i] = i + 1;

	*ret = 0;
	return ret;
}

/**
 * FWM PACKET TEST
 * @param device_handle
 * @param ip_type
 */
static void amdgpu_fwm_test(amdgpu_device_handle device_handle, unsigned ip_type)
{
	struct amdgpu_ring_context *ring_context;
	struct amdgpu_cmd_base *cmd_base = get_cmd_base();
	const struct amdgpu_ip_block_version *ip_block = get_ip_block(device_handle, ip_type);
	int r;
	unsigned int i;
	struct amdgpu_userq_params *userq_params;
	pthread_t fwm_test_thread;
	int *thread_ret;

	ring_context = calloc(1, sizeof(struct amdgpu_ring_context));
	igt_assert(ring_context);
	if (ip_block->funcs->family_id == FAMILY_GFX1150)
		ring_context->max_num_fences_fwm = 4;
	else
		ring_context->max_num_fences_fwm = 32;

	ip_block->funcs->userq_create(device_handle, ring_context, ip_block->type);

	/* Allocate bo for dma */
	ring_context->write_length = 1024;
	r = amdgpu_bo_alloc_and_map_sync(device_handle , ring_context->write_length, 4096,
					 AMDGPU_GEM_DOMAIN_GTT, AMDGPU_GEM_CREATE_CPU_GTT_USWC,
					 AMDGPU_VM_MTYPE_UC, &ring_context->bo,
					 (void **)&ring_context->bo_cpu, &ring_context->bo_mc,
					 &ring_context->va_handle,
					 ring_context->timeline_syncobj_handle,
					 ++ring_context->point, true);
	igt_assert_eq(r, 0);
	memset((void *)ring_context->bo_cpu, 0, ring_context->write_length);

	/* Allocate bo2 for ib */
	r = amdgpu_bo_alloc_and_map_sync(device_handle, 8192, 4096, AMDGPU_GEM_DOMAIN_GTT,
					 AMDGPU_GEM_CREATE_CPU_GTT_USWC, AMDGPU_VM_MTYPE_UC,
					 &ring_context->bo2, (void **)&ring_context->bo2_cpu,
					 &ring_context->bo_mc2, &ring_context->va_handle2,
					 ring_context->timeline_syncobj_handle,
					 ++ring_context->point, true);
	igt_assert_eq(r, 0);
	memset((void *)ring_context->bo2_cpu, 0, 8192);

	/* Allocate bo3 for fence wait packet fences */
	r = amdgpu_bo_alloc_and_map_sync(device_handle, 4096, 4096, AMDGPU_GEM_DOMAIN_GTT,
					 AMDGPU_GEM_CREATE_CPU_GTT_USWC, AMDGPU_VM_MTYPE_UC,
					 &ring_context->bo3, (void **)&ring_context->bo3_cpu,
					 &ring_context->bo_mc3, &ring_context->va_handle3,
					 ring_context->timeline_syncobj_handle,
					 ++ring_context->point, true);
	igt_assert_eq(r, 0);
	memset((void *)ring_context->bo3_cpu, 0, 4096);

	/* wait for gtt mapping to complete */
	r = amdgpu_timeline_syncobj_wait(device_handle, ring_context->timeline_syncobj_handle,
					 ring_context->point);
	igt_assert_eq(r, 0);

	/* assign cmd buffer for ring context */
	cmd_base->attach_buf(cmd_base, (void *)ring_context->bo2_cpu, 8192);

	userq_params = (struct amdgpu_userq_params*)alloca(sizeof(struct amdgpu_userq_params) +
							   sizeof(struct drm_amdgpu_userq_fence_info) * 32);
	userq_params->fence_info = (struct drm_amdgpu_userq_fence_info*)(userq_params + 1);

	ring_context->userq_params = userq_params;
	userq_params->job_start_write_data_va_addr = ring_context->bo_mc3;
	userq_params->job_start_write_data_val = 0x1;
	userq_params->num_fences = 32;
	for (i = 0; i < 32; i++) {
		userq_params->fence_info[i].va = ring_context->bo_mc3 + 8 + (i * 8);
		userq_params->fence_info[i].value = i + 1;
	}

	cmd_base->emit(cmd_base, PACKET3(PACKET3_WRITE_DATA, 3));
	cmd_base->emit(cmd_base, WRITE_DATA_DST_SEL(5) | WR_CONFIRM |
		       WRITE_DATA_CACHE_POLICY(3));
	cmd_base->emit(cmd_base, lower_32_bits(ring_context->bo_mc));
	cmd_base->emit(cmd_base, upper_32_bits(ring_context->bo_mc));
	cmd_base->emit(cmd_base, 0xdeadbeef);

	/* satisfy fence wait multi packet fences in separate thread for checking if fences
	 * gets satisifed incorrectly.
	 */
	pthread_create(&fwm_test_thread, NULL, amdgpu_fwm_test_thread, ring_context);

	ring_context->pm4_dw = cmd_base->cdw;
	ip_block->funcs->userq_submit(device_handle, ring_context, ip_block->type,
				      ring_context->bo_mc2);

	pthread_join(fwm_test_thread, (void **)&thread_ret);
	igt_assert_eq(*thread_ret, 0);
	igt_assert_eq_u32(*ring_context->bo_cpu, 0xdeadbeef);

	free(thread_ret);
	ip_block->funcs->userq_destroy(device_handle, ring_context, ip_block->type);
	amdgpu_bo_unmap_and_free(ring_context->bo, ring_context->va_handle,
				 ring_context->bo_mc, ring_context->write_length);
	amdgpu_bo_unmap_and_free(ring_context->bo2, ring_context->va_handle2,
				 ring_context->bo_mc2, 8192);
	amdgpu_bo_unmap_and_free(ring_context->bo3, ring_context->va_handle3,
				 ring_context->bo_mc3, 4096);
	free_cmd_base(cmd_base);
	free(ring_context);
}

int igt_main()
{
	amdgpu_device_handle device;
	struct amdgpu_gpu_info gpu_info = {0};
	struct drm_amdgpu_info_hw_ip info = {0};
	int fd = -1;
	int r;
	bool arr_cap[AMD_IP_MAX] = {0};
	bool userq_arr_cap[AMD_IP_MAX] = {0};
	bool enable_test = false;
#ifdef AMDGPU_USERQ_ENABLED
	const char *env = getenv("AMDGPU_ENABLE_USERQTEST");

	enable_test = env && atoi(env);
#endif

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
		r = amdgpu_query_hw_ip_info(device, AMDGPU_HW_IP_GFX, 0, &info);
		igt_assert_eq(r, 0);
		r = setup_amdgpu_ip_blocks(major, minor,  &gpu_info, device);
		igt_assert_eq(r, 0);
		asic_rings_readness(device, 1, arr_cap);
		asic_userq_readiness(device, userq_arr_cap);
	}
	igt_describe("Check-alloc-free-VRAM-visible-non-visible-GART-write-combined-cached");
	igt_subtest("memory-alloc")
		amdgpu_memory_alloc(device);

	igt_describe("Check-DMA-CS-works-by-setting-the-pattern-and-after-execution-compare-memory-with-the-golden-settings");
	igt_subtest_with_dynamic("userptr-with-IP-DMA") {
		if (arr_cap[AMD_IP_DMA]) {
			igt_dynamic_f("userptr")
			amdgpu_userptr_test(device);
		}
	}

	igt_describe("Check-GFX-CS-for-every-available-ring-works-for-write-const-fill-and-copy-operation-using-more-than-one-IB-and-shared-IB");
	igt_subtest_with_dynamic("cs-gfx-with-IP-GFX") {
		if (arr_cap[AMD_IP_GFX]) {
			igt_dynamic_f("cs-gfx")
			amdgpu_command_submission_gfx(device, info.hw_ip_version_major < 11, false);
		}
	}

	igt_describe("Check-COMPUTE-CS-for-every-available-ring-works-for-write-const-fill-copy-and-nop-operation");
	igt_subtest_with_dynamic("cs-compute-with-IP-COMPUTE") {
		if (arr_cap[AMD_IP_COMPUTE]) {
			igt_dynamic_f("cs-compute")
			amdgpu_command_submission_compute(device, false);
		}
	}

	igt_describe("Check-GFX-CS-for-multi-fence");
	igt_subtest_with_dynamic("cs-multi-fence-with-IP-GFX") {
		if (arr_cap[AMD_IP_GFX] && info.hw_ip_version_major < 11) {
			igt_dynamic_f("cs-multi-fence")
			amdgpu_command_submission_multi_fence(device);
		} else {
			igt_info("cs-multi-fence-with-IP-GFX testes are skipped due to GFX11 or no GFX_IP\n");
		}
	}

	igt_describe("Check-DMA-CS-for-every-available-ring-works-for-write-const-fill-copy-operation");
	igt_subtest_with_dynamic("cs-sdma-with-IP-DMA") {
		if (arr_cap[AMD_IP_DMA]) {
			igt_dynamic_f("cs-sdma")
			amdgpu_command_submission_sdma(device, false);
		}
	}

	igt_describe("Check-signal-semaphore-on-DMA-wait-on-GFX");
	igt_subtest_with_dynamic("semaphore-with-IP-GFX-and-IP-DMA") {
		if (arr_cap[AMD_IP_GFX] && arr_cap[AMD_IP_DMA]) {
			igt_dynamic_f("semaphore")
			amdgpu_semaphore_test(device);
		}
	}

	igt_describe("Check-eviction-using-DMA-max-allocation-size");
	igt_subtest_with_dynamic("eviction-test-with-IP-DMA") {
		if (arr_cap[AMD_IP_DMA]) {
			igt_dynamic_f("eviction_test")
			amdgpu_bo_eviction_test(device);
		}
	}

	igt_describe("Check-sync-dependency-using-GFX-ring");
	igt_subtest_with_dynamic("sync-dependency-test-with-IP-GFX") {
		if (arr_cap[AMD_IP_GFX]) {
			igt_dynamic_f("sync-dependency-test")
			amdgpu_sync_dependency_test(device, false);
		}
	}

	igt_describe("Check-GFX-CS-for-every-available-ring-works-for-write-const-fill-and-copy-operation-using-more-than-one-IB-and-shared-IB");
	igt_subtest_with_dynamic("cs-gfx-with-IP-GFX-UMQ") {
		if (enable_test && userq_arr_cap[AMD_IP_GFX]) {
			igt_dynamic_f("cs-gfx-with-umq")
			amdgpu_command_submission_gfx(device, info.hw_ip_version_major < 11, true);
		}
	}

	igt_describe("Check-COMPUTE-CS-for-every-available-ring-works-for-write-const-fill-copy-and-nop-operation");
	igt_subtest_with_dynamic("cs-compute-with-IP-COMPUTE-UMQ") {
		if (enable_test && userq_arr_cap[AMD_IP_COMPUTE]) {
			igt_dynamic_f("cs-compute-with-umq")
			amdgpu_command_submission_compute(device, true);
		}
	}

	igt_describe("Check-sync-dependency-using-GFX-ring");
	igt_subtest_with_dynamic("sync-dependency-test-with-IP-GFX-UMQ") {
		if (enable_test && userq_arr_cap[AMD_IP_GFX]) {
			igt_dynamic_f("sync-dependency-test-with-umq")
			amdgpu_sync_dependency_test(device, true);
		}
	}

	igt_describe("Check-DMA-CS-for-every-available-ring-works-for-write-const-fill-copy-operation");
	igt_subtest_with_dynamic("cs-sdma-with-IP-DMA-UMQ") {
		if (enable_test && userq_arr_cap[AMD_IP_DMA]) {
			igt_dynamic_f("cs-sdma-with-umq")
			amdgpu_command_submission_sdma(device, true);
		}
	}

	igt_describe("Check-all-user-queues-for-write-operation");
	igt_subtest_with_dynamic("all-queues-test-with-UMQ") {
		if (enable_test && userq_arr_cap[AMD_IP_GFX] &&
				userq_arr_cap[AMD_IP_COMPUTE] &&
				userq_arr_cap[AMD_IP_DMA]) {
			igt_dynamic_f("all-queues-with-umq")
			amdgpu_test_all_queues(device, true);
		}
	}

	igt_describe("Check-GFX-CS-FENCE_WAIT_MULTI-packet");
	igt_subtest_with_dynamic("cs-gfx-fwm-UMQ") {
		if (enable_test && userq_arr_cap[AMD_IP_GFX]) {
			igt_dynamic_f("cs-gfx-fwm-umq")
				amdgpu_fwm_test(device, AMDGPU_HW_IP_GFX);
		}
	}

	igt_describe("Check-COMPUTE-CS-FENCE_WAIT_MULTI-packet");
	igt_subtest_with_dynamic("cs-compute-fwm-UMQ") {
		if (enable_test && userq_arr_cap[AMD_IP_COMPUTE]) {
			igt_dynamic_f("cs-compute-fwm-umq")
				amdgpu_fwm_test(device, AMDGPU_HW_IP_COMPUTE);
		}
	}

	igt_fixture() {
		amdgpu_device_deinitialize(device);
		drm_close_driver(fd);
	}
}
