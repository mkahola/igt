// SPDX-License-Identifier: MIT
/* Copyright 2023 Advanced Micro Devices, Inc.
 * Copyright 2014 Advanced Micro Devices, Inc.
 */
#include "amd_mmd_shared.h"

bool
is_gfx_pipe_removed(uint32_t family_id, uint32_t chip_id, uint32_t chip_rev)
{

	if (family_id != AMDGPU_FAMILY_AI)
		return false;

	switch (chip_id - chip_rev) {
	/* Arcturus */
	case 0x32:
	/* Aldebaran */
	case 0x3c:
		return true;
	default:
		return false;
	}
}

bool
is_uvd_tests_enable(uint32_t family_id, uint32_t chip_id, uint32_t chip_rev)
{

	if (family_id >= AMDGPU_FAMILY_RV || family_id == AMDGPU_FAMILY_SI ||
			is_gfx_pipe_removed(family_id, chip_id, chip_rev)) {
		igt_info("\n\nThe ASIC NOT support UVD, test skipped\n");
		return false;
	}

	return true;
}

bool
amdgpu_is_vega_or_polaris(uint32_t family_id, uint32_t chip_id, uint32_t chip_rev)
{
	if ((family_id == AMDGPU_FAMILY_AI) ||
		(chip_id == chip_rev + 0x50 || chip_id == chip_rev + 0x5A ||
		chip_id == chip_rev + 0x64)) {
		return true;
	}
	return false;

}

int
mmd_context_init(amdgpu_device_handle device_handle, struct mmd_context *context)
{
	int r;

	r = amdgpu_cs_ctx_create(device_handle, &context->context_handle);
	igt_assert_eq(r, 0);
	r = amdgpu_bo_alloc_and_map(device_handle, IB_SIZE, IB_SIZE,
				    AMDGPU_GEM_DOMAIN_GTT, 0,
				    &context->ib_handle, (void **)&context->ib_cpu,
				    &context->ib_mc_address,
				    &context->ib_va_handle);

	return r;
}

void
mmd_context_clean(amdgpu_device_handle device_handle,
		struct mmd_context *context)
{

	amdgpu_bo_unmap_and_free(context->ib_handle, context->ib_va_handle,
			context->ib_mc_address, IB_SIZE);

	amdgpu_cs_ctx_free(context->context_handle);

}

int
mmd_shared_context_init(amdgpu_device_handle device_handle, struct mmd_shared_context *context)
{
	int r;
	struct amdgpu_gpu_info gpu_info = {0};

	r = amdgpu_query_gpu_info(device_handle, &gpu_info);
	igt_assert_eq(r, 0);

	context->family_id = gpu_info.family_id;
	context->chip_id = gpu_info.chip_external_rev;
	context->chip_rev = gpu_info.chip_rev;
	context->asic_id = gpu_info.asic_id;

	/*vce*/
	context->vce_harvest_config = gpu_info.vce_harvest_config;

	return r;
}

void
alloc_resource(amdgpu_device_handle device_handle,
		struct amdgpu_mmd_bo *mmd_bo, unsigned int size,
		unsigned int domain)
{
	struct amdgpu_bo_alloc_request req = {0};
	amdgpu_bo_handle buf_handle;
	amdgpu_va_handle va_handle;
	uint64_t va = 0;
	int r;

	req.alloc_size = ALIGN(size, IB_SIZE);
	req.preferred_heap = domain;
	r = amdgpu_bo_alloc(device_handle, &req, &buf_handle);
	igt_assert_eq(r, 0);

	r = amdgpu_va_range_alloc(device_handle,
				  amdgpu_gpu_va_range_general,
				  req.alloc_size, 1, 0, &va,
				  &va_handle, 0);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_va_op(buf_handle, 0, req.alloc_size, va, 0,
			    AMDGPU_VA_OP_MAP);

	igt_assert_eq(r, 0);
	mmd_bo->addr = va;
	mmd_bo->handle = buf_handle;
	mmd_bo->size = req.alloc_size;
	mmd_bo->va_handle = va_handle;

	r = amdgpu_bo_cpu_map(mmd_bo->handle, (void **)&mmd_bo->ptr);
	igt_assert_eq(r, 0);

	memset(mmd_bo->ptr, 0, size);
	r = amdgpu_bo_cpu_unmap(mmd_bo->handle);
	igt_assert_eq(r, 0);
}

void
free_resource(struct amdgpu_mmd_bo *mmd_bo)
{
	int r;

	r = amdgpu_bo_va_op(mmd_bo->handle, 0, mmd_bo->size,
			mmd_bo->addr, 0, AMDGPU_VA_OP_UNMAP);
	igt_assert_eq(r, 0);

	r = amdgpu_va_range_free(mmd_bo->va_handle);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_free(mmd_bo->handle);
	igt_assert_eq(r, 0);
	memset(mmd_bo, 0, sizeof(*mmd_bo));
}

int
submit(amdgpu_device_handle device_handle, struct mmd_context *context,
		unsigned int ndw, unsigned int ip)
{
	struct amdgpu_cs_request ibs_request = {0};
	struct amdgpu_cs_ib_info ib_info = {0};
	struct amdgpu_cs_fence fence_status = {0};
	uint32_t expired;
	int r;
	
	unsigned int padded_ndw = ndw;

	/*
	* VCN JPEG (VCN 4.0.5+) requires IB size aligned to 16 DW.
	* We pad using a 2-DW NOP packet, so the initial ndw must be even.
	*/
	if (ip == AMDGPU_HW_IP_VCN_JPEG) {
        /* IB capacity in DW (IB_SIZE is in bytes, divide by 4) */
		unsigned int ib_dw_capacity = IB_SIZE / 4;
		igt_assert_eq(padded_ndw & 1, 0);

        while (padded_ndw & 0xF) {
			igt_assert(padded_ndw + 2 <= ib_dw_capacity);

            /* 2-DW NOP packet */
            context->ib_cpu[padded_ndw++] = 0x60000000;
            context->ib_cpu[padded_ndw++] = 0x00000000;
        }
	}

	ib_info.ib_mc_address = context->ib_mc_address;
	ib_info.size = padded_ndw;

	ibs_request.ip_type = ip;

	r = amdgpu_bo_list_create(device_handle, context->num_resources,
			context->resources, NULL, &ibs_request.resources);
	igt_assert_eq(r, 0);

	ibs_request.number_of_ibs = 1;
	ibs_request.ibs = &ib_info;
	ibs_request.fence_info.handle = NULL;

	r = amdgpu_cs_submit(context->context_handle, 0, &ibs_request, 1);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_list_destroy(ibs_request.resources);
	igt_assert_eq(r, 0);

	fence_status.context = context->context_handle;
	fence_status.ip_type = ip;
	fence_status.fence = ibs_request.seq_no;

	r = amdgpu_cs_query_fence_status(&fence_status,
					 AMDGPU_TIMEOUT_INFINITE,
					 0, &expired);
	return r;
}

int
mm_queue_test_helper(amdgpu_device_handle device_handle, struct mmd_shared_context *context,
		mm_test_callback callback, int err_type, const struct pci_addr *pci)
{
	int r;
	char cmd[1024];
	long sched_mask = 0;
	long mask = 0;
	uint32_t ring_id;
	char sysfs[256];

	if (!callback)
		return -1;

	if (is_spx_mode(pci)) {
		sched_mask = amdgpu_get_ip_schedule_mask(pci, (enum amd_ip_block_type)context->ip_type, sysfs);
	} else {
		sched_mask = 1;
	}

	mask = sched_mask;
	for (ring_id = 0;  mask > 0; ring_id++) {
		/* check sched is ready is on the ring. */
		if (sched_mask > 1) {
			igt_info(" Testing on queue %d\n", ring_id);
			snprintf(cmd, sizeof(cmd) - 1, "sudo echo  0x%x > %s", 0x1 << ring_id, sysfs);
			r = system(cmd);
			igt_assert_eq(r, 0);
		}

		if (callback(device_handle, context, err_type))
			break;

		mask = mask >> 1;
	}

	/* recover the sched mask */
	if (sched_mask > 1) {
		snprintf(cmd, sizeof(cmd) - 1, "sudo echo  0x%lx > %s", sched_mask, sysfs);
		r = system(cmd);
		igt_assert_eq(r, 0);
	}
	return r;
}
