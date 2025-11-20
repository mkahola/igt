/* SPDX-License-Identifier: MIT */
/*
* Copyright © 2024 Intel Corporation
*
* Authors:
*    Sai Gowtham Ch <sai.gowtham.ch@intel.com>
*/
#include "igt.h"
#include "lib/igt_syncobj.h"
#include "xe/xe_gt.c"
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"
#include "xe_drm.h"
#include "igt_debugfs.h"

/**
 * TEST: Check Translation Lookaside Buffer Invalidation.
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: CMD submission
 * Functionality: TLB invalidate
 * Test category: functionality test
 */
struct data {
	uint32_t batch[16];
	uint32_t data;
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

/**
 * SUBTEST: basic-tlb
 * Description: Check Translation Lookaside Buffer Invalidation.
 */
static void tlb_invalidation(int fd, struct drm_xe_engine_class_instance *eci)
{
	struct drm_xe_sync sync[2] = {
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, },
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, }
	};
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 2,
		.syncs = to_user_pointer(&sync),
	};
	struct data *data1;
	struct data *data2;
	uint32_t vm;
	uint32_t exec_queue;
	uint32_t bind_engine;
	uint32_t syncobj;
	size_t bo_size;
	int value1 = 0x123456;
	int value2 = 0x123465;
	uint64_t addr = 0x100000;
	uint32_t bo1, bo2;
	int tlb_pre, tlb_pos;
	const char *stat = "tlb_inval_count";

	syncobj = syncobj_create(fd, 0);
	sync[0].handle = syncobj_create(fd, 0);
	sync[1].handle = syncobj;

	vm = xe_vm_create(fd, 0, 0);
	bo_size = sizeof(*data1);
	bo_size = xe_bb_size(fd, bo_size);
	bo1 = xe_bo_create(fd, vm, bo_size,
				   vram_if_possible(fd, eci->gt_id),
				   DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	bo2 = xe_bo_create(fd, vm, bo_size,
				   vram_if_possible(fd, eci->gt_id),
				   DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);

	tlb_pre = xe_gt_stats_get_count(fd, eci->gt_id, stat);
	exec_queue = xe_exec_queue_create(fd, vm, eci, 0);
	bind_engine = xe_bind_exec_queue_create(fd, vm, 0);
	xe_vm_bind_async(fd, vm, bind_engine, bo1, 0, addr, bo_size, sync, 1);
	data1 = xe_bo_map(fd, bo1, bo_size);

	store_dword_batch(data1, addr, value1);
	exec.exec_queue_id = exec_queue;
	exec.address = data1->addr;
	sync[0].flags &= ~DRM_XE_SYNC_FLAG_SIGNAL;
	sync[1].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
	xe_exec(fd, &exec);
	igt_assert(syncobj_wait(fd, &syncobj, 1, INT64_MAX, 0, NULL));
	xe_vm_bind_async(fd, vm, bind_engine, bo2, 0, addr, bo_size, sync, 1);
	data2 = xe_bo_map(fd, bo2, bo_size);

	store_dword_batch(data2, addr, value2);
	exec.exec_queue_id = exec_queue;
	exec.address = data2->addr;
	sync[0].flags &= ~DRM_XE_SYNC_FLAG_SIGNAL;
	sync[1].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
	xe_exec(fd, &exec);
	igt_assert(syncobj_wait(fd, &syncobj, 1, INT64_MAX, 0, NULL));

	tlb_pos = xe_gt_stats_get_count(fd, eci->gt_id, stat);
	igt_assert_eq(data1->data, value1);
	igt_assert_eq(data2->data, value2);
	igt_assert(tlb_pos > tlb_pre);

	syncobj_destroy(fd, sync[0].handle);
	syncobj_destroy(fd, syncobj);
	munmap(data1, bo_size);
	munmap(data2, bo_size);
	gem_close(fd, bo1);
	gem_close(fd, bo2);
	xe_exec_queue_destroy(fd, exec_queue);
	xe_vm_destroy(fd, vm);
}

int igt_main()
{
	int fd;
	struct drm_xe_engine *engine;

	igt_fixture() {
		fd = drm_open_driver(DRIVER_XE);
	}

	igt_subtest("basic-tlb") {
		engine = xe_engine(fd, 0);
		tlb_invalidation(fd, &engine->instance);
	}

	igt_fixture() {
		drm_close_driver(fd);
	}
}
