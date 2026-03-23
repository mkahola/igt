// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

/**
 * TEST: Tests for prefetch fault functionality
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: prefetch fault
 * Functionality: prefetch fault
 * Test category: functionality test
 */
#include "gpgpu_shader.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"
#include "xe/xe_gt.h"
#include "xe/xe_util.h"
#include "igt.h"
#include "igt_sysfs.h"
#include "intel_pat.h"
#include "intel_mocs.h"

#define WALKER_X_DIM	4
#define WALKER_Y_DIM	1
#define PAGE_SIZE 4096
#define COLOR_C4 0xC4C4C4C4
#define USER_FENCE_VALUE	0xdeadbeefdeadbeefull
#define PREFETCH_ADDR	0x1f000000
#define BB_OFFSET	0x1b000000
#define BB_OFFSET_SVM	0x2b000000

struct dim_t {
	uint32_t x;
	uint32_t y;
	uint32_t alignment;
};

/**
 * gpgpu_shader__e64b_prefetch_fault:
 * @shdr: shader to be modified
 * @addr: ppgtt virtual address to raise prefetch fault
 *
 * This shader can only be used when in efficient 64bit mode.
 * For a given arbitrary ppgtt virtual address, it raises prefetch fault using load instruction.
 */
static void gpgpu_shader__prefetch_fault(struct gpgpu_shader *shdr,
					 uint64_t addr)
{
	igt_assert_f((addr & 0x3) == 0, "address must be aligned to DWord!\n");

	emit_iga64_code(shdr, xe_prefetch_fault_prefetch, "			\n\
#define IGA64_FLAGS \"\"							\n\
#if GEN_VER >= 3500								\n\
L0:										\n\
// Set base address with scalar register					\n\
(W)	mov (1)		s0.0<1>:ud		ARG(0):ud			\n\
(W)	mov (1)		s0.1<1>:ud		ARG(1):ud			\n\
										\n\
// A64 offset									\n\
(W)	mov (8)		r30.0<1>:uq		0x0:uq				\n\
										\n\
// efficient 64bit Read with cached L1, cached L2 and cached L3			\n\
// sendg ugm load with SBID 3							\n\
// Message Descriptor								\n\
//      bspec:71885								\n\
//      0x99C00 =>								\n\
//      [45:44] Offset Scaling: 0 (disable)					\n\
//      [43:22] Global Offset: 0						\n\
//      [21] Overfetch: 0 (disable)						\n\
//      [19:16] Cache: 9 (L1 cached, L2 cached and L3 cached)			\n\
//      [15:14] Address Type and Size: 2 (Flat A64 Base, A64 Index)		\n\
//      [13:11] Data Size: 3 (D64)						\n\
//      [10:10] Transpose : 1 (enable)						\n\
//      [9:7] Vector Size: 0 (Vector length 1)					\n\
//      [5:0] Opcode: 0 (Load)							\n\
// Prefetch operations are implemented using a NULL destination register.       \n\
(W)	sendg.ugm (1|M0)	null	r30:1	null:0	s0.0	0x99C00	{A@1,$5}\n\
										\n\
#endif										\n\
	", lower_32_bits(addr), upper_32_bits(addr));
}

static struct intel_buf *
create_buf(int fd, int width, int height, uint32_t color)
{
	struct intel_buf *buf;
	uint32_t *ptr;
	size_t i;

	buf = intel_buf_create(buf_ops_create(fd), width, height, 32, 0,
			       I915_TILING_NONE, 0);

	ptr = xe_bo_map(fd, buf->handle, buf->size);
	igt_assert(ptr != MAP_FAILED);

	for (i = 0; i < buf->size / sizeof(uint32_t); i++)
		ptr[i] = color;

	munmap(ptr, buf->size);

	return buf;
}

static struct intel_bb *xe_bb_create_on_offset(int fd, uint32_t exec_queue, uint32_t vm,
					       uint64_t offset, uint32_t size)
{
	struct intel_bb *ibb;

	ibb = intel_bb_create_with_context(fd, exec_queue, vm, NULL, size);

	/* update intel bb offset */
	intel_bb_remove_object(ibb, ibb->handle, ibb->batch_offset, ibb->size);
	intel_bb_add_object(ibb, ibb->handle, ibb->size, offset, ibb->alignment, false);
	ibb->batch_offset = offset;

	return ibb;
}

static struct gpgpu_shader *get_prefetch_shader(int fd)
{
	static struct gpgpu_shader *shader;

	shader = gpgpu_shader_create(fd);
	gpgpu_shader__prefetch_fault(shader, xe_canonical_va(fd, PREFETCH_ADDR));
	gpgpu_shader__eot(shader);

	return shader;
}

/**
 * SUBTEST: prefetch-fault
 * Description: Validate prefetch fault and hit-under-miss behavior
 * Run type: FULL
 *
 * SUBTEST: prefetch-fault-svm
 * Description: Validate prefetch fault and hit-under-miss behavior in SVM mode
 * Run type: FULL
 */
static void test_prefetch_fault(int fd, struct drm_xe_engine_class_instance *hwe, bool svm)
{
	uint64_t bb_offset = BB_OFFSET;
	/*
	 * For the hit-under-miss run, place the batch at PREFETCH_ADDR in
	 * non-SVM mode so the BO bind maps that page before the shader runs.
	 * In SVM mode PREFETCH_ADDR is reserved for the mmap, so use a
	 * separate offset that doesn't collide with it.
	 */
	uint64_t bb_offset2 = svm ? BB_OFFSET_SVM : PREFETCH_ADDR;
	const size_t bb_size = 4096;
	static const char *stat = "invalid_prefetch_pagefault_count";
	struct dim_t w_dim = { .x = WALKER_X_DIM, .y = WALKER_Y_DIM };
	struct gpgpu_shader *shader;
	struct intel_bb *ibb;
	struct intel_buf *buf;
	uint32_t exec_queue_id, vm;
	void *cpu_data = NULL;
	int prefetch_pre, prefetch_post;
	uint32_t *ptr;

	buf = create_buf(fd, w_dim.x, w_dim.y, COLOR_C4);

	vm = xe_vm_create(fd, DRM_XE_VM_CREATE_FLAG_LR_MODE |
			  DRM_XE_VM_CREATE_FLAG_FAULT_MODE, 0);

	if (svm) {
		/*
		 * Enable SVM: mirror the full VA space so GPU page faults are
		 * resolved via HMM against the CPU page tables.
		 */
		struct xe_device *xe = xe_device_get(fd);
		uint64_t vm_sync = 0;
		struct drm_xe_sync sync[1] = {
			{ .type = DRM_XE_SYNC_TYPE_USER_FENCE, .flags = DRM_XE_SYNC_FLAG_SIGNAL,
			  .timeline_value = USER_FENCE_VALUE },
		};

		sync[0].addr = to_user_pointer(&vm_sync);
		__xe_vm_bind_assert(fd, vm, 0, 0, 0, 0, 0x1ull << xe->va_bits,
				    DRM_XE_VM_BIND_OP_MAP,
				    DRM_XE_VM_BIND_FLAG_CPU_ADDR_MIRROR,
				    sync, 1, 0, 0);
		xe_wait_ufence(fd, &vm_sync, USER_FENCE_VALUE, 0, NSEC_PER_SEC);
	}

	exec_queue_id = xe_exec_queue_create(fd, vm, hwe, 0);

	prefetch_pre = xe_gt_stats_get_count(fd, hwe->gt_id, stat);

	/* First run: PREFETCH_ADDR is unmapped, so each shader lane raises a prefetch fault. */
	ibb = xe_bb_create_on_offset(fd, exec_queue_id, vm, bb_offset, bb_size);
	intel_bb_set_lr_mode(ibb, true);

	shader = get_prefetch_shader(fd);
	gpgpu_shader_exec(ibb, buf, w_dim.x, w_dim.y, shader, NULL, 0, 0);
	gpgpu_shader_destroy(shader);
	intel_bb_sync(ibb);
	intel_bb_destroy(ibb);

	prefetch_post = xe_gt_stats_get_count(fd, hwe->gt_id, stat);
	igt_assert_eq(prefetch_post, prefetch_pre + w_dim.x * w_dim.y);

	/*
	 * Hit-under-miss: ensure the page at PREFETCH_ADDR is already mapped
	 * before the prefetch shader runs again. The fault is resolved
	 * successfully so the prefetch counter must not change.
	 *
	 * SVM:     mmap at PREFETCH_ADDR creates a CPU page table entry.
	 *          The kernel resolves the GPU pagefault via HMM.
	 * Non-SVM: placing the batch buffer at PREFETCH_ADDR causes the
	 *          BO fault path to map the page.
	 */
	if (svm) {
		cpu_data = mmap((void *)PREFETCH_ADDR, PAGE_SIZE,
				PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
				-1, 0);
		igt_assert(cpu_data == (void *)PREFETCH_ADDR);
		/* Touch the page to populate the CPU PTE so HMM can resolve the GPU fault. */
		memset(cpu_data, 0xAB, PAGE_SIZE);
		prefetch_pre = xe_gt_stats_get_count(fd, hwe->gt_id, stat);
	} else {
		prefetch_pre = prefetch_post;
	}
	ibb = xe_bb_create_on_offset(fd, exec_queue_id, vm, bb_offset2, bb_size);
	intel_bb_set_lr_mode(ibb, true);

	shader = get_prefetch_shader(fd);
	gpgpu_shader_exec(ibb, buf, w_dim.x, w_dim.y, shader, NULL, 0, 0);
	gpgpu_shader_destroy(shader);
	intel_bb_sync(ibb);

	prefetch_post = xe_gt_stats_get_count(fd, hwe->gt_id, stat);
	igt_assert_eq(prefetch_post, prefetch_pre);

	/* Verify buffer contents */
	ptr = xe_bo_mmap_ext(fd, buf->handle, buf->size, PROT_READ);
	for (int j = 0; j < w_dim.y; j++)
		for (int i = 0; i < w_dim.x; i++) {
			igt_assert_f(ptr[j * w_dim.x + i] == COLOR_C4,
				     "Expected 0x%02x, found 0x%02x at (%d,%d)\n",
				     COLOR_C4, ptr[j * w_dim.x + i], i, j);
		}
	munmap(ptr, buf->size);

	/* Cleanup */
	if (svm && cpu_data)
		munmap(cpu_data, PAGE_SIZE);

	intel_bb_destroy(ibb);
	intel_buf_destroy(buf);
	xe_exec_queue_destroy(fd, exec_queue_id);
	xe_vm_destroy(fd, vm);
}

int igt_main()
{
	struct drm_xe_engine_class_instance *hwe;
	bool svm_supported;
	int fd;

	igt_fixture() {
		fd = drm_open_driver(DRIVER_XE);
		igt_require(intel_graphics_ver(intel_get_drm_devid(fd)) >= IP_VER(35, 0));
		svm_supported = !xe_supports_faults(fd);
	}

	igt_subtest_with_dynamic("prefetch-fault") {
		xe_for_each_engine(fd, hwe) {
			if (hwe->engine_class == DRM_XE_ENGINE_CLASS_RENDER ||
			    hwe->engine_class == DRM_XE_ENGINE_CLASS_COMPUTE) {
				igt_dynamic_f("%s%d", xe_engine_class_string(hwe->engine_class),
					      hwe->engine_instance)
					test_prefetch_fault(fd, hwe, false);
			}
		}
	}

	igt_subtest_with_dynamic("prefetch-fault-svm") {
		if (!svm_supported)
			igt_skip("SVM not supported on this device, skipping.\n");
		xe_for_each_engine(fd, hwe) {
			if (hwe->engine_class == DRM_XE_ENGINE_CLASS_RENDER ||
			    hwe->engine_class == DRM_XE_ENGINE_CLASS_COMPUTE) {
				igt_dynamic_f("%s%d", xe_engine_class_string(hwe->engine_class),
					      hwe->engine_instance)
					test_prefetch_fault(fd, hwe, true);
			}
		}
	}

	igt_fixture() {
		drm_close_driver(fd);
	}
}
