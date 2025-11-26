/* SPDX-License-Identifier: MIT
 * Copyright 2014 Advanced Micro Devices, Inc.
 * Copyright 2022 Advanced Micro Devices, Inc.
 *  *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 *
 */
#include <amdgpu.h>
#include "amd_shaders.h"
#include <amdgpu_drm.h>

#define CODE_OFFSET 512
#define DATA_OFFSET 1024

#define SWAP_32(num) (((num & 0xff000000) >> 24) | \
				((num & 0x0000ff00) << 8) | \
				((num & 0x00ff0000) >> 8) | \
				((num & 0x000000ff) << 24))
/*
 * C equivalent of the shader_bin below. This tiny compute kernel performs a
 * busy-wait loop, then writes the constant value 42 into a GPU virtual
 * address passed via COMPUTE_USER_DATA_0..1. This shader is intentionally
 * simple and designed to work across all GFX generations.
 *
 * Host-side logic equivalent in C:
 *
 *	static void gpu_shader_standalone(volatile u32 *dst, u64 iters)
 *	{
 *		Busy loop comparable to the ISA spin, prevents compiler optimization.
 *		for (u64 i = 0; i <= iters; i++)
 *			asm volatile("" ::: "memory");
 *
 *		After spin completes, write sentinel value 42.
 *		*dst = 42;
 *	}
 *
 * The binary shader blob generated below executes equivalent logic on the GPU.
 */

/*
 * Shader ISA: spin_store_flat.s
 *
 * This kernel expects a 64-bit flat/global address provided in s[0:1]
 * (passed via COMPUTE_USER_DATA_0..1) and behaves as follows:
 *
 *	1) Initialize a counter register (s2 = 0).
 *	2) Increment s2 until it exceeds 0x0098967f (~10 million).
 *	3) When the loop completes, store the literal 42 at *(u32 *)(s1:s0).
 *	4) Terminate with s_endpgm.
 *
 * Notes:
 *	- Uses flat/global addressing — no buffer SRD required.
 *	- Works with 1x1x1 dispatches; single work-item.
 *	- Safe for all GFX generations; no HW-specific instructions.
 *
 * Annotated assembly:
 *
 *	.text
 *	.p2align 8
 *	.globl spin_store_flat
 *	.type	spin_store_flat,@function
 *
 * spin_store_flat:
 *	s_mov_b32 s2, 0				Initialize loop counter.
 *
 *	s_cmp_gt_u32 s2, 0x0098967f		Early exit if counter exceeds limit.
 *	s_cbranch_scc1 .Lexit
 *
 * .Lloop:
 *	s_add_i32 s2, s2, 1			Increment counter.
 *	s_cmp_gt_u32 s2, 0x0098967f		Continue looping if below threshold.
 *	s_cbranch_scc0 .Lloop
 *
 *	v_mov_b32_e32 v0, 42			Load value 42 into v0.
 *	v_mov_b32_e32 v1, s0			Load address low bits into v1.
 *	v_mov_b32_e32 v2, s1			Load address high bits into v2.
 *
 *	flat_store_dword v[1:2], v0		Store 42 to *(u32 *)(s1:s0).
 *
 * .Lexit:
 *	s_endpgm				End program.
 *
 *	.size spin_store_flat, .-spin_store_flat
 */


static  const
uint32_t shader_bin[] = {
	SWAP_32(0x800082be), SWAP_32(0x02ff08bf), SWAP_32(0x7f969800), SWAP_32(0x040085bf),
	SWAP_32(0x02810281), SWAP_32(0x02ff08bf), SWAP_32(0x7f969800), SWAP_32(0xfcff84bf),
	SWAP_32(0xff0083be), SWAP_32(0x00f00000), SWAP_32(0xc10082be), SWAP_32(0xaa02007e),
	SWAP_32(0x000070e0), SWAP_32(0x00000080), SWAP_32(0x000081bf)
};

const uint32_t *
get_shader_bin(uint32_t *size_bytes, uint32_t *code_offset, uint32_t *data_offset)
{
	*size_bytes = sizeof(shader_bin);
	*code_offset =  CODE_OFFSET;
	*data_offset = DATA_OFFSET;
	return shader_bin;
}

struct amdgpu_test_shader {
	uint32_t *shader;
	uint32_t header_length;
	uint32_t body_length;
	uint32_t foot_length;
};

int amdgpu_dispatch_load_cs_shader_hang_slow(uint32_t *ptr, uint32_t family_id)
{
	/**
	 * v_sub_f32_e32 v0, s8, v134
	 * buffer_load_format_xyzw v[1:4], v0, s[0:3], 0 idxen
	 * ;;
	 * s_waitcnt vmcnt(0)
	 * buffer_store_format_xyzw v[1:4], v0, s[4:7], 0 idxen
	 * ;;
	 * s_endpgm
	 */
	unsigned int memcpy_cs_hang_slow_ai_codes[] = {
	    0xd1fd0000, 0x04010c08, 0xe00c2000, 0x80000100,
	    0xbf8c0f70, 0xe01c2000, 0x80010100, 0xbf810000
	};

	struct amdgpu_test_shader memcpy_cs_hang_slow_ai = {
		memcpy_cs_hang_slow_ai_codes,
		4,
		3,
		1
	};
	/**
	 * s_lshl_b32 s0, s12, 6
	 * v_add_u32_e32 v0, vcc, s0, v0
	 * buffer_load_format_xyzw v[1:4], v0, s[4:7], 0 idxen
	 * ;;
	 * s_waitcnt vmcnt(0)
	 * buffer_store_format_xyzw v[1:4], v0, s[8:11], 0 idxen
	 * ;;
	 * s_endpgm
	 */
	unsigned int memcpy_cs_hang_slow_rv_codes[] = {
	    0x8e00860c, 0x32000000, 0xe00c2000, 0x80010100,
	    0xbf8c0f70, 0xe01c2000, 0x80020100, 0xbf810000
	};

	struct amdgpu_test_shader memcpy_cs_hang_slow_rv = {
		memcpy_cs_hang_slow_rv_codes,
		4,
		3,
		1
	};
	/**
	 * v_interp_mov_f32_e32 v209, p10, attr0.x
	 * v_sub_f32_e32 v0, s8, v134
	 * buffer_load_format_xyzw v[1:4], v0, s[0:3], 0 idxen
	 * ;;
	 * s_waitcnt vmcnt(0)
	 * buffer_store_format_xyzw v[1:4], v0, s[4:7], 0 idxen
	 * ;;
	 * s_endpgm
	 */
	unsigned int memcpy_cs_hang_slow_nv_codes[] = {
	    0xd7460000, 0x04010c08, 0xe00c2000, 0x80000100,
	    0xbf8c0f70, 0xe01ca000, 0x80010100, 0xbf810000
	};
	struct amdgpu_test_shader memcpy_cs_hang_slow_nv = {
	        memcpy_cs_hang_slow_nv_codes,
	        4,
	        3,
	        1
	};
	struct amdgpu_test_shader *shader;
	int i, loop = 0x100000;

	switch (family_id) {
		case AMDGPU_FAMILY_AI:
			shader = &memcpy_cs_hang_slow_ai;
			break;
		case AMDGPU_FAMILY_RV:
			shader = &memcpy_cs_hang_slow_rv;
			break;
		case AMDGPU_FAMILY_NV:
		default:
			shader = &memcpy_cs_hang_slow_nv;
			break;
	}

	memcpy(ptr, shader->shader, shader->header_length * sizeof(uint32_t));

	for (i = 0; i < loop; i++)
		memcpy(ptr + shader->header_length + shader->body_length * i,
			shader->shader + shader->header_length,
			shader->body_length * sizeof(uint32_t));

	memcpy(ptr + shader->header_length + shader->body_length * loop,
		shader->shader + shader->header_length + shader->body_length,
		shader->foot_length * sizeof(uint32_t));

	return 0;
}

int  amdgpu_dispatch_load_cs_shader(uint8_t *ptr, int cs_type, uint32_t version)
{
	/**
	 * v_and_b32_e32 v0, 0x3ff, v0
	 * ;;
	 * ...
	 * v_sub_f32_e32 v0, s8, v134
	 * v_mov_b32_e32 v1, 0
	 * v_mov_b32_e32 v2, s4
	 * mov_b32_e32 v3, s5
	 * v_mov_b32_e32 v4, s6
	 * v_mov_b32_e32 v5, s7
	 * buffer_store_format_xyzw v[2:5], v0, s[0:3], 0 idxen
	 * ;;
	 * s_waitcnt vmcnt(0) expcnt(0) lgkmcnt(0)
	 * s_endpgm
	 */
	static const uint32_t bufferclear_cs_shader_gfx9[] = {
	    0x260000ff, 0x000003ff, 0xd1fd0000, 0x04010c08,
	    0x7e020280, 0x7e040204, 0x7e060205, 0x7e080206,
	    0x7e0a0207, 0xe01c2000, 0x80000200, 0xbf8c0000,
	    0xbf810000
	};

	/**
	 *
	 * v_and_b32_e32 v0, 0x3ff, v0
	 * ;;
	 * ...
	 * v_sub_f32_e32 v0, s8, v134
	 * v_mov_b32_e32 v1, 0
	 * buffer_load_format_xyzw v[2:5], v0, s[0:3], 0 idxen
	 * ;;
	 * s_waitcnt vmcnt(0)
	 * buffer_store_format_xyzw v[2:5], v0, s[4:7], 0 idxen
	 * ;;
	 * s_endpgm
	 */
	static const uint32_t buffercopy_cs_shader_gfx9[] = {
	    0x260000ff, 0x000003ff, 0xd1fd0000, 0x04010c08,
	    0x7e020280, 0xe00c2000, 0x80000200, 0xbf8c0f70,
	    0xe01c2000, 0x80010200, 0xbf810000
	};

	/**
	 * ...
	 * s_bcnt0_i32_b32 exec_lo, exec_lo
	 * ...
	 * ...
	 * s_dcache_inv
	 * ;;
	 * ...
	 * ...
	 * ...
	 * s_waitcnt lgkmcnt(0)
	 * image_sample v[0:3], v2, s[4:11], s[0:3] dmask:0xf
	 * ;;
	 * s_not_b32 exec_lo, s12
	 * s_waitcnt vmcnt(0)
	 * s_nop 0
	 * s_nop 0
	 * ...
	 * v_add_f32_e32 v129, v0, v0
	 *s_endpgm
	 */
	static const uint32_t memcpy_ps_hang[] = {
			0xFFFFFFFF, 0xBEFE0A7E, 0xBEFC0304, 0xC0C20100,
			0xC0800300, 0xC8080000, 0xC80C0100, 0xC8090001,
			0xC80D0101, 0xBF8C007F, 0xF0800F00, 0x00010002,
			0xBEFE040C, 0xBF8C0F70, 0xBF800000, 0xBF800000,
			0xF800180F, 0x03020100, 0xBF810000
	};

	/**
	 * v_interp_mov_f32_e32 v209, invalid_param_4, attr0.x
	 * v_sub_f32_e32 v0, s8, v134
	 * v_mov_b32_e32 v0, s4
	 * v_mov_b32_e32 v1, s5
	 * v_mov_b32_e32 v2, s6
	 * v_mov_b32_e32 v3, s7
	 * buffer_store_format_xyzw v[0:3], v4, s[0:3], 0 idxen
	 * ;;
	  *s_endpgm
	  */
	static const uint32_t bufferclear_cs_shader_gfx10[] = {
		0xD7460004, 0x04010C08, 0x7E000204, 0x7E020205,
		0x7E040206, 0x7E060207, 0xE01C2000, 0x80000004,
		0xBF810000
	};

	/**
	 * v_interp_mov_f32_e32 v209, p20, attr0.x
	 * v_sub_f32_e32 v0, s8, v134
	 * buffer_load_format_xyzw v[2:5], v1, s[0:3], 0 idxen
	 * ;;
	 * s_waitcnt vmcnt(0)
	 * buffer_store_format_xyzw v[2:5], v1, s[4:7], 0 idxen
	 * ;;
	 * s_endpgm
	 */
	static const uint32_t buffercopy_cs_shader_gfx10[] = {
		0xD7460001, 0x04010C08, 0xE00C2000, 0x80000201,
		0xBF8C3F70, 0xE01C2000, 0x80010201, 0xBF810000
	};

	/**
	 * shader main
	 * asic(GFX11)
	 * type(CS)
	 * s_version     UC_VERSION_GFX11 | UC_VERSION_W64_BIT   // 000000000000: B0802006
	 * s_set_inst_prefetch_distance  0x0003                  // 000000000004: BF840003
	 * v_and_b32     v0, lit(0x000003ff), v0                 // 000000000008: 360000FF 000003FF
	 * v_mov_b32     v1, s5                                  // 000000000010: 7E020205
	 * v_mov_b32     v2, s6                                  // 000000000014: 7E040206
	 * v_mov_b32     v3, s7                                  // 000000000018: 7E060207
	 * s_delay_alu   instid0(VALU_DEP_4)                     // 00000000001C: BF870004
	 * v_lshl_add_u32  v4, s8, 6, v0                         // 000000000020: D6460004 04010C08
	 * v_mov_b32     v0, s4                                  // 000000000028: 7E000204
	 * buffer_store_format_xyzw  v[0:3], v4, s[0:3], 0 idxen // 00000000002C: E01C0000 80800004
	 * s_sendmsg     sendmsg(MSG_DEALLOC_VGPRS, 0, 0)        // 000000000034: BFB60003
	 * s_endpgm                                              // 000000000038: BFB00000
	 */
	static const uint32_t bufferclear_cs_shader_gfx11[] = {
		0xB0802006, 0xBF840003, 0x360000FF, 0x000003FF,
		0x7E020205, 0x7E040206, 0x7E060207, 0xBF870004,
		0xD6460004, 0x04010C08, 0x7E000204, 0xE01C0000,
		0x80800004, 0xBFB60003, 0xBFB00000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000
	};

	static const uint32_t bufferclear_cs_shader_gfx12[] =
	{
		0xB0802009, 0xBF930006, 0x360000FF, 0x000003FF,
		0x7E020205, 0x7E040206, 0x7E060207, 0xBF870004,
		0xD6460004, 0x04010C75, 0x7E000204, 0xC401C07C,
		0x80000000, 0x00000004, 0xBFB60003, 0xBFB00000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000
	};
	/**
	 * shader main
	 * asic(GFX11)
	 * type(CS)
	 * s_version     UC_VERSION_GFX11 | UC_VERSION_W64_BIT   // 000000000000: B0802006
	 * s_set_inst_prefetch_distance  0x0003                  // 000000000004: BF840003
	 * v_and_b32     v0, lit(0x000003ff), v0                 // 000000000008: 360000FF 000003FF
	 * s_delay_alu   instid0(VALU_DEP_1)                     // 000000000010: BF870001
	 * v_lshl_add_u32  v1, s8, 6, v0                         // 000000000014: D6460001 04010C08
	 * buffer_load_format_xyzw  v[2:5], v1, s[0:3], 0 idxen  // 00000000001C: E00C0000 80800201
	 * s_waitcnt     vmcnt(0)                                // 000000000024: BF8903F7
	 * buffer_store_format_xyzw  v[2:5], v1, s[4:7], 0 idxen // 000000000028: E01C0000 80810201
	 * s_sendmsg     sendmsg(MSG_DEALLOC_VGPRS, 0, 0)        // 000000000030: BFB60003
	 * s_endpgm                                              // 000000000034: BFB00000
	 * end
	 */
	static const uint32_t buffercopy_cs_shader_gfx11[] = {
		0xB0802006, 0xBF840003, 0x360000FF, 0x000003FF,
		0xBF870001, 0xD6460001, 0x04010C08, 0xE00C0000,
		0x80800201, 0xBF8903F7, 0xE01C0000, 0x80810201,
		0xBFB60003, 0xBFB00000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000
	};
	static const uint32_t buffercopy_cs_shader_gfx12[] =
	{
		0xB0802009, 0xBF930011, 0x360000FF, 0x000003FF,
		0xBF870001, 0xD6460002, 0x04010C75, 0xC400C07C,
		0x80000003, 0x00000002, 0xBFC00000, 0xC401C07C,
		0x80000803, 0x00000002, 0xBFB60003, 0xBFB00000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000,
		0xBF9F0000, 0xBF9F0000, 0xBF9F0000, 0xBF9F0000
	};

	uint32_t shader_size;
	const uint32_t *shader;

	switch (cs_type) {
		case CS_BUFFERCLEAR:
			if (version == 9) {
				shader = bufferclear_cs_shader_gfx9;
				shader_size = sizeof(bufferclear_cs_shader_gfx9);
			} else if (version == 10) {
				shader = bufferclear_cs_shader_gfx10;
				shader_size = sizeof(bufferclear_cs_shader_gfx10);
			} else if (version == 11) {
				shader = bufferclear_cs_shader_gfx11;
				shader_size = sizeof(bufferclear_cs_shader_gfx11);
			} else if (version == 12) {
				shader = bufferclear_cs_shader_gfx12;
				shader_size = sizeof(bufferclear_cs_shader_gfx12);
			}
			break;
		case CS_BUFFERCOPY:
			if (version == 9) {
				shader = buffercopy_cs_shader_gfx9;
				shader_size = sizeof(buffercopy_cs_shader_gfx9);
			} else if (version == 10) {
				shader = buffercopy_cs_shader_gfx10;
				shader_size = sizeof(buffercopy_cs_shader_gfx10);
			} else if (version == 11) {
				shader = buffercopy_cs_shader_gfx11;
				shader_size = sizeof(buffercopy_cs_shader_gfx11);
			} else if (version == 12) {
				shader = buffercopy_cs_shader_gfx12;
				shader_size = sizeof(buffercopy_cs_shader_gfx12);
			}
			break;
		case CS_HANG:
			shader = memcpy_ps_hang;
			shader_size = sizeof(memcpy_ps_hang);
			break;
		default:
			return -1;
	}

	memcpy(ptr, shader, shader_size);
	return 0;
}


int
amdgpu_draw_load_ps_shader_hang_slow(uint32_t *ptr, int family)
{
	/**
	 * s_mov_b32 m0, s12
	 * s_mov_b64 s[14:15], exec
	 * s_wqm_b64 exec, exec
	 * v_interp_p1_f32_e32 v2, v0, attr0.x
	 * v_interp_p2_f32_e32 v2, v1, attr0.x
	 * v_interp_p1_f32_e32 v3, v0, attr0.y
	 * v_interp_p2_f32_e32 v3, v1, attr0.y
	 * image_sample v[0:3], v2, s[0:7], s[8:11] dmask:0xf
	 * ;;
	 * s_mov_b64 exec, s[14:15]
	 * s_waitcnt vmcnt(0)
	 * s_nop 0
	 * s_nop 0
	 * s_nop 0
	 * s_nop 0
	 * exp mrt0 v0, v1, v2, v3 done vm
	 * ;;
	 * s_endpgm
	 */
	unsigned int memcpy_ps_hang_slow_ai_codes[] = {
		0xbefc000c, 0xbe8e017e, 0xbefe077e, 0xd4080000,
		0xd4090001, 0xd40c0100, 0xd40d0101, 0xf0800f00,
		0x00400002, 0xbefe010e, 0xbf8c0f70, 0xbf800000,
		0xbf800000, 0xbf800000, 0xbf800000, 0xc400180f,
		0x03020100, 0xbf810000
	};

	struct amdgpu_test_shader memcpy_ps_hang_slow_ai = {
		memcpy_ps_hang_slow_ai_codes,
		7,
		2,
		9
	};

	struct amdgpu_test_shader *shader;
	int i, loop = 0x40000;

	switch (family) {
		case AMDGPU_FAMILY_AI:
		case AMDGPU_FAMILY_RV:
		case AMDGPU_FAMILY_NV: /* TODO check for correctness */
		shader = &memcpy_ps_hang_slow_ai;
			break;
		default:
			return -1;
			break;
	}

	memcpy(ptr, shader->shader, shader->header_length * sizeof(uint32_t));

	for (i = 0; i < loop; i++)
		memcpy(ptr + shader->header_length + shader->body_length * i,
			shader->shader + shader->header_length,
			shader->body_length * sizeof(uint32_t));

	memcpy(ptr + shader->header_length + shader->body_length * loop,
		shader->shader + shader->header_length + shader->body_length,
		shader->foot_length * sizeof(uint32_t));

	return 0;
}
