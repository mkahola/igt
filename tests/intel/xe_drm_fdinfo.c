// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

#include <math.h>

#include "igt.h"
#include "igt_core.h"
#include "igt_device.h"
#include "igt_drm_fdinfo.h"
#include "lib/igt_syncobj.h"

#include "xe_drm.h"
#include "xe/xe_gt.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"
#include "xe/xe_spin.h"

/**
 * TEST: xe drm fdinfo
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: driver
 * Functionality: Per client memory and engine utilization statistics
 * Description: Read and verify drm client memory consumption and engine utilization using fdinfo
 * Feature: SMI, core
 * Test category: SysMan
 *
 * SUBTEST: basic-mem
 * Description: Check if basic fdinfo content is present for memory
 *
 * SUBTEST: basic-utilization
 * Description: Check if basic fdinfo content is present for engine utilization
 *
 * SUBTEST: mem-total-resident
 * Description: Create and compare total and resident memory consumption by client
 *
 * SUBTEST: mem-shared
 * Description: Create and compare shared memory consumption by client
 *
 * SUBTEST: mem-active
 * Description: Create and compare active memory consumption by client
 *
 * SUBTEST: utilization-single-idle
 * Description: Check that each engine shows no load
 *
 * SUBTEST: utilization-single-full-load
 * Description: Check that each engine shows full load
 *
 * SUBTEST: utilization-single-full-load-while-active
 * Description: Check that each engine shows full load whle active
 *
 * SUBTEST: utilization-single-full-load-isolation
 * Description: Check that each engine load does not spill over to other drm clients
 *
 * SUBTEST: utilization-single-full-load-destroy-queue
 * Description: Destroy exec queue before idle and ensure load is accurate
 *
 * SUBTEST: utilization-others-idle
 * Description: Check that only the target engine shows load
 *
 * SUBTEST: utilization-others-full-load
 * Description: Check that only the target engine shows idle and all others are busy
 *
 * SUBTEST: utilization-all-full-load
 * Description: Check that all engines show busy when all are loaded
 */

IGT_TEST_DESCRIPTION("Read and verify drm client memory consumption and engine utilization using fdinfo");

#define BO_SIZE (65536)

/* flag masks */
#define TEST_WITH_LOAD		(1 << 0)
#define TEST_WHILE_ACTIVE	(1 << 1)
#define TEST_ISOLATION		(1 << 2)
#define TEST_VIRTUAL		(1 << 3)
#define TEST_PARALLEL		(1 << 4)

enum expected_load {
	EXPECTED_LOAD_IDLE,
	EXPECTED_LOAD_FULL,
};

struct pceu_cycles {
	uint64_t cycles;
	uint64_t total_cycles;
};

const unsigned long batch_duration_usec = (1 * USEC_PER_SEC);

static const char *engine_map[] = {
	"rcs",
	"bcs",
	"vcs",
	"vecs",
	"ccs",
};

static void read_engine_cycles(int xe, struct pceu_cycles *pceu)
{
	struct drm_client_fdinfo info = { };
	int class;

	igt_assert(pceu);
	igt_assert(igt_parse_drm_fdinfo(xe, &info, engine_map,
					ARRAY_SIZE(engine_map), NULL, 0));

	xe_for_each_engine_class(class) {
		pceu[class].cycles = info.cycles[class];
		pceu[class].total_cycles = info.total_cycles[class];
	}
}

/* Subtests */
static void mem_active(int fd, struct drm_xe_engine *engine)
{
	struct drm_xe_mem_region *memregion;
	uint64_t memreg = all_memory_regions(fd), region;
	struct drm_client_fdinfo info = { };
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
#define N_EXEC_QUEUES   2
	uint32_t exec_queues[N_EXEC_QUEUES];
	uint32_t bind_exec_queues[N_EXEC_QUEUES];
	uint32_t syncobjs[N_EXEC_QUEUES + 1];
	size_t bo_size;
	uint32_t bo = 0;
	struct {
		struct xe_spin spin;
		uint32_t batch[16];
		uint64_t pad;
		uint32_t data;
	} *data;
	struct xe_spin_opts spin_opts = { .preempt = true };
	int i, b, ret;

	vm = xe_vm_create(fd, 0, 0);
	bo_size = sizeof(*data) * N_EXEC_QUEUES;
	bo_size = xe_bb_size(fd, bo_size);

	xe_for_each_mem_region(fd, memreg, region) {
		uint64_t pre_size;

		memregion = xe_mem_region(fd, region);

		ret = igt_parse_drm_fdinfo(fd, &info, NULL, 0, NULL, 0);
		igt_assert_f(ret != 0, "failed with err:%d\n", errno);
		pre_size = info.region_mem[memregion->instance + 1].active;

		bo = xe_bo_create(fd, vm, bo_size, region,
				  DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
		data = xe_bo_map(fd, bo, bo_size);

		for (i = 0; i < N_EXEC_QUEUES; i++) {
			exec_queues[i] = xe_exec_queue_create(fd, vm,
							      &engine->instance, 0);
			bind_exec_queues[i] = xe_bind_exec_queue_create(fd, vm, 0);
			syncobjs[i] = syncobj_create(fd, 0);
		}
		syncobjs[N_EXEC_QUEUES] = syncobj_create(fd, 0);

		sync[0].handle = syncobj_create(fd, 0);
		xe_vm_bind_async(fd, vm, bind_exec_queues[0], bo, 0, addr, bo_size,
				 sync, 1);

		for (i = 0; i < N_EXEC_QUEUES; i++) {
			uint64_t spin_offset = (char *)&data[i].spin - (char *)data;
			uint64_t spin_addr = addr + spin_offset;
			int e = i;

			if (i == 0) {
				/* Cork 1st exec_queue with a spinner */
				spin_opts.addr = spin_addr;
				xe_spin_init(&data[i].spin, &spin_opts);
				exec.exec_queue_id = exec_queues[e];
				exec.address = spin_opts.addr;
				sync[0].flags &= ~DRM_XE_SYNC_FLAG_SIGNAL;
				sync[1].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
				sync[1].handle = syncobjs[e];
				xe_exec(fd, &exec);
				xe_spin_wait_started(&data[i].spin);

				addr += bo_size;
				sync[1].flags &= ~DRM_XE_SYNC_FLAG_SIGNAL;
				sync[1].handle = syncobjs[e];
				xe_vm_bind_async(fd, vm, bind_exec_queues[e], bo, 0, addr,
						 bo_size, sync + 1, 1);
				addr += bo_size;
			} else {
				sync[0].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
				xe_vm_bind_async(fd, vm, bind_exec_queues[e], bo, 0, addr,
						 bo_size, sync, 1);
			}
		}

		b = igt_parse_drm_fdinfo(fd, &info, NULL, 0, NULL, 0);
		igt_assert_f(b != 0, "failed with err:%d\n", errno);

		/* Client memory consumption includes public objects
		 * as well as internal objects hence if bo is active on
		 * N_EXEC_QUEUES active memory consumption should be
		 * > = bo_size
		 */
		igt_info("total:%"PRId64" active:%"PRId64" pre_size:%"PRId64" bo_size:%zd\n",
			 info.region_mem[memregion->instance + 1].total,
			 info.region_mem[memregion->instance + 1].active,
			 pre_size,
			 bo_size);
		igt_assert(info.region_mem[memregion->instance + 1].active >=
			   pre_size + bo_size);

		xe_spin_end(&data[0].spin);

		syncobj_destroy(fd, sync[0].handle);
		sync[0].handle = syncobj_create(fd, 0);
		sync[0].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
		xe_vm_unbind_all_async(fd, vm, 0, bo, sync, 1);
		igt_assert(syncobj_wait(fd, &sync[0].handle, 1, INT64_MAX, 0, NULL));

		syncobj_destroy(fd, sync[0].handle);
		for (i = 0; i < N_EXEC_QUEUES; i++) {
			syncobj_destroy(fd, syncobjs[i]);
			xe_exec_queue_destroy(fd, exec_queues[i]);
			xe_exec_queue_destroy(fd, bind_exec_queues[i]);
		}

		munmap(data, bo_size);
		gem_close(fd, bo);
	}
	xe_vm_destroy(fd, vm);
}

static void mem_shared(int xe)
{
	struct drm_xe_mem_region *memregion;
	uint64_t memreg = all_memory_regions(xe), region;
	struct drm_client_fdinfo info = { };
	struct drm_gem_flink flink;
	struct drm_gem_open open_struct;
	uint32_t bo;
	int ret;

	xe_for_each_mem_region(xe, memreg, region) {
		uint64_t pre_size;

		memregion = xe_mem_region(xe, region);

		ret = igt_parse_drm_fdinfo(xe, &info, NULL, 0, NULL, 0);
		igt_assert_f(ret != 0, "failed with err:%d\n", errno);
		pre_size = info.region_mem[memregion->instance + 1].shared;

		bo = xe_bo_create(xe, 0, BO_SIZE, region, 0);

		flink.handle = bo;
		ret = igt_ioctl(xe, DRM_IOCTL_GEM_FLINK, &flink);
		igt_assert_eq(ret, 0);

		open_struct.name = flink.name;
		ret = igt_ioctl(xe, DRM_IOCTL_GEM_OPEN, &open_struct);
		igt_assert_eq(ret, 0);
		igt_assert(open_struct.handle != 0);

		ret = igt_parse_drm_fdinfo(xe, &info, NULL, 0, NULL, 0);
		igt_assert_f(ret != 0, "failed with err:%d\n", errno);

		igt_info("total:%"PRId64" pre_size:%"PRId64" shared:%"PRId64"\n",
			 info.region_mem[memregion->instance + 1].total,
			 pre_size,
			 info.region_mem[memregion->instance + 1].shared);
		igt_assert(info.region_mem[memregion->instance + 1].shared >=
			   pre_size + BO_SIZE);

		gem_close(xe, open_struct.handle);
		gem_close(xe, bo);
	}
}

static void mem_total_resident(int xe)
{
	struct drm_xe_mem_region *memregion;
	uint64_t memreg = all_memory_regions(xe), region;
	struct drm_client_fdinfo info = { };
	uint32_t vm;
	uint32_t handle;
	uint64_t addr = 0x1a0000;
	int ret;

	vm = xe_vm_create(xe, DRM_XE_VM_CREATE_FLAG_SCRATCH_PAGE, 0);

	xe_for_each_mem_region(xe, memreg, region) {
		uint64_t pre_size;

		memregion = xe_mem_region(xe, region);

		ret = igt_parse_drm_fdinfo(xe, &info, NULL, 0, NULL, 0);
		igt_assert_f(ret != 0, "failed with err:%d\n", errno);
		pre_size = info.region_mem[memregion->instance + 1].shared;

		handle = xe_bo_create(xe, vm, BO_SIZE, region, 0);
		xe_vm_bind_sync(xe, vm, handle, 0, addr, BO_SIZE);

		ret = igt_parse_drm_fdinfo(xe, &info, NULL, 0, NULL, 0);
		igt_assert_f(ret != 0, "failed with err:%d\n", errno);
		/* currently xe KMD maps memory class system region to
		 * XE_PL_TT thus we need memregion->instance + 1
		 */
		igt_info("total:%"PRId64" resident:%"PRId64" pre_size:%"PRId64" bo_size:%d\n",
			 info.region_mem[memregion->instance + 1].total,
			 info.region_mem[memregion->instance + 1].resident,
			 pre_size, BO_SIZE);
		/* Client memory consumption includes public objects
		 * as well as internal objects hence it should be
		 * >= pre_size + BO_SIZE
		 */
		igt_assert(info.region_mem[memregion->instance + 1].total >=
			   pre_size + BO_SIZE);
		igt_assert(info.region_mem[memregion->instance + 1].resident >=
			   pre_size + BO_SIZE);
		xe_vm_unbind_sync(xe, vm, 0, addr, BO_SIZE);
		gem_close(xe, handle);
	}

	xe_vm_destroy(xe, vm);
}

static void basic_memory(int xe)
{
	struct drm_xe_mem_region *memregion;
	uint64_t memreg = all_memory_regions(xe), region;
	struct drm_client_fdinfo info = { };
	unsigned int ret;

	ret = igt_parse_drm_fdinfo(xe, &info, NULL, 0, NULL, 0);
	igt_assert_f(ret != 0, "failed with err:%d\n", errno);

	igt_assert(!strcmp(info.driver, "xe"));

	xe_for_each_mem_region(xe, memreg, region) {
		memregion = xe_mem_region(xe, region);
		igt_assert(info.region_mem[memregion->instance + 1].total >=
			   0);
		igt_assert(info.region_mem[memregion->instance + 1].shared >=
			   0);
		igt_assert(info.region_mem[memregion->instance + 1].resident >=
			   0);
		igt_assert(info.region_mem[memregion->instance + 1].active >=
			   0);
		if (memregion->instance == 0)
			igt_assert(info.region_mem[memregion->instance].purgeable >=
				   0);
	}
}

static void require_engine_utilization_data(int xe)
{
	struct drm_client_fdinfo info = {};

	igt_require(igt_parse_drm_fdinfo(xe, &info, engine_map,
					 ARRAY_SIZE(engine_map), NULL, 0));
	igt_require(info.num_engines);
	igt_require(info.utilization_mask & DRM_FDINFO_UTILIZATION_CYCLES);
}

static void basic_engine_utilization(int xe)
{
	struct drm_client_fdinfo info = { };
	unsigned int ret;

	ret = igt_parse_drm_fdinfo(xe, &info, engine_map,
				   ARRAY_SIZE(engine_map), NULL, 0);
	igt_assert_f(ret != 0, "failed with err:%d\n", errno);
	igt_assert(!strcmp(info.driver, "xe"));
	igt_assert(info.num_engines);
}

static void
check_results(struct pceu_cycles *s1, struct pceu_cycles *s2,
	      int class, int width, uint32_t spin_stamp,
	      enum expected_load expected_load)
{
	double percent;
	uint64_t den, num;

	igt_debug("%s: sample 1: cycles %"PRIu64", total_cycles %"PRIu64"\n",
		  engine_map[class], s1[class].cycles, s1[class].total_cycles);
	igt_debug("%s: sample 2: cycles %"PRIu64", total_cycles %"PRIu64"\n",
		  engine_map[class], s2[class].cycles, s2[class].total_cycles);
	igt_debug("spin_stamp: %u width: %d\n", spin_stamp, width);

	num = s2[class].cycles - s1[class].cycles;
	den = s2[class].total_cycles - s1[class].total_cycles;

	percent = (num * 100.0) / (den + 1);

	/* For parallel submission scale the busyness with width */
	percent /= width;

	igt_debug("%s: percent: %.2f%%\n", engine_map[class], percent);

	switch (expected_load) {
	case EXPECTED_LOAD_IDLE:
		igt_assert_eq(num, 0);
		break;
	case EXPECTED_LOAD_FULL:
		/* For parallel submission scale the busyness with width */
		spin_stamp *= width;

		/*
		 * Percentage error between value saved by gpu in xe_spin and what
		 * is reported via fdinfo
		 */
		percent = fabs((int64_t)(num - spin_stamp) * 100.0) / (spin_stamp + 1);
		igt_debug("%s: error: %.2f%%\n", engine_map[class], percent);
		igt_assert_lt_double(percent, 5.0);
		break;
	}
}

static void end_cork(int fd, struct xe_cork *cork)
{
	if (cork && !cork->ended)
		xe_cork_sync_end(fd, cork);
}

static void
utilization_single(int fd, struct drm_xe_engine_class_instance *hwe, unsigned int flags)
{
	struct pceu_cycles pceu1[2][DRM_XE_ENGINE_CLASS_COMPUTE + 1];
	struct pceu_cycles pceu2[2][DRM_XE_ENGINE_CLASS_COMPUTE + 1];
	struct xe_cork *cork = NULL;
	enum expected_load expected_load;
	uint32_t vm;
	int new_fd;

	igt_debug("Target class: %s\n", engine_map[hwe->engine_class]);

	if (flags & TEST_ISOLATION)
		new_fd = drm_reopen_driver(fd);

	vm = xe_vm_create(fd, 0, 0);
	if (flags & TEST_WITH_LOAD) {
		cork = xe_cork_create_opts(fd, hwe, vm, 1, 1);
		xe_cork_sync_start(fd, cork);
	}

	read_engine_cycles(fd, pceu1[0]);
	if (flags & TEST_ISOLATION)
		read_engine_cycles(new_fd, pceu1[1]);

	usleep(batch_duration_usec);
	if (!(flags & TEST_WHILE_ACTIVE))
		end_cork(fd, cork);

	read_engine_cycles(fd, pceu2[0]);
	if (flags & TEST_ISOLATION)
		read_engine_cycles(new_fd, pceu2[1]);

	end_cork(fd, cork);

	expected_load = flags & TEST_WITH_LOAD ?
	       EXPECTED_LOAD_FULL : EXPECTED_LOAD_IDLE;

	check_results(pceu1[0], pceu2[0], hwe->engine_class, 1,
		      cork ? cork->spin->timestamp : 0, expected_load);

	if (flags & TEST_ISOLATION) {
		/*
		 * Load from one client shouldn't spill on another,
		 * so check for idle
		 */
		check_results(pceu1[1], pceu2[1], hwe->engine_class, 1, 0,
			      EXPECTED_LOAD_IDLE);
		close(new_fd);
	}

	if (cork)
		xe_cork_destroy(fd, cork);

	xe_vm_destroy(fd, vm);
}

static void
utilization_single_destroy_queue(int fd, struct drm_xe_engine_class_instance *hwe)
{
	struct pceu_cycles pceu1[DRM_XE_ENGINE_CLASS_COMPUTE + 1];
	struct pceu_cycles pceu2[DRM_XE_ENGINE_CLASS_COMPUTE + 1];
	struct xe_cork *cork;
	uint32_t timestamp;
	uint32_t vm;

	igt_debug("Target class: %s\n", engine_map[hwe->engine_class]);

	vm = xe_vm_create(fd, 0, 0);
	cork = xe_cork_create_opts(fd, hwe, vm, 1, 1);
	xe_cork_sync_start(fd, cork);

	read_engine_cycles(fd, pceu1);
	usleep(batch_duration_usec);

	/* destroy queue before sampling again */
	end_cork(fd, cork);
	timestamp = cork->spin->timestamp;
	xe_cork_destroy(fd, cork);

	read_engine_cycles(fd, pceu2);

	xe_vm_destroy(fd, vm);

	check_results(pceu1, pceu2, hwe->engine_class, 1, timestamp,
		      EXPECTED_LOAD_FULL);
}

static void
utilization_others_idle(int fd, struct drm_xe_engine_class_instance *hwe)
{
	struct pceu_cycles pceu1[DRM_XE_ENGINE_CLASS_COMPUTE + 1];
	struct pceu_cycles pceu2[DRM_XE_ENGINE_CLASS_COMPUTE + 1];
	struct xe_cork *cork;
	uint32_t vm;
	int class;

	igt_debug("Target class: %s\n", engine_map[hwe->engine_class]);

	vm = xe_vm_create(fd, 0, 0);
	cork = xe_cork_create_opts(fd, hwe, vm, 1, 1);
	xe_cork_sync_start(fd, cork);

	read_engine_cycles(fd, pceu1);
	usleep(batch_duration_usec);
	end_cork(fd, cork);
	read_engine_cycles(fd, pceu2);

	xe_for_each_engine_class(class) {
		enum expected_load expected_load = hwe->engine_class != class ?
			EXPECTED_LOAD_IDLE : EXPECTED_LOAD_FULL;

		check_results(pceu1, pceu2, class, 1, cork->spin->timestamp,
			      expected_load);
	}

	xe_cork_destroy(fd, cork);
	xe_vm_destroy(fd, vm);
}

static void
utilization_others_full_load(int fd, struct drm_xe_engine_class_instance *hwe)
{
	struct pceu_cycles pceu1[DRM_XE_ENGINE_CLASS_COMPUTE + 1];
	struct pceu_cycles pceu2[DRM_XE_ENGINE_CLASS_COMPUTE + 1];
	struct xe_cork *cork[DRM_XE_ENGINE_CLASS_COMPUTE + 1] = {};
	struct drm_xe_engine_class_instance *_hwe;
	uint32_t vm;
	int class;

	igt_debug("Target class: %s\n", engine_map[hwe->engine_class]);

	vm = xe_vm_create(fd, 0, 0);

	/* spin on one hwe per class except the target class hwes */
	xe_for_each_engine(fd, _hwe) {
		int _class = _hwe->engine_class;

		if (_class == hwe->engine_class || cork[_class])
			continue;

		cork[_class] = xe_cork_create_opts(fd, _hwe, vm, 1, 1);
		xe_cork_sync_start(fd, cork[_class]);
	}

	read_engine_cycles(fd, pceu1);
	usleep(batch_duration_usec);
	xe_for_each_engine_class(class)
		end_cork(fd, cork[class]);

	read_engine_cycles(fd, pceu2);

	xe_for_each_engine_class(class) {
		enum expected_load expected_load = hwe->engine_class == class ?
			EXPECTED_LOAD_IDLE : EXPECTED_LOAD_FULL;

		check_results(pceu1, pceu2, class, 1,
			      cork[class] ? cork[class]->spin->timestamp : 0,
			      expected_load);

		if (cork[class])
			xe_cork_destroy(fd, cork[class]);
	}

	xe_vm_destroy(fd, vm);
}

static void
utilization_all_full_load(int fd)
{
	struct pceu_cycles pceu1[DRM_XE_ENGINE_CLASS_COMPUTE + 1];
	struct pceu_cycles pceu2[DRM_XE_ENGINE_CLASS_COMPUTE + 1];
	struct xe_cork *cork[DRM_XE_ENGINE_CLASS_COMPUTE + 1] = {};
	struct drm_xe_engine_class_instance *hwe;
	uint32_t vm;
	int class;

	vm = xe_vm_create(fd, 0, 0);

	/* spin on one hwe per class */
	xe_for_each_engine(fd, hwe) {
		class = hwe->engine_class;
		if (cork[class])
			continue;
		cork[class] = xe_cork_create_opts(fd, hwe, vm, 1, 1);
		xe_cork_sync_start(fd, cork[class]);
	}

	read_engine_cycles(fd, pceu1);
	usleep(batch_duration_usec);
	xe_for_each_engine_class(class)
		end_cork(fd, cork[class]);

	read_engine_cycles(fd, pceu2);

	xe_for_each_engine_class(class) {
		if (!cork[class])
			continue;

		check_results(pceu1, pceu2, class, 1,
			      cork[class]->spin->timestamp,
			      EXPECTED_LOAD_FULL);
		xe_cork_destroy(fd, cork[class]);
	}

	xe_vm_destroy(fd, vm);
}

/**
 * SUBTEST: %s-utilization-single-idle
 * Description: Check that each engine shows no load
 *
 * SUBTEST: %s-utilization-single-full-load
 * Description: Check that each engine shows full load
 *
 * SUBTEST: %s-utilization-single-full-load-isolation
 * Description: Check that each engine load does not spill over to other drm clients
 *
 * arg[1]:
 *
 * @virtual:			virtual
 * @parallel:			parallel
 */
static void
utilization_multi(int fd, int gt, int class, unsigned int flags)
{
	struct pceu_cycles pceu[2][DRM_XE_ENGINE_CLASS_COMPUTE + 1];
	struct pceu_cycles pceu_spill[2][DRM_XE_ENGINE_CLASS_COMPUTE + 1];
	struct drm_xe_engine_class_instance eci[XE_MAX_ENGINE_INSTANCE];
	struct xe_cork *cork = NULL;
	enum expected_load expected_load;
	int fd_spill, num_placements;
	uint32_t vm;
	bool virtual = flags & TEST_VIRTUAL;
	bool parallel = flags & TEST_PARALLEL;
	uint16_t width;

	igt_assert(virtual ^ parallel);

	num_placements = xe_gt_fill_engines_by_class(fd, gt, class, eci);
	if (num_placements < 2)
		return;

	igt_debug("Target class: %s\n", engine_map[class]);

	if (parallel) {
		width = num_placements;
		num_placements = 1;
	} else {
		width = 1;
	}

	if (flags & TEST_ISOLATION)
		fd_spill = drm_reopen_driver(fd);

	vm = xe_vm_create(fd, 0, 0);
	if (flags & TEST_WITH_LOAD) {
		cork = xe_cork_create_opts(fd, eci, vm, width, num_placements);
		xe_cork_sync_start(fd, cork);
	}

	read_engine_cycles(fd, pceu[0]);
	if (flags & TEST_ISOLATION)
		read_engine_cycles(fd_spill, pceu_spill[0]);

	usleep(batch_duration_usec);
	if (!(flags & TEST_WHILE_ACTIVE))
		end_cork(fd, cork);

	read_engine_cycles(fd, pceu[1]);
	if (flags & TEST_ISOLATION)
		read_engine_cycles(fd_spill, pceu_spill[1]);

	expected_load = flags & TEST_WITH_LOAD ?
	       EXPECTED_LOAD_FULL : EXPECTED_LOAD_IDLE;

	check_results(pceu[0], pceu[1], class, width,
		      cork ? cork->spin->timestamp : 0, expected_load);

	if (flags & TEST_ISOLATION) {
		/*
		 * Load from one client shouldn't spill on another,
		 * so check for idle
		 */
		check_results(pceu_spill[0], pceu_spill[1], class, width, 0,
			      EXPECTED_LOAD_IDLE);
		close(fd_spill);
	}

	if (cork)
		xe_cork_destroy(fd, cork);

	xe_vm_destroy(fd, vm);
}

igt_main
{
	const struct section {
		const char *name;
		unsigned int flags;
	} sections[] = {
		{ .name = "virtual", .flags = TEST_VIRTUAL },
		{ .name = "parallel", .flags = TEST_PARALLEL },
		{ }
	};
	struct drm_xe_engine_class_instance *hwe;
	int xe, gt, class;

	igt_fixture() {
		struct drm_client_fdinfo info = { };

		xe = drm_open_driver(DRIVER_XE);
		igt_require_xe(xe);
		igt_require(igt_parse_drm_fdinfo(xe, &info, NULL, 0, NULL, 0));
	}

	igt_describe("Check if basic fdinfo content is present for memory");
	igt_subtest("basic-mem")
		basic_memory(xe);

	igt_describe("Check if basic fdinfo content is present for engine utilization");
	igt_subtest("basic-utilization") {
		require_engine_utilization_data(xe);
		basic_engine_utilization(xe);
	}

	igt_describe("Create and compare total and resident memory consumption by client");
	igt_subtest("mem-total-resident")
		mem_total_resident(xe);

	igt_describe("Create and compare shared memory consumption by client");
	igt_subtest("mem-shared")
		mem_shared(xe);

	igt_describe("Create and compare active memory consumption by client");
	igt_subtest("mem-active")
		mem_active(xe, xe_engine(xe, 0));

	igt_subtest("utilization-single-idle") {
		require_engine_utilization_data(xe);
		xe_for_each_engine(xe, hwe)
			utilization_single(xe, hwe, 0);
	}

	igt_subtest("utilization-single-full-load-while-active") {
		require_engine_utilization_data(xe);
		xe_for_each_engine(xe, hwe)
			utilization_single(xe, hwe,
					   TEST_WITH_LOAD | TEST_WHILE_ACTIVE);
	}

	igt_subtest("utilization-single-full-load") {
		require_engine_utilization_data(xe);
		xe_for_each_engine(xe, hwe)
			utilization_single(xe, hwe, TEST_WITH_LOAD);
	}

	igt_subtest("utilization-single-full-load-isolation") {
		require_engine_utilization_data(xe);
		xe_for_each_engine(xe, hwe)
			utilization_single(xe, hwe, TEST_WITH_LOAD | TEST_ISOLATION);
	}

	igt_subtest("utilization-single-full-load-destroy-queue") {
		require_engine_utilization_data(xe);
		xe_for_each_engine(xe, hwe)
			utilization_single_destroy_queue(xe, hwe);
	}

	igt_subtest("utilization-others-idle") {
		require_engine_utilization_data(xe);
		xe_for_each_engine(xe, hwe)
			utilization_others_idle(xe, hwe);
	}

	igt_subtest("utilization-others-full-load") {
		require_engine_utilization_data(xe);
		xe_for_each_engine(xe, hwe)
			utilization_others_full_load(xe, hwe);
	}

	igt_subtest("utilization-all-full-load") {
		require_engine_utilization_data(xe);
		utilization_all_full_load(xe);
	}


	for (const struct section *s = sections; s->name; s++) {
		igt_subtest_f("%s-utilization-single-idle", s->name) {
			require_engine_utilization_data(xe);
			xe_for_each_gt(xe, gt)
				xe_for_each_engine_class(class)
					utilization_multi(xe, gt, class, s->flags);
		}

		igt_subtest_f("%s-utilization-single-full-load", s->name) {
			require_engine_utilization_data(xe);
			xe_for_each_gt(xe, gt)
				xe_for_each_engine_class(class)
					utilization_multi(xe, gt, class,
							  s->flags |
							  TEST_WITH_LOAD);
		}

		igt_subtest_f("%s-utilization-single-full-load-isolation",
			      s->name) {
			require_engine_utilization_data(xe);
			xe_for_each_gt(xe, gt)
				xe_for_each_engine_class(class)
					utilization_multi(xe, gt, class,
							  s->flags |
							  TEST_WITH_LOAD |
							  TEST_ISOLATION);
		}
	}

	igt_fixture() {
		drm_close_driver(xe);
	}
}
