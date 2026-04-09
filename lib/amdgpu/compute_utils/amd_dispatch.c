// SPDX-License-Identifier: MIT
// Copyright 2014 Advanced Micro Devices, Inc.
// Copyright 2022 Advanced Micro Devices, Inc.
// Copyright 2023 Advanced Micro Devices, Inc.

#include <amdgpu.h>
#include "amdgpu/amd_memory.h"
#include "amd_dispatch.h"
#include "amd_shared_dispatch.h"
#include "amd_dispatch_helpers.h"
#include "amdgpu/amd_PM4.h"
#include "amdgpu/amd_ip_blocks.h"
#include "amdgpu/shaders/amd_shaders.h"

/*
 * Static state for sched_mask cleanup on abnormal subtest exit.
 *
 * When amdgpu_dispatch_hang_slow_helper() or amdgpu_gfx_dispatch_test()
 * isolate a single compute/gfx ring via sysfs sched_mask, an igt_assert
 * failure inside the dispatch helpers triggers siglongjmp() back to the
 * subtest entry point, bypassing the mask restore at the end of the
 * function.  This leaves all other HW rings disabled, which causes
 * drm_sched to see ready == false and can lead to NULL-pointer
 * dereferences on subsequent tests.
 *
 * Saving the original mask in file-scoped variables and registering an
 * IGT exit handler guarantees restoration on both normal and abnormal
 * exit paths (siglongjmp, signals, process exit).
 */
static char sched_mask_sysfs[256];
static long sched_mask_saved;
static bool sched_mask_dirty;

static void sched_mask_exit_handler(int sig)
{
	char cmd[1024];

	if (!sched_mask_dirty)
		return;

	sched_mask_dirty = false;
	snprintf(cmd, sizeof(cmd) - 1, "sudo echo  0x%lx > %s",
		 sched_mask_saved, sched_mask_sysfs);
	system(cmd);
}

static void sched_mask_arm(const char *sysfs, long mask)
{
	/* If a prior subtest left the mask dirty, restore it first */
	if (sched_mask_dirty)
		sched_mask_exit_handler(0);

	strncpy(sched_mask_sysfs, sysfs, sizeof(sched_mask_sysfs) - 1);
	sched_mask_sysfs[sizeof(sched_mask_sysfs) - 1] = '\0';
	sched_mask_saved = mask;
	sched_mask_dirty = true;
	igt_install_exit_handler(sched_mask_exit_handler);
}

static void
amdgpu_memset_dispatch_test(amdgpu_device_handle device_handle,
			    uint32_t ip_type, uint32_t priority,
			    uint32_t version, bool user_queue)
{
	amdgpu_context_handle context_handle;
	amdgpu_bo_handle bo_dst, bo_shader, bo_cmd, resources[3];
	volatile unsigned char *ptr_dst;
	void *ptr_shader;
	uint32_t *ptr_cmd;
	uint64_t mc_address_dst, mc_address_shader, mc_address_cmd;
	amdgpu_va_handle va_dst, va_shader, va_cmd;
	int i, r;
	int bo_dst_size = 16384;
	int bo_shader_size = 4096;
	int bo_cmd_size = 4096;
	struct amdgpu_cs_request ibs_request = {0};
	struct amdgpu_cs_ib_info ib_info = {0};

	amdgpu_bo_list_handle bo_list;
	struct amdgpu_cs_fence fence_status = {0};
	uint32_t expired;

	struct amdgpu_cmd_base *base_cmd = get_cmd_base();
	struct amdgpu_ring_context *ring_context = NULL;
	const struct amdgpu_ip_block_version *ip_block = NULL;
	uint64_t seq_no;

	if (user_queue) {
		ring_context = calloc(1, sizeof(*ring_context));
		igt_assert(ring_context);

		ip_block = get_ip_block(device_handle, ip_type);
		ip_block->funcs->userq_create(device_handle, ring_context, ip_block->type);
	} else {
		if (priority == AMDGPU_CTX_PRIORITY_HIGH)
		    r = amdgpu_cs_ctx_create2(device_handle, AMDGPU_CTX_PRIORITY_HIGH, &context_handle);
		else
		    r = amdgpu_cs_ctx_create(device_handle, &context_handle);
		igt_assert_eq(r, 0);
	}

	r = amdgpu_bo_alloc_and_map_sync(device_handle, bo_cmd_size, 4096,
					AMDGPU_GEM_DOMAIN_GTT, 0, 0,
					&bo_cmd, (void **)&ptr_cmd, &mc_address_cmd, &va_cmd,
					user_queue ? ring_context->timeline_syncobj_handle : 0,
					user_queue ? ++ring_context->point : 0,
					user_queue);

	if (user_queue) {
		r = amdgpu_timeline_syncobj_wait(device_handle,
		       ring_context->timeline_syncobj_handle,
		       ring_context->point);
	}
	igt_assert_eq(r, 0);
	memset(ptr_cmd, 0, bo_cmd_size);
	base_cmd->attach_buf(base_cmd, ptr_cmd, bo_cmd_size);

	r = amdgpu_bo_alloc_and_map(device_handle, bo_shader_size, 4096,
					AMDGPU_GEM_DOMAIN_VRAM, 0,
					&bo_shader, &ptr_shader,
					&mc_address_shader, &va_shader);
	igt_assert_eq(r, 0);
	memset(ptr_shader, 0, bo_shader_size);

	r = amdgpu_dispatch_load_cs_shader(ptr_shader, CS_BUFFERCLEAR, version);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_alloc_and_map(device_handle, bo_dst_size, 4096,
					AMDGPU_GEM_DOMAIN_VRAM, 0,
					&bo_dst, (void **)&ptr_dst,
					&mc_address_dst, &va_dst);
	igt_assert_eq(r, 0);
	/// TODO helper function for this bloc
	amdgpu_dispatch_init(ip_type, base_cmd, version);

	/*  Issue commands to set cu mask used in current dispatch */
	amdgpu_dispatch_write_cumask(base_cmd, version);

	/* Writes shader state to HW */
	amdgpu_dispatch_write2hw(base_cmd, mc_address_shader, version, 0);

	/* Write constant data */
	/* Writes the UAV constant data to the SGPRs. */
	base_cmd->emit(base_cmd, PACKET3_COMPUTE(PKT3_SET_SH_REG, 4));
	base_cmd->emit(base_cmd, 0x240);

	base_cmd->emit(base_cmd, mc_address_dst);
	base_cmd->emit(base_cmd, (mc_address_dst >> 32) | 0x100000);

	base_cmd->emit(base_cmd, 0x400);
	if (version == 9)
		base_cmd->emit(base_cmd, 0x74fac);
	else if (version == 10)
		base_cmd->emit(base_cmd, 0x1104bfac);
	else if (version == 11)
		base_cmd->emit(base_cmd, 0x1003dfac);
	else if (version == 12)
		base_cmd->emit(base_cmd, 0x1203dfac);

	/* Sets a range of pixel shader constants */
	base_cmd->emit(base_cmd, PACKET3_COMPUTE(PKT3_SET_SH_REG, 4));
	base_cmd->emit(base_cmd, 0x244);

	base_cmd->emit(base_cmd, 0x22222222);
	base_cmd->emit(base_cmd, 0x22222222);
	base_cmd->emit(base_cmd, 0x22222222);
	base_cmd->emit(base_cmd, 0x22222222);

	/* clear mmCOMPUTE_RESOURCE_LIMITS */
	base_cmd->emit(base_cmd, PACKET3_COMPUTE(PKT3_SET_SH_REG, 1));
	base_cmd->emit(base_cmd, 0x215);
	base_cmd->emit(base_cmd, 0);

	/* dispatch direct command */
	base_cmd->emit(base_cmd, PACKET3_COMPUTE(PACKET3_DISPATCH_DIRECT, 3));
	base_cmd->emit(base_cmd, 0x10);
	base_cmd->emit(base_cmd, 1);
	base_cmd->emit(base_cmd, 1);
	base_cmd->emit(base_cmd, 1);

	base_cmd->emit_aligned(base_cmd, 7, GFX_COMPUTE_NOP);
	resources[0] = bo_dst;
	resources[1] = bo_shader;
	resources[2] = bo_cmd;

	if (!user_queue) {
		r = amdgpu_bo_list_create(device_handle, 3, resources, NULL, &bo_list);
		igt_assert_eq(r, 0);
	}

	ib_info.ib_mc_address = mc_address_cmd;
	ib_info.size = base_cmd->cdw;
	ibs_request.ip_type = ip_type;
	ibs_request.ring = 0;
	ibs_request.resources = bo_list;
	ibs_request.number_of_ibs = 1;
	ibs_request.ibs = &ib_info;
	ibs_request.fence_info.handle = NULL;

	if (user_queue) {
		ring_context->pm4_dw = ib_info.size;
		ip_block->funcs->userq_submit(device_handle, ring_context, ip_block->type,
				       mc_address_cmd);
		seq_no = ring_context->point;
	} else {
		r = amdgpu_cs_submit(context_handle, 0, &ibs_request, 1);
		igt_assert_eq(r, 0);
		seq_no = ibs_request.seq_no;
	}

	if (!user_queue) {
		r = amdgpu_bo_list_destroy(bo_list);
		igt_assert_eq(r, 0);

		fence_status.ip_type = ip_type;
		fence_status.ip_instance = 0;
		fence_status.ring = 0;
		fence_status.context = context_handle;
		fence_status.fence = seq_no;

		r = amdgpu_cs_query_fence_status(&fence_status,
				 AMDGPU_TIMEOUT_INFINITE,
				 0, &expired);
		igt_assert_eq(r, 0);
		igt_assert_eq(expired, true);
	}

	if (!user_queue) {
		/* verify if memset test result meets with expected */
		i = 0;
		while (i < bo_dst_size)
			igt_assert_eq(ptr_dst[i++], 0x22);
	}
	amdgpu_bo_unmap_and_free(bo_dst, va_dst, mc_address_dst, bo_dst_size);
	amdgpu_bo_unmap_and_free(bo_shader, va_shader, mc_address_shader,
				 bo_shader_size);
	amdgpu_bo_unmap_and_free(bo_cmd, va_cmd, mc_address_cmd, bo_cmd_size);

	if (user_queue) {
		ip_block->funcs->userq_destroy(device_handle, ring_context, ip_block->type);
		free(ring_context);
	} else {
		amdgpu_cs_ctx_free(context_handle);
	}
	free_cmd_base(base_cmd);
}

int
amdgpu_memcpy_dispatch_test(amdgpu_device_handle device_handle,
				amdgpu_context_handle context_handle_param,
				uint32_t ip_type, uint32_t ring, uint32_t priority,
				uint32_t version, enum cmd_error_type hang,
				struct amdgpu_cs_err_codes *err_codes, bool user_queue)
{
	amdgpu_context_handle context_handle_free = NULL;
	amdgpu_context_handle context_handle_in_use = NULL;
	amdgpu_bo_handle bo_src, bo_dst, bo_shader, bo_cmd, resources[4];
	volatile unsigned char *ptr_dst;
	void *ptr_shader;
	unsigned char *ptr_src;
	uint32_t *ptr_cmd;
	uint64_t mc_address_src, mc_address_dst, mc_address_shader, mc_address_cmd;
	amdgpu_va_handle va_src, va_dst, va_shader, va_cmd;
	int i, r, r2;
	int bo_dst_size = 16384;
	int bo_shader_size = 4096;
	int bo_cmd_size = 4096;
	struct amdgpu_cs_request ibs_request = {0};
	struct amdgpu_cs_ib_info ib_info = {0};
	uint32_t expired, hang_state, hangs;
	enum cs_type cs_type;
	amdgpu_bo_list_handle bo_list;
	struct amdgpu_cs_fence fence_status = {0};
	struct amdgpu_cmd_base *base_cmd = get_cmd_base();
	struct amdgpu_ring_context *ring_context = NULL;
	const struct amdgpu_ip_block_version *ip_block = NULL;
	uint64_t seq_no;

	if (user_queue) {
		ring_context = calloc(1, sizeof(*ring_context));
		igt_assert(ring_context);

		ip_block = get_ip_block(device_handle, ip_type);
		ip_block->funcs->userq_create(device_handle, ring_context, ip_block->type);
	} else if (context_handle_param == NULL) {
		if (priority == AMDGPU_CTX_PRIORITY_HIGH) {
			r = amdgpu_cs_ctx_create2(device_handle, AMDGPU_CTX_PRIORITY_HIGH, &context_handle_in_use);
			context_handle_free = context_handle_in_use;
			igt_assert_eq(r, 0);
		} else {
			r = amdgpu_cs_ctx_create(device_handle, &context_handle_in_use);
			context_handle_free = context_handle_in_use;
			igt_assert_eq(r, 0);
		}
	} else {
		context_handle_in_use = context_handle_param;
	}

	r = amdgpu_bo_alloc_and_map_sync(device_handle, bo_cmd_size, 4096,
				    AMDGPU_GEM_DOMAIN_GTT, 0, AMDGPU_VM_MTYPE_UC,
				    &bo_cmd, (void **)&ptr_cmd,
				    &mc_address_cmd, &va_cmd,
				    user_queue ? ring_context->timeline_syncobj_handle : 0,
				    user_queue ? ++ring_context->point : 0,
				    user_queue);
	if (user_queue) {
		r = amdgpu_timeline_syncobj_wait(device_handle,
		       ring_context->timeline_syncobj_handle,
		       ring_context->point);
	}

	igt_assert_eq(r, 0);
	memset(ptr_cmd, 0, bo_cmd_size);
	base_cmd->attach_buf(base_cmd, ptr_cmd, bo_cmd_size);

	r = amdgpu_bo_alloc_and_map(device_handle, bo_shader_size, 4096,
					AMDGPU_GEM_DOMAIN_VRAM, 0,
					&bo_shader, &ptr_shader,
					&mc_address_shader, &va_shader);
	igt_assert_eq(r, 0);
	memset(ptr_shader, 0, bo_shader_size);

	cs_type = hang == BACKEND_SE_GC_SHADER_INVALID_SHADER ? CS_HANG : CS_BUFFERCOPY;
	r = amdgpu_dispatch_load_cs_shader(ptr_shader, cs_type, version);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_alloc_and_map(device_handle, bo_dst_size, 4096,
					AMDGPU_GEM_DOMAIN_VRAM, 0,
					&bo_src, (void **)&ptr_src,
					&mc_address_src, &va_src);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_alloc_and_map(device_handle, bo_dst_size, 4096,
					AMDGPU_GEM_DOMAIN_VRAM, 0,
					&bo_dst, (void **)&ptr_dst,
					&mc_address_dst, &va_dst);
	igt_assert_eq(r, 0);
	///TODO helper function for this bloc
	amdgpu_dispatch_init(ip_type, base_cmd,  version);
	/*  Issue commands to set cu mask used in current dispatch */
	amdgpu_dispatch_write_cumask(base_cmd, version);

	if (hang == BACKEND_SE_GC_SHADER_INVALID_PROGRAM_ADDR)
		mc_address_shader = 0;
	/* Writes shader state to HW */
	amdgpu_dispatch_write2hw(base_cmd, mc_address_shader, version, hang);
	memset(ptr_src, 0x55, bo_dst_size);

	/* Write constant data */
	/* Writes the texture resource constants data to the SGPRs */
	base_cmd->emit(base_cmd, PACKET3_COMPUTE(PKT3_SET_SH_REG, 4));
	base_cmd->emit(base_cmd, 0x240);
	if (hang == BACKEND_SE_GC_SHADER_INVALID_USER_DATA) {
		base_cmd->emit(base_cmd, mc_address_src + ring * 0x1000);
		base_cmd->emit(base_cmd, 0);
	} else {
		base_cmd->emit(base_cmd, mc_address_src);
		base_cmd->emit(base_cmd, (mc_address_src >> 32) | 0x100000);
	}

	base_cmd->emit(base_cmd, 0x400);
	if (version == 9)
		base_cmd->emit(base_cmd, 0x74fac);
	else if (version == 10)
		base_cmd->emit(base_cmd, 0x1104bfac);
	else if (version == 11)
		base_cmd->emit(base_cmd, 0x1003dfac);
	else if (version == 12)
		base_cmd->emit(base_cmd, 0x1203dfac);

	/* Writes the UAV constant data to the SGPRs. */
	base_cmd->emit(base_cmd, PACKET3_COMPUTE(PKT3_SET_SH_REG, 4));
	base_cmd->emit(base_cmd, 0x244);
	if (hang == BACKEND_SE_GC_SHADER_INVALID_USER_DATA) {
		base_cmd->emit(base_cmd, mc_address_dst + ring * 0x1000);
		base_cmd->emit(base_cmd, 0);
	} else {
		base_cmd->emit(base_cmd, mc_address_dst);
		base_cmd->emit(base_cmd, (mc_address_dst >> 32) | 0x100000);
	}
	base_cmd->emit(base_cmd, 0x400);
	if (version == 9)
		base_cmd->emit(base_cmd, 0x74fac);
	else if (version == 10)
		base_cmd->emit(base_cmd, 0x1104bfac);
	else if (version == 11)
		base_cmd->emit(base_cmd, 0x1003dfac);
	else if (version == 12)
		base_cmd->emit(base_cmd, 0x1203dfac);

	/* clear mmCOMPUTE_RESOURCE_LIMITS */
	base_cmd->emit(base_cmd, PACKET3_COMPUTE(PKT3_SET_SH_REG, 1));
	base_cmd->emit(base_cmd, 0x215);
	base_cmd->emit(base_cmd, 0);

	/* dispatch direct command */
	base_cmd->emit(base_cmd, PACKET3_COMPUTE(PACKET3_DISPATCH_DIRECT, 3));
	base_cmd->emit(base_cmd, 0x10);
	base_cmd->emit(base_cmd, 1);
	base_cmd->emit(base_cmd, 1);
	base_cmd->emit(base_cmd, 1);

	base_cmd->emit_aligned(base_cmd, 7, GFX_COMPUTE_NOP); /* type3 nop packet */

	resources[0] = bo_shader;
	resources[1] = bo_src;
	resources[2] = bo_dst;
	resources[3] = bo_cmd;

	if (!user_queue) {
		r = amdgpu_bo_list_create(device_handle, 4, resources, NULL, &bo_list);
		igt_assert_eq(r, 0);
	}

	ib_info.ib_mc_address = mc_address_cmd;
	ib_info.size = base_cmd->cdw;
	ibs_request.ip_type = ip_type;
	ibs_request.ring = 0;
	ibs_request.resources = bo_list;
	ibs_request.number_of_ibs = 1;
	ibs_request.ibs = &ib_info;
	ibs_request.fence_info.handle = NULL;
	if (user_queue) {
		ring_context->pm4_dw = ib_info.size;
		ip_block->funcs->userq_submit(device_handle, ring_context, ip_block->type,
				       mc_address_cmd);
		seq_no = ring_context->point;
	} else {
		r = amdgpu_cs_submit(context_handle_in_use, 0, &ibs_request, 1);
		seq_no = ibs_request.seq_no;
	}

	if (err_codes)
		err_codes->err_code_cs_submit = r;

	if (!user_queue) {
		fence_status.ip_type = ip_type;
		fence_status.ip_instance = 0;
		fence_status.ring = 0;
		fence_status.context = context_handle_in_use;
		fence_status.fence = seq_no;

		r = amdgpu_cs_query_fence_status(&fence_status,
				 AMDGPU_TIMEOUT_INFINITE,
				 0, &expired);
		if (err_codes)
		    err_codes->err_code_wait_for_fence = r;
	}

	if (!hang) {
		if (!user_queue) {
		    igt_assert_eq(r, 0);
		    igt_assert_eq(expired, true);
		}

		/* verify if memcpy test result meets with expected */
		i = 0;
		/*it works up to 12287 ? vs required 16384 for gfx 8*/
		while (i < bo_dst_size) {
			igt_assert_eq(ptr_dst[i], ptr_src[i]);
			i++;
		}
	} else if (!user_queue) {
		r2 = amdgpu_cs_query_reset_state(context_handle_in_use, &hang_state, &hangs);
		igt_assert_eq(r2, 0);
	}

	if (!user_queue) {
		amdgpu_bo_list_destroy(bo_list);
	}

	amdgpu_bo_unmap_and_free(bo_src, va_src, mc_address_src, bo_dst_size);
	amdgpu_bo_unmap_and_free(bo_dst, va_dst, mc_address_dst, bo_dst_size);
	amdgpu_bo_unmap_and_free(bo_cmd, va_cmd, mc_address_cmd, bo_cmd_size);
	amdgpu_bo_unmap_and_free(bo_shader, va_shader, mc_address_shader,
				 bo_shader_size);

	if (user_queue) {
		ip_block->funcs->userq_destroy(device_handle, ring_context, ip_block->type);
		free(ring_context);
	} else if (context_handle_free) {
		amdgpu_cs_ctx_free(context_handle_free);
	}

	free_cmd_base(base_cmd);
	return r;
}

static void
amdgpu_memcpy_dispatch_hang_slow_test(amdgpu_device_handle device_handle,
				      uint32_t ip_type, uint32_t priority,
				      int version, uint32_t gpu_reset_status_equel,
				      bool user_queue)
{
	amdgpu_context_handle context_handle;
	amdgpu_bo_handle bo_src, bo_dst, bo_shader, bo_cmd, resources[4];
	volatile unsigned char *ptr_dst;
	void *ptr_shader;
	unsigned char *ptr_src;
	uint32_t *ptr_cmd;
	uint64_t mc_address_src, mc_address_dst, mc_address_shader, mc_address_cmd, reset_flags;
	amdgpu_va_handle va_src, va_dst, va_shader, va_cmd;
	int r, r2;

	int bo_dst_size = 0x4000000;
	int bo_shader_size = 0x4000000;
	int bo_cmd_size = 4096;

	struct amdgpu_cs_request ibs_request = {0};
	struct amdgpu_cs_ib_info ib_info = {0};
	uint32_t hang_state, hangs, expired;
	struct amdgpu_gpu_info gpu_info = {0};
	amdgpu_bo_list_handle bo_list;
	struct amdgpu_cs_fence fence_status = {0};

	struct amdgpu_cmd_base *base_cmd = get_cmd_base();
	struct amdgpu_ring_context *ring_context = NULL;
	const struct amdgpu_ip_block_version *ip_block = NULL;
	uint64_t seq_no;

	r = amdgpu_query_gpu_info(device_handle, &gpu_info);
	igt_assert_eq(r, 0);

	if (user_queue) {
		ring_context = calloc(1, sizeof(*ring_context));
		igt_assert(ring_context);

		ip_block = get_ip_block(device_handle, ip_type);
		ip_block->funcs->userq_create(device_handle, ring_context, ip_block->type);
	} else {
		if (priority == AMDGPU_CTX_PRIORITY_HIGH)
			r = amdgpu_cs_ctx_create2(device_handle, AMDGPU_CTX_PRIORITY_HIGH, &context_handle);
		else
			r = amdgpu_cs_ctx_create(device_handle, &context_handle);
		igt_assert_eq(r, 0);
	}

	r = amdgpu_bo_alloc_and_map_sync(device_handle, bo_cmd_size, 4096,
				    AMDGPU_GEM_DOMAIN_GTT, 0, AMDGPU_VM_MTYPE_UC,
				    &bo_cmd, (void **)&ptr_cmd,
				    &mc_address_cmd, &va_cmd,
				    user_queue ? ring_context->timeline_syncobj_handle : 0,
				    user_queue ? ++ring_context->point : 0,
				    user_queue);
	if (user_queue) {
		r = amdgpu_timeline_syncobj_wait(device_handle,
					       ring_context->timeline_syncobj_handle,
					       ring_context->point);
		igt_assert_eq(r, 0);
	}
	igt_assert_eq(r, 0);
	memset(ptr_cmd, 0, bo_cmd_size);
	base_cmd->attach_buf(base_cmd, ptr_cmd, bo_cmd_size);

	r = amdgpu_bo_alloc_and_map(device_handle, bo_shader_size, 4096,
				    AMDGPU_GEM_DOMAIN_VRAM, 0, &bo_shader,
				    &ptr_shader, &mc_address_shader, &va_shader);
	igt_assert_eq(r, 0);
	memset(ptr_shader, 0, bo_shader_size);

	r = amdgpu_dispatch_load_cs_shader_hang_slow(ptr_shader,
						     gpu_info.family_id);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_alloc_and_map(device_handle, bo_dst_size, 4096,
				    AMDGPU_GEM_DOMAIN_VRAM, 0, &bo_src,
				    (void **)&ptr_src, &mc_address_src, &va_src);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_alloc_and_map(device_handle, bo_dst_size, 4096,
				    AMDGPU_GEM_DOMAIN_VRAM, 0, &bo_dst,
				    (void **)&ptr_dst, &mc_address_dst, &va_dst);
	igt_assert_eq(r, 0);

	memset(ptr_src, 0x55, bo_dst_size);

	amdgpu_dispatch_init(ip_type, base_cmd, version);



	/*  Issue commands to set cu mask used in current dispatch */
	amdgpu_dispatch_write_cumask(base_cmd, version);

	/* Writes shader state to HW */
	amdgpu_dispatch_write2hw(base_cmd, mc_address_shader, version, 0);

	/* Write constant data */
	/* Writes the texture resource constants data to the SGPRs */
	base_cmd->emit(base_cmd, PACKET3_COMPUTE(PKT3_SET_SH_REG, 4));
	base_cmd->emit(base_cmd, 0x240);
	base_cmd->emit(base_cmd, mc_address_src);
	base_cmd->emit(base_cmd, (mc_address_src >> 32) | 0x100000);
	base_cmd->emit(base_cmd, 0x400000);
	if (version == 9)
		base_cmd->emit(base_cmd, 0x74fac);
	else if (version == 10)
		base_cmd->emit(base_cmd, 0x1104bfac);
	else if (version == 11)
		base_cmd->emit(base_cmd, 0x1003dfac);
	else if (version == 12)
		base_cmd->emit(base_cmd, 0x1203dfac);


	/* Writes the UAV constant data to the SGPRs. */
	base_cmd->emit(base_cmd, PACKET3_COMPUTE(PKT3_SET_SH_REG, 4));
	base_cmd->emit(base_cmd, 0x244);
	base_cmd->emit(base_cmd, mc_address_dst);
	base_cmd->emit(base_cmd, (mc_address_dst >> 32) | 0x100000);
	base_cmd->emit(base_cmd, 0x400000);
	if (version == 9)
		base_cmd->emit(base_cmd, 0x74fac);
	else if (version == 10)
		base_cmd->emit(base_cmd, 0x1104bfac);
	else if (version == 11)
		base_cmd->emit(base_cmd, 0x1003dfac);
	else if (version == 12)
		base_cmd->emit(base_cmd, 0x1203dfac);


	/* clear mmCOMPUTE_RESOURCE_LIMITS */
	base_cmd->emit(base_cmd, PACKET3_COMPUTE(PKT3_SET_SH_REG, 1));
	base_cmd->emit(base_cmd, 0x215);
	base_cmd->emit(base_cmd, 0);

	/* dispatch direct command */

	base_cmd->emit(base_cmd, PACKET3_COMPUTE(PACKET3_DISPATCH_DIRECT, 3));
	base_cmd->emit(base_cmd, 0x10000);
	base_cmd->emit(base_cmd, 1);
	base_cmd->emit(base_cmd, 1);
	base_cmd->emit(base_cmd, 1);

	base_cmd->emit_aligned(base_cmd, 7, GFX_COMPUTE_NOP); /* type3 nop packet */

	resources[0] = bo_shader;
	resources[1] = bo_src;
	resources[2] = bo_dst;
	resources[3] = bo_cmd;
	if (!user_queue) {
		r = amdgpu_bo_list_create(device_handle, 4, resources, NULL, &bo_list);
		igt_assert_eq(r, 0);
	}

	ib_info.ib_mc_address = mc_address_cmd;
	ib_info.size = base_cmd->cdw;
	ibs_request.ip_type = ip_type;
	ibs_request.ring = 0;
	ibs_request.resources = bo_list;
	ibs_request.number_of_ibs = 1;
	ibs_request.ibs = &ib_info;
	ibs_request.fence_info.handle = NULL;
	if (user_queue) {
		ring_context->pm4_dw = ib_info.size;
		ip_block->funcs->userq_submit(device_handle, ring_context, ip_block->type,
					      mc_address_cmd);
		seq_no = ring_context->point;
	} else {
		r = amdgpu_cs_submit(context_handle, 0, &ibs_request, 1);
		igt_assert_eq(r, 0);
		seq_no = ibs_request.seq_no;
	}

	if (!user_queue) {
		fence_status.ip_type = ip_type;
		fence_status.ip_instance = 0;
		fence_status.ring = 0;
		fence_status.context = context_handle;
		fence_status.fence = seq_no;

		/* wait for IB accomplished */
		r = amdgpu_cs_query_fence_status(&fence_status,
						 AMDGPU_TIMEOUT_INFINITE,
						 0, &expired);

		r = amdgpu_cs_query_reset_state(context_handle, &hang_state, &hangs);
		igt_assert_eq(r, 0);
		r2 = amdgpu_cs_query_reset_state2(context_handle, &reset_flags);
		igt_assert_eq(r2, 0);

		if (!(reset_flags == 0 ||
			  reset_flags & AMDGPU_CTX_QUERY2_FLAGS_RESET_IN_PROGRESS)) {
			/* If we're in reset and reset hasn't occurred, then check
			 * that the hang state is equal to the GPU reset status and
			 * assert otherwise.
			 */
			igt_assert_eq(hang_state, gpu_reset_status_equel);
		}

		r = amdgpu_bo_list_destroy(bo_list);
		igt_assert_eq(r, 0);
	}

	amdgpu_bo_unmap_and_free(bo_src, va_src, mc_address_src, bo_dst_size);
	amdgpu_bo_unmap_and_free(bo_dst, va_dst, mc_address_dst, bo_dst_size);
	amdgpu_bo_unmap_and_free(bo_cmd, va_cmd, mc_address_cmd, bo_cmd_size);
	amdgpu_bo_unmap_and_free(bo_shader, va_shader, mc_address_shader,
				 bo_shader_size);
	if (user_queue) {
		ip_block->funcs->userq_destroy(device_handle, ring_context, ip_block->type);
		free(ring_context);
	} else {
		amdgpu_cs_ctx_free(context_handle);
	}
	free_cmd_base(base_cmd);
}

void
amdgpu_dispatch_hang_slow_helper(amdgpu_device_handle device_handle,
				 uint32_t ip_type, const struct pci_addr *pci, bool userq)
{
	int r;
	char cmd[1024];
	long sched_mask = 0;
	struct drm_amdgpu_info_hw_ip info;
	uint32_t ring_id, version, prio;
	char sysfs[256];

	r = amdgpu_query_hw_ip_info(device_handle, ip_type, 0, &info);
	igt_assert_eq(r, 0);
	if (!info.available_rings)
		igt_info("SKIP ... as there's no ring for ip %d\n", ip_type);

	version = info.hw_ip_version_major;
	if (version != 9 && version != 10 && version != 11 && version != 12) {
		igt_info("SKIP ... unsupported gfx version %d\n", version);
		return;
	}

	if (is_spx_mode(pci)) {
		sched_mask = amdgpu_get_ip_schedule_mask(pci, (enum amd_ip_block_type)ip_type, sysfs);
	} else {
		sched_mask = 1;
	}

	if (sched_mask > 1)
		sched_mask_arm(sysfs, sched_mask);

	for (ring_id = 0; (0x1 << ring_id) <= sched_mask; ring_id++) {
		/* check sched is ready is on the ring. */
		if (!((1 << ring_id) & sched_mask))
			continue;

		if (sched_mask > 1 && ring_id == 0 &&
			ip_type == AMD_IP_COMPUTE) {
			/* for the compute multiple rings, the first queue
			 * as high priority compute queue.
			 * Need to create a high priority ctx.
			 */
			prio = AMDGPU_CTX_PRIORITY_HIGH;
		} else if (sched_mask > 1 && ring_id == 1 &&
			 ip_type == AMD_IP_GFX) {
			/* for the gfx multiple rings, pipe1 queue0 as
			 * high priority graphics queue.
			 * Need to create a high priority ctx.
			 */
			prio = AMDGPU_CTX_PRIORITY_HIGH;
		} else {
			prio = AMDGPU_CTX_PRIORITY_NORMAL;
		}

		if (sched_mask > 1) {
			snprintf(cmd, sizeof(cmd) - 1, "sudo echo  0x%x > %s",
						0x1 << ring_id, sysfs);
			igt_info("Disable other rings, keep only ring: %d enabled, cmd: %s\n", ring_id, cmd);
			r = system(cmd);
			igt_assert_eq(r, 0);
		}

		amdgpu_memcpy_dispatch_test(device_handle, NULL, ip_type,
					    ring_id, prio, version,
						BACKEND_SE_GC_SHADER_EXEC_SUCCESS, NULL, userq);
		amdgpu_memcpy_dispatch_hang_slow_test(device_handle, ip_type,
						      prio, version, AMDGPU_CTX_UNKNOWN_RESET, userq);

		amdgpu_memcpy_dispatch_test(device_handle, NULL, ip_type, ring_id, prio,
					    version, BACKEND_SE_GC_SHADER_EXEC_SUCCESS, NULL, userq);
	}

	/* recover the sched mask */
	if (sched_mask > 1) {
		snprintf(cmd, sizeof(cmd) - 1, "sudo echo  0x%lx > %s",sched_mask, sysfs);
		r = system(cmd);
		igt_assert_eq(r, 0);
		sched_mask_dirty = false;
	}
}

void amdgpu_gfx_dispatch_test(amdgpu_device_handle device_handle, uint32_t ip_type,
		enum cmd_error_type hang, const struct pci_addr *pci,  bool userq)
{
	int r;
	char cmd[1024];
	long sched_mask = 0;
	struct drm_amdgpu_info_hw_ip info;
	uint32_t ring_id, version, prio;
	char sysfs[256];

	r = amdgpu_query_hw_ip_info(device_handle, ip_type, 0, &info);
	igt_assert_eq(r, 0);
	if (!info.available_rings)
		igt_info("SKIP ... as there's no graphics ring\n");

	version = info.hw_ip_version_major;
	if (version != 9 && version != 10 && version != 11 && version != 12) {
		igt_info("SKIP ... unsupported gfx version %d\n", version);
		return;
	}
	if (version < 9)
		version = 9;

	if (userq) {
		sched_mask = 1;
	} else {
		if (is_spx_mode(pci)) {
			sched_mask = amdgpu_get_ip_schedule_mask(pci, (enum amd_ip_block_type)ip_type, sysfs);
		} else {
			sched_mask = 1;
		}
	}

	if (sched_mask > 1)
		sched_mask_arm(sysfs, sched_mask);

	for (ring_id = 0; (0x1 << ring_id) <= sched_mask; ring_id++) {
		/* check sched is ready is on the ring. */
		if (!((1 << ring_id) & sched_mask))
			continue;

		if (sched_mask > 1 && ring_id == 0 &&
			ip_type == AMD_IP_COMPUTE) {
			/* for the compute multiple rings, the first queue
			 * as high priority compute queue.
			 * Need to create a high priority ctx.
			 */
			prio = AMDGPU_CTX_PRIORITY_HIGH;
		} else if (sched_mask > 1 && ring_id == 1 &&
			 ip_type == AMD_IP_GFX) {
			/* for the gfx multiple rings, pipe1 queue0 as
			 * high priority graphics queue.
			 * Need to create a high priority ctx.
			 */
			prio = AMDGPU_CTX_PRIORITY_HIGH;
		} else {
			prio = AMDGPU_CTX_PRIORITY_NORMAL;
		}

		if (sched_mask > 1) {
			snprintf(cmd, sizeof(cmd) - 1, "sudo echo  0x%x > %s",
						0x1 << ring_id, sysfs);
			igt_info("Disable other rings, keep only ring: %d enabled, cmd: %s\n", ring_id, cmd);
			r = system(cmd);
			igt_assert_eq(r, 0);
		}
		amdgpu_memset_dispatch_test(device_handle, ip_type, prio,
					    version, userq);
		if (!userq)
			amdgpu_memcpy_dispatch_test(device_handle, NULL, ip_type, ring_id, prio,
							version, hang, NULL, userq);
	}

	/* recover the sched mask */
	if (sched_mask > 1) {
		snprintf(cmd, sizeof(cmd) - 1, "sudo echo  0x%lx > %s",sched_mask, sysfs);
		r = system(cmd);
		igt_assert_eq(r, 0);
		sched_mask_dirty = false;
	}
}

