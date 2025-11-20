// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

#include "drm.h"
#include "igt.h"
#include "igt_device.h"
#include "intel_blt.h"
#include "intel_mocs.h"
#include "lib/igt_sysfs.h"
#include "lib/intel_chipset.h"
#include "lib/intel_pat.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"
#include "xe/xe_util.h"

/**
 * TEST: xe_peer2peer
 * Category: Core
 * Mega feature: MultiGPU
 * Sub-category: MultiGPU tests
 * Functionality: dma buf copy
 * Description: Peer2peer dma buf copy tests
 * Test category: xe
 *
 * SUBTEST: read
 * Description:
 *   dma buf copy read
 *
 * SUBTEST: write
 * Description:
 *   dma buf copy write
 */

IGT_TEST_DESCRIPTION("Exercise blitter read/writes between two Xe devices");

struct blt_fast_copy_data {
	int xe;
	struct blt_copy_object src;
	struct blt_copy_object mid;
	struct blt_copy_object dst;

	struct blt_copy_batch bb;
	enum blt_color_depth color_depth;
};

struct gpu_info {
	uint32_t id;
	int fd;
	struct igt_collection *set;
};

static bool has_prime(int fd)
{
	uint64_t value;
	uint64_t mask = DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT;

	if (drmGetCap(fd, DRM_CAP_PRIME, &value))
		return false;

	return (value & mask) == mask;
}

static int get_device_info(struct gpu_info gpus[], int num_gpus)
{
	int cnt;
	int xe;
	int i;

	for (i = 0, cnt = 0 && i < 128; cnt < num_gpus; i++) {
		xe = __drm_open_driver_another(i, DRIVER_XE);
		if (xe < 0)
			break;

		/* dma-buf is required */
		if (!has_prime(xe) || !blt_has_fast_copy(xe)) {
			close(xe);
			continue;
		}

		gpus[cnt].fd = xe;
		gpus[cnt].set = xe_get_memory_region_set(xe,
							 DRM_XE_MEM_REGION_CLASS_SYSMEM,
							 DRM_XE_MEM_REGION_CLASS_VRAM);
		cnt++;
	}

	return cnt;
}

/**
 * test_read - Read an imported buffer from an external GPU via dma-buf
 * @ex_gpu: device providing the original object
 * @im_gpu: device doing the read
 * @ex_reg: the source region to copy from
 * @im_reg: the destination region to copy to
 *
 */
static void test_read(struct gpu_info *ex_gpu, struct gpu_info *im_gpu,
		      uint32_t ex_reg, uint32_t im_reg)
{
	struct blt_copy_data im_blt = {};
	struct blt_copy_data ex_blt = {};
	struct blt_copy_object *dst;
	struct blt_copy_object *im_src;
	struct blt_copy_object *src;
	const uint32_t bpp = 32;
	uint64_t im_bb_size = xe_bb_size(im_gpu->fd, SZ_4K);
	uint64_t ahnd;
	uint32_t bb;
	uint32_t width = 1024, height = 1024;
	int result;
	uint32_t vm, exec_queue;
	uint32_t ex_xe = ex_gpu->fd;
	uint32_t im_xe = im_gpu->fd;
	uint32_t ex_src, dmabuf;
	uint32_t stride;
	struct drm_xe_engine_class_instance inst = {
		.engine_class = DRM_XE_ENGINE_CLASS_COPY,
	};
	intel_ctx_t *ctx;
	int err;

	blt_copy_init(ex_xe, &ex_blt);
	src = blt_create_object(&ex_blt, ex_reg, width, height, bpp, 0,
				T_LINEAR, COMPRESSION_DISABLED, 0, true);

	dmabuf = prime_handle_to_fd(ex_xe, src->handle);
	err = __prime_fd_to_handle(im_xe, dmabuf, &ex_src);
	if (err == -ENOTSUP) {
		blt_destroy_object(ex_xe, src);
		igt_assert(ex_reg != system_memory(ex_xe));
		igt_skip("P2P VRAM import not supported on this device, skipping.\n");
	}
	igt_assert(!err);

	vm = xe_vm_create(im_xe, 0, 0);
	exec_queue = xe_exec_queue_create(im_xe, vm, &inst, 0);
	ctx = intel_ctx_xe(im_xe, vm, exec_queue, 0, 0, 0);
	ahnd = intel_allocator_open_full(im_xe, ctx->vm, 0, 0,
					 INTEL_ALLOCATOR_SIMPLE,
					 ALLOC_STRATEGY_LOW_TO_HIGH, 0);

	blt_copy_init(im_xe, &im_blt);

	dst = blt_create_object(&im_blt, im_reg, width, height, bpp, 0,
				T_LINEAR, COMPRESSION_DISABLED, 0, true);
	blt_surface_fill_rect(ex_xe, src, width, height);

	im_src = calloc(1, sizeof(*im_src));

	stride = width * 4;
	blt_set_object(im_src, ex_src, src->size, ex_reg, 0, DEFAULT_PAT_INDEX,
		       T_LINEAR, COMPRESSION_DISABLED, 0);
	blt_set_geom(im_src, stride, 0, 0, width, height, 0, 0);
	igt_assert(im_src->size == dst->size);

	im_blt.color_depth = CD_32bit;
	blt_set_copy_object(&im_blt.src, im_src);
	blt_set_copy_object(&im_blt.dst, dst);

	bb = xe_bo_create(im_xe, 0, im_bb_size, im_reg, 0);
	blt_set_batch(&im_blt.bb, bb, im_bb_size, im_reg);

	blt_fast_copy(im_xe, ctx, NULL, ahnd, &im_blt);

	result = memcmp(src->ptr, im_blt.dst.ptr, src->size);

	put_offset(ahnd, im_src->handle);
	put_offset(ahnd, dst->handle);
	put_offset(ahnd, bb);
	intel_allocator_bind(ahnd, 0, 0);
	blt_destroy_object(im_xe, im_src);
	blt_destroy_object(im_xe, dst);
	blt_destroy_object(ex_xe, src);
	put_ahnd(ahnd);

	igt_assert_f(!result, "source and destination surfaces differs!\n");
}

/**
 * test_write - Write an imported buffer to an external GPU via dma-buf
 * @ex_gpu: device providing the destination object
 * @im_gpu: device doing the write
 * @ex_reg: the source region to copy from
 * @im_reg: the destination region to copy to
 *
 */
static void test_write(struct gpu_info *ex_gpu, struct gpu_info *im_gpu,
		       uint32_t ex_reg, uint32_t im_reg)
{
	struct blt_copy_data im_blt = {};
	struct blt_copy_data ex_blt = {};
	struct blt_copy_object *dst;
	struct blt_copy_object *im_dst;
	struct blt_copy_object *src;
	const uint32_t bpp = 32;
	uint64_t im_bb_size = xe_bb_size(im_gpu->fd, SZ_4K);
	uint64_t ahnd;
	uint32_t bb;
	uint32_t width = 1024, height = 1024;
	int result;
	uint32_t vm, exec_queue;
	uint32_t ex_xe = ex_gpu->fd;
	uint32_t im_xe = im_gpu->fd;
	uint32_t ex_dst, dmabuf;
	uint32_t stride;
	struct drm_xe_engine_class_instance inst = {
		.engine_class = DRM_XE_ENGINE_CLASS_COPY,
	};
	intel_ctx_t *ctx;
	int err;

	blt_copy_init(ex_xe, &ex_blt);
	dst = blt_create_object(&ex_blt, ex_reg, width, height, bpp, 0,
				T_LINEAR, COMPRESSION_DISABLED, 0, true);
	dmabuf = prime_handle_to_fd(ex_xe, dst->handle);
	err = __prime_fd_to_handle(im_xe, dmabuf, &ex_dst);
	if (err == -ENOTSUP) {
		blt_destroy_object(ex_xe, dst);
		igt_assert(ex_reg != system_memory(ex_xe));
		igt_skip("P2P VRAM import not supported on this device, skipping.\n");
	}
	igt_assert(!err);

	vm = xe_vm_create(im_xe, 0, 0);
	exec_queue = xe_exec_queue_create(im_xe, vm, &inst, 0);
	ctx = intel_ctx_xe(im_xe, vm, exec_queue, 0, 0, 0);
	ahnd = intel_allocator_open_full(im_xe, ctx->vm, 0, 0,
					 INTEL_ALLOCATOR_SIMPLE,
					 ALLOC_STRATEGY_LOW_TO_HIGH, 0);

	blt_copy_init(im_xe, &im_blt);

	src = blt_create_object(&im_blt, im_reg, width, height, bpp, 0,
				T_LINEAR, COMPRESSION_DISABLED, 0, true);
	blt_surface_fill_rect(im_xe, src, width, height);

	im_dst = calloc(1, sizeof(*im_dst));

	stride = width * 4;
	blt_set_object(im_dst, ex_dst, src->size, ex_reg, 0, DEFAULT_PAT_INDEX,
		       T_LINEAR, COMPRESSION_DISABLED, 0);
	blt_set_geom(im_dst, stride, 0, 0, width, height, 0, 0);
	igt_assert(im_dst->size == src->size);

	im_blt.color_depth = CD_32bit;
	blt_set_copy_object(&im_blt.src, src);
	blt_set_copy_object(&im_blt.dst, im_dst);

	bb = xe_bo_create(im_xe, 0, im_bb_size, im_reg, 0);
	blt_set_batch(&im_blt.bb, bb, im_bb_size, im_reg);

	blt_fast_copy(im_xe, ctx, NULL, ahnd, &im_blt);

	result = memcmp(dst->ptr, im_blt.src.ptr, src->size);

	put_offset(ahnd, im_dst->handle);
	put_offset(ahnd, dst->handle);
	put_offset(ahnd, bb);
	intel_allocator_bind(ahnd, 0, 0);
	blt_destroy_object(im_xe, src);
	blt_destroy_object(im_xe, im_dst);
	blt_destroy_object(ex_xe, dst);
	put_ahnd(ahnd);

	igt_assert_f(!result, "source and destination surfaces differs!\n");
}

static const char *p2p_path(int ex_reg, struct gpu_info *ex_gpu, struct gpu_info *im_gpu)
{
	return "-p2p";
}

static char *region_name(int xe, uint32_t region)
{
	char *name;
	struct drm_xe_mem_region *memreg;
	int r;
	int len = 7;

	/* enough for "name%d" * n */
	name = malloc(len);
	igt_assert(name);

	memreg = xe_mem_region(xe, region);

	if (XE_IS_CLASS_VRAM(memreg))
		r = snprintf(name, len, "%s%d",
			     xe_region_name(region),
			     memreg->instance);
	else
		r = snprintf(name, len, "%s",
			     xe_region_name(region));

	igt_assert_lt(0, r);

	return name;
}

/**
 * gpu_read - Set up a read from the exporting GPU to the importing GPU
 * @ex_gpu: GPU that is exporting a buffer for read
 * @im_gpu: GPU that is importing and reading the buffer
 */
static void gpu_read(struct gpu_info *ex_gpu, struct gpu_info *im_gpu)
{
	struct igt_collection *ex_regs, *im_regs;
	int ex_reg, im_reg;
	char *ex_name, *im_name;
	const char *path;

	for_each_variation_r(ex_regs, 1, ex_gpu->set) {
		ex_reg = igt_collection_get_value(ex_regs, 0);
		ex_name = region_name(ex_gpu->fd, ex_reg);

		for_each_variation_r(im_regs, 1, im_gpu->set) {
			im_reg = igt_collection_get_value(im_regs, 0);
			im_name = region_name(im_gpu->fd, im_reg);

			path = p2p_path(ex_reg, ex_gpu, im_gpu);
			igt_dynamic_f("read-gpuA-%s-gpuB-%s%s", ex_name,
				      im_name, path)
			test_read(ex_gpu, im_gpu, ex_reg, im_reg);

			free(im_name);
		}
		free(ex_name);
	}
}

/**
 * gpu_write - Set up a write from the importing GPU to the exporting GPU
 * @ex_gpu: GPU that is exporting a buffer for read
 * @im_gpu: GPU that is importing and reading the buffer
 */
static void gpu_write(struct gpu_info *ex_gpu, struct gpu_info *im_gpu)
{
	struct igt_collection *ex_regs, *im_regs;
	int ex_reg, im_reg;
	char *ex_name, *im_name;
	const char *path;

	for_each_variation_r(ex_regs, 1, ex_gpu->set) {
		ex_reg = igt_collection_get_value(ex_regs, 0);
		ex_name = region_name(ex_gpu->fd, ex_reg);

		for_each_variation_r(im_regs, 1, im_gpu->set) {
			im_reg = igt_collection_get_value(im_regs, 0);
			im_name = region_name(im_gpu->fd, im_reg);

			path = p2p_path(ex_reg, ex_gpu, im_gpu);
			igt_dynamic_f("write-gpuA-%s-gpuB-%s%s", ex_name,
				      im_name, path)
			test_write(ex_gpu, im_gpu, ex_reg, im_reg);

			free(im_name);
		}
		free(ex_name);
	}
}

#define DEFAULT_SIZE 0

int igt_main_args("", NULL, NULL, NULL, NULL)
{
	struct gpu_info gpus[2];
	int gpu_cnt;

	igt_fixture() {
		gpu_cnt = get_device_info(gpus, ARRAY_SIZE(gpus));
		igt_skip_on(gpu_cnt < 2);
	}

	igt_describe("dmabuf gpu-gpu read");
	igt_subtest_with_dynamic_f("read")
		gpu_read(&gpus[0], &gpus[1]);

	igt_describe("dmabuf gpu-gpu write");
	igt_subtest_with_dynamic_f("write")
		gpu_write(&gpus[0], &gpus[1]);

	igt_fixture() {
		int cnt;

		for (cnt = 0; cnt < gpu_cnt; cnt++)
			drm_close_driver(gpus[cnt].fd);
	}
}
