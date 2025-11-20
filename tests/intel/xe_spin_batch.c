#include "igt.h"
#include "igt_syncobj.h"
#include "lib/intel_mocs.h"
#include "lib/intel_pat.h"
#include "lib/intel_reg.h"
#include "xe_drm.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"
#include "xe/xe_spin.h"
#include "xe/xe_util.h"

/**
 * TEST: Tests for spin batch submissons.
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: IGT Lib
 * Functionality: parallel execution
 * Test category: functionality test
 */

/**
 * SUBTEST: spin-basic
 * Description: Basic test to submit spin batch submissons on copy engine.
 */

static void spin_basic(int fd)
{
	uint64_t ahnd;
	igt_spin_t *spin;

	ahnd = intel_allocator_open(fd, 0, INTEL_ALLOCATOR_RELOC);
	spin = igt_spin_new(fd, .ahnd = ahnd);

	igt_spin_free(fd, spin);
	put_ahnd(ahnd);
}

/**
 * SUBTEST: spin-batch
 * Description: Create vm and engine of hwe class and run the spinner on it.
 */

static void spin(int fd, struct drm_xe_engine_class_instance *hwe)
{
	uint64_t ahnd;
	unsigned int exec_queue;
	uint32_t vm;
	igt_spin_t *spin;

	vm = xe_vm_create(fd, 0, 0);
	exec_queue = xe_exec_queue_create(fd, vm, hwe, 0);
	ahnd = intel_allocator_open(fd, 0, INTEL_ALLOCATOR_RELOC);

	spin = igt_spin_new(fd, .ahnd = ahnd, .engine = exec_queue, .vm = vm);

	igt_spin_free(fd, spin);
	xe_exec_queue_destroy(fd, exec_queue);
	xe_vm_destroy(fd, vm);

	put_ahnd(ahnd);
}

/**
 * SUBTEST: spin-basic-all
 * Description: Basic test which validates the functionality of spinner on all hwe.
 */
static void spin_basic_all(int fd)
{
	struct drm_xe_engine_class_instance *hwe;
	uint64_t ahnd;
	uint32_t vm;
	igt_spin_t **spin;
	int i = 0;

	vm = xe_vm_create(fd, 0, 0);
	ahnd = intel_allocator_open(fd, vm, INTEL_ALLOCATOR_RELOC);
	spin = malloc(sizeof(*spin) * xe_number_engines(fd));
	xe_for_each_engine(fd, hwe) {
		igt_debug("Run on engine: %s:%d\n",
			  xe_engine_class_string(hwe->engine_class), hwe->engine_instance);
		spin[i] = igt_spin_new(fd, .ahnd = ahnd, .vm = vm, .hwe = hwe);
		i++;
	}

	while (--i >= 0)
		igt_spin_free(fd, spin[i]);

	put_ahnd(ahnd);
	xe_vm_destroy(fd, vm);
	free(spin);
}

/**
 * SUBTEST: spin-all
 * Description: Spinner test to run on all the engines!
 */

static void spin_all(int fd, int gt, int class)
{
	uint64_t ahnd;
	uint32_t exec_queues[XE_MAX_ENGINE_INSTANCE], vm;
	int i, num_placements = 0;
	struct drm_xe_engine_class_instance eci[XE_MAX_ENGINE_INSTANCE];
	igt_spin_t *spin[XE_MAX_ENGINE_INSTANCE];
	struct drm_xe_engine_class_instance *hwe;

	ahnd = intel_allocator_open(fd, 0, INTEL_ALLOCATOR_RELOC);

	xe_for_each_engine(fd, hwe) {
		if (hwe->engine_class != class || hwe->gt_id != gt)
			continue;
		eci[num_placements++] = *hwe;
	}
	if (num_placements < 2)
		return;
	vm = xe_vm_create(fd, 0, 0);

	for (i = 0; i < num_placements; i++) {
		igt_assert_eq(__xe_exec_queue_create(fd, vm, 1, num_placements,
						     eci, 0, &exec_queues[i]), 0);
		spin[i] = igt_spin_new(fd, .ahnd = ahnd, .engine = exec_queues[i], .vm = vm);
	}

	for (i = 0; i < num_placements; i++) {
		igt_spin_free(fd, spin[i]);
		xe_exec_queue_destroy(fd, exec_queues[i]);
	}

	put_ahnd(ahnd);
	xe_vm_destroy(fd, vm);
}

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

static void preempter(int fd, struct drm_xe_engine_class_instance *hwe)
{
	struct drm_xe_sync sync = {
		.flags = DRM_XE_SYNC_TYPE_SYNCOBJ | DRM_XE_SYNC_FLAG_SIGNAL
	};
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 1,
		.syncs = to_user_pointer(&sync),
	};
	struct drm_xe_ext_set_property ext = {
		.base.next_extension = 0,
		.base.name = DRM_XE_EXEC_QUEUE_EXTENSION_SET_PROPERTY,
		.property = DRM_XE_EXEC_QUEUE_SET_PROPERTY_PRIORITY,
		.value = 2, /* High priority */
	};
	struct data *data;
	uint32_t vm;
	uint32_t exec_queue;
	uint32_t syncobj;
	size_t bo_size;
	int value = 0x123456;
	uint64_t addr = 0x100000;
	uint32_t bo = 0;

	syncobj = syncobj_create(fd, 0);
	sync.handle = syncobj;

	vm = xe_vm_create(fd, 0, 0);
	bo_size = sizeof(*data);
	bo_size = xe_bb_size(fd, bo_size);

	bo = xe_bo_create(fd, vm, bo_size,
			  vram_if_possible(fd, hwe->gt_id),
			  DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);

	xe_vm_bind_async(fd, vm, 0, bo, 0, addr, bo_size, &sync, 1);
	data = xe_bo_map(fd, bo, bo_size);
	store_dword_batch(data, addr, value);

	igt_assert(syncobj_wait(fd, &syncobj, 1, INT64_MAX, 0, NULL));
	syncobj_reset(fd, &syncobj, 1);

	exec_queue = xe_exec_queue_create(fd, vm, hwe, to_user_pointer(&ext));
	exec.exec_queue_id = exec_queue;
	exec.address = data->addr;
	sync.flags &= DRM_XE_SYNC_FLAG_SIGNAL;
	xe_exec(fd, &exec);

	igt_assert(syncobj_wait(fd, &syncobj, 1, INT64_MAX, 0, NULL));
	igt_assert_eq(data->data, value);

	syncobj_destroy(fd, syncobj);
	munmap(data, bo_size);
	gem_close(fd, bo);

	xe_exec_queue_destroy(fd, exec_queue);
	xe_vm_destroy(fd, vm);
}

#define SPIN_FIX_DURATION_NORMAL		0
#define SPIN_FIX_DURATION_PREEMPT		1
/**
 * SUBTEST: spin-fixed-duration
 * Description: Basic test which validates the functionality of xe_spin with
 *		fixed duration.
 */
/**
 * SUBTEST: spin-fixed-duration-with-preempter
 * Description: Basic test which validates the functionality of xe_spin
 *		preemption which gets preempted with a short duration
 *		high-priority task.
 */
static void xe_spin_fixed_duration(int fd, int gt, int class, int flags)
{
	struct drm_xe_sync sync = {
		.handle = syncobj_create(fd, 0),
		.type = DRM_XE_SYNC_TYPE_SYNCOBJ,
		.flags = DRM_XE_SYNC_FLAG_SIGNAL,
	};
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 1,
		.syncs = to_user_pointer(&sync),
	};
	struct drm_xe_ext_set_property ext_prio = {
		.base.next_extension = 0,
		.base.name = DRM_XE_EXEC_QUEUE_EXTENSION_SET_PROPERTY,
		.property = DRM_XE_EXEC_QUEUE_SET_PROPERTY_PRIORITY,
		.value = 0, /* Low priority */
	};
	struct drm_xe_engine_class_instance *hwe = NULL, *_hwe;
	const uint64_t duration_ns = NSEC_PER_SEC / 10; /* 100ms */
	uint64_t spin_addr;
	uint64_t ahnd;
	uint32_t exec_queue;
	uint32_t vm;
	uint32_t bo;
	uint64_t ext = 0;
	size_t bo_size;
	struct xe_spin *spin;
	struct timespec tv;
	double elapsed_ms;
	igt_stats_t stats;
	int i;

	if (flags & SPIN_FIX_DURATION_PREEMPT)
		ext = to_user_pointer(&ext_prio);

	xe_for_each_engine(fd, _hwe)
		if (_hwe->engine_class == class && _hwe->gt_id == gt)
			hwe = _hwe;

	if (!hwe)
		return;

	vm = xe_vm_create(fd, 0, 0);
	exec_queue = xe_exec_queue_create(fd, vm, hwe, ext);
	ahnd = intel_allocator_open(fd, 0, INTEL_ALLOCATOR_RELOC);
	bo_size = xe_bb_size(fd, sizeof(*spin));
	bo = xe_bo_create(fd, vm, bo_size, vram_if_possible(fd, 0),
			  DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	spin = xe_bo_map(fd, bo, bo_size);
	spin_addr = intel_allocator_alloc_with_strategy(ahnd, bo, bo_size, 0,
							ALLOC_STRATEGY_LOW_TO_HIGH);
	xe_vm_bind_sync(fd, vm, bo, 0, spin_addr, bo_size);
	xe_spin_init_opts(spin, .addr = spin_addr,
				.preempt = true,
				.ctx_ticks = xe_spin_nsec_to_ticks(fd, 0, duration_ns));
	exec.address = spin_addr;
	exec.exec_queue_id = exec_queue;

#define NSAMPLES 5
	igt_stats_init_with_size(&stats, NSAMPLES);
	for (i = 0; i < NSAMPLES; ++i) {
		igt_gettime(&tv);
		xe_exec(fd, &exec);
		xe_spin_wait_started(spin);
		if (flags & SPIN_FIX_DURATION_PREEMPT)
			preempter(fd, hwe);

		igt_assert(syncobj_wait(fd, &sync.handle, 1, INT64_MAX, 0, NULL));
		igt_stats_push_float(&stats, igt_nsec_elapsed(&tv) * 1e-6);
		syncobj_reset(fd, &sync.handle, 1);
		igt_debug("i=%d %.2fms\n", i, stats.values_f[i]);
	}
	elapsed_ms = igt_stats_get_median(&stats);
	igt_info("%s: %.0fms spin took %.2fms (median)\n", xe_engine_class_string(hwe->engine_class),
		 duration_ns * 1e-6, elapsed_ms);
	igt_assert(elapsed_ms < duration_ns * 1.5e-6 && elapsed_ms > duration_ns * 0.5e-6);

	xe_vm_unbind_sync(fd, vm, 0, spin_addr, bo_size);
	syncobj_destroy(fd, sync.handle);
	gem_munmap(spin, bo_size);
	gem_close(fd, bo);
	xe_exec_queue_destroy(fd, exec_queue);
	xe_vm_destroy(fd, vm);
	put_ahnd(ahnd);
}

static void xe_spin_mem_copy_region(int fd, struct drm_xe_engine_class_instance *hwe,
				    uint32_t region)
{
	uint32_t copy_size = SZ_256K;
	uint64_t duration_ns = NSEC_PER_SEC / 10;
	intel_ctx_t *ctx;
	uint32_t vm, exec_queue;
	uint64_t ahnd;
	uint32_t width = copy_size;
	uint32_t height = 1;
	uint32_t bo_size = ALIGN(SZ_4K, xe_get_default_alignment(fd));
	uint32_t bo;
	struct xe_spin *spin;
	uint64_t spin_addr;
	int32_t src_handle, dst_handle;
	struct blt_mem_object src, dst;
	struct xe_spin_mem_copy mem_copy = {
		.fd = fd,
		.src = &src,
		.dst = &dst,
	};

	igt_debug("Using spinner to copy %u kB in region %u with engine %s\n",
		  copy_size / 1024, region, xe_engine_class_string(hwe->engine_class));

	vm = xe_vm_create(fd, 0, 0);
	exec_queue = xe_exec_queue_create(fd, vm, hwe, 0);
	ctx = intel_ctx_xe(fd, vm, exec_queue, 0, 0, 0);
	ahnd = intel_allocator_open_full(fd, vm, 0, 0,
					 INTEL_ALLOCATOR_SIMPLE,
					 ALLOC_STRATEGY_LOW_TO_HIGH, 0);

	/* Create source and destination objects used for the copy */
	src_handle = xe_bo_create(fd, 0, copy_size, region,
				  DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	dst_handle = xe_bo_create(fd, 0, copy_size, region,
				  DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	blt_set_mem_object(mem_copy.src, src_handle, copy_size, width, width, height, region,
			   intel_get_uc_mocs_index(fd), DEFAULT_PAT_INDEX, COMPRESSION_DISABLED);
	blt_set_mem_object(mem_copy.dst, dst_handle, copy_size, width, width, height, region,
			   intel_get_uc_mocs_index(fd), DEFAULT_PAT_INDEX, COMPRESSION_DISABLED);
	mem_copy.src->ptr = xe_bo_map(fd, src_handle, copy_size);
	mem_copy.dst->ptr = xe_bo_map(fd, dst_handle, copy_size);
	mem_copy.src_offset = get_offset_pat_index(ahnd, mem_copy.src->handle,
						   mem_copy.src->size, 0, mem_copy.src->pat_index);
	mem_copy.dst_offset = get_offset_pat_index(ahnd, mem_copy.dst->handle,
						   mem_copy.dst->size, 0, mem_copy.dst->pat_index);

	/* Create spinner */
	bo = xe_bo_create(fd, vm, bo_size, vram_if_possible(fd, 0),
			  DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	spin = xe_bo_map(fd, bo, bo_size);
	spin_addr = intel_allocator_alloc_with_strategy(ahnd, bo, bo_size, 0,
							ALLOC_STRATEGY_LOW_TO_HIGH);
	xe_vm_bind_sync(fd, vm, bo, 0, spin_addr, bo_size);
	xe_spin_init_opts(spin, .addr = spin_addr,
			  .preempt = true,
			  .ctx_ticks = xe_spin_nsec_to_ticks(fd, 0, duration_ns),
			  .mem_copy = &mem_copy);

	/* Run spinner with mem copy and fixed duration */
	src.ptr[0] = 0xdeadbeaf;
	intel_ctx_xe_exec(ctx, ahnd, spin_addr);
	xe_spin_wait_started(spin);
	igt_assert_f(!memcmp(mem_copy.src->ptr, mem_copy.dst->ptr, mem_copy.src->size),
		     "source and destination differ\n");

	/* Cleanup */
	xe_vm_unbind_sync(fd, vm, 0, spin_addr, bo_size);
	gem_munmap(spin, bo_size);
	gem_close(fd, bo);
	gem_munmap(mem_copy.dst->ptr, copy_size);
	gem_munmap(mem_copy.src->ptr, copy_size);
	gem_close(fd, dst_handle);
	gem_close(fd, src_handle);
	put_ahnd(ahnd);
	xe_exec_queue_destroy(fd, exec_queue);
	xe_vm_destroy(fd, vm);
}

/**
 * SUBTEST: spin-mem-copy
 * Description: Basic test which validates the functionality of xe_spin with
 *		fixed duration while performing a copy for each provided region
 */
static void xe_spin_mem_copy(int fd, struct drm_xe_engine_class_instance *hwe,
			     struct igt_collection *set)
{
	struct igt_collection *regions;

	for_each_variation_r(regions, 1, set) {
		uint32_t region = igt_collection_get_value(regions, 0);

		xe_spin_mem_copy_region(fd, hwe, region);
	}
}

#define HANG 1
static void exec_store(int fd, struct drm_xe_engine_class_instance *eci,
		       bool flags)
{
	uint64_t ahnd, bb_size, bb_addr;
	uint32_t vm, exec_queue, bb;
#define USER_FENCE_VALUE	0xdeadbeefdeadbeefull
	struct drm_xe_sync syncobj = {
		.type = DRM_XE_SYNC_TYPE_USER_FENCE,
		.flags = DRM_XE_SYNC_FLAG_SIGNAL,
		.timeline_value = USER_FENCE_VALUE,
	};

	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 1,
		.syncs = to_user_pointer(&syncobj),
	};
	struct {
		uint32_t batch[16];
		uint64_t pad;
		uint32_t data;
		uint64_t vm_sync;
		uint64_t exec_sync;
	} *data;
	uint64_t batch_offset, batch_addr, sdi_offset, sdi_addr;
	int64_t timeout = NSEC_PER_SEC;
	int i, ret;

	ahnd = intel_allocator_open(fd, 0, INTEL_ALLOCATOR_RELOC);

	vm = xe_vm_create(fd, 0, 0);
	exec_queue = xe_exec_queue_create(fd, vm, eci, 0);
	bb_size = xe_bb_size(fd, sizeof(*data));
	bb = xe_bo_create(fd, vm, bb_size, vram_if_possible(fd, eci->gt_id),
			  DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	bb_addr = intel_allocator_alloc_with_strategy(ahnd, bb, bb_size, 0,
						      ALLOC_STRATEGY_LOW_TO_HIGH);
	data = xe_bo_map(fd, bb, bb_size);
	syncobj.addr = to_user_pointer(&data->vm_sync);
	xe_vm_bind_async(fd, vm, 0, bb, 0, bb_addr, bb_size, &syncobj, 1);
	xe_wait_ufence(fd, &data->vm_sync, USER_FENCE_VALUE, 0, NSEC_PER_SEC);

	batch_offset = (char *)&data->batch - (char *)data;
	batch_addr = bb_addr + batch_offset;
	sdi_offset = (char *)&data->data - (char *)data;
	sdi_addr = bb_addr + sdi_offset;

	i = 0;

	data->batch[i++] = MI_STORE_DWORD_IMM_GEN4;
	data->batch[i++] = sdi_addr;
	data->batch[i++] = sdi_addr >> 32;
	data->batch[i++] = 0;
	if (!(flags & HANG))
		data->batch[i++] = MI_BATCH_BUFFER_END;
	igt_assert(i <= ARRAY_SIZE(data->batch));

	syncobj.addr = bb_addr + (char *)&data->exec_sync - (char *)data;
	exec.exec_queue_id = exec_queue;
	exec.address = batch_addr;
	xe_exec(fd, &exec);
	ret = __xe_wait_ufence(fd, &data->exec_sync, USER_FENCE_VALUE, 0, &timeout);
	igt_assert(flags ? ret < 0 : ret == 0);

	munmap(data, bb_size);
	gem_close(fd, bb);

	xe_exec_queue_destroy(fd, exec_queue);
	xe_vm_destroy(fd, vm);

	put_ahnd(ahnd);
}

static void run_spinner(int fd, struct drm_xe_engine_class_instance *eci)
{
	struct xe_cork *cork;
	uint32_t vm;
	uint32_t ts_1, ts_2;
	uint64_t ahnd;

	vm = xe_vm_create(fd, 0, 0);
	ahnd = intel_allocator_open(fd, 0, INTEL_ALLOCATOR_RELOC);
	cork = xe_cork_create_opts(fd, eci, vm, 1, 1, .ahnd = ahnd);
	xe_cork_sync_start(fd, cork);

	/* Collect and check timestamps before stopping the spinner */
	usleep(50000);
	ts_1 = READ_ONCE(cork->spin->timestamp);
	usleep(50000);
	ts_2 = READ_ONCE(cork->spin->timestamp);
	igt_assert_neq_u32(ts_1, ts_2);

	xe_cork_sync_end(fd, cork);
	xe_cork_destroy(fd, cork);

	xe_vm_destroy(fd, vm);
	put_ahnd(ahnd);
}

/**
 * SUBTEST: spin-timestamp-check
 * Description: Initiate gt reset then check the timestamp register for each engine.
 * Test category: functionality test
 */
static void xe_spin_timestamp_check(int fd, struct drm_xe_engine_class_instance *eci)
{
	exec_store(fd, eci, 0); /* sanity check */

	exec_store(fd, eci, HANG); /* hang the engine */

	run_spinner(fd, eci);
}

int igt_main()
{
	struct drm_xe_engine_class_instance *hwe;
	int fd;
	int gt, class;
	struct igt_collection *regions;

	igt_fixture() {
		fd = drm_open_driver(DRIVER_XE);
		regions = xe_get_memory_region_set(fd, DRM_XE_MEM_REGION_CLASS_SYSMEM,
						   DRM_XE_MEM_REGION_CLASS_VRAM);
	}

	igt_subtest("spin-basic")
		spin_basic(fd);

	igt_subtest("spin-batch")
		xe_for_each_engine(fd, hwe)
			spin(fd, hwe);

	igt_subtest("spin-basic-all")
		spin_basic_all(fd);

	igt_subtest("spin-all") {
		xe_for_each_gt(fd, gt)
			xe_for_each_engine_class(class)
				spin_all(fd, gt, class);
	}

	igt_subtest("spin-fixed-duration")
		xe_spin_fixed_duration(fd, 0, DRM_XE_ENGINE_CLASS_COPY, SPIN_FIX_DURATION_NORMAL);


	igt_subtest("spin-fixed-duration-with-preempter")
		xe_for_each_gt(fd, gt)
			xe_for_each_engine_class(class)
				xe_spin_fixed_duration(fd, gt, class, SPIN_FIX_DURATION_PREEMPT);

	igt_subtest_with_dynamic("spin-timestamp-check")
		xe_for_each_engine(fd, hwe)
			igt_dynamic_f("engine-%s", xe_engine_class_string(hwe->engine_class))
				xe_spin_timestamp_check(fd, hwe);

	igt_subtest("spin-mem-copy") {
		igt_require(blt_has_mem_copy(fd));
		xe_for_each_engine(fd, hwe)
			if (hwe->engine_class == DRM_XE_ENGINE_CLASS_COPY)
				xe_spin_mem_copy(fd, hwe, regions);
	}

	igt_fixture()
		drm_close_driver(fd);
}
