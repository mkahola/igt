// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

/**
 * TEST: Basic tests for execbuf compute machine functionality
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: CMD submission
 * Functionality: compute test
 */

#include <fcntl.h>

#include "igt.h"
#include "lib/igt_syncobj.h"
#include "lib/intel_reg.h"
#include <sys/ioctl.h>
#include "xe_drm.h"

#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"
#include "xe/xe_spin.h"
#include <string.h>

#define MAX_N_EXECQUEUES 	16
#define USERPTR				(0x1 << 0)
#define REBIND				(0x1 << 1)
#define INVALIDATE			(0x1 << 2)
#define RACE				(0x1 << 3)
#define BIND_EXECQUEUE			(0x1 << 4)
#define VM_FOR_BO			(0x1 << 5)
#define EXEC_QUEUE_EARLY		(0x1 << 6)
#define FREE_MAPPPING			(0x1 << 7)
#define UNMAP_MAPPPING			(0x1 << 8)

/**
 * SUBTEST: twice-%s
 * Description: Run %arg[1] compute machine test twice
 * Test category: functionality test
 *
 * SUBTEST: once-%s
 * Description: Run %arg[1] compute machine test only once
 * Test category: functionality test
 *
 * SUBTEST: many-%s
 * Description: Run %arg[1] compute machine test many times
 * Test category: stress test
 *
 * arg[1]:
 *
 * @basic:				basic
 * @preempt-fence-early:		preempt fence early
 * @userptr:				userptr
 * @userptr-free:			userptr free
 * @userptr-unmap:			userptr unmap
 * @rebind:				rebind
 * @userptr-rebind:			userptr rebind
 * @userptr-invalidate:			userptr invalidate
 * @userptr-invalidate-race:		userptr invalidate race
 * @bindexecqueue:				bindexecqueue
 * @bindexecqueue-userptr:			bindexecqueue userptr
 * @bindexecqueue-rebind:			bindexecqueue rebind
 * @bindexecqueue-userptr-rebind:		bindexecqueue userptr rebind
 * @bindexecqueue-userptr-invalidate:	bindexecqueue userptr invalidate
 * @bindexecqueue-userptr-invalidate-race:	bindexecqueue-userptr invalidate race
 */

/**
 *
 * SUBTEST: many-execqueues-%s
 * Description: Run %arg[1] compute machine test on many exec_queues
 * Test category: functionality test
 *
 * arg[1]:
 *
 * @basic:				basic
 * @preempt-fence-early:		preempt fence early
 * @userptr:				userptr
 * @userptr-free:			userptr free
 * @userptr-unmap:			userptr unmap
 * @rebind:				rebind
 * @userptr-rebind:			userptr rebind
 * @userptr-invalidate:			userptr invalidate
 * @bindexecqueue:				bindexec_queue
 * @bindexecqueue-userptr:			bindexecqueue userptr
 * @bindexecqueue-rebind:			bindexecqueue rebind
 * @bindexecqueue-userptr-rebind:		bindexecqueue userptr rebind
 * @bindexecqueue-userptr-invalidate:	bindexecqueue userptr invalidate
 */
static void
test_exec(int fd, struct drm_xe_engine_class_instance *eci,
	  int n_exec_queues, int n_execs, unsigned int flags)
{
	uint32_t vm;
	uint64_t addr = 0x1a0000, dummy_addr = 0x10001a0000;
#define USER_FENCE_VALUE	0xdeadbeefdeadbeefull
	struct drm_xe_sync sync[1] = {
		{ .type = DRM_XE_SYNC_TYPE_USER_FENCE,
		  .flags = DRM_XE_SYNC_FLAG_SIGNAL,
		  .timeline_value = USER_FENCE_VALUE },
	};
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 1,
		.syncs = to_user_pointer(sync),
	};
	uint32_t exec_queues[MAX_N_EXECQUEUES];
	uint32_t bind_exec_queues[MAX_N_EXECQUEUES];
	size_t bo_size;
	uint32_t bo = 0;
	struct {
		uint32_t batch[16];
		uint64_t pad;
		uint64_t vm_sync;
		uint64_t exec_sync;
		uint32_t data;
	} *data;
	int i, j, b;
	int map_fd = -1;
	int64_t fence_timeout;
	void *dummy;

	igt_debug("%s running on: %s\n", __func__, xe_engine_class_string(eci->engine_class));
	igt_assert_lte(n_exec_queues, MAX_N_EXECQUEUES);

	vm = xe_vm_create(fd, DRM_XE_VM_CREATE_FLAG_LR_MODE, 0);
	bo_size = sizeof(*data) * n_execs;
	bo_size = xe_bb_size(fd, bo_size);

	for (i = 0; (flags & EXEC_QUEUE_EARLY) && i < n_exec_queues; i++) {
		exec_queues[i] = xe_exec_queue_create(fd, vm, eci, 0);
		if (flags & BIND_EXECQUEUE)
			bind_exec_queues[i] =
				xe_bind_exec_queue_create(fd, vm, 0);
		else
			bind_exec_queues[i] = 0;
	};

	if (flags & USERPTR) {
#define	MAP_ADDRESS	0x00007fadeadbe000
		if (flags & INVALIDATE) {
			data = mmap((void *)MAP_ADDRESS, bo_size, PROT_READ |
				    PROT_WRITE, MAP_SHARED | MAP_FIXED |
				    MAP_ANONYMOUS, -1, 0);
			igt_assert(data != MAP_FAILED);
		} else {
			data = aligned_alloc(xe_get_default_alignment(fd),
					     bo_size);
			igt_assert(data);
		}
		if (flags & UNMAP_MAPPPING) {
			dummy = mmap((void *)MAP_ADDRESS, bo_size, PROT_READ |
				     PROT_WRITE, MAP_SHARED | MAP_FIXED |
				     MAP_ANONYMOUS, -1, 0);
			igt_assert(dummy != MAP_FAILED);
		}
		if (flags & FREE_MAPPPING) {
			dummy = aligned_alloc(xe_get_default_alignment(fd),
					      bo_size);
			igt_assert(dummy);
		}
	} else {
		bo = xe_bo_create(fd, flags & VM_FOR_BO ? vm : 0,
				  bo_size, vram_if_possible(fd, eci->gt_id),
				  DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
		data = xe_bo_map(fd, bo, bo_size);
	}
	memset(data, 0, bo_size);

	for (i = 0; !(flags & EXEC_QUEUE_EARLY) && i < n_exec_queues; i++) {
		exec_queues[i] = xe_exec_queue_create(fd, vm, eci, 0);
		if (flags & BIND_EXECQUEUE)
			bind_exec_queues[i] =
				xe_bind_exec_queue_create(fd, vm, 0);
		else
			bind_exec_queues[i] = 0;
	}

	sync[0].addr = to_user_pointer(&data[0].vm_sync);
	if (bo) {
		xe_vm_bind_async(fd, vm, bind_exec_queues[0], bo, 0, addr,
				 bo_size, sync, 1);
	} else {
		if (flags & (FREE_MAPPPING | UNMAP_MAPPPING))
			xe_vm_bind_userptr_async(fd, vm, bind_exec_queues[0],
						 to_user_pointer(dummy),
						 dummy_addr, bo_size, 0, 0);
		xe_vm_bind_userptr_async(fd, vm, bind_exec_queues[0],
					 to_user_pointer(data), addr,
					 bo_size, sync, 1);
	}

	fence_timeout = (igt_run_in_simulation() ? 100 : 1) * NSEC_PER_SEC;

	xe_wait_ufence(fd, &data[0].vm_sync, USER_FENCE_VALUE,
		       bind_exec_queues[0], fence_timeout);
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

		sync[0].addr = addr + (char *)&data[i].exec_sync - (char *)data;

		exec.exec_queue_id = exec_queues[e];
		exec.address = batch_addr;
		xe_exec(fd, &exec);

		if (flags & FREE_MAPPPING && !i)
			free(dummy);

		if (flags & UNMAP_MAPPPING && !i)
			munmap(dummy, bo_size);

		if (flags & REBIND && i + 1 != n_execs) {
			xe_wait_ufence(fd, &data[i].exec_sync, USER_FENCE_VALUE,
				       exec_queues[e], fence_timeout);
			xe_vm_unbind_async(fd, vm, bind_exec_queues[e], 0,
					   addr, bo_size, NULL, 0);

			sync[0].addr = to_user_pointer(&data[0].vm_sync);
			addr += bo_size;
			if (bo)
				xe_vm_bind_async(fd, vm, bind_exec_queues[e], bo,
						 0, addr, bo_size, sync, 1);
			else
				xe_vm_bind_userptr_async(fd, vm,
							 bind_exec_queues[e],
							 to_user_pointer(data),
							 addr, bo_size, sync,
							 1);
			xe_wait_ufence(fd, &data[0].vm_sync, USER_FENCE_VALUE,
				       bind_exec_queues[e], fence_timeout);
			data[0].vm_sync = 0;
		}

		if (flags & INVALIDATE && i + 1 != n_execs) {
			if (!(flags & RACE)) {
				/*
				 * Wait for exec completion and check data as
				 * userptr will likely change to different
				 * physical memory on next mmap call triggering
				 * an invalidate.
				 */
				xe_wait_ufence(fd, &data[i].exec_sync,
					       USER_FENCE_VALUE, exec_queues[e],
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
					      0666);
				igt_assert_neq(map_fd, -1);
				igt_assert_eq(write(map_fd, data, bo_size),
				              bo_size);
				data = mmap((void *)MAP_ADDRESS, bo_size,
					    PROT_READ | PROT_WRITE, MAP_SHARED |
					    MAP_FIXED, map_fd, 0);
			} else {
				data = mmap((void *)MAP_ADDRESS, bo_size,
					    PROT_READ | PROT_WRITE, MAP_SHARED |
					    MAP_FIXED | MAP_ANONYMOUS, -1, 0);
			}
			igt_assert(data != MAP_FAILED);
		}
	}

	j = flags & INVALIDATE ? n_execs - 1 : 0;
	for (i = j; i < n_execs; i++)
		xe_wait_ufence(fd, &data[i].exec_sync, USER_FENCE_VALUE,
			       exec_queues[i % n_exec_queues], fence_timeout);

	/* Wait for all execs to complete */
	if (flags & INVALIDATE)
		usleep(250000);

	sync[0].addr = to_user_pointer(&data[0].vm_sync);
	xe_vm_unbind_async(fd, vm, bind_exec_queues[0], 0, addr, bo_size,
			   sync, 1);
	xe_wait_ufence(fd, &data[0].vm_sync, USER_FENCE_VALUE,
		       bind_exec_queues[0], fence_timeout);

	for (i = j; i < n_execs; i++)
		igt_assert_eq(data[i].data, 0xc0ffee);

	for (i = 0; i < n_exec_queues; i++) {
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
	xe_vm_destroy(fd, vm);
	if (map_fd != -1)
		close(map_fd);
}

#define LR_SPINNER_TIME 30
/**
 * SUBTEST: lr-mode-workload
 * Description: Stress LR mode workload for 30s.
 * Test category: functionality test
 */
static void lr_mode_workload(int fd)
{
	struct drm_xe_sync sync = {
		.type = DRM_XE_SYNC_TYPE_USER_FENCE,
		.flags = DRM_XE_SYNC_FLAG_SIGNAL,
		.timeline_value = USER_FENCE_VALUE,
	};
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 1,
		.syncs = to_user_pointer(&sync),
	};
	struct xe_spin *spin;
	struct drm_xe_engine *engine;
	uint64_t vm_sync;
	size_t bo_size;
	uint32_t vm;
	uint32_t exec_queue;
	uint64_t spin_addr;
	uint64_t ahnd;
	uint32_t bo;
	uint32_t ts_1, ts_2;

	vm = xe_vm_create(fd, DRM_XE_VM_CREATE_FLAG_LR_MODE, 0);
	ahnd = intel_allocator_open(fd, 0, INTEL_ALLOCATOR_RELOC);
	bo_size = xe_bb_size(fd, sizeof(*spin));
	engine = xe_find_engine_by_class(fd, DRM_XE_ENGINE_CLASS_COPY);
	igt_assert(engine);

	bo = xe_bo_create(fd, vm, bo_size, vram_if_possible(fd, engine->instance.gt_id),
			  DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	spin = xe_bo_map(fd, bo, bo_size);

	exec_queue = xe_exec_queue_create(fd, vm, &engine->instance, 0);
	spin_addr = intel_allocator_alloc_with_strategy(ahnd, bo, bo_size, 0,
							ALLOC_STRATEGY_LOW_TO_HIGH);

	sync.addr = to_user_pointer(&vm_sync);
	xe_vm_bind_async(fd, vm, 0, bo, 0, spin_addr, bo_size, &sync, 1);
	xe_wait_ufence(fd, &vm_sync, USER_FENCE_VALUE, 0, NSEC_PER_SEC);

	xe_spin_init_opts(spin, .addr = spin_addr, .write_timestamp = true);
	sync.addr = spin_addr + (char *)&spin->exec_sync - (char *)spin;
	exec.exec_queue_id = exec_queue;
	exec.address = spin_addr;
	xe_exec(fd, &exec);
	xe_spin_wait_started(spin);

	/* Collect and check timestamps before stopping the spinner */
	sleep(LR_SPINNER_TIME);
	ts_1 = spin->timestamp;
	sleep(1);
	ts_2 = spin->timestamp;
	igt_assert_neq_u32(ts_1, ts_2);

	xe_spin_end(spin);
	xe_wait_ufence(fd, &spin->exec_sync, USER_FENCE_VALUE, 0, NSEC_PER_SEC);

	/* Check timestamps to make sure spinner is stopped */
	ts_1 = spin->timestamp;
	sleep(1);
	ts_2 = spin->timestamp;
	igt_assert_eq_u32(ts_1, ts_2);

	sync.addr = to_user_pointer(&vm_sync);
	xe_vm_unbind_async(fd, vm, 0, 0, spin_addr, bo_size, &sync, 1);
	xe_wait_ufence(fd, &vm_sync, USER_FENCE_VALUE, 0, NSEC_PER_SEC);
	munmap(spin, bo_size);
	gem_close(fd, bo);

	xe_exec_queue_destroy(fd, exec_queue);
	xe_vm_destroy(fd, vm);
	put_ahnd(ahnd);
}

int igt_main()
{
	struct drm_xe_engine_class_instance *hwe;
	const struct section {
		const char *name;
		unsigned int flags;
	} sections[] = {
		{ "basic", 0 },
		{ "preempt-fence-early", VM_FOR_BO | EXEC_QUEUE_EARLY },
		{ "userptr", USERPTR },
		{ "userptr-free", USERPTR | FREE_MAPPPING },
		{ "userptr-unmap", USERPTR | UNMAP_MAPPPING },
		{ "rebind", REBIND },
		{ "userptr-rebind", USERPTR | REBIND },
		{ "userptr-invalidate", USERPTR | INVALIDATE },
		{ "userptr-invalidate-race", USERPTR | INVALIDATE | RACE },
		{ "bindexecqueue", BIND_EXECQUEUE },
		{ "bindexecqueue-userptr", BIND_EXECQUEUE | USERPTR },
		{ "bindexecqueue-rebind",  BIND_EXECQUEUE | REBIND },
		{ "bindexecqueue-userptr-rebind",  BIND_EXECQUEUE | USERPTR |
			REBIND },
		{ "bindexecqueue-userptr-invalidate",  BIND_EXECQUEUE | USERPTR |
			INVALIDATE },
		{ "bindexecqueue-userptr-invalidate-race", BIND_EXECQUEUE | USERPTR |
			INVALIDATE | RACE },
		{ NULL },
	};
	int fd;

	igt_fixture()
		fd = drm_open_driver(DRIVER_XE);

	for (const struct section *s = sections; s->name; s++) {
		igt_subtest_f("once-%s", s->name)
			xe_for_each_engine(fd, hwe)
				test_exec(fd, hwe, 1, 1, s->flags);

		igt_subtest_f("twice-%s", s->name)
			xe_for_each_engine(fd, hwe)
				test_exec(fd, hwe, 1, 2, s->flags);

		igt_subtest_f("many-%s", s->name)
			xe_for_each_engine(fd, hwe)
				test_exec(fd, hwe, 1,
					  s->flags & (REBIND | INVALIDATE) ?
					  64 : 128,
					  s->flags);

		if (s->flags & RACE)
			continue;

		igt_subtest_f("many-execqueues-%s", s->name)
			xe_for_each_engine(fd, hwe)
				test_exec(fd, hwe, 16,
					  s->flags & (REBIND | INVALIDATE) ?
					  64 : 128,
					  s->flags);
	}

	igt_subtest("lr-mode-workload")
		lr_mode_workload(fd);


	igt_fixture()
		drm_close_driver(fd);
}
