// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

/**
 * TEST: Basic tests for execbuf functionality
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: CMD submission
 * Functionality: exec_queues
 */

#include "igt.h"
#include "igt_multigpu.h"
#include "lib/igt_syncobj.h"
#include "lib/intel_reg.h"
#include "xe_drm.h"

#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"
#include <string.h>

#define MAX_N_EXEC_QUEUES 16
#define USERPTR			(0x1 << 0)
#define REBIND			(0x1 << 1)
#define INVALIDATE		(0x1 << 2)
#define RACE			(0x1 << 3)
#define BIND_EXEC_QUEUE	(0x1 << 4)
#define DEFER_ALLOC		(0x1 << 5)
#define DEFER_BIND		(0x1 << 6)
#define SPARSE			(0x1 << 7)

/**
 * SUBTEST: once-%s
 * Description: Run %arg[1] test only once
 * Test category: functionality test
 *
 * SUBTEST: many-%s
 * Description: Run %arg[1] test many times
 * Test category: stress test
 *
 * SUBTEST: many-execqueues-%s
 * Description: Run %arg[1] test on many exec_queues
 * Test category: stress test
 *
 * SUBTEST: many-execqueues-many-vm-%s
 * Description: Run %arg[1] test on many exec_queues and many VMs
 * Test category: stress test
 *
 * SUBTEST: twice-%s
 * Description: Run %arg[1] test twice
 * Test category: functionality test
 *
 * SUBTEST: no-exec-%s
 * Description: Run no-exec %arg[1] test
 * Test category: functionality test
 *
 * SUBTEST: multigpu-once-%s
 * Description: Run %arg[1] test only once on multiGPU
 * Mega feature: MultiGPU
 * Test category: functionality test
 *
 * SUBTEST: multigpu-many-execqueues-many-vm-%s
 * Mega feature: MultiGPU
 * Description: Run %arg[1] test on many exec_queues and many VMs on multiGPU
 * Test category: stress test
 *
 * SUBTEST: multigpu-no-exec-%s
 * Mega feature: MultiGPU
 * Description: Run no-exec %arg[1] test on multiGPU
 * Test category: functionality test
 *
 * arg[1]:
 *
 * @basic:				basic
 * @basic-defer-mmap:			basic defer mmap
 * @basic-defer-bind:			basic defer bind
 * @userptr:				userptr
 * @rebind:				rebind
 * @userptr-rebind:			userptr rebind
 * @userptr-invalidate:			userptr invalidate
 * @userptr-invalidate-race:		userptr invalidate racy
 * @bindexecqueue:				bind exec_queue
 * @bindexecqueue-userptr:			bind exec_queue userptr description
 * @bindexecqueue-rebind:			bind exec_queue rebind description
 * @bindexecqueue-userptr-rebind:		bind exec_queue userptr rebind
 * @bindexecqueue-userptr-invalidate:	bind exec_queue userptr invalidate
 * @bindexecqueue-userptr-invalidate-race:	bind exec_queue userptr invalidate racy
 * @null:				null
 * @null-defer-mmap:			null defer mmap
 * @null-defer-bind:			null defer bind
 * @null-rebind:			null rebind
 */

static void
test_exec(int fd, struct drm_xe_engine_class_instance *eci,
	  int n_exec_queues, int n_execs, int n_vm, unsigned int flags)
{
	struct drm_xe_sync sync[2] = {
	    { .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, },
	    { .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, },
	};
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 2,
		.syncs = to_user_pointer(sync),
	};
	uint64_t addr[MAX_N_EXEC_QUEUES];
	uint64_t sparse_addr[MAX_N_EXEC_QUEUES];
	uint32_t vm[MAX_N_EXEC_QUEUES];
	uint32_t exec_queues[MAX_N_EXEC_QUEUES];
	uint32_t bind_exec_queues[MAX_N_EXEC_QUEUES];
	uint32_t syncobjs[MAX_N_EXEC_QUEUES];
	uint32_t bind_syncobjs[MAX_N_EXEC_QUEUES];
	size_t bo_size;
	uint32_t bo = 0;
	struct {
		uint32_t batch[16];
		uint64_t pad;
		uint32_t data;
	} *data;
	int i, b;

	igt_assert_lte(n_exec_queues, MAX_N_EXEC_QUEUES);
	igt_assert_lte(n_vm, MAX_N_EXEC_QUEUES);

	for (i = 0; i < n_vm; ++i)
		vm[i] = xe_vm_create(fd, 0, 0);
	bo_size = sizeof(*data) * n_execs;
	bo_size = xe_bb_size(fd, bo_size);

	addr[0] = 0x1a0000;
	sparse_addr[0] = 0x301a0000;
	for (i = 1; i < MAX_N_EXEC_QUEUES; ++i) {
		addr[i] = addr[i - 1] + (0x1ull << 32);
		sparse_addr[i] = sparse_addr[i - 1] + (0x1ull << 32);
	}

	if (flags & USERPTR) {
#define	MAP_ADDRESS	0x00007fadeadbe000
		if (flags & INVALIDATE) {
			data = mmap((void *)MAP_ADDRESS, bo_size, PROT_READ |
				    PROT_WRITE, MAP_SHARED | MAP_FIXED |
				    MAP_ANONYMOUS, -1, 0);
			igt_assert(data != MAP_FAILED);
		} else {
			data = aligned_alloc(xe_get_default_alignment(fd), bo_size);
			igt_assert(data);
		}
		memset(data, 0, bo_size);
	} else {
		uint32_t bo_flags;

		bo_flags = DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM;
		if (flags & DEFER_ALLOC)
			bo_flags |= DRM_XE_GEM_CREATE_FLAG_DEFER_BACKING;

		bo = xe_bo_create(fd, n_vm == 1 ? vm[0] : 0, bo_size,
				  vram_if_possible(fd, eci->gt_id), bo_flags);
		if (!(flags & DEFER_BIND))
			data = xe_bo_map(fd, bo, bo_size);
	}

	for (i = 0; i < n_exec_queues; i++) {
		uint32_t __vm = vm[i % n_vm];

		exec_queues[i] = xe_exec_queue_create(fd, __vm, eci, 0);
		if (flags & BIND_EXEC_QUEUE)
			bind_exec_queues[i] = xe_bind_exec_queue_create(fd,
									__vm, 0);
		else
			bind_exec_queues[i] = 0;
		syncobjs[i] = syncobj_create(fd, 0);
		bind_syncobjs[i] = syncobj_create(fd, 0);
	};

	for (i = 0; i < n_vm; ++i) {
		sync[0].handle = bind_syncobjs[i];
		if (bo)
			xe_vm_bind_async(fd, vm[i], bind_exec_queues[i], bo, 0,
					 addr[i], bo_size, sync, 1);
		else
			xe_vm_bind_userptr_async(fd, vm[i], bind_exec_queues[i],
						 to_user_pointer(data), addr[i],
						 bo_size, sync, 1);
		if (flags & SPARSE)
			__xe_vm_bind_assert(fd, vm[i], bind_exec_queues[i],
					    0, 0, sparse_addr[i], bo_size,
					    DRM_XE_VM_BIND_OP_MAP,
					    DRM_XE_VM_BIND_FLAG_NULL, sync,
					    1, 0, 0);
	}

	if (flags & DEFER_BIND)
		data = xe_bo_map(fd, bo, bo_size);

	for (i = 0; i < n_execs; i++) {
		int cur_vm = i % n_vm;
		uint64_t __addr = addr[cur_vm];
		uint64_t batch_offset = (char *)&data[i].batch - (char *)data;
		uint64_t batch_addr = __addr + batch_offset;
		uint64_t sdi_offset = (char *)&data[i].data - (char *)data;
		uint64_t sdi_addr = (flags & SPARSE ? sparse_addr[i % n_vm] :
				     __addr)+ sdi_offset;
		int e = i % n_exec_queues;

		b = 0;
		data[i].batch[b++] = MI_STORE_DWORD_IMM_GEN4;
		data[i].batch[b++] = sdi_addr;
		data[i].batch[b++] = sdi_addr >> 32;
		data[i].batch[b++] = 0xc0ffee;
		data[i].batch[b++] = MI_BATCH_BUFFER_END;
		igt_assert(b <= ARRAY_SIZE(data[i].batch));

		sync[0].flags &= ~DRM_XE_SYNC_FLAG_SIGNAL;
		sync[0].handle = bind_syncobjs[cur_vm];
		sync[1].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
		sync[1].handle = syncobjs[e];

		exec.exec_queue_id = exec_queues[e];
		exec.address = batch_addr;
		if (e != i)
			 syncobj_reset(fd, &syncobjs[e], 1);
		xe_exec(fd, &exec);

		if (flags & REBIND && i + 1 != n_execs) {
			uint32_t __vm = vm[cur_vm];

			sync[1].flags &= ~DRM_XE_SYNC_FLAG_SIGNAL;
			xe_vm_unbind_async(fd, __vm, bind_exec_queues[e], 0,
					   __addr, bo_size, sync + 1, 1);

			sync[0].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
			addr[i % n_vm] += bo_size;
			__addr = addr[i % n_vm];
			if (bo)
				xe_vm_bind_async(fd, __vm, bind_exec_queues[e], bo,
						 0, __addr, bo_size, sync, 1);
			else
				xe_vm_bind_userptr_async(fd, __vm,
							 bind_exec_queues[e],
							 to_user_pointer(data),
							 __addr, bo_size, sync,
							 1);
		}

		if (flags & INVALIDATE && i + 1 != n_execs) {
			if (!(flags & RACE)) {
				/*
				 * Wait for exec completion and check data as
				 * userptr will likely change to different
				 * physical memory on next mmap call triggering
				 * an invalidate.
				 */
				igt_assert(syncobj_wait(fd, &syncobjs[e], 1,
							INT64_MAX, 0, NULL));
				igt_assert_eq(data[i].data, 0xc0ffee);
			} else if (i * 2 != n_execs) {
				/*
				 * We issue 1 mmap which races against running
				 * jobs. No real check here aside from this test
				 * not faulting on the GPU.
				 */
				continue;
			}

			data = mmap((void *)MAP_ADDRESS, bo_size, PROT_READ |
				    PROT_WRITE, MAP_SHARED | MAP_FIXED |
				    MAP_ANONYMOUS, -1, 0);
			igt_assert(data != MAP_FAILED);
		}
	}

	for (i = 0; i < n_exec_queues && n_execs; i++)
		igt_assert(syncobj_wait(fd, &syncobjs[i], 1, INT64_MAX, 0,
					NULL));

	for (i = 0; i < n_vm; i++)
		igt_assert(syncobj_wait(fd, &bind_syncobjs[i], 1, INT64_MAX, 0,
					NULL));

	sync[0].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
	for (i = 0; i < n_vm; ++i) {
		syncobj_reset(fd, &sync[0].handle, 1);
		xe_vm_unbind_async(fd, vm[i], bind_exec_queues[i], 0, addr[i],
				   bo_size, sync, 1);
		igt_assert(syncobj_wait(fd, &sync[0].handle, 1,
					INT64_MAX, 0, NULL));
	}

	if (!(flags & SPARSE)) {
		for (i = (flags & INVALIDATE && n_execs) ? n_execs - 1 : 0;
		     i < n_execs; i++)
			igt_assert_eq(data[i].data, 0xc0ffee);
	}

	for (i = 0; i < n_exec_queues; i++) {
		syncobj_destroy(fd, syncobjs[i]);
		xe_exec_queue_destroy(fd, exec_queues[i]);
		if (bind_exec_queues[i])
			xe_exec_queue_destroy(fd, bind_exec_queues[i]);
	}

	if (bo) {
		munmap(data, bo_size);
		gem_close(fd, bo);
	} else if (!(flags & INVALIDATE)) {
		free(data);
	}
	for (i = 0; i < n_vm; ++i) {
		syncobj_destroy(fd, bind_syncobjs[i]);
		xe_vm_destroy(fd, vm[i]);
	}
}

igt_main
{
	struct drm_xe_engine_class_instance *hwe;
	const struct section {
		const char *name;
		unsigned int flags;
	} sections[] = {
		{ "basic", 0 },
		{ "basic-defer-mmap", DEFER_ALLOC },
		{ "basic-defer-bind", DEFER_ALLOC | DEFER_BIND },
		{ "userptr", USERPTR },
		{ "rebind", REBIND },
		{ "null", SPARSE },
		{ "null-defer-mmap", SPARSE | DEFER_ALLOC },
		{ "null-defer-bind", SPARSE | DEFER_ALLOC | DEFER_BIND },
		{ "null-rebind", SPARSE | REBIND },
		{ "userptr-rebind", USERPTR | REBIND },
		{ "userptr-invalidate", USERPTR | INVALIDATE },
		{ "userptr-invalidate-race", USERPTR | INVALIDATE | RACE },
		{ "bindexecqueue", BIND_EXEC_QUEUE },
		{ "bindexecqueue-userptr", BIND_EXEC_QUEUE | USERPTR },
		{ "bindexecqueue-rebind", BIND_EXEC_QUEUE | REBIND },
		{ "bindexecqueue-userptr-rebind", BIND_EXEC_QUEUE | USERPTR | REBIND },
		{ "bindexecqueue-userptr-invalidate", BIND_EXEC_QUEUE | USERPTR |
			INVALIDATE },
		{ "bindexecqueue-userptr-invalidate-race", BIND_EXEC_QUEUE | USERPTR |
			INVALIDATE | RACE },
		{ NULL },
	};
	int fd;

	igt_fixture()
		fd = drm_open_driver(DRIVER_XE);

	for (const struct section *s = sections; s->name; s++) {
		igt_subtest_f("once-%s", s->name)
			xe_for_each_engine(fd, hwe)
				test_exec(fd, hwe, 1, 1, 1, s->flags);

		igt_subtest_f("twice-%s", s->name)
			xe_for_each_engine(fd, hwe)
				test_exec(fd, hwe, 1, 2, 1, s->flags);

		igt_subtest_f("many-%s", s->name)
			xe_for_each_engine(fd, hwe)
				test_exec(fd, hwe, 1,
					  s->flags & (REBIND | INVALIDATE) ?
					  64 : 1024, 1,
					  s->flags);

		igt_subtest_f("many-execqueues-%s", s->name)
			xe_for_each_engine(fd, hwe)
				test_exec(fd, hwe, 16,
					  s->flags & (REBIND | INVALIDATE) ?
					  64 : 1024, 1,
					  s->flags);

		igt_subtest_f("many-execqueues-many-vm-%s", s->name)
			xe_for_each_engine(fd, hwe)
				test_exec(fd, hwe, 16,
					  s->flags & (REBIND | INVALIDATE) ?
					  64 : 1024, 16,
					  s->flags);

		igt_subtest_f("no-exec-%s", s->name)
			xe_for_each_engine(fd, hwe)
				test_exec(fd, hwe, 1, 0, 1, s->flags);
	}

	igt_fixture()
		drm_close_driver(fd);

	for (const struct section *s = sections; s->name; s++) {
		igt_subtest_f("multigpu-once-%s", s->name) {
			igt_multi_fork_foreach_multigpu(gpu_fd, gpu_idx, DRIVER_XE)
				xe_for_each_engine(gpu_fd, hwe)
					test_exec(gpu_fd, hwe, 1, 1, 1, s->flags);
			igt_waitchildren();
		}

		igt_subtest_f("multigpu-many-execqueues-many-vm-%s", s->name) {
			igt_multi_fork_foreach_multigpu(gpu_fd, gpu_idx, DRIVER_XE)
				xe_for_each_engine(gpu_fd, hwe)
					test_exec(gpu_fd, hwe, 16, 32, 16, s->flags);
			igt_waitchildren();
		}

		igt_subtest_f("multigpu-no-exec-%s", s->name) {
			igt_multi_fork_foreach_multigpu(gpu_fd, gpu_idx, DRIVER_XE)
				xe_for_each_engine(gpu_fd, hwe)
					test_exec(gpu_fd, hwe, 1, 0, 1, s->flags);
			igt_waitchildren();
		}
	}
}
