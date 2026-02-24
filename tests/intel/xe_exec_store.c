/* SPDX-License-Identifier: MIT */
/*
* Copyright © 2023 Intel Corporation
*
* Authors:
*    Sai Gowtham Ch <sai.gowtham.ch@intel.com>
*/

#include "igt.h"
#include "lib/igt_syncobj.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"
#include "xe_drm.h"

#include "intel_pat.h"
#include "intel_mocs.h"
#include "gpgpu_shader.h"
#include "xe/xe_util.h"

/**
 * TEST: Tests to verify store dword functionality.
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: CMD submission
 * Functionality: intel-bb
 * Test category: functionality test
 */

#define STORE 0
#define COND_BATCH 1
#define MAX_DATA_WRITE ((size_t)(262143)) //Maximum data MEM_COPY operate for linear mode

struct data {
	uint32_t batch[16];
	uint64_t pad;
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

static void cond_batch(struct data *data, uint64_t addr, int value,
		       uint16_t dev_id)
{
	int b;
	uint64_t batch_offset = (char *)&(data->batch) - (char *)data;
	uint64_t batch_addr = addr + batch_offset;
	uint64_t sdi_offset = (char *)&(data->data) - (char *)data;
	uint64_t sdi_addr = addr + sdi_offset;

	b = 0;
	data->batch[b++] = MI_ATOMIC | MI_ATOMIC_INC;
	data->batch[b++] = sdi_addr;
	data->batch[b++] = sdi_addr >> 32;

	if (intel_graphics_ver(dev_id) >= IP_VER(20, 0))
		data->batch[b++] = MI_MEM_FENCE | MI_WRITE_FENCE;

	data->batch[b++] = MI_CONDITIONAL_BATCH_BUFFER_END | MI_DO_COMPARE | 5 << 12 | 2;
	data->batch[b++] = value;
	data->batch[b++] = sdi_addr;
	data->batch[b++] = sdi_addr >> 32;
	data->batch[b++] = MI_BATCH_BUFFER_START | 1;
	data->batch[b++] = lower_32_bits(batch_addr);
	data->batch[b++] = upper_32_bits(batch_addr);
	igt_assert(b <= ARRAY_SIZE(data->batch));

	data->addr = batch_addr;
}

static void persistance_batch(struct data *data, uint64_t addr)
{
	int b;
	uint64_t batch_offset = (char *)&(data->batch) - (char *)data;
	uint64_t batch_addr = addr + batch_offset;
	uint64_t prt_offset = (char *)&(data->data) - (char *)data;
	uint64_t prt_addr = addr + prt_offset;

	b = 0;
	data->batch[b++] = MI_BATCH_BUFFER_START;
	data->batch[b++] = MI_PRT_BATCH_BUFFER_START;
	data->batch[b++] = prt_addr;
	data->batch[b++] = prt_addr >> 32;
	data->batch[b++] = MI_BATCH_BUFFER_END;

	data->addr = batch_addr;

}
/**
 * SUBTEST: basic-store
 * Description: Basic test to verify store dword.
 *
 * SUBTEST: basic-cond-batch
 * Description: Basic test to verify cond batch end instruction.
 *
 * SUBTEST: basic-all
 * Description: Test to verify store dword on all available engines.
 */
static void basic_inst(int fd, int inst_type, struct drm_xe_engine_class_instance *eci,
		       uint16_t dev_id)
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
	struct data *data;
	uint32_t vm;
	uint32_t exec_queue;
	uint32_t bind_engine;
	uint32_t syncobj;
	size_t bo_size;
	int value = 0x123456;
	uint64_t addr = 0x100000;
	uint32_t bo = 0;

	syncobj = syncobj_create(fd, 0);
	sync[0].handle = syncobj_create(fd, 0);
	sync[1].handle = syncobj;

	vm = xe_vm_create(fd, 0, 0);
	bo_size = sizeof(*data);
	bo_size = xe_bb_size(fd, bo_size);

	bo = xe_bo_create(fd, vm, bo_size,
			  vram_if_possible(fd, eci->gt_id),
			  DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);

	exec_queue = xe_exec_queue_create(fd, vm, eci, 0);
	bind_engine = xe_bind_exec_queue_create(fd, vm, 0);
	xe_vm_bind_async(fd, vm, bind_engine, bo, 0, addr, bo_size, sync, 1);
	data = xe_bo_map(fd, bo, bo_size);

	if (inst_type == STORE)
		store_dword_batch(data, addr, value);
	else if (inst_type == COND_BATCH) {
		/* A random value where it stops at the below value. */
		value = 20 + random() % 10;
		cond_batch(data, addr, value, dev_id);
	}
	else
		igt_assert_f(inst_type < 2, "Entered wrong inst_type.\n");

	exec.exec_queue_id = exec_queue;
	exec.address = data->addr;
	sync[0].flags &= ~DRM_XE_SYNC_FLAG_SIGNAL;
	sync[1].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
	xe_exec(fd, &exec);

	igt_assert(syncobj_wait(fd, &syncobj, 1, INT64_MAX, 0, NULL));
	igt_assert_eq(data->data, value);

	syncobj_destroy(fd, sync[0].handle);
	syncobj_destroy(fd, syncobj);
	munmap(data, bo_size);
	gem_close(fd, bo);

	xe_exec_queue_destroy(fd, exec_queue);
	xe_vm_destroy(fd, vm);
}

#define PAGES 1
#define NCACHELINES (4096/64)
/**
 * SUBTEST: %s
 * Description: Verify that each engine can store a dword to different %arg[1] of a object.
 * Test category: functionality test
 *
 * arg[1]:
 *
 * @cachelines: cachelines
 * @page-sized: page-sized
 */
static void store_cachelines(int fd, struct drm_xe_engine_class_instance *eci,
			     unsigned int flags)
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

	int count = flags & PAGES ? NCACHELINES + 1 : 2;
	int i, object_index, b = 0;
	uint64_t dst_offset[count];
	uint32_t exec_queues, vm, syncobjs;
	uint32_t bo[count], *bo_map[count];
	uint32_t value[NCACHELINES], *ptr[NCACHELINES], delta;
	uint64_t offset[NCACHELINES];
	uint64_t ahnd;
	uint32_t *batch_map;
	size_t bo_size = 4096;

	bo_size = xe_bb_size(fd, bo_size);
	vm = xe_vm_create(fd, 0, 0);
	ahnd = intel_allocator_open(fd, 0, INTEL_ALLOCATOR_SIMPLE);
	exec_queues = xe_exec_queue_create(fd, vm, eci, 0);
	syncobjs = syncobj_create(fd, 0);
	sync[0].handle = syncobj_create(fd, 0);

	for (i = 0; i < count; i++) {
		bo[i] = xe_bo_create(fd, vm, bo_size,
				     vram_if_possible(fd, eci->gt_id),
				     DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
		bo_map[i] = xe_bo_map(fd, bo[i], bo_size);
		dst_offset[i] = intel_allocator_alloc_with_strategy(ahnd, bo[i],
								    bo_size, 0,
								    ALLOC_STRATEGY_LOW_TO_HIGH);
		xe_vm_bind_async(fd, vm, 0, bo[i], 0, dst_offset[i], bo_size, sync, 1);
	}

	batch_map = xe_bo_map(fd, bo[i-1], bo_size);
	exec.address = dst_offset[i-1];

	for (unsigned int n = 0; n < NCACHELINES; n++) {
		delta = 4 * (n * 16 + n % 16);
		value[n] = n | ~n << 16;
		offset[n] = dst_offset[n % (count - 1)] + delta;

		batch_map[b++] = MI_STORE_DWORD_IMM_GEN4;
		batch_map[b++] = offset[n];
		batch_map[b++] = offset[n] >> 32;
		batch_map[b++] = value[n];
	}
	batch_map[b++] = MI_BATCH_BUFFER_END;
	sync[0].flags &= ~DRM_XE_SYNC_FLAG_SIGNAL;
	sync[1].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
	sync[1].handle = syncobjs;
	exec.exec_queue_id = exec_queues;
	xe_exec(fd, &exec);
	igt_assert(syncobj_wait(fd, &syncobjs, 1, INT64_MAX, 0, NULL));

	for (unsigned int n = 0; n < NCACHELINES; n++) {
		delta = 4 * (n * 16 + n % 16);
		value[n] = n | ~n << 16;
		object_index = n % (count - 1);
		ptr[n]  = bo_map[object_index] + delta / 4;

		igt_assert_eq_u32(*ptr[n], value[n]);
	}

	for (i = 0; i < count; i++) {
		munmap(bo_map[i], bo_size);
		gem_close(fd, bo[i]);
	}

	munmap(batch_map, bo_size);
	put_ahnd(ahnd);
	syncobj_destroy(fd, sync[0].handle);
	syncobj_destroy(fd, syncobjs);
	xe_exec_queue_destroy(fd, exec_queues);
	xe_vm_destroy(fd, vm);
}

/**
 * SUBTEST: persistent
 * Description: Validate MI_PRT_BATCH_BUFFER_START functionality
 */
static void persistent(int fd)
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
	struct data *sd_data;
	struct data *prt_data;
	struct drm_xe_engine *engine;
	uint32_t vm, exec_queue, syncobj;
	uint32_t sd_batch, prt_batch;
	uint64_t addr = 0x100000;
	int value = 0x123456;
	size_t batch_size = 4096;

	syncobj = syncobj_create(fd, 0);
	sync.handle = syncobj;

	vm = xe_vm_create(fd, 0, 0);
	batch_size = xe_bb_size(fd, batch_size);

	engine = xe_engine(fd, 1);
	sd_batch = xe_bo_create(fd, vm, batch_size,
			      vram_if_possible(fd, engine->instance.gt_id),
			      DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	prt_batch = xe_bo_create(fd, vm, batch_size,
			      vram_if_possible(fd, engine->instance.gt_id),
			      DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);

	xe_vm_bind_sync(fd, vm, sd_batch, 0, addr, batch_size);
	sd_data = xe_bo_map(fd, sd_batch, batch_size);
	prt_data = xe_bo_map(fd, prt_batch, batch_size);

	store_dword_batch(sd_data, addr, value);
	persistance_batch(prt_data, addr);

	exec_queue = xe_exec_queue_create(fd, vm, &engine->instance, 0);
	exec.exec_queue_id = exec_queue;
	exec.address = prt_data->addr;
	sync.flags &= DRM_XE_SYNC_FLAG_SIGNAL;
	xe_exec(fd, &exec);

	igt_assert(syncobj_wait(fd, &syncobj, 1, INT64_MAX, 0, NULL));
	igt_assert_eq(sd_data->data, value);

	syncobj_destroy(fd, syncobj);
	munmap(sd_data, batch_size);
	munmap(prt_data, batch_size);
	gem_close(fd, sd_batch);
	gem_close(fd, prt_batch);

	xe_exec_queue_destroy(fd, exec_queue);
	xe_vm_destroy(fd, vm);
}

#define LONG_SHADER_VALUE(n)	(0xcafe0000 + (n))

/**
 * SUBTEST: long-shader-bb-check
 * Description: Write incrementing values to 2-page-long target surface using
 *		long shader. Check if the bb contains full shader. Check if all
 *		written values are in the target surface. Place bb and surface
 *		in various memory regions to validate memory coherency.
 */
static void long_shader(int fd, struct drm_xe_engine_class_instance *hwe,
			uint64_t bb_region, uint64_t target_region)
{
	const uint64_t target_offset = 0x1a000000;
	const uint64_t bb_offset = 0x1b000000;
	const size_t bb_size = 32768;
	uint32_t vm_id;
	uint32_t exec_queue;
	const unsigned int instruction_count = 128;
	const unsigned int walker_dim_x = 4;
	const unsigned int walker_dim_y = 8;
	const unsigned int surface_dim_x = 64;
	const unsigned int surface_dim_y = instruction_count;
	struct gpgpu_shader *shader;
	struct intel_buf *buf;
	struct intel_bb *ibb;
	uint32_t *ptr;

	buf = intel_buf_create_full(buf_ops_create(fd), 0, surface_dim_x / 4, surface_dim_y,
				    32, 0, I915_TILING_NONE, 0, 0, 0, target_region,
				    DEFAULT_PAT_INDEX, DEFAULT_MOCS_INDEX);
	buf->addr.offset = target_offset;

	vm_id = xe_vm_create(fd, DRM_XE_VM_CREATE_FLAG_LR_MODE, 0);
	exec_queue = xe_exec_queue_create(fd, vm_id, hwe, 0);

	ibb = intel_bb_create_with_context_in_region(fd, exec_queue, vm_id, NULL, bb_size, bb_region);
	intel_bb_remove_object(ibb, ibb->handle, ibb->batch_offset, ibb->size);
	intel_bb_add_object(ibb, ibb->handle, ibb->size, bb_offset, ibb->alignment, false);
	ibb->batch_offset = bb_offset;

	intel_bb_set_lr_mode(ibb, true);

	shader = gpgpu_shader_create(fd);
	gpgpu_shader__nop(shader);
	for (int i = 0; i < instruction_count; i++)
		gpgpu_shader__common_target_write_u32(shader, i, LONG_SHADER_VALUE(i));
	gpgpu_shader__nop(shader);
	gpgpu_shader__eot(shader);

	gpgpu_shader_exec(ibb, buf, walker_dim_x, walker_dim_y, shader, NULL, 0, 0);
	intel_bb_sync(ibb);

	ptr = xe_bo_map(fd, ibb->handle, ibb->size);
	igt_assert_f(memmem(ptr, ibb->size, shader->code, shader->size * sizeof(uint32_t)),
		     "Could not find kernel in bb!\n");
	gem_munmap(ptr, ibb->size);

	gpgpu_shader_destroy(shader);

	ptr = xe_bo_map(fd, buf->handle, buf->surface[0].size);
	for (int i = 0; i < buf->surface[0].size / 4; i += 16)
		for (int j = 0; j < 4; j++)
			igt_assert(ptr[i + j] == LONG_SHADER_VALUE(i / 16));
	gem_munmap(ptr, buf->surface[0].size);

	intel_bb_destroy(ibb);
	xe_exec_queue_destroy(fd, exec_queue);
	xe_vm_destroy(fd, vm_id);
	free(buf);
}

/**
 * SUBTEST: mem-write-ordering-check
 * Description: Verify that copy engines writes to sys mem is ordered
 * Test category: functionality test
 *
 */
static void mem_transaction_ordering(int fd, size_t bo_size, bool fence)
{
	struct drm_xe_engine_class_instance inst = {
		.engine_class = DRM_XE_ENGINE_CLASS_COPY,
	};
	struct drm_xe_sync sync[2] = {
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, },
		{ .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, }
	};

	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 2,
		.syncs = to_user_pointer(&sync),
	};

	int count = 3; // src, dest, batch
	int i, b = 0;
	uint64_t offset[count];
	uint64_t dst_offset;
	uint64_t src_offset;
	uint32_t exec_queues, vm, syncobjs;
	uint32_t bo[count], *bo_map[count];
	uint64_t ahnd;
	uint32_t *batch_map;
	int src_idx = 0, dst_idx = 1;
	size_t bytes_written, size;

	bo_size = ALIGN(bo_size, xe_get_default_alignment(fd));
	bytes_written = bo_size;
	vm = xe_vm_create(fd, 0, 0);
	ahnd = intel_allocator_open(fd, 0, INTEL_ALLOCATOR_SIMPLE);
	exec_queues = xe_exec_queue_create(fd, vm, &inst, 0);
	syncobjs = syncobj_create(fd, 0);
	sync[0].handle = syncobj_create(fd, 0);

	for (i = 0; i < count; i++) {
		bo[i] = xe_bo_create_caching(fd, vm, bo_size, system_memory(fd), 0,
					     DRM_XE_GEM_CPU_CACHING_WC);
		bo_map[i] = xe_bo_map(fd, bo[i], bo_size);
		offset[i] = intel_allocator_alloc_with_strategy(ahnd, bo[i],
								bo_size, 0,
								ALLOC_STRATEGY_NONE);
		xe_vm_bind_async(fd, vm, 0, bo[i], 0, offset[i], bo_size, sync, 1);
	}

	batch_map = xe_bo_map(fd, bo[i - 1], bo_size);
	exec.address = offset[i - 1];

	// Fill source buffer with a pattern
	for (i = 0; i < bo_size; i++)
		((uint8_t *)bo_map[src_idx])[i] = i % bo_size;

	dst_offset = offset[dst_idx];
	src_offset = offset[src_idx];
	while (bo_size) {
		size = min(MAX_DATA_WRITE, bo_size);
		batch_map[b++] = MEM_COPY_CMD;
		batch_map[b++] = size - 1;// src # of bytes
		batch_map[b++] = 0; //src height
		batch_map[b++] = -1; // src pitch
		batch_map[b++] = -1; // dist pitch
		batch_map[b++] = src_offset;
		batch_map[b++] = src_offset  >> 32;
		batch_map[b++] = dst_offset;
		batch_map[b++] = dst_offset  >> 32;
		batch_map[b++] = intel_get_uc_mocs_index(fd) << 25 | intel_get_uc_mocs_index(fd);

		src_offset += size;
		dst_offset += size;
		bo_size -= size;
	}
	if (fence)
		batch_map[b++] = MI_MEM_FENCE | MI_WRITE_FENCE;

	batch_map[b++] = MI_BATCH_BUFFER_END;
	sync[0].flags &= ~DRM_XE_SYNC_FLAG_SIGNAL;
	sync[1].flags |= DRM_XE_SYNC_FLAG_SIGNAL;
	sync[1].handle = syncobjs;
	exec.exec_queue_id = exec_queues;
	xe_exec(fd, &exec);
	igt_assert(syncobj_wait(fd, &syncobjs, 1, INT64_MAX, 0, NULL));

	if (fence) {
		igt_assert(memcmp(bo_map[src_idx], bo_map[dst_idx], bytes_written) == 0);
	} else {
		bool detected_out_of_order = false;

		for (i = bo_size - 1; i >= 0; i--) {
			if (((uint8_t *)bo_map[src_idx])[i] != ((uint8_t *)bo_map[dst_idx])[i]) {
				detected_out_of_order = true;
				break;
			}
		}

		if (detected_out_of_order)
			igt_info("Test detected out of order write at idx %d\n", i);
		else
			igt_info("Test didn't detect out of order writes\n");
	}

	for (i = 0; i < count; i++) {
		munmap(bo_map[i], bo_size);
		gem_close(fd, bo[i]);
	}

	munmap(batch_map, bo_size);
	put_ahnd(ahnd);
	syncobj_destroy(fd, sync[0].handle);
	syncobj_destroy(fd, syncobjs);
	xe_exec_queue_destroy(fd, exec_queues);
	xe_vm_destroy(fd, vm);
}

int igt_main()
{
	struct drm_xe_engine_class_instance *hwe;
	int fd;
	uint16_t dev_id;
	struct drm_xe_engine *engine;

	igt_fixture() {
		fd = drm_open_driver(DRIVER_XE);
		xe_device_get(fd);
		dev_id = intel_get_drm_devid(fd);
	}

	igt_subtest("basic-store") {
		engine = xe_engine(fd, 1);
		basic_inst(fd, STORE, &engine->instance, dev_id);
	}

	igt_subtest("basic-cond-batch") {
		engine = xe_engine(fd, 1);
		basic_inst(fd, COND_BATCH, &engine->instance, dev_id);
	}

	igt_subtest_with_dynamic("basic-all") {
		xe_for_each_engine(fd, hwe) {
			igt_dynamic_f("Engine-%s-Instance-%d-Tile-%d",
				      xe_engine_class_string(hwe->engine_class),
				      hwe->engine_instance,
				      hwe->gt_id);
			basic_inst(fd, STORE, hwe, dev_id);
		}
	}

	igt_subtest("cachelines")
		xe_for_each_engine(fd, hwe)
			store_cachelines(fd, hwe, 0);

	igt_subtest("page-sized")
		xe_for_each_engine(fd, hwe)
			store_cachelines(fd, hwe, PAGES);

	igt_subtest("persistent")
		persistent(fd);

	igt_subtest_with_dynamic("long-shader-bb-check") {
		struct igt_collection *set;
		struct igt_collection *regions;

		set = xe_get_memory_region_set(fd, DRM_XE_MEM_REGION_CLASS_SYSMEM,
					       DRM_XE_MEM_REGION_CLASS_VRAM);

		xe_for_each_engine(fd, hwe) {
			if (hwe->engine_class != DRM_XE_ENGINE_CLASS_RENDER &&
			    hwe->engine_class != DRM_XE_ENGINE_CLASS_COMPUTE)
				continue;

			for_each_variation_r(regions, 2, set) {
				uint32_t bb_region = igt_collection_get_value(regions, 0);
				uint32_t target_region = igt_collection_get_value(regions, 1);

				igt_dynamic_f("gt%d-%s%d-bb-%s-target-%s",
					      hwe->gt_id, xe_engine_class_string(hwe->engine_class),
					      hwe->engine_instance, xe_region_name(bb_region),
					      xe_region_name(target_region))
					long_shader(fd, hwe, bb_region, target_region);
			}
		}

		igt_collection_destroy(set);
	}

	igt_describe("Verify memory relax ordering using copy/write operations");
	igt_subtest_with_dynamic("mem-write-ordering-check") {
		struct {
			size_t size;
			const char *label;
		} sizes[] = {
			{ SZ_1M,  "1M" },
			{ SZ_2M,  "2M" },
			{ SZ_8M,  "8M" },
		};

		for (size_t i = 0; i < ARRAY_SIZE(sizes); i++) {
			igt_dynamic_f("size-%s", sizes[i].label) {
				mem_transaction_ordering(fd, sizes[i].size, true);
				mem_transaction_ordering(fd, sizes[i].size, false);
			}
		}
	}

	igt_fixture() {
		xe_device_put(fd);
		close(fd);
	}
}
