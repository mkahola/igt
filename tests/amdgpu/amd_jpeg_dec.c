// SPDX-License-Identifier: MIT
// Copyright 2023 Advanced Micro Devices, Inc.
// Copyright 2017 Advanced Micro Devices, Inc.

#include "lib/amdgpu/amd_jpeg_shared.h"

/* send a bitstream buffer command */
static void
send_cmd_bitstream(struct mmd_context *context, uint64_t addr, uint32_t *idx)
{
	/* jpeg soft reset */
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_JPEG_CNTL), COND0, TYPE0, 1, idx);

	/* ensuring the Reset is asserted in SCLK domain */
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_CTX_INDEX), COND0, TYPE0,
			0x01C2, idx);
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_CTX_DATA), COND0, TYPE0,
			0x01400200, idx);
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_CTX_INDEX), COND0, TYPE0,
			0x01C3, idx);
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_CTX_DATA), COND0, TYPE0,
			(1 << 9), idx);
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_SOFT_RESET), COND0, TYPE3,
			(1 << 9), idx);

	/* wait mem */
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_JPEG_CNTL), COND0, TYPE0,
			0, idx);

	/* ensuring the Reset is de-asserted in SCLK domain */
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_CTX_INDEX), COND0, TYPE0,
			0x01C3, idx);
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_CTX_DATA), COND0, TYPE0,
			(0 << 9), idx);
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_SOFT_RESET), COND0, TYPE3,
			(1 << 9), idx);

	/* set UVD_LMI_JPEG_READ_64BIT_BAR_LOW/HIGH based on bitstream buffer address */
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_LMI_JPEG_READ_64BIT_BAR_HIGH),
			COND0, TYPE0, (addr >> 32), idx);
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_LMI_JPEG_READ_64BIT_BAR_LOW),
			COND0, TYPE0, (uint32_t)addr, idx);

	/* set jpeg_rb_base */
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_JPEG_RB_BASE), COND0, TYPE0,
			0, idx);

	/* set jpeg_rb_base */
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_JPEG_RB_SIZE), COND0, TYPE0,
			0xFFFFFFF0, idx);

	/* set jpeg_rb_wptr */
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_JPEG_RB_WPTR), COND0, TYPE0,
			(JPEG_DEC_BSD_SIZE >> 2), idx);
}

/* send a target buffer command */
static void
send_cmd_target(struct mmd_context *context, uint64_t addr,
		uint32_t *idx)
{

	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_JPEG_PITCH), COND0, TYPE0,
				(JPEG_DEC_DT_PITCH >> 4), idx);
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_JPEG_UV_PITCH), COND0, TYPE0,
				(JPEG_DEC_DT_PITCH >> 4), idx);

	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_JPEG_TILING_CTRL), COND0, TYPE0,
			0, idx);
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_JPEG_UV_TILING_CTRL), COND0, TYPE0,
			0, idx);

	/* set UVD_LMI_JPEG_WRITE_64BIT_BAR_LOW/HIGH based on target buffer address */
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_LMI_JPEG_WRITE_64BIT_BAR_HIGH),
			COND0, TYPE0, (addr >> 32), idx);
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_LMI_JPEG_WRITE_64BIT_BAR_LOW),
			COND0, TYPE0, (uint32_t)addr, idx);

	/* set output buffer data address */
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_JPEG_INDEX), COND0, TYPE0, 0, idx);
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_JPEG_DATA), COND0, TYPE0,
			JPEG_DEC_LUMA_OFFSET, idx);
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_JPEG_INDEX), COND0, TYPE0, 1, idx);
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_JPEG_DATA), COND0, TYPE0,
			JPEG_DEC_CHROMA_OFFSET, idx);
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_JPEG_TIER_CNTL2), COND0, TYPE3,
			0, idx);

	/* set output buffer read pointer */
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_JPEG_OUTBUF_RPTR), COND0, TYPE0,
			0, idx);

	/* enable error interrupts */
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_JPEG_INT_EN), COND0, TYPE0,
			0xFFFFFFFE, idx);

	/* start engine command */
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_JPEG_CNTL), COND0, TYPE0,
			0x6, idx);

	/* wait for job completion, wait for job JBSI fetch done */
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_CTX_INDEX), COND0, TYPE0,
			0x01C3, idx);
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_CTX_DATA), COND0, TYPE0,
			(JPEG_DEC_BSD_SIZE >> 2), idx);
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_CTX_INDEX), COND0, TYPE0,
			0x01C2, idx);
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_CTX_DATA), COND0, TYPE0,
			0x01400200, idx);
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_JPEG_RB_RPTR), COND0, TYPE3,
			0xFFFFFFFF, idx);

	/* wait for job jpeg outbuf idle */
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_CTX_INDEX), COND0, TYPE0,
			0x01C3, idx);
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_CTX_DATA), COND0, TYPE0,
			0xFFFFFFFF, idx);
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_JPEG_OUTBUF_WPTR), COND0, TYPE3,
			0x00000001, idx);

	/* stop engine */
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_JPEG_CNTL), COND0, TYPE0,
			0x4, idx);

	/* asserting jpeg lmi drop */
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_CTX_INDEX), COND0, TYPE0,
			0x0005, idx);
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_CTX_DATA), COND0, TYPE0,
			(1 << 23 | 1 << 0), idx);
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_CTX_DATA), COND0, TYPE1, 0, idx);
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_CTX_DATA), COND0, TYPE0, 0, idx);

	/* asserting jpeg reset */
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_JPEG_CNTL), COND0, TYPE0, 1, idx);

	/* ensure reset is asserted in sclk domain */
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_CTX_INDEX), COND0, TYPE0, 0x01C3,
			idx);
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_CTX_DATA), COND0, TYPE0, (1 << 9),
			idx);
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_SOFT_RESET), COND0, TYPE3, (1 << 9),
			idx);

	/* de-assert jpeg reset */
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_JPEG_CNTL), COND0, TYPE0, 0, idx);

	/* ensure reset is de-asserted in sclk domain */
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_CTX_INDEX), COND0, TYPE0, 0x01C3,
			idx);
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_CTX_DATA), COND0, TYPE0, (0 << 9),
			idx);
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_SOFT_RESET), COND0, TYPE3, (1 << 9),
			idx);

	/* de-asserting jpeg lmi drop */
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_CTX_INDEX), COND0, TYPE0, 0x0005,
			idx);
	set_reg_jpeg(context, SOC15_REG_ADDR(mmUVD_CTX_DATA), COND0, TYPE0, 0, idx);
}

static void
amdgpu_cs_jpeg_decode(amdgpu_device_handle device_handle,
		struct mmd_shared_context *shared_context)
{

	struct amdgpu_mmd_bo dec_buf;
	int size, r;
	uint8_t *dec;
	int sum = 0, i, j;
	uint32_t idx;
	struct mmd_context acontext = {0};
	struct mmd_context *context = &acontext;

	r = mmd_context_init(device_handle, context);
	igt_assert_eq(r, 0);
	size = 32 * 1024; /* 8K bitstream + 24K output */

	context->num_resources = 0;
	alloc_resource(device_handle, &dec_buf, size, AMDGPU_GEM_DOMAIN_VRAM);
	context->resources[context->num_resources++] = dec_buf.handle;
	context->resources[context->num_resources++] = context->ib_handle;
	r = amdgpu_bo_cpu_map(dec_buf.handle, (void **)&dec_buf.ptr);
	igt_assert_eq(r, 0);
	memcpy(dec_buf.ptr, jpeg_bitstream, sizeof(jpeg_bitstream));

	idx = 0;

	if (shared_context->jpeg_direct_reg == true) {
		send_cmd_bitstream_direct(context, dec_buf.addr, &idx);
		send_cmd_target_direct(context, dec_buf.addr + (size / 4), &idx);
	} else {
		send_cmd_bitstream(context, dec_buf.addr, &idx);
		send_cmd_target(context, dec_buf.addr + (size / 4), &idx);
	}

	/*
	 * VCN JPEG (VCN 4.0.5+) requires IB size aligned to 16 DW.
	 * We pad using a 2-DW NOP packet, so the initial ndw must be even.
	 */
	/* IB capacity in DW (IB_SIZE is in bytes, divide by 4) */
	unsigned int ib_dw_capacity = IB_SIZE / 4;
	igt_assert_eq(idx & 1, 0);

	while (idx & 0xF) {
		igt_assert(idx + 2 <= ib_dw_capacity);

		/* 2-DW NOP packet for VCN JPEG */
		context->ib_cpu[idx++] = 0x60000000;
		context->ib_cpu[idx++] = 0x00000000;
	}

	amdgpu_bo_cpu_unmap(dec_buf.handle);
	r = submit(device_handle, context, idx, AMDGPU_HW_IP_VCN_JPEG);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_cpu_map(dec_buf.handle, (void **)&dec_buf.ptr);
	igt_assert_eq(r, 0);

	dec = dec_buf.ptr + (size / 4);

	/* calculate result checksum */
	for (i = 0; i < WIDTH; i++)
		for (j = 0; j < WIDTH; j++)
			sum += *((dec + JPEG_DEC_LUMA_OFFSET + i * JPEG_DEC_DT_PITCH) + j);
	for (i = 0; i < (WIDTH/2); i++)
		for (j = 0; j < WIDTH; j++)
			sum += *((dec + JPEG_DEC_CHROMA_OFFSET + i * JPEG_DEC_DT_PITCH) + j);

	amdgpu_bo_cpu_unmap(dec_buf.handle);
	igt_assert_eq(sum, JPEG_DEC_SUM);

	free_resource(&dec_buf);
	mmd_context_clean(device_handle, context);
}

int igt_main()
{
	amdgpu_device_handle device;
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
		igt_skip_on(!is_jpeg_tests_enable(device, &shared_context));
	}
	igt_describe("Test whether jpeg dec decodes");
	igt_subtest("amdgpu_cs_jpeg_decode")
	amdgpu_cs_jpeg_decode(device, &shared_context);

	igt_fixture() {
		amdgpu_device_deinitialize(device);
		drm_close_driver(fd);
	}

}
