// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

/**
 * TEST: Basic tests for execbuf functionality
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: CMD submission
 * Functionality: multi-threads
 * Test category: functionality test
 */

#include <fcntl.h>

#include "igt.h"
#include "lib/igt_syncobj.h"
#include "lib/intel_reg.h"
#include "xe_drm.h"

#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"
#include "xe/xe_gt.h"
#include "xe/xe_spin.h"
#include <string.h>

#define MAX_N_EXEC_QUEUES	16
#define USERPTR		(0x1 << 0)
#define REBIND		(0x1 << 1)
#define INVALIDATE	(0x1 << 2)
#define RACE		(0x1 << 3)
#define SHARED_VM	(0x1 << 4)
#define FD		(0x1 << 5)
#define COMPUTE_MODE	(0x1 << 6)
#define MIXED_MODE	(0x1 << 7)
#define BALANCER	(0x1 << 8)
#define PARALLEL	(0x1 << 9)
#define VIRTUAL		(0x1 << 10)
#define HANG		(0x1 << 11)
#define REBIND_ERROR	(0x1 << 12)
#define BIND_EXEC_QUEUE	(0x1 << 13)
#define MANY_QUEUES	(0x1 << 14)

pthread_barrier_t barrier;

static void
test_balancer(int fd, int gt, uint32_t vm, uint64_t addr, uint64_t userptr,
	      int class, int n_exec_queues, int n_execs, unsigned int flags)
{
	struct drm_xe_sync sync[2] = {
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, },
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, },
	};
	struct drm_xe_sync sync_all[MAX_N_EXEC_QUEUES];
	struct drm_xe_exec exec = {
		.num_syncs = 2,
		.syncs = to_user_pointer(sync),
	};
	uint32_t exec_queues[MAX_N_EXEC_QUEUES];
	uint32_t syncobjs[MAX_N_EXEC_QUEUES];
	size_t bo_size;
	uint32_t bo = 0;
	struct {
		uint32_t batch[16];
		uint64_t pad;
		uint32_t data;
	} *data;
	struct drm_xe_engine_class_instance eci[XE_MAX_ENGINE_INSTANCE];
	int i, j, b, num_placements;
	bool owns_vm = false, owns_fd = false;

	igt_assert_lte(n_exec_queues, MAX_N_EXEC_QUEUES);

	if (flags & FD) {
		fd = drm_reopen_driver(fd);
		owns_fd = true;
	}

	if (!vm) {
		vm = xe_vm_create(fd, 0, 0);
		owns_vm = true;
	}

	num_placements = xe_gt_fill_engines_by_class(fd, gt, class, eci);
	igt_assert_lt(1, num_placements);

	bo_size = sizeof(*data) * n_execs;
	bo_size = xe_bb_size(fd, bo_size);

	if (flags & USERPTR) {
		if (flags & INVALIDATE) {
			data = mmap(from_user_pointer(userptr), bo_size,
				    PROT_READ | PROT_WRITE,
				    MAP_SHARED | MAP_FIXED | MAP_ANONYMOUS,
				    -1, 0);
			igt_assert(data != MAP_FAILED);
		} else {
			data = aligned_alloc(xe_get_default_alignment(fd),
					     bo_size);
			igt_assert(data);
		}
	} else {
		bo = xe_bo_create(fd, vm, bo_size,
				  vram_if_possible(fd, gt),
				  DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
		data = xe_bo_map(fd, bo, bo_size);
	}
	memset(data, 0, bo_size);

	memset(sync_all, 0, sizeof(sync_all));
	for (i = 0; i < n_exec_queues; i++) {
		igt_assert_eq(__xe_exec_queue_create(fd, vm,
						     flags & PARALLEL ? num_placements : 1,
						     flags & PARALLEL ? 1 : num_placements,
						     eci, 0, &exec_queues[i]), 0);
		syncobjs[i] = syncobj_create(fd, 0);
		sync_all[i].type = DRM_XE_SYNC_TYPE_SYNCOBJ;
		sync_all[i].handle = syncobjs[i];
	};
	exec.num_batch_buffer = flags & PARALLEL ? num_placements : 1;

	pthread_barrier_wait(&barrier);

	sync[0].handle = syncobj_create(fd, 0);
	if (bo)
		xe_vm_bind_async(fd, vm, 0, bo, 0, addr, bo_size, sync, 1);
	else
		xe_vm_bind_userptr_async(fd, vm, 0, to_user_pointer(data), addr,
					 bo_size, sync, 1);

	for (i = 0; i < n_execs; i++) {
		uint64_t batch_offset = (char *)&data[i].batch - (char *)data;
		uint64_t batch_addr = addr + batch_offset;
		uint64_t sdi_offset = (char *)&data[i].data - (char *)data;
		uint64_t sdi_addr = addr + sdi_offset;
		uint64_t batches[XE_MAX_ENGINE_INSTANCE];
		int e = i % n_exec_queues;

		for (j = 0; j < num_placements && flags & PARALLEL; ++j)
			batches[j] = batch_addr;

		b = 0;
		data[i].batch[b++] = MI_STORE_DWORD_IMM_GEN4;
		data[i].batch[b++] = sdi_addr;
		data[i].batch[b++] = sdi_addr >> 32;
		data[i].batch[b++] = 0xc0ffee;
		data[i].batch[b++] = MI_BATCH_BUFFER_END;
		igt_assert(b <= ARRAY_SIZE(data[i].batch));

		sync[0].flags &= ~DRM_XE_SYNC_FLAG_SIGNAL;
		sync[1].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
		sync[1].handle = syncobjs[e];

		exec.exec_queue_id = exec_queues[e];
		exec.address = flags & PARALLEL ?
			to_user_pointer(batches) : batch_addr;
		if (e != i)
			 syncobj_reset(fd, &syncobjs[e], 1);
		xe_exec(fd, &exec);

		if (flags & REBIND && i && !(i & 0x1f)) {
			xe_vm_unbind_async(fd, vm, 0, 0, addr, bo_size,
					   sync_all, n_exec_queues);

			sync[0].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
			addr += bo_size;
			if (bo)
				xe_vm_bind_async(fd, vm, 0, bo, 0, addr,
						 bo_size, sync, 1);
			else
				xe_vm_bind_userptr_async(fd, vm, 0,
							 to_user_pointer(data),
							 addr, bo_size, sync,
							 1);
		}

		if (flags & INVALIDATE && i && !(i & 0x1f)) {
			if (!(flags & RACE)) {
				/*
				 * Wait for exec completion and check data as
				 * userptr will likely change to different
				 * physical memory on next mmap call triggering
				 * an invalidate.
				 */
				for (j = 0; j < n_exec_queues; ++j)
					igt_assert(syncobj_wait(fd,
								&syncobjs[j], 1,
								INT64_MAX, 0,
								NULL));
				igt_assert_eq(data[i].data, 0xc0ffee);
			} else if (i * 2 != n_execs) {
				/*
				 * We issue 1 mmap which races against running
				 * jobs. No real check here aside from this test
				 * not faulting on the GPU.
				 */
				continue;
			}

			data = mmap(from_user_pointer(userptr), bo_size,
				    PROT_READ | PROT_WRITE,
				    MAP_SHARED | MAP_FIXED | MAP_ANONYMOUS,
				    -1, 0);
			igt_assert(data != MAP_FAILED);
		}
	}

	for (i = 0; i < n_exec_queues; i++)
		igt_assert(syncobj_wait(fd, &syncobjs[i], 1, INT64_MAX, 0,
					NULL));
	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));

	sync[0].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
	xe_vm_unbind_async(fd, vm, 0, 0, addr, bo_size, sync, 1);
	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));

	for (i = (flags & INVALIDATE && n_execs) ? n_execs - 1 : 0;
	     i < n_execs; i++)
		igt_assert_eq(data[i].data, 0xc0ffee);

	syncobj_destroy(fd, sync[0].handle);
	for (i = 0; i < n_exec_queues; i++) {
		syncobj_destroy(fd, syncobjs[i]);
		xe_exec_queue_destroy(fd, exec_queues[i]);
	}

	if (bo) {
		munmap(data, bo_size);
		gem_close(fd, bo);
	} else if (!(flags & INVALIDATE)) {
		free(data);
	}
	if (owns_vm)
		xe_vm_destroy(fd, vm);
	if (owns_fd)
		drm_close_driver(fd);
}

static void
test_compute_mode(int fd, uint32_t vm, uint64_t addr, uint64_t userptr,
		  struct drm_xe_engine_class_instance *eci,
		  int n_exec_queues, int n_execs, unsigned int flags)
{
	uint64_t sync_addr = addr + 0x10000000;
#define USER_FENCE_VALUE	0xdeadbeefdeadbeefull
	struct drm_xe_sync sync[1] = {
		{ .type = DRM_XE_SYNC_TYPE_USER_FENCE, .flags = DRM_XE_SYNC_FLAG_SIGNAL,
	          .timeline_value = USER_FENCE_VALUE },
	};
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 1,
		.syncs = to_user_pointer(sync),
	};
	int64_t fence_timeout;
	uint32_t exec_queues[MAX_N_EXEC_QUEUES];
	size_t bo_size, sync_size;
	uint32_t bo = 0;
	struct {
		uint32_t batch[16];
		uint64_t pad;
		uint64_t vm_sync;
		uint32_t data;
	} *data;
	uint64_t *exec_sync;
	int i, j, b;
	int map_fd = -1;
	bool owns_vm = false, owns_fd = false;

	igt_assert_lte(n_exec_queues, MAX_N_EXEC_QUEUES);

	if (flags & FD) {
		fd = drm_reopen_driver(fd);
		owns_fd = true;
	}

	if (!vm) {
		vm = xe_vm_create(fd, DRM_XE_VM_CREATE_FLAG_LR_MODE, 0);
		owns_vm = true;
	}

	bo_size = sizeof(*data) * n_execs;
	bo_size = xe_bb_size(fd, bo_size);
	sync_size = sizeof(*exec_sync) * n_execs;
	sync_size = xe_bb_size(fd, sync_size);

	if (flags & USERPTR) {
		if (flags & INVALIDATE) {
			data = mmap(from_user_pointer(userptr), bo_size,
				    PROT_READ | PROT_WRITE,
				    MAP_SHARED | MAP_FIXED | MAP_ANONYMOUS,
				    -1, 0);
			igt_assert(data != MAP_FAILED);
		} else {
			data = aligned_alloc(xe_get_default_alignment(fd),
					     bo_size);
			igt_assert(data);
		}
	} else {
		bo = xe_bo_create(fd, 0, bo_size,
				  vram_if_possible(fd, eci->gt_id),
				  DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
		data = xe_bo_map(fd, bo, bo_size);
	}
	memset(data, 0, bo_size);

	exec_sync = mmap(from_user_pointer(userptr + 0x10000000),
			 sync_size, PROT_READ | PROT_WRITE,
			 MAP_SHARED | MAP_FIXED | MAP_ANONYMOUS, -1, 0);
	igt_assert(exec_sync != MAP_FAILED);
	memset(exec_sync, 0, sync_size);

	for (i = 0; i < n_exec_queues; i++)
		exec_queues[i] = xe_exec_queue_create(fd, vm, eci, 0);

	pthread_barrier_wait(&barrier);

	sync[0].addr = to_user_pointer(&data[0].vm_sync);
	if (bo)
		xe_vm_bind_async(fd, vm, 0, bo, 0, addr, bo_size, sync, 1);
	else
		xe_vm_bind_userptr_async(fd, vm, 0, to_user_pointer(data), addr,
					 bo_size, sync, 1);
	fence_timeout = (igt_run_in_simulation() ? 30 : 3) * NSEC_PER_SEC;
	xe_wait_ufence(fd, &data[0].vm_sync, USER_FENCE_VALUE, 0, fence_timeout);
	data[0].vm_sync = 0;

	xe_vm_bind_userptr_async(fd, vm, 0, to_user_pointer(exec_sync),
				 sync_addr, sync_size, sync, 1);
	xe_wait_ufence(fd, &data[0].vm_sync, USER_FENCE_VALUE, 0, fence_timeout);
	data[0].vm_sync = 0;

	for (i = 0; i < n_execs; i++) {
		uint64_t batch_offset = (char *)&data[i].batch - (char *)data;
		uint64_t batch_addr = addr + batch_offset;
		uint64_t sdi_offset = (char *)&data[i].data - (char *)data;
		uint64_t sdi_addr = addr + sdi_offset;
		int e = i % n_exec_queues;

		b = 0;
		data[i].batch[b++] = MI_STORE_DWORD_IMM_GEN4;
		data[i].batch[b++] = sdi_addr;
		data[i].batch[b++] = sdi_addr >> 32;
		data[i].batch[b++] = 0xc0ffee;
		data[i].batch[b++] = MI_BATCH_BUFFER_END;
		igt_assert(b <= ARRAY_SIZE(data[i].batch));

		sync[0].addr = sync_addr + (char *)&exec_sync[i] - (char *)exec_sync;

		exec.exec_queue_id = exec_queues[e];
		exec.address = batch_addr;
		xe_exec(fd, &exec);

		if (flags & REBIND && i && !(i & 0x1f)) {
			for (j = i == 0x20 ? 0 : i - 0x1f; j <= i; ++j)
				xe_wait_ufence(fd, &exec_sync[j],
					       USER_FENCE_VALUE,
					       exec_queues[e], fence_timeout);
			xe_vm_unbind_async(fd, vm, 0, 0, addr, bo_size,
					   NULL, 0);

			sync[0].addr = to_user_pointer(&data[0].vm_sync);
			addr += bo_size;
			if (bo)
				xe_vm_bind_async(fd, vm, 0, bo, 0, addr,
						 bo_size, sync, 1);
			else
				xe_vm_bind_userptr_async(fd, vm, 0,
							 to_user_pointer(data),
							 addr, bo_size, sync,
							 1);
			xe_wait_ufence(fd, &data[0].vm_sync, USER_FENCE_VALUE,
				       0, fence_timeout);
			data[0].vm_sync = 0;
		}

		if (flags & INVALIDATE && i && !(i & 0x1f)) {
			if (!(flags & RACE)) {
				/*
				 * Wait for exec completion and check data as
				 * userptr will likely change to different
				 * physical memory on next mmap call triggering
				 * an invalidate.
				 */
				for (j = i == 0x20 ? 0 : i - 0x1f; j <= i; ++j)
					xe_wait_ufence(fd, &exec_sync[j],
						       USER_FENCE_VALUE,
						       exec_queues[e],
						       fence_timeout);
				igt_assert_eq(data[i].data, 0xc0ffee);
			} else if (i * 2 != n_execs) {
				/*
				 * We issue 1 mmap which races against running
				 * jobs. No real check here aside from this test
				 * not faulting on the GPU.
				 */
				continue;
			}

			if (flags & RACE) {
				map_fd = open("/tmp", O_TMPFILE | O_RDWR,
					      0x666);
				igt_assert_eq(write(map_fd, data, bo_size),
					      bo_size);
				data = mmap(from_user_pointer(userptr), bo_size,
					    PROT_READ | PROT_WRITE,
					    MAP_SHARED | MAP_FIXED,
					    map_fd, 0);
			} else {
				data = mmap(from_user_pointer(userptr), bo_size,
					    PROT_READ | PROT_WRITE,
					    MAP_SHARED | MAP_FIXED | MAP_ANONYMOUS,
					    -1, 0);
			}
			igt_assert(data != MAP_FAILED);
		}
	}

	j = 0; /* wait for all submissions to complete */
	if (flags & INVALIDATE)
		/*
		 * For !RACE cases xe_wait_ufence has been called in above for-loop
		 * except the last batch of submissions. For RACE cases we will need
		 * to wait for all submissions to complete.
		 */
		j = (flags & RACE) ? 0 : (((n_execs - 1) & ~0x1f) + 1);
	else if (flags & REBIND)
		/*
		 * For REBIND cases xe_wait_ufence has been called in above for-loop
		 * except the last batch of submissions.
		 */
		j = ((n_execs - 1) & ~0x1f) + 1;

	for (i = j; i < n_execs; i++)
		xe_wait_ufence(fd, &exec_sync[i], USER_FENCE_VALUE,
			       exec_queues[i % n_exec_queues], fence_timeout);

	/*
	 * For INVALIDATE && RACE cases, due the the remmap in the
	 * middle of the execution, we lose access to some of the
	 * 0xc0ffee written to the old location, so check only for
	 * the second half of the submissions.
	 */
	if (flags & INVALIDATE && flags & RACE)
		j = n_execs / 2 + 1;
	for (i = j; i < n_execs; i++)
		igt_assert_eq(data[i].data, 0xc0ffee);

	sync[0].addr = to_user_pointer(&data[0].vm_sync);
	xe_vm_unbind_async(fd, vm, 0, 0, sync_addr, sync_size, sync, 1);
	xe_wait_ufence(fd, &data[0].vm_sync, USER_FENCE_VALUE, 0, fence_timeout);
	data[0].vm_sync = 0;
	xe_vm_unbind_async(fd, vm, 0, 0, addr, bo_size, sync, 1);
	xe_wait_ufence(fd, &data[0].vm_sync, USER_FENCE_VALUE, 0, fence_timeout);

	for (i = 0; i < n_exec_queues; i++)
		xe_exec_queue_destroy(fd, exec_queues[i]);

	munmap(exec_sync, sync_size);
	if (bo) {
		munmap(data, bo_size);
		gem_close(fd, bo);
	} else if (!(flags & INVALIDATE)) {
		free(data);
	}
	if (map_fd != -1)
		close(map_fd);
	if (owns_vm)
		xe_vm_destroy(fd, vm);
	if (owns_fd) {
		drm_close_driver(fd);
	}
}

static void
test_legacy_mode(int fd, uint32_t vm, uint64_t addr, uint64_t userptr,
		 struct drm_xe_engine_class_instance *eci, int n_exec_queues,
		 int n_execs, unsigned int flags)
{
	struct drm_xe_sync sync[2] = {
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, },
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, },
	};
	struct drm_xe_sync sync_all[MAX_N_EXEC_QUEUES];
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 2,
		.syncs = to_user_pointer(sync),
	};
	uint32_t exec_queues[MAX_N_EXEC_QUEUES];
	uint32_t bind_exec_queues[MAX_N_EXEC_QUEUES];
	uint32_t syncobjs[MAX_N_EXEC_QUEUES];
	size_t bo_size;
	uint32_t bo = 0;
	struct {
		struct xe_spin spin;
		uint32_t batch[16];
		uint64_t pad;
		uint32_t data;
	} *data;
	struct xe_spin_opts spin_opts = { .preempt = false };
	int i, j, b, hang_exec_queue = n_exec_queues / 2;
	bool owns_vm = false, owns_fd = false;

	igt_assert_lte(n_exec_queues, MAX_N_EXEC_QUEUES);

	if (flags & FD) {
		fd = drm_reopen_driver(fd);
		owns_fd = true;
	}

	if (!vm) {
		vm = xe_vm_create(fd, 0, 0);
		owns_vm = true;
	}

	bo_size = sizeof(*data) * n_execs;
	bo_size = xe_bb_size(fd, bo_size);

	if (flags & USERPTR) {
		if (flags & INVALIDATE) {
			data = mmap(from_user_pointer(userptr), bo_size,
				    PROT_READ | PROT_WRITE,
				    MAP_SHARED | MAP_FIXED | MAP_ANONYMOUS,
				    -1, 0);
			igt_assert(data != MAP_FAILED);
		} else {
			data = aligned_alloc(xe_get_default_alignment(fd),
					     bo_size);
			igt_assert(data);
		}
	} else {
		bo = xe_bo_create(fd, vm, bo_size,
				  vram_if_possible(fd, eci->gt_id),
				  DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
		data = xe_bo_map(fd, bo, bo_size);
	}
	memset(data, 0, bo_size);

	memset(sync_all, 0, sizeof(sync_all));
	for (i = 0; i < n_exec_queues; i++) {
		if (!(flags & MANY_QUEUES))
			exec_queues[i] = xe_exec_queue_create(fd, vm, eci, 0);
		if (flags & BIND_EXEC_QUEUE)
			bind_exec_queues[i] = xe_bind_exec_queue_create(fd, vm,
									0);
		else
			bind_exec_queues[i] = 0;
		syncobjs[i] = syncobj_create(fd, 0);
		sync_all[i].type = DRM_XE_SYNC_TYPE_SYNCOBJ;
		sync_all[i].handle = syncobjs[i];
	};

	pthread_barrier_wait(&barrier);

	sync[0].handle = syncobj_create(fd, 0);
	if (bo)
		xe_vm_bind_async(fd, vm, bind_exec_queues[0], bo, 0, addr,
				 bo_size, sync, 1);
	else
		xe_vm_bind_userptr_async(fd, vm, bind_exec_queues[0],
					 to_user_pointer(data), addr,
					 bo_size, sync, 1);

	for (i = 0; i < n_execs; i++) {
		uint64_t batch_offset = (char *)&data[i].batch - (char *)data;
		uint64_t batch_addr = addr + batch_offset;
		uint64_t spin_offset = (char *)&data[i].spin - (char *)data;
		uint64_t sdi_offset = (char *)&data[i].data - (char *)data;
		uint64_t sdi_addr = addr + sdi_offset;
		uint64_t exec_addr;
		int e = i % n_exec_queues;

		if (flags & MANY_QUEUES) {
			if (exec_queues[e]) {
				igt_assert(syncobj_wait(fd, &syncobjs[e], 1,
							INT64_MAX, 0, NULL));
				xe_exec_queue_destroy(fd, exec_queues[e]);
			}
			exec_queues[e] = xe_exec_queue_create(fd, vm, eci, 0);
		}

		if (flags & HANG && e == hang_exec_queue && i == e) {
			spin_opts.addr = addr + spin_offset;
			xe_spin_init(&data[i].spin, &spin_opts);
			exec_addr = spin_opts.addr;
		} else {
			b = 0;
			data[i].batch[b++] = MI_STORE_DWORD_IMM_GEN4;
			data[i].batch[b++] = sdi_addr;
			data[i].batch[b++] = sdi_addr >> 32;
			data[i].batch[b++] = 0xc0ffee;
			data[i].batch[b++] = MI_BATCH_BUFFER_END;
			igt_assert(b <= ARRAY_SIZE(data[i].batch));

			exec_addr = batch_addr;
		}

		sync[0].flags &= ~DRM_XE_SYNC_FLAG_SIGNAL;
		sync[1].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
		sync[1].handle = syncobjs[e];

		exec.exec_queue_id = exec_queues[e];
		exec.address = exec_addr;
		if (e != i && !(flags & HANG))
			 syncobj_reset(fd, &syncobjs[e], 1);
		if ((flags & HANG && e == hang_exec_queue)) {
			int err;

			do {
				err = igt_ioctl(fd, DRM_IOCTL_XE_EXEC, &exec);
			} while (err && errno == ENOMEM);
		} else {
			xe_exec(fd, &exec);
		}

		if (flags & REBIND && i && !(i & 0x1f)) {
			xe_vm_unbind_async(fd, vm, bind_exec_queues[e],
					   0, addr, bo_size,
					   sync_all, n_exec_queues);

			sync[0].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
			addr += bo_size;
			if (bo)
				xe_vm_bind_async(fd, vm, bind_exec_queues[e],
						 bo, 0, addr, bo_size, sync, 1);
			else
				xe_vm_bind_userptr_async(fd, vm,
							 bind_exec_queues[e],
							 to_user_pointer(data),
							 addr, bo_size, sync,
							 1);
		}

		if (flags & INVALIDATE && i && !(i & 0x1f)) {
			if (!(flags & RACE)) {
				/*
				 * Wait for exec completion and check data as
				 * userptr will likely change to different
				 * physical memory on next mmap call triggering
				 * an invalidate.
				 */
				for (j = 0; j < n_exec_queues; ++j)
					igt_assert(syncobj_wait(fd,
								&syncobjs[j], 1,
								INT64_MAX, 0,
								NULL));
				if (!(flags & HANG && e == hang_exec_queue))
					igt_assert_eq(data[i].data, 0xc0ffee);
			} else if (i * 2 != n_execs) {
				/*
				 * We issue 1 mmap which races against running
				 * jobs. No real check here aside from this test
				 * not faulting on the GPU.
				 */
				continue;
			}

			data = mmap(from_user_pointer(userptr), bo_size,
				    PROT_READ | PROT_WRITE,
				    MAP_SHARED | MAP_FIXED | MAP_ANONYMOUS,
				    -1, 0);
			igt_assert(data != MAP_FAILED);
		}
	}

	for (i = 0; i < n_exec_queues; i++)
		igt_assert(syncobj_wait(fd, &syncobjs[i], 1, INT64_MAX, 0,
					NULL));
	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));

	sync[0].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
	xe_vm_unbind_async(fd, vm, bind_exec_queues[0], 0, addr,
			   bo_size, sync, 1);
	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));

	for (i = flags & INVALIDATE ? n_execs - 1 : 0;
	     i < n_execs; i++) {
		int e = i % n_exec_queues;

		if (flags & HANG && e == hang_exec_queue)
			igt_assert_eq(data[i].data, 0x0);
		else
			igt_assert_eq(data[i].data, 0xc0ffee);
	}

	syncobj_destroy(fd, sync[0].handle);
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
	if (owns_vm)
		xe_vm_destroy(fd, vm);
	if (owns_fd)
		drm_close_driver(fd);
}

struct thread_data {
	pthread_t thread;
	pthread_mutex_t *mutex;
	pthread_cond_t *cond;
	uint64_t addr;
	uint64_t userptr;
	int class;
	int fd;
	int gt;
	uint32_t vm_legacy_mode;
	uint32_t vm_compute_mode;
	struct drm_xe_engine_class_instance *eci;
	int n_exec_queue;
	int n_exec;
	int flags;
	bool *go;
};

static void *thread(void *data)
{
	struct thread_data *t = data;

	pthread_mutex_lock(t->mutex);
	while (*t->go == 0)
		pthread_cond_wait(t->cond, t->mutex);
	pthread_mutex_unlock(t->mutex);

	if (t->flags & PARALLEL || t->flags & VIRTUAL)
		test_balancer(t->fd, t->gt, t->vm_legacy_mode, t->addr,
			      t->userptr, t->class, t->n_exec_queue, t->n_exec,
			      t->flags);
	else if (t->flags & COMPUTE_MODE)
		test_compute_mode(t->fd, t->vm_compute_mode, t->addr,
				  t->userptr, t->eci, t->n_exec_queue, t->n_exec,
				  t->flags);
	else
		test_legacy_mode(t->fd, t->vm_legacy_mode, t->addr, t->userptr,
				 t->eci, t->n_exec_queue, t->n_exec,
				 t->flags);

	return NULL;
}

/**
 * SUBTEST: threads-%s
 * Description: Run threads %arg[1] test with multi threads
 *
 * arg[1]:
 *
 * @shared-vm-basic:		shared vm basic
 * @fd-basic:			fd basic
 * @bal-basic:			bal basic
 * @cm-basic:			cm basic
 * @cm-fd-basic:		cm fd basic
 * @mixed-basic:		mixed basic
 * @mixed-shared-vm-basic:	mixed shared vm basic
 * @mixed-fd-basic:		mixed fd basic
 * @bal-mixed-basic:		bal mixed basic
 * @bal-mixed-shared-vm-basic:	bal mixed shared vm basic
 * @bal-mixed-fd-basic:		bal mixed fd basic
 */

/**
 * SUBTEST: threads-%s
 * Description: Run threads %arg[1] test with multi threads
 * arg[1]:
 * @basic:
 *	basic
 * @many-queues:
 *	many queues
 * @userptr:
 *	userptr
 * @rebind:
 *	rebind
 * @rebind-bindexecqueue:
 *	rebind bindexecqueue
 * @userptr-rebind:
 *	userptr rebind
 * @userptr-invalidate:
 *	userptr invalidate
 * @userptr-invalidate-race:
 *	userptr invalidate race
 * @shared-vm-userptr:
 *	shared vm userptr
 * @shared-vm-rebind:
 *	shared vm rebind
 * @shared-vm-rebind-bindexecqueue:
 *	shared vm rebind bindexecqueue
 * @shared-vm-userptr-rebind:
 *	shared vm userptr rebind
 * @rebind-err:
 *	rebind err
 * @userptr-rebind-err:
 *	userptr rebind err
 * @shared-vm-userptr-invalidate:
 *	shared vm userptr invalidate
 * @shared-vm-userptr-invalidate-race:
 *	shared vm userptr invalidate race
 * @fd-userptr:
 *	fd userptr
 * @fd-rebind:
 *	fd rebind
 * @fd-userptr-rebind:
 *	fd userptr rebind
 * @fd-userptr-invalidate:
 *	fd userptr invalidate
 * @fd-userptr-invalidate-race:
 *	fd userptr invalidate race
 * @hang-basic:
 *	hang basic
 * @hang-userptr:
 *	hang userptr
 * @hang-rebind:
 *	hang rebind
 * @hang-userptr-rebind:
 *	hang userptr rebind
 * @hang-userptr-invalidate:
 *	hang userptr invalidate
 * @hang-userptr-invalidate-race:
 *	hang userptr invalidate race
 * @hang-shared-vm-basic:
 *	hang shared vm basic
 * @hang-shared-vm-userptr:
 *	hang shared vm userptr
 * @hang-shared-vm-rebind:
 *	hang shared vm rebind
 * @hang-shared-vm-userptr-rebind:
 *	hang shared vm userptr rebind
 * @hang-rebind-err:
 *	hang rebind err
 * @hang-userptr-rebind-err:
 *	hang userptr rebind err
 * @hang-shared-vm-userptr-invalidate:
 *	hang shared vm userptr invalidate
 * @hang-shared-vm-userptr-invalidate-race:
 *	hang shared vm userptr invalidate race
 * @hang-fd-basic:
 *	hang fd basic
 * @hang-fd-userptr:
 *	hang fd userptr
 * @hang-fd-rebind:
 *	hang fd rebind
 * @hang-fd-userptr-rebind:
 *	hang fd userptr rebind
 * @hang-fd-userptr-invalidate:
 *	hang fd userptr invalidate
 * @hang-fd-userptr-invalidate-race:
 *	hang fd userptr invalidate race
 * @bal-userptr:
 *	balancer userptr
 * @bal-rebind:
 *	balancer rebind
 * @bal-userptr-rebind:
 *	balancer userptr rebind
 * @bal-userptr-invalidate:
 *	balancer userptr invalidate
 * @bal-userptr-invalidate-race:
 *	balancer userptr invalidate race
 * @bal-shared-vm-basic:
 *	balancer shared vm basic
 * @bal-shared-vm-userptr:
 *	balancer shared vm userptr
 * @bal-shared-vm-rebind:
 *	balancer shared vm rebind
 * @bal-shared-vm-userptr-rebind:
 *	balancer shared vm userptr rebind
 * @bal-shared-vm-userptr-invalidate:
 *	balancer shared vm userptr invalidate
 * @bal-shared-vm-userptr-invalidate-race:
 *	balancer shared vm userptr invalidate race
 * @bal-fd-basic:
 *	balancer fd basic
 * @bal-fd-userptr:
 *	balancer fd userptr
 * @bal-fd-rebind:
 *	balancer fd rebind
 * @bal-fd-userptr-rebind:
 *	balancer fd userptr rebind
 * @bal-fd-userptr-invalidate:
 *	balancer fd userptr invalidate
 * @bal-fd-userptr-invalidate-race:
 *	balancer fd userptr invalidate race
 * @cm-userptr:
 *	compute mode userptr
 * @cm-rebind:
 *	compute mode rebind
 * @cm-userptr-rebind:
 *	compute mode userptr rebind
 * @cm-userptr-invalidate:
 *	compute mode userptr invalidate
 * @cm-userptr-invalidate-race:
 *	compute mode userptr invalidate race
 * @cm-shared-vm-basic:
 *	compute mode shared vm basic
 * @cm-shared-vm-userptr:
 *	compute mode shared vm userptr
 * @cm-shared-vm-rebind:
 *	compute mode shared vm rebind
 * @cm-shared-vm-userptr-rebind:
 *	compute mode shared vm userptr rebind
 * @cm-shared-vm-userptr-invalidate:
 *	compute mode shared vm userptr invalidate
 * @cm-shared-vm-userptr-invalidate-race:
 *	compute mode shared vm userptr invalidate race
 * @cm-fd-userptr:
 *	compute mode fd userptr
 * @cm-fd-rebind:
 *	compute mode fd rebind
 * @cm-fd-userptr-rebind:
 *	compute mode fd userptr rebind
 * @cm-fd-userptr-invalidate:
 *	compute mode fd userptr invalidate
 * @cm-fd-userptr-invalidate-race:
 *	compute mode fd userptr invalidate race
 * @mixed-userptr:
 *	mixed userptr
 * @mixed-rebind:
 *	mixed rebind
 * @mixed-userptr-rebind:
 *	mixed userptr rebind
 * @mixed-userptr-invalidate:
 *	mixed userptr invalidate
 * @mixed-userptr-invalidate-race:
 *	mixed userptr invalidate race
 * @mixed-shared-vm-userptr:
 *	mixed shared vm userptr
 * @mixed-shared-vm-rebind:
 *	mixed shared vm rebind
 * @mixed-shared-vm-userptr-rebind:
 *	mixed shared vm userptr rebind
 * @mixed-shared-vm-userptr-invalidate:
 *	mixed shared vm userptr invalidate
 * @mixed-shared-vm-userptr-invalidate-race:
 *	mixed shared vm userptr invalidate race
 * @mixed-fd-userptr:
 *	mixed fd userptr
 * @mixed-fd-rebind:
 *	mixed fd rebind
 * @mixed-fd-userptr-rebind:
 *	mixed fd userptr rebind
 * @mixed-fd-userptr-invalidate:
 *	mixed fd userptr invalidate
 * @mixed-fd-userptr-invalidate-race:
 *	mixed fd userptr invalidate race
 * @bal-mixed-userptr:
 *	balancer mixed userptr
 * @bal-mixed-rebind:
 *	balancer mixed rebind
 * @bal-mixed-userptr-rebind:
 *	balancer mixed userptr rebind
 * @bal-mixed-userptr-invalidate:
 *	balancer mixed userptr invalidate
 * @bal-mixed-userptr-invalidate-race:
 *	balancer mixed userptr invalidate race
 * @bal-mixed-shared-vm-userptr:
 *	balancer mixed shared vm userptr
 * @bal-mixed-shared-vm-rebind:
 *	balancer mixed shared vm rebind
 * @bal-mixed-shared-vm-userptr-rebind:
 *	balancer mixed shared vm userptr rebind
 * @bal-mixed-shared-vm-userptr-invalidate:
 *	balancer mixed shared vm userptr invalidate
 * @bal-mixed-shared-vm-userptr-invalidate-race:
 *	balancer mixed shared vm userptr invalidate race
 * @bal-mixed-fd-userptr:
 *	balancer mixed fd userptr
 * @bal-mixed-fd-rebind:
 *	balancer mixed fd rebind
 * @bal-mixed-fd-userptr-rebind:
 *	balancer mixed fd userptr rebind
 * @bal-mixed-fd-userptr-invalidate:
 *	balancer mixed fd userptr invalidate
 * @bal-mixed-fd-userptr-invalidate-race:
 *	balancer mixed fd userptr invalidate race
 */

static void threads(int fd, int flags)
{
	struct thread_data *threads_data;
	struct drm_xe_engine_class_instance *hwe;
	uint64_t addr = 0x1a0000;
	uint64_t userptr = 0x00007000eadbe000;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	int n_engines = 0, class;
	uint64_t i = 0;
	uint32_t vm_legacy_mode = 0, vm_compute_mode = 0;
	bool go = false;
	int n_threads = 0;
	int gt;

	xe_for_each_engine(fd, hwe)
		++n_engines;

	if (flags & BALANCER) {
		xe_for_each_gt(fd, gt)
			xe_for_each_engine_class(class) {
				int num_placements = xe_gt_count_engines_by_class(fd, gt, class);

				if (num_placements > 1)
					n_engines += 2;
			}
	}

	threads_data = calloc(n_engines, sizeof(*threads_data));
	igt_assert(threads_data);

	pthread_mutex_init(&mutex, 0);
	pthread_cond_init(&cond, 0);

	if (flags & SHARED_VM) {
		vm_legacy_mode = xe_vm_create(fd, 0, 0);
		vm_compute_mode = xe_vm_create(fd,
					       DRM_XE_VM_CREATE_FLAG_LR_MODE,
					       0);
	}

	xe_for_each_engine(fd, hwe) {
		threads_data[i].mutex = &mutex;
		threads_data[i].cond = &cond;
#define ADDRESS_SHIFT	39
		threads_data[i].addr = addr | (i << ADDRESS_SHIFT);
		threads_data[i].userptr = userptr | (i << ADDRESS_SHIFT);
		threads_data[i].fd = fd;
		threads_data[i].vm_legacy_mode = vm_legacy_mode;
		threads_data[i].vm_compute_mode = vm_compute_mode;
		threads_data[i].eci = hwe;
#define N_EXEC_QUEUE	16
		threads_data[i].n_exec_queue = N_EXEC_QUEUE;
#define N_EXEC		1024
		threads_data[i].n_exec = N_EXEC;
		threads_data[i].flags = flags;
		if (flags & MIXED_MODE) {
			threads_data[i].flags &= ~MIXED_MODE;
			if (i & 1)
				threads_data[i].flags |= COMPUTE_MODE;
		}
		threads_data[i].go = &go;

		++n_threads;
		pthread_create(&threads_data[i].thread, 0, thread,
			       &threads_data[i]);
		++i;
	}

	if (flags & BALANCER) {
		xe_for_each_gt(fd, gt)
			xe_for_each_engine_class(class) {
				int num_placements;
				int *data_flags = (int[]){ VIRTUAL, PARALLEL, -1 };

				num_placements = xe_gt_count_engines_by_class(fd, gt, class);
				if (num_placements <= 1)
					continue;

				while (*data_flags >= 0) {
					threads_data[i].mutex = &mutex;
					threads_data[i].cond = &cond;
					if (flags & SHARED_VM)
						threads_data[i].addr = addr |
							(i << ADDRESS_SHIFT);
					else
						threads_data[i].addr = addr;
					threads_data[i].userptr = userptr |
						(i << ADDRESS_SHIFT);
					threads_data[i].fd = fd;
					threads_data[i].gt = gt;
					threads_data[i].vm_legacy_mode =
						vm_legacy_mode;
					threads_data[i].class = class;
					threads_data[i].n_exec_queue = N_EXEC_QUEUE;
					threads_data[i].n_exec = N_EXEC;
					threads_data[i].flags = flags;
					threads_data[i].flags &= ~BALANCER;
					threads_data[i].flags |= *data_flags;
					threads_data[i].go = &go;

					++n_threads;
					pthread_create(&threads_data[i].thread, 0,
						       thread, &threads_data[i]);
					++i;
					data_flags++;
				}
			}
	}

	pthread_barrier_init(&barrier, NULL, n_threads);

	pthread_mutex_lock(&mutex);
	go = true;
	pthread_cond_broadcast(&cond);
	pthread_mutex_unlock(&mutex);

	for (i = 0; i < n_engines; ++i)
		pthread_join(threads_data[i].thread, NULL);

	if (vm_legacy_mode)
		xe_vm_destroy(fd, vm_legacy_mode);
	if (vm_compute_mode)
		xe_vm_destroy(fd, vm_compute_mode);
	free(threads_data);
	pthread_barrier_destroy(&barrier);
}

igt_main
{
	const struct section {
		const char *name;
		unsigned int flags;
	} sections[] = {
		{ "basic", 0 },
		{ "many-queues", MANY_QUEUES },
		{ "userptr", USERPTR },
		{ "rebind", REBIND },
		{ "rebind-bindexecqueue", REBIND | BIND_EXEC_QUEUE },
		{ "userptr-rebind", USERPTR | REBIND },
		{ "userptr-invalidate", USERPTR | INVALIDATE },
		{ "userptr-invalidate-race", USERPTR | INVALIDATE | RACE },
		{ "shared-vm-basic", SHARED_VM },
		{ "shared-vm-userptr", SHARED_VM | USERPTR },
		{ "shared-vm-rebind", SHARED_VM | REBIND },
		{ "shared-vm-rebind-bindexecqueue", SHARED_VM | REBIND |
			BIND_EXEC_QUEUE },
		{ "shared-vm-userptr-rebind", SHARED_VM | USERPTR | REBIND },
		{ "rebind-err", REBIND | REBIND_ERROR },
		{ "userptr-rebind-err", USERPTR | REBIND | REBIND_ERROR},
		{ "shared-vm-userptr-invalidate", SHARED_VM | USERPTR |
			INVALIDATE },
		{ "shared-vm-userptr-invalidate-race", SHARED_VM | USERPTR |
			INVALIDATE | RACE },
		{ "fd-basic", FD },
		{ "fd-userptr", FD | USERPTR },
		{ "fd-rebind", FD | REBIND },
		{ "fd-userptr-rebind", FD | USERPTR | REBIND },
		{ "fd-userptr-invalidate", FD | USERPTR | INVALIDATE },
		{ "fd-userptr-invalidate-race", FD | USERPTR | INVALIDATE |
			RACE },
		{ "hang-basic", HANG | 0 },
		{ "hang-userptr", HANG | USERPTR },
		{ "hang-rebind", HANG | REBIND },
		{ "hang-userptr-rebind", HANG | USERPTR | REBIND },
		{ "hang-userptr-invalidate", HANG | USERPTR | INVALIDATE },
		{ "hang-userptr-invalidate-race", HANG | USERPTR | INVALIDATE |
			RACE },
		{ "hang-shared-vm-basic", HANG | SHARED_VM },
		{ "hang-shared-vm-userptr", HANG | SHARED_VM | USERPTR },
		{ "hang-shared-vm-rebind", HANG | SHARED_VM | REBIND },
		{ "hang-shared-vm-userptr-rebind", HANG | SHARED_VM | USERPTR |
			REBIND },
		{ "hang-rebind-err", HANG | REBIND | REBIND_ERROR },
		{ "hang-userptr-rebind-err", HANG | USERPTR | REBIND |
			REBIND_ERROR },
		{ "hang-shared-vm-userptr-invalidate", HANG | SHARED_VM |
			USERPTR | INVALIDATE },
		{ "hang-shared-vm-userptr-invalidate-race", HANG | SHARED_VM |
			USERPTR | INVALIDATE | RACE },
		{ "hang-fd-basic", HANG | FD },
		{ "hang-fd-userptr", HANG | FD | USERPTR },
		{ "hang-fd-rebind", HANG | FD | REBIND },
		{ "hang-fd-userptr-rebind", HANG | FD | USERPTR | REBIND },
		{ "hang-fd-userptr-invalidate", HANG | FD | USERPTR |
			INVALIDATE },
		{ "hang-fd-userptr-invalidate-race", HANG | FD | USERPTR |
			INVALIDATE | RACE },
		{ "bal-basic", BALANCER },
		{ "bal-userptr", BALANCER | USERPTR },
		{ "bal-rebind", BALANCER | REBIND },
		{ "bal-userptr-rebind", BALANCER | USERPTR | REBIND },
		{ "bal-userptr-invalidate", BALANCER | USERPTR | INVALIDATE },
		{ "bal-userptr-invalidate-race", BALANCER | USERPTR |
			INVALIDATE | RACE },
		{ "bal-shared-vm-basic", BALANCER | SHARED_VM },
		{ "bal-shared-vm-userptr", BALANCER | SHARED_VM | USERPTR },
		{ "bal-shared-vm-rebind", BALANCER | SHARED_VM | REBIND },
		{ "bal-shared-vm-userptr-rebind", BALANCER | SHARED_VM |
			USERPTR | REBIND },
		{ "bal-shared-vm-userptr-invalidate", BALANCER | SHARED_VM |
			USERPTR | INVALIDATE },
		{ "bal-shared-vm-userptr-invalidate-race", BALANCER |
			SHARED_VM | USERPTR | INVALIDATE | RACE },
		{ "bal-fd-basic", BALANCER | FD },
		{ "bal-fd-userptr", BALANCER | FD | USERPTR },
		{ "bal-fd-rebind", BALANCER | FD | REBIND },
		{ "bal-fd-userptr-rebind", BALANCER | FD | USERPTR | REBIND },
		{ "bal-fd-userptr-invalidate", BALANCER | FD | USERPTR |
			INVALIDATE },
		{ "bal-fd-userptr-invalidate-race", BALANCER | FD | USERPTR |
			INVALIDATE | RACE },
		{ "cm-basic", COMPUTE_MODE },
		{ "cm-userptr", COMPUTE_MODE | USERPTR },
		{ "cm-rebind", COMPUTE_MODE | REBIND },
		{ "cm-userptr-rebind", COMPUTE_MODE | USERPTR | REBIND },
		{ "cm-userptr-invalidate", COMPUTE_MODE | USERPTR |
			INVALIDATE },
		{ "cm-userptr-invalidate-race", COMPUTE_MODE | USERPTR |
			INVALIDATE | RACE },
		{ "cm-shared-vm-basic", COMPUTE_MODE | SHARED_VM },
		{ "cm-shared-vm-userptr", COMPUTE_MODE | SHARED_VM | USERPTR },
		{ "cm-shared-vm-rebind", COMPUTE_MODE | SHARED_VM | REBIND },
		{ "cm-shared-vm-userptr-rebind", COMPUTE_MODE | SHARED_VM |
			USERPTR | REBIND },
		{ "cm-shared-vm-userptr-invalidate", COMPUTE_MODE | SHARED_VM |
			USERPTR | INVALIDATE },
		{ "cm-shared-vm-userptr-invalidate-race", COMPUTE_MODE |
			SHARED_VM | USERPTR | INVALIDATE | RACE },
		{ "cm-fd-basic", COMPUTE_MODE | FD },
		{ "cm-fd-userptr", COMPUTE_MODE | FD | USERPTR },
		{ "cm-fd-rebind", COMPUTE_MODE | FD | REBIND },
		{ "cm-fd-userptr-rebind", COMPUTE_MODE | FD | USERPTR |
			REBIND },
		{ "cm-fd-userptr-invalidate", COMPUTE_MODE | FD |
			USERPTR | INVALIDATE },
		{ "cm-fd-userptr-invalidate-race", COMPUTE_MODE | FD |
			USERPTR | INVALIDATE | RACE },
		{ "mixed-basic", MIXED_MODE },
		{ "mixed-userptr", MIXED_MODE | USERPTR },
		{ "mixed-rebind", MIXED_MODE | REBIND },
		{ "mixed-userptr-rebind", MIXED_MODE | USERPTR | REBIND },
		{ "mixed-userptr-invalidate", MIXED_MODE | USERPTR |
			INVALIDATE },
		{ "mixed-userptr-invalidate-race", MIXED_MODE | USERPTR |
			INVALIDATE | RACE },
		{ "mixed-shared-vm-basic", MIXED_MODE | SHARED_VM },
		{ "mixed-shared-vm-userptr", MIXED_MODE | SHARED_VM |
			USERPTR },
		{ "mixed-shared-vm-rebind", MIXED_MODE | SHARED_VM | REBIND },
		{ "mixed-shared-vm-userptr-rebind", MIXED_MODE | SHARED_VM |
			USERPTR | REBIND },
		{ "mixed-shared-vm-userptr-invalidate", MIXED_MODE |
			SHARED_VM | USERPTR | INVALIDATE },
		{ "mixed-shared-vm-userptr-invalidate-race", MIXED_MODE |
			SHARED_VM | USERPTR | INVALIDATE | RACE },
		{ "mixed-fd-basic", MIXED_MODE | FD },
		{ "mixed-fd-userptr", MIXED_MODE | FD | USERPTR },
		{ "mixed-fd-rebind", MIXED_MODE | FD | REBIND },
		{ "mixed-fd-userptr-rebind", MIXED_MODE | FD | USERPTR |
			REBIND },
		{ "mixed-fd-userptr-invalidate", MIXED_MODE | FD |
			USERPTR | INVALIDATE },
		{ "mixed-fd-userptr-invalidate-race", MIXED_MODE | FD |
			USERPTR | INVALIDATE | RACE },
		{ "bal-mixed-basic", BALANCER | MIXED_MODE },
		{ "bal-mixed-userptr", BALANCER | MIXED_MODE | USERPTR },
		{ "bal-mixed-rebind", BALANCER | MIXED_MODE | REBIND },
		{ "bal-mixed-userptr-rebind", BALANCER | MIXED_MODE | USERPTR |
			REBIND },
		{ "bal-mixed-userptr-invalidate", BALANCER | MIXED_MODE |
			USERPTR | INVALIDATE },
		{ "bal-mixed-userptr-invalidate-race", BALANCER | MIXED_MODE |
			USERPTR | INVALIDATE | RACE },
		{ "bal-mixed-shared-vm-basic", BALANCER | MIXED_MODE |
			SHARED_VM },
		{ "bal-mixed-shared-vm-userptr", BALANCER | MIXED_MODE |
			SHARED_VM | USERPTR },
		{ "bal-mixed-shared-vm-rebind", BALANCER | MIXED_MODE |
			SHARED_VM | REBIND },
		{ "bal-mixed-shared-vm-userptr-rebind", BALANCER | MIXED_MODE |
			SHARED_VM | USERPTR | REBIND },
		{ "bal-mixed-shared-vm-userptr-invalidate", BALANCER |
			MIXED_MODE | SHARED_VM | USERPTR | INVALIDATE },
		{ "bal-mixed-shared-vm-userptr-invalidate-race", BALANCER |
			MIXED_MODE | SHARED_VM | USERPTR | INVALIDATE | RACE },
		{ "bal-mixed-fd-basic", BALANCER | MIXED_MODE | FD },
		{ "bal-mixed-fd-userptr", BALANCER | MIXED_MODE | FD |
			USERPTR },
		{ "bal-mixed-fd-rebind", BALANCER | MIXED_MODE | FD | REBIND },
		{ "bal-mixed-fd-userptr-rebind", BALANCER | MIXED_MODE | FD |
			USERPTR | REBIND },
		{ "bal-mixed-fd-userptr-invalidate", BALANCER | MIXED_MODE |
			FD | USERPTR | INVALIDATE },
		{ "bal-mixed-fd-userptr-invalidate-race", BALANCER |
			MIXED_MODE | FD | USERPTR | INVALIDATE | RACE },
		{ NULL },
	};
	int fd;

	igt_fixture()
		fd = drm_open_driver(DRIVER_XE);

	for (const struct section *s = sections; s->name; s++) {
		igt_subtest_f("threads-%s", s->name)
			threads(fd, s->flags);
	}

	igt_fixture()
		drm_close_driver(fd);
}
