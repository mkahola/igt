// SPDX-License-Identifier: MIT
// Copyright 2025 Advanced Micro Devices, Inc.

#include "amd_jpeg_shared.h"

bool
is_jpeg_tests_enable(amdgpu_device_handle device_handle,
		struct mmd_shared_context *context)
{
	struct drm_amdgpu_info_hw_ip info;
	int r;

	r = amdgpu_query_hw_ip_info(device_handle, AMDGPU_HW_IP_VCN_JPEG, 0, &info);

	if (r != 0 || !info.available_rings ||
			(context->family_id < AMDGPU_FAMILY_RV &&
			(context->family_id == AMDGPU_FAMILY_AI &&
			(context->chip_id - context->chip_rev) < 0x32))) { /* Arcturus */
		igt_info("\n\nThe ASIC NOT support JPEG, test disabled\n");
		return false;
	}

	if (info.hw_ip_version_major == 1)
		context->jpeg_direct_reg = false;
	else if (info.hw_ip_version_major > 1 && info.hw_ip_version_major <= 5)
		context->jpeg_direct_reg = true;
	else
		return false;

	context->vcn_ip_version_major = info.hw_ip_version_major;
	context->vcn_ip_version_minor = info.hw_ip_version_minor;
	jrbc_ib_cond_rd_timer = vcnipUVD_JRBC_IB_COND_RD_TIMER;
	jrbc_ib_ref_data = vcnipUVD_JRBC_IB_REF_DATA;
	jpeg_rb_base = vcnipUVD_JPEG_RB_BASE;
	jpeg_rb_size = vcnipUVD_JPEG_RB_SIZE;
	jpeg_rb_wptr = vcnipUVD_JPEG_RB_WPTR;
	jpeg_int_en = vcnipUVD_JPEG_INT_EN;
	jpeg_cntl = vcnipUVD_JPEG_CNTL;
	jpeg_rb_rptr = vcnipUVD_JPEG_RB_RPTR;

	if ((context->vcn_ip_version_major >= 5) ||
		((context->family_id == AMDGPU_FAMILY_AI) &&
		((context->chip_id - context->chip_rev) > 0x3c))) { /* gfx940 */
		jpeg_dec_soft_rst = vcnipUVD_JPEG_DEC_SOFT_RST_1;
		lmi_jpeg_read_64bit_bar_high = vcnipUVD_LMI_JPEG_READ_64BIT_BAR_HIGH_1;
		lmi_jpeg_read_64bit_bar_low = vcnipUVD_LMI_JPEG_READ_64BIT_BAR_LOW_1;
		jpeg_pitch = vcnipUVD_JPEG_PITCH_1;
		jpeg_uv_pitch = vcnipUVD_JPEG_UV_PITCH_1;
		dec_addr_mode = vcnipJPEG_DEC_ADDR_MODE_1;
		dec_y_gfx10_tiling_surface = vcnipJPEG_DEC_Y_GFX10_TILING_SURFACE_1;
		dec_uv_gfx10_tiling_surface = vcnipJPEG_DEC_UV_GFX10_TILING_SURFACE_1;
		lmi_jpeg_write_64bit_bar_high = vcnipUVD_LMI_JPEG_WRITE_64BIT_BAR_HIGH_1;
		lmi_jpeg_write_64bit_bar_low = vcnipUVD_LMI_JPEG_WRITE_64BIT_BAR_LOW_1;
		jpeg_tier_cntl2 = vcnipUVD_JPEG_TIER_CNTL2_1;
		jpeg_outbuf_cntl = vcnipUVD_JPEG_OUTBUF_CNTL_1;
		jpeg_outbuf_rptr = vcnipUVD_JPEG_OUTBUF_RPTR_1;
		jpeg_outbuf_wptr = vcnipUVD_JPEG_OUTBUF_WPTR_1;
		jpeg_luma_base0_0 = vcnipUVD_JPEG_LUMA_BASE0_0;
		jpeg_chroma_base0_0 = vcnipUVD_JPEG_CHROMA_BASE0_0;
	} else {
		jpeg_dec_soft_rst = vcnipUVD_JPEG_DEC_SOFT_RST;
		lmi_jpeg_read_64bit_bar_high = vcnipUVD_LMI_JPEG_READ_64BIT_BAR_HIGH;
		lmi_jpeg_read_64bit_bar_low = vcnipUVD_LMI_JPEG_READ_64BIT_BAR_LOW;
		jpeg_pitch = vcnipUVD_JPEG_PITCH;
		jpeg_uv_pitch = vcnipUVD_JPEG_UV_PITCH;
		dec_addr_mode = vcnipJPEG_DEC_ADDR_MODE;
		dec_y_gfx10_tiling_surface = vcnipJPEG_DEC_Y_GFX10_TILING_SURFACE;
		dec_uv_gfx10_tiling_surface = vcnipJPEG_DEC_UV_GFX10_TILING_SURFACE;
		lmi_jpeg_write_64bit_bar_high = vcnipUVD_LMI_JPEG_WRITE_64BIT_BAR_HIGH;
		lmi_jpeg_write_64bit_bar_low = vcnipUVD_LMI_JPEG_WRITE_64BIT_BAR_LOW;
		jpeg_tier_cntl2 = vcnipUVD_JPEG_TIER_CNTL2;
		jpeg_outbuf_cntl = vcnipUVD_JPEG_OUTBUF_CNTL;
		jpeg_outbuf_rptr = vcnipUVD_JPEG_OUTBUF_RPTR;
		jpeg_outbuf_wptr = vcnipUVD_JPEG_OUTBUF_WPTR;
	}

	return true;
}

void
set_reg_jpeg(struct mmd_context *context, uint32_t reg, uint32_t cond,
		uint32_t type, uint32_t val, uint32_t *idx)
{
	context->ib_cpu[(*idx)++] = RDECODE_PKTJ(reg, cond, type);
	context->ib_cpu[(*idx)++] = val;
}

/* send a target buffer command */
void
send_cmd_target_direct(struct mmd_context *context, uint64_t addr,
		uint32_t *idx)
{

	set_reg_jpeg(context, jpeg_pitch, COND0, TYPE0,
			(JPEG_DEC_DT_PITCH >> 4), idx);
	set_reg_jpeg(context, jpeg_uv_pitch, COND0, TYPE0,
			(JPEG_DEC_DT_PITCH >> 4), idx);

	set_reg_jpeg(context, dec_addr_mode, COND0, TYPE0, 0, idx);
	set_reg_jpeg(context, dec_y_gfx10_tiling_surface, COND0, TYPE0,
			0, idx);
	set_reg_jpeg(context, dec_uv_gfx10_tiling_surface, COND0, TYPE0,
			0, idx);

	/* set UVD_LMI_JPEG_WRITE_64BIT_BAR_LOW/HIGH based on target buffer address */
	set_reg_jpeg(context, lmi_jpeg_write_64bit_bar_high, COND0, TYPE0,
			(addr >> 32), idx);
	set_reg_jpeg(context, lmi_jpeg_write_64bit_bar_low, COND0, TYPE0,
			addr, idx);

	/* set output buffer data address */
	if (jpeg_luma_base0_0) {
		set_reg_jpeg(context, jpeg_luma_base0_0, COND0, TYPE0,
			JPEG_DEC_LUMA_OFFSET, idx);
		set_reg_jpeg(context, jpeg_chroma_base0_0, COND0, TYPE0,
			JPEG_DEC_CHROMA_OFFSET, idx);
	} else {
		set_reg_jpeg(context, vcnipUVD_JPEG_INDEX, COND0, TYPE0, 0, idx);
		set_reg_jpeg(context, vcnipUVD_JPEG_DATA, COND0, TYPE0,
			JPEG_DEC_LUMA_OFFSET, idx);
		set_reg_jpeg(context, vcnipUVD_JPEG_INDEX, COND0, TYPE0, 1, idx);
		set_reg_jpeg(context, vcnipUVD_JPEG_DATA, COND0, TYPE0,
			JPEG_DEC_CHROMA_OFFSET, idx);
	}
	set_reg_jpeg(context, jpeg_tier_cntl2, COND0, 0, 0, idx);

	/* set output buffer read pointer */
	set_reg_jpeg(context, jpeg_outbuf_rptr, COND0, TYPE0, 0, idx);
	set_reg_jpeg(context, jpeg_outbuf_cntl, COND0, TYPE0,
				((0x00001587 & (~0x00000180L)) | (0x1 << 0x7) | (0x1 << 0x6)),
				 idx);

	/* enable error interrupts */
	set_reg_jpeg(context, jpeg_int_en, COND0, TYPE0, 0xFFFFFFFE, idx);

	/* start engine command */
	set_reg_jpeg(context, jpeg_cntl, COND0, TYPE0, 0xE, idx);

	/* wait for job completion, wait for job JBSI fetch done */
	set_reg_jpeg(context, jrbc_ib_ref_data, COND0, TYPE0,
			(JPEG_DEC_BSD_SIZE >> 2), idx);
	set_reg_jpeg(context, jrbc_ib_cond_rd_timer, COND0, TYPE0,
			0x01400200, idx);
	set_reg_jpeg(context, jpeg_rb_rptr, COND3, TYPE3, 0xFFFFFFFF, idx);

	/* wait for job jpeg outbuf idle */
	set_reg_jpeg(context, jrbc_ib_ref_data, COND0, TYPE0, 0xFFFFFFFF,
			idx);
	set_reg_jpeg(context, jpeg_outbuf_wptr, COND3, TYPE3, 0x00000001,
			idx);

	/* stop engine */
	set_reg_jpeg(context, jpeg_cntl, COND0, TYPE0, 0x4, idx);
}

/* send a bitstream buffer command */
void
send_cmd_bitstream_direct(struct mmd_context *context, uint64_t addr,
		uint32_t *idx)
{

	/* jpeg soft reset */
	set_reg_jpeg(context, jpeg_dec_soft_rst, COND0, TYPE0, 1, idx);

	/* ensuring the Reset is asserted in SCLK domain */
	set_reg_jpeg(context, jrbc_ib_cond_rd_timer, COND0, TYPE0, 0x01400200, idx);
	set_reg_jpeg(context, jrbc_ib_ref_data, COND0, TYPE0, (0x1 << 0x10), idx);
	set_reg_jpeg(context, jpeg_dec_soft_rst, COND3, TYPE3, (0x1 << 0x10), idx);

	/* wait mem */
	set_reg_jpeg(context, jpeg_dec_soft_rst, COND0, TYPE0, 0, idx);

	/* ensuring the Reset is de-asserted in SCLK domain */
	set_reg_jpeg(context, jrbc_ib_ref_data, COND0, TYPE0, (0 << 0x10), idx);
	set_reg_jpeg(context, jpeg_dec_soft_rst, COND3, TYPE3, (0x1 << 0x10), idx);

	/* set UVD_LMI_JPEG_READ_64BIT_BAR_LOW/HIGH based on bitstream buffer address */
	set_reg_jpeg(context, lmi_jpeg_read_64bit_bar_high, COND0, TYPE0,
			(addr >> 32), idx);
	set_reg_jpeg(context, lmi_jpeg_read_64bit_bar_low, COND0, TYPE0,
			addr, idx);

	/* set jpeg_rb_base */
	set_reg_jpeg(context, jpeg_rb_base, COND0, TYPE0, 0, idx);

	/* set jpeg_rb_base */
	set_reg_jpeg(context, jpeg_rb_size, COND0, TYPE0, 0xFFFFFFF0, idx);

	/* set jpeg_rb_wptr */
	set_reg_jpeg(context, jpeg_rb_wptr, COND0, TYPE0,
			(JPEG_DEC_BSD_SIZE >> 2), idx);
}
