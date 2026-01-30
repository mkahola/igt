// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

/**
 * TEST: Test for selecting per-VMA pat_index
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: VMA
 * Functionality: pat_index
 */

#include <fcntl.h>

#include "igt.h"
#include "igt_vgem.h"
#include "intel_blt.h"
#include "intel_mocs.h"
#include "intel_pat.h"
#include "linux_scaffold.h"

#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"
#include "xe/xe_util.h"

#define XE_COH_NONE          1
#define XE_COH_AT_LEAST_1WAY 2

static bool do_slow_check;

static uint32_t create_object(int fd, int r, int size, uint16_t coh_mode,
			      bool force_cpu_wc);

/**
 * SUBTEST: userptr-coh-none
 * Test category: functionality test
 * Description: Test non-coherent pat_index on userptr
 */
static void userptr_coh_none(int fd)
{
	size_t size = xe_get_default_alignment(fd);
	uint32_t vm;
	void *data;

	data = mmap(0, size, PROT_READ |
		    PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	igt_assert(data != MAP_FAILED);

	vm = xe_vm_create(fd, 0, 0);

	/*
	 * Try some valid combinations first just to make sure we're not being
	 * swindled.
	 */
	igt_assert_eq(__xe_vm_bind(fd, vm, 0, 0, to_user_pointer(data), 0x40000,
				   size, DRM_XE_VM_BIND_OP_MAP_USERPTR, 0, NULL, 0, 0,
				   DEFAULT_PAT_INDEX, 0),
		      0);
	xe_vm_unbind_sync(fd, vm, 0, 0x40000, size);
	igt_assert_eq(__xe_vm_bind(fd, vm, 0, 0, to_user_pointer(data), 0x40000,
				   size, DRM_XE_VM_BIND_OP_MAP_USERPTR, 0, NULL, 0, 0,
				   intel_get_pat_idx_wb(fd), 0),
		      0);
	xe_vm_unbind_sync(fd, vm, 0, 0x40000, size);

	/* And then some known COH_NONE pat_index combos which should fail. */
	igt_assert_eq(__xe_vm_bind(fd, vm, 0, 0, to_user_pointer(data), 0x40000,
				   size, DRM_XE_VM_BIND_OP_MAP_USERPTR, 0, NULL, 0, 0,
				   intel_get_pat_idx_uc(fd), 0),
		      -EINVAL);
	igt_assert_eq(__xe_vm_bind(fd, vm, 0, 0, to_user_pointer(data), 0x40000,
				   size, DRM_XE_VM_BIND_OP_MAP_USERPTR, 0, NULL, 0, 0,
				   intel_get_pat_idx_wt(fd), 0),
		      -EINVAL);

	munmap(data, size);
	xe_vm_destroy(fd, vm);
}
#define REG_FIELD_GET(__mask, __val) \
	((uint32_t)FIELD_GET(__mask, __val))

#define XE2_NO_PROMOTE	REG_BIT(10)
#define XE2_COMP_EN	REG_BIT(9)
#define XE2_L3_CLOS	GENMASK(7, 6)
#define XE2_L3_POLICY	GENMASK(5, 4)
#define XE2_L4_POLICY	GENMASK(3, 2)
#define XE2_COH_MODE	GENMASK(1, 0)

#define L3_CLOS1		1
#define L3_CLOS2		2
#define L3_CLOS3		3

#define L3_CACHE_POLICY_WB	0
#define L3_CACHE_POLICY_XD	1
#define L3_CACHE_POLICY_UC	3

#define L4_CACHE_POLICY_WB	0
#define L4_CACHE_POLICY_WT	1
#define L4_CACHE_POLICY_UC	3

#define COH_MODE_NONE	  	0
#define COH_MODE_1WAY		2
#define COH_MODE_2WAY		3

static int xe_fetch_pat_sw_config(int fd, struct intel_pat_cache *pat_sw_config)
{
	int32_t parsed = xe_get_pat_sw_config(fd, pat_sw_config);

	igt_assert_f(parsed > 0, "Couldn't get Xe PAT software configuration\n");

	return parsed;
}

/**
 * SUBTEST: pat-sanity
 * Test category: functionality test
 * Description: Test debugfs PAT config vs getters
 */
static void pat_sanity(int fd)
{
	uint16_t dev_id = intel_get_drm_devid(fd);
	struct intel_pat_cache pat_sw_config = {};
	int32_t parsed;
	bool has_uc_comp = false, has_wt = false;

	parsed = xe_fetch_pat_sw_config(fd, &pat_sw_config);

	if (intel_graphics_ver(dev_id) >= IP_VER(20, 0)) {
		for (int i = 0; i < parsed; i++) {
			uint32_t pat = pat_sw_config.entries[i].pat;
			if (pat_sw_config.entries[i].rsvd)
				continue;
			if (!!(pat & XE2_COMP_EN) &&
			    REG_FIELD_GET(XE2_L3_POLICY, pat) == L3_CACHE_POLICY_UC &&
			    REG_FIELD_GET(XE2_L4_POLICY, pat) == L4_CACHE_POLICY_UC) {
				has_uc_comp = true;
			}
			if (REG_FIELD_GET(XE2_L3_POLICY, pat) == L3_CACHE_POLICY_XD &&
			    REG_FIELD_GET(XE2_L4_POLICY, pat) == L4_CACHE_POLICY_WT) {
				has_wt = true;
			}
		}
	} else {
		has_wt = true;
	}
	igt_assert_eq(pat_sw_config.max_index, intel_get_max_pat_index(fd));
	igt_assert_eq(pat_sw_config.uc, intel_get_pat_idx_uc(fd));
	igt_assert_eq(pat_sw_config.wb, intel_get_pat_idx_wb(fd));
	if (has_wt)
		igt_assert_eq(pat_sw_config.wt, intel_get_pat_idx_wt(fd));
	if (has_uc_comp)
		igt_assert_eq(pat_sw_config.uc_comp, intel_get_pat_idx_uc_comp(fd));
}

/**
 * SUBTEST: pat-index-all
 * Test category: functionality test
 * Description: Test every pat_index
 */
static void pat_index_all(int fd)
{
	size_t size = xe_get_default_alignment(fd);
	struct intel_pat_cache pat_sw_config = {};
	uint32_t vm, bo;
	uint8_t pat_index;

	vm = xe_vm_create(fd, 0, 0);

	bo = xe_bo_create_caching(fd, 0, size, all_memory_regions(fd), 0,
				  DRM_XE_GEM_CPU_CACHING_WC);

	igt_assert_eq(__xe_vm_bind(fd, vm, 0, bo, 0, 0x40000,
				   size, DRM_XE_VM_BIND_OP_MAP, 0, NULL, 0, 0,
				   intel_get_pat_idx_uc(fd), 0),
		      0);
	xe_vm_unbind_sync(fd, vm, 0, 0x40000, size);

	igt_assert_eq(__xe_vm_bind(fd, vm, 0, bo, 0, 0x40000,
				   size, DRM_XE_VM_BIND_OP_MAP, 0, NULL, 0, 0,
				   intel_get_pat_idx_wt(fd), 0),
		      0);
	xe_vm_unbind_sync(fd, vm, 0, 0x40000, size);

	igt_assert_eq(__xe_vm_bind(fd, vm, 0, bo, 0, 0x40000,
				   size, DRM_XE_VM_BIND_OP_MAP, 0, NULL, 0, 0,
				   intel_get_pat_idx_wb(fd), 0),
		      0);
	xe_vm_unbind_sync(fd, vm, 0, 0x40000, size);

	igt_assert(intel_get_max_pat_index(fd));

	xe_fetch_pat_sw_config(fd, &pat_sw_config);

	for (pat_index = 0; pat_index <= intel_get_max_pat_index(fd);
	     pat_index++) {

		if (pat_sw_config.entries[pat_index].rsvd) {
			igt_assert_eq(__xe_vm_bind(fd, vm, 0, bo, 0, 0x40000,
						   size, DRM_XE_VM_BIND_OP_MAP, 0, NULL, 0, 0,
						   pat_index, 0),
				      -EINVAL);

			igt_assert_eq(__xe_vm_bind(fd, vm, 0, 0, 0, 0x40000,
						   size, DRM_XE_VM_BIND_OP_MAP,
						   DRM_XE_VM_BIND_FLAG_NULL, NULL, 0, 0,
						   pat_index, 0),
				      -EINVAL);
		} else {
			igt_assert_eq(__xe_vm_bind(fd, vm, 0, bo, 0, 0x40000,
						   size, DRM_XE_VM_BIND_OP_MAP, 0, NULL, 0, 0,
						   pat_index, 0),
				      0);
			xe_vm_unbind_sync(fd, vm, 0, 0x40000, size);

			/*
			 * There is no real memory being mapped here, so any
			 * platform supported pat_index should be acceptable for
			 * NULL mappings.
			 */
			igt_assert_eq(__xe_vm_bind(fd, vm, 0, 0, 0, 0x40000,
						   size, DRM_XE_VM_BIND_OP_MAP,
						   DRM_XE_VM_BIND_FLAG_NULL, NULL, 0, 0,
						   pat_index, 0),
				      0);
			xe_vm_unbind_sync(fd, vm, 0, 0x40000, size);
		}
	}

	igt_assert_eq(__xe_vm_bind(fd, vm, 0, bo, 0, 0x40000,
				   size, DRM_XE_VM_BIND_OP_MAP, 0, NULL, 0, 0,
				   pat_index, 0),
		      -EINVAL);

	igt_assert_eq(__xe_vm_bind(fd, vm, 0, 0, 0, 0x40000,
				   size, DRM_XE_VM_BIND_OP_MAP,
				   DRM_XE_VM_BIND_FLAG_NULL, NULL, 0, 0,
				   pat_index, 0),
		      -EINVAL);

	gem_close(fd, bo);

	/* coh_none is never allowed with cpu_caching WB. */

	bo = xe_bo_create_caching(fd, 0, size, system_memory(fd), 0,
				  DRM_XE_GEM_CPU_CACHING_WB);

	igt_assert_eq(__xe_vm_bind(fd, vm, 0, bo, 0, 0x40000,
				   size, DRM_XE_VM_BIND_OP_MAP, 0, NULL, 0, 0,
				   intel_get_pat_idx_uc(fd), 0),
		      -EINVAL);

	igt_assert_eq(__xe_vm_bind(fd, vm, 0, bo, 0, 0x40000,
				   size, DRM_XE_VM_BIND_OP_MAP, 0, NULL, 0, 0,
				   intel_get_pat_idx_wt(fd), 0),
		      -EINVAL);

	igt_assert_eq(__xe_vm_bind(fd, vm, 0, bo, 0, 0x40000,
				   size, DRM_XE_VM_BIND_OP_MAP, 0, NULL, 0, 0,
				   intel_get_pat_idx_wb(fd), 0),
		      0);
	xe_vm_unbind_sync(fd, vm, 0, 0x40000, size);

	gem_close(fd, bo);

	xe_vm_destroy(fd, vm);
}

#define CLEAR_1 0xFFFFFFFF /* something compressible */

static void xe2_blt_decompress_dst(int fd,
				   intel_ctx_t *ctx,
				   uint64_t ahnd,
				   struct blt_copy_data *blt,
				   uint32_t alias_handle,
				   uint32_t size)
{
	struct blt_copy_object tmp = {};

	/*
	 * Xe2 in-place decompression using an alias to the same physical
	 * memory, but with the dst mapped using some uncompressed pat_index.
	 * This should allow checking the object pages via mmap.
	 */

	memcpy(&tmp, &blt->src, sizeof(blt->dst));
	memcpy(&blt->src, &blt->dst, sizeof(blt->dst));
	blt_set_object(&blt->dst, alias_handle, size, 0,
		       intel_get_uc_mocs_index(fd),
		       intel_get_pat_idx_uc(fd), /* compression disabled */
		       T_LINEAR, 0, 0);
	blt_fast_copy(fd, ctx, NULL, ahnd, blt);
	memcpy(&blt->dst, &blt->src, sizeof(blt->dst));
	memcpy(&blt->src, &tmp, sizeof(blt->dst));
}

struct xe_pat_size_mode {
	uint16_t width;
	uint16_t height;
	uint32_t alignment;

	const char *name;
};

struct xe_pat_param {
	int fd;

	const struct xe_pat_size_mode *size;

	uint32_t r1;
	uint32_t r1_bo;
	uint32_t *r1_map;
	uint8_t  r1_pat_index;
	bool     r1_compressed; /* xe2+ compression */

	uint32_t r2;
	uint32_t r2_bo;
	uint32_t *r2_map;
	uint8_t  r2_pat_index;
	bool     r2_compressed;

};

static void pat_index_blt(struct xe_pat_param *p)
{
	struct drm_xe_engine_class_instance inst = {
		.engine_class = DRM_XE_ENGINE_CLASS_COPY,
	};
	struct blt_copy_data blt = {};
	struct blt_copy_object src = {};
	struct blt_copy_object dst = {};
	uint32_t vm, exec_queue, bb;
	intel_ctx_t *ctx;
	uint64_t ahnd;
	int width = p->size->width, height = p->size->height;
	int size, stride, bb_size;
	int bpp = 32;
	uint32_t alias, name;
	int fd = p->fd;
	int i;

	igt_require(blt_has_fast_copy(fd));

	vm = xe_vm_create(fd, 0, 0);
	exec_queue = xe_exec_queue_create(fd, vm, &inst, 0);
	ctx = intel_ctx_xe(fd, vm, exec_queue, 0, 0, 0);
	ahnd = intel_allocator_open_full(fd, ctx->vm, 0, 0,
					 INTEL_ALLOCATOR_SIMPLE,
					 ALLOC_STRATEGY_LOW_TO_HIGH,
					 p->size->alignment);

	bb_size = xe_bb_size(fd, SZ_4K);
	bb = xe_bo_create(fd, 0, bb_size, system_memory(fd), 0);

	size = width * height * bpp / 8;
	stride = width * 4;

	if (p->r2_compressed) {
		name = gem_flink(fd, p->r2_bo);
		alias = gem_open(fd, name);
	}

	blt_copy_init(fd, &blt);
	blt.color_depth = CD_32bit;

	blt_set_object(&src, p->r1_bo, size, p->r1, intel_get_uc_mocs_index(fd),
		       p->r1_pat_index, T_LINEAR,
		       COMPRESSION_DISABLED, COMPRESSION_TYPE_3D);
	blt_set_geom(&src, stride, 0, 0, width, height, 0, 0);

	blt_set_object(&dst, p->r2_bo, size, p->r2, intel_get_uc_mocs_index(fd),
		       p->r2_pat_index, T_LINEAR,
		       COMPRESSION_DISABLED, COMPRESSION_TYPE_3D);
	blt_set_geom(&dst, stride, 0, 0, width, height, 0, 0);

	blt_set_copy_object(&blt.src, &src);
	blt_set_copy_object(&blt.dst, &dst);
	blt_set_batch(&blt.bb, bb, bb_size, system_memory(fd));

	/* Ensure we always see zeroes for the initial KMD zeroing */
	blt_fast_copy(fd, ctx, NULL, ahnd, &blt);
	if (p->r2_compressed)
		xe2_blt_decompress_dst(fd, ctx, ahnd, &blt, alias, size);

	if (do_slow_check) {
		for (i = 0; i < size / 4; i++)
			igt_assert_eq(p->r2_map[i], 0);
	} else {
		igt_assert_eq(p->r2_map[0], 0);
		igt_assert_eq(p->r2_map[size/4 - 1], 0);

		for (i = 0; i < 128; i++) {
			int dw = rand() % (size/4);

			igt_assert_eq(p->r2_map[dw], 0);
		}
	}

	/* Write some values from the CPU, potentially dirtying the CPU cache */
	for (i = 0; i < size / 4; i++) {
		if (p->r2_compressed)
			p->r1_map[i] = CLEAR_1;
		else
			p->r1_map[i] = i;
	}

	/* And finally ensure we always see the CPU written values */
	blt_fast_copy(fd, ctx, NULL, ahnd, &blt);
	if (p->r2_compressed)
		xe2_blt_decompress_dst(fd, ctx, ahnd, &blt, alias, size);

	if (do_slow_check) {
		for (i = 0; i < size / 4; i++) {
			if (p->r2_compressed)
				igt_assert_eq(p->r2_map[i], CLEAR_1);
			else
				igt_assert_eq(p->r2_map[i], i);
		}
	} else {
		if (p->r2_compressed) {
			igt_assert_eq(p->r2_map[0], CLEAR_1);
			igt_assert_eq(p->r2_map[size/4 - 1], CLEAR_1);
		} else {
			igt_assert_eq(p->r2_map[0], 0);
			igt_assert_eq(p->r2_map[size/4 - 1], size/4 -1);
		}

		for (i = 0; i < 128; i++) {
			int dw = rand() % (size/4);

			if (p->r2_compressed)
				igt_assert_eq(p->r2_map[dw], CLEAR_1);
			else
				igt_assert_eq(p->r2_map[dw], dw);
		}
	}

	gem_close(fd, bb);

	xe_exec_queue_destroy(fd, exec_queue);
	xe_vm_destroy(fd, vm);

	put_ahnd(ahnd);
	intel_ctx_destroy(fd, ctx);
}

static void pat_index_render(struct xe_pat_param *p)
{
	int fd = p->fd;
	igt_render_copyfunc_t render_copy = NULL;
	int size, stride, width = p->size->width, height = p->size->height;
	struct intel_buf src, dst;
	struct intel_bb *ibb;
	struct buf_ops *bops;
	int bpp = 32;
	int i;

	render_copy = igt_get_render_copyfunc(fd);
	igt_require(render_copy);
	igt_require(xe_has_engine_class(fd, DRM_XE_ENGINE_CLASS_RENDER));

	if (p->r2_compressed) /* XXX */
		return;

	bops = buf_ops_create(fd);

	ibb = intel_bb_create_full(fd, 0, 0, NULL, xe_get_default_alignment(fd),
				   0, 0, p->size->alignment,
				   INTEL_ALLOCATOR_SIMPLE,
				   ALLOC_STRATEGY_HIGH_TO_LOW, vram_if_possible(fd, 0));

	size = width * height * bpp / 8;
	stride = width * 4;

	intel_buf_init_full(bops, p->r1_bo, &src, width, height, bpp, 0,
			    I915_TILING_NONE, I915_COMPRESSION_NONE, size,
			    stride, p->r1, p->r1_pat_index, DEFAULT_MOCS_INDEX);

	intel_buf_init_full(bops, p->r2_bo, &dst, width, height, bpp, 0,
			    I915_TILING_NONE, I915_COMPRESSION_NONE, size,
			    stride, p->r2, p->r2_pat_index, DEFAULT_MOCS_INDEX);

	/* Ensure we always see zeroes for the initial KMD zeroing */
	render_copy(ibb,
		    &src,
		    0, 0, width, height,
		    &dst,
		    0, 0);
	intel_bb_sync(ibb);

	if (do_slow_check) {
		for (i = 0; i < size / 4; i++)
			igt_assert_eq(p->r2_map[i], 0);
	} else {
		igt_assert_eq(p->r2_map[0], 0);
		igt_assert_eq(p->r2_map[size/4 - 1], 0);

		for (i = 0; i < 128; i++) {
			int dw = rand() % (size/4);

			igt_assert_eq(p->r2_map[dw], 0);
		}
	}

	/* Write some values from the CPU, potentially dirtying the CPU cache */
	if (p->r1_compressed) {
		uint32_t tmp_bo;
		uint32_t *tmp_map;
		uint32_t tmp_region = system_memory(fd);
		uint8_t tmp_pat_index = intel_get_pat_idx_wb(fd);

		tmp_bo = create_object(fd, tmp_region, size,
				       XE_COH_AT_LEAST_1WAY, false);
		tmp_map = xe_bo_map(fd, tmp_bo, size);

		for (i = 0; i < size / sizeof(uint32_t); i++)
			tmp_map[i] = i;

		xe_fast_copy(fd,
			     tmp_bo, tmp_region, tmp_pat_index,
			     p->r1_bo, p->r1, p->r1_pat_index,
			     size);

		munmap(tmp_map, size);
		gem_close(fd, tmp_bo);
	} else {
		for (i = 0; i < size / sizeof(uint32_t); i++)
			p->r1_map[i] = i;
	}

	/* And finally ensure we always see the CPU written values */
	render_copy(ibb,
		    &src,
		    0, 0, width, height,
		    &dst,
		    0, 0);
	intel_bb_sync(ibb);

	if (do_slow_check) {
		for (i = 0; i < size / 4; i++)
			igt_assert_eq(p->r2_map[i], i);
	} else {
		igt_assert_eq(p->r2_map[0], 0);
		igt_assert_eq(p->r2_map[size/4 - 1], size/4 - 1);

		for (i = 0; i < 128; i++) {
			int dw = rand() % (size/4);

			igt_assert_eq(p->r2_map[dw], dw);
		}
	}

	intel_bb_destroy(ibb);
}

static void pat_index_dw(struct xe_pat_param *p)
{
	int fd = p->fd;
	int size, stride, width = p->size->width, height = p->size->height;
	struct drm_xe_engine_class_instance *hwe;
	struct intel_bb *ibb;
	int dw_gpu_map[16] = {};
	int dw_cpu_map[16] = {};
	struct buf_ops *bops;
	struct intel_buf r1_buf, r2_buf;
	int bpp = 32, i, n_engines;
	uint32_t ctx, vm;

	if (p->r1_compressed || p->r2_compressed)
		return;

	bops = buf_ops_create(fd);

	n_engines = 0;
	i = rand() % xe_number_engines(fd);
	xe_for_each_engine(fd, hwe) {
		if (i == n_engines++)
			break;
	}

	vm = xe_vm_create(fd, 0, 0);
	ctx = xe_exec_queue_create(fd, vm, hwe, 0);

	ibb = intel_bb_create_full(fd, ctx, vm, NULL, xe_get_default_alignment(fd),
				   0, 0, p->size->alignment,
				   INTEL_ALLOCATOR_SIMPLE,
				   ALLOC_STRATEGY_LOW_TO_HIGH, vram_if_possible(fd, 0));

	size = width * height * bpp / 8;
	stride = width * 4;

	intel_buf_init_full(bops, p->r1_bo, &r1_buf, width, height, bpp, 0,
			    I915_TILING_NONE, I915_COMPRESSION_NONE, size,
			    stride, p->r1, p->r1_pat_index, DEFAULT_MOCS_INDEX);
	intel_bb_add_intel_buf(ibb, &r1_buf, true);

	intel_buf_init_full(bops, p->r2_bo, &r2_buf, width, height, bpp, 0,
			    I915_TILING_NONE, I915_COMPRESSION_NONE, size,
			    stride, p->r2, p->r2_pat_index, DEFAULT_MOCS_INDEX);
	intel_bb_add_intel_buf(ibb, &r2_buf, true);

	/*
	 * Partially dirty some random selection of cache-lines using the CPU.
	 * On the GPU (using some random engine) we then do some dword writes
	 * into those same cache-lines. Finally we read back from the CPU and
	 * verify.
	 */

	for (i = 0; i < ARRAY_SIZE(dw_cpu_map); i++) {
		int cl = rand() % (size/64);
		int dw_cpu = cl * (64/4) + rand() % (64/4);
		int dw_gpu = cl * (64/4) + rand() % (64/4);
		uint64_t offset;

		p->r1_map[dw_cpu] = dw_cpu;

		offset = r1_buf.addr.offset + dw_gpu * 4;
		intel_bb_out(ibb, MI_STORE_DWORD_IMM_GEN4);
		intel_bb_out(ibb, offset);
		intel_bb_out(ibb, offset >> 32);
		intel_bb_out(ibb, dw_gpu);

		p->r2_map[dw_cpu] = dw_cpu;

		offset = r2_buf.addr.offset + dw_gpu * 4;
		intel_bb_out(ibb, MI_STORE_DWORD_IMM_GEN4);
		intel_bb_out(ibb, offset);
		intel_bb_out(ibb, offset >> 32);
		intel_bb_out(ibb, dw_gpu);

		dw_cpu_map[i] = dw_cpu;
		dw_gpu_map[i] = dw_gpu;
	}

	intel_bb_out(ibb, MI_BATCH_BUFFER_END);
	intel_bb_ptr_align(ibb, 8);

	intel_bb_exec(ibb, intel_bb_offset(ibb), 0, false);
	intel_bb_sync(ibb);

	for (i = 0; i < ARRAY_SIZE(dw_cpu_map); i++) {
		int dw_cpu = dw_cpu_map[i];
		int dw_gpu = dw_gpu_map[i];
		int dw_rng;

		igt_assert_eq(p->r1_map[dw_cpu], dw_cpu);
		igt_assert_eq(p->r1_map[dw_gpu], dw_gpu);

		igt_assert_eq(p->r2_map[dw_gpu], dw_gpu);
		igt_assert_eq(p->r2_map[dw_cpu], dw_cpu);

		/* Also ensure we see KMD zeroing */
		dw_rng = rand() % (size/4);
		igt_assert(p->r1_map[dw_rng] == dw_rng ||
			   p->r1_map[dw_rng] == 0);

		dw_rng = rand() % (size/4);
		igt_assert(p->r2_map[dw_rng] == dw_rng ||
			   p->r2_map[dw_rng] == 0);
	}

	intel_bb_destroy(ibb);

	xe_exec_queue_destroy(fd, ctx);
	xe_vm_destroy(fd, vm);
}

/**
 * SUBTEST: prime-self-import-coh
 * Test category: functionality test
 * Description: Check prime import from same device.
 */

static void prime_self_import_coh(void)
{
	uint32_t src_handle, dst_handle, handle_import;
	struct xe_pat_param p = {};
	struct xe_pat_size_mode mode_size = {
		.width = 1024,
		.height = 512,
	};
	int fd1, fd2;
	int dma_buf_fd;
	int bpp = 32;
	int size = mode_size.width * mode_size.height * bpp / 8;
	uint32_t vm;

	fd1 = drm_open_driver(DRIVER_XE);
	fd2 = drm_open_driver(DRIVER_XE);

	dst_handle = xe_bo_create_caching(fd1, 0, size, all_memory_regions(fd1),
					  DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM,
					  DRM_XE_GEM_CPU_CACHING_WC);

	dma_buf_fd = prime_handle_to_fd(fd1, dst_handle);
	handle_import = prime_fd_to_handle(fd2, dma_buf_fd);

	vm = xe_vm_create(fd2, 0, 0);

	/*
	 * Try with coherent and incoherent PAT index modes. Since this is self
	 * import we should have the original cpu_caching tracked (wc) in the
	 * KMD.
	 */
	igt_assert_eq(__xe_vm_bind(fd2, vm, 0, handle_import, 0, 0x40000,
				   size, DRM_XE_VM_BIND_OP_MAP, 0, NULL, 0, 0,
				   intel_get_pat_idx_uc(fd2), 0),
		      0);
	xe_vm_unbind_sync(fd2, vm, 0, 0x40000, size);

	igt_assert_eq(__xe_vm_bind(fd2, vm, 0, handle_import, 0, 0x40000,
				   size, DRM_XE_VM_BIND_OP_MAP, 0, NULL, 0, 0,
				   intel_get_pat_idx_wb(fd2), 0),
		      0);
	xe_vm_unbind_sync(fd2, vm, 0, 0x40000, size);

	xe_vm_destroy(fd2, vm);

	/*
	 * And finally verify that we can do a full blit operation, using an
	 * uncached and potentially incoherent PAT index, using the imported
	 * object as the dst.
	 */

	src_handle = xe_bo_create_caching(fd2, 0, size, system_memory(fd2), 0,
					  DRM_XE_GEM_CPU_CACHING_WB);

	p.fd = fd2;
	p.size = &mode_size;

	p.r1 = all_memory_regions(p.fd);
	p.r1_bo = src_handle;
	p.r1_map = xe_bo_map(p.fd, p.r1_bo, size);
	p.r1_pat_index = intel_get_pat_idx_wb(p.fd);

	p.r2 = all_memory_regions(p.fd);
	p.r2_bo = handle_import;
	p.r2_map = xe_bo_map(p.fd, p.r2_bo, size);
	p.r2_pat_index = intel_get_pat_idx_uc(p.fd);

	pat_index_blt(&p);

	close(dma_buf_fd);
	gem_close(fd1, dst_handle);

	gem_close(fd2, src_handle);

	drm_close_driver(fd1);
	drm_close_driver(fd2);
}

/**
 * SUBTEST: prime-external-import-coh
 * Test category: functionality test
 * Description: Check prime import from different device.
 */

static void prime_external_import_coh(void)
{
	uint32_t handle_import, src_handle;
	struct xe_pat_param p = {};
	struct xe_pat_size_mode mode_size = {
		.width = 1024,
		.height = 512,
	};
	struct vgem_bo vgem_bo = {};
	int fd1, fd2;
	int dma_buf_fd;
	int bpp = 32;
	int size = mode_size.width * mode_size.height * bpp / 8;
	uint32_t vm;

	fd1 = drm_open_driver(DRIVER_VGEM);
	fd2 = drm_open_driver(DRIVER_XE);

	vgem_bo.width = mode_size.width;
	vgem_bo.height = mode_size.height;
	vgem_bo.bpp = bpp;
	vgem_create(fd1, &vgem_bo);

	dma_buf_fd = prime_handle_to_fd(fd1, vgem_bo.handle);
	handle_import = prime_fd_to_handle(fd2, dma_buf_fd);

	vm = xe_vm_create(fd2, 0, 0);

	/*
	 * Try with coherent and incoherent PAT index modes. Since this is
	 * external import we have no concept of cpu_caching, hence we should
	 * require 1way+ when choosing the PAT index mode.
	 */
	igt_assert_eq(__xe_vm_bind(fd2, vm, 0, handle_import, 0, 0x40000,
				   size, DRM_XE_VM_BIND_OP_MAP, 0, NULL, 0, 0,
				   intel_get_pat_idx_uc(fd2), 0),
		      -EINVAL);
	xe_vm_unbind_sync(fd2, vm, 0, 0x40000, size);

	igt_assert_eq(__xe_vm_bind(fd2, vm, 0, handle_import, 0, 0x40000,
				   size, DRM_XE_VM_BIND_OP_MAP, 0, NULL, 0, 0,
				   intel_get_pat_idx_wb(fd2), 0),
		      0);
	xe_vm_unbind_sync(fd2, vm, 0, 0x40000, size);

	xe_vm_destroy(fd2, vm);

	/*
	 * And finally verify that we can do a full blit operation, using
	 * coherent PAT index, where the imported object is the dst.
	 */

	src_handle = xe_bo_create_caching(fd2, 0, size, system_memory(fd2), 0,
					  DRM_XE_GEM_CPU_CACHING_WB);

	p.fd = fd2;
	p.size = &mode_size;

	p.r1 = system_memory(p.fd);
	p.r1_bo = src_handle;
	p.r1_map = xe_bo_map(p.fd, p.r1_bo, size);
	p.r1_pat_index = intel_get_pat_idx_wb(p.fd);

	p.r2 = system_memory(p.fd);
	p.r2_bo = handle_import;
	p.r2_map = vgem_mmap(fd1, &vgem_bo, PROT_WRITE);
	p.r2_pat_index = intel_get_pat_idx_wb(p.fd);

	pat_index_blt(&p);

	close(dma_buf_fd);

	drm_close_driver(fd1);
	drm_close_driver(fd2);
}

static bool has_no_compression_hint(int fd)
{
	struct drm_xe_query_config *config = xe_config(fd);

	return config->info[DRM_XE_QUERY_CONFIG_FLAGS] &
		DRM_XE_QUERY_CONFIG_FLAG_HAS_NO_COMPRESSION_HINT;
}

/**
 * SUBTEST: bo-comp-disable-bind
 * Test category: functionality test
 * Description: Validates that binding a BO created with
 * the NO_COMPRESSION flag using a compressed PAT index fails
 * with -EINVAL on Xe2+ platforms.
 */

static void bo_comp_disable_bind(int fd)
{
	size_t size = xe_get_default_alignment(fd);
	uint8_t comp_pat_index, uncomp_pat_index;
	bool supported;
	uint32_t vm, bo;
	int ret;

	supported = has_no_compression_hint(fd);
	ret = __xe_bo_create_caching(fd, 0, size, vram_if_possible(fd, 0),
				     DRM_XE_GEM_CREATE_FLAG_NO_COMPRESSION,
				     DRM_XE_GEM_CPU_CACHING_WC, &bo);

	if (!supported) {
		igt_assert_neq(ret, 0);
		igt_skip("Missing NO_COMPRESSION support, skipping.\n");
	}

	igt_assert_eq(ret, 0);
	vm = xe_vm_create(fd, 0, 0);

	comp_pat_index = intel_get_pat_idx_uc_comp(fd);
	uncomp_pat_index = intel_get_pat_idx_uc(fd);

	igt_assert_eq(__xe_vm_bind(fd, vm, 0, bo, 0, 0x100000,
				   size, 0, 0, NULL, 0,
				   0, comp_pat_index, 0),
		      -EINVAL);

	igt_assert_eq(__xe_vm_bind(fd, vm, 0, bo, 0, 0x100000,
				   size, 0, 0, NULL, 0,
				   0, uncomp_pat_index, 0),
		      0);
	xe_vm_unbind_sync(fd, vm, 0, 0x100000, size);

	gem_close(fd, bo);

	xe_vm_destroy(fd, vm);
}

/**
 * SUBTEST: display-vs-wb-transient
 * Test category: functionality test
 * Description: Scanout with dirty L3:XD
 */
static void display_vs_wb_transient(int fd)
{
	uint16_t pat_index_modes[] = {
		3, /* UC (baseline) */
		6, /* L3:XD (uncompressed) */
	};
	uint32_t devid = intel_get_drm_devid(fd);
	igt_render_copyfunc_t render_copy = NULL;
	igt_crc_t ref_crc = {}, crc = {};
	igt_plane_t *primary;
	igt_display_t display;
	igt_output_t *output;
	igt_pipe_crc_t *pipe_crc;
	drmModeModeInfoPtr mode;
	struct intel_bb *ibb;
	struct buf_ops *bops;
	struct igt_fb src_fb, dst_fb;
	struct intel_buf src, dst;
	igt_crtc_t *crtc;
	int bpp = 32;
	int i;

	igt_require(intel_get_device_info(devid)->graphics_ver >= 20);

	render_copy = igt_get_render_copyfunc(fd);
	igt_require(render_copy);
	igt_require(xe_has_engine_class(fd, DRM_XE_ENGINE_CLASS_RENDER));

	igt_display_require(&display, fd);
	igt_display_require_output(&display);
	kmstest_set_vt_graphics_mode();

	bops = buf_ops_create(fd);
	ibb = intel_bb_create(fd, SZ_4K);

	for_each_crtc_with_valid_output(&display, crtc, output) {
		igt_display_reset(&display);

		igt_output_set_crtc(output,
				    crtc);
		if (!intel_pipe_output_combo_valid(&display))
			continue;

		mode = igt_output_get_mode(output);
		pipe_crc = igt_crtc_crc_new(crtc,
					    IGT_PIPE_CRC_SOURCE_AUTO);
		break;
	}

	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);

	igt_create_fb(fd, mode->hdisplay, mode->vdisplay,
		      DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR, &src_fb);
	igt_draw_fill_fb(fd, &src_fb, 0xFF);

	igt_plane_set_fb(primary, &src_fb);
	igt_assert_eq(igt_display_commit2(&display, COMMIT_ATOMIC), 0);
	igt_pipe_crc_collect_crc(pipe_crc, &ref_crc);

	intel_buf_init_full(bops, src_fb.gem_handle, &src, src_fb.width,
			    src_fb.height, bpp, 0, I915_TILING_NONE,
			    I915_COMPRESSION_NONE, src_fb.size,
			    src_fb.strides[0], 0, DEFAULT_PAT_INDEX,
			    DEFAULT_MOCS_INDEX);

	for (i = 0; i < ARRAY_SIZE(pat_index_modes); i++) {
		int fw_handle;

		igt_create_fb(fd, mode->hdisplay, mode->vdisplay,
			      DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR, &dst_fb);

		intel_buf_init_full(bops, dst_fb.gem_handle, &dst,
				    dst_fb.width, dst_fb.height, bpp, 0,
				    I915_TILING_NONE, I915_COMPRESSION_NONE,
				    dst_fb.size, dst_fb.strides[0], 0,
				    pat_index_modes[i],
				    intel_get_defer_to_pat_mocs_index(fd));

		/* c0 -> c6 might flush caches */
		fw_handle = igt_debugfs_open(fd, "forcewake_all", O_RDONLY);
		igt_assert_lte(0, fw_handle);

		render_copy(ibb,
			    &src,
			    0, 0, dst_fb.width, dst_fb.height,
			    &dst,
			    0, 0);
		intel_bb_sync(ibb);

		/*
		 * Display engine is not coherent with GPU/CPU caches, however
		 * with new L3:XD caching mode, the GPU cache entries marked as
		 * transient should be automatically flushed by the time we do
		 * the flip.
		 */

		igt_plane_set_fb(primary, &dst_fb);
		igt_assert_eq(igt_display_commit2(&display, COMMIT_ATOMIC), 0);
		igt_pipe_crc_collect_crc(pipe_crc, &crc);

		igt_assert_crc_equal(&crc, &ref_crc);

		intel_bb_reset(ibb, false);

		intel_buf_close(dst.bops, &dst);
		igt_remove_fb(fd, &dst_fb);
		close(fw_handle);
	}

	igt_remove_fb(fd, &src_fb);
	intel_bb_destroy(ibb);
}

static uint8_t get_pat_idx_uc(int fd, bool *compressed)
{
	if (compressed)
		*compressed = false;

	return intel_get_pat_idx_uc(fd);
}

static uint8_t get_pat_idx_wt(int fd, bool *compressed)
{
	uint16_t dev_id = intel_get_drm_devid(fd);

	if (compressed)
		*compressed = intel_get_device_info(dev_id)->graphics_ver >= 20;

	return intel_get_pat_idx_wt(fd);
}

static uint8_t get_pat_idx_wb(int fd, bool *compressed)
{
	if (compressed)
		*compressed = false;

	return intel_get_pat_idx_wb(fd);
}

struct pat_index_entry {
	uint8_t (*get_pat_index)(int fd, bool *compressed);
	uint8_t pat_index;
	bool    compressed;

	const char *name;
	uint16_t   coh_mode;
	bool       force_cpu_wc;
};

/*
 * The common modes are available on all platforms supported by Xe and so should
 * be commonly supported. There are many more possible pat_index modes, however
 * most IGTs shouldn't really care about them so likely no need to add them to
 * lib/intel_pat.c. We do try to test some on the non-common modes here.
 */
#define XE_COMMON_PAT_INDEX_MODES \
	{ get_pat_idx_uc, 0, 0, "uc",        XE_COH_NONE                }, \
	{ get_pat_idx_wt, 0, 0, "wt",        XE_COH_NONE                }, \
	{ get_pat_idx_wb, 0, 0, "wb",        XE_COH_AT_LEAST_1WAY       }, \
	{ get_pat_idx_wb, 0, 0, "wb-cpu-wc", XE_COH_AT_LEAST_1WAY, true }  \

const struct pat_index_entry xelp_pat_index_modes[] = {
	XE_COMMON_PAT_INDEX_MODES,

	{ NULL, 1, false, "wc", XE_COH_NONE },
};

const struct pat_index_entry xehpc_pat_index_modes[] = {
	XE_COMMON_PAT_INDEX_MODES,

	{ NULL, 1, false, "wc",    XE_COH_NONE          },
	{ NULL, 4, false, "c1-wt", XE_COH_NONE          },
	{ NULL, 5, false, "c1-wb", XE_COH_AT_LEAST_1WAY },
	{ NULL, 6, false, "c2-wt", XE_COH_NONE          },
	{ NULL, 7, false, "c2-wb", XE_COH_AT_LEAST_1WAY },
};

const struct pat_index_entry xelpg_pat_index_modes[] = {
	XE_COMMON_PAT_INDEX_MODES,

	{ NULL, 0, false, "wb-none",             XE_COH_NONE                },
	{ NULL, 3, false, "1way",                XE_COH_AT_LEAST_1WAY       },
	{ NULL, 4, false, "2way-atomics",        XE_COH_AT_LEAST_1WAY       },
	{ NULL, 4, false, "2way-atomics-cpu-wc", XE_COH_AT_LEAST_1WAY, true },
};

const struct pat_index_entry xe2_pat_index_modes[] = {
	XE_COMMON_PAT_INDEX_MODES,

	/* Too many, just pick some of the interesting ones */
	{ NULL, 1,  false, "1way",        XE_COH_AT_LEAST_1WAY       },
	{ NULL, 2,  false, "2way",        XE_COH_AT_LEAST_1WAY       },
	{ NULL, 2,  false, "2way-cpu-wc", XE_COH_AT_LEAST_1WAY, true },
	{ NULL, 5,  false, "uc-1way",     XE_COH_AT_LEAST_1WAY       },
	{ NULL, 12, true,  "uc-comp",     XE_COH_NONE                },
	{ NULL, 31, false, "c3-2way",     XE_COH_AT_LEAST_1WAY       },
};

const struct pat_index_entry bmg_g21_pat_index_modes[] = {
	XE_COMMON_PAT_INDEX_MODES,

	/* Too many, just pick some of the interesting ones */
	{ NULL, 1,  false, "1way",        XE_COH_AT_LEAST_1WAY       },
	{ NULL, 2,  false, "2way",        XE_COH_AT_LEAST_1WAY       },
	{ NULL, 2,  false, "2way-cpu-wc", XE_COH_AT_LEAST_1WAY, true },
	{ NULL, 5,  false, "uc-1way",     XE_COH_AT_LEAST_1WAY       },
	{ NULL, 12, true,  "uc-comp",     XE_COH_NONE                },
	{ NULL, 27, false, "c2-2way",     XE_COH_AT_LEAST_1WAY       },
};

/*
 * Depending on 2M/1G GTT pages we might trigger different PTE layouts for the
 * PAT bits, so make sure we test with and without huge-pages. Also ensure we
 * have a mix of different pat_index modes for each PDE.
 */
const struct xe_pat_size_mode size_modes[] =  {
	{ 256,  256, 0,        "mixed-pde"  }, /* 256K */
	{ 1024, 512, 1u << 21, "single-pde" }, /* 2M and hopefully 2M GTT page */
};

typedef void (*copy_fn)(struct xe_pat_param *p);

const struct xe_pat_copy_mode {
	copy_fn    fn;
	const char *name;
} copy_modes[] =  {
	{ pat_index_dw,     "dw"     },
	{ pat_index_blt,    "blt"    },
	{ pat_index_render, "render" },
};

static uint32_t create_object(int fd, int r, int size, uint16_t coh_mode,
			      bool force_cpu_wc)
{
	uint32_t flags;
	uint16_t cpu_caching;

	flags = 0;
	if (r != system_memory(fd))
		flags |= DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM;

	if (coh_mode == XE_COH_AT_LEAST_1WAY && r == system_memory(fd) &&
	    !force_cpu_wc)
		cpu_caching = DRM_XE_GEM_CPU_CACHING_WB;
	else
		cpu_caching = DRM_XE_GEM_CPU_CACHING_WC;

	return xe_bo_create_caching(fd, 0, size, r, flags, cpu_caching);
}

/**
 * SUBTEST: pat-index-xelp
 * Test category: functionality test
 * Description: Check some of the xelp pat_index modes.
 */

/**
 * SUBTEST: pat-index-xehpc
 * Test category: functionality test
 * Description: Check some of the xehpc pat_index modes.
 */

/**
 * SUBTEST: pat-index-xelpg
 * Test category: functionality test
 * Description: Check some of the xelpg pat_index modes.
 */

/**
 * SUBTEST: pat-index-xe2
 * Test category: functionality test
 * Description: Check some of the xe2 pat_index modes.
 */

static void subtest_pat_index_modes_with_regions(int fd,
						 const struct pat_index_entry *modes_arr,
						 int n_modes)
{
	struct igt_collection *copy_set;
	struct igt_collection *pat_index_set;
	struct igt_collection *regions_set;
	struct igt_collection *sizes_set;
	struct igt_collection *copies;
	struct xe_pat_param p = {};

	p.fd = fd;

	copy_set = igt_collection_create(ARRAY_SIZE(copy_modes));

	pat_index_set = igt_collection_create(n_modes);

	regions_set = xe_get_memory_region_set(fd,
					       DRM_XE_MEM_REGION_CLASS_SYSMEM,
					       DRM_XE_MEM_REGION_CLASS_VRAM);

	sizes_set = igt_collection_create(ARRAY_SIZE(size_modes));

	for_each_variation_r(copies, 1, copy_set) {
		struct igt_collection *regions;
		struct xe_pat_copy_mode copy_mode;

		copy_mode = copy_modes[igt_collection_get_value(copies, 0)];

		igt_dynamic_f("%s", copy_mode.name) {
			for_each_variation_r(regions, 2, regions_set) {
				struct igt_collection *pat_modes;
				uint32_t r1, r2;
				char *reg_str;

				r1 = igt_collection_get_value(regions, 0);
				r2 = igt_collection_get_value(regions, 1);

				reg_str = xe_memregion_dynamic_subtest_name(fd, regions);

				for_each_variation_r(pat_modes, 2, pat_index_set) {
					struct igt_collection *sizes;
					struct pat_index_entry r1_entry, r2_entry;
					int r1_idx, r2_idx;

					r1_idx = igt_collection_get_value(pat_modes, 0);
					r2_idx = igt_collection_get_value(pat_modes, 1);

					r1_entry = modes_arr[r1_idx];
					r2_entry = modes_arr[r2_idx];

					if (r1_entry.get_pat_index) {
						p.r1_pat_index = r1_entry.get_pat_index(fd, &p.r1_compressed);
					} else {
						p.r1_pat_index = r1_entry.pat_index;
						p.r1_compressed = r1_entry.compressed;
					}

					if (r2_entry.get_pat_index)
						p.r2_pat_index = r2_entry.get_pat_index(fd, &p.r2_compressed);
					else {
						p.r2_pat_index = r2_entry.pat_index;
						p.r2_compressed = r2_entry.compressed;
					}

					p.r1 = r1;
					p.r2 = r2;

					for_each_variation_r(sizes, 1, sizes_set) {
						int size_mode_idx = igt_collection_get_value(sizes, 0);
						int bpp = 32;
						int size;

						p.size = &size_modes[size_mode_idx];

						size = p.size->width * p.size->height * bpp / 8;
						p.r1_bo = create_object(fd, p.r1, size, r1_entry.coh_mode,
									r1_entry.force_cpu_wc);
						p.r1_map = xe_bo_map(fd, p.r1_bo, size);

						p.r2_bo = create_object(fd, p.r2, size, r2_entry.coh_mode,
									r2_entry.force_cpu_wc);
						p.r2_map = xe_bo_map(fd, p.r2_bo, size);

						igt_debug("[r1]: r: %u, idx: %u (%s), coh: %u, wc: %d, comp: %d\n",
							  p.r1, p.r1_pat_index, r1_entry.name, r1_entry.coh_mode,
							  r1_entry.force_cpu_wc, p.r1_compressed);
						igt_debug("[r2]: r: %u, idx: %u (%s), coh: %u, wc: %d, comp: %d, w: %u, h: %u, a: %u\n",
							  p.r2, p.r2_pat_index, r2_entry.name, r2_entry.coh_mode,
							  r1_entry.force_cpu_wc, p.r2_compressed,
							  p.size->width, p.size->height,
							  p.size->alignment);

						copy_mode.fn(&p);

						munmap(p.r1_map, size);
						munmap(p.r2_map, size);

						gem_close(fd, p.r1_bo);
						gem_close(fd, p.r2_bo);
					}
				}

				free(reg_str);
			}
		}
	}
}

static int opt_handler(int opt, int opt_index, void *data)
{
	switch (opt) {
	case 'V':
		do_slow_check = true;
		break;
	default:
		return IGT_OPT_HANDLER_ERROR;
	}

	return IGT_OPT_HANDLER_SUCCESS;
}

const char *help_str =
	"  -V\tVerify every dword (might be slow)\n";

int igt_main_args("V", NULL, help_str, opt_handler, NULL)
{
	uint16_t dev_id;
	int fd;

	igt_fixture() {
		uint32_t seed;

		fd = drm_open_driver(DRIVER_XE);
		dev_id = intel_get_drm_devid(fd);

		seed = time(NULL);
		srand(seed);
		igt_debug("seed: %d\n", seed);

		xe_device_get(fd);
	}

	igt_subtest("pat-sanity")
		pat_sanity(fd);

	igt_subtest("pat-index-all")
		pat_index_all(fd);

	igt_subtest("userptr-coh-none")
		userptr_coh_none(fd);

	igt_subtest("prime-self-import-coh")
		prime_self_import_coh();

	igt_subtest("prime-external-import-coh")
		prime_external_import_coh();

	igt_subtest("bo-comp-disable-bind")
		bo_comp_disable_bind(fd);

	igt_subtest_with_dynamic("pat-index-xelp") {
		igt_require(intel_graphics_ver(dev_id) <= IP_VER(12, 55));
		subtest_pat_index_modes_with_regions(fd, xelp_pat_index_modes,
						     ARRAY_SIZE(xelp_pat_index_modes));
	}

	igt_subtest_with_dynamic("pat-index-xehpc") {
		igt_require(IS_PONTEVECCHIO(dev_id));
		subtest_pat_index_modes_with_regions(fd, xehpc_pat_index_modes,
						     ARRAY_SIZE(xehpc_pat_index_modes));
	}

	igt_subtest_with_dynamic("pat-index-xelpg") {
		igt_require(IS_METEORLAKE(dev_id));
		subtest_pat_index_modes_with_regions(fd, xelpg_pat_index_modes,
						     ARRAY_SIZE(xelpg_pat_index_modes));
	}

	igt_subtest_with_dynamic("pat-index-xe2") {
		igt_require(intel_get_device_info(dev_id)->graphics_ver >= 20);
		igt_assert(HAS_FLATCCS(dev_id));

		if (intel_graphics_ver(dev_id) == IP_VER(20, 1))
			subtest_pat_index_modes_with_regions(fd, bmg_g21_pat_index_modes,
							     ARRAY_SIZE(bmg_g21_pat_index_modes));
		else
			subtest_pat_index_modes_with_regions(fd, xe2_pat_index_modes,
							     ARRAY_SIZE(xe2_pat_index_modes));
	}

	igt_subtest("display-vs-wb-transient")
		display_vs_wb_transient(fd);

	igt_fixture()
		drm_close_driver(fd);
}
