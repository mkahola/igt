// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

#include "igt.h"
#include "drm.h"
#include "lib/intel_chipset.h"
#include "intel_blt.h"
#include "intel_mocs.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"
#include "xe/xe_util.h"

/**
 * TEST: xe exercise blt
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: Blitter tests
 * Functionality: flat_ccs
 * Description: Exercise blitter commands on Xe
 * Test category: functionality test
 *
 * SUBTEST: fast-copy
 * Description:
 *   Check fast-copy blit
 *   blitter
 *
 * SUBTEST: fast-copy-inc-dimension
 * Description:
 *   Check fast-copy blit with sizes from small to large
 *
 * SUBTEST: fast-copy-emit
 * Description:
 *   Check multiple fast-copy in one batch
 *   blitter
 */

IGT_TEST_DESCRIPTION("Exercise blitter commands on Xe");

static struct param {
	int tiling;
	bool write_png;
	bool print_bb;
	bool print_surface_info;
	int width;
	int height;
	int width_increment;
	int width_steps;
} param = {
	.tiling = -1,
	.write_png = false,
	.print_bb = false,
	.print_surface_info = false,
	.width = 512,
	.height = 512,
};

#define PRINT_SURFACE_INFO(name, obj) do { \
	if (param.print_surface_info) \
		blt_surface_info((name), (obj)); } while (0)

#define WRITE_PNG(fd, id, name, obj, w, h, bpp) do { \
	if (param.write_png) \
		blt_surface_to_png((fd), (id), (name), (obj), (w), (h), (bpp)); } while (0)

struct blt_fast_copy_data {
	int xe;
	struct blt_copy_object src;
	struct blt_copy_object mid;
	struct blt_copy_object dst;

	struct blt_copy_batch bb;
	enum blt_color_depth color_depth;

	/* debug stuff */
	bool print_bb;
};

static int fast_copy_one_bb(int xe,
			    const intel_ctx_t *ctx,
			    uint64_t ahnd,
			    const struct blt_fast_copy_data *blt)
{
	struct blt_copy_data blt_tmp;
	uint64_t bb_offset, alignment;
	uint64_t bb_pos = 0;
	int ret = 0;

	alignment = xe_get_default_alignment(xe);

	get_offset(ahnd, blt->src.handle, blt->src.size, alignment);
	get_offset(ahnd, blt->mid.handle, blt->mid.size, alignment);
	get_offset(ahnd, blt->dst.handle, blt->dst.size, alignment);
	bb_offset = get_offset(ahnd, blt->bb.handle, blt->bb.size, alignment);

	/* First blit */
	blt_copy_init(xe, &blt_tmp);
	blt_tmp.src = blt->src;
	blt_tmp.dst = blt->mid;
	blt_tmp.bb = blt->bb;
	blt_tmp.color_depth = blt->color_depth;
	blt_tmp.print_bb = blt->print_bb;
	bb_pos = emit_blt_fast_copy(xe, ahnd, &blt_tmp, bb_pos, false);

	/*
	 * Bspec asks to insert a flush between two BLIT instructions to resolve
	 * BLIT level dependency. Though the spec is not clear in explaining
	 * it, HW team requested for a flush instuction between these two BLIT
	 * instuctions as they have a dependency.
	 */
	bb_pos = emit_xe_flush_dw(xe, &blt_tmp, bb_pos);

	/* Second blit */
	blt_copy_init(xe, &blt_tmp);
	blt_tmp.src = blt->mid;
	blt_tmp.dst = blt->dst;
	blt_tmp.bb = blt->bb;
	blt_tmp.color_depth = blt->color_depth;
	blt_tmp.print_bb = blt->print_bb;
	bb_pos = emit_blt_fast_copy(xe, ahnd, &blt_tmp, bb_pos, true);

	intel_ctx_xe_exec(ctx, ahnd, bb_offset);

	return ret;
}

static void fast_copy_emit(int xe, const intel_ctx_t *ctx,
			   uint32_t width, uint32_t height,
			   uint32_t region1, uint32_t region2,
			   enum blt_tiling_type mid_tiling)
{
	struct blt_copy_data bltinit = {};
	struct blt_fast_copy_data blt = {};
	struct blt_copy_object *src, *mid, *dst;
	const uint32_t bpp = 32;
	uint64_t bb_size = xe_bb_size(xe, SZ_4K);
	uint64_t ahnd = intel_allocator_open_full(xe, ctx->vm, 0, 0,
						  INTEL_ALLOCATOR_SIMPLE,
						  ALLOC_STRATEGY_LOW_TO_HIGH, 0);
	uint32_t bb;
	int result;

	bb = xe_bo_create(xe, 0, bb_size, region1, DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);

	blt_copy_init(xe, &bltinit);
	src = blt_create_object(&bltinit, region1, width, height, bpp, 0,
				T_LINEAR, COMPRESSION_DISABLED, 0, true);
	mid = blt_create_object(&bltinit, region2, width, height, bpp, 0,
				mid_tiling, COMPRESSION_DISABLED, 0, true);
	dst = blt_create_object(&bltinit, region1, width, height, bpp, 0,
				T_LINEAR, COMPRESSION_DISABLED, 0, true);
	igt_assert(src->size == dst->size);

	PRINT_SURFACE_INFO("src", src);
	PRINT_SURFACE_INFO("mid", mid);
	PRINT_SURFACE_INFO("dst", dst);

	blt_surface_fill_rect(xe, src, width, height);
	WRITE_PNG(xe, mid_tiling, "src", src, width, height, bpp);

	memset(&blt, 0, sizeof(blt));
	blt.color_depth = CD_32bit;
	blt.print_bb = param.print_bb;
	blt_set_copy_object(&blt.src, src);
	blt_set_copy_object(&blt.mid, mid);
	blt_set_copy_object(&blt.dst, dst);
	blt_set_batch(&blt.bb, bb, bb_size, region1);

	fast_copy_one_bb(xe, ctx, ahnd, &blt);

	WRITE_PNG(xe, mid_tiling, "mid", &blt.mid, width, height, bpp);
	WRITE_PNG(xe, mid_tiling, "dst", &blt.dst, width, height, bpp);

	result = memcmp(src->ptr, blt.dst.ptr, src->size);

	blt_destroy_object(xe, src);
	blt_destroy_object(xe, mid);
	blt_destroy_object(xe, dst);
	gem_close(xe, bb);
	put_ahnd(ahnd);

	munmap(&bb, bb_size);

	igt_assert_f(!result, "source and destination surfaces differs!\n");
}

static void fast_copy(int xe, const intel_ctx_t *ctx,
		      uint32_t width, uint32_t height,
		      uint32_t region1, uint32_t region2,
		      enum blt_tiling_type mid_tiling)
{
	struct blt_copy_data blt = {};
	struct blt_copy_object *src, *mid, *dst;
	const uint32_t bpp = 32;
	uint64_t bb_size = xe_bb_size(xe, SZ_4K);
	uint64_t ahnd = intel_allocator_open_full(xe, ctx->vm, 0, 0,
						  INTEL_ALLOCATOR_SIMPLE,
						  ALLOC_STRATEGY_LOW_TO_HIGH, 0);
	uint32_t bb;
	int result;

	bb = xe_bo_create(xe, 0, bb_size, region1, DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);

	blt_copy_init(xe, &blt);
	src = blt_create_object(&blt, region1, width, height, bpp, 0,
				T_LINEAR, COMPRESSION_DISABLED, 0, true);
	mid = blt_create_object(&blt, region2, width, height, bpp, 0,
				mid_tiling, COMPRESSION_DISABLED, 0, true);
	dst = blt_create_object(&blt, region1, width, height, bpp, 0,
				T_LINEAR, COMPRESSION_DISABLED, 0, true);
	igt_assert(src->size == dst->size);

	blt_surface_fill_rect(xe, src, width, height);

	blt.color_depth = CD_32bit;
	blt.print_bb = param.print_bb;
	blt_set_copy_object(&blt.src, src);
	blt_set_copy_object(&blt.dst, mid);
	blt_set_batch(&blt.bb, bb, bb_size, region1);

	blt_fast_copy(xe, ctx, NULL, ahnd, &blt);

	WRITE_PNG(xe, mid_tiling, "src", &blt.src, width, height, bpp);
	WRITE_PNG(xe, mid_tiling, "mid", &blt.dst, width, height, bpp);

	blt_copy_init(xe, &blt);
	blt.color_depth = CD_32bit;
	blt.print_bb = param.print_bb;
	blt_set_copy_object(&blt.src, mid);
	blt_set_copy_object(&blt.dst, dst);
	blt_set_batch(&blt.bb, bb, bb_size, region1);

	blt_fast_copy(xe, ctx, NULL, ahnd, &blt);

	WRITE_PNG(xe, mid_tiling, "dst", &blt.dst, width, height, bpp);

	result = memcmp(src->ptr, blt.dst.ptr, src->size);

	put_offset(ahnd, src->handle);
	put_offset(ahnd, mid->handle);
	put_offset(ahnd, dst->handle);
	put_offset(ahnd, bb);
	intel_allocator_bind(ahnd, 0, 0);
	blt_destroy_object(xe, src);
	blt_destroy_object(xe, mid);
	blt_destroy_object(xe, dst);
	gem_close(xe, bb);
	put_ahnd(ahnd);

	igt_assert_f(!result, "source and destination surfaces differs!\n");
}

enum fast_copy_func {
	FAST_COPY,
	FAST_COPY_EMIT
};

static char
	*full_subtest_str(char *regtxt, uint32_t width, uint32_t height,
			  enum blt_tiling_type tiling,
			  enum fast_copy_func func)
{
	char *name;
	uint32_t len;

	if (!width || !height)
		len = asprintf(&name, "%s-%s%s", blt_tiling_name(tiling), regtxt,
			       func == FAST_COPY_EMIT ? "-emit" : "");
	else
		len = asprintf(&name, "%s-%s%s-%ux%u", blt_tiling_name(tiling), regtxt,
			       func == FAST_COPY_EMIT ? "-emit" : "",
			       width, height);

	igt_assert_f(len >= 0, "asprintf failed!\n");

	return name;
}

static void fast_copy_test(int xe,
			   struct igt_collection *set,
			   enum fast_copy_func func)
{
	struct drm_xe_engine_class_instance inst = {
		.engine_class = DRM_XE_ENGINE_CLASS_COPY,
	};
	struct igt_collection *regions;
	void (*copy_func)(int xe, const intel_ctx_t *ctx,
			  uint32_t width, uint32_t height,
			  uint32_t r1, uint32_t r2, enum blt_tiling_type tiling);
	intel_ctx_t *ctx;
	int tiling;

	for_each_tiling(tiling) {
		if (!blt_fast_copy_supports_tiling(xe, tiling))
			continue;

		for_each_variation_r(regions, 2, set) {
			uint32_t region1, region2;
			uint32_t vm, exec_queue;
			char *regtxt, *test_name;

			region1 = igt_collection_get_value(regions, 0);
			region2 = igt_collection_get_value(regions, 1);

			vm = xe_vm_create(xe, 0, 0);
			exec_queue = xe_exec_queue_create(xe, vm, &inst, 0);
			ctx = intel_ctx_xe(xe, vm, exec_queue, 0, 0, 0);

			copy_func = (func == FAST_COPY) ? fast_copy : fast_copy_emit;
			regtxt = xe_memregion_dynamic_subtest_name(xe, regions);

			if (!param.width_increment) {
				test_name = full_subtest_str(regtxt, 0, 0, tiling, func);
				igt_dynamic_f("%s", test_name) {
					copy_func(xe, ctx,
						  param.width, param.height,
						  region1, region2,
						  tiling);
				}
				free(test_name);
			} else {
				for (int w = param.width;
				     w < param.width + param.width_steps;
				     w += param.width_increment) {
					test_name = full_subtest_str(regtxt, w, w, tiling, func);
					igt_dynamic_f("%s", test_name) {
						copy_func(xe, ctx,
							  w, w,
							  region1, region2,
							  tiling);
					}
					free(test_name);
				}
			}

			free(regtxt);
			xe_exec_queue_destroy(xe, exec_queue);
			xe_vm_destroy(xe, vm);
			free(ctx);
		}
	}
}

static int opt_handler(int opt, int opt_index, void *data)
{
	switch (opt) {
	case 'b':
		param.print_bb = true;
		igt_debug("Print bb: %d\n", param.print_bb);
		break;
	case 'p':
		param.write_png = true;
		igt_debug("Write png: %d\n", param.write_png);
		break;
	case 's':
		param.print_surface_info = true;
		igt_debug("Print surface info: %d\n", param.print_surface_info);
		break;
	case 't':
		param.tiling = atoi(optarg);
		igt_debug("Tiling: %d\n", param.tiling);
		break;
	case 'W':
		param.width = atoi(optarg);
		igt_debug("Width: %d\n", param.width);
		break;
	case 'H':
		param.height = atoi(optarg);
		igt_debug("Height: %d\n", param.height);
		break;
	default:
		return IGT_OPT_HANDLER_ERROR;
	}

	return IGT_OPT_HANDLER_SUCCESS;
}

const char *help_str =
	"  -b\tPrint bb\n"
	"  -p\tWrite PNG\n"
	"  -s\tPrint surface info\n"
	"  -t\tTiling format (0 - linear, 1 - XMAJOR, 2 - YMAJOR, 3 - TILE4, 4 - TILE64, 5 - YFMAJOR)\n"
	"  -W\tWidth (default 512)\n"
	"  -H\tHeight (default 512)"
	;

int igt_main_args("b:pst:W:H:", NULL, help_str, opt_handler, NULL)
{
	struct igt_collection *set;
	int xe;

	igt_fixture() {
		xe = drm_open_driver(DRIVER_XE);
		igt_require(blt_has_fast_copy(xe));

		xe_device_get(xe);

		set = xe_get_memory_region_set(xe,
					       DRM_XE_MEM_REGION_CLASS_SYSMEM,
					       DRM_XE_MEM_REGION_CLASS_VRAM);
	}

	igt_describe("Check fast-copy blit");
	igt_subtest_with_dynamic("fast-copy") {
		fast_copy_test(xe, set, FAST_COPY);
	}

	igt_describe("Check fast-copy with increment width/height");
	igt_subtest_with_dynamic("fast-copy-inc-dimension") {
		param.width = 1;
		param.height = 1;
		param.width_increment = 15;
		param.width_steps = 512;

		fast_copy_test(xe, set, FAST_COPY);
	}

	igt_describe("Check multiple fast-copy in one batch");
	igt_subtest_with_dynamic("fast-copy-emit") {
		fast_copy_test(xe, set, FAST_COPY_EMIT);
	}

	igt_fixture() {
		drm_close_driver(xe);
	}
}
