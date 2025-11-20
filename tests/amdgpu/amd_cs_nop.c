// SPDX-License-Identifier: MIT
/*
 * Copyright 2017 Intel Corporation
 * Copyright 2023 Advanced Micro Devices, Inc.
 */

#include "igt.h"
#include "drmtest.h"

#include <amdgpu.h>
#include <amdgpu_drm.h>
#include "lib/amdgpu/amd_PM4.h"
#include "lib/amdgpu/amd_ip_blocks.h"
#include "lib/amdgpu/amd_memory.h"

static void amdgpu_cs_sync(amdgpu_context_handle context,
			   unsigned int ip_type,
			   int ring,
			   unsigned int seqno)
{
	struct amdgpu_cs_fence fence = {
		.context = context,
		.ip_type = ip_type,
		.ring = ring,
		.fence = seqno,
	};
	uint32_t expired;
	int err;

	err = amdgpu_cs_query_fence_status(&fence,
					   AMDGPU_TIMEOUT_INFINITE,
					   0, &expired);
	igt_assert_eq(err, 0);
}

#define SYNC 0x1
#define FORK 0x2
static void nop_cs(amdgpu_device_handle device,
		   amdgpu_context_handle context,
		   const char *name,
		   unsigned int ip_type,
		   unsigned int ring,
		   unsigned int timeout,
		   unsigned int flags,
		   bool user_queue)
{
	const int ncpus = flags & FORK ? sysconf(_SC_NPROCESSORS_ONLN) : 1;
	amdgpu_bo_handle ib_result_handle;
	void *ib_result_cpu;
	uint64_t ib_result_mc_address;
	uint32_t *ptr;
	int i, r;
	amdgpu_bo_list_handle bo_list;
	amdgpu_va_handle va_handle;
	struct amdgpu_ring_context *ring_context;
	const struct amdgpu_ip_block_version *ip_block = NULL;

	ip_block = get_ip_block(device, ip_type);
	ring_context = calloc(1, sizeof(*ring_context));
	igt_assert(ring_context);

	if (user_queue)
		ip_block->funcs->userq_create(device, ring_context, ip_type);

	r = amdgpu_bo_alloc_and_map_sync(device, 4096, 4096,
					 AMDGPU_GEM_DOMAIN_GTT, 0, AMDGPU_VM_MTYPE_UC,
					 &ib_result_handle, &ib_result_cpu,
					 &ib_result_mc_address, &va_handle,
					 ring_context->timeline_syncobj_handle,
					 ++ring_context->point, user_queue);
	igt_assert_eq(r, 0);

	if (user_queue) {
		r = amdgpu_timeline_syncobj_wait(device, ring_context->timeline_syncobj_handle,
						 ring_context->point);
		igt_assert_eq(r, 0);
	}

	ptr = ib_result_cpu;
	for (i = 0; i < 16; ++i)
		ptr[i] = GFX_COMPUTE_NOP;

	if (!user_queue) {
		r = amdgpu_bo_list_create(device, 1, &ib_result_handle, NULL, &bo_list);
		igt_assert_eq(r, 0);
	}

	igt_fork(child, ncpus) {
		struct amdgpu_cs_request ibs_request;
		struct amdgpu_cs_ib_info ib_info;
		struct timespec tv = {};
		uint64_t submit_ns, sync_ns;
		unsigned long count;

		memset(&ib_info, 0, sizeof(struct amdgpu_cs_ib_info));
		ib_info.ib_mc_address = ib_result_mc_address;
		ib_info.size = 16;

		memset(&ibs_request, 0, sizeof(struct amdgpu_cs_request));
		ibs_request.ip_type = ip_type;
		ibs_request.ring = ring;
		ibs_request.number_of_ibs = 1;
		ibs_request.ibs = &ib_info;
		ibs_request.resources = bo_list;

		count = 0;
		igt_nsec_elapsed(&tv);
		igt_until_timeout(timeout) {
			if (user_queue) {
				ring_context->pm4_dw = ib_info.size;
				ip_block->funcs->userq_submit(device, ring_context, ip_type,
							 ib_info.ib_mc_address);
				igt_assert_eq(r, 0);
			} else {
				r = amdgpu_cs_submit(context, 0, &ibs_request, 1);
				igt_assert_eq(r, 0);
				if (flags & SYNC)
					amdgpu_cs_sync(context, ip_type, ring,
						       ibs_request.seq_no);
			}

			count++;
		}
		submit_ns = igt_nsec_elapsed(&tv);
		if (!user_queue)
			amdgpu_cs_sync(context, ip_type, ring, ibs_request.seq_no);

		sync_ns = igt_nsec_elapsed(&tv);

		igt_info("%s.%d: %'lu cycles, submit %.2fus, sync %.2fus\n",
			 name, child, count,
			 1e-3 * submit_ns / count, 1e-3 * sync_ns / count);
	}
	igt_waitchildren();

	if (!user_queue) {
		r = amdgpu_bo_list_destroy(bo_list);
		igt_assert_eq(r, 0);
	}

	amdgpu_bo_unmap_and_free(ib_result_handle, va_handle,
				 ib_result_mc_address, 4096);
	if (user_queue)
		ip_block->funcs->userq_destroy(device, ring_context, ip_type);

	free(ring_context);
}

igt_main
{
	amdgpu_device_handle device;
	amdgpu_context_handle context;
	const struct phase {
		const char *name;
		unsigned int flags;
	} phase[] = {
		{ "nop", 0 },
		{ "sync", SYNC },
		{ "fork", FORK },
		{ "sync-fork", SYNC | FORK },
		{ },
	}, *p;
	const struct engine {
		const char *name;
		unsigned int ip_type;
	} engines[] = {
		{ "compute", AMDGPU_HW_IP_COMPUTE },
		{ "gfx", AMDGPU_HW_IP_GFX },
		{ },
	}, *e;

	int fd = -1;
	bool arr_cap[AMD_IP_MAX] = {0};
	bool userq_arr_cap[AMD_IP_MAX] = {0};
#ifdef AMDGPU_USERQ_ENABLED
	bool enable_test;
	const char *env = getenv("AMDGPU_ENABLE_USERQTEST");

	enable_test = env && atoi(env);
#endif

	igt_fixture() {
		uint32_t major, minor;
		int err;

		fd = drm_open_driver(DRIVER_AMDGPU);

		err = amdgpu_device_initialize(fd, &major, &minor, &device);
		igt_require(err == 0);

		err = amdgpu_cs_ctx_create(device, &context);
		igt_assert_eq(err, 0);
		asic_rings_readness(device, 1, arr_cap);
		asic_userq_readiness(device, userq_arr_cap);
	}

	for (p = phase; p->name; p++) {
		for (e = engines; e->name; e++) {
			igt_describe("Stressful-and-multiple-cs-of-nop-operations-using-multiple-processes-with-the-same-GPU-context");
			igt_subtest_with_dynamic_f("cs-nops-with-%s-%s0", p->name, e->name) {
				if (arr_cap[e->ip_type]) {
					igt_dynamic_f("cs-nop-with-%s-%s0", p->name, e->name)
					nop_cs(device, context, e->name, e->ip_type, 0, 20,
					       p->flags, 0);
				}
			}
		}
	}

#ifdef AMDGPU_USERQ_ENABLED
	for (p = phase; p->name; p++) {
		for (e = engines; e->name; e++) {
			igt_describe("Stressful-and-multiple-cs-of-nop-operations-using-multiple-processes-with-the-same-GPU-context-UMQ");
			igt_subtest_with_dynamic_f("cs-nops-with-%s-%s0-with-UQ-Submission", p->name, e->name) {
				if (enable_test && userq_arr_cap[e->ip_type]) {
					igt_dynamic_f("cs-nop-with-%s-%s0-with-UQ-Submission", p->name, e->name)
					nop_cs(device, context, e->name, e->ip_type, 0, 20,
					       p->flags, 1);
				}
			}
		}
	}
#endif

	igt_fixture() {
		amdgpu_cs_ctx_free(context);
		amdgpu_device_deinitialize(device);
		drm_close_driver(fd);
	}
}
