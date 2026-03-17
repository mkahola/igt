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
#include "igt_configfs.h"
#include "igt_device.h"
#include "igt_fs.h"
#include "igt_kmod.h"
#include "igt_map.h"
#include "igt_syncobj.h"
#include "igt_sysfs.h"
#include "igt_vgem.h"
#include "intel_blt.h"
#include "intel_mocs.h"
#include "intel_pat.h"
#include "linux_scaffold.h"

#include "xe/xe_gt.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"
#include "xe/xe_util.h"

#define XE_COH_NONE          1
#define XE_COH_AT_LEAST_1WAY 2

/*
 * PAT index 18: XA (eXclusive Access) + UC (Uncached).
 */
#define XE_PAT_IDX_XA_UC     18

static bool do_slow_check;
static char bus_addr[NAME_MAX];
static struct pci_device *pci_dev;

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
	igt_assert_eq(__xe_vm_bind(fd, vm, 0, 0, to_user_pointer(data), 0x40000,
				   size, DRM_XE_VM_BIND_OP_MAP_USERPTR, 0, NULL, 0, 0,
				   XE_PAT_IDX_XA_UC, 0),
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

/* Pre-Xe2 PAT bit fields (from kernel xe_pat.c) */
#define XELP_MEM_TYPE_MASK	GENMASK(1, 0)

static bool pat_entry_is_uc(unsigned int gfx_ver, uint32_t pat)
{
	if (gfx_ver >= IP_VER(20, 0))
		return REG_FIELD_GET(XE2_L3_POLICY, pat) == L3_CACHE_POLICY_UC &&
		       REG_FIELD_GET(XE2_L4_POLICY, pat) == L4_CACHE_POLICY_UC;

	if (gfx_ver >= IP_VER(12, 70))
		return REG_FIELD_GET(XE2_L4_POLICY, pat) == L4_CACHE_POLICY_UC;

	return REG_FIELD_GET(XELP_MEM_TYPE_MASK, pat) == 0;
}

static bool pat_entry_is_wb(unsigned int gfx_ver, uint32_t pat)
{
	if (gfx_ver >= IP_VER(20, 0)) {
		uint32_t l3 = REG_FIELD_GET(XE2_L3_POLICY, pat);

		return l3 == L3_CACHE_POLICY_WB || l3 == L3_CACHE_POLICY_XD;
	}

	if (gfx_ver >= IP_VER(12, 70))
		return REG_FIELD_GET(XE2_L4_POLICY, pat) == L4_CACHE_POLICY_WB;

	return REG_FIELD_GET(XELP_MEM_TYPE_MASK, pat) == 3;
}

static bool pat_entry_is_wt(unsigned int gfx_ver, uint32_t pat)
{
	if (gfx_ver >= IP_VER(20, 0))
		return REG_FIELD_GET(XE2_L3_POLICY, pat) == L3_CACHE_POLICY_XD &&
		       REG_FIELD_GET(XE2_L4_POLICY, pat) == L4_CACHE_POLICY_WT;

	if (gfx_ver >= IP_VER(12, 70))
		return REG_FIELD_GET(XE2_L4_POLICY, pat) == L4_CACHE_POLICY_WT;

	return REG_FIELD_GET(XELP_MEM_TYPE_MASK, pat) == 2;
}

static bool pat_entry_is_compressed(unsigned int gfx_ver, uint32_t pat)
{
	if (gfx_ver < IP_VER(20, 0))
		return false;

	return !!(pat & XE2_COMP_EN);
}

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
	unsigned int gfx_ver = intel_graphics_ver(dev_id);
	struct intel_pat_cache pat_sw_config = {};
	int32_t parsed;
	bool has_uc_comp = false, has_wt = false;

	parsed = xe_fetch_pat_sw_config(fd, &pat_sw_config);

	if (gfx_ver >= IP_VER(20, 0)) {
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

	/*
	 * Validate that the selected PAT indices actually have the expected
	 * cache types rather than comparing against hardcoded values.
	 */
	igt_assert_f(pat_entry_is_uc(gfx_ver, pat_sw_config.entries[pat_sw_config.uc].pat),
		     "UC index %d does not point to an uncached entry (pat=%#x)\n",
		     pat_sw_config.uc, pat_sw_config.entries[pat_sw_config.uc].pat);
	igt_assert_f(pat_entry_is_wb(gfx_ver, pat_sw_config.entries[pat_sw_config.wb].pat),
		     "WB index %d does not point to a WB/XA/XD entry (pat=%#x)\n",
		     pat_sw_config.wb, pat_sw_config.entries[pat_sw_config.wb].pat);
	if (has_wt)
		igt_assert_f(pat_entry_is_wt(gfx_ver, pat_sw_config.entries[pat_sw_config.wt].pat),
			     "WT index %d does not point to a WT entry (pat=%#x)\n",
			     pat_sw_config.wt, pat_sw_config.entries[pat_sw_config.wt].pat);
	if (has_uc_comp) {
		uint32_t uc_comp_pat = pat_sw_config.entries[pat_sw_config.uc_comp].pat;

		igt_assert_f(pat_entry_is_compressed(gfx_ver, uc_comp_pat) &&
			     pat_entry_is_uc(gfx_ver, uc_comp_pat),
			     "UC_COMP index %d does not point to a compressed UC entry (pat=%#x)\n",
			     pat_sw_config.uc_comp, uc_comp_pat);
	}
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
	uint16_t dev_id = intel_get_drm_devid(fd);
	uint8_t mocs_index;
	int i;

	igt_require(blt_has_fast_copy(fd));
	mocs_index = intel_get_device_info(dev_id)->graphics_ver >= 20 ?
		     intel_get_defer_to_pat_mocs_index(fd) : intel_get_uc_mocs_index(fd);

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

	blt_set_object(&src, p->r1_bo, size, p->r1, mocs_index,
		       p->r1_pat_index, T_LINEAR,
		       COMPRESSION_DISABLED, COMPRESSION_TYPE_3D);
	blt_set_geom(&src, stride, 0, 0, width, height, 0, 0);

	blt_set_object(&dst, p->r2_bo, size, p->r2, mocs_index,
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
	uint16_t dev_id = intel_get_drm_devid(fd);
	uint8_t mocs_index;
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

	mocs_index = intel_get_device_info(dev_id)->graphics_ver >= 20 ?
		     intel_get_defer_to_pat_mocs_index(fd) : DEFAULT_MOCS_INDEX;

	bops = buf_ops_create(fd);

	ibb = intel_bb_create_full(fd, 0, 0, NULL, xe_get_default_alignment(fd),
				   0, 0, p->size->alignment,
				   INTEL_ALLOCATOR_SIMPLE,
				   ALLOC_STRATEGY_HIGH_TO_LOW, vram_if_possible(fd, 0));

	size = width * height * bpp / 8;
	stride = width * 4;

	intel_buf_init_full(bops, p->r1_bo, &src, width, height, bpp, 0,
			    I915_TILING_NONE, I915_COMPRESSION_NONE, size,
			    stride, p->r1, p->r1_pat_index, mocs_index);

	intel_buf_init_full(bops, p->r2_bo, &dst, width, height, bpp, 0,
			    I915_TILING_NONE, I915_COMPRESSION_NONE, size,
			    stride, p->r2, p->r2_pat_index, mocs_index);

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
	uint16_t dev_id = intel_get_drm_devid(fd);
	uint8_t mocs_index;
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

	mocs_index = intel_get_device_info(dev_id)->graphics_ver >= 20 ?
		     intel_get_defer_to_pat_mocs_index(fd) : DEFAULT_MOCS_INDEX;

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
			    stride, p->r1, p->r1_pat_index, mocs_index);
	intel_bb_add_intel_buf(ibb, &r1_buf, true);

	intel_buf_init_full(bops, p->r2_bo, &r2_buf, width, height, bpp, 0,
			    I915_TILING_NONE, I915_COMPRESSION_NONE, size,
			    stride, p->r2, p->r2_pat_index, mocs_index);

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
 * SUBTEST: l2-flush-opt-svm-pat-restrict
 * Test category: negative test
 * Description: Validate that on L2 flush optimized platforms, SVM
 *		(CPU_ADDR_MIRROR) mappings only accept pat_index 19
 *		(XA+UC+1WAY) and reject all other pat indices with -EINVAL.
 */
static void l2_flush_opt_svm_pat_restrict(int fd)
{
	struct drm_xe_query_config *config = xe_config(fd);
	uint32_t vm;
	void *buffer;
	size_t alloc_size = xe_get_default_alignment(fd);
	struct xe_device *xe_dev = xe_device_get(fd);
	uint64_t svm_size;
	size_t size = xe_get_default_alignment(fd);

	svm_size = 1ull << xe_dev->va_bits;
	vm = xe_vm_create(fd, DRM_XE_VM_CREATE_FLAG_LR_MODE |
			  DRM_XE_VM_CREATE_FLAG_FAULT_MODE, 0);
	xe_vm_bind_lr_sync(fd, vm, 0, 0, 0, svm_size,
			   DRM_XE_VM_BIND_FLAG_CPU_ADDR_MIRROR);
	buffer = aligned_alloc(alloc_size, SZ_2M);

	igt_require(config->info[DRM_XE_QUERY_CONFIG_FLAGS] &
		    DRM_XE_QUERY_CONFIG_FLAG_HAS_CPU_ADDR_MIRROR);

	igt_assert_eq(__xe_vm_madvise(fd, vm, to_user_pointer(buffer), size,
					0, DRM_XE_MEM_RANGE_ATTR_PAT,
					intel_get_pat_idx_wb(fd), 0, 0), 0);

	igt_assert_eq(__xe_vm_madvise(fd, vm, to_user_pointer(buffer), size,
					0, DRM_XE_MEM_RANGE_ATTR_PAT,
					intel_get_pat_idx_uc(fd), 0, 0), -EINVAL);

	igt_assert_eq(__xe_vm_madvise(fd, vm, to_user_pointer(buffer), size,
					0, DRM_XE_MEM_RANGE_ATTR_PAT,
					intel_get_pat_idx_wt(fd), 0, 0), -EINVAL);

	igt_assert_eq(__xe_vm_madvise(fd, vm, to_user_pointer(buffer), size,
					0, DRM_XE_MEM_RANGE_ATTR_PAT,
					XE_PAT_IDX_XA_UC, 0, 0), -EINVAL);

	free(buffer);
	xe_vm_unbind_lr_sync(fd, vm, 0, 0, svm_size);
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

	igt_assert_eq(__xe_vm_bind(fd2, vm, 0, handle_import, 0, 0x40000,
				   size, DRM_XE_VM_BIND_OP_MAP, 0, NULL, 0, 0,
				   XE_PAT_IDX_XA_UC, 0),
		      -EINVAL);

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
 * with -EINVAL on Xe2+ platforms. On platforms where CCS
 * does not exist, the test verifies uncompressed access works.
 */

static void bo_comp_disable_bind(int fd)
{
	size_t size = xe_get_default_alignment(fd);
	uint16_t dev_id = intel_get_drm_devid(fd);
	bool has_flatccs = HAS_FLATCCS(dev_id);
	uint8_t uncomp_pat_index;
	uint32_t vm, bo;
	bool supported;
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

	uncomp_pat_index = intel_get_pat_idx_uc(fd);

	/*
	 * On platforms with CCS, binding a NO_COMPRESSION BO with a
	 * compressed PAT index must fail. On platforms without CCS,
	 * there is no valid compressed PAT index, so skip this check.
	 */
	if (has_flatccs) {
		uint8_t comp_pat_index = intel_get_pat_idx_uc_comp(fd);

		igt_assert_eq(__xe_vm_bind(fd, vm, 0, bo, 0, 0x100000,
					   size, 0, 0, NULL, 0,
					   0, comp_pat_index, 0),
			      -EINVAL);
	} else {
		igt_debug("Platform has no CCS, skipping compressed PAT bind check\n");
	}

	/* Uncompressed bind must always succeed */
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

const struct pat_index_entry xe3p_lpg_coherency_pat_index_modes[] = {
	{ NULL, 18, false, "xa-l3-uc",	 XE_COH_NONE          },
	{ NULL, 19, false, "xa-l3-1way", XE_COH_AT_LEAST_1WAY },
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

/**
 * SUBTEST: xa-app-transient-media-off
 * Test category: functionality test
 * Description: Check some of the xe4-lpg pat_index modes with media off.
 */

/**
 * SUBTEST:  xa-app-transient-media-on
 * Test category: functionality test
 * Description: Check some of the xe3p-lpg pat_index modes with media on.
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

struct fs_pat_entry {
	uint8_t pat_index;
	const char *name;
	uint16_t cpu_caching;
	bool exp_result;
};

const struct fs_pat_entry fs_xe2_integrated[] = {
	{ 2, "cpu-wb-gpu-l3-2way", DRM_XE_GEM_CPU_CACHING_WB, true },
	{ 3, "cpu-wc-gpu-uc-non-coh", DRM_XE_GEM_CPU_CACHING_WC, false },
	{ 5, "cpu-wb-gpu-uc-1way", DRM_XE_GEM_CPU_CACHING_WB, false },
};

const struct fs_pat_entry fs_xe2_discrete[] = {
	{ 2, "cpu-wb-gpu-l3-2way", DRM_XE_GEM_CPU_CACHING_WB, true },
	{ 3, "cpu-wc-gpu-uc-non-coh", DRM_XE_GEM_CPU_CACHING_WC, true },
	{ 5, "cpu-wb-gpu-uc-1way", DRM_XE_GEM_CPU_CACHING_WB, true },
};

const struct fs_pat_entry fs_xe3[] = {
	{ 2, "cpu-wb-gpu-l3-2way", DRM_XE_GEM_CPU_CACHING_WB, true },
	{ 3, "cpu-wc-gpu-uc-non-coh", DRM_XE_GEM_CPU_CACHING_WC, true },
	{ 5, "cpu-wb-gpu-uc-1way", DRM_XE_GEM_CPU_CACHING_WB, true },
};

const struct fs_pat_entry fs_xe3p_xpc[] = {
	{ 2, "cpu-wb-gpu-l3-2way", DRM_XE_GEM_CPU_CACHING_WB, true },
	{ 3, "cpu-wc-gpu-uc-non-coh", DRM_XE_GEM_CPU_CACHING_WC, true },
	{ 4, "cpu-wb-gpu-uc-1way", DRM_XE_GEM_CPU_CACHING_WB, true },
};

const struct fs_pat_entry fs_xe3p_lpg[] = {
	{ 2, "cpu-wb-gpu-l3-2way", DRM_XE_GEM_CPU_CACHING_WB, true },
	{ 18, "cpu-wc-gpu-xa-non-coh", DRM_XE_GEM_CPU_CACHING_WC, true },
	{ 19, "cpu-wb-gpu-xa-1way", DRM_XE_GEM_CPU_CACHING_WB, true },
};

#define CPUDW_INC   0x0
#define GPUDW_WRITE 0x4
#define GPUDW_READY 0x40
#define READY_VAL   0xabcd
#define FINISH_VAL  0x0bae

static void __false_sharing(int fd, const struct fs_pat_entry *fs_entry)
{
	size_t size = xe_get_default_alignment(fd), bb_size;
	uint32_t vm, exec_queue, bo, bb, *map, *batch;
	struct drm_xe_engine_class_instance *hwe;
	struct drm_xe_sync sync = {
	    .type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL,
	};
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 1,
		.syncs = to_user_pointer(&sync),
	};
	uint64_t addr = 0x40000;
	uint64_t bb_addr = 0x100000;
	uint32_t loops = 0x0, gpu_exp_value;
	uint32_t region = system_memory(fd);
	int loop_addr, i = 0;
	int pat_index = fs_entry->pat_index;
	int inc_idx, write_idx, ready_idx;
	bool result;

	inc_idx = CPUDW_INC / sizeof(*map);
	write_idx = GPUDW_WRITE / sizeof(*map);
	ready_idx = GPUDW_READY / sizeof(*map);

	vm = xe_vm_create(fd, 0, 0);

	bo = xe_bo_create_caching(fd, 0, size, region, 0, fs_entry->cpu_caching);
	map = xe_bo_map(fd, bo, size);

	bb_size = xe_bb_size(fd, SZ_4K);
	bb = xe_bo_create(fd, 0, bb_size, region, 0);
	batch = xe_bo_map(fd, bb, bb_size);

	sync.handle = syncobj_create(fd, 0);
	igt_assert_eq(__xe_vm_bind(fd, vm, 0, bo, 0, addr,
				   size, DRM_XE_VM_BIND_OP_MAP, 0, &sync, 1, 0,
				   pat_index, 0),
			0);
	igt_assert_eq(syncobj_wait_err(fd, &sync.handle, 1, INT64_MAX, 0), 0);

	syncobj_reset(fd, &sync.handle, 1);
	igt_assert_eq(__xe_vm_bind(fd, vm, 0, bb, 0, bb_addr,
				   bb_size, DRM_XE_VM_BIND_OP_MAP, 0, &sync, 1, 0,
				   DEFAULT_PAT_INDEX, 0),
			0);
	igt_assert_eq(syncobj_wait_err(fd, &sync.handle, 1, INT64_MAX, 0), 0);

	/* Unblock cpu wait */
	batch[i++] = MI_STORE_DWORD_IMM_GEN4;
	batch[i++] = addr + GPUDW_READY;
	batch[i++] = addr >> 32;
	batch[i++] = READY_VAL;

	/* Unblock after cpu started to spin */
	batch[i++] = MI_SEMAPHORE_WAIT_CMD | MI_SEMAPHORE_POLL |
		     MI_SEMAPHORE_SAD_NEQ_SDD | (4 - 2);
	batch[i++] = 0;
	batch[i++] = addr + CPUDW_INC;
	batch[i++] = addr >> 32;

	loop_addr = i;
	batch[i++] = MI_STORE_DWORD_IMM_GEN4;
	batch[i++] = addr + GPUDW_WRITE;
	batch[i++] = addr >> 32;
	batch[i++] = READY_VAL;

	batch[i++] = MI_COND_BATCH_BUFFER_END | MI_DO_COMPARE | MAD_EQ_IDD | 2;
	batch[i++] = READY_VAL;
	batch[i++] = addr + GPUDW_READY;
	batch[i++] = addr >> 32;

	batch[i++] = MI_BATCH_BUFFER_START | 1 << 8 | 1;
	batch[i++] = bb_addr + loop_addr * sizeof(uint32_t);
	batch[i++] = bb_addr >> 32;

	batch[i++] = MI_BATCH_BUFFER_END;

	xe_for_each_engine(fd, hwe)
		break;

	exec_queue = xe_exec_queue_create(fd, vm, hwe, 0);
	exec.exec_queue_id = exec_queue;
	exec.address = bb_addr;
	syncobj_reset(fd, &sync.handle, 1);
	xe_exec(fd, &exec);

	while(READ_ONCE(map[ready_idx]) != READY_VAL);

	igt_until_timeout(2) {
		WRITE_ONCE(map[inc_idx], map[inc_idx] + 1);
		loops++;
	}

	WRITE_ONCE(map[ready_idx], FINISH_VAL);

	igt_assert_eq(syncobj_wait_err(fd, &sync.handle, 1, INT64_MAX, 0), 0);

	igt_debug("[%d]: %08x (cpu) [loops: %08x] | [%d]: %08x (gpu) | [%d]: %08x (ready)\n",
		  inc_idx, map[inc_idx], loops, write_idx, map[write_idx],
		  ready_idx, map[ready_idx]);

	result = map[inc_idx] == loops;
	gpu_exp_value = map[ready_idx];
	igt_debug("got: %d, expected: %d\n", result, fs_entry->exp_result);

	xe_vm_unbind_sync(fd, vm, 0, addr, size);
	xe_vm_unbind_sync(fd, vm, 0, bb_addr, bb_size);
	gem_munmap(batch, bb_size);
	gem_munmap(map, size);
	gem_close(fd, bo);
	gem_close(fd, bb);

	xe_vm_destroy(fd, vm);

	igt_assert_eq(result, fs_entry->exp_result);
	igt_assert_eq(gpu_exp_value, FINISH_VAL);
}

/**
 * SUBTEST: false-sharing
 * Test category: functionality test
 * Description: Check cache line coherency on 1way/coh_none
 */

static void false_sharing(int fd)
{
	uint16_t dev_id = intel_get_drm_devid(fd);
	uint32_t graphics_ver = intel_get_device_info(dev_id)->graphics_ver;
	bool is_dgfx = xe_has_vram(fd);

	const struct fs_pat_entry *fs_entries;
	int num_entries;

	if (intel_graphics_ver(dev_id) == IP_VER(35, 11)) {
		num_entries = ARRAY_SIZE(fs_xe3p_xpc);
		fs_entries = fs_xe3p_xpc;
	} else if (intel_graphics_ver(dev_id) == IP_VER(35, 10)) {
		num_entries = ARRAY_SIZE(fs_xe3p_lpg);
		fs_entries = fs_xe3p_lpg;
	} else if (graphics_ver == 20) {
		if (is_dgfx) {
			num_entries = ARRAY_SIZE(fs_xe2_discrete);
			fs_entries = fs_xe2_discrete;
		} else {
			num_entries = ARRAY_SIZE(fs_xe2_integrated);
			fs_entries = fs_xe2_integrated;
		}
	} else {
		num_entries = ARRAY_SIZE(fs_xe3);
		fs_entries = fs_xe3;
	}

	for (int i = 0; i < num_entries; i++) {
		igt_dynamic_f("%s", fs_entries[i].name) {
			__false_sharing(fd, &fs_entries[i]);
		}
	}
}

static void reset(int sig)
{
	int configfs_fd;

	igt_kmod_unbind("xe", bus_addr);

	/* Drop all custom configfs settings from subtests */
	configfs_fd = igt_configfs_open("xe");
	if (configfs_fd >= 0)
		igt_fs_remove_dir(configfs_fd, bus_addr);
	close(configfs_fd);

	/* Bind again a clean driver with no custom settings */
	igt_kmod_bind("xe", bus_addr);
}

static void xa_app_transient_test(int configfs_device_fd, bool media_on)
{
	int fd, fw_handle, gt;

	igt_kmod_unbind("xe", bus_addr);

	if (media_on)
		igt_assert(igt_sysfs_set(configfs_device_fd,
					 "gt_types_allowed", "primary,media"));
	else
		igt_assert(igt_sysfs_set(configfs_device_fd,
					 "gt_types_allowed", "primary"));

	igt_kmod_bind("xe", bus_addr);

	fd = drm_open_driver(DRIVER_XE);

	/* Prevent entering C6 for the duration of the test, since this can result
	 * in randomly flushing the entire device side caches, invalidating our XA
	 * testing.
	 */
	fw_handle = igt_debugfs_open(fd, "forcewake_all", O_RDONLY);
	igt_require(fw_handle >= 0);

	subtest_pat_index_modes_with_regions(fd, xe3p_lpg_coherency_pat_index_modes,
					     ARRAY_SIZE(xe3p_lpg_coherency_pat_index_modes));

	/* check status of c state, it should not be in c6 due to forcewake. */
	xe_for_each_gt(fd, gt)
		igt_assert(!xe_gt_is_in_c6(fd, gt));

	close(fw_handle);
}

/**
 * SUBTEST: pt-caching
 * Description: verify pt fetch doesn't trigger pagefaults on TLB eviction
 *              (use many objects)
 *
 * SUBTEST: pt-caching-single-object
 * Description: verify pt fetch doesn't trigger pagefaults on TLB eviction
 *              (use single object bound with different offsets)
 *
 * SUBTEST: pt-caching-random-offsets
 * Description: verify pt fetch doesn't trigger pagefaults on TLB eviction
 *              (use many objects with random offsets)
 *
 * SUBTEST: pt-caching-update-pat-and-pte
 * Description: verify read after write returns expected values when
 *              NULL and normal object PTEs exists in same cacheline
 *
 */

/*
 * Helpers for spreading over pagetables
 *
 *      E         D         C         B         A
 * +- 9bits -+- 9bits -+- 9bits -+- 9bits -+- 9bits -+
 * |876543210|876543210|876543210|876543210|876543210|
 *
 * index is spread over n-groups
 *
 * (example) index = 876543210 (bits, not value)
 *
 * for 3-group bit shift will land
 *      C         B         A
 * |000000852|000000741|000000630|
 *
 * for 4-group bit shift will land
 *      D         C         B         A
 * |000000073|000000062|000000051|000000840|
 *
 * for 5-group bit shift will land
 *      E         D         C         B         A
 * |000000004|000000083|000000072|000000061|000000050|
 */

enum pt_groups {
	GROUPS_3s = 3,
	GROUPS_4s,
	GROUPS_5s,
};

enum pt_test_opts {
	PT_SINGLE_OBJECT = 1,
	PT_RANDOM_OFFSETS = 2,
	PT_UPDATE_PAT_AND_PTE = 3,
};

static uint64_t get_every_nth_bit(uint64_t v, int nth)
{
	uint64_t ret = 0;
	int i = 0;

	while (v) {
		ret |= (v & 1) << i;
		v >>= nth;
		i++;
	}

	return ret;
}

static uint64_t pt_spread(uint64_t nr, enum pt_groups groups)
{
	uint64_t ret_pt = 0;

	switch (groups) {
	case GROUPS_5s:
		ret_pt |= get_every_nth_bit(nr >> 4, groups) << 36;
	case GROUPS_4s:
		ret_pt |= get_every_nth_bit(nr >> 3, groups) << 27;
	case GROUPS_3s:
		ret_pt |= get_every_nth_bit(nr >> 2, groups) << 18 |
			  get_every_nth_bit(nr >> 1, groups) << 9 |
			  get_every_nth_bit(nr, groups);
		break;
	}

	return ret_pt;
}

struct pt_object {
	uint32_t bo;
	uint32_t size;
	uint64_t address;
	uint64_t offset;
	uint32_t dword;
	uint32_t flags;
};

/* Batch address, selected low to avoid being placed in 48-57 range */
#define BATCH_ADDRESS 0x10000

/* Start address for all allocations */
#define OFFSET_START 0x200000

static struct pt_object *
pt_create_objects(int xe, int num_objs, enum pt_test_opts opts, enum pt_groups groups,
		  uint64_t addr_shift)
{
	struct pt_object *objs;
	uint64_t obj_size = SZ_4K;
	uint64_t va_mask = (1ULL << xe_va_bits(xe)) - 1;
	struct igt_map *hash = NULL;
	int i, max_randoms;
	uint32_t region = system_memory(xe), vram_region = vram_memory(xe, 0);
	uint32_t flags = 0;

	objs = calloc(num_objs, sizeof(*objs));
	igt_assert(objs);

	if (xe_has_vram(xe) && xe_min_page_size(xe, vram_region) == SZ_4K) {
		region = vram_region;
		flags = DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM;
	}

	if (opts == PT_RANDOM_OFFSETS)
		hash = igt_map_create(igt_map_hash_64, igt_map_equal_64);

	for (i = 0; i < num_objs; i++) {
		if (opts == PT_RANDOM_OFFSETS) {
			uint64_t addr, *paddr;

			objs[i].bo = xe_bo_create(xe, 0, obj_size, region, flags);
			objs[i].size = obj_size;

			max_randoms = 16; /* arbitrary, very unlikely to exceed */
			while (--max_randoms) {
				addr = (uint64_t)rand() << 32 | rand();
				addr &= ~((1 << 21) - 1) & va_mask;
				if (addr > addr + OFFSET_START)
					continue; /* avoid wrap around */
				addr += OFFSET_START;
				paddr = igt_map_search(hash, &addr);
				if (!paddr)
					break;
				else
					igt_debug("Address 0x%lx already found, randomizing again\n",
						  addr);
			}
			igt_assert_neq(max_randoms, 0);
			igt_map_insert(hash, &addr, from_user_pointer(i));
			objs[i].address = addr;
		} else if (opts == PT_SINGLE_OBJECT) {
			if (!i) {
				uint64_t total_size = num_objs * obj_size;

				objs[i].bo = xe_bo_create(xe, 0, total_size,
							  region, flags);
				objs[i].size = total_size;
			} else {
				objs[i].bo = objs[0].bo;
			}
			objs[i].offset = i * obj_size;
			objs[i].address = (pt_spread(i, groups) << 21) +
					  OFFSET_START + addr_shift;
		} else {
			objs[i].bo = xe_bo_create_caching(xe, 0, obj_size, region, flags,
							  DRM_XE_GEM_CPU_CACHING_WC);
			objs[i].size = obj_size;
			objs[i].address = (pt_spread(i, groups) << 21) +
					  OFFSET_START + addr_shift;
		}
		objs[i].dword = i + 1 + addr_shift;

		igt_debug("object[%d]: [bo: %u, size: 0x%x, offset: 0x%lx, address: 0x%lx]\n",
			  i, objs[i].bo, objs[i].size, objs[i].offset, objs[i].address);
	}

	igt_map_destroy(hash, NULL);

	return objs;
}

static void pt_destroy_objects(int xe, struct pt_object *objs, int num_objs)
{
	int i;

	for (i = 0; i < num_objs; i++) {
		if (objs[i].size)
			gem_close(xe, objs[i].bo);
		else
			break;
	}
	free(objs);
}

static void pt_bind_objects(int xe, uint32_t vm, struct pt_object *objs,
			    int num_objs, uint32_t flags, uint8_t pat_index)
{
	struct drm_xe_sync sync = {
		.type = DRM_XE_SYNC_TYPE_SYNCOBJ,
		.flags = DRM_XE_SYNC_FLAG_SIGNAL,
	};
	uint32_t obj_size = SZ_4K;
	int i;

	for (i = 0; i < num_objs; i++) {
		igt_debug("i: %x, address: 0x%lx, offset: %lx\n",
			  i, objs[i].address, objs[i].offset);

		sync.handle = syncobj_create(xe, 0);

		igt_assert_eq(__xe_vm_bind(xe, vm, 0,
					   flags == DRM_XE_VM_BIND_FLAG_NULL ? 0 : objs[i].bo,
					   objs[i].offset,
					   objs[i].address, obj_size,
					   DRM_XE_VM_BIND_OP_MAP,
					   flags,
					   &sync, 1, 0,
					   pat_index, 0), 0);

		igt_assert(syncobj_wait(xe, &sync.handle, 1, INT64_MAX, 0, NULL));
		syncobj_destroy(xe, sync.handle);
		objs[i].flags = flags;
	}
}

static void pt_fill_objects(int xe, uint32_t vm, struct pt_object *objs,
			    int num_objs)
{
	uint64_t bb_size = num_objs * 4 * sizeof(uint32_t) + sizeof(uint32_t);
	uint64_t batch_addr = 0;
	uint32_t bb, exec_queue;
	uint32_t *batch;
	int i, n = 0;

	batch_addr = ALIGN(BATCH_ADDRESS, xe_get_default_alignment(xe));
	igt_debug("Batch offset: 0x%lx\n", batch_addr);

	bb_size = xe_bb_size(xe, bb_size);
	bb = xe_bo_create(xe, 0, bb_size, vram_if_possible(xe, 0),
			  DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
	xe_vm_bind_sync(xe, vm,	bb, 0, batch_addr, bb_size);
	batch = xe_bo_map(xe, bb, bb_size);
	for (i = 0; i < num_objs; i++) {
		batch[n++] = MI_STORE_DWORD_IMM_GEN4;
		batch[n++] = objs[i].address;
		batch[n++] = objs[i].address >> 32;
		batch[n++] = objs[i].dword;
	}
	batch[n++] = MI_BATCH_BUFFER_END;
	munmap(batch, bb_size);

	exec_queue = xe_exec_queue_create_class(xe, vm, DRM_XE_ENGINE_CLASS_COPY);
	xe_exec_wait(xe, exec_queue, batch_addr);

	xe_exec_queue_destroy(xe, exec_queue);
	xe_vm_unbind_sync(xe, vm, 0, batch_addr, bb_size);
	gem_close(xe, bb);
}

static void pt_check_objects(int xe, struct pt_object *objs, int num_objs,
			     enum pt_test_opts opts)
{
	uint32_t *map, v;
	int i;

	igt_debug("Checking objects\n");
	if (opts == PT_SINGLE_OBJECT) {
		map = xe_bo_map(xe, objs[0].bo, objs[0].size);
		for (i = 0; i < num_objs; i++) {
			v = map[0 + (i * SZ_4K / sizeof(*map))];
			igt_debug("[%d]: bo: %u, value %08x\n", i, objs[i].bo, v);
			igt_assert_eq(v, objs[i].dword);
		}
		munmap(map, objs[0].size);
	} else {
		for (i = 0; i < num_objs; i++) {
			map = xe_bo_map(xe, objs[i].bo, objs[i].size);
			v = map[0];
			igt_debug("[%d]: bo: %u, value %08x\n", i, objs[i].bo, v);

			/*
			 * CPU mapping points to object data, so after rebind
			 * from writeable to NULL GPU binding writing to it
			 * will still keep previous data in the object from
			 * CPU point of view. Clear it to ensure we don't read
			 * stale data.
			 */
			if (opts == PT_UPDATE_PAT_AND_PTE)
				memset(map, 0, objs[i].size);

			munmap(map, objs[i].size);
			if (objs[i].flags == DRM_XE_VM_BIND_FLAG_NULL)
				igt_assert_eq(v, 0);
			else
				igt_assert_eq(v, objs[i].dword);
		}
	}
}

#define FILL_TLB_SIZE SZ_1M
static void pt_caching_test(int xe, enum pt_test_opts opts)
{
	struct pt_object *objs1, *objs2;
	uint32_t vm;
	uint8_t pat_index = DEFAULT_PAT_INDEX;
	int num_objs = FILL_TLB_SIZE / 64, every, bits, bgrps, i;

	igt_require_f(xe_min_page_size(xe, system_memory(xe)) == SZ_4K,
		      "We need at least one region with 4K alignment\n");

	/*
	 * For discrete where alignment is larger than 4K fallback to system
	 * memory for objects location and decrease number of iterations.
	 */
	if (xe_get_default_alignment(xe) != SZ_4K) {
		num_objs /= 4;
	} else if (xe_has_vram(xe)) {
		/*
		 * For random offsets each 4K object may consume up to 3 or 4
		 * 4K pages for pde/pte - so divisor == 12 should be enough
		 * to keep everything in vram with some minor free space margin.
		 * For rest each 4K object has 4K pte + pde which consumes about
		 * ~7%, so divisor == 4 should cause to occupy ~82% of vram.
		 */
		int div = opts == PT_RANDOM_OFFSETS ? 12 : 4;

		num_objs = min_t(int, num_objs,
				 (xe_visible_vram_size(xe, 0) / SZ_4K / div));
	}

	vm = xe_vm_create(xe, 0, 0);
	igt_info("va_bits: %d, num_objs: %u, spread over pt levels:\n",
		 xe_va_bits(xe), num_objs);

	if (xe_va_bits(xe) > 48)
		every = 4;
	else
		every = 3;

	if (opts != PT_RANDOM_OFFSETS) {
		bits = igt_fls(num_objs - 1);
		bgrps = DIV_ROUND_UP(bits, every);
		for (i = 0; i < every; i++)
			igt_info("[%d]: %d\n", i, num_objs >> bgrps * i);
	} else {
		uint32_t seed = time(NULL);

		igt_info("-> random, seed: %u\n", seed);
		srand(seed);
	}

	objs1 = pt_create_objects(xe, num_objs, opts, every, 0);
	objs2 = pt_create_objects(xe, num_objs, opts, every, SZ_8K);

	/*
	 * Testing scenario:
	 *  - bind 4K objects or single one (using different offsets)
	 *    spreading them over different pt/pd levels
	 *  - do memory writes to all of these objects causing TLB eviction
	 *  - bind another set of 4K objects which will reside in same cache
	 *    line, but after one entry gap
	 *  - ensure writes succeed and no pagefault occur
	 */
	if (opts != PT_UPDATE_PAT_AND_PTE) {
		pt_bind_objects(xe, vm, objs1, num_objs, 0, pat_index);
		pt_fill_objects(xe, vm, objs1, num_objs);
		pt_check_objects(xe, objs1, num_objs, opts);

		pt_bind_objects(xe, vm, objs2, num_objs, 0, pat_index);
		pt_fill_objects(xe, vm, objs2, num_objs);
		pt_check_objects(xe, objs2, num_objs, opts);
	/*
	 * For PAT/PTE change we use different pat indices and
	 * we keep <NULL PTE, invalid, valid obj PTE> in same cache line.
	 * After write/read scenario we exchange the arrangement to
	 * <valid obj PTE, invalid, NULL PTE> to ensure cachelines were
	 * properly invalidated.
	 */
	} else {
		pat_index = get_pat_idx_uc(xe, NULL);
		pt_bind_objects(xe, vm, objs1, num_objs, DRM_XE_VM_BIND_FLAG_NULL, pat_index);
		pt_bind_objects(xe, vm, objs2, num_objs, 0, pat_index);
		pt_fill_objects(xe, vm, objs1, num_objs);
		pt_fill_objects(xe, vm, objs2, num_objs);
		pt_check_objects(xe, objs1, num_objs, opts);
		pt_check_objects(xe, objs2, num_objs, opts);

		pat_index = get_pat_idx_wb(xe, NULL);
		pt_bind_objects(xe, vm, objs1, num_objs, 0, pat_index);
		pt_bind_objects(xe, vm, objs2, num_objs, DRM_XE_VM_BIND_FLAG_NULL, pat_index);
		pt_fill_objects(xe, vm, objs1, num_objs);
		pt_fill_objects(xe, vm, objs2, num_objs);
		pt_check_objects(xe, objs1, num_objs, opts);
		pt_check_objects(xe, objs2, num_objs, opts);
	}

	/* Implicit vm cleanup */
	xe_vm_destroy(xe, vm);
	pt_destroy_objects(xe, objs1, num_objs);
	pt_destroy_objects(xe, objs2, num_objs);
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

	igt_subtest_with_dynamic("false-sharing") {
		igt_require(intel_get_device_info(dev_id)->graphics_ver >= 20);

		false_sharing(fd);
	}

	igt_subtest_group() {
		int configfs_fd, configfs_device_fd;

		igt_fixture() {
			igt_require(intel_graphics_ver(dev_id) == IP_VER(35, 10));

			pci_dev = igt_device_get_pci_device(fd);
			snprintf(bus_addr, sizeof(bus_addr), "%04x:%02x:%02x.%01x",
				 pci_dev->domain, pci_dev->bus, pci_dev->dev, pci_dev->func);

			configfs_fd = igt_configfs_open("xe");
			igt_require(configfs_fd != -1);
			configfs_device_fd = igt_fs_create_dir(configfs_fd, bus_addr,
							       S_IRWXU | S_IRGRP | S_IXGRP |
							       S_IROTH | S_IXOTH);
			igt_install_exit_handler(reset);
		}

		igt_subtest_with_dynamic("xa-app-transient-media-off")
			xa_app_transient_test(configfs_device_fd, false);

		igt_subtest_with_dynamic("xa-app-transient-media-on")
			xa_app_transient_test(configfs_device_fd, true);

		igt_fixture() {
			close(configfs_device_fd);
			close(configfs_fd);
		}
	}

	igt_subtest("l2-flush-opt-svm-pat-restrict") {
		igt_require(intel_graphics_ver(dev_id) == IP_VER(35, 10));
		l2_flush_opt_svm_pat_restrict(fd);
	}

	igt_subtest("pt-caching")
		pt_caching_test(fd, 0);

	igt_subtest("pt-caching-single-object")
		pt_caching_test(fd, PT_SINGLE_OBJECT);

	igt_subtest("pt-caching-random-offsets")
		pt_caching_test(fd, PT_RANDOM_OFFSETS);

	igt_subtest("pt-caching-update-pat-and-pte")
		pt_caching_test(fd, PT_UPDATE_PAT_AND_PTE);

	igt_fixture()
		drm_close_driver(fd);
}
