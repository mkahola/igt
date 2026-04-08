// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

/**
 * TEST: Basic tests for execbuf functionality for virtual and parallel exec_queues
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: CMD submission
 * Functionality: reset
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
#include "xe/xe_legacy.h"
#include "xe/xe_spin.h"
#include <string.h>

#define SYNC_OBJ_SIGNALED	(0x1 << 0)
#define LEGACY_MODE_ADDR	0x1a0000

/**
 * SUBTEST: spin
 * Description: test spin
 *
 * SUBTEST: spin-signaled
 * Description: test spin with signaled sync obj
 */
static void test_spin(int fd, struct drm_xe_engine_class_instance *eci,
		      unsigned int flags)
{
	uint32_t vm;
	uint64_t addr = 0x1a0000;
	struct drm_xe_sync sync[2] = {
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, },
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, },
	};
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 2,
		.syncs = to_user_pointer(sync),
	};
	uint32_t exec_queue;
	uint32_t syncobj;
	size_t bo_size;
	uint32_t bo = 0;
	struct xe_spin *spin;
	struct xe_spin_opts spin_opts = { .addr = addr, .preempt = false };
	int i;

	vm = xe_vm_create(fd, 0, 0);
	bo_size = sizeof(*spin);
	bo_size = xe_bb_size(fd, bo_size);

	bo = xe_bo_create(fd, vm, bo_size,
			  vram_if_possible(fd, eci->gt_id),
			  DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	spin = xe_bo_map(fd, bo, bo_size);

	exec_queue = xe_exec_queue_create(fd, vm, eci, 0);
	syncobj = syncobj_create(fd, (flags & SYNC_OBJ_SIGNALED) ?
				 DRM_SYNCOBJ_CREATE_SIGNALED : 0);

	sync[0].handle = syncobj_create(fd, 0);
	xe_vm_bind_async(fd, vm, 0, bo, 0, addr, bo_size, sync, 1);

#define N_TIMES 4
	for (i = 0; i < N_TIMES; ++i) {
		xe_spin_init(spin, &spin_opts);

		sync[0].flags &= ~DRM_XE_SYNC_FLAG_SIGNAL;
		sync[1].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
		sync[1].handle = syncobj;

		exec.exec_queue_id = exec_queue;
		exec.address = addr;
		xe_exec(fd, &exec);

		xe_spin_wait_started(spin);
		usleep(50000);
		igt_assert(!syncobj_wait(fd, &syncobj, 1, 1, 0, NULL));
		xe_spin_end(spin);

		igt_assert(syncobj_wait(fd, &syncobj, 1, INT64_MAX, 0, NULL));
		igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));
	}
#undef N_TIMES

	sync[0].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
	xe_vm_unbind_async(fd, vm, 0, 0, addr, bo_size, sync, 1);
	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));

	syncobj_destroy(fd, sync[0].handle);
	syncobj_destroy(fd, syncobj);
	xe_exec_queue_destroy(fd, exec_queue);

	munmap(spin, bo_size);
	gem_close(fd, bo);
	xe_vm_destroy(fd, vm);
}

#define MAX_N_EXECQUEUES	16
#define GT_RESET			(0x1 << 0)
#define CLOSE_FD			(0x1 << 1)
#define CLOSE_EXEC_QUEUES	(0x1 << 2)
#define VIRTUAL				(0x1 << 3)
#define PARALLEL			(0x1 << 4)
#define CAT_ERROR			(0x1 << 5)
#define PREEMPT				(0x1 << 6)
#define CANCEL				(0x1 << 7)
#define LONG_SPIN			(0x1 << 8)
#define GT0				(0x1 << 9)
#define GT1				(0x1 << 10)
#define LONG_SPIN_REUSE_QUEUE		(0x1 << 11)
#define SYSTEM				(0x1 << 12)
#define COMPRESSION			(0x1 << 13)

/**
 * SUBTEST: %s-cat-error
 * Description: Test %arg[1] cat error
 *
 * SUBTEST: %s-gt-reset
 * Description: Test %arg[1] GT reset
 *
 * SUBTEST: virtual-close-fd-no-exec
 * Description: Test virtual close fd no-exec
 *
 * SUBTEST: parallel-close-fd-no-exec
 * Description: Test parallel close fd no-exec
 *
 * SUBTEST: %s-close-fd
 * Description: Test %arg[1] close fd
 *
 * SUBTEST: %s-close-execqueues-close-fd
 * Description: Test %arg[1] close exec_queues close fd
 *
 * arg[1]:
 *
 * @virtual:	virtual
 * @parallel:	parallel
 */

static void
test_balancer(int fd, int gt, int class, int n_exec_queues, int n_execs,
	      unsigned int flags)
{
	uint32_t vm;
	uint64_t addr = 0x1a0000;
	struct drm_xe_sync sync[2] = {
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, },
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, },
	};
	struct drm_xe_exec exec = {
		.num_syncs = 2,
		.syncs = to_user_pointer(sync),
	};
	uint32_t exec_queues[MAX_N_EXECQUEUES];
	uint32_t syncobjs[MAX_N_EXECQUEUES];
	size_t bo_size;
	uint32_t bo = 0;
	struct {
		struct xe_spin spin;
		uint32_t batch[16];
		uint64_t pad;
		uint32_t data;
	} *data;
	struct xe_spin_opts spin_opts = { .preempt = false };
	struct drm_xe_engine_class_instance eci[XE_MAX_ENGINE_INSTANCE];
	int i, j, b, num_placements, bad_batches = 1;

	igt_assert_lte(n_exec_queues, MAX_N_EXECQUEUES);

	if (flags & CLOSE_FD)
		fd = drm_open_driver(DRIVER_XE);

	num_placements = xe_gt_fill_engines_by_class(fd, gt, class, eci);
	if (num_placements < 2 ||
	    ((flags & PARALLEL) && !xe_engine_class_supports_multi_lrc(fd, class)))
		return;

	vm = xe_vm_create(fd, 0, 0);
	bo_size = sizeof(*data) * n_execs;
	bo_size = xe_bb_size(fd, bo_size);

	bo = xe_bo_create(fd, vm, bo_size, vram_if_possible(fd, gt),
			  DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	data = xe_bo_map(fd, bo, bo_size);

	for (i = 0; i < n_exec_queues; i++) {
		igt_assert_eq(__xe_exec_queue_create(fd, vm,
						     flags & PARALLEL ? num_placements : 1,
						     flags & PARALLEL ? 1 : num_placements,
						     eci, 0, &exec_queues[i]), 0);
		syncobjs[i] = syncobj_create(fd, 0);
	};
	exec.num_batch_buffer = flags & PARALLEL ? num_placements : 1;

	sync[0].handle = syncobj_create(fd, 0);
	xe_vm_bind_async(fd, vm, 0, bo, 0, addr, bo_size, sync, 1);

	if (flags & VIRTUAL && (flags & CAT_ERROR || flags & GT_RESET))
		bad_batches = num_placements;

	for (i = 0; i < n_execs; i++) {
		uint64_t base_addr = flags & CAT_ERROR && i < bad_batches ?
			addr + bo_size * 128 : addr;
		uint64_t batch_offset = (char *)&data[i].batch - (char *)data;
		uint64_t batch_addr = base_addr + batch_offset;
		uint64_t spin_offset = (char *)&data[i].spin - (char *)data;
		uint64_t sdi_offset = (char *)&data[i].data - (char *)data;
		uint64_t sdi_addr = base_addr + sdi_offset;
		uint64_t exec_addr;
		uint64_t batches[XE_MAX_ENGINE_INSTANCE];
		int e = i % n_exec_queues;

		for (j = 0; j < num_placements && flags & PARALLEL; ++j)
			batches[j] = batch_addr;

		if (i < bad_batches) {
			spin_opts.addr = base_addr + spin_offset;
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

		for (j = 0; j < num_placements && flags & PARALLEL; ++j)
			batches[j] = exec_addr;

		sync[0].flags &= ~DRM_XE_SYNC_FLAG_SIGNAL;
		sync[1].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
		sync[1].handle = syncobjs[e];

		exec.exec_queue_id = exec_queues[e];
		exec.address = flags & PARALLEL ?
			to_user_pointer(batches) : exec_addr;
		if (e != i)
			 syncobj_reset(fd, &syncobjs[e], 1);
		xe_exec(fd, &exec);

		if (i < bad_batches && !(flags & CAT_ERROR))
			xe_spin_wait_started(&data[i].spin);

	}

	if (flags & GT_RESET)
		xe_force_gt_reset_async(fd, gt);

	if (flags & CLOSE_FD) {
		if (flags & CLOSE_EXEC_QUEUES) {
			for (i = 0; i < n_exec_queues; i++)
				xe_exec_queue_destroy(fd, exec_queues[i]);
		}
		drm_close_driver(fd);
		/* FIXME: wait for idle */
		usleep(150000);
		return;
	}

	for (i = 0; i < n_exec_queues && n_execs; i++)
		igt_assert(syncobj_wait(fd, &syncobjs[i], 1, INT64_MAX, 0,
					NULL));
	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));

	sync[0].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
	xe_vm_unbind_async(fd, vm, 0, 0, addr, bo_size, sync, 1);
	igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));

	if (!(flags & GT_RESET)) {
		for (i = bad_batches; i < n_execs; i++)
			igt_assert_eq(data[i].data, 0xc0ffee);
	}

	syncobj_destroy(fd, sync[0].handle);
	for (i = 0; i < n_exec_queues; i++) {
		syncobj_destroy(fd, syncobjs[i]);
		xe_exec_queue_destroy(fd, exec_queues[i]);
	}

	munmap(data, bo_size);
	gem_close(fd, bo);
	xe_vm_destroy(fd, vm);
}

/**
 * SUBTEST: cat-error
 * Description: Test cat error
 *
 * SUBTEST: cancel
 * Description: Test job cancel
 *
 * SUBTEST: cancel-preempt
 * Description: Test job cancel with a preemptable job
 *
 * SUBTEST: cancel-timeslice-preempt
 * Description: Test job cancel with 2 preemptable jobs
 *
 * SUBTEST: cancel-timeslice-many-preempt
 * Description: Test job cancel with many preemptable jobs
 *
 * SUBTEST: long-spin-many-preempt
 * Description: Test long spinners with many preemptable jobs
 *
 * SUBTEST: long-spin-many-preempt-media
 * Description: Test long spinners with many preemptable jobs on media GT
 *
 * SUBTEST: long-spin-reuse-many-preempt
 * Description: Test long spinners with many preemptable jobs, use queues again spinners complete
 *
 * SUBTEST: long-spin-reuse-many-preempt-media
 * Description: Test long spinners with many preemptable jobs, use queues again spinners complete on media GT
 *
 * SUBTEST: gt-reset
 * Description: Test GT reset
 *
 * SUBTEST: close-fd-no-exec
 * Description: Test close fd no-exec
 *
 * SUBTEST: close-fd
 * Description: Test close fd
 *
 * SUBTEST: close-execqueues-close-fd
 * Description: Test close exec_queues close fd
 *
 * SUBTEST: cm-cat-error
 * Description: Test compute mode cat-error
 *
 * SUBTEST: cm-gt-reset
 * Description: Test compute mode GT reset
 *
 * SUBTEST: cm-close-fd-no-exec
 * Description: Test compute mode close fd no-exec
 *
 * SUBTEST: cm-close-fd
 * Description: Test compute mode close fd
 *
 * SUBTEST: cm-close-execqueues-close-fd
 * Description: Test compute mode close exec_queues close fd
 */

static void
test_compute_mode(int fd, struct drm_xe_engine_class_instance *eci,
		  int n_exec_queues, int n_execs, unsigned int flags)
{
	uint32_t vm;
	uint64_t addr = 0x1a0000;
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
	uint32_t exec_queues[MAX_N_EXECQUEUES];
	size_t bo_size;
	uint32_t bo = 0;
	struct {
		struct xe_spin spin;
		uint32_t batch[16];
		uint64_t pad;
		uint64_t vm_sync;
		uint64_t exec_sync;
		uint32_t data;
	} *data;
	struct xe_spin_opts spin_opts = { .preempt = flags & PREEMPT };
	int i, b;

	igt_assert_lte(n_exec_queues, MAX_N_EXECQUEUES);

	if (flags & CLOSE_FD)
		fd = drm_open_driver(DRIVER_XE);

	vm = xe_vm_create(fd, DRM_XE_VM_CREATE_FLAG_LR_MODE, 0);
	bo_size = sizeof(*data) * n_execs;
	bo_size = xe_bb_size(fd, bo_size);

	bo = xe_bo_create(fd, vm, bo_size,
			  vram_if_possible(fd, eci->gt_id),
			  DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	data = xe_bo_map(fd, bo, bo_size);
	memset(data, 0, bo_size);

	for (i = 0; i < n_exec_queues; i++) {
		exec_queues[i] = xe_exec_queue_create(fd, vm, eci, 0);
	};

	sync[0].addr = to_user_pointer(&data[0].vm_sync);
	xe_vm_bind_async(fd, vm, 0, bo, 0, addr, bo_size, sync, 1);

	xe_wait_ufence(fd, &data[0].vm_sync, USER_FENCE_VALUE, 0, 3 * NSEC_PER_SEC);
	data[0].vm_sync = 0;

	for (i = 0; i < n_execs; i++) {
		uint64_t base_addr = flags & CAT_ERROR && !i ?
			addr + bo_size * 128 : addr;
		uint64_t batch_offset = (char *)&data[i].batch - (char *)data;
		uint64_t batch_addr = base_addr + batch_offset;
		uint64_t spin_offset = (char *)&data[i].spin - (char *)data;
		uint64_t sdi_offset = (char *)&data[i].data - (char *)data;
		uint64_t sdi_addr = base_addr + sdi_offset;
		uint64_t exec_addr;
		int e = i % n_exec_queues;

		if (!i || flags & CANCEL) {
			spin_opts.addr = base_addr + spin_offset;
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

		sync[0].addr = base_addr +
			(char *)&data[i].exec_sync - (char *)data;

		exec.exec_queue_id = exec_queues[e];
		exec.address = exec_addr;
		xe_exec(fd, &exec);
	}

	if (flags & GT_RESET) {
		xe_spin_wait_started(&data[0].spin);
		xe_force_gt_reset_sync(fd, eci->gt_id);
	}

	if (flags & CLOSE_FD) {
		if (flags & CLOSE_EXEC_QUEUES) {
			for (i = 0; i < n_exec_queues; i++)
				xe_exec_queue_destroy(fd, exec_queues[i]);
		}
		drm_close_driver(fd);
		/* FIXME: wait for idle */
		usleep(150000);
		return;
	}

	for (i = 1; i < n_execs; i++) {
		int64_t timeout = 3 * NSEC_PER_SEC;
		int err;

		err = __xe_wait_ufence(fd, &data[i].exec_sync, USER_FENCE_VALUE,
				       exec_queues[i % n_exec_queues], &timeout);
		if (flags & GT_RESET || flags & CAT_ERROR)
			/* exec races with reset: may return -EIO or complete */
			igt_assert(err == -EIO || !err);
		else
			igt_assert_eq(err, 0);
	}

	sync[0].addr = to_user_pointer(&data[0].vm_sync);
	xe_vm_unbind_async(fd, vm, 0, 0, addr, bo_size, sync, 1);
	xe_wait_ufence(fd, &data[0].vm_sync, USER_FENCE_VALUE, 0, 3 * NSEC_PER_SEC);

	if (!(flags & (GT_RESET | CANCEL))) {
		for (i = 1; i < n_execs; i++)
			igt_assert_eq(data[i].data, 0xc0ffee);
	}

	for (i = 0; i < n_exec_queues; i++)
		xe_exec_queue_destroy(fd, exec_queues[i]);

	munmap(data, bo_size);
	gem_close(fd, bo);
	xe_vm_destroy(fd, vm);
}

struct gt_thread_data {
	pthread_t thread;
	pthread_mutex_t *mutex;
	pthread_cond_t *cond;
	int fd;
	int gt;
	int *go;
	int *exit;
	int *num_reset;
	bool do_reset;
};

static void do_resets(struct gt_thread_data *t)
{
	while (!*(t->exit)) {
		usleep(250000);	/* 250 ms */
		(*t->num_reset)++;
		xe_force_gt_reset_async(t->fd, t->gt);
	}
}

static void submit_jobs(struct gt_thread_data *t)
{
	int fd = t->fd;
	uint32_t vm = xe_vm_create(fd, 0, 0);
	uint64_t addr = 0x1a0000;
	size_t bo_size = xe_bb_size(fd, SZ_4K);
	uint32_t bo;
	uint32_t *data;

	bo = xe_bo_create(fd, vm, bo_size, vram_if_possible(fd, 0),
			  DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	data = xe_bo_map(fd, bo, bo_size);
	data[0] = MI_BATCH_BUFFER_END;

	xe_vm_bind_sync(fd, vm, bo, 0, addr, bo_size);

	while (!*(t->exit)) {
		struct drm_xe_engine_class_instance instance = {
			.engine_class = DRM_XE_ENGINE_CLASS_COPY,
			.engine_instance = 0,
			.gt_id = 0,
		};
		struct drm_xe_exec exec = {
			.address = addr,
			.num_batch_buffer = 1,
		};
		int ret;

		/* GuC IDs can get exhausted */
		ret = __xe_exec_queue_create(fd, vm, 1, 1, &instance, 0, &exec.exec_queue_id);
		if (ret)
			continue;

		xe_exec(fd, &exec);
		xe_exec_queue_destroy(fd, exec.exec_queue_id);
	}

	munmap(data, bo_size);
	gem_close(fd, bo);
	xe_vm_destroy(fd, vm);
}

static void *gt_reset_thread(void *data)
{
	struct gt_thread_data *t = data;

	pthread_mutex_lock(t->mutex);
	while (*t->go == 0)
		pthread_cond_wait(t->cond, t->mutex);
	pthread_mutex_unlock(t->mutex);

	if (t->do_reset)
		do_resets(t);
	else
		submit_jobs(t);

	return NULL;
}

/**
 * SUBTEST: gt-reset-stress
 * Description: Stress GT reset
 * Test category: stress test
 *
 */
static void
gt_reset(int fd, int n_threads, int n_sec)
{
	struct gt_thread_data *threads;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	int go = 0, exit = 0, num_reset = 0, i;

	threads = calloc(n_threads, sizeof(struct gt_thread_data));
	igt_assert(threads);

	pthread_mutex_init(&mutex, 0);
	pthread_cond_init(&cond, 0);

	for (i = 0; i < n_threads; ++i) {
		threads[i].mutex = &mutex;
		threads[i].cond = &cond;
		threads[i].fd = fd;
		threads[i].gt = 0;
		threads[i].go = &go;
		threads[i].exit = &exit;
		threads[i].num_reset = &num_reset;
		threads[i].do_reset = (i == 0);

		pthread_create(&threads[i].thread, 0, gt_reset_thread,
			       &threads[i]);
	}

	pthread_mutex_lock(&mutex);
	go = 1;
	pthread_cond_broadcast(&cond);
	pthread_mutex_unlock(&mutex);

	sleep(n_sec);
	exit = 1;

	for (i = 0; i < n_threads; i++)
		pthread_join(threads[i].thread, NULL);

	igt_info("number of resets %d\n", num_reset);

	free(threads);
}

/**
 * SUBTEST: gt-mocs-reset
 * Description: Validate mocs register contents over GT reset
 * Test category: mocs
 *
 */
static void
gt_mocs_reset(int fd, int gt)
{
	char path[256];
	char *mocs_content_pre, *mocs_contents_post;

	sprintf(path, "gt%d/mocs", gt);
	igt_require_f(igt_debugfs_exists(fd, path, O_RDONLY),
		      "Failed to open required debugfs entry: %s\n", path);

	/* Mocs debugfs contents before and after GT reset.
	 * Allocate memory to store 10k characters sufficient enough
	 * to store global mocs and lncf mocs data.
	 */
	mocs_content_pre = (char *)malloc(10000 * sizeof(char));
	mocs_contents_post = (char *)malloc(10000 * sizeof(char));

	igt_assert(mocs_content_pre);
	igt_assert(mocs_contents_post);

	igt_debugfs_dump(fd, path);
	igt_debugfs_read(fd, path, mocs_content_pre);

	xe_force_gt_reset_sync(fd, gt);

	igt_assert(igt_debugfs_exists(fd, path, O_RDONLY));
	igt_debugfs_dump(fd, path);
	igt_debugfs_read(fd, path, mocs_contents_post);

	igt_assert(strcmp(mocs_content_pre, mocs_contents_post) == 0);

	free(mocs_content_pre);
	free(mocs_contents_post);
}

struct thread_data {
	pthread_t thread;
	pthread_mutex_t *mutex;
	pthread_cond_t *cond;
	int fd;
	struct drm_xe_engine_class_instance *hwe;
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

	xe_legacy_test_mode(t->fd, t->hwe, t->n_exec_queue, t->n_exec,
			    t->flags, LEGACY_MODE_ADDR, false);

	return NULL;
}

/**
 * SUBTEST: long-spin-many-preempt-threads
 * Description: Test long spinners with many preemptable jobs on each engine instance with a thread, both GTs
 *
 * SUBTEST: long-spin-many-preempt-gt0-threads
 * Description: Test long spinners with many preemptable jobs on each engine instance with a thread, primary GT
 *
 * SUBTEST: long-spin-many-preempt-gt1-threads
 * Description: Test long spinners with many preemptable jobs on each engine instance with a thread, media GT
 *
 * SUBTEST: long-spin-reuse-many-preempt-threads
 * Description: Test long spinners with many preemptable jobs on each engine instance with a thread, use queues again spinners complete, both GTs
 *
 * SUBTEST: long-spin-sys-reuse-many-preempt-threads
 * Description: Test long spinners with many preemptable jobs on each engine instance with a thread, use queues again spinners complete, both GTs, use system memory
 *
 * SUBTEST: long-spin-comp-reuse-many-preempt-threads
 * Description: Test long spinners with many preemptable jobs on each engine instance with a thread, use queues again spinners complete, both GTs, use compressed memory
 *
 * SUBTEST: long-spin-reuse-many-preempt-gt0-threads
 * Description: Test long spinners with many preemptable jobs on each engine instance with a thread, use queues again spinners complete, primary GT
 *
 * SUBTEST: long-spin-reuse-many-preempt-gt1-threads
 * Description: Test long spinners with many preemptable jobs on each engine instance with a thread, use queues again spinners complete,  media GT
 */

static void threads(int fd, int n_exec_queues, int n_execs, unsigned int flags)
{
	struct thread_data *threads_data;
	struct drm_xe_engine_class_instance *hwe;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	int n_engines = 0, i = 0;
	bool go = false;

	xe_for_each_engine(fd, hwe) {
		if (hwe->gt_id && (flags & GT0))
			continue;
		if (!hwe->gt_id && (flags & GT1))
			continue;

		++n_engines;
	}

	threads_data = calloc(n_engines, sizeof(*threads_data));
	igt_assert(threads_data);

	pthread_mutex_init(&mutex, 0);
	pthread_cond_init(&cond, 0);

	xe_for_each_engine(fd, hwe) {
		if (hwe->gt_id && (flags & GT0))
			continue;
		if (!hwe->gt_id && (flags & GT1))
			continue;

		threads_data[i].fd = fd;
		threads_data[i].mutex = &mutex;
		threads_data[i].cond = &cond;
		threads_data[i].hwe = hwe;
		threads_data[i].n_exec_queue = n_exec_queues;
		threads_data[i].n_exec = n_execs;
		threads_data[i].flags = flags;
		threads_data[i].go = &go;

		pthread_create(&threads_data[i].thread, 0, thread,
			       &threads_data[i]);
		++i;
	}

	pthread_mutex_lock(&mutex);
	go = true;
	pthread_cond_broadcast(&cond);
	pthread_mutex_unlock(&mutex);

	for (i = 0; i < n_engines; ++i)
		pthread_join(threads_data[i].thread, NULL);

	free(threads_data);
}

int igt_main()
{
	struct drm_xe_engine_class_instance *hwe;
	const struct section {
		const char *name;
		unsigned int flags;
	} sections[] = {
		{ "virtual", VIRTUAL },
		{ "parallel", PARALLEL },
		{ NULL },
	};
	int gt;
	int class;
	int fd;

	igt_fixture()
		fd = drm_open_driver(DRIVER_XE);

	igt_subtest("spin")
		xe_for_each_engine(fd, hwe)
			test_spin(fd, hwe, 0);

	igt_subtest("spin-signaled")
		xe_for_each_engine(fd, hwe)
			test_spin(fd, hwe, SYNC_OBJ_SIGNALED);

	igt_subtest("cat-error")
		xe_for_each_engine(fd, hwe)
			xe_legacy_test_mode(fd, hwe, 2, 2, CAT_ERROR,
					    LEGACY_MODE_ADDR, false);

	igt_subtest("cancel")
		xe_for_each_engine(fd, hwe) {
			xe_legacy_test_mode(fd, hwe, 1, 1, 0,
					    LEGACY_MODE_ADDR, false);
			break;
		}

	igt_subtest("cancel-preempt")
		xe_for_each_engine(fd, hwe) {
			xe_legacy_test_mode(fd, hwe, 1, 1, PREEMPT,
					    LEGACY_MODE_ADDR, false);
			break;
		}

	igt_subtest("cancel-timeslice-preempt")
		xe_for_each_engine(fd, hwe) {
			xe_legacy_test_mode(fd, hwe, 2, 2, CANCEL | PREEMPT,
					    LEGACY_MODE_ADDR, false);
			break;
		}

	igt_subtest("cancel-timeslice-many-preempt")
		xe_for_each_engine(fd, hwe) {
			xe_legacy_test_mode(fd, hwe, 4, 4, CANCEL | PREEMPT,
					    LEGACY_MODE_ADDR, false);
			break;
		}

	igt_subtest("long-spin-many-preempt")
		xe_for_each_engine(fd, hwe) {
			xe_legacy_test_mode(fd, hwe, 4, 8,
					    LONG_SPIN | PREEMPT,
					    LEGACY_MODE_ADDR, false);
			break;
		}

	igt_subtest("long-spin-many-preempt-media")
		xe_for_each_engine(fd, hwe) {
			if (!hwe->gt_id)
				continue;
			xe_legacy_test_mode(fd, hwe, 4, 8,
					    LONG_SPIN | PREEMPT,
					    LEGACY_MODE_ADDR, false);
			break;
		}

	igt_subtest("long-spin-reuse-many-preempt")
		xe_for_each_engine(fd, hwe) {
			xe_legacy_test_mode(fd, hwe, 4, 8,
					    LONG_SPIN | PREEMPT |
					    LONG_SPIN_REUSE_QUEUE,
					    LEGACY_MODE_ADDR, false);
			break;
		}

	igt_subtest("long-spin-reuse-many-preempt-media")
		xe_for_each_engine(fd, hwe) {
			if (!hwe->gt_id)
				continue;
			xe_legacy_test_mode(fd, hwe, 4, 8,
					    LONG_SPIN | PREEMPT |
					    LONG_SPIN_REUSE_QUEUE,
					    LEGACY_MODE_ADDR, false);
			break;
		}

	igt_subtest("long-spin-many-preempt-threads")
		threads(fd, 2, 16, LONG_SPIN | PREEMPT);

	igt_subtest("long-spin-many-preempt-gt0-threads")
		threads(fd, 2, 16, LONG_SPIN | PREEMPT | GT0);

	igt_subtest("long-spin-many-preempt-gt1-threads")
		threads(fd, 2, 16, LONG_SPIN | PREEMPT | GT1);

	igt_subtest("long-spin-reuse-many-preempt-threads")
		threads(fd, 2, 16, LONG_SPIN | PREEMPT |
			LONG_SPIN_REUSE_QUEUE);

	igt_subtest("long-spin-sys-reuse-many-preempt-threads")
		threads(fd, 2, 16, SYSTEM | LONG_SPIN | PREEMPT |
			LONG_SPIN_REUSE_QUEUE);

	igt_subtest("long-spin-comp-reuse-many-preempt-threads")
		threads(fd, 2, 16, COMPRESSION | LONG_SPIN | PREEMPT |
			LONG_SPIN_REUSE_QUEUE);

	igt_subtest("long-spin-reuse-many-preempt-gt0-threads")
		threads(fd, 2, 16, LONG_SPIN | PREEMPT | GT0 |
			LONG_SPIN_REUSE_QUEUE);

	igt_subtest("long-spin-reuse-many-preempt-gt1-threads")
		threads(fd, 2, 16, LONG_SPIN | PREEMPT | GT1 |
			LONG_SPIN_REUSE_QUEUE);

	igt_subtest("gt-reset")
		xe_for_each_engine(fd, hwe)
			xe_legacy_test_mode(fd, hwe, 2, 2, GT_RESET,
					    LEGACY_MODE_ADDR, false);

	igt_subtest("close-fd-no-exec")
		xe_for_each_engine(fd, hwe)
			xe_legacy_test_mode(-1, hwe, 16, 0, CLOSE_FD,
					    LEGACY_MODE_ADDR, false);

	igt_subtest("close-fd")
		xe_for_each_engine(fd, hwe)
			xe_legacy_test_mode(-1, hwe, 16, 256, CLOSE_FD,
					    LEGACY_MODE_ADDR, false);

	igt_subtest("close-execqueues-close-fd")
		xe_for_each_engine(fd, hwe)
			xe_legacy_test_mode(-1, hwe, 16, 256, CLOSE_FD |
					    CLOSE_EXEC_QUEUES,
					    LEGACY_MODE_ADDR, false);

	igt_subtest("cm-cat-error")
		xe_for_each_engine(fd, hwe)
			test_compute_mode(fd, hwe, 2, 2, CAT_ERROR);

	igt_subtest("cm-gt-reset")
		xe_for_each_engine(fd, hwe)
			test_compute_mode(fd, hwe, 2, 2, GT_RESET);

	igt_subtest("cm-close-fd-no-exec")
		xe_for_each_engine(fd, hwe)
			test_compute_mode(-1, hwe, 16, 0, CLOSE_FD);

	igt_subtest("cm-close-fd")
		xe_for_each_engine(fd, hwe)
			test_compute_mode(-1, hwe, 16, 256, CLOSE_FD);

	igt_subtest("cm-close-execqueues-close-fd")
		xe_for_each_engine(fd, hwe)
			test_compute_mode(-1, hwe, 16, 256, CLOSE_FD |
					  CLOSE_EXEC_QUEUES);

	for (const struct section *s = sections; s->name; s++) {
		igt_subtest_f("%s-cat-error", s->name)
			xe_for_each_gt(fd, gt)
				xe_for_each_engine_class(class)
					test_balancer(fd, gt, class, XE_MAX_ENGINE_INSTANCE + 1,
						      XE_MAX_ENGINE_INSTANCE + 1,
						      CAT_ERROR | s->flags);

		igt_subtest_f("%s-gt-reset", s->name)
			xe_for_each_gt(fd, gt)
				xe_for_each_engine_class(class)
					test_balancer(fd, gt, class, XE_MAX_ENGINE_INSTANCE + 1,
						      XE_MAX_ENGINE_INSTANCE + 1,
						      GT_RESET | s->flags);

		igt_subtest_f("%s-close-fd-no-exec", s->name)
			xe_for_each_gt(fd, gt)
				xe_for_each_engine_class(class)
					test_balancer(-1, gt, class, 16, 0,
						      CLOSE_FD | s->flags);

		igt_subtest_f("%s-close-fd", s->name)
			xe_for_each_gt(fd, gt)
				xe_for_each_engine_class(class)
					test_balancer(-1, gt, class, 16, 256,
						      CLOSE_FD | s->flags);

		igt_subtest_f("%s-close-execqueues-close-fd", s->name)
			xe_for_each_gt(fd, gt)
				xe_for_each_engine_class(class)
					test_balancer(-1, gt, class, 16, 256, CLOSE_FD |
						      CLOSE_EXEC_QUEUES | s->flags);
	}

	igt_subtest("gt-reset-stress")
		gt_reset(fd, 4, 1);

	igt_subtest("gt-mocs-reset")
		xe_for_each_gt(fd, gt)
			gt_mocs_reset(fd, gt);

	igt_fixture()
		drm_close_driver(fd);
}
