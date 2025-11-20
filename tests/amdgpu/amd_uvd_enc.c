// SPDX-License-Identifier: MIT
// Copyright 2023 Advanced Micro Devices, Inc.
// Copyright 2017 Advanced Micro Devices, Inc.

#include "lib/amdgpu/amd_mmd_shared.h"

static bool
is_uvd_enc_enable(amdgpu_device_handle device_handle)
{
	int r;
	struct drm_amdgpu_info_hw_ip info = {};

	r = amdgpu_query_hw_ip_info(device_handle, AMDGPU_HW_IP_UVD_ENC, 0, &info);

	if (!info.available_rings)
		igt_info("\n\nThe ASIC NOT support UVD ENC, test skipped\n");

	return (r == 0 && (info.available_rings ? true : false));
}

static void
amdgpu_uvd_enc_create(amdgpu_device_handle device_handle,
					struct uvd_enc_context *context)
{
	context->enc.width = 160;
	context->enc.height = 128;

	context->uvd.num_resources  = 0;
	alloc_resource(device_handle, &context->enc.session,
			128 * 1024, AMDGPU_GEM_DOMAIN_GTT);
	context->uvd.resources[context->uvd.num_resources++] = context->enc.session.handle;
	context->uvd.resources[context->uvd.num_resources++] = context->uvd.ib_handle;
}

static void
check_result(struct amdgpu_uvd_enc *enc)
{
	uint64_t sum;
	uint32_t s = 175602;
	uint32_t *ptr, size;
	int j, r;

	r = amdgpu_bo_cpu_map(enc->fb.handle, (void **)&enc->fb.ptr);
	igt_assert_eq(r, 0);
	ptr = (uint32_t *)enc->fb.ptr;
	size = ptr[6];
	r = amdgpu_bo_cpu_unmap(enc->fb.handle);
	igt_assert_eq(r, 0);
	r = amdgpu_bo_cpu_map(enc->bs.handle, (void **)&enc->bs.ptr);
	igt_assert_eq(r, 0);
	for (j = 0, sum = 0; j < size; ++j)
		sum += enc->bs.ptr[j];
	igt_assert_eq(sum, s);
	r = amdgpu_bo_cpu_unmap(enc->bs.handle);
	igt_assert_eq(r, 0);

}

static void
amdgpu_uvd_enc_session_init(amdgpu_device_handle device_handle,
		struct uvd_enc_context *context)
{
	int len, r;

	len = 0;
	memcpy((context->uvd.ib_cpu + len), uve_session_info, sizeof(uve_session_info));
	len += sizeof(uve_session_info) / 4;
	context->uvd.ib_cpu[len++] = context->enc.session.addr >> 32;
	context->uvd.ib_cpu[len++] = context->enc.session.addr;

	memcpy((context->uvd.ib_cpu + len), uve_task_info, sizeof(uve_task_info));
	len += sizeof(uve_task_info) / 4;
	context->uvd.ib_cpu[len++] = 0x000000d8;
	context->uvd.ib_cpu[len++] = 0x00000000;
	context->uvd.ib_cpu[len++] = 0x00000000;

	memcpy((context->uvd.ib_cpu + len), uve_op_init, sizeof(uve_op_init));
	len += sizeof(uve_op_init) / 4;

	memcpy((context->uvd.ib_cpu + len), uve_session_init, sizeof(uve_session_init));
	len += sizeof(uve_session_init) / 4;

	memcpy((context->uvd.ib_cpu + len), uve_layer_ctrl, sizeof(uve_layer_ctrl));
	len += sizeof(uve_layer_ctrl) / 4;

	memcpy((context->uvd.ib_cpu + len), uve_slice_ctrl, sizeof(uve_slice_ctrl));
	len += sizeof(uve_slice_ctrl) / 4;

	memcpy((context->uvd.ib_cpu + len), uve_spec_misc, sizeof(uve_spec_misc));
	len += sizeof(uve_spec_misc) / 4;

	memcpy((context->uvd.ib_cpu + len), uve_rc_session_init,
			sizeof(uve_rc_session_init));
	len += sizeof(uve_rc_session_init) / 4;

	memcpy((context->uvd.ib_cpu + len), uve_deblocking_filter,
			sizeof(uve_deblocking_filter));
	len += sizeof(uve_deblocking_filter) / 4;

	memcpy((context->uvd.ib_cpu + len), uve_quality_params,
			sizeof(uve_quality_params));
	len += sizeof(uve_quality_params) / 4;

	memcpy((context->uvd.ib_cpu + len), uve_op_init_rc, sizeof(uve_op_init_rc));
	len += sizeof(uve_op_init_rc) / 4;

	memcpy((context->uvd.ib_cpu + len), uve_op_init_rc_vbv_level,
			sizeof(uve_op_init_rc_vbv_level));
	len += sizeof(uve_op_init_rc_vbv_level) / 4;

	r = submit(device_handle, &context->uvd, len, AMDGPU_HW_IP_UVD_ENC);
	igt_assert_eq(r, 0);
}

static void
amdgpu_uvd_enc_encode(amdgpu_device_handle device_handle,
		struct uvd_enc_context *context, struct mmd_shared_context *shared_context)
{
	int len, r, i;
	uint64_t luma_offset, chroma_offset;
	unsigned int luma_size;
	uint32_t vbuf_size, bs_size = 0x003f4800, cpb_size;
	unsigned int align = (shared_context->family_id >= AMDGPU_FAMILY_AI) ? 256 : 16;

	vbuf_size = ALIGN(context->enc.width, align) *
			ALIGN(context->enc.height, 16) * 1.5;
	cpb_size = vbuf_size * 10;
	context->uvd.num_resources  = 0;
	alloc_resource(device_handle, &context->enc.fb, 4096,
			AMDGPU_GEM_DOMAIN_VRAM);
	context->uvd.resources[context->uvd.num_resources++] = context->enc.fb.handle;
	alloc_resource(device_handle, &context->enc.bs, bs_size,
			AMDGPU_GEM_DOMAIN_VRAM);
	context->uvd.resources[context->uvd.num_resources++] = context->enc.bs.handle;
	alloc_resource(device_handle, &context->enc.vbuf, vbuf_size,
			AMDGPU_GEM_DOMAIN_VRAM);
	context->uvd.resources[context->uvd.num_resources++] = context->enc.vbuf.handle;
	alloc_resource(device_handle, &context->enc.cpb, cpb_size,
			AMDGPU_GEM_DOMAIN_VRAM);
	context->uvd.resources[context->uvd.num_resources++] = context->enc.cpb.handle;
	context->uvd.resources[context->uvd.num_resources++] = context->uvd.ib_handle;

	r = amdgpu_bo_cpu_map(context->enc.vbuf.handle, (void **)&context->enc.vbuf.ptr);
	igt_assert_eq(r, 0);

	memset(context->enc.vbuf.ptr, 0, vbuf_size);
	for (i = 0; i < context->enc.height; ++i) {
		memcpy(context->enc.vbuf.ptr, (frame + i * context->enc.width),
				context->enc.width);
		context->enc.vbuf.ptr += ALIGN(context->enc.width, align);
	}
	for (i = 0; i < context->enc.height / 2; ++i) {
		memcpy(context->enc.vbuf.ptr, ((frame + context->enc.height *
				context->enc.width) + i * context->enc.width), context->enc.width);
		context->enc.vbuf.ptr += ALIGN(context->enc.width, align);
	}

	r = amdgpu_bo_cpu_unmap(context->enc.vbuf.handle);
	igt_assert_eq(r, 0);

	len = 0;
	memcpy((context->uvd.ib_cpu + len), uve_session_info, sizeof(uve_session_info));
	len += sizeof(uve_session_info) / 4;
	context->uvd.ib_cpu[len++] = context->enc.session.addr >> 32;
	context->uvd.ib_cpu[len++] = context->enc.session.addr;

	memcpy((context->uvd.ib_cpu + len), uve_task_info, sizeof(uve_task_info));
	len += sizeof(uve_task_info) / 4;
	context->uvd.ib_cpu[len++] = 0x000005e0;
	context->uvd.ib_cpu[len++] = 0x00000001;
	context->uvd.ib_cpu[len++] = 0x00000001;

	memcpy((context->uvd.ib_cpu + len), uve_nalu_buffer_1, sizeof(uve_nalu_buffer_1));
	len += sizeof(uve_nalu_buffer_1) / 4;

	memcpy((context->uvd.ib_cpu + len), uve_nalu_buffer_2, sizeof(uve_nalu_buffer_2));
	len += sizeof(uve_nalu_buffer_2) / 4;

	memcpy((context->uvd.ib_cpu + len), uve_nalu_buffer_3, sizeof(uve_nalu_buffer_3));
	len += sizeof(uve_nalu_buffer_3) / 4;

	memcpy((context->uvd.ib_cpu + len), uve_nalu_buffer_4, sizeof(uve_nalu_buffer_4));
	len += sizeof(uve_nalu_buffer_4) / 4;

	memcpy((context->uvd.ib_cpu + len), uve_slice_header, sizeof(uve_slice_header));
	len += sizeof(uve_slice_header) / 4;

	context->uvd.ib_cpu[len++] = 0x00000254;
	context->uvd.ib_cpu[len++] = 0x00000010;
	context->uvd.ib_cpu[len++] = context->enc.cpb.addr >> 32;
	context->uvd.ib_cpu[len++] = context->enc.cpb.addr;
	memcpy((context->uvd.ib_cpu + len), uve_ctx_buffer, sizeof(uve_ctx_buffer));
	len += sizeof(uve_ctx_buffer) / 4;

	memcpy((context->uvd.ib_cpu + len), uve_bitstream_buffer,
			sizeof(uve_bitstream_buffer));
	len += sizeof(uve_bitstream_buffer) / 4;
	context->uvd.ib_cpu[len++] = 0x00000000;
	context->uvd.ib_cpu[len++] = context->enc.bs.addr >> 32;
	context->uvd.ib_cpu[len++] = context->enc.bs.addr;
	context->uvd.ib_cpu[len++] = 0x003f4800;
	context->uvd.ib_cpu[len++] = 0x00000000;

	memcpy((context->uvd.ib_cpu + len), uve_feedback_buffer,
			sizeof(uve_feedback_buffer));
	len += sizeof(uve_feedback_buffer) / 4;
	context->uvd.ib_cpu[len++] = context->enc.fb.addr >> 32;
	context->uvd.ib_cpu[len++] = context->enc.fb.addr;
	context->uvd.ib_cpu[len++] = 0x00000010;
	context->uvd.ib_cpu[len++] = 0x00000028;

	memcpy((context->uvd.ib_cpu + len), uve_feedback_buffer_additional,
			sizeof(uve_feedback_buffer_additional));
	len += sizeof(uve_feedback_buffer_additional) / 4;

	memcpy((context->uvd.ib_cpu + len), uve_intra_refresh, sizeof(uve_intra_refresh));
	len += sizeof(uve_intra_refresh) / 4;

	memcpy((context->uvd.ib_cpu + len), uve_layer_select, sizeof(uve_layer_select));
	len += sizeof(uve_layer_select) / 4;

	memcpy((context->uvd.ib_cpu + len), uve_rc_layer_init, sizeof(uve_rc_layer_init));
	len += sizeof(uve_rc_layer_init) / 4;

	memcpy((context->uvd.ib_cpu + len), uve_layer_select, sizeof(uve_layer_select));
	len += sizeof(uve_layer_select) / 4;

	memcpy((context->uvd.ib_cpu + len), uve_rc_per_pic, sizeof(uve_rc_per_pic));
	len += sizeof(uve_rc_per_pic) / 4;

	luma_size = ALIGN(context->enc.width, align) * ALIGN(context->enc.height, 16);
	luma_offset = context->enc.vbuf.addr;
	chroma_offset = luma_offset + luma_size;
	context->uvd.ib_cpu[len++] = 0x00000054;
	context->uvd.ib_cpu[len++] = 0x0000000c;
	context->uvd.ib_cpu[len++] = 0x00000002;
	context->uvd.ib_cpu[len++] = 0x003f4800;
	context->uvd.ib_cpu[len++] = luma_offset >> 32;
	context->uvd.ib_cpu[len++] = luma_offset;
	context->uvd.ib_cpu[len++] = chroma_offset >> 32;
	context->uvd.ib_cpu[len++] = chroma_offset;
	memcpy((context->uvd.ib_cpu + len), uve_encode_param, sizeof(uve_encode_param));
	context->uvd.ib_cpu[len] = ALIGN(context->enc.width, align);
	context->uvd.ib_cpu[len + 1] = ALIGN(context->enc.width, align);
	len += sizeof(uve_encode_param) / 4;

	memcpy((context->uvd.ib_cpu + len), uve_op_speed_enc_mode,
			sizeof(uve_op_speed_enc_mode));
	len += sizeof(uve_op_speed_enc_mode) / 4;

	memcpy((context->uvd.ib_cpu + len), uve_op_encode, sizeof(uve_op_encode));
	len += sizeof(uve_op_encode) / 4;

	r = submit(device_handle, &context->uvd, len, AMDGPU_HW_IP_UVD_ENC);
	igt_assert_eq(r, 0);

	check_result(&context->enc);

	free_resource(&context->enc.fb);
	free_resource(&context->enc.bs);
	free_resource(&context->enc.vbuf);
	free_resource(&context->enc.cpb);
}

static void
amdgpu_uvd_enc_destroy(amdgpu_device_handle device_handle,
		struct uvd_enc_context *context)
{
	int len, r;

	context->uvd.num_resources  = 0;
	context->uvd.resources[context->uvd.num_resources++] = context->uvd.ib_handle;

	len = 0;
	memcpy((context->uvd.ib_cpu + len), uve_session_info, sizeof(uve_session_info));
	len += sizeof(uve_session_info) / 4;
	context->uvd.ib_cpu[len++] = context->enc.session.addr >> 32;
	context->uvd.ib_cpu[len++] = context->enc.session.addr;

	memcpy((context->uvd.ib_cpu + len), uve_task_info, sizeof(uve_task_info));
	len += sizeof(uve_task_info) / 4;
	context->uvd.ib_cpu[len++] = 0xffffffff;
	context->uvd.ib_cpu[len++] = 0x00000002;
	context->uvd.ib_cpu[len++] = 0x00000000;

	memcpy((context->uvd.ib_cpu + len), uve_op_close, sizeof(uve_op_close));
	len += sizeof(uve_op_close) / 4;

	r = submit(device_handle, &context->uvd, len, AMDGPU_HW_IP_UVD_ENC);
	igt_assert_eq(r, 0);

	free_resource(&context->enc.session);
}

static void
amdgpu_uvd_enc_test(amdgpu_device_handle device, struct mmd_shared_context *shared_context)
{
	struct uvd_enc_context context = {0};
	int r;

	r = mmd_context_init(device, &context.uvd);
	igt_require(r == 0);
	amdgpu_uvd_enc_create(device, &context);
	amdgpu_uvd_enc_session_init(device, &context);
	amdgpu_uvd_enc_encode(device, &context, shared_context);
	amdgpu_uvd_enc_destroy(device, &context);

	mmd_context_clean(device, &context.uvd);

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
		memset(&shared_context, 0, sizeof(shared_context));
		err = mmd_shared_context_init(device, &shared_context);
		igt_require(err == 0);

		igt_skip_on(!is_uvd_enc_enable(device));
	}

	igt_describe("Test uvd session, encode, destroy");
	igt_subtest("uvd_encoder")
		amdgpu_uvd_enc_test(device, &shared_context);

	igt_fixture() {
		amdgpu_device_deinitialize(device);
		drm_close_driver(fd);
	}
}
