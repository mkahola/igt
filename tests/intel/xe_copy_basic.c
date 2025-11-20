// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 *
 * Authors:
 *      Sai Gowtham Ch <sai.gowtham.ch@intel.com>
 */

#include "igt.h"
#include "lib/igt_syncobj.h"
#include "intel_blt.h"
#include "lib/intel_cmds_info.h"
#include "lib/intel_mocs.h"
#include "lib/intel_pat.h"
#include "lib/intel_reg.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"
#include "xe/xe_util.h"

#define MEM_FILL 0x8b

static struct param {
	bool print_bb;
} param = {
	.print_bb = false,
};

struct rect {
	uint32_t pitch;
	uint32_t width;
	uint32_t height;
	enum blt_memop_mode mode;
};

/**
 * TEST: Test to validate copy commands on xe
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: Blitter tests
 * Functionality: copy
 */

/**
 * SUBTEST: mem-copy-linear-%s
 * Description: Test validates MEM_COPY command, it takes various
 *              parameters needed for the filling batch buffer for MEM_COPY command
 *              with size %arg[1].
 * Test category: functionality test
 *
 * arg[1]:
 * @0x369: 0x369
 * @0x3fff: 0x3fff
 * @0xfd: 0xfd
 * @0xfffe: 0xfffe
 * @0x8fffe: 0x8fffe
 */

/**
 *
 * SUBTEST: mem-page-copy-%s
 * Description: Test validates MEM_COPY command in page mode (256B chunks).
 *              Size %arg[1] means number of pages copied.
 * Test category: functionality test
 *
 * arg[1]:
 * @1: 1
 * @17: 17
 */

/**
 *
 * SUBTEST: mem-matrix-copy-%s
 * Description: Test validates MEM_COPY command in matrix type.
 *              Size %arg[1] represents width x height.
 * Test category: functionality test
 *
 * arg[1]:
 * @2x2: 2x2
 * @200x127: 200x127
 */

static void
mem_copy(int fd, uint32_t src_handle, uint32_t dst_handle, const intel_ctx_t *ctx,
	 enum blt_memop_type type, enum blt_memop_mode mode,
	 uint32_t size, uint32_t pitch, uint32_t width, uint32_t height, uint32_t region)
{
	struct blt_mem_copy_data mem = {};
	uint64_t bb_size = xe_bb_size(fd, SZ_4K);
	uint64_t ahnd = intel_allocator_open_full(fd, ctx->vm, 0, 0,
						  INTEL_ALLOCATOR_SIMPLE,
						  ALLOC_STRATEGY_LOW_TO_HIGH, 0);
	uint8_t src_mocs = intel_get_uc_mocs_index(fd);
	uint8_t dst_mocs = src_mocs;
	uint32_t bb;
	uint8_t *psrc, *pdst;
	int result, i;

	igt_debug("size: %u, pitch: %u, width: %u, height: %u (type: %d, mode: %d)\n",
		  size, pitch, width, height, type, mode);

	bb = xe_bo_create(fd, 0, bb_size, region, DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);

	blt_mem_copy_init(fd, &mem, mode, type);
	mem.print_bb = param.print_bb;

	blt_set_mem_object(&mem.src, src_handle, size, pitch, width, height,
			   region, src_mocs, DEFAULT_PAT_INDEX, COMPRESSION_DISABLED);
	blt_set_mem_object(&mem.dst, dst_handle, size, pitch, width, height,
			   region, dst_mocs, DEFAULT_PAT_INDEX, COMPRESSION_DISABLED);

	mem.src.ptr = xe_bo_map(fd, src_handle, size);
	mem.dst.ptr = xe_bo_map(fd, dst_handle, size);
	psrc = (uint8_t *) mem.src.ptr;
	pdst = (uint8_t *) mem.dst.ptr;

	srand(time(NULL));

	/* Randomize whole src */
	for (i = 0; i < size; i++)
		psrc[i] = rand();

	blt_set_batch(&mem.bb, bb, bb_size, region);
	igt_assert(mem.src.width == mem.dst.width);

	blt_mem_copy(fd, ctx, NULL, ahnd, &mem);

	if (type == TYPE_LINEAR && mode == MODE_BYTE) {
		result = memcmp(psrc, pdst, width);

		/* Rest of dst must contain 0 */
		for (i = width; i < size; i++) {
			if (pdst[i] != 0) {
				result = -1;
				break;
			}
		}
	} else if (type == TYPE_LINEAR && mode == MODE_PAGE) {
		result = memcmp(psrc, pdst, pitch << 8);
	} else {
		result = 0;

		for (i = 0; i < pitch * height; i++) {
			if (i % pitch > width && pdst[i] != 0) {
				result = -1;
				break;
			} else if (i % pitch < width && psrc[i] != pdst[i]) {
				result = -1;
				break;
			}
		}
	}

	intel_allocator_bind(ahnd, 0, 0);
	munmap(mem.src.ptr, size);
	munmap(mem.dst.ptr, size);
	gem_close(fd, bb);
	put_ahnd(ahnd);

	igt_assert_f(!result, "destination doesn't contain valid data\n");
}

/**
 * SUBTEST: mem-set-linear-%s
 * Description: Test validates MEM_SET command with size %arg[1].
 * Test category: functionality test
 *
 * arg[1]:
 *
 * @0x369: 0x369
 * @0x3fff: 0x3fff
 * @0xfd: 0xfd
 * @0xfffe: 0xfffe
 * @0x8fffe: 0x8fffe
 */
static void
mem_set(int fd, uint32_t dst_handle, const intel_ctx_t *ctx, uint32_t size,
	uint32_t width, uint32_t height, uint8_t fill_data, uint32_t region)
{
	struct blt_mem_set_data mem = {};
	uint64_t bb_size = xe_bb_size(fd, SZ_4K);
	uint64_t ahnd = intel_allocator_open_full(fd, ctx->vm, 0, 0,
						  INTEL_ALLOCATOR_SIMPLE,
						  ALLOC_STRATEGY_LOW_TO_HIGH, 0);
	uint8_t dst_mocs = intel_get_uc_mocs_index(fd);
	uint32_t bb;
	uint8_t *result;

	bb = xe_bo_create(fd, 0, bb_size, region, DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	blt_mem_set_init(fd, &mem, TYPE_LINEAR);
	blt_set_mem_object(&mem.dst, dst_handle, size, width, width, height, region,
			   dst_mocs, DEFAULT_PAT_INDEX, COMPRESSION_DISABLED);
	mem.dst.ptr = xe_bo_map(fd, dst_handle, size);
	blt_set_batch(&mem.bb, bb, bb_size, region);
	blt_mem_set(fd, ctx, NULL, ahnd, &mem, fill_data);

	result = (uint8_t *)mem.dst.ptr;

	intel_allocator_bind(ahnd, 0, 0);
	gem_close(fd, bb);
	put_ahnd(ahnd);

	igt_assert(result[0] == fill_data);
	igt_assert(result[width - 1] == fill_data);
	igt_assert(result[width] != fill_data);

	munmap(mem.dst.ptr, size);
}

static void copy_test(int fd, struct rect *rect, enum blt_cmd_type cmd, uint32_t region)
{
	struct drm_xe_engine_class_instance inst = {
		.engine_class = DRM_XE_ENGINE_CLASS_COPY,
	};
	uint32_t src_handle, dst_handle, vm, exec_queue;
	uint32_t pitch = rect->pitch ?: rect->width;
	uint32_t blocksize = rect->mode == MODE_PAGE ? pitch << 8 : pitch;
	uint32_t bo_size = ALIGN(blocksize * rect->height, xe_get_default_alignment(fd));
	intel_ctx_t *ctx;

	src_handle = xe_bo_create(fd, 0, bo_size, region,
				  DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	dst_handle = xe_bo_create(fd, 0, bo_size, region,
				  DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	vm = xe_vm_create(fd, 0, 0);
	exec_queue = xe_exec_queue_create(fd, vm, &inst, 0);
	ctx = intel_ctx_xe(fd, vm, exec_queue, 0, 0, 0);

	if (cmd == MEM_COPY)
		mem_copy(fd, src_handle, dst_handle, ctx,
			 rect->height > 1 ? TYPE_MATRIX : TYPE_LINEAR,
			 rect->mode, bo_size, pitch,
			 rect->width, rect->height, region);
	else if (cmd == MEM_SET)
		mem_set(fd, dst_handle, ctx, bo_size, rect->width, 1, MEM_FILL, region);

	gem_close(fd, src_handle);
	gem_close(fd, dst_handle);
	xe_exec_queue_destroy(fd, exec_queue);
	xe_vm_destroy(fd, vm);
	free(ctx);
}

static int opt_handler(int opt, int opt_index, void *data)
{
	switch (opt) {
	case 'b':
		param.print_bb = true;
		igt_debug("Print bb: %d\n", param.print_bb);
		break;
	default:
		return IGT_OPT_HANDLER_ERROR;
	}

	return IGT_OPT_HANDLER_SUCCESS;
}

const char *help_str =
	"  -b\tPrint bb"
	;

igt_main_args("b", NULL, help_str, opt_handler, NULL)
{
	int fd;
	uint16_t dev_id;
	struct igt_collection *set, *regions;
	uint32_t region;
	struct rect linear[] = { { 0, 0xfd, 1, MODE_BYTE },
				 { 0, 0x369, 1, MODE_BYTE },
				 { 0, 0x3fff, 1, MODE_BYTE },
				 { 0, 0xfffe, 1, MODE_BYTE },
				 { 0, 0x8fffe, 1, MODE_BYTE } };
	struct rect page[] = { { 0, 1, 1, MODE_PAGE },
			       { 0, 17, 1, MODE_PAGE }};
	struct rect matrix[] = { { 4, 2, 2 }, { 256, 200, 127 } };

	igt_fixture() {
		fd = drm_open_driver(DRIVER_XE);
		dev_id = intel_get_drm_devid(fd);
		xe_device_get(fd);
		set = xe_get_memory_region_set(fd,
					       DRM_XE_MEM_REGION_CLASS_SYSMEM,
					       DRM_XE_MEM_REGION_CLASS_VRAM);
	}

	for (int i = 0; i < ARRAY_SIZE(linear); i++) {
		igt_subtest_f("mem-copy-linear-0x%x", linear[i].width) {
			igt_require(blt_has_mem_copy(fd));
			for_each_variation_r(regions, 1, set) {
				region = igt_collection_get_value(regions, 0);
				copy_test(fd, &linear[i], MEM_COPY, region);
			}
		}
	}

	for (int i = 0; i < ARRAY_SIZE(page); i++) {
		igt_subtest_f("mem-page-copy-%u", page[i].width) {
			igt_require(blt_has_mem_copy(fd));
			igt_require(intel_get_device_info(dev_id)->graphics_ver >= 20);
			for_each_variation_r(regions, 1, set) {
				region = igt_collection_get_value(regions, 0);
				copy_test(fd, &page[i], MEM_COPY, region);
			}
		}
	}

	for (int i = 0; i < ARRAY_SIZE(matrix); i++) {
		igt_subtest_f("mem-matrix-copy-%ux%u", matrix[i].width, matrix[i].height) {
			igt_require(blt_has_mem_copy(fd));
			for_each_variation_r(regions, 1, set) {
				region = igt_collection_get_value(regions, 0);
				copy_test(fd, &matrix[i], MEM_COPY, region);
			}
		}
	}

	for (int i = 0; i < ARRAY_SIZE(linear); i++) {
		igt_subtest_f("mem-set-linear-0x%x", linear[i].width) {
			/* Skip mem-set-linear test for values greater than
			 * 0x3FFFF. As hardware with graphics_ver<20 support
			 * till 0x3FFFF.
			 */
			if (linear[i].width > 0x3ffff &&
			    (intel_get_device_info(dev_id)->graphics_ver < 20))
				igt_skip("Skipping: width exceeds 18-bit limit on gfx_ver < 20\n");
			igt_require(blt_has_mem_set(fd));
			for_each_variation_r(regions, 1, set) {
				region = igt_collection_get_value(regions, 0);
				copy_test(fd, &linear[i], MEM_SET, region);
			}
		}
	}

	igt_fixture() {
		drm_close_driver(fd);
	}
}
