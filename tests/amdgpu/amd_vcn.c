// SPDX-License-Identifier: MIT
/*
 * Copyright 2023 Advanced Micro Devices, Inc.
 * Copyright 2014 Advanced Micro Devices, Inc.
 */

#include "lib/amdgpu/amd_vcn_shared.h"

static uint32_t
bs_read_u(struct buffer_info *buf_info, int n);

static uint32_t
bs_read_ue(struct buffer_info *buf_info);

static void
amdgpu_cs_vcn_dec_create(amdgpu_device_handle device_handle,
		struct mmd_shared_context *shared_context,  struct mmd_context *context,
		struct vcn_context *v_context)
{
	struct amdgpu_mmd_bo msg_buf;
	unsigned int ip;
	int len, r;

	context->num_resources  = 0;
	alloc_resource(device_handle, &msg_buf, 4096, AMDGPU_GEM_DOMAIN_GTT);
	alloc_resource(device_handle, &v_context->session_ctx_buf, 32 * 4096, AMDGPU_GEM_DOMAIN_GTT);
	context->resources[context->num_resources++] = msg_buf.handle;
	context->resources[context->num_resources++] = v_context->session_ctx_buf.handle;
	context->resources[context->num_resources++] = context->ib_handle;
	r = amdgpu_bo_cpu_map(msg_buf.handle, (void **)&msg_buf.ptr);
	igt_assert_eq(r, 0);

	memset(msg_buf.ptr, 0, 4096);
	memcpy(msg_buf.ptr, vcn_dec_create_msg, sizeof(vcn_dec_create_msg));

	len = 0;
	vcn_dec_cmd(shared_context, context, v_context, v_context->session_ctx_buf.addr,
		DECODE_CMD_SESSION_CONTEXT_BUFFER, &len, INVALID_DECODER_NONE);
	if (shared_context->vcn_dec_sw_ring == true) {
		vcn_dec_cmd(shared_context, context, v_context, msg_buf.addr,
			DECODE_CMD_MSG_BUFFER, &len, INVALID_DECODER_NONE);
	} else {
		context->ib_cpu[len++] = reg[shared_context->vcn_reg_index].data0;
		context->ib_cpu[len++] = msg_buf.addr;
		context->ib_cpu[len++] = reg[shared_context->vcn_reg_index].data1;
		context->ib_cpu[len++] = msg_buf.addr >> 32;
		context->ib_cpu[len++] = reg[shared_context->vcn_reg_index].cmd;
		context->ib_cpu[len++] = 0;
		for (; len % 16; ) {
			context->ib_cpu[len++] = reg[shared_context->vcn_reg_index].nop;
			context->ib_cpu[len++] = 0;
		}
	}
	if (shared_context->vcn_unified_ring) {
		amdgpu_cs_sq_ib_tail(v_context, context->ib_cpu + len);
		ip = AMDGPU_HW_IP_VCN_ENC;
	} else
		ip = AMDGPU_HW_IP_VCN_DEC;

	r = submit(device_handle, context, len, ip);

	igt_assert_eq(r, 0);

	free_resource(&msg_buf);
}

static void
amdgpu_cs_vcn_dec_decode(amdgpu_device_handle device_handle,
			struct mmd_shared_context *shared_context,
			struct mmd_context *context,
			struct vcn_context *v_context)
{
	const unsigned int dpb_size = 15923584, dt_size = 737280;
	uint64_t msg_addr, fb_addr, bs_addr, dpb_addr, ctx_addr, dt_addr, it_addr, sum;
	struct amdgpu_mmd_bo dec_buf;
	int size, len, i, r;
	unsigned int ip;
	uint8_t *dec;
	enum decoder_error_type err_type = INVALID_DECODER_NONE;

	size = 4*1024; /* msg */
	size += 4*1024; /* fb */
	size += 4096; /*it_scaling_table*/
	size += ALIGN(sizeof(uvd_bitstream), 4*1024);
	size += ALIGN(dpb_size, 4*1024);
	size += ALIGN(dt_size, 4*1024);


	context->num_resources = 0;
	alloc_resource(device_handle, &dec_buf, size, AMDGPU_GEM_DOMAIN_GTT);
	context->resources[context->num_resources++] = dec_buf.handle;
	context->resources[context->num_resources++] = context->ib_handle;

	r = amdgpu_bo_cpu_map(dec_buf.handle, (void **)&dec_buf.ptr);
	dec = dec_buf.ptr;

	igt_assert_eq(r, 0);
	memset(dec_buf.ptr, 0, size);
	memcpy(dec_buf.ptr, vcn_dec_decode_msg, sizeof(vcn_dec_decode_msg));
	memcpy(dec_buf.ptr + sizeof(vcn_dec_decode_msg),
			avc_decode_msg, sizeof(avc_decode_msg));

	dec += 4*1024;
	memcpy(dec, feedback_msg, sizeof(feedback_msg));
	dec += 4*1024;
	memcpy(dec, uvd_it_scaling_table, sizeof(uvd_it_scaling_table));

	dec += 4*1024;
	memcpy(dec, uvd_bitstream, sizeof(uvd_bitstream));

	dec += ALIGN(sizeof(uvd_bitstream), 4*1024);

	dec += ALIGN(dpb_size, 4*1024);

	msg_addr = dec_buf.addr;
	fb_addr = msg_addr + 4*1024;
	it_addr = fb_addr + 4*1024;
	bs_addr = it_addr + 4*1024;
	dpb_addr = ALIGN(bs_addr + sizeof(uvd_bitstream), 4*1024);
	ctx_addr = ALIGN(dpb_addr + 0x006B9400, 4*1024);
	dt_addr = ALIGN(dpb_addr + dpb_size, 4*1024);

	len = 0;
	vcn_dec_cmd(shared_context, context, v_context,
		v_context->session_ctx_buf.addr, DECODE_CMD_SESSION_CONTEXT_BUFFER, &len, err_type);
	vcn_dec_cmd(shared_context, context, v_context,
		msg_addr, DECODE_CMD_MSG_BUFFER, &len, err_type);
	vcn_dec_cmd(shared_context, context, v_context,
		dpb_addr, DECODE_CMD_DPB_BUFFER, &len, err_type);
	vcn_dec_cmd(shared_context, context, v_context,
		dt_addr, DECODE_CMD_DECODING_TARGET_BUFFER, &len, err_type);
	vcn_dec_cmd(shared_context, context, v_context,
		fb_addr, DECODE_CMD_FEEDBACK_BUFFER, &len, err_type);
	vcn_dec_cmd(shared_context, context, v_context,
		bs_addr, DECODE_CMD_BITSTREAM_BUFFER, &len, err_type);
	vcn_dec_cmd(shared_context, context, v_context,
		it_addr, DECODE_CMD_IT_SCALING_TABLE_BUFFER, &len, err_type);
	vcn_dec_cmd(shared_context, context, v_context,
		ctx_addr, DECODE_CMD_CONTEXT_BUFFER, &len, err_type);

	if (shared_context->vcn_dec_sw_ring == false) {
		context->ib_cpu[len++] = reg[shared_context->vcn_reg_index].cntl;
		context->ib_cpu[len++] = 0x1;
		for (; len % 16; ) {
			context->ib_cpu[len++] = reg[shared_context->vcn_reg_index].nop;
			context->ib_cpu[len++] = 0;
		}
	}

	if (shared_context->vcn_unified_ring) {
		amdgpu_cs_sq_ib_tail(v_context, context->ib_cpu + len);
		ip = AMDGPU_HW_IP_VCN_ENC;
	} else
		ip = AMDGPU_HW_IP_VCN_DEC;

	r = submit(device_handle, context, len, ip);
	igt_assert_eq(r, 0);

	for (i = 0, sum = 0; i < dt_size; ++i)
		sum += dec[i];

	igt_assert_eq(sum, SUM_DECODE);

	free_resource(&dec_buf);
}

static void
amdgpu_cs_vcn_dec_destroy(amdgpu_device_handle device_handle,
		struct mmd_shared_context *shared_context,
		struct mmd_context *context, struct vcn_context *v_context)
{
	struct amdgpu_mmd_bo msg_buf;
	unsigned int ip;
	int len, r;

	context->num_resources = 0;
	alloc_resource(device_handle, &msg_buf, 1024, AMDGPU_GEM_DOMAIN_GTT);
	context->resources[context->num_resources++] = msg_buf.handle;
	context->resources[context->num_resources++] = context->ib_handle;

	r = amdgpu_bo_cpu_map(msg_buf.handle, (void **)&msg_buf.ptr);
	igt_assert_eq(r, 0);

	memset(msg_buf.ptr, 0, 1024);
	memcpy(msg_buf.ptr, vcn_dec_destroy_msg, sizeof(vcn_dec_destroy_msg));

	len = 0;
	vcn_dec_cmd(shared_context, context, v_context, v_context->session_ctx_buf.addr,
		DECODE_CMD_SESSION_CONTEXT_BUFFER, &len, INVALID_DECODER_NONE);
	if (shared_context->vcn_dec_sw_ring == true) {
		vcn_dec_cmd(shared_context, context, v_context, msg_buf.addr,
			DECODE_CMD_MSG_BUFFER, &len, INVALID_DECODER_NONE);
	} else {
		context->ib_cpu[len++] = reg[shared_context->vcn_reg_index].data0;
		context->ib_cpu[len++] = msg_buf.addr;
		context->ib_cpu[len++] = reg[shared_context->vcn_reg_index].data1;
		context->ib_cpu[len++] = msg_buf.addr >> 32;
		context->ib_cpu[len++] = reg[shared_context->vcn_reg_index].cmd;
		context->ib_cpu[len++] = 0;
		for (; len % 16; ) {
			context->ib_cpu[len++] = reg[shared_context->vcn_reg_index].nop;
			context->ib_cpu[len++] = 0;
		}
	}
	if (shared_context->vcn_unified_ring) {
		amdgpu_cs_sq_ib_tail(v_context, context->ib_cpu + len);
		ip = AMDGPU_HW_IP_VCN_ENC;
	} else
		ip = AMDGPU_HW_IP_VCN_DEC;

	r = submit(device_handle, context, len, ip);
	igt_assert_eq(r, 0);

	free_resource(&msg_buf);
	free_resource(&v_context->session_ctx_buf);
}

static void
amdgpu_cs_vcn_enc_create(amdgpu_device_handle device_handle,
			struct mmd_shared_context *shared_context,
			struct mmd_context *context, struct vcn_context *v_context)
{
	int len, r;
	uint32_t *p_task_size = NULL;
	uint32_t task_offset = 0, st_offset;
	uint32_t *st_size = NULL;
	unsigned int width = 160, height = 128, buf_size;
	uint32_t fw_maj = 1, fw_min = 9;

	if (shared_context->vcn_ip_version_major == 2) {
		fw_maj = 1;
		fw_min = 1;
	} else if (shared_context->vcn_ip_version_major == 3) {
		fw_maj = 1;
		fw_min = 0;
	}

	v_context->gWidth = width;
	v_context->gHeight = height;
	buf_size = ALIGN(width, 256) * ALIGN(height, 32) * 3 / 2;
	v_context->enc_task_id = 1;

	context->num_resources = 0;
	alloc_resource(device_handle, &v_context->enc_buf, 128 * 1024, AMDGPU_GEM_DOMAIN_GTT);
	alloc_resource(device_handle, &v_context->cpb_buf, buf_size * 2, AMDGPU_GEM_DOMAIN_GTT);
	context->resources[context->num_resources++] = v_context->enc_buf.handle;
	context->resources[context->num_resources++] = v_context->cpb_buf.handle;
	context->resources[context->num_resources++] = context->ib_handle;

	r = amdgpu_bo_cpu_map(v_context->enc_buf.handle, (void **)&v_context->enc_buf.ptr);
	memset(v_context->enc_buf.ptr, 0, 128 * 1024);
	r = amdgpu_bo_cpu_unmap(v_context->enc_buf.handle);

	r = amdgpu_bo_cpu_map(v_context->cpb_buf.handle, (void **)&v_context->enc_buf.ptr);
	memset(v_context->enc_buf.ptr, 0, buf_size * 2);
	r = amdgpu_bo_cpu_unmap(v_context->cpb_buf.handle);

	len = 0;

	if (shared_context->vcn_unified_ring)
		amdgpu_cs_sq_head(v_context, context->ib_cpu, &len, true);

	/* session info */
	st_offset = len;
	st_size = &context->ib_cpu[len++];	/* size */
	context->ib_cpu[len++] = 0x00000001;	/* RENCODE_IB_PARAM_SESSION_INFO */
	context->ib_cpu[len++] = ((fw_maj << 16) | (fw_min << 0));
	context->ib_cpu[len++] = upper_32_bits(v_context->enc_buf.addr);
	context->ib_cpu[len++] = lower_32_bits(v_context->enc_buf.addr);
	context->ib_cpu[len++] = 1;	/* RENCODE_ENGINE_TYPE_ENCODE; */
	*st_size = (len - st_offset) * 4;

	/* task info */
	task_offset = len;
	st_offset = len;
	st_size = &context->ib_cpu[len++];	/* size */
	context->ib_cpu[len++] = 0x00000002;	/* RENCODE_IB_PARAM_TASK_INFO */
	p_task_size = &context->ib_cpu[len++];
	context->ib_cpu[len++] = v_context->enc_task_id++;	/* task_id */
	context->ib_cpu[len++] = 0;	/* feedback */
	*st_size = (len - st_offset) * 4;

	/* op init */
	st_offset = len;
	st_size = &context->ib_cpu[len++];	/* size */
	context->ib_cpu[len++] = 0x01000001;	/* RENCODE_IB_OP_INITIALIZE */
	*st_size = (len - st_offset) * 4;

	/* session_init */
	st_offset = len;
	st_size = &context->ib_cpu[len++];	/* size */
	context->ib_cpu[len++] = 0x00000003;	/* RENCODE_IB_PARAM_SESSION_INIT */
	context->ib_cpu[len++] = 1;	/* RENCODE_ENCODE_STANDARD_H264 */
	context->ib_cpu[len++] = width;
	context->ib_cpu[len++] = height;
	context->ib_cpu[len++] = 0;
	context->ib_cpu[len++] = 0;
	context->ib_cpu[len++] = 0;	/* pre encode mode */
	context->ib_cpu[len++] = 0;	/* chroma enabled : false */
	context->ib_cpu[len++] = 0;
	context->ib_cpu[len++] = 0;
	*st_size = (len - st_offset) * 4;

	/* slice control */
	st_offset = len;
	st_size = &context->ib_cpu[len++];	/* size */
	context->ib_cpu[len++] = 0x00200001;	/* RENCODE_H264_IB_PARAM_SLICE_CONTROL */
	context->ib_cpu[len++] = 0;	/* RENCODE_H264_SLICE_CONTROL_MODE_FIXED_MBS */
	context->ib_cpu[len++] = ALIGN(width, 16) / 16 * ALIGN(height, 16) / 16;
	*st_size = (len - st_offset) * 4;

	/* enc spec misc */
	st_offset = len;
	st_size = &context->ib_cpu[len++];	/* size */
	context->ib_cpu[len++] = 0x00200002;	/* RENCODE_H264_IB_PARAM_SPEC_MISC */
	context->ib_cpu[len++] = 0;	/* constrained intra pred flag */
	context->ib_cpu[len++] = 0;	/* cabac enable */
	context->ib_cpu[len++] = 0;	/* cabac init idc */
	if (shared_context->vcn_ip_version_major >= 5)
		context->ib_cpu[len++] = 0;	/* transform_8x8_mode_flag */
	context->ib_cpu[len++] = 1;	/* half pel enabled */
	context->ib_cpu[len++] = 1;	/* quarter pel enabled */
	context->ib_cpu[len++] = 100;	/* BASELINE profile */
	context->ib_cpu[len++] = 11;	/* level */
	if (shared_context->vcn_ip_version_major >= 3) {
		context->ib_cpu[len++] = 0;	/* b_picture_enabled */
		context->ib_cpu[len++] = 0;	/* weighted_bipred_idc */
	}
	*st_size = (len - st_offset) * 4;

	/* deblocking filter */
	st_offset = len;
	st_size = &context->ib_cpu[len++];	/* size */
	context->ib_cpu[len++] = 0x00200004;	/* RENCODE_H264_IB_PARAM_DEBLOCKING_FILTER */
	context->ib_cpu[len++] = 0;	/* disable deblocking filter idc */
	context->ib_cpu[len++] = 0;	/* alpha c0 offset */
	context->ib_cpu[len++] = 0;	/* tc offset */
	context->ib_cpu[len++] = 0;	/* cb offset */
	context->ib_cpu[len++] = 0;	/* cr offset */
	*st_size = (len - st_offset) * 4;

	/* layer control */
	st_offset = len;
	st_size = &context->ib_cpu[len++];	/* size */
	context->ib_cpu[len++] = 0x00000004;	/* RENCODE_IB_PARAM_LAYER_CONTROL */
	context->ib_cpu[len++] = 1;	/* max temporal layer */
	context->ib_cpu[len++] = 1;	/* no of temporal layer */
	*st_size = (len - st_offset) * 4;

	/* rc_session init */
	st_offset = len;
	st_size = &context->ib_cpu[len++];	/* size */
	context->ib_cpu[len++] = 0x00000006;	/* RENCODE_IB_PARAM_RATE_CONTROL_SESSION_INIT */
	context->ib_cpu[len++] = 0;	/* rate control */
	context->ib_cpu[len++] = 48;	/* vbv buffer level */
	*st_size = (len - st_offset) * 4;

	/* quality params */
	st_offset = len;
	st_size = &context->ib_cpu[len++];	/* size */
	context->ib_cpu[len++] = 0x00000009;	/* RENCODE_IB_PARAM_QUALITY_PARAMS */
	context->ib_cpu[len++] = 0;	/* vbaq mode */
	context->ib_cpu[len++] = 0;	/* scene change sensitivity */
	context->ib_cpu[len++] = 0;	/* scene change min idr interval */
	context->ib_cpu[len++] = 0;
	if (shared_context->vcn_ip_version_major >= 3)
		context->ib_cpu[len++] = 0;
	*st_size = (len - st_offset) * 4;

	/* layer select */
	st_offset = len;
	st_size = &context->ib_cpu[len++];	/* size */
	context->ib_cpu[len++] = 0x00000005;	/* RENCODE_IB_PARAM_LAYER_SELECT */
	context->ib_cpu[len++] = 0;	/* temporal layer */
	*st_size = (len - st_offset) * 4;

	/* rc layer init */
	st_offset = len;
	st_size = &context->ib_cpu[len++];	/* size */
	context->ib_cpu[len++] = 0x00000007;	/* RENCODE_IB_PARAM_RATE_CONTROL_LAYER_INIT */
	context->ib_cpu[len++] = 0;
	context->ib_cpu[len++] = 0;
	context->ib_cpu[len++] = 25;
	context->ib_cpu[len++] = 1;
	context->ib_cpu[len++] = 0x01312d00;
	context->ib_cpu[len++] = 0;
	context->ib_cpu[len++] = 0;
	context->ib_cpu[len++] = 0;
	*st_size = (len - st_offset) * 4;

	/* layer select */
	st_offset = len;
	st_size = &context->ib_cpu[len++];	/* size */
	context->ib_cpu[len++] = 0x00000005;	/* RENCODE_IB_PARAM_LAYER_SELECT */
	context->ib_cpu[len++] = 0;	/* temporal layer */
	*st_size = (len - st_offset) * 4;

	/* rc per pic */
	st_offset = len;
	st_size = &context->ib_cpu[len++];	/* size */
	context->ib_cpu[len++] = 0x00000008;	/* RENCODE_IB_PARAM_RATE_CONTROL_PER_PICTURE */
	context->ib_cpu[len++] = 20;
	context->ib_cpu[len++] = 0;
	context->ib_cpu[len++] = 51;
	context->ib_cpu[len++] = 0;
	context->ib_cpu[len++] = 1;
	context->ib_cpu[len++] = 0;
	context->ib_cpu[len++] = 1;
	context->ib_cpu[len++] = 0;
	*st_size = (len - st_offset) * 4;

	/* op init rc */
	st_offset = len;
	st_size = &context->ib_cpu[len++];	/* size */
	context->ib_cpu[len++] = 0x01000004;	/* RENCODE_IB_OP_INIT_RC */
	*st_size = (len - st_offset) * 4;

	/* op init rc vbv */
	st_offset = len;
	st_size = &context->ib_cpu[len++];	/* size */
	context->ib_cpu[len++] = 0x01000005;	/* RENCODE_IB_OP_INIT_RC_VBV_BUFFER_LEVEL */
	*st_size = (len - st_offset) * 4;

	*p_task_size = (len - task_offset) * 4;

	if (shared_context->vcn_unified_ring)
		amdgpu_cs_sq_ib_tail(v_context, context->ib_cpu + len);

	r = submit(device_handle, context, len, AMDGPU_HW_IP_VCN_ENC);
	igt_assert_eq(r, 0);
}

static void
amdgpu_cs_vcn_ib_zero_count(struct mmd_context *context, int *len, int num)
{
	int i;

	for (i = 0; i < num; i++)
		context->ib_cpu[(*len)++] = 0;
}

static int32_t
h264_se(struct buffer_info *buf_info)
{
	uint32_t ret, temp;

	ret = bs_read_ue(buf_info);
	if ((ret & 0x1) == 0) {
		ret >>= 1;
		temp = 0 - ret;
		return temp;
	}

	return (ret + 1) >> 1;
}

static void
h264_check_0s(struct buffer_info *buf_info, int count)
{
	uint32_t val;

	val = bs_read_u(buf_info, count);
	if (val != 0)
		igt_info("field error - %d bits should be 0 is %x\n", count, val);
}

static int
bs_eof(struct buffer_info *buf_info)
{
	if (buf_info->dec_buffer >= buf_info->end)
		return 1;
	else
		return 0;
}

static uint32_t
bs_read_u1(struct buffer_info *buf_info)
{
	uint32_t r = 0;
	uint32_t temp = 0;

	buf_info->num_bits_in_buffer--;
	if (!bs_eof(buf_info)) {
		temp = (((buf_info->dec_data)) >> buf_info->num_bits_in_buffer);
		r = temp & 0x01;
	}

	if (buf_info->num_bits_in_buffer == 0) {
		buf_info->dec_buffer++;
		buf_info->dec_data = *buf_info->dec_buffer;
		buf_info->num_bits_in_buffer = 8;
	}

	return r;
}

static uint32_t
bs_read_u(struct buffer_info *buf_info, int n)
{
	uint32_t r = 0;
	int i;

	for (i = 0; i < n; i++)
		r |= (bs_read_u1(buf_info) << (n - i - 1));

	return r;
}

static uint32_t
bs_read_ue(struct buffer_info *buf_info)
{
	int32_t r = 0;
	int i = 0;

	while ((bs_read_u1(buf_info) == 0) && (i < 32) && (!bs_eof(buf_info)))
		i++;
	r = bs_read_u(buf_info, i);
	r += (1 << i) - 1;
	return r;
}

static uint32_t
remove_03(uint8_t *bptr, uint32_t len)
{
	uint32_t nal_len = 0;

	while (nal_len + 2 < len) {
		if (bptr[0] == 0 && bptr[1] == 0 && bptr[2] == 3) {
			bptr += 2;
			nal_len += 2;
			len--;
			memmove(bptr, bptr + 1, len - nal_len);
		} else {
			bptr++;
			nal_len++;
		}
	}
	return len;
}

static void
scaling_list(uint32_t ix, uint32_t size_scaling_list, struct buffer_info *buf_info)
{
	uint32_t last_scale = 8, next_scale = 8;
	uint32_t jx;
	int delta_scale;

	for (jx = 0; jx < size_scaling_list; jx++) {
		if (next_scale != 0) {
			delta_scale = h264_se(buf_info);
			next_scale = (last_scale + delta_scale + 256) % 256;
		}
		if (next_scale != 0)
			last_scale = next_scale;
	}
}

static void
h264_parse_sequence_parameter_set(struct h264_decode *dec, struct buffer_info *buf_info)
{
	uint32_t temp, seq_scaling_matrix_present_flag, pic_order_cnt_type,
				pic_width_in_mbs, pic_height_in_map_units, frame_mbs_only_flag;

	dec->profile = bs_read_u(buf_info, 8);
	bs_read_u(buf_info, 1);		/* constaint_set0_flag */
	bs_read_u(buf_info, 1);		/* constaint_set1_flag */
	bs_read_u(buf_info, 1);		/* constaint_set2_flag */
	bs_read_u(buf_info, 1);		/* constaint_set3_flag */
	bs_read_u(buf_info, 1);		/* constaint_set4_flag */
	bs_read_u(buf_info, 1);		/* constaint_set5_flag */


	h264_check_0s(buf_info, 2);
	dec->level_idc = bs_read_u(buf_info, 8);
	bs_read_ue(buf_info);	/* SPS id*/
	if (dec->profile == 100 || dec->profile == 110 ||
		dec->profile == 122 || dec->profile == 144) {
		uint32_t chroma_format_idc = bs_read_ue(buf_info);

		if (chroma_format_idc == 3)
			bs_read_u(buf_info, 1);	/* residual_colour_transform_flag */

		bs_read_ue(buf_info);	/* bit_depth_luma_minus8 */
		bs_read_ue(buf_info);	/* bit_depth_chroma_minus8 */
		bs_read_u(buf_info, 1);	/* qpprime_y_zero_transform_bypass_flag */
		seq_scaling_matrix_present_flag = bs_read_u(buf_info, 1);

		if (seq_scaling_matrix_present_flag) {
			for (uint32_t ix = 0; ix < 8; ix++) {
				temp = bs_read_u(buf_info, 1);
				if (temp)
					scaling_list(ix, ix < 6 ? 16 : 64, buf_info);
			}
		}
	}

	bs_read_ue(buf_info);	/* log2_max_frame_num_minus4 */
	pic_order_cnt_type = bs_read_ue(buf_info);

	if (pic_order_cnt_type == 0) {
		bs_read_ue(buf_info);	/* log2_max_pic_order_cnt_lsb_minus4 */
	} else if (pic_order_cnt_type == 1) {
		bs_read_u(buf_info, 1);	/* delta_pic_order_always_zero_flag */
		h264_se(buf_info);	/* offset_for_non_ref_pic */
		h264_se(buf_info);	/* offset_for_top_to_bottom_field */
		temp = bs_read_ue(buf_info);
		for (uint32_t ix = 0; ix < temp; ix++)
			h264_se(buf_info);	/* offset_for_ref_frame[index] */
	}
	bs_read_ue(buf_info);	/* num_ref_frames */
	bs_read_u(buf_info, 1);	/* gaps_in_frame_num_flag */
	pic_width_in_mbs = bs_read_ue(buf_info) + 1;

	dec->pic_width = pic_width_in_mbs * 16;
	pic_height_in_map_units = bs_read_ue(buf_info) + 1;

	dec->pic_height = pic_height_in_map_units * 16;
	frame_mbs_only_flag = bs_read_u(buf_info, 1);
	if (!frame_mbs_only_flag)
		bs_read_u(buf_info, 1);	/* mb_adaptive_frame_field_flag */
	bs_read_u(buf_info, 1);	/* direct_8x8_inference_flag */
	temp = bs_read_u(buf_info, 1);
	if (temp) {
		bs_read_ue(buf_info);	/* frame_crop_left_offset */
		bs_read_ue(buf_info);	/* frame_crop_right_offset */
		bs_read_ue(buf_info);	/* frame_crop_top_offset */
		bs_read_ue(buf_info);	/* frame_crop_bottom_offset */
	}
	temp = bs_read_u(buf_info, 1);	/* VUI Parameters  */
}

static void
h264_slice_header(struct h264_decode *dec, struct buffer_info *buf_info)
{
	uint32_t temp;

	bs_read_ue(buf_info);	/* first_mb_in_slice */
	temp = bs_read_ue(buf_info);
	dec->slice_type = ((temp > 5) ? (temp - 5) : temp);
}

static uint8_t
h264_parse_nal(struct h264_decode *dec, struct buffer_info *buf_info)
{
	uint8_t type = 0;

	h264_check_0s(buf_info, 1);
	dec->nal_ref_idc = bs_read_u(buf_info, 2);
	dec->nal_unit_type = type = bs_read_u(buf_info, 5);
	switch (type) {
	case H264_NAL_TYPE_NON_IDR_SLICE:
	case H264_NAL_TYPE_IDR_SLICE:
		h264_slice_header(dec, buf_info);
		break;
	case H264_NAL_TYPE_SEQ_PARAM:
		h264_parse_sequence_parameter_set(dec, buf_info);
		break;
	case H264_NAL_TYPE_PIC_PARAM:
	case H264_NAL_TYPE_SEI:
	case H264_NAL_TYPE_ACCESS_UNIT:
	case H264_NAL_TYPE_SEQ_EXTENSION:
		/* NOP */
		break;
	default:
		igt_info("Nal type unknown %d\n", type);
		break;
	}
	return type;
}

static uint32_t
h264_find_next_start_code(uint8_t *pBuf, uint32_t bufLen)
{
	uint32_t val;
	uint32_t offset, startBytes;

	offset = startBytes = 0;
	if (pBuf[0] == 0 && pBuf[1] == 0 && pBuf[2] == 0 && pBuf[3] == 1) {
		pBuf += 4;
		offset = 4;
		startBytes = 1;
	} else if (pBuf[0] == 0 && pBuf[1] == 0 && pBuf[2] == 1) {
		pBuf += 3;
		offset = 3;
		startBytes = 1;
	}
	val = 0xffffffff;
	while (offset < bufLen - 3) {
		val <<= 8;
		val |= *pBuf++;
		offset++;
		if (val == H264_START_CODE)
			return offset - 4;

		if ((val & 0x00ffffff) == H264_START_CODE)
			return offset - 3;
	}
	if (bufLen - offset <= 3 && startBytes == 0) {
		startBytes = 0;
		return 0;
	}

	return offset;
}

static int
verify_checksum(struct vcn_context *v_context, uint8_t *buffer, uint32_t buffer_size)
{
	uint32_t buffer_pos = 0;
	struct h264_decode dec;
	int done = 0;

	memset(&dec, 0, sizeof(struct h264_decode));
	do {
		uint32_t ret;

		ret = h264_find_next_start_code(buffer + buffer_pos,
				 buffer_size - buffer_pos);
		if (ret == 0) {
			done = 1;
			if (buffer_pos == 0)
				igt_info("couldn't find start code in buffer from 0\n");
		} else {
		/* have a complete NAL from buffer_pos to end */
			if (ret > 3) {
				uint32_t nal_len;
				struct buffer_info buf_info;

				nal_len = remove_03(buffer + buffer_pos, ret);
				buf_info.dec_buffer = buffer + buffer_pos + (buffer[buffer_pos + 2] == 1 ? 3 : 4);
				buf_info.dec_buffer_size = (nal_len - (buffer[buffer_pos + 2] == 1 ? 3 : 4)) * 8;
				buf_info.end = buffer + buffer_pos + nal_len;
				buf_info.num_bits_in_buffer = 8;
				buf_info.dec_data = *buf_info.dec_buffer;
				h264_parse_nal(&dec, &buf_info);
			}
			buffer_pos += ret;	/*  buffer_pos points to next code */
		}
	} while (done == 0);

	if ((dec.pic_width == v_context->gWidth) &&
		(dec.pic_height == v_context->gHeight) &&
		(dec.slice_type == v_context->gSliceType))
		return 0;
	else
		return -1;
}

static void
check_result(struct vcn_context *v_context, struct amdgpu_mmd_bo fb_buf,
		struct amdgpu_mmd_bo bs_buf, int frame_type)
{
	uint32_t *fb_ptr;
	uint8_t *bs_ptr;
	uint32_t size;
	int r;
/*	uint64_t s[3] = {0, 1121279001727, 1059312481445};	*/

	r = amdgpu_bo_cpu_map(fb_buf.handle, (void **)&fb_buf.ptr);
	igt_assert_eq(r, 0);
	fb_ptr = (uint32_t *)fb_buf.ptr;
	size = fb_ptr[6];
	r = amdgpu_bo_cpu_unmap(fb_buf.handle);
	igt_assert_eq(r, 0);
	r = amdgpu_bo_cpu_map(bs_buf.handle, (void **)&bs_buf.ptr);
	igt_assert_eq(r, 0);

	bs_ptr = (uint8_t *)bs_buf.ptr;
	r = verify_checksum(v_context, bs_ptr, size);
	igt_assert_eq(r, 0);
	r = amdgpu_bo_cpu_unmap(bs_buf.handle);

	igt_assert_eq(r, 0);
}

static void
amdgpu_cs_vcn_enc_encode_frame(amdgpu_device_handle device_handle,
			struct mmd_shared_context *shared_context,
			struct mmd_context *context, struct vcn_context *v_context,
			int frame_type)
{
	struct amdgpu_mmd_bo bs_buf, fb_buf, input_buf, meta_buf;
	int len, r, i;
	unsigned	int width = 160, height = 128, buf_size, luma_size;
	uint32_t *p_task_size = NULL;
	uint32_t task_offset = 0, st_offset;
	uint32_t *st_size = NULL;
	uint32_t fw_maj = 1, fw_min = 9;

	if (shared_context->vcn_ip_version_major == 2) {
		fw_maj = 1;
		fw_min = 1;
	} else if (shared_context->vcn_ip_version_major == 3) {
		fw_maj = 1;
		fw_min = 0;
	}
	v_context->gSliceType = frame_type;
	buf_size = ALIGN(width, 256) * ALIGN(height, 32) * 3 / 2;
	luma_size = ALIGN(width, 256) * ALIGN(height, 32);

	context->num_resources = 0;
	alloc_resource(device_handle, &bs_buf, 4096, AMDGPU_GEM_DOMAIN_GTT);
	alloc_resource(device_handle, &fb_buf, 4096, AMDGPU_GEM_DOMAIN_GTT);
	alloc_resource(device_handle, &input_buf, buf_size, AMDGPU_GEM_DOMAIN_GTT);
	alloc_resource(device_handle, &meta_buf, 1024, AMDGPU_GEM_DOMAIN_GTT);
	context->resources[context->num_resources++] = v_context->enc_buf.handle;
	context->resources[context->num_resources++] = v_context->cpb_buf.handle;
	context->resources[context->num_resources++] = bs_buf.handle;
	context->resources[context->num_resources++] = fb_buf.handle;
	context->resources[context->num_resources++] = input_buf.handle;
	context->resources[context->num_resources++] = meta_buf.handle;
	context->resources[context->num_resources++] = context->ib_handle;


	r = amdgpu_bo_cpu_map(bs_buf.handle, (void **)&bs_buf.ptr);
	memset(bs_buf.ptr, 0, 4096);
	r = amdgpu_bo_cpu_unmap(bs_buf.handle);

	r = amdgpu_bo_cpu_map(fb_buf.handle, (void **)&fb_buf.ptr);
	memset(fb_buf.ptr, 0, 4096);
	r = amdgpu_bo_cpu_unmap(fb_buf.handle);

	r = amdgpu_bo_cpu_map(meta_buf.handle, (void **)&meta_buf.ptr);
	igt_assert_eq(r, 0);

	memset(meta_buf.ptr, 0, 1024);

	r = amdgpu_bo_cpu_unmap(meta_buf.handle);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_cpu_map(input_buf.handle, (void **)&input_buf.ptr);
	igt_assert_eq(r, 0);

	for (i = 0; i < ALIGN(height, 32) * 3 / 2; i++)
		memcpy(input_buf.ptr + i * ALIGN(width, 256), frame + i * width, width);

	r = amdgpu_bo_cpu_unmap(input_buf.handle);
	igt_assert_eq(r, 0);

	len = 0;

	if (shared_context->vcn_unified_ring)
		amdgpu_cs_sq_head(v_context, context->ib_cpu, &len, true);

	/* session info */
	st_offset = len;
	st_size = &context->ib_cpu[len++];	/* size */
	context->ib_cpu[len++] = 0x00000001;	/* RENCODE_IB_PARAM_SESSION_INFO */
	context->ib_cpu[len++] = ((fw_maj << 16) | (fw_min << 0));
	context->ib_cpu[len++] = upper_32_bits(v_context->enc_buf.addr);
	context->ib_cpu[len++] = lower_32_bits(v_context->enc_buf.addr);
	context->ib_cpu[len++] = 1;	/* RENCODE_ENGINE_TYPE_ENCODE */;
	*st_size = (len - st_offset) * 4;

	/* task info */
	task_offset = len;
	st_offset = len;
	st_size = &context->ib_cpu[len++];	/* size */
	context->ib_cpu[len++] = 0x00000002;	/* RENCODE_IB_PARAM_TASK_INFO */
	p_task_size = &context->ib_cpu[len++];
	context->ib_cpu[len++] = v_context->enc_task_id++;	/* task_id */
	context->ib_cpu[len++] = 1;	/* feedback */
	*st_size = (len - st_offset) * 4;

	if (frame_type == 2) {
		/* sps */
		st_offset = len;
		st_size = &context->ib_cpu[len++];	/* size */
		if (shared_context->vcn_ip_version_major == 1)
			context->ib_cpu[len++] = 0x00000020;	/* RENCODE_IB_PARAM_DIRECT_OUTPUT_NALU vcn 1 */
		else
			context->ib_cpu[len++] = 0x0000000a;	/* RENCODE_IB_PARAM_DIRECT_OUTPUT_NALU other vcn */
		context->ib_cpu[len++] = 0x00000002;	/* RENCODE_DIRECT_OUTPUT_NALU_TYPE_SPS */
		context->ib_cpu[len++] = 0x00000011;	/* sps len */
		context->ib_cpu[len++] = 0x00000001;	/* start code */
		context->ib_cpu[len++] = 0x6764440b;
		context->ib_cpu[len++] = 0xac54c284;
		context->ib_cpu[len++] = 0x68078442;
		context->ib_cpu[len++] = 0x37000000;
		*st_size = (len - st_offset) * 4;

		/* pps */
		st_offset = len;
		st_size = &context->ib_cpu[len++];	/* size */
		if (shared_context->vcn_ip_version_major == 1)
			context->ib_cpu[len++] = 0x00000020;	/* RENCODE_IB_PARAM_DIRECT_OUTPUT_NALU vcn 1*/
		else
			context->ib_cpu[len++] = 0x0000000a;	/* RENCODE_IB_PARAM_DIRECT_OUTPUT_NALU other vcn*/
		context->ib_cpu[len++] = 0x00000003;	/* RENCODE_DIRECT_OUTPUT_NALU_TYPE_PPS */
		context->ib_cpu[len++] = 0x00000008;	/* pps len */
		context->ib_cpu[len++] = 0x00000001;	/* start code */
		context->ib_cpu[len++] = 0x68ce3c80;
		*st_size = (len - st_offset) * 4;
	}

	/* slice header */
	st_offset = len;
	st_size = &context->ib_cpu[len++];	/* size */
	if (shared_context->vcn_ip_version_major == 1)
		context->ib_cpu[len++] = 0x0000000a; /* RENCODE_IB_PARAM_SLICE_HEADER vcn 1 */
	else
		context->ib_cpu[len++] = 0x0000000b; /* RENCODE_IB_PARAM_SLICE_HEADER vcn 2,3 */
	if (frame_type == 2) {
		context->ib_cpu[len++] = 0x65000000;
		context->ib_cpu[len++] = 0x11040000;
	} else {
		context->ib_cpu[len++] = 0x41000000;
		context->ib_cpu[len++] = 0x34210000;
	}
	context->ib_cpu[len++] = 0xe0000000;
	amdgpu_cs_vcn_ib_zero_count(context, &len, 13);

	context->ib_cpu[len++] = 0x00000001;
	context->ib_cpu[len++] = 0x00000008;
	context->ib_cpu[len++] = 0x00020000;
	context->ib_cpu[len++] = 0x00000000;
	context->ib_cpu[len++] = 0x00000001;
	context->ib_cpu[len++] = 0x00000015;
	context->ib_cpu[len++] = 0x00020001;
	context->ib_cpu[len++] = 0x00000000;
	context->ib_cpu[len++] = 0x00000001;
	context->ib_cpu[len++] = 0x00000003;
	amdgpu_cs_vcn_ib_zero_count(context, &len, 22);

	*st_size = (len - st_offset) * 4;

	/* encode params */
	st_offset = len;
	st_size = &context->ib_cpu[len++];	/* size */
	if (shared_context->vcn_ip_version_major == 1)
		context->ib_cpu[len++] = 0x0000000b;	/* RENCODE_IB_PARAM_ENCODE_PARAMS vcn 1*/
	else
		context->ib_cpu[len++] = 0x0000000f;	/* RENCODE_IB_PARAM_ENCODE_PARAMS other vcn*/
	context->ib_cpu[len++] = frame_type;
	context->ib_cpu[len++] = 0x0001f000;
	context->ib_cpu[len++] = upper_32_bits(input_buf.addr);
	context->ib_cpu[len++] = lower_32_bits(input_buf.addr);
	context->ib_cpu[len++] = upper_32_bits(input_buf.addr + luma_size);
	context->ib_cpu[len++] = lower_32_bits(input_buf.addr + luma_size);
	context->ib_cpu[len++] = 0x00000100;
	context->ib_cpu[len++] = 0x00000080;
	context->ib_cpu[len++] = 0x00000000;
	if (shared_context->vcn_ip_version_major < 5)
		context->ib_cpu[len++] = 0xffffffff;
	context->ib_cpu[len++] = 0x00000000;
	*st_size = (len - st_offset) * 4;

	/* encode params h264 */
	st_offset = len;
	st_size = &context->ib_cpu[len++];	/* size */
	context->ib_cpu[len++] = 0x00200003;	/* RENCODE_H264_IB_PARAM_ENCODE_PARAMS */
	if (shared_context->vcn_ip_version_major <= 2) {
		context->ib_cpu[len++] = 0x00000000;
		context->ib_cpu[len++] = 0x00000000;
		context->ib_cpu[len++] = 0x00000000;
		context->ib_cpu[len++] = 0xffffffff;
	} else if (shared_context->vcn_ip_version_major < 5) {
		context->ib_cpu[len++] = 0x00000000;
		context->ib_cpu[len++] = 0x00000000;
		context->ib_cpu[len++] = 0x00000000;
		context->ib_cpu[len++] = 0x00000000;
		context->ib_cpu[len++] = 0x00000000;
		context->ib_cpu[len++] = 0x00000000;
		context->ib_cpu[len++] = 0x00000000;
		context->ib_cpu[len++] = 0xffffffff;
		context->ib_cpu[len++] = 0x00000000;
		context->ib_cpu[len++] = 0x00000000;
		context->ib_cpu[len++] = 0x00000000;
		context->ib_cpu[len++] = 0x00000000;
		context->ib_cpu[len++] = 0xffffffff;
		context->ib_cpu[len++] = 0x00000000;
		context->ib_cpu[len++] = 0x00000000;
		context->ib_cpu[len++] = 0x00000000;
		context->ib_cpu[len++] = 0x00000000;
		context->ib_cpu[len++] = 0x00000001;
	} else {
		context->ib_cpu[len++] = 0x00000000;	/* input_picture_structure */
		context->ib_cpu[len++] = 0x00000000;	/* input_pic_order_cnt */
		context->ib_cpu[len++] = 0x00000001;	/* is_reference */
		context->ib_cpu[len++] = 0x00000000;	/* is_long_term */
		context->ib_cpu[len++] = 0x00000000;	/* interlaced_mode */
		context->ib_cpu[len++] = 0xffffffff;
		for (i = 1; i < 32; i++)
			context->ib_cpu[len++] = 0x00000000;	/* ref_list0 */
		context->ib_cpu[len++] = 0x00000000;	/* num_ref_idx_l0_active */
		for (i = 0; i < 32; i++)
			context->ib_cpu[len++] = 0x00000000;	/* ref_list1 */
		context->ib_cpu[len++] = 0x00000000;	/* num_ref_idx_l1_active */
		context->ib_cpu[len++] = 0x00000000;	/* lsm_reference_pictures[0].list */
		context->ib_cpu[len++] = 0xffffffff;	/* lsm_reference_pictures[0].list_index */
		context->ib_cpu[len++] = 0x00000000;	/* lsm_reference_pictures[1].list */
		context->ib_cpu[len++] = 0xffffffff;	/* lsm_reference_pictures[1].list_index */
	}
	*st_size = (len - st_offset) * 4;

	/* encode context */
	st_offset = len;
	st_size = &context->ib_cpu[len++];	/* size */
	if (shared_context->vcn_ip_version_major == 1)
		context->ib_cpu[len++] = 0x0000000d;	/* ENCODE_CONTEXT_BUFFER  vcn 1 */
	else
		context->ib_cpu[len++] = 0x00000011;	/* ENCODE_CONTEXT_BUFFER  other vcn*/
	context->ib_cpu[len++] = upper_32_bits(v_context->cpb_buf.addr);
	context->ib_cpu[len++] = lower_32_bits(v_context->cpb_buf.addr);
	if (shared_context->vcn_ip_version_major < 5) {
		context->ib_cpu[len++] = 0x00000000;	/* swizzle mode */
		context->ib_cpu[len++] = 0x00000100;	/* luma pitch */
		context->ib_cpu[len++] = 0x00000100;	/* chroma pitch */
		context->ib_cpu[len++] = 0x00000002;	/* no reconstructed picture */
		context->ib_cpu[len++] = 0x00000000;	/* reconstructed pic 1 luma offset */
		context->ib_cpu[len++] = luma_size;	/* pic1 chroma offset */
		if (shared_context->vcn_ip_version_major == 4)
			amdgpu_cs_vcn_ib_zero_count(context, &len, 2);
		context->ib_cpu[len++] = luma_size * 3 / 2;	/* pic2 luma offset */
		context->ib_cpu[len++] = luma_size * 5 / 2;	/* pic2 chroma offset */

		amdgpu_cs_vcn_ib_zero_count(context, &len, 280);
	} else {	/* VCN5 */
		context->ib_cpu[len++] = 0x00000002;	/* num_reconstructed_pictures */
		for (i = 0; i < 2; i++) {
			context->ib_cpu[len++] = upper_32_bits(v_context->cpb_buf.addr);
			context->ib_cpu[len++] = lower_32_bits(v_context->cpb_buf.addr);
			context->ib_cpu[len++] = 0x00000100;	/* rec luma pitch */
			context->ib_cpu[len++] = upper_32_bits(v_context->cpb_buf.addr);
			context->ib_cpu[len++] = lower_32_bits(v_context->cpb_buf.addr);
			context->ib_cpu[len++] = 0x00000100;	/* rec chroma pitch */
			context->ib_cpu[len++] = upper_32_bits(v_context->cpb_buf.addr);
			context->ib_cpu[len++] = lower_32_bits(v_context->cpb_buf.addr);
			context->ib_cpu[len++] = 0x00000000;	/* chroma v pitch */
			context->ib_cpu[len++] = 0x00000001;	/* swizzle mode*/
			context->ib_cpu[len++] = upper_32_bits(meta_buf.addr);
			context->ib_cpu[len++] = lower_32_bits(meta_buf.addr);
			context->ib_cpu[len++] = 0xffffffff;	/* h264 colloc buffer offset */
			context->ib_cpu[len++] = 0x00000000;
			context->ib_cpu[len++] = 0x00000000;	/* meta_data_offset */
		}
		amdgpu_cs_vcn_ib_zero_count(context, &len, 15 * (32 + 34) + 6);
	}
	*st_size = (len - st_offset) * 4;

	if (shared_context->vcn_ip_version_major >= 5) {
		/* encode context override */
		st_offset = len;
		st_size = &context->ib_cpu[len++];	/* size */
		context->ib_cpu[len++] = 0x0000001d;	/* ENCODE_CONTEXT_BUFFER_OVERRIDE */
		context->ib_cpu[len++] = 0;	/* luma offset */
		context->ib_cpu[len++] = luma_size;	/* chroma offset */
		context->ib_cpu[len++] = 0;	/* chroma v offset */
		context->ib_cpu[len++] = luma_size * 3 / 2;	/* luma offset */
		context->ib_cpu[len++] = luma_size * 5 / 2;	/* chroma offset */
		context->ib_cpu[len++] = 0;	/* chroma v offset */
		amdgpu_cs_vcn_ib_zero_count(context, &len, (34 + 32) * 3);
		*st_size = (len - st_offset) * 4;

		/* meta_data */
		st_offset = len;
		st_size = &context->ib_cpu[len++];	/* size */
		context->ib_cpu[len++] = 0x0000001c;	/* META_DATA */
		context->ib_cpu[len++] = upper_32_bits(meta_buf.addr);
		context->ib_cpu[len++] = lower_32_bits(meta_buf.addr);
		context->ib_cpu[len++] = 0x00000000;
		*st_size = (len - st_offset) * 4;
	}

	/* bitstream buffer */
	st_offset = len;
	st_size = &context->ib_cpu[len++];	/* size */
	if (shared_context->vcn_ip_version_major == 1)
		context->ib_cpu[len++] = 0x0000000e;	/* VIDEO_BITSTREAM_BUFFER vcn 1 */
	else
		context->ib_cpu[len++] = 0x00000012;	/* VIDEO_BITSTREAM_BUFFER other vcn */
	context->ib_cpu[len++] = 0x00000000;	/* mode */
	context->ib_cpu[len++] = upper_32_bits(bs_buf.addr);
	context->ib_cpu[len++] = lower_32_bits(bs_buf.addr);
	context->ib_cpu[len++] = 0x0001f000;
	context->ib_cpu[len++] = 0x00000000;
	*st_size = (len - st_offset) * 4;

	/* feedback */
	st_offset = len;
	st_size = &context->ib_cpu[len++];	/* size */
	if (shared_context->vcn_ip_version_major == 1)
		context->ib_cpu[len++] = 0x00000010;	/* FEEDBACK_BUFFER vcn 1 */
	else
		context->ib_cpu[len++] = 0x00000015;	/* FEEDBACK_BUFFER vcn 2,3 */
	context->ib_cpu[len++] = 0x00000000;
	context->ib_cpu[len++] = upper_32_bits(fb_buf.addr);
	context->ib_cpu[len++] = lower_32_bits(fb_buf.addr);
	context->ib_cpu[len++] = 0x00000010;
	context->ib_cpu[len++] = 0x00000028;
	*st_size = (len - st_offset) * 4;

	/* intra refresh */
	st_offset = len;
	st_size = &context->ib_cpu[len++];
	if (shared_context->vcn_ip_version_major == 1)
		context->ib_cpu[len++] = 0x0000000c;	/* INTRA_REFRESH vcn 1 */
	else
		context->ib_cpu[len++] = 0x00000010;	/* INTRA_REFRESH vcn 2,3 */
	context->ib_cpu[len++] = 0x00000000;
	context->ib_cpu[len++] = 0x00000000;
	context->ib_cpu[len++] = 0x00000000;
	*st_size = (len - st_offset) * 4;

	if (shared_context->vcn_ip_version_major != 1) {
		/* Input Format */
		st_offset = len;
		st_size = &context->ib_cpu[len++];
		context->ib_cpu[len++] = 0x0000000c;
		context->ib_cpu[len++] = 0x00000000;	/* RENCODE_COLOR_VOLUME_G22_BT709 */
		context->ib_cpu[len++] = 0x00000000;
		context->ib_cpu[len++] = 0x00000000;
		context->ib_cpu[len++] = 0x00000000;
		context->ib_cpu[len++] = 0x00000000;
		context->ib_cpu[len++] = 0x00000000;	/* RENCODE_COLOR_BIT_DEPTH_8_BIT */
		context->ib_cpu[len++] = 0x00000000;	/* RENCODE_COLOR_PACKING_FORMAT_NV12 */
		*st_size = (len - st_offset) * 4;

		/* Output Format */
		st_offset = len;
		st_size = &context->ib_cpu[len++];
		context->ib_cpu[len++] = 0x0000000d;
		context->ib_cpu[len++] = 0x00000000;	/* RENCODE_COLOR_VOLUME_G22_BT709 */
		context->ib_cpu[len++] = 0x00000000;
		context->ib_cpu[len++] = 0x00000000;
		context->ib_cpu[len++] = 0x00000000;	/* RENCODE_COLOR_BIT_DEPTH_8_BIT */
		*st_size = (len - st_offset) * 4;
	}

	/* op_speed */
	st_offset = len;
	st_size = &context->ib_cpu[len++];
	context->ib_cpu[len++] = 0x01000006;	/* SPEED_ENCODING_MODE */
	*st_size = (len - st_offset) * 4;

	/* op_enc */
	st_offset = len;
	st_size = &context->ib_cpu[len++];
	context->ib_cpu[len++] = 0x01000003;
	*st_size = (len - st_offset) * 4;

	*p_task_size = (len - task_offset) * 4;

	if (shared_context->vcn_unified_ring)
		amdgpu_cs_sq_ib_tail(v_context, context->ib_cpu + len);

	r = submit(device_handle, context, len, AMDGPU_HW_IP_VCN_ENC);
	igt_assert_eq(r, 0);

	/* check result */
	check_result(v_context, fb_buf, bs_buf, frame_type);

	free_resource(&fb_buf);
	free_resource(&bs_buf);
	free_resource(&input_buf);
	free_resource(&meta_buf);
}

static void
amdgpu_cs_vcn_enc_encode(amdgpu_device_handle device_handle,
			struct mmd_shared_context *shared_context,
			struct mmd_context *context, struct vcn_context *v_context)
{
	amdgpu_cs_vcn_enc_encode_frame(device_handle, shared_context,
			context, v_context, 2);	/* IDR frame */
}

static void
amdgpu_cs_vcn_enc_destroy(amdgpu_device_handle device_handle,
			struct mmd_shared_context *shared_context,
			struct mmd_context *context,
			struct vcn_context *v_context)
{
	int len = 0, r;
	uint32_t *p_task_size = NULL;
	uint32_t task_offset = 0, st_offset;
	uint32_t *st_size = NULL;
	uint32_t fw_maj = 1, fw_min = 9;

	if (shared_context->vcn_ip_version_major == 2) {
		fw_maj = 1;
		fw_min = 1;
	} else if (shared_context->vcn_ip_version_major == 3) {
		fw_maj = 1;
		fw_min = 0;
	}

	context->num_resources = 0;
	//alloc_resource(&v_context->enc_buf, 128 * 1024, AMDGPU_GEM_DOMAIN_GTT);
	context->resources[context->num_resources++] = v_context->enc_buf.handle;
	context->resources[context->num_resources++] = context->ib_handle;

	if (shared_context->vcn_unified_ring)
		amdgpu_cs_sq_head(v_context, context->ib_cpu, &len, true);

	/* session info */
	st_offset = len;
	st_size = &context->ib_cpu[len++];	/* size */
	context->ib_cpu[len++] = 0x00000001;	/* RENCODE_IB_PARAM_SESSION_INFO */
	context->ib_cpu[len++] = ((fw_maj << 16) | (fw_min << 0));
	context->ib_cpu[len++] = upper_32_bits(v_context->enc_buf.addr);
	context->ib_cpu[len++] = lower_32_bits(v_context->enc_buf.addr);
	context->ib_cpu[len++] = 1;	/* RENCODE_ENGINE_TYPE_ENCODE; */
	*st_size = (len - st_offset) * 4;

	/* task info */
	task_offset = len;
	st_offset = len;
	st_size = &context->ib_cpu[len++];	/* size */
	context->ib_cpu[len++] = 0x00000002;	/* RENCODE_IB_PARAM_TASK_INFO */
	p_task_size = &context->ib_cpu[len++];
	context->ib_cpu[len++] = v_context->enc_task_id++;	/* task_id */
	context->ib_cpu[len++] = 0;	/* feedback */
	*st_size = (len - st_offset) * 4;

	/*  op close */
	st_offset = len;
	st_size = &context->ib_cpu[len++];
	context->ib_cpu[len++] = 0x01000002;	/* RENCODE_IB_OP_CLOSE_SESSION */
	*st_size = (len - st_offset) * 4;

	*p_task_size = (len - task_offset) * 4;

	if (shared_context->vcn_unified_ring)
		amdgpu_cs_sq_ib_tail(v_context, context->ib_cpu + len);

	r = submit(device_handle, context, len, AMDGPU_HW_IP_VCN_ENC);
	igt_assert_eq(r, 0);

	free_resource(&v_context->cpb_buf);
	free_resource(&v_context->enc_buf);
}

int igt_main()
{
	amdgpu_device_handle device;
	struct mmd_context context = {};
	struct vcn_context v_context = {};
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
		igt_skip_on(!is_vcn_tests_enable(device, &shared_context));
		igt_skip_on_f(!shared_context.dec_ring && !shared_context.enc_ring, "vcn no decorder and encoder rings\n");
	}

	igt_describe("Test whether vcn decorder is created, decodes, destroyed");
	igt_subtest_with_dynamic("vcn-decoder-create-decode-destroy") {
		if (shared_context.dec_ring) {
			igt_dynamic_f("vcn-decoder-create")
			amdgpu_cs_vcn_dec_create(device, &shared_context, &context, &v_context);
			igt_dynamic_f("vcn-decoder-decode")
			amdgpu_cs_vcn_dec_decode(device, &shared_context, &context, &v_context);
			igt_dynamic_f("vcn-decoder-destroy")
			amdgpu_cs_vcn_dec_destroy(device, &shared_context, &context, &v_context);
		}
	}

	igt_describe("Test whether vcn encoder is created, encodes, destroyed");
	igt_subtest_with_dynamic("vcn-encoder-create-encode-destroy") {
		if (shared_context.enc_ring) {
			igt_dynamic_f("vcn-encoder-create")
			amdgpu_cs_vcn_enc_create(device, &shared_context, &context, &v_context);
			igt_dynamic_f("vcn-encoder-encodes")
			amdgpu_cs_vcn_enc_encode(device, &shared_context, &context, &v_context);
			igt_dynamic_f("vcn-encoder-destroy")
			amdgpu_cs_vcn_enc_destroy(device, &shared_context, &context, &v_context);
		}
	}

	igt_fixture() {
		mmd_context_clean(device, &context);
		amdgpu_device_deinitialize(device);
		drm_close_driver(fd);
	}

}
