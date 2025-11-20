/*
 * Copyright © 2016 Intel Corporation
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
 *
 */

/**
 * TEST: kms ccs
 * Category: Display
 * Description: Test render compression (RC), in which the main surface is
 *              complemented by a color control surface (CCS) that the display
 *              uses to interpret the compressed data.
 * Driver requirement: i915, xe
 * Mega feature: E2E Compression
 */
#include <fcntl.h>

#include "igt.h"
#include "igt_halffloat.h"

#include "i915/gem_create.h"
#include "intel_pat.h"
#include "intel_blt.h"
#include "intel_mocs.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"

/**
 * SUBTEST: %s-%s
 * Description: Test %arg[1] with given %arg[2] modifier
 *
 * arg[1]:
 *
 * @bad-aux-stride:            Bad AUX stride
 * @ccs-on-another-bo:         CCS with different BO
 * @missing-ccs-buffer:        Missing CCS buffer
 *
 * arg[2]:
 *
 * @y-tiled-ccs:               Y tiled ccs
 * @y-tiled-gen12-mc-ccs:      Y tiled gen12 mc ccs
 * @y-tiled-gen12-rc-ccs:      Y tiled gen12 rc ccs
 * @y-tiled-gen12-rc-ccs-cc:   Y tiled gen12 rc ccs cc
 * @yf-tiled-ccs:              YF tiled ccs
 * @4-tiled-mtl-mc-ccs:        4 tiled mtl mc ccs
 * @4-tiled-mtl-rc-ccs:        4 tiled mtl rc ccs
 * @4-tiled-mtl-rc-ccs-cc:     4 tiled mtl rc ccs cc
 */

/**
 * SUBTEST: %s-%s
 * Description: Test %arg[1] with %arg[2] modifier
 *
 * arg[1]:
 *
 * @bad-pixel-format:            Bad pixel format
 *
 * arg[2]:
 *
 * @4-tiled-dg2-mc-ccs:        4 tiled mc ccs
 * @4-tiled-dg2-rc-ccs:        4 tiled dg2 rc ccs
 * @4-tiled-dg2-rc-ccs-cc:     4 tiled dg2 rc ccs cc
 * @4-tiled-mtl-mc-ccs:        4 tiled mtl mc ccs
 * @4-tiled-mtl-rc-ccs:        4 tiled mtl rc ccs
 * @4-tiled-mtl-rc-ccs-cc:     4 tiled mtl rc ccs cc
 * @y-tiled-ccs:               Y tiled ccs
 * @y-tiled-gen12-mc-ccs:      Y tiled gen12 mc ccs
 * @y-tiled-gen12-rc-ccs:      Y tiled gen12 rc ccs
 * @y-tiled-gen12-rc-ccs-cc:   Y tiled gen12 rc ccs cc
 * @yf-tiled-ccs:              YF tiled ccs
 */

/**
 * SUBTEST: %s-%s
 * Description: Test %arg[1] with %arg[2] modifier
 *
 * arg[1]:
 *
 * @crc-primary-basic:           Primary plane CRC compatibility
 * @crc-primary-suspend:         Primary plane CRC after suspend
 * @crc-sprite-planes-basic:     Sprite plane CRC compatability
 * @random-ccs-data:             Random CCS data
 *
 * arg[2]:
 *
 * @4-tiled-bmg-ccs:           4 tiled xe2 pat controlled ccs
 * @4-tiled-lnl-ccs:           4 tiled xe2 pat controlled ccs
 * @4-tiled-dg2-mc-ccs:        4 tiled mc ccs
 * @4-tiled-dg2-rc-ccs:        4 tiled dg2 rc ccs
 * @4-tiled-dg2-rc-ccs-cc:     4 tiled dg2 rc ccs cc
 * @4-tiled-mtl-mc-ccs:        4 tiled mtl mc ccs
 * @4-tiled-mtl-rc-ccs:        4 tiled mtl rc ccs
 * @4-tiled-mtl-rc-ccs-cc:     4 tiled mtl rc ccs cc
 * @y-tiled-ccs:               Y tiled ccs
 * @y-tiled-gen12-mc-ccs:      Y tiled gen12 mc ccs
 * @y-tiled-gen12-rc-ccs:      Y tiled gen12 rc ccs
 * @y-tiled-gen12-rc-ccs-cc:   Y tiled gen12 rc ccs cc
 * @yf-tiled-ccs:              YF tiled ccs
 */

/**
 * SUBTEST: %s-%s
 * Description: Test %arg[1] with %arg[2] modifier
 * arg[1]:
 *
 * @bad-rotation-90:             90 degree rotation
 * @crc-primary-rotation-180:    180 degree rotation
 *
 * arg[2]:
 *
 * @4-tiled-bmg-ccs:           4 tiled xe2 pat controlled ccs
 * @4-tiled-lnl-ccs:           4 tiled xe2 pat controlled ccs
 * @4-tiled-dg2-mc-ccs:        4 tiled mc ccs
 * @4-tiled-dg2-rc-ccs:        4 tiled dg2 rc ccs
 * @4-tiled-dg2-rc-ccs-cc:     4 tiled dg2 rc ccs cc
 * @4-tiled-mtl-mc-ccs:        4 tiled mtl mc ccs
 * @4-tiled-mtl-rc-ccs:        4 tiled mtl rc ccs
 * @4-tiled-mtl-rc-ccs-cc:     4 tiled mtl rc ccs cc
 * @y-tiled-ccs:               Y tiled ccs
 * @y-tiled-gen12-mc-ccs:      Y tiled gen12 mc ccs
 * @y-tiled-gen12-rc-ccs:      Y tiled gen12 rc ccs
 * @y-tiled-gen12-rc-ccs-cc:   Y tiled gen12 rc ccs cc
 * @yf-tiled-ccs:              YF tiled ccs
 */

#define SDR_PLANE_BASE	3

IGT_TEST_DESCRIPTION("Test render compression (RC), in which the main surface "
		     "is complemented by a color control surface (CCS) that "
		     "the display uses to interpret the compressed data.");

enum test_flags {
	TEST_CRC			= 1 << 1,
	TEST_ROTATE_180			= 1 << 2,
	TEST_BAD_PIXEL_FORMAT		= 1 << 3,
	TEST_BAD_ROTATION_90		= 1 << 4,
	TEST_NO_AUX_BUFFER		= 1 << 5,
	TEST_BAD_CCS_HANDLE		= 1 << 6,
	TEST_BAD_AUX_STRIDE		= 1 << 7,
	TEST_RANDOM			= 1 << 8,
	TEST_ALL_PLANES			= 1 << 9,
	TEST_SUSPEND			= 1 << 10,
};

#define TEST_BAD_CCS_PLANE	(TEST_NO_AUX_BUFFER | TEST_BAD_CCS_HANDLE | \
				 TEST_BAD_AUX_STRIDE)
#define TEST_FAIL_ON_ADDFB2	(TEST_BAD_PIXEL_FORMAT | TEST_BAD_CCS_PLANE)

enum test_fb_flags {
	FB_COMPRESSED			= 1 << 0,
	FB_HAS_PLANE			= 1 << 1,
	FB_MISALIGN_AUX_STRIDE		= 1 << 2,
	FB_SMALL_AUX_STRIDE		= 1 << 3,
	FB_ZERO_AUX_STRIDE		= 1 << 4,
	FB_RANDOM			= 1 << 5,
};

typedef struct {
	int drm_fd;
	igt_display_t display;
	igt_output_t *output;
	enum pipe pipe;
	enum test_flags flags;
	igt_plane_t *plane;
	igt_pipe_crc_t *pipe_crc;
	uint32_t format;
	uint64_t ccs_modifier;
	unsigned int seed;
	bool user_seed;
	enum igt_commit_style commit;
	int fb_list_length;
	bool do_hibernate;
	struct {
		struct igt_fb fb;
		int width, height;
		double r, g, b;
		u64 modifier;
		u32 format;
	} *fb_list;
} data_t;

static const struct {
	double r;
	double g;
	double b;
} colors[2] = {
	{1.0, 0.0, 0.0},
	{0.0, 1.0, 0.0}
};

static const uint32_t formats[] = {
	DRM_FORMAT_XYUV8888,
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_XRGB2101010,
	DRM_FORMAT_XBGR16161616F,
	DRM_FORMAT_YUYV,
	DRM_FORMAT_NV12,
	DRM_FORMAT_P012,
	DRM_FORMAT_P016,
};

static const struct {
	uint64_t modifier;
	const char *str;
} ccs_modifiers[] = {
	{I915_FORMAT_MOD_Y_TILED_CCS, "y-tiled-ccs"},
	{I915_FORMAT_MOD_Yf_TILED_CCS, "yf-tiled-ccs"},
	{I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS, "y-tiled-gen12-rc-ccs"},
	{I915_FORMAT_MOD_Y_TILED_GEN12_RC_CCS_CC, "y-tiled-gen12-rc-ccs-cc"},
	{I915_FORMAT_MOD_Y_TILED_GEN12_MC_CCS, "y-tiled-gen12-mc-ccs"},
	{I915_FORMAT_MOD_4_TILED_DG2_RC_CCS, "4-tiled-dg2-rc-ccs"},
	{I915_FORMAT_MOD_4_TILED_DG2_MC_CCS, "4-tiled-dg2-mc-ccs"},
	{I915_FORMAT_MOD_4_TILED_DG2_RC_CCS_CC, "4-tiled-dg2-rc-ccs-cc"},
	{I915_FORMAT_MOD_4_TILED_MTL_RC_CCS, "4-tiled-mtl-rc-ccs"},
	{I915_FORMAT_MOD_4_TILED_MTL_MC_CCS, "4-tiled-mtl-mc-ccs"},
	{I915_FORMAT_MOD_4_TILED_MTL_RC_CCS_CC, "4-tiled-mtl-rc-ccs-cc"},
	{I915_FORMAT_MOD_4_TILED_BMG_CCS, "4-tiled-bmg-ccs"},
	{I915_FORMAT_MOD_4_TILED_LNL_CCS, "4-tiled-lnl-ccs"},
};

static bool check_ccs_planes;

static const struct {
	const enum test_flags	flags;
	const char		*testname;
	const char		*description;
} tests[] = {
	{TEST_BAD_PIXEL_FORMAT, "bad-pixel-format", "Test bad pixel format with given CCS modifier"},
	{TEST_BAD_ROTATION_90, "bad-rotation-90", "Test 90 degree rotation with given CCS modifier"},
	{TEST_CRC, "crc-primary-basic", "Test primary plane CRC compatibility with given CCS modifier"},
	{TEST_CRC | TEST_ROTATE_180, "crc-primary-rotation-180", "Test 180 degree rotation with given CCS modifier"},
	{TEST_RANDOM, "random-ccs-data", "Test random CCS data"},
	{TEST_NO_AUX_BUFFER, "missing-ccs-buffer", "Test missing CCS buffer with given CCS modifier"},
	{TEST_BAD_CCS_HANDLE, "ccs-on-another-bo", "Test CCS with different BO with given modifier"},
	{TEST_BAD_AUX_STRIDE, "bad-aux-stride", "Test with bad AUX stride with given CCS modifier"},
	{TEST_CRC | TEST_ALL_PLANES, "crc-sprite-planes-basic", "Test sprite plane CRC compatibility with given CCS modifier"},

	/*
	 * suspend test has to be kept last because it will decompress
	 * framebuffers when run on BMG
	 */
	{TEST_CRC | TEST_SUSPEND, "crc-primary-suspend", "Test primary plane CRC with suspend on XR24 and P016 formats"},
};

/*
 * Limit maximum used sprite plane width so this test will not mistakenly
 * fail on hardware limitations which are not interesting to this test.
 * On this test too wide sprite plane may fail during creation with dmesg
 * comment saying:
 * "Requested display configuration exceeds system watermark limitations"
 */
#define MAX_SPRITE_PLANE_WIDTH 2000


/**
 * check_hibernation_support:
 *
 * Return: True if kernel is configured with resume point for hibernate.
 */
static bool check_hibernation_support(void)
{
	int fd;
	char buffer[2048];
	ssize_t bytes_read;
	FILE *cmdline;

	/* Check if hibernation is supported in /sys/power/state */
	fd = open("/sys/power/state", O_RDONLY);

	if (fd <= 0) {
		igt_debug("Failed to open /sys/power/state\n");
		return false;
	}

	bytes_read = read(fd, buffer, sizeof(buffer) - 1);
	close(fd);

	if (bytes_read <= 0) {
		igt_debug("Failed to read /sys/power/state");
		return false;
	}

	buffer[bytes_read] = '\0';
	if (strstr(buffer, "disk") == NULL) {
		igt_debug("Hibernation (suspend to disk) is not supported on this system.\n");
		return false;
	}

	/* Check if resume is configured in kernel command line */
	cmdline = fopen("/proc/cmdline", "r");

	if (!cmdline) {
		igt_debug("Failed to open /proc/cmdline");
		return false;
	}

	fread(buffer, 1, sizeof(buffer) - 1, cmdline);
	fclose(cmdline);

	if (strstr(buffer, "resume=") == NULL) {
		igt_debug("Kernel does not have 'resume' parameter configured for hibernation.\n");
		return false;
	}

	return true;
}

/**
 * Ensure_grub_boots_same_kernel:
 *
 * Return: True if kernel was found and set for next reboot.
 */
static bool ensure_grub_boots_same_kernel(void)
{
	char cmdline[1024];
	char current_kernel[256];
	char last_menuentry[512] = "";
	char grub_entry[512];
	char command[1024];
	FILE *cmdline_file, *grub_cfg;
	char line[1024];
	bool kernel_found = false;
	char *kernel_arg;
	char *kernel_end;

	/* Read /proc/cmdline to get the current kernel image */
	cmdline_file = fopen("/proc/cmdline", "r");
	if (!cmdline_file) {
		igt_debug("Failed to open /proc/cmdline");
		return false;
	}

	if (!fgets(cmdline, sizeof(cmdline), cmdline_file)) {
		fclose(cmdline_file);
		igt_debug("Failed to read /proc/cmdline");
		return false;
	}
	fclose(cmdline_file);

	/* Parse the kernel image from cmdline */
	kernel_arg = strstr(cmdline, "BOOT_IMAGE=");
	if (!kernel_arg) {
		igt_debug("BOOT_IMAGE= not found in /proc/cmdline\n");
		return false;
	}

	kernel_arg += strlen("BOOT_IMAGE=");
	kernel_end = strchr(kernel_arg, ' ');

	if (!kernel_end)
		kernel_end = kernel_arg + strlen(kernel_arg);

	snprintf(current_kernel, sizeof(current_kernel), "%.*s",
		 (int)(kernel_end - kernel_arg), kernel_arg);
	igt_debug("Current kernel image: %s\n", current_kernel);

	/* Open GRUB config file to find matching entry */
	grub_cfg = fopen("/boot/grub/grub.cfg", "r");
	if (!grub_cfg) {
		igt_debug("Failed to open GRUB configuration file");
		return false;
	}

	while (fgets(line, sizeof(line), grub_cfg)) {
		/* Check if the line contains a menuentry */
		if (strstr(line, "menuentry")) {
		/* Store the menuentry line */
			char *start = strchr(line, '\'');
			char *end = start ? strchr(start + 1, '\'') : NULL;

			if (start && end) {
				snprintf(last_menuentry,
					 sizeof(last_menuentry),
					 "%.*s", (int)(end - start - 1),
					 start + 1);
			}
		}

		/* Check if the current line contains the kernel */
		if (strstr(line, current_kernel)) {
			/* Use the last seen menuentry as the match */
			snprintf(grub_entry, sizeof(grub_entry), "%s",
				 last_menuentry);
			kernel_found = true;
			break;
		}
	}

	fclose(grub_cfg);

	if (!kernel_found) {
		igt_debug("Failed to find matching GRUB entry for kernel: %s\n",
			  current_kernel);
		return false;
	}

	/* Set the GRUB boot target using grub-reboot */
	snprintf(command, sizeof(command), "grub-reboot \"%s\"", grub_entry);
	if (system(command) != 0) {
		igt_debug("Failed to set GRUB boot target to: %s\n",
			  grub_entry);
		return false;
	}

	igt_debug("Set GRUB to boot kernel: %s (GRUB entry: %s)\n",
		  current_kernel, grub_entry);
	return true;
}

static void addfb_init(struct igt_fb *fb, struct drm_mode_fb_cmd2 *f)
{
	int i;

	f->width = fb->width;
	f->height = fb->height;
	f->pixel_format = fb->drm_format;
	f->flags = DRM_MODE_FB_MODIFIERS;

	for (i = 0; i < fb->num_planes; i++) {
		f->handles[i] = fb->gem_handle;
		f->modifier[i] = fb->modifier;
		f->pitches[i] = fb->strides[i];
		f->offsets[i] = fb->offsets[i];
	}
}

static void
create_fb_prepare_add(int drm_fd, int width, int height,
		      uint32_t format, uint64_t modifier,
		      igt_fb_t *fb, struct drm_mode_fb_cmd2 *f)
{
	igt_create_bo_for_fb(drm_fd, width, height, format, modifier, fb);
	igt_assert_lt_u32(0, fb->gem_handle);

	addfb_init(fb, f);
}

/*
 * The CCS planes of compressed framebuffers contain non-zero bytes if the
 * engine compressed effectively the framebuffer. The actual encoding of these
 * bytes is not specified, but we know that seeing an all-zero CCS plane means
 * that the engine left the FB uncompressed, which is not what we expect in
 * the test. Look for the first non-zero byte in the given CCS plane to get a
 * minimal assurance that compression took place.
 */
static void check_ccs_plane(int drm_fd, igt_fb_t *fb, int plane)
{
	void *map;
	void *ccs_p;
	size_t ccs_size;
	int i;

	ccs_size = fb->strides[plane] * fb->plane_height[plane];
	igt_assert(ccs_size);

	if (is_i915_device(drm_fd)) {
		gem_set_domain(drm_fd, fb->gem_handle, I915_GEM_DOMAIN_CPU, 0);
		map = gem_mmap__cpu(drm_fd, fb->gem_handle, 0, fb->size, PROT_READ);
	} else {
		map = xe_bo_mmap_ext(drm_fd, fb->gem_handle, fb->size, PROT_READ);
	}
	ccs_size = fb->strides[plane] * fb->plane_height[plane];
	ccs_p = map + fb->offsets[plane];
	for (i = 0; i < ccs_size; i += sizeof(uint32_t))
		if (*(uint32_t *)(ccs_p + i))
			break;

	igt_assert_eq(0, gem_munmap(map, fb->size));

	igt_assert_f(i < ccs_size,
		     "CCS plane %d (for main plane %d) lacks compression meta-data\n",
		     plane, igt_fb_ccs_to_main_plane(fb, plane));
}

static void check_ccs_cc_plane(int drm_fd, igt_fb_t *fb, int plane, const float *cc_color)
{
	union cc {
		float f;
		uint32_t d;
	} *cc_p;
	void *map;
	uint32_t native_color[2] = {};
	uint16_t half[4];

	if (is_i915_device(drm_fd)) {
		gem_set_domain(drm_fd, fb->gem_handle, I915_GEM_DOMAIN_CPU, 0);
		map = gem_mmap__cpu(drm_fd, fb->gem_handle, 0, fb->size, PROT_READ);
	} else {
		map = xe_bo_mmap_ext(drm_fd, fb->gem_handle, fb->size, PROT_READ);
	}
	cc_p = map + fb->offsets[plane];

	igt_assert(cc_color[0] == cc_p[0].f &&
		   cc_color[1] == cc_p[1].f &&
		   cc_color[2] == cc_p[2].f &&
		   cc_color[3] == cc_p[3].f);

	switch (fb->drm_format) {
	case DRM_FORMAT_XRGB8888:
		native_color[0] = (uint32_t)(cc_color[3] * 0xff) << 24 |
			(uint32_t)(cc_color[0] * 0xff) << 16 |
			(uint32_t)(cc_color[1] * 0xff) << 8 |
			(uint32_t)(cc_color[2] * 0xff);
		break;
	case DRM_FORMAT_XRGB2101010:
		native_color[0] = (uint32_t)(cc_color[3] * 0x3) << 30 |
			(uint32_t)(cc_color[0] * 0x3ff) << 20 |
			(uint32_t)(cc_color[1] * 0x3ff) << 10 |
			(uint32_t)(cc_color[2] * 0x3ff);
		break;
	case DRM_FORMAT_XBGR16161616F:
		igt_float_to_half(cc_color, half, 4);

		native_color[1] = (uint64_t)half[3] << 16 | (uint64_t)half[2];
		native_color[0] = (uint64_t)half[1] << 16 | (uint64_t)half[0];
		break;
	default:
		break;
	}

	igt_assert_eq_u32(native_color[0], cc_p[4].d);
	igt_assert_eq_u32(native_color[1], cc_p[5].d);

	igt_assert_eq(0, gem_munmap(map, fb->size));
}

static void check_all_ccs_planes(int drm_fd, igt_fb_t *fb, const float *cc_color, bool check_cc_plane)
{
	int i;

	for (i = 0; i < fb->num_planes; i++) {
		if (igt_fb_is_ccs_plane(fb, i) &&
		    !igt_fb_is_gen12_ccs_cc_plane(fb, i))
			check_ccs_plane(drm_fd, fb, i);
		else if (igt_fb_is_gen12_ccs_cc_plane(fb, i) && check_cc_plane)
			check_ccs_cc_plane(drm_fd, fb, i, cc_color);
	}
}

static void access_flat_ccs_surface(struct igt_fb *fb, bool verify_compression)
{
	uint64_t bb_size, ccssize = fb->size / CCS_RATIO(fb->fd);
	uint64_t ccs_bo_size = ALIGN(ccssize, xe_get_default_alignment(fb->fd));
	struct blt_ctrl_surf_copy_data surf = {};
	uint32_t sysmem = system_memory(fb->fd);
	uint32_t bb1, ccs, *ccsmap;
	uint16_t cpu_caching = DRM_XE_GEM_CPU_CACHING_WC;
	uint8_t uc_mocs = intel_get_uc_mocs_index(fb->fd);
	uint8_t comp_pat_index = intel_get_pat_idx_wt(fb->fd);
	uint32_t region = (intel_gen(intel_get_drm_devid(fb->fd)) >= 20 &&
			   xe_has_vram(fb->fd)) ? REGION_LMEM(0) : REGION_SMEM;

	struct drm_xe_engine_class_instance inst = {
		.engine_class = DRM_XE_ENGINE_CLASS_COPY,
	};

	uint64_t surf_ahnd;
	uint32_t vm, exec_queue;
	intel_ctx_t *surf_ctx;

	vm = xe_vm_create(fb->fd, 0, 0);
	exec_queue = xe_exec_queue_create(fb->fd, vm, &inst, 0);
	surf_ctx = intel_ctx_xe(fb->fd, vm, exec_queue, 0, 0, 0);
	surf_ahnd = intel_allocator_open(fb->fd, surf_ctx->vm,
						INTEL_ALLOCATOR_RELOC);

	ccs = xe_bo_create_caching(fb->fd, 0, ccs_bo_size, sysmem, 0, cpu_caching);

	blt_ctrl_surf_copy_init(fb->fd, &surf);
	blt_set_ctrl_surf_object(&surf.src, fb->gem_handle, region, fb->size,
				 uc_mocs, comp_pat_index, BLT_INDIRECT_ACCESS);
	blt_set_ctrl_surf_object(&surf.dst, ccs, sysmem, ccssize, uc_mocs,
				 DEFAULT_PAT_INDEX, DIRECT_ACCESS);

	bb_size = xe_bb_size(fb->fd, SZ_4K);
	bb1 = xe_bo_create(fb->fd, 0, bb_size, sysmem, 0);
	blt_set_batch(&surf.bb, bb1, bb_size, sysmem);
	blt_ctrl_surf_copy(fb->fd, surf_ctx, NULL, surf_ahnd, &surf);
	intel_ctx_xe_sync(surf_ctx, true);

	ccsmap = xe_bo_map(fb->fd, ccs, surf.dst.size);

	if (verify_compression) {
		for (int i = 0; i < surf.dst.size / sizeof(uint32_t); i++) {
			if (ccsmap[i] != 0)
				goto ccs_verified;
		}
		igt_assert_f(false, "framebuffer was not compressed!\n");
	} else {
		for (int i = 0; i < surf.dst.size / sizeof(uint32_t); i++)
			ccsmap[i] = rand();
	}

	blt_set_ctrl_surf_object(&surf.src, ccs, sysmem, ccssize,
				 uc_mocs, DEFAULT_PAT_INDEX, DIRECT_ACCESS);
	blt_set_ctrl_surf_object(&surf.dst, fb->gem_handle, region, fb->size,
				 uc_mocs, comp_pat_index, INDIRECT_ACCESS);
	blt_ctrl_surf_copy(fb->fd, surf_ctx, NULL, surf_ahnd, &surf);
	intel_ctx_xe_sync(surf_ctx, true);

ccs_verified:
	munmap(ccsmap, ccssize);
	gem_close(fb->fd, ccs);
	gem_close(fb->fd, bb1);
	xe_exec_queue_destroy(fb->fd, exec_queue);
	xe_vm_destroy(fb->fd, vm);
	free(surf_ctx);
	put_ahnd(surf_ahnd);
}

static void fill_fb_random(int drm_fd, igt_fb_t *fb)
{
	void *map;
	uint8_t *p;
	int i;

	if (is_i915_device(drm_fd)) {
		gem_set_domain(drm_fd, fb->gem_handle, I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);
		p = map = gem_mmap__cpu(drm_fd, fb->gem_handle, 0, fb->size, PROT_WRITE);
	} else {
		p = map = xe_bo_mmap_ext(drm_fd, fb->gem_handle, fb->size, PROT_WRITE);
	}

	for (i = 0; i < fb->size; i++)
		p[i] = rand();

	igt_assert_eq(0, gem_munmap(map, fb->size));

	/* randomize also ccs surface on Xe2 */
	if (intel_gen(intel_get_drm_devid(drm_fd)) >= 20)
		access_flat_ccs_surface(fb, false);
}

static void test_bad_ccs_plane(data_t *data, int width, int height, int ccs_plane,
			       enum test_fb_flags fb_flags)
{
	struct igt_fb fb = {};
	struct drm_mode_fb_cmd2 f = {};
	uint32_t bad_ccs_bo = 0;
	int addfb_errno;
	int ret;

	igt_assert(fb_flags & FB_COMPRESSED);
	create_fb_prepare_add(data->drm_fd, width, height,
			      data->format, data->ccs_modifier,
			      &fb, &f);

	/*
	 * The stride of CCS planes on GEN12+ is fixed, so we can check for
	 * an incorrect stride with the same delta as on earlier platforms.
	 */
	if (fb_flags & FB_MISALIGN_AUX_STRIDE) {
		igt_skip_on_f(HAS_FLATCCS(intel_get_drm_devid(data->drm_fd)), "No aux plane on flat ccs.\n");
		igt_skip_on_f(width <= 1024,
			      "FB already has the smallest possible stride\n");
		f.pitches[ccs_plane] -= 64;
	}

	if (fb_flags & FB_SMALL_AUX_STRIDE) {
		igt_skip_on_f(HAS_FLATCCS(intel_get_drm_devid(data->drm_fd)), "No aux plane on flat ccs.\n");
		igt_skip_on_f(width <= 1024,
			      "FB already has the smallest possible stride\n");
		f.pitches[ccs_plane] = ALIGN(f.pitches[ccs_plane] / 2, 128);
	}

	if (fb_flags & FB_ZERO_AUX_STRIDE)
		f.pitches[ccs_plane] = 0;

	/* Put the CCS buffer on a different BO. */
	if (data->flags & TEST_BAD_CCS_HANDLE) {
		bad_ccs_bo = is_i915_device(data->drm_fd) ?
				gem_create(data->drm_fd, fb.size) :
				xe_bo_create(data->drm_fd, 0, fb.size,
					     vram_if_possible(data->drm_fd, 0),
					     DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);
		f.handles[ccs_plane] = bad_ccs_bo;
	}

	if (data->flags & TEST_NO_AUX_BUFFER) {
		igt_skip_on_f(HAS_FLATCCS(intel_get_drm_devid(data->drm_fd)), "No aux plane on flat ccs.\n");
		f.handles[ccs_plane] = 0;
		f.modifier[ccs_plane] = 0;
		f.pitches[ccs_plane] = 0;
		f.offsets[ccs_plane] = 0;
	}

	ret = drmIoctl(data->drm_fd, DRM_IOCTL_MODE_ADDFB2, &f);
	addfb_errno = errno;

	if (bad_ccs_bo)
		gem_close(data->drm_fd, bad_ccs_bo);

	igt_assert_eq(ret, -1);
	igt_assert_eq(addfb_errno, EINVAL);

	gem_close(data->drm_fd, fb.gem_handle);
}

static void test_bad_ccs_plane_params(data_t *data, int width, int height,
				      enum test_fb_flags fb_flags)
{
	for (int ccs_plane = 1;
	     ccs_plane <= (igt_format_is_yuv_semiplanar(data->format) ? 2 : 1);
	     ccs_plane++)
		test_bad_ccs_plane(data, width, height, ccs_plane, fb_flags);
}

static void test_bad_pixel_format(data_t *data, int width, int height,
				  enum test_fb_flags fb_flags)
{
	struct igt_fb fb = {};
	struct drm_mode_fb_cmd2 f = {};
	int ret;

	igt_assert(fb_flags & FB_COMPRESSED);
	create_fb_prepare_add(data->drm_fd, width, height,
			      DRM_FORMAT_RGB565, data->ccs_modifier,
			      &fb, &f);

	ret = drmIoctl(data->drm_fd, DRM_IOCTL_MODE_ADDFB2, &f);
	igt_assert_eq(ret, -1);
	igt_assert_eq(errno, EINVAL);

	gem_close(data->drm_fd, fb.gem_handle);
}

static void test_bad_fb_params(data_t *data, int width, int height, enum test_fb_flags fb_flags)
{
	if (data->flags & TEST_BAD_PIXEL_FORMAT)
		test_bad_pixel_format(data, width, height, fb_flags);

	if (data->flags & TEST_BAD_CCS_PLANE)
		test_bad_ccs_plane_params(data, width, height, fb_flags);
}

static void fast_clear_fb(int drm_fd, struct igt_fb *fb, const float *cc_color)
{
	igt_render_clearfunc_t fast_clear = igt_get_render_clearfunc(intel_get_drm_devid(drm_fd));
	struct intel_bb *ibb = intel_bb_create(drm_fd, 4096);
	struct buf_ops *bops = buf_ops_create(drm_fd);
	struct intel_buf *dst = igt_fb_create_intel_buf(drm_fd, bops, fb, "fast clear dst");

	if (is_i915_device(drm_fd))
		gem_set_domain(drm_fd, fb->gem_handle,
			       I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);

	fast_clear(ibb, dst, 0, 0, fb->width, fb->height, cc_color);

	intel_bb_sync(ibb);
	intel_bb_destroy(ibb);
	intel_buf_destroy(dst);
	buf_ops_destroy(bops);
}

static struct igt_fb *get_fb(data_t *data, u64 modifier, double r, double g,
			     double b, int width, int height, u32 format)
{
	for (int i = 0; i < data->fb_list_length; i++) {
		if (data->fb_list[i].width == width &&
		    data->fb_list[i].height == height &&
		    data->fb_list[i].modifier == modifier &&
		    data->fb_list[i].format == format &&
		    data->fb_list[i].r == r && data->fb_list[i].g == g &&
		    data->fb_list[i].b == b)
			return &data->fb_list[i].fb;
	}

	data->fb_list = realloc(data->fb_list, sizeof(*data->fb_list) *
				(data->fb_list_length + 1));
	data->fb_list[data->fb_list_length].width = width;
	data->fb_list[data->fb_list_length].height = height;
	data->fb_list[data->fb_list_length].r = r;
	data->fb_list[data->fb_list_length].g = g;
	data->fb_list[data->fb_list_length].b = b;
	data->fb_list[data->fb_list_length].modifier = modifier;
	data->fb_list[data->fb_list_length].format = format;

	if (modifier == I915_FORMAT_MOD_4_TILED_BMG_CCS ||
	    modifier == I915_FORMAT_MOD_4_TILED_LNL_CCS) {
		struct igt_fb *temp_fb, *fb;
		/* copy xe2 framebuffer to compresssed memory
		 */

		igt_create_fb(data->drm_fd, width, height, format, modifier,
			      &data->fb_list[data->fb_list_length++].fb);

		/* temp fb non compressed linear fb */
		temp_fb = get_fb(data, DRM_FORMAT_MOD_NONE, r, g, b,
				 width, height, data->format);

		/* because of possible realloc happening get 'current' fb
		 * back from the list
		 */
		fb = get_fb(data, modifier, r, g, b, width, height,
			    data->format);

		igt_xe2_blit_with_dst_pat(fb, temp_fb, intel_get_pat_idx_uc_comp(fb->fd));
		access_flat_ccs_surface(fb, true);
		return fb;

	} else if (r + g + b == 0)
		igt_create_pattern_fb(data->drm_fd, width, height, format,
				      modifier,
				      &data->fb_list[data->fb_list_length].fb);
	else
		igt_create_color_fb(data->drm_fd, width, height, format,
				    modifier, r, g, b,
				    &data->fb_list[data->fb_list_length].fb);

	return &data->fb_list[data->fb_list_length++].fb;
}

static void remove_compressed_bmg_fbs(data_t *data) {
	/* if running on bmg ccs become uncompressed at suspend. Here marked
	 * bmg ccs in book keeping as not there hence it will not match to
	 * anything. Here cannot remove fb yet since it's on screen. Remove
	 * will happen at final fixture.
	 */
	for (int i = 0; i < data->fb_list_length; i++) {
		if (data->fb_list[i].modifier == I915_FORMAT_MOD_4_TILED_BMG_CCS)
			data->fb_list[i].modifier = ~1;
	}
}

static void generate_fb(data_t *data, struct igt_fb *fb,
			int width, int height,
			enum test_fb_flags fb_flags)
{
	struct drm_mode_fb_cmd2 f = {0};
	uint64_t modifier;
	struct igt_fb *temp_fb;
	bool do_fast_clear = igt_fb_is_gen12_rc_ccs_cc_modifier(data->ccs_modifier);
	bool do_solid_fill = do_fast_clear || data->plane;
	int c = !!data->plane;
	const float cc_color[4] = {colors[!!data->plane].r,
				   colors[!!data->plane].g,
				   colors[!!data->plane].b,
				   1.0};

	/* Use either compressed or linear to test. However, given the lack of
	 * available bandwidth, we use linear for the primary plane when
	 * testing sprites, since we cannot fit two CCS planes into the
	 * available FIFO configurations.
	 */
	if (fb_flags & FB_COMPRESSED)
		modifier = data->ccs_modifier;
	else
		modifier = DRM_FORMAT_MOD_LINEAR;

	if (data->flags & TEST_RANDOM) {
		create_fb_prepare_add(data->drm_fd, width, height,
				data->format, modifier,
				fb, &f);
		do_ioctl(data->drm_fd, DRM_IOCTL_MODE_ADDFB2, &f);
		fb->fb_id = f.fb_id;
	} else {
		if (do_solid_fill)
			temp_fb = get_fb(data, modifier,
					colors[c].r, colors[c].g, colors[c].b,
					width, height, data->format);
		else
			temp_fb = get_fb(data, modifier, 0.0, 0.0, 0.0,
					width, height, data->format);

		*fb = *temp_fb;
		addfb_init(fb, &f);
	}

	if (data->flags & TEST_RANDOM) {
		srand(data->seed);
		fill_fb_random(data->drm_fd, fb);
	} else {
		if (do_fast_clear && (fb_flags & FB_COMPRESSED)) {
			fast_clear_fb(data->drm_fd, fb, cc_color);
		}
	}

	if (check_ccs_planes)
		check_all_ccs_planes(data->drm_fd, fb, cc_color, !(data->flags & TEST_RANDOM));

}

static igt_plane_t *first_sdr_plane(data_t *data)
{
	return igt_output_get_plane(data->output, SDR_PLANE_BASE);
}

static bool is_sdr_plane(const igt_plane_t *plane)
{
	return plane->index >= SDR_PLANE_BASE;
}

/*
 * Mixing SDR and HDR planes results in a CRC mismatch, so use the first
 * SDR/HDR plane as the main plane matching the SDR/HDR type of the sprite
 * plane under test.
 */
static igt_plane_t *compatible_main_plane(data_t *data)
{
	if (data->plane && is_sdr_plane(data->plane) &&
	    igt_format_is_yuv(data->format))
		return first_sdr_plane(data);

	return igt_output_get_plane_type(data->output, DRM_PLANE_TYPE_PRIMARY);
}

static bool try_config(data_t *data, enum test_fb_flags fb_flags,
		       igt_crc_t *crc)
{
	igt_display_t *display = &data->display;
	igt_plane_t *primary = compatible_main_plane(data);
	drmModeModeInfo *drm_mode = igt_output_get_mode(data->output);
	int fb_width = drm_mode->hdisplay;
	struct igt_fb fb = {};
	struct igt_fb fb_sprite = {};
	int ret;

	if (primary == data->plane)
		return false;

	if (!igt_plane_has_format_mod(primary, data->format,
				      data->ccs_modifier))
		return false;

	if (igt_fb_is_gen12_rc_ccs_cc_modifier(data->ccs_modifier) &&
	    data->format != DRM_FORMAT_XRGB8888 &&
	    data->format != DRM_FORMAT_XRGB2101010 &&
	    data->format != DRM_FORMAT_XBGR16161616F)
		return false;

	/* VEBOX just hangs with an actual 10bpc format */
	if (igt_fb_is_gen12_mc_ccs_modifier(data->ccs_modifier) &&
	    data->format == DRM_FORMAT_XRGB2101010)
		return false;

	if ((fb_flags & FB_MISALIGN_AUX_STRIDE) ||
	    (fb_flags & FB_SMALL_AUX_STRIDE))
		fb_width = max(fb_width, 1536);

	fb_width = min(MAX_SPRITE_PLANE_WIDTH, fb_width);

	if (data->flags & TEST_FAIL_ON_ADDFB2) {
		test_bad_fb_params(data, fb_width, drm_mode->vdisplay, fb_flags);
		return true;
	}

	if (data->plane && fb_flags & FB_COMPRESSED) {
		if (!igt_plane_has_format_mod(data->plane, data->format,
					      data->ccs_modifier))
			return false;

		generate_fb(data, &fb, fb_width, drm_mode->vdisplay,
			    (fb_flags & ~FB_COMPRESSED) | FB_HAS_PLANE);
		generate_fb(data, &fb_sprite, 256, 256, fb_flags);
	} else {
		generate_fb(data, &fb, fb_width, drm_mode->vdisplay, fb_flags);
	}

	igt_plane_set_position(primary, 0, 0);
	igt_plane_set_size(primary, drm_mode->hdisplay, drm_mode->vdisplay);
	igt_plane_set_fb(primary, &fb);

	if (data->plane && fb_flags & FB_COMPRESSED) {
		igt_plane_set_position(data->plane, 0, 0);
		igt_plane_set_size(data->plane, 256, 256);
		igt_plane_set_fb(data->plane, &fb_sprite);
	}

	if (data->flags & TEST_ROTATE_180)
		igt_plane_set_rotation(primary, IGT_ROTATION_180);
	if (data->flags & TEST_BAD_ROTATION_90)
		igt_plane_set_rotation(primary, IGT_ROTATION_90);

	ret = igt_display_try_commit2(display, data->commit);

	if (ret == 0 && !(fb_flags & TEST_BAD_ROTATION_90) && crc) {
		if (data->flags & TEST_SUSPEND && fb_flags & FB_COMPRESSED) {
			if (data->do_hibernate) {
				igt_require_f(check_hibernation_support(),
					      "Kernel is not cofigured for resume\n");
				igt_require_f(ensure_grub_boots_same_kernel(),
					      "Couldn't find correct kernel in grub.cfg\n");
				igt_system_suspend_autoresume(SUSPEND_STATE_DISK,
							      SUSPEND_TEST_NONE);
			} else {
				igt_system_suspend_autoresume(SUSPEND_STATE_MEM,
							      SUSPEND_TEST_NONE);
			}

			/* on resume check flat ccs is still compressed */
			if (is_xe_device(data->drm_fd) &&
			    HAS_FLATCCS(intel_get_drm_devid(data->drm_fd))) {
				if (IS_BATTLEMAGE(intel_get_drm_devid(data->drm_fd)))
					remove_compressed_bmg_fbs(data);
				else
					access_flat_ccs_surface(&fb, true);
			}
		}
		igt_pipe_crc_collect_crc(data->pipe_crc, crc);
	}

	igt_debug_wait_for_keypress("ccs");

	if (data->plane && fb_flags & FB_COMPRESSED) {
		igt_plane_set_position(data->plane, 0, 0);
		igt_plane_set_size(data->plane, 0, 0);
		igt_plane_set_fb(data->plane, NULL);
	}

	igt_plane_set_fb(primary, NULL);
	igt_plane_set_rotation(primary, IGT_ROTATION_0);

	if (data->flags & TEST_RANDOM) {
		igt_display_commit2(display, data->commit);
		igt_remove_fb(data->drm_fd, &fb);
	}

	igt_assert_eq(ret, data->flags & TEST_BAD_ROTATION_90 ? -EINVAL : 0);

	return true;
}

static int test_ccs(data_t *data)
{	int valid_tests = 0;
	igt_crc_t crc, ref_crc;
	enum test_fb_flags fb_flags = 0;

	if (data->flags & TEST_SUSPEND &&
	    !(data->format == DRM_FORMAT_XRGB8888 ||
	      data->format == DRM_FORMAT_P016))
	      return 0;

	igt_info("Testing format " IGT_FORMAT_FMT " / modifier " IGT_MODIFIER_FMT "\n",
		 IGT_FORMAT_ARGS(data->format), IGT_MODIFIER_ARGS(data->ccs_modifier));

	if (data->flags & TEST_CRC) {
		data->pipe_crc = igt_pipe_crc_new(data->drm_fd, data->pipe,
						  IGT_PIPE_CRC_SOURCE_AUTO);

		if (try_config(data, fb_flags | FB_COMPRESSED, &ref_crc) &&
		    try_config(data, fb_flags, &crc)) {
			igt_assert_crc_equal(&crc, &ref_crc);
			valid_tests++;
		}

		igt_pipe_crc_free(data->pipe_crc);
		data->pipe_crc = NULL;
	}

	if (data->flags & TEST_RANDOM)
		valid_tests += try_config(data, fb_flags | FB_COMPRESSED | FB_RANDOM, NULL);

	if (data->flags & TEST_BAD_PIXEL_FORMAT ||
	    data->flags & TEST_BAD_ROTATION_90 ||
	    data->flags & TEST_NO_AUX_BUFFER ||
	    data->flags & TEST_BAD_CCS_HANDLE) {
		valid_tests += try_config(data, fb_flags | FB_COMPRESSED, NULL);
	}

	if (data->flags & TEST_BAD_AUX_STRIDE) {
		valid_tests += try_config(data, fb_flags | FB_COMPRESSED | FB_MISALIGN_AUX_STRIDE , NULL);
		valid_tests += try_config(data, fb_flags | FB_COMPRESSED | FB_SMALL_AUX_STRIDE , NULL);
		valid_tests += try_config(data, fb_flags | FB_COMPRESSED | FB_ZERO_AUX_STRIDE , NULL);
	}

	return valid_tests;
}

static bool skip_plane(data_t *data, igt_plane_t *plane)
{
	int index = plane->index;

	if (intel_display_ver(intel_get_drm_devid(data->drm_fd)) < 11)
		return false;

	/*
	 * Test 1 HDR plane, 1 SDR UV plane, 1 SDR Y plane.
	 *
	 * Kernel registers planes in the hardware Z order:
	 * 0,1,2 HDR planes
	 * 3,4 SDR UV planes
	 * 5,6 SDR Y planes
	 */
	return index != 0 && index != 3 && index != 5;
}

static bool valid_modifier_test(u64 modifier, const enum test_flags flags)
{
	switch (modifier) {
	case I915_FORMAT_MOD_4_TILED_DG2_RC_CCS:
	case I915_FORMAT_MOD_4_TILED_DG2_MC_CCS:
	case I915_FORMAT_MOD_4_TILED_DG2_RC_CCS_CC:
		if (flags & TEST_BAD_CCS_PLANE)
			return false;
		break;
	case I915_FORMAT_MOD_4_TILED_BMG_CCS:
	case I915_FORMAT_MOD_4_TILED_LNL_CCS:
		if (flags & TEST_FAIL_ON_ADDFB2)
			return false;
		break;
	default:
		break;
	}

	return true;
}

static void test_output(data_t *data, const int testnum)
{
	uint16_t dev_id;

	igt_fixture()
		dev_id = intel_get_drm_devid(data->drm_fd);

	data->flags = tests[testnum].flags;

	for (int i = 0; i < ARRAY_SIZE(ccs_modifiers); i++) {
		if (!valid_modifier_test(ccs_modifiers[i].modifier,
					 data->flags))
			continue;

		data->ccs_modifier = ccs_modifiers[i].modifier;

		igt_describe(tests[testnum].description);
		igt_subtest_with_dynamic_f("%s-%s", tests[testnum].testname, ccs_modifiers[i].str) {
			if (ccs_modifiers[i].modifier == I915_FORMAT_MOD_4_TILED_BMG_CCS ||
			    ccs_modifiers[i].modifier == I915_FORMAT_MOD_4_TILED_LNL_CCS) {
				igt_require_f(intel_gen(dev_id) >= 20,
					      "Xe2 platform needed.\n");
			} else {
				igt_require_f(intel_gen(dev_id) < 20,
					      "Older than Xe2 platform needed.\n");
			}

			for_each_pipe_with_valid_output(&data->display, data->pipe, data->output) {
				igt_display_reset(&data->display);

				igt_output_set_pipe(data->output, data->pipe);
				if (!intel_pipe_output_combo_valid(&data->display))
					continue;

				igt_dynamic_f("pipe-%s-%s", kmstest_pipe_name(data->pipe),
							    data->output->name) {
					int valid_tests = 0;

					if (data->flags == TEST_RANDOM)
						igt_info("Testing with seed %d\n", data->seed);

					if (data->flags & TEST_ALL_PLANES) {
						igt_display_require_output_on_pipe(&data->display, data->pipe);

						for_each_plane_on_pipe(&data->display, data->pipe, data->plane) {
							if (skip_plane(data, data->plane))
								continue;

							for (int j = 0; j < ARRAY_SIZE(formats); j++) {
								data->format = formats[j];
								valid_tests += test_ccs(data);
							}
						}
					} else {
						for (int j = 0; j < ARRAY_SIZE(formats); j++) {
							data->format = formats[j];
							valid_tests += test_ccs(data);
						}
					}
					igt_require_f(valid_tests > 0,
						      "no valid tests for %s on pipe %s\n",
						      ccs_modifiers[i].str,
						      kmstest_pipe_name(data->pipe));
				}
			}
		}
	}

	igt_fixture()
		data->plane = NULL;
}

static int opt_handler(int opt, int opt_index, void *opt_data)
{
	data_t *data = opt_data;

	switch (opt) {
	case 'c':
		check_ccs_planes = true;
		break;
	case 's':
		data->user_seed = true;
		data->seed = strtoul(optarg, NULL, 0);
		break;
	case 'r':
		data->do_hibernate = true;
		break;
	default:
		return IGT_OPT_HANDLER_ERROR;
	}

	return IGT_OPT_HANDLER_SUCCESS;
}

static data_t data;

static const char *help_str =
"  -c\t\tCheck the presence of compression meta-data\n"
"  -s <seed>\tSeed for random number generator\n"
"  -r\t\tOn suspend test do full hibernate with reboot\n"
;

igt_main_args("csr:", NULL, help_str, opt_handler, &data)
{
	igt_fixture() {
		data.drm_fd = drm_open_driver_master(DRIVER_INTEL | DRIVER_XE);

		igt_require(intel_display_ver(intel_get_drm_devid(data.drm_fd)) >= 9);
		kmstest_set_vt_graphics_mode();
		igt_require_pipe_crc(data.drm_fd);

		igt_display_require(&data.display, data.drm_fd);
		igt_display_require_output(&data.display);

		if (!data.user_seed)
			data.seed = time(NULL);

		data.commit = data.display.is_atomic ? COMMIT_ATOMIC :
			COMMIT_UNIVERSAL;

		data.fb_list_length = 0;
	}

	for (int c = 0; c < ARRAY_SIZE(tests); c++)
		test_output(&data, c);

	igt_fixture() {
		igt_display_commit2(&data.display, data.commit);
		for (int i = 0; i < data.fb_list_length; i++)
			igt_remove_fb(data.drm_fd, &data.fb_list[i].fb);
		free(data.fb_list);

		igt_display_fini(&data.display);
		drm_close_driver(data.drm_fd);
	}
}
