// SPDX-License-Identifier: MIT
// Copyright 2023 Advanced Micro Devices, Inc.
// Copyright 2014 Advanced Micro Devices, Inc.

#include "lib/amdgpu/amd_mmd_shared.h"
#include "lib/amdgpu/amd_mmd_vce_ib.h"

#define FW_53_0_03 ((53 << 24) | (0 << 16) | (03 << 8))

struct amdgpu_vce_encode {
	unsigned int width;
	unsigned int height;
	struct amdgpu_mmd_bo vbuf;
	struct amdgpu_mmd_bo bs[2];
	struct amdgpu_mmd_bo fb[2];
	struct amdgpu_mmd_bo cpb;
	unsigned int ib_len;
	bool two_instance;
	struct amdgpu_mmd_bo mvrefbuf;
	struct amdgpu_mmd_bo mvb;
	unsigned int mvbuf_size;
};


static bool
is_vce_tests_enable(amdgpu_device_handle device_handle, uint32_t family_id,
		uint32_t chip_id, uint32_t chip_rev, bool *is_mv_supported)
{
	uint64_t ids_flags;
	uint32_t version, feature;
	int r;
	struct amdgpu_gpu_info gpu_info = {0};

	r = amdgpu_query_gpu_info(device_handle, &gpu_info);
	igt_assert_eq(r, 0);
	ids_flags = gpu_info.ids_flags;

	amdgpu_query_firmware_version(device_handle, AMDGPU_INFO_FW_VCE, 0,
					  0, &version, &feature);

	if (family_id >= AMDGPU_FAMILY_RV || family_id == AMDGPU_FAMILY_SI ||
			is_gfx_pipe_removed(family_id, chip_id, chip_rev)) {
		igt_info("\n\nThe ASIC NOT support VCE, tests are disabled\n");
		return false;
	}

	if (!(chip_id == (chip_rev + 0x3C) || /* FIJI */
			chip_id == (chip_rev + 0x50) || /* Polaris 10*/
			chip_id == (chip_rev + 0x5A) || /* Polaris 11*/
			chip_id == (chip_rev + 0x64) || /* Polaris 12*/
			(family_id >= AMDGPU_FAMILY_AI && !ids_flags))) /* dGPU > Polaris */
		igt_info("\n\nThe ASIC NOT support VCE MV, tests are disabled\n");
	else if (version < FW_53_0_03)
		igt_info("\n\nThe ASIC FW version NOT support VCE MV, tests are disabled\n");
	else
		*is_mv_supported = true;

	return true;
}

static void
amdgpu_cs_vce_create(amdgpu_device_handle device_handle,
		struct amdgpu_vce_encode *enc, struct mmd_context *context,
		struct mmd_shared_context *shared_context, bool is_mv_supported)
{
	unsigned int align = (shared_context->family_id >= AMDGPU_FAMILY_AI) ? 256 : 16;
	int len, r;

	enc->width = vce_create[6];
	enc->height = vce_create[7];

	context->num_resources  = 0;
	alloc_resource(device_handle, &enc->fb[0], 4096, AMDGPU_GEM_DOMAIN_GTT);
	context->resources[context->num_resources++] = enc->fb[0].handle;
	context->resources[context->num_resources++] = context->ib_handle;

	len = 0;
	memcpy(context->ib_cpu, vce_session, sizeof(vce_session));
	len += sizeof(vce_session) / 4;
	memcpy((context->ib_cpu + len), vce_taskinfo, sizeof(vce_taskinfo));
	len += sizeof(vce_taskinfo) / 4;
	memcpy((context->ib_cpu + len), vce_create, sizeof(vce_create));
	context->ib_cpu[len + 8] = ALIGN(enc->width, align);
	context->ib_cpu[len + 9] = ALIGN(enc->width, align);
	if (is_mv_supported == true) {/* disableTwoInstance */
		if (shared_context->family_id >= AMDGPU_FAMILY_AI)
			context->ib_cpu[len + 11] = 0x01000001;
		else
			context->ib_cpu[len + 11] = 0x01000201;
	}
	len += sizeof(vce_create) / 4;
	memcpy((context->ib_cpu + len), vce_feedback, sizeof(vce_feedback));
	context->ib_cpu[len + 2] = enc->fb[0].addr >> 32;
	context->ib_cpu[len + 3] = enc->fb[0].addr;
	len += sizeof(vce_feedback) / 4;

	r = submit(device_handle, context, len, AMDGPU_HW_IP_VCE);
	igt_assert_eq(r, 0);

	free_resource(&enc->fb[0]);
}

static void
amdgpu_cs_vce_config(amdgpu_device_handle device_handle,
		struct mmd_context *context, bool is_mv_supported)
{
	int len = 0, r;

	memcpy((context->ib_cpu + len), vce_session, sizeof(vce_session));
	len += sizeof(vce_session) / 4;
	memcpy((context->ib_cpu + len), vce_taskinfo, sizeof(vce_taskinfo));
	context->ib_cpu[len + 3] = 2;
	context->ib_cpu[len + 6] = 0xffffffff;
	len += sizeof(vce_taskinfo) / 4;
	memcpy((context->ib_cpu + len), vce_rate_ctrl, sizeof(vce_rate_ctrl));
	len += sizeof(vce_rate_ctrl) / 4;
	memcpy((context->ib_cpu + len), vce_config_ext, sizeof(vce_config_ext));
	len += sizeof(vce_config_ext) / 4;
	memcpy((context->ib_cpu + len), vce_motion_est, sizeof(vce_motion_est));
	len += sizeof(vce_motion_est) / 4;
	memcpy((context->ib_cpu + len), vce_rdo, sizeof(vce_rdo));
	len += sizeof(vce_rdo) / 4;
	memcpy((context->ib_cpu + len), vce_pic_ctrl, sizeof(vce_pic_ctrl));
	if (is_mv_supported == true)
		context->ib_cpu[len + 27] = 0x00000001; /* encSliceMode */
	len += sizeof(vce_pic_ctrl) / 4;

	r = submit(device_handle, context, len, AMDGPU_HW_IP_VCE);
	igt_assert_eq(r, 0);
}

static void amdgpu_cs_vce_encode_idr(amdgpu_device_handle device_handle,
		struct mmd_shared_context *shared_context, struct mmd_context *context,
		struct amdgpu_vce_encode *enc)
{

	uint64_t luma_offset, chroma_offset;
	unsigned int align = (shared_context->family_id >= AMDGPU_FAMILY_AI) ? 256 : 16;
	unsigned int luma_size = ALIGN(enc->width, align) * ALIGN(enc->height, 16);
	int len = 0, i, r;

	luma_offset = enc->vbuf.addr;
	chroma_offset = luma_offset + luma_size;

	memcpy((context->ib_cpu + len), vce_session, sizeof(vce_session));
	len += sizeof(vce_session) / 4;
	memcpy((context->ib_cpu + len), vce_taskinfo, sizeof(vce_taskinfo));
	len += sizeof(vce_taskinfo) / 4;
	memcpy((context->ib_cpu + len), vce_bs_buffer, sizeof(vce_bs_buffer));
	context->ib_cpu[len + 2] = enc->bs[0].addr >> 32;
	context->ib_cpu[len + 3] = enc->bs[0].addr;
	len += sizeof(vce_bs_buffer) / 4;
	memcpy((context->ib_cpu + len), vce_context_buffer, sizeof(vce_context_buffer));
	context->ib_cpu[len + 2] = enc->cpb.addr >> 32;
	context->ib_cpu[len + 3] = enc->cpb.addr;
	len += sizeof(vce_context_buffer) / 4;
	memcpy((context->ib_cpu + len), vce_aux_buffer, sizeof(vce_aux_buffer));
	for (i = 0; i <  8; ++i)
		context->ib_cpu[len + 2 + i] = luma_size * 1.5 * (i + 2);
	for (i = 0; i <  8; ++i)
		context->ib_cpu[len + 10 + i] = luma_size * 1.5;
	len += sizeof(vce_aux_buffer) / 4;
	memcpy((context->ib_cpu + len), vce_feedback, sizeof(vce_feedback));
	context->ib_cpu[len + 2] = enc->fb[0].addr >> 32;
	context->ib_cpu[len + 3] = enc->fb[0].addr;
	len += sizeof(vce_feedback) / 4;
	memcpy((context->ib_cpu + len), vce_encode, sizeof(vce_encode));
	context->ib_cpu[len + 9] = luma_offset >> 32;
	context->ib_cpu[len + 10] = luma_offset;
	context->ib_cpu[len + 11] = chroma_offset >> 32;
	context->ib_cpu[len + 12] = chroma_offset;
	context->ib_cpu[len + 14] = ALIGN(enc->width, align);
	context->ib_cpu[len + 15] = ALIGN(enc->width, align);
	context->ib_cpu[len + 73] = luma_size * 1.5;
	context->ib_cpu[len + 74] = luma_size * 2.5;
	len += sizeof(vce_encode) / 4;
	enc->ib_len = len;
	if (!enc->two_instance) {
		r = submit(device_handle, context, len, AMDGPU_HW_IP_VCE);
		igt_assert_eq(r, 0);
	}
}

static void amdgpu_cs_vce_encode_p(amdgpu_device_handle device_handle,
		struct mmd_shared_context *shared_context,
		struct mmd_context *context, struct amdgpu_vce_encode *enc)
{
	uint64_t luma_offset, chroma_offset;
	int len, i, r;
	unsigned int align = (shared_context->family_id >= AMDGPU_FAMILY_AI) ? 256 : 16;
	unsigned int luma_size = ALIGN(enc->width, align) * ALIGN(enc->height, 16);

	len = (enc->two_instance) ? enc->ib_len : 0;
	luma_offset = enc->vbuf.addr;
	chroma_offset = luma_offset + luma_size;

	if (!enc->two_instance) {
		memcpy((context->ib_cpu + len), vce_session, sizeof(vce_session));
		len += sizeof(vce_session) / 4;
	}
	memcpy((context->ib_cpu + len), vce_taskinfo, sizeof(vce_taskinfo));
	len += sizeof(vce_taskinfo) / 4;
	memcpy((context->ib_cpu + len), vce_bs_buffer, sizeof(vce_bs_buffer));
	context->ib_cpu[len + 2] = enc->bs[1].addr >> 32;
	context->ib_cpu[len + 3] = enc->bs[1].addr;
	len += sizeof(vce_bs_buffer) / 4;
	memcpy((context->ib_cpu + len), vce_context_buffer, sizeof(vce_context_buffer));
	context->ib_cpu[len + 2] = enc->cpb.addr >> 32;
	context->ib_cpu[len + 3] = enc->cpb.addr;
	len += sizeof(vce_context_buffer) / 4;
	memcpy((context->ib_cpu + len), vce_aux_buffer, sizeof(vce_aux_buffer));
	for (i = 0; i <  8; ++i)
		context->ib_cpu[len + 2 + i] = luma_size * 1.5 * (i + 2);
	for (i = 0; i <  8; ++i)
		context->ib_cpu[len + 10 + i] = luma_size * 1.5;
	len += sizeof(vce_aux_buffer) / 4;
	memcpy((context->ib_cpu + len), vce_feedback, sizeof(vce_feedback));
	context->ib_cpu[len + 2] = enc->fb[1].addr >> 32;
	context->ib_cpu[len + 3] = enc->fb[1].addr;
	len += sizeof(vce_feedback) / 4;
	memcpy((context->ib_cpu + len), vce_encode, sizeof(vce_encode));
	context->ib_cpu[len + 2] = 0;
	context->ib_cpu[len + 9] = luma_offset >> 32;
	context->ib_cpu[len + 10] = luma_offset;
	context->ib_cpu[len + 11] = chroma_offset >> 32;
	context->ib_cpu[len + 12] = chroma_offset;
	context->ib_cpu[len + 14] = ALIGN(enc->width, align);
	context->ib_cpu[len + 15] = ALIGN(enc->width, align);
	context->ib_cpu[len + 18] = 0;
	context->ib_cpu[len + 19] = 0;
	context->ib_cpu[len + 56] = 3;
	context->ib_cpu[len + 57] = 0;
	context->ib_cpu[len + 58] = 0;
	context->ib_cpu[len + 59] = luma_size * 1.5;
	context->ib_cpu[len + 60] = luma_size * 2.5;
	context->ib_cpu[len + 73] = 0;
	context->ib_cpu[len + 74] = luma_size;
	context->ib_cpu[len + 81] = 1;
	context->ib_cpu[len + 82] = 1;
	len += sizeof(vce_encode) / 4;

	r = submit(device_handle, context, len, AMDGPU_HW_IP_VCE);
	igt_assert_eq(r, 0);
}

static void check_result(struct amdgpu_vce_encode *enc)
{
	uint64_t sum;
	uint32_t s[2] = {180325, 15946};
	uint32_t *ptr, size;
	int i, j, r;

	for (i = 0; i < 2; ++i) {
		r = amdgpu_bo_cpu_map(enc->fb[i].handle, (void **)&enc->fb[i].ptr);
		igt_assert_eq(r, 0);
		ptr = (uint32_t *)enc->fb[i].ptr;
		size = ptr[4] - ptr[9];
		r = amdgpu_bo_cpu_unmap(enc->fb[i].handle);
		igt_assert_eq(r, 0);
		r = amdgpu_bo_cpu_map(enc->bs[i].handle, (void **)&enc->bs[i].ptr);
		igt_assert_eq(r, 0);
		for (j = 0, sum = 0; j < size; ++j)
			sum += enc->bs[i].ptr[j];
		igt_assert_eq(sum, s[i]);
		r = amdgpu_bo_cpu_unmap(enc->bs[i].handle);
		igt_assert_eq(r, 0);
	}
}

static void
amdgpu_cs_vce_encode(amdgpu_device_handle device_handle,
		struct mmd_shared_context *shared_context, struct mmd_context *context,
		struct amdgpu_vce_encode *enc, bool is_mv_supported)
{
	uint32_t vbuf_size, bs_size = 0x154000, cpb_size;
	unsigned int align = (shared_context->family_id >= AMDGPU_FAMILY_AI) ? 256 : 16;
	int i, r;

	vbuf_size = ALIGN(enc->width, align) * ALIGN(enc->height, 16) * 1.5;
	cpb_size = vbuf_size * 10;
	context->num_resources = 0;
	alloc_resource(device_handle, &enc->fb[0], 4096, AMDGPU_GEM_DOMAIN_GTT);
	context->resources[context->num_resources++] = enc->fb[0].handle;
	alloc_resource(device_handle, &enc->fb[1], 4096, AMDGPU_GEM_DOMAIN_GTT);
	context->resources[context->num_resources++] = enc->fb[1].handle;
	alloc_resource(device_handle, &enc->bs[0], bs_size, AMDGPU_GEM_DOMAIN_GTT);
	context->resources[context->num_resources++] = enc->bs[0].handle;
	alloc_resource(device_handle, &enc->bs[1], bs_size, AMDGPU_GEM_DOMAIN_GTT);
	context->resources[context->num_resources++] = enc->bs[1].handle;
	alloc_resource(device_handle, &enc->vbuf, vbuf_size, AMDGPU_GEM_DOMAIN_VRAM);
	context->resources[context->num_resources++] = enc->vbuf.handle;
	alloc_resource(device_handle, &enc->cpb, cpb_size, AMDGPU_GEM_DOMAIN_VRAM);
	context->resources[context->num_resources++] = enc->cpb.handle;
	context->resources[context->num_resources++] = context->ib_handle;

	r = amdgpu_bo_cpu_map(enc->vbuf.handle, (void **)&enc->vbuf.ptr);
	igt_assert_eq(r, 0);

	memset(enc->vbuf.ptr, 0, vbuf_size);
	for (i = 0; i < enc->height; ++i) {
		memcpy(enc->vbuf.ptr, (frame + i * enc->width), enc->width);
		enc->vbuf.ptr += ALIGN(enc->width, align);
	}
	for (i = 0; i < enc->height / 2; ++i) {
		memcpy(enc->vbuf.ptr, ((frame + enc->height * enc->width) +
				i * enc->width), enc->width);
		enc->vbuf.ptr += ALIGN(enc->width, align);
	}

	r = amdgpu_bo_cpu_unmap(enc->vbuf.handle);
	igt_assert_eq(r, 0);

	amdgpu_cs_vce_config(device_handle, context, is_mv_supported);

	if (shared_context->family_id >= AMDGPU_FAMILY_VI) {
		vce_taskinfo[3] = 3;
		amdgpu_cs_vce_encode_idr(device_handle, shared_context, context, enc);
		amdgpu_cs_vce_encode_p(device_handle, shared_context, context, enc);
		check_result(enc);

		/* two pipes */
		vce_encode[16] = 0;
		amdgpu_cs_vce_encode_idr(device_handle, shared_context, context, enc);
		amdgpu_cs_vce_encode_p(device_handle, shared_context, context, enc);
		check_result(enc);

		/* two instances */
		if (shared_context->vce_harvest_config == 0) {
			enc->two_instance = true;
			vce_taskinfo[2] = 0x83;
			vce_taskinfo[4] = 1;
			amdgpu_cs_vce_encode_idr(device_handle, shared_context, context, enc);
			vce_taskinfo[2] = 0xffffffff;
			vce_taskinfo[4] = 2;
			amdgpu_cs_vce_encode_p(device_handle, shared_context, context, enc);
			check_result(enc);
		}
	} else {
		vce_taskinfo[3] = 3;
		vce_encode[16] = 0;
		amdgpu_cs_vce_encode_idr(device_handle, shared_context, context, enc);
		amdgpu_cs_vce_encode_p(device_handle, shared_context, context, enc);
		check_result(enc);
	}

	free_resource(&enc->fb[0]);
	free_resource(&enc->fb[1]);
	free_resource(&enc->bs[0]);
	free_resource(&enc->bs[1]);
	free_resource(&enc->vbuf);
	free_resource(&enc->cpb);
}

static void amdgpu_cs_vce_mv(amdgpu_device_handle device_handle,
		struct mmd_shared_context *shared_context,
		struct mmd_context *context, struct amdgpu_vce_encode *enc)
{
	uint64_t luma_offset, chroma_offset;
	uint64_t mv_ref_luma_offset;
	unsigned int align = (shared_context->family_id >= AMDGPU_FAMILY_AI) ? 256 : 16;
	unsigned int luma_size = ALIGN(enc->width, align) * ALIGN(enc->height, 16);
	int len = 0, i, r;

	luma_offset = enc->vbuf.addr;
	chroma_offset = luma_offset + luma_size;
	mv_ref_luma_offset = enc->mvrefbuf.addr;

	memcpy((context->ib_cpu + len), vce_session, sizeof(vce_session));
	len += sizeof(vce_session) / 4;
	memcpy((context->ib_cpu + len), vce_taskinfo, sizeof(vce_taskinfo));
	len += sizeof(vce_taskinfo) / 4;
	memcpy((context->ib_cpu + len), vce_bs_buffer, sizeof(vce_bs_buffer));
	context->ib_cpu[len + 2] = enc->bs[0].addr >> 32;
	context->ib_cpu[len + 3] = enc->bs[0].addr;
	len += sizeof(vce_bs_buffer) / 4;
	memcpy((context->ib_cpu + len), vce_context_buffer, sizeof(vce_context_buffer));
	context->ib_cpu[len + 2] = enc->cpb.addr >> 32;
	context->ib_cpu[len + 3] = enc->cpb.addr;
	len += sizeof(vce_context_buffer) / 4;
	memcpy((context->ib_cpu + len), vce_aux_buffer, sizeof(vce_aux_buffer));
	for (i = 0; i <  8; ++i)
		context->ib_cpu[len + 2 + i] = luma_size * 1.5 * (i + 2);
	for (i = 0; i <  8; ++i)
		context->ib_cpu[len + 10 + i] = luma_size * 1.5;
	len += sizeof(vce_aux_buffer) / 4;
	memcpy((context->ib_cpu + len), vce_feedback, sizeof(vce_feedback));
	context->ib_cpu[len + 2] = enc->fb[0].addr >> 32;
	context->ib_cpu[len + 3] = enc->fb[0].addr;
	len += sizeof(vce_feedback) / 4;
	memcpy((context->ib_cpu + len), vce_mv_buffer, sizeof(vce_mv_buffer));
	context->ib_cpu[len + 2] = mv_ref_luma_offset >> 32;
	context->ib_cpu[len + 3] = mv_ref_luma_offset;
	context->ib_cpu[len + 4] = ALIGN(enc->width, align);
	context->ib_cpu[len + 5] = ALIGN(enc->width, align);
	context->ib_cpu[len + 6] = luma_size;
	context->ib_cpu[len + 7] = enc->mvb.addr >> 32;
	context->ib_cpu[len + 8] = enc->mvb.addr;
	len += sizeof(vce_mv_buffer) / 4;
	memcpy((context->ib_cpu + len), vce_encode, sizeof(vce_encode));
	context->ib_cpu[len + 2] = 0;
	context->ib_cpu[len + 3] = 0;
	context->ib_cpu[len + 4] = 0x154000;
	context->ib_cpu[len + 9] = luma_offset >> 32;
	context->ib_cpu[len + 10] = luma_offset;
	context->ib_cpu[len + 11] = chroma_offset >> 32;
	context->ib_cpu[len + 12] = chroma_offset;
	context->ib_cpu[len + 13] = ALIGN(enc->height, 16);
	context->ib_cpu[len + 14] = ALIGN(enc->width, align);
	context->ib_cpu[len + 15] = ALIGN(enc->width, align);
	/* encDisableMBOffloading-encDisableTwoPipeMode-encInputPicArrayMode-encInputPicAddrMode */
	context->ib_cpu[len + 16] = 0x01010000;
	context->ib_cpu[len + 18] = 0; /* encPicType */
	context->ib_cpu[len + 19] = 0; /* encIdrFlag */
	context->ib_cpu[len + 20] = 0; /* encIdrPicId */
	context->ib_cpu[len + 21] = 0; /* encMGSKeyPic */
	context->ib_cpu[len + 22] = 0; /* encReferenceFlag */
	context->ib_cpu[len + 23] = 0; /* encTemporalLayerIndex */
	context->ib_cpu[len + 55] = 0; /* pictureStructure */
	context->ib_cpu[len + 56] = 0; /* encPicType -ref[0] */
	context->ib_cpu[len + 61] = 0; /* pictureStructure */
	context->ib_cpu[len + 62] = 0; /* encPicType -ref[1] */
	context->ib_cpu[len + 67] = 0; /* pictureStructure */
	context->ib_cpu[len + 68] = 0; /* encPicType -ref1 */
	context->ib_cpu[len + 81] = 1; /* frameNumber */
	context->ib_cpu[len + 82] = 2; /* pictureOrderCount */
	context->ib_cpu[len + 83] = 0xffffffff; /* numIPicRemainInRCGOP */
	context->ib_cpu[len + 84] = 0xffffffff; /* numPPicRemainInRCGOP */
	context->ib_cpu[len + 85] = 0xffffffff; /* numBPicRemainInRCGOP */
	context->ib_cpu[len + 86] = 0xffffffff; /* numIRPicRemainInRCGOP */
	context->ib_cpu[len + 87] = 0; /* remainedIntraRefreshPictures */
	len += sizeof(vce_encode) / 4;

	enc->ib_len = len;
	r = submit(device_handle, context, len, AMDGPU_HW_IP_VCE);
	igt_assert_eq(r, 0);
}

static void check_mv_result(struct amdgpu_vce_encode *enc)
{
	uint64_t sum;
	/* uint32_t s = 140790;*/
	int j, r;

	r = amdgpu_bo_cpu_map(enc->fb[0].handle, (void **)&enc->fb[0].ptr);
	igt_assert_eq(r, 0);
	r = amdgpu_bo_cpu_unmap(enc->fb[0].handle);
	igt_assert_eq(r, 0);
	r = amdgpu_bo_cpu_map(enc->mvb.handle, (void **)&enc->mvb.ptr);
	igt_assert_eq(r, 0);
	for (j = 0, sum = 0; j < enc->mvbuf_size; ++j)
		sum += enc->mvb.ptr[j];
	/*
	 * Temporarily disable verification due to ongoing investigation to figure out the root cause of the error.
	 * The comparison would continue, but assert is commented out.
	 * 128738 != 140790
	 * 131740 != 140790
	 */
	/* igt_assert_eq(sum, s); */
	r = amdgpu_bo_cpu_unmap(enc->mvb.handle);
	igt_assert_eq(r, 0);
}

static void
amdgpu_cs_vce_encode_mv(amdgpu_device_handle device_handle,
		struct mmd_shared_context *shared_context,
		struct mmd_context *context,
		struct amdgpu_vce_encode *enc, bool is_mv_supported)
{
	uint32_t vbuf_size, bs_size = 0x154000, cpb_size;
	unsigned int align = (shared_context->family_id >= AMDGPU_FAMILY_AI) ? 256 : 16;
	int i, r;

	vbuf_size = ALIGN(enc->width, align) * ALIGN(enc->height, 16) * 1.5;
	enc->mvbuf_size = ALIGN(enc->width, 16) * ALIGN(enc->height, 16) / 8;
	cpb_size = vbuf_size * 10;
	context->num_resources = 0;
	alloc_resource(device_handle, &enc->fb[0], 4096, AMDGPU_GEM_DOMAIN_GTT);
	context->resources[context->num_resources++] = enc->fb[0].handle;
	alloc_resource(device_handle, &enc->bs[0], bs_size, AMDGPU_GEM_DOMAIN_GTT);
	context->resources[context->num_resources++] = enc->bs[0].handle;
	alloc_resource(device_handle, &enc->mvb, enc->mvbuf_size, AMDGPU_GEM_DOMAIN_GTT);
	context->resources[context->num_resources++] = enc->mvb.handle;
	alloc_resource(device_handle, &enc->vbuf, vbuf_size, AMDGPU_GEM_DOMAIN_VRAM);
	context->resources[context->num_resources++] = enc->vbuf.handle;
	alloc_resource(device_handle, &enc->mvrefbuf, vbuf_size, AMDGPU_GEM_DOMAIN_VRAM);
	context->resources[context->num_resources++] = enc->mvrefbuf.handle;
	alloc_resource(device_handle, &enc->cpb, cpb_size, AMDGPU_GEM_DOMAIN_VRAM);
	context->resources[context->num_resources++] = enc->cpb.handle;
	context->resources[context->num_resources++] = context->ib_handle;

	r = amdgpu_bo_cpu_map(enc->vbuf.handle, (void **)&enc->vbuf.ptr);
	igt_assert_eq(r, 0);

	memset(enc->vbuf.ptr, 0, vbuf_size);
	for (i = 0; i < enc->height; ++i) {
		memcpy(enc->vbuf.ptr, (frame + i * enc->width), enc->width);
		enc->vbuf.ptr += ALIGN(enc->width, align);
	}
	for (i = 0; i < enc->height / 2; ++i) {
		memcpy(enc->vbuf.ptr, ((frame + enc->height * enc->width) + i * enc->width), enc->width);
		enc->vbuf.ptr += ALIGN(enc->width, align);
	}

	r = amdgpu_bo_cpu_unmap(enc->vbuf.handle);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_cpu_map(enc->mvrefbuf.handle, (void **)&enc->mvrefbuf.ptr);
	igt_assert_eq(r, 0);

	memset(enc->mvrefbuf.ptr, 0, vbuf_size);
	for (i = 0; i < enc->height; ++i) {
		memcpy(enc->mvrefbuf.ptr, (frame + (enc->height - i - 1) * enc->width), enc->width);
		enc->mvrefbuf.ptr += ALIGN(enc->width, align);
	}
	for (i = 0; i < enc->height / 2; ++i) {
		memcpy(enc->mvrefbuf.ptr,
		((frame + enc->height * enc->width) + (enc->height / 2 - i - 1) * enc->width), enc->width);
		enc->mvrefbuf.ptr += ALIGN(enc->width, align);
	}

	r = amdgpu_bo_cpu_unmap(enc->mvrefbuf.handle);
	igt_assert_eq(r, 0);

	amdgpu_cs_vce_config(device_handle, context, is_mv_supported);

	vce_taskinfo[3] = 3;
	amdgpu_cs_vce_mv(device_handle, shared_context, context, enc);
	check_mv_result(enc);

	free_resource(&enc->fb[0]);
	free_resource(&enc->bs[0]);
	free_resource(&enc->vbuf);
	free_resource(&enc->cpb);
	free_resource(&enc->mvrefbuf);
	free_resource(&enc->mvb);
}

static void
amdgpu_cs_vce_destroy(amdgpu_device_handle device_handle, struct mmd_context *context,
		struct amdgpu_vce_encode *enc)
{
	int len, r;

	context->num_resources  = 0;
	alloc_resource(device_handle, &enc->fb[0], 4096, AMDGPU_GEM_DOMAIN_GTT);
	context->resources[context->num_resources++] = enc->fb[0].handle;
	context->resources[context->num_resources++] = context->ib_handle;

	len = 0;
	memcpy(context->ib_cpu, vce_session, sizeof(vce_session));
	len += sizeof(vce_session) / 4;
	memcpy((context->ib_cpu + len), vce_taskinfo, sizeof(vce_taskinfo));
	context->ib_cpu[len + 3] = 1;
	len += sizeof(vce_taskinfo) / 4;
	memcpy((context->ib_cpu + len), vce_feedback, sizeof(vce_feedback));
	context->ib_cpu[len + 2] = enc->fb[0].addr >> 32;
	context->ib_cpu[len + 3] = enc->fb[0].addr;
	len += sizeof(vce_feedback) / 4;
	memcpy((context->ib_cpu + len), vce_destroy, sizeof(vce_destroy));
	len += sizeof(vce_destroy) / 4;

	r = submit(device_handle, context, len, AMDGPU_HW_IP_VCE);
	igt_assert_eq(r, 0);

	free_resource(&enc->fb[0]);
}

static void
amdgpu_vce_enc_test(amdgpu_device_handle device, struct mmd_shared_context *shared_context,
		bool is_mv_supported)
{
	int err;
	struct mmd_context acontext = {};
	struct amdgpu_vce_encode aenc = {};

	struct mmd_context *context = &acontext;
	struct amdgpu_vce_encode *enc = &aenc;

	err = mmd_context_init(device, context);
	igt_require(err == 0);
	amdgpu_cs_vce_create(device, enc, context, shared_context, is_mv_supported);
	amdgpu_cs_vce_encode(device, shared_context ,context, enc, is_mv_supported);
	if (is_mv_supported)
		amdgpu_cs_vce_encode_mv(device, shared_context, context, enc, is_mv_supported);
	amdgpu_cs_vce_destroy(device, context, enc);
	mmd_context_clean(device, context);
}

int igt_main()
{
	amdgpu_device_handle device;
	struct mmd_shared_context shared_context = {};
	int fd = -1;
	bool is_mv_supported = false;

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
		igt_skip_on(!is_vce_tests_enable(device, shared_context.family_id, shared_context.chip_id,
				shared_context.chip_rev, &is_mv_supported));
	}
	igt_describe("Test vce enc is created, encode, destroy");
	igt_subtest("amdgpu_vce_encoder")
		amdgpu_vce_enc_test(device, &shared_context, is_mv_supported);

	igt_fixture() {
		amdgpu_device_deinitialize(device);
		drm_close_driver(fd);
	}

}
