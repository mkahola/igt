// SPDX-License-Identifier: MIT
/*
 * Copyright 2024 Advanced Micro Devices, Inc.
 * Copyright 2022 Advanced Micro Devices, Inc.
 * Copyright 2014 Advanced Micro Devices, Inc.
 */

#include "amdgpu/compute_utils/amd_dispatch_helpers.h"
#include "amdgpu/amd_memory.h"
#include <amdgpu_drm.h>
#include "amdgpu/amd_PM4.h"
#include "amdgpu/amd_ip_blocks.h"
#include "igt.h"

int
amdgpu_dispatch_init(uint32_t ip_type, struct amdgpu_cmd_base *base, uint32_t version)
{
	int i = base->cdw;

	/* Write context control and load shadowing register if necessary */
	if (ip_type == AMDGPU_HW_IP_GFX) {
		base->emit(base, PACKET3(PKT3_CONTEXT_CONTROL, 1));
		base->emit(base, 0x80000000);
		base->emit(base, 0x80000000);
	}

	/* Issue commands to set default compute state. */
	/* clear mmCOMPUTE_START_Z - mmCOMPUTE_START_X */
	base->emit(base, PACKET3_COMPUTE(PKT3_SET_SH_REG, 3));
	base->emit(base, 0x204);
	base->emit(base, 0);
	base->emit(base, 0);
	base->emit(base, 0);

	/* clear mmCOMPUTE_TMPRING_SIZE */
	base->emit(base, PACKET3_COMPUTE(PKT3_SET_SH_REG, 1));
	base->emit(base, 0x218);
	base->emit(base, 0);
	if (version == 10) {
		/* mmCOMPUTE_SHADER_CHKSUM */
		base->emit(base, PACKET3_COMPUTE(PKT3_SET_SH_REG, 1));
		base->emit(base, 0x22a);
		base->emit(base, 0);
		/* mmCOMPUTE_REQ_CTRL */
		base->emit(base, PACKET3_COMPUTE(PKT3_SET_SH_REG, 6));
		base->emit(base, 0x222);
		base->emit(base, 0x222);
		base->emit(base, 0x222);
		base->emit(base, 0x222);
		base->emit(base, 0x222);
		base->emit(base, 0x222);
		base->emit(base, 0x222);
		/* mmCP_COHER_START_DELAY */
		base->emit(base, PACKET3(PACKET3_SET_UCONFIG_REG, 1));
		base->emit(base, 0x7b);
		base->emit(base, 0x20);
	} else if (version == 11) {
		base->emit(base, PACKET3_COMPUTE(PKT3_SET_SH_REG, 1));
		base->emit(base, 0x222);
		base->emit(base, 0);
		base->emit(base, PACKET3_COMPUTE(PKT3_SET_SH_REG, 4));
		base->emit(base, 0x224);
		base->emit(base, 0);
		base->emit(base, 0);
		base->emit(base, 0);
		base->emit(base, 0);
		base->emit(base, PACKET3_COMPUTE(PKT3_SET_SH_REG, 1));
		base->emit(base, 0x22a);
		base->emit(base, 0);
	}
	return base->cdw - i;
}

int
amdgpu_dispatch_write_cumask(struct amdgpu_cmd_base *base, uint32_t version)
{
	int offset_prev = base->cdw;

	if (version == 9) {
	/*  Issue commands to set cu mask used in current dispatch */
	/* set mmCOMPUTE_STATIC_THREAD_MGMT_SE1 - mmCOMPUTE_STATIC_THREAD_MGMT_SE0 */
		base->emit(base, PACKET3_COMPUTE(PKT3_SET_SH_REG, 2));
		base->emit(base, 0x216);
		base->emit(base, 0xffffffff);
		base->emit(base, 0xffffffff);
	} else if ((version == 10) || (version == 11)) {
		/* set mmCOMPUTE_STATIC_THREAD_MGMT_SE1 - mmCOMPUTE_STATIC_THREAD_MGMT_SE0 */
		base->emit(base, PACKET3_COMPUTE(PKT3_SET_SH_REG_INDEX, 2));
		base->emit(base, 0x30000216);
		base->emit(base, 0xffffffff);
		base->emit(base, 0xffffffff);
	}
	/* set mmCOMPUTE_STATIC_THREAD_MGMT_SE3 - mmCOMPUTE_STATIC_THREAD_MGMT_SE2 */
	base->emit(base, PACKET3_COMPUTE(PKT3_SET_SH_REG_INDEX, 2));
	base->emit(base, 0x219);
	base->emit(base, 0xffffffff);
	base->emit(base, 0xffffffff);

	return base->cdw - offset_prev;
}


int amdgpu_dispatch_write2hw(struct amdgpu_cmd_base *base, uint64_t shader_addr, uint32_t version, enum  cmd_error_type hang)
{
	static const uint32_t bufferclear_cs_shader_registers_gfx9[][2] = {
		{0x2e12, 0x000C0041},	//{ mmCOMPUTE_PGM_RSRC1,	0x000C0041 },
		{0x2e13, 0x00000090},	//{ mmCOMPUTE_PGM_RSRC2,	0x00000090 },
		{0x2e07, 0x00000040},	//{ mmCOMPUTE_NUM_THREAD_X,	0x00000040 },
		{0x2e08, 0x00000001},	//{ mmCOMPUTE_NUM_THREAD_Y,	0x00000001 },
		{0x2e09, 0x00000001},	//{ mmCOMPUTE_NUM_THREAD_Z,	0x00000001 }
	};

	static uint32_t bufferclear_cs_shader_registers_gfx11[][2] = {
		{0x2e12, 0x600C0041},	//{ mmCOMPUTE_PGM_RSRC1,	  0x600C0041 },
		{0x2e13, 0x00000090},	//{ mmCOMPUTE_PGM_RSRC2,	  0x00000090 },
		{0x2e07, 0x00000040},	//{ mmCOMPUTE_NUM_THREAD_X, 0x00000040 },
		{0x2e08, 0x00000001},	//{ mmCOMPUTE_NUM_THREAD_Y, 0x00000001 },
		{0x2e09, 0x00000001},	//{ mmCOMPUTE_NUM_THREAD_Z, 0x00000001 }
	};

	static uint32_t bufferclear_cs_shader_invalid_registers[][2] = {
		{0x2e12, 0xffffffff},	//{ mmCOMPUTE_PGM_RSRC1,	  0x600C0041 },
		{0x2e13, 0xffffffff},	//{ mmCOMPUTE_PGM_RSRC2,	  0x00000090 },
		{0x2e07, 0x00000040},	//{ mmCOMPUTE_NUM_THREAD_X, 0x00000040 },
		{0x2e08, 0x00000001},	//{ mmCOMPUTE_NUM_THREAD_Y, 0x00000001 },
		{0x2e09, 0x00000001},	//{ mmCOMPUTE_NUM_THREAD_Z, 0x00000001 }
	};

	static const uint32_t bufferclear_cs_shader_registers_num_gfx9 = ARRAY_SIZE(bufferclear_cs_shader_registers_gfx9);
	static const uint32_t bufferclear_cs_shader_registers_num_gfx11 = ARRAY_SIZE(bufferclear_cs_shader_registers_gfx11);
	int offset_prev = base->cdw;
	int j;

	/* Writes shader state to HW */
	/* set mmCOMPUTE_PGM_HI - mmCOMPUTE_PGM_LO */
	base->emit(base, PACKET3_COMPUTE(PKT3_SET_SH_REG, 2));
	base->emit(base, 0x20c);
	base->emit(base, shader_addr >> 8);
	base->emit(base, shader_addr >> 40);
	/* write sh regs */
	if ((version == 11) || (version == 12)) {
		for (j = 0; j < bufferclear_cs_shader_registers_num_gfx11; j++) {
			base->emit(base, PACKET3_COMPUTE(PKT3_SET_SH_REG, 1));
			if (hang == BACKEND_SE_GC_SHADER_INVALID_PROGRAM_SETTING) {
				/* - Gfx11ShRegBase */
				base->emit(base, bufferclear_cs_shader_invalid_registers[j][0] - 0x2c00);
				if (bufferclear_cs_shader_invalid_registers[j][0] == 0x2E12)
					bufferclear_cs_shader_invalid_registers[j][1] &= ~(1<<29);

				base->emit(base, bufferclear_cs_shader_invalid_registers[j][1]);
			} else {
				/* - Gfx11ShRegBase */
				base->emit(base, bufferclear_cs_shader_registers_gfx11[j][0] - 0x2c00);
				if (bufferclear_cs_shader_registers_gfx11[j][0] == 0x2E12)
					bufferclear_cs_shader_registers_gfx11[j][1] &= ~(1<<29);

				base->emit(base, bufferclear_cs_shader_registers_gfx11[j][1]);
			}
		}
	} else {
		for (j = 0; j < bufferclear_cs_shader_registers_num_gfx9; j++) {
			base->emit(base, PACKET3_COMPUTE(PKT3_SET_SH_REG, 1));
			/* - Gfx9ShRegBase */
			if (hang == BACKEND_SE_GC_SHADER_INVALID_PROGRAM_SETTING) {
				base->emit(base, bufferclear_cs_shader_invalid_registers[j][0] - 0x2c00);
				base->emit(base, bufferclear_cs_shader_invalid_registers[j][1]);
			} else {
				base->emit(base, bufferclear_cs_shader_registers_gfx9[j][0] - 0x2c00);
				base->emit(base, bufferclear_cs_shader_registers_gfx9[j][1]);
			}
		}
	}
	if (version == 10) {
		/* mmCOMPUTE_PGM_RSRC3 */
		base->emit(base, PACKET3_COMPUTE(PKT3_SET_SH_REG, 1));
		base->emit(base, 0x228);
		base->emit(base, 0);
	} else if (version == 11) {
		/* mmCOMPUTE_PGM_RSRC3 */
		base->emit(base, PACKET3_COMPUTE(PKT3_SET_SH_REG, 1));
		base->emit(base, 0x228);
		base->emit(base, 0x3f0);
	}
	return base->cdw - offset_prev;
}

/**
 * execute_compute_dispatch - Execute a single compute dispatch for memory copy
 * @device: AMDGPU device handle
 * @version: GPU version for register programming
 * @shader_mc_addr: Shader buffer GPU address
 * @src_mc_addr: Source memory GPU address to read from
 * @dst_mc_addr: Destination buffer GPU address to write to
 * @expected_value: Expected 32-bit value for verification
 *
 * This function sets up and executes a compute dispatch that copies a single DWORD
 * from source to destination using a pre-compiled shader. Each dispatch uses
 * completely independent resources to avoid state pollution.
 *
 * Returns: 0 on success, -1 on failure
 */
int execute_compute_dispatch(amdgpu_device_handle device,
				   uint32_t version,
				   uint64_t shader_mc_addr,
				   uint64_t src_mc_addr,
				   uint64_t dst_mc_addr,
				   uint32_t expected_value)
{
	amdgpu_context_handle context_handle;
	amdgpu_bo_handle bo_cmd = NULL, bo_shader = NULL, bo_dst = NULL;
	uint32_t *ptr_cmd = NULL;
	void *ptr_shader = NULL;
	volatile uint32_t *ptr_dst = NULL;
	uint64_t mc_cmd = 0, mc_shader = 0, mc_dst = 0;
	amdgpu_va_handle va_cmd = 0, va_shader = 0, va_dst = 0;
	struct amdgpu_cs_request ib_req = {0};
	struct amdgpu_cs_ib_info ib_info = {0};
	amdgpu_bo_list_handle bo_list = NULL;
	struct amdgpu_cs_fence fence = {0};
	uint32_t expired = 0;
	struct amdgpu_cmd_base *base_cmd = get_cmd_base();
	amdgpu_bo_handle bos[3];
	int ret = 0;

	/* Create fresh context for each dispatch to avoid state pollution */
	if (amdgpu_cs_ctx_create(device, &context_handle) != 0) {
		igt_info("Failed to create context\n");
		return -1;
	}

	/* Command buffer for PM4 packets */
	if (amdgpu_bo_alloc_and_map(device, 4096, 4096,
	    AMDGPU_GEM_DOMAIN_GTT, 0, &bo_cmd, (void **)&ptr_cmd, &mc_cmd, &va_cmd) != 0) {
		igt_info("Failed to allocate command buffer\n");
		ret = -1;
		goto cleanup_context;
	}
	memset(ptr_cmd, 0, 4096);
	base_cmd->attach_buf(base_cmd, ptr_cmd, 4096);

	/* Shader buffer containing compiled ISA */
	if (amdgpu_bo_alloc_and_map(device, 4096, 4096,
			AMDGPU_GEM_DOMAIN_VRAM, 0,
			&bo_shader, &ptr_shader, &mc_shader, &va_shader) != 0) {
		igt_info("Failed to allocate shader buffer\n");
		ret = -1;
		goto cleanup_cmd;
	}
	memset(ptr_shader, 0, 4096);
	/* Note: shader content should be copied by caller */

	/* Destination buffer for shader output (CPU-visible) */
	if (amdgpu_bo_alloc_and_map(device, 4096, 4096,
			AMDGPU_GEM_DOMAIN_GTT, 0,
			&bo_dst, (void **)&ptr_dst, &mc_dst, &va_dst) != 0) {
		igt_info("Failed to allocate destination buffer\n");
		ret = -1;
		goto cleanup_shader;
	}
	*ptr_dst = 0; /* Initialize for verification */

	/* Initialize compute dispatch state */
	amdgpu_dispatch_init(AMDGPU_HW_IP_COMPUTE, base_cmd, version);
	amdgpu_dispatch_write_cumask(base_cmd, version);
	amdgpu_dispatch_write2hw(base_cmd, shader_mc_addr, version, 0);

	/* Program user data registers:
	* s[0:1] = source address (64-bit)
	* s[2:3] = destination address (64-bit)
	*/
	base_cmd->emit(base_cmd, PACKET3_COMPUTE(PKT3_SET_SH_REG, 4));
	base_cmd->emit(base_cmd, 0x240); /* SH0 register offset */
	base_cmd->emit(base_cmd, src_mc_addr & 0xFFFFFFFF);
	base_cmd->emit(base_cmd, src_mc_addr >> 32);
	base_cmd->emit(base_cmd, mc_dst & 0xFFFFFFFF);
	base_cmd->emit(base_cmd, mc_dst >> 32);

	/* Dispatch compute shader with 1x1x1 workgroup */
	base_cmd->emit(base_cmd, PACKET3_COMPUTE(PACKET3_DISPATCH_DIRECT, 3));
	base_cmd->emit(base_cmd, 0x10); /* Dispatch initiator */
	base_cmd->emit(base_cmd, 1);    /* dim_x */
	base_cmd->emit(base_cmd, 1);    /* dim_y */
	base_cmd->emit(base_cmd, 1);    /* dim_z */
	base_cmd->emit_aligned(base_cmd, 7, GFX_COMPUTE_NOP);

	/* Submit command buffer */
	bos[0] = bo_cmd;
	bos[1] = bo_shader;
	bos[2] = bo_dst;
	if (amdgpu_bo_list_create(device, 3, bos, NULL, &bo_list) != 0) {
		igt_info("Failed to create BO list\n");
		ret = -1;
		goto cleanup_dst;
	}

	ib_info.ib_mc_address = mc_cmd;
	ib_info.size = base_cmd->cdw;
	ib_req.ip_type = AMDGPU_HW_IP_COMPUTE;
	ib_req.ring = 0;
	ib_req.resources = bo_list;
	ib_req.number_of_ibs = 1;
	ib_req.ibs = &ib_info;
	ib_req.fence_info.handle = NULL;

	if (amdgpu_cs_submit(context_handle, 0, &ib_req, 1) != 0) {
		igt_info("Failed to submit CS\n");
		ret = -1;
		goto cleanup_bo_list;
	}

	/* Wait for completion */
	fence.context = context_handle;
	fence.ip_type = AMDGPU_HW_IP_COMPUTE;
	fence.ip_instance = 0;
	fence.ring = 0;
	fence.fence = ib_req.seq_no;

	if (amdgpu_cs_query_fence_status(&fence, AMDGPU_TIMEOUT_INFINITE, 0, &expired) != 0) {
		igt_info("Fence wait failed\n");
		ret = -1;
		goto cleanup_bo_list;
	}

	if (!expired) {
		igt_info("Fence not expired\n");
		ret = -1;
		goto cleanup_bo_list;
	}

	/* Verify result */
	if (*ptr_dst != expected_value) {
		igt_info("Result mismatch: got 0x%x, expected 0x%x\n", *ptr_dst, expected_value);
		ret = -1;
	} else {
		igt_info("Dispatch successful: got expected value 0x%x\n", *ptr_dst);
	}

cleanup_bo_list:
	amdgpu_bo_list_destroy(bo_list);
cleanup_dst:
	amdgpu_bo_unmap_and_free(bo_dst, va_dst, mc_dst, 4096);
cleanup_shader:
	amdgpu_bo_unmap_and_free(bo_shader, va_shader, mc_shader, 4096);
cleanup_cmd:
	amdgpu_bo_unmap_and_free(bo_cmd, va_cmd, mc_cmd, 4096);
cleanup_context:
	amdgpu_cs_ctx_free(context_handle);

	return ret;
}
