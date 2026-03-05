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
	gpgpu_shader__prefetch_fault(shader, xe_canonical_va(fd, 0x1f000000));
	gpgpu_shader__eot(shader);

	return shader;
}

/**
 * SUBTEST: prefetch-fault
 * Description: Validate L1/L2 cache prefetch fault.
 * Run type: FULL
 */

static void test_prefetch(int fd, struct drm_xe_engine_class_instance *hwe)
{
	/* faulty address 0x1f000000 should be beyond bb_offset+bb_size. */
	const uint64_t bb_offset = 0x1b000000;
	const size_t bb_size = 4096;
	struct dim_t w_dim;
	struct gpgpu_shader *shader;
	struct intel_bb *ibb;
	struct intel_buf *buf;
	uint32_t *ptr;
	uint32_t exec_queue_id, vm;
	int prefetch_pre, prefetch_pos;
	static const char *stat = "prefetch_pagefault_count";

	w_dim.x = WALKER_X_DIM;
	w_dim.y = WALKER_Y_DIM;

	buf = create_buf(fd, w_dim.x, w_dim.y, COLOR_C4);

	prefetch_pre = xe_gt_stats_get_count(fd, hwe->gt_id, stat);

	vm = xe_vm_create(fd, DRM_XE_VM_CREATE_FLAG_LR_MODE |
			  DRM_XE_VM_CREATE_FLAG_FAULT_MODE, 0);

	exec_queue_id = xe_exec_queue_create(fd, vm, hwe, 0);

	ibb = xe_bb_create_on_offset(fd, exec_queue_id, vm,
				     bb_offset, bb_size);
	intel_bb_set_lr_mode(ibb, true);

	shader = get_prefetch_shader(fd);

	gpgpu_shader_exec(ibb, buf, w_dim.x, w_dim.y, shader, NULL, 0, 0);

	gpgpu_shader_destroy(shader);

	intel_bb_sync(ibb);

	ptr = xe_bo_mmap_ext(fd, buf->handle, buf->size, PROT_READ);

	for (int j = 0; j < w_dim.y; j++)
		for (int i = 0; i < w_dim.x; i++) {
			igt_assert_f(ptr[j * w_dim.x + i] == COLOR_C4,
				     "Expected 0x%02x, found 0x%02x at (%d,%d)\n",
				     COLOR_C4, ptr[j * w_dim.x + i], i, j);
		}
	/* Validate prefetch count. */
	prefetch_pos = xe_gt_stats_get_count(fd, hwe->gt_id, stat);
	igt_assert_eq(prefetch_pos, prefetch_pre + w_dim.x * w_dim.y);

	munmap(ptr, buf->size);

	intel_bb_destroy(ibb);
}

int igt_main()
{
	struct drm_xe_engine_class_instance *hwe;
	int fd;

	igt_fixture() {
		fd = drm_open_driver(DRIVER_XE);
		igt_require(intel_graphics_ver(intel_get_drm_devid(fd)) >= IP_VER(35, 0));
	}

	igt_subtest_with_dynamic("prefetch-fault") {
		xe_for_each_engine(fd, hwe) {
			if (hwe->engine_class == DRM_XE_ENGINE_CLASS_RENDER ||
			    hwe->engine_class == DRM_XE_ENGINE_CLASS_COMPUTE) {
				igt_dynamic_f("%s%d", xe_engine_class_string(hwe->engine_class),
					      hwe->engine_instance)
					test_prefetch(fd, hwe);
			}
		}
	}

	igt_fixture() {
		drm_close_driver(fd);
	}
}
