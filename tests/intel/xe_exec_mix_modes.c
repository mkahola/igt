// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 Intel Corporation
 */

/**
 * TEST: Test the parallel submission of jobs in LR and dma fence modes
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: CMD submission
 * Functionality: fault mode
 * GPU requirements: GPU needs support for DRM_XE_VM_CREATE_FLAG_FAULT_MODE
 */

#include <fcntl.h>

#include "igt.h"
#include "lib/igt_syncobj.h"
#include "lib/intel_reg.h"
#include "xe_drm.h"

#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"
#include "xe/xe_spin.h"
#include "xe/xe_util.h"
#include <string.h>

#define FLAG_EXEC_MODE_LR	(0x1 << 0)
#define FLAG_JOB_TYPE_SIMPLE	(0x1 << 1)

#define NUM_INTERRUPTING_JOBS	1
#define USER_FENCE_VALUE	0xdeadbeefdeadbeefull
#define VM_DATA			0
#define SPIN_DATA		1
#define EXEC_DATA		2
#define DATA_COUNT		3

struct data {
	struct xe_spin spin;
	uint32_t batch[16];
	uint64_t vm_sync;
	uint32_t data;
	uint64_t exec_sync;
	uint64_t addr;
};

static void store_dword_batch(struct data *data, uint64_t addr, int value)
{
	int b;
	uint64_t batch_offset = (char *)&(data->batch) - (char *)data;
	uint64_t batch_addr = addr + batch_offset;
	uint64_t sdi_offset = (char *)&(data->data) - (char *)data;
	uint64_t sdi_addr = addr + sdi_offset;

	b = 0;
	data->batch[b++] = MI_STORE_DWORD_IMM_GEN4;
	data->batch[b++] = sdi_addr;
	data->batch[b++] = sdi_addr >> 32;
	data->batch[b++] = value;
	data->batch[b++] = MI_BATCH_BUFFER_END;
	igt_assert(b <= ARRAY_SIZE(data->batch));

	data->addr = batch_addr;
}

enum engine_execution_mode {
	EXEC_MODE_LR,
	EXEC_MODE_DMA_FENCE,
};

enum job_type {
	SIMPLE_BATCH_STORE,
	SPINNER_INTERRUPTED,
};

static void
run_job(int fd, struct drm_xe_engine_class_instance *hwe,
	enum engine_execution_mode engine_execution_mode,
	enum job_type job_type, bool allow_recursion,
	struct xe_spin *dma_fence_job_spin)
{
	struct drm_xe_sync sync[1] = {
		{ .flags = DRM_XE_SYNC_FLAG_SIGNAL, },
	};
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 1,
		.syncs = to_user_pointer(&sync),
	};
	struct data *data;
	uint32_t vm;
	uint32_t exec_queue;
	size_t bo_size;
	int value = 0x123456;
	uint64_t addr = 0x100000;
	uint32_t bo = 0;
	unsigned int vm_flags = 0;
	struct xe_spin_opts spin_opts = { .preempt = true };
	struct timespec tv;
	enum engine_execution_mode interrupting_engine_execution_mode;
	int64_t timeout_short = 1;

	if (engine_execution_mode == EXEC_MODE_LR) {
		sync[0].type = DRM_XE_SYNC_TYPE_USER_FENCE;
		sync[0].timeline_value = USER_FENCE_VALUE;
		vm_flags = DRM_XE_VM_CREATE_FLAG_LR_MODE | DRM_XE_VM_CREATE_FLAG_FAULT_MODE;
	} else if (engine_execution_mode == EXEC_MODE_DMA_FENCE) {
		sync[0].type = DRM_XE_SYNC_TYPE_SYNCOBJ;
		sync[0].handle = syncobj_create(fd, 0);
	}

	vm = xe_vm_create(fd, vm_flags, 0);
	bo_size = sizeof(*data) * DATA_COUNT;
	bo_size = xe_bb_size(fd, bo_size);
	bo = xe_bo_create(fd, vm, bo_size,
			  vram_if_possible(fd, hwe->gt_id),
			  DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	data = xe_bo_map(fd, bo, bo_size);
	if (engine_execution_mode == EXEC_MODE_LR)
		sync[0].addr = to_user_pointer(&data[VM_DATA].vm_sync);
	xe_vm_bind_async(fd, vm, 0, bo, 0, addr, bo_size, &sync[0], 1);

	store_dword_batch(data, addr, value);
	if (engine_execution_mode == EXEC_MODE_LR) {
		xe_wait_ufence(fd, &data[VM_DATA].vm_sync, USER_FENCE_VALUE, 0, NSEC_PER_SEC);
		sync[0].addr = addr + (char *)&data[EXEC_DATA].exec_sync - (char *)data;
	} else if (engine_execution_mode == EXEC_MODE_DMA_FENCE) {
		igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));
		syncobj_reset(fd, &sync[0].handle, 1);
		sync[0].flags &= DRM_XE_SYNC_FLAG_SIGNAL;
	}
	exec_queue = xe_exec_queue_create(fd, vm, hwe, 0);
	exec.exec_queue_id = exec_queue;

	if (job_type == SPINNER_INTERRUPTED) {
		spin_opts.addr = addr + (char *)&data[SPIN_DATA].spin - (char *)data;
		xe_spin_init(&data[SPIN_DATA].spin, &spin_opts);
		if (engine_execution_mode == EXEC_MODE_LR)
			sync[0].addr = addr + (char *)&data[SPIN_DATA].exec_sync - (char *)data;
		exec.address = spin_opts.addr;
	} else if (job_type == SIMPLE_BATCH_STORE) {
		exec.address = data->addr;
	}
	xe_exec(fd, &exec);

	if (job_type == SPINNER_INTERRUPTED) {
		if (engine_execution_mode == EXEC_MODE_LR)
			interrupting_engine_execution_mode = EXEC_MODE_DMA_FENCE;
		else if (engine_execution_mode == EXEC_MODE_DMA_FENCE)
			interrupting_engine_execution_mode = EXEC_MODE_LR;
		xe_spin_wait_started(&data[SPIN_DATA].spin);
	} else if (job_type == SIMPLE_BATCH_STORE) {
		interrupting_engine_execution_mode = engine_execution_mode;
	}

	if (allow_recursion) {
		igt_gettime(&tv);
		for (int i = 0; i < NUM_INTERRUPTING_JOBS; i++)
		{
			struct xe_spin *spin_arg;

			if (job_type == SPINNER_INTERRUPTED &&
			    engine_execution_mode == EXEC_MODE_DMA_FENCE &&
			    interrupting_engine_execution_mode == EXEC_MODE_LR)
				/**
				 * In this case, jobs in LR mode are submitted while a job in dma
				 * fence mode is running. It is expected that the KMD will wait
				 * for completion of the dma fence job before executing the jobs
				 * in LR mode. Provide a pointer to the spinner to the interrupting
				 * dma fence job so that it can check that it was blocked, then
				 * end the spinner, then check that it was unblocked and completed,
				 * see "if (dma_fence_job_spin) ... " below.
				 */
				spin_arg = &data[SPIN_DATA].spin;

			run_job(fd, hwe, interrupting_engine_execution_mode, SIMPLE_BATCH_STORE,
				false, spin_arg);

			if (job_type == SPINNER_INTERRUPTED &&
			    engine_execution_mode == EXEC_MODE_LR &&
			    interrupting_engine_execution_mode == EXEC_MODE_DMA_FENCE) {
				/**
				 * In that case, jobs in dma fence mode are submitted while a job
				 * in LR mode is running. It is expected that the KMD will preempt
				 * the LR mode job to execute the dma fence mode jobs. At this
				 * point the dma fence job has completed, check that the LR mode
				 * job is still running, meaning was successfully preempted.
				 */
				igt_assert_neq(0, __xe_wait_ufence(fd, &data[SPIN_DATA].exec_sync,
								   USER_FENCE_VALUE,
								   0, &timeout_short));
			}
		}
	}

	if (dma_fence_job_spin) {
		igt_assert_neq(0, __xe_wait_ufence(fd, &data[EXEC_DATA].exec_sync,
						   USER_FENCE_VALUE, 0, &timeout_short));
		xe_spin_end(dma_fence_job_spin);
	} else if (job_type == SPINNER_INTERRUPTED &&
		   engine_execution_mode == EXEC_MODE_LR) {
		xe_spin_end(&data[SPIN_DATA].spin);
	}

	if (engine_execution_mode == EXEC_MODE_LR) {
		if (job_type == SPINNER_INTERRUPTED)
			xe_wait_ufence(fd, &data[SPIN_DATA].exec_sync, USER_FENCE_VALUE, 0, NSEC_PER_SEC);
		else if (job_type == SIMPLE_BATCH_STORE)
			xe_wait_ufence(fd, &data[EXEC_DATA].exec_sync, USER_FENCE_VALUE, 0, NSEC_PER_SEC);
	} else if (engine_execution_mode == EXEC_MODE_DMA_FENCE) {
		igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));
		syncobj_destroy(fd, sync[0].handle);
	}

	if (job_type == SIMPLE_BATCH_STORE)
		igt_assert_eq(data->data, value);

	munmap(data, bo_size);
	gem_close(fd, bo);
	xe_exec_queue_destroy(fd, exec_queue);
	xe_vm_destroy(fd, vm);
}

/**
 * SUBTEST: exec-simple-batch-store-lr
 * Description: Execute a simple batch store job in long running mode
 *
 * SUBTEST: exec-simple-batch-store-dma-fence
 * Description: Execute a simple batch store job in dma fence mode
 *
 * SUBTEST: exec-spinner-interrupted-lr
 * Description: Spin in long running mode then get interrupted by a simple
 *              batch store job in dma fence mode
 *
 * SUBTEST: exec-spinner-interrupted-dma-fence
 * Description: Spin in dma fence mode then get interrupted by a simple
 *              batch store job in long running mode
 */
static void
test_exec(int fd, struct drm_xe_engine_class_instance *hwe,
	  unsigned int flags)
{
	enum engine_execution_mode engine_execution_mode;
	enum job_type job_type;

	if (flags & FLAG_EXEC_MODE_LR)
		engine_execution_mode = EXEC_MODE_LR;
	else
		engine_execution_mode = EXEC_MODE_DMA_FENCE;

	if (flags & FLAG_JOB_TYPE_SIMPLE)
		job_type = SIMPLE_BATCH_STORE;
	else
		job_type = SPINNER_INTERRUPTED;

	run_job(fd, hwe, engine_execution_mode, job_type, true, NULL);
}

int igt_main()
{
	struct drm_xe_engine_class_instance *hwe;
	const struct section {
		const char *name;
		unsigned int flags;
	} sections[] = {
		{ "simple-batch-store-lr", FLAG_JOB_TYPE_SIMPLE | FLAG_EXEC_MODE_LR },
		{ "simple-batch-store-dma-fence", FLAG_JOB_TYPE_SIMPLE },
		{ "spinner-interrupted-lr", FLAG_EXEC_MODE_LR },
		{ "spinner-interrupted-dma-fence", 0 },
		{ NULL },
	};
	int fd;

	igt_fixture() {
		bool supports_faults;
		int ret = 0;

		fd = drm_open_driver(DRIVER_XE);
		ret = xe_supports_faults(fd);
		supports_faults = !ret;
		igt_require(supports_faults);
	}

	for (const struct section *s = sections; s->name; s++) {
		igt_subtest_f("exec-%s", s->name)
			xe_for_each_engine(fd, hwe)
				if (hwe->engine_class == DRM_XE_ENGINE_CLASS_COMPUTE)
					test_exec(fd, hwe, s->flags);
	}

	igt_fixture() {
		drm_close_driver(fd);
	}
}
