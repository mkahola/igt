/*
 * Copyright © 2019 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/**
 * TEST: kms prime
 * Category: Display
 * Description: Prime tests, focusing on KMS side
 * Driver requirement: i915, xe
 * Mega feature: General Display Features
 */

#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <time.h>

#include "igt.h"
#include "igt_device.h"
#include "igt_debugfs.h"
#include "igt_sysfs.h"

#include "lib/intel_blt.h"
#include "lib/intel_common.h"
#include "lib/intel_pat.h"
#include "lib/intel_mocs.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"

/**
 * SUBTEST: D3hot
 * Description: Validate pci state of dGPU when dGPU is idle and  scanout is on iGPU
 *
 * SUBTEST: basic-modeset-hybrid
 * Description: Basic modeset on the one device when the other device is active
 *
 * SUBTEST: basic-crc-%s
 * Description: Make a dumb color buffer, export to another device and compare
 *              the CRCs with a buffer native to that device
 *
 * arg[1]:
 *
 * @hybrid:
 * @vgem:
 */

#define KMS_HELPER "/sys/module/drm_kms_helper/parameters/"
#define KMS_POLL_DISABLE 0

bool kms_poll_saved_state;
bool kms_poll_disabled;

struct dumb_bo {
	uint32_t handle;
	uint32_t width, height;
	uint32_t bpp, pitch;
	uint64_t size;
};

struct crc_info {
	igt_crc_t crc;
	char *str;
	const char *name;
};

static struct {
	double r, g, b;
	uint32_t color;
	struct crc_info prime_crc, direct_crc;
} colors[3] = {
	{ .r = 0.0, .g = 0.0, .b = 0.0, .color = 0xff000000 },
	{ .r = 1.0, .g = 1.0, .b = 1.0, .color = 0xffffffff },
	{ .r = 1.0, .g = 0.0, .b = 0.0, .color = 0xffff0000 },
};

IGT_TEST_DESCRIPTION("Prime tests, focusing on KMS side");

static bool has_prime_import(int fd)
{
	uint64_t value;

	if (drmGetCap(fd, DRM_CAP_PRIME, &value))
		return false;

	return value & DRM_PRIME_CAP_IMPORT;
}

static bool has_prime_export(int fd)
{
	uint64_t value;

	if (drmGetCap(fd, DRM_CAP_PRIME, &value))
		return false;

	return value & DRM_PRIME_CAP_EXPORT;
}

static igt_output_t *setup_display(int importer_fd, igt_display_t *display,
				   enum pipe *pipe)
{
	igt_output_t *output;
	bool found = false;

	for_each_pipe_with_valid_output(display, *pipe, output) {
		igt_display_reset(display);

		igt_output_set_pipe(output, *pipe);
		if (intel_pipe_output_combo_valid(display)) {
			found = true;
			break;
		}
	}

	igt_require_f(found, "No valid connector/pipe found\n");

	return output;
}

static uint32_t *create_and_map_dumb_bo(int fd, struct dumb_bo *bo,
					uint32_t width, uint32_t height,
					uint32_t bpp, uint32_t *pitch,
					uint64_t *size)
{
	uint32_t *ptr;
	const uint32_t alloc_w = ALIGN(width, 256);

	bo->height = height;
	bo->bpp = bpp;
	bo->handle = kmstest_dumb_create(fd, alloc_w,
					 bo->height, bo->bpp, pitch, size);

	ptr = kmstest_dumb_map_buffer(fd, bo->handle, *size, PROT_READ | PROT_WRITE);

	return ptr;
}

static uint32_t *prepare_xe_dgfx_scratch(int exporter_fd, struct dumb_bo *scratch)
{
	uint32_t *ptr;
	struct blt_copy_data ex_blt = {};
	struct blt_copy_object *src = NULL;

	igt_debug("Preparing scratch buffer for DGfx exporter\n");

	blt_copy_init(exporter_fd, &ex_blt);
	src = blt_create_object(&ex_blt, DRM_XE_MEM_REGION_CLASS_VRAM,
				scratch->width, scratch->height,
				scratch->bpp, 0, T_LINEAR,
				COMPRESSION_DISABLED, 0, true);
	scratch->handle = src->handle;
	scratch->size = src->size;
	scratch->pitch = src->pitch;
	ptr = xe_bo_mmap_ext(exporter_fd, scratch->handle, scratch->size,
			     PROT_READ | PROT_WRITE);

	return ptr;
}

static void prepare_scratch(int exporter_fd, struct dumb_bo *scratch,
			    drmModeModeInfo *mode, uint32_t color)
{
	uint32_t *ptr;
	bool is_dgfx;

	scratch->width = mode->hdisplay;
	scratch->height = mode->vdisplay;
	scratch->bpp = 32;

	if (drm_get_chipset(exporter_fd) == DRIVER_VGEM)
		is_dgfx = false;
	else
		is_dgfx = is_intel_dgfx(exporter_fd);

	if (is_xe_device(exporter_fd) && is_dgfx) {
		ptr = prepare_xe_dgfx_scratch(exporter_fd, scratch);
	} else if (is_i915_device(exporter_fd)) {
		struct igt_fb fb;

		igt_init_fb(&fb, exporter_fd, mode->hdisplay, mode->vdisplay,
			    DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR,
			    IGT_COLOR_YCBCR_BT709, IGT_COLOR_YCBCR_LIMITED_RANGE);
		igt_calc_fb_size(&fb);

		scratch->size = fb.size;
		scratch->pitch = fb.strides[0];

		if (gem_has_lmem(exporter_fd))
			scratch->handle = gem_create_in_memory_regions(exporter_fd, scratch->size,
								       REGION_LMEM(0), REGION_SMEM);
		else
			scratch->handle = gem_create_in_memory_regions(exporter_fd, scratch->size,
								       REGION_SMEM);

		ptr = gem_mmap__device_coherent(exporter_fd, scratch->handle, 0, scratch->size,
						PROT_WRITE | PROT_READ);
	} else {
		ptr = create_and_map_dumb_bo(exporter_fd, scratch,
					     scratch->width, scratch->height,
					     scratch->bpp, &scratch->pitch,
					     &scratch->size);
	}

	for (size_t idx = 0; idx < scratch->size / sizeof(*ptr); ++idx)
		ptr[idx] = color;

	munmap(ptr, scratch->size);
}

static void prepare_fb(int importer_fd, struct dumb_bo *scratch, struct igt_fb *fb)
{
	enum igt_color_encoding color_encoding = IGT_COLOR_YCBCR_BT709;
	enum igt_color_range color_range = IGT_COLOR_YCBCR_LIMITED_RANGE;

	igt_init_fb(fb, importer_fd, scratch->width, scratch->height,
		    DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR,
		    color_encoding, color_range);
}

static void import_fb(int importer_fd, struct igt_fb *fb,
		      int dmabuf_fd, struct dumb_bo *scratch)
{
	uint32_t offsets[4] = {}, pitches[4] = {}, handles[4] = {}, temp_buf_handle;
	int ret;

	igt_debug("Importing FB: importer_fd=%d, dmabuf_fd=%d, width=%u, height=%u\n",
		  importer_fd, dmabuf_fd, fb->width, fb->height);

	if (is_intel_dgfx(importer_fd)) {
		uint64_t fb_size = 0;
		uint64_t ahnd = 0;

		temp_buf_handle = prime_fd_to_handle(importer_fd, dmabuf_fd);
		igt_assert(temp_buf_handle > 0);
		fb->gem_handle = igt_create_bo_with_dimensions(importer_fd, fb->width,
							       fb->height, fb->drm_format,
							       fb->modifier, scratch->pitch,
							       &fb_size, NULL, NULL);
		igt_assert(fb->gem_handle > 0);

		if (is_i915_device(importer_fd)) {
			ahnd = get_reloc_ahnd(importer_fd, 0);

			igt_blitter_src_copy(importer_fd, ahnd, 0, NULL, temp_buf_handle,
						 0, scratch->pitch, fb->modifier,
						 0, 0, fb_size, fb->width,
						 fb->height, 32, fb->gem_handle,
						 0, scratch->pitch, fb->modifier,
						 0, 0, fb_size);

			gem_sync(importer_fd, fb->gem_handle);
			gem_close(importer_fd, temp_buf_handle);
			put_ahnd(ahnd);
		}  else if (is_xe_device(importer_fd)) {
			uint32_t xe_bb;
			struct blt_copy_data blt = {0};
			struct blt_copy_object *src, *dst;
			uint32_t vm, xe_exec;
			intel_ctx_t *xe_ctx;
			uint64_t bb_size;

			struct drm_xe_engine_class_instance inst = {
				.engine_class = DRM_XE_ENGINE_CLASS_COPY,
			};

			vm = xe_vm_create(importer_fd, 0, 0);
			xe_exec = xe_exec_queue_create(importer_fd, vm, &inst, 0);
			xe_ctx = intel_ctx_xe(importer_fd, vm, xe_exec, 0, 0, 0);
			ahnd = intel_allocator_open_full(importer_fd, xe_ctx->vm, 0, 0,
							 INTEL_ALLOCATOR_SIMPLE,
							 ALLOC_STRATEGY_LOW_TO_HIGH, 0);
			bb_size = xe_bb_size(importer_fd, SZ_4K);
			igt_calc_fb_size(fb);

			fb->gem_handle = xe_bo_create(importer_fd, 0, fb->size,
						      vram_if_possible(importer_fd, 0),
						      DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
			igt_require(fb->gem_handle);

			blt_copy_init(importer_fd, &blt);

			src = blt_create_object(&blt, vram_if_possible(importer_fd, 0),
						scratch->width, scratch->height, 32, 0,
						T_LINEAR, COMPRESSION_DISABLED, 0, true);
			blt_set_object(src, temp_buf_handle, scratch->size,
				       vram_if_possible(importer_fd, 0), 0,
				       DEFAULT_PAT_INDEX,
				       T_LINEAR, COMPRESSION_DISABLED, 0);

			dst = blt_create_object(&blt, vram_if_possible(importer_fd, 0),
						scratch->width, scratch->height, 32, 0,
						T_LINEAR, COMPRESSION_DISABLED, 0, true);
			blt_set_object(dst, fb->gem_handle, fb->size,
				       vram_if_possible(importer_fd, 0), 0,
				       DEFAULT_PAT_INDEX,
				       T_LINEAR, COMPRESSION_DISABLED, 0);

			blt.color_depth = CD_32bit;
			blt_set_copy_object(&blt.src, src);
			blt_set_copy_object(&blt.dst, dst);

			xe_bb = xe_bo_create(importer_fd, 0, bb_size,
					     vram_if_possible(importer_fd, 0),
					     DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);

			blt_set_batch(&blt.bb, xe_bb, bb_size, vram_if_possible(importer_fd, 0));
			blt_fast_copy(importer_fd, xe_ctx, NULL, ahnd, &blt);

			put_offset(ahnd, dst->handle);
			put_offset(ahnd, src->handle);
			put_offset(ahnd, xe_bb);

			xe_exec_queue_destroy(importer_fd, xe_exec);
			xe_vm_destroy(importer_fd, vm);
			blt_destroy_object(importer_fd, src);
			gem_close(importer_fd, xe_bb);
			intel_allocator_close(ahnd);
			intel_ctx_destroy(importer_fd, xe_ctx);
		}
	} else {
		fb->gem_handle = prime_fd_to_handle(importer_fd, dmabuf_fd);
		igt_assert(fb->gem_handle > 0);
	}

	handles[0] = fb->gem_handle;
	pitches[0] = scratch->pitch;
	offsets[0] = 0;

	ret = drmModeAddFB2(importer_fd, fb->width, fb->height,
			    DRM_FORMAT_XRGB8888,
			    handles, pitches, offsets,
			    &fb->fb_id, 0);
	igt_assert_eq(ret, 0);
}

static void set_fb(struct igt_fb *fb,
		   igt_display_t *display,
		   igt_output_t *output)
{
	igt_plane_t *primary;
	int ret;

	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_assert(primary);

	igt_plane_set_fb(primary, fb);
	ret = igt_display_commit(display);

	igt_assert_eq(ret, 0);
}

static void collect_crc_for_fb(int importer_fd, struct igt_fb *fb, igt_display_t *display,
			       igt_output_t *output, igt_pipe_crc_t *pipe_crc,
			       uint32_t color, struct crc_info *info)
{
	set_fb(fb, display, output);
	igt_pipe_crc_collect_crc(pipe_crc, &info->crc);
	info->str = igt_crc_to_string(&info->crc);
	igt_debug("CRC through '%s' method for %#08x is %s\n",
		  info->name, color, info->str);
	igt_remove_fb(importer_fd, fb);
}

static void test_crc(int exporter_fd, int importer_fd)
{
	igt_display_t display;
	igt_output_t *output;
	igt_pipe_crc_t *pipe_crc;
	enum pipe pipe;
	struct igt_fb fb;
	int dmabuf_fd;
	struct dumb_bo scratch = {};
	bool crc_equal;
	int i, j;
	drmModeModeInfo *mode;

	igt_device_set_master(importer_fd);
	igt_require_pipe_crc(importer_fd);
	igt_display_require(&display, importer_fd);

	output = setup_display(importer_fd, &display, &pipe);

	mode = igt_output_get_mode(output);
	pipe_crc = igt_pipe_crc_new(importer_fd, pipe,
				    IGT_PIPE_CRC_SOURCE_AUTO);

	for (i = 0; i < ARRAY_SIZE(colors); i++) {
		prepare_scratch(exporter_fd, &scratch, mode, colors[i].color);
		dmabuf_fd = prime_handle_to_fd(exporter_fd, scratch.handle);
		gem_close(exporter_fd, scratch.handle);

		prepare_fb(importer_fd, &scratch, &fb);
		import_fb(importer_fd, &fb, dmabuf_fd, &scratch);
		close(dmabuf_fd);

		colors[i].prime_crc.name = "prime";
		collect_crc_for_fb(importer_fd, &fb, &display, output,
				   pipe_crc, colors[i].color, &colors[i].prime_crc);
		igt_create_color_fb(importer_fd,
				    mode->hdisplay, mode->vdisplay,
				    DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR,
				    colors[i].r, colors[i].g, colors[i].b,
				    &fb);

		colors[i].direct_crc.name = "direct";
		collect_crc_for_fb(importer_fd, &fb, &display, output,
				   pipe_crc, colors[i].color, &colors[i].direct_crc);
	}
	igt_pipe_crc_free(pipe_crc);

	igt_debug("CRC table:\n");
	igt_debug("Color\t\tPrime\t\tDirect\n");
	for (i = 0; i < ARRAY_SIZE(colors); i++) {
		igt_debug("%#08x\t%.8s\t%.8s\n", colors[i].color,
			  colors[i].prime_crc.str, colors[i].direct_crc.str);
		free(colors[i].prime_crc.str);
		free(colors[i].direct_crc.str);
	}

	for (i = 0; i < ARRAY_SIZE(colors); i++) {
		for (j = 0; j < ARRAY_SIZE(colors); j++) {
			if (i == j) {
				igt_assert_crc_equal(&colors[i].prime_crc.crc,
						     &colors[j].direct_crc.crc);
				continue;
			}
			crc_equal = igt_check_crc_equal(&colors[i].prime_crc.crc,
							&colors[j].direct_crc.crc);
			igt_assert_f(!crc_equal, "CRC should be different");
		}
	}
	igt_display_fini(&display);
}

static void test_basic_modeset(int drm_fd)
{
	igt_display_t display;
	igt_output_t *output;
	enum pipe pipe;
	drmModeModeInfo *mode;
	struct igt_fb fb;
	uint32_t bo;
	int ret;
	uint32_t offsets[4] = { 0 };

	igt_device_set_master(drm_fd);
	igt_display_require(&display, drm_fd);

	output = setup_display(drm_fd, &display, &pipe);
	mode = igt_output_get_mode(output);
	igt_assert(mode);

	if (is_xe_device(drm_fd) && xe_has_vram(drm_fd)) {
		uint32_t strides[4] = { ALIGN(mode->hdisplay * 4, 64) };

		igt_info("Doing modeset on discrete\n");

		igt_init_fb(&fb, drm_fd, mode->hdisplay, mode->vdisplay,
			    DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR,
			    IGT_COLOR_YCBCR_BT709, IGT_COLOR_YCBCR_LIMITED_RANGE);
		igt_calc_fb_size(&fb);

		bo =  xe_bo_create(drm_fd, 0, fb.size, vram_if_possible(drm_fd, 0),
				   DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
		igt_require(bo);

		ret = __kms_addfb(drm_fd, bo,
				  mode->hdisplay, mode->vdisplay,
				  DRM_FORMAT_XRGB8888,
				  DRM_FORMAT_MOD_LINEAR,
				  strides, offsets, 1,
				  DRM_MODE_FB_MODIFIERS, &fb.fb_id);
		igt_assert_eq(ret, 0);

		set_fb(&fb, &display, output);
		gem_close(drm_fd, bo);

		cairo_surface_destroy(fb.cairo_surface);
		do_or_die(drmModeRmFB(drm_fd, fb.fb_id));

		igt_display_fini(&display);
		igt_info("Modeset on discrete done\n");
		return;
	}

	igt_create_pattern_fb(drm_fd, mode->hdisplay, mode->vdisplay, DRM_FORMAT_XRGB8888,
			      DRM_FORMAT_MOD_LINEAR, &fb);

	set_fb(&fb, &display, output);
	igt_remove_fb(drm_fd, &fb);
	igt_display_fini(&display);
}

static bool has_connected_output(int drm_fd)
{
	igt_display_t display;
	igt_output_t *output;

	igt_device_set_master(drm_fd);
	igt_display_require(&display, drm_fd);

	for_each_connected_output(&display, output)
		return true;

	return false;
}

static void validate_d3_hot(int drm_fd)
{
	igt_assert(igt_debugfs_search(drm_fd, "i915_runtime_pm_status", "GPU idle: yes"));
	igt_assert(igt_debugfs_search(drm_fd, "i915_runtime_pm_status", "PCI device power state: D3hot [3]"));
}

static void kms_poll_state_restore(void)
{
	int sysfs_fd;

	igt_assert_lte(0, (sysfs_fd = open(KMS_HELPER, O_RDONLY)));
	__igt_sysfs_set_boolean(sysfs_fd, "poll", kms_poll_saved_state);
	close(sysfs_fd);

}

static void kms_poll_disable(void)
{
	int sysfs_fd;

	igt_require((sysfs_fd = open(KMS_HELPER, O_RDONLY)) >= 0);
	kms_poll_saved_state = igt_sysfs_get_boolean(sysfs_fd, "poll");
	igt_sysfs_set_boolean(sysfs_fd, "poll", KMS_POLL_DISABLE);
	kms_poll_disabled = true;
	close(sysfs_fd);
}

igt_main
{
	int first_fd = -1;
	int second_fd_vgem = -1;
	int second_fd_hybrid = -1;
	bool first_output, second_output;

	igt_fixture() {
		kmstest_set_vt_graphics_mode();
		/* ANY = anything that is not VGEM */
		first_fd = __drm_open_driver_another(0, DRIVER_ANY);
		igt_require(first_fd >= 0);
		first_output = has_connected_output(first_fd);
	}

	igt_describe("Hybrid GPU subtests");
	igt_subtest_group() {
		igt_fixture() {
			second_fd_hybrid = __drm_open_driver_another(1, DRIVER_ANY);
			igt_require(second_fd_hybrid >= 0);
			second_output = has_connected_output(second_fd_hybrid);
		}

		igt_describe("Hybrid GPU: Make a dumb color buffer, export to another device and"
			     " compare the CRCs with a buffer native to that device");
		igt_subtest_with_dynamic("basic-crc-hybrid") {
			if (has_prime_export(first_fd) &&
			    has_prime_import(second_fd_hybrid) && second_output)
				igt_dynamic("first-to-second")
					test_crc(first_fd, second_fd_hybrid);

			if (has_prime_import(first_fd) &&
			    has_prime_export(second_fd_hybrid) && first_output)
				igt_dynamic("second-to-first")
					test_crc(second_fd_hybrid, first_fd);
		}

		igt_describe("Basic modeset on the one device when the other device is active");
		igt_subtest_with_dynamic("basic-modeset-hybrid") {
			igt_require(second_fd_hybrid >= 0);
			if (first_output) {
				igt_dynamic("first")
					test_basic_modeset(first_fd);
			}

			if (second_output) {
				igt_dynamic("second")
					test_basic_modeset(second_fd_hybrid);
			}
		}

		igt_describe("Validate pci state of dGPU when dGPU is idle and  scanout is on iGPU");
		igt_subtest("D3hot") {
			igt_require_f(is_i915_device(second_fd_hybrid), "i915 device required\n");
			igt_require_f(gem_has_lmem(second_fd_hybrid), "Second GPU is not dGPU\n");
			igt_require_f(first_output, "No display connected to iGPU\n");
			igt_require_f(!second_output, "Display connected to dGPU\n");

			kms_poll_disable();

			igt_set_timeout(10, "Wait for dGPU to enter D3hot before starting the subtest");
			while (!igt_debugfs_search(second_fd_hybrid,
			       "i915_runtime_pm_status",
			       "PCI device power state: D3hot [3]"));
			igt_reset_timeout();

			test_basic_modeset(first_fd);
			validate_d3_hot(second_fd_hybrid);
		}

		igt_fixture() {
			if (kms_poll_disabled)
				kms_poll_state_restore();

			drm_close_driver(second_fd_hybrid);
		}
	}

	igt_describe("VGEM subtests");
	igt_subtest_group() {
		igt_fixture() {
			second_fd_vgem = __drm_open_driver_another(1, DRIVER_VGEM);
			igt_require(second_fd_vgem >= 0);
			if (is_i915_device(first_fd))
				igt_require(!gem_has_lmem(first_fd));
			if (is_xe_device(first_fd))
				igt_require(!xe_has_vram(first_fd));
		}

		igt_describe("Make a dumb color buffer, export to another device and"
			     " compare the CRCs with a buffer native to that device");
		igt_subtest_with_dynamic("basic-crc-vgem") {
			if (has_prime_import(first_fd) &&
			    has_prime_export(second_fd_vgem) && first_output)
				igt_dynamic("second-to-first")
					test_crc(second_fd_vgem, first_fd);
		}

		igt_fixture()
			drm_close_driver(second_fd_vgem);
	}

	igt_fixture()
		drm_close_driver(first_fd);
}
