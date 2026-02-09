/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 */

/**
 * TEST: kms dirtyfb
 * Category: Display
 * Description: Test DIRTYFB ioctl functionality.
 * Driver requirement: i915, xe
 * Mega feature: General Display Features
 */

#include <sys/types.h>

#include "igt.h"
#include "igt_psr.h"

#include "i915/intel_drrs.h"
#include "i915/intel_fbc.h"
#include "intel_mocs.h"
#include "intel_pat.h"

#include "xe/xe_query.h"

/**
 * SUBTEST: default-dirtyfb-ioctl
 * Description: Test DIRTYFB ioctl is working properly using GPU
 *              frontbuffer rendering with features like FBC, PSR
 *              and DRRS.
 *
 * SUBTEST: %s-dirtyfb-ioctl
 * Description: Test DIRTYFB ioctl is working properly using GPU
 *              frontbuffer rendering with %arg[1] feature.
 *
 * arg[1]:
 *
 * @drrs:    drrs
 * @fbc:     fbc
 * @psr:     psr1
 */

IGT_TEST_DESCRIPTION("Test the DIRTYFB ioctl is working properly with "
		     "its related features: FBC, PSR and DRRS");

#ifndef PAGE_ALIGN
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif
#define PAGE_ALIGN(x) ALIGN(x, PAGE_SIZE)
#endif

typedef struct {
	int drm_fd, devid;
	int debugfs_fd;
	igt_display_t display;
	drmModeModeInfo *mode;
	uint64_t modifier;
	igt_output_t *output;
	igt_pipe_crc_t *pipe_crc;
	enum pipe pipe;

	struct igt_fb fbs[3];

	igt_crc_t ref_crc;

	struct buf_ops *bops;
	enum {
		FEATURE_NONE  = 0,
		FEATURE_PSR   = 1,
		FEATURE_FBC   = 2,
		FEATURE_DRRS  = 4,
		FEATURE_COUNT = 8,
		FEATURE_DEFAULT = 8,
	} feature;
	igt_render_copyfunc_t rendercopy;
} data_t;

static const char *feature_str(int feature)
{
	switch (feature) {
	case FEATURE_NONE:
		return "nop";
	case FEATURE_FBC:
		return "fbc";
	case FEATURE_PSR:
		return "psr";
	case FEATURE_DRRS:
		return "drrs";
	case FEATURE_DEFAULT:
		return "default";
	default:
		igt_assert(false);
	}
}

static bool check_support(data_t *data)
{
	switch (data->feature) {
	case FEATURE_NONE:
		return true;
	case FEATURE_FBC:
		if (!intel_fbc_supported_on_chipset(data->drm_fd, data->pipe)) {
			igt_info("FBC is not supported on this chipset\n");
			return false;
		}

		if (!intel_fbc_plane_size_supported(data->drm_fd,
						    data->mode->hdisplay,
						    data->mode->vdisplay)) {
			igt_info("Plane size not supported as per FBC size restrictions\n");
			return false;
		}
		return true;

	case FEATURE_PSR:
		if (data->output->config.connector->connector_type !=
		    DRM_MODE_CONNECTOR_eDP) {
			igt_info("Output is not an eDP\n");
			return false;
		}
		if (!psr_sink_support(data->drm_fd, data->debugfs_fd,
				      PSR_MODE_1, NULL)) {
			igt_info("Output doesn't support PSR\n");
			return false;
		}
		return true;

	case FEATURE_DRRS:
		if (!(intel_is_drrs_supported(data->drm_fd, data->pipe) &&
		      intel_output_has_drrs(data->drm_fd, data->output))) {
			igt_info("Output doesn't support DRRS\n");
			return false;
		}
		return true;

	case FEATURE_DEFAULT:
		return true;
	default:
		igt_assert(false);
	}
}

static void enable_feature(data_t *data)
{
	switch (data->feature) {
	case FEATURE_NONE:
		break;
	case FEATURE_FBC:
		intel_fbc_enable(data->drm_fd);
		break;
	case FEATURE_PSR:
		psr_enable(data->drm_fd, data->debugfs_fd, PSR_MODE_1, NULL);
		break;
	case FEATURE_DRRS:
		intel_drrs_enable(data->drm_fd, data->pipe);
		break;
	case FEATURE_DEFAULT:
		break;
	default:
		igt_assert(false);
	}
}

static void check_feature_enabled(data_t *data)
{
	switch (data->feature) {
	case FEATURE_NONE:
		break;
	case FEATURE_FBC:
		igt_assert_f(intel_fbc_wait_until_enabled(data->drm_fd,
							  data->pipe),
			     "FBC still disabled\n");
		break;
	case FEATURE_PSR:
		igt_require(!psr_disabled_check(data->debugfs_fd));
		igt_assert_f(psr_wait_entry(data->debugfs_fd, PSR_MODE_1, NULL),
			     "PSR still disabled\n");
		break;
	case FEATURE_DRRS:
		igt_assert_f(!intel_is_drrs_inactive(data->drm_fd, data->pipe),
			     "DRRS INACTIVE\n");
		break;
	case FEATURE_DEFAULT:
		break;
	default:
		igt_assert(false);
	}
}

static void check_feature(data_t *data)
{
	switch (data->feature) {
	case FEATURE_NONE:
		break;
	case FEATURE_FBC:
		igt_assert_f(intel_fbc_wait_until_enabled(data->drm_fd,
							  data->pipe),
			     "FBC disabled\n");
		/* TODO: Add compression check here */
		break;
	case FEATURE_PSR:
		igt_assert_f(psr_wait_entry(data->debugfs_fd, PSR_MODE_1, data->output),
			     "No PSR entry\n");
		psr_sink_error_check(data->debugfs_fd, PSR_MODE_1, data->output);
		break;
	case FEATURE_DRRS:
		igt_assert_f(!intel_is_drrs_inactive(data->drm_fd, data->pipe),
			     "DRRS INACTIVE\n");
		break;
	case FEATURE_DEFAULT:
		break;
	default:
		igt_assert(false);
	}
}

static void disable_features(data_t *data)
{
	intel_fbc_disable(data->drm_fd);

	if (psr_sink_support(data->drm_fd, data->debugfs_fd, PSR_MODE_1, NULL))
		psr_disable(data->drm_fd, data->debugfs_fd, NULL);

	intel_drrs_disable(data->drm_fd, data->pipe);
}

static void prepare(data_t *data)
{
	igt_display_t *display = &data->display;
	igt_plane_t *primary;

	igt_output_set_crtc(data->output,
			    igt_crtc_for_pipe(display, data->pipe));

	data->pipe_crc = igt_crtc_crc_new(igt_crtc_for_pipe(display, data->pipe),
					  IGT_PIPE_CRC_SOURCE_AUTO);

	igt_create_color_fb(data->drm_fd, data->mode->hdisplay,
			    data->mode->vdisplay, DRM_FORMAT_XRGB8888,
			    data->modifier, 0.0, 1.0, 0.0,
			    &data->fbs[0]);

	igt_draw_rect_fb(data->drm_fd, data->bops, 0, &data->fbs[0],
			 data->rendercopy ? IGT_DRAW_RENDER : IGT_DRAW_BLT,
			 0, 0, data->fbs[0].width,
			 data->fbs[0].height, 0xFF);

	primary = igt_output_get_plane_type(data->output,
					    DRM_PLANE_TYPE_PRIMARY);

	igt_plane_set_fb(primary, &data->fbs[0]);

	if (data->feature != FEATURE_DEFAULT)
		disable_features(data);

	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	igt_pipe_crc_collect_crc(data->pipe_crc, &data->ref_crc);

	igt_create_color_fb(data->drm_fd,  data->mode->hdisplay,
			    data->mode->vdisplay, DRM_FORMAT_XRGB8888,
			    data->modifier, 0.0, 1.0, 0.0,
			    &data->fbs[1]);
	igt_draw_rect_fb(data->drm_fd, data->bops, 0, &data->fbs[1],
			 data->rendercopy ? IGT_DRAW_RENDER : IGT_DRAW_BLT,
			 0, 0, data->fbs[1].width,
			 data->fbs[1].height, 0xFF);

	igt_create_color_fb(data->drm_fd, data->mode->hdisplay,
			    data->mode->vdisplay, DRM_FORMAT_XRGB8888,
			    data->modifier, 0.0, 1.0, 0.0,
			    &data->fbs[2]);

	igt_plane_set_fb(primary, &data->fbs[2]);

	enable_feature(data);

	igt_display_commit2(&data->display, COMMIT_ATOMIC);

	check_feature_enabled(data);
}

static void cleanup(data_t *data)
{
	igt_remove_fb(data->drm_fd, &data->fbs[0]);
	igt_remove_fb(data->drm_fd, &data->fbs[1]);
	igt_remove_fb(data->drm_fd, &data->fbs[2]);

	igt_pipe_crc_free(data->pipe_crc);

	igt_output_set_crtc(data->output, NULL);

	igt_display_commit2(&data->display, COMMIT_ATOMIC);
}

static void run_test(data_t *data)
{
	igt_crc_t crc;
	struct intel_buf *src, *dst;
	struct intel_bb *ibb;
	igt_spin_t *spin;
	int r;

	src = intel_buf_create_full(data->bops, data->fbs[1].gem_handle,
				    data->fbs[1].width,
				    data->fbs[1].height,
				    igt_drm_format_to_bpp(data->fbs[1].drm_format),
				    0,
				    igt_fb_mod_to_tiling(data->fbs[1].modifier),
				    0, data->fbs[1].size, 0, is_xe_device(data->drm_fd) ?
				    system_memory(data->drm_fd) : 0,
				    intel_get_pat_idx_uc(data->drm_fd),
				    DEFAULT_MOCS_INDEX);
	dst = intel_buf_create_full(data->bops, data->fbs[2].gem_handle,
				    data->fbs[2].width,
				    data->fbs[2].height,
				    igt_drm_format_to_bpp(data->fbs[2].drm_format),
				    0, igt_fb_mod_to_tiling(data->fbs[2].modifier),
				    0, data->fbs[2].size, 0, is_xe_device(data->drm_fd) ?
				    system_memory(data->drm_fd) : 0,
				    intel_get_pat_idx_uc(data->drm_fd),
				    DEFAULT_MOCS_INDEX);
	ibb = intel_bb_create(data->drm_fd, PAGE_SIZE);

	spin = igt_spin_new(data->drm_fd, .ahnd = ibb->allocator_handle);
	igt_spin_set_timeout(spin, NSEC_PER_SEC);

	if (data->rendercopy) {
		data->rendercopy(ibb, src, 0, 0, data->fbs[2].width, data->fbs[2].height, dst, 0, 0);
	} else {
		intel_bb_blt_copy(ibb, src, 0, 0, src->surface[0].stride,
				  dst, 0, 0, dst->surface[0].stride,
				  data->fbs[2].width, data->fbs[2].height, dst->bpp);
	}

	/* Perfom dirtyfb right after initiating rendercopy/blitter */
	r = drmModeDirtyFB(data->drm_fd, data->fbs[2].fb_id, NULL, 0);
	igt_assert(r == 0 || r == -ENOSYS);

	/* Ensure rendercopy/blitter is complete */
	intel_bb_sync(ibb);

	igt_pipe_crc_collect_crc(data->pipe_crc, &crc);
	igt_assert_crc_equal(&crc, &data->ref_crc);

	check_feature(data);

	igt_spin_free(data->drm_fd, spin);
	intel_bb_destroy(ibb);
	intel_buf_destroy(src);
	intel_buf_destroy(dst);
}

int igt_main()
{
	igt_crtc_t *crtc;
	data_t data = {};

	igt_fixture() {
		data.drm_fd = drm_open_driver_master(DRIVER_INTEL | DRIVER_XE);
		data.debugfs_fd = igt_debugfs_dir(data.drm_fd);
		kmstest_set_vt_graphics_mode();

		igt_display_require(&data.display, data.drm_fd);
		igt_display_require_output(&data.display);
		igt_require(data.display.is_atomic);
		data.devid = intel_get_drm_devid(data.drm_fd);

		data.bops = buf_ops_create(data.drm_fd);
		data.rendercopy = igt_get_render_copyfunc(data.drm_fd);
	}

	for (data.feature = FEATURE_DEFAULT; data.feature > 0;
	     data.feature = data.feature >> 1) {
		igt_describe_f("Test dirtyFB ioctl with %s", feature_str(data.feature));
		igt_subtest_with_dynamic_f("%s-dirtyfb-ioctl", feature_str(data.feature)) {
			for_each_crtc(&data.display, crtc) {
				int valid_tests = 0;

				data.pipe = crtc->pipe;

				for_each_valid_output_on_pipe(&data.display,
							      crtc->pipe,
							      data.output) {
					data.mode = igt_output_get_mode(data.output);

					igt_skip_on_f((IS_BATTLEMAGE(data.devid) && data.feature == FEATURE_FBC),
						       "FBC isn't supported on BMG\n");

					/* FBC Disp_ver 8 and below supports only I915_FORMAT_MOD_X_TILED */
					if (data.feature == FEATURE_FBC &&
					    intel_display_ver(intel_get_drm_devid(data.drm_fd)) <= 8)
						data.modifier = I915_FORMAT_MOD_X_TILED;
					else
						data.modifier = DRM_FORMAT_MOD_LINEAR;

					if (!check_support(&data))
						continue;

					igt_display_reset(&data.display);
					igt_output_set_crtc(data.output,
							    crtc);
					if (!intel_pipe_output_combo_valid(&data.display))
						continue;

					valid_tests++;
					igt_dynamic_f("%s-%s",
						      igt_crtc_name(crtc),
						      igt_output_name(data.output)) {
						prepare(&data);
						run_test(&data);
						cleanup(&data);
					}
				}

				/* One pipe is enough. */
				if (valid_tests)
					break;
			}
		}
	}

	igt_fixture() {
		buf_ops_destroy(data.bops);
		igt_display_fini(&data.display);
		close(data.drm_fd);
	}
}
