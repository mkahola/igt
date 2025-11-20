// SPDX-License-Identifier: MIT
// Copyright 2023 Advanced Micro Devices, Inc.
// Copyright 2014 Advanced Micro Devices, Inc.

#include "lib/amdgpu/amd_mmd_shared.h"

static void
uvd_cmd(uint32_t family_id, uint64_t addr, uint32_t cmd, uint32_t *idx,
		uint32_t *ib_cpu)
{
	ib_cpu[(*idx)++] = (family_id < AMDGPU_FAMILY_AI) ?
			UVD_4_0_GPCOM_VCPU_DATA0 : VEGA_20_GPCOM_VCPU_DATA0;
	ib_cpu[(*idx)++] = addr;
	ib_cpu[(*idx)++] = (family_id < AMDGPU_FAMILY_AI) ?
			UVD_4_0_GPCOM_VCPU_DATA1 : VEGA_20_GPCOM_VCPU_DATA1;
	ib_cpu[(*idx)++] = addr >> 32;
	ib_cpu[(*idx)++] = (family_id < AMDGPU_FAMILY_AI) ?
			UVD_4_0_GPCOM_VCPU_CMD : VEGA_20_GPCOM_VCPU_CMD;
	ib_cpu[(*idx)++] = cmd << 1;
}

static void
amdgpu_uvd_dec_create(amdgpu_device_handle device_handle,
		struct mmd_shared_context *shared_context,
		struct mmd_context *context)
{
	struct amdgpu_bo_alloc_request req = {0};
	amdgpu_bo_handle buf_handle;
	uint64_t va = 0;
	amdgpu_va_handle va_handle;
	void *msg;
	uint32_t i;
	int r;

	req.alloc_size = 4*1024;
	req.preferred_heap = AMDGPU_GEM_DOMAIN_GTT;

	r = amdgpu_bo_alloc(device_handle, &req, &buf_handle);
	igt_assert_eq(r, 0);

	r = amdgpu_va_range_alloc(device_handle,
				  amdgpu_gpu_va_range_general,
				  req.alloc_size, 1, 0, &va,
				  &va_handle, 0);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_va_op(buf_handle, 0, req.alloc_size, va, 0, AMDGPU_VA_OP_MAP);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_cpu_map(buf_handle, &msg);
	igt_assert_eq(r, 0);

	memcpy(msg, uvd_create_msg, sizeof(uvd_create_msg));

	if (shared_context->family_id >= AMDGPU_FAMILY_VI) {
		((uint8_t *)msg)[0x10] = 7;
		if (amdgpu_is_vega_or_polaris(shared_context->family_id, shared_context->chip_id,
				shared_context->chip_rev)) {
			/* dpb size */
			((uint8_t *)msg)[0x28] = 0x00;
			((uint8_t *)msg)[0x29] = 0x94;
			((uint8_t *)msg)[0x2A] = 0x6B;
			((uint8_t *)msg)[0x2B] = 0x00;
		}
	}

	r = amdgpu_bo_cpu_unmap(buf_handle);
	igt_assert_eq(r, 0);

	context->num_resources = 0;
	context->resources[context->num_resources++] = buf_handle;
	context->resources[context->num_resources++] = context->ib_handle;

	i = 0;
	uvd_cmd(shared_context->family_id, va, 0x0, &i, context->ib_cpu);

	for (; i % 16; ++i)
		context->ib_cpu[i] = 0x80000000;

	r = submit(device_handle, context, i, AMDGPU_HW_IP_UVD);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_va_op(buf_handle, 0, req.alloc_size, va, 0, AMDGPU_VA_OP_UNMAP);
	igt_assert_eq(r, 0);

	r = amdgpu_va_range_free(va_handle);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_free(buf_handle);
	igt_assert_eq(r, 0);
}

static void
amdgpu_uvd_decode(amdgpu_device_handle device_handle,
		struct mmd_context *context, struct mmd_shared_context *shared_context)
{
	const unsigned int dpb_size = 15923584, dt_size = 737280;
	uint64_t msg_addr, fb_addr, bs_addr, dpb_addr, ctx_addr, dt_addr, it_addr;
	struct amdgpu_bo_alloc_request req = {0};
	amdgpu_bo_handle buf_handle;
	amdgpu_va_handle va_handle;
	uint64_t va = 0;
	uint64_t sum;
	uint8_t *ptr;
	uint32_t i;
	int r;

	req.alloc_size = 4 * 1024; /* msg */
	req.alloc_size += 4 * 1024; /* fb */
	if (shared_context->family_id >= AMDGPU_FAMILY_VI)
		req.alloc_size += req.alloc_size; /*it_scaling_table*/
	req.alloc_size += ALIGN(sizeof(uvd_bitstream), 4 * 1024);
	req.alloc_size += ALIGN(dpb_size, 4*1024);
	req.alloc_size += ALIGN(dt_size, 4*1024);

	req.preferred_heap = AMDGPU_GEM_DOMAIN_GTT;

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

	r = amdgpu_bo_cpu_map(buf_handle, (void **)&ptr);
	igt_assert_eq(r, 0);

	memcpy(ptr, uvd_decode_msg, sizeof(uvd_decode_msg));
	memcpy(ptr + sizeof(uvd_decode_msg), avc_decode_msg, sizeof(avc_decode_msg));

	if (shared_context->family_id >= AMDGPU_FAMILY_VI) {
		ptr[0x10] = 7;
		ptr[0x98] = 0x00;
		ptr[0x99] = 0x02;
		if (amdgpu_is_vega_or_polaris(shared_context->family_id, shared_context->chip_id,
				shared_context->chip_rev)) {
			/* dpb size */
			ptr[0x24] = 0x00;
			ptr[0x25] = 0x94;
			ptr[0x26] = 0x6B;
			ptr[0x27] = 0x00;
			/*ctx size */
			ptr[0x2C] = 0x00;
			ptr[0x2D] = 0xAF;
			ptr[0x2E] = 0x50;
			ptr[0x2F] = 0x00;
		}
	}

	ptr += 4 * 1024;
	memset(ptr, 0, 4 * 1024);
	if (shared_context->family_id >= AMDGPU_FAMILY_VI) {
		ptr += 4 * 1024;
		memcpy(ptr, uvd_it_scaling_table, sizeof(uvd_it_scaling_table));
	}

	ptr += 4 * 1024;
	memcpy(ptr, uvd_bitstream, sizeof(uvd_bitstream));

	ptr += ALIGN(sizeof(uvd_bitstream), 4*1024);
	memset(ptr, 0, dpb_size);

	ptr += ALIGN(dpb_size, 4 * 1024);
	memset(ptr, 0, dt_size);

	context->num_resources = 0;
	context->resources[context->num_resources++] = buf_handle;
	context->resources[context->num_resources++] = context->ib_handle;

	msg_addr = va;
	fb_addr = msg_addr + 4 * 1024;
	if (shared_context->family_id >= AMDGPU_FAMILY_VI) {
		it_addr = fb_addr + 4 * 1024;
		bs_addr = it_addr + 4 * 1024;
	} else
		bs_addr = fb_addr + 4 * 1024;
	dpb_addr = ALIGN(bs_addr + sizeof(uvd_bitstream), 4 * 1024);

	ctx_addr = 0;
	if (shared_context->family_id >= AMDGPU_FAMILY_VI) {
		if (amdgpu_is_vega_or_polaris(shared_context->family_id, shared_context->chip_id,
				shared_context->chip_rev)) {
			ctx_addr = ALIGN(dpb_addr + 0x006B9400, 4 * 1024);
		}
	}

	dt_addr = ALIGN(dpb_addr + dpb_size, 4 * 1024);

	i = 0;
	uvd_cmd(shared_context->family_id, msg_addr, 0x0, &i, context->ib_cpu);
	uvd_cmd(shared_context->family_id, dpb_addr, 0x1, &i, context->ib_cpu);
	uvd_cmd(shared_context->family_id, dt_addr, 0x2, &i, context->ib_cpu);
	uvd_cmd(shared_context->family_id, fb_addr, 0x3, &i, context->ib_cpu);
	uvd_cmd(shared_context->family_id, bs_addr, 0x100, &i, context->ib_cpu);

	if (shared_context->family_id >= AMDGPU_FAMILY_VI) {
		uvd_cmd(shared_context->family_id, it_addr, 0x204, &i, context->ib_cpu);
		if (amdgpu_is_vega_or_polaris(shared_context->family_id, shared_context->chip_id,
				shared_context->chip_rev)) {
			uvd_cmd(shared_context->family_id, ctx_addr, 0x206, &i, context->ib_cpu);
		}
	}

	context->ib_cpu[i++] = (shared_context->family_id < AMDGPU_FAMILY_AI) ?
			UVD_4_0__ENGINE_CNTL : VEGA_20_UVD_ENGINE_CNTL;
	context->ib_cpu[i++] = 0x1;
	for (; i % 16; ++i)
		context->ib_cpu[i] = 0x80000000;

	r = submit(device_handle, context, i, AMDGPU_HW_IP_UVD);
	igt_assert_eq(r, 0);

	/* TODO: use a real CRC32 */
	for (i = 0, sum = 0; i < dt_size; ++i)
		sum += ptr[i];
	igt_assert_eq(sum, SUM_DECODE);

	r = amdgpu_bo_cpu_unmap(buf_handle);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_va_op(buf_handle, 0, req.alloc_size, va, 0, AMDGPU_VA_OP_UNMAP);
	igt_assert_eq(r, 0);

	r = amdgpu_va_range_free(va_handle);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_free(buf_handle);
	igt_assert_eq(r, 0);
}

static void
amdgpu_uvd_dec_destroy(amdgpu_device_handle device_handle, struct mmd_context *context,
		 struct mmd_shared_context *shared_context)
{
	struct amdgpu_bo_alloc_request req = {0};
	amdgpu_bo_handle buf_handle;
	amdgpu_va_handle va_handle;
	uint64_t va = 0;
	void *msg;
	uint32_t i;
	int r;

	req.alloc_size = 4 * 1024;
	req.preferred_heap = AMDGPU_GEM_DOMAIN_GTT;

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

	r = amdgpu_bo_cpu_map(buf_handle, &msg);
	igt_assert_eq(r, 0);

	memcpy(msg, uvd_destroy_msg, sizeof(uvd_destroy_msg));
	if (shared_context->family_id >= AMDGPU_FAMILY_VI)
		((uint8_t *)msg)[0x10] = 7;

	r = amdgpu_bo_cpu_unmap(buf_handle);
	igt_assert_eq(r, 0);

	context->num_resources = 0;
	context->resources[context->num_resources++] = buf_handle;
	context->resources[context->num_resources++] = context->ib_handle;

	i = 0;
	uvd_cmd(shared_context->family_id, va, 0x0, &i, context->ib_cpu);
	for (; i % 16; ++i)
		context->ib_cpu[i] = 0x80000000;

	r = submit(device_handle, context, i, AMDGPU_HW_IP_UVD);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_va_op(buf_handle, 0, req.alloc_size, va, 0, AMDGPU_VA_OP_UNMAP);
	igt_assert_eq(r, 0);

	r = amdgpu_va_range_free(va_handle);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_free(buf_handle);
	igt_assert_eq(r, 0);
}

igt_main
{
	amdgpu_device_handle device;
	struct mmd_context context = {};
	struct mmd_shared_context shared_context = {};
	int fd = -1;

	igt_fixture() {
		uint32_t major, minor;
		int err;

		fd = drm_open_driver(DRIVER_AMDGPU);
		err = amdgpu_device_initialize(fd, &major, &minor, &device);
		igt_require(err == 0);
		igt_info("Initialized amdgpu, driver version %d.%d\n",
			 major, minor);
		err = mmd_shared_context_init(device, &shared_context);
		igt_require(err == 0);
		err = mmd_context_init(device, &context);
		igt_require(err == 0);
		igt_skip_on(!is_uvd_tests_enable(shared_context.family_id, shared_context.chip_id,
				shared_context.chip_rev));
	}
	igt_describe("Test whether uvd dec is created");
	igt_subtest("amdgpu_uvd_dec_create")
	amdgpu_uvd_dec_create(device, &shared_context, &context);

	igt_describe("Test whether uvd dec can decode");
	igt_subtest("amdgpu_uvd_decode")
	amdgpu_uvd_decode(device, &context, &shared_context);

	igt_describe("Test whether uvd dec is destroyed");
	igt_subtest("amdgpu_uvd_dec_destroy")
	amdgpu_uvd_dec_destroy(device, &context, &shared_context);

	igt_fixture() {
		mmd_context_clean(device, &context);
		amdgpu_device_deinitialize(device);
		drm_close_driver(fd);
	}
}
