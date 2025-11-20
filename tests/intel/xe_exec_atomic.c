/* SPDX-License-Identifier: MIT */
/*
* Copyright © 2024 Intel Corporation
*
* Authors:
*    Nirmoy Das <nirmoy.das@intel.com>
*/

#include "igt.h"
#include "lib/igt_syncobj.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"
#include "xe_drm.h"

/**
 * TEST: Tests to verify atomic functionality.
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: HW
 * Functionality: intel-bb
 * Test category: functionality test
 */

struct data {
	uint32_t batch[16];
	uint64_t pad;
	uint32_t data;
	uint64_t addr;
};

static void atomic_batch(struct data *data, uint64_t addr, int ops)
{
	int b;
	uint64_t batch_offset = (char *)&(data->batch) - (char *)data;
	uint64_t batch_addr = addr + batch_offset;
	uint64_t sdi_offset = (char *)&(data->data) - (char *)data;
	uint64_t sdi_addr = addr + sdi_offset;

	b = 0;
	data->batch[b++] = MI_ATOMIC | ops;
	data->batch[b++] = sdi_addr;
	data->batch[b++] = sdi_addr >> 32;
	data->batch[b++] = MI_BATCH_BUFFER_END;
	igt_assert(b <= ARRAY_SIZE(data->batch));

	data->addr = batch_addr;
}

/**
 * SUBTEST: basic-inc-all
 * Description: Test to verify atomic increment on all available engines
 *		and memory types.
 *
 * SUBTEST: basic-dec-all
 * Description: Test to verify atomic decrement on all available engines
 *		and memory types.
 */
static void basic_inst(int fd, int inst_type, struct drm_xe_engine_class_instance *eci,
		       uint32_t placement)
{
	struct drm_xe_sync sync = {
		.type = DRM_XE_SYNC_TYPE_SYNCOBJ,
		.flags = DRM_XE_SYNC_FLAG_SIGNAL,
	};
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 1,
		.syncs = to_user_pointer(&sync),
	};
	struct data *data;
	uint32_t vm;
	uint32_t exec_queue;
	uint32_t bind_engine;
	uint32_t syncobj;
	size_t bo_size;
	int value = 0x123456, match;
	uint64_t addr = 0x100000;
	uint32_t bo = 0;

	syncobj = syncobj_create(fd, 0);
	sync.handle = syncobj;

	vm = xe_vm_create(fd, 0, 0);
	bo_size = sizeof(*data);
	bo_size = xe_bb_size(fd, bo_size);

	bo = xe_bo_create(fd, vm, bo_size, placement,
			  DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);

	exec_queue = xe_exec_queue_create(fd, vm, eci, 0);
	bind_engine = xe_bind_exec_queue_create(fd, vm, 0);
	xe_vm_bind_async(fd, vm, bind_engine, bo, 0, addr, bo_size, &sync, 1);
	data = xe_bo_mmap_ext(fd, bo, bo_size, PROT_READ|PROT_WRITE);
	data->data = value;

	atomic_batch(data, addr, inst_type);

	exec.exec_queue_id = exec_queue;
	exec.address = data->addr;
	sync.flags &= DRM_XE_SYNC_FLAG_SIGNAL;
	xe_exec(fd, &exec);

	igt_assert(syncobj_wait(fd, &syncobj, 1, INT64_MAX, 0, NULL));
	munmap(data, bo_size);
	data = xe_bo_mmap_ext(fd, bo, bo_size, PROT_READ|PROT_WRITE);
	match = (inst_type == MI_ATOMIC_INC) ? ++value : --value;
	igt_assert_eq(data->data, match);

	syncobj_destroy(fd, syncobj);
	munmap(data, bo_size);
	gem_close(fd, bo);

	xe_exec_queue_destroy(fd, exec_queue);
	xe_vm_destroy(fd, vm);
}

static bool has_atomics(int fd, uint32_t region)
{
	/* System memory atomics on dGPU is not functional as of now */
	if (region == system_memory(fd) && xe_has_vram(fd))
		return false;

	return true;
}

igt_main
{
	struct drm_xe_engine_class_instance *hwe;
	int fd;

	igt_fixture()
		fd = drm_open_driver(DRIVER_XE);

	igt_subtest_with_dynamic("basic-dec-all") {
		xe_for_each_engine(fd, hwe) {
			uint64_t memreg = all_memory_regions(fd), region;

			xe_for_each_mem_region(fd, memreg, region) {
				if (!has_atomics(fd, region))
					continue;

				igt_dynamic_f("Engine-%s-Instance-%d-Tile-%d-%s-memory",
					      xe_engine_class_string(hwe->engine_class),
					      hwe->engine_instance,
					      hwe->gt_id, xe_region_name(region))

					basic_inst(fd, MI_ATOMIC_DEC, hwe, region);
			}
		}
	}

	igt_subtest_with_dynamic("basic-inc-all") {
		xe_for_each_engine(fd, hwe) {
			uint64_t memreg = all_memory_regions(fd), region;

			xe_for_each_mem_region(fd, memreg, region) {
				if (!has_atomics(fd, region))
					continue;

				igt_dynamic_f("Engine-%s-Instance-%d-Tile-%d-%s-memory",
					      xe_engine_class_string(hwe->engine_class),
					      hwe->engine_instance,
					      hwe->gt_id, xe_region_name(region))

					basic_inst(fd, MI_ATOMIC_INC, hwe, region);
			}
		}
	}

	igt_fixture()
		drm_close_driver(fd);
}
