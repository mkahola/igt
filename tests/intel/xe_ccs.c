// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

#include <errno.h>
#ifdef ANDROID
#include "android/glib.h"
#else
#include <glib.h>
#endif
#include <sys/ioctl.h>
#include <sys/time.h>
#include <malloc.h>
#include "drm.h"
#include "igt.h"
#include "igt_syncobj.h"
#include "intel_blt.h"
#include "intel_common.h"
#include "intel_mocs.h"
#include "intel_pat.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"
#include "xe/xe_util.h"
/**
 * TEST: xe ccs
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: Blitter tests
 * Functionality: flat_ccs
 * Description: Exercise gen12 blitter with and without flatccs compression on Xe
 * Test category: functionality test
 *
 * SUBTEST: block-copy-compressed
 * Description: Check block-copy flatccs compressed blit
 *
 * SUBTEST: block-copy-compressed-inc-dimension
 * Description: Check block-copy compressed blit for different sizes
 *
 * SUBTEST: block-copy-uncompressed
 * Description: Check block-copy uncompressed blit
 *
 * SUBTEST: block-copy-uncompressed-inc-dimension
 * Description: Check block-copy uncompressed blit for different sizes
 *
 * SUBTEST: block-multicopy-compressed
 * Description: Check block-multicopy flatccs compressed blit
 *
 * SUBTEST: block-multicopy-inplace
 * Description: Check block-multicopy flatccs inplace decompression blit
 *
 * SUBTEST: ctrl-surf-copy
 * Description: Check flatccs data can be copied from/to surface
 *
 * SUBTEST: ctrl-surf-copy-new-ctx
 * Description: Check flatccs data are physically tagged and visible in vm
 *
 * SUBTEST: large-ctrl-surf-copy
 * Description: Check flatccs data can be copied from large surface
 *
 * SUBTEST: suspend-resume
 * Description: Check flatccs data persists after suspend / resume (S0)
 */

IGT_TEST_DESCRIPTION("Exercise gen12 blitter with and without flatccs compression on Xe");

static struct param {
	int compression_format;
	int tiling;
	bool write_png;
	bool print_bb;
	bool print_surface_info;
	int width;
	int height;
	int incdim_width;
} param = {
	.compression_format = 0,
	.tiling = -1,
	.write_png = false,
	.print_bb = false,
	.print_surface_info = false,
	.width = 512,
	.height = 512,
	.incdim_width = 1,
};

struct test_config {
	bool compression;
	bool inplace;
	bool surfcopy;
	bool new_ctx;
	bool suspend_resume;
	int width_increment;
	int width_steps;
	int overwrite_width;
	int overwrite_height;
};

#define PRINT_SURFACE_INFO(name, obj) do { \
	if (param.print_surface_info) \
		blt_surface_info((name), (obj)); } while (0)

#define WRITE_PNG(fd, id, name, obj, w, h, bpp) do { \
	if (param.write_png) \
		blt_surface_to_png((fd), (id), (name), (obj), (w), (h), (bpp)); } while (0)

static void surf_copy(int xe,
		      intel_ctx_t *ctx,
		      uint64_t ahnd,
		      const struct blt_copy_object *src,
		      const struct blt_copy_object *mid,
		      const struct blt_copy_object *dst,
		      int run_id, bool suspend_resume)
{
	struct blt_copy_data blt = {};
	struct blt_block_copy_data_ext ext = {};
	struct blt_ctrl_surf_copy_data surf = {};
	const uint32_t bpp = 32;
	uint32_t bb1, bb2, ccs, ccs2, *ccsmap, *ccsmap2;
	uint64_t bb_size, ccssize = mid->size / CCS_RATIO(xe);
	uint64_t ccs_bo_size = ALIGN(ccssize, xe_get_default_alignment(xe));
	uint32_t *ccscopy;
	uint8_t uc_mocs = intel_get_uc_mocs_index(xe);
	uint32_t sysmem = system_memory(xe);
	uint8_t comp_pat_index = DEFAULT_PAT_INDEX;
	uint16_t cpu_caching = __xe_default_cpu_caching(xe, sysmem, 0);
	uint32_t devid = intel_get_drm_devid(xe);
	int result;

	igt_assert(mid->compression);
	if (intel_gen(devid) >= 20 && mid->compression) {
		comp_pat_index  = intel_get_pat_idx_uc_comp(xe);
		cpu_caching = DRM_XE_GEM_CPU_CACHING_WC;
	}
	ccscopy = (uint32_t *) malloc(ccssize);
	ccs = xe_bo_create_caching(xe, 0, ccs_bo_size, sysmem, 0, cpu_caching);
	ccs2 = xe_bo_create_caching(xe, 0, ccs_bo_size, sysmem, 0, cpu_caching);

	blt_ctrl_surf_copy_init(xe, &surf);
	surf.print_bb = param.print_bb;
	blt_set_ctrl_surf_object(&surf.src, mid->handle, mid->region, mid->size,
				 uc_mocs, comp_pat_index, BLT_INDIRECT_ACCESS);
	blt_set_ctrl_surf_object(&surf.dst, ccs, sysmem, ccssize, uc_mocs,
				 DEFAULT_PAT_INDEX, DIRECT_ACCESS);
	bb_size = xe_bb_size(xe, SZ_4K);
	bb1 = xe_bo_create(xe, 0, bb_size, sysmem, 0);
	blt_set_batch(&surf.bb, bb1, bb_size, sysmem);
	blt_ctrl_surf_copy(xe, ctx, NULL, ahnd, &surf);
	intel_ctx_xe_sync(ctx, true);

	ccsmap = xe_bo_map(xe, ccs, surf.dst.size);
	memcpy(ccscopy, ccsmap, ccssize);

	if (suspend_resume) {
		char *orig, *orig2, *newsum, *newsum2;

		orig = g_compute_checksum_for_data(G_CHECKSUM_SHA1,
						   (void *)ccsmap, surf.dst.size);
		orig2 = g_compute_checksum_for_data(G_CHECKSUM_SHA1,
						    (void *)mid->ptr, mid->size);

		igt_system_suspend_autoresume(SUSPEND_STATE_FREEZE, SUSPEND_TEST_NONE);

		blt_set_ctrl_surf_object(&surf.dst, ccs2, system_memory(xe), ccssize,
					 0, DEFAULT_PAT_INDEX, DIRECT_ACCESS);
		blt_ctrl_surf_copy(xe, ctx, NULL, ahnd, &surf);
		intel_ctx_xe_sync(ctx, true);

		ccsmap2 = xe_bo_map(xe, ccs2, surf.dst.size);
		newsum = g_compute_checksum_for_data(G_CHECKSUM_SHA1,
						     (void *)ccsmap2, surf.dst.size);
		newsum2 = g_compute_checksum_for_data(G_CHECKSUM_SHA1,
						      (void *)mid->ptr, mid->size);

		munmap(ccsmap2, ccssize);
		if (blt_platform_has_flat_ccs_enabled(xe)) {
			if (IS_GEN(devid, 12) && is_intel_dgfx(xe)) {
				igt_assert(!strcmp(orig, newsum));
				igt_assert(!strcmp(orig2, newsum2));
			} else if (intel_gen(devid) >= 20) {
				if (is_intel_dgfx(xe)) {
					/* buffer object would become
					 * uncompressed in xe2+ dgfx
					 */
					igt_assert(!blt_surface_is_compressed(xe, ctx,
							NULL, ahnd, mid));
				} else {
					/* ccs should be present in xe2+ igfx */
					igt_assert(blt_surface_is_compressed(xe, ctx,
							NULL, ahnd, mid));
				}
			}
		}
		g_free(orig);
		g_free(orig2);
		g_free(newsum);
		g_free(newsum2);
	}

	/* corrupt ccs */
	for (int i = 0; i < surf.dst.size / sizeof(uint32_t); i++)
		ccsmap[i] = i;
	blt_set_ctrl_surf_object(&surf.src, ccs, sysmem, ccssize,
				 uc_mocs, DEFAULT_PAT_INDEX, DIRECT_ACCESS);
	blt_set_ctrl_surf_object(&surf.dst, mid->handle, mid->region, mid->size,
				 uc_mocs, comp_pat_index, INDIRECT_ACCESS);
	blt_ctrl_surf_copy(xe, ctx, NULL, ahnd, &surf);
	intel_ctx_xe_sync(ctx, true);

	blt_copy_init(xe, &blt);
	blt.color_depth = CD_32bit;
	blt.print_bb = param.print_bb;
	blt_set_copy_object(&blt.src, mid);
	blt_set_copy_object(&blt.dst, dst);
	blt_set_object_ext(&ext.src, mid->compression_type, mid->x2, mid->y2, SURFACE_TYPE_2D);
	blt_set_object_ext(&ext.dst, 0, dst->x2, dst->y2, SURFACE_TYPE_2D);
	bb2 = xe_bo_create(xe, 0, bb_size, sysmem, 0);
	blt_set_batch(&blt.bb, bb2, bb_size, sysmem);
	blt_block_copy(xe, ctx, NULL, ahnd, &blt, &ext);
	intel_ctx_xe_sync(ctx, true);
	WRITE_PNG(xe, run_id, "corrupted", &blt.dst, dst->x2, dst->y2, bpp);
	result = memcmp(src->ptr, dst->ptr, src->size);
	if (blt_platform_has_flat_ccs_enabled(xe))
		igt_assert_neq(result, 0);

	/* In case of suspend_resume, buffer object would become
	 * uncompressed in xe2+ dgfx, and therefore retrieve the
	 * ccs by copying 0 to ccsmap
	 */
	if (suspend_resume && intel_gen(devid) >= 20 && is_intel_dgfx(xe))
		memset(ccsmap, 0, ccssize);
	else
		/* retrieve back ccs */
		memcpy(ccsmap, ccscopy, ccssize);
	blt_ctrl_surf_copy(xe, ctx, NULL, ahnd, &surf);

	blt_block_copy(xe, ctx, NULL, ahnd, &blt, &ext);
	intel_ctx_xe_sync(ctx, true);
	WRITE_PNG(xe, run_id, "corrected", &blt.dst, dst->x2, dst->y2, bpp);
	result = memcmp(src->ptr, dst->ptr, src->size);
	if (result)
		blt_dump_corruption_info_32b(src, dst);

	munmap(ccsmap, ccssize);
	gem_close(xe, ccs);
	gem_close(xe, ccs2);
	gem_close(xe, bb1);
	gem_close(xe, bb2);

	igt_assert_f(result == 0,
		     "Source and destination surfaces are different after "
		     "restoring source ccs data\n");
}

struct blt_copy3_data {
	int xe;
	struct blt_copy_object src;
	struct blt_copy_object mid;
	struct blt_copy_object dst;
	struct blt_copy_object final;
	struct blt_copy_batch bb;
	enum blt_color_depth color_depth;

	/* debug stuff */
	bool print_bb;
};

struct blt_block_copy3_data_ext {
	struct blt_block_copy_object_ext src;
	struct blt_block_copy_object_ext mid;
	struct blt_block_copy_object_ext dst;
	struct blt_block_copy_object_ext final;
};

static int blt_block_copy3(int xe,
			   const intel_ctx_t *ctx,
			   uint64_t ahnd,
			   const struct blt_copy3_data *blt3,
			   const struct blt_block_copy3_data_ext *ext3)
{
	struct blt_copy_data blt0;
	struct blt_block_copy_data_ext ext0;
	uint64_t bb_offset, alignment;
	uint64_t bb_pos = 0;
	int ret;

	igt_assert_f(ahnd, "block-copy3 supports softpin only\n");
	igt_assert_f(blt3, "block-copy3 requires data to do blit\n");

	alignment = xe_get_default_alignment(xe);
	get_offset_pat_index(ahnd, blt3->src.handle, blt3->src.size, alignment, blt3->src.pat_index);
	get_offset_pat_index(ahnd, blt3->mid.handle, blt3->mid.size, alignment, blt3->mid.pat_index);
	get_offset_pat_index(ahnd, blt3->dst.handle, blt3->dst.size, alignment, blt3->dst.pat_index);
	get_offset_pat_index(ahnd, blt3->final.handle, blt3->final.size, alignment, blt3->final.pat_index);
	bb_offset = get_offset(ahnd, blt3->bb.handle, blt3->bb.size, alignment);

	/* First blit src -> mid */
	blt_copy_init(xe, &blt0);
	blt0.src = blt3->src;
	blt0.dst = blt3->mid;
	blt0.bb = blt3->bb;
	blt0.color_depth = blt3->color_depth;
	blt0.print_bb = blt3->print_bb;
	ext0.src = ext3->src;
	ext0.dst = ext3->mid;
	bb_pos = emit_blt_block_copy(xe, ahnd, &blt0, &ext0, bb_pos, false);

	/* Second blit mid -> dst */
	blt_copy_init(xe, &blt0);
	blt0.src = blt3->mid;
	blt0.dst = blt3->dst;
	blt0.bb = blt3->bb;
	blt0.color_depth = blt3->color_depth;
	blt0.print_bb = blt3->print_bb;
	ext0.src = ext3->mid;
	ext0.dst = ext3->dst;
	bb_pos = emit_blt_block_copy(xe, ahnd, &blt0, &ext0, bb_pos, false);

	/* Third blit dst -> final */
	blt_copy_init(xe, &blt0);
	blt0.src = blt3->dst;
	blt0.dst = blt3->final;
	blt0.bb = blt3->bb;
	blt0.color_depth = blt3->color_depth;
	blt0.print_bb = blt3->print_bb;
	ext0.src = ext3->dst;
	ext0.dst = ext3->final;
	bb_pos = emit_blt_block_copy(xe, ahnd, &blt0, &ext0, bb_pos, true);

	intel_ctx_xe_exec(ctx, ahnd, bb_offset);

	return ret;
}

#define CHECK_MIN_WIDTH 2
#define CHECK_MIN_HEIGHT 2
#define MIN_EXP_WH(w, h) ((w) >= CHECK_MIN_WIDTH && (h) >= CHECK_MIN_HEIGHT)
#define CHECK_FROM_WIDTH 256
#define CHECK_FROM_HEIGHT 256
#define FROM_EXP_WH(w, h) ((w) >= CHECK_FROM_WIDTH && (h) >= CHECK_FROM_HEIGHT)

static void block_copy(int xe,
		       intel_ctx_t *ctx,
		       uint32_t region1, uint32_t region2,
		       uint32_t width, uint32_t height,
		       enum blt_tiling_type mid_tiling,
		       const struct test_config *config)
{
	struct blt_copy_data blt = {};
	struct blt_block_copy_data_ext ext = {}, *pext = &ext;
	struct blt_copy_object *src, *mid, *dst;
	const uint32_t bpp = 32;
	uint64_t bb_size = xe_bb_size(xe, SZ_4K);
	uint64_t ahnd = intel_allocator_open(xe, ctx->vm, INTEL_ALLOCATOR_RELOC);
	uint32_t run_id = mid_tiling;
	uint32_t mid_region = (intel_gen(intel_get_drm_devid(xe)) >= 20 &&
			       !xe_has_vram(xe)) ? region1 : region2;
	uint32_t bb;
	enum blt_compression mid_compression = config->compression;
	int mid_compression_format = param.compression_format;
	enum blt_compression_type comp_type = COMPRESSION_TYPE_3D;
	uint8_t uc_mocs = intel_get_uc_mocs_index(xe);
	int result;

	bb = xe_bo_create(xe, 0, bb_size, region1, DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);

	if (!blt_uses_extended_block_copy(xe))
		pext = NULL;

	blt_copy_init(xe, &blt);

	src = blt_create_object(&blt, region1, width, height, bpp, uc_mocs,
				T_LINEAR, COMPRESSION_DISABLED, comp_type, true);
	mid = blt_create_object(&blt, mid_region, width, height, bpp, uc_mocs,
				mid_tiling, mid_compression, comp_type, true);
	dst = blt_create_object(&blt, region1, width, height, bpp, uc_mocs,
				T_LINEAR, COMPRESSION_DISABLED, comp_type, true);
	igt_assert(src->size == dst->size);
	PRINT_SURFACE_INFO("src", src);
	PRINT_SURFACE_INFO("mid", mid);
	PRINT_SURFACE_INFO("dst", dst);

	blt_surface_fill_rect(xe, src, width, height);
	WRITE_PNG(xe, run_id, "src", src, width, height, bpp);

	blt.color_depth = CD_32bit;
	blt.print_bb = param.print_bb;
	blt_set_copy_object(&blt.src, src);
	blt_set_copy_object(&blt.dst, mid);
	blt_set_object_ext(&ext.src, 0, width, height, SURFACE_TYPE_2D);
	blt_set_object_ext(&ext.dst, mid_compression_format, width, height, SURFACE_TYPE_2D);
	blt_set_batch(&blt.bb, bb, bb_size, region1);
	blt_block_copy(xe, ctx, NULL, ahnd, &blt, pext);
	intel_ctx_xe_sync(ctx, true);

	/*
	 * If there's a compression we expect ctrl surface is not fully zeroed.
	 * Gradient image used as the reference may be not compressible for
	 * smaller sizes. Let's use some 'safe' size we're sure compression
	 * occurs and ctrl surface will be filled with some not-zeroed values.
	 */
	if (mid->compression && FROM_EXP_WH(width, height))
		if (blt_platform_has_flat_ccs_enabled(xe))
			igt_assert(blt_surface_is_compressed(xe, ctx, NULL, ahnd, mid));

	WRITE_PNG(xe, run_id, "mid", &blt.dst, width, height, bpp);

	if (config->surfcopy && pext) {
		struct drm_xe_engine_class_instance inst = {
			.engine_class = DRM_XE_ENGINE_CLASS_COPY,
		};
		intel_ctx_t *surf_ctx = ctx;
		uint64_t surf_ahnd = ahnd;
		uint32_t vm, exec_queue;

		if (config->new_ctx) {
			vm = xe_vm_create(xe, 0, 0);
			exec_queue = xe_exec_queue_create(xe, vm, &inst, 0);
			surf_ctx = intel_ctx_xe(xe, vm, exec_queue, 0, 0, 0);
			surf_ahnd = intel_allocator_open(xe, surf_ctx->vm,
							 INTEL_ALLOCATOR_RELOC);
		}
		surf_copy(xe, surf_ctx, surf_ahnd, src, mid, dst, run_id,
			  config->suspend_resume);

		if (surf_ctx != ctx) {
			xe_exec_queue_destroy(xe, exec_queue);
			xe_vm_destroy(xe, vm);
			free(surf_ctx);
			put_ahnd(surf_ahnd);
		}
	}

	blt_copy_init(xe, &blt);
	blt.color_depth = CD_32bit;
	blt.print_bb = param.print_bb;
	blt_set_copy_object(&blt.src, mid);
	blt_set_copy_object(&blt.dst, dst);
	blt_set_object_ext(&ext.src, mid_compression_format, width, height, SURFACE_TYPE_2D);
	blt_set_object_ext(&ext.dst, 0, width, height, SURFACE_TYPE_2D);
	if (config->inplace) {
		uint8_t pat_index = DEFAULT_PAT_INDEX;

		if (intel_gen(intel_get_drm_devid(xe)) >= 20 && config->compression)
			pat_index = intel_get_pat_idx_uc_comp(xe);

		blt_set_object(&blt.dst, mid->handle, dst->size, mid->region, 0,
			       pat_index, T_LINEAR, COMPRESSION_DISABLED,
			       comp_type);
		blt.dst.ptr = mid->ptr;
	}

	blt_set_batch(&blt.bb, bb, bb_size, region1);
	blt_block_copy(xe, ctx, NULL, ahnd, &blt, pext);
	intel_ctx_xe_sync(ctx, true);

	WRITE_PNG(xe, run_id, "dst", &blt.dst, width, height, bpp);

	result = memcmp(src->ptr, blt.dst.ptr, src->size);

	/* Politely clean vm */
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

static void block_multicopy(int xe,
			    intel_ctx_t *ctx,
			    uint32_t region1, uint32_t region2,
			    uint32_t width, uint32_t height,
			    enum blt_tiling_type mid_tiling,
			    const struct test_config *config)
{
	struct blt_copy3_data blt3 = {};
	struct blt_copy_data blt = {};
	struct blt_block_copy3_data_ext ext3 = {}, *pext3 = &ext3;
	struct blt_copy_object *src, *mid, *dst, *final;
	const uint32_t bpp = 32;
	uint64_t bb_size = xe_bb_size(xe, SZ_4K);
	uint64_t ahnd = intel_allocator_open(xe, ctx->vm, INTEL_ALLOCATOR_RELOC);
	uint32_t run_id = mid_tiling;
	uint32_t mid_region = (intel_gen(intel_get_drm_devid(xe)) >= 20 &&
			       !xe_has_vram(xe)) ? region1 : region2;
	uint32_t bb;
	enum blt_compression mid_compression = config->compression;
	int mid_compression_format = param.compression_format;
	enum blt_compression_type comp_type = COMPRESSION_TYPE_3D;
	uint8_t uc_mocs = intel_get_uc_mocs_index(xe);
	int result;

	bb = xe_bo_create(xe, 0, bb_size, region1, DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);

	if (!blt_uses_extended_block_copy(xe))
		pext3 = NULL;

	blt_copy_init(xe, &blt);

	src = blt_create_object(&blt, region1, width, height, bpp, uc_mocs,
				T_LINEAR, COMPRESSION_DISABLED, comp_type, true);
	mid = blt_create_object(&blt, mid_region, width, height, bpp, uc_mocs,
				mid_tiling, mid_compression, comp_type, true);
	dst = blt_create_object(&blt, region1, width, height, bpp, uc_mocs,
				mid_tiling, COMPRESSION_DISABLED, comp_type, true);
	final = blt_create_object(&blt, region1, width, height, bpp, uc_mocs,
				  T_LINEAR, COMPRESSION_DISABLED, comp_type, true);
	igt_assert(src->size == dst->size);
	PRINT_SURFACE_INFO("src", src);
	PRINT_SURFACE_INFO("mid", mid);
	PRINT_SURFACE_INFO("dst", dst);
	PRINT_SURFACE_INFO("final", final);

	blt_surface_fill_rect(xe, src, width, height);

	blt3.color_depth = CD_32bit;
	blt3.print_bb = param.print_bb;
	blt_set_copy_object(&blt3.src, src);
	blt_set_copy_object(&blt3.mid, mid);
	blt_set_copy_object(&blt3.dst, dst);
	blt_set_copy_object(&blt3.final, final);

	if (config->inplace) {
		uint8_t pat_index = DEFAULT_PAT_INDEX;

		if (intel_gen(intel_get_drm_devid(xe)) >= 20 && config->compression)
			pat_index = intel_get_pat_idx_uc_comp(xe);

		blt_set_object(&blt3.dst, mid->handle, dst->size, mid->region,
			       mid->mocs_index, pat_index, mid_tiling,
			       COMPRESSION_DISABLED, comp_type);
		blt3.dst.ptr = mid->ptr;
	}

	blt_set_object_ext(&ext3.src, 0, width, height, SURFACE_TYPE_2D);
	blt_set_object_ext(&ext3.mid, mid_compression_format, width, height, SURFACE_TYPE_2D);
	blt_set_object_ext(&ext3.dst, 0, width, height, SURFACE_TYPE_2D);
	blt_set_object_ext(&ext3.final, 0, width, height, SURFACE_TYPE_2D);
	blt_set_batch(&blt3.bb, bb, bb_size, region1);

	blt_block_copy3(xe, ctx, ahnd, &blt3, pext3);
	intel_ctx_xe_sync(ctx, true);

	WRITE_PNG(xe, run_id, "src", &blt3.src, width, height, bpp);
	if (!config->inplace)
		WRITE_PNG(xe, run_id, "mid", &blt3.mid, width, height, bpp);
	WRITE_PNG(xe, run_id, "dst", &blt3.dst, width, height, bpp);
	WRITE_PNG(xe, run_id, "final", &blt3.final, width, height, bpp);

	result = memcmp(src->ptr, blt3.final.ptr, src->size);

	put_offset(ahnd, src->handle);
	put_offset(ahnd, mid->handle);
	put_offset(ahnd, dst->handle);
	put_offset(ahnd, final->handle);
	put_offset(ahnd, bb);
	intel_allocator_bind(ahnd, 0, 0);
	blt_destroy_object(xe, src);
	blt_destroy_object(xe, mid);
	blt_destroy_object(xe, dst);
	blt_destroy_object(xe, final);
	gem_close(xe, bb);
	put_ahnd(ahnd);

	igt_assert_f(!result, "source and destination surfaces differs!\n");
}

static void block_copy_large(int xe,
			     intel_ctx_t *ctx,
			     uint32_t region1, uint32_t region2,
			     uint32_t width, uint32_t height,
			     enum blt_tiling_type tiling,
			     const struct test_config *config)
{
	struct blt_copy_data blt = {};
	struct blt_block_copy_data_ext ext = {}, *pext = &ext;
	struct blt_copy_object *src, *dst;
	const uint32_t bpp = 32;
	uint64_t bb_size = xe_bb_size(xe, SZ_4K);
	uint64_t ahnd = intel_allocator_open(xe, ctx->vm, INTEL_ALLOCATOR_RELOC);
	uint64_t size;
	uint32_t run_id = tiling;
	uint32_t bb;
	uint32_t *ptr;
	uint8_t uc_mocs = intel_get_uc_mocs_index(xe);
	bool result = true;
	int i;

	bb = xe_bo_create(xe, 0, bb_size, region1, DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);

	if (!blt_uses_extended_block_copy(xe))
		pext = NULL;

	blt_copy_init(xe, &blt);

	src = blt_create_object(&blt, region1, width, height, bpp, uc_mocs,
				T_LINEAR, COMPRESSION_DISABLED,
				COMPRESSION_TYPE_3D, true);
	dst = blt_create_object(&blt, region2, width, height, bpp, uc_mocs,
				tiling, COMPRESSION_ENABLED,
				COMPRESSION_TYPE_3D, true);
	PRINT_SURFACE_INFO("src", src);
	PRINT_SURFACE_INFO("dst", dst);

	blt_surface_fill_rect(xe, src, width, height);
	WRITE_PNG(xe, run_id, "src", src, width, height, bpp);

	blt.color_depth = CD_32bit;
	blt.print_bb = param.print_bb;
	blt_set_copy_object(&blt.src, src);
	blt_set_copy_object(&blt.dst, dst);
	blt_set_object_ext(&ext.src, 0, width, height, SURFACE_TYPE_2D);
	blt_set_object_ext(&ext.dst, param.compression_format,
			   width, height, SURFACE_TYPE_2D);
	blt_set_batch(&blt.bb, bb, bb_size, region1);
	blt_block_copy(xe, ctx, NULL, ahnd, &blt, pext);
	intel_ctx_xe_sync(ctx, true);

	blt_surface_get_flatccs_data(xe, ctx, NULL, ahnd, dst, &ptr, &size);
	for (i = 0; i < size / sizeof(*ptr); i++) {
		if (ptr[i] == 0) {
			result = false;
			break;
		}
	}

	if (!result) {
		for (i = 0; i < size / sizeof(*ptr); i += 8)
			igt_debug("[%08x]: %08x %08x %08x %08x %08x %08x %08x %08x\n",
				  i,
				  ptr[i], ptr[i + 1], ptr[i + 2], ptr[i + 3],
				  ptr[i + 4], ptr[i + 5], ptr[i + 6], ptr[i + 7]);
	}

	WRITE_PNG(xe, run_id, "dst", &blt.dst, width, height, bpp);

	/* Politely clean vm */
	put_offset(ahnd, src->handle);
	put_offset(ahnd, dst->handle);
	put_offset(ahnd, bb);
	intel_allocator_bind(ahnd, 0, 0);
	blt_destroy_object(xe, src);
	blt_destroy_object(xe, dst);
	gem_close(xe, bb);
	put_ahnd(ahnd);

	igt_assert_f(result, "ccs data must have no zeros!\n");
}

enum copy_func {
	BLOCK_COPY,
	BLOCK_MULTICOPY,
	LARGE_SURFCOPY,
};

static const struct {
	const char *suffix;
	void (*copyfn)(int fd,
		       intel_ctx_t *ctx,
		       uint32_t region1, uint32_t region2,
		       uint32_t width, uint32_t height,
		       enum blt_tiling_type btype,
		       const struct test_config *config);
} copyfns[] = {
	[BLOCK_COPY] = { "", block_copy },
	[BLOCK_MULTICOPY] = { "-multicopy", block_multicopy },
	[LARGE_SURFCOPY] = { "", block_copy_large },
};

static void single_copy(int xe, const struct test_config *config,
			int32_t region1, uint32_t region2,
			uint32_t width, uint32_t height,
			int tiling, enum copy_func copy_function)
{
	struct drm_xe_engine_class_instance inst = {
		.engine_class = DRM_XE_ENGINE_CLASS_COPY,
	};
	uint32_t vm, exec_queue;
	uint32_t sync_bind, sync_out;
	intel_ctx_t *ctx;

	vm = xe_vm_create(xe, 0, 0);
	exec_queue = xe_exec_queue_create(xe, vm, &inst, 0);
	sync_bind = syncobj_create(xe, 0);
	sync_out = syncobj_create(xe, 0);
	ctx = intel_ctx_xe(xe, vm, exec_queue,
			   0, sync_bind, sync_out);

	copyfns[copy_function].copyfn(xe, ctx,
				      region1, region2,
				      width, height,
				      tiling, config);

	xe_exec_queue_destroy(xe, exec_queue);
	xe_vm_destroy(xe, vm);
	syncobj_destroy(xe, sync_bind);
	syncobj_destroy(xe, sync_out);
	free(ctx);
}

static void block_copy_test(int xe,
			    const struct test_config *config,
			    struct igt_collection *set,
			    enum copy_func copy_function)
{
	uint16_t dev_id = intel_get_drm_devid(xe);
	struct igt_collection *regions;
	int tiling, width, height;


	if (intel_gen(dev_id) >= 20 && config->compression)
		igt_require(HAS_FLATCCS(dev_id));

	if (config->compression && !blt_block_copy_supports_compression(xe))
		return;

	if (config->inplace && !config->compression)
		return;

	width = config->overwrite_width ?: param.width;
	height = config->overwrite_height ?: param.height;

	for_each_tiling(tiling) {
		if (!blt_block_copy_supports_tiling(xe, tiling) ||
		    (param.tiling >= 0 && param.tiling != tiling))
			continue;

		for_each_variation_r(regions, 2, set) {
			uint32_t region1, region2;
			char *regtxt;
			char testname[256];

			region1 = igt_collection_get_value(regions, 0);
			region2 = igt_collection_get_value(regions, 1);

			/* if not XE2, then Compressed surface must be in device memory */
			if (config->compression && !is_intel_region_compressible(xe, region2))
				continue;

			regtxt = xe_memregion_dynamic_subtest_name(xe, regions);

			snprintf(testname, sizeof(testname),
				 "%s-%s-compfmt%d-%s%s",
				 blt_tiling_name(tiling),
				 config->compression ?
					 "compressed" : "uncompressed",
				 param.compression_format, regtxt,
				 copyfns[copy_function].suffix);

			if (!config->width_increment) {
				igt_dynamic(testname)
					single_copy(xe, config, region1, region2,
						    width, height,
						    tiling, copy_function);
			} else {
				for (int w = param.incdim_width;
				     w < param.incdim_width + config->width_steps;
				     w += config->width_increment) {
					snprintf(testname, sizeof(testname),
						 "%s-%s-compfmt%d-%s%s-%dx%d",
						 blt_tiling_name(tiling),
						 config->compression ?
							 "compressed" : "uncompressed",
						 param.compression_format, regtxt,
						 copyfns[copy_function].suffix,
						 w, w);
					igt_dynamic(testname)
						single_copy(xe, config, region1, region2,
							    w, w, tiling, copy_function);
				}

			}

			free(regtxt);
		}
	}
}

static void large_surf_ctrl_copy(int xe, const struct test_config *config)
{
	uint16_t dev_id = intel_get_drm_devid(xe);
	int tiling, width, height;
	uint32_t region1, region2;

	igt_require(HAS_FLATCCS(dev_id));

	region1 = system_memory(xe);
	region2 = vram_if_possible(xe, 0);

	width = config->overwrite_width;
	height = config->overwrite_height;

	/* Prefer TILE4 if supported */
	if (blt_block_copy_supports_tiling(xe, T_TILE4)) {
		tiling = T_TILE4;
	} else {
		for_each_tiling(tiling) {
			if (!blt_block_copy_supports_tiling(xe, tiling))
				continue;
			break;
		}
	}

	single_copy(xe, config, region1, region2, width, height, tiling,
		    LARGE_SURFCOPY);
}

static int opt_handler(int opt, int opt_index, void *data)
{
	switch (opt) {
	case 'b':
		param.print_bb = true;
		igt_debug("Print bb: %d\n", param.print_bb);
		break;
	case 'f':
		param.compression_format = atoi(optarg);
		igt_debug("Compression format: %d\n", param.compression_format);
		igt_assert_eq((param.compression_format & ~0x1f), 0);
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
	"  -f\tCompression format (0-31)\n"
	"  -p\tWrite PNG\n"
	"  -s\tPrint surface info\n"
	"  -t\tTiling format (0 - linear, 1 - XMAJOR, 2 - YMAJOR, 3 - TILE4, 4 - TILE64)\n"
	"  -W\tWidth (default 512)\n"
	"  -H\tHeight (default 512)"
	;

int igt_main_args("bf:pst:W:H:", NULL, help_str, opt_handler, NULL)
{
	struct igt_collection *set;
	int xe;

	igt_fixture() {
		xe = drm_open_driver(DRIVER_XE);
		igt_require(blt_has_block_copy(xe));

		xe_device_get(xe);

		set = xe_get_memory_region_set(xe,
					       DRM_XE_MEM_REGION_CLASS_SYSMEM,
					       DRM_XE_MEM_REGION_CLASS_VRAM);
	}

	igt_describe("Check block-copy uncompressed blit");
	igt_subtest_with_dynamic("block-copy-uncompressed") {
		struct test_config config = {};

		block_copy_test(xe, &config, set, BLOCK_COPY);
	}

	igt_describe("Check block-copy uncompressed blit with increment width/height");
	igt_subtest_with_dynamic("block-copy-uncompressed-inc-dimension") {
		struct test_config config = { .width_increment = 15,
					      .width_steps = 512 };

		block_copy_test(xe, &config, set, BLOCK_COPY);
	}

	igt_describe("Check block-copy flatccs compressed blit");
	igt_subtest_with_dynamic("block-copy-compressed") {
		struct test_config config = { .compression = true };

		block_copy_test(xe, &config, set, BLOCK_COPY);
	}

	igt_describe("Check block-copy compressed blit with increment width/height");
	igt_subtest_with_dynamic("block-copy-compressed-inc-dimension") {
		struct test_config config = { .compression = true,
					      .width_increment = 15,
					      .width_steps = 512 };

		block_copy_test(xe, &config, set, BLOCK_COPY);
	}

	igt_describe("Check block-multicopy flatccs compressed blit");
	igt_subtest_with_dynamic("block-multicopy-compressed") {
		struct test_config config = { .compression = true };

		block_copy_test(xe, &config, set, BLOCK_MULTICOPY);
	}

	igt_describe("Check block-multicopy flatccs inplace decompression blit");
	igt_subtest_with_dynamic("block-multicopy-inplace") {
		struct test_config config = { .compression = true,
					      .inplace = true };

		block_copy_test(xe, &config, set, BLOCK_MULTICOPY);
	}

	igt_describe("Check flatccs data can be copied from/to surface");
	igt_subtest_with_dynamic("ctrl-surf-copy") {
		struct test_config config = { .compression = true,
					      .surfcopy = true };

		block_copy_test(xe, &config, set, BLOCK_COPY);
	}

	igt_describe("Check flatccs data are physically tagged and visible"
		     " in different contexts");
	igt_subtest_with_dynamic("ctrl-surf-copy-new-ctx") {
		struct test_config config = { .compression = true,
					      .surfcopy = true,
					      .new_ctx = true };

		block_copy_test(xe, &config, set, BLOCK_COPY);
	}

	/*
	 * Why 4096x4160 is chosen as WxH?
	 *
	 * On Xe ctrl-surf-copy size single increment does 256B ccs copy which
	 * covers 64KiB surface. Size field is 10-bit so to exceed 64K * 1024
	 * surface which is bigger than 4K x 4K x 32bpp (>64MiB) must be used.
	 *
	 * On Xe2+ ctrl-surf-copy has finer granularity - single size increment
	 * copies 8B ccs which in turn covers 4KiB surface. So 64MiB+ surface
	 * will require > 16 separate ctrl-surf-copy commands.
	 */
	igt_describe("Check flatccs data can be copied from large surface");
	igt_subtest("large-ctrl-surf-copy") {
		struct test_config config = { .overwrite_width = 4096,
					      .overwrite_height = 4096+64, };

		large_surf_ctrl_copy(xe, &config);
	}

	igt_describe("Check flatccs data persists after suspend / resume (S0)");
	igt_subtest_with_dynamic("suspend-resume") {
		struct test_config config = { .compression = true,
					      .surfcopy = true,
					      .suspend_resume = true };

		block_copy_test(xe, &config, set, BLOCK_COPY);
	}

	igt_fixture() {
		xe_device_put(xe);
		close(xe);
	}
}
